/*
 * KnightStalker UCI Chess Engine
 *
 * Standalone 32-bit UCI wrapper around the original 16-bit 8086
 * KnightStalker / Fritz 1.00 (1991) chess engine module (FRITZ.BIN)
 * and its opening book (FRITZ.FBK).
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <sys/select.h>
#include <unistd.h>
#endif

#include "fritz_data.h"

/* =========================================================================
 * 8086 Real-Mode Memory Map & Emulator State
 * ========================================================================= */

#define ENGINE_SEG 0x1000u
#define BOOK_SEG   0x2000u
#define STACK_SEG  0x3000u
#define TRAP_SEG   0xF000u

#define ENGINE_BASE (ENGINE_SEG << 4)
#define BOOK_BASE   (BOOK_SEG << 4)
#define RAM_SIZE    (1u << 20)

enum {
    R_AX = 0,
    R_CX = 1,
    R_DX = 2,
    R_BX = 3,
    R_SP = 4,
    R_BP = 5,
    R_SI = 6,
    R_DI = 7
};

enum {
    S_ES = 0,
    S_CS = 1,
    S_SS = 2,
    S_DS = 3
};

#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_TF 0x0100u
#define F_IF 0x0200u
#define F_DF 0x0400u
#define F_OF 0x0800u

typedef struct {
    uint16_t regs[8];
    uint16_t sregs[4];
    uint16_t ip;
    uint16_t flags;
} Cpu8086;

static uint8_t ram[RAM_SIZE];
static uint8_t parity_lut[256];
static Cpu8086 cpu;

/* Search & UCI global state */
static uint64_t prog_start_ms = 0;
static uint32_t bios_tick_base = 0;

static int      opt_own_book = 1;
static int      pos_is_startpos = 1;

static int      searching = 0;
static int      is_infinite = 0;
static uint64_t search_start_ms = 0;
static uint64_t search_hard_ms = 0;
static int      search_depth = 1;
static uint64_t search_nodes = 0;
static int      stop_requested = 0;
static int      quit_requested = 0;
static int      stdin_eof = 0;

/* Non-blocking stdin line queue */
#define MAX_PENDING_LINES 64
#define MAX_LINE_LEN      8192
static char     inbuf[MAX_LINE_LEN];
static int      inlen = 0;
static char     pending_lines[MAX_PENDING_LINES][MAX_LINE_LEN];
static int      pending_head = 0;
static int      pending_tail = 0;
static int      pending_count = 0;

#ifdef _WIN32
static CRITICAL_SECTION stdin_cs;
static HANDLE           stdin_event = NULL;
static char             win_lines[MAX_PENDING_LINES][MAX_LINE_LEN];
static int              win_head = 0;
static int              win_tail = 0;
static int              win_count = 0;
static int              win_eof = 0;
#endif

/* =========================================================================
 * Platform Monotonic Clock & Non-blocking Stdin
 * ========================================================================= */

static uint64_t now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int init = 0;
    LARGE_INTEGER cnt;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    QueryPerformanceCounter(&cnt);
    return (uint64_t)((cnt.QuadPart * 1000ULL) / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
#endif
}

static uint32_t current_bios_ticks(void) {
    uint64_t elapsed = now_ms() - prog_start_ms;
    return bios_tick_base + (uint32_t)((elapsed * 182065ULL) / 10000000ULL);
}

static char *trim_ws(char *s) {
    size_t n;
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        s++;
    }
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

static void enqueue_line(const char *line) {
    if (pending_count < MAX_PENDING_LINES) {
        snprintf(pending_lines[pending_tail], MAX_LINE_LEN, "%s", line);
        pending_tail = (pending_tail + 1) % MAX_PENDING_LINES;
        pending_count++;
    }
}

static int dequeue_line(char *out, int out_cap) {
    if (pending_count == 0) return 0;
    snprintf(out, (size_t)out_cap, "%s", pending_lines[pending_head]);
    pending_head = (pending_head + 1) % MAX_PENDING_LINES;
    pending_count--;
    return 1;
}

/* Try to read from stdin without blocking; returns 1 if bytes or EOF processed. */
static int try_fill_inbuf(int blocking) {
    if (stdin_eof) return 0;
    if (inlen >= MAX_LINE_LEN - 1) return 0;

#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        stdin_eof = 1;
        return 0;
    }
    DWORD ftype = GetFileType(h);
    if (!blocking) {
        if (ftype == FILE_TYPE_PIPE) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
                stdin_eof = 1;
                return 0;
            }
            if (avail == 0) return 0;
        } else if (ftype == FILE_TYPE_DISK) {
            /* Regular file: check if at EOF or bytes remain before ReadFile */
            LARGE_INTEGER cur = {{0}}, sz = {{0}};
            if (!GetFileSizeEx(h, &sz)) return 0;
            if (!SetFilePointerEx(h, cur, &cur, FILE_CURRENT)) return 0;
            if (cur.QuadPart >= sz.QuadPart) {
                stdin_eof = 1;
                return 0;
            }
        } else {
            /* Console or other handle: avoid blocking unless an input event is ready */
            if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) {
                return 0;
            }
            if (ftype == FILE_TYPE_CHAR) {
                INPUT_RECORD recs[16];
                DWORD nread = 0;
                int has_cr = 0;
                if (PeekConsoleInputA(h, recs, 16, &nread)) {
                    for (DWORD i = 0; i < nread; i++) {
                        if (recs[i].EventType == KEY_EVENT && recs[i].Event.KeyEvent.bKeyDown &&
                            (recs[i].Event.KeyEvent.uChar.AsciiChar == '\r' ||
                             recs[i].Event.KeyEvent.uChar.AsciiChar == '\n')) {
                            has_cr = 1;
                            break;
                        }
                    }
                }
                if (!has_cr) return 0;
            }
        }
    }
    DWORD got = 0;
    if (!ReadFile(h, inbuf + inlen, (DWORD)(MAX_LINE_LEN - 1 - inlen), &got, NULL) || got == 0) {
        stdin_eof = 1;
        return 0;
    }
    inlen += (int)got;
    inbuf[inlen] = '\0';
    return 1;
#else
    if (!blocking) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        int rc = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) return 0;
    }
    ssize_t got;
    do {
        got = read(STDIN_FILENO, inbuf + inlen, (size_t)(MAX_LINE_LEN - 1 - inlen));
    } while (got < 0 && errno == EINTR);
    if (got <= 0) {
        stdin_eof = 1;
        return 0;
    }
    inlen += (int)got;
    inbuf[inlen] = '\0';
    return 1;
#endif
}

static int extract_line_from_inbuf(char *out, int out_cap) {
    char *nl = memchr(inbuf, '\n', (size_t)inlen);
    if (!nl) {
        if (stdin_eof && inlen > 0) {
            int copy = inlen < out_cap - 1 ? inlen : out_cap - 1;
            memcpy(out, inbuf, (size_t)copy);
            out[copy] = '\0';
            inlen = 0;
            return 1;
        }
        return 0;
    }
    int len = (int)(nl - inbuf);
    int copy = len < out_cap - 1 ? len : out_cap - 1;
    memcpy(out, inbuf, (size_t)copy);
    out[copy] = '\0';
    int rem = inlen - (len + 1);
    if (rem > 0) {
        memmove(inbuf, nl + 1, (size_t)rem);
    }
    inlen = rem;
    inbuf[inlen] = '\0';
    return 1;
}

static void handle_polled_line(char *line) {
    char *t = trim_ws(line);
    if (*t == '\0') return;
    if (strcmp(t, "stop") == 0) {
        stop_requested = 1;
        ram[ENGINE_BASE + 0x009cu] |= 0x80u;
    } else if (strcmp(t, "quit") == 0) {
        quit_requested = 1;
        stop_requested = 1;
        ram[ENGINE_BASE + 0x009cu] |= 0x80u;
    } else if (strcmp(t, "isready") == 0) {
        printf("readyok\n");
        fflush(stdout);
    } else {
        enqueue_line(t);
    }
}

#ifdef _WIN32
static void win32_push_line(const char *line) {
    size_t len = strlen(line);
    if (len >= MAX_LINE_LEN) return;
    EnterCriticalSection(&stdin_cs);
    while (win_count >= MAX_PENDING_LINES) {
        LeaveCriticalSection(&stdin_cs);
        Sleep(1);
        EnterCriticalSection(&stdin_cs);
    }
    memcpy(win_lines[win_tail], line, len + 1);
    win_tail = (win_tail + 1) % MAX_PENDING_LINES;
    win_count++;
    SetEvent(stdin_event);
    LeaveCriticalSection(&stdin_cs);
}

static DWORD WINAPI win32_stdin_thread(LPVOID arg) {
    (void)arg;
    char line[MAX_LINE_LEN];
    for (;;) {
        while (extract_line_from_inbuf(line, sizeof(line))) {
            win32_push_line(line);
        }
        if (stdin_eof) break;
        if (!try_fill_inbuf(1)) {
            if (!stdin_eof && inlen >= MAX_LINE_LEN - 1) {
                inlen = 0;
                inbuf[0] = '\0';
                continue;
            }
            break;
        }
    }
    if (inlen > 0) {
        inbuf[inlen] = '\0';
        win32_push_line(inbuf);
        inlen = 0;
    }
    EnterCriticalSection(&stdin_cs);
    win_eof = 1;
    SetEvent(stdin_event);
    LeaveCriticalSection(&stdin_cs);
    return 0;
}

static void poll_stdin_during_search(void) {
    for (;;) {
        char line[MAX_LINE_LEN];
        int have = 0;
        int eof_now = 0;
        EnterCriticalSection(&stdin_cs);
        if (win_count > 0) {
            size_t len = strlen(win_lines[win_head]);
            memcpy(line, win_lines[win_head], len + 1);
            win_head = (win_head + 1) % MAX_PENDING_LINES;
            win_count--;
            have = 1;
        } else {
            eof_now = win_eof;
        }
        LeaveCriticalSection(&stdin_cs);
        if (!have) {
            if (eof_now && is_infinite) {
                stop_requested = 1;
                ram[ENGINE_BASE + 0x009cu] |= 0x80u;
            }
            break;
        }
        handle_polled_line(line);
    }
}

static int read_next_uci_line(char *out, int out_cap) {
    if (dequeue_line(out, out_cap)) return 1;
    for (;;) {
        int have = 0;
        int eof_now = 0;
        EnterCriticalSection(&stdin_cs);
        if (win_count > 0) {
            size_t len = strlen(win_lines[win_head]);
            if (len >= (size_t)out_cap) len = (size_t)out_cap - 1;
            memcpy(out, win_lines[win_head], len);
            out[len] = '\0';
            win_head = (win_head + 1) % MAX_PENDING_LINES;
            win_count--;
            have = 1;
        } else {
            eof_now = win_eof;
            if (!eof_now) ResetEvent(stdin_event);
        }
        LeaveCriticalSection(&stdin_cs);
        if (have) return 1;
        if (eof_now) return 0;
        WaitForSingleObject(stdin_event, INFINITE);
    }
}
#else
static void poll_stdin_during_search(void) {
    char line[MAX_LINE_LEN];
    while (extract_line_from_inbuf(line, sizeof(line))) {
        handle_polled_line(line);
    }
    while (try_fill_inbuf(0)) {
        while (extract_line_from_inbuf(line, sizeof(line))) {
            handle_polled_line(line);
        }
    }
    if (stdin_eof && is_infinite) {
        stop_requested = 1;
        ram[ENGINE_BASE + 0x009cu] |= 0x80u;
    }
}

static int read_next_uci_line(char *out, int out_cap) {
    if (dequeue_line(out, out_cap)) return 1;
    while (!extract_line_from_inbuf(out, out_cap)) {
        if (stdin_eof) return 0;
        if (!try_fill_inbuf(1)) {
            if (extract_line_from_inbuf(out, out_cap)) return 1;
            return 0;
        }
    }
    return 1;
}
#endif

