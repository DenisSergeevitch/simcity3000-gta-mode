// Minimal GZCOM (Maxis framework) contract as used by SC3U.exe, recovered from MaxisAddOn.dll and
// SC3U.exe (see MODLOG.md). All methods are MSVC __thiscall: `this` in ECX, callee pops stack args.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define THISCALL __attribute__((thiscall))

#define GZIID_cIGZUnknown        0x00000001u
#define GZIID_cIGZCOMDirector    0xA21EE941u
#define GZIID_cIGZFrameWorkHooks 0x03FA40BFu

// cIGZCOM (SC3U.exe vtable 0x4d6d18): +0x10 FrameWork(), +0x14 AddLibrary(path)
// cIGZFrameWork (SC3U.exe vtable 0x4d6bf4):
enum {
    FW_AddSystemService = 0x0c / 4,
    FW_GetSystemService = 0x10 / 4,   // (GZGUID srvid, GZIID iid, void **out)
    FW_AddHook          = 0x18 / 4,
    FW_AddToTick        = 0x20 / 4,   // service->vt[11](counter) every main-loop iteration
    FW_RemoveFromTick   = 0x24 / 4,
    FW_AddToOnIdle      = 0x28 / 4,
};

typedef struct GZObj { void **vt; } GZObj;

static inline void *gz_vfn(void *obj, int idx) { return (*(void ***)obj)[idx]; }
