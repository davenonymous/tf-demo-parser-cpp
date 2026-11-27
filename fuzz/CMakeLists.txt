# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# The libFuzzer targets: demo_fuzzer runs any bytes through the parser with
# the analysers and the game state, reencode_demo, anonymize_demo and
# splice_entity_poses; messages_fuzzer reads any bits as net messages and
# checks that each writes back the same bits after a read. Clang only;
# build them with the "fuzz" preset, which adds AddressSanitizer and
# UndefinedBehaviorSanitizer to the library as well. make_corpus.py writes
# their seed corpus from a demo.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "The fuzzers need clang (libFuzzer)")
endif()

foreach(fuzzer demo_fuzzer messages_fuzzer)
  add_executable(${fuzzer} ${fuzzer}.cpp)
  target_link_libraries(${fuzzer} PRIVATE tfdemo)
  target_compile_options(${fuzzer} PRIVATE -fsanitize=fuzzer ${TFDEMO_WARNINGS})
  target_link_options(${fuzzer} PRIVATE -fsanitize=fuzzer)
endforeach()
