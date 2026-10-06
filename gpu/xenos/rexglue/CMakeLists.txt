# Xenos GPU from rexglue-sdk (owner: Agent 5 / gpu/xenos). Sources come from
# build/xenos-rexglue-src (gpu/xenos/rexglue/prepare.sh: pinned copy +
# patches); R-comp boundary headers in shim/ come first.
#   cmake -S gpu/xenos/rexglue -B build/xenos-gpu-host -G Ninja -DRCOMP_ROOT=$PWD
cmake_minimum_required(VERSION 3.20)
project(rcomp_xenos_gpu C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED REXGLUE_SRC)
  set(REXGLUE_SRC ${RCOMP_ROOT}/build/xenos-rexglue-src)
endif()
set(R ${REXGLUE_SRC})
# Same ISA floor as the rest of the project (docs/ARCHITECTURE.md).
add_compile_options(-march=x86-64-v3)
set(T ${R}/thirdparty)
option(RCOMP_XENOS_VULKAN "Build the Vulkan rendering backend" ON)
option(RCOMP_XENOS_DIAGNOSTICS "Enable Xenos bring-up logs and packet inspection" OFF)
option(RCOMP_XENOS_PROFILE_TIMINGS "Time Xenos packets, draws, interrupts and submissions" OFF)
option(RCOMP_XENOS_RENDER_DIAGNOSTICS "Compile bounded Xenos render-state diagnostics" OFF)
option(RCOMP_XENOS_FSI "Emulate EDRAM with fragment shader interlock (render_target_path_vulkan=fsi)" OFF)
option(RCOMP_XENOS_DIAG_SKIP_CPU_INVALIDATION "DIAGNOSTIC ONLY: skip per-kick CPU invalidation (stale data; upper-bound measurement)" OFF)

# glslang's SPIR-V builder only (no GLSL front end).
add_library(rcomp_glslang_spirv STATIC
  ${T}/glslang/SPIRV/SpvBuilder.cpp
  ${T}/glslang/SPIRV/InReadableOrder.cpp
  ${T}/glslang/SPIRV/Logger.cpp)
target_include_directories(rcomp_glslang_spirv PUBLIC ${T}/glslang ${T}/glslang/SPIRV ${T}/spirv-headers/include)

# Base utilities of rexglue's core that the GPU layers use.
add_library(rcomp_rex_base STATIC
  ${R}/src/core/memory.cpp
  ${R}/src/core/string_buffer.cpp
  ${R}/src/core/math_gcc.cpp
  ${R}/src/core/filesystem_posix.cpp
  ${R}/src/core/utf8.cpp
  ${R}/src/core/platform/env_posix.cpp
  ${R}/src/ui/graphics_util.cpp
  # R-comp implementations of rexglue's cvar/logging APIs (no CLI11, toml++,
  # spdlog sinks): defaults, REX_<NAME> environment overrides, stderr log.
  ${CMAKE_CURRENT_SOURCE_DIR}/shim_src/rex_cvar.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/shim_src/rex_logging.cpp)
if(NOT PS5)
  # RenderDoc capture hooks (dlopen) exist on the host only.
  target_sources(rcomp_rex_base PRIVATE ${R}/src/core/dynlib_posix.cpp ${R}/src/ui/renderdoc_api.cpp)
endif()
target_include_directories(rcomp_rex_base PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/shim ${R}/include ${R}/src/core ${RCOMP_ROOT}/include
  ${T}/fmt/include ${T}/spdlog/include ${T}/vulkan-headers/include ${T}/xxHash ${T}/renderdoc ${T}/utfcpp/source)
