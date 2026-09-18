#include "table.hpp"
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data,size_t size){
  auto bytes=std::string_view(reinterpret_cast<const char*>(data),size);
  tinylsm::detail::Table::Validate(bytes);
  std::vector<tinylsm::detail::Record> rows;tinylsm::detail::BlockDecode(bytes,rows);
  return 0;
}
