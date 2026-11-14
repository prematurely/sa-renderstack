#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// Test-only interception of the real loader boundary. Every call reaches
// Vulkan. Timed benchmark runs do not install this observer.
namespace command_observer {
inline PFN_vkGetInstanceProcAddr instanceProc;
inline PFN_vkGetDeviceProcAddr deviceProc;
inline PFN_vkAllocateCommandBuffers allocate;
inline PFN_vkBeginCommandBuffer begin;
inline PFN_vkResetCommandPool reset;
inline PFN_vkDestroyCommandPool destroy;
inline decltype(&GetProcAddress) getProc;
inline std::mutex mutex;
struct Pool { unsigned allocated=0; std::unordered_set<VkCommandBuffer> used; };
inline std::unordered_map<VkCommandPool,Pool> pools;
inline std::unordered_map<VkCommandBuffer,VkCommandPool> owners;
inline unsigned calls=0, buffers=0, batches=0, resets=0, destroyed=0, maxSpare=0, errors=0;
inline void (*beforeBegin)(VkCommandBuffer) = nullptr;
inline void (*beforeAllocate)() = nullptr;
inline VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice d,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
  if(beforeAllocate)beforeAllocate();
  auto result=allocate(d,info,out);
  if(result==VK_SUCCESS && info->level==VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
    std::lock_guard<std::mutex> lock(mutex);++calls;buffers+=info->commandBufferCount;batches+=info->commandBufferCount>1;
    auto& pool=pools[info->commandPool];pool.allocated+=info->commandBufferCount;
    for(unsigned i=0;i<info->commandBufferCount;++i)owners[out[i]]=info->commandPool;
  }
  return result;
}
inline VKAPI_ATTR VkResult VKAPI_CALL Begin(VkCommandBuffer buffer,const VkCommandBufferBeginInfo* info) {
  if(beforeBegin)beforeBegin(buffer);
  auto result=begin(buffer,info);
  std::lock_guard<std::mutex> lock(mutex);
  auto it=owners.find(buffer);if(it!=owners.end()){auto& pool=pools[it->second];pool.used.insert(buffer);maxSpare=std::max(maxSpare,pool.allocated-unsigned(pool.used.size()));}
  errors+=result!=VK_SUCCESS;return result;
}
inline VKAPI_ATTR VkResult VKAPI_CALL Reset(VkDevice d,VkCommandPool pool,VkCommandPoolResetFlags flags) {
  auto result=reset(d,pool,flags);std::lock_guard<std::mutex> lock(mutex);++resets;errors+=result!=VK_SUCCESS;return result;
}
inline VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice d,VkCommandPool pool,const VkAllocationCallbacks* callbacks) {
  destroy(d,pool,callbacks);std::lock_guard<std::mutex> lock(mutex);++destroyed;
  for(auto i=owners.begin();i!=owners.end();)if(i->second==pool)i=owners.erase(i);else ++i;
  pools.erase(pool);
}
inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceProc(VkDevice d,const char* name) {
  auto p=deviceProc(d,name);
  if(!std::strcmp(name,"vkAllocateCommandBuffers")){allocate=reinterpret_cast<PFN_vkAllocateCommandBuffers>(p);return reinterpret_cast<PFN_vkVoidFunction>(&Allocate);}
  if(!std::strcmp(name,"vkBeginCommandBuffer")){begin=reinterpret_cast<PFN_vkBeginCommandBuffer>(p);return reinterpret_cast<PFN_vkVoidFunction>(&Begin);}
  if(!std::strcmp(name,"vkResetCommandPool")){reset=reinterpret_cast<PFN_vkResetCommandPool>(p);return reinterpret_cast<PFN_vkVoidFunction>(&Reset);}
  if(!std::strcmp(name,"vkDestroyCommandPool")){destroy=reinterpret_cast<PFN_vkDestroyCommandPool>(p);return reinterpret_cast<PFN_vkVoidFunction>(&Destroy);}
  return p;
}
inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL InstanceProc(VkInstance i,const char* name) {
  auto p=instanceProc(i,name);
  if(!std::strcmp(name,"vkGetDeviceProcAddr")){deviceProc=reinterpret_cast<PFN_vkGetDeviceProcAddr>(p);return reinterpret_cast<PFN_vkVoidFunction>(&DeviceProc);}return p;
}
inline FARPROC WINAPI GetProc(HMODULE m,LPCSTR name) {
  auto p=getProc(m,name);
  if(reinterpret_cast<uintptr_t>(name)>65535 && !std::strcmp(name,"vkGetInstanceProcAddr")) {
    instanceProc=reinterpret_cast<PFN_vkGetInstanceProcAddr>(p);return reinterpret_cast<FARPROC>(&InstanceProc);
  }
  return p;
}
}
