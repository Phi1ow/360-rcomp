# CPU corpus test binary (Agent 6 harness + Agent 1 generated code).
# Configure with:
#   -DRCOMP_ROOT=<repo> -DCORPUS_DIR=<build_corpus.py out> -DCASES_DIR=<gen_harness.py out>
# Host: default toolchain (clang). PS5: -DCMAKE_TOOLCHAIN_FILE=<ps5 toolchain>.
cmake_minimum_required(VERSION 3.20)
project(rcomp_cpu_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "XenonRecomp output requires Clang")
endif()

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
# ISA contract (docs/ARCHITECTURE.md): x86-64-v3 (AVX2, FMA, BMI2, MOVBE) is
# the documented floor for both host and PS5 (Zen 2). Never -march=native.
# -ffp-contract=on is explicit because fmadd-family instructions are emitted as
# `a * b + c` and only contraction to FMA reproduces PowerPC's single rounding.
set(RCOMP_ISA_FLAGS "-march=x86-64-v3;-ffp-contract=on" CACHE STRING "ISA flags for generated code")

file(GLOB GENERATED_SOURCES ${CORPUS_DIR}/*/*/gen/*.cpp)
file(GLOB CASE_SOURCES ${CASES_DIR}/cases_*.cpp)
list(LENGTH GENERATED_SOURCES n_gen)
message(STATUS "generated translation units: ${n_gen}")

# Platform layer (guest memory backend chosen per target: posix on the host,
# sceKernel* direct memory on PS5; see platform/ps5/README.md).
set(RCOMP_PLATFORM_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RCOMP_ROOT}/platform ${CMAKE_BINARY_DIR}/platform)

set(RCOMP_CORPUS_OPTIONS
  ${RCOMP_ISA_FLAGS} -O2
  "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h"
  -Wno-unused-label -Wno-unused-variable -Wno-builtin-macro-redefined)
if(PS5)
  list(APPEND RCOMP_CORPUS_OPTIONS -fno-exceptions -fno-rtti)
endif()
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-include;${CASES_DIR}/generated_decls.h")

# Generated corpus + case tables + CPU runtime pieces.
add_library(rcomp_cpu_generated STATIC
  ${GENERATED_SOURCES}
  ${CASE_SOURCES}
  ${RCOMP_ROOT}/cpu/runtime/indirect.cpp
  ${RCOMP_ROOT}/cpu/runtime/func_table.cpp
  ${RCOMP_ROOT}/cpu/runtime/guest_access.cpp
  ${RCOMP_ROOT}/cpu/runtime/isa_check.cpp)
target_include_directories(rcomp_cpu_generated PUBLIC
  ${RCOMP_ROOT}/include ${CMAKE_CURRENT_SOURCE_DIR}
  ${XENON}/XenonUtils ${XENON}/thirdparty/simde)
target_compile_options(rcomp_cpu_generated PRIVATE ${RCOMP_CORPUS_OPTIONS})
target_link_libraries(rcomp_cpu_generated PUBLIC rcomp_platform)

# Runner as a library (exports rcomp_cpu_selftest) for the PS5 test title.
add_library(rcomp_cpu_selftest STATIC ${CMAKE_CURRENT_SOURCE_DIR}/harness_main.cpp)
target_compile_definitions(rcomp_cpu_selftest PRIVATE RCOMP_HARNESS_LIBRARY)
target_compile_options(rcomp_cpu_selftest PRIVATE ${RCOMP_CORPUS_OPTIONS})
target_link_libraries(rcomp_cpu_selftest PUBLIC rcomp_cpu_generated)

# Standalone runner (host; on PS5 only a payload-style link check).
add_executable(rcomp_cpu_tests ${CMAKE_CURRENT_SOURCE_DIR}/harness_main.cpp)
target_compile_options(rcomp_cpu_tests PRIVATE ${RCOMP_CORPUS_OPTIONS})
target_link_libraries(rcomp_cpu_tests PRIVATE rcomp_cpu_generated)
