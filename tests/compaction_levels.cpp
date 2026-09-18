// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Leveled compaction behaviour (design section 3.5). The L0 trigger is covered
// everywhere else; this test drives the size-ratio path that moves data from
// L1 into L2 and beyond (level N+1 capacity = 10x level N), and verifies that
// data stays correct across the resulting cascades.
#include "tinylsm/db.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
using namespace tinylsm;

static void Check(bool ok, const std::string& why) {
  if (!ok) throw std::runtime_error(why);
}
static void Require(Status status, const std::string& operation) {
  Check(status == Status::Ok, operation + ": " + StatusName(status));
}
static std::string Key(unsigned i) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "key/%05u", i);
  return buffer;
}

int main() {
  const auto path = std::filesystem::temp_directory_path() / ("tinylsm-levels-" + std::to_string(getpid()));
  try {
    std::filesystem::remove_all(path);
    Options options;
    options.db_path = path.string();
    options.memtable_size_bytes = 4096;   // small: many flushes, small level budgets
    options.block_size_bytes = 256;
    Status status;
    auto db = DB::Open(options, status);
    Require(status, "Open");
    Check(bool(db), "Open returned no DB");

    std::map<std::string, std::string> expected;
    // Enough data to exceed the L1 budget (10x the 4 KiB memtable) and cascade
    // into L2, while staying small enough to settle quickly: the worker merges
    // one file per wake-up, so the workload is sized to that cadence.
    constexpr unsigned kKeys = 2000;
    for (unsigned i = 0; i < kKeys; ++i) {
      const auto key = Key(i);
      if (i % 8 == 7) {
        Require(db->Delete(key), "Delete");
        expected.erase(key);
      } else {
        std::string value = "v" + std::to_string(i) + std::string(32, 'q');
        Require(db->Put(key, value), "Put");
        expected[key] = value;
      }
      // Keep L0 from being the only trigger: flush as soon as one memtable fills.
      if (i % 512 == 511) Require(db->Flush(), "Flush");
    }
    Require(db->Compact(), "Compact");

    // Wait for the size-ratio policy to push data past L1 into L2.
    bool deep = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    Statistics stats{};
    while (std::chrono::steady_clock::now() < deadline) {
      stats = db->Stats();
      if (stats.level_files.size() >= 3 && stats.level_files[2] > 0) { deep = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Check(deep, "size-ratio compaction never reached L2 (levels " +
                    [&] { std::string s; for (auto n : stats.level_files) s += std::to_string(n) + " "; return s; }() + ")");

    // Wait for the cascade to settle, then confirm the deeper level still holds
    // data (a single pass would have drained back into L1).
    uint64_t last = stats.compactions;
    auto quiet_since = std::chrono::steady_clock::now();
    const auto quiet_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < quiet_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      stats = db->Stats();
      if (stats.compactions != last) {
        last = stats.compactions;
        quiet_since = std::chrono::steady_clock::now();
      } else if (std::chrono::steady_clock::now() - quiet_since > std::chrono::seconds(1)) {
        break;
      }
    }
    Check(stats.compactions >= 3, "expected several compaction passes, saw " + std::to_string(stats.compactions));
    const auto deepest = std::find_if(stats.level_files.rbegin(), stats.level_files.rend(),
                                      [](size_t count) { return count > 0; });
    Check(deepest != stats.level_files.rend() &&
              static_cast<size_t>(std::distance(deepest, stats.level_files.rend())) >= 3,
          "compaction settled without any data below L1");

    // Every key must still resolve correctly after multi-level merges.
    for (unsigned i = 0; i < kKeys; i += 137) {
      const auto key = Key(i);
      std::string value;
      const auto found = expected.find(key);
      if (found == expected.end()) {
        Check(db->Get(key, value) == Status::NotFound, "deleted key surfaced: " + key);
      } else {
        Require(db->Get(key, value), "Get " + key);
        Check(value == found->second, "stale value for " + key);
      }
    }
    // The scan must agree with the model exactly, in order, with no tombstones.
    const std::vector<std::pair<std::string, std::string>> ordered(expected.begin(), expected.end());
    auto iterator = db->NewIterator();
    Require(iterator->status(), "iterator");
    size_t seen = 0;
    for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
      Check(seen < ordered.size() && ordered[seen].first == iterator->Key() &&
            ordered[seen].second == iterator->Value(),
            "scan diverged at row " + std::to_string(seen));
      ++seen;
    }
    Require(iterator->status(), "scan completion");
    Check(seen == expected.size(), "scan returned " + std::to_string(seen) + " of " + std::to_string(expected.size()));

    db.reset();
    std::filesystem::remove_all(path);
    std::cout << "leveled compaction passed: " << stats.compactions << " passes, levels";
    for (auto count : stats.level_files) std::cout << ' ' << count;
    std::cout << ", " << expected.size() << " live keys verified\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\nEvidence directory: " << path << '\n';
    return 1;
  }
}