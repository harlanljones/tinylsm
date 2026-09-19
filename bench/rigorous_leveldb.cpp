// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// LevelDB side of the rigorous benchmark: the IDENTICAL workload as
// bench/rigorous.cpp (durable puts, random reads, cold reads, reads under
// write pressure, scan, reopen, identically-defined write amplification), so
// neither engine gets a home-field workload.
//
// Usage: tinylsm_rigorous_leveldb [operations] [db_dir] [trial]
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/options.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#if defined(__linux__)
#include <sys/vfs.h>
#endif
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace leveldb {
// From util/env_posix_test_helper.h, which LevelDB does not install; the symbol
// is exported by libleveldb.a, so redeclaring it here links. Zeroing the mmap
// limit forces pread reads so page-cache eviction actually evicts.
class EnvPosixTestHelper {
 public:
  static void SetReadOnlyMMapLimit(int limit);
};
}  // namespace leveldb

namespace {

constexpr const char* kEngine = "leveldb";

void Require(const leveldb::Status& s, const char* what) {
  if (!s.ok()) throw std::runtime_error(std::string(what) + ": " + s.ToString());
}

struct Summary {
  size_t n = 0;
  double mean = 0, max = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0;
};

Summary Summarize(std::vector<double>& samples) {
  if (samples.empty()) throw std::runtime_error("no samples");
  std::sort(samples.begin(), samples.end());
  Summary s;
  s.n = samples.size();
  s.mean = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
  s.max = samples.back();
  auto p = [&](double q) { return samples[std::min(samples.size() - 1, static_cast<size_t>(q * samples.size()))]; };
  s.p50 = p(.5);
  s.p90 = p(.9);
  s.p99 = p(.99);
  s.p999 = p(.999);
  return s;
}

void ReportLatency(long trial, const char* metric, std::vector<double>& samples, double extra_ops = -1.0,
                   const char* note = nullptr) {
  Summary s = Summarize(samples);
  std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"" << metric
            << "\",\"unit\":\"us\",\"samples\":" << s.n << ",\"mean\":" << s.mean << ",\"max\":" << s.max
            << ",\"p50\":" << s.p50 << ",\"p90\":" << s.p90 << ",\"p99\":" << s.p99 << ",\"p999\":" << s.p999;
  if (extra_ops >= 0) std::cout << ",\"ops_per_second\":" << extra_ops;
  if (note) std::cout << ",\"note\":\"" << note << "\"";
  std::cout << "}\n";
}

double CalibrateClockNs() {
  std::vector<double> deltas;
  deltas.reserve(20000);
  for (int i = 0; i < 20000; ++i) {
    auto a = Clock::now();
    auto b = Clock::now();
    deltas.push_back(std::chrono::duration<double, std::nano>(b - a).count());
  }
  std::sort(deltas.begin(), deltas.end());
  return deltas[deltas.size() / 2];
}

leveldb::Options Tuned(size_t cache_bytes, size_t memtable_bytes) {
  leveldb::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = memtable_bytes;  // match tinylsm memtable
  options.block_size = 4 * 1024;
  options.block_cache = leveldb::NewLRUCache(cache_bytes);
  options.filter_policy = leveldb::NewBloomFilterPolicy(10);
  options.compression = leveldb::kNoCompression;  // tinylsm stores uncompressed
  return options;
}

uint64_t SstBytes(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(dir, ec)) {
    auto ext = entry.path().extension();
    if (ext == ".ldb" || ext == ".sst") total += entry.file_size(ec);
  }
  return total;
}

std::vector<std::string> SstFiles(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(dir, ec)) {
    auto ext = entry.path().extension();
    if (ext == ".ldb" || ext == ".sst") out.push_back(entry.path().string());
  }
  return out;
}

