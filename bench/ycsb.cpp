// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// YCSB-style skewed workloads against tinylsm.
//
// The rigorous benchmark (bench/rigorous.cpp) is uniform-random by design —
// adversarial to sequential prefetch, but blind to skew. Real workloads are
// skewed: a hot minority takes most reads. This driver implements the YCSB
// core-workload mix with a scrambled Zipfian key chooser (theta=0.99, the
// YCSB default) so hot-key caching, bloom-filter accuracy, and memtable hit
// rate actually matter:
//
//   A: 50% read / 50% update      (update-heavy session store)
//   B: 95% read / 5% update       (photo tags)
//   C: 100% read                  (read-only cache)
//   D: 95% read-latest / 5% insert (timeline: reads skew to newest keys)
//   E: 95% short scan / 5% insert  (threads: start key zipfian, len 1..100)
//   F: 50% read / 50% read-modify-write (read, then update, one timed op)
//
// Records are 100-byte values under `user%010zu` keys, matching the rigorous
// value size. Output is the same JSONL schema as the rigorous drivers
// (engine/trial/metric/unit/samples/mean/max/p50/p90/p99/p999 plus
// ops_per_second on the workload record), so scripts/ycsb_table.mjs can
// render it and future engines can reuse the workload verbatim.
//
// Workload E honesty note: short scans construct a lazy merge iterator per
// scan (cursor seeks + walked keys only), so E runs at full op count and its
// cost is directly comparable to a B-tree range scan.
//
// Usage: tinylsm_ycsb [records] [ops_per_workload] [db_dir]
#include "tinylsm/db.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {

constexpr const char* kEngine = "tinylsm";

void Require(tinylsm::Status s, const char* what) {
  if (s != tinylsm::Status::Ok) throw std::runtime_error(std::string(what) + ": " + tinylsm::StatusName(s));
}

// Scrambled Zipfian after YCSB's ZipfianGenerator + ScrambledZipfianGenerator:
// zeta(n,theta) with an FNV hash scramble so hot ranks spread across the key
// space instead of clustering at key zero (which would subsidize prefix
// compression and range locality).
class Zipfian {
 public:
  Zipfian(uint64_t n, double theta = 0.99, uint64_t seed = 42) : n_(n), rng_(seed) {
    // One O(n) cumulative table up front; every draw is then O(log n) binary
    // search. (Evaluating zeta per draw would be O(n) per op — minutes for
    // 20k ops — so the table is load-bearing, not cosmetic.)
    cum_.reserve((size_t)n);
    double sum = 0;
    for (uint64_t i = 1; i <= n; ++i) {
      sum += 1.0 / std::pow((double)i, theta);
      cum_.push_back(sum);
    }
    zeta_ = sum;
  }
  uint64_t Next() {
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    double uz = uni(rng_) * zeta_;
    if (uz < 1.0) return Scramble(0);
    auto it = std::upper_bound(cum_.begin(), cum_.end(), uz);
    uint64_t rank = (uint64_t)(it - cum_.begin());
    if (rank >= n_) rank = n_ - 1;
    return Scramble(rank);
  }

 private:
  uint64_t Scramble(uint64_t rank) const {
    // 64-bit FNV-1a mix, then modulo: deterministic scramble of hot ranks.
    uint64_t h = 1469598103934665603ULL ^ (rank + 0x9e3779b97f4a7c15ULL);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h % n_;
  }
  uint64_t n_;
  double zeta_;
  std::vector<double> cum_;
  std::mt19937_64 rng_;
};

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

void ReportOp(const char* workload, const char* op, std::vector<double>& samples) {
  Summary s = Summarize(samples);
  std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":0,\"metric\":\"ycsb_" << workload << "_" << op
            << "\",\"unit\":\"us\",\"samples\":" << s.n << ",\"mean\":" << s.mean << ",\"max\":" << s.max
            << ",\"p50\":" << s.p50 << ",\"p90\":" << s.p90 << ",\"p99\":" << s.p99 << ",\"p999\":" << s.p999 << "}\n";
}

void ReportWorkload(const char* workload, size_t ops, double secs) {
  std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":0,\"metric\":\"ycsb_" << workload
            << "\",\"ops\":" << ops << ",\"total_s\":" << secs << ",\"ops_per_second\":" << ops / secs << "}\n";
}

