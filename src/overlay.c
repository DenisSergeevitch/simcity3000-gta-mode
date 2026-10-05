// Overlay compositing at present time.
// cnc-ddraw's OpenGL renderer uploads the game's primary surface every frame with
//   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, fmt, type, primary->surface)
// through a function pointer it resolved with GetProcAddress (cnc-ddraw src/opengl_utils.c). We swap that pointer
// for our hook, copy the frame, draw our sprites into the copy and upload the copy. The game's own surfaces are
// never touched, so there are no trails and no interaction with its dirty-rectangle redraw.
#include "overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNSIGNED_BYTE        0x1401
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367

typedef void (WINAPI *PFN_TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static PFN_TexSubImage2D g_real;
static uint32_t *g_slot;              // the patched pointer inside cnc-ddraw
static void *g_scratch; static size_t g_scratch_size;
static CRITICAL_SECTION g_cs; static bool g_cs_init;
#define MAX_ITEMS 2048
static DrawItem g_build[MAX_ITEMS], g_live[MAX_ITEMS];
static int g_nbuild, g_nlive;
static int g_clip[4] = { 0, 0, 800, 600 };
static int g_excl[4] = { 0, 0, 0, 0 };   // a rectangle inside the clip that stays untouched (minimap panel)
static char g_grab_path[MAX_PATH];
volatile LONG g_overlay_frames;
volatile LONG g_overlay_commits;   // caches must not free an image referenced by a recently committed list
static unsigned g_last_fmt, g_last_type; static int g_last_w, g_last_h;

void overlay_set_clip(int x0, int y0, int x1, int y1) { g_clip[0] = x0; g_clip[1] = y0; g_clip[2] = x1; g_clip[3] = y1; }
void overlay_set_exclude(int x0, int y0, int x1, int y1) { g_excl[0] = x0; g_excl[1] = y0; g_excl[2] = x1; g_excl[3] = y1; }

// ------------------------------------------------------------------ pixel writers
typedef struct { uint8_t *base; int w, h, bpp, pitch; } Frame;

static inline void put(Frame *f, int x, int y, uint32_t c) {
    if (x < g_clip[0] || y < g_clip[1] || x >= g_clip[2] || y >= g_clip[3] || x >= f->w || y >= f->h) return;
    if (x >= g_excl[0] && y >= g_excl[1] && x < g_excl[2] && y < g_excl[3]) return;
    unsigned a = c >> 24; if (!a) return;
    unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (f->bpp == 2) {
        uint16_t *p = (uint16_t *)(f->base + y * f->pitch) + x;
        if (a < 255) {
            unsigned d = *p, dr = (d >> 11) << 3, dg = ((d >> 5) & 63) << 2, db = (d & 31) << 3;
            r = (r * a + dr * (255 - a)) / 255; g = (g * a + dg * (255 - a)) / 255; b = (b * a + db * (255 - a)) / 255;
        }
        *p = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    } else {
        uint32_t *p = (uint32_t *)(f->base + y * f->pitch) + x;
        if (a < 255) {
            unsigned d = *p, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
            r = (r * a + dr * (255 - a)) / 255; g = (g * a + dg * (255 - a)) / 255; b = (b * a + db * (255 - a)) / 255;
        }
        *p = 0xff000000u | (r << 16) | (g << 8) | b;
    }
}

static void draw_line(Frame *f, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (int guard = 0; guard < 4000; guard++) {
        put(f, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

static void draw_image(Frame *f, const DrawItem *d) {
    const Image *im = d->img; if (!im || !im->px) return;
    int ox = d->x - im->ax, oy = d->y - im->ay;
    for (int y = 0; y < im->h; y++) {
        int sy = oy + y; if (sy < g_clip[1] || sy >= g_clip[3]) continue;
        const uint32_t *row = im->px + y * im->w;
        for (int x = 0; x < im->w; x++) if (row[x] >> 24) put(f, ox + x, sy, row[x]);
    }
}

// 3x5 pixel font for debug labels (digits, letters A-Z, a few symbols)
static const char *FONT_CH = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-:.,!?/ ";
static const uint16_t FONT[] = {
    0x7B6F,0x2C97,0x73E7,0x73CF,0x5BC9,0x79CF,0x79EF,0x7249,0x7BEF,0x7BCF,
    0x2BED,0x6BAE,0x7927,0x6B6E,0x79A7,0x79A4,0x796F,0x5BED,0x7497,0x124F,0x5BAD,0x4927,0x5FED,0x6B6D,0x2B6A,
    0x6BA4,0x2B76,0x6BAD,0x388E,0x7492,0x5B6F,0x5B6A,0x5B7D,0x5AAD,0x5A92,0x72A7,0x01C0,0x0410,0x0002,0x0014,
    0x2482,0x6282,0x1250,0x0000 };
static void draw_text(Frame *f, int x, int y, const char *s, uint32_t c) {
    for (; *s; s++, x += 4) {
        char ch = (*s >= 'a' && *s <= 'z') ? *s - 32 : *s;
        const char *p = strchr(FONT_CH, ch); if (!p) continue;
        uint16_t g = FONT[p - FONT_CH];
        for (int r = 0; r < 5; r++) for (int k = 0; k < 3; k++)
            if (g & (1 << (14 - (r * 3 + k)))) { put(f, x + k, y + r, 0xff000000); put(f, x + k - 1, y + r - 1, c); }
    }
}

// X-ray silhouette for a sprite that the game draws itself (native sprite): find where it actually landed (the
// game's blit can sit a pixel off our projection), then mark the sprite pixels that something else covers.
static inline uint32_t frame_px(Frame *f, int x, int y) {
    if (f->bpp == 2) { uint16_t p = ((uint16_t *)(f->base + y * f->pitch))[x]; return p; }
    uint32_t p = ((uint32_t *)(f->base + y * f->pitch))[x];
    return (((p >> 16) & 255) >> 3) << 11 | (((p >> 8) & 255) >> 2) << 5 | ((p & 255) >> 3);
}
static inline uint32_t to565(uint32_t c) { return (((c >> 16) & 255) >> 3) << 11 | (((c >> 8) & 255) >> 2) << 5 | ((c & 255) >> 3); }
static void draw_ghost(Frame *f, const DrawItem *d) {
    const Image *im = d->img; if (!im || !im->px) return;
    int bx = 0, by = 0, best = -1, total = 0;
    for (int oy = -3; oy <= 3; oy++) for (int ox = -3; ox <= 3; ox++) {   // best alignment with the frame
        int hit = 0, n = 0;
        for (int y = 0; y < im->h; y++) {
            int sy = d->y - im->ay + oy + y; if (sy < 0 || sy >= f->h) continue;
            for (int x = 0; x < im->w; x++) {
                uint32_t c = im->px[y * im->w + x]; if (!(c >> 24)) continue;
                int sx = d->x - im->ax + ox + x; if (sx < 0 || sx >= f->w) continue;
                n++; hit += frame_px(f, sx, sy) == to565(c);
            }
        }
        if (hit > best) { best = hit; bx = ox; by = oy; total = n; }
    }
    if (total == 0 || best > total * 0.9f) return;   // (almost) fully visible: nothing to add
    if (best < total * 0.2f) bx = by = 0;           // fully covered: trust our projection
    for (int y = 0; y < im->h; y++) {
        int sy = d->y - im->ay + by + y; if (sy < g_clip[1] || sy >= g_clip[3] || sy >= f->h) continue;
        for (int x = 0; x < im->w; x++) {
            uint32_t c = im->px[y * im->w + x]; if (!(c >> 24)) continue;
            int sx = d->x - im->ax + bx + x; if (sx < g_clip[0] || sx >= g_clip[2] || sx >= f->w) continue;
            if (frame_px(f, sx, sy) == to565(c)) continue;   // this pixel is visible: the game drew it
            bool edge = x == 0 || y == 0 || x == im->w - 1 || y == im->h - 1 ||
                        !(im->px[y * im->w + x - 1] >> 24) || !(im->px[y * im->w + x + 1] >> 24) ||
                        !(im->px[(y - 1) * im->w + x] >> 24) || !(im->px[(y + 1) * im->w + x] >> 24);
            put(f, sx, sy, edge ? (d->color | 0xff000000u) : ((d->color & 0xffffff) | 0x60000000u));
        }
    }
}

static int cmp_depth(const void *a, const void *b) {
    const DrawItem *x = a, *y = b; return (x->depth > y->depth) - (x->depth < y->depth);
}

static void composite(Frame *f) {
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nlive; i++) {
        const DrawItem *d = &g_live[i];
        switch (d->kind) {
        case OV_IMAGE: draw_image(f, d); break;
        case OV_LINE: draw_line(f, d->x, d->y, d->x2, d->y2, d->color); break;
        case OV_RECT: for (int y = 0; y < d->y2; y++) for (int x = 0; x < d->x2; x++) put(f, d->x + x, d->y + y, d->color); break;
        case OV_TEXT: draw_text(f, d->x, d->y, d->text, d->color); break;
        case OV_GHOST: draw_ghost(f, d); break;
        }
    }
    LeaveCriticalSection(&g_cs);
}

static void save_bmp(const char *path, Frame *f) {
    FILE *o = fopen(path, "wb"); if (!o) return;
    int rowb = f->w * 3, pad = (4 - rowb % 4) % 4, size = 54 + (rowb + pad) * f->h;
    uint8_t h[54] = { 'B', 'M' };
    *(uint32_t *)(h + 2) = size; *(uint32_t *)(h + 10) = 54; *(uint32_t *)(h + 14) = 40;
    *(int32_t *)(h + 18) = f->w; *(int32_t *)(h + 22) = f->h; *(uint16_t *)(h + 26) = 1; *(uint16_t *)(h + 28) = 24;
    fwrite(h, 1, 54, o);
    uint8_t *row = malloc(rowb + pad); memset(row, 0, rowb + pad);
    for (int y = f->h - 1; y >= 0; y--) {
        for (int x = 0; x < f->w; x++) {
            unsigned r, g, b;
            if (f->bpp == 2) { uint16_t p = ((uint16_t *)(f->base + y * f->pitch))[x]; r = (p >> 11) << 3; g = ((p >> 5) & 63) << 2; b = (p & 31) << 3; }
            else { uint32_t p = ((uint32_t *)(f->base + y * f->pitch))[x]; r = (p >> 16) & 255; g = (p >> 8) & 255; b = p & 255; }
            row[x * 3] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r;
        }
        fwrite(row, 1, rowb + pad, o);
    }
    free(row); fclose(o);
}

// ------------------------------------------------------------------ recorder
// "rec start <file>": a writer thread samples the latest composited frame at a fixed 30 fps and appends it raw
// (w*h*bpp bytes per frame), so wall-clock timing is exact even when the game renders unevenly. Encode with
//   ffmpeg -f rawvideo -pix_fmt rgb565le -s 800x600 -r 30 -i take.raw ...
static CRITICAL_SECTION g_rec_cs;
static void *g_rec_frame; static size_t g_rec_size; static int g_rec_w, g_rec_h, g_rec_bpp;
static volatile LONG g_rec_on, g_rec_count; static HANDLE g_rec_thread; static FILE *g_rec_file;

static DWORD WINAPI rec_thread(void *arg) {
    (void)arg;
    void *buf = NULL; size_t bsz = 0;
    while (g_rec_on && !g_rec_size) Sleep(5);   // the clock starts at the first frame
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (LONG n = 0; g_rec_on; ) {
        QueryPerformanceCounter(&t);
        if ((double)(t.QuadPart - t0.QuadPart) / f.QuadPart < n / 30.0) { Sleep(1); continue; }
        EnterCriticalSection(&g_rec_cs);
        if (bsz != g_rec_size) { free(buf); bsz = g_rec_size; buf = malloc(bsz); }
        memcpy(buf, g_rec_frame, bsz);
        LeaveCriticalSection(&g_rec_cs);
        fwrite(buf, 1, bsz, g_rec_file);
        g_rec_count = ++n;
    }
    free(buf);
    return 0;
}

static void rec_frame(const Frame *f) {
    size_t need = (size_t)f->w * f->h * f->bpp;
    EnterCriticalSection(&g_rec_cs);
    if (g_rec_size != need) { free(g_rec_frame); g_rec_frame = malloc(need); g_rec_size = need; }
    memcpy(g_rec_frame, f->base, need); g_rec_w = f->w; g_rec_h = f->h; g_rec_bpp = f->bpp;
    LeaveCriticalSection(&g_rec_cs);
}

static void rec_stop(void) {
    if (!g_rec_on) return;
    g_rec_on = 0; WaitForSingleObject(g_rec_thread, 5000); CloseHandle(g_rec_thread); g_rec_thread = NULL;
    fclose(g_rec_file); g_rec_file = NULL;
}

static bool rec_start(const char *path) {
    rec_stop();
    static bool init; if (!init) { InitializeCriticalSection(&g_rec_cs); init = true; }
    g_rec_file = fopen(path, "wb"); if (!g_rec_file) return false;
    setvbuf(g_rec_file, NULL, _IOFBF, 4 << 20);
    EnterCriticalSection(&g_rec_cs); free(g_rec_frame); g_rec_frame = NULL; g_rec_size = 0; LeaveCriticalSection(&g_rec_cs);
    g_rec_count = 0; g_rec_on = 1;
    g_rec_thread = CreateThread(NULL, 0, rec_thread, NULL, 0, NULL);
    return true;
}

static void WINAPI hook_TexSubImage2D(unsigned target, int level, int xo, int yo, int w, int h,
                                      unsigned fmt, unsigned type, const void *pixels) {
    g_last_fmt = fmt; g_last_type = type; g_last_w = w; g_last_h = h;
    int bpp = type == GL_UNSIGNED_SHORT_5_6_5 ? 2 : (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8_REV) ? 4 : 0;
    if (!pixels || level != 0 || xo || yo || w < 320 || h < 200 || !bpp || (bpp == 4 && fmt == 0x1908 /*RGBA palette*/ && w == 256)) {
        g_real(target, level, xo, yo, w, h, fmt, type, pixels); return;
    }
    size_t need = (size_t)w * h * bpp;
    if (g_scratch_size < need) { free(g_scratch); g_scratch = malloc(need); g_scratch_size = need; }
    memcpy(g_scratch, pixels, need);
    Frame f = { g_scratch, w, h, bpp, w * bpp };
    composite(&f);
    InterlockedIncrement(&g_overlay_frames);
    if (g_grab_path[0]) { save_bmp(g_grab_path, &f); g_grab_path[0] = 0; }
    if (g_rec_on) rec_frame(&f);
    g_real(target, level, xo, yo, w, h, fmt, type, g_scratch);
}

bool overlay_install(void) {
    if (g_slot) return true;
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = true; }
    HMODULE gl = GetModuleHandleA("opengl32.dll"), dd = GetModuleHandleA("ddraw.dll");
    if (!gl || !dd) return false;
    uint32_t target = (uint32_t)(uintptr_t)GetProcAddress(gl, "glTexSubImage2D");
    if (!target) return false;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)dd + ((IMAGE_DOS_HEADER *)dd)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uint32_t *p = (uint32_t *)((uint8_t *)dd + sec[i].VirtualAddress);
        size_t n = sec[i].Misc.VirtualSize / 4;
        for (size_t k = 0; k < n; k++) if (p[k] == target) {
            g_real = (PFN_TexSubImage2D)(uintptr_t)target;
            g_slot = &p[k];
            InterlockedExchange((LONG *)g_slot, (LONG)(uintptr_t)hook_TexSubImage2D);
            LOG("overlay: hooked cnc-ddraw glTexSubImage2D slot %p (sec %.8s)", (void *)g_slot, sec[i].Name);
            return true;
        }
    }
    return false;
}