# PS5 titles have no emulated TLS runtime (__emutls_get_address) and their
# libc has no xlocale (*_l) functions: spdlog without thread_local and without
# thread ids, fmt without std::locale.
target_compile_definitions(rcomp_rex_base PUBLIC SPDLOG_FMT_EXTERNAL FMT_HEADER_ONLY
  SPDLOG_NO_TLS SPDLOG_NO_THREAD_ID FMT_USE_LOCALE=0 RCOMP_XENOS_NO_WINDOW_SYSTEM
  RCOMP_XENOS_DIAGNOSTICS=$<BOOL:${RCOMP_XENOS_DIAGNOSTICS}>
  RCOMP_XENOS_PROFILE_TIMINGS=$<BOOL:${RCOMP_XENOS_PROFILE_TIMINGS}>
  RCOMP_XENOS_RENDER_DIAGNOSTICS=$<BOOL:${RCOMP_XENOS_RENDER_DIAGNOSTICS}>
  RCOMP_XENOS_DIAG_SKIP_CPU_INVALIDATION=$<BOOL:${RCOMP_XENOS_DIAG_SKIP_CPU_INVALIDATION}>
  RCOMP_XENOS_FSI=$<BOOL:${RCOMP_XENOS_FSI}>)
if(RCOMP_XENOS_VULKAN)
  # Declares/defines the Vulkan backend's settings (rex/graphics/flags.h,
  # rex/ui/flags.h) in every library, so all agree on them.
  target_compile_definitions(rcomp_rex_base PUBLIC REX_HAS_VULKAN=1)
endif()

# Xenos microcode -> SPIR-V shader translator.
add_library(rcomp_xenos_shader STATIC
  ${R}/src/graphics/format/ucode.cpp
  ${R}/src/graphics/pipeline/shader/shader.cpp
  ${R}/src/graphics/pipeline/shader/translator.cpp
  ${R}/src/graphics/pipeline/shader/translator_disasm.cpp
  ${R}/src/graphics/flags.cpp
  ${R}/src/graphics/util/draw.cpp
  ${R}/src/graphics/registers.cpp
  ${R}/src/graphics/xenos.cpp
  ${R}/src/graphics/pipeline/texture/util.cpp
  ${R}/src/graphics/pipeline/texture/info.cpp
  ${R}/src/graphics/pipeline/texture/info_formats.cpp
  ${R}/src/graphics/pipeline/shader/spirv_builder.cpp
  ${R}/src/graphics/pipeline/shader/spirv.cpp
  ${R}/src/graphics/pipeline/shader/spirv_translator.cpp
  ${R}/src/graphics/pipeline/shader/spirv_translator_alu.cpp
  ${R}/src/graphics/pipeline/shader/spirv_translator_fetch.cpp
  ${R}/src/graphics/pipeline/shader/spirv_translator_memexport.cpp
  ${R}/src/graphics/pipeline/shader/spirv_translator_rb.cpp)
target_link_libraries(rcomp_xenos_shader PUBLIC rcomp_rex_base rcomp_glslang_spirv)
target_compile_definitions(rcomp_xenos_shader PUBLIC RCOMP_XENOS_NO_VULKAN_DEVICE)
target_compile_options(rcomp_xenos_shader PRIVATE -Wno-unused-variable)

add_executable(xenos_to_spirv tools/xenos_to_spirv.cpp)
target_link_libraries(xenos_to_spirv PRIVATE rcomp_xenos_shader)

# PM4 command processor (patch 0003: owned by a GpuHost, plain worker thread)
# with R-comp's memory view (shim/rex/system/xmemory.h).
add_library(rcomp_xenos_cp STATIC
  ${R}/src/graphics/command_processor.cpp
  ${R}/src/graphics/register_file.cpp
  ${R}/src/core/ring_buffer.cpp
  ${R}/src/core/byte_stream.cpp
  ${R}/src/core/clock.cpp
  ${R}/src/core/clock_posix.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/shim_src/rex_threading.cpp)
target_link_libraries(rcomp_xenos_cp PUBLIC rcomp_xenos_shader)
target_compile_options(rcomp_xenos_cp PRIVATE -Wno-unused-variable)

# R-comp host: GpuHost over GuestMemory, register-window bridge, vblank,
# include/rcomp/xenos_gpu.h.
add_library(rcomp_xenos_host STATIC rcomp/xenos_host.cpp)
target_link_libraries(rcomp_xenos_host PUBLIC rcomp_xenos_cp)

