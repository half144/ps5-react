# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
cmake_minimum_required(VERSION 3.16)
project(overdrive-react-ps5 C)
if(NOT ER_ROOT)
  message(FATAL_ERROR "ER_ROOT must name the pinned embedded-react checkout")
endif()
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ERUI_BORDER_AA ON CACHE BOOL "" FORCE)
set(ERUI_TRANSFORMS FULL CACHE STRING "" FORCE)
set(ERUI_SCRATCH_W 512 CACHE STRING "" FORCE)
set(ERUI_SCRATCH_H 512 CACHE STRING "" FORCE)
# Motion allocates one Animated.Value per animated key per element (716 B each with 8 bindings). A
# search over a large catalog mounts enough animated cards at once to exhaust 256.
set(ERUI_MAX_ANIM_VALUES 1024 CACHE STRING "" FORCE)
set(ERUI_MAX_VALUE_BINDINGS 8 CACHE STRING "" FORCE)
# Photos are baked at half size and drawn up to 2560 px wide: bilinear, one full-width row in the
# scaler and gradient pools (+7 KB each over 800 px), and 256 image slots (+11 KB at 88 B per slot).
set(ERUI_BILINEAR_SCALE ON CACHE BOOL "" FORCE)
set(ERUI_MAX_IMG_ROW_PIXELS 2560 CACHE STRING "" FORCE)
set(ERUI_IMAGE_REGISTRY_MAX 256 CACHE STRING "" FORCE)
# View backgroundGradient (linear and radial; also <Svg> gradient fills): about 5 KB of code, an 8 KB
# colour ramp and a 10 KB row per render worker; nodes without a gradient pay nothing per frame.
set(ERUI_GRADIENT ON CACHE BOOL "" FORCE)
set(ERUI_GRADIENT_RADIAL ON CACHE BOOL "" FORCE)
# Icons are <Svg> nodes: 128 geometry slots (4.5 KB each) and 32 cached static nodes (84 KB each),
# about 3.1 MB of .bss over the 8/2 defaults, so icons are not re-flattened on nearby repaints. 64
# slots ran out in Overdrive (its hidden library page keeps a heart per favorited card mounted), and
# the legend glyphs past the 64th drew nothing.
set(ERUI_MAX_VECTOR_NODES 128 CACHE STRING "" FORCE)
set(ERUI_VECTOR_CACHE_NODES 32 CACHE STRING "" FORCE)
# Per-frame js/layout/raster split for the frame log (native/shared/frame_stats.cpp): a clock read
# per phase, per backend blit and per bridge marshal call; no overlay is drawn.
set(ERUI_PERF_STATS ON CACHE BOOL "" FORCE)
# A screen transition keeps the leaving screen mounted while the next one is created, so two full
# screens must fit at once; the bridge needs a handle per node. Node creation fails silently past this.
set(ERUI_MAX_NODES 2048 CACHE STRING "" FORCE)
foreach(FEATURE SMOKE RUNTEST SPLITTEST GCTEST COMPILE)
  set(ER_BRIDGE_QUICKJS_${FEATURE} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${ER_ROOT}/bridges/quickjs" bridge)
# Every running motion key holds an engine animation and a completion slot; lift both pools from 64.
target_compile_definitions(embedded-react PRIVATE ERUI_MAX_ANIMATIONS=256)
target_compile_definitions(er-bridge-quickjs PRIVATE ER_BRIDGE_MAX_ANIM_COMPLETIONS=256 ER_BRIDGE_MAX_HANDLES=2048)
get_target_property(ENGINE_LIBRARIES embedded-react INTERFACE_LINK_LIBRARIES)
list(REMOVE_ITEM ENGINE_LIBRARIES m)
set_property(TARGET embedded-react PROPERTY INTERFACE_LINK_LIBRARIES "${ENGINE_LIBRARIES}")
add_library(er-software STATIC "${ER_ROOT}/backends/software/renderer_backend.c")
target_include_directories(er-software PUBLIC "${ER_ROOT}/backends/software")
target_link_libraries(er-software PUBLIC embedded-react)
