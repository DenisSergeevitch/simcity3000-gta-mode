// File-drop command channel: tools write Apps\gta_cmd.txt, the main-thread tick executes it and writes
// Apps\gta_out.txt (atomically, ending with "#done <seq>"). Used for RE (peek/poke/scan) and for tests.
#include "util.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static char *g_out; static size_t g_outlen, g_outcap;
uint32_t g_scratch[64];   // writable scratch for "call" output parameters

void out_printf(const char *fmt, ...) {
    char tmp[2048];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
    if (g_outlen + n + 1 > g_outcap) {
        g_outcap = (g_outlen + n + 1) * 2 + 4096; g_out = realloc(g_out, g_outcap);
    }
    memcpy(g_out + g_outlen, tmp, n); g_outlen += n; g_out[g_outlen] = 0;
}

bool mem_read(uintptr_t addr, void *buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (void *)addr, buf, n, &got) && got == n;
}

bool mem_write(uintptr_t addr, const void *buf, size_t n) {
    DWORD old;
    if (!VirtualProtect((void *)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void *)addr, buf, n);
    VirtualProtect((void *)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, n);
    return true;
}

static void cmd_mods(void) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    MODULEENTRY32 me = { sizeof me };
    if (s == INVALID_HANDLE_VALUE) { out_printf("toolhelp failed\n"); return; }
    for (BOOL ok = Module32First(s, &me); ok; ok = Module32Next(s, &me))
        out_printf("%08lx %08lx %s\n", (unsigned long)(uintptr_t)me.modBaseAddr, (unsigned long)me.modBaseSize, me.szModule);
    CloseHandle(s);
}

static void hexdump(uintptr_t a, size_t n) {
    unsigned char buf[16];
    for (size_t off = 0; off < n; off += 16) {
        size_t k = n - off < 16 ? n - off : 16;
        if (!mem_read(a + off, buf, k)) { out_printf("%08lx: <unreadable>\n", (unsigned long)(a + off)); return; }
        out_printf("%08lx:", (unsigned long)(a + off));
        for (size_t i = 0; i < k; i++) out_printf(" %02x", buf[i]);
        out_printf("\n");
    }
}

