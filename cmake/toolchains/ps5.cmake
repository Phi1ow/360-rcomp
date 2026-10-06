# R-comp PS5 cross toolchain (owner: Agent 2, file granted by PRIME).
#
# Wraps ps5-payload-sdk v0.42 (pinned in deps/deps.lock, sha256 8cfbc7cd...02da):
# its toolchain/prospero.cmake sets the compilers (bin/prospero-clang{,++},
# which add -target x86_64-sie-ps5 -fno-stack-protector -fno-plt
# -femulated-tls and the SDK sysroot), CMAKE_SYSTEM_NAME=FreeBSD, PS5=TRUE and
# PIC-by-default. Nothing here adds -march=native, and nothing here depends on
# the build machine's CPU.
#
# SDK location, first match wins:
#   -DRCOMP_PS5_SDK=<dir>   |  $PS5_PAYLOAD_SDK   |  ${RCOMP_DEPS}/sdk/ps5-payload-sdk
#   with RCOMP_DEPS defaulting to $RCOMP_DEPS or <repo>/../deps (AGENTS.md).
#
# ISA policy (see platform/ps5/README.md, "CPU and ISA flags"):
#   * The x86_64-sie-ps5 target already defaults to -target-cpu znver2 (AVX2,
#     FMA, BMI1/2, F16C, LZCNT, MOVBE, SSE4a; checked with `prospero-clang -###`).
#     Ordinary code (platform, runtime, HLE) keeps that default.
#   * Generated XenonRecomp code uses RCOMP_GENERATED_ISA_FLAGS, PRIME's
#     choice -march=x86-64-v3 (a strict subset of Zen 2, so identical code runs
#     on the PS5 and on any x86-64-v3 host used for oracle tests) plus
#     -mtune=znver2 so the subset is scheduled for the real core. Consumers
#     apply it per target:
#       target_compile_options(<gen> PRIVATE ${RCOMP_GENERATED_CODE_FLAGS})
#   * RCOMP_GENERATED_FP_FLAGS = -ffp-contract=on: only contracts a*b+c written
#     inside one expression (what an fmadd emitter writes), never across
#     statements, so separately rounded PPC fmul/fadd stay separate. Never "fast".
cmake_minimum_required(VERSION 3.20)

get_filename_component(_rcomp_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

if(NOT RCOMP_PS5_SDK)
  if(DEFINED ENV{PS5_PAYLOAD_SDK} AND EXISTS "$ENV{PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
    set(RCOMP_PS5_SDK "$ENV{PS5_PAYLOAD_SDK}")
  else()
    if(DEFINED ENV{RCOMP_DEPS})
      set(_rcomp_deps "$ENV{RCOMP_DEPS}")
    else()
      get_filename_component(_rcomp_deps "${_rcomp_root}/../deps" ABSOLUTE)
    endif()
    set(RCOMP_PS5_SDK "${_rcomp_deps}/sdk/ps5-payload-sdk")
  endif()
endif()
get_filename_component(RCOMP_PS5_SDK "${RCOMP_PS5_SDK}" ABSOLUTE)
set(RCOMP_PS5_SDK "${RCOMP_PS5_SDK}" CACHE PATH "ps5-payload-sdk root (v0.42)")

if(NOT EXISTS "${RCOMP_PS5_SDK}/toolchain/prospero.cmake")
  message(FATAL_ERROR "BLOCKED: ps5-payload-sdk not found at '${RCOMP_PS5_SDK}' "
                      "(set -DRCOMP_PS5_SDK=, PS5_PAYLOAD_SDK or RCOMP_DEPS).")
endif()

# try_compile() re-reads this file in a fresh scope: forward our variables.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RCOMP_PS5_SDK)

include("${RCOMP_PS5_SDK}/toolchain/prospero.cmake")

# prospero.cmake forces verbose makefiles; keep that choice to the user.
set(CMAKE_VERBOSE_MAKEFILE OFF CACHE BOOL "" FORCE)
# The SDK's install prefix is /user/homebrew (payload convention); a title
# folder is assembled by platform/ps5/tools/build_title.sh instead.

set(RCOMP_TARGET_PS5 ON)

set(RCOMP_GENERATED_ISA_FLAGS "-march=x86-64-v3;-mtune=znver2"
    CACHE STRING "ISA flags for XenonRecomp-generated code on PS5 (list)")
set(RCOMP_GENERATED_FP_FLAGS "-ffp-contract=on"
    CACHE STRING "Floating-point flags for XenonRecomp-generated code (list)")
set(RCOMP_GENERATED_CODE_FLAGS ${RCOMP_GENERATED_ISA_FLAGS} ${RCOMP_GENERATED_FP_FLAGS})

foreach(_f IN LISTS RCOMP_GENERATED_ISA_FLAGS)
  if(_f MATCHES "native")
    message(FATAL_ERROR "RCOMP_GENERATED_ISA_FLAGS must not depend on the build machine: ${_f}")
  endif()
endforeach()
