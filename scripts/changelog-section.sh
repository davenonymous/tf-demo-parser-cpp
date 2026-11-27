#!/bin/sh
# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# Prints the CHANGELOG.md section of a version (without its heading), the
# release notes; fails when there is none or it is empty.
#   scripts/changelog-section.sh <version>
set -eu
if [ $# -ne 1 ]; then
	echo "usage: scripts/changelog-section.sh <version>" >&2
	exit 2
fi
cd "$(dirname "$0")/.."
section="$(awk -v version="$1" '
	/^## \[/ { inside = index($0, "## [" version "]") == 1; next }
	/^\[[^]]*\]: / { inside = 0 }
	inside { print }
' CHANGELOG.md | sed -e '/./,$!d')"
if [ -z "$(printf '%s' "${section}" | tr -d '[:space:]')" ]; then
	echo "changelog-section.sh: CHANGELOG.md has no section for $1" >&2
	exit 1
fi
printf '%s\n' "${section}"
