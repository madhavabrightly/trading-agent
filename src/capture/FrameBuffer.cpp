#include "capture/FrameBuffer.hpp"
#include "logger.hpp"

namespace edgemon {

FrameBuffer::FrameBuffer(size_t maxSize)
    : queue_(maxSize)
    , dropped_(0) {
}

FrameBuffer::~FrameBuffer() {
    clear();
}

bool FrameBuffer::push(const CapturedFrame& frame) {
    if (queue_.size() >= queue_.capacity()) {
        dropped_++;
        LOG_WARN("Frame dropped (buffer full), total dropped: {}", dropped_.load());
        return false;
    }
    queue_.push(frame);
    return true;
}

std::optional<CapturedFrame> FrameBuffer::pop() {
    return queue_.pop();
}

std::optional<CapturedFrame> FrameBuffer::popFor(std::chrono::milliseconds timeout) {
    return queue_.popFor(timeout);
}

size_t FrameBuffer::size() const {
    return queue_.size();
}

bool FrameBuffer::empty() const {
    return queue_.empty();
}

void FrameBuffer::clear() {
    queue_.clear();
    LOG_DEBUG("FrameBuffer cleared, total dropped frames: {}", dropped_.load());
}

}