void EvictPageCache(const std::vector<std::string>& files, bool sync_first) {
  // One full writeback before the loop; per-iteration DONTNEED after that.
  if (sync_first) ::sync();
  for (auto& f : files) {
    int fd = ::open(f.c_str(), O_RDONLY);
    if (fd < 0) continue;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    leveldb::EnvPosixTestHelper::SetReadOnlyMMapLimit(0);
    const size_t n = argc > 1 ? std::stoull(argv[1]) : 20000;
    if (n == 0 || n > 10000000) throw std::runtime_error("count must be 1..10000000");
    fs::path path = argc > 2 ? fs::path(argv[2]) : fs::temp_directory_path() / ("leveldb-rig-" + std::to_string(getpid()));
    const long trial = argc > 3 ? std::stol(argv[3]) : 0;
#if defined(__linux__)
    {
      // tmpfs has no device behind the page cache: durable puts and "cold"
      // reads measured there are RAM benchmarks wearing a costume.
      struct statfs info {};
      if (::statfs(path.parent_path().string().c_str(), &info) == 0 &&
          static_cast<unsigned long>(info.f_type) == 0x01021994UL)
        std::cerr << "WARNING: " << path.string()
                  << " is on tmpfs; durable_put/cold_get numbers are not device measurements\n";
    }
#endif
    fs::remove_all(path);

    constexpr size_t kMemtable = 1 * 1024 * 1024;
    constexpr size_t kCache = 64 * 1024 * 1024;

    const double clock_ns = CalibrateClockNs();
    const double clock_res = double(Clock::period::num) / Clock::period::den * 1e9;
    std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"env\""
              << ",\"ops\":" << n << ",\"memtable_bytes\":" << kMemtable << ",\"block_cache_bytes\":" << kCache
              << ",\"block_bytes\":4096,\"value_bytes\":100"
              << ",\"timer_overhead_ns\":" << clock_ns << ",\"timer_resolution_ns\":" << clock_res << "}\n";

    char keybuf[32];
    std::vector<std::string> keys;
    keys.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      std::snprintf(keybuf, sizeof keybuf, "key%08zu", i);
      keys.emplace_back(keybuf);
    }
    const std::string value(100, 'v');
    const std::string overwrite(100, 'w');

    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937(42));

    leveldb::Options options = Tuned(kCache, kMemtable);
    leveldb::DB* raw = nullptr;
    Require(leveldb::DB::Open(options, path.string(), &raw), "open");
    std::unique_ptr<leveldb::DB> db(raw);

    leveldb::WriteOptions durable;
    durable.sync = true;  // fdatasync per Put: the honest write number

    // Phase 1: durable puts, shuffled order.
    {
      std::vector<double> samples;
      samples.reserve(n);
      for (size_t idx : order) {
        auto start = Clock::now();
        Require(db->Put(durable, keys[idx], value), "durable put");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      double secs = std::accumulate(samples.begin(), samples.end(), 0.0) / 1e6;
      ReportLatency(trial, "durable_put", samples, n / secs);
    }

    db->CompactRange(nullptr, nullptr);

    // Phase 2: uniform-random reads.
    std::vector<size_t> read_order(n);
    std::iota(read_order.begin(), read_order.end(), 0);
    std::shuffle(read_order.begin(), read_order.end(), std::mt19937(1234));
    {
      std::string got;
      for (size_t idx : read_order) Require(db->Get(leveldb::ReadOptions(), keys[idx], &got), "warmup get");
      std::vector<double> samples;
      samples.reserve(n);
      std::string out;
      for (size_t idx : read_order) {
        auto start = Clock::now();
        Require(db->Get(leveldb::ReadOptions(), keys[idx], &out), "random get");
        if (out != value) throw std::runtime_error("wrong value");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      ReportLatency(trial, "random_get", samples);
    }

    // Phase 3: cold reads with a cacheless handle plus page-cache eviction.
    {
      db.reset();
      EvictPageCache(SstFiles(path), true);
      leveldb::Options cold_opts = Tuned(1, kMemtable);
      leveldb::DB* cold_raw = nullptr;
      Require(leveldb::DB::Open(cold_opts, path.string(), &cold_raw), "cold open");
      std::unique_ptr<leveldb::DB> cdb(cold_raw);
      const size_t count = std::min<size_t>(n, 2000);
      std::mt19937 rng(777);
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      std::vector<double> samples;
      samples.reserve(count);
      std::string out;
      auto files = SstFiles(path);
      // fill_cache=false: a tiny LRU alone is not enough, LevelDB only evicts
      // on Insert so the resident block would keep serving sequential keys.
      leveldb::ReadOptions cold_read;
      cold_read.fill_cache = false;
      for (size_t i = 0; i < count; ++i) {
        EvictPageCache(files, false);
        size_t idx = pick(rng);
        auto start = Clock::now();
        Require(cdb->Get(cold_read, keys[idx], &out), "cold get");
        if (out != value) throw std::runtime_error("wrong cold value");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      cdb.reset();
      ReportLatency(trial, "cold_get", samples);
      leveldb::DB* raw2 = nullptr;
      Require(leveldb::DB::Open(options, path.string(), &raw2), "reopen after cold");
      db.reset(raw2);
    }

    // Phase 4: reads under write pressure.
    {
      std::atomic<bool> stop{false};
      std::atomic<uint64_t> written{0};
      std::atomic<bool> wok{true};
      auto wstart = Clock::now();
      std::thread writer([&] {
        size_t pos = 0;
        leveldb::WriteOptions async;
        while (!stop.load()) {
          if (!db->Put(async, keys[order[pos % n]], overwrite).ok()) {
            wok = false;
            return;
          }
          ++written;
          ++pos;
        }
      });
      std::mt19937 rng(999);
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      std::vector<double> samples;
      samples.reserve(20000);
      std::string out;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      auto rstart = Clock::now();
      while (Clock::now() - rstart < std::chrono::seconds(2)) {
        size_t idx = pick(rng);
        auto start = Clock::now();
        if (!db->Get(leveldb::ReadOptions(), keys[idx], &out).ok() || (out != value && out != overwrite))
          throw std::runtime_error("read under write failed");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      stop = true;
      writer.join();
      if (!wok) throw std::runtime_error("writer failed");
      double wsecs = std::chrono::duration<double>(Clock::now() - wstart).count();
      ReportLatency(trial, "read_under_write", samples, written.load() / wsecs, "writer=buffered");
    }

    // Phase 5: full scan, timed from iterator construction to match tinylsm,
    // whose merge happens inside NewIterator.
    {
      auto start = Clock::now();
      std::unique_ptr<leveldb::Iterator> it(db->NewIterator(leveldb::ReadOptions()));
      size_t count = 0;
      for (it->SeekToFirst(); it->Valid(); it->Next()) ++count;
      double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      Require(it->status(), "scan");
      if (count != n) throw std::runtime_error("scan count mismatch");
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"scan\""
                << ",\"records\":" << count << ",\"total_ms\":" << ms << ",\"ns_per_key\":" << ms * 1e6 / count << "}\n";
    }

    // Phase 6: close + reopen cost with verification.
    {
      db.reset();
      auto start = Clock::now();
      leveldb::DB* rraw = nullptr;
      Require(leveldb::DB::Open(options, path.string(), &rraw), "reopen");
      db.reset(rraw);
      double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      std::mt19937 rng(555);
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      std::string out;
      for (int i = 0; i < 1000; ++i) {
        size_t idx = pick(rng);
        Require(db->Get(leveldb::ReadOptions(), keys[idx], &out), "post-reopen get");
        if (out != value && out != overwrite) throw std::runtime_error("post-reopen wrong value");
      }
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"reopen\""
                << ",\"reopen_ms\":" << ms << ",\"verify_gets\":1000}\n";
    }

    // Phase 7: identically-defined write amplification.
    {
      db->CompactRange(nullptr, nullptr);
      std::unique_ptr<leveldb::Iterator> it(db->NewIterator(leveldb::ReadOptions()));
      uint64_t records = 0, logical = 0;
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        ++records;
        logical += it->key().size() + it->value().size();
      }
      Require(it->status(), "amp scan");
      uint64_t live = SstBytes(path);
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"write_amp\""
                << ",\"live_ratio\":" << (logical == 0 ? 0.0 : double(live) / logical) << ",\"live_records\":" << records
                << ",\"live_logical_bytes\":" << logical << ",\"live_sst_bytes\":" << live
                << ",\"definition\":\"live_sst_bytes/live_logical_bytes (iterator-scanned, identical for both engines)\"}\n";
    }

    db.reset();
    // LevelDB's block cache and filter policy are heap objects owned by the
    // options, not the DB: free them explicitly.
    delete options.block_cache;
    delete options.filter_policy;
    fs::remove_all(path);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rigorous_leveldb: " << error.what() << '\n';
    return 1;
  }
}
