#include "core.hpp"
#include <iostream>
#include <thread>
#include <sys/wait.h>
using namespace tinylsm;using namespace tinylsm::detail;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string("line ")+std::to_string(__LINE__)+": " #x);}while(false)
int main(){
  auto root=std::filesystem::temp_directory_path()/("tinylsm-recovery-"+std::to_string(getpid()));
  try{
    CHECK(CRC("123456789")==0xcbf43926);
    auto one=Encode({"first","committed",1,false}),two=Encode({"second","partial",2,false});
    for(size_t cut=0;cut<two.size();++cut){std::vector<Record> rows;size_t used;CHECK(Decode(one+two.substr(0,cut),rows,used)==Status::Ok);CHECK(rows.size()==1&&used==one.size());}
    auto corrupt=one;corrupt.back()^=1;std::vector<Record> rows;size_t used;CHECK(Decode(corrupt,rows,used)==Status::Corruption);
    // Exhaust every byte-boundary power loss on an actual manifest-referenced WAL.
    for(size_t cut=0;cut<=two.size();++cut){
      Options o;o.db_path=(root/std::to_string(cut)).string();Status s;auto db=DB::Open(o,s);CHECK(db);db.reset();
      std::filesystem::path wal;for(auto& e:std::filesystem::directory_iterator(o.db_path))if(e.path().extension()==".wal")wal=e.path();
      int fd=::open(wal.c_str(),O_WRONLY|O_TRUNC);CHECK(fd>=0);CHECK(WriteAll(fd,one+two.substr(0,cut)));CHECK(Durable(fd));::close(fd);
      db=DB::Open(o,s);CHECK(db&&s==Status::Ok);std::string value;CHECK(db->Get("first",value)==Status::Ok&&value=="committed");
      CHECK(db->Get("second",value)==(cut==two.size()?Status::Ok:Status::NotFound));CHECK(db->Put("after","recovery")==Status::Ok);CHECK(db->Compact()==Status::Ok);db.reset();
      db=DB::Open(o,s);CHECK(db);CHECK(db->Get("after",value)==Status::Ok&&value=="recovery");db.reset();
    }
    // Child terminates without destructors: only Sync(), not close(), provides the guarantee.
    Options o;o.db_path=(root/"crash").string();pid_t child=fork();CHECK(child>=0);
    if(child==0){Status s;auto db=DB::Open(o,s);if(!db)_exit(2);for(int i=0;i<100;++i)if(db->Put(std::to_string(i),"durable")!=Status::Ok)_exit(3);_exit(db->Sync()==Status::Ok?0:4);}
    int result;CHECK(waitpid(child,&result,0)==child&&WIFEXITED(result)&&WEXITSTATUS(result)==0);
    Status s;auto db=DB::Open(o,s);CHECK(db);for(int i=0;i<100;++i){std::string v;CHECK(db->Get(std::to_string(i),v)==Status::Ok&&v=="durable");}db.reset();
    // Lock-free skiplist traversal with its documented single-writer contract.
    MemTable mem;std::atomic<bool> done=false;std::thread writer([&]{for(int i=1;i<=10000;++i)mem.Put({std::to_string(i),"value",static_cast<uint64_t>(i),false});done=true;});
    std::vector<std::thread> readers;for(int n=0;n<4;++n)readers.emplace_back([&]{Record r;while(!done){mem.Get("500",r);mem.Get("9999",r);}});
    writer.join();for(auto& t:readers)t.join();CHECK(mem.Rows().size()==10000);
    o.db_path=(root/"concurrent").string();o.memtable_size_bytes=8192;db=DB::Open(o,s);CHECK(db);std::atomic<bool> ok=true;
    std::vector<std::thread> clients;for(int t=0;t<4;++t)clients.emplace_back([&,t]{for(int i=0;i<500;++i){auto k=std::to_string(t)+":"+std::to_string(i);std::string v;if(db->Put(k,k)!=Status::Ok||db->Get(k,v)!=Status::Ok||v!=k)ok=false;}});
    for(auto& t:clients)t.join();CHECK(ok);CHECK(db->Compact()==Status::Ok);db.reset();db=DB::Open(o,s);CHECK(db);size_t count=0;auto it=db->NewIterator();CHECK(it->status()==Status::Ok);for(;it->Valid();it->Next())++count;CHECK(count==2000);db.reset();
    std::filesystem::remove_all(root);std::cout<<"WAL truncation boundaries, CRC, process crash, skiplist concurrency, concurrent DB passed\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<"\nEvidence: "<<root<<'\n';return 1;}
}
