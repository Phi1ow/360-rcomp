# Guest runtime / HLE / files (owner: Agent 3).
#
# Standalone:  cmake -S runtime -B build/runtime-host -G Ninja \
#                -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/runtime-host
# PS5 (compile + archive only):
#   see runtime/docs/RUNTIME.md ("PS5 cross build").
cmake_minimum_required(VERSION 3.20)
project(rcomp_runtime LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(RCOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(EXISTS "${RCOMP_ROOT}/build/cpu-xenonrecomp-src/XenonUtils/ppc_context.h")
  set(_rcomp_xr_default "${RCOMP_ROOT}/build/cpu-xenonrecomp-src")
else()
  set(_rcomp_xr_default "${RCOMP_ROOT}/third_party/XenonRecomp")
endif()
set(RCOMP_XENONRECOMP "${_rcomp_xr_default}" CACHE PATH "XenonRecomp tree (patched copy preferred)")
option(RCOMP_RUNTIME_TESTS "Build runtime tests (needs platform/ sources)" ON)
option(RCOMP_XMA_DECODER_DIAGNOSTICS "Trace first FFmpeg calls per XMA context" OFF)
option(RCOMP_RUNTIME_DIAGNOSTICS "Enable expensive runtime bring-up tracing" OFF)
option(RCOMP_RUNTIME_WAIT_STATS "Count the outcomes of guest waits (an atomic add per wait; the frame-rate counter prints them)" OFF)
set(RCOMP_XMA_FFMPEG_ROOT "" CACHE PATH "Pinned XMAFRAMES FFmpeg install prefix (optional)")

set(RCOMP_RT_WARNINGS -Wall -Wextra -Werror=return-type -Wno-unused-parameter)

add_library(rcomp_runtime STATIC
    src/status.cpp
    src/handle_table.cpp
    src/guest_heap.cpp
    src/guest_thread.cpp
    src/host_fiber.cpp
    src/thread_object.cpp
    src/virtual_fields.cpp
    src/kernel_variables.cpp
    src/import_registry.cpp
    src/import_thunks.cpp
    src/clock_sync.cpp
    src/vfs.cpp
    src/hdd.cpp
    src/runtime.cpp
    src/hle_xboxkrnl.cpp
    src/hle_xboxkrnl_threads.cpp
    src/hle_xboxkrnl_memory.cpp
    src/hle_xboxkrnl_pool_rtl.cpp
    src/hle_xboxkrnl_modules.cpp
    src/hle_xboxkrnl_time.cpp
    src/hle_xboxkrnl_io.cpp
    src/hle_xboxkrnl_objects.cpp
    src/hle_xboxkrnl_devices.cpp
    src/hle_xboxkrnl_sched.cpp
    src/hle_xboxkrnl_xma.cpp
    src/xma_decoder.cpp
    src/hle_xboxkrnl_xaudio.cpp
    src/hle_xboxkrnl_strings.cpp
    src/hle_xboxkrnl_format.cpp
    src/hle_xboxkrnl_config.cpp
    src/hle_xboxkrnl_process.cpp
    src/hle_xboxkrnl_query_memory.cpp
    src/hle_xboxkrnl_seh.cpp
    src/hle_xboxkrnl_more.cpp
    src/hle_xboxkrnl_dpc.cpp
    src/hle_xboxkrnl_crypto_more.cpp
    src/hle_xboxkrnl_xma_api.cpp
    src/hle_xboxkrnl_audio_ducker.cpp
    src/hle_xam.cpp
    src/hle_xam_loader.cpp
    src/hle_xam_net.cpp
    src/hle_xam_net_wsa.cpp
    src/hle_xam_notifications.cpp
    src/hle_xam_enum.cpp
    src/hle_xam_msg.cpp
    src/hle_xam_system.cpp
    src/hle_xam_content.cpp
    src/hle_xam_profile.cpp
    src/hle_xam_misc.cpp
    src/hle_xam_ui_more.cpp
    src/hle_xam_live_dialogs.cpp
    src/hle_xam_more.cpp
    src/hle_xam_net_more.cpp
    src/xam_execution_info.cpp
    src/guest_context_check.cpp
    src/xex_loader.cpp
    src/aot_modules.cpp
)
# Keep the platform socket implementation in the same archive so manual PS5
# title links cannot accidentally omit the backend selected for this runtime.
if(PS5)
    target_sources(rcomp_runtime PRIVATE "${RCOMP_ROOT}/platform/ps5/net_ps5.cpp")
else()
    target_sources(rcomp_runtime PRIVATE "${RCOMP_ROOT}/platform/posix/net_posix.cpp")
endif()
target_include_directories(rcomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${RCOMP_ROOT}/include"
    # ppc_context.h + export tables (read-only), simde for ppc_context.h
    "${RCOMP_XENONRECOMP}/XenonUtils"
    "${RCOMP_XENONRECOMP}/thirdparty/simde")
target_compile_options(rcomp_runtime PRIVATE ${RCOMP_RT_WARNINGS})
target_compile_definitions(rcomp_runtime PRIVATE
    RCOMP_RUNTIME_DIAGNOSTICS=$<BOOL:${RCOMP_RUNTIME_DIAGNOSTICS}>
    RCOMP_RUNTIME_WAIT_STATS=$<BOOL:${RCOMP_RUNTIME_WAIT_STATS}>
    RCOMP_XMA_DECODER_DIAGNOSTICS=$<BOOL:${RCOMP_XMA_DECODER_DIAGNOSTICS}>)
find_package(Threads REQUIRED)
target_link_libraries(rcomp_runtime PUBLIC Threads::Threads)
if(RCOMP_XMA_FFMPEG_ROOT)
    foreach(_xma_file include/libavcodec/avcodec.h lib/libavcodec.a lib/libavutil.a)
        if(NOT EXISTS "${RCOMP_XMA_FFMPEG_ROOT}/${_xma_file}")
            message(FATAL_ERROR "Missing pinned XMA decoder dependency: ${RCOMP_XMA_FFMPEG_ROOT}/${_xma_file}")
        endif()
    endforeach()
    target_include_directories(rcomp_runtime PRIVATE "${RCOMP_XMA_FFMPEG_ROOT}/include")
    target_compile_definitions(rcomp_runtime PRIVATE RCOMP_XMA_DECODER=1)
    target_link_libraries(rcomp_runtime PUBLIC
        "${RCOMP_XMA_FFMPEG_ROOT}/lib/libavcodec.a"
        "${RCOMP_XMA_FFMPEG_ROOT}/lib/libavutil.a")
    if(NOT WIN32)
        target_link_libraries(rcomp_runtime PUBLIC m)
    endif()
    if(WIN32 OR CYGWIN)
        target_link_libraries(rcomp_runtime PUBLIC bcrypt)
    endif()
endif()

# Vd* video exports (include/rcomp/runtime/video.h). Separate so titles
# without the GPU do not need an implementation of include/rcomp/xenos_gpu.h;
# whoever links this also links one (gpu/xenos/rexglue: rcomp_xenos_host).
add_library(rcomp_runtime_video STATIC src/hle_xboxkrnl_video.cpp)
target_link_libraries(rcomp_runtime_video PUBLIC rcomp_runtime)
target_compile_options(rcomp_runtime_video PRIVATE ${RCOMP_RT_WARNINGS})
target_compile_definitions(rcomp_runtime_video PRIVATE
    RCOMP_RUNTIME_DIAGNOSTICS=$<BOOL:${RCOMP_RUNTIME_DIAGNOSTICS}>)

if(RCOMP_RUNTIME_TESTS)
    # Agent 2's platform implementation, compiled read-only from its path into
    # this build dir (not owned here). Integration (PRIME) links the real
    # platform library instead.
    add_library(rcomp_runtime_platform_deps STATIC
        "${RCOMP_ROOT}/platform/common/diag.cpp"
        "${RCOMP_ROOT}/platform/common/guest_memory.cpp"
        "${RCOMP_ROOT}/platform/posix/os_vm_posix.cpp"
        # guest address -> function table (ExCreateThread), implemented in cpu/
        "${RCOMP_ROOT}/cpu/runtime/func_table.cpp"
        # TSC calibration behind rcomp/fast_clock.h: the runtime's clocks and waits
        "${RCOMP_ROOT}/cpu/runtime/timebase.cpp")
    target_include_directories(rcomp_runtime_platform_deps PUBLIC
        "${RCOMP_ROOT}/include" "${RCOMP_ROOT}/platform/common")

    target_sources(rcomp_runtime_platform_deps PRIVATE tests/TESTDOUBLE_audio_output.cpp)
    enable_testing()
    add_executable(rt_test_xaudio_submit_log tests/test_xaudio_submit_log.cpp)
    target_link_libraries(rt_test_xaudio_submit_log PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    add_test(NAME rt_xaudio_submit_log COMMAND rt_test_xaudio_submit_log)
    set_tests_properties(rt_xaudio_submit_log PROPERTIES TIMEOUT 30)
    add_executable(rt_test_xaudio_output tests/test_xaudio_output.cpp)
    target_link_libraries(rt_test_xaudio_output PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    add_test(NAME rt_xaudio_output COMMAND rt_test_xaudio_output)
    set_tests_properties(rt_xaudio_output PROPERTIES TIMEOUT 30)
    add_executable(rt_test_xaudio_no_backend tests/test_xaudio_no_backend.cpp)
    target_link_libraries(rt_test_xaudio_no_backend PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    add_test(NAME rt_xaudio_no_backend COMMAND rt_test_xaudio_no_backend)
    set_tests_properties(rt_xaudio_no_backend PROPERTIES TIMEOUT 30)
    if(RCOMP_XMA_FFMPEG_ROOT)
        add_executable(rt_test_xma_decode tests/test_xma_decode.cpp)
        target_link_libraries(rt_test_xma_decode PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
        add_test(NAME rt_xma_decode COMMAND rt_test_xma_decode)
        set_tests_properties(rt_xma_decode PROPERTIES TIMEOUT 30)
    endif()
    foreach(t handles heap stack vfs missing_import clock_sync hle xex_loader threads physical static_tls lifetime sync_objects pool_rtl modules time strings file_completion io objects format xconfig process_lifecycle query_memory kernel_fields xam_net xam_net_wsa xam_live_dialogs notifications xma xma_packets timers slist kernel_devices suspend_pulse memory_statistics xam_msg hdd)
        add_executable(rt_test_${t} tests/test_${t}.cpp)
        target_link_libraries(rt_test_${t} PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
        target_compile_options(rt_test_${t} PRIVATE ${RCOMP_RT_WARNINGS})
        add_test(NAME rt_${t} COMMAND rt_test_${t})
        set_tests_properties(rt_${t} PROPERTIES TIMEOUT 30)
    endforeach()
    # Kernel imports of the 3 Oct 2026 catalog batch (runtime/docs/KERNEL_IMPORTS_20261003.md, SEH.md).
    foreach(t kernel_seh kernel_xex kernel_dispatch kernel_dpc kernel_more kernel_iocp file_apc)
        add_executable(rt_test_${t} tests/test_${t}.cpp)
        target_link_libraries(rt_test_${t} PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
        target_compile_options(rt_test_${t} PRIVATE ${RCOMP_RT_WARNINGS})
        add_test(NAME rt_${t} COMMAND rt_test_${t})
        set_tests_properties(rt_${t} PROPERTIES TIMEOUT 30)
    endforeach()
    # AOT secondary modules (XexLoadImage of title DLLs, runtime/docs/MODULES.md); rcomp::add_functions
    # comes from cpu/runtime/func_table.cpp above.
    add_executable(rt_test_aot_modules tests/test_aot_modules.cpp)
    target_link_libraries(rt_test_aot_modules PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_options(rt_test_aot_modules PRIVATE ${RCOMP_RT_WARNINGS})
    add_test(NAME rt_aot_modules COMMAND rt_test_aot_modules)
    set_tests_properties(rt_aot_modules PROPERTIES TIMEOUT 30)
    add_executable(rt_test_swap_pacing tests/test_swap_pacing.cpp)
    target_include_directories(rt_test_swap_pacing PRIVATE src)
    target_link_libraries(rt_test_swap_pacing PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_options(rt_test_swap_pacing PRIVATE ${RCOMP_RT_WARNINGS})
    add_test(NAME rt_swap_pacing COMMAND rt_test_swap_pacing)
    set_tests_properties(rt_swap_pacing PROPERTIES TIMEOUT 30)
    add_executable(rt_test_fast_clock tests/test_fast_clock.cpp)
    target_link_libraries(rt_test_fast_clock PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_options(rt_test_fast_clock PRIVATE ${RCOMP_RT_WARNINGS})
    add_test(NAME rt_fast_clock COMMAND rt_test_fast_clock)
    set_tests_properties(rt_fast_clock PROPERTIES TIMEOUT 30)
    add_executable(rt_test_virtual_guest_access tests/test_virtual_guest_access.cpp
        "${RCOMP_ROOT}/cpu/runtime/guest_access.cpp")
    target_link_libraries(rt_test_virtual_guest_access PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_definitions(rt_test_virtual_guest_access PRIVATE
        RCOMP_VIRTUAL_GUEST_ACCESS=1 RCOMP_CHECKED_GUEST_ACCESS=1)
    target_compile_options(rt_test_virtual_guest_access PRIVATE ${RCOMP_RT_WARNINGS}
        "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h")
    add_test(NAME rt_virtual_guest_access COMMAND rt_test_virtual_guest_access)
    set_tests_properties(rt_virtual_guest_access PROPERTIES TIMEOUT 30)
    # Guest fibers: xapi's routines as test doubles over the generated-code memory macros
    # (runtime/docs/THREAD_OBJECTS.md ("Guest fibers")); host_fiber.h is internal (src/).
    add_executable(rt_test_fibers tests/test_fibers.cpp
        "${RCOMP_ROOT}/cpu/runtime/guest_access.cpp")
    target_include_directories(rt_test_fibers PRIVATE src)
    target_link_libraries(rt_test_fibers PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_definitions(rt_test_fibers PRIVATE
        RCOMP_VIRTUAL_GUEST_ACCESS=1 RCOMP_CHECKED_GUEST_ACCESS=1)
    target_compile_options(rt_test_fibers PRIVATE ${RCOMP_RT_WARNINGS}
        "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h")
    add_test(NAME rt_fibers COMMAND rt_test_fibers)
    set_tests_properties(rt_fibers PROPERTIES TIMEOUT 60)
    # The compared stores of the generated code (RCOMP_STORE_COMPARE): which stores mark a page for the GPU caches.
    add_executable(rt_test_store_compare tests/test_store_compare.cpp
        "${RCOMP_ROOT}/cpu/runtime/guest_access.cpp")
    target_link_libraries(rt_test_store_compare PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_definitions(rt_test_store_compare PRIVATE
        RCOMP_VIRTUAL_GUEST_ACCESS=1 RCOMP_STORE_COMPARE=1)
    target_compile_options(rt_test_store_compare PRIVATE ${RCOMP_RT_WARNINGS}
        "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h")
    add_test(NAME rt_store_compare COMMAND rt_test_store_compare)
    set_tests_properties(rt_store_compare PROPERTIES TIMEOUT 30)
    # Deterministic bad_alloc injection needs host C++ exception support. PS5
    # archives use -fno-exceptions and do not promise host-OOM recovery.
    if(NOT CMAKE_CROSSCOMPILING)
        add_executable(rt_test_heap_failures tests/test_heap_failures.cpp)
        target_link_libraries(rt_test_heap_failures PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
        target_compile_options(rt_test_heap_failures PRIVATE ${RCOMP_RT_WARNINGS})
        add_test(NAME rt_heap_failures COMMAND rt_test_heap_failures)
        set_tests_properties(rt_heap_failures PROPERTIES TIMEOUT 30)
    endif()
    add_executable(rt_test_func_table "${RCOMP_ROOT}/cpu/tests/test_func_table.cpp")
    target_link_libraries(rt_test_func_table PRIVATE rcomp_runtime_platform_deps)
    add_test(NAME rt_func_table COMMAND rt_test_func_table)
    set_tests_properties(rt_func_table PROPERTIES TIMEOUT 30)
    add_executable(rt_test_video_lifetime tests/test_video_lifetime.cpp tests/TESTDOUBLE_video_gpu.cpp)
    target_link_libraries(rt_test_video_lifetime PRIVATE rcomp_runtime_video rcomp_runtime rcomp_runtime_platform_deps)
    add_test(NAME rt_video_lifetime COMMAND rt_test_video_lifetime)
    set_tests_properties(rt_video_lifetime PROPERTIES TIMEOUT 30)
    # XAM input over a scripted controller (TESTDOUBLE backend).
    add_executable(rt_test_xam tests/test_xam.cpp tests/TESTDOUBLE_input.cpp)
    target_link_libraries(rt_test_xam PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_options(rt_test_xam PRIVATE ${RCOMP_RT_WARNINGS})
    add_test(NAME rt_xam COMMAND rt_test_xam)
    # Local profile, saves (content packages) and profile settings over a temporary save root.
    add_executable(rt_test_xam_content tests/test_xam_content.cpp tests/TESTDOUBLE_input.cpp)
    target_link_libraries(rt_test_xam_content PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_options(rt_test_xam_content PRIVATE ${RCOMP_RT_WARNINGS})
    add_test(NAME rt_xam_content COMMAND rt_test_xam_content)
    set_tests_properties(rt_xam_content PROPERTIES TIMEOUT 30)
    # Tasks, sessions, system UI, voice, QoS and XNetLogon for a locally signed-in, not-LIVE profile.
    add_executable(rt_test_xam_misc tests/test_xam_misc.cpp tests/TESTDOUBLE_input.cpp)
    target_link_libraries(rt_test_xam_misc PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_options(rt_test_xam_misc PRIVATE ${RCOMP_RT_WARNINGS})
    add_test(NAME rt_xam_misc COMMAND rt_test_xam_misc)
    set_tests_properties(rt_xam_misc PROPERTIES TIMEOUT 30)
    # Guide dialogs (keyboard, achievements, LIVE-only screens), launch data and gamer pictures (SPA -> profile).
    add_executable(rt_test_xam_ui_more tests/test_xam_ui_more.cpp tests/TESTDOUBLE_input.cpp)
    target_link_libraries(rt_test_xam_ui_more PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_options(rt_test_xam_ui_more PRIVATE ${RCOMP_RT_WARNINGS})
    add_test(NAME rt_xam_ui_more COMMAND rt_test_xam_ui_more)
    set_tests_properties(rt_xam_ui_more PROPERTIES TIMEOUT 30)
    # Second title wave (Agent 3b): crypto vectors, XMA context helpers, ducker, XNet keys/queries.
    foreach(t crypto_more xma_api xaudio_ducker xam_net_more)
        add_executable(rt_test_${t} tests/test_${t}.cpp)
        target_link_libraries(rt_test_${t} PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
        target_compile_options(rt_test_${t} PRIVATE ${RCOMP_RT_WARNINGS})
        add_test(NAME rt_${t} COMMAND rt_test_${t})
        set_tests_properties(rt_${t} PROPERTIES TIMEOUT 30)
    endforeach()
    # Overlapped results, custom gamercard actions, licences, device context, launch data, message boxes.
    add_executable(rt_test_xam_more tests/test_xam_more.cpp tests/TESTDOUBLE_input.cpp)
    target_link_libraries(rt_test_xam_more PRIVATE rcomp_runtime rcomp_runtime_platform_deps)
    target_compile_options(rt_test_xam_more PRIVATE ${RCOMP_RT_WARNINGS})
    add_test(NAME rt_xam_more COMMAND rt_test_xam_more)
    set_tests_properties(rt_xam_more PROPERTIES TIMEOUT 30)
    # Same VFS suite through the lexical confinement used when realpath() is
    # refused (PS5: lstat -> EPERM).
    add_test(NAME rt_vfs_lexical COMMAND rt_test_vfs)
    set_tests_properties(rt_vfs_lexical PROPERTIES ENVIRONMENT RCOMP_VFS_FORCE_LEXICAL=1)
endif()
