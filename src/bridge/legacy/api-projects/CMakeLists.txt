cmake_minimum_required(VERSION 3.25)
project(sa_renderstack_api_projects LANGUAGES CXX)
enable_testing()

# This aggregate is owned by the Bridge main program. Child projects consume
# the parent-owned SDK/backend paths and are not standalone products.
set(SA_RENDERSTACK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." CACHE INTERNAL "SA RenderStack root")
set(SA_RENDERSTACK_SDK_INCLUDE "${SA_RENDERSTACK_ROOT}/sdk/include" CACHE INTERNAL "SA RenderStack SDK include")
set(SA_RENDERSTACK_DXVK_INCLUDE "${SA_RENDERSTACK_ROOT}/backend/dxvk/include" CACHE INTERNAL "SA RenderStack DXVK include")
set(SA_RENDERSTACK_VULKAN_INCLUDE "${SA_RENDERSTACK_ROOT}/backend/dxvk/include/vulkan/include" CACHE INTERNAL "SA RenderStack Vulkan include")

# Each child remains independently configurable; this aggregate is only a
# convenience for building the seven demos and their contract tests together.
foreach(api IN ITEMS
    api1-status
    api2-vulkan-pass
    api3-state-batch
    api4-state-journal
    api5-effect-batch
    api6-state-draw
    api7-selective-journal)
  add_subdirectory(${api})
endforeach()
