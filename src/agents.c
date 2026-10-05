// The game's own street agents (STRTSIM.DLL): cars, pedestrians, boats... Read-only view for now.
// Agent object (see MODLOG.md "Agents"): +0 class vtable (pooled/dead objects keep the base vtable 0x1002b95c),
// +0x18/+0x1c interface vtables, +0x24 type bits, +0x44 owner (spawner), +0x58 progress (float),
// +0x5c segment length (float), +0x60 segment start (int x,y,z), +0x6c segment end, +0x78 direction (float x,y,z).
#include "agents.h"
#include <string.h>
#include <math.h>

static const uint32_t AGENT_VT[] = { 0x1002cd20, 0x1002a330, 0x1002a758, 0x1002ad44, 0x1002af90, 0x1002b24c,
                                     0x1002bbe0, 0x1002be78, 0x1002c19c, 0x1002c418, 0x1002c9bc };
#define NCLASS (int)(sizeof AGENT_VT / sizeof *AGENT_VT)
Agent g_agents[MAX_AGENTS];
int g_nagents;

static bool agent_pos(uint32_t a, Agent *out) {
    uint8_t b[0x90];
    if (!mem_read(a, b, sizeof b)) return false;
    float prog = *(float *)(b + 0x58), len = *(float *)(b + 0x5c);
    int32_t *p0 = (int32_t *)(b + 0x60); float *dir = (float *)(b + 0x78);
    if (!(prog >= -1 && prog <= 100000) || !(len >= 0 && len < 100000)) return false;
    if (p0[0] < 0 || p0[0] > 65535 || p0[2] < 0 || p0[2] > 65535) return false;
    out->x = p0[0] + dir[0] * prog; out->y = (float)p0[1]; out->z = p0[2] + dir[2] * prog;
    out->dx = dir[0]; out->dz = dir[2];
    out->type = *(uint32_t *)(b + 0x24);
    return true;
}

int agents_scan(void) {
    uint32_t rt[NCLASS], lo = 0xffffffff, hi = 0;
    for (int k = 0; k < NCLASS; k++) { rt[k] = ghidra_to_rt("STRTSIM.DLL", AGENT_VT[k]); if (rt[k] < lo) lo = rt[k]; if (rt[k] > hi) hi = rt[k]; }
    g_nagents = 0; if (!rt[0]) return 0;
    static uint8_t buf[1 << 16]; MEMORY_BASIC_INFORMATION mi;
    for (uintptr_t a = 0x10000; a < 0x7fff0000 && g_nagents < MAX_AGENTS; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
        if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
        uintptr_t rs = (uintptr_t)mi.BaseAddress, re = rs + mi.RegionSize;
        for (uintptr_t c = rs; c < re && g_nagents < MAX_AGENTS; c += sizeof buf) {
            size_t n = re - c < sizeof buf ? re - c : sizeof buf;
            if (!mem_read(c, buf, n)) continue;
            for (size_t i = 0; i + 4 <= n; i += 4) {
                uint32_t v = *(uint32_t *)(buf + i); int k;
                if (v < lo || v > hi) continue;
                for (k = 0; k < NCLASS && v != rt[k]; k++);
                if (k == NCLASS) continue;
                Agent ag; memset(&ag, 0, sizeof ag);
                if (!agent_pos(c + i, &ag)) continue;
                ag.addr = c + i; ag.cls = k;
                g_agents[g_nagents++] = ag;
                if (g_nagents >= MAX_AGENTS) break;
            }
        }
    }
    return g_nagents;
}

bool agent_refresh(Agent *a) { int cls = a->cls; uint32_t addr = a->addr; bool ok = agent_pos(addr, a); a->cls = cls; a->addr = addr; return ok; }
