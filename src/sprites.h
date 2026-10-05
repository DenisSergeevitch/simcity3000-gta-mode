#pragma once
#include "overlay.h"

// Sprite group IDs: (set << 16) | (frame * 16 + (zoom4 ? 8 : 0) + dir). People sets 0x2EE1..0x2F36 (People.DAT),
// vehicle sets in Vehicles.DAT. Dir 0..7 are the 8 screen headings of the game's sprites.
int spr_open(const char *relpath);          // e.g. "00000007_People.DAT"; returns archive index or -1
const Image *spr_get(uint32_t group);       // decoded + cached; NULL if missing
bool spr_has(uint32_t group);               // the group exists in an opened archive
Image *img_tinted(const Image *src, uint32_t mul);
