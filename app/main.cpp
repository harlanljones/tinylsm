#include "commands.hpp"
#include <iostream>
int main(int argc,char** argv){
  tinylsm::Options options;if(argc>1)options.db_path=argv[1];
  tinylsm::Status status;auto db=tinylsm::DB::Open(options,status);
  if(!db){std::cerr<<tinylsm::StatusName(status)<<'\n';return 1;}
  std::string line;while(std::getline(std::cin,line)){if(line=="QUIT")break;std::cout<<tinylsm::Execute(*db,line)<<'\n';}
  return db->Sync()==tinylsm::Status::Ok?0:1;
}
