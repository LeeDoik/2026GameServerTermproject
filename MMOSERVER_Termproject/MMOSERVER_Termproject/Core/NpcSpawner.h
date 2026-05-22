#pragma once

// 스크립트(`data/npc_spawn.txt`) 파일에서 NPC 스폰 구성을 읽어 g_npcs를 채운다.
// 반환: 스폰된 NPC 수. 실패 시 -1. 호출 후 호출자가 각 NPC를 Sector에 등록해야 함.
int LoadNpcSpawnScript(const char* path);
