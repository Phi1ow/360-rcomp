cmake_minimum_required(VERSION 3.20)
project(rcomp_m6_title LANGUAGES C CXX)

foreach(v RCOMP_ROOT M6_WORK)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "${v} is required")
  endif()
endforeach()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(RCOMP_PLATFORM_TESTS OFF CACHE BOOL "" FORCE)
set(RCOMP_RUNTIME_TESTS OFF CACHE BOOL "" FORCE)
set(RCOMP_XENONRECOMP "${RCOMP_ROOT}/build/cpu-xenonrecomp-src" CACHE PATH "" FORCE)
add_subdirectory("${RCOMP_ROOT}/platform" "${CMAKE_BINARY_DIR}/platform")
add_subdirectory("${RCOMP_ROOT}/runtime" "${CMAKE_BINARY_DIR}/runtime")
add_subdirectory("${RCOMP_ROOT}/gpu/xenos/rexglue" "${CMAKE_BINARY_DIR}/xenos")
add_subdirectory("${RCOMP_ROOT}/app" "${CMAKE_BINARY_DIR}/app")

file(GLOB generated CONFIGURE_DEPENDS "${M6_WORK}/ppc/*.cpp")
if(NOT generated)
  message(FATAL_ERROR "no XenonRecomp sources in ${M6_WORK}/ppc")
endif()
add_library(rcomp_m6_generated STATIC ${generated}
  "${RCOMP_ROOT}/cpu/runtime/indirect.cpp"
  "${RCOMP_ROOT}/cpu/runtime/func_table.cpp"
  "${RCOMP_ROOT}/cpu/runtime/guest_access.cpp"
  "${RCOMP_ROOT}/cpu/runtime/timebase.cpp")
target_include_directories(rcomp_m6_generated PUBLIC "${M6_WORK}/ppc" "${RCOMP_ROOT}/include"
  "${RCOMP_XENONRECOMP}/thirdparty/simde")
target_compile_options(rcomp_m6_generated PRIVATE ${RCOMP_GENERATED_CODE_FLAGS}
  "SHELL:-include ${RCOMP_ROOT}/include/rcomp/ppc_prelude.h" -Wno-unused-label -Wno-unused-variable)
target_link_libraries(rcomp_m6_generated PUBLIC rcomp_runtime rcomp_platform)
target_compile_definitions(rcomp_m6_generated PRIVATE RCOMP_VIRTUAL_GUEST_ACCESS=1)
# Exploratory boot builds only (never a release title): optimisation level of
# the generated code and page-permission checks on every guest access.
set(RCOMP_M6_GENERATED_OPT "" CACHE STRING "Optional -O level appended after RCOMP_GENERATED_CODE_FLAGS")
option(RCOMP_M6_CHECKED_ACCESS "Validate guest page permissions in generated code" OFF)
if(RCOMP_M6_GENERATED_OPT)
  target_compile_options(rcomp_m6_generated PRIVATE ${RCOMP_M6_GENERATED_OPT})
endif()
if(RCOMP_M6_CHECKED_ACCESS)
  target_compile_definitions(rcomp_m6_generated PRIVATE RCOMP_CHECKED_GUEST_ACCESS=1)
