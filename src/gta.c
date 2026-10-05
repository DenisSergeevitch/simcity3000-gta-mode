// GTA mode: possess a citizen, walk with WASD, punch, steal and drive cars, inside any SimCity 3000 city.
// Entities live in world coordinates (256 units per tile) and are drawn by the overlay with the game's own sprites
// (People.DAT / Vehicles.DAT, decoded at runtime from the user's install).
#include "util.h"
#include "input.h"
#include "overlay.h"
#include "camera.h"
#include "sprites.h"
#include "world.h"
#include "text.h"
#include "gta.h"
#include "net.h"
#include "native.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
#define MAX_PEDS 64
#define MAX_CARS 18

// ------------------------------------------------------------------ tunables (world units, seconds)
#define WALK_SPEED   150.0f
#define RUN_SPEED    340.0f
#define NPC_SPEED     90.0f
#define FLEE_SPEED   300.0f
#define PUNCH_RANGE  110.0f
#define ENTER_RANGE  220.0f
#define CAR_MAX     1500.0f
#define CAR_REV      450.0f
#define CAR_ACCEL    650.0f
#define CAR_BRAKE   1400.0f
#define CAR_DRAG     0.55f
#define CAR_TURN     2.6f      // rad/s at full steer and speed
#define SPAWN_RADIUS 2200.0f
#define DESPAWN_RADIUS 3400.0f

// People sprite sets with a walk cycle in frames 0..3 (People.DAT); vehicle sets that are ordinary cars.
static const uint16_t WALKERS[] = { 0x2ee6, 0x2eeb, 0x2ef5, 0x2f11, 0x2f1e, 0x2f2a, 0x2f30, 0x2f33, 0x2f27, 0x2eed, 0x2ee3,
                                    0x2f32, 0x2ef1, 0x2efe, 0x2ef0, 0x2ee5, 0x2ee5, 0x2ef0 };
static const uint16_t CARS[] = { 0x2c89, 0x2c8a, 0x2ca1, 0x2ca9, 0x2cb2, 0x2cb9, 0x2cc0, 0x2cc2, 0x2cc4, 0x2cc6, 0x2cc7,
                                 0x2cc8, 0x2cc9, 0x2cca, 0x2ccd, 0x2cce, 0x2ed4, 0x2ed7, 0x2ede, 0x2c8f, 0x2cc5 };
#define NWALKERS (int)(sizeof WALKERS / sizeof *WALKERS)
#define NCARS (int)(sizeof CARS / sizeof *CARS)

// Sets with extra animation frames, all from the game's People.DAT (see MODLOG.md "Fight animations"):
#define SET_FIGHTER 0x2ef0   // fitness guy: jog 0-3, kick 7, punches 8-9
#define SET_COP     0x2efa   // police officer: walk 0-2, baton swing 3-5
#define SET_FALLER  0x2ee5   // man who trips: walk 0-2, falls 3-8 (8 = lying)
#define SET_ARREST  0x2ef9   // officer tackling a suspect: 0-14
typedef struct { uint16_t set; uint8_t nwalk; int8_t punch[3]; int8_t fall[6]; } AnimInfo;
static const AnimInfo ANIMS[] = {
    { SET_FIGHTER, 4, { 8, 9, 7 }, { -1 } },
    { SET_COP, 3, { 3, 4, 5 }, { -1 } },
    { SET_FALLER, 3, { -1 }, { 3, 4, 5, 6, 7, 8 } },
};
static const AnimInfo *anim_info(uint16_t set) {
    for (unsigned i = 0; i < sizeof ANIMS / sizeof *ANIMS; i++) if (ANIMS[i].set == set) return &ANIMS[i];
    return NULL;
}

typedef enum { P_WALK, P_IDLE, P_PUNCH, P_HIT, P_DOWN, P_FLEE, P_ATTACK, P_ARREST, P_DODGE } PState;

typedef struct {
    bool used, player;
    float x, z;            // world
    float hx, hz;          // facing (unit vector, world)
    float vx, vz;          // velocity
    uint16_t set;
    float anim;            // walk cycle phase (frames)
    PState st; float t;    // state + time in state
    int hp;
    bool tough;            // fights back
    bool cop;              // police officer (chases the player when wanted)
    float think;           // AI timer
    float punch_cd;
    int car;               // index of car being driven (player), -1 otherwise
    int tx, tz, dir, side; // sidewalk walking: current road tile, travel dir (0 +x,1 +z,2 -x,3 -z), side of road
    float wx, wz;          // current waypoint
    bool on_walk;          // following sidewalks
    float y, vy, spin;     // height above the ground, vertical speed, tumble phase (thrown by a car)
    bool crowd;            // standing in a crowd ("gta crowd"): may dodge an oncoming car once
} Ped;

typedef struct {
    bool used;
    float x, z;
    float ang;             // world heading: 0 = +z, PI/2 = +x
    float speed;
    uint16_t set;
    bool parked;
    float think, target_speed, steer;
    float hit_cd;
    int tx, tz, dir;       // road following: tile we are heading to and travel direction
    float wx, wz;          // waypoint (lane position at the centre of the target tile)
    bool on_road;
    float stuck;           // seconds traffic has been blocked by a building or tree
} Car;

static Ped g_ped[MAX_PEDS];
static Car g_car[MAX_CARS];
static int g_player = -1;          // ped index when possessed
static int g_select = -1;          // highlighted ped while choosing a citizen
GtaMode g_mode = GTA_OFF;
static WorldInfo g_world;
static Camera g_cam;
static uint32_t g_rng = 12345;
static float g_time;
static int g_score, g_wanted;
static char g_msg[96]; static float g_msg_t;
static bool g_sprites_ok;
static int g_zoom_clicks;
static int g_toggle_vk = 'G';
static uintptr_t g_view_at_start;   // a different view object means another city was loaded
static float g_autodrive;           // "gta autodrive <speed>": the player's car follows the roads by itself (0 = off)
static int g_combo; static float g_combo_t;   // people run over in quick succession
static int g_focus_x = 360, g_focus_y = 270;  // where the follow camera keeps the player ("gta focus")
static bool g_follow = true;        // "gta follow 0": lock the camera (static shots)
static float g_timescale = 1;       // "gta timescale <f>": slow motion for the mod's people and cars
static float g_slowmo_scale, g_slowmo_secs; static uint32_t g_slowmo_until;   // "gta slowmo": armed by the next hit

static float frand(void) { g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) / 16777216.0f; }
static float frange(float a, float b) { return a + (b - a) * frand(); }
static void msg(const char *s) { strncpy(g_msg, s, sizeof g_msg - 1); g_msg_t = 3.0f; }

// ------------------------------------------------------------------ directions
// The sprites are drawn for the screen; with view rotation r the world axes map to screen axes like rotate()
static void view_dir(float dx, float dz, float *ox, float *oz) {
    switch (g_cam.rot) {
    case 0: *ox = dx; *oz = dz; break;
    case 1: *ox = -dz; *oz = dx; break;
    case 2: *ox = -dx; *oz = -dz; break;
    default: *ox = dz; *oz = -dx; break;
    }
}
static void world_dir(float vx, float vz, float *dx, float *dz) {   // inverse of view_dir
    switch (g_cam.rot) {
    case 0: *dx = vx; *dz = vz; break;
    case 1: *dx = vz; *dz = -vx; break;
    case 2: *dx = -vx; *dz = -vz; break;
    default: *dx = -vz; *dz = vx; break;
    }
}
// Sprite heading index, same formula as the game (STRTSIM FUN_1000b5bf: (atan2(dz, dx) + pi) / (pi/4) + 0.5):
// 0 = -x (screen up-right), then counter-clockwise on screen: 1 up, 2 -z, 3 left, 4 +x, 5 down, 6 +z, 7 right.
// People use 8 headings, vehicles 32 (index = 4 * dir8). Computed on the view-rotated vector so rotation is handled.
static float view_angle(float dx, float dz) { float vx, vz; view_dir(dx, dz, &vx, &vz); return atan2f(vz, vx) + PI; }
static int dir8(float dx, float dz) { return (int)floorf(view_angle(dx, dz) / (PI / 4) + 0.5f) & 7; }
static int dir32(float ang) { return (int)floorf(view_angle(sinf(ang), cosf(ang)) / (PI / 16) + 0.5f) & 31; }

// Screen-relative input -> world direction (W = up on screen).
static void screen_to_world_dir(float ux, float uy, float *dx, float *dz) {
    float vz = ux + 2 * uy, vx = 2 * uy - ux;     // from sx ~ (z'-x'), sy ~ (z'+x')/2
    world_dir(vx, vz, dx, dz);
    float l = sqrtf(*dx * *dx + *dz * *dz); if (l > 0) { *dx /= l; *dz /= l; }
}

// ------------------------------------------------------------------ sprites
static const Image *ped_image(const Ped *p, int frame, int d) {
    if (g_cam.zoom < 3) return NULL;
    uint32_t g = ((uint32_t)p->set << 16) | (frame * 16 + (g_cam.zoom == 4 ? 8 : 0) + d);
    const Image *im = spr_get(g);
    if (!im && frame) im = spr_get(((uint32_t)p->set << 16) | ((g_cam.zoom == 4 ? 8 : 0) + d));
    return im;
}
static const Image *car_image(const Car *c) {
    int base = g_cam.zoom == 4 ? 0x60 : g_cam.zoom == 3 ? 0x30 : 0x00;
    if (g_cam.zoom < 2) return NULL;
    return spr_get(((uint32_t)c->set << 16) | (base + dir32(c->ang)));
}

