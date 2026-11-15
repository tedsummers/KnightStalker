# KnightStalkerUCI

Standalone **32-bit** UCI chess engines for **Linux** (`KnightStalkerUCI`, 32-bit `i386` ELF) and **Windows** (`KnightStalkerUCI.exe`, 32-bit `i386` PE32), wrapping Frans Morsch's 1991 16-bit MS-DOS *KnightStalker* / *Fritz 1.00* engine (`FRITZ.BIN`) and opening book (`FRITZ.FBK`).

## Overview

- **Zero runtime dependencies**: Both `FRITZ.BIN` (`28,828` bytes) and `FRITZ.FBK` (`16,132` bytes) are embedded directly into the executable via `fritz_data.h`. No external files, DOSBox, or GUI files are required at runtime.
- **Faithful 1991 engine behavior**: `knightstalker.c` implements a self-contained real-mode 8086 emulator around the original unmodified 16-bit machine code (`ENGINE_SEG:0x0100`) together with a UCI protocol frontend (`stdin`/`stdout`), producing identical best moves, search depths, centipawn/mate scores, and principal variations to the original MS-DOS `FRITZ.BIN` running in DOSBox.

## Supported UCI Commands

- `uci` (`id name KnightStalkerUCI 1.1 (Fritz 1.00 1991)`, `id author Frans Morsch`, `option name OwnBook type check default true`, `option name Ponder type check default false`, `uciok`)
- `isready` (`readyok`, including responsive handling during `go infinite` and `go ponder`)
- `ucinewgame`
- `setoption name OwnBook value <true|false>`
- `setoption name Ponder value <true|false>`
- `position [startpos | fen <fen>] [moves <m1> <m2> ...]`
- `go [ponder] [depth <n> | movetime <ms> | wtime <ms> btime <ms> winc <ms> binc <ms> movestogo <n> | infinite]`
- `ponderhit`
- `stop`
- `quit`

> Note:
> - `FRITZ.FBK` tracks ply history from the standard starting position, so `OwnBook` is used from `startpos` (with move history) and automatically bypassed for custom FEN positions.
> - When `Ponder` is enabled (`setoption name Ponder value true`) or `go ponder` is used, `bestmove` outputs a verified legal `ponder <move>` continuation whenever one exists, and `go ponder` searches without time-abort until `ponderhit` (which starts the move clock) or `stop` is received.

## Build Instructions

### Prerequisites

- **Linux 32-bit target (`KnightStalkerUCI`)**:
  - `gcc` with 32-bit (`-m32`) multilib C libraries:
    - **Arch Linux**: enable `[multilib]` and install `lib32-glibc` / multilib toolchain
    - **Debian / Ubuntu**: `sudo apt install gcc-multilib libc6-dev-i386`
    - **Fedora**: `sudo dnf install glibc-devel.i686 libgcc.i686`
- **Windows 32-bit target (`KnightStalkerUCI.exe`)**:
  - **MinGW-w64** (`i686-w64-mingw32-gcc`), **or**
  - **Zig** (`zig cc -target x86-windows-gnu`)

### Build Commands

Build both 32-bit binaries using `Makefile`:

```bash
make
```

Or build each target individually:

```bash
# Linux 32-bit ELF (i386)
make linux
# Equivalent direct command:
gcc -m32 -O3 -Wall -Wextra -std=c11 -s -o KnightStalkerUCI knightstalker.c

# Windows 32-bit PE32 console executable (i386)
make windows
# Equivalent direct commands:
i686-w64-mingw32-gcc -O3 -Wall -Wextra -std=c11 -s -o KnightStalkerUCI.exe knightstalker.c
# or using Zig:
zig cc -target x86-windows-gnu -O3 -Wall -Wextra -std=c11 -s -o KnightStalkerUCI.exe knightstalker.c
```

On Windows natively with MSYS2 MinGW 32-bit or Zig installed, you can also run:

```powershell
zig cc -target x86-windows-gnu -O3 -Wall -Wextra -std=c11 -s -o KnightStalkerUCI.exe knightstalker.c
```

## Usage

Download the prebuilt release binary for your platform (`KnightStalkerUCI` for Linux or `KnightStalkerUCI.exe` for Windows) or compile it from source, then register it as a **UCI engine** in any compatible chess GUI such as Arena, CuteChess, BanksiaGUI, Nibbler, or Lucas Chess.

You can also test it directly from a terminal:

```text
uci
isready
position startpos
go depth 5
quit
```
