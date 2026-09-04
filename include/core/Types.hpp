#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <optional>
#include <vector>
#include <variant>

namespace edgemon {

// ============================================================================
// Strong Type Aliases
// ============================================================================

struct TargetId {
    std::string value;
    
    explicit TargetId() = default;
    explicit TargetId(std::string v) : value(std::move(v)) {}
    
    bool empty() const { return value.empty(); }
    bool operator==(const TargetId& other) const { return value == other.value; }
    bool operator!=(const TargetId& other) const { return value != other.value; }
    bool operator<(const TargetId& other) const { return value < other.value; }
};

struct CDPTargetId {
    std::string value;
    
    explicit CDPTargetId() = default;
    explicit CDPTargetId(std::string v) : value(std::move(v)) {}
    
    bool empty() const { return value.empty(); }
    bool operator==(const CDPTargetId& other) const { return value == other.value; }
    bool operator!=(const CDPTargetId& other) const { return value != other.value; }
};

struct PageUrl {
    std::string value;
    
    explicit PageUrl() = default;
    explicit PageUrl(std::string v) : value(std::move(v)) {}
    
    bool empty() const { return value.empty(); }
    bool operator==(const PageUrl& other) const { return value == other.value; }
    bool contains(const std::string& substr) const { 
        return value.find(substr) != std::string::npos; 
    }
};

struct EdgeProcessId {
    uint32_t value;
    
    explicit EdgeProcessId() : value(0) {}
    explicit EdgeProcessId(uint32_t v) : value(v) {}
    
    bool empty() const { return value == 0; }
    bool operator==(const EdgeProcessId& other) const { return value == other.value; }
    bool operator!=(const EdgeProcessId& other) const { return value != other.value; }
    bool operator<(const EdgeProcessId& other) const { return value < other.value; }
};

// ============================================================================
// Target State Machine
// ============================================================================

enum class TargetState {
    Discovered,
    Discovering,
    Connecting,
    Connected,
    Monitoring,
    Navigating,
    Reconnecting,
    Offline,
    Closed,
    Error
};

constexpr const char* TargetStateToString(TargetState state) {
    switch (state) {
        case TargetState::Discovered:    return "Discovered";
        case TargetState::Discovering:   return "Discovering";
        case TargetState::Connecting:   return "Connecting";
        case TargetState::Connected:     return "Connected";
        case TargetState::Monitoring:   return "Monitoring";
        case TargetState::Navigating:   return "Navigating";
        case TargetState::Reconnecting:  return "Reconnecting";
        case TargetState::Offline:       return "Offline";
        case TargetState::Closed:       return "Closed";
        case TargetState::Error:        return "Error";
        default:                        return "Unknown";
    }
}

// ============================================================================
// Target Configuration
// ============================================================================

// How a monitored target's page is located again after Edge restarts / the
// CDP target id changes. The durable identity is the URL RULE, never the CDP
// target id alone (CDP ids are ephemeral across browser restarts).
enum class TargetMatchRule {
    Exact,        // normalized URL equality
    UrlPrefix,    // requested URL is a prefix of the live tab URL
    OriginPath    // scheme + host + path equality (ignores query/fragment)
};

constexpr const char* TargetMatchRuleToString(TargetMatchRule rule) {
    switch (rule) {
        case TargetMatchRule::Exact:     return "exact";
        case TargetMatchRule::UrlPrefix: return "url-prefix";
        case TargetMatchRule::OriginPath: return "origin-path";
        default:                         return "exact";
    }
}

enum class CaptureMode {
    DOM,
    Visual,
    Hybrid
};

enum class ContentSource {
    DOM,
    OCR,
    Unknown
};

struct MonitoringPolicy {
    double domPollRateHz = 1.0;
    double visualCaptureRateHz = 1.0;
    double maxOcrRateHz = 0.5;
    bool adaptive = false;
};

struct MonitoringConfig {
    bool domEnabled = true;
    bool visualEnabled = true;
    bool ocrEnabled = false;
    CaptureMode mode = CaptureMode::Hybrid;
    MonitoringPolicy policy;
};

// ============================================================================
// Core Data Structures
// ============================================================================

struct MonitoredTarget {
    TargetId id;
    CDPTargetId cdpTargetId;
    EdgeProcessId edgeProcessId;
    
