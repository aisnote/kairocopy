// db.hpp — a thin layer over SQLite: a connection and prepared statements that throw on error.
#pragma once
#include <sqlite3.h>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace kc {

struct DbError : std::runtime_error { using std::runtime_error::runtime_error; };

class Stmt {
  public:
    Stmt() = default;
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) throw DbError(std::string(sqlite3_errmsg(db)) + " in: " + sql);
    }
    ~Stmt() { if (s_) sqlite3_finalize(s_); }
    Stmt(Stmt&& o) noexcept : db_(o.db_), s_(o.s_) { o.s_ = nullptr; }
    Stmt& operator=(Stmt&& o) noexcept { if (this != &o) { if (s_) sqlite3_finalize(s_); db_ = o.db_; s_ = o.s_; o.s_ = nullptr; } return *this; }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, int64_t v) { check(sqlite3_bind_int64(s_, i, v)); return *this; }
    Stmt& bind(int i, int v) { return bind(i, int64_t(v)); }
    Stmt& bind(int i, uint64_t v) { return bind(i, int64_t(v)); }
    Stmt& bind(int i, double v) { check(sqlite3_bind_double(s_, i, v)); return *this; }
    Stmt& bind(int i, const std::string& v) { check(sqlite3_bind_text(s_, i, v.data(), int(v.size()), SQLITE_TRANSIENT)); return *this; }
    Stmt& bind(int i, const char* v) { return bind(i, std::string(v)); }
    Stmt& bind_null(int i) { check(sqlite3_bind_null(s_, i)); return *this; }
    Stmt& bind_blob(int i, const std::string& v) { check(sqlite3_bind_blob(s_, i, v.data(), int(v.size()), SQLITE_TRANSIENT)); return *this; }
    std::string blob(int c) const { auto p = sqlite3_column_blob(s_, c); return p ? std::string(static_cast<const char*>(p), size_t(sqlite3_column_bytes(s_, c))) : std::string(); }
    // true while there is a row
    bool step() {
        const int r = sqlite3_step(s_);
        if (r == SQLITE_ROW) return true;
        if (r == SQLITE_DONE) return false;
        throw DbError(sqlite3_errmsg(db_));
    }
    void run() { step(); reset(); }
    void reset() { sqlite3_reset(s_); sqlite3_clear_bindings(s_); }
    int64_t i64(int c) const { return sqlite3_column_int64(s_, c); }
    double f64(int c) const { return sqlite3_column_double(s_, c); }
    std::string text(int c) const { auto p = sqlite3_column_text(s_, c); return p ? std::string(reinterpret_cast<const char*>(p), size_t(sqlite3_column_bytes(s_, c))) : std::string(); }
    bool null(int c) const { return sqlite3_column_type(s_, c) == SQLITE_NULL; }
    int columns() const { return sqlite3_column_count(s_); }
    const char* name(int c) const { return sqlite3_column_name(s_, c); }
    int type(int c) const { return sqlite3_column_type(s_, c); }
  private:
    void check(int r) { if (r != SQLITE_OK) throw DbError(sqlite3_errmsg(db_)); }
    sqlite3* db_ = nullptr;
    sqlite3_stmt* s_ = nullptr;
};

class Db {
  public:
    explicit Db(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            std::string e = db_ ? sqlite3_errmsg(db_) : "out of memory";
            if (db_) sqlite3_close(db_);
            throw DbError("cannot open the database " + path + ": " + e);
        }
        sqlite3_busy_timeout(db_, 10000);                      // the web server and a scan may share it
        exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY;");
    }
    ~Db() { if (db_) sqlite3_close(db_); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    void exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) { std::string e = err ? err : "?"; sqlite3_free(err); throw DbError(e); }
    }
    Stmt prepare(const char* sql) { return Stmt(db_, sql); }
    int64_t last_id() const { return sqlite3_last_insert_rowid(db_); }
    int changes() const { return sqlite3_changes(db_); }
    sqlite3* raw() { return db_; }
  private:
    sqlite3* db_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT, rolled back when left by an exception
class Transaction {
  public:
    explicit Transaction(Db& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
    void commit() { db_.exec("COMMIT"); done_ = true; }
    ~Transaction() { if (!done_) { try { db_.exec("ROLLBACK"); } catch (...) {} } }
  private:
    Db& db_;
    bool done_ = false;
};

}  // namespace kc
