# M5 integrated graphics slice: XenonRecomp output of fixtures/ppc/m5 + guest
# runtime + gfx HLE (gpu/hle) + Vulkan renderer.
#   -DRCOMP_ROOT=<repo> -DCORPUS_DIR=<build_corpus.py out> -DCASES_DIR=<gen_harness.py out>
# Host: links the system Vulkan loader (HOST-ONLY evidence). PS5: the archives
# (rcomp_m5_selftest, rcomp_m5_generated, rcomp_gfx) are linked into an eboot
# together with PS5_Vulkan by gpu/vulkan/ps5/build-ps5.sh.
cmake_minimum_required(VERSION 3.20)
project(rcomp_m5_tests C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# Generated code must see the *patched* XenonUtils/ppc_context.h
# (cpu/tools/prepare_xenonrecomp.sh -> build/cpu-xenonrecomp-src).
if(NOT DEFINED RCOMP_XENONRECOMP_SRC)
  if(EXISTS ${RCOMP_ROOT}/build/cpu-xenonrecomp-src/XenonUtils/ppc_context.h)
    set(RCOMP_XENONRECOMP_SRC ${RCOMP_ROOT}/build/cpu-xenonrecomp-src)
  else()
    set(RCOMP_XENONRECOMP_SRC ${RCOMP_ROOT}/third_party/XenonRecomp)
  endif()
endif()
set(XENON ${RCOMP_XENONRECOMP_SRC})
set(RCOMP_ISA_FLAGS "-march=x86-64-v3;-ffp-contract=on" CACHE STRING "")
if(PS5)
  string(APPEND CMAKE_CXX_FLAGS " -fno-exceptions -fno-rtti")
endif()

set(RCOMP_PLATFORM_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RCOMP_ROOT}/platform ${CMAKE_BINARY_DIR}/platform)
set(RCOMP_XENONRECOMP ${XENON} CACHE PATH "" FORCE)
set(RCOMP_RUNTIME_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RCOMP_ROOT}/runtime ${CMAKE_BINARY_DIR}/runtime)

file(GLOB GENERATED_SOURCES ${CORPUS_DIR}/*/*/gen/*.cpp)
file(GLOB CASE_SOURCES ${CASES_DIR}/cases_*.cpp)
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-include;${CASES_DIR}/generated_decls.h")
set(M5_OPTIONS ${RCOMP_ISA_FLAGS} -O2
  "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h"
  -Wno-unused-label -Wno-unused-variable -Wno-builtin-macro-redefined)
set(M5_INCLUDES ${RCOMP_ROOT}/include ${RCOMP_ROOT}/tests/cpu ${XENON}/XenonUtils ${XENON}/thirdparty/simde)

# Vulkan headers: system on the host; the driver snapshot's on PS5.
set(RCOMP_VK_INCLUDE "" CACHE PATH "Vulkan headers (PS5 build)")

# The loader shim is not part of rcomp_gfx: the PS5 eboot link provides its
# own copy (gpu/vulkan/ps5/build-ps5.sh), the host executable links it below.
add_library(rcomp_gfx STATIC
  ${RCOMP_ROOT}/gpu/hle/gfx_vk.c
  ${RCOMP_ROOT}/gpu/hle/gfx_hle.cpp)
target_include_directories(rcomp_gfx PUBLIC ${M5_INCLUDES}
  ${RCOMP_ROOT}/gpu/vulkan/common ${RCOMP_ROOT}/gpu/vulkan/draw_test/generated)
if(RCOMP_VK_INCLUDE)
  target_include_directories(rcomp_gfx PUBLIC ${RCOMP_VK_INCLUDE})
endif()

add_library(rcomp_m5_generated STATIC ${GENERATED_SOURCES} ${CASE_SOURCES}
  ${RCOMP_ROOT}/cpu/runtime/indirect.cpp
  ${RCOMP_ROOT}/cpu/runtime/func_table.cpp
  ${RCOMP_ROOT}/cpu/runtime/guest_access.cpp)
target_include_directories(rcomp_m5_generated PUBLIC ${M5_INCLUDES})
target_compile_options(rcomp_m5_generated PRIVATE ${M5_OPTIONS})
target_link_libraries(rcomp_m5_generated PUBLIC rcomp_gfx rcomp_runtime rcomp_platform)

add_library(rcomp_m5_selftest STATIC ${CMAKE_CURRENT_SOURCE_DIR}/m5_runner.cpp)
target_compile_definitions(rcomp_m5_selftest PRIVATE RCOMP_HARNESS_LIBRARY)
target_compile_options(rcomp_m5_selftest PRIVATE ${M5_OPTIONS})
target_link_libraries(rcomp_m5_selftest PUBLIC rcomp_m5_generated)

if(NOT PS5)
  find_library(VULKAN_LIB vulkan REQUIRED)
  add_library(rcomp_vk_loader STATIC ${RCOMP_ROOT}/gpu/vulkan/common/rcvk_loader.c)
  target_include_directories(rcomp_vk_loader PUBLIC ${RCOMP_ROOT}/gpu/vulkan/common)
  target_link_libraries(rcomp_gfx PUBLIC rcomp_vk_loader)
  add_executable(rcomp_m5_tests ${CMAKE_CURRENT_SOURCE_DIR}/m5_runner.cpp)
  target_compile_options(rcomp_m5_tests PRIVATE ${M5_OPTIONS})
  target_link_libraries(rcomp_m5_tests PRIVATE rcomp_m5_generated rcomp_vk_loader ${VULKAN_LIB} m)
endif()
