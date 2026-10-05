// Native sprites: the mod's people and cars drawn by SIMSPR's own renderer instead of the overlay, so buildings,
// trees and the game's own props in front of them hide them (the overlay can only draw on top of everything).
//
// SIMSPR view (*(SIMSPR!0x10072bec)) dynamic sprite API, __thiscall on the view:
//   FUN_1000e52a AddSprite(img, x, z, y, invalidate, layer, flags)  -> bool (al)
//   FUN_1000e83d MoveSprite(img, oldx, oldz, newx, newz, y)         -> bool, redraws the old and new rects
//   FUN_1000e6d5 RemoveSprite(img, x, z, invalidate)                -> bool
// The view keeps a 44-byte record per sprite (bbox, world x/z/y, depth key) and when it redraws a region it sorts
// those records together with buildings, trees and road props by their ground footprint (FUN_1000d0f5,
// comparator FUN_1000c7a6, draw loop FUN_1000dc17).
//
// A sprite image (0x14 bytes: vtable, id, resource, child, two flag bytes) draws one frame of a sprite resource,
// which holds every frame of one sprite set from the DAT files. Resources come from the resource manager
// (FUN_1005a97e) with GetResource({0x6300, 0x6400, set}, IID 0x6100, &res) (vtable +0x14); the frame index is the
// DAT instance number (people: frame*16 + (zoom4 ? 8 : 0) + dir). The game's own image class (vtable 0x10062b80)
// picks its frame with vtable +0x68 FrameKey(zoom, rot). We copy that vtable and replace FrameKey, AddRef and
// Release (the game keeps the refcount in id bits 17-29 and deletes the object at zero), so GetBounds, Load,
// Unload and Draw are the game's own code running on our object.
#include "native.h"
#include <math.h>
#include <string.h>

#define THISCALL __attribute__((thiscall))
typedef uint32_t (THISCALL *AddFn)(void *view, void *img, int x, int z, int y, int inval, int layer, int flags);
typedef uint32_t (THISCALL *MoveFn)(void *view, void *img, int ox, int oz, int nx, int nz, int ny);
typedef uint32_t (THISCALL *RemoveFn)(void *view, void *img, int x, int z, int inval);
typedef void *(*GetRmFn)(void);
typedef uint32_t (THISCALL *GetResFn)(void *rm, const uint32_t *key, uint32_t iid, void **out);
typedef uint32_t (THISCALL *ResKeyFn)(void *res, int key);

static AddFn s_add; static MoveFn s_move; static RemoveFn s_remove; static GetRmFn s_getrm;
static uintptr_t s_viewptr;          // address of SIMSPR's view pointer
static void *s_vt[32];
bool g_nat_ok;
int g_nat_calls;

static uint32_t THISCALL img_addref(NatImg *n) { return (uint32_t)InterlockedIncrement(&n->refs); }
static uint32_t THISCALL img_release(NatImg *n) { return n->refs > 0 ? (uint32_t)InterlockedDecrement(&n->refs) : 0; }
static uint32_t THISCALL img_framekey(NatImg *n, int zoom, int rot) {
    (void)rot;   // our keys are recomputed every tick for the current view rotation
    int k = zoom >= 0 && zoom <= 4 ? n->key[zoom] : -1;
    for (int z = 4; k < 0 && z >= 0; z--) k = n->key[z];   // never hand the resource a frame it doesn't have
    return k < 0 ? 0 : (uint32_t)k;
}

bool nat_init(void) {
    if (g_nat_ok) return true;
    uintptr_t vt = ghidra_to_rt("SIMSPR.DLL", 0x10062b80);
    if (!vt || !mem_read(vt, s_vt, sizeof s_vt)) return false;
    s_vt[0x04 / 4] = (void *)img_addref;
    s_vt[0x08 / 4] = (void *)img_release;
    s_vt[0x68 / 4] = (void *)img_framekey;
    s_add = (AddFn)ghidra_to_rt("SIMSPR.DLL", 0x1000e52a);
    s_move = (MoveFn)ghidra_to_rt("SIMSPR.DLL", 0x1000e83d);
    s_remove = (RemoveFn)ghidra_to_rt("SIMSPR.DLL", 0x1000e6d5);
    s_getrm = (GetRmFn)ghidra_to_rt("SIMSPR.DLL", 0x1005a97e);
    s_viewptr = ghidra_to_rt("SIMSPR.DLL", 0x10072bec);
    g_nat_ok = s_add && s_move && s_remove && s_getrm && s_viewptr;
    LOG("nat_init: %s", g_nat_ok ? "ok" : "missing SIMSPR");
    return g_nat_ok;
}

