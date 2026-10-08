#include "archives.hpp"
#include "archive_preflight.hpp"
#include <chrono>
#include <iostream>
#include <thread>
int main(int argc,char** argv){
 if(argc>2&&std::string(argv[1])=="inspect"){
  const auto inspection=archives::inspect(std::vector<std::string>(argv+2,argv+argc));
  std::cout<<inspection.kind<<"\n"<<inspection.refusal<<"\n";return 0;}
 if(argc<4)return 2;
 archives::Request request;request.destination=argv[1];request.max_bytes=std::stoull(argv[2]);
 for(int i=3;i<argc;i++)request.sources.emplace_back(argv[i]);
 std::string error;const auto id=archives::enqueue(std::move(request),error);
 if(!id){std::cout<<"failed\n"<<error;return 0;}
 while(true){for(const auto& s:archives::poll())if(s.state=="completed"||s.state=="failed"||s.state=="cancelled"){
  std::cout<<s.state<<"\n"<<s.written<<"\n"<<s.error;archives::stop();return 0;}
 std::this_thread::sleep_for(std::chrono::milliseconds(10));}
}
