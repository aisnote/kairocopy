// rules.hpp — what a file is (its category, from its extension and where it is) and what is never worth copying.
#pragma once
#include <string>

namespace kc {

enum class Cat { Photo, Video, Music, Document, Mail, Chat, Code, Design, Archive, Other, Count };
constexpr int kCats = int(Cat::Count);
const char* cat_name(Cat c);         // "photo", ... (the command line, JSON)
const char* cat_label(Cat c);        // "照片", ... (the window)
// The categories "only what matters" keeps: everything but Other.
bool important(Cat c);

// `name` and `path` in lower case (path: the whole one, backslashes)
Cat category_of(const std::wstring& name_lower, const std::wstring& path_lower);

// Folders never entered: the system's, programs', caches, build output, the recycle bin. `at_drive_root`: the folder is
// directly under a drive's root (Windows, Program Files ... are skipped only there).
bool skip_dir(const std::wstring& name_lower, const std::wstring& parent_lower, bool at_drive_root);
// Files never copied: the page file and its kin, half-copied files of an earlier run.
bool skip_file(const std::wstring& name_lower);

constexpr const wchar_t* kPartSuffix = L".kcpart";     // a file being copied: renamed to its name once complete
constexpr const char* kStateDir = ".kairocopy";       // the record of what was copied, at the root of the target drive

}  // namespace kc