/* =========================================================================
 * 8086 Memory Accessors & Parity LUT
 * ========================================================================= */

static void init_parity_lut(void) {
    for (int i = 0; i < 256; i++) {
        int b = 0;
        for (int k = 0; k < 8; k++) {
            b ^= (i >> k) & 1;
        }
        parity_lut[i] = b ? 0 : F_PF;
    }
}

static inline uint32_t lin_addr(uint16_t seg, uint16_t off) {
    return (((uint32_t)seg << 4) + (uint32_t)off) & 0xFFFFFu;
}

static void refresh_bios_ticks_if_needed(uint32_t lin) {
    if (lin >= 0x0046Cu && lin <= 0x0046Fu) {
        uint32_t t = current_bios_ticks();
        ram[0x0046Cu] = (uint8_t)(t & 0xFFu);
        ram[0x0046Du] = (uint8_t)((t >> 8) & 0xFFu);
        ram[0x0046Eu] = (uint8_t)((t >> 16) & 0xFFu);
        ram[0x0046Fu] = (uint8_t)((t >> 24) & 0xFFu);
    }
}

static inline uint8_t mem_r8(uint16_t seg, uint16_t off) {
    uint32_t lin = lin_addr(seg, off);
    if (__builtin_expect(lin >= 0x0046Cu && lin <= 0x0046Fu, 0)) {
        refresh_bios_ticks_if_needed(lin);
    }
    return ram[lin];
}

static inline uint16_t mem_r16(uint16_t seg, uint16_t off) {
    if (__builtin_expect(off != 0xFFFFu, 1)) {
        uint32_t lin = lin_addr(seg, off);
        if (__builtin_expect(lin >= 0x0046Bu && lin <= 0x0046Fu, 0)) {
            refresh_bios_ticks_if_needed(0x0046Cu);
        }
        return (uint16_t)ram[lin] | ((uint16_t)ram[(lin + 1u) & 0xFFFFFu] << 8);
    }
    return (uint16_t)mem_r8(seg, off) | ((uint16_t)mem_r8(seg, (uint16_t)(off + 1u)) << 8);
}

static inline void mem_w8(uint16_t seg, uint16_t off, uint8_t val) {
    ram[lin_addr(seg, off)] = val;
}

static inline void mem_w16(uint16_t seg, uint16_t off, uint16_t val) {
    if (__builtin_expect(off != 0xFFFFu, 1)) {
        uint32_t lin = lin_addr(seg, off);
        ram[lin] = (uint8_t)(val & 0xFFu);
        ram[(lin + 1u) & 0xFFFFFu] = (uint8_t)(val >> 8);
    } else {
        mem_w8(seg, off, (uint8_t)(val & 0xFFu));
        mem_w8(seg, (uint16_t)(off + 1u), (uint8_t)(val >> 8));
    }
}

static inline uint8_t fetch8(void) {
    uint8_t v = ram[lin_addr(cpu.sregs[S_CS], cpu.ip)];
    cpu.ip = (uint16_t)(cpu.ip + 1u);
    return v;
}

static inline uint16_t fetch16(void) {
    uint16_t v = mem_r16(cpu.sregs[S_CS], cpu.ip);
    cpu.ip = (uint16_t)(cpu.ip + 2u);
    return v;
}

static inline void push16(uint16_t val) {
    cpu.regs[R_SP] = (uint16_t)(cpu.regs[R_SP] - 2u);
    mem_w16(cpu.sregs[S_SS], cpu.regs[R_SP], val);
}

static inline uint16_t pop16(void) {
    uint16_t val = mem_r16(cpu.sregs[S_SS], cpu.regs[R_SP]);
    cpu.regs[R_SP] = (uint16_t)(cpu.regs[R_SP] + 2u);
    return val;
}

static inline uint8_t get_r8(int idx) {
    uint16_t w = cpu.regs[idx & 3];
    return (idx & 4) ? (uint8_t)(w >> 8) : (uint8_t)(w & 0xFFu);
}

static inline void set_r8(int idx, uint8_t v) {
    uint16_t *p = &cpu.regs[idx & 3];
    if (idx & 4) {
        *p = (*p & 0x00FFu) | ((uint16_t)v << 8);
    } else {
        *p = (*p & 0xFF00u) | (uint16_t)v;
    }
}

/* =========================================================================
 * 8086 ALU & Flag Helpers
 * ========================================================================= */

static inline void set_szp8(uint8_t v) {
    cpu.flags &= ~(F_SF | F_ZF | F_PF);
    if (v == 0) cpu.flags |= F_ZF;
    if (v & 0x80u) cpu.flags |= F_SF;
    cpu.flags |= parity_lut[v];
}

static inline void set_szp16(uint16_t v) {
    cpu.flags &= ~(F_SF | F_ZF | F_PF);
    if (v == 0) cpu.flags |= F_ZF;
    if (v & 0x8000u) cpu.flags |= F_SF;
    cpu.flags |= parity_lut[v & 0xFFu];
}

static inline uint8_t alu_add8(uint8_t a, uint8_t b, uint8_t cin) {
    uint16_t res = (uint16_t)a + (uint16_t)b + (uint16_t)cin;
    uint8_t r = (uint8_t)res;
    cpu.flags &= ~(F_CF | F_OF | F_AF | F_SF | F_ZF | F_PF);
    if (res > 0xFFu) cpu.flags |= F_CF;
    if (((a ^ r) & (b ^ r) & 0x80u) != 0) cpu.flags |= F_OF;
    if (((a ^ b ^ r) & 0x10u) != 0) cpu.flags |= F_AF;
    if (r == 0) cpu.flags |= F_ZF;
    if (r & 0x80u) cpu.flags |= F_SF;
    cpu.flags |= parity_lut[r];
    return r;
}

static inline uint16_t alu_add16(uint16_t a, uint16_t b, uint16_t cin) {
    uint32_t res = (uint32_t)a + (uint32_t)b + (uint32_t)cin;
    uint16_t r = (uint16_t)res;
    cpu.flags &= ~(F_CF | F_OF | F_AF | F_SF | F_ZF | F_PF);
    if (res > 0xFFFFu) cpu.flags |= F_CF;
    if (((a ^ r) & (b ^ r) & 0x8000u) != 0) cpu.flags |= F_OF;
    if (((a ^ b ^ r) & 0x10u) != 0) cpu.flags |= F_AF;
    if (r == 0) cpu.flags |= F_ZF;
    if (r & 0x8000u) cpu.flags |= F_SF;
    cpu.flags |= parity_lut[r & 0xFFu];
    return r;
}

static inline uint8_t alu_sub8(uint8_t a, uint8_t b, uint8_t cin) {
    uint16_t res = (uint16_t)a - (uint16_t)b - (uint16_t)cin;
    uint8_t r = (uint8_t)res;
    cpu.flags &= ~(F_CF | F_OF | F_AF | F_SF | F_ZF | F_PF);
    if ((uint16_t)a < (uint16_t)b + (uint16_t)cin) cpu.flags |= F_CF;
    if (((a ^ b) & (a ^ r) & 0x80u) != 0) cpu.flags |= F_OF;
    if (((a ^ b ^ r) & 0x10u) != 0) cpu.flags |= F_AF;
    if (r == 0) cpu.flags |= F_ZF;
    if (r & 0x80u) cpu.flags |= F_SF;
    cpu.flags |= parity_lut[r];
    return r;
}

static inline uint16_t alu_sub16(uint16_t a, uint16_t b, uint16_t cin) {
    uint32_t res = (uint32_t)a - (uint32_t)b - (uint32_t)cin;
    uint16_t r = (uint16_t)res;
    cpu.flags &= ~(F_CF | F_OF | F_AF | F_SF | F_ZF | F_PF);
    if ((uint32_t)a < (uint32_t)b + (uint32_t)cin) cpu.flags |= F_CF;
    if (((a ^ b) & (a ^ r) & 0x8000u) != 0) cpu.flags |= F_OF;
    if (((a ^ b ^ r) & 0x10u) != 0) cpu.flags |= F_AF;
    if (r == 0) cpu.flags |= F_ZF;
    if (r & 0x8000u) cpu.flags |= F_SF;
    cpu.flags |= parity_lut[r & 0xFFu];
    return r;
}

static inline uint8_t alu_logic8(uint8_t r) {
    cpu.flags &= ~(F_CF | F_OF | F_AF | F_SF | F_ZF | F_PF);
    if (r == 0) cpu.flags |= F_ZF;
    if (r & 0x80u) cpu.flags |= F_SF;
    cpu.flags |= parity_lut[r];
    return r;
}

static inline uint16_t alu_logic16(uint16_t r) {
    cpu.flags &= ~(F_CF | F_OF | F_AF | F_SF | F_ZF | F_PF);
    if (r == 0) cpu.flags |= F_ZF;
    if (r & 0x8000u) cpu.flags |= F_SF;
    cpu.flags |= parity_lut[r & 0xFFu];
    return r;
}

static inline uint8_t alu_inc8(uint8_t a) {
    uint16_t keep_cf = cpu.flags & F_CF;
    uint8_t r = alu_add8(a, 1u, 0);
    cpu.flags = (cpu.flags & ~F_CF) | keep_cf;
    return r;
}

static inline uint16_t alu_inc16(uint16_t a) {
    uint16_t keep_cf = cpu.flags & F_CF;
    uint16_t r = alu_add16(a, 1u, 0);
    cpu.flags = (cpu.flags & ~F_CF) | keep_cf;
    return r;
}

static inline uint8_t alu_dec8(uint8_t a) {
    uint16_t keep_cf = cpu.flags & F_CF;
    uint8_t r = alu_sub8(a, 1u, 0);
    cpu.flags = (cpu.flags & ~F_CF) | keep_cf;
    return r;
}

static inline uint16_t alu_dec16(uint16_t a) {
    uint16_t keep_cf = cpu.flags & F_CF;
    uint16_t r = alu_sub16(a, 1u, 0);
    cpu.flags = (cpu.flags & ~F_CF) | keep_cf;
    return r;
}

static inline uint8_t alu_op8(int op, uint8_t a, uint8_t b) {
    switch (op) {
        case 0: return alu_add8(a, b, 0);
        case 1: return alu_logic8(a | b);
        case 2: return alu_add8(a, b, (cpu.flags & F_CF) ? 1u : 0u);
        case 3: return alu_sub8(a, b, (cpu.flags & F_CF) ? 1u : 0u);
        case 4: return alu_logic8(a & b);
        case 5: return alu_sub8(a, b, 0);
        case 6: return alu_logic8(a ^ b);
        case 7: (void)alu_sub8(a, b, 0); return a;
    }
    return a;
}

static inline uint16_t alu_op16(int op, uint16_t a, uint16_t b) {
    switch (op) {
        case 0: return alu_add16(a, b, 0);
        case 1: return alu_logic16(a | b);
        case 2: return alu_add16(a, b, (cpu.flags & F_CF) ? 1u : 0u);
        case 3: return alu_sub16(a, b, (cpu.flags & F_CF) ? 1u : 0u);
        case 4: return alu_logic16(a & b);
        case 5: return alu_sub16(a, b, 0);
        case 6: return alu_logic16(a ^ b);
        case 7: (void)alu_sub16(a, b, 0); return a;
    }
    return a;
}

