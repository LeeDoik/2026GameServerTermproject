#pragma once

// Stage 8: 아이템 시스템.
// 카탈로그(ItemDef)는 data/items.txt에서 로드 (스크립트 로딩 가산점 연계).
// 드롭 로직(가중 랜덤)도 여기 모음. 인벤토리/장착 상태는 Player가 보유.

#include <unordered_map>
#include <vector>
#include "NPC.h"               // NpcType, NpcMoveMode
#include "../protocol_2026.h"  // MAX_NAME_LEN

enum class ItemType : unsigned char {
    Consumable = 0,  // value = 회복 HP
    Weapon     = 1,  // value = 공격력 보너스
    Armor      = 2,  // value = 최대 HP 보너스
};

struct ItemDef {
    int id = 0;
    char name[MAX_NAME_LEN] = { 0 };
    ItemType type = ItemType::Consumable;
    int value = 0;
    int stack_max = 1;
    int drop_weight = 0;
};

// 카탈로그 (부팅 시 1회 로드 후 read-only) — 동시 읽기 안전
extern std::unordered_map<int, ItemDef> g_item_defs;

// 정의 조회. 없으면 nullptr.
const ItemDef* GetItemDef(int item_id);

// data/items.txt 파싱. 성공 시 로드된 정의 수(>=0), 파일 못 열면 -1.
int LoadItemDefs(const char* path);

// --- 드롭 ---
// NPC 처치 시 아이템을 떨어뜨릴지 (일반 35% / Agro 50% / Boss 100%)
bool RollShouldDrop(NpcType type, NpcMoveMode mode);

// 떨어뜨릴 아이템 ID 목록. 일반/Agro는 1개(가중 랜덤), Boss는 무기+방어구+포션 다중.
std::vector<int> RollDropItems(NpcType type, int level);
