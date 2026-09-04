#pragma once

#include "common.hpp"
#include <shared_mutex>
#include <unordered_map>
#include <optional>

namespace edgemon {

class TargetRegistry {
public:
    static TargetRegistry& instance() {
        static TargetRegistry registry;
        return registry;
    }

    bool add(const MonitoredPage& target) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (targets_.contains(target.id)) {
            LOG_WARN("Target {} already exists", target.id);
            return false;
        }
        targets_.emplace(target.id, target);
        LOG_INFO("Target {} registered: {}", target.id, target.url);
        return true;
    }

    bool remove(const std::string& id) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (auto it = targets_.find(id); it != targets_.end()) {
            LOG_INFO("Target {} removed", id);
            targets_.erase(it);
            return true;
        }
        return false;
    }

    std::optional<MonitoredPage> get(const std::string& id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (auto it = targets_.find(id); it != targets_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<MonitoredPage> getByUrl(const std::string& url) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const auto& [id, target] : targets_) {
            if (target.url == url) return target;
        }
        return std::nullopt;
    }

    std::optional<MonitoredPage> getByProcessId(int pid) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const auto& [id, target] : targets_) {
            if (target.edgeProcessId == pid) return target;
        }
        return std::nullopt;
    }

    std::vector<MonitoredPage> getAll() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<MonitoredPage> result;
        result.reserve(targets_.size());
        for (const auto& [id, target] : targets_) {
            result.push_back(target);
        }
        return result;
    }

    std::vector<MonitoredPage> getEnabled() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<MonitoredPage> result;
        for (const auto& [id, target] : targets_) {
            if (target.enabled) result.push_back(target);
        }
        return result;
    }

    bool update(const MonitoredPage& target) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (auto it = targets_.find(target.id); it != targets_.end()) {
            it->second = target;
            return true;
        }
        return false;
    }

    bool setStatus(const std::string& id, TargetStatus status) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (auto it = targets_.find(id); it != targets_.end()) {
            it->second.lastActivity = Clock::now();
            LOG_DEBUG("Target {} status: {}", id, static_cast<int>(status));
            return true;
        }
        return false;
    }

    size_t count() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return targets_.size();
    }

    bool exists(const std::string& id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return targets_.contains(id);
    }

    void clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        targets_.clear();
        LOG_INFO("Target registry cleared");
    }

private:
    TargetRegistry() = default;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, MonitoredPage> targets_;
};

}
