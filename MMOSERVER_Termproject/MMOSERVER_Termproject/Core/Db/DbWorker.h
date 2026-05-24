#pragma once

// Stage 6.3: DB 워커. 단일 스레드가 큐를 폴링해 백엔드(IDbBackend)에 동기 호출.
// 완료 시 게임 스레드는 IOCP를 통해 IO_DB_DONE 이벤트로 응답 받음 — 콜백은 PostQueuedCompletionStatus를 호출.

#include "DbTypes.h"
#include "IDbBackend.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

class DbWorker {
public:
    using OnDone = std::function<void(DbResponse&&)>;

    DbWorker() = default;
    ~DbWorker() { Stop(); }

    DbWorker(const DbWorker&) = delete;
    DbWorker& operator=(const DbWorker&) = delete;

    // backend는 ownership 이전. on_done은 워커 스레드에서 호출됨 (IOCP post용).
    void Start(std::unique_ptr<IDbBackend> backend, OnDone on_done);
    void Stop();

    void EnqueueLoad(int client_id, std::string username);
    void EnqueueSave(int client_id, PlayerSnapshot snap);

private:
    void Loop();

    std::unique_ptr<IDbBackend> backend_;
    OnDone on_done_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<DbRequest> queue_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> running_{ false };
};
