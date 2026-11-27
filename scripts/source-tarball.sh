#!/bin/sh
# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# Writes the source tarball of a commit as the release has it:
# tf-demo-parser-cpp-<version>.tar.gz with everything under
# tf-demo-parser-cpp-<version>/, without the test demo (.gitattributes).
# The same commit always gives the same bytes (git archive, gzip -n).
#   scripts/source-tarball.sh <ref> <out dir>
set -eu
if [ $# -ne 2 ]; then
	echo "usage: scripts/source-tarball.sh <ref> <out dir>" >&2
	exit 2
fi
here="$(cd "$(dirname "$0")/.." && pwd)"
ref="$1"
mkdir -p "$2"
out="$(cd "$2" && pwd)"
version="$("${here}/scripts/version.sh" "${ref}")"
name="tf-demo-parser-cpp-${version}"
git -C "${here}" archive --format=tar --prefix="${name}/" "${ref}" | gzip -9n > "${out}/${name}.tar.gz"
printf '%s\n' "${out}/${name}.tar.gz"
