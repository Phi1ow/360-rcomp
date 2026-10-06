# Xenos command processor (rexglue, gpu/xenos/rexglue) over R-comp guest
# memory, driven through the register window like a title drives it.
#   cmake -S tests/xenos_cp -B build/tests-xenos-cp -G Ninja -DRCOMP_ROOT=$PWD
# (run gpu/xenos/rexglue/prepare.sh first)
cmake_minimum_required(VERSION 3.20)
project(rcomp_xenos_cp_tests C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(RCOMP_PLATFORM_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RCOMP_ROOT}/platform ${CMAKE_BINARY_DIR}/platform)
add_subdirectory(${RCOMP_ROOT}/gpu/xenos/rexglue ${CMAKE_BINARY_DIR}/xenos)
set(RCOMP_RUNTIME_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RCOMP_ROOT}/runtime ${CMAKE_BINARY_DIR}/runtime)
# Guest address -> function table (the interrupt callback), from cpu/.
add_library(rcomp_xenos_video_functable STATIC ${RCOMP_ROOT}/cpu/runtime/func_table.cpp)
target_include_directories(rcomp_xenos_video_functable PUBLIC ${RCOMP_ROOT}/include)
if(NOT PS5)  # the host runner (fork/exec for the fatal-path child)
  add_executable(rcomp_xenos_cp_tests test_command_processor.cpp)
  target_compile_options(rcomp_xenos_cp_tests PRIVATE -march=x86-64-v3)
  target_link_libraries(rcomp_xenos_cp_tests PRIVATE rcomp_xenos_host rcomp_platform)
endif()

# Library form for the PS5 test title (platform/ps5/tools/build_title.sh --xenos-cp).
add_library(rcomp_xenos_cp_selftest STATIC test_command_processor.cpp)
target_compile_definitions(rcomp_xenos_cp_selftest PRIVATE RCOMP_HARNESS_LIBRARY)
target_compile_options(rcomp_xenos_cp_selftest PRIVATE -march=x86-64-v3)
target_link_libraries(rcomp_xenos_cp_selftest PUBLIC rcomp_xenos_host rcomp_platform)

# Kernel video exports + physical memory over the command processor
# (runtime/src/hle_xboxkrnl_video.cpp, hle_xboxkrnl_memory.cpp).
set(XV_LIBS rcomp_runtime_video rcomp_runtime rcomp_xenos_host rcomp_xenos_video_functable rcomp_platform)
if(NOT PS5)
  add_executable(rcomp_xenos_video_tests test_video.cpp)
  target_compile_options(rcomp_xenos_video_tests PRIVATE -march=x86-64-v3)
  target_link_libraries(rcomp_xenos_video_tests PRIVATE ${XV_LIBS})
endif()
add_library(rcomp_xenos_video_selftest STATIC test_video.cpp)
target_compile_definitions(rcomp_xenos_video_selftest PRIVATE RCOMP_HARNESS_LIBRARY)
target_compile_options(rcomp_xenos_video_selftest PRIVATE -march=x86-64-v3)
target_link_libraries(rcomp_xenos_video_selftest PUBLIC ${XV_LIBS})

# Real Vulkan backend. Host: the system Vulkan device (lavapipe in CI). PS5:
# library for the PS5_Vulkan title (gpu/vulkan/ps5/build-ps5.sh,
# RCOMP_XENOS_VK_BUILD), which links the driver.
if(RCOMP_XENOS_VULKAN)
  if(NOT PS5)
    add_executable(rcomp_xenos_vulkan_tests test_vulkan.cpp)
    target_compile_options(rcomp_xenos_vulkan_tests PRIVATE -march=x86-64-v3)
    target_link_libraries(rcomp_xenos_vulkan_tests PRIVATE rcomp_xenos_vulkan rcomp_platform)
  endif()
  add_library(rcomp_xenos_vulkan_selftest STATIC test_vulkan.cpp)
  target_compile_definitions(rcomp_xenos_vulkan_selftest PRIVATE RCOMP_HARNESS_LIBRARY)
  target_compile_options(rcomp_xenos_vulkan_selftest PRIVATE -march=x86-64-v3)
  target_link_libraries(rcomp_xenos_vulkan_selftest PUBLIC rcomp_xenos_vulkan rcomp_platform)
endif()
