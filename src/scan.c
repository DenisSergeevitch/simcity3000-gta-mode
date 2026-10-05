// In-process value scanner (a tiny Cheat Engine) for dynamic RE through the command channel.
//   scan <u8|u16|u32|i32|f32|f64> <value>|any [lo hi]   new scan of writable committed memory (any = snapshot)
//   next <eq|ne|gt|lt|changed|unchanged|inc|dec> [value] refine
//   list [n]                                           print candidates with current values
//   ptrs <addr> [maxoff]                               find dwords pointing into [addr-maxoff, addr]
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { T_U8, T_U16, T_U32, T_I32, T_F32, T_F64 } VT;
typedef struct { uint32_t addr; uint64_t prev; } Cand;
static Cand *g_c; static size_t g_n, g_cap; static VT g_t; static int g_sz;
#define MAX_CANDS (12u * 1000u * 1000u)

static int tsize(VT t) { return t == T_U8 ? 1 : t == T_U16 ? 2 : t == T_F64 ? 8 : 4; }
static bool parse_type(const char *s, VT *t) {
    static const char *names[] = { "u8", "u16", "u32", "i32", "f32", "f64" };
    for (int i = 0; i < 6; i++) if (!strcmp(s, names[i])) { *t = i; return true; }
    return false;
}
static uint64_t raw_of(const char *s, VT t) {
    uint64_t r = 0;
    if (t == T_F32) { float f = strtof(s, 0); memcpy(&r, &f, 4); }
    else if (t == T_F64) { double d = strtod(s, 0); memcpy(&r, &d, 8); }
    else if (t == T_I32) { int32_t v = strtol(s, 0, 0); memcpy(&r, &v, 4); }
    else r = strtoull(s, 0, 0);
    return r;
}
static double num(uint64_t raw, VT t) {
    switch (t) {
    case T_U8: return (uint8_t)raw; case T_U16: return (uint16_t)raw; case T_U32: return (uint32_t)raw;
    case T_I32: return (int32_t)(uint32_t)raw;
    case T_F32: { float f; uint32_t u = (uint32_t)raw; memcpy(&f, &u, 4); return f; }
    default: { double d; memcpy(&d, &raw, 8); return d; }
    }
}
static void fmt(char *b, uint64_t raw, VT t) {
    if (t == T_F32 || t == T_F64) sprintf(b, "%g", num(raw, t));
    else sprintf(b, "%.0f (0x%llx)", num(raw, t), (unsigned long long)raw);
}
// The candidate array lives in its own VirtualAlloc reservation, and scans skip it and this DLL's image;
// otherwise a scan finds its own candidates (prev values) and runs away.
static bool add(uint32_t a, uint64_t v) {
    if (g_n >= MAX_CANDS) return false;
    if (!g_c) g_c = VirtualAlloc(NULL, MAX_CANDS * sizeof *g_c, MEM_RESERVE, PAGE_READWRITE);
    if (g_n == g_cap) {
        size_t ncap = g_cap ? g_cap * 2 : 65536; if (ncap > MAX_CANDS) ncap = MAX_CANDS;
        if (!VirtualAlloc(g_c, ncap * sizeof *g_c, MEM_COMMIT, PAGE_READWRITE)) return false;
        g_cap = ncap;
    }
    g_c[g_n].addr = a; g_c[g_n].prev = v; g_n++; return true;
}
static bool writable(DWORD p) {
    return (p & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) && !(p & PAGE_GUARD);
}
static bool excluded(const MEMORY_BASIC_INFORMATION *mi) {
    static void *self_base;
    if (!self_base) { MEMORY_BASIC_INFORMATION m; VirtualQuery((void *)&excluded, &m, sizeof m); self_base = m.AllocationBase; }
    return mi->AllocationBase == self_base || (g_c && mi->AllocationBase == (void *)g_c);
}

