#pragma once

#include "Entity.h"
#include <atomic>

enum class NpcType : unsigned char {
    Peace,  // 피격 전까지 비공격 (Stage 5에서 Agro 전환 구현)
    Agro,   // 11x11 시야에 플레이어 진입 시 추적
    Boss    // 특수 패턴: 광역 공격 + 2단계 분노 + 채팅, 5분 리스폰
};

enum class NpcMoveMode : unsigned char {
    Fixed,    // 스폰 위치 고정
    Roaming   // area 사각형 내 랜덤 1칸 이동
};

enum class NpcFsmState : unsigned char {
    Idle,
    Roaming,
    Chasing,
    Attacking,
    Returning,
    Dead
};

// Entity 상속. 시야/이동 동기화 로직은 Core/World.cpp
// active: 시야에 플레이어가 있어 AI 타이머가 등록되어 있는지. Lazy AI의 핵심 플래그
struct NPC : public Entity {
    NpcType type;
    NpcMoveMode move_mode;
    NpcFsmState state;

    short spawn_x;
    short spawn_y;
    // roaming 가능한 사각형 영역 (스폰 시 결정, 이후 불변)
    short area_x1;
    short area_y1;
    short area_x2;
    short area_y2;

    int target_id;             // Agro 상태에서 추적 중인 플레이어 ID, 없으면 -1
    std::atomic<bool> active;  // worker 여러 명이 동시에 들여다보므로 atomic

    // 표시/전투 (Stage 5에서 본격 사용)
    int visual_id;
    int hp;
    int max_hp;
    unsigned char level;
    char name[20];  // protocol MAX_NAME_LEN
    std::atomic<long long> last_attack_ms;  // NPC 공격 쿨타임용 (Stage 5)

    // Stage 7.2 보스 전용: 분노 2단계 진입 여부 + 틱 카운터
    std::atomic<int> boss_tick_count;       // 보스가 몇 번째 tick인지 (AoE/채팅 주기용)

    NPC()
        : Entity()
        , type(NpcType::Peace)
        , move_mode(NpcMoveMode::Fixed)
        , state(NpcFsmState::Idle)
        , spawn_x(-1)
        , spawn_y(-1)
        , area_x1(0)
        , area_y1(0)
        , area_x2(0)
        , area_y2(0)
        , target_id(-1)
        , active(false)
        , visual_id(0)
        , hp(100)
        , max_hp(100)
        , level(1)
        , last_attack_ms(0)
        , boss_tick_count(0)
    {
        name[0] = '\0';
    }
};
