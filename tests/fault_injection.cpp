// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Fault injection for the recovery parser (design section 6): a simulated VFS
// that models how bytes reach the device under power loss. Every plan asserts
// the crash-consistency contract: recovery either reports Corruption on a
// damaged committed record, or it yields an exact prefix of the log — never a
// torn value, never a gap, never silently wrong data.
#include "tinylsm/db.hpp"
#include "core.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
using namespace tinylsm;
using namespace tinylsm::detail;

// --- simulated VFS ---------------------------------------------------------
// Writes are staged in RAM and reach the device in whole sectors. A power loss
// keeps only completed sectors; a torn write leaves the final sector partially
// updated. Both are modelled explicitly so each plan reads like a real failure.
class SimVfs {
 public:
  explicit SimVfs(size_t sector_bytes = 512) : sector_(sector_bytes) {}
  void Write(const std::string& bytes) { staged_ += bytes; }
  // Whole sectors reach the device; the trailing partial sector does not.
  void PowerLoss() { image_ = staged_.substr(0, staged_.size() / sector_ * sector_); }
  // Torn write: the final sector lands with only `keep` bytes of its payload.
  void TearLastSector(size_t keep) {
    const size_t completed = staged_.size() / sector_ * sector_;
    const std::string tail = staged_.substr(completed);
    image_ = staged_.substr(0, completed) + tail.substr(0, std::min(keep, tail.size()));
  }
  // Every staged byte reached the device before power was cut.
  void Complete() { image_ = staged_; }
  void DropFrom(size_t offset) { image_ = staged_.substr(0, std::min(offset, staged_.size())); }
  std::string& Image() { return image_; }
  size_t Bytes() const { return staged_.size(); }

 private:
  size_t sector_;
  std::string staged_;
  std::string image_;
};

namespace {
struct Log {
  std::vector<std::string> keys;    // fixed-width keys: lexicographic == write order
  std::vector<std::string> values;
  std::vector<size_t> ends;         // exclusive end offset of each record
  std::string bytes;
};

Log BuildLog(size_t count, uint64_t first_stamp) {
  Log log;
  for (size_t i = 0; i < count; ++i) {
    char key[8];
    std::snprintf(key, sizeof key, "k%02zu", i);
    std::string value = "value-" + std::to_string(i) + std::string(i % 17, 'p');
    log.bytes += Encode(Record{key, value, first_stamp + i, false});
    log.ends.push_back(log.bytes.size());
    log.keys.emplace_back(key);
    log.values.push_back(std::move(value));
  }
  return log;
}

// Reads every live key from a recovered database.
std::vector<std::pair<std::string, std::string>> ReadAll(DB& db) {
  std::vector<std::pair<std::string, std::string>> rows;
  auto iterator = db.NewIterator();
  if (iterator->status() != Status::Ok) throw std::runtime_error("scan status");
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next())
    rows.emplace_back(std::string(iterator->Key()), std::string(iterator->Value()));
  if (iterator->status() != Status::Ok) throw std::runtime_error("scan completion");
  return rows;
}

struct Outcome { Status status = Status::Ok; size_t recovered = 0; };

