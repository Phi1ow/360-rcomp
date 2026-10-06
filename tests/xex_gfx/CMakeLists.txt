# Synthetic graphics XEX through the game shell (app/): recompiled PPC code
# -> kernel memory/video exports -> Xenos GPU -> Vulkan -> screen.
#   -DRCOMP_ROOT=<repo> -DXEX_GEN_DIR=<XenonRecomp out dir> -DRCOMP_XEX=<file.xex>
# (run gpu/xenos/rexglue/prepare.sh and tools/cpu_pipeline.sh first)
cmake_minimum_required(VERSION 3.20)
project(rcomp_xex_gfx_tests C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED RCOMP_XENONRECOMP_SRC)
  set(RCOMP_XENONRECOMP_SRC ${RCOMP_ROOT}/build/cpu-xenonrecomp-src)
endif()
set(XENON ${RCOMP_XENONRECOMP_SRC})
set(RCOMP_PLATFORM_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RCOMP_ROOT}/platform ${CMAKE_BINARY_DIR}/platform)
set(RCOMP_XENONRECOMP ${XENON} CACHE PATH "" FORCE)
set(RCOMP_RUNTIME_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RCOMP_ROOT}/runtime ${CMAKE_BINARY_DIR}/runtime)
add_subdirectory(${RCOMP_ROOT}/gpu/xenos/rexglue ${CMAKE_BINARY_DIR}/xenos)
add_subdirectory(${RCOMP_ROOT}/app ${CMAKE_BINARY_DIR}/app)

set(GEN_OPTIONS -march=x86-64-v3 -ffp-contract=on -O2
  "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h" -Wno-unused-label -Wno-unused-variable)
file(GLOB GEN_SOURCES ${XEX_GEN_DIR}/*.cpp)
add_library(rcomp_xex_gfx_generated STATIC ${GEN_SOURCES}
  ${RCOMP_ROOT}/cpu/runtime/indirect.cpp ${RCOMP_ROOT}/cpu/runtime/func_table.cpp
  ${RCOMP_ROOT}/cpu/runtime/guest_access.cpp)
target_include_directories(rcomp_xex_gfx_generated PUBLIC ${XEX_GEN_DIR} ${RCOMP_ROOT}/include ${XENON}/thirdparty/simde)
target_compile_options(rcomp_xex_gfx_generated PRIVATE ${GEN_OPTIONS})
target_link_libraries(rcomp_xex_gfx_generated PUBLIC rcomp_runtime rcomp_platform)

add_library(rcomp_xex_gfx_selftest STATIC xex_gfx_runner.cpp)
target_compile_definitions(rcomp_xex_gfx_selftest PRIVATE RCOMP_HARNESS_LIBRARY)
target_compile_options(rcomp_xex_gfx_selftest PRIVATE -march=x86-64-v3)
target_link_libraries(rcomp_xex_gfx_selftest PUBLIC rcomp_app rcomp_xex_gfx_generated)
if(NOT PS5)
  add_executable(rcomp_xex_gfx_tests xex_gfx_runner.cpp)
  target_compile_options(rcomp_xex_gfx_tests PRIVATE -march=x86-64-v3)
  target_compile_definitions(rcomp_xex_gfx_tests PRIVATE RCOMP_XEX_PATH="${RCOMP_XEX}")
  target_link_libraries(rcomp_xex_gfx_tests PRIVATE rcomp_app rcomp_xex_gfx_generated)
endif()
