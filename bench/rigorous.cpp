// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Rigorous benchmark driver.
//
// Addresses the legitimacy holes of bench/main.cpp:
//   1. DURABILITY PRICED: writes use sync_wal_on_write=true (fdatasync per Put),
//      the only write number that means anything for a crash-consistent engine.
//      bench/main.cpp measured buffered (unsynced) puts: pure skiplist speed.
//   2. FORCED STEADY STATE: 1 MiB memtable with a ~2.3 MiB dataset forces real
//      flushes during the write phase instead of fitting everything in RAM.
//   3. ADVERSARIAL ORDER: shuffled put order, uniform-random read order.
//      Sequential key order is the best case for prefix compression, prefetch,
//      and branch prediction; it flatters both engines and hides LSM overlap.
//   4. TAIL UNDER PRESSURE: read_under_write measures reader p99/p999 WHILE a
//      writer hammers overwrites (triggering flushes). Quiescent post-Compact
//      reads hide exactly the stalls an LSM must be judged on.
//   5. FULL TAIL: p50/p90/p99/p999 + mean + max, not three summary points.
//   6. CALIBRATED CLOCK: reports steady_clock overhead/resolution so
//      sub-microsecond claims can be judged against the measurement floor.
//   7. IDENTICAL WRITE-AMP DEFINITION: live SST bytes over iterator-scanned
//      live logical bytes, computed the same way for both engines.
//   8. COLDNESS HONESTY: ::sync() before page-cache eviction; the table script
//      flags runs where cold p50 is not clearly above warm p50 (eviction fail).
//   9. RECOVERY PRICED: close/reopen timed with read verification.
//  10. SCAN PRICED: full-iteration cost, never measured by bench/main.cpp.
//
// Usage: tinylsm_rigorous [operations] [db_dir] [trial]
#include "tinylsm/db.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
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

