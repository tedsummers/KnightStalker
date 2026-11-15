CC_LINUX ?= gcc
CC_WIN   ?= $(shell command -v i686-w64-mingw32-gcc 2>/dev/null || echo "zig cc -target x86-windows-gnu")
CFLAGS   := -O3 -Wall -Wextra -std=c11

.PHONY: all linux windows clean

all: linux windows

linux: KnightStalkerUCI

windows: KnightStalkerUCI.exe

KnightStalkerUCI: knightstalker.c fritz_data.h
	$(CC_LINUX) -m32 $(CFLAGS) -s -o $@ knightstalker.c

KnightStalkerUCI.exe: knightstalker.c fritz_data.h
	$(CC_WIN) $(CFLAGS) -s -o $@ knightstalker.c

clean:
	rm -f KnightStalkerUCI KnightStalkerUCI.exe
