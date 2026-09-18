// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// WebAssembly entry points for the portfolio terminal (section 7).
// The REPL reuses the same command parser as the native CLI, so the browser
// surface and the command-line surface cannot drift apart.
#include "commands.hpp"
#include <emscripten.h>
#include <filesystem>
#include <memory>
#include <string>

namespace {
std::unique_ptr<tinylsm::DB> g_db;
std::string g_message;
constexpr const char* kDefaultPath = "/data/tinylsm";

int Open(const char* path, int memtable_bytes, int block_bytes, int sync_on_write) {
  tinylsm::Options options;
  options.db_path = (path != nullptr && *path != '\0') ? path : kDefaultPath;
  if (memtable_bytes > 0) options.memtable_size_bytes = static_cast<size_t>(memtable_bytes);
  if (block_bytes >= 64) options.block_size_bytes = static_cast<size_t>(block_bytes);
  options.sync_wal_on_write = sync_on_write != 0;
  g_db.reset();
  tinylsm::Status status;
  g_db = tinylsm::DB::Open(options, status);
  if (!g_db) {
    g_message = std::string("open failed: ") + tinylsm::StatusName(status);
    return static_cast<int>(status) + 1;
  }
  g_message.clear();
  return 0;
}
}  // namespace

extern "C" {
// Returns 0 on success; otherwise the Status ordinal + 1, message in LastError.
EMSCRIPTEN_KEEPALIVE int tinylsm_open(const char* path, int memtable_bytes, int block_bytes, int sync_on_write) {
  return Open(path, memtable_bytes, block_bytes, sync_on_write);
}
// Returns a borrowed pointer to the command's output, valid until the next call.
EMSCRIPTEN_KEEPALIVE const char* tinylsm_command(const char* line) {
  if (!g_db) { g_message = "no open database"; return g_message.c_str(); }
  try {
    g_message = tinylsm::Execute(*g_db, line != nullptr ? line : "");
  } catch (const std::exception& error) {
    g_message = std::string("exception: ") + error.what();
  }
  return g_message.c_str();
}
// Result of the most recent command or open failure.
EMSCRIPTEN_KEEPALIVE const char* tinylsm_last_error() { return g_message.c_str(); }
// Close the engine (flushing the WAL) and reopen it from MEMFS: the browser
// analogue of a process restart, proving WAL replay inside the page.
EMSCRIPTEN_KEEPALIVE int tinylsm_recover(const char* path, int memtable_bytes, int block_bytes, int sync_on_write) {
  g_db.reset();
  int code = Open(path, memtable_bytes, block_bytes, sync_on_write);
  if (code != 0) return code;
  g_message = "STATS " + tinylsm::Execute(*g_db, "STATS");
  return 0;
}
// Discard the database directory and start from an empty engine.
EMSCRIPTEN_KEEPALIVE int tinylsm_reset(const char* path, int memtable_bytes, int block_bytes, int sync_on_write) {
  g_db.reset();
  std::error_code ec;
  std::filesystem::remove_all((path != nullptr && *path != '\0') ? path : kDefaultPath, ec);
  return Open(path, memtable_bytes, block_bytes, sync_on_write);
}
EMSCRIPTEN_KEEPALIVE int tinylsm_close() {
  g_db.reset();
  g_message.clear();
  return 0;
}
EMSCRIPTEN_KEEPALIVE int tinylsm_is_open() { return g_db ? 1 : 0; }
}  // extern "C"