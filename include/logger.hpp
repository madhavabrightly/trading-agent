#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <iomanip>
#include <functional>

namespace edgemon {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5
};

class Logger {
public:
    // Optional external sink: receives every formatted log line (no timestamp
    // decorations beyond the raw text). The GUI terminal subscribes here.
    using Sink = std::function<void(const std::string& line)>;

    static void addSink(Sink sink) {
        std::lock_guard<std::mutex> lock(get().sinkMutex_);
        get().sinks_.push_back(std::move(sink));
    }

    static void initialize(const std::string& appName = "EdgeMonitor",
                          const std::string& logPath = "logs/") {
        get().appName_ = appName;
        get().logPath_ = logPath;
        get().level_ = LogLevel::Debug;

        std::string fullPath = logPath + appName + ".log";
#ifdef _WIN32
        system(("mkdir " + logPath + " 2>nul").c_str());
#else
        system(("mkdir -p " + logPath).c_str());
#endif
        get().logFile_.open(fullPath, std::ios::app);
    }

    static Logger& get() {
        static Logger instance;
        return instance;
    }

    static void shutdown() {
        std::lock_guard<std::mutex> lock(get().mutex_);
        if (get().logFile_.is_open()) {
            get().logFile_.close();
        }
    }

    void setLevel(LogLevel level) {
        level_ = level;
    }

    // Variadic-template logging: substitutes every "{}" placeholder in the
    // message with the stringified argument, in order. Extra args (more than
    // placeholders) are appended at the end so nothing is silently dropped.
    // This is why logs read "Target target-2 added ... CDP: ABC, URL: https://"
    // instead of "Target {} added ... {}target-2...".
    template<typename... Args>
    void log(LogLevel level, const char* file, int line, const std::string& prefix, const Args&... args) {
        if (level < level_) return;

        std::lock_guard<std::mutex> lock(mutex_);

        // Stringify all args first (outside the placeholder scan).
        std::vector<std::string> argStrs;
        (argStrs.push_back(to_string_impl(args)), ...);

        std::string body = prefix;
        std::string out;
        out.reserve(body.size() + 64);
        size_t argIdx = 0;
        size_t pos = 0;
        while (pos < body.size()) {
            size_t found = body.find("{}", pos);
            if (found == std::string::npos) {
                out.append(body, pos, std::string::npos);
                pos = body.size();
                break;
            }
            out.append(body, pos, found - pos);
            if (argIdx < argStrs.size()) {
                out += argStrs[argIdx++];
            } else {
                out += "{}";  // keep the marker if no arg remains
            }
            pos = found + 2;
        }
        // Append any leftover args (no placeholder for them).
        for (; argIdx < argStrs.size(); ++argIdx) {
            out += " ";
            out += argStrs[argIdx];
        }

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::ostringstream oss;
        oss << "[" << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "]";
        oss << " [" << levelToString(level) << "]";
        oss << " [" << appName_ << "]";
        oss << " " << out;

        std::string message = oss.str();
        std::cout << message << std::endl;

        if (logFile_.is_open()) {
            logFile_ << message << std::endl;
            logFile_.flush();
        }

        // Copy sinks under lock, invoke outside (sinks must not deadlock).
        std::vector<Sink> sinks;
        {
            std::lock_guard<std::mutex> lock(sinkMutex_);
            sinks = sinks_;
        }
        for (auto& s : sinks) {
            try { s(message); } catch (...) {}
        }
    }

    // Message-only form (no extra args) — must NOT recurse into itself.
    void log(LogLevel level, const char* file, int line, const std::string& message) {
        logImpl(level, file, line, message);
    }

private:
    // Converts any loggable type to a string via ostringstream.
    template<typename T>
    static std::string to_string_impl(const T& v) {
        std::ostringstream oss;
        oss << v;
        return oss.str();
    }
    static std::string to_string_impl(const std::string& v) { return v; }
    static std::string to_string_impl(const char* v) { return v ? v : "(null)"; }
    static std::string to_string_impl(char v) { return std::string(1, v); }
    static std::string to_string_impl(bool v) { return v ? "true" : "false"; }
    // Wide strings (e.g. Windows paths) are logged as narrow UTF-8-ish text.
    static std::string to_string_impl(const std::wstring& v) {
        std::string out;
        out.reserve(v.size());
        for (wchar_t c : v) out.push_back(static_cast<char>(c & 0xFF));
        return out;
    }
    static std::string to_string_impl(const wchar_t* v) {
        return v ? to_string_impl(std::wstring(v)) : std::string("(null)");
    }
    void logImpl(LogLevel level, const char* file, int line, const std::string& message) {
        if (level < level_) return;

        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::ostringstream oss;
        oss << "[" << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "]";
        oss << " [" << levelToString(level) << "]";
        oss << " [" << appName_ << "]";
        oss << " " << message;

        std::string out = oss.str();
        std::cout << out << std::endl;

        if (logFile_.is_open()) {
            logFile_ << out << std::endl;
            logFile_.flush();
        }

        // Copy sinks under lock, invoke outside.
        std::vector<Sink> sinks;
        {
            std::lock_guard<std::mutex> lock(sinkMutex_);
            sinks = sinks_;
        }
        for (auto& s : sinks) {
            try { s(out); } catch (...) {}
        }
    }
    Logger() = default;

    const char* levelToString(LogLevel level) {
        switch (level) {
            case LogLevel::Trace: return "TRACE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info: return "INFO";
            case LogLevel::Warn: return "WARN";
            case LogLevel::Error: return "ERROR";
            case LogLevel::Critical: return "CRITICAL";
            default: return "UNKNOWN";
        }
    }

    std::string appName_ = "EdgeMonitor";
    std::string logPath_ = "logs/";
    LogLevel level_ = LogLevel::Debug;
    std::ofstream logFile_;
    std::mutex mutex_;
    std::mutex sinkMutex_;
    std::vector<Sink> sinks_;
};

} // namespace edgemon

#define LOG_TRACE(...) edgemon::Logger::get().log(edgemon::LogLevel::Trace, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...) edgemon::Logger::get().log(edgemon::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...) edgemon::Logger::get().log(edgemon::LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...) edgemon::Logger::get().log(edgemon::LogLevel::Warn, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) edgemon::Logger::get().log(edgemon::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_CRITICAL(...) edgemon::Logger::get().log(edgemon::LogLevel::Critical, __FILE__, __LINE__, __VA_ARGS__)
