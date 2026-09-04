#pragma once

#include "common.hpp"
#include "CDPClient.hpp"
#include "TargetRegistry.hpp"
#include <memory>
#include <atomic>
#include <functional>

namespace edgemon {

class PageSession : public std::enable_shared_from_this<PageSession> {
public:
    PageSession(std::string targetId, int debugPort, std::shared_ptr<ThreadPool> pool);
    ~PageSession();

    void start();
    void stop();
    bool isRunning() const { return running_; }

    void setTargetId(const std::string& id) { targetId_ = id; }
    std::string getTargetId() const { return targetId_; }
    int getDebugPort() const { return debugPort_; }

    std::string extractDOMText();
    CapturedFrame captureVisual();

    using DOMCallback = std::function<void(const std::string&)>;
    using VisualCallback = std::function<void(const CapturedFrame&)>;
    using ChangeCallback = std::function<void(const std::string&, bool)>;

    void onDOMChange(DOMCallback callback);
    void onVisualCapture(VisualCallback callback);
    void onContentChange(ChangeCallback callback);

    void setInterval(std::chrono::milliseconds interval);
    std::chrono::milliseconds getInterval() const;

private:
    void monitorLoop();
    void setupEventHandlers();

    std::string targetId_;
    int debugPort_;
    std::shared_ptr<ThreadPool> pool_;
    std::shared_ptr<CDPClient> client_;
    
    std::atomic<bool> running_;
    std::jthread monitorThread_;
    std::chrono::milliseconds interval_;
    
    DOMCallback domCallback_;
    VisualCallback visualCallback_;
    ChangeCallback changeCallback_;
    
    std::string lastDOMHash_;
    TimePoint lastCapture_;
};

}
