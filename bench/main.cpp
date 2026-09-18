#include "tinylsm/db.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include <atomic>
#include <unistd.h>
using Clock=std::chrono::steady_clock;
#if defined(__linux__)
#include <sys/vfs.h>
// tmpfs has no page cache to evict, so a "cold" read there measures nothing.
static bool IsTmpfs(const std::string& path) {
  struct statfs info{};
  if(::statfs(path.c_str(),&info)!=0) return false;
  return static_cast<unsigned long>(info.f_type)==0x01021994UL;  // TMPFS_MAGIC
}
#else
static bool IsTmpfs(const std::string&) { return false; }
#endif
static void Require(tinylsm::Status s){if(s!=tinylsm::Status::Ok)throw std::runtime_error(tinylsm::StatusName(s));}
// Emits one JSON object per metric. Besides the percentile summary it emits a
// log-bucketed histogram, so the portfolio charts can draw a real latency
// distribution rather than three summary points.
static void Report(const char* name,std::vector<double> samples){
  std::sort(samples.begin(),samples.end());
  auto p=[&](double q){return samples[std::min(samples.size()-1,static_cast<size_t>(q*samples.size()))];};
  constexpr int kBuckets=24;
  double lo=samples.front()>0?samples.front():1e-3,hi=samples.back()>lo?samples.back():lo*10;
  double low=std::log10(lo),high=std::log10(hi);if(high-low<1e-9)high=low+1;
  std::vector<double> edges;std::vector<size_t> counts(kBuckets,0);
  for(int i=0;i<=kBuckets;++i)edges.push_back(std::pow(10.0,low+(high-low)*i/kBuckets));
  for(double s:samples){auto b=static_cast<int>((std::log10(s>0?s:lo)-low)/(high-low)*kBuckets);++counts[static_cast<size_t>(std::clamp(b,0,kBuckets-1))];}
  std::cout<<"{\"metric\":\""<<name<<"\",\"unit\":\"us\",\"samples\":"<<samples.size()<<",\"p50\":"<<p(.5)<<",\"p90\":"<<p(.9)<<",\"p99\":"<<p(.99)<<",\"histogram\":{\"edges_us\":[";
  for(size_t i=0;i<edges.size();++i){if(i)std::cout<<',';std::cout<<edges[i];}
  std::cout<<"],\"counts\":[";
  for(size_t i=0;i<counts.size();++i){if(i)std::cout<<',';std::cout<<counts[i];}
  std::cout<<"]}}\n";
}
int main(int argc,char** argv){try{
  size_t n=argc>1?std::stoull(argv[1]):20000;if(n==0||n>10000000)throw std::runtime_error("count must be 1..10000000");
  // argv[2] optionally places the database on a disk-backed filesystem so the
  // cold-read metric measures a real device instead of tmpfs.
  std::filesystem::path path=argc>2?std::filesystem::path(argv[2]):std::filesystem::temp_directory_path()/("tinylsm-bench-"+std::to_string(getpid()));
  std::filesystem::remove_all(path);
  const bool disk_backed=!IsTmpfs(path.parent_path().string());
  tinylsm::Options o;o.db_path=path.string();tinylsm::Status s;auto db=tinylsm::DB::Open(o,s);Require(s);
  std::string value(100,'v');std::vector<std::string> keys;for(size_t i=0;i<n;++i)keys.push_back("key"+std::to_string(i));
  std::vector<double> samples;for(auto& k:keys){auto start=Clock::now();Require(db->Put(k,value));samples.push_back(std::chrono::duration<double,std::micro>(Clock::now()-start).count());}Report("buffered_put",samples);
  Require(db->Compact());for(auto& k:keys)Require(db->Get(k,value));samples.clear();
  for(auto& k:keys){auto start=Clock::now();Require(db->Get(k,value));samples.push_back(std::chrono::duration<double,std::micro>(Clock::now()-start).count());}Report("cached_get",samples);
  // Cold point reads are measured at the end of this run: the database is
  // reopened with a non-caching block cache and every SSTable is evicted from
  // the page cache, so the timed path really fetches from the device.
  std::atomic<bool> ok=true;auto start=Clock::now();std::vector<std::thread> clients;
  for(int t=0;t<4;++t)clients.emplace_back([&,t]{for(size_t i=t;i<n;i+=4)if(db->Put(keys[i],value)!=tinylsm::Status::Ok)ok=false;});
  for(auto& t:clients)t.join();if(!ok)throw std::runtime_error("concurrent writes failed");
  double seconds=std::chrono::duration<double>(Clock::now()-start).count();std::cout<<"{\"metric\":\"concurrent_write\",\"threads\":4,\"ops_per_second\":"<<n/seconds<<"}\n";
  Require(db->Compact());auto stats=db->Stats();
  // Two amplification figures: cumulative SST bytes written over logical bytes
  // (tinylsm's own accounting) and the live SST footprint, which is the figure
  // both engines can report so the comparison chart is like for like.
  std::error_code size_ec;uint64_t live=0;
  for(auto& entry:std::filesystem::directory_iterator(path)) if(entry.path().extension()==".sst") live+=entry.file_size(size_ec);
  std::cout<<"{\"metric\":\"sst_write_amplification\",\"ratio\":"<<double(stats.table_bytes_written)/stats.logical_bytes
           <<",\"live_ratio\":"<<(stats.logical_bytes==0?0.0:double(live)/stats.logical_bytes)
           <<",\"logical_bytes\":"<<stats.logical_bytes<<",\"sst_bytes\":"<<stats.table_bytes_written
           <<",\"live_sst_bytes\":"<<live<<"}\n";
  // Cold point reads (design section 5). Opening a second engine with a
  // non-caching block cache removes the engine-side cache, and evicting each
  // SSTable from the page cache removes the OS-side cache, so the timed path is
  // the real read-path hierarchy ending in a device fetch.
  db.reset();
#if defined(__linux__)
  std::vector<std::string> sstables;
  for(auto& entry:std::filesystem::directory_iterator(path)) if(entry.path().extension()==".sst") sstables.push_back(entry.path().string());
  if(sstables.empty()) {
    std::cerr<<"cold_get not measured: no SSTables under "<<path.string()<<'\n';
  } else if(!disk_backed) {
    std::cerr<<"cold_get not measured: "<<path.string()<<" is on tmpfs, so there is no page cache to evict. Pass a disk-backed directory as argv[2].\n";
  } else {
    std::vector<int> fds;for(auto& file:sstables) {int fd=::open(file.c_str(),O_RDONLY);if(fd>=0) fds.push_back(fd);}
    tinylsm::Options cold_options=o;cold_options.block_cache_size_bytes=1;
    auto cold_db=tinylsm::DB::Open(cold_options,s);Require(s);
    const size_t count=std::min<size_t>(n,2000);
    std::vector<double> cold_samples;
    for(size_t i=0;i<count&&!fds.empty();++i) {
      for(int fd:fds) ::posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);
      auto& key=keys[(i*sstables.size())%n];
      auto start=Clock::now();Require(cold_db->Get(key,value));cold_samples.push_back(std::chrono::duration<double,std::micro>(Clock::now()-start).count());
    }
    for(int fd:fds) ::close(fd);
    if(!cold_samples.empty()) Report("cold_get",cold_samples);
    cold_db.reset();
  }
#endif
  std::filesystem::remove_all(path);
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
