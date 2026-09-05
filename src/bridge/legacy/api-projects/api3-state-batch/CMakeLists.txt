cmake_minimum_required(VERSION 3.25)
project(api3_state_batch LANGUAGES CXX)
if (NOT DEFINED SA_RENDERSTACK_ROOT)
  message(FATAL_ERROR "api3-state-batch must be added through the Bridge API aggregate")
endif()
add_library(api3-state-batch-lib STATIC src/api3_state_batch.cpp)
target_compile_features(api3-state-batch-lib PUBLIC cxx_std_23)
target_compile_definitions(api3-state-batch-lib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(api3-state-batch-lib PUBLIC include
  "${SA_RENDERSTACK_SDK_INCLUDE}"
  "${SA_RENDERSTACK_DXVK_INCLUDE}"
  "${SA_RENDERSTACK_VULKAN_INCLUDE}")
add_executable(api3-state-batch main.cpp)
target_compile_features(api3-state-batch PRIVATE cxx_std_23)
target_compile_definitions(api3-state-batch PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(api3-state-batch PRIVATE
  include
  "${SA_RENDERSTACK_SDK_INCLUDE}"
  "${SA_RENDERSTACK_DXVK_INCLUDE}"
  "${SA_RENDERSTACK_VULKAN_INCLUDE}")
target_link_libraries(api3-state-batch PRIVATE d3d9 user32)
target_link_libraries(api3-state-batch PRIVATE api3-state-batch-lib)
enable_testing()
add_executable(api3-state-batch-unit-tests tests/test_builder.cpp)
target_link_libraries(api3-state-batch-unit-tests PRIVATE api3-state-batch-lib)
target_include_directories(api3-state-batch-unit-tests PRIVATE include)
add_test(NAME api3-state-batch-unit-tests COMMAND api3-state-batch-unit-tests)
