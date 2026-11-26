// kairocopy-cli — the copy engine from the command line (scripts, coding agents). Every command takes --json: one JSON
// document on stdout, progress on stderr. Exit codes: 0 done, 1 failed or stopped, 2 wrong usage.
//   kairocopy-cli analyze <source>... --to <folder> [--important]
//   kairocopy-cli copy <source>... --to <folder> [--important] [--verify] [--writers BIG,SMALL] [--quiet]
//   kairocopy-cli status <folder>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "core/copier.hpp"
#include "core/manifest.hpp"
#include "core/plan.hpp"
#include "json/json.hpp"
#include "win/sys.hpp"

using namespace kc;
using json::Value;

namespace {
struct Usage : std::runtime_error { using std::runtime_error::runtime_error; };
bool g_json = false;
std::atomic<bool> g_stop{false};

const char* kUsage =
    "kairocopy-cli analyze <source>... --to <folder> [--important] [--json]\n"
    "kairocopy-cli copy <source>... --to <folder> [--important] [--verify] [--writers BIG,SMALL] [--quiet] [--json]\n"
    "kairocopy-cli status <folder> [--json]\n"
    "  <source>: files or folders; each lands in <folder> under its own name (a drive's root C:\\ as C)\n"
    "  --important: only photos, videos, music, documents, mail, chats, code, designs and archives\n"
    "  --verify: read both copies back and compare them\n";

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    bool has(const std::string& k) const { return opt.count(k) > 0; }
};
Args parse(int argc, char** argv, const std::set<std::string>& values, const std::set<std::string>& flags) {
    Args a;
    for (int i = 2; i < argc; i++) {
        std::string s = argv[i];
        if (s.size() > 2 && s.compare(0, 2, "--") == 0) {
            const std::string k = s.substr(2);
            if (k == "json" || flags.count(k)) { a.opt[k] = "1"; continue; }
            if (!values.count(k)) throw Usage("unknown option --" + k);
            if (i + 1 >= argc) throw Usage("--" + k + " needs a value");
            a.opt[k] = argv[++i];
        } else a.pos.push_back(s);
    }
    return a;
}

std::string gb(int64_t b) {
    char s[32];
    if (b >= 1000000000) snprintf(s, sizeof s, "%.2f GB", b / 1e9);
    else snprintf(s, sizeof s, "%.1f MB", b / 1e6);
    return s;
}
Value counts(const Counts& c) { return Value::object({{"files", c.files}, {"bytes", c.bytes}}); }
void emit(const Value& v, const std::string& text) {
    if (g_json) std::printf("%s\n", v.dump(2).c_str()); else std::printf("%s", text.c_str());
    std::fflush(stdout);
}