endif()
# AOT modules (rcomp/aot_modules.h): tools/m6_inventory.py recompiles the other XEX2 DLLs of the disc into
# ${M6_WORK}/modules/<key>/ppc with their symbols prefixed (generator patch 0022), writes one descriptor per
# module (modules/<key>/aot_module.cpp) and their list (aot_modules.cpp). They compile into this archive
# with every flag and definition of the main module's generated code (target-wide, set here and below);
# main.cpp registers the list. A title without that list builds exactly as before.
set(RCOMP_M6_AOT_MODULES OFF)
if(EXISTS "${M6_WORK}/aot_modules.cpp")
  file(GLOB aot_descriptors CONFIGURE_DEPENDS "${M6_WORK}/modules/*/aot_module.cpp")
  if(NOT aot_descriptors)
    message(FATAL_ERROR "${M6_WORK}/aot_modules.cpp lists AOT modules but no modules/*/aot_module.cpp exists")
  endif()
  set(aot_sources "${M6_WORK}/aot_modules.cpp")
  foreach(descriptor IN LISTS aot_descriptors)
    get_filename_component(aot_dir "${descriptor}" DIRECTORY)
    file(GLOB aot_generated CONFIGURE_DEPENDS "${aot_dir}/ppc/*.cpp")
    if(NOT aot_generated)
      message(FATAL_ERROR "no XenonRecomp sources in ${aot_dir}/ppc")
    endif()
    list(APPEND aot_sources "${descriptor}" ${aot_generated})
  endforeach()
  list(LENGTH aot_descriptors aot_count)
  message(STATUS "rcomp_m6_generated: ${aot_count} AOT module(s) from ${M6_WORK}/modules")
  target_sources(rcomp_m6_generated PRIVATE ${aot_sources})
  set(RCOMP_M6_AOT_MODULES ON)
endif()
option(RCOMP_M6_FUNCTION_RING "Per-thread function-entry ring for the diagnostic sampler/crash reports" OFF)
option(RCOMP_CPU_DIAGNOSTICS "Verbose CPU bring-up diagnostics alongside PC sampling" OFF)
set_source_files_properties("${RCOMP_ROOT}/cpu/runtime/indirect.cpp" PROPERTIES
  COMPILE_DEFINITIONS "RCOMP_CPU_DIAGNOSTICS=$<BOOL:${RCOMP_CPU_DIAGNOSTICS}>")
if(RCOMP_M6_FUNCTION_RING)
  target_compile_definitions(rcomp_m6_generated PRIVATE RCOMP_DIAGNOSTIC_FUNCTION_RING=1)
endif()

# Guest store comparison (include/rcomp/guest_write_tracking.h): the compiled stores inside the physical windows load the old bytes first and mark the page for
# the GPU caches only if they change them (or if the GPU wrote the page). The generated code is compiled with RCOMP_STORE_COMPARE=1; changing the option
# recompiles all of it. OFF is the historic behaviour (every store marks its page).
option(RCOMP_M6_STORE_COMPARE "Guest stores mark a page for the GPU caches only when they change bytes" OFF)
if(RCOMP_M6_STORE_COMPARE)
  target_compile_definitions(rcomp_m6_generated PRIVATE RCOMP_STORE_COMPARE=1)
endif()

# Console layout of the physical windows (runtime/src/physical_window.h): 4 KiB-page physical blocks in 0xE0000000 with the
# console's one-page shift (virtual 0xE0000000 + X = physical X + 0x1000, which XDK Direct3D relies on), 16 MiB ones in 0xC0000000.
# The generated code applies the shift on every access (RCOMP_PHYSICAL_4K_WINDOW_OFFSET, cpu/patches/xenonrecomp/0020, needs the
# patched generator); main.cpp tells the runtime. OFF keeps every physical block at its 0xA0000000 address (GTA IV / EFLC tuning).
option(RCOMP_M6_PHYSICAL_4K_WINDOW_OFFSET "Console physical window layout (0xE0000000 shift) for generated code and runtime" OFF)

add_library(rcomp_m6_title STATIC main.cpp)
if(RCOMP_M6_AOT_MODULES)
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_AOT_MODULES=1)
endif()
if(RCOMP_M6_STORE_COMPARE)
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_STORE_COMPARE=1)
endif()
set(RCOMP_M6_DRAW_RESOLUTION_SCALE 1 CACHE STRING "Integer internal Xenos render scale, 1..7")
if(NOT RCOMP_M6_DRAW_RESOLUTION_SCALE MATCHES "^[1-7]$")
  message(FATAL_ERROR "RCOMP_M6_DRAW_RESOLUTION_SCALE must be one integer from 1 to 7")
