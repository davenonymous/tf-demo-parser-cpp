# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# The overlay port of this checkout: vcpkg install --overlay-ports=ports tfdemo
# builds the sources next to it. A port for vcpkg's registry takes them from
# the release instead (vcpkg_from_github with REF v${VERSION} and its SHA512).
set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    -DTFDEMO_BUILD_TOOLS=OFF
    -DTFDEMO_BUILD_TESTS=OFF
    -DTFDEMO_INSTALL=ON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/tfdemo)
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt" "${SOURCE_PATH}/licenses/tf-demo-parser.txt")