// Quarter-turn variants of a ped sprite (cached per source image and turn): lying down, tumbling through the air
typedef struct { const Image *src; int q; Image img; LONG used; } RotSlot;
#define NROT 192
static RotSlot g_rot[NROT];
static const Image *rotated(const Image *src, int q) {
    q &= 3;
    if (!src || !q) return src;
    LONG now = g_overlay_commits;
    for (int i = 0; i < NROT; i++) if (g_rot[i].src == src && g_rot[i].q == q) { g_rot[i].used = now; return &g_rot[i].img; }
    int best = -1;   // least recently used slot that no recent draw list references
    for (int i = 0; i < NROT; i++) {
        if (!g_rot[i].src) { best = i; break; }
        if (now - g_rot[i].used > 8 && (best < 0 || g_rot[i].used < g_rot[best].used)) best = i;
    }
    if (best < 0) return NULL;
    RotSlot *r = &g_rot[best]; r->used = now;
    free(r->img.px); r->src = src; r->q = q;
    int w = q == 2 ? src->w : src->h, h = q == 2 ? src->h : src->w;
    r->img.w = w; r->img.h = h; r->img.px = calloc(w * h, 4);
    for (int y = 0; y < src->h; y++) for (int x = 0; x < src->w; x++) {
        int dx = q == 1 ? src->h - 1 - y : q == 2 ? src->w - 1 - x : y;
        int dy = q == 1 ? x : q == 2 ? src->h - 1 - y : src->w - 1 - x;
        r->img.px[dy * w + dx] = src->px[y * src->w + x];
    }
    r->img.ax = w / 2; r->img.ay = h - 1;
    return &r->img;
}
static const Image *lying(const Image *src) { return rotated(src, 1); }

// ------------------------------------------------------------------ road network helpers
static const int DX[4] = { 1, 0, -1, 0 }, DZ[4] = { 0, 1, 0, -1 };
#define ROAD_MASK (1u << NET_ROAD)
static bool road(int x, int z) { return net_has(x, z, ROAD_MASK); }
// Buildings, trees and other SIMGEOM occupants block walking and driving; a road on the same tile wins.
static bool solid(float wx, float wz) {
    if (wx < 0 || wz < 0) return true;
    int x = (int)(wx / 256), z = (int)(wz / 256);
    if (x > 255 || z > 255) return true;
    return (g_geo[x][z] & 7) && !g_net[x][z];
}
// Nearest free spot to (x,z) within ~2 tiles (spawns must never start inside a building or a tree)
static bool free_spot(float *x, float *z) {
    if (!solid(*x, *z)) return true;
    for (float r = 64; r <= 512; r += 64)
        for (int k = 0; k < 16; k++) {
            float a = k * (PI / 8), nx = *x + sinf(a) * r, nz = *z + cosf(a) * r;
            if (!solid(nx, nz)) { *x = nx; *z = nz; return true; }
        }
    return false;
}
// lane point: centre of tile + offset to the right of travel direction d (right of heading (fx,fz) is (fz,-fx))
static void lane_point(int x, int z, int d, float off, float *wx, float *wz) {
    *wx = x * 256.0f + 128 + DZ[d] * off;
    *wz = z * 256.0f + 128 - DX[d] * off;
}
// pick the next direction at tile (x,z) arriving with direction d: any road neighbour except straight back
static float g_straight = 0.6f;     // chance to keep going straight at a junction
static int next_dir(int x, int z, int d) {
    int opts[4], n = 0;
    for (int k = 0; k < 4; k++) if (k != ((d + 2) & 3) && road(x + DX[k], z + DZ[k])) opts[n++] = k;
    if (!n) return (d + 2) & 3;                          // dead end: turn around
    for (int i = 0; i < n; i++) if (opts[i] == d && frand() < g_straight) return d;   // prefer straight
    return opts[(int)(frand() * n) % n];
}
// random road tile within [rmin, rmax] tiles of (cx,cz) world, optionally off-screen
#define HWAY_MASK (1u << 2)
static bool pick_road_tile(float cx, float cz, int rmin, int rmax, bool offscreen, int *ox, int *oz) {
    if (!g_net_ok) return false;
    int ctx = (int)(cx / 256), ctz = (int)(cz / 256);
    for (int tries = 0; tries < 60; tries++) {
        int x = ctx + (int)frange(-rmax, rmax + 1), z = ctz + (int)frange(-rmax, rmax + 1);
        int d = abs(x - ctx) > abs(z - ctz) ? abs(x - ctx) : abs(z - ctz);
        if (d < rmin || !road(x, z) || net_has(x, z, HWAY_MASK)) continue;
        if (offscreen) {
            int sx, sy; cam_project(&g_cam, x * 256.0f + 128, g_net_alt[x][z] * 256.0f, z * 256.0f + 128, &sx, &sy);
            if (sx > -60 && sx < 780 && sy > -60 && sy < 600) continue;
        }
        *ox = x; *oz = z; return true;
    }
    return false;
}
static int road_dir_any(int x, int z) {   // a direction along the road at (x,z)
    int opts[4], n = 0;
    for (int k = 0; k < 4; k++) if (road(x + DX[k], z + DZ[k])) opts[n++] = k;
    return n ? opts[(int)(frand() * n) % n] : (int)(frand() * 4);
}

// ------------------------------------------------------------------ spawning
static float ground(float x, float z) { return world_ground(&g_world, x, z); }

static int spawn_ped(float x, float z, uint16_t set) {
    if (!free_spot(&x, &z)) return -1;
    for (int i = 0; i < MAX_PEDS; i++) if (!g_ped[i].used) {
        Ped *p = &g_ped[i]; memset(p, 0, sizeof *p);
        p->used = true; p->x = x; p->z = z; p->set = set; p->hp = 3; p->car = -1;
        float a = frange(0, 2 * PI); p->hx = sinf(a); p->hz = cosf(a);
        p->st = P_WALK; p->think = frange(0.5f, 3); p->tough = set == SET_FIGHTER || frand() < 0.15f;
        return i;
    }
    return -1;
}

static int spawn_car(float x, float z, float ang, bool parked);
static int spawn_road_car(int tx, int tz, bool parked) {
    int d = road_dir_any(tx, tz); float wx, wz;
    lane_point(tx, tz, d, parked ? 92 : 56, &wx, &wz);
    int i = spawn_car(wx, wz, atan2f((float)DX[d], (float)DZ[d]), parked);
    if (i >= 0 && !parked) {
        Car *c = &g_car[i]; c->on_road = true; c->dir = d; c->tx = tx + DX[d]; c->tz = tz + DZ[d];
        if (!road(c->tx, c->tz)) { c->tx = tx; c->tz = tz; }
        lane_point(c->tx, c->tz, c->dir, 56, &c->wx, &c->wz);
    }
    return i;
}
static int spawn_ped(float x, float z, uint16_t set);
static int spawn_walker(int tx, int tz) {
    int d = road_dir_any(tx, tz), side = frand() < 0.5f ? 1 : -1; float wx, wz;
    lane_point(tx, tz, d, side * 108.0f, &wx, &wz);
    int i = spawn_ped(wx + frange(-60, 60) * DX[d], wz + frange(-60, 60) * DZ[d], WALKERS[(int)(frand() * NWALKERS) % NWALKERS]);
    if (i >= 0) {
        Ped *p = &g_ped[i]; p->on_walk = true; p->dir = d; p->side = side; p->tx = tx + DX[d]; p->tz = tz + DZ[d];
        if (!road(p->tx, p->tz)) { p->tx = tx; p->tz = tz; }
        lane_point(p->tx, p->tz, p->dir, side * 108.0f, &p->wx, &p->wz);
    }
    return i;
}

static int spawn_car(float x, float z, float ang, bool parked) {
    if (!free_spot(&x, &z)) return -1;
    for (int i = 0; i < MAX_CARS; i++) if (!g_car[i].used) {
        Car *c = &g_car[i]; memset(c, 0, sizeof *c);
        c->used = true; c->x = x; c->z = z; c->ang = ang; c->parked = parked;
        c->set = CARS[(int)(frand() * NCARS) % NCARS];
        c->target_speed = parked ? 0 : frange(250, 600);
        c->think = frange(1, 4);
        return i;
    }
    return -1;
}

static void focus_point(float *x, float *z) {
    if (g_player >= 0) {
        Ped *p = &g_ped[g_player];
        if (p->car >= 0) { *x = g_car[p->car].x; *z = g_car[p->car].z; } else { *x = p->x; *z = p->z; }
    } else {
        cam_unproject(&g_cam, 360, 270, ground(0, 0), x, z);
    }
}

static void spawn_police(float fx, float fz) {
    int stars = g_wanted / 3 > 5 ? 5 : g_wanted / 3, cops = 0;
    for (int i = 0; i < MAX_PEDS; i++) if (g_ped[i].used && g_ped[i].cop) cops++;
    if (cops >= stars || g_player < 0 || g_ped[g_player].st == P_ARREST) return;
    float a = frange(0, 2 * PI), r = frange(1100, 1700);
    float x = fx + sinf(a) * r, z = fz + cosf(a) * r;
    if (!world_inside(&g_world, x, z)) return;
    int i = spawn_ped(x, z, SET_COP);
    if (i >= 0) { g_ped[i].cop = true; g_ped[i].tough = false; g_ped[i].hp = 4; g_ped[i].st = P_ATTACK; if (stars == 1 && !cops) msg("The police are after you"); }
}

