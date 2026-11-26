CC_LINUX ?= gcc
CC_WIN   ?= $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null || echo "zig cc -target x86_64-windows-gnu")
CFLAGS   := -O3 -Wall -Wextra -std=c11

.PHONY: all linux windows recompile clean

all: linux windows

linux: KnightStalkerUCI

windows: KnightStalkerUCI.exe

recompile: fritz_recompiled.h

fritz_recompiled.h: tools/lift_fritz.py fritz_data.h
	python3 tools/lift_fritz.py

KnightStalkerUCI: knightstalker.c fritz_data.h fritz_recompiled.h
	$(CC_LINUX) $(CFLAGS) -pthread -s -o $@ knightstalker.c -lm

KnightStalkerUCI.exe: knightstalker.c fritz_data.h fritz_recompiled.h
	$(CC_WIN) $(CFLAGS) -s -o $@ knightstalker.c -lm

clean:
	rm -f KnightStalkerUCI KnightStalkerUCI.exe
