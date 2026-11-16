# CrossIPC Windows build (MinGW-w64 / GCC)
# Run from this directory with: mingw32-make

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 -D_WIN32_WINNT=0x0601
LDFLAGS := -static-libgcc

SERVER_SRCS := \
	server/main.c \
	server/server.c \
	server/client_registry.c \
	server/authentication.c \
	server/transaction.c \
	server/persistence.c \
	server/operator.c \
	synchronization/semaphore.c \
	synchronization/txlock.c \
	shared_memory/shared_memory.c

CLIENT_SRCS := \
	client/main.c \
	client/cli.c \
	client/session.c \
	named_pipe/named_pipe.c

SERVER_OBJS := $(SERVER_SRCS:.c=.o)
CLIENT_OBJS := $(CLIENT_SRCS:.c=.o)

.PHONY: all server client clean
all: server client
server: server_app.exe
client: client_app.exe

server_app.exe: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) $(LDFLAGS)

client_app.exe: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) server_app.exe client_app.exe
	rm -f utilities/*.dat utilities/*.dat.tmp utilities/*.log
