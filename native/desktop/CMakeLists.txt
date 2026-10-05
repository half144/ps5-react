# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
cmake_minimum_required(VERSION 3.16)
project(ps5-react-preview C CXX)
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
get_target_property(SDL_INCLUDES SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
foreach(SDL_INCLUDE IN LISTS SDL_INCLUDES)
  if(EXISTS "${SDL_INCLUDE}/SDL.h")
    get_filename_component(SDL_PARENT "${SDL_INCLUDE}" DIRECTORY)
    include_directories("${SDL_PARENT}")
  endif()
endforeach()
# Same engine options on desktop and PS5, rather than the upstream demo's profile.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../ps5" runtime)
add_executable(ps5-react-preview host.cpp host_platform.cpp ../shared/host_api.cpp ../shared/gl_presenter.cpp ../shared/frame_stats.cpp ../shared/damage_tracker.cpp ../shared/input_script.cpp ../shared/scroll_layer.cpp "${APP_GENERATED}/assets.generated.c")
target_compile_features(ps5-react-preview PRIVATE cxx_std_20)
target_compile_options(ps5-react-preview PRIVATE -Wall -Wextra -Werror)
target_include_directories(ps5-react-preview PRIVATE ../shared "${APP_GENERATED}")
if(APPLE)
  target_compile_definitions(ps5-react-preview PRIVATE GL_SILENCE_DEPRECATION)
endif()
target_link_libraries(ps5-react-preview PRIVATE SDL2::SDL2 OpenGL::GL er-bridge-quickjs er-software)
