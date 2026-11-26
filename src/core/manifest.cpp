// manifest.cpp — see manifest.hpp.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "core/manifest.hpp"
#include "core/rules.hpp"
#include "win/sys.hpp"
#include <cstdlib>

namespace kc {

static const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS copies(dst_key TEXT PRIMARY KEY, dst TEXT NOT NULL, src TEXT, size INTEGER, mtime INTEGER, hash TEXT, copied_ms INTEGER);
CREATE TABLE IF NOT EXISTS runs(id INTEGER PRIMARY KEY, started_ms INTEGER, finished_ms INTEGER, sources TEXT, dest TEXT, planned_files INTEGER,
  planned_bytes INTEGER, files INTEGER, bytes INTEGER, errors INTEGER, stopped INTEGER);
)SQL";

static bool make_hidden_dir(const std::string& dir) {
    const std::wstring w = win::longw(dir);
    if (!CreateDirectoryW(w.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY);
    return true;
}

Manifest::Manifest(const std::string& dest) {
    // KAIROCOPY_STATE: another place for the record (the tests: not the root of the drive they run on)
    const char* over = std::getenv("KAIROCOPY_STATE");
    std::string dir = win::join(over && *over ? std::string(over) : win::volume_root(dest), kStateDir);
    if (!make_hidden_dir(dir)) {
        dir = win::join(dest, kStateDir);
        if (!make_hidden_dir(dir)) throw DbError("cannot make " + dir);
    }
    path_ = win::join(dir, "manifest.db");
    db_ = std::make_unique<Db>(path_);
    db_->exec(kSchema);
    // added after the first version
    bool has_options = false;
    {
        auto s = db_->prepare("PRAGMA table_info(runs)");
        while (s.step()) if (s.text(1) == "options") has_options = true;
    }
    if (!has_options) db_->exec("ALTER TABLE runs ADD COLUMN options TEXT; ALTER TABLE runs ADD COLUMN set_aside INTEGER DEFAULT 0;");
}

bool Manifest::unfinished(const std::string& path, Run& r) {
    if (GetFileAttributesW(win::longw(path).c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    try {
        Db db(path);
        bool has_options = false;
        {
            auto s = db.prepare("PRAGMA table_info(runs)");
            while (s.step()) if (s.text(1) == "options") has_options = true;
        }
        if (!has_options) return false;
        auto s = db.prepare("SELECT id, started_ms, planned_files, planned_bytes, (SELECT COUNT(*) FROM copies c WHERE c.copied_ms>=r.started_ms), "
                            "(SELECT COALESCE(SUM(size),0) FROM copies c WHERE c.copied_ms>=r.started_ms), sources, dest, COALESCE(options,'') FROM runs r "
                            "WHERE (finished_ms IS NULL OR stopped=1) AND set_aside=0 AND NOT EXISTS (SELECT 1 FROM runs l WHERE l.id>r.id "
                            "AND l.sources=r.sources AND l.dest=r.dest AND l.finished_ms IS NOT NULL AND l.stopped=0) ORDER BY id DESC LIMIT 1");
        if (!s.step()) return false;
        r = Run{s.i64(0), s.i64(1), s.i64(2), s.i64(3), s.i64(4), s.i64(5), s.text(6), s.text(7), s.text(8)};
        return true;
    } catch (const std::exception&) { return false; }
}

void Manifest::set_aside(const std::string& path, int64_t run) {
    try {
        Db db(path);
        db.prepare("UPDATE runs SET set_aside=1 WHERE id=?").bind(1, run).run();
    } catch (const std::exception&) {}
}

std::unordered_map<std::string, Manifest::Rec> Manifest::under(const std::string& dir) {
    std::unordered_map<std::string, Rec> m;
    const std::string prefix = win::key_of(dir) + "\\";
    auto s = db_->prepare("SELECT dst_key, size, mtime FROM copies WHERE dst_key >= ? AND dst_key < ?");
    // every key that starts with prefix sorts between prefix and prefix with its last character raised by one
    std::string upper = prefix;
    upper.back() = char(upper.back() + 1);
    s.bind(1, prefix).bind(2, upper);
    while (s.step()) m[s.text(0)] = Rec{uint64_t(s.i64(1)), s.i64(2)};
    return m;
}

void Manifest::record(const std::vector<CopiedFile>& files) {
    if (files.empty()) return;
    Transaction t(*db_);
    auto s = db_->prepare("INSERT INTO copies(dst_key,dst,src,size,mtime,hash,copied_ms) VALUES(?,?,?,?,?,?,?) "
                          "ON CONFLICT(dst_key) DO UPDATE SET dst=excluded.dst, src=excluded.src, size=excluded.size, mtime=excluded.mtime, "
                          "hash=excluded.hash, copied_ms=excluded.copied_ms");
    const int64_t now = win::now_ms();
    for (auto& f : files) {
        s.bind(1, win::key_of(f.dst)); s.bind(2, f.dst); s.bind(3, f.src); s.bind(4, int64_t(f.size)); s.bind(5, f.mtime);
        if (f.hash.empty()) s.bind_null(6); else s.bind(6, f.hash);
        s.bind(7, now);
        s.run();
    }
    t.commit();
}

int64_t Manifest::begin_run(const std::string& sources, const std::string& dest, const std::string& options, int64_t files, int64_t bytes) {
    db_->prepare("INSERT INTO runs(started_ms,sources,dest,options,planned_files,planned_bytes,files,bytes,errors,stopped) VALUES(?,?,?,?,?,?,0,0,0,0)")
        .bind(1, win::now_ms()).bind(2, sources).bind(3, dest).bind(4, options).bind(5, files).bind(6, bytes).run();
    return db_->last_id();
}

void Manifest::end_run(int64_t run, int64_t files, int64_t bytes, int64_t errors, bool stopped) {
    db_->prepare("UPDATE runs SET finished_ms=?, files=?, bytes=?, errors=?, stopped=? WHERE id=?")
        .bind(1, win::now_ms()).bind(2, files).bind(3, bytes).bind(4, errors).bind(5, int64_t(stopped ? 1 : 0)).bind(6, run).run();
}

Manifest::Summary Manifest::summary(const std::string& dir) {
    Summary r;
    if (dir.empty()) {
        auto s = db_->prepare("SELECT COUNT(*), COALESCE(SUM(size),0) FROM copies");
        if (s.step()) { r.files = s.i64(0); r.bytes = s.i64(1); }
    } else {
        std::string prefix = win::key_of(dir) + "\\", upper = prefix;
        upper.back() = char(upper.back() + 1);
        auto s = db_->prepare("SELECT COUNT(*), COALESCE(SUM(size),0) FROM copies WHERE dst_key >= ? AND dst_key < ?");
        s.bind(1, prefix).bind(2, upper);
        if (s.step()) { r.files = s.i64(0); r.bytes = s.i64(1); }
    }
    auto n = db_->prepare("SELECT COUNT(*), MAX(started_ms) FROM runs");
    if (n.step()) {
        r.runs = n.i64(0);
        if (!n.null(1)) {
            // ms since 1970 to file time ticks
            r.last_run = win::time_text((n.i64(1) + 11644473600000LL) * 10000);
        }
    }
    return r;
}

}  // namespace kc
