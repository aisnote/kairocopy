// plan.cpp — see plan.hpp. The folders are listed with FindFirstFileExW (basic information, large fetches): sizes and
// times come with the listing, no call per file.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include "core/plan.hpp"
#include "win/sys.hpp"

namespace kc {

namespace {
using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
int64_t ticks(const FILETIME& f) { return int64_t((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime); }
// times on exFAT and FAT are kept to 10 ms and 2 s: within that they are the same
constexpr int64_t kSameTime = 2 * win::kTicksPerSecond;

void lower(std::wstring& s) { if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size())); }

struct Entry { uint64_t size; int64_t mtime; };

// every file under `dir` (the target side: no rules), keys in lower case
void list_target(const std::string& dir, std::unordered_map<std::string, Entry>& out) {
    struct D { std::wstring path; std::string key; };     // the path to list, and its key (lower case, no \\?\)
    std::vector<D> stack{{win::longw(dir), win::key_of(dir)}};
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
                if (!(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(D{d.path + L"\\" + f.cFileName, std::move(key)});
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
}  // namespace

Plan analyze(const std::vector<std::string>& sources, const std::string& dest_in, bool important_only, std::atomic<bool>* stop,
             const std::function<void(const AnalyzeProgress&)>& progress) {
    const auto t0 = Clock::now();
    Plan p;
    p.dest = win::absolute(dest_in);
    p.important_only = important_only;
    win::free_space(p.dest, p.free_bytes);
    win::has_seek_penalty(p.dest, p.target_seeks);
    AnalyzeProgress pr;
    auto last = Clock::now();

    for (const auto& s_in : sources) {
        const std::string src = win::absolute(s_in);
        p.sources.push_back(src);
        const std::string dst_root = win::join(p.dest, target_name(src));
        // what the target already has, keyed by lower-case path
        std::unordered_map<std::string, Entry> have;
        if (win::is_dir(dst_root)) list_target(dst_root, have);

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
            if (important_only && !important(cat)) { p.not_important.add(size); return; }
            Action act = Action::Copy;
            auto it = have.find(win::key_of(dst_path));
            if (it != have.end()) {
                const Entry& e = it->second;
                const int64_t dt = mtime - e.mtime;
                if (e.size == size && dt <= kSameTime && dt >= -kSameTime) { p.same.add(size); return; }
                if (dt > kSameTime) act = Action::Overwrite;
                else {
                    p.target_newer.add(size);
                    if (p.target_newer_samples.size() < 20) p.target_newer_samples.push_back(dst_path);
                    return;
                }
            }
            (act == Action::Copy ? p.fresh : p.overwrite).add(size);
            p.todo.add(size);
            p.todo_by_cat[int(cat)].add(size);
            p.items.push_back(Item{src_path, dst_path, size, mtime, cat, act});
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
            continue;
        }
        // a folder, depth first; the folder's own name is not checked against the rules (it was chosen)
        struct Dir { std::wstring wsrc; std::string src, dst; std::wstring lower_path; bool drive_root; };
        std::wstring root_lower = win::widen(src);
        lower(root_lower);
        std::vector<Dir> stack{{win::longw(src), src, dst_root, root_lower, src.size() <= 3}};
        while (!stack.empty()) {
            if (stop && stop->load()) { p.stopped = true; break; }
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
        if (p.stopped) break;
    }
    // copied in the order of the target's paths: a hard disk writes one folder after the other
    std::sort(p.items.begin(), p.items.end(), [](const Item& a, const Item& b) { return a.dst < b.dst; });
    p.seconds = since(t0);
    if (progress) { pr.dir.clear(); progress(pr); }
    return p;
}

void keep_important(Plan& p) {
    if (p.important_only) return;
    p.important_only = true;
    std::vector<Item> kept;
    kept.reserve(p.items.size());
    for (auto& it : p.items) {
        if (important(it.cat)) { kept.push_back(std::move(it)); continue; }
        (it.action == Action::Copy ? p.fresh : p.overwrite).files--;
        (it.action == Action::Copy ? p.fresh : p.overwrite).bytes -= int64_t(it.size);
        p.todo.files--; p.todo.bytes -= int64_t(it.size);
    }
    p.items = std::move(kept);
    const Counts other = p.by_cat[int(Cat::Other)];
    p.not_important.files += other.files; p.not_important.bytes += other.bytes;
    p.todo_by_cat[int(Cat::Other)] = Counts{};
}

}  // namespace kc
