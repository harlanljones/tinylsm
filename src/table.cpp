#include "table.hpp"
namespace tinylsm::detail {
namespace {
constexpr uint64_t Magic=0x54494E594C534D00ULL;
struct Footer {uint64_t io,is,fo,fs,count,magic;};
bool FooterRead(std::string_view s,Footer& f) {return Number(s,f.io,8)&&Number(s,f.is,8)&&Number(s,f.fo,8)&&Number(s,f.fs,8)&&Number(s,f.count,8)&&Number(s,f.magic,8)&&f.magic==Magic;}
Status Metadata(std::string_view data,uint64_t file_size,Footer& f) {
  if(file_size<48||!FooterRead(data.substr(data.size()-48),f)||f.fo>file_size-48||f.fs<12||f.fs>file_size-48-f.fo||f.io!=f.fo+f.fs||f.is>file_size-48-f.io||f.io+f.is!=file_size-48||f.is<4) return Status::Corruption;
  return Status::Ok;
}
Status IndexRead(std::string_view s,uint64_t data_end,std::vector<BlockIndex>& index) {
  if(s.size()<4) return Status::Corruption;
  auto tail=s.substr(s.size()-4);uint64_t crc;Number(tail,crc,4);s.remove_suffix(4);if(CRC(s)!=crc) return Status::Corruption;
  uint64_t expected=0;while(!s.empty()) {uint64_t len,off,bytes;if(!Number(s,len,2)||s.size()<len) return Status::Corruption;std::string key(s.substr(0,len));s.remove_prefix(len);
    if(!Number(s,off,8)||!Number(s,bytes,8)||off!=expected||off>data_end||bytes<4||bytes>data_end-off||(!index.empty()&&index.back().last>=key)) return Status::Corruption;
    index.push_back({key,off,bytes});expected+=bytes;
  }return !index.empty()&&expected==data_end?Status::Ok:Status::Corruption;
}
}
Status Table::Build(const std::string& path,const std::vector<Record>& rows,size_t block_size) {
  if(rows.empty()) return Status::InvalidArgument;
  int fd=::open(path.c_str(),O_CREAT|O_EXCL|O_WRONLY,0644);if(fd<0) return Status::IOError;
  bool ok=true;uint64_t offset=0;std::string index;Bloom bloom(rows.size());std::vector<Record> block;size_t estimate=0;
  auto flush=[&] {if(block.empty()) return;auto encoded=BlockEncode(block);ok=ok&&WriteAll(fd,encoded);Number(index,block.back().key.size(),2);index+=block.back().key;Number(index,offset,8);Number(index,encoded.size(),8);offset+=encoded.size();block.clear();estimate=0;};
  for(auto& r:rows) {if(!block.empty()&&estimate+r.key.size()+r.value.size()+17>block_size) flush();block.push_back(r);estimate+=r.key.size()+r.value.size()+17;bloom.Add(r.key);}flush();
  uint64_t filter_offset=offset;std::string filter=bloom.Bytes();Number(filter,CRC(filter),4);ok=ok&&WriteAll(fd,filter);offset+=filter.size();
  Number(index,CRC(index),4);ok=ok&&WriteAll(fd,index);
  std::string footer;Number(footer,offset,8);Number(footer,index.size(),8);Number(footer,filter_offset,8);Number(footer,filter.size(),8);Number(footer,rows.size(),8);Number(footer,Magic,8);
  ok=ok&&WriteAll(fd,footer)&&Durable(fd);if(::close(fd)!=0) ok=false;
  return ok?Status::Ok:Status::IOError;
}
Status Table::Open(const std::string& path,uint64_t id,unsigned level,std::shared_ptr<Table>& out) {
  auto t=std::make_shared<Table>();t->path=path;t->id=id;t->level=level;t->fd_=::open(path.c_str(),O_RDONLY);if(t->fd_<0) return Status::IOError;
#if defined(__linux__)
  ::posix_fadvise(t->fd_,0,0,POSIX_FADV_RANDOM);
#endif
  auto size=::lseek(t->fd_,0,SEEK_END);if(size<48) return Status::Corruption;t->bytes=static_cast<uint64_t>(size);
  std::string footer; if(!ReadAt(t->fd_,t->bytes-48,48,footer)) return Status::IOError;
  Footer f;auto s=Metadata(footer,t->bytes,f);if(s!=Status::Ok) return s;
  std::string index,filter;if(!ReadAt(t->fd_,f.io,f.is,index)||!ReadAt(t->fd_,f.fo,f.fs,filter)) return Status::IOError;
  s=IndexRead(index,f.fo,t->index_);if(s!=Status::Ok) return s;
  std::string_view tail(filter.data()+filter.size()-4,4);uint64_t crc;Number(tail,crc,4);filter.resize(filter.size()-4);if(CRC(filter)!=crc) return Status::Corruption;t->bloom_=Bloom(std::move(filter));
  Cache none(0);std::shared_ptr<const std::vector<Record>> first;s=t->ReadBlock(0,none,first);if(s!=Status::Ok||first->empty()) return Status::Corruption;
  t->first=first->front().key;t->last=t->index_.back().last;out=std::move(t);return Status::Ok;
}
Status Table::ReadBlock(size_t i,Cache& cache,std::shared_ptr<const std::vector<Record>>& out) const {
  std::string key;
  if(cache.capacity()>1) {
    key=path+":"+std::to_string(index_[i].offset);out=cache.Get(key);if(out) return Status::Ok;
  }
  std::string bytes;if(!ReadAt(fd_,index_[i].offset,index_[i].length,bytes)) return Status::IOError;
  auto rows=std::make_shared<std::vector<Record>>();auto s=BlockDecode(bytes,*rows);if(s!=Status::Ok||rows->empty()||rows->back().key!=index_[i].last||(i&&rows->front().key<=index_[i-1].last)) return Status::Corruption;
  out=rows;if(cache.capacity()>1) cache.Put(std::move(key),rows);return Status::Ok;
}
Status Table::Get(std::string_view key,Cache& cache,Record& out) const {
  if(key<first||key>last||!bloom_.MayContain(key)) return Status::NotFound;
  auto i=std::lower_bound(index_.begin(),index_.end(),key,[](auto& b,auto k){return b.last<k;});if(i==index_.end()) return Status::NotFound;
  if(cache.capacity()>1) {
    std::shared_ptr<const std::vector<Record>> rows;auto s=ReadBlock(i-index_.begin(),cache,rows);if(s!=Status::Ok) return s;
    auto r=std::lower_bound(rows->begin(),rows->end(),key,[](auto& r,auto k){return r.key<k;});if(r==rows->end()||r->key!=key) return Status::NotFound;out=*r;return Status::Ok;
  }
  char buf[65536];
  if(i->length>sizeof(buf)||!ReadAt(fd_,i->offset,i->length,buf)) return Status::IOError;
  std::string_view s(buf,i->length);if(s.size()<4) return Status::Corruption;
  auto tail=s.substr(s.size()-4);uint64_t crc;Number(tail,crc,4);s.remove_suffix(4);if(CRC(s)!=crc) return Status::Corruption;
  std::string curr_key;curr_key.reserve(64);
  while(!s.empty()) {
    uint64_t shared,non,vl,stamp,type;
    if(!Number(s,shared,2)||!Number(s,non,2)||!Number(s,vl,4)||!Number(s,stamp,8)||!Number(s,type,1)||shared>curr_key.size()||non>s.size()||vl>s.size()-non||shared+non>65535||type>1||stamp==0||(type==0&&vl)) return Status::Corruption;
    curr_key.resize(shared);curr_key.append(s.substr(0,non));s.remove_prefix(non);
    std::string_view val=s.substr(0,vl);s.remove_prefix(vl);
    if(curr_key==key) {out.key=std::move(curr_key);out.value=std::string(val);out.stamp=stamp;out.deleted=(type==0);return Status::Ok;}
    if(curr_key>key) return Status::NotFound;
  }
  return Status::NotFound;
}
Status Table::Rows(Cache& cache,std::vector<Record>& out) const {for(size_t i=0;i<index_.size();++i) {std::shared_ptr<const std::vector<Record>> rows;auto s=ReadBlock(i,cache,rows);if(s!=Status::Ok) return s;out.insert(out.end(),rows->begin(),rows->end());}return Status::Ok;}
Status Table::Validate(std::string_view bytes) {
  if(bytes.size()<48)return Status::Corruption;
  Footer f;std::vector<BlockIndex> index;auto s=Metadata(bytes,bytes.size(),f);if(s!=Status::Ok)return s;
  s=IndexRead(bytes.substr(f.io,f.is),f.fo,index);if(s!=Status::Ok)return s;
  auto filter=bytes.substr(f.fo,f.fs);auto tail=filter.substr(filter.size()-4);uint64_t crc;Number(tail,crc,4);filter.remove_suffix(4);if(CRC(filter)!=crc)return Status::Corruption;
  Bloom bloom{std::string(filter)};uint64_t count=0;std::string previous;
  for(auto& b:index){std::vector<Record> rows;s=BlockDecode(bytes.substr(b.offset,b.length),rows);if(s!=Status::Ok||rows.empty()||rows.back().key!=b.last||(count&&rows.front().key<=previous))return Status::Corruption;
    for(auto& row:rows)if(!bloom.MayContain(row.key))return Status::Corruption;
    count+=rows.size();previous=rows.back().key;
  }return count==f.count?Status::Ok:Status::Corruption;
}
} // namespace tinylsm::detail
