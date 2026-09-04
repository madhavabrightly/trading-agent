#pragma once

#include <stdexcept>
#include <string>

namespace edgemon {

class EdgeMonitorException : public std::runtime_error {
public:
    explicit EdgeMonitorException(const std::string& msg) : runtime_error(msg) {}
};

class ConnectionException : public EdgeMonitorException {
public:
    explicit ConnectionException(const std::string& msg) : EdgeMonitorException("Connection: " + msg) {}
};

class CDPException : public EdgeMonitorException {
public:
    explicit CDPException(const std::string& msg) : EdgeMonitorException("CDP: " + msg) {}
};

class CaptureException : public EdgeMonitorException {
public:
    explicit CaptureException(const std::string& msg) : EdgeMonitorException("Capture: " + msg) {}
};

class ExtractionException : public EdgeMonitorException {
public:
    explicit ExtractionException(const std::string& msg) : EdgeMonitorException("Extraction: " + msg) {}
};

class EmbeddingException : public EdgeMonitorException {
public:
    explicit EmbeddingException(const std::string& msg) : EdgeMonitorException("Embedding: " + msg) {}
};

class StorageException : public EdgeMonitorException {
public:
    explicit StorageException(const std::string& msg) : EdgeMonitorException("Storage: " + msg) {}
};

class TargetException : public EdgeMonitorException {
public:
    explicit TargetException(const std::string& msg) : EdgeMonitorException("Target: " + msg) {}
};

}
