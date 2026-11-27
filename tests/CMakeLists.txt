# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# The unit tests (tfdemo_tests), the C API test (tfdemo_c_test) and the
# check of the tool's output against docs/schema/. Real demos make them do
# more: TFDEMO_TEST_DEMOS names a directory of .dem files, or
# TFDEMO_TEST_DATA=ON downloads the anonymized match of the test-data
# release (tests/data/README.md) and uses it. tests/data/client-start.dem is
# always tested.
set(TFDEMO_TEST_DEMOS "" CACHE PATH "A directory of demos for the tests (optional)")
option(TFDEMO_TEST_DATA "Download the test data demos when TFDEMO_TEST_DEMOS is not set" OFF)
set(TFDEMO_TEST_DATA_URL
  "https://github.com/davenonymous/tf-demo-parser-cpp/releases/download/test-data-2/tf-demo-parser-cpp-test-data-2.tar.xz"
  CACHE STRING "Where the test data archive is downloaded from (file:// for a local copy)")
set(TFDEMO_TEST_DATA_DIR "${PROJECT_BINARY_DIR}/test-data" CACHE PATH "Where the test data is kept between builds")
set(TFDEMO_TEST_DATA_SHA256 2c06804e7e96440a237ea600fd84e2cfc5517c140b2ff79512a8ee11548a97b4)

# The archive keeps the file name of its URL. It is unpacked into a directory
# named after its SHA-256, so that new test data is unpacked afresh and the
# demos of other test data are never used.
set(test_demos "${TFDEMO_TEST_DEMOS}")
if(NOT test_demos AND TFDEMO_TEST_DATA)
  get_filename_component(archive_name "${TFDEMO_TEST_DATA_URL}" NAME)
  if(archive_name STREQUAL "")
    message(FATAL_ERROR "TFDEMO_TEST_DATA_URL names no file: ${TFDEMO_TEST_DATA_URL}")
  endif()
  set(archive "${TFDEMO_TEST_DATA_DIR}/${archive_name}")
  set(have_archive OFF)
  if(EXISTS "${archive}")
    file(SHA256 "${archive}" archive_hash)
    if(archive_hash STREQUAL TFDEMO_TEST_DATA_SHA256)
      set(have_archive ON)
    endif()
  endif()
  if(NOT have_archive)
    message(STATUS "Downloading the test data from ${TFDEMO_TEST_DATA_URL}")
    file(DOWNLOAD "${TFDEMO_TEST_DATA_URL}" "${archive}" EXPECTED_HASH SHA256=${TFDEMO_TEST_DATA_SHA256}
      STATUS download_status)
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
      message(FATAL_ERROR "The test data could not be downloaded: ${download_status}")
    endif()
  endif()
  # Unpacked into a directory of its own, renamed once complete, so that an
  # interrupted unpacking is never taken for the test data.
  set(test_demos "${TFDEMO_TEST_DATA_DIR}/${TFDEMO_TEST_DATA_SHA256}/demos")
  if(NOT IS_DIRECTORY "${test_demos}")
    set(unpacking "${test_demos}.unpacking")
    file(REMOVE_RECURSE "${unpacking}")
    file(MAKE_DIRECTORY "${unpacking}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${archive}" WORKING_DIRECTORY "${unpacking}"
      RESULT_VARIABLE extract_result)
    if(NOT extract_result EQUAL 0)
      message(FATAL_ERROR "The test data could not be unpacked: ${extract_result}")
    endif()
    file(RENAME "${unpacking}" "${test_demos}")
  endif()
endif()

add_executable(tfdemo_tests
  main.cpp
  bitstream_tests.cpp
  sendtable_tests.cpp
  messages_tests.cpp
  demo_tests.cpp
  encoder_tests.cpp
  splice_tests.cpp
  gamestate_tests.cpp
  writer_tests.cpp
  encoding_tests.cpp
  anonymize_tests.cpp)
target_link_libraries(tfdemo_tests PRIVATE tfdemo)
# The tests reach internal headers such as compression.hpp.
target_include_directories(tfdemo_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(tfdemo_tests PRIVATE ${TFDEMO_WARNINGS})
if(MSVC)
  # getenv is what the tests read their demos with.
  target_compile_definitions(tfdemo_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set(first_demo "")
if(test_demos)
  file(GLOB demos "${test_demos}/*.dem")
  list(SORT demos)
  list(LENGTH demos demo_count)
  if(demo_count GREATER 0)
    list(GET demos 0 first_demo)
  endif()
endif()

add_test(NAME unit COMMAND tfdemo_tests)
set_tests_properties(unit PROPERTIES ENVIRONMENT
  "TFDEMO_TEST_DEMOS=${test_demos};TFDEMO_TEST_CLIENT_START=${CMAKE_CURRENT_SOURCE_DIR}/data/client-start.dem")

if(TFDEMO_BUILD_C_API)
  add_executable(tfdemo_c_test c_api_test.c)
  target_link_libraries(tfdemo_c_test PRIVATE tfdemo_c)
  target_compile_definitions(tfdemo_c_test PRIVATE TFDEMO_EXPECTED_VERSION="${PROJECT_VERSION}")
  if(MSVC)
    target_compile_definitions(tfdemo_c_test PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  # Linked as C++: the C library holds C++, whose sanitizer runtime (vptr
  # checks) a C link leaves out.
  set_target_properties(tfdemo_c_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tfdemo_c_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME c_api COMMAND tfdemo_c_test)
  set_tests_properties(c_api PROPERTIES ENVIRONMENT "TFDEMO_TEST_DEMO=${first_demo}")
  if(WIN32)
    # The DLL lies next to the library, not the test.
    set_property(TEST c_api APPEND PROPERTY ENVIRONMENT "PATH=$<TARGET_FILE_DIR:tfdemo_c>;$ENV{PATH}")
  endif()
endif()

# Every command of the tool on the first demo, against docs/schema/; needs
# Python 3 with jsonschema.
if(TFDEMO_BUILD_TOOLS AND NOT first_demo STREQUAL "")
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import jsonschema" RESULT_VARIABLE no_jsonschema
      OUTPUT_QUIET ERROR_QUIET)
    if(no_jsonschema EQUAL 0)
      add_test(NAME schemas COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_schemas.py
        $<TARGET_FILE:tfdemo_cli> ${PROJECT_SOURCE_DIR}/docs/schema ${first_demo}
        ${CMAKE_CURRENT_BINARY_DIR}/schema-check)
    else()
      message(STATUS "No jsonschema for Python 3: the schema check is left out")
    endif()
  endif()
endif()