static void populate(void) {
    float fx, fz; focus_point(&fx, &fz);
    spawn_police(fx, fz);
    int peds = 0, cars = 0;
    for (int i = 0; i < MAX_PEDS; i++) if (g_ped[i].used && !g_ped[i].player) {
        float d = hypotf(g_ped[i].x - fx, g_ped[i].z - fz);
        if (d > DESPAWN_RADIUS) g_ped[i].used = false; else if (!g_ped[i].cop) peds++;
    }
    for (int i = 0; i < MAX_CARS; i++) if (g_car[i].used) {
        bool driven = g_player >= 0 && g_ped[g_player].car == i;
        float d = hypotf(g_car[i].x - fx, g_car[i].z - fz);
        if (!driven && d > DESPAWN_RADIUS) g_car[i].used = false; else cars++;
    }
    int tx, tz;
    for (int k = 0; k < 3 && peds < 16; k++) {
        if (pick_road_tile(fx, fz, 3, 9, true, &tx, &tz)) { if (spawn_walker(tx, tz) >= 0) peds++; continue; }
        float a = frange(0, 2 * PI), r = frange(900, SPAWN_RADIUS);   // no road nearby: wander on open ground
        float x = fx + sinf(a) * r, z = fz + cosf(a) * r;
        if (world_inside(&g_world, x, z) && !solid(x, z) && spawn_ped(x, z, WALKERS[(int)(frand() * NWALKERS) % NWALKERS]) >= 0) peds++;
    }
    for (int k = 0; k < 2 && cars < 11; k++) {
        if (pick_road_tile(fx, fz, 4, 10, true, &tx, &tz)) { if (spawn_road_car(tx, tz, frand() < 0.25f) >= 0) cars++; continue; }
        if (cars >= 5) continue;   // no road nearby: a few parked cars on open ground
        float a = frange(0, 2 * PI), r = frange(700, SPAWN_RADIUS);
        float x = fx + sinf(a) * r, z = fz + cosf(a) * r;
        float ang = (int)(frand() * 4) * (PI / 2);
        if (world_inside(&g_world, x, z) && !solid(x, z) && spawn_car(x, z, ang, true) >= 0) cars++;
    }
}

