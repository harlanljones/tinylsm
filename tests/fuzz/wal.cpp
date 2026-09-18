#include "core.hpp"
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data,size_t size){
  std::vector<tinylsm::detail::Record> records;size_t consumed;
  tinylsm::detail::Decode(std::string_view(reinterpret_cast<const char*>(data),size),records,consumed);
  return 0;
}