namespace {

void Require(tinylsm::Status s, const char* what) {
  if (s != tinylsm::Status::Ok) throw std::runtime_error(std::string(what) + ": " + tinylsm::StatusName(s));
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

void ReportLatency(const char* engine, long trial, const char* metric, std::vector<double>& samples,
                   double extra_ops = -1.0, const char* note = nullptr) {
  Summary s = Summarize(samples);
  std::cout << "{\"engine\":\"" << engine << "\",\"trial\":" << trial << ",\"metric\":\"" << metric
            << "\",\"unit\":\"us\",\"samples\":" << s.n << ",\"mean\":" << s.mean << ",\"max\":" << s.max
            << ",\"p50\":" << s.p50 << ",\"p90\":" << s.p90 << ",\"p99\":" << s.p99 << ",\"p999\":" << s.p999;
  if (extra_ops >= 0) std::cout << ",\"ops_per_second\":" << extra_ops;
  if (note) std::cout << ",\"note\":\"" << note << "\"";
  std::cout << "}\n";
}

double CalibrateClockNs() {
  // Median cost of one steady_clock::now() call: the measurement floor.
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

uint64_t SstBytes(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(dir, ec))
    if (entry.path().extension() == ".sst") total += entry.file_size(ec);
  return total;
}

std::vector<std::string> SstFiles(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(dir, ec))
    if (entry.path().extension() == ".sst") out.push_back(entry.path().string());
  return out;
}

void EvictPageCache(const std::vector<std::string>& files, bool sync_first) {
  // One full writeback before the loop; per-iteration DONTNEED after that.
  // (A ::sync() per read would benchmark the filesystem's writeback path,
  // not the engine, and take minutes.)
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
    const size_t n = argc > 1 ? std::stoull(argv[1]) : 20000;
    if (n == 0 || n > 10000000) throw std::runtime_error("count must be 1..10000000");
    fs::path path = argc > 2 ? fs::path(argv[2]) : fs::temp_directory_path() / ("tinylsm-rig-" + std::to_string(getpid()));
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

    constexpr const char* kEngine = "tinylsm";
    constexpr size_t kMemtable = 1 * 1024 * 1024;  // small on purpose: force flushes
    constexpr size_t kCache = 64 * 1024 * 1024;
    constexpr size_t kBlock = 4 * 1024;

    const double clock_ns = CalibrateClockNs();
    const double clock_res = double(Clock::period::num) / Clock::period::den * 1e9;
    std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"env\""
              << ",\"ops\":" << n << ",\"memtable_bytes\":" << kMemtable << ",\"block_cache_bytes\":" << kCache
              << ",\"block_bytes\":" << kBlock << ",\"value_bytes\":100"
              << ",\"timer_overhead_ns\":" << clock_ns << ",\"timer_resolution_ns\":" << clock_res << "}\n";

    // Fixed-width keys: fair prefix compression for both engines.
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
    std::shuffle(order.begin(), order.end(), std::mt19937(42));  // adversarial insert order

    tinylsm::Options o;
    o.db_path = path.string();
    o.memtable_size_bytes = kMemtable;
    o.block_cache_size_bytes = kCache;
    o.block_size_bytes = kBlock;
    o.sync_wal_on_write = true;  // DURABLE writes: fdatasync per Put

    tinylsm::Status st;
    auto db = tinylsm::DB::Open(o, st);
    Require(st, "open");

    // Phase 1: durable puts, shuffled order.
    {
      std::vector<double> samples;
      samples.reserve(n);
      for (size_t idx : order) {
        auto start = Clock::now();
        Require(db->Put(keys[idx], value), "durable put");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      double secs = std::accumulate(samples.begin(), samples.end(), 0.0) / 1e6;
      ReportLatency(kEngine, trial, "durable_put", samples, n / secs);
    }

    Require(db->Compact(), "compact");

    // Phase 2: uniform-random reads, page cache warm, block cache warm.
    std::vector<size_t> read_order(n);
    std::iota(read_order.begin(), read_order.end(), 0);
    std::shuffle(read_order.begin(), read_order.end(), std::mt19937(1234));
    {
      std::string got;
      for (size_t idx : read_order) Require(db->Get(keys[idx], got), "warmup get");
      std::vector<double> samples;
      samples.reserve(n);
      std::string out;
      for (size_t idx : read_order) {
        auto start = Clock::now();
        Require(db->Get(keys[idx], out), "random get");
        if (out != value) throw std::runtime_error("wrong value");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      ReportLatency(kEngine, trial, "random_get", samples);
    }

    // Phase 3: cold reads. Fresh handle with a ~disabled block cache plus real
    // page-cache eviction, timed over a uniform-random subset.
    {
      db.reset();
      EvictPageCache(SstFiles(path), true);
      tinylsm::Options cold = o;
      cold.block_cache_size_bytes = 1;
      auto cdb = tinylsm::DB::Open(cold, st);
      Require(st, "cold open");
      const size_t count = std::min<size_t>(n, 2000);
      std::mt19937 rng(777);
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      std::vector<double> samples;
      samples.reserve(count);
      std::string out;
      auto files = SstFiles(path);
      for (size_t i = 0; i < count; ++i) {
        EvictPageCache(files, false);
        size_t idx = pick(rng);
        auto start = Clock::now();
        Require(cdb->Get(keys[idx], out), "cold get");
        if (out != value) throw std::runtime_error("wrong cold value");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      cdb.reset();
      ReportLatency(kEngine, trial, "cold_get", samples);
      db = tinylsm::DB::Open(o, st);
      Require(st, "reopen after cold");
    }

    // Phase 4: reads under write pressure. Both engines' writers run in their
    // fastest BUFFERED mode here: the phase measures READER tail latency under
    // maximal write pressure (cyclical overwrites force flushes through the
    // 1 MiB memtable), so handicapping either writer with fsyncs would weaken
    // the very pressure being measured. (Durable-write cost is phase 1.)
    {
      db.reset();
      tinylsm::Options storm = o;
      storm.sync_wal_on_write = false;
      db = tinylsm::DB::Open(storm, st);
      Require(st, "reopen for storm");
      std::atomic<bool> stop{false};
      std::atomic<uint64_t> written{0};
      std::atomic<bool> wok{true};
      auto wstart = Clock::now();
      std::thread writer([&] {
        size_t pos = 0;
        while (!stop.load()) {
          if (db->Put(keys[order[pos % n]], overwrite) != tinylsm::Status::Ok) {
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
      std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let writer engage flush
      auto rstart = Clock::now();
      while (Clock::now() - rstart < std::chrono::seconds(2)) {
        size_t idx = pick(rng);
        auto start = Clock::now();
        auto s = db->Get(keys[idx], out);
        if (s != tinylsm::Status::Ok || (out != value && out != overwrite))
          throw std::runtime_error("read under write failed");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      stop = true;
      writer.join();
      if (!wok) throw std::runtime_error("writer failed");
      double wsecs = std::chrono::duration<double>(Clock::now() - wstart).count();
      ReportLatency(kEngine, trial, "read_under_write", samples, written.load() / wsecs, "writer=buffered");
    }

    // Phase 5: full scan, timed from iterator construction: tinylsm merges at
    // NewIterator time, so timing only the walk would measure a vector loop.
    {
      auto start = Clock::now();
      auto it = db->NewIterator();
      size_t count = 0;
      for (it->SeekToFirst(); it->Valid(); it->Next()) ++count;
      double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      if (it->status() != tinylsm::Status::Ok) throw std::runtime_error("scan failed");
      if (count != n) throw std::runtime_error("scan count mismatch");
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"scan\""
                << ",\"records\":" << count << ",\"total_ms\":" << ms << ",\"ns_per_key\":" << ms * 1e6 / count << "}\n";
    }

    // Phase 6: close + reopen cost with verification.
    {
      db.reset();
      auto start = Clock::now();
      db = tinylsm::DB::Open(o, st);
      Require(st, "reopen");
      double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      std::mt19937 rng(555);
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      std::string out;
      for (int i = 0; i < 1000; ++i) {
        size_t idx = pick(rng);
        Require(db->Get(keys[idx], out), "post-reopen get");
        if (out != value && out != overwrite) throw std::runtime_error("post-reopen wrong value");
      }
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"reopen\""
                << ",\"reopen_ms\":" << ms << ",\"verify_gets\":1000}\n";
    }

    // Phase 7: honest write amplification. Both engines compute it identically:
    // live SST bytes on disk over iterator-scanned live logical bytes.
    {
      Require(db->Compact(), "final compact");
      auto it = db->NewIterator();
      uint64_t records = 0, logical = 0;
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        ++records;
        logical += it->Key().size() + it->Value().size();
      }
      if (it->status() != tinylsm::Status::Ok) throw std::runtime_error("amp scan failed");
      uint64_t live = SstBytes(path);
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"write_amp\""
                << ",\"live_ratio\":" << (logical == 0 ? 0.0 : double(live) / logical) << ",\"live_records\":" << records
                << ",\"live_logical_bytes\":" << logical << ",\"live_sst_bytes\":" << live
                << ",\"definition\":\"live_sst_bytes/live_logical_bytes (iterator-scanned, identical for both engines)\"}\n";
    }

    db.reset();
    fs::remove_all(path);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "rigorous: " << e.what() << '\n';
    return 1;
  }
}
