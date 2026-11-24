#include "chat.h"

#include <stdio.h>
#include <unistd.h>

#include <gio/gunixinputstream.h>

#define CHAT_PATH "/io/github/tydik42/Messenger"
#define CHAT_INTERFACE "io.github.tydik42.Messenger"
#define SEND_METHOD "SendMessage"

static const char interface_xml[] =
    "<node>"
    "  <interface name='" CHAT_INTERFACE "'>"
    "    <method name='" SEND_METHOD "'>"
    "      <arg name='text' type='s' direction='in'/>"
    "    </method>"
    "  </interface>"
    "</node>";

typedef struct {
    GMainLoop *loop;
    GDBusConnection *connection;
    GDataInputStream *input;
    GCancellable *cancellable;
    int status;
} Chat;

static gboolean allow_fifo_auth(GDBusAuthObserver *observer G_GNUC_UNUSED,
                                const char *mechanism,
                                gpointer user_data G_GNUC_UNUSED)
{
    /* The private temporary directory controls access to the transport. */
    return g_str_equal(mechanism, "ANONYMOUS");
}

static void receive_message(GDBusConnection *connection G_GNUC_UNUSED,
                            const char *sender G_GNUC_UNUSED,
                            const char *object_path G_GNUC_UNUSED,
                            const char *interface_name G_GNUC_UNUSED,
                            const char *method_name G_GNUC_UNUSED,
                            GVariant *parameters,
                            GDBusMethodInvocation *invocation,
                            gpointer user_data G_GNUC_UNUSED)
{
    const char *message;
    g_variant_get(parameters, "(&s)", &message);
    printf("Собеседник: %s\n", message);
    fflush(stdout);
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void read_input(Chat *chat);

static void message_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Chat *chat = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply =
        g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (g_cancellable_is_cancelled(chat->cancellable)) {
        g_main_loop_quit(chat->loop);
        return;
    }
    if (reply == NULL) {
        fprintf(stderr, "Ошибка при отправке: %s\n", error->message);
    }
    read_input(chat);
}

static void line_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Chat *chat = user_data;
    g_autoptr(GError) error = NULL;
    gsize length;
    g_autofree char *line =
        g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),
                                             result, &length, &error);

    if (g_cancellable_is_cancelled(chat->cancellable)) {
        g_main_loop_quit(chat->loop);
    } else if (line == NULL) {
        if (error != NULL) {
            fprintf(stderr, "Ошибка чтения: %s\n", error->message);
            chat->status = 1;
        }
        g_main_loop_quit(chat->loop);
    } else if (!g_utf8_validate(line, length, NULL)) {
        fprintf(stderr, "Сообщение должно быть в UTF-8.\n");
        read_input(chat);
    } else if (g_str_equal(line, "/quit")) {
        g_main_loop_quit(chat->loop);
    } else if (length == 0) {
        read_input(chat);
    } else {
        g_dbus_connection_call(chat->connection, NULL, CHAT_PATH,
                               CHAT_INTERFACE, SEND_METHOD,
                               g_variant_new("(s)", line), G_VARIANT_TYPE_UNIT,
                               G_DBUS_CALL_FLAGS_NONE, 5000, chat->cancellable,
                               message_sent, chat);
    }
}

static void read_input(Chat *chat)
{
    g_data_input_stream_read_line_async(chat->input, G_PRIORITY_DEFAULT,
                                       chat->cancellable, line_read, chat);
}

static void peer_closed(GDBusConnection *connection G_GNUC_UNUSED,
                        gboolean vanished G_GNUC_UNUSED,
                        GError *error G_GNUC_UNUSED, gpointer user_data)
{
    Chat *chat = user_data;
    puts("Собеседник отключился.");
    fflush(stdout);
    g_cancellable_cancel(chat->cancellable);
}

int chat_run(GIOStream *transport, gboolean listener, GError **error)
{
    g_autoptr(GDBusAuthObserver) auth = g_dbus_auth_observer_new();
    g_signal_connect(auth, "allow-mechanism", G_CALLBACK(allow_fifo_auth), NULL);
    g_autofree char *guid = listener ? g_dbus_generate_guid() : NULL;

    GDBusConnectionFlags flags = G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING;
    flags |= listener ? G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER |
                        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_ALLOW_ANONYMOUS
                      : G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT;
    g_autoptr(GDBusConnection) connection =
        g_dbus_connection_new_sync(transport, guid, flags, auth, NULL, error);
    if (connection == NULL) {
        return 1;
    }

    g_autoptr(GDBusNodeInfo) interface = g_dbus_node_info_new_for_xml(interface_xml, error);
    static const GDBusInterfaceVTable methods = {.method_call = receive_message};
    if (interface == NULL ||
        g_dbus_connection_register_object(connection, CHAT_PATH, interface->interfaces[0],
                                          &methods, NULL, NULL, error) == 0) {
        return 1;
    }

    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    g_autoptr(GInputStream) stdin_stream = g_unix_input_stream_new(STDIN_FILENO, FALSE);
    g_autoptr(GDataInputStream) input = g_data_input_stream_new(stdin_stream);
    Chat chat = {loop, connection, input, cancellable, 0};

    gulong closed_handler =
        g_signal_connect(connection, "closed", G_CALLBACK(peer_closed), &chat);
    puts("Связь установлена. Enter отправляет сообщение, /quit завершает чат.");
    fflush(stdout);
    read_input(&chat);
    g_dbus_connection_start_message_processing(connection);
    g_main_loop_run(loop);

    g_signal_handler_disconnect(connection, closed_handler);
    g_dbus_connection_flush_sync(connection, NULL, NULL);
    g_dbus_connection_close_sync(connection, NULL, NULL);
    return chat.status;
}
