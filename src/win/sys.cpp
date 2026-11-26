// sys.cpp — see sys.hpp.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <chrono>
#include "win/sys.hpp"

namespace kc::win {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w, size_t len) {
    if (!len) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, int(len), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, int(len), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring longw(const std::string& path) {
    std::wstring w = widen(path);
    for (auto& c : w) if (c == L'/') c = L'\\';
    if (w.rfind(L"\\\\?\\", 0) == 0) return w;
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    if (w.size() >= 2 && w[1] == L':') return L"\\\\?\\" + w;
    return w;
}

std::string absolute(const std::string& path) {
    std::wstring w = widen(path), out(32768, L'\0');
    const DWORD n = GetFullPathNameW(w.c_str(), DWORD(out.size()), out.data(), nullptr);
    if (!n || n >= out.size()) return path;
    out.resize(n);
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return narrow(out);
}

std::string key_of(const std::string& path) {
    std::wstring w = widen(path);
    CharLowerBuffW(w.data(), DWORD(w.size()));
    for (auto& c : w) if (c == L'/') c = L'\\';
    return narrow(w);
}

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '\\' ? dir + name : dir + "\\" + name;
}
std::string parent_of(const std::string& path) {
    const size_t cut = path.find_last_of('\\');
    if (cut == std::string::npos) return "";
    if (cut == path.size() - 1) return "";                  // "C:\"
    if (cut == 2 && path[1] == ':') return path.substr(0, 3);
    return path.substr(0, cut);
}
std::string name_of(const std::string& path) {
    const size_t cut = path.find_last_of('\\');
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

std::string error_text(unsigned long e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    if (!buf) FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                             reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string m = buf ? narrow(buf, wcslen(buf)) : "error " + std::to_string(e);
    if (buf) LocalFree(buf);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == '.' || m.back() == ' ')) m.pop_back();
    return m;
}

std::string volume_root(const std::string& path) {
    wchar_t out[MAX_PATH + 1];
    if (GetVolumePathNameW(widen(path).c_str(), out, MAX_PATH)) return narrow(out, wcslen(out));
    return path.size() >= 3 && path[1] == ':' ? path.substr(0, 3) : path;
}

bool is_dir(const std::string& path) {
    const DWORD a = GetFileAttributesW(longw(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool free_space(const std::string& path, uint64_t& free_bytes) {
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExW(widen(volume_root(path)).c_str(), &avail, nullptr, nullptr)) return false;
    free_bytes = avail.QuadPart;
    return true;
}

bool has_seek_penalty(const std::string& path, bool& seeks) {
    std::wstring root = widen(volume_root(path));
    if (root.size() < 2 || root[1] != L':') return false;
    const std::wstring dev = L"\\\\.\\" + root.substr(0, 2);
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
    DWORD got = 0;
    const bool ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &d, sizeof d, &got, nullptr) && got >= sizeof d;
    CloseHandle(h);
    if (ok) seeks = d.IncursSeekPenalty != 0;
    return ok;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string time_text(int64_t ticks) {
    FILETIME ft{DWORD(uint64_t(ticks) & 0xffffffff), DWORD(uint64_t(ticks) >> 32)}, local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return "";
    char b[32];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

}  // namespace kc::win
