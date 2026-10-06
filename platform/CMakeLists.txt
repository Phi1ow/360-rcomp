# Platform layer (owner: Agent 2): guest memory, OS VM backends, diagnostics.
#
# Standalone:  cmake -S platform -B build/platform-host -G Ninja
#              cmake -S platform -B build/platform-ps5 -G Ninja \
#                    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ps5.cmake
# Also usable through add_subdirectory(platform) from the root build.
cmake_minimum_required(VERSION 3.20)
project(rcomp_platform LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(RCOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" AND DEFINED PS5)
  set(RCOMP_PLATFORM_PS5 ON)
else()
  set(RCOMP_PLATFORM_PS5 OFF)
endif()

set(RCOMP_PLATFORM_WARNINGS -Wall -Wextra -Werror)

set(RCOMP_PLATFORM_COMMON_SOURCES
  common/diag.cpp
  common/guest_memory.cpp)

if(RCOMP_PLATFORM_PS5)
  set(RCOMP_PLATFORM_VM_SOURCE ps5/os_vm_ps5.cpp)
else()
  set(RCOMP_PLATFORM_VM_SOURCE posix/os_vm_posix.cpp)
endif()

add_library(rcomp_platform STATIC ${RCOMP_PLATFORM_COMMON_SOURCES} ${RCOMP_PLATFORM_VM_SOURCE})
target_include_directories(rcomp_platform PUBLIC "${RCOMP_ROOT}/include"
                                          PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common")
target_compile_options(rcomp_platform PRIVATE ${RCOMP_PLATFORM_WARNINGS})
if(RCOMP_PLATFORM_PS5)
  # Title code is built without exceptions/RTTI (the title has no unwinder
  # registration of its own beyond libunwind's __eh_frame_* symbols).
  target_compile_options(rcomp_platform PRIVATE -fno-exceptions -fno-rtti)
endif()

# Controller backend for the XAM input exports (include/rcomp/input.h); linked
# only by titles that register the XAM HLE. PS5: needs libScePad.so and
# libSceUserService.so at link time.
if(RCOMP_PLATFORM_PS5)
  add_library(rcomp_platform_input STATIC ps5/input_ps5.cpp)
  # Scripted controller input from /app0/autopilot.txt for unattended measurement runs. Player builds turn it off.
  option(RCOMP_PS5_AUTOPILOT "Read /app0/autopilot.txt (diagnostic scripted input)" ON)
  target_compile_definitions(rcomp_platform_input PRIVATE RCOMP_PS5_AUTOPILOT=$<BOOL:${RCOMP_PS5_AUTOPILOT}>)
  target_compile_options(rcomp_platform_input PRIVATE -fno-exceptions -fno-rtti)
else()
  add_library(rcomp_platform_input STATIC common/input_none.cpp)
endif()
target_include_directories(rcomp_platform_input PUBLIC "${RCOMP_ROOT}/include")
target_compile_options(rcomp_platform_input PRIVATE ${RCOMP_PLATFORM_WARNINGS})


# Six-channel 48 kHz guest audio output. Host has no device backend and reports
# UNSUPPORTED. PS5 uses an independently owned AudioOut port with bounded queues.
if(RCOMP_PLATFORM_PS5)
  add_library(rcomp_platform_audio STATIC ps5/audio_output_ps5.cpp)
  target_compile_options(rcomp_platform_audio PRIVATE -fno-exceptions -fno-rtti)
  target_link_libraries(rcomp_platform_audio PUBLIC SceAudioOut)
else()
  add_library(rcomp_platform_audio STATIC common/audio_output_none.cpp)
endif()
target_include_directories(rcomp_platform_audio PUBLIC "${RCOMP_ROOT}/include")
target_compile_options(rcomp_platform_audio PRIVATE ${RCOMP_PLATFORM_WARNINGS})

option(RCOMP_PLATFORM_TESTS "Build platform tests" ON)
if(RCOMP_PLATFORM_TESTS)
  add_executable(test_guest_memory tests/test_guest_memory.cpp)
  target_link_libraries(test_guest_memory PRIVATE rcomp_platform)
  target_compile_options(test_guest_memory PRIVATE ${RCOMP_PLATFORM_WARNINGS})
  if(RCOMP_PLATFORM_PS5)
    target_compile_definitions(test_guest_memory PRIVATE RCOMP_TEST_PS5_BACKEND)
    # Library form of the suite, linked into the PS5 test title.
    add_library(rcomp_platform_selftest STATIC tests/test_guest_memory.cpp)
    target_compile_definitions(rcomp_platform_selftest PRIVATE RCOMP_TEST_PS5_BACKEND
                                                               RCOMP_SELFTEST_NO_MAIN)
    target_compile_options(rcomp_platform_selftest PRIVATE ${RCOMP_PLATFORM_WARNINGS}
                                                           -fno-exceptions -fno-rtti)
    target_link_libraries(rcomp_platform_selftest PUBLIC rcomp_platform)
  else()
    enable_testing()
    add_test(NAME guest_memory_posix COMMAND test_guest_memory)
    add_executable(test_opaque_runtime_range tests/test_opaque_runtime_range.cpp)
    target_link_libraries(test_opaque_runtime_range PRIVATE rcomp_platform)
    target_compile_options(test_opaque_runtime_range PRIVATE ${RCOMP_PLATFORM_WARNINGS})
    add_test(NAME opaque_runtime_address_reservation COMMAND test_opaque_runtime_range)
    add_executable(test_pad_map tests/test_pad_map.cpp)
    target_include_directories(test_pad_map PRIVATE "${RCOMP_ROOT}/include")
    target_compile_options(test_pad_map PRIVATE ${RCOMP_PLATFORM_WARNINGS})
    add_test(NAME pad_map COMMAND test_pad_map)


    add_executable(test_audio_output_none tests/test_audio_output_none.cpp)
    target_link_libraries(test_audio_output_none PRIVATE rcomp_platform_audio)
    target_compile_options(test_audio_output_none PRIVATE ${RCOMP_PLATFORM_WARNINGS})
    add_test(NAME audio_output_host_unsupported COMMAND test_audio_output_none)

    find_package(Threads REQUIRED)
    if(CMAKE_USE_PTHREADS_INIT)
      # Explicit host-only TESTDOUBLE native ABI; never a production sink.
      add_executable(test_audio_output_ps5_backend ps5/audio_output_ps5.cpp
        tests/TESTDOUBLE_audio_native.cpp tests/test_audio_output.cpp)
      target_include_directories(test_audio_output_ps5_backend PRIVATE "${RCOMP_ROOT}/include")
      target_compile_options(test_audio_output_ps5_backend PRIVATE ${RCOMP_PLATFORM_WARNINGS})
      target_link_libraries(test_audio_output_ps5_backend PRIVATE Threads::Threads)
      add_test(NAME audio_output_ps5_backend_testdouble COMMAND test_audio_output_ps5_backend)
    endif()

    # Failure paths use the real host VM with test-only failures before each
    # mutation, never success-returning substitutes for memory operations.
    add_executable(test_guest_memory_failures common/guest_memory.cpp
      tests/test_guest_memory_failures.cpp tests/TESTDOUBLE_os_vm_failures.cpp)
    target_include_directories(test_guest_memory_failures PRIVATE "${RCOMP_ROOT}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/common")
    target_compile_options(test_guest_memory_failures PRIVATE ${RCOMP_PLATFORM_WARNINGS})
    add_test(NAME guest_memory_permission_failures COMMAND test_guest_memory_failures)

    # The PS5 backend's bookkeeping, run on the host against a TESTDOUBLE
    # kernel (platform/tests/TESTDOUBLE_ps5_kernel_host.cpp). Linux only.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      add_library(rcomp_platform_ps5emu STATIC ${RCOMP_PLATFORM_COMMON_SOURCES}
                  ps5/os_vm_ps5.cpp tests/TESTDOUBLE_ps5_kernel_host.cpp)
      target_include_directories(rcomp_platform_ps5emu PUBLIC "${RCOMP_ROOT}/include"
                                                       PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common")
      target_compile_options(rcomp_platform_ps5emu PRIVATE ${RCOMP_PLATFORM_WARNINGS})
      add_executable(test_guest_memory_ps5emu tests/test_guest_memory.cpp)
      target_compile_definitions(test_guest_memory_ps5emu PRIVATE RCOMP_TEST_PS5_BACKEND
                                                                  RCOMP_TEST_TESTDOUBLE_PS5)
      target_compile_options(test_guest_memory_ps5emu PRIVATE ${RCOMP_PLATFORM_WARNINGS})
      target_link_libraries(test_guest_memory_ps5emu PRIVATE rcomp_platform_ps5emu)
      add_test(NAME guest_memory_ps5_backend_testdouble COMMAND test_guest_memory_ps5emu)
    endif()
  endif()
endif()
