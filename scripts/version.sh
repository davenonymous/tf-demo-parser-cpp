#!/bin/sh
# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# Prints the library's version from include/tfdemo/version.hpp, of the
# working tree or, with a git ref, of that commit.
#   scripts/version.sh [ref]
set -eu
cd "$(dirname "$0")/.."
if [ $# -gt 0 ]; then
	header="$(git show "$1:include/tfdemo/version.hpp")"
else
	header="$(cat include/tfdemo/version.hpp)"
fi
version="$(printf '%s\n' "${header}" | sed -n 's/^#define TFDEMO_VERSION_STRING "\([0-9]*\.[0-9]*\.[0-9]*\)"$/\1/p')"
if [ -z "${version}" ]; then
	echo "version.sh: no TFDEMO_VERSION_STRING in include/tfdemo/version.hpp" >&2
	exit 1
fi
printf '%s\n' "${version}"
