// Runtime loader for SimCity 3000 sprite archives (Res\Sprites\*.DAT), read from the user's own install.
// Format (see tools/sc3spr.py and MODLOG.md "Sprites"):
//   IXF index: magic D7 81 C3 80, then 20-byte records (type, group, instance, offset, size), zero-terminated.
//   instance 0 = image: 20-byte header (u32 0x107, u32, u32 w, u32 h, u32 csize) + RefPack-compressed body.
//   instance 1 = 8 bytes: s16 left, up, right, down (anchor = (left, up)).
//   Decompressed body: u32 total, u16 w, u16 h, u16, u16, u32 colorkey (RGB565 0xF81F), u32 0, then one run per
//   row (u16 x, u16 len|flag, u32 cumulative pixel offset after this run, absent on the last row), then RGB565.
#include "sprites.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t group; uint32_t off0, size0, off1, size1; } Entry;
typedef struct { char path[MAX_PATH]; uint8_t *data; size_t len; Entry *ents; int n; } Archive;
static Archive g_arch[4];
static int g_narch;

typedef struct { uint32_t group; Image img; bool ok; } CacheSlot;
#define CACHE_SIZE 4096
static CacheSlot g_cache[CACHE_SIZE];

static int cmp_ent(const void *a, const void *b) {
    uint32_t x = ((const Entry *)a)->group, y = ((const Entry *)b)->group; return (x > y) - (x < y);
}

int spr_open(const char *relpath) {
    if (g_narch >= 4) return -1;
    Archive *A = &g_arch[g_narch];
    snprintf(A->path, sizeof A->path, "%s\\Res\\Sprites\\%s", g_moddir, relpath);
    FILE *f = fopen(A->path, "rb");
    if (!f) { LOG("spr_open: cannot open %s", A->path); return -1; }
    fseek(f, 0, SEEK_END); A->len = ftell(f); fseek(f, 0, SEEK_SET);
    A->data = malloc(A->len); fread(A->data, 1, A->len, f); fclose(f);
    if (A->len < 24 || memcmp(A->data, "\xd7\x81\xc3\x80", 4)) { LOG("spr_open: bad magic %s", A->path); return -1; }
    // pass 1: image records (instance 0); pass 2: attach anchors (instance 1) by binary search
    int cap = 1024; A->ents = malloc(cap * sizeof *A->ents); A->n = 0;
    for (size_t p = 4; p + 20 <= A->len; p += 20) {
        uint32_t *r = (uint32_t *)(A->data + p);
        if (!r[0] && !r[1] && !r[3]) break;
        if (r[2] != 0) continue;
        if (A->n == cap) { cap *= 2; A->ents = realloc(A->ents, cap * sizeof *A->ents); }
        Entry *e = &A->ents[A->n++]; memset(e, 0, sizeof *e); e->group = r[1]; e->off0 = r[3]; e->size0 = r[4];
    }
    qsort(A->ents, A->n, sizeof *A->ents, cmp_ent);
    for (size_t p = 4; p + 20 <= A->len; p += 20) {
        uint32_t *r = (uint32_t *)(A->data + p);
        if (!r[0] && !r[1] && !r[3]) break;
        if (r[2] != 1) continue;
        int lo = 0, hi = A->n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (A->ents[mid].group == r[1]) { A->ents[mid].off1 = r[3]; A->ents[mid].size1 = r[4]; break; }
            if (A->ents[mid].group < r[1]) lo = mid + 1; else hi = mid - 1;
        }
    }
    LOG("spr_open: %s: %d sprites", relpath, A->n);
    return g_narch++;
}

static bool refpack(const uint8_t *src, size_t slen, uint8_t **out, size_t *olen) {
    if (slen < 5 || src[1] != 0xFB) return false;
    int n = (src[0] & 0x80) ? 4 : 3; size_t pos = 2, usize = 0;
    for (int i = 0; i < n; i++) usize = (usize << 8) | src[pos++];
    if (src[0] & 1) pos += n;
    uint8_t *o = malloc(usize + 16); size_t op = 0;
    while (pos < slen) {
        unsigned b0 = src[pos], plain, copy, off;
        if (b0 < 0x80) { unsigned b1 = src[pos + 1]; pos += 2; plain = b0 & 3; copy = ((b0 & 0x1c) >> 2) + 3; off = ((b0 & 0x60) << 3) + b1 + 1; }
        else if (b0 < 0xC0) { unsigned b1 = src[pos + 1], b2 = src[pos + 2]; pos += 3; plain = (b1 >> 6) & 3; copy = (b0 & 0x3f) + 4; off = ((b1 & 0x3f) << 8) + b2 + 1; }
        else if (b0 < 0xE0) { unsigned b1 = src[pos + 1], b2 = src[pos + 2], b3 = src[pos + 3]; pos += 4; plain = b0 & 3; copy = ((b0 & 0x0c) << 6) + b3 + 5; off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1; }
        else if (b0 < 0xFC) { pos += 1; plain = ((b0 & 0x1f) << 2) + 4; copy = 0; off = 0; }
        else { pos += 1; plain = b0 & 3; if (op + plain <= usize) memcpy(o + op, src + pos, plain); op += plain; break; }
        if (op + plain + copy > usize || pos + plain > slen || off > op + plain) { free(o); return false; }
        memcpy(o + op, src + pos, plain); op += plain; pos += plain;
        for (unsigned k = 0; k < copy; k++, op++) o[op] = o[op - off];
    }
    *out = o; *olen = op;
    return op == usize;
}

