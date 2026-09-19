// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// SQLite side of the rigorous benchmark: the IDENTICAL workload as
// bench/rigorous.cpp (durable puts, random reads, cold reads, reads under
// write pressure, scan, reopen, identically-defined write amplification), so
// no engine gets a home-field workload.
//
// Fairness mapping (documented, not hidden):
//   - Schema: WITHOUT ROWID table keyed by the key blob — a B-tree ordered by
//     key, the closest structural analogue to point reads on an LSM.
//   - Cache: 64 MiB (PRAGMA cache_size=-65536) vs tinylsm's 64 MiB block
//     cache; 4 KiB pages vs 4 KiB SST blocks.
//   - Durable puts: WAL mode + synchronous=FULL, one autocommit transaction
//     per put — an fsync per put, matching tinylsm sync_wal_on_write and
//     LevelDB sync=true. (SQLite's default rollback-journal mode is not
//     crash-comparable, so WAL is the honest choice.)
//   - Compact: SQLite has no LSM compaction; PRAGMA wal_checkpoint(TRUNCATE)
//     folds the WAL into the db file. That is the steady-state equivalent and
//     it does real merge I/O rather than being a no-op.
//   - Storm writer: synchronous=NORMAL (buffered WAL appends, no per-commit
//     fsync), matching tinylsm buffered mode and LevelDB async writes. Reader
//     and writer use separate connections; WAL mode gives the reader snapshot
//     isolation, the analogue of tinylsm's Version pin.
//   - Write amp: on-disk db bytes (db + wal after checkpoint) over
//     iterator-scanned live logical bytes, the same definition as every engine.
//
// Usage: tinylsm_rigorous_sqlite [operations] [db_dir] [trial]
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
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

constexpr const char* kEngine = "sqlite";

void Require(int rc, sqlite3* db, const char* what) {
  if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
    throw std::runtime_error(std::string(what) + ": " + (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc)));
}

void Exec(sqlite3* db, const char* sql) {
  char* err = nullptr;
  int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  std::string msg = err ? err : "";
  sqlite3_free(err);
  if (rc != SQLITE_OK) throw std::runtime_error(std::string("exec failed: ") + msg + " [" + sql + "]");
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

uint64_t DbBytes(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(dir, ec))
    if (entry.is_regular_file(ec)) total += entry.file_size(ec);
  return total;
}

std::vector<std::string> DbFiles(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(dir, ec))
    if (entry.is_regular_file(ec)) out.push_back(entry.path().string());
  return out;
}

void EvictPageCache(const std::vector<std::string>& files, bool sync_first) {
  if (sync_first) ::sync();
  for (auto& f : files) {
    int fd = ::open(f.c_str(), O_RDONLY);
    if (fd < 0) continue;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);
  }
}

struct Conn {
  sqlite3* db = nullptr;
  ~Conn() { if (db) sqlite3_close(db); }
  void Open(const fs::path& file, bool wal, const char* sync, long cache_kib) {
    int rc = sqlite3_open(file.string().c_str(), &db);
    Require(rc, db, "open");
    sqlite3_busy_timeout(db, 10000);
    Exec(db, "PRAGMA page_size=4096;");
    if (wal) Exec(db, "PRAGMA journal_mode=WAL;");
    Exec(db, (std::string("PRAGMA synchronous=") + sync + ";").c_str());
    Exec(db, (std::string("PRAGMA cache_size=") + std::to_string(cache_kib) + ";").c_str());
    Exec(db, "CREATE TABLE IF NOT EXISTS kv(k BLOB PRIMARY KEY, v BLOB) WITHOUT ROWID;");
  }
};

