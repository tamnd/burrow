#!/bin/sh
# No name burrow exports, or will export, can be one the system's libraries
# already define.
#
# burrow's public names have no prefix, so a function called uuid_parse in
# burrow and one in libuuid are the same symbol to the linker. When both are
# static that is a link error, and when the other one is in a shared library it
# is worse, because the program's copy quietly answers the library's own calls.
# This hands every library the host has to burrow-gen collisions, which compares
# their symbol tables with libburrow.a and with the C name of everything in Go's
# API that burrow has not written yet, and fails on a match nobody waived in
# tools/collision-waivers.txt.
#
# Which libraries it reads: on Linux, everything in the ldconfig cache, musl's
# libc.a and MinGW's import libraries when they are installed. On macOS, the
# SDK's usr/lib and Homebrew's lib. Set COLLISION_LIBS to a list of files or
# directories to use those instead. What it finds depends on what is installed,
# so CI installs a fixed set first, and this is not part of make check.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

if ! command -v python3 >/dev/null 2>&1; then
	printf 'check-collisions: no python3, skipping\n'
	exit 0
fi
if ! command -v nm >/dev/null 2>&1; then
	printf 'check-collisions: no nm, skipping\n'
	exit 0
fi

lib="${BURROW_LIB:-build/libburrow.a}"
[ -f "$lib" ] || make -s "$lib"

planned=""
if ! command -v go >/dev/null 2>&1; then
	printf 'check-collisions: no go, so only the names burrow defines today are checked\n'
	planned="--no-planned"
fi

set --
if [ -n "${COLLISION_LIBS:-}" ]; then
	# shellcheck disable=SC2086
	set -- $COLLISION_LIBS
else
	case "$(uname -s)" in
	Darwin)
		sdk=$(xcrun --show-sdk-path)
		set -- "$sdk/usr/lib" "$sdk/usr/lib/system"
		for d in /opt/homebrew/lib /usr/local/lib; do
			[ -d "$d" ] && set -- "$@" "$d"
		done
		;;
	*)
		list=$(mktemp)
		trap 'rm -f "$list"' EXIT
		if command -v ldconfig >/dev/null 2>&1; then
			ldconfig -p | sed -n 's|.* => \(/.*\)$|\1|p' | sort -u >"$list"
		elif [ -x /sbin/ldconfig ]; then
			/sbin/ldconfig -p | sed -n 's|.* => \(/.*\)$|\1|p' | sort -u >"$list"
		fi
		while read -r f; do
			set -- "$@" "$f"
		done <"$list"
		for f in /usr/lib/musl/lib/libc.a /usr/local/musl/lib/libc.a /usr/lib/x86_64-linux-musl/libc.a; do
			[ -f "$f" ] && set -- "$@" "$f"
		done
		for d in /usr/x86_64-w64-mingw32/lib /usr/i686-w64-mingw32/lib; do
			[ -d "$d" ] && set -- "$@" "$d"
		done
		;;
	esac
fi

[ "$#" -gt 0 ] || {
	printf 'check-collisions: found no libraries to check against\n' >&2
	exit 1
}

# shellcheck disable=SC2086
exec tools/burrow-gen collisions --burrow "$lib" $planned "$@"
