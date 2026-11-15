# Windows build. Run from Win/ipc in cmd or PowerShell:  mingw32-make
# No make installed? Run build.bat, it does the same two gcc commands.

CC     = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2 -D_WIN32_WINNT=0x0600 -D__USE_MINGW_ANSI_STDIO=1

COMMON = named_pipe/named_pipe.c utilities/status_string.c

SERVER = server/main.c server/server.c server/authentication.c \
         server/client_registry.c server/transaction.c server/persistence.c \
         server/operator.c shared_memory/shared_memory.c \
         synchronization/semaphore.c $(COMMON)

CLIENT = client/main.c client/runner.c client/session.c $(COMMON)

HEADERS = $(wildcard common/*.h server/*.h client/*.h named_pipe/*.h \
            shared_memory/*.h synchronization/*.h)

all: build/server.exe build/client.exe

build/server.exe: $(SERVER) $(HEADERS)
	@if not exist build mkdir build
	$(CC) $(CFLAGS) -o $@ $(SERVER)

build/client.exe: $(CLIENT) $(HEADERS)
	@if not exist build mkdir build
	$(CC) $(CFLAGS) -o $@ $(CLIENT)

clean:
	@if exist build rmdir /s /q build

.PHONY: all clean
