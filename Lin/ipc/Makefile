# Compiler flags:
#   -std=c11       : Target standard C11 (works cleanly alongside _POSIX_C_SOURCE)
#   -Wall -Wextra  : Turn on strict warnings so subtle bugs get caught at compile time
#   -Wpedantic     : Enforce strict ISO C rules so the code stays portable to Windows/MinGW
#   -O2            : Standard optimization level without making debugging painful
#   -pthread       : Sets _REENTRANT for thread-safe libc/errno (needed by the server's operator thread)
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread

# Linker flags:
#   -pthread       : Links libpthread into the server for pthread_create/pthread_detach.
#                    Notice the client link rule below omits $(LDFLAGS) because the client
#                    is single-threaded and doesn't need pthreads linked in.
LDFLAGS := -pthread

# Server translation units:
# Listing files explicitly instead of using wildcards prevents accidentally
# compiling unfinished scratch files while building the system layer by layer.
# Notice the server links semaphore.c and shared_memory.c directly, while using
# its own internal System V message-queue helpers inside server/server.c.
SERVER_SRCS := \
	server/main.c \
	server/server.c \
	server/client_registry.c \
	server/authentication.c \
	server/transaction.c \
	server/persistence.c \
	server/operator.c \
	synchronization/semaphore.c \
	shared_memory/shared_memory.c

# Client translation units:
# Notice how the client never compiles anything from server/ or synchronization/—
# clients never acquire locks or touch server logic directly; they talk through
# msg_queue.c and read snapshots through shared_memory.c.
CLIENT_SRCS := \
	client/main.c \
	client/cli.c \
	client/session.c \
	msg_q/msg_queue.c \
	shared_memory/shared_memory.c

# Swap every '.c' extension for '.o' so each object file sits right next to its
# source file (e.g. client/cli.c -> client/cli.o).
SERVER_OBJS := $(SERVER_SRCS:.c=.o)
CLIENT_OBJS := $(CLIENT_SRCS:.c=.o)

# Output executables placed right in Lin/ipc/
SERVER_BIN  := server_app
CLIENT_BIN  := client_app
TEST_BIN    := tests/transaction_stress_test

TEST_SRCS := \
	tests/transaction_stress_test.c \
	server/transaction.c \
	server/client_registry.c \
	server/persistence.c \
	server/authentication.c \
	server/server.c \
	msg_q/msg_queue.c \
	synchronization/semaphore.c \
	shared_memory/shared_memory.c

# Declare non-file targets as .PHONY so commands like 'make clean' or 'make client'
# always run even if a file with that exact name ever appears in this folder.
.PHONY: all server client test bench clean

# Default target when running bare 'make': builds both binaries together
all: server client

# Convenience aliases so 'make server' or 'make client' builds just one side
server: $(SERVER_BIN)

client: $(CLIENT_BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

bench: $(TEST_BIN)
	sh ./tests/run_measurements.sh

$(TEST_BIN): $(TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRCS) $(LDFLAGS)

# Link the server executable ($@ expands to 'server_app')
$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) $(LDFLAGS)

# Link the client executable ($@ expands to 'client_app')
$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_OBJS)

# Pattern compilation rule: turns any foo.c into foo.o independently.
# That way Make only recompiles the specific .c file you edited instead of
# rebuilding both applications from scratch every time.
#   '$<' expands to the first prerequisite (the .c source file)
#   '$@' expands to the target (the .o object file)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Remove all intermediate .o files and both executables for a clean rebuild.
# '-f' keeps rm from failing with an error if some files are already gone.
clean:
	find . -type f \( -name '*.o' -o -name '*.exe' -o -name '*.out' -o -name '*.save' \) -delete
	rm -f $(SERVER_BIN) $(CLIENT_BIN) $(TEST_BIN)
	rm -f utilities/*.dat utilities/*.dat.tmp utilities/*.tmp utilities/*.log utilities/*.key utilities/*.db
