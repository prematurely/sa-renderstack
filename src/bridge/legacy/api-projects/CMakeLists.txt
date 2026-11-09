cmake_minimum_required(VERSION 3.25)
project(sa_renderstack_api_projects LANGUAGES CXX)
enable_testing()

add_executable(api-test-check-failure tests/check_failure.cpp)
target_compile_features(api-test-check-failure PRIVATE cxx_std_23)
add_test(NAME api-test-check-failure COMMAND api-test-check-failure)
set_tests_properties(api-test-check-failure PROPERTIES WILL_FAIL TRUE)

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

# GPU tests are invoked by tools/test.ps1 with an explicit newly built backend.
# Keeping them outside unqualified CTest preserves hosted runners' GPU skip.
add_executable(api-state-runtime-tests tests/state_runtime.cpp)
target_compile_features(api-state-runtime-tests PRIVATE cxx_std_23)
target_link_libraries(api-state-runtime-tests PRIVATE api3-state-batch-lib api7_selective_journal_lib user32)

add_executable(constant-submit-benchmark tests/constant_benchmark.cpp)
target_compile_features(constant-submit-benchmark PRIVATE cxx_std_23)
target_link_libraries(constant-submit-benchmark PRIVATE api3-state-batch-lib user32 d3dcompiler)

add_executable(bridge-lifecycle-runtime-tests tests/lifecycle_runtime.cpp)
target_compile_features(bridge-lifecycle-runtime-tests PRIVATE cxx_std_23)
target_link_libraries(bridge-lifecycle-runtime-tests PRIVATE user32)

add_executable(bridge-iat-hook-tests ../tests/ProperShadersIatHookTests.cpp)
target_compile_features(bridge-iat-hook-tests PRIVATE cxx_std_23)
target_include_directories(bridge-iat-hook-tests PRIVATE ..)
add_test(NAME bridge-iat-hook-tests COMMAND bridge-iat-hook-tests)
set_tests_properties(bridge-iat-hook-tests PROPERTIES TIMEOUT 30)

add_executable(bridge-code-transaction-tests tests/code_patch_tests.cpp)
target_compile_features(bridge-code-transaction-tests PRIVATE cxx_std_23)
target_include_directories(bridge-code-transaction-tests PRIVATE ..)
add_test(NAME bridge-code-transaction-tests COMMAND bridge-code-transaction-tests)
set_tests_properties(bridge-code-transaction-tests PROPERTIES TIMEOUT 30)

add_executable(bridge-log-failure-tests tests/log_failure_tests.cpp)
target_compile_features(bridge-log-failure-tests PRIVATE cxx_std_23)
add_test(NAME bridge-log-failure-tests COMMAND bridge-log-failure-tests)

add_executable(proper-shaders-fading-runtime-tests
  tests/fading_runtime.cpp ../ProperShadersStateRing.cpp)
target_compile_features(proper-shaders-fading-runtime-tests PRIVATE cxx_std_23)
target_compile_definitions(proper-shaders-fading-runtime-tests PRIVATE NOMINMAX)
target_link_libraries(proper-shaders-fading-runtime-tests PRIVATE user32)
add_test(NAME proper-shaders-fading-runtime-tests COMMAND proper-shaders-fading-runtime-tests)
set_tests_properties(proper-shaders-fading-runtime-tests PROPERTIES TIMEOUT 30)

add_executable(measurement-core-tests tests/measurement_tests.cpp)
target_compile_features(measurement-core-tests PRIVATE cxx_std_17)
add_test(NAME measurement-core-tests COMMAND measurement-core-tests)
