#pragma once
#include "util.h"

// A sprite image: premultiplied-free BGRA8 pixels, alpha 0 = transparent.
typedef struct {
    int w, h;          // size in pixels
    int ax, ay;        // anchor (pixel that sits on the world position, usually the feet)
    uint32_t *px;      // w*h BGRA (0xAARRGGBB)
} Image;

// OV_GHOST: an image the game itself draws (a native sprite); wherever something in the frame covers it, the
// covered pixels are shown as an x-ray silhouette in `color` (outline solid, inside translucent).
enum { OV_IMAGE = 1, OV_RECT, OV_LINE, OV_TEXT, OV_GHOST };

typedef struct {
    int kind;
    int x, y;          // screen position (anchor), game pixels
    int x2, y2;        // line end / rect size
    uint32_t color;    // 0xAARRGGBB
    const Image *img;
    int depth;         // sort key (larger = drawn later)
    char text[48];
} DrawItem;

bool overlay_install(void);          // patch cnc-ddraw's glTexSubImage2D pointer; idempotent
void overlay_begin(void);            // main thread: start a new draw list
void overlay_add(const DrawItem *d); // main thread
void overlay_commit(void);           // main thread: publish the list to the render thread
void overlay_set_clip(int x0, int y0, int x1, int y1);
void overlay_set_exclude(int x0, int y0, int x1, int y1);
void overlay_grab(const char *path); // save the next composited frame as a BMP
bool overlay_cmd(int argc, char **argv);
extern volatile LONG g_overlay_frames;   // count of composited frames
extern volatile LONG g_overlay_commits;  // count of committed draw lists
