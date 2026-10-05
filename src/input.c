// Main-window discovery, keyboard capture for GTA mode, and synthetic input (for tests / RE).
#include "util.h"
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

HWND g_hwnd;
static WNDPROC g_prev_proc;
static volatile uint8_t g_keys[256];      // 1 = down (from window messages, includes synthetic input)
static volatile uint8_t g_pressed[256];   // edge-triggered presses, cleared by input_pressed()
bool (*g_capture_key)(int vk);            // set by the game layer: return true to hide this key from the game

static BOOL CALLBACK enum_cb(HWND h, LPARAM lp) {
    char title[128], cls[128]; RECT r, c;
    GetWindowTextA(h, title, sizeof title); GetClassNameA(h, cls, sizeof cls);
    GetWindowRect(h, &r); GetClientRect(h, &c);
    if (lp) out_printf("hwnd=%p vis=%d class='%s' title='%s' rect=%ld,%ld-%ld,%ld client=%ldx%ld\n", h, IsWindowVisible(h),
                       cls, title, r.left, r.top, r.right, r.bottom, c.right, c.bottom);
    if (!g_hwnd && IsWindowVisible(h) && !strcmp(title, "SimCity 3000")) g_hwnd = h;
    return TRUE;
}

HWND input_hwnd(void) {
    if (!g_hwnd || !IsWindow(g_hwnd)) { g_hwnd = NULL; EnumThreadWindows(GetCurrentThreadId(), enum_cb, 0); }
    return g_hwnd;
}

static LRESULT CALLBACK sub_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (wp < 256) { if (!g_keys[wp]) g_pressed[wp] = 1; g_keys[wp] = 1; }
        if (wp < 256 && g_capture_key && g_capture_key((int)wp)) return 0;
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (wp < 256) g_keys[wp] = 0;
        if (wp < 256 && g_capture_key && g_capture_key((int)wp)) return 0;
        break;
    case WM_CHAR: {
        int vk = wp == '\r' ? VK_RETURN : wp == ' ' ? VK_SPACE : toupper((int)(wp & 0xff));
        if (g_capture_key && g_capture_key(vk)) return 0;
        break;
    }
    case WM_KILLFOCUS: case WM_ACTIVATEAPP:
        memset((void *)g_keys, 0, sizeof g_keys);
        break;
    }
    return CallWindowProcA(g_prev_proc, h, msg, wp, lp);
}

bool input_install(void) {
    if (g_prev_proc) return true;
    HWND h = input_hwnd(); if (!h) return false;
    g_prev_proc = (WNDPROC)SetWindowLongA(h, GWL_WNDPROC, (LONG)(uintptr_t)sub_proc);
    LOG("input: subclassed %p, previous proc %p", h, g_prev_proc);
    return g_prev_proc != NULL;
}

bool input_down(int vk) {
    if (vk < 0 || vk > 255) return false;
    if (g_keys[vk]) return true;
    HWND h = input_hwnd();
    return h && GetForegroundWindow() == h && (GetAsyncKeyState(vk) & 0x8000);
}

bool input_pressed(int vk) {
    if (vk < 0 || vk > 255 || !g_pressed[vk]) return false;
    g_pressed[vk] = 0; return true;
}

void input_clear_pressed(void) { memset((void *)g_pressed, 0, sizeof g_pressed); }

// cnc-ddraw subclasses the window and drops mouse messages unless its cursor lock is active, and rescales
// coordinates. The class's own window procedure (GZGraphicD) takes game coordinates (800x600) directly.
LRESULT input_send(UINT msg, WPARAM wp, LPARAM lp) {
    HWND h = input_hwnd(); if (!h) return 0;
    WNDPROC orig = (WNDPROC)GetClassLongA(h, GCL_WNDPROC);
    return CallWindowProcA(orig, h, msg, wp, lp);
}

static void post_click(int x, int y, int button) {
    HWND h = input_hwnd(); if (!h) { out_printf("no window\n"); return; }
    LPARAM lp = MAKELPARAM(x, y);
    UINT dn = button == 2 ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, up = button == 2 ? WM_RBUTTONUP : WM_LBUTTONUP;
    WPARAM mk = button == 2 ? MK_RBUTTON : MK_LBUTTON;
    input_send(WM_MOUSEMOVE, 0, lp);
    input_send(dn, mk, lp);
    input_send(up, 0, lp);
}

void input_click(int x, int y) { post_click(x, y, 1); }

// Synthetic key through our own subclass (so GTA-mode capture and key state see it like a real key).
void input_key(int vk, bool down) {
    HWND h = input_hwnd(); if (!h) return;
    UINT sc = MapVirtualKeyA(vk, 0);
    LPARAM lp = 1 | (sc << 16) | (down ? 0 : (3u << 30));
    SendMessageA(h, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
}

bool input_cmd(int argc, char **argv) {
    const char *c = argv[0];
    if (!strcmp(c, "win")) { g_hwnd = NULL; EnumThreadWindows(GetCurrentThreadId(), enum_cb, 1); out_printf("main=%p\n", input_hwnd()); }
    else if (!strcmp(c, "wndproc")) { HWND h = input_hwnd(); out_printf("class proc=%08lx window proc=%08lx\n", GetClassLongA(h, GCL_WNDPROC), GetWindowLongA(h, GWL_WNDPROC)); }
    else if (!strcmp(c, "move") && argc >= 3) input_send(WM_MOUSEMOVE, 0, MAKELPARAM(atoi(argv[1]), atoi(argv[2])));
    else if (!strcmp(c, "click") && argc >= 3) post_click(atoi(argv[1]), atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 1);
    else if (!strcmp(c, "key") && argc >= 2) {           // key <vk|char> [down|up]  (default: press)
        int vk = strlen(argv[1]) == 1 ? (int)(unsigned char)toupper(argv[1][0]) : (int)strtol(argv[1], 0, 0);
        if (argc < 3 || !strcmp(argv[2], "down")) input_key(vk, true);
        if (argc < 3 || !strcmp(argv[2], "up")) input_key(vk, false);
    }
    else return false;
    return true;
}
