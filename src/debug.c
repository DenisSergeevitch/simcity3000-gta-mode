// Debug / RE helpers: world markers, agent tracking, camera goto, synthetic held keys.
#include "util.h"
#include "input.h"
#include "overlay.h"
#include "camera.h"
#include "gta.h"
#include "world.h"
#include "net.h"
#include "agents.h"
#include "native.h"
#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z; bool on; } Marker;
static Marker g_marks[16];
static uint32_t g_track[8];   // agent objects to mark every frame (STRTSIM: int pos at +0x44)

static bool cam_goto(float x, float y, float z) {
    Camera c; if (!cam_read(&c)) return false;
    int sx, sy; cam_project(&c, x, y, z, &sx, &sy);
    return cam_scroll_by(sx - 400, sy - 280);
}

static void push_line(const Camera *c, float x0, float y0, float z0, float x1, float y1, float z1, uint32_t col) {
    DrawItem d = { OV_LINE }; d.color = col; d.depth = 1000;
    cam_project(c, x0, y0, z0, &d.x, &d.y); cam_project(c, x1, y1, z1, &d.x2, &d.y2);
    overlay_add(&d);
}

static uint32_t g_netshow;   // network type mask to visualise
static const uint32_t NET_COL[23] = { 0, 0xffff3030, 0xff30ff30, 0xff3080ff, 0xffffff30, 0xffff30ff, 0xff30ffff, 0xffff8000,
    0xff8000ff, 0xffffffff, 0xff808080, 0xff804000, 0xff008040, 0xff400080, 0xffff8080, 0xff80ff80, 0xff8080ff, 0xffffff80,
    0xffff80ff, 0xff80ffff, 0xffc0c000, 0xff00c0c0, 0xffc000c0 };
static uint32_t g_geoshow; static bool g_agentshow;
static void draw_agents(const Camera *c) {
    if (!g_agentshow) return;
    static uint32_t last; if (GetTickCount() - last > 500) { last = GetTickCount(); agents_scan(); }
    for (int i = 0; i < g_nagents; i++) {
        Agent *a = &g_agents[i]; agent_refresh(a);
        int sx, sy; cam_project(c, a->x, a->y, a->z, &sx, &sy);
        if (sx < 0 || sy < 0 || sx > 720 || sy > 540) continue;
        char b[16]; snprintf(b, sizeof b, "%d", a->cls);
        DrawItem d = { OV_IMAGE }; d.img = text_image(b, 0xffffff00, 11); d.x = sx - 3; d.y = sy - 6; d.depth = 1 << 29; overlay_add(&d);
    }
}
static void draw_net(const Camera *c) {
    if (g_geoshow) {
        float cx, cz; cam_unproject(c, 360, 270, 7936, &cx, &cz);
        int tx0 = (int)cx / 256, tz0 = (int)cz / 256; static const uint32_t col[3] = { 0xff40ff40, 0xffff4040, 0xff4080ff };
        for (int x = tx0 - 8; x <= tx0 + 8; x++) for (int z = tz0 - 8; z <= tz0 + 8; z++) {
            if (x < 0 || z < 0 || x > 255 || z > 255) continue;
            uint32_t m = g_geo[x][z] & g_geoshow; if (!m) continue;
            int k = 0; while (!(m & (1u << k))) k++;
            float y = 31 * 256.0f, X = x * 256.0f, Z = z * 256.0f, in = 40 + k * 20;
            push_line(c, X + in, y, Z + in, X + 256 - in, y, Z + in, col[k]); push_line(c, X + 256 - in, y, Z + in, X + 256 - in, y, Z + 256 - in, col[k]);
            push_line(c, X + 256 - in, y, Z + 256 - in, X + in, y, Z + 256 - in, col[k]); push_line(c, X + in, y, Z + 256 - in, X + in, y, Z + in, col[k]);
        }
    }
    if (!g_netshow || !g_net_ok) return;
    float cx, cz; cam_unproject(c, 360, 270, 7936, &cx, &cz);
    int tx0 = (int)cx / 256, tz0 = (int)cz / 256;
    for (int x = tx0 - 8; x <= tx0 + 8; x++) for (int z = tz0 - 8; z <= tz0 + 8; z++) {
        if (x < 0 || z < 0 || x > 255 || z > 255) continue;
        uint32_t m = g_net[x][z] & g_netshow; if (!m) continue;
        int t = 1; while (!(m & (1u << t))) t++;
        float y = g_net_alt[x][z] * 256.0f, X = x * 256.0f, Z = z * 256.0f; uint32_t col = NET_COL[t];
        float in = 24;   // inset so neighbouring tiles stay distinguishable
        push_line(c, X + in, y, Z + in, X + 256 - in, y, Z + in, col); push_line(c, X + 256 - in, y, Z + in, X + 256 - in, y, Z + 256 - in, col);
        push_line(c, X + 256 - in, y, Z + 256 - in, X + in, y, Z + 256 - in, col); push_line(c, X + in, y, Z + 256 - in, X + in, y, Z + in, col);
    }
}

