// kairocopy — two panes, like Total Commander: the folder on the left and the one on the right; the selection of the
// active pane is copied into the other pane's folder (F5). F3 analyses first; the copy itself runs in the background with
// its progress below the panes and on the taskbar. Plain Win32 (common controls 6), no framework.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <winioctl.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "core/copier.hpp"
#include "core/manifest.hpp"
#include "core/plan.hpp"
#include "win/sys.hpp"

using namespace kc;

namespace {

// ------------------------------------------------------------------------------------------------ ids and messages
enum : int {
    ID_LEFT_LIST = 100, ID_RIGHT_LIST, ID_LEFT_DRIVE, ID_RIGHT_DRIVE, ID_LEFT_UP, ID_RIGHT_UP, ID_LEFT_PATH, ID_RIGHT_PATH,
    ID_ANALYZE = 200, ID_COPY, ID_PAUSE, ID_STOP, ID_REFRESH, ID_TAB, ID_SELALL,
};
constexpr UINT WM_APP_ANALYZED = WM_APP + 1, WM_APP_COPIED = WM_APP + 2, WM_APP_START = WM_APP + 3;
constexpr UINT_PTR TIMER_PROGRESS = 1;

std::wstring W(const std::string& s) { return win::widen(s); }
std::string U(const std::wstring& s) { return win::narrow(s); }

std::wstring size_text(uint64_t b) {
    wchar_t s[32];
    if (b >= 1000000000ull) swprintf(s, 32, L"%.2f GB", b / 1e9);
    else if (b >= 1000000ull) swprintf(s, 32, L"%.1f MB", b / 1e6);
    else if (b >= 1000ull) swprintf(s, 32, L"%.0f KB", b / 1e3);
    else swprintf(s, 32, L"%llu B", (unsigned long long)b);
    return s;
}
std::wstring num(int64_t n) {
    std::wstring s = std::to_wstring(n), out;
    for (size_t i = 0; i < s.size(); i++) { if (i && (s.size() - i) % 3 == 0) out += L','; out += s[i]; }
    return out;
}

// ------------------------------------------------------------------------------------------------ a pane
struct Entry {
    std::wstring name;
    bool dir = false, up = false;
    uint64_t size = 0;
    FILETIME mtime{};
    DWORD attrs = 0;
};

struct Pane {
    HWND list = nullptr, drive = nullptr, up = nullptr, path_edit = nullptr;
    std::wstring dir;                                   // the folder shown ("C:\" for a root)
    std::vector<Entry> entries;
    std::vector<std::wstring> drives;                   // "C:\" ... in the drive box's order
    bool marking = false;                               // the last key was Space/Insert (marking item by item)
};

struct App {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr, status[2]{}, total_bar = nullptr, total_text = nullptr, file_bar = nullptr, file_text = nullptr;
    HWND buttons[4]{};
    HFONT font = nullptr, big_font = nullptr;
    UINT dpi = 96;
    Pane pane[2];
    int active = 0;
    ITaskbarList3* taskbar = nullptr;
    // the work in the background
    enum class Busy { No, Analyzing, Copying } busy = Busy::No;
    std::thread worker;
    std::atomic<bool> stop{false}, pause{false};
    std::mutex mu;                                      // guards the progress below
    AnalyzeProgress aprog;
    CopyProgress cprog;
    Plan plan;
    CopyReport report;
    bool copy_after_analysis = false;
} g;

int S(int px) { return MulDiv(px, int(g.dpi), 96); }

// ------------------------------------------------------------------------------------------------ settings
std::wstring settings_path() {
    wchar_t* p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { dir = std::wstring(p) + L"\\kairocopy"; CoTaskMemFree(p); }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}
std::wstring setting(const wchar_t* key, const std::wstring& fallback) {
    wchar_t b[4096];
    GetPrivateProfileStringW(L"kairocopy", key, fallback.c_str(), b, 4096, settings_path().c_str());
    return b;
}
void set_setting(const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(L"kairocopy", key, v.c_str(), settings_path().c_str()); }

// ------------------------------------------------------------------------------------------------ drives
bool usb_drive(wchar_t letter) {
    const std::wstring dev = std::wstring(L"\\\\.\\") + letter + L":";
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024];
    DWORD got = 0;
    bool usb = false;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, nullptr) && got >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
        usb = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf)->BusType == BusTypeUsb;
    CloseHandle(h);
    return usb;
}

void fill_drives(Pane& p) {
    SendMessageW(p.drive, CB_RESETCONTENT, 0, 0);
    p.drives.clear();
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, wchar_t(L'A' + i)) + L":\\";
        const UINT t = GetDriveTypeW(root.c_str());
        if (t == DRIVE_NO_ROOT_DIR || t == DRIVE_UNKNOWN) continue;
        wchar_t label[MAX_PATH + 1] = L"";
        GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        ULARGE_INTEGER freeb{};
        const bool has_free = GetDiskFreeSpaceExW(root.c_str(), &freeb, nullptr, nullptr);
        std::wstring text = root.substr(0, 2);
        if (*label) text += std::wstring(L"  ") + label;
        if (t == DRIVE_REMOVABLE || (t == DRIVE_FIXED && usb_drive(root[0]))) text += L"  [移动]";
        else if (t == DRIVE_REMOTE) text += L"  [网络]";
        else if (t == DRIVE_CDROM) text += L"  [光驱]";
        if (has_free) text += L"  " + size_text(freeb.QuadPart) + L" 可用";
        SendMessageW(p.drive, CB_ADDSTRING, 0, LPARAM(text.c_str()));
        p.drives.push_back(root);
    }
}

void select_drive_of(Pane& p) {
    for (size_t i = 0; i < p.drives.size(); i++)
        if (!p.dir.empty() && towupper(p.dir[0]) == p.drives[i][0]) SendMessageW(p.drive, CB_SETCURSEL, WPARAM(i), 0);
}

