cmake_minimum_required(VERSION 3.21)
project(sa_api2_vulkan_pass LANGUAGES CXX)

if (NOT DEFINED SA_RENDERSTACK_ROOT)
  message(FATAL_ERROR "api2-vulkan-pass must be added through the Bridge API aggregate")
endif()

if (NOT WIN32)
  message(FATAL_ERROR "API2 probe requires Windows D3D9/Vulkan backend")
endif()

add_library(sa-api2-vulkan-pass-lib STATIC src/pass_registration.cpp)
target_compile_features(sa-api2-vulkan-pass-lib PUBLIC cxx_std_23)
target_include_directories(sa-api2-vulkan-pass-lib PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${SA_RENDERSTACK_SDK_INCLUDE}"
  "${SA_RENDERSTACK_VULKAN_INCLUDE}")
target_compile_definitions(sa-api2-vulkan-pass-lib PUBLIC UNICODE _UNICODE NOMINMAX)
add_executable(sa-api2-vulkan-pass main.cpp)
target_link_libraries(sa-api2-vulkan-pass PRIVATE sa-api2-vulkan-pass-lib d3d9 user32)
add_executable(sa-api2-vulkan-pass-tests tests/pass_registration_tests.cpp)
target_link_libraries(sa-api2-vulkan-pass-tests PRIVATE sa-api2-vulkan-pass-lib)
enable_testing()
add_test(NAME api2-registration-unit COMMAND sa-api2-vulkan-pass-tests)
