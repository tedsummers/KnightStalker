# KnightStalkerUCI

Standalone **64-bit** UCI chess engines for **Linux** (`KnightStalkerUCI`, `x86_64` ELF) and **Windows** (`KnightStalkerUCI.exe`, `x86_64` PE32+), wrapping Frans Morsch's 1991 16-bit MS-DOS *KnightStalker* / *Fritz 1.00* engine (`FRITZ.BIN`) and opening book (`FRITZ.FBK`).

## Try It Out on Lichess

You can play against **KnightStalkerUCI** live on Lichess via the [**KnightStalker_UCI**](https://lichess.org/@/KnightStalker_UCI) bot account:

- **Profile**: [https://lichess.org/@/KnightStalker_UCI](https://lichess.org/@/KnightStalker_UCI)
- **Challenge directly**: [Challenge KnightStalker_UCI](https://lichess.org/?user=KnightStalker_UCI#friend)
- **Accepted challenges**: Standard chess, **Casual** games, **Blitz** or **Rapid** time controls (human challengers)

## What's New in v2.1

- **Search depth cap lifted from 28/30 plies to 128 plies**: Relocated `FRITZ.BIN`'s 20 tightly-packed 32-ply arrays into unused `ENGINE_SEG` RAM (`0x7800..0x9FFF`, 512 bytes / 256 plies per array), expanded the internal 8086 stack from 288 bytes (`0x3630`) to ~24 KB (`0xFFF0`), and raised the internal ply guards and UCI depth cap to **128 plies** — allowing deep endgames and `go infinite` / `go depth 40+` searches to go far past the original 1991 28-ply ceiling while preserving 100% bit-for-bit fidelity at depths ≤ 28 (`Hash=0, Threads=1`).

## What's New in v2.0

- **~30×–38× faster emulated clock speed (~4+ GIPS equivalent)**: `tools/lift_fritz.py` statically translates all 3,904 reachable 16-bit 8086 instructions of `FRITZ.BIN` ahead-of-time into native C (`fritz_recompiled.h`) with backward flag-liveness elimination, flat-segment fast paths, and computed-goto dispatch (with the original 8086 interpreter retained as a safety fallback).
- **Zero-syscall asynchronous I/O**: Dedicated background `stdin` reader threads on both Linux (`pthread`) and Windows eliminate per-poll `select()` syscalls from the search hot loop.
- **Transposition Table (`Hash`) & Lazy SMP (`Threads`) enabled by default (`Hash=64`, `Threads=4`)**: Uses a lockless 64-bit XOR-keyed Zobrist transposition table (up to `512` MB) and multi-threaded Lazy SMP search (up to `32` threads) right out of the box for **+3 to +10 extra plies** over v1.1 in middlegames and endgames.
- **Strict 1991 faithful mode anytime (`Hash=0`, `Threads=1`)**: Setting `Hash=0` and `Threads=1` disables the TT and SMP to produce bit-for-bit identical moves, depths, scores, and principal variations to the original 1991 MS-DOS `FRITZ.BIN` at ~30×–38× faster clock speed (**+2 to +4 extra plies** on the clock).

### Performance Comparison (v1.1 vs v2.0, `movetime 2000`)

Measured on an Intel Core Ultra 7 265 (`OwnBook=false`):

| Position | v1.1 (32-bit interp) | v2.0 Faithful (`Hash=0, T=1`) | v2.0 + TT (`Hash=32, T=1`) | v2.0 + TT + SMP (`Hash=64, T=4`) |
|---|---:|---:|---:|---:|
| Starting position | depth 8 | **depth 10** | **depth 10** (0.44s) | **depth 11** (36.5M nps) |
| Middlegame | depth 8 | **depth 11** | **depth 11** (1.50s) | **depth 12** (54.6M nps) |
| K+P vs K endgame | depth 11 | **depth 15** | **depth 21** (0.88s) | **depth 20** (46.2M nps) |
| Rook endgame | depth 7 | **depth 9** | **depth 11** | **depth 12** (39.0M nps) |

## Overview

- **Zero runtime dependencies**: Both `FRITZ.BIN` (`28,828` bytes) and `FRITZ.FBK` (`16,132` bytes) are embedded directly into the executable via `fritz_data.h` and statically lifted into `fritz_recompiled.h`. No external files, DOSBox, or GUI files are required at runtime.
- **Faithful 1991 engine core**: Built directly on the unmodified 1991 `FRITZ.BIN` evaluation and search logic. By default (`Hash=64`, `Threads=4`), v2.0 adds a lockless transposition table and Lazy SMP threading on top; set `Hash=0` and `Threads=1` for 100% bit-for-bit identical move selection to the original MS-DOS binary running in DOSBox.

## Supported UCI Commands

- `uci` (`id name KnightStalkerUCI 2.1 (Fritz 1.00 1991)`, `id author Frans Morsch`, options below, `uciok`)
- `isready` (`readyok`, including responsive handling during `go infinite` and `go ponder`)
- `ucinewgame`
- `setoption name OwnBook value <true|false>` (default `true`)
- `setoption name Ponder value <true|false>` (default `false`)
- `setoption name Hash value <0..512>` (default `64` MB; set `0` to disable TT for strict 1991 mode)
- `setoption name Threads value <1..32>` (default `4`; set `1` for single-threaded mode; `>1` auto-allocates 16 MB TT if `Hash` is `0`)
- `setoption name Clear Hash`
- `position [startpos | fen <fen>] [moves <m1> <m2> ...]`
- `go [ponder] [depth <n> | movetime <ms> | wtime <ms> btime <ms> winc <ms> binc <ms> movestogo <n> | infinite]`
- `ponderhit`
- `stop`
- `quit`

> Note:
> - `FRITZ.FBK` tracks ply history from the standard starting position, so `OwnBook` is used from `startpos` (with move history) and automatically bypassed for custom FEN positions.
> - When `Ponder` is enabled (`setoption name Ponder value true`) or `go ponder` is used, `bestmove` outputs a verified legal `ponder <move>` continuation whenever one exists, and `go ponder` searches without time-abort until `ponderhit` (which starts the move clock) or `stop` is received.
> - In v2.0, `nodes` reports actual chess nodes (calls to `FRITZ.BIN`'s make-move routine) rather than the synthetic instruction-scaled count used in v1.1.

## Build Instructions

### Prerequisites

- **Linux 64-bit target (`KnightStalkerUCI`)**:
  - `gcc` (or `clang`) with `pthreads`
- **Windows 64-bit target (`KnightStalkerUCI.exe`)**:
  - **MinGW-w64** (`x86_64-w64-mingw32-gcc`), **or**
  - **Zig** (`zig cc -target x86_64-windows-gnu`)
- **Regenerating `fritz_recompiled.h`** (optional — already checked in):
  - `python3` (`make recompile`)

### Build Commands

Build both 64-bit binaries using `Makefile`:

```bash
make
```

Or build each target individually:

```bash
# Linux 64-bit ELF (x86_64)
make linux
# Equivalent direct command:
gcc -O3 -Wall -Wextra -std=c11 -pthread -s -o KnightStalkerUCI knightstalker.c

# Windows 64-bit PE32+ console executable (x86_64)
make windows
# Equivalent direct commands:
x86_64-w64-mingw32-gcc -O3 -Wall -Wextra -std=c11 -s -o KnightStalkerUCI.exe knightstalker.c
# or using Zig:
zig cc -target x86_64-windows-gnu -O3 -Wall -Wextra -std=c11 -s -o KnightStalkerUCI.exe knightstalker.c
```

On Windows natively with MSYS2 MinGW-w64 or Zig installed, you can also run:

```powershell
zig cc -target x86_64-windows-gnu -O3 -Wall -Wextra -std=c11 -s -o KnightStalkerUCI.exe knightstalker.c
```

## Usage

Download the prebuilt release binary for your platform (`KnightStalkerUCI` for Linux or `KnightStalkerUCI.exe` for Windows) or compile it from source, then register it as a **UCI engine** in any compatible chess GUI such as Arena, CuteChess, BanksiaGUI, Nibbler, or Lucas Chess.

You can also test it directly from a terminal:

```text
uci
isready
setoption name Hash value 32
setoption name Threads value 4
position startpos
go depth 9
quit
```
