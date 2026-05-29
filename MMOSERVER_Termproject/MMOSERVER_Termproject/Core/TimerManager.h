#pragma once

#include <winsock2.h>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include "OverlappedTypes.h"

enum class TimerEventKind : unsigned char {
    NpcMove,
    NpcRespawn,
    HpRegen,
    AttackCooldown,
    SkillCooldown,
    PlayerAutoSave,  // Stage 6.3 — 주기적 DB 자동 저장
    GroundItemExpire, // Stage 8 — 바닥 아이템 만료 (entity_id 자리에 drop_id)
    TestPing,    // 검증 테스트용
};

struct TimerEvent {
    std::chrono::steady_clock::time_point wakeup;
    int entity_id;
    TimerEventKind kind;

    // priority_queue를 min-heap으로 쓰기 위해 비교 반전 (top()이 가장 빠른 만기 반환)
    bool operator<(const TimerEvent& o) const {
        return wakeup > o.wakeup;
    }
};

// IOCP에 PostQueuedCompletionStatus로 전달할 timer 전용 OVERLAPPED.
// 첫 두 필드(WSAOVERLAPPED, IO_TYPE)는 OVERLAPPED_EX와 동일 레이아웃으로 시작해야
// worker_thread가 안전하게 type 필드를 읽을 수 있음.
struct TimerOverlapped {
    WSAOVERLAPPED overlapped;
    IO_TYPE type;
    TimerEventKind kind;

    TimerOverlapped() : type(IO_TIMER), kind(TimerEventKind::NpcMove) {
        memset(&overlapped, 0, sizeof(overlapped));
    }
};

// [perf] 샤딩된 타이머.
// 기존 단일 디스패처 스레드 + 단일 mutex/priority_queue는 활성 NPC가 많을 때(초당 수십만
// reschedule) 직렬화 병목이었다. entity_id로 N개 샤드에 분산해 스케줄/디스패치를 병렬화한다.
// 같은 entity는 항상 같은 샤드로 가므로 엔티티별 처리 순서는 보존된다.
// 만기 처리(콜백)는 기존과 동일하게 PostQueuedCompletionStatus로 공유 IOCP에 post한다.
class TimerManager {
public:
    using FireCallback = std::function<void(const TimerEvent&)>;

    void Start(FireCallback cb);
    void Stop();
    void Schedule(int entity_id, TimerEventKind kind, int delay_ms);
    size_t QueueSize();  // 전체 샤드 합계

private:
    static constexpr int kShardCount = 8;

    struct Shard {
        std::priority_queue<TimerEvent> queue;
        std::mutex mu;
        std::condition_variable cv;
        std::thread thread;
    };

    void DispatcherLoop(int shard_index);

    Shard shards_[kShardCount];
    std::atomic<bool> running_{ false };
    FireCallback callback_;
};
