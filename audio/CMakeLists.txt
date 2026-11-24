# OpenAL for the guest: OpenAL Soft built natively (static) + the generated
# guest bridge (openal_bridge.c, see tools/gen_openal_bridge.py).
# Expects TM_ROOT, PCRECOMP and COMMON_DEFS from the including project.
set(LIBTYPE STATIC CACHE STRING "" FORCE)
foreach(o ALSOFT_UTILS ALSOFT_EXAMPLES ALSOFT_TESTS ALSOFT_INSTALL ALSOFT_INSTALL_CONFIG ALSOFT_INSTALL_HRTF_DATA
          ALSOFT_INSTALL_AMBDEC_PRESETS ALSOFT_INSTALL_EXAMPLES ALSOFT_INSTALL_UTILS ALSOFT_UPDATE_BUILD_VERSION)
  set(${o} OFF CACHE BOOL "" FORCE)
endforeach()
set(ALSOFT_NO_CONFIG_UTIL ON CACHE BOOL "" FORCE)
add_subdirectory(${TM_ROOT}/third_party/openal-soft openal-soft EXCLUDE_FROM_ALL)
add_library(tm_audio STATIC ${CMAKE_CURRENT_LIST_DIR}/openal_bridge.c ${CMAKE_CURRENT_LIST_DIR}/audio_pause.c)
target_include_directories(tm_audio PRIVATE ${TM_ROOT}/runtime/include ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(tm_audio PRIVATE ${COMMON_DEFS})
target_compile_options(tm_audio PRIVATE -O2 -g -fno-strict-aliasing)
set_property(TARGET tm_audio PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(tm_audio PUBLIC OpenAL)
