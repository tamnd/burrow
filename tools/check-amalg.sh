#!/bin/sh
# The ways of trimming the amalgamation have to keep producing a library that
# builds.
#
# burrow-gen amalgamate can drop packages two ways, with --packages when the
# file is generated and with BURROW_OMIT_ macros when it is compiled. Both are
# only worth having if every combination they allow compiles, and a skeleton of
# #if lines nobody builds is a skeleton that has already rotted. So this builds
# the full file with each package that can be left out left out, along with
# whatever needs it, then with all of them left out at once, and links and runs
# tests/amalgamation/hello.c against each. It checks that leaving out a package
# something else needs is an #error that says so, that --packages gives the same
# bytes twice, and that a file generated for each single package compiles on its
# own.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

if ! command -v python3 >/dev/null 2>&1; then
	printf 'check-amalg: no python3, skipping\n'
	exit 0
fi

CC="${CC:-cc}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() {
	printf 'check-amalg: %s\n' "$*" >&2
	exit 1
}

tools/burrow-gen amalgamate --out "$tmp/full" --source-id check 2>/dev/null

# The package list is in the manifest and the ones that can be left out are in
# burrow.h, both written by the generator, so this reads them rather than
# keeping a copy.
manifest_packages() {
	python3 -c 'import json, sys; print("\n".join(json.load(open(sys.argv[1]))["packages"]))' "$1"
}
packages=$(manifest_packages "$tmp/full/burrow-manifest.json")
omittable=$(sed -n 's|^#if !defined(\(BURROW_OMIT_[A-Z0-9_]*\))$|\1|p' "$tmp/full/burrow.h" | sort -u)
required=$(sed -n 's|^#error "\(BURROW_OMIT_[A-Z0-9_]*\) is set, and the runtime uses.*|\1|p' "$tmp/full/burrow.h")

[ -n "$omittable" ] || fail "burrow.h has no BURROW_OMIT_ guards at all"
[ -n "$required" ] || fail "burrow.h has no check for the packages the runtime needs"

build() {
	dir=$1
	shift
	"$CC" -std=c11 -O0 -Wall -Wextra -Werror "$@" -c "$dir/burrow.c" -o "$tmp/burrow.o" ||
		fail "$dir/burrow.c does not compile with $*"
	"$CC" -std=c11 -O0 -Wall -Wextra -Werror "$@" -I"$dir" tests/amalgamation/hello.c \
		"$tmp/burrow.o" -pthread -o "$tmp/hello" || fail "hello does not link with $*"
	"$tmp/hello" >/dev/null || fail "hello fails with $*"
}

# Leaving out a package means leaving out everything that needs it too, and
# burrow.h says what needs what in its #if defined(A) && !defined(B) checks.
# This prints, for each package that can be left out, the macros that do that.
omit_sets() {
	python3 - "$1" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
guarded = sorted(set(re.findall(r"^#if !defined\((BURROW_OMIT_\w+)\)$", text, re.M)))
users = {}
for q, p in re.findall(r"^#if defined\((BURROW_OMIT_\w+)\) && !defined\((BURROW_OMIT_\w+)\)$", text, re.M):
    users.setdefault(q, set()).add(p)
for m in guarded:
    out, todo = {m}, [m]
    while todo:
        for p in users.get(todo.pop(), ()):
            if p not in out:
                out.add(p)
                todo.append(p)
    print(m, " ".join("-D" + x for x in sorted(out)))
PY
}

build "$tmp/full" -DBURROW_OMIT_NOTHING
all=""
omit_sets "$tmp/full/burrow.h" >"$tmp/sets"
while read -r m flags; do
	# shellcheck disable=SC2086
	build "$tmp/full" $flags
	all="$all -D$m"
	if [ "$flags" != "-D$m" ]; then
		if "$CC" -std=c11 -fsyntax-only "-D$m" "$tmp/full/burrow.c" 2>"$tmp/err"; then
			fail "$m on its own compiles, and other packages need it"
		fi
		grep -q "$m is set, and" "$tmp/err" || fail "$m on its own fails, but not with the #error that explains it"
	fi
done <"$tmp/sets"
# shellcheck disable=SC2086
build "$tmp/full" $all

for m in $required; do
	if "$CC" -std=c11 -fsyntax-only "-D$m" "$tmp/full/burrow.c" 2>"$tmp/err"; then
		fail "$m compiles, and the runtime needs that package"
	fi
	grep -q "$m is set" "$tmp/err" || fail "$m fails, but not with the #error that explains it"
done

for p in $packages; do
	d="$tmp/one"
	rm -rf "$d"
	tools/burrow-gen amalgamate --packages "$p" --out "$d" --source-id check 2>/dev/null
	tools/burrow-gen amalgamate --packages "$p" --out "$d-again" --source-id check 2>/dev/null
	for f in burrow.c burrow.h burrow-manifest.json; do
		cmp -s "$d/$f" "$d-again/$f" || fail "--packages $p gives different bytes for $f on a second run"
	done
	rm -rf "$d-again"
	manifest_packages "$d/burrow-manifest.json" | grep -qx "$p" || fail "--packages $p left out $p"
	"$CC" -std=c11 -Wall -Wextra -Werror -fsyntax-only "$d/burrow.c" ||
		fail "--packages $p does not compile"
done

if tools/burrow-gen amalgamate --packages no/such --out "$tmp/none" 2>/dev/null; then
	fail "--packages accepted a package that does not exist"
fi

n=$(printf '%s\n' "$omittable" | wc -l | tr -d ' ')
printf 'check-amalg: %d packages, %d of them can be left out, every combination builds\n' \
	"$(printf '%s\n' "$packages" | wc -l | tr -d ' ')" "$n"