static uint8_t shift8(int op, uint8_t val, uint8_t cnt) {
    if (cnt == 0) return val;
    switch (op) {
        case 0: /* ROL */
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t msb = (val >> 7) & 1u;
                val = (uint8_t)((val << 1) | msb);
                cpu.flags = (cpu.flags & ~F_CF) | (msb ? F_CF : 0u);
            }
            if (cnt == 1) {
                uint8_t msb = (val >> 7) & 1u;
                uint8_t cf = (cpu.flags & F_CF) ? 1u : 0u;
                cpu.flags = (cpu.flags & ~F_OF) | ((msb ^ cf) ? F_OF : 0u);
            }
            return val;
        case 1: /* ROR */
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t lsb = val & 1u;
                val = (uint8_t)((val >> 1) | (lsb << 7));
                cpu.flags = (cpu.flags & ~F_CF) | (lsb ? F_CF : 0u);
            }
            if (cnt == 1) {
                uint8_t b7 = (val >> 7) & 1u;
                uint8_t b6 = (val >> 6) & 1u;
                cpu.flags = (cpu.flags & ~F_OF) | ((b7 ^ b6) ? F_OF : 0u);
            }
            return val;
        case 2: /* RCL */
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t msb = (val >> 7) & 1u;
                uint8_t cf = (cpu.flags & F_CF) ? 1u : 0u;
                val = (uint8_t)((val << 1) | cf);
                cpu.flags = (cpu.flags & ~F_CF) | (msb ? F_CF : 0u);
            }
            if (cnt == 1) {
                uint8_t msb = (val >> 7) & 1u;
                uint8_t cf = (cpu.flags & F_CF) ? 1u : 0u;
                cpu.flags = (cpu.flags & ~F_OF) | ((msb ^ cf) ? F_OF : 0u);
            }
            return val;
        case 3: /* RCR */
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t lsb = val & 1u;
                uint8_t cf = (cpu.flags & F_CF) ? 1u : 0u;
                val = (uint8_t)((val >> 1) | (cf << 7));
                cpu.flags = (cpu.flags & ~F_CF) | (lsb ? F_CF : 0u);
            }
            if (cnt == 1) {
                uint8_t b7 = (val >> 7) & 1u;
                uint8_t b6 = (val >> 6) & 1u;
                cpu.flags = (cpu.flags & ~F_OF) | ((b7 ^ b6) ? F_OF : 0u);
            }
            return val;
        case 4: /* SHL */
        case 6: {
            uint8_t msb = 0;
            for (uint8_t i = 0; i < cnt; i++) {
                msb = (val >> 7) & 1u;
                val = (uint8_t)(val << 1);
            }
            cpu.flags = (cpu.flags & ~(F_CF | F_OF)) | (msb ? F_CF : 0u);
            if (cnt == 1) {
                uint8_t nmsb = (val >> 7) & 1u;
                if (nmsb ^ msb) cpu.flags |= F_OF;
            }
            set_szp8(val);
            return val;
        }
        case 5: { /* SHR */
            uint8_t orig_msb = (val >> 7) & 1u;
            uint8_t lsb = 0;
            for (uint8_t i = 0; i < cnt; i++) {
                lsb = val & 1u;
                val = (uint8_t)(val >> 1);
            }
            cpu.flags = (cpu.flags & ~(F_CF | F_OF)) | (lsb ? F_CF : 0u);
            if (cnt == 1 && orig_msb) cpu.flags |= F_OF;
            set_szp8(val);
            return val;
        }
        case 7: { /* SAR */
            uint8_t sign = val & 0x80u;
            uint8_t lsb = 0;
            for (uint8_t i = 0; i < cnt; i++) {
                lsb = val & 1u;
                val = (uint8_t)((val >> 1) | sign);
            }
            cpu.flags = (cpu.flags & ~(F_CF | F_OF)) | (lsb ? F_CF : 0u);
            set_szp8(val);
            return val;
        }
    }
    return val;
}

static uint16_t shift16(int op, uint16_t val, uint8_t cnt) {
    if (cnt == 0) return val;
    switch (op) {
        case 0: /* ROL */
            for (uint8_t i = 0; i < cnt; i++) {
                uint16_t msb = (val >> 15) & 1u;
                val = (uint16_t)((val << 1) | msb);
                cpu.flags = (cpu.flags & ~F_CF) | (msb ? F_CF : 0u);
            }
            if (cnt == 1) {
                uint16_t msb = (val >> 15) & 1u;
                uint16_t cf = (cpu.flags & F_CF) ? 1u : 0u;
                cpu.flags = (cpu.flags & ~F_OF) | ((msb ^ cf) ? F_OF : 0u);
            }
            return val;
        case 1: /* ROR */
            for (uint8_t i = 0; i < cnt; i++) {
                uint16_t lsb = val & 1u;
                val = (uint16_t)((val >> 1) | (lsb << 15));
                cpu.flags = (cpu.flags & ~F_CF) | (lsb ? F_CF : 0u);
            }
            if (cnt == 1) {
                uint16_t b15 = (val >> 15) & 1u;
                uint16_t b14 = (val >> 14) & 1u;
                cpu.flags = (cpu.flags & ~F_OF) | ((b15 ^ b14) ? F_OF : 0u);
            }
            return val;
        case 2: /* RCL */
            for (uint8_t i = 0; i < cnt; i++) {
                uint16_t msb = (val >> 15) & 1u;
                uint16_t cf = (cpu.flags & F_CF) ? 1u : 0u;
                val = (uint16_t)((val << 1) | cf);
                cpu.flags = (cpu.flags & ~F_CF) | (msb ? F_CF : 0u);
            }
            if (cnt == 1) {
                uint16_t msb = (val >> 15) & 1u;
                uint16_t cf = (cpu.flags & F_CF) ? 1u : 0u;
                cpu.flags = (cpu.flags & ~F_OF) | ((msb ^ cf) ? F_OF : 0u);
            }
            return val;
        case 3: /* RCR */
            for (uint8_t i = 0; i < cnt; i++) {
                uint16_t lsb = val & 1u;
                uint16_t cf = (cpu.flags & F_CF) ? 1u : 0u;
                val = (uint16_t)((val >> 1) | (cf << 15));
                cpu.flags = (cpu.flags & ~F_CF) | (lsb ? F_CF : 0u);
            }
            if (cnt == 1) {
                uint16_t b15 = (val >> 15) & 1u;
                uint16_t b14 = (val >> 14) & 1u;
                cpu.flags = (cpu.flags & ~F_OF) | ((b15 ^ b14) ? F_OF : 0u);
            }
            return val;
        case 4: /* SHL */
        case 6: {
            uint16_t msb = 0;
            for (uint8_t i = 0; i < cnt; i++) {
                msb = (val >> 15) & 1u;
                val = (uint16_t)(val << 1);
            }
            cpu.flags = (cpu.flags & ~(F_CF | F_OF)) | (msb ? F_CF : 0u);
            if (cnt == 1) {
                uint16_t nmsb = (val >> 15) & 1u;
                if (nmsb ^ msb) cpu.flags |= F_OF;
            }
            set_szp16(val);
            return val;
        }
        case 5: { /* SHR */
            uint16_t orig_msb = (val >> 15) & 1u;
            uint16_t lsb = 0;
            for (uint8_t i = 0; i < cnt; i++) {
                lsb = val & 1u;
                val = (uint16_t)(val >> 1);
            }
            cpu.flags = (cpu.flags & ~(F_CF | F_OF)) | (lsb ? F_CF : 0u);
            if (cnt == 1 && orig_msb) cpu.flags |= F_OF;
            set_szp16(val);
            return val;
        }
        case 7: { /* SAR */
            uint16_t sign = val & 0x8000u;
            uint16_t lsb = 0;
            for (uint8_t i = 0; i < cnt; i++) {
                lsb = val & 1u;
                val = (uint16_t)((val >> 1) | sign);
            }
            cpu.flags = (cpu.flags & ~(F_CF | F_OF)) | (lsb ? F_CF : 0u);
            set_szp16(val);
            return val;
        }
    }
    return val;
}

static inline int check_cond(int cc) {
    int cf = (cpu.flags & F_CF) != 0;
    int pf = (cpu.flags & F_PF) != 0;
    int zf = (cpu.flags & F_ZF) != 0;
    int sf = (cpu.flags & F_SF) != 0;
    int of = (cpu.flags & F_OF) != 0;
    switch (cc & 0xF) {
        case 0x0: return of;
        case 0x1: return !of;
        case 0x2: return cf;
        case 0x3: return !cf;
        case 0x4: return zf;
        case 0x5: return !zf;
        case 0x6: return cf || zf;
        case 0x7: return !cf && !zf;
        case 0x8: return sf;
        case 0x9: return !sf;
        case 0xA: return pf;
        case 0xB: return !pf;
        case 0xC: return sf != of;
        case 0xD: return sf == of;
        case 0xE: return zf || (sf != of);
        case 0xF: return !zf && (sf == of);
    }
    return 0;
}

static void decode_modrm(uint8_t modrm, int seg_ovr, int *is_reg, int *rm_idx, uint16_t *seg, uint16_t *off) {
    int mod = (modrm >> 6) & 3;
    int rm  = modrm & 7;
    if (mod == 3) {
        *is_reg = 1;
        *rm_idx = rm;
        *seg = 0;
        *off = 0;
        return;
    }
    *is_reg = 0;
    *rm_idx = rm;
    int def_seg = S_DS;
    uint16_t ea = 0;
    if (mod == 0 && rm == 6) {
        ea = fetch16();
    } else {
        switch (rm) {
            case 0: ea = (uint16_t)(cpu.regs[R_BX] + cpu.regs[R_SI]); break;
            case 1: ea = (uint16_t)(cpu.regs[R_BX] + cpu.regs[R_DI]); break;
            case 2: ea = (uint16_t)(cpu.regs[R_BP] + cpu.regs[R_SI]); def_seg = S_SS; break;
            case 3: ea = (uint16_t)(cpu.regs[R_BP] + cpu.regs[R_DI]); def_seg = S_SS; break;
            case 4: ea = cpu.regs[R_SI]; break;
            case 5: ea = cpu.regs[R_DI]; break;
            case 6: ea = cpu.regs[R_BP]; def_seg = S_SS; break;
            case 7: ea = cpu.regs[R_BX]; break;
        }
        if (mod == 1) {
            ea = (uint16_t)(ea + (int16_t)(int8_t)fetch8());
        } else if (mod == 2) {
            ea = (uint16_t)(ea + fetch16());
        }
    }
    *seg = cpu.sregs[(seg_ovr >= 0) ? seg_ovr : def_seg];
    *off = ea;
}

static inline uint8_t read_rm8(int is_reg, int rm_idx, uint16_t seg, uint16_t off) {
    return is_reg ? get_r8(rm_idx) : mem_r8(seg, off);
}

static inline void write_rm8(int is_reg, int rm_idx, uint16_t seg, uint16_t off, uint8_t v) {
    if (is_reg) set_r8(rm_idx, v);
    else mem_w8(seg, off, v);
}

static inline uint16_t read_rm16(int is_reg, int rm_idx, uint16_t seg, uint16_t off) {
    return is_reg ? cpu.regs[rm_idx] : mem_r16(seg, off);
}

static inline void write_rm16(int is_reg, int rm_idx, uint16_t seg, uint16_t off, uint16_t v) {
    if (is_reg) cpu.regs[rm_idx] = v;
    else mem_w16(seg, off, v);
}

/* =========================================================================
 * Engine Callback Handlers (TRAP_SEG:0x0001 and TRAP_SEG:0x0002)
 * ========================================================================= */

static int mailbox_byte_to_sq(uint8_t b) {
    int idx = ((int)b >> 1) - 25;
    int r = idx / 10;
    int f = idx % 10;
    if (r < 0 || r >= 8 || f < 0 || f >= 8) return -1;
    return r * 8 + f;
}

static void format_sq(int sq, char out[3]) {
    out[0] = (char)('a' + (sq & 7));
    out[1] = (char)('1' + (sq >> 3));
    out[2] = '\0';
}

