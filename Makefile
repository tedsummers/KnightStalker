CC_LINUX         ?= gcc
CC_WIN           ?= $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null || echo "zig cc -target x86_64-windows-gnu")
CC_MACOS_ARM64   ?= zig cc -target aarch64-macos.11.0
CC_MACOS_X64     ?= zig cc -target x86_64-macos.11.0
CC_ANDROID_ARM64 ?= zig cc -target aarch64-linux-musl
CC_ANDROID_ARMV7 ?= zig cc -target arm-linux-musleabihf
CFLAGS           := -O3 -Wall -Wextra -std=c11

.PHONY: all release linux windows macos macos-arm64 macos-x86_64 macos-universal android android-arm64 android-armv7 recompile clean

all: linux windows

release: linux windows macos android

linux: KnightStalkerUCI

windows: KnightStalkerUCI.exe

macos: macos-arm64 macos-x86_64 macos-universal

macos-arm64: KnightStalkerUCI-macos-arm64

macos-x86_64: KnightStalkerUCI-macos-x86_64

macos-universal: KnightStalkerUCI-macos-universal

android: android-arm64 android-armv7

android-arm64: KnightStalkerUCI-android-arm64

android-armv7: KnightStalkerUCI-android-armv7

recompile: fritz_recompiled.h

fritz_recompiled.h: tools/lift_fritz.py fritz_data.h
	python3 tools/lift_fritz.py

KnightStalkerUCI: knightstalker.c fritz_data.h fritz_recompiled.h
	$(CC_LINUX) $(CFLAGS) -pthread -s -o $@ knightstalker.c -lm

KnightStalkerUCI.exe: knightstalker.c fritz_data.h fritz_recompiled.h
	$(CC_WIN) $(CFLAGS) -s -o $@ knightstalker.c -lm

KnightStalkerUCI-macos-arm64: knightstalker.c fritz_data.h fritz_recompiled.h
	$(CC_MACOS_ARM64) $(CFLAGS) -pthread -s -o $@ knightstalker.c -lm

KnightStalkerUCI-macos-x86_64: knightstalker.c fritz_data.h fritz_recompiled.h
	$(CC_MACOS_X64) $(CFLAGS) -pthread -s -o $@ knightstalker.c -lm

KnightStalkerUCI-macos-universal: KnightStalkerUCI-macos-arm64 KnightStalkerUCI-macos-x86_64 tools/make_fat_macho.py
	python3 tools/make_fat_macho.py $@ KnightStalkerUCI-macos-x86_64 KnightStalkerUCI-macos-arm64

KnightStalkerUCI-android-arm64: knightstalker.c fritz_data.h fritz_recompiled.h
	$(CC_ANDROID_ARM64) $(CFLAGS) -pthread -static -fPIE -pie -s -o $@ knightstalker.c -lm

KnightStalkerUCI-android-armv7: knightstalker.c fritz_data.h fritz_recompiled.h
	$(CC_ANDROID_ARMV7) $(CFLAGS) -pthread -static -fPIE -pie -s -o $@ knightstalker.c -lm

clean:
	rm -f KnightStalkerUCI KnightStalkerUCI.exe \
	      KnightStalkerUCI-macos-arm64 KnightStalkerUCI-macos-x86_64 KnightStalkerUCI-macos-universal \
	      KnightStalkerUCI-android-arm64 KnightStalkerUCI-android-armv7