// Sprite resources, one per set, held for the whole session, with the frames we have loaded.
typedef struct { uint16_t set; void *res; uint32_t loaded[8]; } Res;
static Res s_res[64];
static int s_nres;

static Res *get_res(uint16_t set) {
    for (int i = 0; i < s_nres; i++) if (s_res[i].set == set) return s_res[i].res ? &s_res[i] : NULL;
    if (s_nres == 64) return NULL;
    Res *r = &s_res[s_nres++]; memset(r, 0, sizeof *r); r->set = set;
    void *rm = s_getrm(), *res = NULL;
    uint32_t key[3] = { 0x6300, 0x6400, set };
    if (rm && (((GetResFn)(*(void ***)rm)[0x14 / 4])(rm, key, 0x6100, &res) & 0xff) && res) r->res = res;
    g_nat_calls++;
    LOG("nat: resource for sprite set %04x -> %p", set, res);
    return r->res ? r : NULL;
}

// The view loads a sprite's current frame when the sprite is added; frames we switch to later must be loaded by
// us. We load each frame once and keep it (the view's own Load/Unload pairs stay balanced on top of that).
static void load_key(Res *r, int key) {
    if (key < 0 || key > 255 || ((r->loaded[key >> 5] >> (key & 31)) & 1)) return;
    ((ResKeyFn)(*(void ***)r->res)[0x34 / 4])(r->res, key);
    r->loaded[key >> 5] |= 1u << (key & 31);
    g_nat_calls++;
}

static uintptr_t cur_view(void) { uintptr_t v = 0; return s_viewptr && mem_read(s_viewptr, &v, 4) ? v : 0; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void nat_forget(NatImg *n) { n->added = false; }

void nat_hide(NatImg *n) {
    if (!n->added) return;
    if (g_nat_ok && cur_view() == n->view) { s_remove((void *)n->view, n, n->x, n->z, 1); g_nat_calls++; }
    n->added = false;
}

void nat_show(NatImg *n, uint16_t set, const int key[5], float fx, float fz, float fy) {
    if (!g_nat_ok) return;
    uintptr_t view = cur_view();
    if (!view) return;
    if (n->added && n->view != view) nat_forget(n);   // another city: the old view and its records are gone
    if (!n->vt) { memset(n, 0, sizeof *n); n->vt = s_vt; }
    Res *r = get_res(set);
    if (!r) { nat_hide(n); return; }
    if (n->added && n->res != r->res) nat_hide(n);    // another sprite set: re-add so Load/Unload stay paired
    for (int i = 0; i < 5; i++) load_key(r, key[i]);
    bool key_changed = memcmp(n->key, key, sizeof n->key) != 0;
    memcpy(n->key, key, sizeof n->key);
    n->res = r->res; n->set = set;
    int x = clampi((int)lrintf(fx), 0, 65535), z = clampi((int)lrintf(fz), 0, 65535), y = (int)lrintf(fy);
    if (!n->added) {
        // Layer 1 like buildings: where two footprints overlap the view sorts by layer first, and a ped on a
        // sidewalk or a car in the outer lane reaches into the next tile, so layer 2 (road props) put them on top
        // of the building beside them. With equal layers the view uses its depth order.
        uint32_t ok = s_add((void *)view, n, x, z, y, 1, 1, 0);
        g_nat_calls++;
        if (ok & 0xff) { n->added = true; n->view = view; n->x = x; n->z = z; n->y = y; }
        return;
    }
    if (x != n->x || z != n->z || y != n->y || key_changed) {   // same position: refreshes the frame and its rect
        s_move((void *)view, n, n->x, n->z, x, z, y);
        g_nat_calls++;
        n->x = x; n->z = z; n->y = y;
    }
}

bool nat_cmd(int argc, char **argv) {
    if (strcmp(argv[0], "nat")) return false;
    (void)argc;
    out_printf("native sprites %s, %d game calls, %d resources:", g_nat_ok ? "on" : "off", g_nat_calls, s_nres);
    for (int i = 0; i < s_nres; i++) out_printf(" %04x%s", s_res[i].set, s_res[i].res ? "" : "(missing)");
    out_printf("\n");
    return true;
}
