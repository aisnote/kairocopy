// rules.cpp — see rules.hpp.
#include "core/rules.hpp"
#include <unordered_map>
#include <unordered_set>

namespace kc {

const char* cat_name(Cat c) {
    static const char* n[] = {"photo", "video", "music", "document", "mail", "chat", "code", "design", "archive", "other"};
    return n[int(c)];
}
const char* cat_label(Cat c) {
    static const char* n[] = {"照片", "视频", "音乐", "文档", "邮件", "聊天记录", "代码", "设计稿", "压缩包", "其他"};
    return n[int(c)];
}
bool important(Cat c) { return c != Cat::Other; }

static const std::unordered_map<std::wstring, Cat>& by_ext() {
    static const std::unordered_map<std::wstring, Cat> m = [] {
        std::unordered_map<std::wstring, Cat> m;
        auto add = [&](Cat c, std::initializer_list<const wchar_t*> exts) { for (auto e : exts) m[e] = c; };
        add(Cat::Photo, {L"jpg", L"jpeg", L"png", L"heic", L"heif", L"webp", L"gif", L"bmp", L"tif", L"tiff", L"dng", L"cr2", L"cr3", L"nef",
                         L"arw", L"orf", L"rw2", L"raf", L"srw", L"pef", L"avif", L"jxl"});
        add(Cat::Video, {L"mp4", L"mov", L"m4v", L"avi", L"mkv", L"wmv", L"flv", L"webm", L"3gp", L"mts", L"m2ts", L"mpg", L"mpeg", L"vob", L"insv"});
        add(Cat::Music, {L"mp3", L"flac", L"wav", L"m4a", L"aac", L"ogg", L"opus", L"wma", L"ape", L"aiff", L"mid", L"midi"});
        add(Cat::Document, {L"doc", L"docx", L"xls", L"xlsx", L"xlsm", L"ppt", L"pptx", L"pdf", L"txt", L"rtf", L"odt", L"ods", L"odp", L"md",
                            L"csv", L"pages", L"numbers", L"key", L"wps", L"et", L"dps", L"epub", L"mobi", L"azw3", L"one", L"vsdx", L"xmind",
                            L"mm", L"tex", L"djvu", L"ofd"});
        add(Cat::Mail, {L"pst", L"ost", L"eml", L"msg", L"mbox", L"dbx", L"emlx"});
        add(Cat::Code, {L"c", L"cc", L"cpp", L"cxx", L"h", L"hpp", L"hxx", L"cs", L"java", L"kt", L"py", L"js", L"mjs", L"ts", L"tsx", L"jsx",
                        L"go", L"rs", L"rb", L"php", L"swift", L"m", L"mm", L"lua", L"sh", L"ps1", L"bat", L"cmd", L"sql", L"html", L"htm",
                        L"css", L"scss", L"vue", L"json", L"yaml", L"yml", L"toml", L"ini", L"xml", L"gradle", L"cmake", L"sln",
                        L"vcxproj", L"csproj", L"ipynb", L"r", L"scala", L"dart", L"pem", L"pub", L"ppk", L"kdbx"});
        add(Cat::Design, {L"psd", L"psb", L"ai", L"sketch", L"fig", L"xd", L"indd", L"cdr", L"dwg", L"dxf", L"skp", L"blend", L"fbx", L"obj",
                          L"stl", L"3ds", L"max", L"c4d", L"prproj", L"aep", L"drp", L"fcpbundle", L"afphoto", L"afdesign", L"svg", L"eps"});
        add(Cat::Archive, {L"zip", L"7z", L"rar", L"tar", L"gz", L"tgz", L"bz2", L"xz", L"zst", L"iso", L"dmg", L"cab"});
        return m;
    }();
    return m;
}

Cat category_of(const std::wstring& name, const std::wstring& path) {
    // chat apps keep their files (and databases) in folders of their own
    if (path.find(L"\\wechat files\\") != std::wstring::npos || path.find(L"\\tencent files\\") != std::wstring::npos ||
        path.find(L"\\xwechat_files\\") != std::wstring::npos || path.find(L"\\whatsapp\\") != std::wstring::npos ||
        path.find(L"\\telegram desktop\\") != std::wstring::npos || path.find(L"\\dingtalk\\") != std::wstring::npos)
        return Cat::Chat;
    // keys and settings of a developer: small and hard to make again
    if (path.find(L"\\.ssh\\") != std::wstring::npos || path.find(L"\\.gnupg\\") != std::wstring::npos || name == L".gitconfig" ||
        name == L".bashrc" || name == L".zshrc" || name == L".npmrc" || name == L".vimrc")
        return Cat::Code;
    // a browser's bookmarks
    if ((name == L"bookmarks" || name == L"bookmarks.bak") && path.find(L"\\user data\\") != std::wstring::npos) return Cat::Document;
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return Cat::Other;
    auto it = by_ext().find(name.substr(dot + 1));
    return it == by_ext().end() ? Cat::Other : it->second;
}

bool skip_dir(const std::wstring& name, const std::wstring& parent, bool at_drive_root) {
    static const std::unordered_set<std::wstring> anywhere = {
        L"$recycle.bin", L"system volume information", L"recycler", L"$windows.~bt", L"$windows.~ws", L"$winreagent",
        L".kairocopy", L"node_modules", L"__pycache__", L".gradle", L".m2", L".nuget", L".cargo", L".rustup", L".npm", L".pnpm-store",
        L"bower_components", L".tox", L".mypy_cache", L".pytest_cache", L".next", L".nuxt", L".parcel-cache", L".vs", L"ipch"};
    // names that are caches when a program made them (under AppData, or at a drive's root), but may be a person's folder elsewhere
    static const std::unordered_set<std::wstring> programs = {
        L"cache", L"cache2", L"code cache", L"gpucache", L"shadercache", L"grshadercache", L"dawncache", L"cachestorage",
        L"service worker", L"inetcache", L"crashdumps", L"crashpad", L"temp", L"tmp", L"temporary internet files", L"webcache",
        L"d3dscache", L"logs"};
    if (programs.count(name) && (at_drive_root || (parent + L"\\").find(L"\\appdata\\") != std::wstring::npos)) return true;
    static const std::unordered_set<std::wstring> root_only = {
        L"windows", L"program files", L"program files (x86)", L"programdata", L"perflogs", L"recovery", L"msocache", L"intel",
        L"amd", L"nvidia", L"drivers", L"windows.old", L"$sysreset", L"onedrivetemp", L"esd", L"swsetup"};
    if (anywhere.count(name)) return true;
    if (at_drive_root && root_only.count(name)) return true;
    // build output inside a project (next to its sources, not a folder of photos named "build")
    if ((name == L"obj" || name == L"build" || name == L"out" || name == L"target" || name == L"dist" || name == L"x64" || name == L"debug" ||
         name == L"release" || name == L"cmake-build-debug" || name == L"cmake-build-release") &&
        parent.find(L"\\source\\repos\\") != std::wstring::npos)
        return true;
    // under AppData: the parts that are caches or programs (the rest, settings and data, is kept)
    if (parent.find(L"\\appdata\\local") != std::wstring::npos &&
        (name == L"packages" || name == L"programs" || name == L"microsoft" || name == L"google" || name == L"d3dscache" || name == L"nvidia" ||
         name == L"jetbrains" || name == L"pip" || name == L"yarn" || name == L"npm-cache" || name == L"docker" || name == L"wsl"))
        return true;
    return false;
}

bool skip_file(const std::wstring& name) {
    if (name == L"pagefile.sys" || name == L"hiberfil.sys" || name == L"swapfile.sys" || name == L"thumbs.db" || name == L"desktop.ini" ||
        name == L".ds_store" || name.rfind(L"dumpstack.log", 0) == 0 || name.rfind(L"ntuser.dat", 0) == 0 || name.rfind(L"usrclass.dat", 0) == 0)
        return true;
    const size_t n = wcslen(kPartSuffix);
    return name.size() > n && name.compare(name.size() - n, n, kPartSuffix) == 0;
}

}  // namespace kc
