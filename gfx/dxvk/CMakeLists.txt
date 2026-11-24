# DXVK native 1.9.2b (d3d11 + dxgi + dxbc) for this port.
#   Android arm64: static archive linked into libtmgfx.so
#   Linux host   : headless static archive for shader/device checks
cmake_minimum_required(VERSION 3.18)
project(tm_dxvk C CXX)
set(DXVK_SOURCE "" CACHE PATH "Source prepared by prepare.py + shaders.py")
if(NOT EXISTS "${DXVK_SOURCE}/PINNED_REVISION" OR NOT EXISTS "${DXVK_SOURCE}/generated")
  message(FATAL_ERROR "Run gfx/dxvk/prepare.py and shaders.py first (DXVK_SOURCE=${DXVK_SOURCE})")
endif()
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/prepare.py" WANT_SHA)
file(READ "${DXVK_SOURCE}/PREPARE_SHA256" HAVE_SHA)
string(STRIP "${HAVE_SHA}" HAVE_SHA)
if(NOT WANT_SHA STREQUAL HAVE_SHA)
  message(FATAL_ERROR "DXVK source is stale: rerun gfx/dxvk/prepare.py")
endif()
set(S "${DXVK_SOURCE}/src")
file(GLOB UTIL "${S}/util/*.cpp" "${S}/util/com/*.cpp" "${S}/util/config/*.cpp" "${S}/util/log/*.cpp"
  "${S}/util/sha1/*.c" "${S}/util/sha1/*.cpp" "${S}/util/sync/*.cpp")
list(FILTER UTIL EXCLUDE REGEX "util_gdi.cpp$")
list(APPEND UTIL "${S}/util/platform/util_env_linux.cpp" "${S}/util/platform/util_luid_linux.cpp"
  "${S}/util/platform/util_string_linux.cpp" "${S}/util/platform/thread_native.cpp")
file(GLOB CORE "${S}/spirv/*.cpp" "${S}/vulkan/*.cpp" "${S}/dxvk/*.cpp" "${S}/dxvk/hud/*.cpp" "${S}/dxbc/*.cpp")
list(FILTER CORE EXCLUDE REGEX "dxvk_open(vr|xr).cpp$")
file(GLOB D3D11 "${S}/d3d11/*.cpp" "${S}/d3d10/*.cpp" "${S}/dxgi/*.cpp")
list(FILTER D3D11 EXCLUDE REGEX "d3d11_gdi.cpp$|d3d10_main.cpp$|d3d10_reflection.cpp$|d3d10_state_block.cpp$")
add_library(tm_dxvk STATIC ${UTIL} ${CORE} ${D3D11} tm_wsi.cpp)
target_compile_features(tm_dxvk PUBLIC cxx_std_17)
target_compile_definitions(tm_dxvk PUBLIC DXVK_NATIVE DXVK_PLATFORM_LINUX DXVK_WSI_TM NOMINMAX)
if(ANDROID)
  target_compile_definitions(tm_dxvk PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
endif()
target_include_directories(tm_dxvk PUBLIC "${DXVK_SOURCE}" "${DXVK_SOURCE}/include" "${S}/util"
  "${DXVK_SOURCE}/include/native" "${DXVK_SOURCE}/include/native/windows"
  "${DXVK_SOURCE}/include/native/directx" "${DXVK_SOURCE}/generated")
target_compile_options(tm_dxvk PRIVATE -O2 -w -fvisibility=hidden)
set_property(TARGET tm_dxvk PROPERTY POSITION_INDEPENDENT_CODE ON)
