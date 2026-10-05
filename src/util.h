#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void log_init(const char *dir);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)

extern char g_moddir[MAX_PATH];   // folder holding gta_mode.dll (the game's Apps folder)
extern bool g_dev;                // developer command channel enabled (Apps\gta_mode.dev present at start)

// command channel (cmd.c)
void cmd_poll(void);              // called from the main-thread tick
void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// safe memory access (cmd.c)
bool mem_read(uintptr_t addr, void *buf, size_t n);
bool mem_write(uintptr_t addr, const void *buf, size_t n);

// addresses (addr.c, callraw.S)
uintptr_t mod_base(const char *name);
uintptr_t ghidra_to_rt(const char *mod, uint32_t va);
uintptr_t addr_eval(const char *s);
uint32_t call_raw(void *fn, uint32_t ecx, const uint32_t *args, int n);

// memory scanner (scan.c)
void scan_cmd(int argc, char **argv);

// game layer (gta.c)
void gta_tick(uint32_t now_ms);
bool gta_cmd(int argc, char **argv);   // returns true if handled
