# Host checks of the launch options menu model (no Vulkan, no PS5):
#   cmake -S tests/launch_menu -B build/tests-launch-menu -G Ninja && ninja -C build/tests-launch-menu && ctest --test-dir build/tests-launch-menu
cmake_minimum_required(VERSION 3.20)
project(rcomp_launch_menu_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
add_executable(rcomp_launch_menu_tests test_launch_menu.cpp ${CMAKE_CURRENT_LIST_DIR}/../../app/src/launch_menu.cpp)
target_include_directories(rcomp_launch_menu_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../app/include
                           ${CMAKE_CURRENT_LIST_DIR}/../../include)
target_compile_options(rcomp_launch_menu_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME launch_menu COMMAND rcomp_launch_menu_tests)