Value plan_json(const Plan& p) {
    Value v = Value::object();
    Value s = Value::array();
    for (auto& x : p.sources) s.push(x);
    v.set("sources", s);
    v.set("to", p.dest);
    v.set("important_only", p.important_only);
    v.set("all", counts(p.all)); v.set("to_copy", counts(p.todo)); v.set("new", counts(p.fresh)); v.set("overwrite", counts(p.overwrite));
    v.set("already_there", counts(p.same)); v.set("target_newer", counts(p.target_newer)); v.set("not_important", counts(p.not_important));
    v.set("online_only", counts(p.online_only)); v.set("skipped_folders", p.skipped_dirs);
    Value cats = Value::object();
    for (int i = 0; i < kCats; i++)
        if (p.by_cat[i].files)
            cats.set(cat_name(Cat(i)), Value::object({{"files", p.by_cat[i].files}, {"bytes", p.by_cat[i].bytes}, {"to_copy_files", p.todo_by_cat[i].files},
                                                      {"to_copy_bytes", p.todo_by_cat[i].bytes}}));
    v.set("categories", cats);
    v.set("free_bytes", int64_t(p.free_bytes));
    v.set("target_is_hard_disk", p.target_seeks);
    Value e = Value::array();
    for (auto& x : p.errors) e.push(Value::object({{"path", x.first}, {"error", x.second}}));
    v.set("unreadable_folders", e);
    Value n = Value::array();
    for (auto& x : p.target_newer_samples) n.push(x);
    v.set("target_newer_samples", n);
    v.set("seconds", p.seconds);
    v.set("stopped", p.stopped);
    return v;
}
std::string plan_text(const Plan& p) {
    std::string t;
    char b[256];
    snprintf(b, sizeof b, "%lld files (%s) under the sources, %.1f s; %lld folders left out (system, programs, caches)\n", (long long)p.all.files,
             gb(p.all.bytes).c_str(), p.seconds, (long long)p.skipped_dirs);
    t += b;
    for (int i = 0; i < kCats; i++) {
        if (!p.by_cat[i].files) continue;
        snprintf(b, sizeof b, "  %-9s %8lld files %11s   to copy %8lld (%s)\n", cat_name(Cat(i)), (long long)p.by_cat[i].files, gb(p.by_cat[i].bytes).c_str(),
                 (long long)p.todo_by_cat[i].files, gb(p.todo_by_cat[i].bytes).c_str());
        t += b;
    }
    snprintf(b, sizeof b, "to copy: %lld files, %s (new %lld, newer than the target's %lld)\n", (long long)p.todo.files, gb(p.todo.bytes).c_str(),
             (long long)p.fresh.files, (long long)p.overwrite.files);
    t += b;
    snprintf(b, sizeof b, "already at the target: %lld (%s); the target's is newer: %lld; not important: %lld; online only: %lld\n", (long long)p.same.files,
             gb(p.same.bytes).c_str(), (long long)p.target_newer.files, (long long)p.not_important.files, (long long)p.online_only.files);
    t += b;
    snprintf(b, sizeof b, "target %s: %s free, %s\n", p.dest.c_str(), gb(int64_t(p.free_bytes)).c_str(), p.target_seeks ? "a hard disk" : "an SSD");
    t += b;
    for (size_t i = 0; i < p.errors.size() && i < 5; i++) t += "  cannot read " + p.errors[i].first + ": " + p.errors[i].second + "\n";
    return t;
}

