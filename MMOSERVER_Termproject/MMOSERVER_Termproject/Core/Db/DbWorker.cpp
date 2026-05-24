#include "DbWorker.h"

#include <utility>

void DbWorker::Start(std::unique_ptr<IDbBackend> backend, OnDone on_done) {
    if (running_.exchange(true)) return;  // 이미 시작됨
    backend_ = std::move(backend);
    on_done_ = std::move(on_done);
    stop_.store(false);
    thread_ = std::thread([this] { Loop(); });
}

void DbWorker::Stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_.store(true);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    backend_.reset();
    on_done_ = nullptr;
    std::queue<DbRequest> empty;
    std::swap(queue_, empty);
}

void DbWorker::EnqueueLoad(int client_id, std::string username) {
    DbRequest req;
    req.kind = DbReqKind::Load;
    req.client_id = client_id;
    req.data.username = std::move(username);
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push(std::move(req));
    }
    cv_.notify_one();
}

void DbWorker::EnqueueSave(int client_id, PlayerSnapshot snap) {
    DbRequest req;
    req.kind = DbReqKind::Save;
    req.client_id = client_id;
    req.data = std::move(snap);
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push(std::move(req));
    }
    cv_.notify_one();
}

void DbWorker::Loop() {
    while (true) {
        DbRequest req;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_.load() || !queue_.empty(); });
            if (stop_.load() && queue_.empty()) return;
            req = std::move(queue_.front());
            queue_.pop();
        }

        DbResponse resp;
        resp.kind = req.kind;
        resp.client_id = req.client_id;

        if (!backend_) {
            resp.ok = false;
        }
        else if (req.kind == DbReqKind::Load) {
            auto r = backend_->Load(req.data.username, resp.data);
            resp.ok = r.ok;
            resp.exists = r.exists;
            if (resp.data.username.empty()) resp.data.username = req.data.username;
        }
        else {
            // Save: 요청 데이터를 응답에 그대로 실어 보내서 핸들러가 confirm 가능
            resp.data = req.data;
            resp.ok = backend_->Save(req.data);
        }

        if (on_done_) on_done_(std::move(resp));
    }
}