static struct { int vk; uint32_t until; } g_held[8];
void gta_hold(int vk, int ms) {
    for (int i = 0; i < 8; i++) if (!g_held[i].vk || g_held[i].vk == vk) {
        g_held[i].vk = vk; g_held[i].until = GetTickCount() + ms; input_key(vk, true); return;
    }
}
static void held_tick(void) {
    uint32_t now = GetTickCount();
    for (int i = 0; i < 8; i++) if (g_held[i].vk && (int32_t)(now - g_held[i].until) >= 0) { input_key(g_held[i].vk, false); g_held[i].vk = 0; }
}

void gta_debug_draw(const Camera *cc) {
    held_tick();
    Camera c = *cc;
    if (c.valid) { draw_net(&c); draw_agents(&c); }
    if (c.valid) {
        for (int i = 0; i < 16; i++) if (g_marks[i].on) {
            Marker *m = &g_marks[i];
            float tx = (float)((int)m->x & ~255), tz = (float)((int)m->z & ~255);
            uint32_t col = 0xffff2020;
            push_line(&c, tx, m->y, tz, tx + 256, m->y, tz, col);
            push_line(&c, tx + 256, m->y, tz, tx + 256, m->y, tz + 256, col);
            push_line(&c, tx + 256, m->y, tz + 256, tx, m->y, tz + 256, col);
            push_line(&c, tx, m->y, tz + 256, tx, m->y, tz, col);
            int sx, sy; cam_project(&c, m->x, m->y, m->z, &sx, &sy);
            DrawItem d = { OV_LINE }; d.color = 0xffffff00; d.depth = 1001;
            d.x = sx - 6; d.y = sy; d.x2 = sx + 6; d.y2 = sy; overlay_add(&d);
            d.x = sx; d.y = sy - 6; d.x2 = sx; d.y2 = sy + 6; overlay_add(&d);
        }
        for (int i = 0; i < 8; i++) if (g_track[i]) {
            int32_t p[3]; if (!mem_read(g_track[i] + 0x44, p, 12)) continue;
            int sx, sy; cam_project(&c, p[0], p[1], p[2], &sx, &sy);
            DrawItem d = { OV_LINE }; d.color = 0xff00ffff; d.depth = 1002;
            d.x = sx - 8; d.y = sy - 8; d.x2 = sx + 8; d.y2 = sy + 8; overlay_add(&d);
            d.x = sx - 8; d.y = sy + 8; d.x2 = sx + 8; d.y2 = sy - 8; overlay_add(&d);
        }
    }
}