// Installs `image` as the engine's WAL and reports what recovery makes of it.
Outcome Recover(const std::filesystem::path& root, const Log& log, const std::string& image, const char* tag) {
  auto path = root / tag;
  std::filesystem::remove_all(path);
  Options options;
  options.db_path = path.string();
  Status status;
  { auto db = DB::Open(options, status); if (!db) throw std::runtime_error("seed Open failed"); }
  std::filesystem::path wal;
  for (auto& entry : std::filesystem::directory_iterator(path))
    if (entry.path().extension() == ".wal") wal = entry.path();
  if (wal.empty()) throw std::runtime_error("no WAL to corrupt");
  int fd = ::open(wal.c_str(), O_WRONLY | O_TRUNC);
  if (fd < 0) throw std::runtime_error("open WAL");
  if (!WriteAll(fd, image) || !Durable(fd)) throw std::runtime_error("write WAL image");
  ::close(fd);
  auto db = DB::Open(options, status);
  Outcome outcome;
  outcome.status = status;
  if (db) {
    auto rows = ReadAll(*db);
    outcome.recovered = rows.size();
    // Contract 1: never a torn, altered, or reordered value.
    if (rows.size() > log.keys.size())
      throw std::runtime_error(std::string(tag) + ": recovered more records than were written");
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].first != log.keys[i] || rows[i].second != log.values[i])
        throw std::runtime_error(std::string(tag) + ": recovered row " + std::to_string(i) +
                                 " is not the exact record at that position");
    }
    // Contract 2: what remains is a prefix of the log, with no gap.
  } else if (status != Status::Corruption) {
    throw std::runtime_error(std::string(tag) + ": recovery failed with " + StatusName(status) +
                             " instead of Corruption");
  }
  // Release the engine (joining its workers and closing its files) before the
  // directory is removed.
  db.reset();
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  return outcome;
}
}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("tinylsm-faults-" + std::to_string(getpid()));
  try {
    constexpr size_t kRecords = 40;
    const Log log = BuildLog(kRecords, 1000);
    size_t clean = 0, corruptions = 0;

    // 1. Exhaustive power loss: every byte boundary loses the unflushed tail.
    for (size_t cut = 0; cut <= log.bytes.size(); ++cut) {
      SimVfs vfs;
      vfs.Write(log.bytes);
      vfs.PowerLoss();          // whole sectors only
      vfs.DropFrom(cut);        // then the staged tail at the cut point
      const auto outcome = Recover(root, log, vfs.Image(), "cut");
      if (outcome.status == Status::Corruption) { ++corruptions; continue; }
      ++clean;
      const size_t expected = static_cast<size_t>(
          std::count_if(log.ends.begin(), log.ends.end(), [cut](size_t end) { return end <= cut; }));
      if (outcome.recovered != expected)
        throw std::runtime_error("truncation at " + std::to_string(cut) + ": recovered " +
                                 std::to_string(outcome.recovered) + " of " + std::to_string(expected) +
                                 " complete records");
    }

    // 2. Randomized plans: torn sectors, flipped bytes, garbage tails, abrupt cuts.
    std::mt19937 rng(20260917);
    size_t torn = 0, flipped = 0, garbage = 0, abrupt = 0;
    for (int plan = 0; plan < 600; ++plan) {
      const size_t cut = rng() % (log.bytes.size() + 1);
      const char* tag = "plan";
      SimVfs vfs;
      vfs.Write(log.bytes);
      switch (plan % 4) {
        case 0:  // torn write: the final sector lands only partially
          vfs.TearLastSector(rng() % 512);
          tag = "tear";
          break;
        case 1:  // flipped byte inside committed data (bit rot, torn sector)
          vfs.Complete();
          if (!vfs.Image().empty())
            vfs.Image()[rng() % vfs.Image().size()] ^= static_cast<char>(1u << (rng() % 8));
          tag = "flip";
          break;
        case 2: {  // committed prefix followed by uninitialised flash garbage
          vfs.DropFrom(cut);
          std::string tail(rng() % 64, '\0');
          for (char& byte : tail) byte = static_cast<char>(rng() % 256);
          vfs.Image() += tail;
          tag = "garbage";
          break;
        }
        default:  // abrupt cut in the middle of the append
          vfs.DropFrom(cut);
          tag = "abrupt";
          break;
      }
      const auto outcome = Recover(root, log, vfs.Image(), tag);
      if (outcome.status == Status::Corruption) { ++corruptions; continue; }
      ++clean;
      if (tag[0] == 't' && tag[1] == 'e') ++torn;
      else if (tag[0] == 'f') ++flipped;
      else if (tag[0] == 'g') ++garbage;
      else ++abrupt;
    }

    // 3. A fully flushed log must always replay completely.
    {
      SimVfs vfs;
      vfs.Write(log.bytes);
      vfs.Complete();
      const auto outcome = Recover(root, log, vfs.Image(), "complete");
      if (outcome.status != Status::Ok || outcome.recovered != kRecords)
        throw std::runtime_error("a fully flushed log did not replay completely");
    }

    std::filesystem::remove_all(root);
    std::cout << "fault injection passed: " << (log.bytes.size() + 1)
              << " exhaustive byte-boundary power losses and 600 randomized plans ("
              << torn << " torn sectors, " << flipped << " flipped bytes, " << garbage
              << " garbage tails, " << abrupt << " abrupt cuts) with " << clean
              << " clean recoveries of an exact log prefix and " << corruptions
              << " Corruption verdicts; no torn value, no gap, no silent loss\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\nEvidence directory: " << root << '\n';
    return 1;
  }
}