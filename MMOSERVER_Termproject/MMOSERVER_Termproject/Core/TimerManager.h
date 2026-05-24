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

class TimerManager {
public:
    using FireCallback = std::function<void(const TimerEvent&)>;

    void Start(FireCallback cb);
    void Stop();
    void Schedule(int entity_id, TimerEventKind kind, int delay_ms);
    size_t QueueSize();

private:
    void DispatcherLoop();

    std::priority_queue<TimerEvent> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::thread dispatcher_;
    std::atomic<bool> running_{ false };
    FireCallback callback_;
};
