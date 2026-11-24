#define _POSIX_C_SOURCE 200809L

#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>

#define TO_LISTENER "to-listener"
#define TO_CONNECTOR "to-connector"

static volatile sig_atomic_t interrupted;

static void stop_waiting(int signum G_GNUC_UNUSED)
{
    interrupted = 1;
}

static void file_error(GError **error, const char *operation)
{
    int saved_errno = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                "%s: %s", operation, g_strerror(saved_errno));
}

static int open_fifo(int directory_fd, const char *name, int mode, GError **error)
{
    if (interrupted) {
        errno = EINTR;
        file_error(error, name);
        return -1;
    }

    struct stat info;
    if (fstatat(directory_fd, name, &info, AT_SYMLINK_NOFOLLOW) < 0) {
        file_error(error, name);
        return -1;
    }
    if (!S_ISFIFO(info.st_mode)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "%s: ожидался именованный канал", name);
        return -1;
    }

    int fd = openat(directory_fd, name, mode | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        file_error(error, name);
    }
    return fd;
}

static void release_client_lock(gpointer data)
{
    int *directory_fd = data;
    close(*directory_fd);
    g_free(directory_fd);
}

static GIOStream *open_fifos(const char *directory, gboolean listener, GError **error)
{
    int directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        file_error(error, directory);
        return NULL;
    }
    if (!listener && flock(directory_fd, LOCK_EX | LOCK_NB) < 0) {
        file_error(error, "Не удалось занять место собеседника");
        close(directory_fd);
        return NULL;
    }

    /* Both processes open the two FIFOs in the same order. */
    int first = open_fifo(directory_fd, TO_LISTENER,
                          listener ? O_RDONLY : O_WRONLY, error);
    int second = -1;
    if (first >= 0) {
        second = open_fifo(directory_fd, TO_CONNECTOR,
                           listener ? O_WRONLY : O_RDONLY, error);
    }
    if (first < 0) {
        close(directory_fd);
        return NULL;
    }
    if (second < 0) {
        close(first);
        close(directory_fd);
        return NULL;
    }
    g_autoptr(GInputStream) input =
        g_unix_input_stream_new(listener ? first : second, TRUE);
    g_autoptr(GOutputStream) output =
        g_unix_output_stream_new(listener ? second : first, TRUE);
    GIOStream *transport = g_simple_io_stream_new(input, output);

    if (listener) {
        close(directory_fd);
    } else {
        /* Retain the directory lock until the connector leaves the chat. */
        int *client_lock = g_new(int, 1);
        *client_lock = directory_fd;
        g_object_set_data_full(G_OBJECT(transport), "connector-lock", client_lock,
                               release_client_lock);
    }
    return transport;
}

static GIOStream *listen_for_peer(const char *program, GError **error)
{
    g_autofree char *directory = g_dir_make_tmp("rpc-messenger-XXXXXX", error);
    if (directory == NULL) {
        return NULL;
    }
    g_autofree char *incoming = g_build_filename(directory, TO_LISTENER, NULL);
    g_autofree char *outgoing = g_build_filename(directory, TO_CONNECTOR, NULL);
    GIOStream *transport = NULL;

    if (mkfifo(incoming, 0600) < 0 || mkfifo(outgoing, 0600) < 0) {
        file_error(error, "Не удалось создать каналы");
    } else {
        g_autofree char *command = g_shell_quote(program);
        g_autofree char *argument = g_shell_quote(directory);
        printf("Ожидаю собеседника. Во втором терминале выполните:\n"
               "%s connect %s\n", command, argument);
        fflush(stdout);
        transport = open_fifos(directory, TRUE, error);
    }

    /* Open FIFO descriptors keep working after their paths are removed. */
    g_unlink(incoming);
    g_unlink(outgoing);
    g_rmdir(directory);
    return transport;
}

GIOStream *transport_open(const char *program, const char *directory, GError **error)
{
    struct sigaction interrupt = {.sa_handler = stop_waiting};
    struct sigaction previous;
    sigemptyset(&interrupt.sa_mask);
    interrupted = 0;
    if (sigaction(SIGINT, &interrupt, &previous) < 0) {
        file_error(error, "sigaction");
        return NULL;
    }

    g_autoptr(GError) open_error = NULL;
    GIOStream *transport = directory == NULL
        ? listen_for_peer(program, &open_error)
        : open_fifos(directory, FALSE, &open_error);
    sigaction(SIGINT, &previous, NULL);

    if (interrupted) {
        g_clear_object(&transport);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Ожидание прервано");
    } else if (open_error != NULL) {
        g_propagate_error(error, g_steal_pointer(&open_error));
    }
    return transport;
}
