// plan.hpp — the first step of a copy: what is under the chosen files and folders, what the target already has, and so
// what is left to copy. Nothing is written.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "core/rules.hpp"

namespace kc {

enum class Action { Copy, Overwrite };

struct Item {
    std::string src, dst;
    uint64_t size = 0;
    int64_t mtime = 0;                 // last write time, 100 ns ticks since 1601
    Cat cat = Cat::Other;
    Action action = Action::Copy;
};

struct Counts {
    int64_t files = 0, bytes = 0;
    void add(uint64_t b) { files++; bytes += int64_t(b); }
};

struct Plan {
    std::vector<std::string> sources;
    std::string dest;
    bool important_only = false;
    std::vector<Item> items;           // what to copy, in the order to copy it (by target path)
    Counts all;                        // every file under the sources (after the rules)
    Counts todo;                       // to copy: new + overwrite
    Counts fresh, overwrite;           // not at the target / older at the target
    Counts same;                       // already there (same size and time): skipped
    Counts target_newer;               // the target's copy is newer or different and not older: left alone
    Counts not_important;              // left out by "only what matters"
    Counts online_only;                // cloud placeholders (OneDrive ...): not downloaded
    Counts by_cat[kCats], todo_by_cat[kCats];
    int64_t skipped_dirs = 0;          // folders left out by the rules (system, programs, caches)
    std::vector<std::pair<std::string, std::string>> errors;   // folders that could not be read
    std::vector<std::string> target_newer_samples;
    uint64_t free_bytes = 0;           // on the target drive
    bool target_seeks = true;          // the target is a hard disk (one writer)
    bool stopped = false;
    double seconds = 0;
};

struct AnalyzeProgress { int64_t files = 0, bytes = 0; std::string dir; };

// sources: files or folders (each lands in dest under its own name; a drive's root "C:\" as "C"); dest: a folder (made
// when copying)
Plan analyze(const std::vector<std::string>& sources, const std::string& dest, bool important_only, std::atomic<bool>* stop,
             const std::function<void(const AnalyzeProgress&)>& progress);

}  // namespace kc
