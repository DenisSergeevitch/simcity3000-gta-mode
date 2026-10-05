// Transportation/utility network map, rebuilt from SIMNTWRK.DLL's network piece objects.
// SIMNTWRK FUN_1000bdcd creates one 32-byte (or 28-byte) object per network tile; its class vtable depends on the
// network type (1..22). Piece layout:
//   +0 vtable, +4 vtable2, +8 ptr, +0xc ptr,
//   +0x10 u32: byte0 = 1, byte2 = network type,
//   +0x14 u32: x = bits 0-10, z = bits 11-21, altitude = bits 22-29 (world y = alt * 256), bits 30-31 = variant,
//   +0x18 u16 piece code (shape).
// We scan the heap for these objects (no hooks needed) and keep a per-tile bitmask of network types.
#include "net.h"
#include <string.h>
#include <stdlib.h>

static const uint32_t NET_VT[23] = { 0,
    0x1002b724, 0x1002b3cc, 0x1002b220, 0x1002b074, 0x1002ad1c, 0x1002ab70, 0x1002a9c4, 0x1002a818, 0x1002aec8,
    0x1002b578, 0x1002a66c, 0x1002a4c0, 0x1002a314, 0x1002a168, 0x10029fbc, 0x10029e10, 0x10029c64, 0x10029ab8,
    0x1002990c, 0x10029760, 0x100295b4, 0x10029408 };

uint32_t g_net[256][256];      // bit t set: network type t on tile [x][z]
uint8_t g_net_alt[256][256];   // altitude of the (last) piece on the tile
uint8_t g_net_code[256][256];  // piece shape code (low byte)
int g_net_count[23];
bool g_net_ok;

int net_scan(void) {
    uint32_t rt[23], lo = 0xffffffff, hi = 0;
    for (int t = 1; t <= 22; t++) { rt[t] = ghidra_to_rt("SIMNTWRK.DLL", NET_VT[t]); if (rt[t] < lo) lo = rt[t]; if (rt[t] > hi) hi = rt[t]; }
    if (!rt[1]) return 0;
    memset(g_net, 0, sizeof g_net); memset(g_net_count, 0, sizeof g_net_count);
    static uint8_t buf[1 << 16]; MEMORY_BASIC_INFORMATION mi; int total = 0;
    for (uintptr_t a = 0x10000; a < 0x7fff0000; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
        if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
        uintptr_t rs = (uintptr_t)mi.BaseAddress, re = rs + mi.RegionSize;
        for (uintptr_t c = rs; c < re; c += sizeof buf - 32) {
            size_t n = re - c < sizeof buf ? re - c : sizeof buf;
            if (n < 32 || !mem_read(c, buf, n)) continue;
            for (size_t i = 0; i + 28 <= n; i += 4) {
                uint32_t v = *(uint32_t *)(buf + i);
                if (v < lo || v > hi) continue;
                uint32_t tag = *(uint32_t *)(buf + i + 0x10), pos = *(uint32_t *)(buf + i + 0x14);
                int t = (tag >> 16) & 0xff;
                if ((tag & 0xffff) != 1 || t < 1 || t > 22 || v != rt[t]) continue;
                int x = pos & 0x7ff, z = (pos >> 11) & 0x7ff;
                if (x > 255 || z > 255) continue;
                if (!(g_net[x][z] & (1u << t))) { g_net_count[t]++; total++; }
                g_net[x][z] |= 1u << t;
                g_net_alt[x][z] = (pos >> 22) & 0xff;
                g_net_code[x][z] = *(uint8_t *)(buf + i + 0x18);
            }
        }
    }
    g_net_ok = total > 0;
    LOG("net_scan: %d network tiles (roads %d)", total, g_net_count[NET_ROAD]);
    return total;
}

bool net_has(int x, int z, uint32_t mask) {
    return x >= 0 && z >= 0 && x < 256 && z < 256 && (g_net[x][z] & mask);
}

// ---------------------------------------------------------------- SIMGEOM occupants (buildings, flora, ...)
// Same packed position as network pieces; tag word = byte0 1, byte2 subtype; class -> (tag offset).
typedef struct { uint32_t vt; int tagoff; } GeoClass;
static const GeoClass GEO[] = { { 0x10029ba4, 0x10 }, { 0x1002bb80, 0x14 }, { 0x1002b058, 0x0c } };
uint8_t g_geo[256][256];        // bit k: SIMGEOM class k occupies the tile
uint8_t g_geo_sub[3][256][256]; // subtype byte per class
int g_geo_count[3];

int geo_scan(void) {
    uint32_t rt[3]; for (int k = 0; k < 3; k++) rt[k] = ghidra_to_rt("SIMGEOM.DLL", GEO[k].vt);
    if (!rt[0]) return 0;
    memset(g_geo, 0, sizeof g_geo); memset(g_geo_count, 0, sizeof g_geo_count);
    static uint8_t buf[1 << 16]; MEMORY_BASIC_INFORMATION mi; int total = 0;
    for (uintptr_t a = 0x10000; a < 0x7fff0000; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
        if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
        uintptr_t rs = (uintptr_t)mi.BaseAddress, re = rs + mi.RegionSize;
        for (uintptr_t c = rs; c < re; c += sizeof buf - 32) {
            size_t n = re - c < sizeof buf ? re - c : sizeof buf;
            if (n < 32 || !mem_read(c, buf, n)) continue;
            for (size_t i = 0; i + 32 <= n; i += 4) {
                uint32_t v = *(uint32_t *)(buf + i); int k;
                for (k = 0; k < 3 && v != rt[k]; k++);
                if (k == 3) continue;
                uint32_t tag = *(uint32_t *)(buf + i + GEO[k].tagoff), pos = *(uint32_t *)(buf + i + GEO[k].tagoff + 4);
                if (k != 2 && ((tag & 0xff) < 1 || (tag & 0xff) > 8 || ((tag >> 8) & 0xff))) continue;
                int x = pos & 0x7ff, z = (pos >> 11) & 0x7ff;
                if (x > 255 || z > 255) continue;
                if (!(g_geo[x][z] & (1 << k))) { g_geo_count[k]++; total++; }
                g_geo[x][z] |= 1 << k; g_geo_sub[k][x][z] = (tag >> 16) & 0xff;
            }
        }
    }
    LOG("geo_scan: %d occupied tiles (%d/%d/%d)", total, g_geo_count[0], g_geo_count[1], g_geo_count[2]);
    return total;
}
