// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// LevelDB measured with the identical workload and reporting format as
// bench/main.cpp, so the portfolio charts compare like with like: same key and
// value sizes, same write buffer / block size / block cache, same phases
// (buffered puts, cached reads, cold reads, 4-thread writes, SST footprint).
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/options.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/vfs.h>
#endif

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace leveldb {
// From util/env_posix_test_helper.h, which LevelDB does not install; the symbol
// is exported by libleveldb.a, so redeclaring it here with public access links.
class EnvPosixTestHelper {
 public:
  static void SetReadOnlyMMapLimit(int limit);
};
}  // namespace leveldb

static bool IsTmpfs(const std::string& path) {
#if defined(__linux__)
  struct statfs info{};
  if (::statfs(path.c_str(), &info) != 0) return false;
  return static_cast<unsigned long>(info.f_type) == 0x01021994UL;
#else
  (void)path;
  return false;
#endif
}

// Same JSON record shape as bench/main.cpp, with an "engine" discriminator.
static void Report(const char* name, std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  auto p = [&](double q) { return samples[std::min(samples.size() - 1, static_cast<size_t>(q * samples.size()))]; };
  std::cout << "{\"engine\":\"leveldb\",\"metric\":\"" << name << "\",\"unit\":\"us\",\"samples\":" << samples.size()
            << ",\"p50\":" << p(.5) << ",\"p90\":" << p(.9) << ",\"p99\":" << p(.99) << "}\n";
}

static uint64_t SstBytes(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".ldb" || entry.path().extension() == ".sst")
      total += entry.file_size(ec);
  }
  return total;
}

static leveldb::Options Tuned(size_t cache_bytes) {
  leveldb::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 32 * 1024 * 1024;  // matches Options::memtable_size_bytes
  options.block_size = 4 * 1024;                 // matches Options::block_size_bytes
  options.block_cache = leveldb::NewLRUCache(cache_bytes);
  options.filter_policy = leveldb::NewBloomFilterPolicy(10);
  // tinylsm stores records uncompressed, so the identical workload disables
  // LevelDB's Snappy; otherwise byte counts and cold-read sizes are skewed.
  options.compression = leveldb::kNoCompression;
  return options;
}

