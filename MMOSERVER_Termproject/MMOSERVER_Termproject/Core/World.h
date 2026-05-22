#pragma once

#include <memory>
#include "NPC.h"
#include "../protocol_2026.h"

// NPC 스토리지: 시작 시 한 번 할당, 종료까지 유지. NPC id = NPC_ID_START + index
// shared_ptr이 아닌 raw 배열로 200K 메모리 풋프린트 최소화 (Stage 4 리스크 대응)
extern std::unique_ptr<NPC[]> g_npcs;
extern int g_npc_count;  // 실제 스폰된 NPC 수 (배열 사용 범위)

inline bool IsNpcId(int id) { return id >= NPC_ID_START; }
inline int NpcIndex(int id) { return id - NPC_ID_START; }
inline NPC& GetNpc(int id) { return g_npcs[NpcIndex(id)]; }

// 시작 시 capacity 만큼 NPC 객체 할당 (id는 LoadNpcSpawnScript에서 채움)
void InitWorld(int capacity);
void DestroyWorld();
