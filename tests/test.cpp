#include "tinylsm/db.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace tinylsm;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string("line ")+std::to_string(__LINE__)+": " #x); } while(false)
int main(int argc,char**) {
  const auto path=std::filesystem::temp_directory_path()/("tinylsm-test-"+std::to_string(getpid()));
  try {
    std::filesystem::remove_all(path);
    Options o; o.db_path=path.string(); o.memtable_size_bytes=1024; o.block_size_bytes=128;
    Status s; auto db=DB::Open(o,s); CHECK(db&&s==Status::Ok);
    CHECK(db->Put("a","one")==Status::Ok);
    CHECK(db->Put("a","two")==Status::Ok);
    CHECK(db->Put("b","gone")==Status::Ok);
    CHECK(db->Delete("b")==Status::Ok);
    CHECK(db->Put("c","three")==Status::Ok);
    CHECK(db->Put(std::string("x\0y",3),std::string("v\0z",3))==Status::Ok);
    CHECK(db->Sync()==Status::Ok); db.reset();
    db=DB::Open(o,s); CHECK(db&&s==Status::Ok);
    std::string value; CHECK(db->Get("a",value)==Status::Ok&&value=="two");
    CHECK(db->Get("b",value)==Status::NotFound);
    CHECK(db->Get(std::string("x\0y",3),value)==Status::Ok&&value==std::string("v\0z",3));
    auto it=db->NewIterator(); CHECK(it->Valid()&&it->Key()=="a"&&it->Value()=="two");
    it->Next(); CHECK(it->Valid()&&it->Key()=="c"); it->Seek("b"); CHECK(it->Key()=="c");
    it->Next(); CHECK(it->Valid()); it->Next(); CHECK(!it->Valid());
    Status locked; CHECK(!DB::Open(o,locked)&&locked==Status::IOError);
    CHECK(db->Put(std::string(65536,'x'),"v")==Status::InvalidArgument);
    if(argc>1) {
      CHECK(db->Flush()==Status::Ok);
      bool found=false; for(auto& entry:std::filesystem::directory_iterator(path)) found|=entry.path().extension()==".sst";
      CHECK(found); CHECK(db->Stats().active_bytes==0);
      db.reset(); db=DB::Open(o,s); CHECK(db&&s==Status::Ok);
      CHECK(db->Get("a",value)==Status::Ok&&value=="two"); CHECK(db->Get("b",value)==Status::NotFound);
    }
    db.reset(); std::filesystem::remove_all(path);
    std::cout<<"persistence"<<(argc>1?" + SST flush":"")<<" passed\n"; return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<"\nEvidence directory: "<<path<<'\n'; return 1; }
}
