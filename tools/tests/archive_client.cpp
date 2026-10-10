#include <dirent.h>
#include "archives.hpp"
#include "archive_preflight.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
void archives::trace(const char*,const char*){}
// ARCHIVE_PIPELINE_BYTES=0 exercises the inline writer.
std::size_t archives::pipeline_bytes(){const char* v=std::getenv("ARCHIVE_PIPELINE_BYTES");return v?std::strtoull(v,nullptr,10):32u<<20;}
bool archives::list_directory(const char* path,void(*visit)(const char*,void*),void* user){
 DIR* d=opendir(path);if(!d)return false;while(auto* e=readdir(d))visit(e->d_name,user);closedir(d);return true;}
int main(int argc,char** argv){
 if(argc>2&&std::string(argv[1])=="inspect"){
  const auto inspection=archives::inspect(std::vector<std::string>(argv+2,argv+argc));
  std::cout<<inspection.kind<<"\n"<<inspection.refusal<<"\n";return 0;}
 if(argc<4)return 2;
 archives::Request request;request.destination=argv[1];request.max_bytes=std::stoull(argv[2]);
 for(int i=3;i<argc;i++){
  if(std::string(argv[i])=="--password"&&i+1<argc)request.password=argv[++i];
  else request.sources.emplace_back(argv[i]);
 }
 std::string error;const auto id=archives::enqueue(std::move(request),error);
 if(!id){std::cout<<"failed\n"<<error;return 0;}
 while(true){for(const auto& s:archives::poll())if(s.state=="completed"||s.state=="failed"||s.state=="cancelled"){
  std::cout<<s.state<<"\n"<<s.written<<"\n"<<s.error;archives::stop();return 0;}
 std::this_thread::sleep_for(std::chrono::milliseconds(10));}
}
