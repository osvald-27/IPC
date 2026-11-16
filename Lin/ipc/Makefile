# Compiles the multi-process System V IPC banking server and links the
# synchronization module into a single standalone executable (server_app).

# Use ':=' (simple expansion) so variables get evaluated once up front instead
# of being re-expanded recursively every time a rule runs.
CC      := gcc

# Compiler flags:
#   -std=c11       : Target modern standard C11 (works cleanly with _POSIX_C_SOURCE)
#   -Wall -Wextra  : Turn on strict warnings so subtle bugs get caught at compile time
#   -Wpedantic     : Enforce strict ISO C rules so the code stays portable to Windows/MinGW
#   -O2            : Standard optimization level without hurting debuggability
#   -pthread       : Define _REENTRANT for thread-safe libc/errno (needed for the operator thread)
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread

# Linker flags:
#   -pthread       : Link against libpthread so pthread_create/pthread_detach resolve properly
LDFLAGS := -pthread

# Explicit list of server and synchronization source files.
# Listing them out manually instead of using wildcards prevents accidentally
# pulling in unfinished modules or scratch files while building layer by layer.
SERVER_SRCS := \
	server/main.c \
	server/server.c \
	server/client_registry.c \
	server/authentication.c \
	server/transaction.c \
	server/persistence.c \
	server/operator.c \
	synchronization/semaphore.c

# Swap every '.c' extension for '.o' so each object file lives right next
# to its source file (e.g. server/main.c -> server/main.o).
SERVER_OBJS := $(SERVER_SRCS:.c=.o)

# Final executable name dropped in the current directory
SERVER_BIN  := server_app

# Mark non-file targets as .PHONY so commands like 'make clean' still execute
# even if a file named 'clean' or 'server' ever ends up in this folder.
.PHONY: all server clean

# Default target when running bare 'make'
all: server

# Alias so 'make server' builds the server binary directly
server: $(SERVER_BIN)

# Link step: bundles all compiled .o files into the final server_app binary.
# '$@' expands to the target name (server_app).
$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) $(LDFLAGS)

# Pattern rule: compiles each .c file into its matching .o file independently.
# That way Make only recompiles the specific file you touched instead of
# rebuilding the whole project every time.
# '$<' is the .c source file, and '$@' is the output .o file.
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Wipe all intermediate object files and the executable for a fresh build.
# '-f' keeps rm from failing if the files were already deleted.
clean:
	rm -f $(SERVER_OBJS) $(SERVER_BIN)
