// copier.cpp — see copier.hpp. Each file goes through CopyFile2 (the system's own copy: large unbuffered transfers,
// attributes and times kept), to a temporary name, then renamed.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xxhash.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include "core/copier.hpp"
#include "core/manifest.hpp"
#include "win/sys.hpp"

namespace kc {

namespace {
using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// the folders a file needs, made once each
class Dirs {
  public:
    bool make(const std::string& dir) {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (made_.count(dir)) return true;
        }
        const std::wstring w = win::longw(dir);
        if (!CreateDirectoryW(w.c_str(), nullptr)) {
            const DWORD e = GetLastError();
            if (e == ERROR_PATH_NOT_FOUND) {
                const std::string up = win::parent_of(dir);
                if (up.empty() || !make(up)) return false;
                if (!CreateDirectoryW(w.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
            } else if (e != ERROR_ALREADY_EXISTS) return false;
        }
        std::lock_guard<std::mutex> l(mu_);
        made_.insert(dir);
        return true;
    }
  private:
    std::mutex mu_;
    std::set<std::string> made_;
};

// xxh3-128 of a file, read past the system's cache (so that the target's copy is read from the disk, not from memory)
bool hash_file(const std::string& path, std::string& hex, std::string& error) {
    HANDLE h = CreateFileW(win::longw(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { error = win::error_text(GetLastError()); return false; }
    const DWORD kBuf = 4u << 20;                                 // a multiple of any sector size
    void* buf = VirtualAlloc(nullptr, kBuf, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XXH3_state_t* st = XXH3_createState();
    XXH3_128bits_reset(st);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, kBuf, &got, nullptr)) { error = win::error_text(GetLastError()); ok = false; break; }
        if (!got) break;
        XXH3_128bits_update(st, buf, got);
        if (got < kBuf) break;
    }
    const XXH128_hash_t r = XXH3_128bits_digest(st);
    XXH3_freeState(st);
    VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(h);
    if (ok) { char b[40]; snprintf(b, sizeof b, "%016llx%016llx", (unsigned long long)r.high64, (unsigned long long)r.low64); hex = b; }
    return ok;
}

struct Shared {
    const Plan& plan;
    const CopyOptions& opt;
    std::atomic<int64_t> bytes{0}, files{0}, errors{0}, in_use{0};
    std::atomic<bool> disk_full{false};
    std::mutex mu;
    std::vector<CopiedFile> done;                                // to record
    std::vector<std::pair<std::string, std::string>> failed;
    // what is under way, per writer: its file and how far
    struct Now { std::atomic<const Item*> item{nullptr}; std::atomic<uint64_t> at{0}; };
    std::unique_ptr<Now[]> now;
    size_t writers = 0;
    Dirs dirs;
    Shared(const Plan& p, const CopyOptions& o) : plan(p), opt(o) {}
    bool stopping() const { return (opt.stop && opt.stop->load()) || disk_full.load(); }
    void wait_while_paused() const { while (opt.pause && opt.pause->load() && !stopping()) Sleep(100); }
};

struct Ctx { Shared* sh; size_t slot; uint64_t last = 0; };

COPYFILE2_MESSAGE_ACTION CALLBACK on_progress(const COPYFILE2_MESSAGE* m, PVOID p) {
    auto* c = static_cast<Ctx*>(p);
    if (m->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED) {
        const uint64_t at = m->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
        c->sh->bytes += int64_t(at - c->last);
        c->last = at;
        c->sh->now[c->slot].at = at;
    }
    c->sh->wait_while_paused();
    return c->sh->stopping() ? COPYFILE2_PROGRESS_CANCEL : COPYFILE2_PROGRESS_CONTINUE;
}

void copy_one(Shared& sh, size_t slot, const Item& it) {
    auto fail = [&](const std::string& why) {
        sh.errors++;
        std::lock_guard<std::mutex> l(sh.mu);
        if (sh.failed.size() < 100000) sh.failed.push_back({it.src, why});
    };
    if (!sh.dirs.make(win::parent_of(it.dst))) { fail("cannot make the folder " + win::parent_of(it.dst) + ": " + win::error_text(GetLastError())); return; }
    const std::wstring src = win::longw(it.src), dst = win::longw(it.dst), tmp = dst + kPartSuffix;
    sh.now[slot].at = 0;
    sh.now[slot].item = &it;
    Ctx ctx{&sh, slot, 0};
    COPYFILE2_EXTENDED_PARAMETERS p{};
    p.dwSize = sizeof p;
    p.dwCopyFlags = COPY_FILE_ALLOW_DECRYPTED_DESTINATION | (it.size >= kBigFile ? COPY_FILE_NO_BUFFERING : 0);
    p.pProgressRoutine = on_progress;
    p.pvCallbackContext = &ctx;
    const HRESULT hr = CopyFile2(src.c_str(), tmp.c_str(), &p);
    sh.now[slot].item = nullptr;
    if (FAILED(hr)) {
        sh.bytes -= int64_t(ctx.last);                           // what went before the failure is not copied
        DeleteFileW(tmp.c_str());
        const DWORD e = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : DWORD(hr);
        if (e == ERROR_REQUEST_ABORTED || hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED)) return;   // stopped
        if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) { sh.disk_full = true; fail("the target is full"); return; }
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) { sh.in_use++; fail("in use by another program"); return; }
        fail(win::error_text(e));
        return;
    }
    // files of size 0 report no chunk
    sh.bytes += int64_t(it.size - ctx.last);
    std::string hash;
    if (sh.opt.verify) {
        std::string a, b, err;
        if (!hash_file(it.src, a, err) || !hash_file(it.dst + win::narrow(std::wstring(kPartSuffix)), b, err)) { DeleteFileW(tmp.c_str()); sh.bytes -= int64_t(it.size); fail("checking: " + err); return; }
        if (a != b) { DeleteFileW(tmp.c_str()); sh.bytes -= int64_t(it.size); fail("the copy differs from the original"); return; }
        hash = a;
    }
    if (it.action == Action::Overwrite) SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = GetLastError();
        DeleteFileW(tmp.c_str());
        sh.bytes -= int64_t(it.size);
        fail("cannot put it in place: " + win::error_text(e));
        return;
    }
    sh.files++;
    std::lock_guard<std::mutex> l(sh.mu);
    sh.done.push_back(CopiedFile{it.dst, it.src, it.size, it.mtime, hash});
}
}  // namespace