static void exec_line(char *line) {
    char *argv[32]; int argc = 0;
    for (char *t = strtok(line, " \t\r\n"); t && argc < 32; t = strtok(NULL, " \t\r\n")) argv[argc++] = t;
    if (!argc || argv[0][0] == '#') return;
    out_printf("> %s", argv[0]); for (int i = 1; i < argc; i++) out_printf(" %s", argv[i]); out_printf("\n");
    const char *c = argv[0];
    if (!strcmp(c, "ping")) out_printf("pong tick=%lu\n", GetTickCount());
    else if (!strcmp(c, "mods")) cmd_mods();
    else if (!strcmp(c, "rd") && argc >= 3) hexdump(addr_eval(argv[1]), strtoul(argv[2], 0, 0));
    else if (!strcmp(c, "rd32") && argc >= 2) {
        uintptr_t a = addr_eval(argv[1]); int n = argc > 2 ? atoi(argv[2]) : 1;
        for (int i = 0; i < n; i++) {
            uint32_t v; if (!mem_read(a + 4 * i, &v, 4)) { out_printf("%08lx: <unreadable>\n", (unsigned long)(a + 4 * i)); break; }
            out_printf("%08lx: %08lx  %ld\n", (unsigned long)(a + 4 * i), (unsigned long)v, (long)(int32_t)v);
        }
    }
    else if (!strcmp(c, "wr") && argc >= 3) {
        unsigned char b[256]; size_t n = 0; const char *h = argv[2];
        while (h[0] && h[1] && n < sizeof b) { unsigned v; sscanf(h, "%2x", &v); b[n++] = v; h += 2; }
        out_printf(mem_write(addr_eval(argv[1]), b, n) ? "ok %u bytes\n" : "fail\n", (unsigned)n);
    }
    else if (!strcmp(c, "wr32") && argc >= 3) {
        uint32_t v = strtoul(argv[2], 0, 0);
        out_printf(mem_write(addr_eval(argv[1]), &v, 4) ? "ok\n" : "fail\n");
    }
    else if (!strcmp(c, "rdf") && argc >= 2) {        // rdf <addr> [n]: floats
        uintptr_t a = addr_eval(argv[1]); int n = argc > 2 ? atoi(argv[2]) : 1;
        for (int i = 0; i < n; i++) { float f; if (!mem_read(a + 4 * i, &f, 4)) break; out_printf("%08lx: %g\n", (unsigned long)(a + 4 * i), f); }
    }
    else if (!strcmp(c, "scratch")) out_printf("%08lx\n", (unsigned long)(uintptr_t)g_scratch);
    else if (!strcmp(c, "occ") && argc >= 2) {   // occ <obj>: call vt[0xbc](&x,&z) and vt[0x48]() on an occupant
        uint32_t o = addr_eval(argv[1]), vt; if (!mem_read(o, &vt, 4)) { out_printf("bad\n"); return; }
        uint32_t fpos, fflg; mem_read(vt + 0xbc, &fpos, 4); mem_read(vt + 0x48, &fflg, 4);
        g_scratch[0] = g_scratch[1] = 0xdead; uint32_t a2[2] = { (uint32_t)(uintptr_t)&g_scratch[0], (uint32_t)(uintptr_t)&g_scratch[1] };
        call_raw((void *)(uintptr_t)fpos, o, a2, 2);
        uint32_t fl = call_raw((void *)(uintptr_t)fflg, o, NULL, 0);
        out_printf("%08lx pos=%ld,%ld flags=%08lx\n", (unsigned long)o, (long)g_scratch[0], (long)g_scratch[1], (unsigned long)fl);
    }
    else if (!strcmp(c, "addr") && argc >= 2) out_printf("%08lx\n", (unsigned long)addr_eval(argv[1]));
    else if (!strcmp(c, "call") && argc >= 3) {          // call <fn> <this|0> [args: 0x.., dec, f:float]
        uint32_t args[16]; int n = 0;
        for (int i = 3; i < argc && n < 16; i++) {
            if (!strncmp(argv[i], "f:", 2)) { float f = strtof(argv[i] + 2, 0); memcpy(&args[n++], &f, 4); }
            else if (strchr(argv[i], '!') || argv[i][0] == '[') args[n++] = addr_eval(argv[i]);
            else args[n++] = strtoul(argv[i], 0, 0);
        }
        void *fn = (void *)addr_eval(argv[1]); uint32_t self = addr_eval(argv[2]);
        uint32_t r = call_raw(fn, self, args, n);
        out_printf("ret=%08lx (%ld)\n", (unsigned long)r, (long)(int32_t)r);
    }
    else if (!strcmp(c, "scan") || !strcmp(c, "next") || !strcmp(c, "list") || !strcmp(c, "ptrs") || !strcmp(c, "snap") || !strcmp(c, "vmmap") || !strcmp(c, "vtcount") || !strcmp(c, "gridfind") || !strcmp(c, "gridauto"))
        scan_cmd(argc, argv);
    else if (!gta_cmd(argc, argv)) out_printf("unknown command\n");
}

void cmd_poll(void) {
    char path[MAX_PATH], outp[MAX_PATH], tmpp[MAX_PATH];
    snprintf(path, sizeof path, "%s\\gta_cmd.txt", g_moddir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *text = malloc(n + 1); n = fread(text, 1, n, f); text[n] = 0; fclose(f);
    DeleteFileA(path);
    unsigned seq = 0; sscanf(text, "#seq %u", &seq);
    g_outlen = 0; if (g_out) g_out[0] = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char copy[1024]; strncpy(copy, line, sizeof copy - 1); copy[sizeof copy - 1] = 0;
        exec_line(copy);
    }
    free(text);
    out_printf("#done %u\n", seq);
    snprintf(outp, sizeof outp, "%s\\gta_out.txt", g_moddir);
    snprintf(tmpp, sizeof tmpp, "%s\\gta_out.tmp", g_moddir);
    FILE *o = fopen(tmpp, "wb"); if (!o) return;
    fwrite(g_out, 1, g_outlen, o); fclose(o);
    MoveFileExA(tmpp, outp, MOVEFILE_REPLACE_EXISTING);
}