// ------------------------------------------------------------------------------------------------ listing
int icon_of(const Entry& e) {
    SHFILEINFOW fi{};
    const DWORD attrs = e.dir || e.up ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    SHGetFileInfoW(e.up ? L"folder" : e.name.c_str(), attrs, &fi, sizeof fi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    return fi.iIcon;
}

void update_status(int side) {
    Pane& p = g.pane[side];
    int sel = 0;
    uint64_t bytes = 0;
    int files = 0, dirs = 0;
    for (int i = -1; (i = int(SendMessageW(p.list, LVM_GETNEXTITEM, WPARAM(i), LVNI_SELECTED))) >= 0;) {
        const Entry& e = p.entries[size_t(i)];
        if (e.up) continue;
        sel++;
        if (e.dir) dirs++; else { files++; bytes += e.size; }
    }
    ULARGE_INTEGER freeb{};
    std::wstring t;
    if (sel) {
        t = L"已选";
        if (dirs) t += L" " + num(dirs) + L" 个文件夹";
        if (dirs && files) t += L"、";
        if (files) t += L" " + num(files) + L" 个文件（" + size_text(bytes) + L"）";
    }
    else t = num(int64_t(p.entries.size()) - (p.entries.empty() || !p.entries[0].up ? 0 : 1)) + L" 项";
    if (GetDiskFreeSpaceExW(p.dir.substr(0, 3).c_str(), &freeb, nullptr, nullptr)) t += L"   ·   剩余 " + size_text(freeb.QuadPart);
    SetWindowTextW(g.status[side], t.c_str());
}

void load(int side, const std::wstring& dir_in, const std::wstring& select_name = L"") {
    Pane& p = g.pane[side];
    std::wstring dir = dir_in;
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    std::vector<Entry> entries;
    WIN32_FIND_DATAW f;
    const std::wstring pattern = W(win::absolute(U(dir)));
    HANDLE h = FindFirstFileExW(win::longw(U(pattern) + (pattern.back() == L'\\' ? "*" : "\\*")).c_str(), FindExInfoBasic, &f, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        MessageBoxW(g.wnd, (L"打不开 " + dir + L"\n" + W(win::error_text(e))).c_str(), L"kairocopy", MB_ICONWARNING);
        return;
    }
    do {
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && (f.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) continue;
        Entry e;
        e.name = f.cFileName;
        e.dir = f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        e.size = (uint64_t(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
        e.mtime = f.ftLastWriteTime;
        e.attrs = f.dwFileAttributes;
        entries.push_back(std::move(e));
    } while (FindNextFileW(h, &f));
    FindClose(h);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
    p.dir = pattern;
    if (p.dir.size() > 3 || (p.dir.size() == 3 && p.dir[1] != L':')) {
        Entry up;
        up.name = L"..";
        up.up = true;
        entries.insert(entries.begin(), up);
    }
    p.entries = std::move(entries);
    SendMessageW(p.list, LVM_SETITEMSTATE, WPARAM(-1), [] { static LVITEMW it{}; it.stateMask = LVIS_SELECTED | LVIS_FOCUSED; it.state = 0; return LPARAM(&it); }());
    ListView_SetItemCountEx(p.list, int(p.entries.size()), 0);
    int focus = 0;
    for (size_t i = 0; i < p.entries.size(); i++) if (!select_name.empty() && !_wcsicmp(p.entries[i].name.c_str(), select_name.c_str())) focus = int(i);
    if (!p.entries.empty()) {
        ListView_SetItemState(p.list, focus, LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_EnsureVisible(p.list, focus, FALSE);
    }
    InvalidateRect(p.list, nullptr, FALSE);
    SetWindowTextW(p.path_edit, p.dir.c_str());
    select_drive_of(p);
    update_status(side);
    set_setting(side ? L"right" : L"left", p.dir);
}

void go_up(int side) {
    Pane& p = g.pane[side];
    const std::string parent = win::parent_of(U(p.dir));
    if (parent.empty()) return;
    load(side, W(parent), W(win::name_of(U(p.dir))));
}

void open_entry(int side, int i) {
    Pane& p = g.pane[side];
    if (i < 0 || i >= int(p.entries.size())) return;
    const Entry& e = p.entries[size_t(i)];
    if (e.up) { go_up(side); return; }
    const std::wstring full = W(win::join(U(p.dir), U(e.name)));
    if (e.dir) load(side, full);
    else ShellExecuteW(g.wnd, L"open", full.c_str(), nullptr, p.dir.c_str(), SW_SHOWNORMAL);
}

// Space / Insert: the item at the cursor selected (or not), and the cursor one down, as in Total Commander
void toggle_and_next(int side) {
    Pane& p = g.pane[side];
    const int f = int(SendMessageW(p.list, LVM_GETNEXTITEM, WPARAM(-1), LVNI_FOCUSED));
    if (f < 0) return;
    if (!p.entries[size_t(f)].up) {
        // the arrow keys select the item under the cursor: the first Space after them keeps it, the next ones toggle
        const bool selected = ListView_GetItemState(p.list, f, LVIS_SELECTED) != 0;
        const bool only_by_cursor = !p.marking && selected && ListView_GetSelectedCount(p.list) == 1;
        if (!only_by_cursor) ListView_SetItemState(p.list, f, selected ? 0 : LVIS_SELECTED, LVIS_SELECTED);
    }
    p.marking = true;
    if (f + 1 < int(p.entries.size())) {
        ListView_SetItemState(p.list, f + 1, LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_EnsureVisible(p.list, f + 1, FALSE);
    }
}
void select_all(int side) {
    Pane& p = g.pane[side];
    ListView_SetItemState(p.list, -1, LVIS_SELECTED, LVIS_SELECTED);
    if (!p.entries.empty() && p.entries[0].up) ListView_SetItemState(p.list, 0, 0, LVIS_SELECTED);
}

void activate(int side) {
    g.active = side;
    SetWindowTextW(g.buttons[1], side == 0 ? L"F5  拷贝 →" : L"← F5  拷贝");
    for (int s = 0; s < 2; s++) InvalidateRect(g.pane[s].path_edit, nullptr, TRUE);
    InvalidateRect(g.wnd, nullptr, FALSE);
}

// ------------------------------------------------------------------------------------------------ layout
void layout() {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    const int W_ = rc.right, H = rc.bottom, m = S(8), row = S(28), btn_h = S(46), bar_h = S(18), gap = S(6);
    const int bottom_h = btn_h + 2 * (bar_h + S(22)) + 3 * gap + m;
    const int half = (W_ - 3 * m) / 2;
    for (int s = 0; s < 2; s++) {
        Pane& p = g.pane[s];
        const int x = m + s * (half + m);
        MoveWindow(p.drive, x, m, S(230), S(400), TRUE);
        MoveWindow(p.up, x + S(234), m, S(32), row, TRUE);
        MoveWindow(p.path_edit, x + S(270), m, half - S(270), row, TRUE);
        const int list_top = m + row + gap, list_h = H - bottom_h - list_top - S(24) - gap;
        MoveWindow(p.list, x, list_top, half, list_h, TRUE);
        MoveWindow(g.status[s], x, list_top + list_h + S(3), half, S(20), TRUE);
        ListView_SetColumnWidth(p.list, 0, half - S(260));
        ListView_SetColumnWidth(p.list, 1, S(90));
        ListView_SetColumnWidth(p.list, 2, S(140));
    }
    int y = H - bottom_h + gap;
    MoveWindow(g.total_text, m, y, W_ - 2 * m, S(20), TRUE); y += S(22);
    MoveWindow(g.total_bar, m, y, W_ - 2 * m, bar_h, TRUE); y += bar_h + gap;
    MoveWindow(g.file_text, m, y, W_ - 2 * m, S(20), TRUE); y += S(22);
    MoveWindow(g.file_bar, m, y, W_ - 2 * m, bar_h, TRUE); y += bar_h + gap;
    const int n = 4, bw = (W_ - 2 * m - (n - 1) * gap) / n;
    for (int i = 0; i < n; i++) MoveWindow(g.buttons[i], m + i * (bw + gap), y, bw, btn_h, TRUE);
}

void make_fonts() {
    if (g.font) DeleteObject(g.font);
    if (g.big_font) DeleteObject(g.big_font);
    NONCLIENTMETRICSW nm{};
    nm.cbSize = sizeof nm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0, g.dpi);
    g.font = CreateFontIndirectW(&nm.lfMessageFont);
    LOGFONTW big = nm.lfMessageFont;
    big.lfHeight = big.lfHeight * 5 / 4;
    big.lfWeight = FW_SEMIBOLD;
    g.big_font = CreateFontIndirectW(&big);
    EnumChildWindows(g.wnd, [](HWND h, LPARAM) -> BOOL { SendMessageW(h, WM_SETFONT, WPARAM(g.font), TRUE); return TRUE; }, 0);
    for (HWND b : g.buttons) SendMessageW(b, WM_SETFONT, WPARAM(g.big_font), TRUE);
}

void set_busy(App::Busy b) {
    g.busy = b;
    EnableWindow(g.buttons[0], b == App::Busy::No);
    EnableWindow(g.buttons[1], b == App::Busy::No);
    EnableWindow(g.buttons[2], b == App::Busy::Copying);
    EnableWindow(g.buttons[3], b != App::Busy::No);
    SetWindowTextW(g.buttons[2], L"暂停");
    g.pause = false;
    if (b == App::Busy::No) {
        KillTimer(g.wnd, TIMER_PROGRESS);
        if (g.taskbar) g.taskbar->SetProgressState(g.wnd, TBPF_NOPROGRESS);
    } else {
        SetTimer(g.wnd, TIMER_PROGRESS, 200, nullptr);
        if (g.taskbar) g.taskbar->SetProgressState(g.wnd, b == App::Busy::Analyzing ? TBPF_INDETERMINATE : TBPF_NORMAL);
    }
}

// ------------------------------------------------------------------------------------------------ analyse, copy
std::vector<std::string> selection(int side) {
    Pane& p = g.pane[side];
    std::vector<std::string> out;
    for (int i = -1; (i = int(SendMessageW(p.list, LVM_GETNEXTITEM, WPARAM(i), LVNI_SELECTED))) >= 0;)
        if (!p.entries[size_t(i)].up) out.push_back(win::join(U(p.dir), U(p.entries[size_t(i)].name)));
    if (out.empty()) {                                  // nothing selected: the item with the focus
        const int f = int(SendMessageW(p.list, LVM_GETNEXTITEM, WPARAM(-1), LVNI_FOCUSED));
        if (f >= 0 && !p.entries[size_t(f)].up) out.push_back(win::join(U(p.dir), U(p.entries[size_t(f)].name)));
    }
    return out;
}

// what was chosen for the copy under way, kept with its run (to go on with it after a stop) and used as the defaults
// when going on with an unfinished one
struct Choices { bool important = false, verify = false, strict = false; ConflictPolicy conflict = ConflictPolicy::Skip; };
Choices g_choices;
std::vector<std::string> g_sources;
std::string g_dest;

std::string options_text(const Choices& c) {
    return std::string(c.important ? "important " : "") + (c.verify ? "verify " : "") + (c.strict ? "strict " : "") + "conflict=" +
           (c.conflict == ConflictPolicy::Overwrite ? "overwrite" : c.conflict == ConflictPolicy::KeepBoth ? "keep-both" : "skip");
}
Choices options_of(const std::string& s) {
    Choices c;
    c.important = s.find("important") != std::string::npos;
    c.verify = s.find("verify") != std::string::npos;
    c.strict = s.find("strict") != std::string::npos;
    c.conflict = s.find("conflict=overwrite") != std::string::npos ? ConflictPolicy::Overwrite
               : s.find("conflict=keep-both") != std::string::npos ? ConflictPolicy::KeepBoth : ConflictPolicy::Skip;
    return c;
}

void run_analysis(const std::vector<std::string>& src, const std::string& dest, bool strict) {
    g_sources = src;
    g_dest = dest;
    g.stop = false;
    { std::lock_guard<std::mutex> l(g.mu); g.aprog = {}; }
    set_busy(App::Busy::Analyzing);
    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread([src, dest, strict] {
        AnalyzeOptions o;
        o.strict = strict;
        o.stop = &g.stop;
        Plan p = analyze(src, dest, o, [](const AnalyzeProgress& a) { std::lock_guard<std::mutex> l(g.mu); g.aprog = a; });
        { std::lock_guard<std::mutex> l(g.mu); g.plan = std::move(p); }
        PostMessageW(g.wnd, WM_APP_ANALYZED, 0, 0);
    });
}

void start_analysis(bool then_copy) {
    if (g.busy != App::Busy::No) return;
    const int from = g.active, to = 1 - from;
    std::vector<std::string> src = selection(from);
    if (src.empty()) { MessageBoxW(g.wnd, L"先在左边或右边选中要拷贝的文件或文件夹。", L"kairocopy", MB_ICONINFORMATION); return; }
    const std::string dest = U(g.pane[to].dir);
    for (auto& s : src)
        if (win::key_of(dest) == win::key_of(s) || win::key_of(dest).rfind(win::key_of(s) + "\\", 0) == 0) {
            MessageBoxW(g.wnd, L"不能拷贝到它自己里面：目标文件夹在要拷贝的文件夹之内。", L"kairocopy", MB_ICONWARNING);
            return;
        }
    if (win::key_of(U(g.pane[from].dir)) == win::key_of(dest)) { MessageBoxW(g.wnd, L"两边是同一个文件夹。", L"kairocopy", MB_ICONWARNING); return; }
    g.copy_after_analysis = then_copy;
    g_choices = Choices{};
    run_analysis(src, dest, false);
}

std::wstring plan_summary(const Plan& p) {
    std::wstring t = L"共 " + num(p.all.files) + L" 个文件，" + size_text(uint64_t(p.all.bytes)) + L"\n";
    t += L"要拷贝：" + num(p.todo.files) + L" 个，" + size_text(uint64_t(p.todo.bytes));
    if (p.overwrite.files || p.incomplete.files) {
        t += L"（新的 " + num(p.fresh.files);
        if (p.overwrite.files) t += L"，源文件更新的 " + num(p.overwrite.files);
        if (p.incomplete.files) t += L"，目标里没拷完的 " + num(p.incomplete.files);
        t += L"）";
    }
    t += L"\n";
    t += L"已在目标里、完全相同：" + num(p.same.files) + L" 个，" + size_text(uint64_t(p.same.bytes)) + L"\n";
    if (p.same_content.files) {
        t += L"已在目标里、内容相同但时间不同：" + num(p.same_content.files) + L" 个（不拷";
        if (p.same_content.files > p.same_sampled.files) t += L"，把目标的时间改成和源一致";
        if (p.same_sampled.files) t += L"；其中 " + num(p.same_sampled.files) + L" 个大文件是抽查的，时间不改";
        t += L"）\n";
    }
    if (p.conflict.files) t += L"目标里的不一样（冲突）：" + num(p.conflict.files) + L" 个，开始拷贝后选择怎么处理\n";
    if (p.target_only.files) t += L"只在目标里有：" + num(p.target_only.files) + L" 个（不动）\n";
    if (p.checked.files)
        t += L"核对了 " + num(p.checked.files) + L" 个文件的内容（读了 " + size_text(uint64_t(p.checked.bytes)) + L"" +
             (p.sampled.files ? L"，其中 " + num(p.sampled.files) + L" 个大文件是抽查" : std::wstring(p.strict ? L"，完整核对" : L"")) + L"）\n";
    if (!p.stale_parts.empty()) t += L"上次没拷完留下的 .kcpart 半成品：" + num(int64_t(p.stale_parts.size())) + L" 个（拷贝时清理）\n";
    if (p.online_only.files) t += L"只在云端（OneDrive 等），不下载：" + num(p.online_only.files) + L" 个\n";
    if (p.skipped_dirs) t += L"跳过的系统、程序、缓存文件夹：" + num(p.skipped_dirs) + L" 个\n";
    if (!p.errors.empty()) t += L"读不了或核对不了的：" + num(int64_t(p.errors.size())) + L" 个\n";
    t += L"目标剩余空间：" + size_text(p.free_bytes) + (p.target_seeks ? L"（机械硬盘）" : L"（固态）");
    return t;
}
std::wstring category_table(const Plan& p) {
    std::wstring t = L"按类别（全部 / 要拷贝）：\n";
    for (int i = 0; i < kCats; i++) {
        if (!p.by_cat[i].files) continue;
        t += std::wstring(W(cat_label(Cat(i)))) + L"：" + num(p.by_cat[i].files) + L" 个 " + size_text(uint64_t(p.by_cat[i].bytes)) + L"  /  " +
             num(p.todo_by_cat[i].files) + L" 个 " + size_text(uint64_t(p.todo_by_cat[i].bytes)) + L"\n";
    }
    for (size_t i = 0; i < p.conflicts.size() && i < 10; i++) t += L"冲突：" + W(p.conflicts[i].dst) + L"\n";
    for (size_t i = 0; i < p.errors.size() && i < 10; i++) t += L"读不了：" + W(p.errors[i].first) + L"（" + W(p.errors[i].second) + L"）\n";
    return t;
}

void start_copy() {
    if (uint64_t(g.plan.fresh.bytes) > g.plan.free_bytes + uint64_t(g.plan.overwrite.bytes + g.plan.incomplete.bytes)) {
        MessageBoxW(g.wnd, (L"目标空间不够：要拷 " + size_text(uint64_t(g.plan.todo.bytes)) + L"，只剩 " + size_text(g.plan.free_bytes)).c_str(), L"kairocopy",
                    MB_ICONERROR);
        return;
    }
    g.stop = false;
    g.pause = false;
    { std::lock_guard<std::mutex> l(g.mu); g.cprog = {}; g.cprog.files_total = g.plan.todo.files; g.cprog.bytes_total = g.plan.todo.bytes; }
    set_busy(App::Busy::Copying);
    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread([] {
        CopyOptions o;
        o.verify = g_choices.verify;
        o.options = options_text(g_choices);
        o.stop = &g.stop;
        o.pause = &g.pause;
        CopyReport r;
        try {
            r = copy(g.plan, o, [](const CopyProgress& c) { std::lock_guard<std::mutex> l(g.mu); g.cprog = c; });
        } catch (const std::exception& e) {
            r.errors++;
            r.failed.push_back({"", e.what()});
        }
        { std::lock_guard<std::mutex> l(g.mu); g.report = std::move(r); }
        PostMessageW(g.wnd, WM_APP_COPIED, 0, 0);
    });
}

// the target files that differ: left alone (recommended), replaced, or both kept. false: cancelled.
bool ask_conflicts() {
    Plan& p = g.plan;
    if (p.conflicts.empty()) return true;
    std::wstring list;
    for (size_t i = 0; i < p.conflicts.size() && i < 30; i++) list += W(p.conflicts[i].dst) + L"\n";
    if (p.conflicts.size() > 30) list += L"……共 " + num(int64_t(p.conflicts.size())) + L" 个";
    const std::wstring head = L"目标里有 " + num(int64_t(p.conflicts.size())) + L" 个同名文件和源文件不一样";
    TASKDIALOG_BUTTON btns[] = {{201, L"不动目标里的（推荐）\n保留目标里的版本，拷完后列在清单里"},
                                {202, L"用源文件覆盖\n目标里的版本会被替换"},
                                {203, L"两个都保留\n源文件另存为“名字（来自源）.扩展名”"}};
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.hwndParent = g.wnd;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS | TDF_EXPAND_FOOTER_AREA;
    c.pszWindowTitle = L"kairocopy";
    c.pszMainInstruction = head.c_str();
    c.pszContent = L"可能是别的软件拷过来之后又被改过，或者本来就是另一个文件。";
    c.pszExpandedInformation = list.c_str();
    c.pszCollapsedControlText = L"看是哪些";
    c.pszExpandedControlText = L"看是哪些";
    c.pButtons = btns;
    c.cButtons = 3;
    c.nDefaultButton = g_choices.conflict == ConflictPolicy::Overwrite ? 202 : g_choices.conflict == ConflictPolicy::KeepBoth ? 203 : 201;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszMainIcon = TD_WARNING_ICON;
    int button = 0;
    if (FAILED(TaskDialogIndirect(&c, &button, nullptr, nullptr)) || button == IDCANCEL) return false;
    g_choices.conflict = button == 202 ? ConflictPolicy::Overwrite : button == 203 ? ConflictPolicy::KeepBoth : ConflictPolicy::Skip;
    resolve_conflicts(p, g_choices.conflict);
    return true;
}

void on_analyzed() {
    if (g.worker.joinable()) g.worker.join();
    set_busy(App::Busy::No);
    const Plan& p = g.plan;
    SendMessageW(g.total_bar, PBM_SETPOS, 0, 0);
    SetWindowTextW(g.total_text, (L"分析完：" + num(p.all.files) + L" 个文件，" + size_text(uint64_t(p.all.bytes)) + L"，要拷贝 " + num(p.todo.files) + L" 个（" +
                                  size_text(uint64_t(p.todo.bytes)) + L"），用时 " + std::to_wstring(int(p.seconds + 0.5)) + L" 秒").c_str());
    if (p.stopped) { SetWindowTextW(g.total_text, L"分析已停止"); return; }
    const std::wstring dest = W(p.dest);
    if (p.items.empty() && p.conflicts.empty()) {
        TaskDialog(g.wnd, nullptr, L"kairocopy", L"没有要拷贝的：都已经在目标里了", plan_summary(p).c_str(), TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
        return;
    }
    Counts imp = p.todo;
    imp.files -= p.todo_by_cat[int(Cat::Other)].files;
    imp.bytes -= p.todo_by_cat[int(Cat::Other)].bytes;
    const std::wstring all_label = L"全部拷贝（" + num(p.todo.files) + L" 个，" + size_text(uint64_t(p.todo.bytes)) + L"）";
    const std::wstring imp_label = L"只拷重要的：照片、视频、音乐、文档、邮件、聊天记录、代码、设计稿、压缩包（" + num(imp.files) + L" 个，" + size_text(uint64_t(imp.bytes)) + L"）";
    TASKDIALOG_BUTTON radios[] = {{1, all_label.c_str()}, {2, imp_label.c_str()}};
    std::vector<TASKDIALOG_BUTTON> btns = {{IDOK, L"开始拷贝"}};
    // the large files compared in samples: a whole comparison on demand
    if (p.sampled.files && !p.strict) btns.push_back({100, L"完整核对"});
    btns.push_back({IDCANCEL, L"取消"});
    const std::wstring title = L"拷贝到 " + dest, content = plan_summary(p), more = category_table(p);
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.hwndParent = g.wnd;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_EXPAND_FOOTER_AREA | TDF_SIZE_TO_CONTENT | (g_choices.verify ? TDF_VERIFICATION_FLAG_CHECKED : 0);
    c.pszWindowTitle = L"kairocopy";
    c.pszMainInstruction = title.c_str();
    c.pszContent = content.c_str();
    c.pszExpandedInformation = more.c_str();
    c.pszCollapsedControlText = L"按类别看";
    c.pszExpandedControlText = L"按类别看";
    c.pRadioButtons = radios;
    c.cRadioButtons = 2;
    c.nDefaultRadioButton = g_choices.important ? 2 : 1;
    c.pButtons = btns.data();
    c.cButtons = UINT(btns.size());
    c.nDefaultButton = IDOK;
    c.pszVerificationText = L"拷完后逐个读回来校验（更慢，更放心）";
    c.pszMainIcon = TD_INFORMATION_ICON;
    int button = 0, radio = 0;
    BOOL verify = FALSE;
    if (FAILED(TaskDialogIndirect(&c, &button, &radio, &verify)) || button == IDCANCEL) return;
    g_choices.important = radio == 2;
    g_choices.verify = verify != FALSE;
    if (button == 100) { g_choices.strict = true; run_analysis(g_sources, g_dest, true); return; }
    if (g_choices.important) keep_important(g.plan);
    if (!ask_conflicts()) return;
    if (g.plan.items.empty()) { TaskDialog(g.wnd, nullptr, L"kairocopy", L"没有要拷贝的", nullptr, TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr); return; }
    start_copy();
}

// The last copy to a drive that did not finish: offered when the program starts, if its sources are still there.
void offer_unfinished() {
    struct Found { std::string manifest; Manifest::Run run; };
    std::vector<Found> found;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        const std::wstring root = std::wstring(1, wchar_t(L'A' + i)) + L":\\";
        const UINT t = GetDriveTypeW(root.c_str());
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        const std::string path = U(root) + kStateDir + "\\manifest.db";
        Manifest::Run run;
        if (!Manifest::unfinished(path, run)) continue;
        bool there = win::is_dir(run.dest) || win::is_dir(win::parent_of(run.dest));
        size_t from = 0;
        while (there && from <= run.sources.size()) {
            const size_t bar = run.sources.find('|', from);
            const std::string s = run.sources.substr(from, bar == std::string::npos ? std::string::npos : bar - from);
            if (GetFileAttributesW(win::longw(s).c_str()) == INVALID_FILE_ATTRIBUTES) there = false;
            if (bar == std::string::npos) break;
            from = bar + 1;
        }
        if (there) found.push_back({path, run});
    }
    if (found.empty()) return;
    const Found& f = *std::max_element(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.run.started_ms < b.run.started_ms; });
    std::vector<std::string> sources;
    for (size_t from = 0;;) {
        const size_t bar = f.run.sources.find('|', from);
        sources.push_back(f.run.sources.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
        if (bar == std::string::npos) break;
        from = bar + 1;
    }
    std::wstring src_text;
    for (auto& s : sources) src_text += L"  " + W(s) + L"\n";
    const std::wstring content = L"从：\n" + src_text + L"到：\n  " + W(f.run.dest) + L"\n\n开始于 " + W(win::time_text((f.run.started_ms + 11644473600000LL) * 10000)) +
                                 L"，当时已拷 " + num(f.run.files) + L" / " + num(f.run.planned_files) + L" 个文件。\n继续的话会重新分析，只拷还没拷完的。";
    TASKDIALOG_BUTTON btns[] = {{IDOK, L"继续拷贝"}, {IDCANCEL, L"以后再说"}, {300, L"不再提示"}};
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.hwndParent = g.wnd;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    c.pszWindowTitle = L"kairocopy";
    c.pszMainInstruction = L"上次的拷贝没有完成";
    c.pszContent = content.c_str();
    c.pButtons = btns;
    c.cButtons = 3;
    c.nDefaultButton = IDOK;
    c.pszMainIcon = TD_INFORMATION_ICON;
    int button = 0;
    if (FAILED(TaskDialogIndirect(&c, &button, nullptr, nullptr))) return;
    if (button == 300) { Manifest::set_aside(f.manifest, f.run.id); return; }
    if (button != IDOK) return;
    // the panes where the copy was: the sources' folder on the left, the target on the right
    const std::string left = win::is_dir(sources[0]) && sources.size() == 1 ? win::parent_of(sources[0]) : win::parent_of(sources[0]);
    if (!left.empty() && win::is_dir(left)) load(0, W(left), W(win::name_of(sources[0])));
    if (win::is_dir(f.run.dest)) load(1, W(f.run.dest));
    activate(0);
    g_choices = options_of(f.run.options);
    g.copy_after_analysis = true;
    run_analysis(sources, f.run.dest, g_choices.strict);
}

void on_copied() {
    if (g.worker.joinable()) g.worker.join();
    set_busy(App::Busy::No);
    SetWindowTextW(g.file_text, L"");
    SendMessageW(g.file_bar, PBM_SETPOS, 0, 0);
    const CopyReport& r = g.report;
    std::wstring head = r.disk_full ? L"目标空间满了，已停止" : r.stopped ? L"已停止（已拷完的都保留，再按 F5 接着拷）" : r.errors ? L"拷完了，有些文件没拷成" : L"拷完了";
    wchar_t b[300];
    swprintf(b, 300, L"拷贝了 %ls 个文件，%ls，用时 %d 分 %d 秒（平均 %.0f MB/s）\n没拷成：%ls 个（其中被别的程序占用 %ls 个）", num(r.files).c_str(),
             size_text(uint64_t(r.bytes)).c_str(), int(r.seconds) / 60, int(r.seconds) % 60, r.seconds > 0 ? r.bytes / r.seconds / 1e6 : 0.0,
             num(r.errors).c_str(), num(r.in_use).c_str());
    std::wstring extra;
    if (r.times_set) extra += L"\n已在目标里、内容相同的 " + num(r.times_set) + L" 个：时间改成和源一致";
    if (r.parts_removed) extra += L"\n清理了上次留下的半成品 " + num(r.parts_removed) + L" 个";
    if (!g.plan.conflicts.empty()) extra += L"\n目标里不一样、没有动的 " + num(int64_t(g.plan.conflicts.size())) + L" 个（见详情）";
    const std::wstring body = std::wstring(b) + extra;
    std::wstring more;
    for (size_t i = 0; i < g.plan.conflicts.size() && i < 30; i++) more += L"没动（目标里不一样）：" + W(g.plan.conflicts[i].dst) + L"\n";
    for (size_t i = 0; i < r.failed.size() && i < 30; i++) more += W(r.failed[i].first) + L"：" + W(r.failed[i].second) + L"\n";
    if (r.failed.size() > 30) more += L"……共 " + num(int64_t(r.failed.size())) + L" 个\n";
    if (!r.manifest.empty()) more += L"\n拷贝记录：" + W(r.manifest);
    SetWindowTextW(g.total_text, (head + L"：" + num(r.files) + L" 个文件，" + size_text(uint64_t(r.bytes))).c_str());
    SendMessageW(g.total_bar, PBM_SETPOS, r.stopped ? WPARAM(SendMessageW(g.total_bar, PBM_GETPOS, 0, 0)) : 1000, 0);
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.hwndParent = g.wnd;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | (more.empty() ? 0 : TDF_EXPAND_FOOTER_AREA);
    c.pszWindowTitle = L"kairocopy";
    c.pszMainInstruction = head.c_str();
    c.pszContent = body.c_str();
    c.pszExpandedInformation = more.empty() ? nullptr : more.c_str();
    c.pszCollapsedControlText = L"详情";
    c.pszExpandedControlText = L"详情";
    c.dwCommonButtons = TDCBF_OK_BUTTON;
    c.pszMainIcon = r.errors || r.stopped ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
    // the target pane shows what arrived
    const int to = win::key_of(U(g.pane[0].dir)) == win::key_of(g.plan.dest) ? 0 : 1;
    load(to, g.pane[to].dir);
}

void on_timer() {
    std::lock_guard<std::mutex> l(g.mu);
    if (g.busy == App::Busy::Analyzing) {
        if (g.aprog.checking)
            SetWindowTextW(g.total_text, (L"核对目标里已有的文件：" + num(g.aprog.check_done) + L" / " + num(g.aprog.check_total)).c_str());
        else
            SetWindowTextW(g.total_text, (L"分析中：" + num(g.aprog.files) + L" 个文件，" + size_text(uint64_t(g.aprog.bytes)) + L"   " + W(g.aprog.dir)).c_str());
        SendMessageW(g.total_bar, PBM_SETMARQUEE, TRUE, 30);
        return;
    }
    if (g.busy != App::Busy::Copying) return;
    const CopyProgress& c = g.cprog;
    const double part = c.bytes_total > 0 ? double(c.bytes_done) / double(c.bytes_total) : c.files_total ? double(c.files_done) / c.files_total : 0;
    const double eta = c.speed > 0 ? double(c.bytes_total - c.bytes_done) / c.speed : -1;
    wchar_t b[400];
    std::wstring left = eta < 0 ? L"—" : eta >= 3600 ? std::to_wstring(int(eta / 3600)) + L" 小时 " + std::to_wstring(int(eta) % 3600 / 60) + L" 分"
                                                     : std::to_wstring(int(eta / 60)) + L" 分 " + std::to_wstring(int(eta) % 60) + L" 秒";
    std::wstring t = c.paused ? L"【已暂停】 " : L"";
    t += L"总进度 " + std::to_wstring(int(part * 100)) + L"%   " + num(c.files_done) + L" / " + num(c.files_total) + L" 个文件   " +
         size_text(uint64_t(c.bytes_done)) + L" / " + size_text(uint64_t(c.bytes_total)) + L"   " + std::to_wstring(int(c.speed / 1e6 + 0.5)) +
         L" MB/s   剩余 " + left;
    if (c.errors) t += L"   没拷成 " + num(c.errors);
    SetWindowTextW(g.total_text, t.c_str());
    SendMessageW(g.total_bar, PBM_SETPOS, WPARAM(part * 1000), 0);
    if (g.taskbar) {
        g.taskbar->SetProgressValue(g.wnd, ULONGLONG(part * 1000), 1000);
        g.taskbar->SetProgressState(g.wnd, c.paused ? TBPF_PAUSED : TBPF_NORMAL);
    }
    if (!c.current.empty()) {
        swprintf(b, 400, L"当前：%ls   %ls / %ls", W(win::name_of(c.current)).c_str(), size_text(c.current_done).c_str(), size_text(c.current_size).c_str());
        SetWindowTextW(g.file_text, b);
        SendMessageW(g.file_bar, PBM_SETPOS, c.current_size ? WPARAM(double(c.current_done) / double(c.current_size) * 1000) : 0, 0);
    }
}

// ------------------------------------------------------------------------------------------------ the window
void toggle_and_next(int side);
void select_all(int side);
void go_up(int side);

LRESULT CALLBACK list_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    const int side = int(id) - ID_LEFT_LIST;
    if (m == WM_SETFOCUS) activate(side);
    if (m == WM_KEYDOWN) {
        const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        if (w == VK_SPACE || w == VK_INSERT) { toggle_and_next(side); return 0; }
        g.pane[side].marking = false;
        if (w == VK_BACK) { go_up(side); return 0; }
        if (w == VK_TAB) { SetFocus(g.pane[1 - side].list); return 0; }
        if (w == 'A' && ctrl) { select_all(side); return 0; }
    }
    if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) g.pane[side].marking = false;
    if (m == WM_CHAR && (w == L' ' || w == 9 || w == 8 || w == 1)) return 0;   // space, tab, backspace, Ctrl+A: no type-to-find
    if (m == WM_GETDLGCODE && l && reinterpret_cast<MSG*>(l)->message == WM_KEYDOWN && reinterpret_cast<MSG*>(l)->wParam == VK_TAB) return DLGC_WANTALLKEYS;
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK path_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        wchar_t b[32768];
        GetWindowTextW(h, b, 32768);
        const int side = int(id) - ID_LEFT_PATH;
        if (win::is_dir(U(b))) { load(side, b); SetFocus(g.pane[side].list); }
        else MessageBeep(MB_ICONWARNING);
        return 0;
    }
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc(h, m, w, l);
}

void make_pane(int side) {
    Pane& p = g.pane[side];
    p.drive = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, g.wnd,
                              HMENU(INT_PTR(ID_LEFT_DRIVE + side)), g.inst, nullptr);
    p.up = CreateWindowExW(0, WC_BUTTONW, L"↑", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g.wnd, HMENU(INT_PTR(ID_LEFT_UP + side)), g.inst, nullptr);
    p.path_edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, g.wnd,
                                  HMENU(INT_PTR(ID_LEFT_PATH + side)), g.inst, nullptr);
    SetWindowSubclass(p.path_edit, path_proc, UINT_PTR(ID_LEFT_PATH + side), 0);
    p.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS |
                             LVS_SHAREIMAGELISTS, 0, 0, 0, 0, g.wnd, HMENU(INT_PTR(ID_LEFT_LIST + side)), g.inst, nullptr);
    ListView_SetExtendedListViewStyle(p.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(p.list, L"Explorer", nullptr);
    SHFILEINFOW fi{};
    HIMAGELIST il = HIMAGELIST(SHGetFileInfoW(L"C:\\", 0, &fi, sizeof fi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    ListView_SetImageList(p.list, il, LVSIL_SMALL);
    const wchar_t* cols[] = {L"名称", L"大小", L"修改时间"};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 1 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = 100;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        ListView_InsertColumn(p.list, i, &c);
    }
    SetWindowSubclass(p.list, list_proc, UINT_PTR(ID_LEFT_LIST + side), 0);
    g.status[side] = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX, 0, 0, 0, 0, g.wnd, nullptr, g.inst, nullptr);
    fill_drives(p);
}