struct Stmt {
  sqlite3* db = nullptr;
  sqlite3_stmt* s = nullptr;
  ~Stmt() { if (s) sqlite3_finalize(s); }
  void Prepare(sqlite3* d, const char* sql) {
    db = d;
    Require(sqlite3_prepare_v2(db, sql, -1, &s, nullptr), db, "prepare");
  }
  void Reset() {
    Require(sqlite3_reset(s), db, "reset");
    Require(sqlite3_clear_bindings(s), db, "clear bindings");
  }
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const size_t n = argc > 1 ? std::stoull(argv[1]) : 20000;
    if (n == 0 || n > 10000000) throw std::runtime_error("count must be 1..10000000");
    fs::path dir = argc > 2 ? fs::path(argv[2]) : fs::temp_directory_path() / ("sqlite-rig-" + std::to_string(getpid()));
    const long trial = argc > 3 ? std::stol(argv[3]) : 0;
#if defined(__linux__)
    {
      struct statfs info {};
      if (::statfs(dir.parent_path().string().c_str(), &info) == 0 &&
          static_cast<unsigned long>(info.f_type) == 0x01021994UL)
        std::cerr << "WARNING: " << dir.string()
                  << " is on tmpfs; durable_put/cold_get numbers are not device measurements\n";
    }
#endif
    fs::remove_all(dir);
    { std::error_code mkec; fs::create_directories(dir, mkec); }
    fs::path file = dir / "bench.db";

    constexpr long kCacheKib = -65536;  // 64 MiB, matches tinylsm block cache
    constexpr long kColdCacheKib = -4;  // ~1 page: engine-side cache ~disabled

    const double clock_ns = CalibrateClockNs();
    const double clock_res = double(Clock::period::num) / Clock::period::den * 1e9;
    std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"env\""
              << ",\"ops\":" << n << ",\"memtable_bytes\":" << (1 * 1024 * 1024) << ",\"block_cache_bytes\":"
              << (64 * 1024 * 1024) << ",\"block_bytes\":4096,\"value_bytes\":100"
              << ",\"timer_overhead_ns\":" << clock_ns << ",\"timer_resolution_ns\":" << clock_res
              << ",\"sqlite_version\":\"" << sqlite3_libversion() << "\"}\n";

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

    Conn conn;
    conn.Open(file, true, "FULL", kCacheKib);

