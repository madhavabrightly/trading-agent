#pragma once

#include "common.hpp"
#include "threadpool.hpp"
#include <atomic>
#include <memory>
#include <vector>
#include <chrono>

namespace edgemon {

struct ScheduledTask {
    std::string targetId;
    std::chrono::milliseconds interval;
    std::function<void(const std::string&)> callback;
    std::chrono::steady_clock::time_point nextRun;
    bool enabled = true;
};

class Scheduler {
public:
    explicit Scheduler(std::shared_ptr<ThreadPool> pool, size_t numWorkers = 4)
        : pool_(std::move(pool))
        , running_(false)
        , numWorkers_(numWorkers) {}

    ~Scheduler() { stop(); }

    void addTask(const std::string& targetId, 
                 std::chrono::milliseconds interval,
                 std::function<void(const std::string&)> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        ScheduledTask task;
        task.targetId = targetId;
        task.interval = interval;
        task.callback = std::move(callback);
        task.nextRun = Clock::now() + interval;
        task.enabled = true;
        tasks_.push_back(std::move(task));
        LOG_DEBUG("Scheduled task for target {}, interval {}ms", targetId, interval.count());
    }

    void removeTasks(const std::string& targetId) {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.erase(
            std::remove_if(tasks_.begin(), tasks_.end(),
                [&targetId](const auto& t) { return t.targetId == targetId; }),
            tasks_.end()
        );
        LOG_DEBUG("Removed tasks for target {}", targetId);
    }

    void setTargetInterval(const std::string& targetId, std::chrono::milliseconds interval) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& task : tasks_) {
            if (task.targetId == targetId) {
                task.interval = interval;
            }
        }
    }

    void start() {
        if (running_.exchange(true)) return;
        worker_ = std::jthread([this](std::stop_token token) { runLoop(token); });
        LOG_INFO("Scheduler started");
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
        LOG_INFO("Scheduler stopped");
    }

    size_t taskCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

private:
    void runLoop(std::stop_token token) {
        while (!token.stop_requested()) {
            auto now = Clock::now();
            std::vector<std::function<void(const std::string&)>> toRun;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto& task : tasks_) {
                    if (task.enabled && task.nextRun <= now) {
                        toRun.push_back(task.callback);
                        task.nextRun = now + task.interval;
                    }
                }
            }

            for (const auto& cb : toRun) {
                if (token.stop_requested()) break;
                pool_->submit([&cb, this]() {
                    cb(currentTargetId_);
                });
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    std::shared_ptr<ThreadPool> pool_;
    std::atomic<bool> running_;
    std::jthread worker_;
    std::mutex mutex_;
    std::vector<ScheduledTask> tasks_;
    std::string currentTargetId_;
    size_t numWorkers_;
};

}
