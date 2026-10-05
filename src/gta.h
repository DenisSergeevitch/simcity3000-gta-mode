#pragma once
#include "camera.h"
typedef enum { GTA_OFF, GTA_CHOOSE, GTA_PLAY } GtaMode;
extern GtaMode g_mode;
void gta_start(void);
void gta_stop(void);
bool gta_game_cmd(int argc, char **argv);
void gta_debug_draw(const Camera *c);   // debug markers (debug.c)
void gta_hold(int vk, int ms);          // synthetic held key (debug.c)
