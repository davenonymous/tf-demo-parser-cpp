# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# A project that uses libtfdemo as a consumer would, for CI: by default
# through find_package (an install, vcpkg), with -DCONSUMER_FETCH=ON through
# FetchContent of TFDEMO_SOURCE_DIR (a release's source tarball works the
# same way with URL).
cmake_minimum_required(VERSION 3.16)
project(tfdemo_consumer LANGUAGES C CXX)

option(CONSUMER_FETCH "Take tfdemo with FetchContent from TFDEMO_SOURCE_DIR" OFF)
option(CONSUMER_C_API "Build the C consumer as well" ON)
if(CONSUMER_FETCH)
  include(FetchContent)
  FetchContent_Declare(tfdemo SOURCE_DIR "${TFDEMO_SOURCE_DIR}")
  FetchContent_MakeAvailable(tfdemo)
else()
  find_package(tfdemo 1 CONFIG REQUIRED)
endif()

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE tfdemo::tfdemo)
set_target_properties(consumer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

if(CONSUMER_C_API)
  add_executable(consumer_c consumer.c)
  target_link_libraries(consumer_c PRIVATE tfdemo::tfdemo_c)
endif()
