#include "table.hpp"
#include <iostream>
#include <stdexcept>
using namespace tinylsm; using namespace tinylsm::detail;
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);}while(false)
int main() {
  auto path=(std::filesystem::temp_directory_path()/("tinylsm-table-"+std::to_string(getpid())+".sst")).string();
  try {
    std::vector<Record> rows{{"alpha","one",1,false},{"alphabet","two",2,false},{"beta","",3,true},{"zeta",std::string(500,'z'),4,false}};
    auto encoded=BlockEncode(rows);std::vector<Record> decoded;CHECK(BlockDecode(encoded,decoded)==Status::Ok);CHECK(decoded.size()==4&&decoded[1].key=="alphabet"&&decoded[2].deleted);
    encoded[0]^=1;decoded.clear();CHECK(BlockDecode(encoded,decoded)==Status::Corruption);
    CHECK(Table::Build(path,rows,64)==Status::Ok);std::shared_ptr<Table> t;CHECK(Table::Open(path,1,0,t)==Status::Ok);
    Cache cache(4096);Record r;CHECK(t->Get("alphabet",cache,r)==Status::Ok&&r.value=="two");CHECK(t->Get("alphabet",cache,r)==Status::Ok&&cache.hits>0);
    CHECK(t->Get("absent",cache,r)==Status::NotFound);CHECK(t->Get("beta",cache,r)==Status::Ok&&r.deleted);
    Cache nocache(0);CHECK(t->Get("alphabet",nocache,r)==Status::Ok&&r.value=="two");
    CHECK(t->Get("absent",nocache,r)==Status::NotFound);CHECK(t->Get("beta",nocache,r)==Status::Ok&&r.deleted);
    decoded.clear();CHECK(t->Rows(cache,decoded)==Status::Ok&&decoded.size()==4&&decoded.back().value==std::string(500,'z'));
    // SSTable parser: an intact file validates, and each class of damage is
    // reported as Corruption rather than silently accepted.
    std::string file;CHECK(ReadFile(path,file));
    CHECK(Table::Validate(file)==Status::Ok);
    CHECK(Table::Validate(std::string_view(file).substr(0,40))==Status::Corruption);
    CHECK(Table::Validate(std::string(file).substr(0,file.size()-1))==Status::Corruption);
    auto damaged=file;damaged[damaged.size()-1]^=1;CHECK(Table::Validate(damaged)==Status::Corruption); // footer magic
    damaged=file;damaged[4]^=1;CHECK(Table::Validate(damaged)==Status::Corruption);                     // first data block
    damaged=file;damaged[0]^=1;CHECK(Table::Validate(damaged)==Status::Corruption);                     // block length prefix
    Bloom b(10000);for(int i=0;i<10000;++i)b.Add(std::to_string(i));size_t fp=0;
    for(int i=0;i<10000;++i)CHECK(b.MayContain(std::to_string(i)));
    for(int i=10000;i<110000;++i)fp+=b.MayContain(std::to_string(i));CHECK(fp<2000);
    std::filesystem::remove(path);std::cout<<"SST round-trip, checksum, cache, bloom passed; false positives "<<fp<<"/100000\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
