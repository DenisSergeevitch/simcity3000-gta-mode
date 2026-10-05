#pragma once
#include "util.h"

// Native sprites: the mod's people and cars drawn by SIMSPR's own renderer, sorted with the game's buildings,
// trees and road props, so whatever stands in front of them hides them. See native.c.

// Mirrors the 0x14-byte layout of the game's sprite image (vtable, id, resource, child, two flag bytes); the rest
// is ours. Lives in a static pool: the view keeps pointers to it.
typedef struct NatImg {
    void **vt;          // +0x00 copy of the game image vtable with our FrameKey/AddRef/Release
    uint32_t id;        // +0x04 low 17 bits are handed to the blitter as flags (0 = plain), keep 0
    void *res;          // +0x08 sprite resource (all frames of one sprite set)
    void *child;        // +0x0c second image drawn with this one (unused)
    uint8_t base, mode; // +0x10 / +0x11 used only by the game's own frame key; 0
    uint16_t pad;
    int key[5];         // frame index per zoom level (DAT instance number), -1 = not drawn at that zoom
    LONG refs;
    bool added;         // registered in the view at (x, z, y)
    int x, z, y;
    uintptr_t view;
    uint16_t set;
} NatImg;

bool nat_init(void);                         // false when SIMSPR's renderer isn't available
// Show the sprite set's frame key[zoom] at world (x, z) and height y; adds or moves the view's record and only
// touches the game when something changed.
void nat_show(NatImg *n, uint16_t set, const int key[5], float x, float z, float y);
void nat_hide(NatImg *n);                    // take it out of the view
void nat_forget(NatImg *n);                  // the view is gone (city closed): drop our state without calling it
bool nat_cmd(int argc, char **argv);         // "nat": stats
extern bool g_nat_ok;
extern int g_nat_calls;                      // game calls made (stats)
