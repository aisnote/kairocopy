// plan.cpp — see plan.hpp. The folders are listed with FindFirstFileExW (basic information, large fetches): sizes and
// times come with the listing, no call per file. Only the files whose class that leaves open are read.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "core/plan.hpp"
#include "win/sys.hpp"

namespace kc {

namespace {
using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
int64_t ticks(const FILETIME& f) { return int64_t((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime); }
// times on exFAT and FAT are kept to 10 ms and 2 s: within that they are the same
constexpr int64_t kSameTime = 2 * win::kTicksPerSecond;
// robocopy gives a file it has not finished the time 1980-01-01 (so that it is copied again): anything before 1980-01-02
constexpr int64_t kRobocopyMark = 119600928000000000LL;
constexpr uint64_t kBlock = 1u << 20, kWholeUpTo = 64ull << 20;

void lower(std::wstring& s) { if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size())); }

struct Entry { uint64_t size; int64_t mtime; };

// every file under `dir` (the target side: no rules), keys in lower case; our own .kcpart files apart
void list_target(const std::string& dir, std::unordered_map<std::string, Entry>& out, std::vector<std::string>& parts) {
    struct D { std::wstring path; std::string key, name; };    // the path to list, its key (lower case, no \\?\), its path
    std::vector<D> stack{{win::longw(dir), win::key_of(dir), dir}};
    const size_t np = wcslen(kPartSuffix);
    while (!stack.empty()) {
        const D d = std::move(stack.back());
        stack.pop_back();
        WIN32_FIND_DATAW f;
        HANDLE h = FindFirstFileExW((d.path + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
            std::wstring nl = f.cFileName;
            lower(nl);
            std::string key = d.key + "\\" + win::narrow(nl);
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    stack.push_back(D{d.path + L"\\" + f.cFileName, std::move(key), win::join(d.name, win::narrow(std::wstring(f.cFileName)))});
                continue;
            }
            if (nl.size() > np && nl.compare(nl.size() - np, np, kPartSuffix) == 0) {
                parts.push_back(win::join(d.name, win::narrow(std::wstring(f.cFileName))));
                continue;
            }
            out[std::move(key)] = Entry{(uint64_t(f.nFileSizeHigh) << 32) | f.nFileSizeLow, ticks(f.ftLastWriteTime)};
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
}

// the name a source gets at the target: its own, or the drive letter for a drive's root
std::string target_name(const std::string& src) {
    if (src.size() <= 3 && src.size() >= 2 && src[1] == ':') return std::string(1, char(std::toupper(static_cast<unsigned char>(src[0]))));
    return win::name_of(src);
}

// ---------------------------------------------------------------------------------------------- the content check
enum class Verdict { Same, Incomplete, Different, Error };

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit File(const std::string& path) {
        h = CreateFileW(win::longw(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_RANDOM_ACCESS, nullptr);
    }
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    bool read(uint64_t at, char* buf, DWORD len) {
        OVERLAPPED o{};
        o.Offset = DWORD(at & 0xffffffff);
        o.OffsetHigh = DWORD(at >> 32);
        DWORD got = 0;
        return ReadFile(h, buf, len, &got, &o) && got == len;
    }
};

// The target's file against the source's, over the target's length: the blocks compared are all of them (small files,
// strict) or a sample. A block of the target that is all zeros where the source has data: not yet written.
Verdict compare(const std::string& src, const std::string& dst, uint64_t dst_size, bool whole, int64_t& read, std::string& error) {
    File a(src), b(dst);
    if (!a.ok() || !b.ok()) { error = win::error_text(GetLastError()); return Verdict::Error; }
    const uint64_t nblocks = (dst_size + kBlock - 1) / kBlock;
    std::set<uint64_t> blocks;
    if (whole) for (uint64_t i = 0; i < nblocks; i++) blocks.insert(i);
    else {
        for (uint64_t i : {uint64_t(0), uint64_t(1), nblocks - 2, nblocks - 1}) if (i < nblocks) blocks.insert(i);
        for (int k = 1; k <= 8; k++) blocks.insert(nblocks * uint64_t(k) / 9);
    }
    std::vector<char> x(kBlock), y(kBlock);
    bool zeros = false;
    for (uint64_t i : blocks) {
        const uint64_t at = i * kBlock;
        const DWORD len = DWORD(std::min<uint64_t>(kBlock, dst_size - at));
        if (!a.read(at, x.data(), len) || !b.read(at, y.data(), len)) { error = win::error_text(GetLastError()); return Verdict::Error; }
        read += len;
        if (!memcmp(x.data(), y.data(), len)) continue;
        if (std::all_of(y.begin(), y.begin() + len, [](char c) { return c == 0; })) { zeros = true; continue; }
        return Verdict::Different;
    }
    return zeros ? Verdict::Incomplete : Verdict::Same;
}

struct Pending { Item item; uint64_t dst_size = 0; Verdict verdict = Verdict::Error; bool whole = false; std::string error; };
}  // namespace

Plan analyze(const std::vector<std::string>& sources, const std::string& dest_in, const AnalyzeOptions& opt,
             const std::function<void(const AnalyzeProgress&)>& progress) {
    const auto t0 = Clock::now();
    Plan p;
    p.dest = win::absolute(dest_in);
    p.important_only = opt.important_only;
    p.strict = opt.strict;
    win::free_space(p.dest, p.free_bytes);
    win::has_seek_penalty(p.dest, p.target_seeks);
    AnalyzeProgress pr;
    auto last = Clock::now();
    std::vector<Pending> pending;                        // to read before they are classed

    auto add = [&](Item it, Counts& kind) {
        kind.add(it.size);
        p.todo.add(it.size);
        p.todo_by_cat[int(it.cat)].add(it.size);
        p.items.push_back(std::move(it));
    };

    for (const auto& s_in : sources) {
        const std::string src = win::absolute(s_in);
        p.sources.push_back(src);
        const std::string dst_root = win::join(p.dest, target_name(src));
        // what the target already has, keyed by lower-case path
        std::unordered_map<std::string, Entry> have;
        std::vector<std::string> parts;
        if (win::is_dir(dst_root)) list_target(dst_root, have, parts);
        else {
            WIN32_FILE_ATTRIBUTE_DATA da;                // a single file's target
            if (GetFileAttributesExW(win::longw(dst_root).c_str(), GetFileExInfoStandard, &da) && !(da.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                have[win::key_of(dst_root)] = Entry{(uint64_t(da.nFileSizeHigh) << 32) | da.nFileSizeLow, ticks(da.ftLastWriteTime)};
        }
        std::unordered_set<std::string> matched;

        auto consider = [&](const std::wstring& name, const std::wstring& dir_lower, const std::string& src_path, const std::string& dst_path,
                            uint64_t size, int64_t mtime, DWORD attrs) {
            std::wstring nl = name;
            lower(nl);
            if (skip_file(nl)) return;
            if (attrs & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) { p.online_only.add(size); return; }
            const Cat cat = category_of(nl, dir_lower + L"\\" + nl);
            p.all.add(size);
            p.by_cat[int(cat)].add(size);
            pr.files++; pr.bytes += int64_t(size);
            if (opt.important_only && !important(cat)) { p.not_important.add(size); return; }
            Item item{src_path, dst_path, size, mtime, cat, Action::Copy};
            const std::string key = win::key_of(dst_path);
            auto it = have.find(key);
            if (it == have.end()) { add(std::move(item), p.fresh); return; }
            matched.insert(key);
            const Entry& e = it->second;
            const int64_t dt = mtime - e.mtime;
            if (e.size == size && dt <= kSameTime && dt >= -kSameTime) { p.same.add(size); return; }
            item.action = Action::Overwrite;
            if (e.mtime < kRobocopyMark) { add(std::move(item), p.incomplete); return; }     // robocopy's "not finished"
            if (dt > kSameTime) { add(std::move(item), p.overwrite); return; }              // the source is newer
            // the target's is newer (or of the same time and another size): what it holds decides
            if (e.size > size) { p.conflict.add(size); p.conflicts.push_back(std::move(item)); return; }
            Pending q;
            q.item = std::move(item);
            q.dst_size = e.size;
            pending.push_back(std::move(q));
        };

        // a single file
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(win::longw(src).c_str(), GetFileExInfoStandard, &fa)) {
            p.errors.push_back({src, win::error_text(GetLastError())});
            continue;
        }
        if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring dir_lower = win::widen(win::parent_of(src));
            lower(dir_lower);
            consider(win::widen(win::name_of(src)), dir_lower, src, dst_root, (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow,
                     ticks(fa.ftLastWriteTime), fa.dwFileAttributes);
        } else {
            // a folder, depth first; the folder's own name is not checked against the rules (it was chosen)
            struct Dir { std::wstring wsrc; std::string src, dst; std::wstring lower_path; bool drive_root; };
            std::wstring root_lower = win::widen(src);
            lower(root_lower);
            std::vector<Dir> stack{{win::longw(src), src, dst_root, root_lower, src.size() <= 3}};
            while (!stack.empty()) {
                if (opt.stop && opt.stop->load()) { p.stopped = true; break; }
                Dir d = std::move(stack.back());
                stack.pop_back();
                WIN32_FIND_DATAW f;
                HANDLE h = FindFirstFileExW((d.wsrc + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (h == INVALID_HANDLE_VALUE) {
                    const DWORD e = GetLastError();
                    if (e != ERROR_FILE_NOT_FOUND && p.errors.size() < 200) p.errors.push_back({d.src, win::error_text(e)});
                    continue;
                }
                std::vector<Dir> subdirs;
                do {
                    if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
                    const std::wstring name = f.cFileName;
                    const std::string name8 = win::narrow(name);
                    if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        // junctions and links (the old "My Documents" and "Application Data" ones loop back): not followed
                        if (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                        std::wstring nl = name;
                        lower(nl);
                        if (skip_dir(nl, d.lower_path, d.drive_root)) { p.skipped_dirs++; continue; }
                        subdirs.push_back(Dir{d.wsrc + L"\\" + name, win::join(d.src, name8), win::join(d.dst, name8), d.lower_path + L"\\" + nl, false});
                        continue;
                    }
                    consider(name, d.lower_path, win::join(d.src, name8), win::join(d.dst, name8), (uint64_t(f.nFileSizeHigh) << 32) | f.nFileSizeLow,
                             ticks(f.ftLastWriteTime), f.dwFileAttributes);
                } while (FindNextFileW(h, &f));
                FindClose(h);
                std::sort(subdirs.begin(), subdirs.end(), [](const Dir& a, const Dir& b) { return a.lower_path > b.lower_path; });
                for (auto& s : subdirs) stack.push_back(std::move(s));
                if (progress && since(last) > 0.25) { last = Clock::now(); pr.dir = d.src; progress(pr); }
            }
        }
        // what the target has that the sources do not (left alone), and our own half-copied files of an earlier run: those
        // whose file is complete now, or will be written again, go
        for (auto& [key, e] : have) if (!matched.count(key)) p.target_only.add(e.size);
        for (auto& part : parts) p.stale_parts.push_back(part);
        if (p.stopped) break;
    }

    // the content check, a few files at a time (the target is read: one or two readers for a hard disk)
    if (!pending.empty() && !p.stopped) {
        pr.checking = true;
        pr.check_total = int64_t(pending.size());
        std::atomic<size_t> next{0};
        std::atomic<int64_t> done{0}, read{0};
        auto worker = [&] {
            for (;;) {
                if (opt.stop && opt.stop->load()) return;
                const size_t k = next++;
                if (k >= pending.size()) return;
                Pending& q = pending[k];
                q.whole = opt.strict || q.dst_size <= kWholeUpTo;
                int64_t r = 0;
                q.verdict = compare(q.item.src, q.item.dst, q.dst_size, q.whole, r, q.error);
                // smaller than the source and the same as far as it goes: the start of a copy
                if (q.verdict == Verdict::Same && q.dst_size < q.item.size) q.verdict = Verdict::Incomplete;
                read += r;
                done++;
            }
        };
        std::vector<std::thread> pool;
        const int n = p.target_seeks ? 2 : 4;
        for (int i = 0; i < n; i++) pool.emplace_back(worker);
        while (done < int64_t(pending.size()) && !(opt.stop && opt.stop->load())) {
            Sleep(200);
            pr.check_done = done;
            if (progress) progress(pr);
        }
        for (auto& t : pool) t.join();
        if (opt.stop && opt.stop->load()) p.stopped = true;
        p.checked.files = done;
        p.checked.bytes = read;
        for (auto& q : pending) {
            if (!q.whole && q.verdict != Verdict::Error) p.sampled.add(q.item.size);
            switch (q.verdict) {
            case Verdict::Same:
                p.same_content.add(q.item.size);
                if (q.whole) { q.item.action = Action::FixTime; p.items.push_back(std::move(q.item)); }
                else p.same_sampled.add(q.item.size);
                break;
            case Verdict::Incomplete: add(std::move(q.item), p.incomplete); break;
            case Verdict::Different: p.conflict.add(q.item.size); p.conflicts.push_back(std::move(q.item)); break;
            case Verdict::Error:
                // not read (stopped, or in use): left alone
                if (!q.error.empty() && p.errors.size() < 200) p.errors.push_back({q.item.dst, "cannot compare: " + q.error});
                break;
            }
        }
    }
    // copied in the order of the target's paths: a hard disk writes one folder after the other
    std::sort(p.items.begin(), p.items.end(), [](const Item& a, const Item& b) { return a.dst < b.dst; });
    p.seconds = since(t0);
    if (progress) { pr.dir.clear(); pr.check_done = pr.check_total; progress(pr); }
    return p;
}

void keep_important(Plan& p) {
    if (p.important_only) return;
    p.important_only = true;
    std::vector<Item> kept;
    kept.reserve(p.items.size());
    for (auto& it : p.items) {
        if (important(it.cat) || it.action == Action::FixTime) { kept.push_back(std::move(it)); continue; }
        if (it.action == Action::Copy) p.fresh.sub(it.size); else p.overwrite.sub(it.size);
        p.todo.sub(it.size);
    }
    p.items = std::move(kept);
    std::vector<Item> conflicts;
    for (auto& it : p.conflicts) { if (important(it.cat)) conflicts.push_back(std::move(it)); else p.conflict.sub(it.size); }
    p.conflicts = std::move(conflicts);
    const Counts other = p.by_cat[int(Cat::Other)];
    p.not_important.files += other.files; p.not_important.bytes += other.bytes;
    p.todo_by_cat[int(Cat::Other)] = Counts{};
}

void resolve_conflicts(Plan& p, ConflictPolicy policy) {
    if (policy == ConflictPolicy::Skip || p.conflicts.empty()) return;
    for (auto& it : p.conflicts) {
        if (policy == ConflictPolicy::KeepBoth) {
            // "name（来自源）.ext", or "name（来自源 2）.ext" ... when that is taken too
            const std::string dir = win::parent_of(it.dst), name = win::name_of(it.dst);
            const size_t dot = name.find_last_of('.');
            const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot), ext = stem.size() < name.size() ? name.substr(stem.size()) : "";
            for (int n = 1;; n++) {
                const std::string cand = win::join(dir, stem + "（来自源" + (n > 1 ? " " + std::to_string(n) : "") + "）" + ext);
                if (GetFileAttributesW(win::longw(cand).c_str()) == INVALID_FILE_ATTRIBUTES) { it.dst = cand; break; }
            }
            it.action = Action::Copy;
            p.fresh.add(it.size);
        } else {
            it.action = Action::Overwrite;
            p.overwrite.add(it.size);
        }
        p.todo.add(it.size);
        p.todo_by_cat[int(it.cat)].add(it.size);
        p.items.push_back(std::move(it));
    }
    p.conflicts.clear();
    std::sort(p.items.begin(), p.items.end(), [](const Item& a, const Item& b) { return a.dst < b.dst; });
}

}  // namespace kc
