#pragma once

// Stage 6.3: DB 영속성용 타입 정의.
// 백엔드 추상화(IDbBackend)와 워커(DbWorker)가 이 타입들을 공유.

#include <string>
#include <vector>
#include <utility>
#include <array>

// 저장 대상 핵심 필드 (PDF 명세 6장 수준 — 사용자 합의)
struct PlayerSnapshot {
    std::string username;
    int hp = 100;
    int max_hp = 100;          // Stage 8: base max_hp (장착 방어구 보너스 제외). 재장착 시 중복 합산 방지
    unsigned long long exp = 0;
    unsigned char level = 1;
    short x = 0;
    short y = 0;
    unsigned char direction = 0;  // 0=Down 1=Left 2=Right 3=Up
    // Stage 8: 아이템
    std::vector<std::pair<int, int>> inventory;  // (item_id, qty)
    int equipped_weapon_id = -1;
    int equipped_armor_id = -1;
    // Stage 9: 퀘스트 — (quest_id, kill_count, state) state 0=active,1=completed
    std::vector<std::array<int, 3>> quests;
};

enum class DbReqKind {
    Load,
    Save,
};

struct DbRequest {
    DbReqKind kind = DbReqKind::Load;
    int client_id = -1;         // 응답 디스패치용. 클라가 끊겨도 응답은 도착 — 핸들러에서 검증
    PlayerSnapshot data;         // Load: username만 사용. Save: 전체.
};

struct DbResponse {
    DbReqKind kind = DbReqKind::Load;
    int client_id = -1;
    bool ok = false;             // 백엔드 호출 자체 성공 여부
    bool exists = false;         // Load 시: 기존 데이터 존재 여부 (false = 신규 캐릭)
    PlayerSnapshot data;
};