static void handle_poll_callback(void) {
    ram[ENGINE_BASE + 0x009cu] &= (uint8_t)~0x10u;
    if (searching) {
        if (search_hard_ms > 0) {
            uint64_t elapsed = now_ms() - search_start_ms;
            if (elapsed >= search_hard_ms) {
                stop_requested = 1;
                ram[ENGINE_BASE + 0x009cu] |= 0x80u;
                return;
            }
        }
        poll_stdin_during_search();
        if (stop_requested) {
            ram[ENGINE_BASE + 0x009cu] |= 0x80u;
        }
    }
}

static void handle_info_callback(void) {
    uint8_t ev = ram[ENGINE_BASE + 0x00E3u];
    if (ev == 4) {
        uint16_t d = mem_r16(ENGINE_SEG, 0x00EEu);
        search_depth = (int)d;
    } else if (ev == 2) {
        int16_t raw_score = (int16_t)mem_r16(ENGINE_SEG, 0x00E8u);
        uint16_t pv_off = mem_r16(ENGINE_SEG, 0x00E4u);
        uint64_t elapsed = now_ms() - search_start_ms;
        if (elapsed == 0) elapsed = 1;
        uint64_t nps = (search_nodes * 1000ULL) / elapsed;

        if (raw_score > 0x7D00) {
            int mate_moves = ((0x7F80 - (int)raw_score) >> 2) + 1;
            if (mate_moves < 1) mate_moves = 1;
            printf("info depth %d score mate %d nodes %llu nps %llu time %llu",
                   search_depth, mate_moves,
                   (unsigned long long)search_nodes,
                   (unsigned long long)nps,
                   (unsigned long long)elapsed);
        } else if (raw_score < -0x7D00) {
            int mate_moves = ((0x7F80 + (int)raw_score + 2) >> 2);
            if (mate_moves < 1) mate_moves = 1;
            printf("info depth %d score mate -%d nodes %llu nps %llu time %llu",
                   search_depth, mate_moves,
                   (unsigned long long)search_nodes,
                   (unsigned long long)nps,
                   (unsigned long long)elapsed);
        } else {
            int cp = ((int32_t)raw_score * 100) / 256;
            printf("info depth %d score cp %d nodes %llu nps %llu time %llu",
                   search_depth, cp,
                   (unsigned long long)search_nodes,
                   (unsigned long long)nps,
                   (unsigned long long)elapsed);
        }

        int pv_printed = 0;
        for (int i = 0; i < 32; i++) {
            uint8_t rf = ram[ENGINE_BASE + ((pv_off + (uint16_t)(i * 2)) & 0xFFFFu)];
            uint8_t rt = ram[ENGINE_BASE + ((pv_off + (uint16_t)(i * 2 + 1)) & 0xFFFFu)];
            if (rf == 0 || rt == 0 || rf == rt) break;
            int sf = mailbox_byte_to_sq(rf);
            int st = mailbox_byte_to_sq(rt);
            if (sf < 0 || st < 0) break;
            char fbuf[3], tbuf[3];
            format_sq(sf, fbuf);
            format_sq(st, tbuf);
            if (!pv_printed) {
                printf(" pv");
                pv_printed = 1;
            }
            printf(" %s%s", fbuf, tbuf);
        }
        printf("\n");
        fflush(stdout);
    }
}

/* =========================================================================
 * 8086 Execution Engine
 * ========================================================================= */

