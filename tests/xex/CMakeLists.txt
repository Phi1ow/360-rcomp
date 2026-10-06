# Synthetic XEX title: XenonRecomp XEX/TOML-mode output + runtime XEX loader.
#   -DRCOMP_ROOT=<repo> -DXEX_GEN_DIR=<XenonRecomp out_directory_path> -DRCOMP_XEX=<file.xex>
cmake_minimum_required(VERSION 3.20)
project(rcomp_xex_tests C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED RCOMP_XENONRECOMP_SRC)
  set(RCOMP_XENONRECOMP_SRC ${RCOMP_ROOT}/build/cpu-xenonrecomp-src)
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

set(RCOMP_XEX_OPTIONS ${RCOMP_ISA_FLAGS} -O2
  "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h"
  -Wno-unused-label -Wno-unused-variable)
# The generated directory comes first: its ppc_context.h/ppc_config.h are the
# ones the generator wrote for this image.
set(RCOMP_XEX_INCLUDES ${XEX_GEN_DIR} ${RCOMP_ROOT}/include ${XENON}/thirdparty/simde)

file(GLOB XEX_GENERATED ${XEX_GEN_DIR}/*.cpp)
add_library(rcomp_xex_generated STATIC ${XEX_GENERATED}
  ${RCOMP_ROOT}/cpu/runtime/indirect.cpp
  ${RCOMP_ROOT}/cpu/runtime/func_table.cpp
  ${RCOMP_ROOT}/cpu/runtime/guest_access.cpp)
target_include_directories(rcomp_xex_generated PUBLIC ${RCOMP_XEX_INCLUDES})
target_compile_options(rcomp_xex_generated PRIVATE ${RCOMP_XEX_OPTIONS})
target_link_libraries(rcomp_xex_generated PUBLIC rcomp_runtime rcomp_platform)

option(RCOMP_XEX_BOOT_REHEARSAL "Run original PPC boot through production TitleRuntime" OFF)
option(RCOMP_XEX_PACKAGED_BOOT "Run original PPC boot using data packaged beside the XEX" OFF)
option(RCOMP_XEX_POOL_RTL "Run the original pool/Rtl PPC oracle using its packaged XEX" OFF)
option(RCOMP_XEX_MODULES "Run the original module/header/AOT PPC oracle using its packaged XEX" OFF)
option(RCOMP_XEX_CALENDAR_THREADS "Run the original calendar/thread PPC oracle using its packaged XEX" OFF)
option(RCOMP_XEX_SERVICES "Run the original XAM/RTL/IO/object PPC oracle using its packaged XEX" OFF)
option(RCOMP_XEX_KERNEL_BOOT "Run original PPC virtual kernel fields and title lifecycle" OFF)
if((RCOMP_XEX_BOOT_REHEARSAL AND RCOMP_XEX_PACKAGED_BOOT) OR
   (RCOMP_XEX_POOL_RTL AND (RCOMP_XEX_BOOT_REHEARSAL OR RCOMP_XEX_PACKAGED_BOOT)) OR
   (RCOMP_XEX_MODULES AND (RCOMP_XEX_BOOT_REHEARSAL OR RCOMP_XEX_PACKAGED_BOOT OR RCOMP_XEX_POOL_RTL)) OR
   (RCOMP_XEX_CALENDAR_THREADS AND (RCOMP_XEX_BOOT_REHEARSAL OR RCOMP_XEX_PACKAGED_BOOT OR RCOMP_XEX_POOL_RTL OR RCOMP_XEX_MODULES)) OR
   (RCOMP_XEX_SERVICES AND (RCOMP_XEX_BOOT_REHEARSAL OR RCOMP_XEX_PACKAGED_BOOT OR RCOMP_XEX_POOL_RTL OR RCOMP_XEX_MODULES OR RCOMP_XEX_CALENDAR_THREADS)) OR
   (RCOMP_XEX_KERNEL_BOOT AND (RCOMP_XEX_BOOT_REHEARSAL OR RCOMP_XEX_PACKAGED_BOOT OR RCOMP_XEX_POOL_RTL OR RCOMP_XEX_MODULES OR RCOMP_XEX_CALENDAR_THREADS OR RCOMP_XEX_SERVICES)))
  message(FATAL_ERROR "Choose one XEX runner: boot rehearsal, packaged boot, pool/Rtl, modules, calendar/threads, services or kernel-boot")
endif()
if(RCOMP_XEX_KERNEL_BOOT)
  target_compile_definitions(rcomp_xex_generated PRIVATE RCOMP_VIRTUAL_GUEST_ACCESS=1)
  add_library(rcomp_xex_selftest STATIC kernel_boot_runner.cpp TESTDOUBLE_cpu_only_gpu.cpp
    ${RCOMP_ROOT}/app/src/title_runtime.cpp)
  target_include_directories(rcomp_xex_selftest PRIVATE ${RCOMP_ROOT}/app/include)
  target_compile_options(rcomp_xex_selftest PRIVATE ${RCOMP_XEX_OPTIONS})
  target_link_libraries(rcomp_xex_selftest PUBLIC rcomp_xex_generated
    rcomp_runtime_video rcomp_runtime rcomp_platform_input)
  if(PS5)
    target_compile_definitions(rcomp_xex_selftest PRIVATE RCOMP_KERNEL_BOOT_PS5)
  else()
    add_executable(rcomp_xex_kernel_boot boot_packaged_main.cpp TESTDOUBLE_cpu_only_gpu.cpp)
    target_compile_options(rcomp_xex_kernel_boot PRIVATE ${RCOMP_XEX_OPTIONS})
    target_compile_definitions(rcomp_xex_kernel_boot PRIVATE RCOMP_XEX_PATH="${RCOMP_XEX}")
    target_link_libraries(rcomp_xex_kernel_boot PRIVATE rcomp_xex_selftest)
    enable_testing()
    add_test(NAME xex_kernel_boot COMMAND rcomp_xex_kernel_boot)
    set_tests_properties(xex_kernel_boot PROPERTIES TIMEOUT 60
      PASS_REGULAR_EXPRESSION "RCOMP-KERNEL-BOOT checks=29 pass=29 fail=0 scope=host")
  endif()
  return()
endif()
if(RCOMP_XEX_SERVICES)
  add_library(rcomp_xex_selftest STATIC services_runner.cpp TESTDOUBLE_cpu_only_gpu.cpp
    ${RCOMP_ROOT}/app/src/title_runtime.cpp)
  target_include_directories(rcomp_xex_selftest PRIVATE ${RCOMP_ROOT}/app/include)
  target_compile_options(rcomp_xex_selftest PRIVATE ${RCOMP_XEX_OPTIONS})
  target_link_libraries(rcomp_xex_selftest PUBLIC rcomp_xex_generated
    rcomp_runtime_video rcomp_runtime rcomp_platform_input)
  if(PS5)
    target_compile_definitions(rcomp_xex_selftest PRIVATE RCOMP_SERVICES_PS5)
  else()
    add_executable(rcomp_xex_services boot_packaged_main.cpp TESTDOUBLE_cpu_only_gpu.cpp)
    target_include_directories(rcomp_xex_services PRIVATE ${RCOMP_ROOT}/app/include)
    target_compile_options(rcomp_xex_services PRIVATE ${RCOMP_XEX_OPTIONS})
    target_compile_definitions(rcomp_xex_services PRIVATE RCOMP_XEX_PATH="${RCOMP_XEX}")
    target_link_libraries(rcomp_xex_services PRIVATE rcomp_xex_selftest)
    enable_testing()
    add_test(NAME xex_services COMMAND rcomp_xex_services)
    set_tests_properties(xex_services PROPERTIES TIMEOUT 60
      PASS_REGULAR_EXPRESSION "RCOMP-SERVICES checks=20 pass=20 fail=0 scope=host")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME xex_services_bad_package COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_services_package.py $<TARGET_FILE:rcomp_xex_services>
      ${RCOMP_XEX} ${CMAKE_BINARY_DIR})
    set_tests_properties(xex_services_bad_package PROPERTIES TIMEOUT 45)
  endif()
  return()
endif()
if(RCOMP_XEX_CALENDAR_THREADS)
  add_library(rcomp_xex_selftest STATIC calendar_threads_runner.cpp TESTDOUBLE_cpu_only_gpu.cpp
    ${RCOMP_ROOT}/app/src/title_runtime.cpp)
  target_include_directories(rcomp_xex_selftest PRIVATE ${RCOMP_ROOT}/app/include)
  target_compile_options(rcomp_xex_selftest PRIVATE ${RCOMP_XEX_OPTIONS})
  target_link_libraries(rcomp_xex_selftest PUBLIC rcomp_xex_generated
    rcomp_runtime_video rcomp_runtime rcomp_platform_input)
  if(PS5)
    target_compile_definitions(rcomp_xex_selftest PRIVATE RCOMP_CALENDAR_THREADS_PS5)
  else()
    add_executable(rcomp_xex_calendar_threads boot_packaged_main.cpp TESTDOUBLE_cpu_only_gpu.cpp)
    target_include_directories(rcomp_xex_calendar_threads PRIVATE ${RCOMP_ROOT}/app/include)
    target_compile_options(rcomp_xex_calendar_threads PRIVATE ${RCOMP_XEX_OPTIONS})
    target_compile_definitions(rcomp_xex_calendar_threads PRIVATE RCOMP_XEX_PATH="${RCOMP_XEX}")
    target_link_libraries(rcomp_xex_calendar_threads PRIVATE rcomp_xex_selftest)
    enable_testing()
    add_test(NAME xex_calendar_threads COMMAND rcomp_xex_calendar_threads)
    set_tests_properties(xex_calendar_threads PROPERTIES TIMEOUT 60
      PASS_REGULAR_EXPRESSION "RCOMP-CALENDAR-THREADS checks=20 pass=20 fail=0 scope=host")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME xex_calendar_oracle_fixture COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_calendar_oracle_fixture.py
      ${RCOMP_ROOT}/fixtures/xex/rcomp_calendar_oracle.json
      ${RCOMP_ROOT}/fixtures/xex/rcomp_calendar_threads.s
      ${CMAKE_CURRENT_SOURCE_DIR}/ntdll_calendar_oracle.py)
    set_tests_properties(xex_calendar_oracle_fixture PROPERTIES TIMEOUT 10
      PASS_REGULAR_EXPRESSION "CALENDAR-ORACLE checks=45")
    add_test(NAME xex_calendar_threads_bad_package COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_calendar_threads_package.py $<TARGET_FILE:rcomp_xex_calendar_threads>
      ${CMAKE_BINARY_DIR})
    set_tests_properties(xex_calendar_threads_bad_package PROPERTIES TIMEOUT 25)

    add_executable(rcomp_calendar_time_differential calendar_time_differential.cpp
      ${RCOMP_ROOT}/cpu/runtime/func_table.cpp)
    target_compile_options(rcomp_calendar_time_differential PRIVATE ${RCOMP_ISA_FLAGS} -O2)
    target_link_libraries(rcomp_calendar_time_differential PRIVATE rcomp_runtime rcomp_platform)
    set(RCOMP_NTDLL_CALENDAR_DIFFERENTIAL "" CACHE FILEPATH
      "Optional Windows ntdll differential corpus generated by ntdll_calendar_oracle.py")
    if(RCOMP_NTDLL_CALENDAR_DIFFERENTIAL)
      add_test(NAME xex_calendar_time_differential COMMAND rcomp_calendar_time_differential
        ${RCOMP_NTDLL_CALENDAR_DIFFERENTIAL})
      set_tests_properties(xex_calendar_time_differential PROPERTIES TIMEOUT 60
        PASS_REGULAR_EXPRESSION "NTDLL-DIFFERENTIAL checks=4005 pass=4005 fail=0 scope=host")
    endif()
  endif()
  return()
endif()
if(RCOMP_XEX_MODULES)
  add_library(rcomp_xex_selftest STATIC modules_runner.cpp TESTDOUBLE_cpu_only_gpu.cpp
    ${RCOMP_ROOT}/app/src/title_runtime.cpp)
  target_include_directories(rcomp_xex_selftest PRIVATE ${RCOMP_ROOT}/app/include)
  target_compile_options(rcomp_xex_selftest PRIVATE ${RCOMP_XEX_OPTIONS})
  target_link_libraries(rcomp_xex_selftest PUBLIC rcomp_xex_generated
    rcomp_runtime_video rcomp_runtime rcomp_platform_input)
  if(PS5)
    target_compile_definitions(rcomp_xex_selftest PRIVATE RCOMP_MODULES_PS5)
  else()
    add_executable(rcomp_xex_modules boot_packaged_main.cpp TESTDOUBLE_cpu_only_gpu.cpp)
    target_include_directories(rcomp_xex_modules PRIVATE ${RCOMP_ROOT}/app/include)
    target_compile_options(rcomp_xex_modules PRIVATE ${RCOMP_XEX_OPTIONS})
    target_compile_definitions(rcomp_xex_modules PRIVATE RCOMP_XEX_PATH="${RCOMP_XEX}")
    target_link_libraries(rcomp_xex_modules PRIVATE rcomp_xex_selftest)
    enable_testing()
    add_test(NAME xex_modules COMMAND rcomp_xex_modules)
    set_tests_properties(xex_modules PROPERTIES TIMEOUT 60
      PASS_REGULAR_EXPRESSION "RCOMP-MODULES checks=29 pass=29 fail=0 scope=host")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME xex_modules_bad_package COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_modules_package.py $<TARGET_FILE:rcomp_xex_modules>
      ${RCOMP_XEX} ${CMAKE_BINARY_DIR})
    set_tests_properties(xex_modules_bad_package PROPERTIES TIMEOUT 60)
    add_test(NAME xex_modules_memory_layout COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_modules_memory_layout.py $<TARGET_FILE:rcomp_xex_modules>
      ${RCOMP_XEX} ${CMAKE_BINARY_DIR})
    set_tests_properties(xex_modules_memory_layout PROPERTIES TIMEOUT 180)
  endif()
  return()
endif()
if(RCOMP_XEX_POOL_RTL)
  add_library(rcomp_xex_selftest STATIC pool_rtl_runner.cpp TESTDOUBLE_cpu_only_gpu.cpp
    ${RCOMP_ROOT}/app/src/title_runtime.cpp)
  target_include_directories(rcomp_xex_selftest PRIVATE ${RCOMP_ROOT}/app/include)
  target_compile_options(rcomp_xex_selftest PRIVATE ${RCOMP_XEX_OPTIONS})
  target_link_libraries(rcomp_xex_selftest PUBLIC rcomp_xex_generated
    rcomp_runtime_video rcomp_runtime rcomp_platform_input)
  if(PS5)
    target_compile_definitions(rcomp_xex_selftest PRIVATE RCOMP_POOL_RTL_PS5)
  else()
    add_executable(rcomp_xex_pool_rtl boot_packaged_main.cpp TESTDOUBLE_cpu_only_gpu.cpp)
    target_include_directories(rcomp_xex_pool_rtl PRIVATE ${RCOMP_ROOT}/app/include)
    target_compile_options(rcomp_xex_pool_rtl PRIVATE ${RCOMP_XEX_OPTIONS})
    target_compile_definitions(rcomp_xex_pool_rtl PRIVATE RCOMP_XEX_PATH="${RCOMP_XEX}")
    target_link_libraries(rcomp_xex_pool_rtl PRIVATE rcomp_xex_selftest)
    enable_testing()
    add_test(NAME xex_pool_rtl COMMAND rcomp_xex_pool_rtl)
    set_tests_properties(xex_pool_rtl PROPERTIES TIMEOUT 60
      PASS_REGULAR_EXPRESSION "RCOMP-POOL-RTL checks=20 pass=20 fail=0 scope=host")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME xex_pool_rtl_bad_package COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_pool_rtl_package.py $<TARGET_FILE:rcomp_xex_pool_rtl>
      ${CMAKE_BINARY_DIR})
    set_tests_properties(xex_pool_rtl_bad_package PROPERTIES TIMEOUT 25)
  endif()
  return()
endif()
if(RCOMP_XEX_PACKAGED_BOOT)
  add_library(rcomp_xex_selftest STATIC boot_ps5_runner.cpp TESTDOUBLE_cpu_only_gpu.cpp
    ${RCOMP_ROOT}/app/src/title_runtime.cpp)
  target_include_directories(rcomp_xex_selftest PRIVATE ${RCOMP_ROOT}/app/include)
  target_compile_options(rcomp_xex_selftest PRIVATE ${RCOMP_XEX_OPTIONS})
  target_link_libraries(rcomp_xex_selftest PUBLIC rcomp_xex_generated
    rcomp_runtime_video rcomp_runtime rcomp_platform_input)
  if(PS5)
    target_compile_definitions(rcomp_xex_selftest PRIVATE RCOMP_BOOT_PS5)
  else()
    # The host driver calls the same exported archive entry as the PS5 title.
    # Supply the unused GPU boundary as an object too: host linkers need not
    # rescan the selftest archive after runtime_video introduces its symbols.
    add_executable(rcomp_xex_packaged_boot boot_packaged_main.cpp TESTDOUBLE_cpu_only_gpu.cpp)
    target_include_directories(rcomp_xex_packaged_boot PRIVATE ${RCOMP_ROOT}/app/include)
    target_compile_options(rcomp_xex_packaged_boot PRIVATE ${RCOMP_XEX_OPTIONS})
    target_compile_definitions(rcomp_xex_packaged_boot PRIVATE RCOMP_XEX_PATH="${RCOMP_XEX}")
    target_link_libraries(rcomp_xex_packaged_boot PRIVATE rcomp_xex_selftest)
    enable_testing()
    add_test(NAME xex_packaged_boot COMMAND rcomp_xex_packaged_boot)
    set_tests_properties(xex_packaged_boot PROPERTIES TIMEOUT 60
      PASS_REGULAR_EXPRESSION "RCOMP-BOOT-PACKAGED checks=27 pass=27 fail=0 scope=host")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME xex_packaged_boot_bad_package COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_packaged_boot.py $<TARGET_FILE:rcomp_xex_packaged_boot>
      ${RCOMP_XEX} ${CMAKE_BINARY_DIR})
    set_tests_properties(xex_packaged_boot_bad_package PROPERTIES TIMEOUT 30)
  endif()
  return()
endif()
if(RCOMP_XEX_BOOT_REHEARSAL)
  if(PS5)
    message(FATAL_ERROR "CPU boot rehearsal is host-only; no PS5 executable validation")
  endif()
  add_executable(rcomp_xex_boot_rehearsal boot_runner.cpp TESTDOUBLE_cpu_only_gpu.cpp
    ${RCOMP_ROOT}/app/src/title_runtime.cpp)
  target_include_directories(rcomp_xex_boot_rehearsal PRIVATE ${RCOMP_ROOT}/app/include)
  target_compile_options(rcomp_xex_boot_rehearsal PRIVATE ${RCOMP_XEX_OPTIONS})
  target_compile_definitions(rcomp_xex_boot_rehearsal PRIVATE
    RCOMP_XEX_PATH="${RCOMP_XEX}" RCOMP_TEST_DATA_ROOT="${CMAKE_BINARY_DIR}")
  target_link_libraries(rcomp_xex_boot_rehearsal PRIVATE rcomp_xex_generated
    rcomp_runtime_video rcomp_runtime rcomp_platform_input)
  enable_testing()
  add_test(NAME xex_boot_rehearsal COMMAND rcomp_xex_boot_rehearsal)
  set_tests_properties(xex_boot_rehearsal PROPERTIES TIMEOUT 60)
  return()
endif()

# Runner as a library for the PS5 test title (rcomp_xex_selftest).
add_library(rcomp_xex_selftest STATIC ${CMAKE_CURRENT_SOURCE_DIR}/xex_runner.cpp)
target_compile_definitions(rcomp_xex_selftest PRIVATE RCOMP_HARNESS_LIBRARY)
target_compile_options(rcomp_xex_selftest PRIVATE ${RCOMP_XEX_OPTIONS})
target_link_libraries(rcomp_xex_selftest PUBLIC rcomp_xex_generated)

add_executable(rcomp_xex_tests ${CMAKE_CURRENT_SOURCE_DIR}/xex_runner.cpp)
target_compile_options(rcomp_xex_tests PRIVATE ${RCOMP_XEX_OPTIONS})
target_compile_definitions(rcomp_xex_tests PRIVATE RCOMP_XEX_PATH="${RCOMP_XEX}")
target_link_libraries(rcomp_xex_tests PRIVATE rcomp_xex_generated)
