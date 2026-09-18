#pragma once
#include "core.hpp"
#include <list>
#include <unordered_map>
namespace tinylsm::detail {
inline uint32_t Murmur(std::string_view s) {
  uint32_t h=0x9747b28c; size_t i=0;
  auto mix=[](uint32_t k) {k*=0xcc9e2d51; k=(k<<15)|(k>>17); return k*0x1b873593;};
  for(;i+4<=s.size();i+=4) {uint32_t k=0;for(int j=0;j<4;++j) k|=uint32_t(static_cast<unsigned char>(s[i+j]))<<(8*j);h^=mix(k);h=(h<<13)|(h>>19);h=h*5+0xe6546b64;}
  uint32_t tail=0;for(size_t j=i;j<s.size();++j) tail|=uint32_t(static_cast<unsigned char>(s[j]))<<(8*(j-i));
  if(i<s.size()) h^=mix(tail);
  h^=static_cast<uint32_t>(s.size());h^=h>>16;h*=0x85ebca6b;h^=h>>13;h*=0xc2b2ae35;h^=h>>16;return h;
}
class Bloom {
  std::string bits_;
 public:
  explicit Bloom(size_t keys=0):bits_(std::max<size_t>(8,(keys*10+7)/8),'\0'){}
  explicit Bloom(std::string bytes):bits_(std::move(bytes)){}
  void Add(std::string_view k) {uint32_t h=Murmur(k),delta=(h>>17)|(h<<15);for(int i=0;i<7;++i,h+=delta) {auto b=h%(bits_.size()*8);bits_[b/8]=static_cast<char>(static_cast<unsigned char>(bits_[b/8])|(1u<<(b%8)));}}
  bool MayContain(std::string_view k) const {if(bits_.empty()) return false;uint32_t h=Murmur(k),delta=(h>>17)|(h<<15);for(int i=0;i<7;++i,h+=delta) {auto b=h%(bits_.size()*8);if(!(static_cast<unsigned char>(bits_[b/8])&(1u<<(b%8)))) return false;}return true;}
  const std::string& Bytes() const {return bits_;}
};
inline std::string BlockEncode(const std::vector<Record>& rows) {
  std::string out,prev; for(auto& r:rows) {size_t shared=0;while(shared<prev.size()&&shared<r.key.size()&&prev[shared]==r.key[shared]) ++shared;
    Number(out,shared,2);Number(out,r.key.size()-shared,2);Number(out,r.value.size(),4);Number(out,r.stamp,8);Number(out,r.deleted?0:1,1);
    out.append(r.key,shared);out+=r.value;prev=r.key;
  } Number(out,CRC(out),4);return out;
}
inline Status BlockDecode(std::string_view bytes,std::vector<Record>& rows) {
  if(bytes.size()<4) return Status::Corruption;
  auto tail=bytes.substr(bytes.size()-4);uint64_t crc;Number(tail,crc,4);bytes.remove_suffix(4);if(CRC(bytes)!=crc) return Status::Corruption;
  rows.reserve(64);
  std::string prev;while(!bytes.empty()) {uint64_t shared,non,value,stamp,type;
    if(!Number(bytes,shared,2)||!Number(bytes,non,2)||!Number(bytes,value,4)||!Number(bytes,stamp,8)||!Number(bytes,type,1)||shared>prev.size()||non>bytes.size()||value>bytes.size()-non||shared+non>65535||type>1||stamp==0||(type==0&&value)) return Status::Corruption;
    Record r{prev.substr(0,shared),{},stamp,type==0};r.key+=bytes.substr(0,non);bytes.remove_prefix(non);r.value=bytes.substr(0,value);bytes.remove_prefix(value);
    if(!rows.empty()&&rows.back().key>=r.key) return Status::Corruption;
    prev=r.key;rows.push_back(std::move(r));
  }return Status::Ok;
}
class Cache {
  struct Item {std::string id;std::shared_ptr<const std::vector<Record>> rows;size_t bytes;};
  size_t capacity_,used_=0;std::mutex mu_;std::list<Item> lru_;
  std::unordered_map<std::string,std::list<Item>::iterator> index_;
 public:
  std::atomic<uint64_t> hits{0},misses{0};
  explicit Cache(size_t capacity):capacity_(capacity){}
  size_t capacity() const {return capacity_;}
  std::shared_ptr<const std::vector<Record>> Get(const std::string& id) {if(capacity_<=1) return {};std::lock_guard lock(mu_);auto i=index_.find(id);if(i==index_.end()) {++misses;return {};}++hits;lru_.splice(lru_.begin(),lru_,i->second);return i->second->rows;}
  void Put(std::string id,std::shared_ptr<const std::vector<Record>> rows) {if(capacity_<=1) return;size_t bytes=sizeof(Item)+id.size();for(auto& r:*rows) bytes+=sizeof(Record)+r.key.size()+r.value.size();std::lock_guard lock(mu_);if(bytes>capacity_||index_.contains(id)) return;
    while(used_+bytes>capacity_) {used_-=lru_.back().bytes;index_.erase(lru_.back().id);lru_.pop_back();}
    used_+=bytes;lru_.push_front({std::move(id),std::move(rows),bytes});index_[lru_.front().id]=lru_.begin();
  }
};
struct BlockIndex {std::string last;uint64_t offset=0,length=0;};
class Table {
  int fd_=-1;std::vector<BlockIndex> index_;Bloom bloom_;
 public:
  uint64_t id=0,bytes=0;unsigned level=0;std::string first,last,path;
  ~Table(){if(fd_>=0) ::close(fd_);}
  static Status Build(const std::string& path,const std::vector<Record>& rows,size_t block_size);
  static Status Open(const std::string& path,uint64_t id,unsigned level,std::shared_ptr<Table>& out);
  Status ReadBlock(size_t i,Cache& cache,std::shared_ptr<const std::vector<Record>>& out) const;
  Status Get(std::string_view key,Cache& cache,Record& out) const;
  Status Rows(Cache& cache,std::vector<Record>& out) const;
  static Status Validate(std::string_view bytes);
};
inline bool ReadAt(int fd,uint64_t offset,size_t len,char* out) {size_t done=0;while(done<len) {auto n=::pread(fd,out+done,len-done,static_cast<off_t>(offset+done));if(n<0&&errno==EINTR) continue;if(n<=0) return false;done+=static_cast<size_t>(n);}return true;}
inline bool ReadAt(int fd,uint64_t offset,size_t len,std::string& out) {out.resize(len);return ReadAt(fd,offset,len,out.data());}
inline bool SyncDirectory(const std::string& dir) {
#ifdef __EMSCRIPTEN__
  (void)dir;
  return true;  // MEMFS has no separate journal and no directory fsync.
#else
  int fd=::open(dir.c_str(),O_RDONLY|O_DIRECTORY);if(fd<0) return false;bool ok=::fsync(fd)==0;::close(fd);return ok;
#endif
}
} // namespace tinylsm::detail
