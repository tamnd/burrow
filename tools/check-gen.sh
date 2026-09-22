#!/bin/sh
# The checked in generator output has to be what the generator produces today.
#
# tests/gen/shapes_gen.c is committed rather than built, because building burrow
# must never need libclang and CI on a machine without one still has to compile
# and run the differential test. The cost of committing it is that it can drift
# from the header it came from, and the cost of that drift is a test that claims
# the generator agrees with the DSL while testing output nobody has regenerated
# since the generator changed.
#
# So this regenerates into a temporary file and diffs. On a machine with no
# libclang it says so and exits clean, because a developer without one should
# still be able to run make check.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

if ! command -v python3 >/dev/null 2>&1; then
	printf 'check-gen: no python3, skipping\n'
	exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The generator says what it could not find, and not finding libclang is the
# one failure here that is not the tree's fault.
if ! tools/regen.sh "$tmp" 2>"$tmp/err"; then
	if grep -q 'no libclang' "$tmp/err"; then
		printf 'check-gen: no libclang, skipping\n'
		exit 0
	fi
	cat "$tmp/err" >&2
	exit 1
fi

status=0
for f in shapes_gen.c shapes_gen.h; do
	if ! diff -u "tests/gen/$f" "$tmp/$f"; then
		printf 'check-gen: tests/gen/%s is not what the generator produces now.\n' "$f" >&2
		status=1
	fi
done

if [ "$status" -ne 0 ]; then
	printf 'check-gen: regenerate with tools/regen.sh and commit the result.\n' >&2
	exit 1
fi

printf 'check-gen: generated descriptors are up to date\n'
