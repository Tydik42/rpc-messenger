#ifndef RPC_MESSENGER_TRANSPORT_H
#define RPC_MESSENGER_TRANSPORT_H

#include <gio/gio.h>

/* A NULL directory creates a new session and prints the connect command. */
GIOStream *transport_open(const char *program, const char *directory, GError **error);

#endif
