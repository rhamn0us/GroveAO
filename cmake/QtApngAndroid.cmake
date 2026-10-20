# Builds the QtApng image plugin (APNG support) as a *static* Qt plugin and
# links it into the Android app. Android can't load loose imageformats plugins
# from disk, so a static plugin is the simplest reliable option; Qt picks it up
# automatically through the generated plugin-import object.
#
# QtApng is expected in ./qtapng (same location configure.sh uses on desktop).
# scripts/build-android.sh clones it there when missing. Override with
# -DAO_QTAPNG_DIR=/path/to/QtApng.

set(AO_QTAPNG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/qtapng" CACHE PATH "Path to a QtApng checkout")

set(_apng_plugin_dir "${AO_QTAPNG_DIR}/src/plugins/imageformats/apng")
if(NOT EXISTS "${_apng_plugin_dir}/apngimageplugin.cpp")
  message(FATAL_ERROR
    "QtApng not found at ${AO_QTAPNG_DIR}.\n"
    "Run scripts/build-android.sh (it clones https://github.com/jurplel/QtApng), "
    "or pass -DAO_QTAPNG_DIR=<path to QtApng>.")
endif()

# libpng (with the APNG patch) comes from QtApng's bundled sources. Zlib is
# provided by the NDK sysroot; QtApng's own zlib subproject is only declared
# (EXCLUDE_FROM_ALL), never built. libpng's CMake already skips tests/tools
# when cross-compiling for Android.
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_EXECUTABLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${AO_QTAPNG_DIR}/src/3rdparty" "${CMAKE_BINARY_DIR}/qtapng-3rdparty" EXCLUDE_FROM_ALL)

qt_add_plugin(qapng STATIC
  PLUGIN_TYPE imageformats
  CLASS_NAME ApngImagePlugin)

target_sources(qapng PRIVATE
  "${_apng_plugin_dir}/apngimagehandler_p.h"
  "${_apng_plugin_dir}/apngimagehandler.cpp"
  "${_apng_plugin_dir}/apngimageplugin.h"
  "${_apng_plugin_dir}/apngimageplugin.cpp"
  "${_apng_plugin_dir}/apngreader_p.h"
  "${_apng_plugin_dir}/apngreader.cpp")

set_target_properties(qapng PROPERTIES AUTOMOC ON)

get_target_property(_png_include png_static INCLUDE_DIRECTORIES)
target_include_directories(qapng PRIVATE "${_apng_plugin_dir}" ${_png_include})
target_link_libraries(qapng PRIVATE
  Qt6::Core
  Qt6::Gui
  png_static)