std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "user%010llu", (unsigned long long)i);
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const size_t nrec = argc > 1 ? std::stoull(argv[1]) : 20000;
    const size_t nops = argc > 2 ? std::stoull(argv[2]) : 20000;
    if (nrec == 0 || nrec > 10000000 || nops == 0 || nops > 10000000)
      throw std::runtime_error("records and ops must be 1..10000000");
    fs::path path = argc > 3 ? fs::path(argv[3]) : fs::temp_directory_path() / ("tinylsm-ycsb-" + std::to_string(getpid()));
    fs::remove_all(path);

    std::cout << "{\"engine\":\"" << kEngine
              << "\",\"trial\":0,\"metric\":\"ycsb_env\",\"records\":" << nrec << ",\"ops_per_workload\":" << nops
              << ",\"value_bytes\":100,\"distribution\":\"scrambled_zipfian_theta_0.99\"}\n";

    tinylsm::Options o;
    o.db_path = path.string();
    tinylsm::Status st;
    auto db = tinylsm::DB::Open(o, st);
    Require(st, "open");

    const std::string value(100, 'v');
    const std::string update(100, 'u');

    // Load phase: sequential inserts (YCSB load is ordered, not shuffled).
    {
      auto start = Clock::now();
      for (size_t i = 0; i < nrec; ++i) Require(db->Put(Key(i), value), "load put");
      Require(db->Compact(), "load compact");
      double secs = std::chrono::duration<double>(Clock::now() - start).count();
      std::cout << "{\"engine\":\"" << kEngine << "\",\"trial\":0,\"metric\":\"ycsb_load\""
                << ",\"ops\":" << nrec << ",\"total_s\":" << secs << ",\"ops_per_second\":" << nrec / secs << "}\n";
    }

    std::string out;
    uint64_t next_insert = nrec;  // D/E/F inserts grow past the load set

    auto do_read = [&](uint64_t id) {
      Require(db->Get(Key(id), out), "ycsb get");
      if (out != value && out != update) throw std::runtime_error("wrong ycsb value");
    };

    // A: 50% read / 50% update.
    {
      Zipfian zip(nrec);
      std::mt19937_64 coin(1001);
      std::vector<double> reads, updates;
      reads.reserve(nops);
      updates.reserve(nops);
      auto start = Clock::now();
      for (size_t i = 0; i < nops; ++i) {
        uint64_t id = zip.Next() % nrec;
        if (coin() & 1) {
          auto t0 = Clock::now();
          do_read(id);
          reads.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        } else {
          auto t0 = Clock::now();
          Require(db->Put(Key(id), update), "ycsb-a update");
          updates.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        }
      }
      double secs = std::chrono::duration<double>(Clock::now() - start).count();
      ReportOp("a", "read", reads);
      ReportOp("a", "update", updates);
      ReportWorkload("a", nops, secs);
    }

    // B: 95% read / 5% update.
    {
      Zipfian zip(nrec);
      std::mt19937_64 coin(1002);
      std::uniform_int_distribution<int> pct(0, 99);
      std::vector<double> reads, updates;
      reads.reserve(nops);
      auto start = Clock::now();
      for (size_t i = 0; i < nops; ++i) {
        uint64_t id = zip.Next() % nrec;
        if (pct(coin) < 95) {
          auto t0 = Clock::now();
          do_read(id);
          reads.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        } else {
          auto t0 = Clock::now();
          Require(db->Put(Key(id), update), "ycsb-b update");
          updates.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        }
      }
      double secs = std::chrono::duration<double>(Clock::now() - start).count();
      ReportOp("b", "read", reads);
      ReportOp("b", "update", updates);
      ReportWorkload("b", nops, secs);
    }

    // C: 100% read.
    {
      Zipfian zip(nrec);
      std::vector<double> reads;
      reads.reserve(nops);
      auto start = Clock::now();
      for (size_t i = 0; i < nops; ++i) {
        auto t0 = Clock::now();
        do_read(zip.Next() % nrec);
        reads.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
      }
      double secs = std::chrono::duration<double>(Clock::now() - start).count();
      ReportOp("c", "read", reads);
      ReportWorkload("c", nops, secs);
    }

    // D: 95% read-latest / 5% insert. Latest = highest ids: rank 0 maps to the
    // newest key, so the hot set tracks inserts as the db grows. The Zipfian
    // table is rebuilt only when the span grows (on inserts), not per op.
    {
      std::mt19937_64 coin(1004);
      std::uniform_int_distribution<int> pct(0, 99);
      std::vector<double> reads, inserts;
      reads.reserve(nops);
      uint64_t cached = 0;
      std::unique_ptr<Zipfian> zip;
      auto start = Clock::now();
      for (size_t i = 0; i < nops; ++i) {
        uint64_t span = next_insert;
        if (!zip || span != cached) {
          zip = std::make_unique<Zipfian>(span ? span : 1, 0.99, 2004);
          cached = span;
        }
        if (pct(coin) < 95) {
          uint64_t rank = zip->Next() % span;
          uint64_t id = span - 1 - rank;  // invert: hot ranks are newest keys
          auto t0 = Clock::now();
          do_read(id);
          reads.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        } else {
          auto t0 = Clock::now();
          Require(db->Put(Key(next_insert++), value), "ycsb-d insert");
          inserts.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        }
      }
      double secs = std::chrono::duration<double>(Clock::now() - start).count();
      ReportOp("d", "readlatest", reads);
      ReportOp("d", "insert", inserts);
      ReportWorkload("d", nops, secs);
    }

    // E: 95% short scan (zipfian start, uniform length 1..100) / 5% insert.
    // Each scan constructs a lazy merge iterator (cursor seeks only), so the
    // reported time prices seeks + the walked keys — the same definition a
    // B-tree range scan would face. An eager iterator would pay a full merge
    // per scan here; that design lost this workload by ~1000x and was
    // replaced (see MergeIterator).
    {
      const size_t eops = nops;
      std::mt19937_64 coin(1005);
      std::uniform_int_distribution<int> pct(0, 99);
      std::uniform_int_distribution<int> len(1, 100);
      std::vector<double> scans, inserts;
      scans.reserve(eops);
      uint64_t cached = 0;
      std::unique_ptr<Zipfian> zip;
      auto start = Clock::now();
      for (size_t i = 0; i < eops; ++i) {
        uint64_t span = next_insert;
        if (!zip || span != cached) {
          zip = std::make_unique<Zipfian>(span ? span : 1, 0.99, 2005);
          cached = span;
        }
        if (pct(coin) < 95) {
          uint64_t id = zip->Next() % span;
          int want = len(coin);
          auto t0 = Clock::now();
          auto it = db->NewIterator();
          it->Seek(Key(id));
          int got = 0;
          for (; it->Valid() && got < want; it->Next()) ++got;
          Require(it->status(), "ycsb-e scan");
          scans.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        } else {
          auto t0 = Clock::now();
          Require(db->Put(Key(next_insert++), value), "ycsb-e insert");
          inserts.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        }
      }
      double secs = std::chrono::duration<double>(Clock::now() - start).count();
      ReportOp("e", "scan", scans);
      ReportOp("e", "insert", inserts);
      ReportWorkload("e", eops, secs);
    }

    // F: 50% read / 50% read-modify-write (one timed op each).
    {
      uint64_t span = next_insert;
      Zipfian zip(span ? span : 1);
      std::mt19937_64 coin(1006);
      std::vector<double> reads, rmws;
      reads.reserve(nops);
      rmws.reserve(nops);
      auto start = Clock::now();
      for (size_t i = 0; i < nops; ++i) {
        uint64_t id = zip.Next() % span;
        if (coin() & 1) {
          auto t0 = Clock::now();
          do_read(id);
          reads.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        } else {
          auto t0 = Clock::now();
          do_read(id);
          Require(db->Put(Key(id), update), "ycsb-f update");
          rmws.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        }
      }
      double secs = std::chrono::duration<double>(Clock::now() - start).count();
      ReportOp("f", "read", reads);
      ReportOp("f", "readmodifywrite", rmws);
      ReportWorkload("f", nops, secs);
    }

    db.reset();
    fs::remove_all(path);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ycsb: " << e.what() << '\n';
    return 1;
  }
}