bool gta_cmd(int argc, char **argv) {
    if (input_cmd(argc, argv) || overlay_cmd(argc, argv) || nat_cmd(argc, argv) || gta_game_cmd(argc, argv)) return true;
    if (!strcmp(argv[0], "cam")) {
        Camera c; if (!cam_read(&c)) { out_printf("no view\n"); return true; }
        out_printf("view=%08lx zoom=%d rot=%d rect=%d,%d,%d,%d\n", (unsigned long)c.view, c.zoom, c.rot, c.left, c.top, c.right, c.bottom);
        float x, z; cam_unproject(&c, 400, 300, 0, &x, &z);
        out_printf("screen center -> world x=%.0f z=%.0f (tile %d,%d)\n", x, z, (int)x / 256, (int)z / 256);
    } else if (!strcmp(argv[0], "mark") && argc >= 3) {      // mark <x> <z> [y] [slot]   (world units)
        int slot = argc > 4 ? atoi(argv[4]) : 0;
        g_marks[slot] = (Marker){ strtof(argv[1], 0), argc > 3 ? strtof(argv[3], 0) : 0, strtof(argv[2], 0), true };
    } else if (!strcmp(argv[0], "marktile") && argc >= 3) {  // marktile <tx> <tz> [y] [slot]
        int slot = argc > 4 ? atoi(argv[4]) : 0;
        g_marks[slot] = (Marker){ atoi(argv[1]) * 256 + 128, argc > 3 ? strtof(argv[3], 0) : 0, atoi(argv[2]) * 256 + 128, true };
    } else if (!strcmp(argv[0], "unmark")) memset(g_marks, 0, sizeof g_marks);
    else if (!strcmp(argv[0], "goto") && argc >= 4) out_printf("%d\n", cam_goto(strtof(argv[1], 0), strtof(argv[2], 0), strtof(argv[3], 0)));
    else if (!strcmp(argv[0], "track") && argc >= 2) {      // track <agent addr> [slot] ; centers on it
        int slot = argc > 2 ? atoi(argv[2]) : 0; g_track[slot] = addr_eval(argv[1]);
        int32_t p[3]; if (g_track[slot] && mem_read(g_track[slot] + 0x44, p, 12)) { out_printf("pos %ld %ld %ld\n", (long)p[0], (long)p[1], (long)p[2]); cam_goto(p[0], p[1], p[2]); }
    }
    else if (!strcmp(argv[0], "tiles") && argc >= 5) {     // tiles <tx> <tz> <dx> <dz> [n]: dump tile records along a line
        WorldInfo w; world_read(&w); int n = argc > 5 ? atoi(argv[5]) : 8;
        for (int i = 0; i < n; i++) {
            int tx = atoi(argv[1]) + i * atoi(argv[3]), tz = atoi(argv[2]) + i * atoi(argv[4]); uint8_t r[20];
            if (!world_tile(&w, tx, tz, r)) continue;
            out_printf("%3d,%3d:", tx, tz); for (int k = 0; k < 20; k++) out_printf(" %02x", r[k]); out_printf("\n");
        }
    }
    else if (!strcmp(argv[0], "pick") && argc >= 3) {     // pick <sx> <sy>: screen -> tile
        Camera c; WorldInfo w; cam_read(&c); world_read(&w); float x, z;
        cam_unproject(&c, atoi(argv[1]), atoi(argv[2]), world_ground(&w, 32768, 32768), &x, &z);
        out_printf("tile %d,%d (world %.0f,%.0f)\n", (int)x / 256, (int)z / 256, x, z);
    }
    else if (!strcmp(argv[0], "netscan")) {
        uint32_t t0 = GetTickCount(); int n = net_scan();
        out_printf("%d tiles in %lu ms\n", n, GetTickCount() - t0);
        for (int t = 1; t <= 22; t++) if (g_net_count[t]) out_printf("type %d: %d\n", t, g_net_count[t]);
    }
    else if (!strcmp(argv[0], "sprs")) {   // census of SIMSPR's dynamic sprite records, grouped by sprite resource
        uintptr_t view = 0; uint32_t hs[3];
        if (!mem_read(ghidra_to_rt("SIMSPR.DLL", 0x10072bec), &view, 4) || !mem_read(view + 0x3a4, hs, 12)) { out_printf("no view\n"); return true; }
        typedef struct { uint32_t res, n, rec, key[4], imgvt, id, nmov, mov; } Grp;
        static Grp g[96]; int ng = 0, total = 0;
        for (uint32_t b = hs[1]; b < hs[2]; b += 4) {
            uint32_t node; if (!mem_read(b, &node, 4)) break;
            for (int guard = 0; node && guard < 64; guard++) {
                uint32_t nv[2], rec[11], img[4], res[6];
                if (!mem_read(node, nv, 8) || !mem_read(nv[1], rec, 44) || !mem_read(rec[0], img, 16) || !mem_read(img[2], res, 24)) break;
                total++;
                int k; for (k = 0; k < ng && g[k].res != img[2]; k++);
                if (k == ng && ng < 96) { g[ng].res = img[2]; g[ng].n = g[ng].nmov = 0; g[ng].rec = nv[1]; memcpy(g[ng].key, res + 2, 16); g[ng].imgvt = img[0]; g[ng].id = img[1]; ng++; }
                if (k < ng) { g[k].n++; if (((rec[5] + 2) & 255) || ((rec[6] + 0) & 255)) { g[k].nmov++; g[k].mov = nv[1]; } }
                node = nv[0];
            }
        }
        out_printf("%d sprite records, %d resources\n", total, ng);
        for (int i = 0; i < ng; i++) {
            if (argc > 1 && !g[i].nmov) continue;   // "sprs m": only resources with off-grid (moving) sprites
            uint32_t rec[11]; mem_read(argc > 1 ? g[i].mov : g[i].rec, rec, 44);
            out_printf("mov=%u res %08x n=%3u key %04x %04x %04x %04x imgvt %08x id %08x | eg rec %08x x=%d z=%d y=%d layer=%u flags=%02x\n",
                g[i].nmov, g[i].res, g[i].n, g[i].key[0], g[i].key[1], g[i].key[2], g[i].key[3], g[i].imgvt, g[i].id, argc > 1 ? g[i].mov : g[i].rec,
                (int)rec[5], (int)rec[6], (int)rec[7], rec[10] & 0xff, (rec[10] >> 8) & 0xff);
        }
    }
    else if (!strcmp(argv[0], "agents")) {
        int n = agents_scan(); int cnt[16] = { 0 }; for (int i = 0; i < n; i++) cnt[g_agents[i].cls]++;
        out_printf("%d live agents:", n); for (int k = 0; k < 11; k++) out_printf(" c%d=%d", k, cnt[k]); out_printf("\n");
        for (int i = 0; i < n && i < (argc > 1 ? atoi(argv[1]) : 0); i++) out_printf("%08lx c%d type=%08lx pos %.0f,%.0f,%.0f dir %.2f,%.2f\n", (unsigned long)g_agents[i].addr, g_agents[i].cls, (unsigned long)g_agents[i].type, g_agents[i].x, g_agents[i].y, g_agents[i].z, g_agents[i].dx, g_agents[i].dz);
    }
    else if (!strcmp(argv[0], "agentshow")) g_agentshow = argc > 1 && atoi(argv[1]);
    else if (!strcmp(argv[0], "geoscan")) { int n = geo_scan(); out_printf("%d tiles: %d %d %d\n", n, g_geo_count[0], g_geo_count[1], g_geo_count[2]); }
    else if (!strcmp(argv[0], "geoshow")) g_geoshow = argc > 1 ? strtoul(argv[1], 0, 0) : 0;
    else if (!strcmp(argv[0], "geoat") && argc >= 3) { int x = atoi(argv[1]), z = atoi(argv[2]); out_printf("%d,%d geo=%x sub=%x,%x,%x\n", x, z, g_geo[x][z], g_geo_sub[0][x][z], g_geo_sub[1][x][z], g_geo_sub[2][x][z]); }
    else if (!strcmp(argv[0], "netshow")) g_netshow = argc > 1 ? strtoul(argv[1], 0, 0) : 0;
    else if (!strcmp(argv[0], "netat") && argc >= 3) { int x = atoi(argv[1]), z = atoi(argv[2]); out_printf("%d,%d mask=%08lx alt=%d code=%d\n", x, z, (unsigned long)g_net[x][z], g_net_alt[x][z], g_net_code[x][z]); }
    else if (!strcmp(argv[0], "untrack")) memset(g_track, 0, sizeof g_track);
    else if (!strcmp(argv[0], "scroll") && argc >= 3) out_printf("%d\n", cam_scroll_by(atoi(argv[1]), atoi(argv[2])));
    else return false;
    return true;
}
