#include "../src/util/util_thread_scheduling.h"
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

using namespace renderstack::scheduling;
using namespace renderstack::scheduling::detail;
namespace {
int failures = 0, checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (0)

CpuInfo Cpu(uint32_t id, uint8_t logical, uint8_t core, uint8_t efficiency) {
  CpuInfo r{}; r.id = id; r.logical = logical; r.core = core; r.efficiency = efficiency; return r;
}
Topology Hybrid() {
  Topology r{};
  const CpuInfo cpus[] = { Cpu(450,5,3,0), Cpu(200,3,1,8), Cpu(101,1,0,8), Cpu(400,4,2,0), Cpu(199,2,1,8), Cpu(100,0,0,8) };
  r.count = sizeof(cpus)/sizeof(cpus[0]);
  for (size_t i=0;i<r.count;++i) r.cpus[i]=cpus[i];
  return r;
}
Limits All() { return {0x3fu,0x3fu,0,1,6}; }
void SelectionTests() {
  auto t=Hybrid(); auto l=All();
  auto owner=SelectCpuSets(t,l,Role::DeviceOwner); auto cs=SelectCpuSets(t,l,Role::CommandStream); auto bg=SelectCpuSets(t,l,Role::Background);
  CHECK(owner.count==1 && owner.ids[0]==100);
  CHECK(cs.count==1 && cs.ids[0]==199);
  CHECK(bg.count==2 && bg.ids[0]==400 && bg.ids[1]==450);
  l.processMask=0x3e; l.threadMask=0x3b;
  owner=SelectCpuSets(t,l,Role::DeviceOwner); cs=SelectCpuSets(t,l,Role::CommandStream);
  CHECK(owner.count==1 && owner.ids[0]==101); CHECK(cs.count==1 && cs.ids[0]==200);
  l.processMask=3; CHECK(SelectCpuSets(t,l,Role::CommandStream).count==0);
  l.processMask=0; CHECK(SelectCpuSets(t,l,Role::DeviceOwner).count==0);
  l=All(); l.groupCount=2; CHECK(SelectCpuSets(t,l,Role::DeviceOwner).count==0);
  l=All(); l.activeProcessors=33; CHECK(SelectCpuSets(t,l,Role::CommandStream).count==0);
  l=All(); l.threadGroup=1; CHECK(SelectCpuSets(t,l,Role::DeviceOwner).count==0);
  t.cpus[1].parked=true; t.cpus[4].allocated=true; CHECK(SelectCpuSets(t,All(),Role::CommandStream).count==0);
  t.cpus[4].allocatedToProcess=true; CHECK(SelectCpuSets(t,All(),Role::CommandStream).count==1);
  t=Hybrid(); for(auto& cpu:t.cpus) cpu.efficiency=0;
  CHECK(SelectCpuSets(t,All(),Role::Background).count==0); CHECK(SelectCpuSets(t,All(),Role::CommandStream).count==1);
  t.count=1; CHECK(SelectCpuSets(t,All(),Role::CommandStream).count==0);
  t=Hybrid(); t.cpus[0].group=1; CHECK(SelectCpuSets(t,All(),Role::DeviceOwner).count==0);
}
void Write32(std::vector<uint8_t>& b,size_t at,uint32_t v) { for(size_t i=0;i<4;++i) b[at+i]=uint8_t(v>>(i*8)); }
void Append(std::vector<uint8_t>& b,uint32_t id,uint8_t logical,uint8_t flags) {
  auto at=b.size(); b.resize(at+32); Write32(b,at,32); Write32(b,at+8,id);
  b[at+14]=logical; b[at+15]=logical/2; b[at+18]=8; b[at+19]=flags;
}
void ParserTests() {
  std::vector<uint8_t> b(12,0); Write32(b,0,12); Write32(b,4,99); // Future record must be skipped.
  Append(b,7,0,7); Append(b,8,1,0); Topology p{};
  CHECK(ParseCpuSetInformation(b.data(),b.size(),p)); CHECK(p.count==2 && p.cpus[0].id==7);
  CHECK(p.cpus[0].parked && p.cpus[0].allocated && p.cpus[0].allocatedToProcess);
  CHECK(!ParseCpuSetInformation(b.data(),b.size()-1,p)); CHECK(p.count==0);
  Write32(b,0,0); CHECK(!ParseCpuSetInformation(b.data(),b.size(),p));
  Write32(b,0,7); CHECK(!ParseCpuSetInformation(b.data(),b.size(),p));
  Write32(b,0,0xffffffffu); CHECK(!ParseCpuSetInformation(b.data(),b.size(),p));
  b.clear(); Append(b,7,0,0); Append(b,7,1,0); CHECK(!ParseCpuSetInformation(b.data(),b.size(),p));
  CHECK(!ParseCpuSetInformation(nullptr,1,p)); CHECK(ParseCpuSetInformation(nullptr,0,p) && p.count==0);
}
struct FakePlatform {
  Topology topology=Hybrid(); Limits limits=All(); CpuSetList selected{}; uint32_t thread=42;
  int calls=0,writes=0,registrations=0,reversions=0;
  bool topologyOk=true,getOk=true,setOk=true,mmcssOk=true;
  Operations Ops() {
    return {this,
      [](void* p,Topology& t,Limits& l) noexcept {auto& f=*static_cast<FakePlatform*>(p);++f.calls;t=f.topology;l=f.limits;return f.topologyOk;},
      [](void* p,CpuSetList& l) noexcept {auto& f=*static_cast<FakePlatform*>(p);++f.calls;l=f.selected;return f.getOk;},
      [](void* p,const CpuSetList& l) noexcept {auto& f=*static_cast<FakePlatform*>(p);++f.calls;++f.writes;if(f.setOk)f.selected=l;return f.setOk;},
      [](void* p) noexcept {auto& f=*static_cast<FakePlatform*>(p);++f.calls;return f.thread;},
      [](void* p) noexcept -> void* {auto& f=*static_cast<FakePlatform*>(p);++f.calls;++f.registrations;return f.mmcssOk?p:nullptr;},
      [](void* p,void* h) noexcept {auto& f=*static_cast<FakePlatform*>(p);++f.calls;if(h==p)++f.reversions;} };
  }
};
void SessionTests() {
  FakePlatform f;
  {SchedulingSession s({false,true},Role::CommandStream,f.Ops(),nullptr);} CHECK(f.calls==0);
  f.selected.count=1;f.selected.ids[0]=450;
  {SchedulingSession s({true,true},Role::CommandStream,f.Ops(),nullptr);CHECK(f.selected.count==1 && f.selected.ids[0]==199);CHECK(f.registrations==1);}
  CHECK(f.selected.count==1 && f.selected.ids[0]==450);CHECK(f.writes==2 && f.reversions==1);
  f={};{SchedulingSession s({true,false},Role::CommandStream,f.Ops(),nullptr);f.selected.count=1;f.selected.ids[0]=400;}CHECK(f.writes==1 && f.selected.ids[0]==400);
  f={};f.getOk=false;{SchedulingSession s({true,true},Role::CommandStream,f.Ops(),nullptr);}CHECK(f.writes==0 && f.reversions==1);
  f={};f.topologyOk=false;{SchedulingSession s({true,true},Role::CommandStream,f.Ops(),nullptr);}CHECK(f.writes==0 && f.reversions==1);
  f={};f.setOk=false;f.mmcssOk=false;{SchedulingSession s({true,true},Role::CommandStream,f.Ops(),nullptr);}CHECK(f.writes==1 && f.registrations==1 && f.reversions==0);
  f={};{SchedulingSession s({true,true},Role::DeviceOwner,f.Ops(),nullptr);}CHECK(f.registrations==0 && f.writes==2);
  f={};{SchedulingSession s({true,true},Role::Background,f.Ops(),nullptr);auto swap=f.selected.ids[0];f.selected.ids[0]=f.selected.ids[1];f.selected.ids[1]=swap;}
  CHECK(f.registrations==0 && f.selected.count==0 && f.writes==2);
  f={};{SchedulingSession s({true,true},Role::CommandStream,f.Ops(),nullptr);f.thread=43;}CHECK(f.writes==1 && f.reversions==0);
  f={};{SchedulingSession s({true,false},Role::CommandStream,f.Ops(),[](const char*){throw 1;});}CHECK(f.writes==2);
  {ThreadSchedulingScope scope({},Role::CommandStream,nullptr);}
  static_assert(!std::is_copy_constructible_v<ThreadSchedulingScope>);static_assert(!std::is_move_constructible_v<ThreadSchedulingScope>);
  wchar_t tiny[1]={L'X'};CHECK(!renderstack::scheduling::ResolveConfigPath(tiny,1) && tiny[0]==L'\0');CHECK(!renderstack::scheduling::ResolveConfigPath(nullptr,0));
}
}
int main(int argc, char** argv){
  if (argc > 1 && std::strcmp(argv[1], "--config") == 0) {
    auto options = renderstack::scheduling::ReadOptions(); wchar_t path[1024]{};
    const bool found = renderstack::scheduling::ResolveConfigPath(path, 1024);
#ifdef _WIN32
    char narrow[2048]{};
    if (found) WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
    std::printf("enabled=%d mmcss=%d path=%s\n", options.enabled ? 1 : 0, options.mmcss ? 1 : 0, found ? narrow : "none");
#else
    std::printf("enabled=%d mmcss=%d path=%s\n", options.enabled ? 1 : 0, options.mmcss ? 1 : 0, found ? "resolved" : "none");
#endif
    return 0;
  }
  if (argc > 1 && std::strcmp(argv[1], "--runtime") == 0) {
    std::puts("runtime CPU-set smoke: scheduling scope entered (MMCSS may be unavailable; SKIP is non-fatal)");
    ThreadSchedulingScope scope(renderstack::scheduling::ReadOptions(), Role::CommandStream, nullptr);
    std::puts("runtime CPU-set smoke: scope exited"); return 0;
  }
  SelectionTests();ParserTests();SessionTests();std::printf("thread scheduling: %d checks, %d failures\n",checks,failures);return failures?1:0;}
