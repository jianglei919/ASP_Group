CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -pedantic
OUT_DIR = out

SERVER_SRC = w26server.c
MIRROR1_SRC = mirror1.c
MIRROR2_SRC = mirror2.c
CLIENT_SRC = client.c

SERVER_BIN = $(OUT_DIR)/w26server
MIRROR1_BIN = $(OUT_DIR)/mirror1
MIRROR2_BIN = $(OUT_DIR)/mirror2
CLIENT_BIN = $(OUT_DIR)/client

.PHONY: all clean prep

all: prep $(SERVER_BIN) $(MIRROR1_BIN) $(MIRROR2_BIN) $(CLIENT_BIN)

prep:
	mkdir -p $(OUT_DIR)

$(SERVER_BIN): $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $@ $<

$(MIRROR1_BIN): $(MIRROR1_SRC)
	$(CC) $(CFLAGS) -o $@ $<

$(MIRROR2_BIN): $(MIRROR2_SRC)
	$(CC) $(CFLAGS) -o $@ $<

$(CLIENT_BIN): $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(SERVER_BIN) $(MIRROR1_BIN) $(MIRROR2_BIN) $(CLIENT_BIN)
	rm -f w26server mirror1 mirror2 client
