#pragma once

#include "common.hpp"
#include "EventBus.hpp"
#include "TargetRegistry.hpp"
#include "Scheduler.hpp"
#include <memory>
#include <vector>

namespace edgemon {

class TargetManager {
public:
    explicit TargetManager(std::shared_ptr<ThreadPool> pool)
        : pool_(pool)
        , scheduler_(std::make_shared<Scheduler>(pool)) {}

    ~TargetManager() { shutdown(); }

    bool addTarget(const MonitoredPage& target) {
        if (!TargetRegistry::instance().add(target)) {
            return false;
        }

        EventBus::instance().publish(EventType::TargetAdded, target.id, target);
        return true;
    }

    bool removeTarget(const std::string& id) {
        if (!TargetRegistry::instance().exists(id)) {
            return false;
        }

        scheduler_->removeTasks(id);
        TargetRegistry::instance().remove(id);
        EventBus::instance().publish(EventType::TargetRemoved, id, std::string("removed"));
        return true;
    }

    bool enableTarget(const std::string& id) {
        auto target = TargetRegistry::instance().get(id);
        if (!target) return false;
        target->enabled = true;
        TargetRegistry::instance().update(*target);
        EventBus::instance().publish(EventType::TargetStatusChanged, id, *target);
        return true;
    }

    bool disableTarget(const std::string& id) {
        auto target = TargetRegistry::instance().get(id);
        if (!target) return false;
        target->enabled = false;
        target->lastActivity = Clock::now();
        TargetRegistry::instance().update(*target);
        scheduler_->removeTasks(id);
        EventBus::instance().publish(EventType::TargetStatusChanged, id, *target);
        return true;
    }

    void setTargetInterval(const std::string& id, std::chrono::milliseconds interval) {
        scheduler_->setTargetInterval(id, interval);
    }

    void scheduleTarget(const std::string& id, 
                        std::chrono::milliseconds interval,
                        std::function<void(const std::string&)> callback) {
        scheduler_->removeTasks(id);
        scheduler_->addTask(id, interval, std::move(callback));
    }

    std::vector<MonitoredPage> getTargets() const {
        return TargetRegistry::instance().getAll();
    }

    std::vector<MonitoredPage> getEnabledTargets() const {
        return TargetRegistry::instance().getEnabled();
    }

    std::optional<MonitoredPage> getTarget(const std::string& id) const {
        return TargetRegistry::instance().get(id);
    }

    void start() {
        scheduler_->start();
        LOG_INFO("TargetManager started with {} targets", 
                 TargetRegistry::instance().count());
    }

    void shutdown() {
        scheduler_->stop();
        LOG_INFO("TargetManager shutdown complete");
    }

    size_t targetCount() const {
        return TargetRegistry::instance().count();
    }

private:
    std::shared_ptr<ThreadPool> pool_;
    std::shared_ptr<Scheduler> scheduler_;
};

}
