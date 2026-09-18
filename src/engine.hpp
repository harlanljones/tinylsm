#pragma once
#include "table.hpp"
#include <condition_variable>
#include <atomic>
#include <deque>
#include <thread>
namespace tinylsm::detail {
// The browser build cannot block its calling thread on a condition variable:
// Emscripten proxies several syscalls from worker threads back to the main
// thread, so a blocking main-thread wait starves the flush worker. That build
// therefore runs the very same flush and compaction policies inline on the
// calling thread, while the POSIX build keeps the background workers.
#if defined(__EMSCRIPTEN__) && !defined(TINYLSM_BACKGROUND_THREADS) && !defined(TINYLSM_INLINE_MAINTENANCE)
#define TINYLSM_INLINE_MAINTENANCE 1
#endif
class Engine final:public DB {
  struct Pending {uint64_t wal;std::shared_ptr<MemTable> mem;};
  Options options_;
  mutable std::shared_mutex mu_;
  mutable std::mutex manifest_mu_;
  std::atomic<uint64_t> reads_{0},writes_{0},logical_{0};
  std::condition_variable_any changed_;
  std::shared_ptr<MemTable> active_=std::make_shared<MemTable>();
  std::deque<Pending> pending_;
  std::vector<std::shared_ptr<Table>> tables_;
  std::vector<uint64_t> wals_;
  Cache cache_;
  int wal_=-1,lock_=-1;
  uint64_t active_wal_=0,stamp_=0,manual_requested_=0,compact_done_=0;
  std::atomic<uint64_t> next_id_{1};
  bool stop_=false;
  Status error_=Status::Ok;
  Statistics stats_;
  std::thread flush_thread_,compact_thread_;
  std::vector<Record> rows_scratch_;
  std::string Path(uint64_t id,std::string_view extension) const {return options_.db_path+"/"+std::to_string(id)+std::string(extension);}
  Status Manifest(const std::vector<uint64_t>& wals,const std::vector<std::shared_ptr<Table>>& tables);
  Status Load();
  Status Rotate(); // mu_ held, WAL is synced before publication
  Status Write(std::string_view key,std::string_view value,bool deleted);
  Status FlushFront(std::unique_lock<std::shared_mutex>& lock); // mu_ held: flush the oldest immutable
  Status CompactPass(std::unique_lock<std::shared_mutex>& lock,bool manual,bool& did_work); // mu_ held
  Status MaintainInline(std::unique_lock<std::shared_mutex>& lock); // mu_ held: drain immutables, then compact
  void FlushWorker();
  void CompactWorker();
  void Select(bool manual,unsigned& level,bool& all_l0,std::vector<std::shared_ptr<Table>>& inputs) const; // requires mu_
  Status Merge(const std::vector<std::shared_ptr<Table>>& inputs,const std::vector<std::shared_ptr<Table>>& below,std::vector<Record>& rows);
  Status PublishManifest();
  void RetireWAL(uint64_t id);
  void SortTables();
 public:
  explicit Engine(Options o):options_(std::move(o)),cache_(options_.block_cache_size_bytes){}
  Status Init();
  ~Engine() override;
  Status Put(std::string_view k,std::string_view v) override {return Write(k,v,false);}
  Status Delete(std::string_view k) override {return Write(k,{},true);}
  Status Get(std::string_view key,std::string& value) override;
  Status Sync() override;
  Status Flush() override;
  Status Compact() override;
  std::unique_ptr<Iterator> NewIterator() override;
  Statistics Stats() const override;
};
} // namespace tinylsm::detail
