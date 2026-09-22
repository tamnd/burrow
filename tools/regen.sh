#!/bin/sh
# Regenerate the checked in generator output.
#
# One place that knows the flags, so that tools/check-gen.sh and whoever edited
# tests/gen/shapes.h are running the same command rather than two commands that
# agree today. Needs libclang; see the header of tools/burrow-gen for where it
# looks for one.
#
# Takes an output directory, for the benefit of the checker, and writes over the
# committed files when given none.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

out="${1:-tests/gen}"

tools/burrow-gen reflect tests/gen/shapes.h \
	-Iinclude \
	--banner tools/gen-banner.txt \
	--header "$out/shapes_gen.h" \
	-o "$out/shapes_gen.c"