endif()
target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_DRAW_RESOLUTION_SCALE=${RCOMP_M6_DRAW_RESOLUTION_SCALE})
if(RCOMP_M6_PHYSICAL_4K_WINDOW_OFFSET)
  target_compile_definitions(rcomp_m6_generated PRIVATE RCOMP_PHYSICAL_4K_WINDOW_OFFSET=1)
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_PHYSICAL_4K_WINDOW_OFFSET=1)
endif()
# Opt-in RADV_DEBUG value set by the title before Vulkan initialization
# (e.g. "nofmask"); empty keeps the driver defaults.
set(RCOMP_M6_RADV_DEBUG "" CACHE STRING "RADV_DEBUG value for the title, empty for driver defaults")
if(RCOMP_M6_RADV_DEBUG)
  if(NOT RCOMP_M6_RADV_DEBUG MATCHES "^[a-z0-9,]+$")
    message(FATAL_ERROR "RCOMP_M6_RADV_DEBUG must be a comma-separated list of RADV debug flags")
  endif()
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_RADV_DEBUG="${RCOMP_M6_RADV_DEBUG}")
endif()
set(RCOMP_M6_RADV_PERFTEST "" CACHE STRING "RADV_PERFTEST value for the title, empty for driver defaults")
if(RCOMP_M6_RADV_PERFTEST)
  if(NOT RCOMP_M6_RADV_PERFTEST MATCHES "^[a-z0-9,]+$")
    message(FATAL_ERROR "RCOMP_M6_RADV_PERFTEST must be a comma-separated list of RADV perftest flags")
  endif()
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_RADV_PERFTEST="${RCOMP_M6_RADV_PERFTEST}")
endif()
# Experiment knobs (never a release setting): NAME=VALUE pairs, comma separated, set as
# environment variables before the title starts any subsystem, e.g.
# "RADV_PS5_WAIT_SLEEP_US=100,RCOMP_KTIME_SPIN_PAUSES=20". Empty keeps every default.
set(RCOMP_M6_TUNING_ENV "" CACHE STRING "Experiment environment: NAME=VALUE pairs separated by commas")
if(RCOMP_M6_TUNING_ENV)
  if(NOT RCOMP_M6_TUNING_ENV MATCHES "^[A-Z][A-Z0-9_]*=[A-Za-z0-9_.:+-]*(,[A-Z][A-Z0-9_]*=[A-Za-z0-9_.:+-]*)*$")
    message(FATAL_ERROR "RCOMP_M6_TUNING_ENV must be NAME=VALUE pairs separated by commas")
  endif()
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_TUNING_ENV="${RCOMP_M6_TUNING_ENV}")
endif()
# Experiment knobs for the Xenos backend's own flags (rex cvars): NAME=VALUE pairs, comma separated,
# applied through the cvar API before the GPU is created, e.g. "execute_unclipped_draw_vs_on_cpu=true".
# Never a release setting by itself: a measured default belongs in gpu/xenos/rexglue/rcomp.
set(RCOMP_M6_XENOS_CVARS "" CACHE STRING "Xenos backend cvar overrides: NAME=VALUE pairs separated by commas")
if(RCOMP_M6_XENOS_CVARS)
  if(NOT RCOMP_M6_XENOS_CVARS MATCHES "^[a-z][a-z0-9_]*=[A-Za-z0-9_.-]+(,[a-z][a-z0-9_]*=[A-Za-z0-9_.-]+)*$")
    message(FATAL_ERROR "RCOMP_M6_XENOS_CVARS must be NAME=VALUE pairs separated by commas")
  endif()
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_XENOS_CVARS="${RCOMP_M6_XENOS_CVARS}")
endif()
target_link_libraries(rcomp_m6_title PUBLIC rcomp_app rcomp_m6_generated)
option(RCOMP_M6_DIRECT_GUEST_PRESENTATION "Present the borrowed Vulkan guest image without CPU readback" OFF)
if(RCOMP_M6_DIRECT_GUEST_PRESENTATION)
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_DIRECT_GUEST_PRESENTATION=1)
endif()
# The frame-rate counter is part of the tool (owner request, 2026-10-02): every game run through it shows its rate. Turn it off only for an A/B that needs it off.
# Launch options menu before the game starts (internal resolution; remembered in /app0/savedata). Every title
# shows it by default; it starts by itself after RCOMP_M6_LAUNCH_MENU_COUNTDOWN seconds without input.
option(RCOMP_M6_LAUNCH_MENU "Launch options menu (resolution) before the game starts" ON)
# Console diagnostic only: exercise the save services on the real storage before the game starts.
option(RCOMP_M6_CONTENT_SELFTEST "Run the save-service self-test before the game starts (diagnostic)" OFF)
target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_CONTENT_SELFTEST=$<BOOL:${RCOMP_M6_CONTENT_SELFTEST}>)
# Xbox 360 hard drive (runtime/docs/HDD.md): raw Partition0/Cache0/Cache1 and the utility-partition file systems,
# backed by /app0/hdd. Per title: GTA IV, TBoGT and GoW2 also mount a utility drive when one exists, so their
# builds keep it OFF (unchanged behaviour); Halo 3 needs it ON for its map cache and saved content.
option(RCOMP_M6_HDD "Present an Xbox 360 hard drive (utility partitions) backed by /app0/hdd" OFF)
target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_HDD=$<BOOL:${RCOMP_M6_HDD}>)
set(RCOMP_M6_LAUNCH_MENU_COUNTDOWN 15 CACHE STRING "Seconds without input before the launch menu starts the game; 0 waits for the player")
if(NOT RCOMP_M6_LAUNCH_MENU_COUNTDOWN MATCHES "^[0-9]+$")
  message(FATAL_ERROR "RCOMP_M6_LAUNCH_MENU_COUNTDOWN must be a non-negative integer")
