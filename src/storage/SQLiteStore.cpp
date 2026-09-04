#include "storage/SQLiteStore.hpp"
#include "logger.hpp"

namespace edgemon {

SQLiteStore::SQLiteStore()
    : db_(nullptr)
    , connected_(false) {
}
SQLiteStore::~SQLiteStore() {
    if (db_) {
        sqlite3_close(db_);
    }
}

bool SQLiteStore::initialize(const std::string& dbPath) {
    dbPath_ = dbPath;
    
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Failed to open database: {}", dbPath);
        return false;
    }

    createTables();
    connected_ = true;
    LOG_INFO("SQLiteStore initialized: {}", dbPath);
    return true;
}

bool SQLiteStore::execute(const std::string& sql) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQL error: {}", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

void SQLiteStore::createTables() {
    std::string sql = R"(
        CREATE TABLE IF NOT EXISTS records (
            id TEXT PRIMARY KEY,
            target_id TEXT NOT NULL,
            content TEXT NOT NULL,
            url TEXT,
            timestamp INTEGER NOT NULL,
            content_hash TEXT NOT NULL
        );
        
        CREATE INDEX IF NOT EXISTS idx_target ON records(target_id);
        CREATE INDEX IF NOT EXISTS idx_timestamp ON records(timestamp);
        CREATE INDEX IF NOT EXISTS idx_hash ON records(content_hash);
        
        CREATE TABLE IF NOT EXISTS targets (
            id TEXT PRIMARY KEY,
            cdp_target_id TEXT NOT NULL,
            process_id INTEGER NOT NULL DEFAULT 0,
            title TEXT,
            url TEXT,
            web_socket_url TEXT,
            ocr_enabled INTEGER NOT NULL DEFAULT 0,
            state TEXT,
            state_message TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_targets_cdp ON targets(cdp_target_id);
    )";
    
    execute(sql);

    // Migration for older databases: add the target_rule + profile_hint
    // columns when they do not exist yet.
    bool hasRule = false;
    {
        const char* check = "PRAGMA table_info(targets)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, check, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (name && std::string(name) == "target_rule") hasRule = true;
            }
            sqlite3_finalize(stmt);
        }
    }
    if (!hasRule) {
        execute("ALTER TABLE targets ADD COLUMN target_rule TEXT NOT NULL DEFAULT 'exact'");
        execute("ALTER TABLE targets ADD COLUMN profile_hint TEXT NOT NULL DEFAULT ''");
    }
}

bool SQLiteStore::saveRecord(const StoredRecord& record) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    std::string sql = "INSERT OR REPLACE INTO records (id, target_id, content, url, timestamp, content_hash) VALUES (?, ?, ?, ?, ?, ?)";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR("Failed to prepare statement");
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, record.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.targetId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, record.url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, record.timestamp);
    sqlite3_bind_text(stmt, 6, record.contentHash.c_str(), -1, SQLITE_TRANSIENT);
    
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    
    if (!success) {
        LOG_ERROR("Failed to save record: {}", record.id);
    }
    
    return success;
}

bool SQLiteStore::saveRecords(const std::vector<StoredRecord>& records) {
    bool allSuccess = true;
    for (const auto& record : records) {
        if (!saveRecord(record)) {
            allSuccess = false;
        }
    }
    return allSuccess;
}

bool SQLiteStore::deleteRecord(const std::string& id) {
    std::string sql = "DELETE FROM records WHERE id = '" + id + "'";
    return execute(sql);
}

bool SQLiteStore::deleteByTarget(const std::string& targetId) {
    std::string sql = "DELETE FROM records WHERE target_id = '" + targetId + "'";
    return execute(sql);
}

bool SQLiteStore::deleteByTimeRange(int64_t start, int64_t end) {
    std::string sql = "DELETE FROM records WHERE timestamp >= " + std::to_string(start) +
                      " AND timestamp <= " + std::to_string(end);
    return execute(sql);
}

std::optional<StoredRecord> SQLiteStore::getRecord(const std::string& id) const {
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    std::string sql = "SELECT id, target_id, content, url, timestamp, content_hash FROM records WHERE id = ?";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    
    StoredRecord record;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        record.id = (const char*)sqlite3_column_text(stmt, 0);
        record.targetId = (const char*)sqlite3_column_text(stmt, 1);
        record.content = (const char*)sqlite3_column_text(stmt, 2);
        record.url = (const char*)sqlite3_column_text(stmt, 3);
        record.timestamp = sqlite3_column_int64(stmt, 4);
        record.contentHash = (const char*)sqlite3_column_text(stmt, 5);
        sqlite3_finalize(stmt);
        return record;
    }
    
    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::vector<StoredRecord> SQLiteStore::getByTarget(const std::string& targetId) const {
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    std::vector<StoredRecord> records;
    std::string sql = "SELECT id, target_id, content, url, timestamp, content_hash FROM records WHERE target_id = ? ORDER BY timestamp DESC";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return records;
    }
    
    sqlite3_bind_text(stmt, 1, targetId.c_str(), -1, SQLITE_TRANSIENT);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredRecord record;
        record.id = (const char*)sqlite3_column_text(stmt, 0);
        record.targetId = (const char*)sqlite3_column_text(stmt, 1);
        record.content = (const char*)sqlite3_column_text(stmt, 2);
        record.url = (const char*)sqlite3_column_text(stmt, 3);
        record.timestamp = sqlite3_column_int64(stmt, 4);
        record.contentHash = (const char*)sqlite3_column_text(stmt, 5);
        records.push_back(record);
    }
    
    sqlite3_finalize(stmt);
    return records;
}

