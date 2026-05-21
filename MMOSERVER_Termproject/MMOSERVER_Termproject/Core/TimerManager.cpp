#include "TimerManager.h"

using namespace std::chrono;

void TimerManager::Start(FireCallback cb) {
    callback_ = std::move(cb);
    running_ = true;
    dispatcher_ = std::thread(&TimerManager::DispatcherLoop, this);
}

void TimerManager::Stop() {
    running_ = false;
    cv_.notify_all();
    if (dispatcher_.joinable()) dispatcher_.join();
}

void TimerManager::Schedule(int entity_id, TimerEventKind kind, int delay_ms) {
    TimerEvent ev;
    ev.wakeup = steady_clock::now() + milliseconds(delay_ms);
    ev.entity_id = entity_id;
    ev.kind = kind;
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push(ev);
    }
    cv_.notify_one();
}

size_t TimerManager::QueueSize() {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

void TimerManager::DispatcherLoop() {
    while (running_) {
        TimerEvent fire_ev;
        bool has_event = false;
        {
            std::unique_lock<std::mutex> lock(mu_);

            // 빈 큐: 새 이벤트가 들어오거나 stop 신호가 올 때까지 무한 대기
            if (queue_.empty()) {
                cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
                if (!running_) return;
            }

            // 가장 빠른 만기까지 대기. 도중에 더 빠른 이벤트가 push되면 predicate가 true가 되어 깨어남
            auto next_wakeup = queue_.top().wakeup;
            cv_.wait_until(lock, next_wakeup, [this, next_wakeup] {
                return !running_ || queue_.empty() || queue_.top().wakeup < next_wakeup;
            });

            if (!running_) return;
            if (queue_.empty()) continue;

            // 만기가 지났으면 pop해서 콜백 대상에 추가. 아직이면 다음 라운드에서 다시 wait
            auto now = steady_clock::now();
            if (queue_.top().wakeup <= now) {
                fire_ev = queue_.top();
                queue_.pop();
                has_event = true;
            }
        }
        if (has_event) callback_(fire_ev);
    }
}
