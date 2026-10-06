# R-comp game shell (owner: PRIME). Added as a subdirectory by a title (or a
# test) that already added platform/, runtime/ and gpu/xenos/rexglue/.
add_library(rcomp_app STATIC ${CMAKE_CURRENT_LIST_DIR}/src/title.cpp ${CMAKE_CURRENT_LIST_DIR}/src/title_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/launch_menu.cpp ${CMAKE_CURRENT_LIST_DIR}/src/content_selftest.cpp)
target_include_directories(rcomp_app PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(rcomp_app PUBLIC cxx_std_23)  # rexglue headers use std::byteswap
# The controller backend of the platform (PS5: scePad; host: none).
target_link_libraries(rcomp_app PUBLIC rcomp_runtime_video rcomp_runtime rcomp_xenos_vulkan rcomp_platform_input
  rcomp_platform)
# hle_xam.cpp (in rcomp_runtime) calls the controller backend: in a title the
# backend follows the runtime on the link line.
target_link_libraries(rcomp_runtime INTERFACE rcomp_platform_input)
