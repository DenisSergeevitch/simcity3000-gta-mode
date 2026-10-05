#pragma once
#include "util.h"

// Network type ids (SIMNTWRK factory FUN_1000bdcd, case k -> type k+1). Meanings confirmed in game are in MODLOG.md.
enum { NET_ROAD = 1 };
extern uint32_t g_net[256][256];
extern uint8_t g_net_alt[256][256];
extern uint8_t g_net_code[256][256];
extern int g_net_count[23];
extern bool g_net_ok;
int net_scan(void);                         // rebuild from memory; returns number of network tiles
bool net_has(int x, int z, uint32_t mask);  // mask of (1 << type)
extern uint8_t g_geo[256][256];
extern uint8_t g_geo_sub[3][256][256];
extern int g_geo_count[3];
int geo_scan(void);
