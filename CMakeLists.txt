# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# libtfdemo: the C++ library (tfdemo::tfdemo), its C API as a shared library
# (tfdemo::tfdemo_c), the tfdemo command line tool, the tests and the
# fuzzers. Consumed with find_package(tfdemo) after an install, with
# FetchContent or add_subdirectory (tools and tests are then off), or with
# pkg-config (tfdemo-cpp.pc for libtfdemo-cpp, tfdemo.pc for the C API's
# libtfdemo).
cmake_minimum_required(VERSION 3.16)

# The version is written once, in include/tfdemo/version.hpp.
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/include/tfdemo/version.hpp TFDEMO_VERSION_LINE
  REGEX "^#define TFDEMO_VERSION_STRING \"[0-9]+\\.[0-9]+\\.[0-9]+\"$")
string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" TFDEMO_VERSION "${TFDEMO_VERSION_LINE}")
if(NOT TFDEMO_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "include/tfdemo/version.hpp holds no TFDEMO_VERSION_STRING")
endif()

project(tfdemo
  VERSION ${TFDEMO_VERSION}
  DESCRIPTION "Reads and writes Team Fortress 2 demo files"
  HOMEPAGE_URL "https://github.com/davenonymous/tf-demo-parser-cpp"
  LANGUAGES C CXX)

include(GNUInstallDirs)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(TFDEMO_IS_TOP_LEVEL ON)
else()
  set(TFDEMO_IS_TOP_LEVEL OFF)
endif()

