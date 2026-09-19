// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// LMDB side of the rigorous benchmark: the IDENTICAL workload as
// bench/rigorous.cpp (durable puts, random reads, cold reads, reads under
// write pressure, scan, reopen, identically-defined write amplification), so
// no engine gets a home-field workload.
//
// Durability mapping: the environment is opened with MDB_NOSYNC so a plain
// commit is buffered. The durable-put phase then commits AND calls
// mdb_env_sync(env, 1) per key — the explicit equivalent of fdatasync per
// put, matching tinylsm's sync_wal_on_write and LevelDB's sync=true. The
// storm writer commits without syncing, matching LevelDB's async WriteOptions.
// Cold reads rely on FADV_DONTNEED over data.mdb: LMDB mmaps, so an evicted
// page is re-read from disk on the next touch.
//
// Usage: tinylsm_rigorous_lmdb [operations] [db_dir] [trial]
#include <lmdb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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

constexpr const char* kEngine = "lmdb";

void Require(int rc, const char* what) {
  if (rc != MDB_SUCCESS) throw std::runtime_error(std::string(what) + ": " + mdb_strerror(rc));
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
  s.p50 = p(.5); s.p90 = p(.9); s.p99 = p(.99); s.p999 = p(.999);
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

std::vector<std::string> DbFiles(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".mdb") out.push_back(entry.path().string());
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

struct Env {
  MDB_env* env = nullptr;
  MDB_dbi dbi = 0;

  ~Env() {
    if (env) {
      mdb_dbi_close(env, dbi);
      mdb_env_close(env);
    }
  }
  void Open(const fs::path& dir) {
    Require(mdb_env_create(&env), "env create");
    Require(mdb_env_set_mapsize(env, 1ull << 30), "mapsize");  // 1 GiB: headroom for the workload
    // NOSYNC: commits are buffered; the durable phase syncs explicitly per
    // put. NORDAHEAD: no readahead subsidy for the cold-read phase.
    Require(mdb_env_open(env, dir.string().c_str(), MDB_NOSYNC | MDB_NORDAHEAD | MDB_NOTLS, 0644), "env open");
    MDB_txn* tx = nullptr;
    Require(mdb_txn_begin(env, nullptr, 0, &tx), "schema txn");
    Require(mdb_dbi_open(tx, nullptr, 0, &dbi), "dbi open");
    Require(mdb_txn_commit(tx), "schema commit");
  }
  void Sync() { Require(mdb_env_sync(env, 1), "env sync"); }
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const size_t n = argc > 1 ? std::stoull(argv[1]) : 20000;
    if (n == 0 || n > 10000000) throw std::runtime_error("count must be 1..10000000");
    fs::path path = argc > 2 ? fs::path(argv[2]) : fs::temp_directory_path() / ("lmdb-rig-" + std::to_string(getpid()));
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
    {std::error_code mkec;fs::create_directories(path,mkec);}  // mdb_env_open does not create the directory

    constexpr size_t kMemtable = 1 * 1024 * 1024;  // informational: LMDB has no memtable knob
    constexpr size_t kCache = 64 * 1024 * 1024;    // informational: LMDB reads via mmap, no block cache

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
    const std::string_view vref = value;
    const std::string_view wref = overwrite;

    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937(42));

    Env db;
    db.Open(path);

    // Phase 1: durable puts, shuffled order — commit + explicit sync per put.
    {
      std::vector<double> samples;
      samples.reserve(n);
      for (size_t idx : order) {
        auto start = Clock::now();
        MDB_txn* tx = nullptr;
        Require(mdb_txn_begin(db.env, nullptr, 0, &tx), "put txn");
        MDB_val k{keys[idx].size(), const_cast<char*>(keys[idx].data())};
        MDB_val v{value.size(), const_cast<char*>(value.data())};
        Require(mdb_put(tx, db.dbi, &k, &v, 0), "durable put");
        Require(mdb_txn_commit(tx), "durable commit");
        db.Sync();
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      double secs = std::accumulate(samples.begin(), samples.end(), 0.0) / 1e6;
      ReportLatency(trial, "durable_put", samples, n / secs);
    }

    db.Sync();

    // Phase 2: uniform-random reads (one read txn per get: ordinary LMDB usage).
    std::vector<size_t> read_order(n);
    std::iota(read_order.begin(), read_order.end(), 0);
    std::shuffle(read_order.begin(), read_order.end(), std::mt19937(1234));
    {
      MDB_val out;
      for (size_t idx : read_order) {
        MDB_txn* tx = nullptr;
        Require(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &tx), "warmup txn");
        MDB_val k{keys[idx].size(), const_cast<char*>(keys[idx].data())};
        Require(mdb_get(tx, db.dbi, &k, &out), "warmup get");
        Require(mdb_txn_commit(tx), "warmup end");
      }
      std::vector<double> samples;
      samples.reserve(n);
      for (size_t idx : read_order) {
        auto start = Clock::now();
        MDB_txn* tx = nullptr;
        Require(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &tx), "read txn");
        MDB_val k{keys[idx].size(), const_cast<char*>(keys[idx].data())};
        Require(mdb_get(tx, db.dbi, &k, &out), "random get");
        Require(mdb_txn_commit(tx), "read end");
        if (std::string_view(static_cast<const char*>(out.mv_data), out.mv_size) != vref)
          throw std::runtime_error("wrong value");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      ReportLatency(trial, "random_get", samples);
    }

    // Phase 3: cold reads. LMDB reads through a persistent mmap, and
    // FADV_DONTNEED cannot drop pages pinned by a mapping (no root for the
    // global cache drop either). The honest equivalent: per read, copy the
    // data file to a fresh path, fsync + DONTNEED it so the copy is uncached,
    // then open a fresh env over the copy and read. The copy itself is not
    // timed; the measured read faults from actual disk.
    {
      fs::path cold_dir = path.parent_path() / ("lmdb-cold-" + std::to_string(::getpid()));
      fs::path data_src;
      for (auto& f : DbFiles(path)) if (f.find("data.mdb") != std::string::npos) data_src = f;
      if (data_src.empty() && !DbFiles(path).empty()) data_src = DbFiles(path).front();
      EvictPageCache(DbFiles(path), true);
      const size_t count = std::min<size_t>(n, 200);  // copy-per-read is expensive: fewer samples, honestly noted
      std::mt19937 rng(777);
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      std::vector<double> samples;
      samples.reserve(count);
      MDB_val out;
      for (size_t i = 0; i < count; ++i) {
        size_t idx = pick(rng);
        std::error_code ec;
        fs::remove_all(cold_dir, ec);
        fs::create_directories(cold_dir, ec);
        fs::copy_file(data_src, cold_dir / "data.mdb", fs::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("cold copy failed");
        {
          int fd = ::open((cold_dir / "data.mdb").c_str(), O_RDONLY);
          if (fd < 0) throw std::runtime_error("cold copy open failed");
          ::fsync(fd);  // writeback: DONTNEED cannot drop dirty pages
          ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
          ::close(fd);
        }
        auto start = Clock::now();
        Env cold;
        cold.Open(cold_dir);
        MDB_txn* tx = nullptr;
        Require(mdb_txn_begin(cold.env, nullptr, MDB_RDONLY, &tx), "cold txn");
        MDB_val k{keys[idx].size(), const_cast<char*>(keys[idx].data())};
        Require(mdb_get(tx, cold.dbi, &k, &out), "cold get");
        Require(mdb_txn_commit(tx), "cold end");
        if (std::string_view(static_cast<const char*>(out.mv_data), out.mv_size) != vref)
          throw std::runtime_error("wrong cold value");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      std::error_code ec;fs::remove_all(cold_dir, ec);
      ReportLatency(trial, "cold_get", samples, -1.0, "cold=fresh_copy_on_disk");
    }

    // Phase 4: reads under write pressure (buffered overwrites).
    {
      std::atomic<bool> stop{false};
      std::atomic<uint64_t> written{0};
      std::atomic<bool> wok{true};
      auto wstart = Clock::now();
      std::thread writer([&] {
        size_t pos = 0;
        while (!stop.load()) {
          MDB_txn* tx = nullptr;
          if (mdb_txn_begin(db.env, nullptr, 0, &tx) != MDB_SUCCESS) { wok = false; return; }
          const std::string& kstr = keys[order[pos % n]];
          MDB_val k{kstr.size(), const_cast<char*>(kstr.data())};
          MDB_val v{overwrite.size(), const_cast<char*>(overwrite.data())};
          if (mdb_put(tx, db.dbi, &k, &v, 0) != MDB_SUCCESS || mdb_txn_commit(tx) != MDB_SUCCESS) {
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
      MDB_val out;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      auto rstart = Clock::now();
      while (Clock::now() - rstart < std::chrono::seconds(2)) {
        size_t idx = pick(rng);
        auto start = Clock::now();
        MDB_txn* tx = nullptr;
        Require(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &tx), "storm txn");
        MDB_val k{keys[idx].size(), const_cast<char*>(keys[idx].data())};
        int rc = mdb_get(tx, db.dbi, &k, &out);
        std::string_view got;
        if (rc == MDB_SUCCESS) got = std::string_view(static_cast<const char*>(out.mv_data), out.mv_size);
        Require(mdb_txn_commit(tx), "storm end");
        Require(rc, "read under write");
        if (got != vref && got != wref) throw std::runtime_error("read under write wrong value");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      stop = true;
      writer.join();
      if (!wok) throw std::runtime_error("writer failed");
      double wsecs = std::chrono::duration<double>(Clock::now() - wstart).count();
      ReportLatency(trial, "read_under_write", samples, written.load() / wsecs, "writer=buffered");
      db.Sync();
    }

    // Phase 5: full scan, timed from cursor setup to match the other drivers.
    {
      auto start = Clock::now();
      MDB_txn* tx = nullptr;
      Require(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &tx), "scan txn");
      MDB_cursor* cur = nullptr;
      Require(mdb_cursor_open(tx, db.dbi, &cur), "scan cursor");
      MDB_val k, out;
      size_t count = 0;
      while (mdb_cursor_get(cur, &k, &out, MDB_NEXT) == MDB_SUCCESS) ++count;
      mdb_cursor_close(cur);
      Require(mdb_txn_commit(tx), "scan end");
      double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      if (count != n) throw std::runtime_error("scan count mismatch");
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"scan\""
                << ",\"records\":" << count << ",\"total_ms\":" << ms << ",\"ns_per_key\":" << ms * 1e6 / count << "}\n";
    }

    // Phase 6: close + reopen cost with verification. An LMDB reopen is an
    // mmap: the honest number under the same definition as the other engines.
    {
      mdb_dbi_close(db.env, db.dbi);
      mdb_env_close(db.env);
      db.env = nullptr;
      auto start = Clock::now();
      db.Open(path);
      double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      std::mt19937 rng(555);
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      MDB_val out;
      for (int i = 0; i < 1000; ++i) {
        size_t idx = pick(rng);
        MDB_txn* tx = nullptr;
        Require(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &tx), "post-reopen txn");
        MDB_val k{keys[idx].size(), const_cast<char*>(keys[idx].data())};
        Require(mdb_get(tx, db.dbi, &k, &out), "post-reopen get");
        Require(mdb_txn_commit(tx), "post-reopen end");
        std::string_view got(static_cast<const char*>(out.mv_data), out.mv_size);
        if (got != vref && got != wref) throw std::runtime_error("post-reopen wrong value");
      }
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"reopen\""
                << ",\"reopen_ms\":" << ms << ",\"verify_gets\":1000}\n";
    }

    // Phase 7: identically-defined write amplification — on-disk db bytes over
    // iterator-scanned live logical bytes. LMDB's data.mdb never shrinks, so
    // its live_ratio carries the free-page overhead; that is the honest number
    // under the same definition, not a different one.
    {
      db.Sync();
      MDB_txn* tx = nullptr;
      Require(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &tx), "amp txn");
      MDB_cursor* cur = nullptr;
      Require(mdb_cursor_open(tx, db.dbi, &cur), "amp cursor");
      MDB_val k, out;
      uint64_t records = 0, logical = 0;
      while (mdb_cursor_get(cur, &k, &out, MDB_NEXT) == MDB_SUCCESS) {
        ++records;
        logical += k.mv_size + out.mv_size;
      }
      mdb_cursor_close(cur);
      Require(mdb_txn_commit(tx), "amp end");
      uint64_t live = 0;
      std::error_code ec;
      for (auto& f : DbFiles(path)) live += fs::file_size(f, ec);
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"write_amp\""
                << ",\"live_ratio\":" << (logical == 0 ? 0.0 : double(live) / logical) << ",\"live_records\":" << records
                << ",\"live_logical_bytes\":" << logical << ",\"live_sst_bytes\":" << live
                << ",\"definition\":\"live_db_bytes/live_logical_bytes (iterator-scanned, identical for all engines)\"}\n";
    }

    fs::remove_all(path);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rigorous_lmdb: " << error.what() << '\n';
    return 1;
  }
}
