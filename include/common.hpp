#pragma once

// Include the full type system
#include "core/Types.hpp"
#include "logger.hpp"

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <optional>
#include <variant>
#include <memory>
#include <functional>

namespace edgemon {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

constexpr size_t DEFAULT_FRAME_QUEUE_SIZE = 32;
constexpr size_t DEFAULT_OCR_QUEUE_SIZE = 16;
constexpr size_t DEFAULT_EMBEDDING_DIM = 768;
constexpr auto DEFAULT_CAPTURE_INTERVAL = std::chrono::seconds(1);
constexpr auto DEFAULT_OCR_TIMEOUT = std::chrono::seconds(5);

// Backwards compatibility aliases
using MonitoredPage = MonitoredTarget;

template<typename T>
using Callback = std::function<void(const T&)>;

template<typename T>
using AsyncCallback = std::function<void(std::shared_ptr<T>)>;

using CancellationToken = std::atomic<bool>;

}