static const Entry *find(uint32_t group, Archive **arch) {
    for (int a = 0; a < g_narch; a++) {
        Archive *A = &g_arch[a]; int lo = 0, hi = A->n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2; uint32_t g = A->ents[mid].group;
            if (g == group) { *arch = A; return &A->ents[mid]; }
            if (g < group) lo = mid + 1; else hi = mid - 1;
        }
    }
    return NULL;
}

static bool decode(uint32_t group, Image *img) {
    Archive *A; const Entry *e = find(group, &A);
    if (!e || !e->size0 || e->size0 < 24) return false;
    uint8_t *raw; size_t rlen;
    if (!refpack(A->data + e->off0 + 20, e->size0 - 20, &raw, &rlen)) return false;
    bool ok = false;
    if (rlen >= 20) {
        int w = *(uint16_t *)(raw + 4), h = *(uint16_t *)(raw + 6);
        uint16_t key = *(uint16_t *)(raw + 12);
        uint32_t *px = calloc(w * h, 4);
        size_t p = 20; uint32_t prev = 0;
        typedef struct { int x, len; uint32_t start; } Run;
        Run *runs = malloc(sizeof(Run) * h);
        for (int y = 0; y < h; y++) {
            if (p + 4 > rlen) goto done;
            runs[y].x = *(uint16_t *)(raw + p); runs[y].len = *(uint16_t *)(raw + p + 2) & 0x7fff; runs[y].start = prev;
            if (y < h - 1) { if (p + 8 > rlen) goto done; prev = *(uint32_t *)(raw + p + 4); p += 8; } else p += 4;
        }
        for (int y = 0; y < h; y++) for (int k = 0; k < runs[y].len; k++) {
            size_t q = p + 2 * (runs[y].start + k); int x = runs[y].x + k;
            if (q + 2 > rlen || x >= w) continue;
            uint16_t v = *(uint16_t *)(raw + q); if (v == key) continue;
            px[y * w + x] = 0xff000000u | ((uint32_t)((v >> 11) << 3) << 16) | ((uint32_t)(((v >> 5) & 63) << 2) << 8) | ((v & 31) << 3);
        }
        img->w = w; img->h = h; img->px = px; img->ax = 0; img->ay = h - 1;
        if (e->size1 == 8) { int16_t *a = (int16_t *)(A->data + e->off1); img->ax = a[0]; img->ay = a[1]; }
        ok = true;
    done:
        free(runs);
        if (!ok) free(px);
    }
    free(raw);
    return ok;
}

bool spr_has(uint32_t group) {
    Archive *A; const Entry *e = find(group, &A);
    return e && e->size0 >= 24;
}

const Image *spr_get(uint32_t group) {
    unsigned h = (group * 2654435761u) >> 20 & (CACHE_SIZE - 1);
    for (int probe = 0; probe < 16; probe++) {
        CacheSlot *s = &g_cache[(h + probe) & (CACHE_SIZE - 1)];
        if (s->group == group && (s->ok || s->img.w == -1)) return s->ok ? &s->img : NULL;
        if (!s->group) {
            s->group = group;
            s->ok = decode(group, &s->img);
            if (!s->ok) s->img.w = -1;
            return s->ok ? &s->img : NULL;
        }
    }
    return NULL;
}

// Mirror-free recolor helper: make a tinted copy (used to mark the player / knocked-out citizens).
Image *img_tinted(const Image *src, uint32_t mul) {
    Image *d = malloc(sizeof *d); *d = *src; d->px = malloc(src->w * src->h * 4);
    unsigned mr = (mul >> 16) & 255, mg = (mul >> 8) & 255, mb = mul & 255;
    for (int i = 0; i < src->w * src->h; i++) {
        uint32_t c = src->px[i];
        if (!(c >> 24)) { d->px[i] = 0; continue; }
        unsigned r = ((c >> 16) & 255) * mr / 255, g = ((c >> 8) & 255) * mg / 255, b = (c & 255) * mb / 255;
        d->px[i] = (c & 0xff000000u) | (r << 16) | (g << 8) | b;
    }
    return d;
}
