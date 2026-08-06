CC := gcc
CFLAGS := -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -g -I. -Iutils -Inetwork -Iprotocol -Ifsops -Ipaths -Isession -Itransfer -Ilocks

## add here all the source files that need to be compiled
SRCS   := utils/utils.c network/network.c protocol/protocol.c fsops/fsops.c paths/paths.c session/session.c transfer/transfer.c locks/locks.c

# all headers
HDRS := $(wildcard */*.h)

all: Server Client

Server: main-server.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ main-server.c $(SRCS)

Client: main-client.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ main-client.c $(SRCS)

clean:
	rm -f Server Client

.PHONY: all clean