#pragma once

#include "Entity.h"

enum class NpcType : unsigned char {
    Peace,  // 피격 전까지 비공격
    Agro    // 11x11 시야에 플레이어 진입 시 공격
};

enum class NpcMoveMode : unsigned char {
    Fixed,    // 스폰 위치 고정
    Roaming   // 스폰 중심 20x20 내 랜덤 1칸 이동
};

enum class NpcFsmState : unsigned char {
    Idle,
    Roaming,
    Chasing,
    Attacking,
    Returning,
    Dead
};

// Entity 상속. AI/이동 로직은 Stage 4에서 구현.
// active 플래그: 시야에 플레이어가 없으면 false → 타이머 큐에서 제외 (Lazy AI)
struct NPC : public Entity {
    NpcType type;
    NpcMoveMode move_mode;
    NpcFsmState state;
    short spawn_x;
    short spawn_y;
    int target_id;     // Agro 상태에서 추적 중인 플레이어 ID, 없으면 -1
    bool active;       // 현재 시야에 플레이어가 있어 AI 타이머 활성 상태인지

    NPC()
        : Entity()
        , type(NpcType::Peace)
        , move_mode(NpcMoveMode::Fixed)
        , state(NpcFsmState::Idle)
        , spawn_x(-1)
        , spawn_y(-1)
        , target_id(-1)
        , active(false)
    {}
};
