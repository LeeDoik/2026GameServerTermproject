#include "TimerManager.h"

using namespace std::chrono;

void TimerManager::Start(FireCallback cb) {
    callback_ = std::move(cb);
    running_ = true;
    for (int i = 0; i < kShardCount; ++i) {
        shards_[i].thread = std::thread(&TimerManager::DispatcherLoop, this, i);
    }
}

void TimerManager::Stop() {
    running_ = false;
    for (int i = 0; i < kShardCount; ++i) shards_[i].cv.notify_all();
    for (int i = 0; i < kShardCount; ++i) {
        if (shards_[i].thread.joinable()) shards_[i].thread.join();
    }
}

void TimerManager::Schedule(int entity_id, TimerEventKind kind, int delay_ms) {
    TimerEvent ev;
    ev.wakeup = steady_clock::now() + milliseconds(delay_ms);
    ev.entity_id = entity_id;
    ev.kind = kind;

    // 같은 entity는 항상 같은 샤드 → 엔티티별 순서 보존, 크로스 샤드 경쟁 없음
    Shard& s = shards_[static_cast<unsigned>(entity_id) % kShardCount];
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.queue.push(ev);
    }
    s.cv.notify_one();
}

size_t TimerManager::QueueSize() {
    size_t total = 0;
    for (int i = 0; i < kShardCount; ++i) {
        std::lock_guard<std::mutex> lock(shards_[i].mu);
        total += shards_[i].queue.size();
    }
    return total;
}

void TimerManager::DispatcherLoop(int shard_index) {
    Shard& s = shards_[shard_index];
    while (running_) {
        TimerEvent fire_ev;
        bool has_event = false;
        {
            std::unique_lock<std::mutex> lock(s.mu);

            // 빈 큐: 새 이벤트가 들어오거나 stop 신호가 올 때까지 무한 대기
            if (s.queue.empty()) {
                s.cv.wait(lock, [&] { return !s.queue.empty() || !running_; });
                if (!running_) return;
            }

            // 가장 빠른 만기까지 대기. 도중에 더 빠른 이벤트가 push되면 predicate가 true가 되어 깨어남
            auto next_wakeup = s.queue.top().wakeup;
            s.cv.wait_until(lock, next_wakeup, [&] {
                return !running_ || s.queue.empty() || s.queue.top().wakeup < next_wakeup;
            });

            if (!running_) return;
            if (s.queue.empty()) continue;

            // 만기가 지났으면 pop해서 콜백 대상에 추가. 아직이면 다음 라운드에서 다시 wait
            auto now = steady_clock::now();
            if (s.queue.top().wakeup <= now) {
                fire_ev = s.queue.top();
                s.queue.pop();
                has_event = true;
            }
        }
        if (has_event) callback_(fire_ev);
    }
}
