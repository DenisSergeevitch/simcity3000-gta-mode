#pragma once
#include "util.h"
extern HWND g_hwnd;
extern bool (*g_capture_key)(int vk);
HWND input_hwnd(void);
bool input_install(void);              // subclass the game window (keyboard capture)
bool input_down(int vk);               // key held (window messages or real keyboard)
bool input_pressed(int vk);            // key went down since last call (edge)
void input_clear_pressed(void);
void input_key(int vk, bool down);
void input_click(int x, int y);
LRESULT input_send(UINT msg, WPARAM wp, LPARAM lp);
bool input_cmd(int argc, char **argv);