endif()
target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_LAUNCH_MENU=$<BOOL:${RCOMP_M6_LAUNCH_MENU}>
  RCOMP_M6_LAUNCH_MENU_COUNTDOWN=${RCOMP_M6_LAUNCH_MENU_COUNTDOWN})
option(RCOMP_M6_FPS_COUNTER "Frame-rate counter: an RCOMP-FPS line per second and the rate in the top-left corner of the screen" ON)
if(RCOMP_M6_FPS_COUNTER)
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_FPS_COUNTER=1)
else()
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_FPS_COUNTER=0)
endif()
option(RCOMP_M6_TIMEBASE_OBSERVER "Observe guest timebase, swaps and Xenos vblanks at 1 Hz" OFF)
if(RCOMP_M6_TIMEBASE_OBSERVER)
  target_sources(rcomp_m6_title PRIVATE timebase_observer.cpp)
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_TIMEBASE_OBSERVER=1)
endif()
option(RCOMP_M6_TOPOLOGY_PROBE "Measure which logical CPUs the title may use and how far apart they are (RCOMP-TOPO lines), then boot" OFF)
if(RCOMP_M6_TOPOLOGY_PROBE)
  target_sources(rcomp_m6_title PRIVATE topology_probe.cpp)
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_TOPOLOGY_PROBE=1)
endif()
option(RCOMP_M6_NATIVE_AUDIO_PROBE "Play synthetic native-device tones before diagnostic guest boot" OFF)
if(RCOMP_M6_NATIVE_AUDIO_PROBE)
  if(NOT TARGET rcomp_platform_audio)
    message(FATAL_ERROR "RCOMP_M6_NATIVE_AUDIO_PROBE requires the platform audio backend")
  endif()
  target_sources(rcomp_m6_title PRIVATE native_audio_probe.cpp)
  target_link_libraries(rcomp_m6_title PUBLIC rcomp_platform_audio)
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_NATIVE_AUDIO_PROBE=1)
endif()
set(RCOMP_M6_RENDER_DIAGNOSTICS_START_FRAME 0 CACHE STRING "First CP frame for a bounded diagnostic window")
if(RCOMP_XENOS_RENDER_DIAGNOSTICS)
  if(NOT RCOMP_M6_RENDER_DIAGNOSTICS_START_FRAME MATCHES "^[0-9]+$" OR
     RCOMP_M6_RENDER_DIAGNOSTICS_START_FRAME GREATER 1000000)
    message(FATAL_ERROR "RCOMP_M6_RENDER_DIAGNOSTICS_START_FRAME must be in 0..1000000")
  endif()
  target_compile_definitions(rcomp_m6_title PRIVATE
    RCOMP_M6_RENDER_DIAGNOSTICS_START_FRAME=${RCOMP_M6_RENDER_DIAGNOSTICS_START_FRAME})
