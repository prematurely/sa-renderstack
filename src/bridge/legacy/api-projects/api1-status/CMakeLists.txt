cmake_minimum_required(VERSION 3.21)
project(sa_api1_status LANGUAGES CXX)

if (NOT DEFINED SA_RENDERSTACK_ROOT)
  message(FATAL_ERROR "api1-status must be added through the Bridge API aggregate")
endif()

if (NOT WIN32)
  message(FATAL_ERROR "API1 probe requires Windows D3D9")
endif()

add_library(sa-api1-status-lib STATIC src/status_client.cpp)
target_compile_features(sa-api1-status-lib PUBLIC cxx_std_23)
target_include_directories(sa-api1-status-lib PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${SA_RENDERSTACK_SDK_INCLUDE}"
  "${SA_RENDERSTACK_VULKAN_INCLUDE}")
target_compile_definitions(sa-api1-status-lib PUBLIC UNICODE _UNICODE NOMINMAX)
add_executable(sa-api1-status main.cpp)
target_link_libraries(sa-api1-status PRIVATE sa-api1-status-lib d3d9 user32)
add_executable(sa-api1-status-tests tests/status_client_tests.cpp)
target_link_libraries(sa-api1-status-tests PRIVATE sa-api1-status-lib)
enable_testing()
add_test(NAME api1-status-unit COMMAND sa-api1-status-tests)
