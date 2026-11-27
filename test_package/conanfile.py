# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
import os

from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, cmake_layout


class TfdemoTestConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure(variables={"CONSUMER_C_API": bool(self.dependencies["tfdemo"].options.c_api)})
        cmake.build()

    def test(self):
        if can_run(self):
            self.run(os.path.join(self.cpp.build.bindir, "consumer"), env="conanrun")
            if self.dependencies["tfdemo"].options.c_api:
                self.run(os.path.join(self.cpp.build.bindir, "consumer_c"), env="conanrun")