Plan plan_from(const Args& a, bool quiet) {
    if (a.pos.empty()) throw Usage("which files or folders?");
    if (!a.has("to")) throw Usage("where to? --to <folder>");
    for (auto& s : a.pos)
        if (GetFileAttributesW(win::longw(win::absolute(s)).c_str()) == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("cannot find " + s);
    return analyze(a.pos, a.opt.at("to"), a.has("important"), &g_stop, [&](const AnalyzeProgress& p) {
        if (!quiet) std::fprintf(stderr, "\ranalysing: %lld files, %s   ", (long long)p.files, gb(p.bytes).c_str());
    });
}

int cmd_analyze(int argc, char** argv) {
    Args a = parse(argc, argv, {"to"}, {"important", "quiet"});
    Plan p = plan_from(a, a.has("quiet"));
    if (!a.has("quiet")) std::fprintf(stderr, "\n");
    emit(plan_json(p), plan_text(p));
    return p.stopped ? 1 : 0;
}

int cmd_copy(int argc, char** argv) {
    Args a = parse(argc, argv, {"to", "writers"}, {"important", "verify", "quiet"});
    const bool quiet = a.has("quiet");
    Plan p = plan_from(a, quiet);
    if (!quiet) std::fprintf(stderr, "\n%s", plan_text(p).c_str());
    if (p.stopped) throw std::runtime_error("stopped while analysing");
    // the room it needs: what is new, and what grows where an older copy is replaced
    if (uint64_t(p.fresh.bytes) > p.free_bytes + uint64_t(p.overwrite.bytes))
        throw std::runtime_error("not enough room at the target: " + gb(p.todo.bytes) + " to copy, " + gb(int64_t(p.free_bytes)) + " free");
    CopyOptions o;
    o.verify = a.has("verify");
    o.stop = &g_stop;
    if (a.has("writers")) {
        const std::string w = a.opt.at("writers");
        const size_t c = w.find(',');
        o.big_writers = std::atoi(w.c_str());
        o.small_writers = c == std::string::npos ? o.big_writers : std::atoi(w.c_str() + c + 1);
    }
    CopyReport r = copy(p, o, [&](const CopyProgress& x) {
        if (quiet) return;
        const double eta = x.speed > 0 ? double(x.bytes_total - x.bytes_done) / x.speed : 0;
        std::fprintf(stderr, "\rcopying: %lld / %lld files, %s / %s, %.0f MB/s, %d:%02d left, %lld errors   ", (long long)x.files_done,
                     (long long)x.files_total, gb(x.bytes_done).c_str(), gb(x.bytes_total).c_str(), x.speed / 1e6, int(eta / 60), int(eta) % 60,
                     (long long)x.errors);
    });
    if (!quiet) std::fprintf(stderr, "\n");
    Value v = plan_json(p);
    v.set("ok", !r.stopped && r.errors == 0);
    v.set("copied", Value::object({{"files", r.files}, {"bytes", r.bytes}}));
    v.set("errors", r.errors); v.set("in_use", r.in_use); v.set("stopped", r.stopped); v.set("disk_full", r.disk_full);
    v.set("copy_seconds", r.seconds);
    v.set("mb_per_second", r.seconds > 0 ? r.bytes / r.seconds / 1e6 : 0.0);
    v.set("manifest", r.manifest);
    Value f = Value::array();
    for (size_t i = 0; i < r.failed.size() && i < 1000; i++) f.push(Value::object({{"path", r.failed[i].first}, {"error", r.failed[i].second}}));
    v.set("failed", f);
    char b[300];
    snprintf(b, sizeof b, "%s %lld files, %s in %.1f s (%.0f MB/s, %.0f files/s); %lld errors (%lld in use)\n", r.stopped ? "stopped after" : "copied",
             (long long)r.files, gb(r.bytes).c_str(), r.seconds, r.seconds > 0 ? r.bytes / r.seconds / 1e6 : 0.0, r.seconds > 0 ? r.files / r.seconds : 0.0,
             (long long)r.errors, (long long)r.in_use);
    std::string t = b;
    for (size_t i = 0; i < r.failed.size() && i < 10; i++) t += "  " + r.failed[i].first + ": " + r.failed[i].second + "\n";
    if (r.disk_full) t += "  the target is full\n";
    t += "record: " + r.manifest + "\n";
    emit(v, t);
    return r.stopped || r.errors ? 1 : 0;
}

int cmd_status(int argc, char** argv) {
    Args a = parse(argc, argv, {}, {});
    if (a.pos.size() != 1) throw Usage("which folder?");
    const std::string dir = win::absolute(a.pos[0]);
    Manifest m(dir);
    const auto s = m.summary(dir);
    const auto all = m.summary("");
    Value v = Value::object({{"folder", dir}, {"manifest", m.path()}, {"files", s.files}, {"bytes", s.bytes}, {"drive_files", all.files},
                             {"drive_bytes", all.bytes}, {"runs", all.runs}, {"last_run", all.last_run}});
    emit(v, "copied by kairocopy under " + dir + ": " + std::to_string(s.files) + " files, " + gb(s.bytes) + "\non the whole drive: " +
                std::to_string(all.files) + " files, " + gb(all.bytes) + ", " + std::to_string(all.runs) + " runs, the last " + all.last_run +
                "\nrecord: " + m.path() + "\n");
    return 0;
}

BOOL WINAPI on_ctrl(DWORD) {
    if (g_stop) return FALSE;                                     // twice: the default (end now)
    g_stop = true;
    std::fprintf(stderr, "\nstopping: what is copied is kept; run it again to go on\n");
    return TRUE;
}
}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--json")) g_json = true;
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) { std::printf("%s", kUsage); return argc < 2 ? 2 : 0; }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    const std::string cmd = argv[1];
    try {
        if (cmd == "analyze") return cmd_analyze(argc, argv);
        if (cmd == "copy") return cmd_copy(argc, argv);
        if (cmd == "status") return cmd_status(argc, argv);
        throw Usage("unknown command " + cmd);
    } catch (const Usage& e) {
        if (g_json) std::printf("%s\n", Value::object({{"ok", false}, {"error", Value::object({{"code", "usage"}, {"message", e.what()}})}}).dump(2).c_str());
        else std::fprintf(stderr, "kairocopy-cli: %s\n%s", e.what(), kUsage);
        return 2;
    } catch (const std::exception& e) {
        if (g_json) std::printf("%s\n", Value::object({{"ok", false}, {"error", Value::object({{"code", "failed"}, {"message", e.what()}})}}).dump(2).c_str());
        else std::fprintf(stderr, "kairocopy-cli: %s\n", e.what());
        return 1;
    }
}