endif()
# Exploratory boot title: headless GPU, game files installed outside the title.
option(RCOMP_M6_DIAGNOSTIC_HEADLESS "Headless exploratory title (not a release)" OFF)
set(RCOMP_M6_GAME_ROOT_PATH "" CACHE STRING "Game root on the console for the exploratory title")
if(RCOMP_M6_DIAGNOSTIC_HEADLESS)
  if(NOT RCOMP_M6_GAME_ROOT_PATH)
    message(FATAL_ERROR "RCOMP_M6_GAME_ROOT_PATH is required for the headless diagnostic")
  endif()
  target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_DIAGNOSTIC_HEADLESS
    "RCOMP_M6_GAME_ROOT=\"${RCOMP_M6_GAME_ROOT_PATH}\"")
  # Optional HDD cache partitions (cache: and cache1:, writable) under this directory on the console; empty = none (GTA IV).
  set(RCOMP_M6_CACHE_ROOT_PATH "" CACHE STRING "Directory holding the cache/ and cache1/ partitions of the exploratory title")
  if(RCOMP_M6_CACHE_ROOT_PATH)
    target_compile_definitions(rcomp_m6_title PRIVATE "RCOMP_M6_CACHE_ROOT=\"${RCOMP_M6_CACHE_ROOT_PATH}\"")
  endif()
  # The cache partitions are mounted read-only by default: the background install of the Episodes from Liberty City executable costs it half its frame rate (see TitleConfig::cache_read_only).
  option(RCOMP_M6_CACHE_READ_ONLY "Mount the cache partitions read-only (the title streams from the disc instead of installing archives)" ON)
  if(RCOMP_M6_CACHE_READ_ONLY)
    target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_CACHE_READ_ONLY=1)
  else()
    target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_CACHE_READ_ONLY=0)
  endif()
  set(RCOMP_M6_CAPTURE_INTERVAL_SECONDS 30 CACHE STRING "Interval between diagnostic GPU readbacks; 0 disables capture")
  if(NOT RCOMP_M6_CAPTURE_INTERVAL_SECONDS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "RCOMP_M6_CAPTURE_INTERVAL_SECONDS must be a non-negative integer")
  endif()
  target_compile_definitions(rcomp_m6_title PRIVATE
    RCOMP_M6_CAPTURE_INTERVAL_SECONDS=${RCOMP_M6_CAPTURE_INTERVAL_SECONDS})
  # The 1 kHz PC sampler signals the main guest thread, the bridge, the command
  # processor and one other guest thread about 4000 times a second. ON keeps the
  # existing profiles; OFF is for frame-rate runs that must not be perturbed.
  option(RCOMP_M6_PC_SAMPLER "Start the 1 kHz PC sampler in the headless diagnostic title" ON)
  if(RCOMP_M6_PC_SAMPLER)
    target_compile_definitions(rcomp_m6_title PRIVATE RCOMP_M6_PC_SAMPLER=1)
  endif()
endif()
target_compile_options(rcomp_m6_title PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Werror)
