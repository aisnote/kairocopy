// tests.cpp — the engine on a made-up tree: analyse, copy, nothing copied twice, a newer file copied again, the rules.
//   kc_tests                       the checks
//   kc_tests --speed <dir> <n>     a tree of n small files and a few large ones under <dir>\kc_speed_src, copied to
//                                  <dir>\kc_speed_dst on the same drive... or pass the target with --to
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include "core/copier.hpp"
#include "core/manifest.hpp"
#include "core/plan.hpp"
#include "win/sys.hpp"

using namespace kc;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void write(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}
static std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static void touch_later(const fs::path& p, int seconds) {
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(seconds));
}

static AnalyzeOptions opts(bool important_only, bool strict = false) {
    AnalyzeOptions o;
    o.important_only = important_only;
    o.strict = strict;
    return o;
}
static CopyOptions verify_opts() { CopyOptions o; o.verify = true; return o; }
static CopyOptions stop_opts(std::atomic<bool>* stop) { CopyOptions o; o.stop = stop; return o; }
static void set_time(const fs::path& p, int64_t ticks) {
    HANDLE h = CreateFileW(p.wstring().c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME ft{DWORD(uint64_t(ticks) & 0xffffffff), DWORD(uint64_t(ticks) >> 32)};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}
static int64_t get_time(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &d);
    return int64_t((uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime);
}
static std::string noise(size_t n, unsigned seed) {
    std::mt19937 r(seed);
    std::string s(n, '\0');
    for (auto& c : s) c = char(r() % 251 + 1);       // never 0: a block of zeros is "not written yet"
    return s;
}

// A target that another program filled halfway: each kind of file left behind is told apart.
static void others() {
    const fs::path base = fs::temp_directory_path() / ("kc_others_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(base);
    const fs::path src = base / "src" / "Pics", dst = base / "dst", d = dst / "Pics";
    const std::string trunc = noise(3u << 20, 1), pre = noise(3u << 20, 2), robo = noise(1u << 20, 3), same = noise(2u << 20, 4), ed = noise(1u << 20, 5);
    const std::string big = noise(80u << 20, 6), bigsame = noise(70u << 20, 7);
    write(src / "trunc.bin", trunc);
    write(src / "prealloc.bin", pre);
    write(src / "robo.bin", robo);
    write(src / "notime.jpg", same);
    write(src / "edited.doc", ed);
    write(src / "big.mp4", big);
    write(src / "bigsame.mp4", bigsame);
    write(src / "x.txt", "x");
    // the sources a day old; what the other program left: written now (newer)
    for (auto& e : fs::directory_iterator(src)) touch_later(e.path(), -86400);
    write(d / "trunc.bin", trunc.substr(0, 1u << 20));                                   // stopped after 1 MB
    write(d / "prealloc.bin", pre.substr(0, 2u << 20) + std::string(1u << 20, '\0'));    // full size, the end not written
    write(d / "robo.bin", robo);
    set_time(d / "robo.bin", 119600064000000000LL);                                       // robocopy's 1980-01-01 mark
    write(d / "notime.jpg", same);                                                         // complete, another time
    std::string ed2 = ed;
    ed2[500000] ^= 0x55;
    write(d / "edited.doc", ed2);                                                          // changed at the target
    write(d / "big.mp4", big.substr(0, 60u << 20) + std::string(20u << 20, '\0'));
    write(d / "bigsame.mp4", bigsame);
    write(d / "x.txt.kcpart", "half");                                                     // ours, of an earlier run
    write(d / "gone.txt.kcpart", "half");
    write(d / "extra.txt", "only at the target");

    Plan p = analyze({src.string()}, dst.string(), opts(false), nullptr);
    CHECK(p.incomplete.files == 4);          // trunc, prealloc, robo, big
    CHECK(p.same_content.files == 2);        // notime (whole), bigsame (sampled)
    CHECK(p.same_sampled.files == 1);
    CHECK(p.conflict.files == 1 && p.conflicts.size() == 1 && fs::path(p.conflicts[0].dst).filename() == "edited.doc");
    CHECK(p.fresh.files == 1);               // x.txt
    CHECK(p.target_only.files == 1);
    CHECK(p.stale_parts.size() == 2);
    CHECK(p.checked.files == 6);             // all but robo (marked) and x.txt (new) were read
    CHECK(p.checked.bytes < (40ll << 20));   // the 80 and 70 MB ones in samples

    const int64_t robo_src = get_time(src / "notime.jpg");
    CopyReport r = copy(p, CopyOptions{}, nullptr);
    CHECK(r.errors == 0 && r.files == 5 && r.times_set == 1 && r.parts_removed == 2);
    CHECK(read(d / "trunc.bin") == trunc && read(d / "prealloc.bin") == pre && read(d / "robo.bin") == robo && read(d / "big.mp4") == big);
    CHECK(get_time(d / "notime.jpg") == robo_src);          // given the source's time
    CHECK(read(d / "edited.doc") == ed2);                    // a conflict: left alone
    CHECK(!fs::exists(d / "x.txt.kcpart") && !fs::exists(d / "gone.txt.kcpart") && read(d / "x.txt") == "x");
    CHECK(read(d / "extra.txt") == "only at the target");

    // again: nothing left but the conflict, and bigsame (sampled, so its time was not set: compared again)
    Plan again = analyze({src.string()}, dst.string(), opts(false), nullptr);
    CHECK(again.todo.files == 0 && again.conflict.files == 1 && again.same_sampled.files == 1 && again.same.files == 6);
    // strict: bigsame compared whole, so its time is set
    Plan strict = analyze({src.string()}, dst.string(), opts(false, true), nullptr);
    CHECK(strict.same_sampled.files == 0 && strict.same_content.files == 1);
    CHECK(copy(strict, CopyOptions{}, nullptr).times_set == 1);
    CHECK(analyze({src.string()}, dst.string(), opts(false), nullptr).same.files == 7);
    // the conflict, both kept
    Plan both = analyze({src.string()}, dst.string(), opts(false), nullptr);
    resolve_conflicts(both, ConflictPolicy::KeepBoth);
    CHECK(both.todo.files == 1 && both.conflicts.empty());
    CHECK(copy(both, CopyOptions{}, nullptr).files == 1);
    CHECK(read(d / "edited.doc") == ed2 && read(d / u8"edited（来自源）.doc") == ed);

    // a run stopped is found unfinished, until a run of the same finishes
    write(src / "new.txt", "new");
    std::atomic<bool> stop{true};
    Plan ps = analyze({src.string()}, dst.string(), opts(false), nullptr);
    copy(ps, stop_opts(&stop), nullptr);
    const std::string mpath = Manifest(dst.string()).path();
    Manifest::Run run;
    CHECK(Manifest::unfinished(mpath, run) && run.sources == src.string() && run.dest == dst.string());
    copy(analyze({src.string()}, dst.string(), opts(false), nullptr), CopyOptions{}, nullptr);
    CHECK(!Manifest::unfinished(mpath, run) || run.dest != dst.string());   // (the stopped run of checks() may be there)

    std::error_code ec;
    fs::remove_all(base, ec);
}

static int checks() {
    // the copy records in a folder of the tests' own, not at the root of the drive
    const fs::path state = fs::temp_directory_path() / ("kc_state_" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(state);
    SetEnvironmentVariableW(L"KAIROCOPY_STATE", state.wstring().c_str());
    _wputenv_s(L"KAIROCOPY_STATE", state.wstring().c_str());
    const fs::path base = fs::temp_directory_path() / ("kc_test_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(base);
    const fs::path src = base / "src" / "Docs", dst = base / "dst";
    write(src / "a.txt", "hello");
    write(src / u8"照片" / "IMG_1.jpg", std::string(1000, 'j'));
    write(src / "sub" / "deep" / "notes.md", "# notes");
    write(src / "proj" / "node_modules" / "x" / "index.js", "skip me");
    write(src / "AppData" / "Local" / "Temp" / "t.tmp", "skip me too");
    write(src / "thing.bin", "other");
    write(src / "Thumbs.db", "never");
    std::string big(20u << 20, 'b');
    for (size_t i = 0; i < big.size(); i += 4096) big[i] = char(i / 4096);
    write(src / "video.mp4", big);
    fs::permissions(src / "a.txt", fs::perms::owner_write, fs::perm_options::remove);   // read-only

    // 1. analyse
    Plan p = analyze({src.string()}, dst.string(), opts(false), nullptr);
    CHECK(p.all.files == 5);                        // node_modules, AppData\Local\Temp and Thumbs.db left out
    CHECK(p.todo.files == 5 && p.fresh.files == 5);
    CHECK(p.by_cat[int(Cat::Photo)].files == 1 && p.by_cat[int(Cat::Video)].files == 1 && p.by_cat[int(Cat::Document)].files == 2);
    CHECK(p.skipped_dirs == 2);
    Plan pi = analyze({src.string()}, dst.string(), opts(true), nullptr);
    CHECK(pi.todo.files == 4 && pi.not_important.files == 1);   // thing.bin is "other"

    // 2. copy
    CopyReport r = copy(p, CopyOptions{}, nullptr);
    CHECK(r.files == 5 && r.errors == 0);
    CHECK(read(dst / "Docs" / "a.txt") == "hello");
    CHECK(read(dst / "Docs" / "video.mp4") == big);
    CHECK(read(dst / "Docs" / u8"照片" / "IMG_1.jpg").size() == 1000);
    CHECK(!fs::exists(dst / "Docs" / "proj" / "node_modules"));
    CHECK(fs::last_write_time(dst / "Docs" / "video.mp4") == fs::last_write_time(src / "video.mp4"));
    bool part = false;
    for (auto& e : fs::recursive_directory_iterator(dst)) if (e.path().extension() == ".kcpart") part = true;
    CHECK(!part);

    // 3. nothing twice
    Plan again = analyze({src.string()}, dst.string(), opts(false), nullptr);
    CHECK(again.todo.files == 0 && again.same.files == 5);

    // 4. a newer source is copied again (over a read-only copy too); a newer target is left alone
    write(src / "sub" / "deep" / "notes.md", "# notes, longer");
    touch_later(src / "sub" / "deep" / "notes.md", 10);
    fs::permissions(src / "a.txt", fs::perms::owner_write, fs::perm_options::add);
    write(src / "a.txt", "hello again");
    touch_later(src / "a.txt", 10);
    write(dst / "Docs" / "thing.bin", "changed at the target");
    touch_later(dst / "Docs" / "thing.bin", 60);
    Plan p2 = analyze({src.string()}, dst.string(), opts(false), nullptr);
    CHECK(p2.overwrite.files == 2 && p2.conflict.files == 1 && p2.todo.files == 2);   // thing.bin: larger and newer at the target
    CopyReport r2 = copy(p2, verify_opts(), nullptr);
    CHECK(r2.files == 2 && r2.errors == 0);
    CHECK(read(dst / "Docs" / "sub" / "deep" / "notes.md") == "# notes, longer");
    CHECK(read(dst / "Docs" / "a.txt") == "hello again");
    CHECK(read(dst / "Docs" / "thing.bin") == "changed at the target");

    // 5. the record
    Manifest m(dst.string());
    const auto s = m.summary((dst / "Docs").string());
    CHECK(s.files == 5);
    CHECK(s.runs >= 2);

    // 6. a single file as the source
    Plan one = analyze({(src / "a.txt").string()}, (base / "one").string(), opts(false), nullptr);
    CHECK(one.todo.files == 1);
    CHECK(copy(one, CopyOptions{}, nullptr).files == 1);
    CHECK(read(base / "one" / "a.txt") == "hello again");

    // 7. stopped at once: nothing half there
    std::atomic<bool> stop{true};
    Plan p3 = analyze({src.string()}, (base / "stopped").string(), opts(false), nullptr);
    CopyReport r3 = copy(p3, stop_opts(&stop), nullptr);
    CHECK(r3.stopped && r3.files == 0);

    fs::permissions(dst / "Docs" / "a.txt", fs::perms::owner_write, fs::perm_options::add);
    std::error_code ec;
    fs::remove_all(base, ec);
    others();
    fs::remove_all(state, ec);
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

// --speed <source-dir> <target-dir> <small-files> <big-files> [big MB]
static int speed(const std::string& srcdir, const std::string& dstdir, int nsmall, int nbig, int big_mb) {
    const fs::path src = fs::path(srcdir) / "kc_speed", dst = fs::path(dstdir);
    std::mt19937 rng(1);
    if (!fs::exists(src)) {
        std::printf("making %d small files and %d of %d MB under %s\n", nsmall, nbig, big_mb, src.string().c_str());
        std::string buf;
        for (int i = 0; i < nsmall; i++) {
            buf.resize(4000 + rng() % 400000);
            for (size_t k = 0; k < buf.size(); k += 64) buf[k] = char(rng());
            write(src / ("d" + std::to_string(i / 500)) / ("f" + std::to_string(i) + ".jpg"), buf);
        }
        buf.assign(size_t(big_mb) << 20, 'x');
        for (int i = 0; i < nbig; i++) {
            for (size_t k = 0; k < buf.size(); k += 4096) buf[k] = char(rng());
            write(src / "big" / ("v" + std::to_string(i) + ".mp4"), buf);
        }
    }
    Plan p = analyze({src.string()}, dst.string(), opts(false), nullptr);
    std::printf("analysed %lld files (%.2f GB) in %.2f s; to copy %lld; target %s\n", (long long)p.all.files, p.all.bytes / 1e9, p.seconds,
                (long long)p.todo.files, p.target_seeks ? "a hard disk" : "an SSD");
    const char* w = std::getenv("KC_WRITERS");
    CopyOptions o;
    if (w) { o.big_writers = std::atoi(w); const char* c = std::strchr(w, ','); o.small_writers = c ? std::atoi(c + 1) : o.big_writers; }
    CopyReport r = copy(p, o, nullptr);
    std::printf("copied %lld files, %.2f GB in %.1f s: %.0f MB/s, %.0f files/s, %lld errors\n", (long long)r.files, r.bytes / 1e9, r.seconds,
                r.bytes / r.seconds / 1e6, r.files / r.seconds, (long long)r.errors);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 6 && std::string(argv[1]) == "--speed")
        return speed(argv[2], argv[3], std::atoi(argv[4]), std::atoi(argv[5]), argc > 6 ? std::atoi(argv[6]) : 512);
    return checks();
}