static void do_scan(VT t, bool any, uint64_t want, uint32_t lo, uint32_t hi) {
    g_n = 0; g_t = t; g_sz = tsize(t);
    int step = (t == T_U8) ? 1 : (t == T_U16 ? 2 : 4);
    static unsigned char buf[1 << 16];
    MEMORY_BASIC_INFORMATION mi;
    bool full = false;
    for (uintptr_t a = lo; a < hi && !full; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
        if (mi.State != MEM_COMMIT || !writable(mi.Protect) || excluded(&mi)) continue;
        uintptr_t rs = (uintptr_t)mi.BaseAddress, re = rs + mi.RegionSize;
        if (re > hi) re = hi;
        if (rs < lo) rs = lo;
        for (uintptr_t c = rs; c < re && !full; c += sizeof buf) {
            size_t n = re - c < sizeof buf ? re - c : sizeof buf;
            if (!mem_read(c, buf, n)) continue;
            for (size_t i = 0; i + g_sz <= n; i += step) {
                uint64_t v = 0; memcpy(&v, buf + i, g_sz);
                if (any || v == want) if (!add((uint32_t)(c + i), v)) { full = true; break; }
            }
        }
    }
    out_printf("%s%u candidates\n", full ? "FULL (truncated) " : "", (unsigned)g_n);
}

static void do_next(const char *op, uint64_t want) {
    size_t k = 0;
    for (size_t i = 0; i < g_n; i++) {
        uint64_t v = 0; if (!mem_read(g_c[i].addr, &v, g_sz)) continue;
        double a = num(v, g_t), p = num(g_c[i].prev, g_t), w = num(want, g_t);
        bool keep =
            !strcmp(op, "eq") ? v == want : !strcmp(op, "ne") ? v != want :
            !strcmp(op, "gt") ? a > w : !strcmp(op, "lt") ? a < w :
            !strcmp(op, "changed") ? v != g_c[i].prev : !strcmp(op, "unchanged") ? v == g_c[i].prev :
            !strcmp(op, "inc") ? a > p : !strcmp(op, "dec") ? a < p : false;
        if (keep) { g_c[k].addr = g_c[i].addr; g_c[k].prev = v; k++; }
    }
    g_n = k;
    out_printf("%u candidates\n", (unsigned)g_n);
}

