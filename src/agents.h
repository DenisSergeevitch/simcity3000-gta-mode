#pragma once
#include "util.h"
#define MAX_AGENTS 512
typedef struct { uint32_t addr; int cls; uint32_t type; float x, y, z, dx, dz; } Agent;
extern Agent g_agents[MAX_AGENTS];
extern int g_nagents;
int agents_scan(void);
bool agent_refresh(Agent *a);
