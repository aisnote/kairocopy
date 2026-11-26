// copier.hpp — the second step: copying what a plan says, fast, safely and resumably.
//   - every file is written as <name>.kcpart and renamed when complete: the target never holds half a file under its name
//   - each copied file is recorded in the target drive's manifest (with its source's size and time)
//   - large files go unbuffered (straight to the disk); small ones on several threads; on a hard disk one large file at a
//     time, after the small ones, in the order of its folders
//   - pause, stop (what is done is kept), files in use reported, not fatal
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "core/plan.hpp"

namespace kc {

struct CopyOptions {
    int big_writers = 0, small_writers = 0;   // 0: by the target (hard disk: 1 and 8, the large files last; SSD: 2 and 8)
    bool verify = false;                      // read both back and compare (xxh3-128), stored in the manifest
    std::atomic<bool>* stop = nullptr;
    std::atomic<bool>* pause = nullptr;
};

struct CopyProgress {
    int64_t files_done = 0, files_total = 0, bytes_done = 0, bytes_total = 0, errors = 0;
    double seconds = 0, speed = 0;            // bytes a second, over the last few seconds
    std::string current;                      // a file being copied (the largest under way)
    uint64_t current_done = 0, current_size = 0;
    bool paused = false;
};

struct CopyReport {
    int64_t files = 0, bytes = 0, errors = 0, in_use = 0;
    double seconds = 0;
    bool stopped = false, disk_full = false;
    std::vector<std::pair<std::string, std::string>> failed;   // source, why
    std::string manifest;
};

// Large files: this size and more
constexpr uint64_t kBigFile = 8ull << 20;

CopyReport copy(const Plan& plan, const CopyOptions& opt, const std::function<void(const CopyProgress&)>& progress);

}  // namespace kc