void scan_cmd(int argc, char **argv) {
    if (!strcmp(argv[0], "scan") && argc >= 3) {
        VT t; if (!parse_type(argv[1], &t)) { out_printf("bad type\n"); return; }
        bool any = !strcmp(argv[2], "any");
        uint32_t lo = argc > 3 ? strtoul(argv[3], 0, 16) : 0x10000, hi = argc > 4 ? strtoul(argv[4], 0, 16) : 0x7fff0000;
        do_scan(t, any, any ? 0 : raw_of(argv[2], t), lo, hi);
    } else if (!strcmp(argv[0], "next") && argc >= 2) {
        do_next(argv[1], argc > 2 ? raw_of(argv[2], g_t) : 0);
    } else if (!strcmp(argv[0], "list")) {
        size_t n = argc > 1 ? strtoul(argv[1], 0, 0) : 40; char b[64];
        for (size_t i = 0; i < g_n && i < n; i++) {
            uint64_t v = 0; mem_read(g_c[i].addr, &v, g_sz); fmt(b, v, g_t);
            out_printf("%08lx = %s\n", (unsigned long)g_c[i].addr, b);
        }
    } else if (!strcmp(argv[0], "ptrs") && argc >= 2) {
        uint32_t target = strtoul(argv[1], 0, 16), maxoff = argc > 2 ? strtoul(argv[2], 0, 0) : 0x400;
        static unsigned char buf[1 << 16]; MEMORY_BASIC_INFORMATION mi; int found = 0;
        for (uintptr_t a = 0x10000; a < 0x7fff0000 && found < 200; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
            if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
            if (mi.State != MEM_COMMIT || !(writable(mi.Protect) || (mi.Protect & PAGE_READONLY)) || excluded(&mi)) continue;
            uintptr_t rs = (uintptr_t)mi.BaseAddress, re = rs + mi.RegionSize;
            for (uintptr_t c = rs; c < re && found < 200; c += sizeof buf) {
                size_t n = re - c < sizeof buf ? re - c : sizeof buf;
                if (!mem_read(c, buf, n)) continue;
                for (size_t i = 0; i + 4 <= n; i += 4) {
                    uint32_t v; memcpy(&v, buf + i, 4);
                    if (v <= target && target - v <= maxoff) {
                        out_printf("%08lx -> %08lx (+0x%lx)\n", (unsigned long)(c + i), (unsigned long)v, (unsigned long)(target - v));
                        if (++found >= 200) break;
                    }
                }
            }
        }
        out_printf("%d pointers\n", found);
    } else if (!strcmp(argv[0], "vtcount") && argc >= 3) {   // vtcount <lo> <hi>: histogram of aligned dwords in [lo,hi) found at the start of heap objects
        uint32_t lo = addr_eval(argv[1]), hi = addr_eval(argv[2]);
        enum { NB = 4096 }; static uint32_t keys[NB], cnts[NB]; int nk = 0;
        static unsigned char buf[1 << 16]; MEMORY_BASIC_INFORMATION mi;
        for (uintptr_t a = 0x10000; a < 0x7fff0000; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
            if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
            if (mi.State != MEM_COMMIT || !writable(mi.Protect) || mi.Type == MEM_IMAGE || excluded(&mi)) continue;
            uintptr_t rs = (uintptr_t)mi.BaseAddress, re = rs + mi.RegionSize;
            for (uintptr_t c = rs; c < re; c += sizeof buf) {
                size_t n = re - c < sizeof buf ? re - c : sizeof buf;
                if (!mem_read(c, buf, n)) continue;
                for (size_t i = 0; i + 4 <= n; i += 4) {
                    uint32_t v; memcpy(&v, buf + i, 4);
                    if (v < lo || v >= hi || (v & 3)) continue;
                    int k; for (k = 0; k < nk && keys[k] != v; k++);
                    if (k == nk) { if (nk == NB) continue; keys[nk] = v; cnts[nk] = 0; nk++; }
                    cnts[k]++;
                }
            }
        }
        for (int pass = 0; pass < 60; pass++) {   // print top 60
            int best = -1; for (int k = 0; k < nk; k++) if (cnts[k] && (best < 0 || cnts[k] > cnts[best])) best = k;
            if (best < 0) break;
            out_printf("%08lx x%lu\n", (unsigned long)keys[best], (unsigned long)cnts[best]); cnts[best] = 0;
        }
    } else if (!strcmp(argv[0], "gridfind") && argc >= 4) {
        // gridfind <esz 1|2|4> <W> <x,z,+|0> ...: find base B of a WxW grid where element(x,z) is nonzero (+) or zero (0).
        // Tries both index orders (x*W+z and z*W+x). Element stride is esz.
        int esz = atoi(argv[1]), W = atoi(argv[2]), nc = 0; int cx[16], cz[16]; char cv[16];
        for (int i = 3; i < argc && nc < 16; i++) { if (sscanf(argv[i], "%d,%d,%c", &cx[nc], &cz[nc], &cv[nc]) == 3) nc++; }
        MEMORY_BASIC_INFORMATION mi; int found = 0; size_t span = (size_t)W * W * esz;
        for (uintptr_t a = 0x10000; a < 0x7fff0000 && found < 40; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
            if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
            if (mi.State != MEM_COMMIT || !writable(mi.Protect) || excluded(&mi) || mi.RegionSize < span) continue;
            uint8_t *base = (uint8_t *)mi.BaseAddress; size_t n = mi.RegionSize;
            uint8_t *copy = malloc(n); if (!copy) continue;
            if (!mem_read((uintptr_t)base, copy, n)) { free(copy); continue; }
            for (int order = 0; order < 2; order++)
            for (size_t b = 0; b + span <= n && found < 40; b += esz) {
                int ok = 1;
                for (int k = 0; k < nc && ok; k++) {
                    size_t idx = order ? (size_t)cz[k] * W + cx[k] : (size_t)cx[k] * W + cz[k];
                    uint8_t *e = copy + b + idx * esz; uint32_t v = esz == 1 ? *e : esz == 2 ? *(uint16_t *)e : *(uint32_t *)e;
                    ok = (cv[k] == '+') ? v != 0 : v == 0;
                }
                if (ok) {
                    out_printf("B=%08lx order=%s vals:", (unsigned long)(uintptr_t)(base + b), order ? "z*W+x" : "x*W+z");
                    for (int k = 0; k < nc; k++) {
                        size_t idx = order ? (size_t)cz[k] * W + cx[k] : (size_t)cx[k] * W + cz[k];
                        uint8_t *e = copy + b + idx * esz; uint32_t v = esz == 1 ? *e : esz == 2 ? *(uint16_t *)e : *(uint32_t *)e;
                        out_printf(" %lx", (unsigned long)v);
                    }
                    out_printf("\n"); found++;
                }
            }
            free(copy);
        }
        out_printf("%d hits\n", found);
    } else if (!strcmp(argv[0], "gridauto") && argc >= 4) {
        // gridauto <esz> <cx> <cz>: classify ~40x40 tiles around (cx,cz) by SIMSPR tile drawable class
        // (network-base vs terrain), then find grids that are nonzero on network tiles and zero on terrain tiles.
        int esz = atoi(argv[1]), cxx = atoi(argv[2]), czz = atoi(argv[3]);
        uint32_t view = 0; mem_read(ghidra_to_rt("SIMSPR.DLL", 0x10072bec), &view, 4);
        uint32_t cols = 0; mem_read(view + 0x24, &cols, 4);
        uint32_t vt_net = ghidra_to_rt("SIMSPR.DLL", 0x10062bec), vt_ter = ghidra_to_rt("SIMSPR.DLL", 0x10068d1c);
        enum { MAXC = 3000 }; static int tx[MAXC], tz[MAXC]; static char tv[MAXC]; int nc = 0, nn = 0, nt = 0;
        for (int x = cxx - 20; x < cxx + 20; x++) for (int z = czz - 20; z < czz + 20; z++) {
            if (x < 0 || z < 0 || x > 255 || z > 255 || nc >= MAXC) continue;
            uint32_t col; uint8_t r[20]; uint32_t vt = 0;
            if (!mem_read(cols + 4 * x, &col, 4) || !mem_read(col + 20 * z, r, 20)) continue;
            uint32_t d = *(uint32_t *)r; if (!d || (d & 3) || !mem_read(d, &vt, 4)) continue;
            if (vt == vt_net) { tx[nc] = x; tz[nc] = z; tv[nc++] = '+'; nn++; }
            else if (vt == vt_ter && !*(uint32_t *)(r + 12)) { tx[nc] = x; tz[nc] = z; tv[nc++] = '0'; nt++; }
        }
        out_printf("constraints: %d network, %d terrain\n", nn, nt);
        MEMORY_BASIC_INFORMATION mi; int found = 0; size_t span = (size_t)256 * 256 * esz;
        for (uintptr_t a = 0x10000; a < 0x7fff0000 && found < 20; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
            if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
            if (mi.State != MEM_COMMIT || !writable(mi.Protect) || excluded(&mi) || mi.RegionSize < span) continue;
            uint8_t *base = (uint8_t *)mi.BaseAddress; size_t n = mi.RegionSize;
            uint8_t *copy = malloc(n); if (!copy) continue;
            if (!mem_read((uintptr_t)base, copy, n)) { free(copy); continue; }
            for (int order = 0; order < 2; order++)
            for (size_t b = 0; b + span <= n && found < 20; b += esz) {
                int bad = 0, lim = nc / 20 + 1;
                for (int k = 0; k < nc && bad <= lim; k++) {
                    size_t idx = order ? (size_t)tz[k] * 256 + tx[k] : (size_t)tx[k] * 256 + tz[k];
                    uint8_t *e = copy + b + idx * esz; uint32_t v = esz == 1 ? *e : esz == 2 ? *(uint16_t *)e : *(uint32_t *)e;
                    if ((tv[k] == '+') != (v != 0)) bad++;
                }
                if (bad <= lim) { out_printf("B=%08lx order=%s mismatches=%d/%d\n", (unsigned long)(uintptr_t)(base + b), order ? "z*256+x" : "x*256+z", bad, nc); found++; }
            }
            free(copy);
        }
        out_printf("%d hits\n", found);
    } else if (!strcmp(argv[0], "vmmap")) {
        MEMORY_BASIC_INFORMATION mi; unsigned long tot = 0;
        for (uintptr_t a = 0x10000; a < 0x7fff0000; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
            if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
            if (mi.State != MEM_COMMIT || !writable(mi.Protect)) continue;
            if (mi.RegionSize >= 0x10000 || argc > 1)
                out_printf("%08lx %8lx %s prot=%lx\n", (unsigned long)(uintptr_t)mi.BaseAddress, (unsigned long)mi.RegionSize,
                           mi.Type == MEM_IMAGE ? "img" : mi.Type == MEM_MAPPED ? "map" : "prv", (unsigned long)mi.Protect);
            tot += mi.RegionSize;
        }
        out_printf("total writable committed: %lu MB\n", tot >> 20);
    } else out_printf("usage: scan <type> <value|any> [lo hi] | next <op> [v] | list [n] | ptrs <addr> [maxoff]\n");
}
