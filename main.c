#include "chat.h"
#include "transport.h"

#include <signal.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    gboolean listener = argc == 2 && g_str_equal(argv[1], "listen");
    if (!listener && !(argc == 3 && g_str_equal(argv[1], "connect"))) {
        fprintf(stderr, "Использование:\n  %s listen\n  %s connect КАТАЛОГ\n",
                argv[0], argv[0]);
        return 1;
    }

    /* Report a closed FIFO through GIO instead of terminating on a write. */
    signal(SIGPIPE, SIG_IGN);
    g_autoptr(GError) error = NULL;
    g_autoptr(GIOStream) transport =
        transport_open(argv[0], listener ? NULL : argv[2], &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        return 130;
    }

    int status = transport == NULL ? 1 : chat_run(transport, listener, &error);
    if (error != NULL) {
        fprintf(stderr, "%s\n", error->message);
    }
    return status;
}