void on_create(HWND h) {
    g.wnd = h;
    g.dpi = GetDpiForWindow(h);
    make_pane(0);
    make_pane(1);
    g.total_text = CreateWindowExW(0, WC_STATICW, L"选中要拷贝的文件或文件夹，按 F5 拷贝到另一边（F3 先分析）。Tab 切换左右。", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX,
                                   0, 0, 0, 0, h, nullptr, g.inst, nullptr);
    g.total_bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, h, nullptr, g.inst, nullptr);
    g.file_text = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX | SS_PATHELLIPSIS, 0, 0, 0, 0, h, nullptr, g.inst, nullptr);
    g.file_bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, h, nullptr, g.inst, nullptr);
    SendMessageW(g.total_bar, PBM_SETRANGE32, 0, 1000);
    SendMessageW(g.file_bar, PBM_SETRANGE32, 0, 1000);
    const wchar_t* labels[] = {L"F3  分析", L"F5  拷贝 →", L"暂停", L"停止"};
    const int ids[] = {ID_ANALYZE, ID_COPY, ID_PAUSE, ID_STOP};
    for (int i = 0; i < 4; i++)
        g.buttons[i] = CreateWindowExW(0, WC_BUTTONW, labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, h, HMENU(INT_PTR(ids[i])), g.inst, nullptr);
    make_fonts();
    set_busy(App::Busy::No);
    // where each side was last time; else the user's folder on the left and an external drive on the right
    wchar_t* prof = nullptr;
    std::wstring home = L"C:\\";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &prof))) { home = prof; CoTaskMemFree(prof); }
    std::wstring right = L"";
    for (auto& d : g.pane[1].drives) if (towupper(d[0]) != L'C' && (GetDriveTypeW(d.c_str()) == DRIVE_REMOVABLE || usb_drive(d[0]))) { right = d; break; }
    if (right.empty()) right = home;
    std::wstring l = setting(L"left", home), r = setting(L"right", right);
    load(0, win::is_dir(U(l)) ? l : home);
    load(1, win::is_dir(U(r)) ? r : right);
    activate(0);
    SetFocus(g.pane[0].list);
    PostMessageW(h, WM_APP_START, 0, 0);
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, reinterpret_cast<void**>(&g.taskbar))))
        g.taskbar->HrInit();
}

