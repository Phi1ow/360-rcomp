cmake_minimum_required(VERSION 3.20)
project(rcomp_title_entry_diagnostic LANGUAGES C CXX)
if(CMAKE_CROSSCOMPILING OR PS5)
  message(FATAL_ERROR "Entry diagnostics are host-only and cannot produce a PS5 title")
endif()
if(NOT RCOMP_DIAGNOSTIC_ONLY)
  message(FATAL_ERROR "Requires RCOMP_DIAGNOSTIC_ONLY=ON; this is not the M6 production build")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(RCOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
foreach(path XEX_GEN_DIR RCOMP_XENONRECOMP_SRC)
  if(NOT IS_DIRECTORY "${${path}}")
    message(FATAL_ERROR "Missing ${path}")
  endif()
endforeach()
set(RCOMP_PLATFORM_TESTS OFF CACHE BOOL "" FORCE)
set(RCOMP_RUNTIME_TESTS OFF CACHE BOOL "" FORCE)
set(RCOMP_XENONRECOMP "${RCOMP_XENONRECOMP_SRC}" CACHE PATH "" FORCE)
add_subdirectory(${RCOMP_ROOT}/platform ${CMAKE_BINARY_DIR}/platform)
add_subdirectory(${RCOMP_ROOT}/runtime ${CMAKE_BINARY_DIR}/runtime)
file(GLOB generated "${XEX_GEN_DIR}/*.cpp")
if(NOT generated)
  message(FATAL_ERROR "No original generated C++ files")
endif()
add_library(rcomp_diagnostic_generated STATIC ${generated}
    ${RCOMP_ROOT}/cpu/runtime/indirect.cpp
    ${RCOMP_ROOT}/cpu/runtime/func_table.cpp
    ${RCOMP_ROOT}/cpu/runtime/guest_access.cpp)
target_include_directories(rcomp_diagnostic_generated PUBLIC
    ${XEX_GEN_DIR} ${RCOMP_ROOT}/include ${RCOMP_XENONRECOMP_SRC}/thirdparty/simde)
target_compile_definitions(rcomp_diagnostic_generated PRIVATE
    RCOMP_VIRTUAL_GUEST_ACCESS=1 RCOMP_CHECKED_GUEST_ACCESS=1)
target_compile_options(rcomp_diagnostic_generated PRIVATE -O0 -g0
    -march=x86-64-v3 -ffp-contract=on -Wno-unused-label -Wno-unused-variable
    "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h")
target_link_libraries(rcomp_diagnostic_generated PUBLIC rcomp_runtime rcomp_platform)
add_executable(rcomp_title_entry_diagnostic main.cpp
    ${RCOMP_ROOT}/app/src/title_runtime.cpp
    ${RCOMP_ROOT}/tests/xex/TESTDOUBLE_cpu_only_gpu.cpp)
target_compile_options(rcomp_title_entry_diagnostic PRIVATE -O1 -g -march=x86-64-v3 -ffp-contract=on)
target_include_directories(rcomp_title_entry_diagnostic PRIVATE ${RCOMP_ROOT}/app/include)
target_link_libraries(rcomp_title_entry_diagnostic PRIVATE rcomp_diagnostic_generated
    rcomp_runtime_video rcomp_runtime rcomp_platform_input)