# ---------------------------------------------------------------------------
# Vulkan rendering backend (rexglue's VulkanCommandProcessor, Xenia-derived)
# and the headless presenter. Vulkan is reached through vkGetInstanceProcAddr
# linked into the binary (host: the Vulkan loader; PS5: PS5_Vulkan).
if(RCOMP_XENOS_VULKAN)
  # Full glslang (GLSL front end): the backend compiles a few internal
  # shaders (swap post-effects, tessellation stubs) at run time.
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
  add_subdirectory(${T}/glslang ${CMAKE_CURRENT_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)

  add_library(rcomp_xenos_vulkan STATIC
    ${R}/src/graphics/primitive_processor.cpp
    ${R}/src/graphics/shared_memory.cpp
    ${R}/src/graphics/sampler_info.cpp
    ${R}/src/graphics/pipeline/render_target/cache.cpp
    ${R}/src/graphics/pipeline/texture/cache.cpp
    ${R}/src/graphics/pipeline/texture/conversion.cpp
    ${R}/src/graphics/pipeline/texture/extent.cpp
    ${R}/src/graphics/util/draw_extent_estimator.cpp
    ${R}/src/graphics/pipeline/shader/spirv_translator_vulkan_features.cpp
    ${R}/src/graphics/vulkan/command_processor.cpp
    ${R}/src/graphics/vulkan/deferred_command_buffer.cpp
    ${R}/src/graphics/vulkan/pipeline_cache.cpp
    ${R}/src/graphics/vulkan/primitive_processor.cpp
    ${R}/src/graphics/vulkan/render_target_cache.cpp
    ${R}/src/graphics/vulkan/shader.cpp
    ${R}/src/graphics/vulkan/shared_memory.cpp
    ${R}/src/graphics/vulkan/texture_cache.cpp
    ${R}/src/ui/presenter.cpp
    ${R}/src/ui/graphics_upload_buffer_pool.cpp
    ${R}/src/ui/vulkan/linked_type_descriptor_set_allocator.cpp
    ${R}/src/ui/vulkan/single_layout_descriptor_set_pool.cpp
    ${R}/src/ui/vulkan/ui_samplers.cpp
    ${R}/src/ui/vulkan/vulkan_device.cpp
    ${R}/src/ui/vulkan/vulkan_instance.cpp
    ${R}/src/ui/vulkan/vulkan_mem_alloc.cpp
    ${R}/src/ui/vulkan/vulkan_presenter.cpp
    ${R}/src/ui/vulkan/vulkan_provider.cpp
    ${R}/src/ui/vulkan/vulkan_submission_tracker.cpp
    ${R}/src/ui/vulkan/vulkan_upload_buffer_pool.cpp
    ${R}/src/ui/vulkan/vulkan_util.cpp
    ${R}/src/ui/vulkan/vulkan_immediate_drawer.cpp
    ${R}/src/ui/immediate_drawer.cpp
    ${R}/src/graphics/pipeline/shader/interpreter.cpp
    shim_src/rex_backend_support.cpp
    rcomp/vulkan_output.cpp
    rcomp/vulkan_display.cpp)
  target_include_directories(rcomp_xenos_vulkan PUBLIC ${T}/vulkan-memory-allocator/include
    ${RCOMP_ROOT}/third_party/XenonRecomp/thirdparty/simde)
  target_link_libraries(rcomp_xenos_vulkan PUBLIC rcomp_xenos_host glslang SPIRV)
  # PUBLIC: it changes the layout of rex::ui::vulkan::VulkanInstance.
  target_compile_definitions(rcomp_xenos_vulkan PUBLIC RCOMP_XENOS_LINKED_VULKAN)
  if(NOT PS5)
    find_library(RCOMP_VULKAN_LOADER vulkan REQUIRED)  # PS5: PS5_Vulkan, linked by the title
    target_link_libraries(rcomp_xenos_vulkan PUBLIC ${RCOMP_VULKAN_LOADER})
  endif()
  target_compile_options(rcomp_xenos_vulkan PRIVATE -Wno-unused-variable)
endif()
