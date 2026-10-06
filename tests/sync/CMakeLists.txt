# Sync test: XenonRecomp output of fixtures/ppc/sync on concurrent host threads.
cmake_minimum_required(VERSION 3.20)
project(rcomp_sync_tests C CXX)
set(CMAKE_CXX_STANDARD 17)
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
set(RCOMP_PLATFORM_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RCOMP_ROOT}/platform ${CMAKE_BINARY_DIR}/platform)
find_package(Threads REQUIRED)
file(GLOB GENERATED_SOURCES ${CORPUS_DIR}/*/*/gen/*.cpp)
file(GLOB CASE_SOURCES ${CASES_DIR}/cases_*.cpp)
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-include;${CASES_DIR}/generated_decls.h")
add_executable(rcomp_sync_tests ${GENERATED_SOURCES} ${CASE_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/sync_runner.cpp
  ${RCOMP_ROOT}/cpu/runtime/indirect.cpp ${RCOMP_ROOT}/cpu/runtime/func_table.cpp)
target_include_directories(rcomp_sync_tests PRIVATE
  ${RCOMP_ROOT}/include ${RCOMP_ROOT}/tests/cpu ${XENON}/XenonUtils ${XENON}/thirdparty/simde)
target_compile_options(rcomp_sync_tests PRIVATE ${RCOMP_ISA_FLAGS} -O2
  "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h"
  -Wno-unused-label -Wno-unused-variable -Wno-builtin-macro-redefined)
target_link_libraries(rcomp_sync_tests PRIVATE rcomp_platform Threads::Threads)
