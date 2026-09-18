#include "engine.hpp"
namespace tinylsm::detail {
Status Engine::Manifest(const std::vector<uint64_t>& wals,const std::vector<std::shared_ptr<Table>>& tables) {
  std::string body="TLSMMAN1";Number(body,stamp_,8);Number(body,wals.size(),4);Number(body,tables.size(),4);
  for(auto id:wals) Number(body,id,8);
  for(auto& t:tables) {Number(body,t->id,8);Number(body,t->level,4);}
  Number(body,CRC(body),4);
  auto temp=options_.db_path+"/MANIFEST.tmp";int fd=::open(temp.c_str(),O_CREAT|O_TRUNC|O_WRONLY,0644);if(fd<0) return Status::IOError;
  bool ok=WriteAll(fd,body)&&Durable(fd);if(::close(fd)!=0) ok=false;
  if(!ok||::rename(temp.c_str(),(options_.db_path+"/MANIFEST").c_str())!=0||!SyncDirectory(options_.db_path)) return Status::IOError;
  return Status::Ok;
}
Status Engine::Load() {
  std::error_code ec;
  for(auto& entry:std::filesystem::directory_iterator(options_.db_path)) {
    auto name=entry.path().stem().string();if(entry.path().extension()!=".wal"&&entry.path().extension()!=".sst") continue;
    try {size_t end;auto id=std::stoull(name,&end);if(end==name.size()) {auto current=next_id_.load();if(id+1>current) next_id_.store(id+1);}}catch(const std::exception&) {}
  }
  auto path=options_.db_path+"/MANIFEST";
  if(std::filesystem::exists(path,ec)) {
    std::string bytes;if(!ReadFile(path,bytes))return Status::IOError;if(bytes.size()<28||bytes.substr(0,8)!="TLSMMAN1")return Status::Corruption;
    std::string_view view(bytes);auto tail=view.substr(view.size()-4);uint64_t crc;Number(tail,crc,4);view.remove_suffix(4);if(CRC(view)!=crc)return Status::Corruption;
    view.remove_prefix(8);uint64_t nw,nt;if(!Number(view,stamp_,8)||!Number(view,nw,4)||!Number(view,nt,4)||nw>view.size()/8||nt>(view.size()-nw*8)/12||view.size()!=nw*8+nt*12||nw==0)return Status::Corruption;
    for(uint64_t i=0;i<nw;++i){uint64_t id;if(!Number(view,id,8)||!id||std::find(wals_.begin(),wals_.end(),id)!=wals_.end())return Status::Corruption;wals_.push_back(id);if(id+1>next_id_.load())next_id_.store(id+1);}
    for(uint64_t i=0;i<nt;++i){uint64_t id,level;if(!Number(view,id,8)||!Number(view,level,4)||!id||level>63)return Status::Corruption;for(auto& t:tables_)if(t->id==id)return Status::Corruption;
      std::shared_ptr<Table> t;auto s=Table::Open(Path(id,".sst"),id,static_cast<unsigned>(level),t);if(s!=Status::Ok)return s;tables_.push_back(t);if(id+1>next_id_.load())next_id_.store(id+1);}
  } else if(ec) return Status::IOError;
  SortTables();for(size_t i=1;i<tables_.size();++i)if(tables_[i]->level>0&&tables_[i]->level==tables_[i-1]->level&&tables_[i-1]->last>=tables_[i]->first)return Status::Corruption;
  // Replay only manifest-referenced WALs; interrupted unpublished files are orphans.
  for(auto id:wals_) {
    std::string data;if(!ReadFile(Path(id,".wal"),data))return Status::IOError;std::vector<Record> rows;size_t used;auto s=Decode(data,rows,used);if(s!=Status::Ok)return s;
    auto mem=std::make_shared<MemTable>();uint64_t previous=0;for(auto& r:rows){if(r.stamp<=previous)return Status::Corruption;previous=r.stamp;stamp_=std::max(stamp_,r.stamp);mem->Put(std::move(r));}pending_.push_back({id,mem});
  }
  stamp_=std::max(stamp_,NowNanos());
  active_wal_=next_id_++;wal_=::open(Path(active_wal_,".wal").c_str(),O_CREAT|O_EXCL|O_WRONLY|O_APPEND,0644);if(wal_<0||!Durable(wal_)||!SyncDirectory(options_.db_path))return Status::IOError;
  auto wals=wals_;wals.push_back(active_wal_);auto s=Manifest(wals,tables_);if(s!=Status::Ok)return s;wals_=std::move(wals);
  // Clean only numeric storage artifacts not referenced by the durable manifest.
  for(auto& entry:std::filesystem::directory_iterator(options_.db_path)) {
    auto ext=entry.path().extension();if(ext!=".wal"&&ext!=".sst")continue;
    bool live=false;
    for(auto id:wals_) {live=live||entry.path()==Path(id,".wal");}
    for(auto& t:tables_) {live=live||entry.path()==t->path;}
    if(!live)std::filesystem::remove(entry.path(),ec);
  }
  return Status::Ok;
}
Status Engine::Init() {
  std::error_code ec;std::filesystem::create_directories(options_.db_path,ec);if(ec)return Status::IOError;
  lock_=::open((options_.db_path+"/LOCK").c_str(),O_CREAT|O_RDWR,0644);if(lock_<0||!TryLockFile(lock_))return Status::IOError;
  auto s=Load();if(s!=Status::Ok)return s;
  // The browser build performs the same maintenance inline (see engine.hpp).
#ifndef TINYLSM_INLINE_MAINTENANCE
  flush_thread_=std::thread(&Engine::FlushWorker,this);compact_thread_=std::thread(&Engine::CompactWorker,this);
#endif
  return Status::Ok;
}
Engine::~Engine() {
  {std::unique_lock lock(mu_);stop_=true;changed_.notify_all();}
  if(flush_thread_.joinable()) {flush_thread_.join();}
  if(compact_thread_.joinable()) {compact_thread_.join();}
  if(wal_>=0){Durable(wal_);::close(wal_);}if(lock_>=0)::close(lock_);
}
} // namespace tinylsm::detail