static void exec_fritz_call(void) {
    memset(&cpu, 0, sizeof(cpu));
    cpu.sregs[S_CS] = ENGINE_SEG;
    cpu.sregs[S_DS] = ENGINE_SEG;
    cpu.sregs[S_ES] = ENGINE_SEG;
    cpu.sregs[S_SS] = STACK_SEG;
    cpu.regs[R_SP]  = 0xFFFEu;
    cpu.ip          = 0x0100u;
    cpu.flags       = 0x0202u;

    /* Far return target TRAP_SEG:0x0000 */
    push16(TRAP_SEG);
    push16(0x0000u);

    uint32_t insns = 0;

    for (;;) {
        if (__builtin_expect(cpu.sregs[S_CS] == TRAP_SEG, 0)) {
            if (cpu.ip == 0x0000u) {
                break;
            } else if (cpu.ip == 0x0001u) {
                handle_poll_callback();
                cpu.ip = pop16();
                cpu.sregs[S_CS] = pop16();
                continue;
            } else if (cpu.ip == 0x0002u) {
                handle_info_callback();
                cpu.ip = pop16();
                cpu.sregs[S_CS] = pop16();
                continue;
            } else {
                fprintf(stderr, "KnightStalker: unknown trap %04X:%04X\n",
                        cpu.sregs[S_CS], cpu.ip);
                break;
            }
        }

        if (searching) {
            insns++;
            if ((insns & 0x0FFFu) == 0) {
                search_nodes += 64u;
                ram[ENGINE_BASE + 0x009Cu] |= 0x10u;
            }
        }

        uint16_t start_ip = cpu.ip;
        int seg_ovr = -1;
        int rep = 0; /* 1 = REPZ/REP, 2 = REPNZ */
        uint8_t op = fetch8();

        for (int p = 0; p < 8; p++) {
            if (op == 0x26u) { seg_ovr = S_ES; op = fetch8(); }
            else if (op == 0x2Eu) { seg_ovr = S_CS; op = fetch8(); }
            else if (op == 0x36u) { seg_ovr = S_SS; op = fetch8(); }
            else if (op == 0x3Eu) { seg_ovr = S_DS; op = fetch8(); }
            else if (op == 0xF0u) { op = fetch8(); }
            else if (op == 0xF2u) { rep = 2; op = fetch8(); }
            else if (op == 0xF3u) { rep = 1; op = fetch8(); }
            else break;
        }

        int is_reg, rm_idx;
        uint16_t ea_seg, ea_off;

        switch (op) {
            /* 0x00..0x3F: ALU ops */
            case 0x00: case 0x08: case 0x10: case 0x18:
            case 0x20: case 0x28: case 0x30: case 0x38: {
                int aop = (op >> 3) & 7;
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint8_t dst = read_rm8(is_reg, rm_idx, ea_seg, ea_off);
                uint8_t res = alu_op8(aop, dst, get_r8(r));
                if (aop != 7) write_rm8(is_reg, rm_idx, ea_seg, ea_off, res);
                break;
            }
            case 0x01: case 0x09: case 0x11: case 0x19:
            case 0x21: case 0x29: case 0x31: case 0x39: {
                int aop = (op >> 3) & 7;
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint16_t dst = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                uint16_t res = alu_op16(aop, dst, cpu.regs[r]);
                if (aop != 7) write_rm16(is_reg, rm_idx, ea_seg, ea_off, res);
                break;
            }
            case 0x02: case 0x0A: case 0x12: case 0x1A:
            case 0x22: case 0x2A: case 0x32: case 0x3A: {
                int aop = (op >> 3) & 7;
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint8_t src = read_rm8(is_reg, rm_idx, ea_seg, ea_off);
                uint8_t res = alu_op8(aop, get_r8(r), src);
                if (aop != 7) set_r8(r, res);
                break;
            }
            case 0x03: case 0x0B: case 0x13: case 0x1B:
            case 0x23: case 0x2B: case 0x33: case 0x3B: {
                int aop = (op >> 3) & 7;
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint16_t src = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                uint16_t res = alu_op16(aop, cpu.regs[r], src);
                if (aop != 7) cpu.regs[r] = res;
                break;
            }
            case 0x04: case 0x0C: case 0x14: case 0x1C:
            case 0x24: case 0x2C: case 0x34: case 0x3C: {
                int aop = (op >> 3) & 7;
                uint8_t imm = fetch8();
                uint8_t res = alu_op8(aop, get_r8(0), imm);
                if (aop != 7) set_r8(0, res);
                break;
            }
            case 0x05: case 0x0D: case 0x15: case 0x1D:
            case 0x25: case 0x2D: case 0x35: case 0x3D: {
                int aop = (op >> 3) & 7;
                uint16_t imm = fetch16();
                uint16_t res = alu_op16(aop, cpu.regs[R_AX], imm);
                if (aop != 7) cpu.regs[R_AX] = res;
                break;
            }

            /* Segment push/pop */
            case 0x06: push16(cpu.sregs[S_ES]); break;
            case 0x07: cpu.sregs[S_ES] = pop16(); break;
            case 0x0E: push16(cpu.sregs[S_CS]); break;
            case 0x16: push16(cpu.sregs[S_SS]); break;
            case 0x17: cpu.sregs[S_SS] = pop16(); break;
            case 0x1E: push16(cpu.sregs[S_DS]); break;
            case 0x1F: cpu.sregs[S_DS] = pop16(); break;

            /* BCD / ASCII adjust */
            case 0x27: { /* DAA */
                uint8_t al = get_r8(0);
                uint8_t old_al = al;
                uint16_t old_cf = cpu.flags & F_CF;
                cpu.flags &= ~F_CF;
                if ((al & 0x0Fu) > 9u || (cpu.flags & F_AF)) {
                    uint16_t t = (uint16_t)al + 6u;
                    al = (uint8_t)t;
                    if (old_cf || t > 0xFFu) cpu.flags |= F_CF;
                    cpu.flags |= F_AF;
                } else {
                    cpu.flags &= ~F_AF;
                }
                if (old_al > 0x99u || old_cf) {
                    al = (uint8_t)(al + 0x60u);
                    cpu.flags |= F_CF;
                }
                set_r8(0, al);
                set_szp8(al);
                break;
            }
            case 0x2F: { /* DAS */
                uint8_t al = get_r8(0);
                uint8_t old_al = al;
                uint16_t old_cf = cpu.flags & F_CF;
                cpu.flags &= ~F_CF;
                if ((al & 0x0Fu) > 9u || (cpu.flags & F_AF)) {
                    uint16_t t = (uint16_t)al - 6u;
                    al = (uint8_t)t;
                    if (old_cf || t > 0xFFu) cpu.flags |= F_CF;
                    cpu.flags |= F_AF;
                } else {
                    cpu.flags &= ~F_AF;
                }
                if (old_al > 0x99u || old_cf) {
                    al = (uint8_t)(al - 0x60u);
                    cpu.flags |= F_CF;
                }
                set_r8(0, al);
                set_szp8(al);
                break;
            }
            case 0x37: { /* AAA */
                uint8_t al = get_r8(0);
                uint8_t ah = get_r8(4);
                if ((al & 0x0Fu) > 9u || (cpu.flags & F_AF)) {
                    al = (uint8_t)(al + 6u);
                    ah = (uint8_t)(ah + 1u);
                    cpu.flags |= (F_AF | F_CF);
                } else {
                    cpu.flags &= ~(F_AF | F_CF);
                }
                set_r8(0, al & 0x0Fu);
                set_r8(4, ah);
                break;
            }
            case 0x3F: { /* AAS */
                uint8_t al = get_r8(0);
                uint8_t ah = get_r8(4);
                if ((al & 0x0Fu) > 9u || (cpu.flags & F_AF)) {
                    al = (uint8_t)(al - 6u);
                    ah = (uint8_t)(ah - 1u);
                    cpu.flags |= (F_AF | F_CF);
                } else {
                    cpu.flags &= ~(F_AF | F_CF);
                }
                set_r8(0, al & 0x0Fu);
                set_r8(4, ah);
                break;
            }

            /* 0x40..0x4F: INC / DEC r16 */
            case 0x40: case 0x41: case 0x42: case 0x43:
            case 0x44: case 0x45: case 0x46: case 0x47:
                cpu.regs[op & 7] = alu_inc16(cpu.regs[op & 7]);
                break;
            case 0x48: case 0x49: case 0x4A: case 0x4B:
            case 0x4C: case 0x4D: case 0x4E: case 0x4F:
                cpu.regs[op & 7] = alu_dec16(cpu.regs[op & 7]);
                break;

            /* 0x50..0x5F: PUSH / POP r16 */
            case 0x50: case 0x51: case 0x52: case 0x53:
            case 0x55: case 0x56: case 0x57:
                push16(cpu.regs[op & 7]);
                break;
            case 0x54: /* 8086 PUSH SP pushes decremented SP */
                cpu.regs[R_SP] = (uint16_t)(cpu.regs[R_SP] - 2u);
                mem_w16(cpu.sregs[S_SS], cpu.regs[R_SP], cpu.regs[R_SP]);
                break;
            case 0x58: case 0x59: case 0x5A: case 0x5B:
            case 0x5C: case 0x5D: case 0x5E: case 0x5F:
                cpu.regs[op & 7] = pop16();
                break;

            /* 80186 helpers */
            case 0x60: { /* PUSHA */
                uint16_t sp_orig = cpu.regs[R_SP];
                push16(cpu.regs[R_AX]); push16(cpu.regs[R_CX]);
                push16(cpu.regs[R_DX]); push16(cpu.regs[R_BX]);
                push16(sp_orig);        push16(cpu.regs[R_BP]);
                push16(cpu.regs[R_SI]); push16(cpu.regs[R_DI]);
                break;
            }
            case 0x61: { /* POPA */
                cpu.regs[R_DI] = pop16(); cpu.regs[R_SI] = pop16();
                cpu.regs[R_BP] = pop16(); (void)pop16();
                cpu.regs[R_BX] = pop16(); cpu.regs[R_DX] = pop16();
                cpu.regs[R_CX] = pop16(); cpu.regs[R_AX] = pop16();
                break;
            }
            case 0x68: push16(fetch16()); break;
            case 0x6A: push16((uint16_t)(int16_t)(int8_t)fetch8()); break;

            /* 0x70..0x7F: Jcc rel8 */
            case 0x70: case 0x71: case 0x72: case 0x73:
            case 0x74: case 0x75: case 0x76: case 0x77:
            case 0x78: case 0x79: case 0x7A: case 0x7B:
            case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
                int8_t rel = (int8_t)fetch8();
                if (check_cond(op & 0x0F)) {
                    cpu.ip = (uint16_t)((int32_t)cpu.ip + (int32_t)rel);
                }
                break;
            }

            /* 0x80..0x83: Group 1 imm */
            case 0x80:
            case 0x82: {
                uint8_t m = fetch8();
                int aop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint8_t dst = read_rm8(is_reg, rm_idx, ea_seg, ea_off);
                uint8_t imm = fetch8();
                uint8_t res = alu_op8(aop, dst, imm);
                if (aop != 7) write_rm8(is_reg, rm_idx, ea_seg, ea_off, res);
                break;
            }
            case 0x81: {
                uint8_t m = fetch8();
                int aop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint16_t dst = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                uint16_t imm = fetch16();
                uint16_t res = alu_op16(aop, dst, imm);
                if (aop != 7) write_rm16(is_reg, rm_idx, ea_seg, ea_off, res);
                break;
            }
            case 0x83: {
                uint8_t m = fetch8();
                int aop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint16_t dst = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                uint16_t imm = (uint16_t)(int16_t)(int8_t)fetch8();
                uint16_t res = alu_op16(aop, dst, imm);
                if (aop != 7) write_rm16(is_reg, rm_idx, ea_seg, ea_off, res);
                break;
            }

            /* TEST / XCHG / MOV / LEA */
            case 0x84: {
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                (void)alu_logic8(read_rm8(is_reg, rm_idx, ea_seg, ea_off) & get_r8(r));
                break;
            }
            case 0x85: {
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                (void)alu_logic16(read_rm16(is_reg, rm_idx, ea_seg, ea_off) & cpu.regs[r]);
                break;
            }
            case 0x86: {
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint8_t a = read_rm8(is_reg, rm_idx, ea_seg, ea_off);
                uint8_t b = get_r8(r);
                set_r8(r, a);
                write_rm8(is_reg, rm_idx, ea_seg, ea_off, b);
                break;
            }
            case 0x87: {
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint16_t a = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                uint16_t b = cpu.regs[r];
                cpu.regs[r] = a;
                write_rm16(is_reg, rm_idx, ea_seg, ea_off, b);
                break;
            }
            case 0x88: {
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                write_rm8(is_reg, rm_idx, ea_seg, ea_off, get_r8(r));
                break;
            }
            case 0x89: {
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                write_rm16(is_reg, rm_idx, ea_seg, ea_off, cpu.regs[r]);
                break;
            }
            case 0x8A: {
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                set_r8(r, read_rm8(is_reg, rm_idx, ea_seg, ea_off));
                break;
            }
            case 0x8B: {
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                cpu.regs[r] = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                break;
            }
            case 0x8C: {
                uint8_t m = fetch8();
                int sr = (m >> 3) & 3;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                write_rm16(is_reg, rm_idx, ea_seg, ea_off, cpu.sregs[sr]);
                break;
            }
            case 0x8D: { /* LEA */
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                cpu.regs[r] = ea_off;
                break;
            }
            case 0x8E: {
                uint8_t m = fetch8();
                int sr = (m >> 3) & 3;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                cpu.sregs[sr] = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                break;
            }
            case 0x8F: {
                uint8_t m = fetch8();
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                write_rm16(is_reg, rm_idx, ea_seg, ea_off, pop16());
                break;
            }

            /* 0x90..0x9F */
            case 0x90: break; /* NOP */
            case 0x91: case 0x92: case 0x93:
            case 0x94: case 0x95: case 0x96: case 0x97: {
                int r = op & 7;
                uint16_t t = cpu.regs[R_AX];
                cpu.regs[R_AX] = cpu.regs[r];
                cpu.regs[r] = t;
                break;
            }
            case 0x98: /* CBW */
                cpu.regs[R_AX] = (uint16_t)(int16_t)(int8_t)(cpu.regs[R_AX] & 0xFFu);
                break;
            case 0x99: /* CWD */
                cpu.regs[R_DX] = (cpu.regs[R_AX] & 0x8000u) ? 0xFFFFu : 0x0000u;
                break;
            case 0x9A: { /* CALL FAR ptr16:16 */
                uint16_t new_ip = fetch16();
                uint16_t new_cs = fetch16();
                push16(cpu.sregs[S_CS]);
                push16(cpu.ip);
                cpu.sregs[S_CS] = new_cs;
                cpu.ip = new_ip;
                break;
            }
            case 0x9C: push16(cpu.flags | 0xF002u); break;
            case 0x9D: cpu.flags = (pop16() & 0x0FD5u) | 0x0002u; break;
            case 0x9E: cpu.flags = (cpu.flags & 0xFF00u) | (uint16_t)((cpu.regs[R_AX] >> 8) & 0xD5u) | 0x0002u; break;
            case 0x9F: set_r8(4, (uint8_t)((cpu.flags & 0xD5u) | 0x02u)); break;

            /* 0xA0..0xA3: MOV AL/AX, moffs */
            case 0xA0: {
                uint16_t off = fetch16();
                set_r8(0, mem_r8(cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS], off));
                break;
            }
            case 0xA1: {
                uint16_t off = fetch16();
                cpu.regs[R_AX] = mem_r16(cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS], off);
                break;
            }
            case 0xA2: {
                uint16_t off = fetch16();
                mem_w8(cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS], off, get_r8(0));
                break;
            }
            case 0xA3: {
                uint16_t off = fetch16();
                mem_w16(cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS], off, cpu.regs[R_AX]);
                break;
            }

            /* 0xA4..0xAF: String ops */
            case 0xA4: { /* MOVSB */
                uint16_t sseg = cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS];
                int16_t step = (cpu.flags & F_DF) ? -1 : 1;
                uint16_t cnt = rep ? cpu.regs[R_CX] : 1u;
                while (cnt) {
                    mem_w8(cpu.sregs[S_ES], cpu.regs[R_DI], mem_r8(sseg, cpu.regs[R_SI]));
                    cpu.regs[R_SI] = (uint16_t)(cpu.regs[R_SI] + step);
                    cpu.regs[R_DI] = (uint16_t)(cpu.regs[R_DI] + step);
                    cnt--;
                }
                if (rep) cpu.regs[R_CX] = 0;
                break;
            }
            case 0xA5: { /* MOVSW */
                uint16_t sseg = cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS];
                int16_t step = (cpu.flags & F_DF) ? -2 : 2;
                uint16_t cnt = rep ? cpu.regs[R_CX] : 1u;
                while (cnt) {
                    mem_w16(cpu.sregs[S_ES], cpu.regs[R_DI], mem_r16(sseg, cpu.regs[R_SI]));
                    cpu.regs[R_SI] = (uint16_t)(cpu.regs[R_SI] + step);
                    cpu.regs[R_DI] = (uint16_t)(cpu.regs[R_DI] + step);
                    cnt--;
                }
                if (rep) cpu.regs[R_CX] = 0;
                break;
            }
            case 0xA6: { /* CMPSB */
                uint16_t sseg = cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS];
                int16_t step = (cpu.flags & F_DF) ? -1 : 1;
                uint16_t cnt = rep ? cpu.regs[R_CX] : 1u;
                while (cnt) {
                    uint8_t a = mem_r8(sseg, cpu.regs[R_SI]);
                    uint8_t b = mem_r8(cpu.sregs[S_ES], cpu.regs[R_DI]);
                    (void)alu_sub8(a, b, 0);
                    cpu.regs[R_SI] = (uint16_t)(cpu.regs[R_SI] + step);
                    cpu.regs[R_DI] = (uint16_t)(cpu.regs[R_DI] + step);
                    cnt--;
                    if (rep == 1 && !(cpu.flags & F_ZF)) break;
                    if (rep == 2 && (cpu.flags & F_ZF)) break;
                }
                if (rep) cpu.regs[R_CX] = cnt;
                break;
            }
            case 0xA7: { /* CMPSW */
                uint16_t sseg = cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS];
                int16_t step = (cpu.flags & F_DF) ? -2 : 2;
                uint16_t cnt = rep ? cpu.regs[R_CX] : 1u;
                while (cnt) {
                    uint16_t a = mem_r16(sseg, cpu.regs[R_SI]);
                    uint16_t b = mem_r16(cpu.sregs[S_ES], cpu.regs[R_DI]);
                    (void)alu_sub16(a, b, 0);
                    cpu.regs[R_SI] = (uint16_t)(cpu.regs[R_SI] + step);
                    cpu.regs[R_DI] = (uint16_t)(cpu.regs[R_DI] + step);
                    cnt--;
                    if (rep == 1 && !(cpu.flags & F_ZF)) break;
                    if (rep == 2 && (cpu.flags & F_ZF)) break;
                }
                if (rep) cpu.regs[R_CX] = cnt;
                break;
            }
            case 0xA8: (void)alu_logic8(get_r8(0) & fetch8()); break;
            case 0xA9: (void)alu_logic16(cpu.regs[R_AX] & fetch16()); break;
            case 0xAA: { /* STOSB */
                int16_t step = (cpu.flags & F_DF) ? -1 : 1;
                uint16_t cnt = rep ? cpu.regs[R_CX] : 1u;
                uint8_t al = get_r8(0);
                while (cnt) {
                    mem_w8(cpu.sregs[S_ES], cpu.regs[R_DI], al);
                    cpu.regs[R_DI] = (uint16_t)(cpu.regs[R_DI] + step);
                    cnt--;
                }
                if (rep) cpu.regs[R_CX] = 0;
                break;
            }
            case 0xAB: { /* STOSW */
                int16_t step = (cpu.flags & F_DF) ? -2 : 2;
                uint16_t cnt = rep ? cpu.regs[R_CX] : 1u;
                uint16_t ax = cpu.regs[R_AX];
                while (cnt) {
                    mem_w16(cpu.sregs[S_ES], cpu.regs[R_DI], ax);
                    cpu.regs[R_DI] = (uint16_t)(cpu.regs[R_DI] + step);
                    cnt--;
                }
                if (rep) cpu.regs[R_CX] = 0;
                break;
            }
            case 0xAC: { /* LODSB */
                uint16_t sseg = cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS];
                int16_t step = (cpu.flags & F_DF) ? -1 : 1;
                uint16_t cnt = rep ? cpu.regs[R_CX] : 1u;
                while (cnt) {
                    set_r8(0, mem_r8(sseg, cpu.regs[R_SI]));
                    cpu.regs[R_SI] = (uint16_t)(cpu.regs[R_SI] + step);
                    cnt--;
                }
                if (rep) cpu.regs[R_CX] = 0;
                break;
            }
            case 0xAD: { /* LODSW */
                uint16_t sseg = cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS];
                int16_t step = (cpu.flags & F_DF) ? -2 : 2;
                uint16_t cnt = rep ? cpu.regs[R_CX] : 1u;
                while (cnt) {
                    cpu.regs[R_AX] = mem_r16(sseg, cpu.regs[R_SI]);
                    cpu.regs[R_SI] = (uint16_t)(cpu.regs[R_SI] + step);
                    cnt--;
                }
                if (rep) cpu.regs[R_CX] = 0;
                break;
            }
            case 0xAE: { /* SCASB */
                int16_t step = (cpu.flags & F_DF) ? -1 : 1;
                uint16_t cnt = rep ? cpu.regs[R_CX] : 1u;
                uint8_t al = get_r8(0);
                while (cnt) {
                    (void)alu_sub8(al, mem_r8(cpu.sregs[S_ES], cpu.regs[R_DI]), 0);
                    cpu.regs[R_DI] = (uint16_t)(cpu.regs[R_DI] + step);
                    cnt--;
                    if (rep == 1 && !(cpu.flags & F_ZF)) break;
                    if (rep == 2 && (cpu.flags & F_ZF)) break;
                }
                if (rep) cpu.regs[R_CX] = cnt;
                break;
            }
            case 0xAF: { /* SCASW */
                int16_t step = (cpu.flags & F_DF) ? -2 : 2;
                uint16_t cnt = rep ? cpu.regs[R_CX] : 1u;
                uint16_t ax = cpu.regs[R_AX];
                while (cnt) {
                    (void)alu_sub16(ax, mem_r16(cpu.sregs[S_ES], cpu.regs[R_DI]), 0);
                    cpu.regs[R_DI] = (uint16_t)(cpu.regs[R_DI] + step);
                    cnt--;
                    if (rep == 1 && !(cpu.flags & F_ZF)) break;
                    if (rep == 2 && (cpu.flags & F_ZF)) break;
                }
                if (rep) cpu.regs[R_CX] = cnt;
                break;
            }

            /* 0xB0..0xBF: MOV reg, imm */
            case 0xB0: case 0xB1: case 0xB2: case 0xB3:
            case 0xB4: case 0xB5: case 0xB6: case 0xB7:
                set_r8(op & 7, fetch8());
                break;
            case 0xB8: case 0xB9: case 0xBA: case 0xBB:
            case 0xBC: case 0xBD: case 0xBE: case 0xBF:
                cpu.regs[op & 7] = fetch16();
                break;

            /* Shifts with imm8 (80186) and RET / LES / LDS / MOV imm */
            case 0xC0: {
                uint8_t m = fetch8();
                int sop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint8_t cnt = fetch8();
                uint8_t v = read_rm8(is_reg, rm_idx, ea_seg, ea_off);
                write_rm8(is_reg, rm_idx, ea_seg, ea_off, shift8(sop, v, cnt));
                break;
            }
            case 0xC1: {
                uint8_t m = fetch8();
                int sop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint8_t cnt = fetch8();
                uint16_t v = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                write_rm16(is_reg, rm_idx, ea_seg, ea_off, shift16(sop, v, cnt));
                break;
            }
            case 0xC2: {
                uint16_t imm = fetch16();
                cpu.ip = pop16();
                cpu.regs[R_SP] = (uint16_t)(cpu.regs[R_SP] + imm);
                break;
            }
            case 0xC3:
                cpu.ip = pop16();
                break;
            case 0xC4: { /* LES */
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                cpu.regs[r] = mem_r16(ea_seg, ea_off);
                cpu.sregs[S_ES] = mem_r16(ea_seg, (uint16_t)(ea_off + 2u));
                break;
            }
            case 0xC5: { /* LDS */
                uint8_t m = fetch8();
                int r = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                cpu.regs[r] = mem_r16(ea_seg, ea_off);
                cpu.sregs[S_DS] = mem_r16(ea_seg, (uint16_t)(ea_off + 2u));
                break;
            }
            case 0xC6: {
                uint8_t m = fetch8();
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                write_rm8(is_reg, rm_idx, ea_seg, ea_off, fetch8());
                break;
            }
            case 0xC7: {
                uint8_t m = fetch8();
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                write_rm16(is_reg, rm_idx, ea_seg, ea_off, fetch16());
                break;
            }
            case 0xCA: {
                uint16_t imm = fetch16();
                cpu.ip = pop16();
                cpu.sregs[S_CS] = pop16();
                cpu.regs[R_SP] = (uint16_t)(cpu.regs[R_SP] + imm);
                break;
            }
            case 0xCB:
                cpu.ip = pop16();
                cpu.sregs[S_CS] = pop16();
                break;
            case 0xCD: { /* INT imm8 */
                uint8_t vec = fetch8();
                if (vec == 0x16u) {
                    if (get_r8(4) == 0x01u) {
                        /* Keyboard status check: ZF = 1 -> no key waiting */
                        cpu.flags |= F_ZF;
                    } else if (get_r8(4) == 0x00u) {
                        cpu.regs[R_AX] = 0;
                    }
                } else if (vec == 0x1Au && get_r8(4) == 0x00u) {
                    uint32_t t = current_bios_ticks();
                    cpu.regs[R_CX] = (uint16_t)(t >> 16);
                    cpu.regs[R_DX] = (uint16_t)(t & 0xFFFFu);
                    set_r8(0, 0);
                }
                break;
            }
            case 0xCF: { /* IRET */
                cpu.ip = pop16();
                cpu.sregs[S_CS] = pop16();
                cpu.flags = (pop16() & 0x0FD5u) | 0x0002u;
                break;
            }

            /* Shifts / Rotates 0xD0..0xD3 */
            case 0xD0: case 0xD2: {
                uint8_t m = fetch8();
                int sop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint8_t cnt = (op == 0xD0u) ? 1u : get_r8(1);
                uint8_t v = read_rm8(is_reg, rm_idx, ea_seg, ea_off);
                write_rm8(is_reg, rm_idx, ea_seg, ea_off, shift8(sop, v, cnt));
                break;
            }
            case 0xD1: case 0xD3: {
                uint8_t m = fetch8();
                int sop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint8_t cnt = (op == 0xD1u) ? 1u : get_r8(1);
                uint16_t v = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                write_rm16(is_reg, rm_idx, ea_seg, ea_off, shift16(sop, v, cnt));
                break;
            }
            case 0xD4: { /* AAM */
                uint8_t base = fetch8();
                if (base) {
                    uint8_t al = get_r8(0);
                    set_r8(4, (uint8_t)(al / base));
                    set_r8(0, (uint8_t)(al % base));
                    set_szp8(get_r8(0));
                }
                break;
            }
            case 0xD5: { /* AAD */
                uint8_t base = fetch8();
                uint8_t al = get_r8(0);
                uint8_t ah = get_r8(4);
                uint8_t res = (uint8_t)(al + ah * base);
                cpu.regs[R_AX] = res;
                set_szp8(res);
                break;
            }
            case 0xD7: { /* XLAT */
                uint16_t sseg = cpu.sregs[(seg_ovr >= 0) ? seg_ovr : S_DS];
                uint16_t off = (uint16_t)(cpu.regs[R_BX] + (uint16_t)get_r8(0));
                set_r8(0, mem_r8(sseg, off));
                break;
            }

            /* Loops & short/near jumps */
            case 0xE0: { /* LOOPNE */
                int8_t rel = (int8_t)fetch8();
                cpu.regs[R_CX] = (uint16_t)(cpu.regs[R_CX] - 1u);
                if (cpu.regs[R_CX] != 0 && !(cpu.flags & F_ZF)) {
                    cpu.ip = (uint16_t)((int32_t)cpu.ip + (int32_t)rel);
                }
                break;
            }
            case 0xE1: { /* LOOPE */
                int8_t rel = (int8_t)fetch8();
                cpu.regs[R_CX] = (uint16_t)(cpu.regs[R_CX] - 1u);
                if (cpu.regs[R_CX] != 0 && (cpu.flags & F_ZF)) {
                    cpu.ip = (uint16_t)((int32_t)cpu.ip + (int32_t)rel);
                }
                break;
            }
            case 0xE2: { /* LOOP */
                int8_t rel = (int8_t)fetch8();
                cpu.regs[R_CX] = (uint16_t)(cpu.regs[R_CX] - 1u);
                if (cpu.regs[R_CX] != 0) {
                    cpu.ip = (uint16_t)((int32_t)cpu.ip + (int32_t)rel);
                }
                break;
            }
            case 0xE3: { /* JCXZ */
                int8_t rel = (int8_t)fetch8();
                if (cpu.regs[R_CX] == 0) {
                    cpu.ip = (uint16_t)((int32_t)cpu.ip + (int32_t)rel);
                }
                break;
            }
            case 0xE8: { /* CALL rel16 */
                int16_t rel = (int16_t)fetch16();
                push16(cpu.ip);
                cpu.ip = (uint16_t)((int32_t)cpu.ip + (int32_t)rel);
                break;
            }
            case 0xE9: { /* JMP rel16 */
                int16_t rel = (int16_t)fetch16();
                cpu.ip = (uint16_t)((int32_t)cpu.ip + (int32_t)rel);
                break;
            }
            case 0xEA: { /* JMP FAR ptr16:16 */
                uint16_t new_ip = fetch16();
                uint16_t new_cs = fetch16();
                cpu.sregs[S_CS] = new_cs;
                cpu.ip = new_ip;
                break;
            }
            case 0xEB: { /* JMP rel8 */
                int8_t rel = (int8_t)fetch8();
                cpu.ip = (uint16_t)((int32_t)cpu.ip + (int32_t)rel);
                break;
            }

            /* 0xF5..0xF9 flags */
            case 0xF5: cpu.flags ^= F_CF; break; /* CMC */
            case 0xF6: { /* Group 3 rm8 */
                uint8_t m = fetch8();
                int sop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint8_t v = read_rm8(is_reg, rm_idx, ea_seg, ea_off);
                switch (sop) {
                    case 0:
                    case 1:
                        (void)alu_logic8(v & fetch8());
                        break;
                    case 2:
                        write_rm8(is_reg, rm_idx, ea_seg, ea_off, (uint8_t)~v);
                        break;
                    case 3: {
                        uint8_t r = alu_sub8(0, v, 0);
                        write_rm8(is_reg, rm_idx, ea_seg, ea_off, r);
                        break;
                    }
                    case 4: { /* MUL rm8 */
                        uint16_t res = (uint16_t)get_r8(0) * (uint16_t)v;
                        cpu.regs[R_AX] = res;
                        if (res & 0xFF00u) cpu.flags |= (F_CF | F_OF);
                        else cpu.flags &= ~(F_CF | F_OF);
                        break;
                    }
                    case 5: { /* IMUL rm8 */
                        int16_t res = (int16_t)(int8_t)get_r8(0) * (int16_t)(int8_t)v;
                        cpu.regs[R_AX] = (uint16_t)res;
                        if (res != (int16_t)(int8_t)res) cpu.flags |= (F_CF | F_OF);
                        else cpu.flags &= ~(F_CF | F_OF);
                        break;
                    }
                    case 6: { /* DIV rm8 */
                        if (v != 0) {
                            uint16_t num = cpu.regs[R_AX];
                            uint16_t q = num / v;
                            uint16_t r = num % v;
                            if (q <= 0xFFu) {
                                set_r8(0, (uint8_t)q);
                                set_r8(4, (uint8_t)r);
                            }
                        }
                        break;
                    }
                    case 7: { /* IDIV rm8 */
                        if (v != 0) {
                            int16_t num = (int16_t)cpu.regs[R_AX];
                            int16_t den = (int16_t)(int8_t)v;
                            int16_t q = num / den;
                            int16_t r = num % den;
                            if (q >= -128 && q <= 127) {
                                set_r8(0, (uint8_t)(int8_t)q);
                                set_r8(4, (uint8_t)(int8_t)r);
                            }
                        }
                        break;
                    }
                }
                break;
            }
            case 0xF7: { /* Group 3 rm16 */
                uint8_t m = fetch8();
                int sop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint16_t v = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                switch (sop) {
                    case 0:
                    case 1:
                        (void)alu_logic16(v & fetch16());
                        break;
                    case 2:
                        write_rm16(is_reg, rm_idx, ea_seg, ea_off, (uint16_t)~v);
                        break;
                    case 3: {
                        uint16_t r = alu_sub16(0, v, 0);
                        write_rm16(is_reg, rm_idx, ea_seg, ea_off, r);
                        break;
                    }
                    case 4: { /* MUL rm16 */
                        uint32_t res = (uint32_t)cpu.regs[R_AX] * (uint32_t)v;
                        cpu.regs[R_AX] = (uint16_t)(res & 0xFFFFu);
                        cpu.regs[R_DX] = (uint16_t)(res >> 16);
                        if (cpu.regs[R_DX] != 0) cpu.flags |= (F_CF | F_OF);
                        else cpu.flags &= ~(F_CF | F_OF);
                        break;
                    }
                    case 5: { /* IMUL rm16 */
                        int32_t res = (int32_t)(int16_t)cpu.regs[R_AX] * (int32_t)(int16_t)v;
                        cpu.regs[R_AX] = (uint16_t)(res & 0xFFFF);
                        cpu.regs[R_DX] = (uint16_t)(res >> 16);
                        if (res != (int32_t)(int16_t)res) cpu.flags |= (F_CF | F_OF);
                        else cpu.flags &= ~(F_CF | F_OF);
                        break;
                    }
                    case 6: { /* DIV rm16 */
                        if (v != 0) {
                            uint32_t num = ((uint32_t)cpu.regs[R_DX] << 16) | cpu.regs[R_AX];
                            uint32_t q = num / v;
                            uint32_t r = num % v;
                            if (q <= 0xFFFFu) {
                                cpu.regs[R_AX] = (uint16_t)q;
                                cpu.regs[R_DX] = (uint16_t)r;
                            }
                        }
                        break;
                    }
                    case 7: { /* IDIV rm16 */
                        if (v != 0) {
                            int32_t num = (int32_t)(((uint32_t)cpu.regs[R_DX] << 16) | cpu.regs[R_AX]);
                            int32_t den = (int32_t)(int16_t)v;
                            int32_t q = num / den;
                            int32_t r = num % den;
                            if (q >= -32768 && q <= 32767) {
                                cpu.regs[R_AX] = (uint16_t)(int16_t)q;
                                cpu.regs[R_DX] = (uint16_t)(int16_t)r;
                            }
                        }
                        break;
                    }
                }
                break;
            }
            case 0xF8: cpu.flags &= ~F_CF; break; /* CLC */
            case 0xF9: cpu.flags |= F_CF;  break; /* STC */
            case 0xFA: cpu.flags &= ~F_IF; break; /* CLI */
            case 0xFB: cpu.flags |= F_IF;  break; /* STI */
            case 0xFC: cpu.flags &= ~F_DF; break; /* CLD */
            case 0xFD: cpu.flags |= F_DF;  break; /* STD */

            case 0xFE: { /* Group 4 rm8 */
                uint8_t m = fetch8();
                int sop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                uint8_t v = read_rm8(is_reg, rm_idx, ea_seg, ea_off);
                if (sop == 0) write_rm8(is_reg, rm_idx, ea_seg, ea_off, alu_inc8(v));
                else if (sop == 1) write_rm8(is_reg, rm_idx, ea_seg, ea_off, alu_dec8(v));
                break;
            }
            case 0xFF: { /* Group 5 */
                uint8_t m = fetch8();
                int sop = (m >> 3) & 7;
                decode_modrm(m, seg_ovr, &is_reg, &rm_idx, &ea_seg, &ea_off);
                switch (sop) {
                    case 0:
                        write_rm16(is_reg, rm_idx, ea_seg, ea_off,
                                   alu_inc16(read_rm16(is_reg, rm_idx, ea_seg, ea_off)));
                        break;
                    case 1:
                        write_rm16(is_reg, rm_idx, ea_seg, ea_off,
                                   alu_dec16(read_rm16(is_reg, rm_idx, ea_seg, ea_off)));
                        break;
                    case 2: { /* CALL near rm16 */
                        uint16_t target = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                        push16(cpu.ip);
                        cpu.ip = target;
                        break;
                    }
                    case 3: { /* CALL far m16:16 */
                        uint16_t target_ip = mem_r16(ea_seg, ea_off);
                        uint16_t target_cs = mem_r16(ea_seg, (uint16_t)(ea_off + 2u));
                        push16(cpu.sregs[S_CS]);
                        push16(cpu.ip);
                        cpu.sregs[S_CS] = target_cs;
                        cpu.ip = target_ip;
                        break;
                    }
                    case 4: /* JMP near rm16 */
                        cpu.ip = read_rm16(is_reg, rm_idx, ea_seg, ea_off);
                        break;
                    case 5: { /* JMP far m16:16 */
                        uint16_t target_ip = mem_r16(ea_seg, ea_off);
                        uint16_t target_cs = mem_r16(ea_seg, (uint16_t)(ea_off + 2u));
                        cpu.sregs[S_CS] = target_cs;
                        cpu.ip = target_ip;
                        break;
                    }
                    case 6: /* PUSH rm16 */
                        push16(read_rm16(is_reg, rm_idx, ea_seg, ea_off));
                        break;
                }
                break;
            }
            default:
                fprintf(stderr, "KnightStalker: unimplemented opcode 0x%02X at %04X:%04X\n",
                        op, cpu.sregs[S_CS], start_ip);
                return;
        }
    }
}

