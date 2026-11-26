// plan.hpp — the first step of a copy: what is under the chosen files and folders, what the target already has, and so
// what is left to copy. Nothing is written.
//
// The target may hold files of an earlier copy, by kairocopy or by another program, stopped halfway. Each file already
// there is put in one class:
//   same           same size, time within 2 s                                    skipped
//   incomplete     robocopy's mark (time 1980-01-01), or the content check finds   copied again
//                  the start of the source, or the source with zeros at the end
//   same content   the content check finds the source (another time)            skipped; its time set to the source's
//                                                                                 when compared in full
//   conflict       larger and newer, or a content that differs                   left alone (or as asked: conflicts())
//   overwrite      the source is newer                                            copied over it
// The content check reads only the files whose class the sizes and times do not settle: same size and another time, or
// smaller and newer. Files of 64 MB or less are compared whole; larger ones at their start, end and eight places
// between (1 MB each), unless strict.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "core/rules.hpp"

namespace kc {

enum class Action {
    Copy,           // not at the target
    Overwrite,      // the target's is older, or incomplete
    FixTime,        // the target's has the same content: only its time is set to the source's
};

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
    void sub(uint64_t b) { files--; bytes -= int64_t(b); }
};

struct Plan {
    std::vector<std::string> sources;
    std::string dest;
    bool important_only = false, strict = false;
    std::vector<Item> items;           // what to do, in the order of the target's paths
    std::vector<Item> conflicts;       // the source's files whose target file differs (not in items unless resolve_conflicts)
    std::vector<std::string> stale_parts;   // .kcpart files of an earlier kairocopy run no longer needed: removed by copy()
    Counts all;                        // every file under the sources (after the rules)
    Counts todo;                       // to copy: new + overwrite + incomplete (bytes to write)
    Counts fresh, overwrite;           // not at the target / older at the target
    Counts incomplete;                 // at the target but not complete: copied again
    Counts same;                       // at the target, same size and time
    Counts same_content;               // at the target with another time, same content (checked)
    Counts same_sampled;               // ... of which checked only in samples (their time is left as it is)
    Counts conflict;                   // at the target, different
    Counts target_only;                // at the target only (not under the sources): left alone
    Counts not_important;              // left out by "only what matters"
    Counts online_only;                // cloud placeholders (OneDrive ...): not downloaded
    Counts checked;                    // files whose content was read to compare (bytes: read from the target)
    Counts sampled;                    // ... of which large ones compared in samples only (strict compares them whole)
    Counts by_cat[kCats], todo_by_cat[kCats];
    int64_t skipped_dirs = 0;          // folders left out by the rules (system, programs, caches)
    std::vector<std::pair<std::string, std::string>> errors;   // folders that could not be read, files that could not be compared
    uint64_t free_bytes = 0;           // on the target drive
    bool target_seeks = true;          // the target is a hard disk (one writer)
    bool stopped = false;
    double seconds = 0;
};

struct AnalyzeOptions {
    bool important_only = false;
    bool strict = false;               // compare whole files, however large
    std::atomic<bool>* stop = nullptr;
};

struct AnalyzeProgress {
    int64_t files = 0, bytes = 0;      // listed so far
    std::string dir;
    bool checking = false;             // the content check
    int64_t check_done = 0, check_total = 0;
};

// sources: files or folders (each lands in dest under its own name; a drive's root "C:\" as "C"); dest: a folder (made
// when copying)
Plan analyze(const std::vector<std::string>& sources, const std::string& dest, const AnalyzeOptions& opt,
             const std::function<void(const AnalyzeProgress&)>& progress);

// Leaves out what is not important (rules.hpp): the plan as if analysed with important_only.
void keep_important(Plan& p);

enum class ConflictPolicy { Skip, Overwrite, KeepBoth };
// What becomes of the conflicts: left alone (the default), copied over the target's, or copied beside it as
// "name（来自源）.ext".
void resolve_conflicts(Plan& p, ConflictPolicy policy);

}  // namespace kc
