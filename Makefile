CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -pedantic -Iinclude

COMMON_SRCS = \
	src/common/logging.c \
	src/common/network.c \
	src/common/protocol.c

SERVER_SRCS = \
	src/server/server_runtime.c \
	src/server/session.c \
	src/server/command_dispatch.c \
	src/server/file_service.c \
	src/server/archive_service.c

CLIENT_SRCS = \
	src/client/client_runtime.c \
	src/client/client_parser.c \
	src/client/client_transport.c

.PHONY: all clean

all: w26server mirror1 mirror2 client

w26server: w26server.c $(COMMON_SRCS) $(SERVER_SRCS)
	$(CC) $(CFLAGS) -o $@ w26server.c $(COMMON_SRCS) $(SERVER_SRCS)

mirror1: mirror1.c $(COMMON_SRCS) $(SERVER_SRCS)
	$(CC) $(CFLAGS) -o $@ mirror1.c $(COMMON_SRCS) $(SERVER_SRCS)

mirror2: mirror2.c $(COMMON_SRCS) $(SERVER_SRCS)
	$(CC) $(CFLAGS) -o $@ mirror2.c $(COMMON_SRCS) $(SERVER_SRCS)

client: client.c $(COMMON_SRCS) $(CLIENT_SRCS)
	$(CC) $(CFLAGS) -o $@ client.c $(COMMON_SRCS) $(CLIENT_SRCS)

clean:
	rm -f w26server mirror1 mirror2 client