// ------------------------------------------------------------------ gameplay
static int nearest_ped(float x, float z, float maxd, int except) {
    int best = -1; float bd = maxd;
    for (int i = 0; i < MAX_PEDS; i++) if (g_ped[i].used && i != except && g_ped[i].car < 0) {
        float d = hypotf(g_ped[i].x - x, g_ped[i].z - z);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int nearest_car(float x, float z, float maxd) {
    int best = -1; float bd = maxd;
    for (int i = 0; i < MAX_CARS; i++) if (g_car[i].used) {
        float d = hypotf(g_car[i].x - x, g_car[i].z - z);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void hit_ped(Ped *v, float fx, float fz, int dmg, float force, bool by_player) {
    if (v->st == P_DOWN || v->st == P_ARREST) return;
    v->hp -= dmg;
    v->vx = fx * force; v->vz = fz * force;
    if (v->hp <= 0) { v->st = P_DOWN; v->t = 0; if (by_player) { g_score += v->cop ? 30 : 10; g_wanted += v->cop ? 3 : 1; } }
    else { v->st = P_HIT; v->t = 0; }
}

static void do_punch(Ped *p, int self) {
    if (p->punch_cd > 0) return;
    p->st = P_PUNCH; p->t = 0; p->punch_cd = 0.45f;
    for (int i = 0; i < MAX_PEDS; i++) {
        Ped *v = &g_ped[i];
        if (!v->used || i == self || v->car >= 0 || v->st == P_DOWN) continue;
        float dx = v->x - p->x, dz = v->z - p->z, d = hypotf(dx, dz);
        if (d > PUNCH_RANGE || d < 1) continue;
        if ((dx * p->hx + dz * p->hz) / d < 0.3f) continue;
        hit_ped(v, dx / d, dz / d, 1, 260, true);
        if (!v->player) {
            if (v->tough && v->hp > 0) { v->st = P_ATTACK; v->t = 0; }
            for (int k = 0; k < MAX_PEDS; k++) {   // bystanders flee
                Ped *b = &g_ped[k];
                if (b->used && !b->player && k != i && b->st == P_WALK && hypotf(b->x - p->x, b->z - p->z) < 900 && !b->tough) {
                    b->st = P_FLEE; b->t = 0;
                }
            }
        }
        break;
    }
}

static void enter_or_exit(void) {
    Ped *p = &g_ped[g_player];
    if (p->car >= 0) {   // exit: step out to the car's left
        Car *c = &g_car[p->car];
        p->x = c->x - cosf(c->ang) * 120; p->z = c->z + sinf(c->ang) * 120;
        if (solid(p->x, p->z)) { p->x = c->x + cosf(c->ang) * 120; p->z = c->z - sinf(c->ang) * 120; }
        if (solid(p->x, p->z)) { p->x = c->x; p->z = c->z; }
        c->speed *= 0.3f; c->parked = true; c->target_speed = 0;
        p->car = -1; p->st = P_IDLE; msg("Out of the car");
        return;
    }
    int ci = nearest_car(p->x, p->z, ENTER_RANGE);
    if (ci < 0) { msg("No car nearby"); return; }
    Car *c = &g_car[ci];
    if (!c->parked && c->speed > 50) g_wanted++;
    if (!c->parked) {   // the driver gets thrown out
        int d = spawn_ped(c->x + cosf(c->ang) * 100, c->z - sinf(c->ang) * 100, WALKERS[(int)(frand() * NWALKERS) % NWALKERS]);
        if (d >= 0) { g_ped[d].st = P_FLEE; g_ped[d].tough = false; }
    }
    c->parked = false; c->target_speed = 0; c->on_road = false;
    p->car = ci; p->x = c->x; p->z = c->z;
    g_score += 5; msg("Car stolen!");
}

static void update_player(Ped *p, float dt) {
    float ux = 0, uy = 0;
    if (input_down('W') || input_down(VK_UP)) uy -= 1;
    if (input_down('S') || input_down(VK_DOWN)) uy += 1;
    if (input_down('A') || input_down(VK_LEFT)) ux -= 1;
    if (input_down('D') || input_down(VK_RIGHT)) ux += 1;
    bool run = input_down(VK_SHIFT);
    if (input_pressed(VK_RETURN) && p->st != P_DOWN) { enter_or_exit(); if (p->car >= 0) return; }
    if (p->car >= 0 && g_autodrive > 0) { p->x = g_car[p->car].x; p->z = g_car[p->car].z; return; }
    if (p->car >= 0) {
        Car *c = &g_car[p->car];
        float throttle = -uy, steer = ux;
        if (throttle > 0) c->speed += (c->speed < 0 ? CAR_BRAKE : CAR_ACCEL) * dt;
        else if (throttle < 0) c->speed -= (c->speed > 0 ? CAR_BRAKE : CAR_ACCEL * 0.6f) * dt;
        else c->speed -= c->speed * CAR_DRAG * dt;
        if (input_down(VK_SPACE)) c->speed -= c->speed * 3.0f * dt;     // handbrake
        if (c->speed > CAR_MAX) c->speed = CAR_MAX;
        if (c->speed < -CAR_REV) c->speed = -CAR_REV;
        float grip = fminf(1.0f, fabsf(c->speed) / 300.0f) * (c->speed >= 0 ? 1 : -1);
        c->ang += steer * CAR_TURN * grip * dt;
        p->x = c->x; p->z = c->z;
        return;
    }
    if (input_pressed(VK_SPACE) && p->st != P_DOWN) do_punch(p, g_player);
    if (p->st == P_PUNCH || p->st == P_HIT || p->st == P_DOWN) return;
    if (ux || uy) {
        float dx, dz; screen_to_world_dir(ux, uy, &dx, &dz);
        float sp = run ? RUN_SPEED : WALK_SPEED;
        p->vx = dx * sp; p->vz = dz * sp; p->hx = dx; p->hz = dz; p->st = P_WALK;
        p->anim += dt * (run ? 12.0f : 7.0f);
    } else { p->vx = p->vz = 0; p->st = P_IDLE; }
}

static void update_npc(Ped *p, float dt) {
    Ped *pl = g_player >= 0 ? &g_ped[g_player] : NULL;
    p->think -= dt;
    switch (p->st) {
    case P_WALK:
        if (p->on_walk) {
            float dx = p->wx - p->x, dz = p->wz - p->z, d = hypotf(dx, dz);
            if (d < 30) {   // reached the waypoint: continue along the sidewalk
                int nd = next_dir(p->tx, p->tz, p->dir);
                p->dir = nd; p->tx += DX[nd]; p->tz += DZ[nd];
                if (!road(p->tx, p->tz)) { p->tx -= DX[nd]; p->tz -= DZ[nd]; p->dir = (nd + 2) & 3; p->side = -p->side; }
                lane_point(p->tx, p->tz, p->dir, p->side * 108.0f, &p->wx, &p->wz);
                if (frand() < 0.05f) { p->st = P_IDLE; p->t = 0; p->think = frange(1, 3); }
                break;
            }
            p->hx = dx / d; p->hz = dz / d;
        } else if (p->think <= 0) {
            p->think = frange(1.5f, 5);
            if (frand() < 0.25f) { p->st = P_IDLE; p->t = 0; break; }
            int d = (int)(frand() * 8); float a = d * PI / 4; p->hx = sinf(a); p->hz = cosf(a);
        }
        p->vx = p->hx * NPC_SPEED; p->vz = p->hz * NPC_SPEED; p->anim += dt * 6;
        break;
    case P_IDLE:
        p->vx = p->vz = 0;
        if (p->think <= 0) { p->st = P_WALK; p->think = frange(1, 4); }
        break;
    case P_DODGE:   // jump out of the way of a car, in a fixed direction
        p->vx = p->hx * FLEE_SPEED * 1.2f; p->vz = p->hz * FLEE_SPEED * 1.2f; p->anim += dt * 14;
        if (p->t > 1.2f) { p->st = P_FLEE; p->t = 0; }
        break;
    case P_FLEE:
        if (pl) {
            float dx = p->x - pl->x, dz = p->z - pl->z, d = hypotf(dx, dz) + 1;
            p->hx = dx / d; p->hz = dz / d;
            if (d > 1600) { p->st = P_WALK; p->think = 2; p->on_walk = false; }
        }
        p->vx = p->hx * FLEE_SPEED; p->vz = p->hz * FLEE_SPEED; p->anim += dt * 12;
        break;
    case P_ATTACK:
        if (p->cop && g_wanted < 3) { p->st = P_WALK; p->on_walk = false; p->think = 3; break; }   // nothing to chase
        if (!pl || pl->car >= 0 || pl->st == P_ARREST) { p->vx = p->vz = 0; if (!p->cop) p->st = P_WALK; break; }
        {
            float dx = pl->x - p->x, dz = pl->z - p->z, d = hypotf(dx, dz) + 1;
            p->hx = dx / d; p->hz = dz / d;
            if (d > 2000) { p->st = P_WALK; break; }
            if (p->cop && pl->st == P_DOWN && d < PUNCH_RANGE && pl->car < 0) {   // BUSTED
                pl->st = P_ARREST; pl->t = 0; p->used = false; msg("BUSTED!"); break;
            }
            if (d > PUNCH_RANGE * 0.8f) { float sp = p->cop ? 300 : 230; p->vx = p->hx * sp; p->vz = p->hz * sp; p->anim += dt * 10; }
            else {
                p->vx = p->vz = 0;
                if (p->punch_cd <= 0) {
                    p->st = P_PUNCH; p->t = 0; p->punch_cd = p->cop ? 0.7f : 0.9f;
                    if (pl->st != P_DOWN) { hit_ped(pl, p->hx, p->hz, 1, 200, false); msg(p->cop ? "The police hit you!" : "You got punched!"); }
                }
            }
        }
        break;
    default: break;
    }
}

static void move_ped(Ped *p, float dt);
static void step_peds(float dt) {
    for (int i = 0; i < MAX_PEDS; i++) {
        Ped *p = &g_ped[i]; if (!p->used) continue;
        p->t += dt; if (p->punch_cd > 0) p->punch_cd -= dt;
        if (p->player) { if (p->st != P_ARREST) update_player(p, dt); }
        else if (p->st != P_PUNCH && p->st != P_HIT && p->st != P_DOWN) update_npc(p, dt);
        switch (p->st) {
        case P_PUNCH: p->vx = p->vz = 0; if (p->t > 0.3f) p->st = p->player ? P_IDLE : ((p->tough || p->cop) ? P_ATTACK : P_WALK); break;
        case P_HIT: p->vx *= 0.85f; p->vz *= 0.85f; if (p->t > 0.45f) p->st = p->player ? P_IDLE : ((p->tough || p->cop) ? P_ATTACK : P_FLEE); break;
        case P_ARREST:
            p->vx = p->vz = 0;
            if (p->t > 3.2f) { p->st = P_IDLE; p->hp = 5; g_wanted = 0; g_score = g_score > 20 ? g_score - 20 : 0; msg("Busted. Wanted level cleared, $200 fine"); }
            break;
        case P_DOWN:
            if (p->y <= 0) { p->vx *= 0.8f; p->vz *= 0.8f; }
            if (p->t > (p->player ? 2.5f : 6.0f)) {
                if (p->player) { p->hp = 5; p->st = P_IDLE; msg("Back on your feet"); }
                else { p->hp = 2; p->st = P_FLEE; p->t = 0; p->tough = false; }
            }
            break;
        default: break;
        }
        if (p->car >= 0) continue;
        move_ped(p, dt);
    }
}

// Move a ped by its velocity: flight when thrown, never into buildings or trees (the player slides along them).
static void move_ped(Ped *p, float dt) {
    if (p->y > 0 || p->vy > 0) {   // thrown by a car: ballistic flight, one small bounce
        p->vy -= 3200 * dt; p->y += p->vy * dt; p->spin += dt * 9;
        if (p->y <= 0) {
            p->y = 0;
            if (p->vy < -700) { p->vy = -p->vy * 0.25f; p->y = 0.1f; p->vx *= 0.5f; p->vz *= 0.5f; }
            else { p->vy = 0; p->spin = 0; p->t = 0; }
        }
    }
    float nx = p->x + p->vx * dt, nz = p->z + p->vz * dt;
    bool blocked = !world_inside(&g_world, nx, nz) || (solid(nx, nz) && !solid(p->x, p->z));
    if (!blocked) { p->x = nx; p->z = nz; }
    else if (p->player) {   // slide along walls
        if (world_inside(&g_world, nx, p->z) && !solid(nx, p->z)) p->x = nx;
        else if (world_inside(&g_world, p->x, nz) && !solid(p->x, nz)) p->z = nz;
    } else if (p->y > 0) { p->vx *= -0.3f; p->vz *= -0.3f; }
    else { p->hx = -p->hx; p->hz = -p->hz; p->on_walk = false; }
}

static void turn_toward(float *ang, float target, float max_step) {
    float diff = remainderf(target - *ang, 2 * PI);
    *ang += diff > max_step ? max_step : diff < -max_step ? -max_step : diff;
}

// Put a car on the road network: nearest ordinary road tile, the road direction closest to its heading.
static bool road_lock(Car *c, bool snap) {
    int cx = (int)(c->x / 256), cz = (int)(c->z / 256), bx = -1, bz = -1; float bd = 1e9f;
    for (int x = cx - 4; x <= cx + 4; x++) for (int z = cz - 4; z <= cz + 4; z++) {
        if (!road(x, z) || net_has(x, z, HWAY_MASK)) continue;
        float d = hypotf(x * 256.0f + 128 - c->x, z * 256.0f + 128 - c->z);
        if (d < bd) { bd = d; bx = x; bz = z; }
    }
    if (bx < 0) return false;
    int best = -1; float bdot = -2;
    for (int k = 0; k < 4; k++) if (road(bx + DX[k], bz + DZ[k])) {
        float dot = DX[k] * sinf(c->ang) + DZ[k] * cosf(c->ang);
        if (dot > bdot) { bdot = dot; best = k; }
    }
    if (best < 0) best = road_dir_any(bx, bz);
    c->on_road = true; c->dir = best; c->tx = bx; c->tz = bz;
    if (snap) {   // jump onto the lane, pointing along the road
        lane_point(bx, bz, best, 56, &c->x, &c->z); c->ang = atan2f((float)DX[best], (float)DZ[best]);
        bd = 0;
    }
    if (bd < 200 && road(bx + DX[best], bz + DZ[best])) { c->tx += DX[best]; c->tz += DZ[best]; }
    lane_point(c->tx, c->tz, c->dir, 56, &c->wx, &c->wz);
    return true;
}

static void step_cars(float dt) {
    for (int i = 0; i < MAX_CARS; i++) {
        Car *c = &g_car[i]; if (!c->used) continue;
        bool driven = g_player >= 0 && g_ped[g_player].car == i;
        if (c->hit_cd > 0) c->hit_cd -= dt;
        bool autod = driven && g_autodrive > 0;
        if (autod && !c->on_road) road_lock(c, false);
        if (!driven || autod) {
            if (c->parked && !autod) c->speed -= c->speed * 2.5f * dt;
            else if (c->on_road) {
                float dx = c->wx - c->x, dz = c->wz - c->z, d = hypotf(dx, dz);
                if (d < 48 + (autod ? fabsf(c->speed) * 0.08f : 0)) {   // at the lane point of the target tile: pick where to go next
                    if (autod) g_straight = 0.97f;   // the player's autopilot sticks to long straight runs
                    int nd = next_dir(c->tx, c->tz, c->dir);
                    g_straight = 0.6f;
                    c->dir = nd; c->tx += DX[nd]; c->tz += DZ[nd];
                    lane_point(c->tx, c->tz, c->dir, 56, &c->wx, &c->wz);
                    dx = c->wx - c->x; dz = c->wz - c->z; d = hypotf(dx, dz);
                }
                float want = atan2f(dx, dz), diff = remainderf(want - c->ang, 2 * PI);
                float turn = (autod ? 4.8f : 3.2f) * dt; c->ang += diff > turn ? turn : diff < -turn ? -turn : diff;
                float cruise = (autod ? g_autodrive : c->target_speed) * (1.0f - 0.6f * fminf(1.0f, fabsf(diff)));
                for (int k = 0; k < MAX_CARS && !autod; k++) {   // brake for cars ahead
                    Car *o = &g_car[k]; if (k == i || !o->used) continue;
                    float ox = o->x - c->x, oz = o->z - c->z, od = hypotf(ox, oz);
                    if (od < 260 && od > 1 && (ox * sinf(c->ang) + oz * cosf(c->ang)) / od > 0.85f) cruise = 0;
                }
                if (!autod && g_player >= 0 && g_ped[g_player].car < 0) {   // and for the player standing in the road
                    Ped *pl = &g_ped[g_player]; float ox = pl->x - c->x, oz = pl->z - c->z, od = hypotf(ox, oz);
                    if (od < 220 && od > 1 && (ox * sinf(c->ang) + oz * cosf(c->ang)) / od > 0.8f) cruise = 0;
                }
                c->speed += (cruise - c->speed) * (cruise < c->speed ? 4.0f : 1.2f) * dt;
            } else {
                c->think -= dt;
                if (c->think <= 0) { c->think = frange(2, 6); c->steer = frand() < 0.4f ? (frand() < 0.5f ? -1.0f : 1.0f) : 0; }
                c->speed += (c->target_speed - c->speed) * 1.5f * dt;
                c->ang += c->steer * 0.8f * dt;
            }
        }
        float nx = c->x + sinf(c->ang) * c->speed * dt, nz = c->z + cosf(c->ang) * c->speed * dt;
        bool hit_wall = solid(nx, nz) && !solid(c->x, c->z);
        if (hit_wall && !driven) {   // traffic never drives into buildings or trees: stop and aim at its lane again
            if (c->on_road) c->ang = atan2f(c->wx - c->x, c->wz - c->z); else c->ang += PI;
            c->speed *= 0.5f; c->stuck += dt;
            if (c->stuck > 1.5f) { c->used = false; continue; }   // wedged: populate() spawns a replacement
        }
        else if (world_inside(&g_world, nx, nz) && !hit_wall) { c->x = nx; c->z = nz; c->stuck = 0; }
        else if (hit_wall && !solid(nx, c->z) && world_inside(&g_world, nx, c->z)) {   // scrape along the wall:
            c->x = nx; c->speed *= 0.92f;                                                   // lose speed and swing the
            turn_toward(&c->ang, sinf(c->ang) >= 0 ? PI / 2 : -PI / 2, 3.0f * dt);   // nose along it
        }
        else if (hit_wall && !solid(c->x, nz) && world_inside(&g_world, c->x, nz)) {
            c->z = nz; c->speed *= 0.92f;
            turn_toward(&c->ang, cosf(c->ang) >= 0 ? 0 : PI, 3.0f * dt);
        }
        else { if (hit_wall && fabsf(c->speed) > 400) msg("Crash!"); c->speed = -c->speed * 0.3f; if (!driven) c->ang += PI; }
        if (driven && fabsf(c->speed) > 200) for (int k = 0; k < MAX_PEDS; k++) {   // the player runs people over
            Ped *p = &g_ped[k];
            if (!p->used || p->car >= 0 || p->st == P_DOWN) continue;
            float ox = p->x - c->x, oz = p->z - c->z, od = hypotf(ox, oz), fx = sinf(c->ang), fz = cosf(c->ang);
            if (p->crowd && od < 650 && (ox * fx + oz * fz) > 0.3f * od) {   // a crowd member sees the car coming
                p->crowd = false;
                if (frand() < 0.45f) {   // dive to the side away from the car's path
                    float side = (ox * fz - oz * fx) >= 0 ? 1.0f : -1.0f;
                    p->hx = fz * side; p->hz = -fx * side; p->st = P_DODGE; p->t = 0;
                }
            }
            if (od < 90) {   // thrown up and away, tumbling
                float sp = fabsf(c->speed), side = (frand() < 0.5f ? -1.0f : 1.0f) * frange(0.2f, 0.8f);
                hit_ped(p, fx, fz, 5, 0, true);
                p->vx = (fx + fz * side) * sp * 0.5f; p->vz = (fz - fx * side) * sp * 0.5f;
                p->vy = frange(1600, 2400) * fminf(1.3f, sp / 1000); p->y = 0.1f; p->spin = frange(0, 4);
                c->speed *= 0.97f;
                if (g_slowmo_scale > 0 && !g_slowmo_until) { g_timescale = g_slowmo_scale; g_slowmo_until = GetTickCount() + (uint32_t)(g_slowmo_secs * 1000); }
                g_combo = g_time - g_combo_t < 2.5f ? g_combo + 1 : 1; g_combo_t = g_time;
                if (g_combo >= 2) { char b[32]; snprintf(b, sizeof b, "COMBO x%d!", g_combo); msg(b); }
            }
        }
        for (int k = i + 1; k < MAX_CARS; k++) {   // car-car bumps
            Car *o = &g_car[k]; if (!o->used) continue;
            float dx = o->x - c->x, dz = o->z - c->z, d = hypotf(dx, dz);
            if (d < 150 && d > 1) {
                float push = (150 - d) / 2; dx /= d; dz /= d;
                bool o_driven = g_player >= 0 && g_ped[g_player].car == k;
                float pc = driven ? 0 : o_driven ? 2 : 1, po = driven ? 2 : o_driven ? 0 : 1;   // the player rams
                if (pc && !solid(c->x - dx * push * pc, c->z - dz * push * pc)) { c->x -= dx * push * pc; c->z -= dz * push * pc; }
                if (po && !solid(o->x + dx * push * po, o->z + dz * push * po)) { o->x += dx * push * po; o->z += dz * push * po; }
                c->speed *= driven ? 0.97f : 0.5f; o->speed *= o_driven ? 0.97f : 0.5f;
            }
        }
    }
}

// ------------------------------------------------------------------ camera
static void follow_camera(void) {
    float fx, fz; focus_point(&fx, &fz);
    int sx, sy; cam_project(&g_cam, fx, ground(fx, fz), fz, &sx, &sy);
    int dx = sx - g_focus_x, dy = sy - g_focus_y;
    if (abs(dx) > 2 || abs(dy) > 2) {   // ease in, but never let the focus leave the screen
        int mx = abs(dx) > 180 ? dx : dx / 3, my = abs(dy) > 140 ? dy : dy / 3;
        if (mx || my) cam_scroll_by(mx, my);
    }
}

// ------------------------------------------------------------------ drawing
static void add_img(const Image *im, int x, int y, int depth) {
    if (!im) return;
    DrawItem d = { OV_IMAGE }; d.img = im; d.x = x; d.y = y; d.depth = depth; overlay_add(&d);
}
static void add_text(const char *s, int x, int y, uint32_t col, int size, int depth) {
    add_img(text_image(s, col, size), x, y, depth);
}

bool g_debug_arrows;   // "gta arrows": draw each car's heading as a line (sprite/heading check)
// Native sprites: one per entity slot (the game's view keeps pointers to them). "gta native 0" draws everything in
// the overlay instead, on top of the city.
static NatImg g_nped[MAX_PEDS], g_ncar[MAX_CARS];
static bool g_native = true;
static void native_hide_all(void) {
    for (int i = 0; i < MAX_PEDS; i++) nat_hide(&g_nped[i]);
    for (int i = 0; i < MAX_CARS; i++) nat_hide(&g_ncar[i]);
}
// Frame keys per zoom level in the game's sprite resources (= DAT instance numbers), -1 where the set has none.
static void ped_keys(uint16_t set, int frame, int d, int key[5]) {
    for (int z = 0; z < 5; z++) key[z] = -1;
    for (int z = 3; z <= 4; z++) {
        int k = frame * 16 + (z == 4 ? 8 : 0) + d;
        if (!spr_has((uint32_t)set << 16 | k)) k = (z == 4 ? 8 : 0) + d;   // frame missing: standing pose
        if (spr_has((uint32_t)set << 16 | k)) key[z] = k;
    }
}
static void car_keys(uint16_t set, int h32, int key[5]) {
    static const int base[5] = { -1, -1, 0x00, 0x30, 0x60 };
    for (int z = 0; z < 5; z++) key[z] = base[z] >= 0 && spr_has((uint32_t)set << 16 | (base[z] + h32)) ? base[z] + h32 : -1;
}

// X-ray silhouette of the player (and their car) wherever a building or tree in front hides them
#define XRAY_COLOR 0x30ff30
static void add_ghost(const Image *im, int x, int y) {
    if (!im) return;
    DrawItem d = { OV_GHOST }; d.img = im; d.x = x; d.y = y; d.color = XRAY_COLOR; d.depth = (1 << 28) - 1; overlay_add(&d);
}

static void draw_world(void) {
    bool nat = g_native && g_nat_ok, zok = g_cam.zoom >= 0 && g_cam.zoom <= 4;
    for (int i = 0; i < MAX_CARS; i++) {
        Car *c = &g_car[i];
        if (!c->used) { nat_hide(&g_ncar[i]); continue; }
        int sx, sy; cam_project(&g_cam, c->x, ground(c->x, c->z), c->z, &sx, &sy);
        bool on_screen = sx >= -100 && sy >= -100 && sx <= 900 && sy <= 700;
        int key[5]; car_keys(c->set, dir32(c->ang), key);
        if (nat && zok && key[g_cam.zoom] >= 0) {
            nat_show(&g_ncar[i], c->set, key, c->x, c->z, ground(c->x, c->z));
            if (g_player >= 0 && g_ped[g_player].car == i && on_screen) add_ghost(car_image(c), sx, sy);
        }
        else { nat_hide(&g_ncar[i]); if (on_screen) add_img(car_image(c), sx, sy, sy * 4 + 1); }
        if (g_debug_arrows && on_screen) {
            DrawItem l = { OV_LINE }; l.x = sx; l.y = sy; l.color = 0xffff00ff; l.depth = 1 << 28;
            cam_project(&g_cam, c->x + sinf(c->ang) * 200, ground(c->x, c->z), c->z + cosf(c->ang) * 200, &l.x2, &l.y2);
            overlay_add(&l);
        }
    }
    for (int i = 0; i < MAX_PEDS; i++) {
        Ped *p = &g_ped[i];
        if (!p->used || p->car >= 0) { nat_hide(&g_nped[i]); continue; }
        int sx, sy; cam_project(&g_cam, p->x, ground(p->x, p->z), p->z, &sx, &sy);
        bool on_screen = !(sx < -50 || sy < -50 || sx > 850 || sy > 650);
        int d = dir8(p->hx, p->hz), frame = 0;
        const AnimInfo *ai = anim_info(p->set);
        int nwalk = ai ? ai->nwalk : 4;
        if (p->st == P_WALK || p->st == P_FLEE || p->st == P_ATTACK || p->st == P_DODGE) frame = ((int)p->anim) % nwalk;
        if (p->y > 0) {   // flying after a car hit: tumbling sprite in the air (overlay), shadow on the ground
            nat_hide(&g_nped[i]);
            if (!on_screen) continue;
            int ax, ay; cam_project(&g_cam, p->x, ground(p->x, p->z) + p->y, p->z, &ax, &ay);
            const Image *r = rotated(ped_image(p, 0, d), (int)p->spin);
            if (r) add_img(r, ax, ay - 10 + r->h / 2, sy * 4 + 3);
            DrawItem sh = { OV_RECT }; sh.x = sx - 4; sh.y = sy - 1; sh.x2 = 9; sh.y2 = 3; sh.color = 0x60000000; sh.depth = sy * 4; overlay_add(&sh);
            continue;
        }
        Ped look = *p;   // the sprite set and frame actually shown
        bool real_punch = p->st == P_PUNCH && ai && ai->punch[0] >= 0, rotate = false;
        if (p->st == P_ARREST) {   // officer tackling the suspect, from the game's own arrest animation
            look.set = SET_ARREST; frame = (int)(p->t / 0.17f) > 14 ? 14 : (int)(p->t / 0.17f);
        } else if (real_punch) { int k = (int)(p->t / 0.1f); if (k > 2 || ai->punch[k] < 0) k = 0; frame = ai->punch[k]; }
        else if (p->st == P_DOWN) {
            if (ai && ai->fall[0] >= 0) { int k = (int)(p->t / 0.09f); if (k > 5) k = 5; frame = ai->fall[k]; }
            else { frame = 0; rotate = true; }   // no fall animation: standing sprite turned on its side (overlay)
        }
        const Image *im = ped_image(&look, frame, d);
        // the lunge of a punch without punch frames, and the shake of a hit
        float wx = p->x, wz = p->z; int ox = 0, oy = 0;
        if (p->st == P_PUNCH && !real_punch) {
            float k = sinf(fminf(p->t / 0.3f, 1) * PI), vx, vz; view_dir(p->hx, p->hz, &vx, &vz);
            ox = (int)((vz - vx) * 5 * k); oy = (int)((vz + vx) * 2.5f * k); wx += p->hx * 40 * k; wz += p->hz * 40 * k;
        }
        if (p->st == P_HIT) { int s = ((int)(p->t * 40) & 1) ? 1 : -1; ox = s; wx += p->hz * 10 * s; wz -= p->hx * 10 * s; }
        int key[5]; ped_keys(look.set, frame, d, key);
        if (nat && zok && !rotate && key[g_cam.zoom] >= 0) {
            nat_show(&g_nped[i], look.set, key, wx, wz, ground(p->x, p->z));
            if (i == g_player && on_screen) add_ghost(im, sx + ox, sy + oy);
        }
        else { nat_hide(&g_nped[i]); if (on_screen) add_img(rotate ? lying(im) : im, sx + ox, sy + oy, sy * 4 + 2); }
        if (!on_screen) continue;
        if (p->st == P_PUNCH && p->t < 0.15f) {   // impact flash in front of the fist
            float vx, vz; view_dir(p->hx, p->hz, &vx, &vz);
            int fx = sx + (int)((vz - vx) * 14), fy = sy - 14 + (int)((vz + vx) * 7);
            DrawItem s = { OV_RECT }; s.x = fx - 2; s.y = fy - 2; s.x2 = 5; s.y2 = 5; s.color = 0xffffff60; s.depth = 1 << 28; overlay_add(&s);
        }
        if (i == g_player || i == g_select) {   // marker above the head
            uint32_t col = i == g_player ? 0xff30ff30 : ((int)(g_time * 4) & 1 ? 0xffffff00 : 0xffff8000);
            int top = sy - (im ? im->ay : 26) - 6;
            for (int k = 0; k < 5; k++) { DrawItem l = { OV_LINE }; l.x = sx - 4 + k; l.y = top - 6 + k; l.x2 = sx + 4 - k; l.y2 = top - 6 + k; l.color = col; l.depth = 1 << 28; overlay_add(&l); }
        }
    }
}

static void draw_hud(void) {
    const int Z = 1 << 29;
    if (g_mode == GTA_CHOOSE) {
        add_text("GTA MODE: pick a citizen", 12, 10, 0xffffff40, 16, Z);
        add_text("TAB next citizen   ENTER take control   G quit", 12, 30, 0xffffffff, 12, Z);
    } else if (g_mode == GTA_PLAY) {
        Ped *p = &g_ped[g_player];
        char b[96];
        snprintf(b, sizeof b, "$%d", g_score * 10); add_text(b, 12, 8, 0xff60ff60, 18, Z);
        snprintf(b, sizeof b, "HP %d", p->hp > 0 ? p->hp : 0); add_text(b, 12, 30, 0xffff6060, 12, Z);
        int stars = g_wanted / 3 > 5 ? 5 : g_wanted / 3;
        for (int s = 0; s < 5; s++) add_text(s < stars ? "*" : ".", 640 - 60 + s * 12, 8, s < stars ? 0xffffd000 : 0xff808080, 18, Z);
        add_text(p->car >= 0 ? "WASD drive  SPACE handbrake  ENTER get out  G quit"
                             : "WASD walk  SHIFT run  SPACE punch  ENTER steal car  G quit", 12, 520, 0xffffffff, 11, Z);
        if (p->car < 0) {
            int ci = nearest_car(p->x, p->z, ENTER_RANGE);
            if (ci >= 0) {
                int sx, sy; cam_project(&g_cam, g_car[ci].x, ground(g_car[ci].x, g_car[ci].z), g_car[ci].z, &sx, &sy);
                add_text("ENTER", sx - 14, sy - 40, 0xffffff40, 11, Z);
            }
        }
    }
    if (g_msg_t > 0) add_text(g_msg, 260, 60, 0xffffffff, 16, Z);
}

// ------------------------------------------------------------------ mode control
static bool capture_key(int vk) {
    if (vk == g_toggle_vk) return true;
    if (g_mode == GTA_OFF) return false;
    switch (vk) {
    case 'W': case 'A': case 'S': case 'D': case VK_SPACE: case VK_RETURN: case VK_SHIFT: case VK_TAB:
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_LSHIFT: case VK_RSHIFT:
        return true;
    }
    return false;
}

void gta_start(void) {
    if (!g_sprites_ok) {
        g_sprites_ok = spr_open("00000007_People.DAT") >= 0;
        g_sprites_ok = (spr_open("00000006_Vehicles.DAT") >= 0) && g_sprites_ok;
    }
    if (!g_sprites_ok || !world_read(&g_world) || !cam_read(&g_cam)) { LOG("gta_start: not ready"); return; }
    native_hide_all();
    memset(g_ped, 0, sizeof g_ped); memset(g_car, 0, sizeof g_car);
    g_player = -1; g_score = 0; g_wanted = 0; g_rng ^= GetTickCount();
    g_mode = GTA_CHOOSE; g_zoom_clicks = 0; g_view_at_start = g_cam.view;
    g_autodrive = 0; g_follow = true; g_focus_x = 360; g_focus_y = 270;   // filming knobs back to normal play
    g_timescale = 1; g_slowmo_scale = 0; g_slowmo_until = 0;
    float cx, cz; cam_unproject(&g_cam, 360, 270, ground(0, 0), &cx, &cz);   // citizens around the screen centre
    if (!world_inside(&g_world, cx, cz)) { cx = g_world.mapw * 128.0f; cz = g_world.maph * 128.0f; }
    net_scan(); geo_scan();
    int tx, tz, placed = 0;
    for (int k = 0; k < 7; k++) if (pick_road_tile(cx, cz, 0, 3, false, &tx, &tz) && spawn_walker(tx, tz) >= 0) placed++;
    for (int k = 0; k < 3; k++) if (pick_road_tile(cx, cz, 0, 3, false, &tx, &tz)) spawn_road_car(tx, tz, true);
    for (int k = 0; k < 3; k++) if (pick_road_tile(cx, cz, 1, 4, false, &tx, &tz)) spawn_road_car(tx, tz, false);
    for (int k = 0, tries = 0; k < 2 && tries < 40; tries++) {   // always a couple of parked cars close by
        float x = cx + frange(-600, 600), z = cz + frange(-600, 600);
        if (world_inside(&g_world, x, z) && !solid(x, z) && spawn_car(x, z, (int)(frand() * 4) * (PI / 2), true) >= 0) k++;
    }
    for (; placed < 6; placed++) spawn_ped(cx + frange(-500, 500), cz + frange(-400, 400), WALKERS[(int)(frand() * NWALKERS) % NWALKERS]);
    g_select = nearest_ped(cx, cz, 1e9f, -1);
    input_clear_pressed();
    msg("GTA mode");
    LOG("gta_start at %.0f,%.0f map %dx%d", cx, cz, g_world.mapw, g_world.maph);
}

void gta_stop(void) {
    native_hide_all();
    g_mode = GTA_OFF; g_player = -1; g_select = -1;
    memset(g_ped, 0, sizeof g_ped); memset(g_car, 0, sizeof g_car);
    LOG("gta_stop");
}

static void possess(int i) {
    if (i < 0) return;
    g_player = i; g_select = -1; g_ped[i].player = true; g_ped[i].hp = 5; g_ped[i].st = P_IDLE; g_ped[i].tough = false;
    g_mode = GTA_PLAY; msg("You are now this citizen");
}

// The mod uses fixed addresses inside these game DLLs (the April 2000 builds that ship with the Steam release).
// On any other build it switches itself off instead of crashing. Returns -1 while they are still loading.
static int game_build_ok(void) {
    static const struct { const char *mod; uint32_t stamp; } want[] = {
        { "SIMSPR.DLL", 0x38fd55e5 }, { "SIMNTWRK.DLL", 0x38fd555c }, { "SIMGEOM.DLL", 0x38fd54b7 } };
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) {
        HMODULE h = GetModuleHandleA(want[i].mod);
        if (!h) return -1;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)h + ((IMAGE_DOS_HEADER *)h)->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != want[i].stamp) {
            LOG("unsupported game build: %s is %08lx, GTA mode needs %08lx. GTA mode is disabled.", want[i].mod,
                (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)want[i].stamp);
            return 0;
        }
    }
    return 1;
}

void gta_tick(uint32_t now_ms) {
    static uint32_t last; static bool installed, overlay_ok; static int build = -1;
    if (build < 0) { build = game_build_ok(); if (build < 0) return; }
    if (!build) return;
    if (!installed && input_hwnd()) {
        // Input first: G must work even without cnc-ddraw (people and cars are drawn by the game itself; only the
        // HUD and markers need cnc-ddraw's OpenGL renderer).
        installed = input_install();
        g_capture_key = capture_key;
        overlay_ok = overlay_install();
        if (!overlay_ok) LOG("overlay: cnc-ddraw with renderer=opengl not found; HUD and markers are off");
        nat_init();
    }
    if (last && now_ms - last < 15) return;   // ~60 Hz is plenty; the framework ticks much faster
    float dt = last ? (now_ms - last) / 1000.0f : 0; last = now_ms;
    if (dt > 0.1f) dt = 0.1f;
    if (g_slowmo_until && (int32_t)(now_ms - g_slowmo_until) >= 0) { g_timescale = 1; g_slowmo_until = 0; g_slowmo_scale = 0; }
    dt *= g_timescale;
    if (input_pressed(g_toggle_vk)) { if (g_mode == GTA_OFF) gta_start(); else gta_stop(); }

    overlay_begin();
    bool have_cam = cam_read(&g_cam);
    if (g_mode != GTA_OFF && (!have_cam || g_cam.view != g_view_at_start)) gta_stop();   // city closed or reloaded
    if (have_cam && g_mode != GTA_OFF) {
        world_read(&g_world);
        g_time += dt; if (g_msg_t > 0) g_msg_t -= dt;
        if (g_cam.zoom < 4 && g_zoom_clicks < 8) { input_click(667, 545); g_zoom_clicks++; }
        if (g_mode == GTA_CHOOSE) {
            if (input_pressed(VK_TAB)) {   // cycle to the next citizen
                for (int k = 1; k <= MAX_PEDS; k++) { int j = (g_select + k) % MAX_PEDS; if (g_ped[j].used) { g_select = j; break; } }
            }
            if (input_pressed(VK_RETURN)) possess(g_select);
            for (int i = 0; i < MAX_PEDS; i++) if (g_ped[i].used && i != g_select) {
                update_npc(&g_ped[i], dt); move_ped(&g_ped[i], dt);
            }
            if (g_select >= 0) { Ped *s = &g_ped[g_select]; s->vx = s->vz = 0; s->st = P_IDLE; }
            // keep the selected citizen in view
            if (g_select >= 0) {
                Ped *s = &g_ped[g_select]; int sx, sy; cam_project(&g_cam, s->x, ground(s->x, s->z), s->z, &sx, &sy);
                if (sx < 60 || sy < 60 || sx > 660 || sy > 480) cam_scroll_by((sx - 360) / 4 * 2, (sy - 270) / 4 * 2);
            }
        } else {
            step_peds(dt); step_cars(dt);
            static float pop, rescan; pop -= dt; rescan -= dt;
            if (pop <= 0) { pop = 0.5f; populate(); }
            if (rescan <= 0) { rescan = 20; net_scan(); geo_scan(); }
            static float calm; calm += dt;   // stars fade if you lie low
            if (calm > 10 && g_wanted > 0) { calm = 0; g_wanted--; }
            if (g_player >= 0 && g_ped[g_player].st == P_DOWN && g_ped[g_player].t < dt + 0.001f) msg("WASTED");
            if (g_follow) follow_camera();
        }
        draw_world();
        draw_hud();
    }
    overlay_set_clip(0, 0, 704, 542);           // city view: left of the toolbar, above the news ticker
    overlay_set_exclude(638, 432, 800, 600);    // minimap panel
    gta_debug_draw(&g_cam);
    overlay_commit();
}

// ------------------------------------------------------------------ test/debug commands
bool gta_game_cmd(int argc, char **argv) {
    const char *c = argv[0];
    if (!strcmp(c, "gta")) {
        if (argc > 1 && !strcmp(argv[1], "on")) gta_start();
        else if (argc > 1 && !strcmp(argv[1], "off")) gta_stop();
        else if (argc > 1 && !strcmp(argv[1], "possess")) possess(g_select);
        else if (argc > 2 && !strcmp(argv[1], "wanted")) g_wanted = atoi(argv[2]);
        else if (argc > 1 && !strcmp(argv[1], "arrows")) g_debug_arrows = !g_debug_arrows;
        else if (argc > 1 && !strcmp(argv[1], "wallprobe") && g_player >= 0 && g_ped[g_player].car >= 0) {
            // park the driven car diagonally in front of the nearest building tile, nose 30 degrees off the wall normal
            Car *c = &g_car[g_ped[g_player].car]; int cx = (int)(c->x / 256), cz = (int)(c->z / 256), best = -1, bx = 0, bz = 0;
            for (int x = cx - 10; x <= cx + 10; x++) for (int z = cz - 10; z <= cz + 10; z++) {
                float px = x * 256.0f + 128, pz = z * 256.0f - 240;   // probe point in front of the tile's -z face
                if (!solid(x * 256.0f + 128, z * 256.0f + 128) || solid(px, pz)) continue;
                int d = abs(x - cx) + abs(z - cz); if (best < 0 || d < best) { best = d; bx = x; bz = z; }
            }
            if (best >= 0) { c->x = bx * 256.0f + 128 - 60; c->z = bz * 256.0f - 240; c->ang = 30 * PI / 180; c->speed = 0; out_printf("wall at %d,%d\n", bx, bz); }
        }
        else if (argc > 2 && !strcmp(argv[1], "heading") && g_player >= 0 && g_ped[g_player].car >= 0)   // gta heading <deg>
            g_car[g_ped[g_player].car].ang = strtof(argv[2], 0) * PI / 180;
        else if (argc > 2 && !strcmp(argv[1], "tp") && g_player >= 0) {   // gta tp <ped>: stand next to a citizen, facing it
            int i = atoi(argv[2]); Ped *p = &g_ped[g_player], *v = &g_ped[i];
            if (i >= 0 && i < MAX_PEDS && v->used && i != g_player) {
                p->x = v->x - 70; p->z = v->z; p->hx = 1; p->hz = 0; v->vx = v->vz = 0; v->st = P_IDLE; v->think = 5;
            }
        }
        else if (argc > 1 && !strcmp(argv[1], "tpcar") && g_player >= 0) {   // stand next to the nearest car
            Ped *p = &g_ped[g_player]; int ci = nearest_car(p->x, p->z, 1e12f);
            if (ci >= 0) { p->x = g_car[ci].x + 90; p->z = g_car[ci].z + 90; out_printf("next to car %d\n", ci); }
        }
        else if (argc > 2 && !strcmp(argv[1], "fighter") && g_player >= 0) {   // gta fighter <ped>: make a citizen a fighter
            int i = atoi(argv[2]); if (i >= 0 && i < MAX_PEDS && g_ped[i].used) { g_ped[i].set = SET_FIGHTER; g_ped[i].tough = true; }
        }
        else if (argc > 2 && !strcmp(argv[1], "crowd") && g_player >= 0) {   // gta crowd <n> [tiles ahead]
            Ped *pl = &g_ped[g_player]; int tiles = argc > 3 ? atoi(argv[3]) : 4;
            float ang = pl->car >= 0 ? g_car[pl->car].ang : atan2f(pl->hx, pl->hz);
            float cx = pl->x + sinf(ang) * tiles * 256, cz = pl->z + cosf(ang) * tiles * 256;
            if (pl->car >= 0 && g_car[pl->car].on_road) {   // on the road ahead, as far as it runs straight
                Car *c = &g_car[pl->car]; int tx = (int)(c->x / 256), tz = (int)(c->z / 256), d = c->dir;
                for (int k = 0; k < tiles && road(tx + DX[d], tz + DZ[d]); k++) { tx += DX[d]; tz += DZ[d]; }
                cx = tx * 256.0f + 128; cz = tz * 256.0f + 128;
            }
            int n = 0;
            for (int k = 0; k < atoi(argv[2]); k++) {
                float a = frange(0, 2 * PI), r = sqrtf(frand()) * 170;
                int i = spawn_ped(cx + sinf(a) * r, cz + cosf(a) * r, WALKERS[(int)(frand() * NWALKERS) % NWALKERS]);
                if (i < 0) break;
                Ped *p = &g_ped[i]; float hx = cx - p->x, hz = cz - p->z, hl = hypotf(hx, hz) + 1;
                p->st = P_IDLE; p->think = 60; p->crowd = true; p->tough = false; p->hx = hx / hl; p->hz = hz / hl; n++;
            }
            out_printf("crowd of %d at %.0f,%.0f\n", n, cx, cz);
        }
        else if (argc > 1 && !strcmp(argv[1], "autodrive")) {   // gta autodrive <speed> | gta autodrive off
            g_autodrive = argc > 2 ? strtof(argv[2], 0) : 1000;
            if (g_player >= 0 && g_ped[g_player].car >= 0 && g_autodrive > 0) out_printf("road lock %d\n", road_lock(&g_car[g_ped[g_player].car], true));
        }
        else if (argc > 3 && !strcmp(argv[1], "focus")) { g_focus_x = atoi(argv[2]); g_focus_y = atoi(argv[3]); }   // follow-camera target
        else if (argc > 2 && !strcmp(argv[1], "speed") && g_player >= 0 && g_ped[g_player].car >= 0) g_car[g_ped[g_player].car].speed = strtof(argv[2], 0);
        else if (argc > 1 && !strcmp(argv[1], "tpahead") && g_player >= 0 && g_ped[g_player].car < 0) {   // stand in the road ahead of a moving car
            Ped *p = &g_ped[g_player]; int best = -1; float bd = 1e12f, dist = argc > 2 ? strtof(argv[2], 0) : 200;
            for (int i = 0; i < MAX_CARS; i++) if (g_car[i].used && !g_car[i].parked && g_car[i].on_road) {
                float d = hypotf(g_car[i].x - p->x, g_car[i].z - p->z); if (d < bd) { bd = d; best = i; }
            }
            if (best >= 0) {
                Car *c = &g_car[best]; p->x = c->x + sinf(c->ang) * dist; p->z = c->z + cosf(c->ang) * dist;
                p->hx = -sinf(c->ang); p->hz = -cosf(c->ang); p->st = P_IDLE; out_printf("ahead of car %d\n", best);
            }
        }
        else if (argc > 2 && !strcmp(argv[1], "timescale")) g_timescale = strtof(argv[2], 0);
        else if (argc > 2 && !strcmp(argv[1], "native")) g_native = atoi(argv[2]) != 0;   // 0: draw in the overlay
        else if (argc > 2 && !strcmp(argv[1], "follow")) g_follow = atoi(argv[2]) != 0;
        else if (argc > 3 && !strcmp(argv[1], "slowmo")) { g_slowmo_scale = strtof(argv[2], 0); g_slowmo_secs = strtof(argv[3], 0); g_slowmo_until = 0; }   // gta slowmo <scale> <seconds>
        else if (argc > 2 && !strcmp(argv[1], "hp") && g_player >= 0) g_ped[g_player].hp = atoi(argv[2]);
        else if (argc > 2 && !strcmp(argv[1], "mapdump")) {   // gta mapdump <file>: net types (u32), geo flags, net altitude
            FILE *f = fopen(argv[2], "wb");
            if (f) { fwrite(g_net, 1, sizeof g_net, f); fwrite(g_geo, 1, sizeof g_geo, f); fwrite(g_net_alt, 1, sizeof g_net_alt, f); fclose(f); out_printf("dumped\n"); }
        }
        else if (argc > 4 && !strcmp(argv[1], "carat") && g_player >= 0 && g_ped[g_player].car >= 0) {   // gta carat <tx> <tz> <dir 0..3>
            Car *c = &g_car[g_ped[g_player].car]; int tx = atoi(argv[2]), tz = atoi(argv[3]), d = atoi(argv[4]) & 3;
            lane_point(tx, tz, d, 56, &c->x, &c->z); c->ang = atan2f((float)DX[d], (float)DZ[d]); c->speed = 0;
            c->on_road = true; c->dir = d; c->tx = tx + DX[d]; c->tz = tz + DZ[d];
            lane_point(c->tx, c->tz, d, 56, &c->wx, &c->wz);
            g_ped[g_player].x = c->x; g_ped[g_player].z = c->z;
        }
        else if (argc > 3 && !strcmp(argv[1], "at") && g_player >= 0 && g_ped[g_player].car < 0) {   // gta at <tx> <tz>: player on foot
            g_ped[g_player].x = atoi(argv[2]) * 256.0f + 128; g_ped[g_player].z = atoi(argv[3]) * 256.0f + 128;
        }
        else if (argc > 2 && !strcmp(argv[1], "carset")) {   // gta carset <hex set> [car index]: repaint a car (default: the driven one)
            int i = argc > 3 ? atoi(argv[3]) : (g_player >= 0 ? g_ped[g_player].car : -1);
            if (i >= 0 && i < MAX_CARS && g_car[i].used) g_car[i].set = (uint16_t)strtol(argv[2], 0, 16);
        }
        else if (argc > 6 && !strcmp(argv[1], "spawncar")) {   // gta spawncar <hex set> <tx> <tz> <dir> <speed>: traffic on a road
            int tx = atoi(argv[3]), tz = atoi(argv[4]), d = atoi(argv[5]) & 3; float wx, wz;
            lane_point(tx, tz, d, 56, &wx, &wz);
            int i = spawn_car(wx, wz, atan2f((float)DX[d], (float)DZ[d]), false);
            if (i >= 0) {
                Car *c = &g_car[i]; c->set = (uint16_t)strtol(argv[2], 0, 16); c->target_speed = c->speed = strtof(argv[6], 0);
                c->on_road = true; c->dir = d; c->tx = tx + DX[d]; c->tz = tz + DZ[d];
                lane_point(c->tx, c->tz, d, 56, &c->wx, &c->wz);
                out_printf("car %d\n", i);
            }
        }
        else if (argc > 2 && !strcmp(argv[1], "bring") && g_player >= 0) {   // gta bring <ped> [dist]: put a citizen in front of the player
            int i = atoi(argv[2]); float dist = argc > 3 ? strtof(argv[3], 0) : 80; Ped *p = &g_ped[g_player], *v = &g_ped[i];
            if (i >= 0 && i < MAX_PEDS && v->used && i != g_player) {
                v->x = p->x + p->hx * dist; v->z = p->z + p->hz * dist; v->hx = -p->hx; v->hz = -p->hz;
                v->vx = v->vz = 0; v->st = P_IDLE; v->think = 30; v->on_walk = false; v->y = v->vy = 0; v->hp = 3;
            }
        }
        else if (argc > 3 && !strcmp(argv[1], "face") && g_player >= 0) {   // gta face <dx> <dz>: player facing (world)
            float x = strtof(argv[2], 0), z = strtof(argv[3], 0), l = hypotf(x, z);
            if (l > 0) { g_ped[g_player].hx = x / l; g_ped[g_player].hz = z / l; }
        }
        else if (argc > 1 && !strcmp(argv[1], "clear")) {   // remove everyone but the player and their car
            for (int i = 0; i < MAX_PEDS; i++) if (i != g_player) g_ped[i].used = false;
            for (int i = 0; i < MAX_CARS; i++) if (g_player < 0 || g_ped[g_player].car != i) g_car[i].used = false;
            g_wanted = 0;
        }
        else if (argc > 1 && !strcmp(argv[1], "ents")) {
            for (int i = 0; i < MAX_PEDS; i++) if (g_ped[i].used) {
                Ped *p = &g_ped[i]; int sx, sy; cam_project(&g_cam, p->x, ground(p->x, p->z), p->z, &sx, &sy);
                const Image *im = ped_image(p, 0, dir8(p->hx, p->hz));
                out_printf("ped %d set %04x st %d at %.0f,%.0f y=%.0f screen %d,%d img %s\n", i, p->set, p->st, p->x, p->z, ground(p->x, p->z), sx, sy, im ? "ok" : "MISSING");
            }
            for (int i = 0; i < MAX_CARS; i++) if (g_car[i].used) {
                Car *c = &g_car[i]; int sx, sy; cam_project(&g_cam, c->x, ground(c->x, c->z), c->z, &sx, &sy);
                out_printf("car %d set %04x at %.0f,%.0f screen %d,%d img %s\n", i, c->set, c->x, c->z, sx, sy, car_image(c) ? "ok" : "MISSING");
            }
        }
        out_printf("mode=%d player=%d select=%d score=%d wanted=%d zoom=%d rot=%d\n", g_mode, g_player, g_select, g_score, g_wanted, g_cam.zoom, g_cam.rot);
        if (g_player >= 0) {
            Ped *p = &g_ped[g_player]; out_printf("player %.0f,%.0f st=%d hp=%d car=%d\n", p->x, p->z, p->st, p->hp, p->car);
            if (p->car >= 0) { Car *c = &g_car[p->car]; out_printf("car ang=%.2f speed=%.0f set=%04x road=%d tile=%d,%d dir=%d wp=%.0f,%.0f\n", c->ang, c->speed, c->set, c->on_road, c->tx, c->tz, c->dir, c->wx, c->wz); }
        }
        int np = 0, nc = 0; for (int i = 0; i < MAX_PEDS; i++) np += g_ped[i].used; for (int i = 0; i < MAX_CARS; i++) nc += g_car[i].used;
        out_printf("peds=%d cars=%d\n", np, nc);
    } else if (!strcmp(c, "hold") && argc >= 3) {          // hold <key> <ms>: synthetic key held for a duration
        int vk = strlen(argv[1]) == 1 ? (argv[1][0] & ~0x20) : (int)strtol(argv[1], 0, 0);
        gta_hold(vk, atoi(argv[2]));
    } else return false;
    return true;
}
