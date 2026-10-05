// Camera: SIMSPR.DLL view state and its world->screen projection (see MODLOG.md, "Camera").
#include "camera.h"
#include <math.h>

int g_scaleA[5] = { 4, 8, 16, 32, 64 };   // SIMSPR 0x1007183c
int g_scaleB[5] = { 2, 4, 8, 16, 32 };    // SIMSPR 0x10071850
int g_scaleC[5] = { 1, 2, 4, 8, 16 };     // SIMSPR 0x10071864

static uintptr_t g_viewptr_addr;

bool cam_read(Camera *c) {
    c->valid = false;
    if (!g_viewptr_addr) g_viewptr_addr = ghidra_to_rt("SIMSPR.DLL", 0x10072bec);
    if (!g_viewptr_addr) return false;
    uint32_t view = 0; if (!mem_read(g_viewptr_addr, &view, 4) || !view) return false;
    int32_t v[2], r[4];
    if (!mem_read(view + 0x28, v, 8) || !mem_read(view + 0x54, r, 16)) return false;
    if (v[0] < 0 || v[0] > 4 || v[1] < 0 || v[1] > 3) return false;
    c->view = view; c->zoom = v[0]; c->rot = v[1];
    c->left = r[0]; c->top = r[1]; c->right = r[2]; c->bottom = r[3];
    c->valid = true;
    return true;
}

static void rotate(int rot, int x, int z, int *ox, int *oz) {
    switch (rot) {
    case 0: *ox = x; *oz = z; break;
    case 1: *ox = 0xffff - z; *oz = x; break;
    case 2: *ox = 0xffff - x; *oz = 0xffff - z; break;
    default: *ox = z; *oz = 0xffff - x; break;
    }
}

void cam_project(const Camera *c, float x, float y, float z, int *sx, int *sy) {
    int ix = (int)lrintf(x), iz = (int)lrintf(z), iy = (int)lrintf(y), rx, rz;
    rotate(c->rot, ix, iz, &rx, &rz);
    // The game's screen origin sits one tile (A, -B) away from the projected rect (verified against SIMSPR's
    // own screen->tile picker FUN_1000a48a, which returns x+1 for the uncorrected formula).
    *sx = (g_scaleA[c->zoom] * (rz - rx)) / 256 - c->left + g_scaleA[c->zoom];
    *sy = (g_scaleB[c->zoom] * (rz + rx)) / 256 - (g_scaleC[c->zoom] * iy) / 256 - c->top - g_scaleB[c->zoom];
}

void cam_unproject(const Camera *c, int sx, int sy, float y, float *x, float *z) {
    // sx + left = A*(rz-rx)/256 ; sy + top + C*y/256 = B*(rz+rx)/256
    float u = (sx + c->left - g_scaleA[c->zoom]) * 256.0f / g_scaleA[c->zoom];
    float v = (sy + c->top + g_scaleB[c->zoom] + g_scaleC[c->zoom] * y / 256.0f) * 256.0f / g_scaleB[c->zoom];
    float rz = (u + v) / 2, rx = (v - u) / 2;
    switch (c->rot) {   // invert rotate()
    case 0: *x = rx; *z = rz; break;
    case 1: *x = rz; *z = 0xffff - rx; break;
    case 2: *x = 0xffff - rx; *z = 0xffff - rz; break;
    default: *x = 0xffff - rz; *z = rx; break;
    }
}

void cam_axes(const Camera *c, float *xdx, float *xdy, float *zdx, float *zdy) {
    int x0, y0, x1, y1, x2, y2;
    cam_project(c, 32768, 0, 32768, &x0, &y0);
    cam_project(c, 32768 + 4096, 0, 32768, &x1, &y1);
    cam_project(c, 32768, 0, 32768 + 4096, &x2, &y2);
    float l1 = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)), l2 = sqrtf((x2 - x0) * (x2 - x0) + (y2 - y0) * (y2 - y0));
    *xdx = (x1 - x0) / l1; *xdy = (y1 - y0) / l1; *zdx = (x2 - x0) / l2; *zdy = (y2 - y0) / l2;
}

bool cam_scroll_by(int dx, int dy) {
    Camera c; if (!cam_read(&c)) return false;
    void *fn = (*(void ***)c.view)[0x2c / 4];
    uint32_t args[3] = { (uint32_t)dx, (uint32_t)dy, 1 };
    call_raw(fn, c.view, args, 3);
    return true;
}
