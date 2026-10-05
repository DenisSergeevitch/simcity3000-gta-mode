#pragma once
#include "util.h"

// SIMSPR view tile grid: view+0x24 -> uint8_t *cols[mapw]; cols[x] + z*20 is a 20-byte record:
//   +0 ptr, +4 u16 32, u16 32, +8 u8 x, +9 u8 z (lot origin), +0xa u8, +0xb u8 altitude (world y = alt*256)
typedef struct {
    bool ok;
    int mapw, maph;                  // tiles
    uintptr_t cols;                  // uint8_t **
} WorldInfo;

bool world_read(WorldInfo *w);
float world_ground(const WorldInfo *w, float x, float z);     // world y at (x,z)
bool world_tile(const WorldInfo *w, int tx, int tz, uint8_t rec[20]);
bool world_inside(const WorldInfo *w, float x, float z);
