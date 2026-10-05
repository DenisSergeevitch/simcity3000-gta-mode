// gta_mode.dll: a GZCOM plugin for SimCity 3000 Unlimited.
// SC3U.exe loads every *.dll in Apps\, calls GZDllGetGZCOMDirector(), then director->InitializeCOM(com, path)
// and director->OnStart(com). In OnStart we register a tick service with the framework; the framework's
// main loop then calls our OnTick on the game's main thread every iteration.
#include "util.h"
#include "gzcom.h"
#include "version.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

char g_moddir[MAX_PATH];
// The developer command channel (Apps\gta_cmd.txt -> gta_out.txt: memory reads/writes, function calls, input,
// filming commands) is off unless Apps\gta_mode.dev exists when the game starts. tools/run.sh creates it.
bool g_dev;
static HMODULE g_self;
static FILE *g_log;
void *g_com, *g_fw;   // cIGZCOM*, cIGZFrameWork*

// Appends, so a crash's EXCEPTION lines survive the next launch; the previous run is kept as gta_mode.old.log once
// the log passes 1 MB.
void log_init(const char *dir) {
    char p[MAX_PATH], old[MAX_PATH];
    snprintf(p, sizeof p, "%s\\gta_mode.log", dir);
    snprintf(old, sizeof old, "%s\\gta_mode.old.log", dir);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (1u << 20)) MoveFileExA(p, old, MOVEFILE_REPLACE_EXISTING);
    g_log = fopen(p, "a");
    if (g_log) { SYSTEMTIME t; GetLocalTime(&t); fprintf(g_log, "==== session %04d-%02d-%02d %02d:%02d:%02d ====\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); }
}

void logf_(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- tick service (cIGZSystemService-like)
typedef struct { void **vt; } Service;
static THISCALL bool   sv_QI(Service *s, uint32_t iid, void **out) { *out = s; return true; }
static THISCALL uint32_t sv_AddRef(Service *s) { return 2; }
static THISCALL uint32_t sv_Release(Service *s) { return 1; }
static THISCALL uint32_t sv_GetID(Service *s) { return 0x47544131; }
static THISCALL Service *sv_SetID(Service *s, uint32_t id) { return s; }
static THISCALL int32_t  sv_Prio(Service *s) { return 0; }
static THISCALL bool   sv_IsRunning(Service *s) { return true; }
static THISCALL Service *sv_SetRunning(Service *s, bool b) { return s; }
static THISCALL bool   sv_Init(Service *s) { return true; }
static THISCALL bool   sv_Shutdown(Service *s) { return true; }
static THISCALL bool   sv_OnIdle(Service *s, uint32_t n) { return true; }
static THISCALL bool   sv_OnTick(Service *s, uint32_t n) {
    static uint32_t last_cmd;
    uint32_t now = GetTickCount();
    gta_tick(now);
    if (g_dev && now - last_cmd >= 100) { last_cmd = now; cmd_poll(); }
    return true;
}
static THISCALL int32_t  sv_TickPrio(Service *s) { return 0; }
static void *g_service_vt[] = {
    sv_QI, sv_AddRef, sv_Release, sv_GetID, sv_SetID, sv_Prio, sv_IsRunning, sv_SetRunning,
    sv_Init, sv_Shutdown, sv_OnIdle, sv_OnTick, sv_TickPrio,
};
static Service g_service = { g_service_vt };

// ---------------------------------------------------------------- director (cRZCOMDllDirector layout)
typedef struct { void **vt; uint32_t refs; } Director;
static THISCALL bool dr_QI(Director *d, uint32_t iid, void **out) {
    if (iid == GZIID_cIGZUnknown || iid == GZIID_cIGZCOMDirector) { *out = d; d->refs++; return true; }
    *out = NULL; return false;
}
static THISCALL uint32_t dr_AddRef(Director *d) { return ++d->refs; }
static THISCALL uint32_t dr_Release(Director *d) { if (d->refs) d->refs--; return d->refs; }
static THISCALL bool dr_InitializeCOM(Director *d, void *com, void *path) {
    g_com = com;
    g_fw = ((void *(THISCALL *)(void *))gz_vfn(com, 4))(com);   // cIGZCOM::FrameWork()
    LOG("InitializeCOM com=%p fw=%p", com, g_fw);
    return true;
}
// Log access violations (first chance, first 20) with module + offset, to diagnose crashes under Wine.
static LONG WINAPI crash_log(EXCEPTION_POINTERS *e) {
    static int n;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO) && n++ < 20) {
        void *addr = e->ExceptionRecord->ExceptionAddress; HMODULE m = NULL; char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m);
        if (m) GetModuleFileNameA(m, name, sizeof name);
        CONTEXT *c = e->ContextRecord;
        LOG("EXCEPTION %08lx at %p (%s+%lx) access %lx eax=%lx ecx=%lx edx=%lx esi=%lx edi=%lx esp=%lx thread %lu", code, addr, name,
            (unsigned long)((uint8_t *)addr - (uint8_t *)m), e->ExceptionRecord->NumberParameters > 1 ? (unsigned long)e->ExceptionRecord->ExceptionInformation[1] : 0,
            c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp, GetCurrentThreadId());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static THISCALL bool dr_OnStart(Director *d, void *com) {
    AddVectoredExceptionHandler(1, crash_log);
    bool ok = ((bool (THISCALL *)(void *, void *))gz_vfn(g_fw, FW_AddToTick))(g_fw, &g_service);
    LOG("OnStart: AddToTick -> %d", ok);
    return true;
}
static THISCALL void dr_EnumClassObjects(Director *d, void *cb, void *ctx) {}
static THISCALL bool dr_GetClassObject(Director *d, uint32_t clsid, uint32_t iid, void **out) { *out = NULL; return false; }
static THISCALL bool dr_CanUnloadNow(Director *d) { return false; }
static THISCALL bool dr_OnUnload(Director *d) { return true; }
static THISCALL uint32_t dr_RefCount(Director *d) { return d->refs; }
static THISCALL uint32_t dr_RemoveRef(Director *d) { if (d->refs) d->refs--; return d->refs; }
static THISCALL void *dr_FrameWork(Director *d) { return g_fw; }
static THISCALL void *dr_GZCOM(Director *d) { return g_com; }
static THISCALL uint32_t dr_GetDirectorID(Director *d) { return 0x47544144; }  // "GTAD"
static THISCALL bool dr_GetLibraryPath(Director *d, void *str) { return false; }
static THISCALL void *dr_Dtor(Director *d, uint32_t flags) { return d; }
static THISCALL bool dr_Slot16(Director *d, void *p) { return true; }
static THISCALL bool dr_AddDirector(Director *d, void *child) { return true; }
static void *g_director_vt[] = {
    dr_QI, dr_AddRef, dr_Release, dr_InitializeCOM, dr_OnStart, dr_EnumClassObjects, dr_GetClassObject,
    dr_CanUnloadNow, dr_OnUnload, dr_RefCount, dr_RemoveRef, dr_FrameWork, dr_GZCOM, dr_GetDirectorID,
    dr_GetLibraryPath, dr_Dtor, dr_Slot16, dr_AddDirector,
};
static Director g_director = { g_director_vt, 1 };

__declspec(dllexport) void *GZDllGetGZCOMDirector(void) { return &g_director; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(h, g_moddir, MAX_PATH);
        char *slash = strrchr(g_moddir, '\\'); if (slash) *slash = 0;
        log_init(g_moddir);
        char flag[MAX_PATH]; snprintf(flag, sizeof flag, "%s\\gta_mode.dev", g_moddir);
        g_dev = GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES;
        LOG("GTA mode %s loaded at %p from %s; developer command channel %s", GTA_MODE_VERSION, h, g_moddir, g_dev ? "ON" : "off");
    }
    return TRUE;
}