void overlay_begin(void) { g_nbuild = 0; }
void overlay_add(const DrawItem *d) { if (g_nbuild < MAX_ITEMS) g_build[g_nbuild++] = *d; }
void overlay_commit(void) {
    qsort(g_build, g_nbuild, sizeof *g_build, cmp_depth);
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = true; }
    EnterCriticalSection(&g_cs);
    memcpy(g_live, g_build, g_nbuild * sizeof *g_build); g_nlive = g_nbuild;
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_overlay_commits);
}
void overlay_grab(const char *path) { snprintf(g_grab_path, sizeof g_grab_path, "%s", path); }

bool overlay_cmd(int argc, char **argv) {
    if (!strcmp(argv[0], "overlay")) {
        bool ok = overlay_install();
        out_printf("installed=%d slot=%p frames=%ld last fmt=%x type=%x %dx%d\n", ok, (void *)g_slot, g_overlay_frames,
                   g_last_fmt, g_last_type, g_last_w, g_last_h);
    } else if (!strcmp(argv[0], "grab")) {
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", g_moddir, argc > 1 ? argv[1] : "grab.bmp");
        overlay_grab(p); out_printf("grab queued -> %s\n", p);
    } else if (!strcmp(argv[0], "rec")) {   // rec start <file> | rec stop | rec
        if (argc > 2 && !strcmp(argv[1], "start")) {
            char p[MAX_PATH];
            if (strchr(argv[2], ':')) snprintf(p, sizeof p, "%s", argv[2]); else snprintf(p, sizeof p, "%s\\%s", g_moddir, argv[2]);
            out_printf(rec_start(p) ? "recording -> %s\n" : "cannot open %s\n", p);
        } else if (argc > 1 && !strcmp(argv[1], "stop")) rec_stop();
        out_printf("rec on=%ld frames=%ld size=%dx%d bpp=%d\n", g_rec_on, g_rec_count, g_rec_w, g_rec_h, g_rec_bpp);
    } else return false;
    return true;
}
