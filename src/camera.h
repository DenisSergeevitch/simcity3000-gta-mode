#pragma once
#include "util.h"

typedef struct {
    bool valid;
    uintptr_t view;               // SIMSPR view object
    int zoom, rot;                // 0..4 (4 = closest), 0..3
    int left, top, right, bottom; // visible rect in projected pixels
} Camera;

bool cam_read(Camera *c);
// World (x, height y, z) -> game screen pixels (800x600 frame). Same integer math as SIMSPR FUN_100241fb.
void cam_project(const Camera *c, float x, float y, float z, int *sx, int *sy);
// Inverse for height y: screen -> world (x, z)
void cam_unproject(const Camera *c, int sx, int sy, float y, float *x, float *z);
// Unit vectors on screen of +x and +z world axes (for movement and facing)
void cam_axes(const Camera *c, float *xdx, float *xdy, float *zdx, float *zdy);
bool cam_scroll_by(int dx, int dy);   // view->ScrollBy(dx, dy, true) (vtable +0x2c)
extern int g_scaleA[5], g_scaleB[5], g_scaleC[5];
