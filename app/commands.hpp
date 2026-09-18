#pragma once
#include "tinylsm/db.hpp"
#include <sstream>
#include <iomanip>
namespace tinylsm {
// Quoted arguments allow spaces and empty strings; the C++ API remains binary-safe.
inline std::string Execute(DB& db,const std::string& line) {
  std::istringstream in(line);std::string command,key,value,extra;in>>command;
  auto read=[&](std::string& out){return bool(in>>std::quoted(out));};
  auto end=[&]{return !(in>>extra);};
  if(command=="PUT") {if(!read(key)||!read(value)||!end())return "Usage: PUT key value";return StatusName(db.Put(key,value));}
  if(command=="GET") {if(!read(key)||!end())return "Usage: GET key";auto s=db.Get(key,value);return s==Status::Ok?value:StatusName(s);}
  if(command=="DEL") {if(!read(key)||!end())return "Usage: DEL key";return StatusName(db.Delete(key));}
  if(command=="SCAN") {bool seek=read(key);if(!end())return "Usage: SCAN [start-key]";auto it=db.NewIterator();if(it->status()!=Status::Ok)return StatusName(it->status());if(seek)it->Seek(key);else it->SeekToFirst();std::ostringstream out;for(;it->Valid();it->Next())out<<std::quoted(std::string(it->Key()))<<" = "<<std::quoted(std::string(it->Value()))<<'\n';return out.str();}
  if(command=="SYNC"&&end())return StatusName(db.Sync());
  if(command=="FLUSH"&&end())return StatusName(db.Flush());
  if(command=="COMPACT"&&end())return StatusName(db.Compact());
  if(command=="STATS"&&end()) {auto s=db.Stats();std::ostringstream out;
    out<<"{\"writes\":"<<s.writes<<",\"reads\":"<<s.reads<<",\"active_bytes\":"<<s.active_bytes<<",\"immutables\":"<<s.immutable_count<<",\"flushes\":"<<s.flushes<<",\"compactions\":"<<s.compactions<<",\"logical_bytes\":"<<s.logical_bytes<<",\"table_bytes_written\":"<<s.table_bytes_written<<",\"cache_hits\":"<<s.cache_hits<<",\"cache_misses\":"<<s.cache_misses<<",\"levels\":[";
    for(size_t i=0;i<s.level_files.size();++i){if(i)out<<',';out<<s.level_files[i];}out<<"]}";return out.str();}
  return "Commands: PUT key value | GET key | DEL key | SCAN [key] | SYNC | FLUSH | COMPACT | STATS";
}
}
