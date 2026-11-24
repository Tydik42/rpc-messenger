#ifndef RPC_MESSENGER_CHAT_H
#define RPC_MESSENGER_CHAT_H

#include <gio/gio.h>

int chat_run(GIOStream *transport, gboolean listener, GError **error);

#endif
