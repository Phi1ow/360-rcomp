#!/usr/bin/env bash
# R-comp shelf: builds the PC preview (build/host/shelf_host) and the host tests (build/host/shelf_tests).
#
#   tools/build_host.sh
#
# Needs clang++ (C++20) and the Vulkan headers (VULKAN_HEADERS: the folder holding vulkan/vulkan.h). On Windows it
# runs under Cygwin and loads vulkan-1.dll at run time; elsewhere libvulkan.so.1.
#
# Copyright (C) 2026 R-comp contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
obj="$root/build/host/obj"
CXX=${CXX:-clang++}
CC=${CC:-clang}
: "${VULKAN_HEADERS:?set VULKAN_HEADERS to the folder holding vulkan/vulkan.h}"
[[ -f $VULKAN_HEADERS/vulkan/vulkan.h ]] || { echo "BLOCKED: no $VULKAN_HEADERS/vulkan/vulkan.h" >&2; exit 2; }
mkdir -p "$obj"

flags=(-std=gnu++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -Wno-unused-parameter
  -I"$root/src/frontend" -I"$root/src/catalog" -I"$root/src/shelf" -I"$VULKAN_HEADERS")
sources=(frontend/fe_app frontend/fe_covers frontend/fe_renderer frontend/fe_text frontend/fe_vk frontend/fe_i18n frontend/fe_sound
  catalog/xex catalog/json catalog/x360db catalog/fsutil catalog/titles catalog/install shelf/session)
objs=()
for src in "${sources[@]}"; do
  o="$obj/${src//\//_}.o"
  if [[ ! -f $o || $root/src/$src.cpp -nt $o || -n $(find "$root/src" -name '*.h' -newer "$o" -print -quit) ]]; then
    "$CXX" "${flags[@]}" -c "$root/src/$src.cpp" -o "$o"
  fi
  objs+=("$o")
done
"$CXX" "${flags[@]}" -c "$root/src/host/shelf_host.cpp" -o "$obj/shelf_host.o"
"$CXX" -o "$root/build/host/shelf_host" "$obj/shelf_host.o" "${objs[@]}" -lpthread
echo "built $root/build/host/shelf_host"
if [[ -f $root/tests/shelf_tests.cpp ]]; then
  "$CXX" "${flags[@]}" -c "$root/tests/shelf_tests.cpp" -o "$obj/shelf_tests.o"
  "$CXX" -o "$root/build/host/shelf_tests" "$obj/shelf_tests.o" "${objs[@]}" -lpthread
  echo "built $root/build/host/shelf_tests"
fi