int main(int argc, char** argv) {
  try {
    // tinylsm reads its SSTables with pread, so posix_fadvise(DONTNEED) really
    // evicts them from the page cache in the cold-read phase. LevelDB mmaps
    // table files by default, and fadvise cannot evict pages that still have
    // live mappings, which would make "cold" LevelDB reads hit the page cache
    // (and would let cached reads bypass the block cache). Zero the mmap limit
    // before the default Env is created so both engines read through pread.
    leveldb::EnvPosixTestHelper::SetReadOnlyMMapLimit(0);
    const size_t n = argc > 1 ? std::stoull(argv[1]) : 20000;
    fs::path path = argc > 2 ? fs::path(argv[2])
                             : fs::temp_directory_path() / ("leveldb-bench-" + std::to_string(getpid()));
    fs::remove_all(path);
    const bool disk_backed = !IsTmpfs(path.parent_path().string());

    leveldb::DB* raw = nullptr;
    leveldb::Status status = leveldb::DB::Open(Tuned(64 * 1024 * 1024), path.string(), &raw);
    if (!status.ok()) throw std::runtime_error("leveldb open: " + status.ToString());
    std::unique_ptr<leveldb::DB> db(raw);

    const std::string value(100, 'v');
    std::vector<std::string> keys;
    for (size_t i = 0; i < n; ++i) keys.push_back("key" + std::to_string(i));

    std::vector<double> samples;
    for (auto& key : keys) {
      auto start = Clock::now();
      auto s = db->Put(leveldb::WriteOptions(), key, value);
      if (!s.ok()) throw std::runtime_error("leveldb put: " + s.ToString());
      samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    Report("buffered_put", samples);

    db->CompactRange(nullptr, nullptr);
    std::string read;
    for (auto& key : keys)
      if (!db->Get(leveldb::ReadOptions(), key, &read).ok()) throw std::runtime_error("leveldb warm get");
    samples.clear();
    for (auto& key : keys) {
      auto start = Clock::now();
      if (!db->Get(leveldb::ReadOptions(), key, &read).ok()) throw std::runtime_error("leveldb get");
      samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    Report("cached_get", samples);

    std::atomic<bool> ok{true};
    auto start = Clock::now();
    std::vector<std::thread> clients;
    for (int t = 0; t < 4; ++t) {
      clients.emplace_back([&, t] {
        for (size_t i = static_cast<size_t>(t); i < n; i += 4)
          if (!db->Put(leveldb::WriteOptions(), keys[i], value).ok()) ok = false;
      });
    }
    for (auto& thread : clients) thread.join();
    if (!ok) throw std::runtime_error("leveldb concurrent writes failed");
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "{\"engine\":\"leveldb\",\"metric\":\"concurrent_write\",\"threads\":4,\"ops_per_second\":"
              << n / seconds << "}\n";

    db->CompactRange(nullptr, nullptr);
    const uint64_t live = SstBytes(path);
    const double logical = static_cast<double>(n) * static_cast<double>(value.size() + 3);
    // Live table bytes divided by logical bytes: the definition both engines
    // report in the comparison chart.
    std::cout << "{\"engine\":\"leveldb\",\"metric\":\"sst_write_amplification\",\"ratio\":" << live / logical
              << ",\"live_ratio\":" << live / logical << ",\"logical_bytes\":" << logical << ",\"sst_bytes\":" << live
              << ",\"definition\":\"live_sst_bytes/logical_bytes\"}\n";

    // Cold reads: a second handle measured with fill_cache=false (cacheless
    // reads) plus page-cache eviction, mirroring the tinylsm cold-read
    // protocol; mmap reads are disabled at the top of main so the eviction
    // can actually reach the pages.
    db.reset();
    std::vector<std::string> sstables;
    for (auto& entry : fs::directory_iterator(path))
      if (entry.path().extension() == ".ldb" || entry.path().extension() == ".sst")
        sstables.push_back(entry.path().string());
    if (sstables.empty()) {
      std::cerr << "leveldb cold_get not measured: no tables under " << path << '\n';
    } else if (!disk_backed) {
      std::cerr << "leveldb cold_get not measured: " << path << " is on tmpfs\n";
    } else {
      std::vector<int> fds;
      for (auto& file : sstables) {
        int fd = ::open(file.c_str(), O_RDONLY);
        if (fd >= 0) fds.push_back(fd);
      }
      leveldb::DB* cold_raw = nullptr;
      status = leveldb::DB::Open(Tuned(1), path.string(), &cold_raw);
      if (!status.ok()) throw std::runtime_error("leveldb cold open: " + status.ToString());
      std::unique_ptr<leveldb::DB> cold(cold_raw);
      const size_t count = std::min<size_t>(n, 2000);
      std::vector<double> cold_samples;
      // fill_cache=false makes the read path cacheless, matching tinylsm's
      // capacity-1 block cache. A tiny LRUCache alone is not enough: LevelDB
      // only evicts on Insert, so the most recent block stays resident and
      // sequential cold keys would keep hitting it through Lookup.
      leveldb::ReadOptions cold_read;
      cold_read.fill_cache = false;
      for (size_t i = 0; i < count && !fds.empty(); ++i) {
        for (int fd : fds) ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        auto& key = keys[(i * sstables.size()) % n];
        auto begin = Clock::now();
        if (!cold->Get(cold_read, key, &read).ok()) throw std::runtime_error("leveldb cold get");
        cold_samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
      }
      for (int fd : fds) ::close(fd);
      if (!cold_samples.empty()) Report("cold_get", cold_samples);
      cold.reset();
    }

    fs::remove_all(path);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}