/* =========================================================================
 * FRITZ.BIN High-Level Driver
 * ========================================================================= */

static void write_book_descriptor(void) {
    mem_w16(ENGINE_SEG, 0x00F0u, 0x0000u);
    mem_w16(ENGINE_SEG, 0x00F2u, BOOK_SEG);
    mem_w16(ENGINE_SEG, 0x00F4u, (uint16_t)FRITZ_FBK_SIZE);
}

static void engine_cmd(uint8_t cmd) {
    ram[ENGINE_BASE + 0x0080u] = cmd;
    exec_fritz_call();
}

static void apply_book_option(int enable) {
    ram[ENGINE_BASE + 0x0081u] = enable ? 1u : 0u;
    engine_cmd(0x0Cu);
}

static void engine_init(void) {
    memset(ram, 0, sizeof(ram));
    memcpy(ram + ENGINE_BASE + 0x0100u, fritz_bin_data, FRITZ_BIN_SIZE);
    memcpy(ram + BOOK_BASE, fritz_fbk_data, FRITZ_FBK_SIZE);

    /* Abort/poll callback (0x0086) = TRAP_SEG:0x0001 */
    mem_w16(ENGINE_SEG, 0x0086u, 0x0001u);
    mem_w16(ENGINE_SEG, 0x0088u, TRAP_SEG);

    /* Info callback (0x008A) = TRAP_SEG:0x0002 */
    mem_w16(ENGINE_SEG, 0x008Au, 0x0002u);
    mem_w16(ENGINE_SEG, 0x008Cu, TRAP_SEG);

    ram[ENGINE_BASE + 0x009Eu] = 30u;
    engine_cmd(0xFFu);
    write_book_descriptor();
    apply_book_option(opt_own_book);
    pos_is_startpos = 1;
}

