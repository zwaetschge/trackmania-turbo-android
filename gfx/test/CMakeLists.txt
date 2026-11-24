# Host (headless) build of the renderer self-test.
cmake_minimum_required(VERSION 3.18)
project(tm_gfx_selftest C CXX)
set(DXVK_SOURCE "" CACHE PATH "prepared DXVK source")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../dxvk dxvk)
add_executable(gfx_selftest gfx_selftest.cpp)
find_library(X11LIB X11 REQUIRED)
target_link_libraries(gfx_selftest PRIVATE tm_dxvk vulkan ${X11LIB} pthread dl)
target_compile_options(gfx_selftest PRIVATE -O1 -g -Wall -Wno-unused-function)
