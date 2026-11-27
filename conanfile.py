# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# The Conan 2 recipe: conan create . builds the library from this checkout
# and runs test_package/.
import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, load
import re


class TfdemoConan(ConanFile):
    name = "tfdemo"
    description = "Reads and writes Team Fortress 2 demo files: a C++17 library without dependencies and a C API."
    license = "MIT"
    url = "https://github.com/davenonymous/tf-demo-parser-cpp"
    homepage = "https://github.com/davenonymous/tf-demo-parser-cpp"
    topics = ("tf2", "team-fortress-2", "demo", "parser", "source-engine")
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "fPIC": [True, False], "c_api": [True, False]}
    default_options = {"shared": False, "fPIC": True, "c_api": True}
    exports_sources = "CMakeLists.txt", "cmake/*", "include/*", "src/*", "LICENSE.txt", "licenses/*"

    def set_version(self):
        header = load(self, os.path.join(self.recipe_folder, "include", "tfdemo", "version.hpp"))
        self.version = re.search(r'#define TFDEMO_VERSION_STRING "([0-9.]+)"', header).group(1)

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.variables["TFDEMO_BUILD_TOOLS"] = False
        toolchain.variables["TFDEMO_BUILD_TESTS"] = False
        toolchain.variables["TFDEMO_BUILD_C_API"] = bool(self.options.c_api)
        toolchain.variables["TFDEMO_INSTALL"] = True
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE.txt", self.source_folder, os.path.join(self.package_folder, "licenses"))
        copy(self, "tf-demo-parser.txt", os.path.join(self.source_folder, "licenses"),
             os.path.join(self.package_folder, "licenses"))
        CMake(self).install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "tfdemo")
        self.cpp_info.components["tfdemo"].set_property("cmake_target_name", "tfdemo::tfdemo")
        self.cpp_info.components["tfdemo"].set_property("pkg_config_name", "tfdemo-cpp")
        self.cpp_info.components["tfdemo"].libs = ["tfdemo-cpp"]
        if self.options.c_api:
            self.cpp_info.components["tfdemo_c"].set_property("cmake_target_name", "tfdemo::tfdemo_c")
            self.cpp_info.components["tfdemo_c"].set_property("pkg_config_name", "tfdemo")
            self.cpp_info.components["tfdemo_c"].libs = ["tfdemo"]
