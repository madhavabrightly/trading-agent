#include "trading/core/event/TradingEventBus.hpp"
#include "logger.hpp"

#include <chrono>
#include <thread>

namespace trading {

namespace {
uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}
} // namespace

uint64_t TradingEventBus::nextSequence() {
    return sequence_.fetch_add(1) + 1;
}

size_t TradingEventBus::subscribe(EventType type, Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t id = sequence_.fetch_add(1) + 1;
    listeners_[type].push_back(Listener{id, std::move(handler)});
    return id;
}

void TradingEventBus::unsubscribe(EventType type, size_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = listeners_.find(type);
    if (it == listeners_.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [id](const Listener& l) { return l.id == id; }),
              vec.end());
}

void TradingEventBus::emit(EventType type, const std::string& source,
                           const std::string& symbol,
                           const std::string& payloadJson) {
    BusEvent e;
    e.type = type;
    e.source = source;
    e.symbol = symbol;
    e.payload = payloadJson;
    publishAsync(e);
}

void TradingEventBus::publishAsync(const BusEvent& event) {
    BusEvent e = event;
    e.timestampNs = nowNs();
    e.sequence = nextSequence();
    published_.fetch_add(1);

    std::lock_guard<std::mutex> lock(mutex_);
    switch (e.priority()) {
        case 0: {  // HIGH
            if (high_.size() >= kHighCapacity) {
                high_.pop_front();
                droppedHigh_.fetch_add(1);
            }
            high_.push_back(std::move(e));
            break;
        }
        case 1: {  // MEDIUM
            if (medium_.size() >= kMediumCapacity) {
                medium_.pop_front();
                droppedMedium_.fetch_add(1);
            }
            medium_.push_back(std::move(e));
            break;
        }
        default: {  // LOW
            if (low_.size() >= kLowCapacity) {
                low_.pop_front();
                droppedLow_.fetch_add(1);
            }
            low_.push_back(std::move(e));
            break;
        }
    }
    cv_.notify_one();
}

void TradingEventBus::publishSync(const BusEvent& event) {
    BusEvent e = event;
    e.timestampNs = nowNs();
    e.sequence = nextSequence();
    published_.fetch_add(1);

    // Deliver to handlers for this type (outside the lock).
    std::vector<Handler> handlers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = listeners_.find(e.type);
        if (it != listeners_.end())
            for (const auto& l : it->second) handlers.push_back(l.handler);
    }
    for (auto& h : handlers) {
        try {
            h(e);
            delivered_.fetch_add(1);
        } catch (const std::exception& ex) {
            LOG_ERROR("TradingEventBus: handler exception: {}", ex.what());
        }
    }
}

bool TradingEventBus::waitForEvent(std::unique_lock<std::mutex>& lock) {
    cv_.wait(lock, [this] {
        return !running_.load() || !high_.empty() || !medium_.empty() ||
               !low_.empty();
    });
    return running_.load();
}

void TradingEventBus::drain() {
    // Pull a batch under the lock, then invoke handlers outside it.
    std::vector<BusEvent> batch;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!waitForEvent(lock)) return;
        // Drain HIGH first, then MEDIUM, then LOW (one batch each pass).
        auto take = [this, &batch](std::deque<BusEvent>& q, size_t maxTake) {
            size_t n = std::min(q.size(), maxTake);
            for (size_t i = 0; i < n; ++i) {
                batch.push_back(std::move(q.front()));
                q.pop_front();
            }
        };
        take(high_, 64);
        take(medium_, 32);
        take(low_, 16);
    }
    for (const auto& e : batch) {
        std::vector<Handler> handlers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = listeners_.find(e.type);
            if (it != listeners_.end())
                for (const auto& l : it->second) handlers.push_back(l.handler);
        }
        for (auto& h : handlers) {
            try {
                h(e);
                delivered_.fetch_add(1);
            } catch (const std::exception& ex) {
                LOG_ERROR("TradingEventBus: handler exception: {}", ex.what());
            }
        }
    }
}

void TradingEventBus::start() {
    bool wasRunning = running_.exchange(true, std::memory_order_acq_rel);
    if (!wasRunning) {
        thread_ = std::thread([this] {
            while (running_.load(std::memory_order_acquire)) {
                drain();
            }
        });
    }
}

void TradingEventBus::stop() {
    bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
    if (wasRunning) {
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
}

TradingEventBus::Stats TradingEventBus::stats() const {
    Stats s;
    s.published = published_.load();
    s.delivered = delivered_.load();
    s.droppedHigh = droppedHigh_.load();
    s.droppedMedium = droppedMedium_.load();
    s.droppedLow = droppedLow_.load();
    std::lock_guard<std::mutex> lock(mutex_);
    s.queueHigh = high_.size();
    s.queueMedium = medium_.size();
    s.queueLow = low_.size();
    return s;
}

} // namespace trading
