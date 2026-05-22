#pragma once

#include <unordered_set>
#include <mutex>
#include <atomic>

// Player와 NPC의 공통 데이터 베이스.
// view_lock은 view_list 변경 시 보호. 두 Entity의 view_list를 동시에 변경할 때는
// 데드락 방지를 위해 항상 (min id, max id) 순서로 락 획득.
struct Entity {
    int id;
    short x, y;
    std::unordered_set<int> view_list;
    std::mutex view_lock;

    // 직전 이동 시각(ms, steady_clock). IOCP 환경에서 동일 세션을 여러 worker가
    // 동시에 처리할 가능성이 0이 아니므로 atomic으로 보호.
    std::atomic<long long> last_move_ms;

    // 마지막 이동 방향. 0=Down, 1=Left, 2=Right, 3=Up. 공격 모션의 방향 결정용.
    std::atomic<unsigned char> direction;

    Entity() : id(-1), x(-1), y(-1), last_move_ms(0), direction(0) {}
    virtual ~Entity() = default;
};
