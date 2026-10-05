// Address expressions for the command channel, so Ghidra addresses can be used directly:
//   1234abcd                 absolute
//   SIMSPR.DLL!10072bec      module base + (va - preferred image base)
//   [expr]                   dereference (dword)
//   expr+54  expr-8          offsets (hex)
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

uintptr_t mod_base(const char *name) { return (uintptr_t)GetModuleHandleA(name); }

uintptr_t ghidra_to_rt(const char *mod, uint32_t va) {
    uintptr_t b = mod_base(mod); if (!b) return 0;
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)b;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + d->e_lfanew);
    return b + (va - nt->OptionalHeader.ImageBase);
}

static uintptr_t parse(const char **p);
static uintptr_t term(const char **p) {
    const char *s = *p;
    if (*s == '[') {
        s++; uintptr_t a = parse(&s); if (*s == ']') s++;
        uint32_t v = 0; mem_read(a, &v, 4); *p = s; return v;
    }
    const char *bang = NULL; const char *e = s;
    while (*e && *e != '+' && *e != '-' && *e != ']') { if (*e == '!') bang = e; e++; }
    uintptr_t r;
    if (bang) {
        char mod[64]; size_t n = bang - s; if (n >= sizeof mod) n = sizeof mod - 1;
        memcpy(mod, s, n); mod[n] = 0;
        r = ghidra_to_rt(mod, strtoul(bang + 1, NULL, 16));
    } else r = strtoul(s, NULL, 16);
    *p = e; return r;
}
static uintptr_t parse(const char **p) {
    uintptr_t v = term(p);
    while (**p == '+' || **p == '-') {
        char op = *(*p)++; uintptr_t t = term(p);
        v = op == '+' ? v + t : v - t;
    }
    return v;
}
uintptr_t addr_eval(const char *s) { return parse(&s); }
