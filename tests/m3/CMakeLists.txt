# M3 program tests: XenonRecomp output of fixtures/ppc/m3 + guest runtime.
#   -DRCOMP_ROOT=<repo> -DCORPUS_DIR=<build_corpus.py out> -DCASES_DIR=<gen_harness.py out>
cmake_minimum_required(VERSION 3.20)
project(rcomp_m3_tests C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
  # Title code: no exceptions/RTTI (the title links libc++ statically but
  # registers no exception handling of its own).
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

set(RCOMP_M3_OPTIONS ${RCOMP_ISA_FLAGS} -O2
  "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h"
  -Wno-unused-label -Wno-unused-variable -Wno-builtin-macro-redefined)
set(RCOMP_M3_INCLUDES ${RCOMP_ROOT}/include ${RCOMP_ROOT}/tests/cpu ${XENON}/XenonUtils ${XENON}/thirdparty/simde)

# Generated program + import bindings + CPU runtime pieces.
add_library(rcomp_m3_generated STATIC ${GENERATED_SOURCES} ${CASE_SOURCES}
  ${RCOMP_ROOT}/cpu/runtime/indirect.cpp
  ${RCOMP_ROOT}/cpu/runtime/func_table.cpp
  ${RCOMP_ROOT}/cpu/runtime/guest_access.cpp)
target_include_directories(rcomp_m3_generated PUBLIC ${RCOMP_M3_INCLUDES})
target_compile_options(rcomp_m3_generated PRIVATE ${RCOMP_M3_OPTIONS})
target_link_libraries(rcomp_m3_generated PUBLIC rcomp_runtime rcomp_platform)

# Runner as a library for the PS5 test title (rcomp_m3_selftest).
add_library(rcomp_m3_selftest STATIC ${CMAKE_CURRENT_SOURCE_DIR}/m3_runner.cpp)
target_compile_definitions(rcomp_m3_selftest PRIVATE RCOMP_HARNESS_LIBRARY)
target_compile_options(rcomp_m3_selftest PRIVATE ${RCOMP_M3_OPTIONS})
target_link_libraries(rcomp_m3_selftest PUBLIC rcomp_m3_generated)

add_executable(rcomp_m3_tests ${CMAKE_CURRENT_SOURCE_DIR}/m3_runner.cpp)
target_compile_options(rcomp_m3_tests PRIVATE ${RCOMP_M3_OPTIONS})
target_compile_definitions(rcomp_m3_tests PRIVATE
  RCOMP_M3_FIXTURE_DIR="${RCOMP_ROOT}/fixtures/ppc/m3")
target_link_libraries(rcomp_m3_tests PRIVATE rcomp_m3_generated)
