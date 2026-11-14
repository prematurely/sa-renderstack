#include <cstdlib>
#include <new>
#include <cstdio>
#include <array>
#include <cstddef>

// Deterministic failure at the real standard containers' allocation boundary.
// No alternate LRU implementation or product allocator parameter is involved.
static long failAfter=-1, allocations=0, live=0;
void* operator new(std::size_t n) {
  if(failAfter==0)throw std::bad_alloc();
  if(failAfter>0)--failAfter;
  auto p=std::malloc(n?n:1);if(!p)throw std::bad_alloc();++allocations;++live;return p;
}
void operator delete(void* p) noexcept {if(p){--live;std::free(p);}}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void* p) noexcept{::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept{::operator delete(p);}
#include "../backend/dxvk/src/util/util_lru.h"
static unsigned checks=0,failures=0;
#define CHECK(c) do{++checks;if(!(c)){++failures;std::printf("FAIL line=%u %s\n",__LINE__,#c);}}while(0)
static bool Order(const dxvk::lru_list<int>& l,std::initializer_list<int> expected) {
  auto i=l.leastRecentlyUsedIter();for(auto v:expected){if(i==l.leastRecentlyUsedEndIter() || *i++!=v)return false;}return i==l.leastRecentlyUsedEndIter() && l.size()==expected.size();
}
int main() {
  for(long point=0;point<4;++point){const auto before=live;{
    dxvk::lru_list<int> l;l.insert(1);l.insert(2);failAfter=point;bool failed=false;
    try{l.insert(3,false);}catch(const std::bad_alloc&){failed=true;}failAfter=-1;
    if(failed){CHECK(Order(l,{1,2}));l.insert(3,false);}
    CHECK(Order(l,{3,1,2}));l.insert(1,false);CHECK(Order(l,{3,1,2}));l.touch(3);CHECK(Order(l,{1,2,3}));
  }CHECK(live==before);}
  unsigned injected=0,successful=0;
  for(long point=0;point<8;++point){const auto before=live;{
    dxvk::lru_list<int> l;l.insert(1);l.insert(2);failAfter=point;bool failed=false;
    try{l.insert(3);}catch(const std::bad_alloc&){failed=true;++injected;}failAfter=-1;
    if(failed){CHECK(Order(l,{1,2}));l.remove(3);CHECK(Order(l,{1,2}));l.insert(3);}else ++successful;
    CHECK(Order(l,{1,2,3}));l.remove(3);l.remove(1);l.remove(2);CHECK(Order(l,{}));
  }CHECK(live==before);}
  CHECK(injected>=2 && successful>0);
  {
    dxvk::lru_list<int> l;l.insert(1);l.insert(2);l.insert(3);const auto count=allocations;
    failAfter=0;bool failed=false;try{l.insert(1);l.touch(2);l.touch(2);l.touch(99);}catch(const std::bad_alloc&){failed=true;}failAfter=-1;
    CHECK(!failed);CHECK(allocations==count);CHECK(Order(l,{3,1,2}));
    if(!failed){auto i=l.remove(l.leastRecentlyUsedIter());CHECK(*i==1);i=l.remove(i);CHECK(*i==2);l.remove(i);CHECK(Order(l,{}));}
  }
  CHECK(live==0);std::printf("checks=%u failures=%u injected=%u successful=%u live=%ld\n",checks,failures,injected,successful,live);return failures?1:0;
}
