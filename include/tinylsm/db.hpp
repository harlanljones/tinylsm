// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tinylsm {
struct Options {
  size_t memtable_size_bytes = 32 * 1024 * 1024;
  size_t block_cache_size_bytes = 64 * 1024 * 1024;
  size_t block_size_bytes = 4 * 1024;
  bool sync_wal_on_write = false;
  std::string db_path = "./data";
};
enum class Status { Ok, NotFound, Corruption, IOError, InvalidArgument };
// Section 3.2 durability policies. Selected through Options::sync_wal_on_write:
// Fsync == true (fdatasync per batch), Buffered == false (OS page cache).
enum class SyncMode { Fsync, Buffered };
const char* StatusName(Status status);
struct Statistics {
  uint64_t writes = 0, reads = 0, flushes = 0, compactions = 0;
  uint64_t logical_bytes = 0, table_bytes_written = 0;
  uint64_t cache_hits = 0, cache_misses = 0;
  size_t active_bytes = 0, immutable_count = 0;
  std::vector<size_t> level_files;
};
class DB {
 public:
  static std::unique_ptr<DB> Open(const Options& options, Status& status);
  virtual ~DB() = default;
  virtual Status Put(std::string_view key, std::string_view value) = 0;
  virtual Status Get(std::string_view key, std::string& value) = 0;
  virtual Status Delete(std::string_view key) = 0;
  virtual Status Sync() = 0;
  class Iterator {
   public:
    virtual ~Iterator() = default;
    virtual bool Valid() const = 0;
    virtual void SeekToFirst() = 0;
    virtual void Seek(std::string_view key) = 0;
    virtual void Next() = 0;
    virtual std::string_view Key() const = 0;
    virtual std::string_view Value() const = 0;
    // Extension: distinguish an empty scan from a failed scan.
    virtual tinylsm::Status status() const = 0;
  };
  virtual std::unique_ptr<Iterator> NewIterator() = 0;
  // Blocking maintenance extensions used by tooling and the portfolio demo.
  virtual Status Flush() = 0;
  virtual Status Compact() = 0;
  virtual Statistics Stats() const = 0;
};
}  // namespace tinylsm
