#pragma once

#include "common.hpp"
#include "embedding/VectorStore.hpp"
#include <sqlite3.h>
#include <string>
#include <vector>
#include <optional>

namespace edgemon {

struct StoredRecord {
    std::string id;
    std::string targetId;
    std::string content;
    std::string url;
    int64_t timestamp;
    std::string contentHash;
};

class SQLiteStore {
public:
    SQLiteStore();
    ~SQLiteStore();

    bool initialize(const std::string& dbPath = "edge_monitor.db");
    bool isConnected() const { return connected_; }

    bool saveRecord(const StoredRecord& record);
    bool saveRecords(const std::vector<StoredRecord>& records);

    bool deleteRecord(const std::string& id);
    bool deleteByTarget(const std::string& targetId);
    bool deleteByTimeRange(int64_t start, int64_t end);

    std::optional<StoredRecord> getRecord(const std::string& id) const;
    std::vector<StoredRecord> getByTarget(const std::string& targetId) const;
    std::vector<StoredRecord> getByTimeRange(int64_t start, int64_t end) const;
    std::vector<StoredRecord> getAll() const;

    bool recordExists(const std::string& contentHash) const;

    size_t count() const;
    size_t countByTarget(const std::string& targetId) const;

    // ---- Locked-target persistence (survives across CLI invocations) ----
    struct TargetRow {
        std::string id;
        std::string cdpTargetId;
        uint32_t processId = 0;
        std::string title;
        std::string url;
        std::string webSocketUrl;
        bool ocrEnabled = false;
        std::string state;
        std::string stateMessage;
        std::string targetRule;    // TargetMatchRuleToString value
        std::string profileHint;   // user-data-dir when safely known
    };

    bool saveTarget(const TargetRow& row);
    bool deleteTarget(const std::string& id);
    bool deleteTargetByCDP(const std::string& cdpTargetId);
    std::vector<TargetRow> loadTargets() const;

    void vacuum();

private:
    bool execute(const std::string& sql);
    void createTables();

    sqlite3* db_;
    std::atomic<bool> connected_;
    std::string dbPath_;
    mutable std::mutex dbMutex_;
};

}
