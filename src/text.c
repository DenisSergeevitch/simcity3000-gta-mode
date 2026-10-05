// HUD text rendered with GDI into small cached images (proper fonts, drawn by the overlay like sprites).
#include "text.h"
#include <stdlib.h>
#include <string.h>

typedef struct { char s[96]; uint32_t color; int size; Image img; LONG used; } TextSlot;
#define NTXT 256
static TextSlot g_txt[NTXT];

const Image *text_image(const char *s, uint32_t color, int size) {
    LONG now = g_overlay_commits;
    for (int i = 0; i < NTXT; i++)
        if (g_txt[i].img.px && g_txt[i].color == color && g_txt[i].size == size && !strcmp(g_txt[i].s, s)) { g_txt[i].used = now; return &g_txt[i].img; }
    // reuse the least recently used slot, but only if no recent draw list can still reference it
    int best = -1;
    for (int i = 0; i < NTXT; i++) {
        if (!g_txt[i].img.px) { best = i; break; }
        if (now - g_txt[i].used > 8 && (best < 0 || g_txt[i].used < g_txt[best].used)) best = i;
    }
    if (best < 0) return NULL;
    TextSlot *t = &g_txt[best];
    free(t->img.px); memset(t, 0, sizeof *t); t->used = now;
    strncpy(t->s, s, sizeof t->s - 1); t->color = color; t->size = size;
    HDC dc = CreateCompatibleDC(NULL);
    HFONT f = CreateFontA(-size, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          NONANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz; GetTextExtentPoint32A(dc, s, (int)strlen(s), &sz);
    int w = sz.cx + 2, h = sz.cy + 2;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    void *bits; HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ ob = SelectObject(dc, bm);
    memset(bits, 0, w * h * 4);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, 1, 1, s, (int)strlen(s));
    uint32_t *src = bits, *px = calloc(w * h, 4);
    // white glyph mask -> colored text with a 1px black outline
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        if (src[y * w + x] & 0xff) { px[y * w + x] = color; continue; }
        bool edge = false;
        for (int dy = -1; dy <= 1 && !edge; dy++) for (int dx = -1; dx <= 1; dx++) {
            int xx = x + dx, yy = y + dy;
            if (xx >= 0 && yy >= 0 && xx < w && yy < h && (src[yy * w + xx] & 0xff)) { edge = true; break; }
        }
        if (edge) px[y * w + x] = 0xff000000;
    }
    SelectObject(dc, ob); SelectObject(dc, of); DeleteObject(bm); DeleteObject(f); DeleteDC(dc);
    t->img.w = w; t->img.h = h; t->img.px = px; t->img.ax = 0; t->img.ay = 0;
    return &t->img;
}
