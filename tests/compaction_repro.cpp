#include "tinylsm/db.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace tinylsm;

static void Require(Status actual, const std::string& operation) {
  if (actual != Status::Ok)
    throw std::runtime_error(operation + ": " + StatusName(actual));
}
static void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("tinylsm-compaction-repro-" + std::to_string(getpid()));
  try {
    Options options;
    options.db_path = path.string();
    // Explicit Flush() controls every transition; no reopen or WAL replay.
    options.memtable_size_bytes = 1024 * 1024;
    options.block_size_bytes = 128;
    Status status;
    auto db = DB::Open(options, status);
    Require(status, "Open");
    Check(bool(db), "Open returned no DB");
    for (int round = 0; round < 12; ++round) {
      const auto label = "round " + std::to_string(round);
      std::cerr << label << '\n';
      for (int key = 0; key < 40; ++key)
        Require(db->Put("key" + std::to_string(key), std::to_string(round)), label + " Put");
      Require(db->Flush(), label + " Flush");
      Require(db->Compact(), label + " Compact");
      for (int key = 0; key < 40; ++key) {
        std::string value;
        const auto name = "key" + std::to_string(key);
        Require(db->Get(name, value), label + " Get " + name);
        Check(value == std::to_string(round), label + " stale value for " + name + ": " + value);
      }
      auto iterator = db->NewIterator();
      Require(iterator->status(), label + " iterator");
      size_t count = 0;
      for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
        Check(iterator->Value() == std::to_string(round), label + " stale scan value");
        ++count;
      }
      Check(count == 40, label + " scan count " + std::to_string(count));
    }
    for (int key = 0; key < 40; ++key)
      Require(db->Delete("key" + std::to_string(key)), "Delete");
    Require(db->Flush(), "deletion Flush");
    Require(db->Compact(), "deletion Compact");
    for (int key = 0; key < 40; ++key) {
      std::string value;
      Check(db->Get("key" + std::to_string(key), value) == Status::NotFound, "deleted key resurrected");
    }
    auto iterator = db->NewIterator();
    Require(iterator->status(), "empty iterator");
    Check(!iterator->Valid(), "deleted keys remain in scan");
    Require(db->Flush(), "empty Flush");
    Require(db->Compact(), "empty Compact");
    db.reset();
    std::filesystem::remove_all(path);
    std::cout << "12 flush/compact cycles, scans, deletion, and empty compaction passed without reopening\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\nEvidence directory: " << path << '\n';
    return 1;
  }
}