static int parse_square(const char *s) {
    if (!s || s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return -1;
    return (s[1] - '1') * 8 + (s[0] - 'a');
}

static int apply_uci_move(const char *m) {
    int from = parse_square(m);
    int to = parse_square(m + 2);
    if (from < 0 || to < 0) return 0;

    uint8_t promo = 0x00u;
    if (m[4]) {
        switch (tolower((unsigned char)m[4])) {
            case 'q': promo = 0x80u; break;
            case 'r': promo = 0x81u; break;
            case 'b': promo = 0xC0u; break;
            case 'n': promo = 0xC1u; break;
            default:  promo = 0x00u; break;
        }
    }
    ram[ENGINE_BASE + 0x0082u] = (uint8_t)from;
    ram[ENGINE_BASE + 0x0083u] = (uint8_t)to;
    ram[ENGINE_BASE + 0x0084u] = promo;
    engine_cmd(0x02u);
    return ram[ENGINE_BASE + 0x0081u] == 0;
}

static void engine_new_game(void) {
    engine_cmd(0x00u);
    write_book_descriptor();
    apply_book_option(opt_own_book);
    pos_is_startpos = 1;
}

static int is_standard_startpos_fen(const char *board, const char *stm,
                                    const char *castle, const char *ep) {
    return strcmp(board, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR") == 0 &&
           strcmp(stm, "w") == 0 &&
           strcmp(castle, "KQkq") == 0 &&
           strcmp(ep, "-") == 0;
}

static void set_fen_position(const char *board, const char *stm,
                             const char *castle, const char *ep) {
    /* Reset full internal state first so history/hash start clean */
    engine_new_game();

    if (is_standard_startpos_fen(board, stm, castle, ep)) {
        pos_is_startpos = 1;
        return;
    }

    pos_is_startpos = 0;

    uint8_t b64[64];
    memset(b64, 0, sizeof(b64));

    int r = 7;
    int f = 0;
    for (const char *p = board; *p; p++) {
        if (*p == '/') {
            r--;
            f = 0;
            continue;
        }
        if (*p >= '1' && *p <= '8') {
            f += (*p - '0');
            continue;
        }
        if (r < 0 || r > 7 || f < 0 || f > 7) continue;
        int sq = r * 8 + f;
        uint8_t code = 0x00u;
        switch (*p) {
            case 'P': code = 0x10u; break;
            case 'N': code = 0x12u; break;
            case 'B': code = 0x14u; break;
            case 'R': code = 0x18u; break; /* moved unless castling right applies */
            case 'Q': code = 0x1Au; break;
            case 'K': code = 0x1Eu; break;
            case 'p': code = 0x20u; break;
            case 'n': code = 0x22u; break;
            case 'b': code = 0x24u; break;
            case 'r': code = 0x28u; break;
            case 'q': code = 0x2Au; break;
            case 'k': code = 0x2Cu; break;
        }
        b64[sq] = code;
        f++;
    }

    int wk = (strchr(castle, 'K') != NULL);
    int wq = (strchr(castle, 'Q') != NULL);
    int bk = (strchr(castle, 'k') != NULL);
    int bq = (strchr(castle, 'q') != NULL);

    if (b64[4] == 0x1Eu && (wk || wq)) b64[4] = 0x1Cu;
    if (b64[7] == 0x18u && wk) b64[7] = 0x16u;
    if (b64[0] == 0x18u && wq) b64[0] = 0x16u;

    if (b64[60] == 0x2Cu || b64[60] == 0x2Eu) {
        b64[60] = (bk || bq) ? 0x2Cu : 0x2Eu;
    }
    if (b64[63] == 0x28u && bk) b64[63] = 0x26u;
    if (b64[56] == 0x28u && bq) b64[56] = 0x26u;

    memcpy(ram + ENGINE_BASE + 0x00A0u, b64, 64);

    uint8_t side = (stm[0] == 'b' || stm[0] == 'B') ? 1u : 0u;
    ram[ENGINE_BASE + 0x00E0u] = side;

    uint8_t ep_code = 0x00u;
    if (ep[0] >= 'a' && ep[0] <= 'h' && ep[1] >= '1' && ep[1] <= '8') {
        int ep_f = ep[0] - 'a';
        if (side == 0 && ep[1] == '6') {
            /* Black double-pushed from rank 7 to rank 5 (32 + file) */
            ep_code = (uint8_t)(32 + ep_f);
        } else if (side == 1 && ep[1] == '3') {
            /* White double-pushed from rank 2 to rank 4 (24 + file) */
            ep_code = (uint8_t)(24 + ep_f);
        }
    }
    ram[ENGINE_BASE + 0x00E2u] = ep_code;

    engine_cmd(0x01u);
    write_book_descriptor();
    /* FRITZ.FBK expects move history from startpos; disable book on custom FENs */
    apply_book_option(0);
}

static void handle_position(char *args) {
    char *tokens[1024];
    int ntok = 0;
    char *p = args;
    while (*p && ntok < 1024) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        tokens[ntok++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    if (ntok == 0) return;

    int idx = 0;
    if (strcmp(tokens[idx], "startpos") == 0) {
        engine_new_game();
        idx++;
    } else if (strcmp(tokens[idx], "fen") == 0) {
        idx++;
        const char *board  = (idx < ntok) ? tokens[idx++] : "8/8/8/8/8/8/8/8";
        const char *stm    = (idx < ntok && strcmp(tokens[idx], "moves") != 0) ? tokens[idx++] : "w";
        const char *castle = (idx < ntok && strcmp(tokens[idx], "moves") != 0) ? tokens[idx++] : "-";
        const char *ep     = (idx < ntok && strcmp(tokens[idx], "moves") != 0) ? tokens[idx++] : "-";
        /* Skip optional halfmove/fullmove fields until "moves" */
        while (idx < ntok && strcmp(tokens[idx], "moves") != 0) idx++;
        set_fen_position(board, stm, castle, ep);
    }

    if (idx < ntok && strcmp(tokens[idx], "moves") == 0) {
        idx++;
        for (; idx < ntok; idx++) {
            (void)apply_uci_move(tokens[idx]);
        }
    }
}

static void handle_go(char *args) {
    int64_t wtime = -1, btime = -1, winc = 0, binc = 0;
    int64_t movestogo = 30, movetime = -1;
    int depth = -1;
    int infinite = 0;

    char *p = args;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *tok = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';

        char *val = NULL;
        if (strcmp(tok, "wtime") == 0 || strcmp(tok, "btime") == 0 ||
            strcmp(tok, "winc") == 0 || strcmp(tok, "binc") == 0 ||
            strcmp(tok, "movestogo") == 0 || strcmp(tok, "depth") == 0 ||
            strcmp(tok, "movetime") == 0) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p) {
                val = p;
                while (*p && !isspace((unsigned char)*p)) p++;
                if (*p) *p++ = '\0';
            }
        }

        if (strcmp(tok, "infinite") == 0) {
            infinite = 1;
        } else if (strcmp(tok, "depth") == 0 && val) {
            depth = atoi(val);
        } else if (strcmp(tok, "movetime") == 0 && val) {
            movetime = atoll(val);
        } else if (strcmp(tok, "wtime") == 0 && val) {
            wtime = atoll(val);
        } else if (strcmp(tok, "btime") == 0 && val) {
            btime = atoll(val);
        } else if (strcmp(tok, "winc") == 0 && val) {
            winc = atoll(val);
        } else if (strcmp(tok, "binc") == 0 && val) {
            binc = atoll(val);
        } else if (strcmp(tok, "movestogo") == 0 && val) {
            movestogo = atoll(val);
            if (movestogo < 1) movestogo = 1;
        }
    }

    /* Check if any legal move exists (and check book usability) */
    apply_book_option(opt_own_book && pos_is_startpos);
    write_book_descriptor();

    /* Save board state prior to search to detect pawn promotions accurately */
    uint8_t pre_board[64];
    memcpy(pre_board, ram + ENGINE_BASE + 0x00A0u, 64);
    uint8_t stm = ram[ENGINE_BASE + 0x00E0u] & 1u;

    /* Check game status first (3=mate, 4=stalemate) */
    engine_cmd(0x0Bu);
    uint8_t status = ram[ENGINE_BASE + 0x0081u];
    if (status == 3u || status == 4u) {
        printf("bestmove 0000\n");
        fflush(stdout);
        return;
    }

    uint64_t target_ms = 0;
    uint64_t hard_ms = 0;

    if (infinite || depth > 0) {
        int d = infinite ? 30 : depth;
        if (d < 1) d = 1;
        if (d > 30) d = 30;
        ram[ENGINE_BASE + 0x009Du] = 0x01u; /* fixed depth: disable internal timer check */
        ram[ENGINE_BASE + 0x009Eu] = (uint8_t)d;
        mem_w16(ENGINE_SEG, 0x0090u, 0xFFFFu);
        mem_w16(ENGINE_SEG, 0x0092u, 0x0FFFu);
        engine_cmd(0x06u);
        hard_ms = 0;
    } else if (movetime >= 0) {
        uint64_t mt = (movetime > 20) ? (uint64_t)movetime : 20ULL;
        target_ms = (mt * 90ULL) / 100ULL;
        if (target_ms < 10ULL) target_ms = 10ULL;
        hard_ms = (mt > 15ULL) ? (mt - 10ULL) : mt;
        uint32_t ticks = (uint32_t)((target_ms * 182065ULL) / 10000000ULL);
        if (ticks < 1u) ticks = 1u;
        ram[ENGINE_BASE + 0x009Du] = 0x00u;
        ram[ENGINE_BASE + 0x009Eu] = 30u;
        mem_w16(ENGINE_SEG, 0x0090u, (uint16_t)(ticks & 0xFFFFu));
        mem_w16(ENGINE_SEG, 0x0092u, (uint16_t)(ticks >> 16));
        engine_cmd(0x06u);
    } else {
        int64_t my_time = stm ? btime : wtime;
        int64_t my_inc  = stm ? binc  : winc;
        if (my_time < 0) my_time = 60000;
        if (my_time < 50) my_time = 50;
        int64_t alloc = my_time / movestogo + (my_inc * 3) / 4;
        if (alloc > my_time / 2) alloc = my_time / 2;
        if (alloc < 20) alloc = 20;
        target_ms = (uint64_t)alloc;
        hard_ms = (uint64_t)((alloc * 3) / 2);
        if (hard_ms > (uint64_t)(my_time - 20)) {
            hard_ms = (my_time > 30) ? (uint64_t)(my_time - 20) : 15ULL;
        }
        uint32_t ticks = (uint32_t)((target_ms * 182065ULL) / 10000000ULL);
        if (ticks < 1u) ticks = 1u;
        ram[ENGINE_BASE + 0x009Du] = 0x00u;
        ram[ENGINE_BASE + 0x009Eu] = 30u;
        mem_w16(ENGINE_SEG, 0x0090u, (uint16_t)(ticks & 0xFFFFu));
        mem_w16(ENGINE_SEG, 0x0092u, (uint16_t)(ticks >> 16));
        engine_cmd(0x06u);
    }

    searching = 1;
    is_infinite = infinite;
    search_start_ms = now_ms();
    search_hard_ms = hard_ms;
    search_depth = 1;
    search_nodes = 0;
    stop_requested = 0;

    engine_cmd(0x04u);

    searching = 0;
    is_infinite = 0;

    uint8_t err = ram[ENGINE_BASE + 0x0081u];
    if (err != 0u) {
        printf("bestmove 0000\n");
        fflush(stdout);
        return;
    }

    uint8_t from = ram[ENGINE_BASE + 0x0082u];
    uint8_t to   = ram[ENGINE_BASE + 0x0083u];
    uint8_t flg  = ram[ENGINE_BASE + 0x0084u];

    if (from >= 64u || to >= 64u || from == to) {
        printf("bestmove 0000\n");
        fflush(stdout);
        return;
    }

    char fbuf[3], tbuf[3];
    format_sq((int)from, fbuf);
    format_sq((int)to, tbuf);

    char promo_ch = '\0';
    uint8_t moved_piece = pre_board[from];
    if ((moved_piece == 0x10u || moved_piece == 0x20u) && (to < 8u || to >= 56u)) {
        switch (flg & 0xC1u) {
            case 0x80u: promo_ch = 'q'; break;
            case 0x81u: promo_ch = 'r'; break;
            case 0xC0u: promo_ch = 'b'; break;
            case 0xC1u: promo_ch = 'n'; break;
            default:    promo_ch = 'q'; break;
        }
    }

    if (promo_ch) {
        printf("bestmove %s%s%c\n", fbuf, tbuf, promo_ch);
    } else {
        printf("bestmove %s%s\n", fbuf, tbuf);
    }
    fflush(stdout);
}