    // Phase 1: durable puts, shuffled order (one fsync'd txn per put).
    {
      Stmt put;
      put.Prepare(conn.db, "INSERT OR REPLACE INTO kv(k,v) VALUES (?,?);");
      std::vector<double> samples;
      samples.reserve(n);
      for (size_t idx : order) {
        auto start = Clock::now();
        Require(sqlite3_bind_blob(put.s, 1, keys[idx].data(), (int)keys[idx].size(), SQLITE_STATIC), conn.db, "bind k");
        Require(sqlite3_bind_blob(put.s, 2, value.data(), (int)value.size(), SQLITE_STATIC), conn.db, "bind v");
        Require(sqlite3_step(put.s), conn.db, "durable put");
        put.Reset();
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      double secs = std::accumulate(samples.begin(), samples.end(), 0.0) / 1e6;
      ReportLatency(trial, "durable_put", samples, n / secs);
    }

    Exec(conn.db, "PRAGMA wal_checkpoint(TRUNCATE);");

    // Phase 2: uniform-random reads.
    std::vector<size_t> read_order(n);
    std::iota(read_order.begin(), read_order.end(), 0);
    std::shuffle(read_order.begin(), read_order.end(), std::mt19937(1234));
    {
      Stmt get;
      get.Prepare(conn.db, "SELECT v FROM kv WHERE k=?;");
      std::string got;
      for (size_t idx : read_order) {
        Require(sqlite3_bind_blob(get.s, 1, keys[idx].data(), (int)keys[idx].size(), SQLITE_STATIC), conn.db, "bind");
        int rc = sqlite3_step(get.s);
        Require(rc, conn.db, "warmup get");
        if (rc != SQLITE_ROW) throw std::runtime_error("warmup missing key");
        get.Reset();
      }
      std::vector<double> samples;
      samples.reserve(n);
      for (size_t idx : read_order) {
        auto start = Clock::now();
        Require(sqlite3_bind_blob(get.s, 1, keys[idx].data(), (int)keys[idx].size(), SQLITE_STATIC), conn.db, "bind");
        int rc = sqlite3_step(get.s);
        Require(rc, conn.db, "random get");
        if (rc != SQLITE_ROW) throw std::runtime_error("missing key");
        const void* pv = sqlite3_column_blob(get.s, 0);
        int nb = sqlite3_column_bytes(get.s, 0);
        if (nb != (int)value.size() || std::string_view((const char*)pv, nb) != value)
          throw std::runtime_error("wrong value");
        get.Reset();
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      ReportLatency(trial, "random_get", samples);
    }

    // Phase 3: cold reads with a cacheless handle plus page-cache eviction.
    {
      conn = Conn();
      EvictPageCache(DbFiles(dir), true);
      Conn cold;
      cold.Open(file, true, "FULL", kColdCacheKib);
      const size_t count = std::min<size_t>(n, 2000);
      std::mt19937 rng(777);
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      Stmt get;
      get.Prepare(cold.db, "SELECT v FROM kv WHERE k=?;");
      std::vector<double> samples;
      samples.reserve(count);
      auto files = DbFiles(dir);
      for (size_t i = 0; i < count; ++i) {
        EvictPageCache(files, false);
        size_t idx = pick(rng);
        auto start = Clock::now();
        Require(sqlite3_bind_blob(get.s, 1, keys[idx].data(), (int)keys[idx].size(), SQLITE_STATIC), cold.db, "bind");
        int rc = sqlite3_step(get.s);
        Require(rc, cold.db, "cold get");
        if (rc != SQLITE_ROW) throw std::runtime_error("cold missing key");
        const void* pv = sqlite3_column_blob(get.s, 0);
        int nb = sqlite3_column_bytes(get.s, 0);
        if (nb != (int)value.size() || std::string_view((const char*)pv, nb) != value)
          throw std::runtime_error("wrong cold value");
        get.Reset();
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      ReportLatency(trial, "cold_get", samples);
      conn.Open(file, true, "FULL", kCacheKib);
    }

    // Phase 4: reads under write pressure (buffered overwrites).
    {
      conn = Conn();
      Conn storm;
      storm.Open(file, true, "NORMAL", kCacheKib);
      std::atomic<bool> stop{false};
      std::atomic<uint64_t> written{0};
      std::atomic<bool> wok{true};
      auto wstart = Clock::now();
      std::thread writer([&] {
        Stmt put;
        try {
          put.Prepare(storm.db, "INSERT OR REPLACE INTO kv(k,v) VALUES (?,?);");
        } catch (...) { wok = false; return; }
        size_t pos = 0;
        while (!stop.load()) {
          if (sqlite3_bind_blob(put.s, 1, keys[order[pos % n]].data(),
                                (int)keys[order[pos % n]].size(), SQLITE_STATIC) != SQLITE_OK ||
              sqlite3_bind_blob(put.s, 2, overwrite.data(), (int)overwrite.size(), SQLITE_STATIC) != SQLITE_OK ||
              (sqlite3_step(put.s) != SQLITE_DONE) || sqlite3_reset(put.s) != SQLITE_OK ||
              sqlite3_clear_bindings(put.s) != SQLITE_OK) {
            wok = false;
            return;
          }
          ++written;
          ++pos;
        }
      });
      Conn reader;
      reader.Open(file, true, "NORMAL", kCacheKib);
      Stmt get;
      get.Prepare(reader.db, "SELECT v FROM kv WHERE k=?;");
      std::mt19937 rng(999);
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      std::vector<double> samples;
      samples.reserve(20000);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      auto rstart = Clock::now();
      while (Clock::now() - rstart < std::chrono::seconds(2)) {
        size_t idx = pick(rng);
        auto start = Clock::now();
        Require(sqlite3_bind_blob(get.s, 1, keys[idx].data(), (int)keys[idx].size(), SQLITE_STATIC), reader.db, "bind");
        int rc = sqlite3_step(get.s);
        Require(rc, reader.db, "read under write");
        if (rc != SQLITE_ROW) throw std::runtime_error("read under write missing");
        const void* pv = sqlite3_column_blob(get.s, 0);
        int nb = sqlite3_column_bytes(get.s, 0);
        std::string_view got((const char*)pv, nb);
        if (got != value && got != overwrite) throw std::runtime_error("read under write wrong value");
        get.Reset();
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
      stop = true;
      writer.join();
      if (!wok) throw std::runtime_error("writer failed");
      double wsecs = std::chrono::duration<double>(Clock::now() - wstart).count();
      ReportLatency(trial, "read_under_write", samples, written.load() / wsecs, "writer=buffered");
      conn.Open(file, true, "FULL", kCacheKib);
    }

    // Phase 5: full scan, timed from statement preparation.
    {
      auto start = Clock::now();
      Stmt scan;
      scan.Prepare(conn.db, "SELECT k,v FROM kv ORDER BY k;");
      size_t count = 0;
      for (;;) {
        int rc = sqlite3_step(scan.s);
        if (rc == SQLITE_DONE) break;
        Require(rc, conn.db, "scan");
        ++count;
      }
      double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      if (count != n) throw std::runtime_error("scan count mismatch");
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"scan\""
                << ",\"records\":" << count << ",\"total_ms\":" << ms << ",\"ns_per_key\":" << ms * 1e6 / count << "}\n";
    }

    // Phase 6: close + reopen cost with verification.
    {
      conn = Conn();
      auto start = Clock::now();
      conn.Open(file, true, "FULL", kCacheKib);
      double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      Stmt get;
      get.Prepare(conn.db, "SELECT v FROM kv WHERE k=?;");
      std::mt19937 rng(555);
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      for (int i = 0; i < 1000; ++i) {
        size_t idx = pick(rng);
        Require(sqlite3_bind_blob(get.s, 1, keys[idx].data(), (int)keys[idx].size(), SQLITE_STATIC), conn.db, "bind");
        int rc = sqlite3_step(get.s);
        Require(rc, conn.db, "post-reopen get");
        if (rc != SQLITE_ROW) throw std::runtime_error("post-reopen missing");
        const void* pv = sqlite3_column_blob(get.s, 0);
        int nb = sqlite3_column_bytes(get.s, 0);
        std::string_view got((const char*)pv, nb);
        if (got != value && got != overwrite) throw std::runtime_error("post-reopen wrong value");
        get.Reset();
      }
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"reopen\""
                << ",\"reopen_ms\":" << ms << ",\"verify_gets\":1000}\n";
    }

    // Phase 7: identically-defined write amplification.
    {
      Exec(conn.db, "PRAGMA wal_checkpoint(TRUNCATE);");
      Stmt scan;
      scan.Prepare(conn.db, "SELECT k,v FROM kv;");
      uint64_t records = 0, logical = 0;
      for (;;) {
        int rc = sqlite3_step(scan.s);
        if (rc == SQLITE_DONE) break;
        Require(rc, conn.db, "amp scan");
        ++records;
        logical += (uint64_t)sqlite3_column_bytes(scan.s, 0) + (uint64_t)sqlite3_column_bytes(scan.s, 1);
      }
      uint64_t live = DbBytes(dir);
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":" << trial << ",\"metric\":\"write_amp\""
                << ",\"live_ratio\":" << (logical == 0 ? 0.0 : double(live) / logical) << ",\"live_records\":" << records
                << ",\"live_logical_bytes\":" << logical << ",\"live_sst_bytes\":" << live
                << ",\"definition\":\"live_db_bytes/live_logical_bytes (iterator-scanned, identical for all engines)\"}\n";
    }

    fs::remove_all(dir);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rigorous_sqlite: " << error.what() << '\n';
    return 1;
  }
}
