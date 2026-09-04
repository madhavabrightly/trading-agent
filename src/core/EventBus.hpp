#pragma once

#include "common.hpp"
#include "logger.hpp"
#include "core/Types.hpp"
#include "capture/BrowserCapture.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace edgemon {

enum class EventType {
    TargetAdded,
    TargetRemoved,
    TargetStatusChanged,
    TargetConnected,
    TargetDisconnected,
    FrameCaptured,
    TextExtracted,
    ChangeDetected,
    EmbeddingGenerated,
    Error
};

struct Event {
    EventType type;
    TimePoint timestamp;
    std::string targetId;
    std::variant<
        MonitoredTarget,
        CapturedFrame,
        std::string
    > data;
};

class EventBus {
public:
    using EventCallback = std::function<void(const Event&)>;

    static EventBus& instance() {
        static EventBus bus;
        return bus;
    }

    size_t subscribe(EventType type, EventCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto id = nextId_++;
        listeners_[type].push_back(Listener{id, std::move(callback)});
        LOG_DEBUG("Subscribed to event type {}, listener {}", 
                  static_cast<int>(type), id);
        return id;
    }

    void unsubscribe(EventType type, size_t listenerId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = listeners_.find(type);
        if (it != listeners_.end()) {
            auto& vec = it->second;
            vec.erase(
                std::remove_if(vec.begin(), vec.end(),
                    [listenerId](const auto& p) { return p.id == listenerId; }),
                vec.end()
            );
            LOG_DEBUG("Unsubscribed listener {} from event type {}", 
                      listenerId, static_cast<int>(type));
        }
    }

    void publish(const Event& event) {
        std::vector<EventCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = listeners_.find(event.type);
            if (it != listeners_.end()) {
                for (const auto& [id, cb] : it->second) {
                    callbacks.push_back(cb);
                }
            }
        }
        for (const auto& cb : callbacks) {
            try {
                cb(event);
            } catch (const std::exception& e) {
                LOG_ERROR("Event callback exception: {}", e.what());
            }
        }
    }

    template<typename T>
    void publish(EventType type, const std::string& targetId, const T& data) {
        Event event;
        event.type = type;
        event.timestamp = Clock::now();
        event.targetId = targetId;
        event.data = data;
        publish(event);
    }

private:
    EventBus() : nextId_(0) {}

    struct Listener {
        size_t id;
        EventCallback callback;
    };

    std::map<EventType, std::vector<Listener>> listeners_;
    std::mutex mutex_;
    size_t nextId_;
};

}