option(TFDEMO_BUILD_C_API "Build the C API shared library" ON)
option(TFDEMO_BUILD_TOOLS "Build the tfdemo command line tool" ${TFDEMO_IS_TOP_LEVEL})
option(TFDEMO_BUILD_TESTS "Build the tests" ${TFDEMO_IS_TOP_LEVEL})
option(TFDEMO_BUILD_FUZZERS "Build the libFuzzer targets (clang only)" OFF)
option(TFDEMO_INSTALL "Generate the install rules" ${TFDEMO_IS_TOP_LEVEL})
option(TFDEMO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(TFDEMO_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(TFDEMO_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  if(TFDEMO_WARNINGS_AS_ERRORS)
    list(APPEND TFDEMO_WARNINGS -Werror)
  endif()
elseif(MSVC)
  set(TFDEMO_WARNINGS /W4 /permissive-)
  if(TFDEMO_WARNINGS_AS_ERRORS)
    list(APPEND TFDEMO_WARNINGS /WX)
  endif()
endif()

# 32-bit x86 GCC and Clang do float math on the x87 unit by default, whose
# 80-bit intermediates give other bits than the engine (bit normals, coords).
# SSE2 math gives the same bits on every platform; consumers get it too, so
# that the float code inline in the headers agrees with the library.
set(TFDEMO_FLOAT_MATH_FLAGS "")
if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i[3-6]86|AMD64|x86_64)$"
   AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(TFDEMO_FLOAT_MATH_FLAGS -msse2 -mfpmath=sse)
endif()

file(GLOB TFDEMO_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cpp)
list(FILTER TFDEMO_SOURCES EXCLUDE REGEX ".*/c_api\\.cpp$")

# The C++ library, static unless BUILD_SHARED_LIBS asks for a shared one.
add_library(tfdemo ${TFDEMO_SOURCES})
add_library(tfdemo::tfdemo ALIAS tfdemo)
target_compile_features(tfdemo PUBLIC cxx_std_17)
target_include_directories(tfdemo PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(tfdemo PRIVATE ${TFDEMO_WARNINGS})
target_compile_options(tfdemo PUBLIC ${TFDEMO_FLOAT_MATH_FLAGS})
set_target_properties(tfdemo PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})
# libtfdemo-cpp, so that it never stands in for the C API's libtfdemo.
set_target_properties(tfdemo PROPERTIES OUTPUT_NAME tfdemo-cpp)
get_target_property(TFDEMO_LIBRARY_TYPE tfdemo TYPE)
if(TFDEMO_LIBRARY_TYPE STREQUAL "SHARED_LIBRARY")
  # The C++ API has no export annotations: a shared build exports it all.
  set_target_properties(tfdemo PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
  # Hidden, so that the C library built from it exports only tfdemo_*.
  set_target_properties(tfdemo PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
endif()

# The C API: a shared library exporting only the tfdemo_* functions.
if(TFDEMO_BUILD_C_API)
  add_library(tfdemo_c SHARED src/c_api.cpp)
  add_library(tfdemo::tfdemo_c ALIAS tfdemo_c)
  target_link_libraries(tfdemo_c PRIVATE tfdemo)
  target_include_directories(tfdemo_c PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_definitions(tfdemo_c PRIVATE TFDEMO_BUILDING_C_API)
  target_compile_options(tfdemo_c PRIVATE ${TFDEMO_WARNINGS})
  target_compile_options(tfdemo_c PUBLIC ${TFDEMO_FLOAT_MATH_FLAGS})
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(tfdemo_c PRIVATE "-Wl,--version-script=${PROJECT_SOURCE_DIR}/src/c_api.map")
    set_property(TARGET tfdemo_c APPEND PROPERTY LINK_DEPENDS ${PROJECT_SOURCE_DIR}/src/c_api.map)
  endif()
  set_target_properties(tfdemo_c PROPERTIES
    OUTPUT_NAME tfdemo
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

if(TFDEMO_BUILD_TOOLS)
  add_executable(tfdemo_cli tools/tfdemo/main.cpp tools/tfdemo/json.cpp)
  target_link_libraries(tfdemo_cli PRIVATE tfdemo)
  target_compile_options(tfdemo_cli PRIVATE ${TFDEMO_WARNINGS})
  set_target_properties(tfdemo_cli PROPERTIES OUTPUT_NAME tfdemo)
endif()

if(TFDEMO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TFDEMO_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

if(TFDEMO_INSTALL)
  include(CMakePackageConfigHelpers)
  set(TFDEMO_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/tfdemo)

  set(TFDEMO_INSTALL_TARGETS tfdemo)
  if(TFDEMO_BUILD_C_API)
    list(APPEND TFDEMO_INSTALL_TARGETS tfdemo_c)
  endif()
  install(TARGETS ${TFDEMO_INSTALL_TARGETS} EXPORT tfdemoTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  if(TFDEMO_BUILD_TOOLS)
    install(TARGETS tfdemo_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  install(FILES LICENSE.txt licenses/tf-demo-parser.txt DESTINATION ${CMAKE_INSTALL_DOCDIR})

  install(EXPORT tfdemoTargets NAMESPACE tfdemo:: DESTINATION ${TFDEMO_CMAKE_DIR})
  configure_package_config_file(cmake/tfdemoConfig.cmake.in ${PROJECT_BINARY_DIR}/tfdemoConfig.cmake
    INSTALL_DESTINATION ${TFDEMO_CMAKE_DIR})
  write_basic_package_version_file(${PROJECT_BINARY_DIR}/tfdemoConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
  install(FILES ${PROJECT_BINARY_DIR}/tfdemoConfig.cmake ${PROJECT_BINARY_DIR}/tfdemoConfigVersion.cmake
    DESTINATION ${TFDEMO_CMAKE_DIR})

  # Relocatable for relative directories: the prefix is the path from the .pc
  # file up to it. An absolute library or include directory (as distributions
  # pass) is not below the prefix, so then the .pc files name the full paths.
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}" OR IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(TFDEMO_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
    set(TFDEMO_PC_LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}")
    set(TFDEMO_PC_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
  else()
    file(RELATIVE_PATH pc_up "/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/prefix")
    string(REGEX REPLACE "/$" "" pc_up "${pc_up}")
    set(TFDEMO_PC_PREFIX "\${pcfiledir}/${pc_up}")
    set(TFDEMO_PC_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
    set(TFDEMO_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
  endif()
  list(JOIN TFDEMO_FLOAT_MATH_FLAGS " " TFDEMO_PC_CFLAGS)
  if(TFDEMO_PC_CFLAGS)
    string(PREPEND TFDEMO_PC_CFLAGS " ")
  endif()
  configure_file(cmake/tfdemo-cpp.pc.in ${PROJECT_BINARY_DIR}/tfdemo-cpp.pc @ONLY)
  install(FILES ${PROJECT_BINARY_DIR}/tfdemo-cpp.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
  if(TFDEMO_BUILD_C_API)
    configure_file(cmake/tfdemo.pc.in ${PROJECT_BINARY_DIR}/tfdemo.pc @ONLY)
    install(FILES ${PROJECT_BINARY_DIR}/tfdemo.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
  endif()
endif()
