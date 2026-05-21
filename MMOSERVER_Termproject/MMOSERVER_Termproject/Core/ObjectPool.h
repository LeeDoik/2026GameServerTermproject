#pragma once

#include <mutex>
#include <vector>
#include <utility>

// 락 기반 단순 free-list 풀.
// Acquire는 free-list가 비어 있으면 새로 할당. Release는 객체 상태를 초기화하지 않으므로
// 호출자가 다음 Acquire 후 필요한 필드를 직접 세팅해야 함.
template <typename T>
class ObjectPool {
public:
    ObjectPool() = default;
    ~ObjectPool() {
        for (T* p : free_list_) delete p;
    }
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* Acquire(Args&&... args) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!free_list_.empty()) {
                T* p = free_list_.back();
                free_list_.pop_back();
                return p;
            }
        }
        return new T(std::forward<Args>(args)...);
    }

    void Release(T* p) {
        if (!p) return;
        std::lock_guard<std::mutex> lock(mu_);
        free_list_.push_back(p);
    }

    size_t FreeCount() {
        std::lock_guard<std::mutex> lock(mu_);
        return free_list_.size();
    }

private:
    std::mutex mu_;
    std::vector<T*> free_list_;
};
