# Build and run my tests.   mingw32-make clean   mingw32-make run
# Needs the repo's ../common/types.h, protocol.h, transaction.h (the group's files) next to my modules.
SHELL  := cmd.exe
CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2
SRC    := ../shared_memory/shared_memory.c ../synchronization/txlock.c ../synchronization/semaphore.c
HDR    := ../shared_memory/shared_memory.h ../synchronization/txlock.h ../synchronization/semaphore.h ../common/status.h

all: test_shm_sync.exe test_integration.exe

test_shm_sync.exe: test_shm_sync.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ test_shm_sync.c $(SRC)

test_integration.exe: test_integration.c $(SRC) $(HDR) ../server/server.h ../common/protocol.h
	$(CC) $(CFLAGS) -o $@ test_integration.c $(SRC)

run: all
	test_shm_sync.exe
	test_integration.exe

clean:
	-del /Q *.exe 2>nul

.PHONY: all run clean