CopyReport copy(const Plan& plan, const CopyOptions& opt, const std::function<void(const CopyProgress&)>& progress) {
    const auto t0 = Clock::now();
    CopyReport rep;
    Manifest manifest(plan.dest);
    rep.manifest = manifest.path();
    std::string srcs;
    for (auto& s : plan.sources) srcs += (srcs.empty() ? "" : "|") + s;
    const int64_t run = manifest.begin_run(srcs, plan.dest, plan.todo.files, plan.todo.bytes);

    // two queues, both in the order of the target's paths
    std::vector<const Item*> big, small;
    for (auto& it : plan.items) (it.size >= kBigFile ? big : small).push_back(&it);
    const int nbig = opt.big_writers > 0 ? opt.big_writers : (plan.target_seeks ? 1 : 2);
    // measured on a USB hard disk (exFAT): small files 61 a second with one writer, 86 with 8 (no gain past that)
    const int nsmall = opt.small_writers > 0 ? opt.small_writers : 8;
    Shared sh(plan, opt);
    sh.writers = size_t(nbig + nsmall);
    sh.now = std::make_unique<Shared::Now[]>(sh.writers);
    std::atomic<size_t> next_big{0}, next_small{0};
    std::atomic<int> running{0};
    auto writer = [&](size_t slot, std::vector<const Item*>* mine, std::atomic<size_t>* next, std::vector<const Item*>* other, std::atomic<size_t>* onext) {
        // a hard disk: the large files after the small ones (both at once: the heads go back and forth between them)
        if (plan.target_seeks && mine == &big)
            while (next_small.load() < small.size() && !sh.stopping()) Sleep(50);
        for (;;) {
            sh.wait_while_paused();
            if (sh.stopping()) break;
            size_t k = (*next)++;
            const Item* it = nullptr;
            if (k < mine->size()) it = (*mine)[k];
            else {
                // its queue is empty: help with the other one, except on a hard disk, where a second large file written at
                // the same time only makes the heads go back and forth (measured: 24 MB/s instead of 41)
                if (plan.target_seeks && mine == &small) break;
                k = (*onext)++;
                if (k < other->size()) it = (*other)[k]; else break;
            }
            copy_one(sh, slot, *it);
        }
        running--;
    };
    std::vector<std::thread> pool;
    running = nbig + nsmall;
    for (int i = 0; i < nbig; i++) pool.emplace_back(writer, size_t(i), &big, &next_big, &small, &next_small);
    for (int i = 0; i < nsmall; i++) pool.emplace_back(writer, size_t(nbig + i), &small, &next_small, &big, &next_big);

    std::deque<std::pair<double, int64_t>> window;              // (time, bytes) samples for the speed
    auto last_record = Clock::now();
    CopyProgress pr;
    pr.files_total = plan.todo.files;
    pr.bytes_total = plan.todo.bytes;
    for (bool more = true; more;) {
        more = running > 0;
        if (more) Sleep(200);
        if (!more || since(last_record) > 1.0) {
            std::vector<CopiedFile> batch;
            { std::lock_guard<std::mutex> l(sh.mu); batch.swap(sh.done); }
            manifest.record(batch);
            last_record = Clock::now();
        }
        const double t = since(t0);
        window.push_back({t, sh.bytes.load()});
        while (window.size() > 2 && t - window.front().first > 3.0) window.pop_front();
        pr.seconds = t;
        pr.files_done = sh.files; pr.bytes_done = sh.bytes; pr.errors = sh.errors;
        pr.speed = window.size() > 1 && window.back().first > window.front().first
                       ? double(window.back().second - window.front().second) / (window.back().first - window.front().first) : 0;
        pr.paused = opt.pause && opt.pause->load();
        // the largest file under way
        pr.current.clear(); pr.current_done = pr.current_size = 0;
        for (size_t i = 0; i < sh.writers; i++) {
            const Item* it = sh.now[i].item;
            if (it && it->size >= pr.current_size) { pr.current = it->src; pr.current_size = it->size; pr.current_done = sh.now[i].at; }
        }
        if (progress) progress(pr);
    }
    for (auto& th : pool) th.join();
    rep.files = sh.files; rep.bytes = sh.bytes; rep.errors = sh.errors; rep.in_use = sh.in_use;
    rep.failed = std::move(sh.failed);
    rep.disk_full = sh.disk_full;
    rep.stopped = (opt.stop && opt.stop->load()) || rep.disk_full;
    rep.seconds = since(t0);
    manifest.end_run(run, rep.files, rep.bytes, rep.errors, rep.stopped);
    return rep;
}

}  // namespace kc
