# Host checks of the frame-rate counter text (no Vulkan, no PS5):
#   cmake -S tests/fps_overlay -B build/tests-fps-overlay -G Ninja && ninja -C build/tests-fps-overlay && ctest --test-dir build/tests-fps-overlay
cmake_minimum_required(VERSION 3.20)
project(rcomp_fps_overlay_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
add_executable(rcomp_fps_overlay_tests test_fps_overlay.cpp)
target_include_directories(rcomp_fps_overlay_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../gpu/xenos/rexglue/rcomp)
target_compile_options(rcomp_fps_overlay_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME fps_overlay COMMAND rcomp_fps_overlay_tests)
