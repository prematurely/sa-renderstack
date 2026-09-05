cmake_minimum_required(VERSION 3.25)
project(api4_state_journal LANGUAGES CXX)
if (NOT DEFINED SA_RENDERSTACK_ROOT)
  message(FATAL_ERROR "api4-state-journal must be added through the Bridge API aggregate")
endif()
add_library(api4-state-journal-lib STATIC src/api4_journal_session.cpp)
target_compile_features(api4-state-journal-lib PUBLIC cxx_std_23)
target_compile_definitions(api4-state-journal-lib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(api4-state-journal-lib PUBLIC include
  "${SA_RENDERSTACK_SDK_INCLUDE}"
  "${SA_RENDERSTACK_DXVK_INCLUDE}"
  "${SA_RENDERSTACK_VULKAN_INCLUDE}")
add_executable(api4-state-journal main.cpp)
target_compile_features(api4-state-journal PRIVATE cxx_std_23)
target_compile_definitions(api4-state-journal PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(api4-state-journal PRIVATE
  include
  "${SA_RENDERSTACK_SDK_INCLUDE}"
  "${SA_RENDERSTACK_DXVK_INCLUDE}"
  "${SA_RENDERSTACK_VULKAN_INCLUDE}")
target_link_libraries(api4-state-journal PRIVATE d3d9 user32)
target_link_libraries(api4-state-journal PRIVATE api4-state-journal-lib)
enable_testing()
add_executable(api4-state-journal-unit-tests tests/test_session.cpp)
target_link_libraries(api4-state-journal-unit-tests PRIVATE api4-state-journal-lib)
target_include_directories(api4-state-journal-unit-tests PRIVATE include)
add_test(NAME api4-state-journal-unit-tests COMMAND api4-state-journal-unit-tests)
