#pragma once

#include <unordered_set>
#include <mutex>

// Player와 NPC의 공통 데이터 베이스.
// view_lock은 view_list 변경 시 보호. 두 Entity의 view_list를 동시에 변경할 때는
// 데드락 방지를 위해 항상 (min id, max id) 순서로 락 획득.
struct Entity {
    int id;
    short x, y;
    std::unordered_set<int> view_list;
    std::mutex view_lock;

    Entity() : id(-1), x(-1), y(-1) {}
    virtual ~Entity() = default;
};
