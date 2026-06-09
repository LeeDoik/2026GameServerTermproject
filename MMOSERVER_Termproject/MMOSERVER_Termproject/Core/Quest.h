#pragma once

// Stage 9: 퀘스트 시스템.
// 카탈로그(QuestDef)는 data/quests.txt에서 로드 (스크립트 로딩 가산점 연계).
// 슬레이(처치) + 대화(수락/보상) + 연쇄(prereq) 풀세트.
// 플레이어별 진행 상태(QuestProgress)는 Player가 보유.

#include <vector>
#include <unordered_map>
#include "../protocol_2026.h"  // MAX_NAME_LEN

struct QuestDef {
    int id = 0;
    int prereq_id = -1;                 // 선행 퀘스트 ID (-1 = 없음). 연쇄용
    unsigned char giver_npc = 0;        // 퀘스트를 주는 마을 NPC 인덱스 (0 = 장로)
    char target_species[MAX_NAME_LEN] = { 0 };  // 처치 대상 종족 접두사 (예: "Slime")
    int target_count = 1;
    unsigned long long reward_exp = 0;
    int reward_item_id = -1;            // 보상 아이템 (-1 = 없음)
    int reward_item_qty = 0;
};

// 카탈로그 (부팅 시 1회 로드 후 read-only) — 동시 읽기 안전.
extern std::unordered_map<int, QuestDef> g_quest_defs;

// 정의 조회. 없으면 nullptr.
const QuestDef* GetQuestDef(int quest_id);

// data/quests.txt 파싱. 성공 시 로드된 정의 수(>=0), 파일 못 열면 -1.
int LoadQuestDefs(const char* path);

// NPC 이름(예: "Slime_00042")의 '_' 앞 접두사가 species와 일치하는지.
bool QuestSpeciesMatches(const char* species, const char* npc_name);
