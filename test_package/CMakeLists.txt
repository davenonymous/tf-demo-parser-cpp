# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# Conan's test package: the consumers of tests/consumer/ through find_package.
cmake_minimum_required(VERSION 3.16)
project(tfdemo_test_package LANGUAGES C CXX)

option(CONSUMER_C_API "Build the C consumer as well" ON)
find_package(tfdemo CONFIG REQUIRED)
add_executable(consumer ${CMAKE_CURRENT_LIST_DIR}/../tests/consumer/consumer.cpp)
target_link_libraries(consumer PRIVATE tfdemo::tfdemo)
set_target_properties(consumer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(CONSUMER_C_API)
  add_executable(consumer_c ${CMAKE_CURRENT_LIST_DIR}/../tests/consumer/consumer.c)
  target_link_libraries(consumer_c PRIVATE tfdemo::tfdemo_c)
endif()