std::vector<StoredRecord> SQLiteStore::getByTimeRange(int64_t start, int64_t end) const {
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    std::vector<StoredRecord> records;
    std::string sql = "SELECT id, target_id, content, url, timestamp, content_hash FROM records WHERE timestamp >= ? AND timestamp <= ? ORDER BY timestamp DESC";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return records;
    }
    
    sqlite3_bind_int64(stmt, 1, start);
    sqlite3_bind_int64(stmt, 2, end);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredRecord record;
        record.id = (const char*)sqlite3_column_text(stmt, 0);
        record.targetId = (const char*)sqlite3_column_text(stmt, 1);
        record.content = (const char*)sqlite3_column_text(stmt, 2);
        record.url = (const char*)sqlite3_column_text(stmt, 3);
        record.timestamp = sqlite3_column_int64(stmt, 4);
        record.contentHash = (const char*)sqlite3_column_text(stmt, 5);
        records.push_back(record);
    }
    
    sqlite3_finalize(stmt);
    return records;
}

std::vector<StoredRecord> SQLiteStore::getAll() const {
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    std::vector<StoredRecord> records;
    std::string sql = "SELECT id, target_id, content, url, timestamp, content_hash FROM records ORDER BY timestamp DESC";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return records;
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredRecord record;
        record.id = (const char*)sqlite3_column_text(stmt, 0);
        record.targetId = (const char*)sqlite3_column_text(stmt, 1);
        record.content = (const char*)sqlite3_column_text(stmt, 2);
        record.url = (const char*)sqlite3_column_text(stmt, 3);
        record.timestamp = sqlite3_column_int64(stmt, 4);
        record.contentHash = (const char*)sqlite3_column_text(stmt, 5);
        records.push_back(record);
    }
    
    sqlite3_finalize(stmt);
    return records;
}

bool SQLiteStore::recordExists(const std::string& contentHash) const {
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    std::string sql = "SELECT COUNT(*) FROM records WHERE content_hash = ?";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, contentHash.c_str(), -1, SQLITE_TRANSIENT);
    
    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }
    
    sqlite3_finalize(stmt);
    return exists;
}

size_t SQLiteStore::count() const {
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM records", -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    
    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return count;
}

size_t SQLiteStore::countByTarget(const std::string& targetId) const {
    std::lock_guard<std::mutex> lock(dbMutex_);
    
    std::string sql = "SELECT COUNT(*) FROM records WHERE target_id = ?";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    
    sqlite3_bind_text(stmt, 1, targetId.c_str(), -1, SQLITE_TRANSIENT);
    
    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return count;
}

void SQLiteStore::vacuum() {
    execute("VACUUM");
    LOG_INFO("Database vacuumed");
}

// ---- Locked-target persistence ----

bool SQLiteStore::saveTarget(const TargetRow& row) {
    std::lock_guard<std::mutex> lock(dbMutex_);

    const char* sql =
        "INSERT OR REPLACE INTO targets "
        "(id, cdp_target_id, process_id, title, url, web_socket_url, ocr_enabled, state, state_message, target_rule, profile_hint) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR("saveTarget: prepare failed");
        return false;
    }
    sqlite3_bind_text(stmt, 1, row.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, row.cdpTargetId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, static_cast<int>(row.processId));
    sqlite3_bind_text(stmt, 4, row.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, row.url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, row.webSocketUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, row.ocrEnabled ? 1 : 0);
    sqlite3_bind_text(stmt, 8, row.state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, row.stateMessage.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, row.targetRule.empty() ? "exact" : row.targetRule.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, row.profileHint.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool SQLiteStore::deleteTarget(const std::string& id) {
    std::lock_guard<std::mutex> lock(dbMutex_);

    const char* sql = "DELETE FROM targets WHERE id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool SQLiteStore::deleteTargetByCDP(const std::string& cdpTargetId) {
    std::lock_guard<std::mutex> lock(dbMutex_);

    const char* sql = "DELETE FROM targets WHERE cdp_target_id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, cdpTargetId.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<SQLiteStore::TargetRow> SQLiteStore::loadTargets() const {
    std::lock_guard<std::mutex> lock(dbMutex_);

    std::vector<TargetRow> rows;
    const char* sql =
        "SELECT id, cdp_target_id, process_id, title, url, web_socket_url, "
        "ocr_enabled, state, state_message, target_rule, profile_hint "
        "FROM targets ORDER BY id";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rows;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TargetRow row;
        if (sqlite3_column_text(stmt, 0)) row.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (sqlite3_column_text(stmt, 1)) row.cdpTargetId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        row.processId = static_cast<uint32_t>(sqlite3_column_int(stmt, 2));
        if (sqlite3_column_text(stmt, 3)) row.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (sqlite3_column_text(stmt, 4)) row.url = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (sqlite3_column_text(stmt, 5)) row.webSocketUrl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        row.ocrEnabled = sqlite3_column_int(stmt, 6) != 0;
        if (sqlite3_column_text(stmt, 7)) row.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        if (sqlite3_column_text(stmt, 8)) row.stateMessage = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        if (sqlite3_column_text(stmt, 9)) row.targetRule = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        if (sqlite3_column_text(stmt, 10)) row.profileHint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        if (row.targetRule.empty()) row.targetRule = "exact";
        rows.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return rows;
}

}
