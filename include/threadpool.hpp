#pragma once

#include "logger.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <vector>
#include <optional>
#include <atomic>

namespace edgemon {

template<typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t maxSize = 0) 
        : maxSize_(maxSize), closed_(false) {}

    ~BlockingQueue() { close(); }

    void push(T item) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (closed_) return;
            if (maxSize_ > 0) {
                notFull_.wait(lock, [this] { 
                    return queue_.size() < maxSize_; 
                });
            }
            queue_.push(std::move(item));
        }
        notEmpty_.notify_one();
    }

    std::optional<T> pop() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                if (closed_) return std::nullopt;
                return std::nullopt;
            }
            T item = std::move(queue_.front());
            queue_.pop();
            return item;
        }
        notFull_.notify_one();
    }

    std::optional<T> popFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            if (closed_) return std::nullopt;
            if (!notEmpty_.wait_for(lock, timeout, [this] { 
                return !queue_.empty() || closed_; 
            })) {
                return std::nullopt;
            }
            if (queue_.empty()) return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop();
        notFull_.notify_one();
        return item;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    size_t capacity() const { return maxSize_; }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
        notFull_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
    std::queue<T> queue_;
    size_t maxSize_;
    std::atomic<bool> closed_;
};

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stopped_(false) {
        workers_.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
        LOG_INFO("ThreadPool started with {} workers", numThreads);
    }

    ~ThreadPool() {
        stop();
    }

    template<typename F>
    void submit(F&& task) {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (stopped_) return;
            tasks_.push(std::forward<F>(task));
        }
        taskCondition_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopped_ = true;
        }
        taskCondition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        LOG_INFO("ThreadPool stopped");
    }

    size_t size() const { return workers_.size(); }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                taskCondition_.wait(lock, [this] { 
                    return stopped_ || !tasks_.empty(); 
                });
                if (stopped_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            try {
                task();
            } catch (const std::exception& e) {
                LOG_ERROR("Task exception: {}", e.what());
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queueMutex_;
    std::condition_variable taskCondition_;
    std::atomic<bool> stopped_;
};

}
