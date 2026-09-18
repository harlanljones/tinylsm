#include "tinylsm/db.hpp"
#include <csignal>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace tinylsm;
using Model = std::map<std::string, std::string>;

static void Check(bool ok, const std::string& why) {
  if (!ok) throw std::runtime_error(why);
}
static void Require(Status status, const std::string& operation) {
  Check(status == Status::Ok, operation + ": " + StatusName(status));
}
static std::string Key(unsigned number) {
  // Include empty and embedded-NUL keys in addition to ordinary strings.
  if (number == 0) return {};
  if (number == 1) return std::string("binary\0key", 10);
  return "key/" + std::to_string(number);
}
static void Work(unsigned seed, unsigned phase, DB* db, Model* model) {
  std::mt19937 rng(seed * 101 + phase);
  for (unsigned i = 0; i < 500; ++i) {
    auto key = Key(rng() % 150);
    bool remove = rng() % 4 == 0;
    std::string value = std::to_string(seed) + ":" + std::to_string(phase) + ":" + std::to_string(i);
    value.append(rng() % 200, static_cast<char>(rng() % 256));
    if (db) Require(remove ? db->Delete(key) : db->Put(key, value), "write");
    if (model) {
      if (remove) model->erase(key);
      else (*model)[key] = value;
    }
  }
}
static void Verify(DB& db, const Model& expected) {
  for (unsigned i = 0; i < 151; ++i) {
    auto key = Key(i);
    auto found = expected.find(key);
    std::string actual;
    auto status = db.Get(key, actual);
    if (found == expected.end()) Check(status == Status::NotFound, "deleted/missing key resurrected");
    else {
      Require(status, "Get acknowledged key");
      Check(actual == found->second, "wrong value after SIGKILL/reopen");
    }
  }
  auto iterator = db.NewIterator();
  Require(iterator->status(), "scan creation");
  iterator->SeekToFirst();
  for (const auto& [key, value] : expected) {
    Check(iterator->Valid(), "scan ended early");
    Check(iterator->Key() == key && iterator->Value() == value, "scan differs from oracle");
    iterator->Next();
  }
  Check(!iterator->Valid(), "scan contains extra keys");
  Require(iterator->status(), "scan completion");
}
static void Run(unsigned seed, unsigned crash_phase, const std::filesystem::path& root) {
  Options options;
  options.db_path = (root / (std::to_string(seed) + "-" + std::to_string(crash_phase))).string();
  options.memtable_size_bytes = 4096 + seed * 128;
  options.block_size_bytes = 128 + seed * 16;
  int ack[2], proceed[2];
  Check(pipe(ack) == 0, "ack pipe");
  Check(pipe(proceed) == 0, "command pipe");
  pid_t child = fork();
  Check(child >= 0, "fork");
  if (child == 0) {
    close(ack[0]); close(proceed[1]);
    try {
      Status status;
      auto db = DB::Open(options, status);
      Require(status, "child Open");
      Check(bool(db), "child Open returned null");
      for (unsigned phase = 0; phase < 3; ++phase) {
        Work(seed, phase, db.get(), nullptr);
        Require(db->Sync(), "Sync");
        if (phase >= 1) Require(db->Flush(), "Flush");
        if (phase >= 2) Require(db->Compact(), "Compact");
        char ready = 'A', next;
        Check(write(ack[1], &ready, 1) == 1, "acknowledge");
        // The parent kills at an acknowledged barrier, never during an unacknowledged operation.
        Check(read(proceed[0], &next, 1) == 1, "wait for parent");
      }
      _exit(0);
    } catch (const std::exception& error) {
      std::cerr << "child: " << error.what() << '\n';
      _exit(2);
    }
  }
  close(ack[1]); close(proceed[0]);
  try {
    Model expected;
    for (unsigned phase = 0; phase <= crash_phase; ++phase) {
      Work(seed, phase, nullptr, &expected);
      char ready = 0;
      Check(read(ack[0], &ready, 1) == 1 && ready == 'A', "writer failed before acknowledgement");
      if (phase < crash_phase) {
        char next = 'N';
        Check(write(proceed[1], &next, 1) == 1, "advance writer");
      }
    }
    Check(kill(child, SIGKILL) == 0, "SIGKILL");
    int result;
    Check(waitpid(child, &result, 0) == child && WIFSIGNALED(result) && WTERMSIG(result) == SIGKILL,
          "writer was not terminated by SIGKILL");
    child = -1;
    close(ack[0]); close(proceed[1]);
    Status status;
    auto db = DB::Open(options, status);
    Require(status, "reopen after SIGKILL");
    Check(bool(db), "reopen returned null");
    Verify(*db, expected);
    // Recovery must remain writable and survive another clean reopen.
    Require(db->Put("post-recovery", "ok"), "post-recovery Put");
    expected["post-recovery"] = "ok";
    Require(db->Compact(), "post-recovery Compact");
    db.reset();
    db = DB::Open(options, status);
    Require(status, "second reopen");
    Verify(*db, expected);
    db.reset();
    std::filesystem::remove_all(options.db_path);
    std::cout << "PASS seed=" << seed << " crash_after="
              << (crash_phase == 0 ? "Sync" : crash_phase == 1 ? "Flush" : "Compact") << std::endl;
  } catch (...) {
    if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
    throw;
  }
}
int main() {
  auto root = std::filesystem::temp_directory_path() / ("tinylsm-acceptance-" + std::to_string(getpid()));
  try {
    for (unsigned seed = 1; seed <= 10; ++seed)
      for (unsigned phase = 0; phase < 3; ++phase) Run(seed, phase, root);
    std::filesystem::remove_all(root);
    std::cout << "PASS: 30 randomized SIGKILL/reopen cases; all acknowledged state and scans match oracle\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << "\nEvidence: " << root << '\n';
    return 1;
  }
}
