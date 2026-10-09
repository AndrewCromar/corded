// Thin RAII wrapper over the SQLite C API. Header-only so the server can build
// it against stock SQLite and the core against the encrypted build.
#pragma once

#include "corded/common/bytes.hpp"

#ifdef CORDED_USE_SQLITE3MC
#include "sqlite3mc_amalgamation.h"
#else
#include <sqlite3.h>
#endif

#include <stdexcept>
#include <string>

namespace corded::db {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            throw Error(std::string("prepare failed: ") + sqlite3_errmsg(db) + " in: " + sql);
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int i, ByteView v) {
        check(sqlite3_bind_blob64(stmt_, i, v.data() ? v.data() : reinterpret_cast<const uint8_t*>(""),
                                  v.size(), SQLITE_TRANSIENT));
        return *this;
    }
    Statement& bind(int i, const Bytes& v) { return bind(i, ByteView(v)); }
    Statement& bind(int i, const Key32& v) { return bind(i, ByteView(v)); }
    Statement& bind(int i, std::string_view v) {
        check(sqlite3_bind_text64(stmt_, i, v.data() ? v.data() : "", v.size(), SQLITE_TRANSIENT,
                                  SQLITE_UTF8));
        return *this;
    }
    Statement& bind(int i, const std::string& v) { return bind(i, std::string_view(v)); }
    Statement& bind(int i, const char* v) { return bind(i, std::string_view(v)); }
    Statement& bind(int i, int64_t v) {
        check(sqlite3_bind_int64(stmt_, i, v));
        return *this;
    }
    Statement& bind(int i, uint64_t v) { return bind(i, static_cast<int64_t>(v)); }
    Statement& bind(int i, int v) { return bind(i, static_cast<int64_t>(v)); }
    Statement& bind(int i, uint32_t v) { return bind(i, static_cast<int64_t>(v)); }
    Statement& bind_null(int i) {
        check(sqlite3_bind_null(stmt_, i));
        return *this;
    }

    // Returns true while there is a row.
    bool step() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw Error(std::string("step failed: ") + sqlite3_errmsg(db_));
    }
    void exec() {
        while (step()) {}
    }

    Bytes blob(int col) const {
        auto* p = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_, col));
        int n = sqlite3_column_bytes(stmt_, col);
        return p ? Bytes(p, p + n) : Bytes{};
    }
    std::string text(int col) const {
        auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
        int n = sqlite3_column_bytes(stmt_, col);
        return p ? std::string(p, static_cast<size_t>(n)) : std::string{};
    }
    int64_t i64(int col) const { return sqlite3_column_int64(stmt_, col); }
    uint64_t u64(int col) const { return static_cast<uint64_t>(sqlite3_column_int64(stmt_, col)); }
    bool is_null(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }

private:
    void check(int rc) {
        if (rc != SQLITE_OK) throw Error(std::string("bind failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

class Database {
public:
    Database() = default;
    explicit Database(const std::string& path) { open(path); }
    ~Database() { close(); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void open(const std::string& path) {
        close();
        if (sqlite3_open_v2(path.c_str(), &db_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
            close();
            throw Error("cannot open database " + path + ": " + msg);
        }
        sqlite3_busy_timeout(db_, 5000);
    }
    void close() {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
    }
    bool is_open() const { return db_ != nullptr; }
    sqlite3* raw() { return db_; }

    void exec(const char* sql) {
        char* msg = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
            std::string m = msg ? msg : "unknown error";
            sqlite3_free(msg);
            throw Error("exec failed: " + m);
        }
    }
    Statement prepare(const char* sql) { return Statement(db_, sql); }
    int64_t last_insert_rowid() { return sqlite3_last_insert_rowid(db_); }
    int changes() { return sqlite3_changes(db_); }

private:
    sqlite3* db_ = nullptr;
};

// Commits on commit(), rolls back otherwise.
class Transaction {
public:
    explicit Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!done_) {
            try {
                db_.exec("ROLLBACK");
            } catch (...) {
            }
        }
    }
    void commit() {
        db_.exec("COMMIT");
        done_ = true;
    }

private:
    Database& db_;
    bool done_ = false;
};

}  // namespace corded::db