static void handle_setoption(char *args) {
    char *name_kw = strstr(args, "name");
    if (!name_kw) return;
    char *val_kw = strstr(name_kw, "value");
    if (!val_kw) return;

    *val_kw = '\0';
    char *name = trim_ws(name_kw + 4);
    char *val  = trim_ws(val_kw + 5);

    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char *p = val;  *p; p++) *p = (char)tolower((unsigned char)*p);

    if (strcmp(name, "ownbook") == 0) {
        opt_own_book = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) ? 1 : 0;
        apply_book_option(opt_own_book && pos_is_startpos);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    init_parity_lut();

    prog_start_ms = now_ms();
    bios_tick_base = (uint32_t)(((uint64_t)time(NULL) % 86400ULL) * 18ULL);

    engine_init();

#ifdef _WIN32
    InitializeCriticalSection(&stdin_cs);
    stdin_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE hStdinThread = CreateThread(NULL, 0, win32_stdin_thread, NULL, 0, NULL);
    if (hStdinThread) {
        CloseHandle(hStdinThread);
    } else {
        win_eof = 1;
        if (stdin_event) SetEvent(stdin_event);
    }
#endif

    char line[MAX_LINE_LEN];
    while (!quit_requested && read_next_uci_line(line, sizeof(line))) {
        char *cmd = trim_ws(line);
        if (*cmd == '\0') continue;

        if (strcmp(cmd, "uci") == 0) {
            printf("id name KnightStalkerUCI 1.0 (Fritz 1.00 1991)\n");
            printf("id author Frans Morsch\n");
            printf("option name OwnBook type check default true\n");
            printf("uciok\n");
            fflush(stdout);
        } else if (strcmp(cmd, "isready") == 0) {
            printf("readyok\n");
            fflush(stdout);
        } else if (strcmp(cmd, "ucinewgame") == 0) {
            engine_new_game();
        } else if (strncmp(cmd, "setoption", 9) == 0 &&
                   (cmd[9] == '\0' || isspace((unsigned char)cmd[9]))) {
            handle_setoption(cmd + 9);
        } else if (strncmp(cmd, "position", 8) == 0 &&
                   (cmd[8] == '\0' || isspace((unsigned char)cmd[8]))) {
            handle_position(cmd + 8);
        } else if (strncmp(cmd, "go", 2) == 0 &&
                   (cmd[2] == '\0' || isspace((unsigned char)cmd[2]))) {
            handle_go(cmd + 2);
        } else if (strcmp(cmd, "stop") == 0) {
            /* No active search */
        } else if (strcmp(cmd, "quit") == 0) {
            break;
        }
    }

    return 0;
}
