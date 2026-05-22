#include "World.h"

std::unique_ptr<NPC[]> g_npcs;
int g_npc_count = 0;

void InitWorld(int capacity) {
    g_npcs.reset(new NPC[capacity]);
    g_npc_count = 0;
}

void DestroyWorld() {
    g_npcs.reset();
    g_npc_count = 0;
}
