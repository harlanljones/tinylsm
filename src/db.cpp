#include "engine.hpp"
namespace tinylsm {
const char* StatusName(Status s) { switch(s) {case Status::Ok:return "Ok";case Status::NotFound:return "NotFound";case Status::Corruption:return "Corruption";case Status::IOError:return "IOError";case Status::InvalidArgument:return "InvalidArgument";} return "Unknown"; }
std::unique_ptr<DB> DB::Open(const Options& o,Status& s) {
  if(o.db_path.empty()||o.memtable_size_bytes==0||o.block_size_bytes<64) {s=Status::InvalidArgument;return {};}
  auto db=std::make_unique<detail::Engine>(o);s=db->Init();if(s!=Status::Ok) return {};return db;
}
} // namespace tinylsm
