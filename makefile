CC := gcc
CFLAGS := -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -g -I. -Iutils -Inetwork -Iprotocol -Ifsops -Ipaths -Isession -Itransfer -Ilocks -Ish_mem 

## modules shared between Server and Client
COMMON_SRCS := utils/utils.c network/network.c protocol/protocol.c transfer/transfer.c locks/locks.c

## modules used by the server only 
SERVER_ONLY := fsops/fsops.c paths/paths.c session/session.c sh_mem/sh_mem.c

SERVER_SRCS := $(COMMON_SRCS) $(SERVER_ONLY)
CLIENT_SRCS := $(COMMON_SRCS)

# all headers
HDRS := $(wildcard */*.h)

all: Server Client

Server: main-server.c $(SERVER_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ main-server.c $(SERVER_SRCS)

Client: main-client.c $(CLIENT_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ main-client.c $(CLIENT_SRCS)

clean:
	rm -f Server Client

.PHONY: all clean
