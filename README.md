# KnightStalkerUCI

Standalone UCI chess engines for **Linux** (`KnightStalkerUCI`, `x86_64` ELF), **Windows** (`KnightStalkerUCI.exe`, `x86_64` PE32+), **macOS** (`KnightStalkerUCI-macos-universal`, `arm64` Apple Silicon & `x86_64` Intel Mach-O), and **Android** (`KnightStalkerUCI-android-arm64` & `KnightStalkerUCI-android-armv7`, static-PIE ELF), wrapping Frans Morsch's 1991 16-bit MS-DOS *KnightStalker* / *Fritz 1.00* engine (`FRITZ.BIN`) and opening book (`FRITZ.FBK`).

## Try It Out on Lichess

You can play against **KnightStalkerUCI** live on Lichess via the [**KnightStalker_UCI**](https://lichess.org/@/KnightStalker_UCI) bot account:

- **Profile**: [https://lichess.org/@/KnightStalker_UCI](https://lichess.org/@/KnightStalker_UCI)
- **Challenge directly**: [Challenge KnightStalker_UCI](https://lichess.org/?user=KnightStalker_UCI#friend)
- **Accepted challenges**: Standard chess, **Casual** games, **Blitz** or **Rapid** time controls (human challengers)

## What's New in v3.0

- **Modern Search Scaling (+179 ± 94 Elo over v2.5, +5 to +14 plies at 2s/move)**:
  - **Fixed TT hook placement & 4-way associative 64-byte buckets**: Moved the transposition table probe ahead of `FRITZ.BIN`'s internal null-move search (`0x5AD3`) and added probing for null-move child nodes (`0x5C13`). Previously, nodes that attempted null-move and failed low skipped TT probing, move ordering, and storage altogether (~50% of interior nodes). Upgrading to 4-way cache-line buckets with depth+age replacement cuts collisions under Lazy SMP.
  - **Interior Principal Variation Search (PVS)**: Zero-window scout searches on non-first moves at interior PV nodes, with automatic full-window re-search on fail-high.
  - **Late Move Reductions (LMR)** on `FRITZ.BIN`'s isolated quiet-move loop (`0x5B60`) using a logarithmic reduction table (`lmr_table`), with automatic full-depth re-search whenever a reduced move beats alpha.
  - **Adaptive Null-Move Pruning (`R = 2` / `R = 3`)** replacing the 1991 fixed `R = 1`, guarded against Zugzwang in pawn-only endgames, nested null-moves, PV nodes, and TT upper-bound fail-low hints.
  - **Reverse Futility Pruning (RFP), Futility Pruning (FP), and Late Move Pruning (LMP)** at shallow remaining depths using `FRITZ.BIN`'s own incremental evaluation while never pruning checks or the first legal move.
- **`ModernSearch` UCI toggle (`default true`)**: Controls PVS, LMR, Adaptive NMP, and RFP/FP/LMP while keeping the 1991 `FRITZ.BIN` evaluation completely unchanged. Set `Hash=0`, `Threads=1`, and `ModernSearch=false` anytime for 100% bit-for-bit identical 1991 DOS output.

### Performance Comparison (v2.5 vs v3.0, `movetime 2000`, `Hash=64`, `OwnBook=false`)

| Position | v2.5 (`T=1`) | v3.0 (`T=1`) | v2.5 (`T=4`) | v3.0 (`T=4`) |
|---|---:|---:|---:|---:|
| Starting position | depth 10 (9.2M nps) | **depth 15** (8.8M nps) | depth 11 (35.5M nps) | **depth 16** (34.5M nps) |
| Middlegame (QGD) | depth 10 (10.0M nps) | **depth 15** (9.6M nps) | depth 10 (37.1M nps) | **depth 17** (35.7M nps) |
| K+P vs K endgame | depth 21 (15.2M nps) | **depth 33** (15.8M nps) | depth 19 (52.7M nps) | **depth 33** (61.1M nps) |
| Rook endgame | depth 11 (9.5M nps) | **depth 16** (8.8M nps) | depth 12 (34.5M nps) | **depth 18** (32.0M nps) |

- **Head-to-head (`fastchess`, 40 games, `10+0.1`, `1 thread`, `64 MB`, balanced 20-opening EPD suite)**: **v3.0 vs v2.5 = +25 =9 -6 (73.75%, +179 ± 94 Elo, 100.0% LOS)**.

## What's New in v2.5

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

- `uci` (`id name KnightStalkerUCI 3.0 (Fritz 1.00 1991)`, `id author Frans Morsch`, options below, `uciok`)
- `isready` (`readyok`, including responsive handling during `go infinite` and `go ponder`)
- `ucinewgame`
- `setoption name OwnBook value <true|false>` (default `true`)
- `setoption name Ponder value <true|false>` (default `false`)
- `setoption name Hash value <0..512>` (default `64` MB; set `0` to disable TT)
- `setoption name Threads value <1..32>` (default `4`; set `1` for single-threaded mode; `>1` auto-allocates 16 MB TT if `Hash` is `0`)
- `setoption name ModernSearch value <true|false>` (default `true`; set `false` alongside `Hash=0` and `Threads=1` for strict 100% bit-for-bit 1991 mode)
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
- **macOS (`arm64` Apple Silicon, `x86_64` Intel, and Universal) & Android (`arm64` / `armv7` static-PIE) targets**:
  - **Zig** (`zig cc`) + `python3` (for combining the macOS Universal binary)
- **Regenerating `fritz_recompiled.h`** (optional — already checked in):
  - `python3` (`make recompile`)

### Build Commands

Build Linux + Windows binaries using `Makefile`, or all platforms (Linux, Windows, macOS, Android) via `make release`:

```bash
make          # Linux + Windows
make release  # Linux + Windows + macOS (arm64/x86_64/universal) + Android (arm64/armv7)
```

Or build each target individually:

```bash
# Linux 64-bit ELF (x86_64)
make linux

# Windows 64-bit PE32+ console executable (x86_64)
make windows

# macOS Apple Silicon (arm64), Intel (x86_64), and Universal Fat Mach-O
make macos

# Android ARM64 (aarch64) and ARMv7 (32-bit) static-PIE ELF executables
make android
```

On Windows natively with MSYS2 MinGW-w64 or Zig installed, you can also run:

```powershell
zig cc -target x86_64-windows-gnu -O3 -Wall -Wextra -std=c11 -s -o KnightStalkerUCI.exe knightstalker.c -lm
```

## Usage

Download the prebuilt release binary for your platform from [GitHub Releases](https://github.com/tedsummers/KnightStalker/releases) or compile it from source, then register it as a **UCI engine** in any compatible chess GUI:

- **Linux / Windows:** Arena, CuteChess, BanksiaGUI, Nibbler, En Croissant, or Lucas Chess.
- **macOS:** BanksiaGUI, Nibbler, En Croissant, or HIARCS Chess Explorer (`chmod +x KnightStalkerUCI-macos-universal`; if macOS Gatekeeper flags the unsigned download, run `xattr -d com.apple.quarantine KnightStalkerUCI-macos-universal`).
- **Android:** DroidFish, Chess for Android, Acid Ape Chess, or Termux (use `KnightStalkerUCI-android-arm64` on 64-bit devices, or `KnightStalkerUCI-android-armv7` on older 32-bit devices). *(Note: iOS does not allow standalone third-party command-line UCI binaries outside signed App Store apps.)*

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
