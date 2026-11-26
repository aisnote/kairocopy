// manifest.hpp — the record of what kairocopy copied to a drive: <drive>\.kairocopy\manifest.db (SQLite). One row a copied
// file: where it went, where it came from, its size and time; one row a run. With it a copy that was stopped goes on where
// it was, and nothing is copied twice.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/db.hpp"

namespace kc {

struct CopiedFile {
    std::string dst, src;
    uint64_t size = 0;
    int64_t mtime = 0;           // the source's last write time (100 ns ticks)
    std::string hash;            // xxh3-128 when the copy was checked
};

class Manifest {
  public:
    // the record for the drive holding `dest` (its root; `dest` itself when the root cannot be written)
    explicit Manifest(const std::string& dest);
    const std::string& path() const { return path_; }
    struct Rec { uint64_t size; int64_t mtime; };
    // what was copied to paths under `dir` (keys: the destination path in lower case)
    std::unordered_map<std::string, Rec> under(const std::string& dir);
    void record(const std::vector<CopiedFile>& files);
    int64_t begin_run(const std::string& sources, const std::string& dest, int64_t files, int64_t bytes);
    void end_run(int64_t run, int64_t files, int64_t bytes, int64_t errors, bool stopped);
    struct Summary { int64_t files = 0, bytes = 0, runs = 0; std::string last_run; };
    Summary summary(const std::string& dir);   // what was copied under `dir` ("" all)
  private:
    std::string path_;
    std::unique_ptr<Db> db_;
};

}  // namespace kc
