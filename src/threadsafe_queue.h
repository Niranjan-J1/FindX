#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <utility>

template <typename T>
class ThreadSafeQueue {
public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || closed_; });

        if (queue_.empty()) {
            return false;
        }

        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_ = false;
};