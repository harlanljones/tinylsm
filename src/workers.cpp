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
  // section, so WAL order is exactly stamp order. The append is positional
  // (WriteAt): the segment was pre-sized at creation, so no put extends
  // i_size and every fsync is overwrite-class.
  {
    std::lock_guard<std::mutex> wl(wal_mu_);
    if(error_.load(std::memory_order_relaxed)!=Status::Ok) return error_;
    row.stamp=std::max(stamp_.load(std::memory_order_relaxed)+1,NowNanos());
    stamp_.store(row.stamp,std::memory_order_relaxed);
    ++inflight_;
    if(AppendWal(Encode(row))!=Status::Ok) {
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
uint64_t Engine::WalCapacity() const {
  // WAL bytes/record (19 + key + value) are always fewer than memtable
  // bytes/record (~208 + key + value), so a segment sized at the memtable
  // budget plus slack can never overflow before rotation. The slack covers
  // the in-flight record that crosses the threshold.
  return options_.memtable_size_bytes + 65536;
}
int Engine::CreateWal(uint64_t id) {
  uint64_t capacity=WalCapacity();
  int fd=::open(Path(id,".wal").c_str(),O_CREAT|O_EXCL|O_WRONLY,0644);
  if(fd<0) return -1;
  if(::ftruncate(fd,static_cast<off_t>(capacity))!=0) capacity=std::numeric_limits<uint64_t>::max();
  wal_capacity_=capacity;
  wal_offset_=0;
  return fd;
}
Status Engine::AppendWal(const std::string& encoded) {
  // wal_mu_ held by the caller.
  if(!WriteAt(wal_,wal_offset_,encoded,wal_capacity_)) {
    // Safety valve for the impossible: grow the segment once rather than
    // lose the write (correct, just i_size-extending like the old design).
    uint64_t grown=wal_offset_+encoded.size()+65536;
    if(::ftruncate(wal_,static_cast<off_t>(grown))!=0) return Status::IOError;
    wal_capacity_=grown;
    if(!WriteAt(wal_,wal_offset_,encoded,wal_capacity_)) return Status::IOError;
  }
  wal_offset_+=encoded.size();
  if(WalSyncMode(options_)==SyncMode::Fsync&&!Durable(wal_)) return Status::IOError;
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
  uint64_t capacity=WalCapacity();
  // File creation + durability I/O run WITHOUT mu_: the new file is not yet
  // visible to any other thread, and fdatasync on the old WAL races safely
  // with concurrent positional appends (each append carries its own offset
  // under wal_mu_). Previously this I/O held exclusive mu_, stalling every
  // writer for milliseconds per rotation on rotational media — the durable
  // p99 tail. Only the Manifest publish below needs the lock.
  lock.unlock();
  int fd=::open(Path(id,".wal").c_str(),O_CREAT|O_EXCL|O_WRONLY,0644);
  if(fd>=0&&::ftruncate(fd,static_cast<off_t>(capacity))!=0) capacity=std::numeric_limits<uint64_t>::max();
  bool files_ok=fd>=0&&Durable(wal_)&&Durable(fd)&&SyncDirectory(options_.db_path);
  lock.lock();
  if(!files_ok) {if(fd>=0)::close(fd);return error_=Status::IOError;}
  auto wals=wals_;wals.push_back(id);
  auto s=Manifest(wals,tables_);
  if(s!=Status::Ok) {::close(fd);return error_=s;}
  pending_.push_back({active_wal_,active_});active_=std::make_shared<MemTable>();
  // The fd swap must hold wal_mu_ as well as mu_: a concurrent writer may be
  // appending to wal_ under wal_mu_ without holding mu_. wal_offset_ resets
  // here — under wal_mu_ — so an append can never read the reset offset
  // while still writing to the old file.
  {std::lock_guard<std::mutex> wl(wal_mu_);::close(wal_);wal_=fd;wal_offset_=0;wal_capacity_=capacity;}
  active_wal_=id;wals_=std::move(wals);
  Publish();
  changed_.notify_all();return Status::Ok;
}
void Engine::SortTables() {std::sort(tables_.begin(),tables_.end(),[](auto& a,auto& b){if(a->level!=b->level)return a->level<b->level;return a->level==0?a->id>b->id:a->first<b->first;});}
Status Engine::Merge(const std::vector<std::shared_ptr<Table>>& inputs,const std::vector<std::shared_ptr<Table>>& below,std::vector<Record>& rows) {
  rows.clear();
  std::vector<Record> all;
  for(auto& table:inputs) {auto s=table->Rows(no_cache_,all);if(s!=Status::Ok) return s;}
  std::stable_sort(all.begin(),all.end(),[](auto& a,auto& b){return a.key<b.key||(a.key==b.key&&a.stamp>b.stamp);});
  Record found;
  for(size_t i=0;i<all.size();++i) {
    auto& row=all[i];
    if(i&&all[i-1].key==row.key) continue; // newest version wins
    if(row.deleted) {
      // Keep the tombstone while any older copy may exist at or below the output level.
      bool keep=false;
      for(auto& table:below) if(table->Get(row.key,no_cache_,found)==Status::Ok) {keep=true;break;}
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
// Select's trigger conditions without building the input lists: cheap enough
// to evaluate on every CompactWorker wakeup so a flush completing mid-storm
// starts a compaction pass immediately instead of at the next 200ms tick.
bool Engine::AutoWorkAvailable() const {
  std::array<uint64_t,64> sizes{};size_t l0=0;
  for(auto& t:tables_) {sizes[t->level]+=t->bytes;if(t->level==0)++l0;}
  if(l0>=4) return true;
  uint64_t capacity=std::max<uint64_t>(options_.memtable_size_bytes*10,4096);
  for(unsigned l=1;l<63;++l) {
    if(sizes[l]>capacity) return true;
    capacity*=10;
    if(capacity>UINT64_MAX/10) break;
  }
  return false;
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
//
// There is deliberately NO "wait until flushes drain" guard here: inputs are
// snapshotted under mu_ and both FlushFront and CompactPass re-read tables_
// under mu_ at publish time, applying set-deltas with disjoint fresh file
// ids, so a compaction merging L0->L1 composes safely with a concurrent
// flush adding a new L0. Gating compaction on an empty pending_ queue starved
// it exactly when it was needed most — sustained ingest kept pending_
// non-empty, L0 piled up unbounded (55+ files in a 2s storm), and every read,
// scan, and reopen paid the overlap tax. Compaction now runs alongside
// flushes; a file created mid-merge is simply picked up by the next pass.
Status Engine::CompactPass(std::unique_lock<std::shared_mutex>& lock,bool manual,bool& did_work) {
  did_work=false;
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
    // Wake promptly on flush completion (FlushFront notifies): with the
    // pending_ gate gone, automatic L0 work starts at once instead of at the
    // next 200ms tick, which is what keeps L0 bounded during a write storm.
    changed_.wait_for(lock,std::chrono::milliseconds(200),[&]{return stop_||manual_requested_>compact_done_||AutoWorkAvailable();});
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
    for(auto& table:v->tables) {auto s=table->Get(key,*cache_,row);if(s==Status::Ok)return row.deleted?Status::NotFound:(value=std::move(row.value),Status::Ok);if(s!=Status::NotFound)return s;}
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
// Lazy k-way merge over a Version snapshot: the eager NewIterator materialized
// every source into vectors up front (O(n) construction, 2-3x string copies
// per record), so a 50-key YCSB-E scan paid a full-database merge (~4ms) and
// full-scan construction dominated total time. This iterator positions one
// cursor per source (skiplist seek / SST block-index seek + one block decode)
// and merges heap-ordered, so construction is O(k log k) for k sources and
// each Next() is O(log k). A short scan touches only the blocks it walks.
//
// Correctness: stamps are globally strictly increasing (Write assigns
// max(stamp+1, NowNanos())), and each source holds at most one version per
// key (memtable Rows() and SST build/merge dedupe to newest), so the heap's
// (key asc, stamp desc) order presents each key's newest version first. The
// first sighting decides visibility: tombstone → the whole key group is
// skipped; otherwise older versions are skipped as shadowed.
struct SourceCursor {
  virtual ~SourceCursor()=default;
  virtual void SeekToFirst()=0;
  virtual void Seek(std::string_view key)=0;
  virtual void Next()=0;
  virtual bool Valid() const=0;
  virtual const Record& Row() const=0;
  virtual Status status() const=0;
};
struct MemSource final:SourceCursor {
  MemTable::Cursor cursor;
  explicit MemSource(MemTable::Cursor c):cursor(std::move(c)) {}
  void SeekToFirst() override {cursor.SeekToFirst();}
  void Seek(std::string_view key) override {cursor.Seek(key);}
  void Next() override {cursor.Next();}
  bool Valid() const override {return cursor.Valid();}
  const Record& Row() const override {return cursor.Row();}
  Status status() const override {return cursor.status();}
};
struct SstSource final:SourceCursor {
  Table::Cursor cursor;
  explicit SstSource(Table::Cursor c):cursor(std::move(c)) {}
  void SeekToFirst() override {cursor.SeekToFirst();}
  void Seek(std::string_view key) override {cursor.Seek(key);}
  void Next() override {cursor.Next();}
  bool Valid() const override {return cursor.Valid();}
  const Record& Row() const override {return cursor.Row();}
  Status status() const override {return cursor.status();}
};
class MergeIterator final:public DB::Iterator {
  // Pinned snapshot pieces: the shared_ptrs keep every memtable and SST alive
  // for the iterator's lifetime (the Version itself need not be retained).
  std::vector<std::shared_ptr<MemTable>> mems_;
  std::vector<std::shared_ptr<Table>> tables_;
  std::shared_ptr<Cache> cache_;             // decoded blocks stay alive
  std::vector<std::unique_ptr<SourceCursor>> sources_;
  std::vector<size_t> heap_;
  Status status_=Status::Ok;
  bool Worse(size_t a,size_t b) const {
    const Record& ra=sources_[a]->Row();
    const Record& rb=sources_[b]->Row();
    if(ra.key!=rb.key) return ra.key>rb.key;
    return ra.stamp<rb.stamp;
  }
  void SiftDown(size_t i) {
    for(;;) {
      size_t l=2*i+1,r=l+1,m=i;
      if(l<heap_.size()&&Worse(heap_[m],heap_[l]))m=l;
      if(r<heap_.size()&&Worse(heap_[m],heap_[r]))m=r;
      if(m==i) break;
      std::swap(heap_[i],heap_[m]);i=m;
    }
  }
  void NoteStatus(Status s) {if(status_==Status::Ok&&s!=Status::Ok)status_=s;}
  void PopTop() {
    heap_[0]=heap_.back();heap_.pop_back();
    if(!heap_.empty())SiftDown(0);
  }
  // Advance past every version of the top key in every source sharing it.
  void SkipKey() {
    std::string key=sources_[heap_[0]]->Row().key;
    do {
      SourceCursor* top=sources_[heap_[0]].get();
      top->Next();
      NoteStatus(top->status());
      if(!top->Valid())PopTop();
      else SiftDown(0);
    } while(!heap_.empty()&&sources_[heap_[0]]->Row().key==key);
  }
  void Normalize() {
    while(status_==Status::Ok&&!heap_.empty()&&sources_[heap_[0]]->Row().deleted)SkipKey();
  }
  void Rebuild() {
    heap_.clear();
    for(size_t i=0;i<sources_.size();++i) {
      NoteStatus(sources_[i]->status());
      if(sources_[i]->Valid())heap_.push_back(i);
    }
    for(size_t i=heap_.size()/2;i-->0;)SiftDown(i);
    Normalize();
  }
 public:
  MergeIterator(std::vector<std::shared_ptr<MemTable>> mems,std::vector<std::shared_ptr<Table>> tables,
                std::shared_ptr<Cache> c)
    :mems_(std::move(mems)),tables_(std::move(tables)),cache_(std::move(c)) {
    sources_.reserve(mems_.size()+tables_.size());
    for(auto& mem:mems_) sources_.push_back(std::make_unique<MemSource>(mem->NewCursor()));
    for(auto& table:tables_) sources_.push_back(std::make_unique<SstSource>(table->NewCursor(cache_)));
  }
  bool Valid() const override {return status_==Status::Ok&&!heap_.empty();}
  void SeekToFirst() override {status_=Status::Ok;for(auto& s:sources_)s->SeekToFirst();Rebuild();}
  void Seek(std::string_view key) override {status_=Status::Ok;for(auto& s:sources_)s->Seek(key);Rebuild();}
  void Next() override {if(!Valid())return;SkipKey();Normalize();}
  std::string_view Key() const override {return Valid()?std::string_view(sources_[heap_[0]]->Row().key):std::string_view{};}
  std::string_view Value() const override {return Valid()?std::string_view(sources_[heap_[0]]->Row().value):std::string_view{};}
  Status status() const override {return status_;}
};
std::unique_ptr<DB::Iterator> Engine::NewIterator() {
  // One cursor per snapshot source, heap-merged lazily: construction seeks
  // (skiplist walk, block-index binary search + one block decode each) and
  // the walk decodes only blocks it visits — O(k log k) setup, O(log k) per
  // Next(), instead of the old O(n) eager materialization.
  auto v = version_.load(std::memory_order_acquire);
  std::vector<std::shared_ptr<MemTable>> mems;
  mems.reserve(1+v->immutables.size());
  mems.push_back(v->active);
  for(auto& mem:v->immutables) mems.push_back(mem);
  auto it = std::make_unique<MergeIterator>(std::move(mems),v->tables,cache_);
  it->SeekToFirst();
  return it;
}
Statistics Engine::Stats() const {
  // stats_ counters are updated under mu_; take a shared lock only for that
  // copy. The Version snapshot itself is lock-free.
  Statistics s;
  {std::shared_lock lock(mu_); s=stats_;}
  auto v = version_.load(std::memory_order_acquire);
  s.reads=reads_.load();s.writes=writes_.load();s.logical_bytes=logical_.load();
  s.cache_hits=cache_->hits.load();s.cache_misses=cache_->misses.load();
  s.active_bytes=v->active->Bytes();s.immutable_count=v->immutables.size();
  s.level_files.assign(64,0);for(auto& t:v->tables) ++s.level_files[t->level];
  while(s.level_files.size()>1&&s.level_files.back()==0)s.level_files.pop_back();
  return s;
}
} // namespace tinylsm::detail
