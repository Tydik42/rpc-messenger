PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g

.PHONY: all clean

all: messenger

messenger: main.c transport.c chat.c transport.h chat.h
	$(PKG_CONFIG) --exists gio-unix-2.0
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic \
		$$($(PKG_CONFIG) --cflags gio-unix-2.0) $(LDFLAGS) -o $@ $(filter %.c,$^) \
		$$($(PKG_CONFIG) --libs gio-unix-2.0) $(LDLIBS)

clean:
	$(RM) messenger