    std::string title;
    PageUrl url;
    std::string webSocketUrl;
    TargetMatchRule targetRule = TargetMatchRule::Exact;
    std::string profileHint;   // user-data-dir when safely known (empty = unknown)
    
    bool enabled = true;
    MonitoringConfig config;
    
    uint64_t lastDomHash = 0;
    uint64_t lastVisualHash = 0;
    uint64_t lastChangeTimestamp = 0;
    
    TargetState state = TargetState::Discovered;
    std::string stateMessage;
    
    MonitoringPolicy activePolicy;
};

struct EdgeInstance {
    EdgeProcessId processId;
    std::string userDataDir;
    int debuggingPort = 0;
    std::string browserVersion;
    std::string executablePath;
    bool attached = false;
    
    bool empty() const { return processId.empty(); }
};

struct EdgeTarget {
    CDPTargetId id;
    std::string type;
    std::string title;
    PageUrl url;
    std::string webSocketDebuggerUrl;
    EdgeProcessId processId;
    
    bool isPage() const { return type == "page"; }
    bool isBackgroundPage() const { return type == "background_page"; }
    bool isServiceWorker() const { return type == "service_worker"; }
};

// ============================================================================
// Capture & Frame
// ============================================================================

enum class PixelFormat {
    BGRA,
    RGBA,
    Gray,
    Unknown
};

struct PixelBuffer {
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::Unknown;
    
    size_t size() const { return data.size(); }
    bool empty() const { return data.empty(); }
    void clear() { data.clear(); width = 0; height = 0; }
};

struct Frame {
    TargetId targetId;
    CDPTargetId cdpTargetId;
    uint64_t timestamp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::Unknown;
    std::shared_ptr<PixelBuffer> pixels;
    bool available = false;
    std::string unavailableReason;
};

// ============================================================================
// Extraction
// ============================================================================

struct Observation {
    TargetId targetId;
    uint64_t timestamp = 0;
    ContentSource source = ContentSource::Unknown;
    
    std::string rawText;
    std::string normalizedText;
    std::string structuredText;
    
    float confidence = 0.0f;
    uint64_t contentHash = 0;
    
    struct BoundingBox {
        float x = 0, y = 0, width = 0, height = 0;
    };
    std::vector<BoundingBox> boxes;
};

// OCRResult is defined in extraction/OCRManager.hpp
// TextChunk is defined in extraction/TextProcessor.hpp

// ============================================================================
// Embedding & Vector
// ============================================================================

struct Embedding {
    std::string id;
    TargetId targetId;
    std::string content;
    std::vector<float> vector;
    uint64_t timestamp = 0;
    PageUrl url;
    ContentSource source = ContentSource::Unknown;
    float confidence = 0.0f;
};

struct VectorSearchResult {
    std::string id;
    std::string content;
    float score = 0.0f;
    TargetId targetId;
    uint64_t timestamp = 0;
};

struct VectorQuery {
    std::string text;
    std::optional<TargetId> targetFilter;
    std::optional<uint64_t> timeStart;
    std::optional<uint64_t> timeEnd;
    std::optional<ContentSource> sourceFilter;
    size_t limit = 10;
};

// ============================================================================
// Error Handling
// ============================================================================

enum class ErrorCategory {
    Discovery,
    Connection,
    CDP,
    Capture,
    OCR,
    Extraction,
    Embedding,
    Storage,
    Unknown
};

struct ErrorInfo {
    ErrorCategory category;
    TargetId targetId;
    std::string component;
    std::string message;
    bool recoverable = true;
    std::string retryPolicy;
};

} // namespace edgemon

// ============================================================================
// Hash Support
// ============================================================================

namespace std {
    template<>
    struct hash<edgemon::TargetId> {
        size_t operator()(const edgemon::TargetId& id) const {
            return hash<string>{}(id.value);
        }
    };
    
    template<>
    struct hash<edgemon::CDPTargetId> {
        size_t operator()(const edgemon::CDPTargetId& id) const {
            return hash<string>{}(id.value);
        }
    };
    
    template<>
    struct hash<edgemon::EdgeProcessId> {
        size_t operator()(const edgemon::EdgeProcessId& id) const {
            return hash<uint32_t>{}(id.value);
        }
    };
}
