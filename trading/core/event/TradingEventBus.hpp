#pragma once

// trading/core/event/TradingEventBus.hpp — the internal priority event bus
// (spec §12). Thread-safe, bounded per-priority queues, drop-new on overflow
// with counters, per-event ns timestamps and monotonic sequence numbers.
// Logging/UI must never block the trading path — subscribers run on the
// publisher's thread by default; heavy subscribers should dispatch themselves.

#include "trading/core/Types.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trading {

struct BusEvent {
    EventType type = EventType::SYSTEM;
    uint64_t timestampNs = 0;   // publish time, UTC epoch ns
    uint64_t sequence = 0;      // monotonic global
    std::string source;         // component that published
    std::string symbol;         // when symbol-scoped
    std::string payload;        // small JSON payload (never secrets)

    int priority() const { return static_cast<int>(priorityOf(type)); }
};

class TradingEventBus {
public:
    using Handler = std::function<void(const BusEvent&)>;

    struct Stats {
        uint64_t published = 0;
        uint64_t delivered = 0;
        uint64_t droppedHigh = 0;
        uint64_t droppedMedium = 0;
        uint64_t droppedLow = 0;
        size_t queueHigh = 0;
        size_t queueMedium = 0;
        size_t queueLow = 0;
    };

    static TradingEventBus& instance() {
        static TradingEventBus bus;
        return bus;
    }

    // Subscribe to one event type. Returns listener id for unsubscribe.
    size_t subscribe(EventType type, Handler handler);

    void unsubscribe(EventType type, size_t id);

    // Async publish: enqueue onto the priority queue (drop-new when full).
    void publishAsync(const BusEvent& event);

    // Synchronous publish: deliver to handlers immediately (still assigns
    // sequence/timestamp). Used for high-priority control events.
    void publishSync(const BusEvent& event);

    // Convenience: build + publish a JSON payload event.
    void emit(EventType type, const std::string& source,
              const std::string& symbol, const std::string& payloadJson);

    // Runs one dispatch loop iteration (drains HIGH then MEDIUM then LOW).
    // Called by the dedicated bus thread.
    void drain();

    // Starts the internal dispatch thread.
    void start();
    void stop();

    Stats stats() const;

    // Blocking wait until an event is available (for the bus thread).
    // Returns false on stop.
    bool waitForEvent(std::unique_lock<std::mutex>& lock);

private:
    TradingEventBus() = default;
    ~TradingEventBus() { stop(); }
    TradingEventBus(const TradingEventBus&) = delete;
    TradingEventBus& operator=(const TradingEventBus&) = delete;

    uint64_t nextSequence();

    static constexpr size_t kHighCapacity = 4096;
    static constexpr size_t kMediumCapacity = 16384;
    static constexpr size_t kLowCapacity = 65536;

    struct Listener {
        size_t id;
        Handler handler;
    };

    std::deque<BusEvent> high_;
    std::deque<BusEvent> medium_;
    std::deque<BusEvent> low_;

    std::map<EventType, std::vector<Listener>> listeners_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::atomic<uint64_t> sequence_{0};
    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> delivered_{0};
    std::atomic<uint64_t> droppedHigh_{0};
    std::atomic<uint64_t> droppedMedium_{0};
    std::atomic<uint64_t> droppedLow_{0};

    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace trading
