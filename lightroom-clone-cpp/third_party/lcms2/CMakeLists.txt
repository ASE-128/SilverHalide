# Minimal CMake build for vendored Little-CMS 2 (static library).
# The official lcms2 release zip ships autotools/meson only; this script
# gives the `lcms2` target consumed by the top-level CMakeLists.
cmake_minimum_required(VERSION 3.25)

project(lcms2 C)

file(GLOB LCMS2_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/cms*.c")

add_library(lcms2 STATIC ${LCMS2_SOURCES})
target_include_directories(lcms2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(lcms2 PRIVATE _CRT_SECURE_NO_WARNINGS)

if(NOT MSVC)
  target_compile_options(lcms2 PRIVATE -w)
endif()
