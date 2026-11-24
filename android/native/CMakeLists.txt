# libtmturbo.so: portable runtime + lifted TrackmaniaTurbo.exe + D3D11 bridge over
# DXVK for arm64-v8a. Built locally (tools/build_android_so.sh); the result is
# copied into app/src/main/jniLibs and is never committed (derived from the game).
cmake_minimum_required(VERSION 3.22)
project(tmturbo C CXX)
set(TM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(PCRECOMP ${TM_ROOT}/third_party/pcrecomp)
set(GEN_DIR "" CACHE PATH "generated recomp_*.c")
set(STUB OFF CACHE BOOL "stub dispatch (runtime-only build)")
set(DXVK_SOURCE ${TM_ROOT}/work/dxvk-source CACHE PATH "prepared DXVK")
file(GLOB RT_SOURCES CONFIGURE_DEPENDS ${TM_ROOT}/runtime/src/*.c)
# RECOMP_CPU_STRUCT + RECOMP_CPU_TLS_SLOT=2: the guest machine state is one object per
# thread, found through bionic's TLS_SLOT_APP (free for apps since Android 10). Separate
# __thread variables cost a call into the linker's TLS resolver per access in a shared
# library -- 30 % of the game's main thread before this.
set(COMMON_DEFS RECOMP_STRICT_ICALL RECOMP_X87_WINDOW "RECOMP_TLS=__thread" RECOMP_CPU_STRUCT RECOMP_CPU_TLS_SLOT=2 RECOMP_FAST_DISPATCH)

# DXVK backend (d3d11+dxgi) + Android WSI + the guest<->native D3D11 bridge
add_subdirectory(${TM_ROOT}/gfx/dxvk dxvk)
add_library(tm_bridge STATIC ${TM_ROOT}/gfx/d3d11_bridge.cpp)
target_include_directories(tm_bridge PRIVATE ${TM_ROOT}/runtime/include ${PCRECOMP}/runtime/recomp32)
target_link_libraries(tm_bridge PRIVATE tm_dxvk)
target_compile_definitions(tm_bridge PRIVATE ${COMMON_DEFS})
target_compile_options(tm_bridge PRIVATE -O2 -g -w -fno-strict-aliasing -fsigned-char)
set_property(TARGET tm_bridge PROPERTY POSITION_INDEPENDENT_CODE ON)

if(STUB)
  set(GEN_OBJS ${TM_ROOT}/work/stubgen/stub_dispatch.c)
else()
  file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${GEN_DIR}/recomp_*.c ${TM_ROOT}/gen/extra/extra_recomp_*.c
       ${TM_ROOT}/gen/am/am_recomp_*.c ${TM_ROOT}/gen/wp/wp_recomp_*.c)
  add_library(tm_guest OBJECT ${GEN_SOURCES})
  target_include_directories(tm_guest PRIVATE ${PCRECOMP}/runtime/recomp32 ${GEN_DIR} ${TM_ROOT}/gen/extra)
  target_compile_definitions(tm_guest PRIVATE ${COMMON_DEFS} RECOMP_LOCAL_REGS "RECOMP_MEM_QUAL=")
  set(GUEST_OPT "-O1" CACHE STRING "optimisation for generated code")
  # No unwind tables for the lifted C: nothing unwinds through guest frames, and
  # the tables alone pushed an -O0 link past the 2 GB relocation range.
  target_compile_options(tm_guest PRIVATE ${GUEST_OPT} -w -fwrapv -fno-strict-aliasing -ffp-contract=off -g0 -fvisibility=hidden
      -fno-unwind-tables -fno-asynchronous-unwind-tables
      -include ${TM_ROOT}/runtime/include/guest_prelude.h)
  # The files with the hottest lifted code (config/hot_files.txt, from a race profile) get a
  # stronger optimisation; the source property comes after the target options, so it wins.
  set(GUEST_HOT_OPT "" CACHE STRING "optimisation for the hottest generated files (e.g. -O2; empty = GUEST_OPT)")
  if(EXISTS ${TM_ROOT}/config/hot_files.txt AND NOT GUEST_HOT_OPT STREQUAL "")
    file(STRINGS ${TM_ROOT}/config/hot_files.txt HOT_LINES REGEX "^[a-z]")
    foreach(h ${HOT_LINES})
      string(STRIP "${h}" h)
      if(EXISTS ${GEN_DIR}/${h})
        set_source_files_properties(${GEN_DIR}/${h} PROPERTIES COMPILE_OPTIONS "${GUEST_HOT_OPT}")
      endif()
    endforeach()
  endif()
  set(GEN_OBJS $<TARGET_OBJECTS:tm_guest>)
endif()
add_library(tmturbo SHARED ${CMAKE_CURRENT_SOURCE_DIR}/jni_main.c ${RT_SOURCES} ${GEN_OBJS})
target_include_directories(tmturbo PRIVATE ${TM_ROOT}/runtime/include ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(tmturbo PRIVATE ${COMMON_DEFS})
# -fsigned-char: plain char is unsigned on arm64 Linux/Android but signed on the x86 hosts the
# runtime is developed on; keep the two builds identical.
target_compile_options(tmturbo PRIVATE -O2 -g -Wall -Wno-unused-parameter -Wno-missing-field-initializers -fno-strict-aliasing -fsigned-char)
# OpenAL (native OpenAL Soft behind the guest's OpenAL32.dll)
add_subdirectory(${TM_ROOT}/audio audio)
target_compile_definitions(tmturbo PRIVATE TM_HAVE_OPENAL)
target_link_libraries(tmturbo PRIVATE tm_bridge tm_dxvk tm_audio vulkan log android m)
# Native VP8 decoder for the game's video (runtime/src/video.c); built by tools/build_libvpx.sh.
set(TM_VPX_LIB ${TM_ROOT}/work/libvpx-android/libvpx.a)
if(EXISTS ${TM_VPX_LIB})
  target_compile_definitions(tmturbo PRIVATE TM_HAVE_VPX)
  target_include_directories(tmturbo PRIVATE ${TM_ROOT}/third_party/libvpx ${TM_ROOT}/work/libvpx-android)
  target_link_libraries(tmturbo PRIVATE ${TM_VPX_LIB})
else()
  message(WARNING "work/libvpx-android/libvpx.a not found: the game's own (slow) VP8 decoder will be used")
endif()
target_link_options(tmturbo PRIVATE "-Wl,-z,max-page-size=16384")
set_target_properties(tmturbo PROPERTIES LINKER_LANGUAGE CXX)
