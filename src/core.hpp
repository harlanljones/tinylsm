#pragma once
#include "tinylsm/db.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sys/file.h>
#include <unistd.h>
#include <vector>
#include <limits>
#include <stdexcept>
namespace tinylsm::detail {
// The 8-byte field is the "Timestamp(8B)" of the section 3.2 record layout: a
// monotonic logical timestamp seeded from the wall clock and advanced per
// record, so version ordering never depends on clock resolution or on a
// backwards time step.
struct Record { std::string key, value; uint64_t stamp = 0; bool deleted = false; };
inline uint64_t NowNanos() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}
// Section 3.2 sync policies expressed through the public boolean option.
inline SyncMode WalSyncMode(const Options& options) { return options.sync_wal_on_write ? SyncMode::Fsync : SyncMode::Buffered; }
inline uint32_t CRC(std::string_view s) {
  static const auto tables = [] {
    std::array<std::array<uint32_t, 256>, 8> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int j = 0; j < 8; ++j) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
      t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; ++i) {
      for (int j = 1; j < 8; ++j) {
        t[j][i] = (t[j - 1][i] >> 8) ^ t[0][t[j - 1][i] & 0xff];
      }
    }
    return t;
  }();
  uint32_t c = ~0u;
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  size_t len = s.size();
  while (len >= 8) {
    uint32_t low, high;
    std::memcpy(&low, p, 4);
    std::memcpy(&high, p + 4, 4);
    uint32_t one = c ^ low;
    c = tables[7][one & 0xff] ^
        tables[6][(one >> 8) & 0xff] ^
        tables[5][(one >> 16) & 0xff] ^
        tables[4][(one >> 24) & 0xff] ^
        tables[3][high & 0xff] ^
        tables[2][(high >> 8) & 0xff] ^
        tables[1][(high >> 16) & 0xff] ^
        tables[0][(high >> 24) & 0xff];
    p += 8;
    len -= 8;
  }
  while (len--) {
    c = tables[0][(c ^ *p++) & 255] ^ (c >> 8);
  }
  return ~c;
}
inline void Number(std::string& s,uint64_t n,size_t bytes) { for(size_t i=0;i<bytes;++i) { s.push_back(static_cast<char>(n&255)); n>>=8; } }
inline bool Number(std::string_view& s,uint64_t& n,size_t bytes) { if(s.size()<bytes) return false; n=0; for(size_t i=0;i<bytes;++i) n|=uint64_t(static_cast<unsigned char>(s[i]))<<(8*i); s.remove_prefix(bytes); return true; }
inline std::string Encode(const Record& r) {
  std::string body; body.reserve(r.key.size()+r.value.size()+19);
  Number(body,r.stamp,8); Number(body,r.deleted?0:1,1); Number(body,r.key.size(),2); body+=r.key;
  Number(body,r.value.size(),4); body+=r.value;
  std::string out; Number(out,CRC(body),4); out+=body; return out;
}
// A short suffix is an interrupted append; a complete invalid record is corruption.
inline Status Decode(std::string_view s,std::vector<Record>& rows,size_t& consumed) {
  consumed=0; while(!s.empty()) { auto before=s; uint64_t crc,stamp,type,kl,vl;
    if(!Number(s,crc,4)||!Number(s,stamp,8)||!Number(s,type,1)||!Number(s,kl,2)||s.size()<kl) return Status::Ok;
    Record r; r.stamp=stamp; r.deleted=type==0; r.key.assign(s.substr(0,kl)); s.remove_prefix(kl);
    if(!Number(s,vl,4)||s.size()<vl) return Status::Ok;
    r.value.assign(s.substr(0,vl)); s.remove_prefix(vl);
    size_t len=before.size()-s.size();
    if(type>1||stamp==0||(r.deleted&&!r.value.empty())||CRC(before.substr(4,len-4))!=crc) return Status::Corruption;
    rows.push_back(std::move(r)); consumed+=len;
  } return Status::Ok;
}
inline bool WriteAll(int fd,std::string_view s) { while(!s.empty()) { ssize_t n=::write(fd,s.data(),s.size()); if(n<0&&errno==EINTR) continue; if(n<=0) return false; s.remove_prefix(static_cast<size_t>(n)); } return true; }
inline bool ReadFile(const std::string& path,std::string& out) {
  int fd=::open(path.c_str(),O_RDONLY); if(fd<0) return false; char buf[65536]; out.clear(); bool ok=true;
  for(;;) { ssize_t n=::read(fd,buf,sizeof buf); if(n<0&&errno==EINTR) continue; if(n<0) {ok=false;break;} if(!n) break; out.append(buf,static_cast<size_t>(n)); } ::close(fd); return ok;
}
inline bool Durable(int fd) {
#ifdef __EMSCRIPTEN__
  // MEMFS keeps data in RAM: fsync is a completed no-op, so the durability
  // contract is satisfied by the same code path as the POSIX build.
  return ::fsync(fd)==0;
#elif defined(__APPLE__)
  return ::fsync(fd)==0;
#else
  return ::fdatasync(fd)==0;
#endif
}
// A POSIX build takes an exclusive advisory lock so two engines cannot corrupt
// one directory. MEMFS is single-process by construction, so the browser build
// treats the lock as always held.
inline bool TryLockFile(int fd) {
#ifdef __EMSCRIPTEN__
  (void)fd;
  return true;
#else
  return ::flock(fd,LOCK_EX|LOCK_NB)==0;
#endif
}
class Arena {
  std::vector<std::unique_ptr<std::byte[]>> blocks_; size_t left_=0; std::byte* next_=nullptr;
 public:
  void* Allocate(size_t bytes) { bytes=(bytes+alignof(std::max_align_t)-1)&~(alignof(std::max_align_t)-1);
    if(bytes>left_) { left_=std::max<size_t>(65536,bytes); blocks_.push_back(std::make_unique<std::byte[]>(left_)); next_=blocks_.back().get(); }
    void* p=next_; next_+=bytes; left_-=bytes; return p;
  }
};
// Single serialized writer; readers traverse immutable records and acquire/release links.
class MemTable {
  struct Node { Record row; std::array<std::atomic<Node*>,16> next{}; explicit Node(Record r):row(std::move(r)){} };
  Arena arena_; Node head_{Record{}}; uint32_t random_=0x12345678; size_t bytes_=0;
  static bool Before(const Record& a,const Record& b) { return a.key<b.key||(a.key==b.key&&a.stamp>b.stamp); }
 public:
  ~MemTable() { for(Node* p=head_.next[0].load();p;) { Node* n=p->next[0].load(); p->~Node(); p=n; } }
  size_t Bytes() const { return bytes_; }
  void Put(Record row) {
    std::array<Node*,16> prev; Node* p=&head_;
    for(int h=15;h>=0;--h) { Node* n; while((n=p->next[h].load(std::memory_order_acquire))&&Before(n->row,row)) p=n; prev[h]=p; }
    random_^=random_<<13; random_^=random_>>17; random_^=random_<<5;
    unsigned height=1; uint32_t bits=random_; while(height<16&&(bits&3)==0) { ++height; bits>>=2; }
    bytes_+=sizeof(Node)+row.key.size()+row.value.size();
    auto* n=new(arena_.Allocate(sizeof(Node))) Node(std::move(row));
    for(unsigned h=0;h<height;++h) n->next[h].store(prev[h]->next[h].load());
    for(unsigned h=0;h<height;++h) prev[h]->next[h].store(n,std::memory_order_release);
  }
  bool Get(std::string_view key,Record& out) const {
    const Node* p=&head_; for(int h=15;h>=0;--h) { Node* n; while((n=p->next[h].load(std::memory_order_acquire))&&n->row.key<key) p=n; }
    const Node* n=p->next[0].load(std::memory_order_acquire); if(!n||n->row.key!=key) return false; out=n->row; return true;
  }
  std::vector<Record> Rows() const { std::vector<Record> out; for(Node* n=head_.next[0].load();n;n=n->next[0].load()) if(out.empty()||out.back().key!=n->row.key) out.push_back(n->row); return out; }
};
class Snapshot final:public DB::Iterator {
  std::vector<Record> rows_; size_t pos_=0; Status status_;
 public:
  Snapshot(std::vector<Record> rows,Status s):rows_(std::move(rows)),status_(s) { std::erase_if(rows_,[](auto& r){return r.deleted;}); }
  bool Valid() const override { return status_==Status::Ok&&pos_<rows_.size(); }
  void SeekToFirst() override {pos_=0;}
  void Seek(std::string_view k) override {pos_=static_cast<size_t>(std::lower_bound(rows_.begin(),rows_.end(),k,[](auto& r,auto key){return r.key<key;})-rows_.begin());}
  void Next() override {if(Valid()) ++pos_;}
  std::string_view Key() const override {return Valid()?rows_[pos_].key:std::string_view{};}
  std::string_view Value() const override {return Valid()?rows_[pos_].value:std::string_view{};}
  Status status() const override {return status_;}
};
} // namespace tinylsm::detail
