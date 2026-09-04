#pragma once

#include "common.hpp"
#include "threadpool.hpp"
#include "capture/BrowserCapture.hpp"
#include <memory>

namespace edgemon {

class FrameBuffer {
public:
    explicit FrameBuffer(size_t maxSize = DEFAULT_FRAME_QUEUE_SIZE);
    ~FrameBuffer();

    bool push(const CapturedFrame& frame);
    std::optional<CapturedFrame> pop();
    std::optional<CapturedFrame> popFor(std::chrono::milliseconds timeout);

    size_t size() const;
    bool empty() const;
    void clear();

    size_t droppedFrames() const { return dropped_; }

private:
    BlockingQueue<CapturedFrame> queue_;
    std::atomic<size_t> dropped_;
};

}
