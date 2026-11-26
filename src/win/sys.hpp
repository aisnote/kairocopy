// sys.hpp — the few Windows calls kairocopy needs, with UTF-8 paths outside and wide ones inside.
#pragma once
#include <cstdint>
#include <string>

namespace kc::win {

std::wstring widen(const std::string& s);
std::string narrow(const wchar_t* w, size_t len);
inline std::string narrow(const std::wstring& w) { return narrow(w.data(), w.size()); }

// The form every file call gets: \\?\C:\... (or \\?\UNC\server\share\...), so that paths of any length work.
std::wstring longw(const std::string& absolute_path);
std::string absolute(const std::string& path);       // full, backslashes, no trailing one (except "C:\")
std::string key_of(const std::string& path);         // lower case: what two spellings of one path share
std::string join(const std::string& dir, const std::string& name);
std::string parent_of(const std::string& path);      // "" for a drive's root
std::string name_of(const std::string& path);
std::string error_text(unsigned long code);          // the system's message, no trailing dot or newline
std::string volume_root(const std::string& path);    // "L:\" for any path on L:
bool is_dir(const std::string& path);
bool free_space(const std::string& path, uint64_t& free_bytes);
// whether the drive holding `path` has to seek (a hard disk): one writer is then faster than several
bool has_seek_penalty(const std::string& path, bool& seeks);
int64_t now_ms();
// file times are kept as Windows has them: 100 ns ticks since 1601
constexpr int64_t kTicksPerSecond = 10000000;
std::string time_text(int64_t ticks);                // "2024-05-01 13:02:11", local time

}  // namespace kc::win
