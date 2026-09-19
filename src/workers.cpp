#include "engine.hpp"
#include <chrono>
namespace tinylsm::detail {
void Engine::Publish() {
  // mu_ held. Build a fresh immutable snapshot and publish it atomically so
  // Get/NewIterator never need mu_. shared_ptr atomic load/store gives the
  // reader a stable refcount on every memtable and SST in the snapshot.
  auto v = std::make_shared<Version>();
  v->active = active_;
  v->immutables.reserve(pending_.size());
  for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) v->immutables.push_back(it->mem);
  v->tables = tables_;
  version_.store(std::move(v), std::memory_order_release);
}
Status Engine::Write(std::string_view key,std::string_view value,bool deleted) {
  if(key.size()>65535||value.size()>std::numeric_limits<uint32_t>::max()) return Status::InvalidArgument;
  auto row=Record{std::string(key),std::string(value),0,deleted};
  // WAL append under wal_mu_ only: readers hold mu_ shared and are never
  // blocked by the write syscall. Stamps are assigned inside the same critical
  // section, so WAL order is exactly stamp order.
  {
    std::lock_guard<std::mutex> wl(wal_mu_);
    if(error_.load(std::memory_order_relaxed)!=Status::Ok) return error_;
    row.stamp=std::max(stamp_.load(std::memory_order_relaxed)+1,NowNanos());
    stamp_.store(row.stamp,std::memory_order_relaxed);
    ++inflight_;
    if(!WriteAll(wal_,Encode(row))||(WalSyncMode(options_)==SyncMode::Fsync&&!Durable(wal_))) {
      --inflight_;inflight_cv_.notify_all();
      error_.store(Status::IOError,std::memory_order_relaxed);return Status::IOError;
    }
  }
  // Memtable application still needs mu_ exclusive. Because the skiplist
  // orders same-key records stamp-descending, two writers may apply out of
  // order without any read returning a stale version.
  bool rotated=false;
  {
    std::unique_lock lock(mu_);
    active_->Put(std::move(row));++writes_;logical_+=key.size()+value.size();
    if(active_->Bytes()>=options_.memtable_size_bytes) rotated=true;
  }
  // inflight_ is released only after the record is in a memtable, so Rotate's
  // drain can never swap a memtable that is still missing an appended record.
  {std::lock_guard<std::mutex> wl(wal_mu_);--inflight_;inflight_cv_.notify_all();}
  if(rotated) {
    std::unique_lock lock(mu_);
    auto s=Rotate(lock);
    if(s!=Status::Ok) return s;
#ifdef TINYLSM_INLINE_MAINTENANCE
    return MaintainInline(lock);
#endif
  }
  return Status::Ok;
}
Status Engine::Rotate(std::unique_lock<std::shared_mutex>& lock) {
  if(active_->Bytes()==0) return Status::Ok;
  // One rotation at a time. If another writer is already rotating (it released
  // mu_ to drain inflight appends), skip: this memtable is rotated by the next
  // threshold crossing or by Flush().
  std::unique_lock<std::mutex> rot(rotate_mu_,std::try_to_lock);
  if(!rot.owns_lock()) return Status::Ok;
  // Drain: every appended record must be applied to the active memtable before
  // it becomes immutable and its WAL becomes eligible for deletion. mu_ is
  // released so applying writers are never blocked by the drain.
  lock.unlock();
  {
    std::unique_lock<std::mutex> wl(wal_mu_);
    inflight_cv_.wait(wl,[&]{return inflight_==0;});
  }
  lock.lock();
  uint64_t id=next_id_++;
  int fd=::open(Path(id,".wal").c_str(),O_CREAT|O_EXCL|O_WRONLY|O_APPEND,0644);
  if(fd<0) return error_=Status::IOError;
  auto wals=wals_;wals.push_back(id);
  if(!Durable(wal_)||!Durable(fd)||!SyncDirectory(options_.db_path)) {::close(fd);return error_=Status::IOError;}
  auto s=Manifest(wals,tables_);
  if(s!=Status::Ok) {::close(fd);return error_=s;}
  pending_.push_back({active_wal_,active_});active_=std::make_shared<MemTable>();
  // The fd swap must hold wal_mu_ as well as mu_: a concurrent writer may be
  // appending to wal_ under wal_mu_ without holding mu_.
  {std::lock_guard<std::mutex> wl(wal_mu_);::close(wal_);wal_=fd;}
  active_wal_=id;wals_=std::move(wals);
  Publish();
  changed_.notify_all();return Status::Ok;
}
void Engine::SortTables() {std::sort(tables_.begin(),tables_.end(),[](auto& a,auto& b){if(a->level!=b->level)return a->level<b->level;return a->level==0?a->id>b->id:a->first<b->first;});}
Status Engine::Merge(const std::vector<std::shared_ptr<Table>>& inputs,const std::vector<std::shared_ptr<Table>>& below,std::vector<Record>& rows) {
  rows.clear();
  std::vector<Record> all;
  for(auto& table:inputs) {auto s=table->Rows(cache_,all);if(s!=Status::Ok) return s;}
  std::stable_sort(all.begin(),all.end(),[](auto& a,auto& b){return a.key<b.key||(a.key==b.key&&a.stamp>b.stamp);});
  Record found;
  for(size_t i=0;i<all.size();++i) {
    auto& row=all[i];
    if(i&&all[i-1].key==row.key) continue; // newest version wins
    if(row.deleted) {
      // Keep the tombstone while any older copy may exist at or below the output level.
      bool keep=false;
      for(auto& table:below) if(table->Get(row.key,cache_,found)==Status::Ok) {keep=true;break;}
      if(!keep) continue;
    }
    rows.push_back(row);
  }
  return Status::Ok;
}
void Engine::Select(bool manual,unsigned& level,bool& all_l0,std::vector<std::shared_ptr<Table>>& inputs) const {
  inputs.clear();level=0;all_l0=false;
  if(manual) {inputs=tables_;all_l0=true;return;}
  std::array<uint64_t,64> sizes{};size_t l0=0;
  for(auto& t:tables_) {sizes[t->level]+=t->bytes;if(t->level==0)++l0;}
  if(l0>=4) {for(auto& t:tables_)if(t->level==0)inputs.push_back(t);all_l0=true;}
  else {
    uint64_t capacity=std::max<uint64_t>(options_.memtable_size_bytes*10,4096);
    for(unsigned l=1;l<63;++l) {
      if(sizes[l]>capacity) {
        level=l;for(auto& t:tables_)if(t->level==l){inputs.push_back(t);break;}
        break;
      }
      capacity*=10;
      if(capacity>UINT64_MAX/10) {break;}
    }
  }
  if(inputs.empty()) return;
  std::string first=inputs.front()->first,last=inputs.front()->last;
  for(auto& t:inputs){first=std::min(first,t->first);last=std::max(last,t->last);}
  for(auto& t:tables_)if(t->level==level+1&&t->first<=last&&t->last>=first)inputs.push_back(t);
}
// Flushes the oldest immutable MemTable into a Level 0 SST. Requires mu_; the
// long I/O runs with mu_ released so writers and readers are never stalled.
Status Engine::FlushFront(std::unique_lock<std::shared_mutex>& lock) {
  auto job=pending_.front();
  uint64_t id=next_id_++;
  lock.unlock();
  auto rows=job.mem->Rows();
  auto s=rows.empty()?Status::Ok:Table::Build(Path(id,".sst"),rows,options_.block_size_bytes);
  std::shared_ptr<Table> table;
  if(s==Status::Ok&&!rows.empty()) s=Table::Open(Path(id,".sst"),id,0,table);
  lock.lock();
  auto tables=tables_;if(table) tables.push_back(table);
  auto wals=wals_;std::erase(wals,job.wal);
  if(s==Status::Ok) s=Manifest(wals,tables);
  if(s!=Status::Ok) return s;
  tables_=std::move(tables);wals_=std::move(wals);SortTables();
  if(table) {++stats_.flushes;stats_.table_bytes_written+=table->bytes;}
  pending_.pop_front();
  Publish();
  std::error_code ec;std::filesystem::remove(Path(job.wal,".wal"),ec);
  changed_.notify_all();
  return Status::Ok;
}
void Engine::FlushWorker() {
  std::unique_lock lock(mu_);
  while(!stop_) {
    changed_.wait(lock,[&]{return stop_||!pending_.empty();});
    if(stop_) break;
    auto s=FlushFront(lock);
    if(s!=Status::Ok) {error_=s;changed_.notify_all();return;}
  }
}
// Runs at most one compaction pass. Requires mu_; the merge and the SST writes
// run with mu_ released. did_work reports whether files were actually merged.
Status Engine::CompactPass(std::unique_lock<std::shared_mutex>& lock,bool manual,bool& did_work) {
  did_work=false;
  if(!pending_.empty()) return Status::Ok;
  unsigned level=0;bool all_l0=false;
  std::vector<std::shared_ptr<Table>> inputs;
  Select(manual,level,all_l0,inputs);
  if(inputs.empty()) {if(manual)compact_done_=manual_requested_;changed_.notify_all();return Status::Ok;}
  auto request=manual_requested_;
  std::vector<std::shared_ptr<Table>> below;
  for(auto& t:tables_)if(std::find(inputs.begin(),inputs.end(),t)==inputs.end())below.push_back(t);
  lock.unlock();
  std::vector<Record> rows;
  auto s=Merge(inputs,below,rows);
  std::vector<std::shared_ptr<Table>> built;
  size_t start=0;
  while(s==Status::Ok&&start<rows.size()) {
    size_t end=start,bytes=0;do {bytes+=sizeof(Record)+rows[end].key.size()+rows[end].value.size();++end;}while(end<rows.size()&&bytes<std::max<size_t>(4096,options_.memtable_size_bytes));
    uint64_t id=next_id_++;std::vector<Record> group(rows.begin()+start,rows.begin()+end);
    s=Table::Build(Path(id,".sst"),group,options_.block_size_bytes);
    std::shared_ptr<Table> table;if(s==Status::Ok)s=Table::Open(Path(id,".sst"),id,level+1,table);
    if(s==Status::Ok) {built.push_back(table);}
    start=end;
  }
  lock.lock();
  auto tables=tables_;
  for(auto& input:inputs)std::erase(tables,input);
  tables.insert(tables.end(),built.begin(),built.end());
  if(s==Status::Ok)s=Manifest(wals_,tables);
  if(s!=Status::Ok) return s;
  tables_=std::move(tables);SortTables();Publish();++stats_.compactions;
  for(auto& t:built)stats_.table_bytes_written+=t->bytes;
  if(manual)compact_done_=request;
  for(auto& input:inputs){std::error_code ec;std::filesystem::remove(input->path,ec);}
  changed_.notify_all();
  did_work=true;
  return Status::Ok;
}
void Engine::CompactWorker() {
  std::unique_lock lock(mu_);
  while(!stop_) {
    changed_.wait_for(lock,std::chrono::milliseconds(200),[&]{return stop_||(manual_requested_>compact_done_&&pending_.empty());});
    if(stop_) break;
    bool manual=manual_requested_>compact_done_;
    bool did_work=false;
    auto s=CompactPass(lock,manual,did_work);
    if(s!=Status::Ok){error_=s;changed_.notify_all();return;}
  }
}
// Inline maintenance: the browser build's substitute for the worker threads.
// The policies are identical, only the scheduler differs.
Status Engine::MaintainInline(std::unique_lock<std::shared_mutex>& lock) {
  while(error_==Status::Ok&&!pending_.empty()) {auto s=FlushFront(lock);if(s!=Status::Ok)return error_=s;}
  for(;;) {bool did_work=false;auto s=CompactPass(lock,false,did_work);if(s!=Status::Ok)return error_=s;if(!did_work)break;}
  return error_;
}
Status Engine::Get(std::string_view key,std::string& value) {
  ++reads_;
  // Fast path: lock-free Version pin + skiplist/SST search. The skiplist is
  // designed for concurrent readers under a single writer; Version keeps SST
  // and immutable memtable pointers stable. On a miss, fall back to a shared
  // lock and re-search so a Get that raced Rotate still observes a Put that
  // completed before we started (read-your-writes under concurrency).
  auto lookup=[&](const std::shared_ptr<Version>& v, Record& row)->Status {
    if(v->active->Get(key,row)) return row.deleted?Status::NotFound:(value=std::move(row.value),Status::Ok);
    for(auto& mem:v->immutables) if(mem->Get(key,row)) return row.deleted?Status::NotFound:(value=std::move(row.value),Status::Ok);
    for(auto& table:v->tables) {auto s=table->Get(key,cache_,row);if(s==Status::Ok)return row.deleted?Status::NotFound:(value=std::move(row.value),Status::Ok);if(s!=Status::NotFound)return s;}
    return Status::NotFound;
  };
  Record row;
  auto v = version_.load(std::memory_order_acquire);
  auto s = lookup(v, row);
  if(s!=Status::NotFound) return s;
  std::shared_lock lock(mu_);
  v = version_.load(std::memory_order_acquire);
  return lookup(v, row);
}
Status Engine::Sync() {
  std::shared_lock lock(mu_);if(error_!=Status::Ok)return error_;
  int fd=wal_;
  if(fd<0||!Durable(fd))return Status::IOError;
  return Status::Ok;
}
Status Engine::Flush() {
  std::unique_lock lock(mu_);
  if(error_!=Status::Ok)return error_;
  Durable(wal_);
  auto s=Rotate(lock);
  if(s!=Status::Ok)return s;
#ifdef TINYLSM_INLINE_MAINTENANCE
  while(error_==Status::Ok&&!pending_.empty()) {s=FlushFront(lock);if(s!=Status::Ok)return error_=s;}
  return Status::Ok;
#else
  auto wait=changed_.wait_for(lock,std::chrono::seconds(30),[&]{return pending_.empty()||error_!=Status::Ok;});
  if(error_!=Status::Ok)return error_;
  return wait?Status::Ok:Status::IOError;
#endif
}
Status Engine::Compact() {
  auto s=Flush();if(s!=Status::Ok)return s;
  std::unique_lock lock(mu_);if(error_!=Status::Ok)return error_;
#ifdef TINYLSM_INLINE_MAINTENANCE
  bool did_work=false;s=CompactPass(lock,true,did_work);
  return s==Status::Ok?Status::Ok:(error_=s);
#else
  auto request=++manual_requested_;changed_.notify_all();
  changed_.wait(lock,[&]{return compact_done_>=request||error_!=Status::Ok;});
  return error_;
#endif
}
std::unique_ptr<DB::Iterator> Engine::NewIterator() {
  // K-way merge over sorted sources from a lock-free Version snapshot.
  // Every source is already ordered key-ascending, stamp-descending within a
  // key, so the first record seen per key is the newest version and no map or
  // re-sort is needed: O(n log k) instead of the old O(n log n) std::map.
  auto v = version_.load(std::memory_order_acquire);
  struct Source {std::vector<Record> rows;size_t pos=0;};
  std::vector<Source> sources;
  sources.push_back({v->active->Rows()});
  for(auto& mem:v->immutables) sources.push_back({mem->Rows()});
  for(auto& table:v->tables) {std::vector<Record> rows;table->Rows(cache_,rows);sources.push_back({std::move(rows)});}
  auto worse=[&](const Source& a,const Source& b) {
    const Record& ra=a.rows[a.pos];const Record& rb=b.rows[b.pos];
    if(ra.key!=rb.key) return ra.key>rb.key;
    return ra.stamp<rb.stamp;
  };
  std::vector<size_t> heap;
  for(size_t i=0;i<sources.size();++i) if(!sources[i].rows.empty()) heap.push_back(i);
  auto sift_down=[&](size_t i) {
    for(;;) {size_t l=2*i+1,r=l+1,m=i;
      if(l<heap.size()&&worse(sources[heap[m]],sources[heap[l]]))m=l;
      if(r<heap.size()&&worse(sources[heap[m]],sources[heap[r]]))m=r;
      if(m==i) break;
      std::swap(heap[i],heap[m]); i=m;
      }
  };
  for(size_t i=heap.size()/2;i-->0;) sift_down(i);
  std::vector<Record> rows;
  std::string last_key;
  bool first=true;
  while(!heap.empty()) {
    Source& top=sources[heap[0]];
    Record& row=top.rows[top.pos];
    bool newest=first||row.key!=last_key;
    if(newest) {
      last_key=row.key;first=false;
      if(!row.deleted) rows.push_back(std::move(row));
    }
    if(++top.pos<top.rows.size()) sift_down(0);
    else {heap[0]=heap.back();heap.pop_back();if(!heap.empty())sift_down(0);}
  }
  return std::make_unique<Snapshot>(std::move(rows),Status::Ok);
}
Statistics Engine::Stats() const {
  // stats_ counters are updated under mu_; take a shared lock only for that
  // copy. The Version snapshot itself is lock-free.
  Statistics s;
  {std::shared_lock lock(mu_); s=stats_;}
  auto v = version_.load(std::memory_order_acquire);
  s.reads=reads_.load();s.writes=writes_.load();s.logical_bytes=logical_.load();
  s.cache_hits=cache_.hits.load();s.cache_misses=cache_.misses.load();
  s.active_bytes=v->active->Bytes();s.immutable_count=v->immutables.size();
  s.level_files.assign(64,0);for(auto& t:v->tables) ++s.level_files[t->level];
  while(s.level_files.size()>1&&s.level_files.back()==0)s.level_files.pop_back();
  return s;
}
} // namespace tinylsm::detail