LRESULT on_notify(NMHDR* n) {
    const int side = int(n->idFrom) - ID_LEFT_LIST;
    if (side != 0 && side != 1) return 0;
    Pane& p = g.pane[side];
    switch (n->code) {
    case LVN_GETDISPINFOW: {
        auto* d = reinterpret_cast<NMLVDISPINFOW*>(n);
        if (d->item.iItem < 0 || d->item.iItem >= int(p.entries.size())) return 0;
        const Entry& e = p.entries[size_t(d->item.iItem)];
        static std::wstring buf;
        if (d->item.mask & LVIF_TEXT) {
            if (d->item.iSubItem == 0) buf = e.name;
            else if (d->item.iSubItem == 1) buf = e.up ? L"" : e.dir ? L"<文件夹>" : size_text(e.size);
            else if (d->item.iSubItem == 2 && !e.up) {
                FILETIME lf;
                SYSTEMTIME st;
                FileTimeToLocalFileTime(&e.mtime, &lf);
                FileTimeToSystemTime(&lf, &st);
                wchar_t t[32];
                swprintf(t, 32, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
                buf = t;
            } else buf.clear();
            lstrcpynW(d->item.pszText, buf.c_str(), d->item.cchTextMax);
        }
        if (d->item.mask & LVIF_IMAGE) d->item.iImage = icon_of(e);
        return 0;
    }
    case LVN_ITEMACTIVATE: open_entry(side, reinterpret_cast<NMITEMACTIVATE*>(n)->iItem); return 0;
    case LVN_ITEMCHANGED:
    case LVN_ODSTATECHANGED: update_status(side); return 0;
    case NM_SETFOCUS: activate(side); return 0;
    }
    return 0;
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: on_create(h); return 0;
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: { auto* mm = reinterpret_cast<MINMAXINFO*>(l); mm->ptMinTrackSize = {S(820), S(520)}; return 0; }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(w);
        const RECT* r = reinterpret_cast<RECT*>(l);
        make_fonts();
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_NOTIFY: return on_notify(reinterpret_cast<NMHDR*>(l));
    case WM_CTLCOLOREDIT: {
        // the active side's path stands out
        HWND e = HWND(l);
        if (e == g.pane[g.active].path_edit) { SetBkColor(HDC(w), RGB(220, 234, 255)); static HBRUSH b = CreateSolidBrush(RGB(220, 234, 255)); return LRESULT(b); }
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(w), code = HIWORD(w);
        if ((id == ID_LEFT_DRIVE || id == ID_RIGHT_DRIVE) && code == CBN_SELCHANGE) {
            const int side = id - ID_LEFT_DRIVE;
            const int i = int(SendMessageW(g.pane[side].drive, CB_GETCURSEL, 0, 0));
            if (i >= 0 && i < int(g.pane[side].drives.size())) load(side, g.pane[side].drives[size_t(i)]);
            SetFocus(g.pane[side].list);
            return 0;
        }
        if (code == CBN_DROPDOWN) { fill_drives(g.pane[id - ID_LEFT_DRIVE]); select_drive_of(g.pane[id - ID_LEFT_DRIVE]); return 0; }
        switch (id) {
        case ID_LEFT_UP: case ID_RIGHT_UP: go_up(id - ID_LEFT_UP); SetFocus(g.pane[id - ID_LEFT_UP].list); return 0;
        case ID_ANALYZE: start_analysis(false); return 0;
        case ID_COPY: start_analysis(true); return 0;
        case ID_PAUSE:
            g.pause = !g.pause;
            SetWindowTextW(g.buttons[2], g.pause ? L"继续" : L"暂停");
            return 0;
        case ID_STOP: g.stop = true; g.pause = false; return 0;
        case ID_REFRESH: for (int s = 0; s < 2; s++) load(s, g.pane[s].dir); return 0;
        }
        break;
    }
    case WM_APP_ANALYZED:
        if (g.copy_after_analysis) on_analyzed();
        else {
            // F3: the analysis, and the copy offered from it
            on_analyzed();
        }
        return 0;
    case WM_APP_COPIED: on_copied(); return 0;
    case WM_APP_START: offer_unfinished(); return 0;
    case WM_TIMER: if (w == TIMER_PROGRESS) on_timer(); return 0;
    case WM_CLOSE:
        if (g.busy == App::Busy::Copying &&
            MessageBoxW(h, L"正在拷贝。停止并退出吗？已经拷完的会保留，下次按 F5 接着拷。", L"kairocopy", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        g.stop = true;
        g.pause = false;
        if (g.worker.joinable()) g.worker.join();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g.taskbar) g.taskbar->Release();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g.inst = inst;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"kairocopy";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(2));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    HWND h = CreateWindowExW(0, L"kairocopy", L"kairocopy — 快速拷贝", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1200, int(dpi), 96),
                             MulDiv(760, int(dpi), 96), nullptr, nullptr, inst, nullptr);
    ShowWindow(h, show);
    ACCEL acc[] = {{FVIRTKEY, VK_F3, ID_ANALYZE}, {FVIRTKEY, VK_F5, ID_COPY}, {FVIRTKEY | FCONTROL, 'R', ID_REFRESH}};
    HACCEL ha = CreateAcceleratorTableW(acc, 3);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (TranslateAcceleratorW(h, ha, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyAcceleratorTable(ha);
    CoUninitialize();
    return 0;
}
