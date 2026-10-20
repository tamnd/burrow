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
# libclang it says so and moves on, because a developer without one should
# still be able to run make check. The output of burrow-gen tests for its
# fixture is checked the same way, and needs Go rather than libclang.
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
status=0
if tools/regen.sh "$tmp" 2>"$tmp/err"; then
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
elif grep -q 'no libclang' "$tmp/err"; then
	printf 'check-gen: no libclang, skipping the descriptors\n'
else
	cat "$tmp/err" >&2
	exit 1
fi

# The variables of tests/embed_test.c, which burrow-gen embed writes from the
# files in tests/embedtest. Then the errors it gives for patterns go build
# refuses, against a tree made here, since a tree with bad names in it is not
# something to check in.
tools/burrow-gen embed tests/embed_test.c --dir tests/embedtest -o "$tmp/embed_test_embed.c" 2>"$tmp/err" || {
	cat "$tmp/err" >&2
	exit 1
}
# The #embed and __has_embed lines name the files relative to where the output
# is, which here is somewhere else, so they are left out of the comparison.
grep -v -e '^#embed ' -e '^#if __has_embed(' "$tmp/embed_test_embed.c" >"$tmp/embed.c"
grep -v -e '^#embed ' -e '^#if __has_embed(' tests/embedtest/embed_test_embed.c >"$tmp/embed_want.c"
if ! diff -u "$tmp/embed_want.c" "$tmp/embed.c"; then
	printf 'check-gen: tests/embedtest/embed_test_embed.c is not what burrow-gen embed writes now.\n' >&2
	printf 'check-gen: regenerate with tools/burrow-gen embed tests/embed_test.c --dir tests/embedtest -o tests/embedtest/embed_test_embed.c\n' >&2
	exit 1
fi

# The time zone database, src/time/zoneinfo.zip as C.
tools/burrow-gen embed src/time/tzdata.c --include burrow/time/tzdata.h -o "$tmp/tzdata_zip.c" || exit 1
grep -v -e '^#embed ' -e '^#if __has_embed(' "$tmp/tzdata_zip.c" >"$tmp/tz.c"
grep -v -e '^#embed ' -e '^#if __has_embed(' src/time/tzdata_zip.c >"$tmp/tz_want.c"
if ! cmp -s "$tmp/tz_want.c" "$tmp/tz.c"; then
	printf 'check-gen: src/time/tzdata_zip.c is not what burrow-gen embed writes now.\n' >&2
	printf 'check-gen: regenerate with tools/burrow-gen embed src/time/tzdata.c --include burrow/time/tzdata.h -o src/time/tzdata_zip.c\n' >&2
	exit 1
fi

e="$tmp/embed"
mkdir -p "$e/d/.dot" "$e/empty/_skip" "$e/bad"
printf 'x\n' >"$e/a.txt"
printf 'x\n' >"$e/b.txt"
printf 'x\n' >"$e/d/.dot/f"
printf 'x\n' >"$e/empty/_skip/f"
printf 'x\n' >"$e/bad/aux.txt"
embed_fails() {
	printf '%s\n' "$1" >"$e/x.c"
	if tools/burrow-gen embed "$e/x.c" -o "$e/x_embed.c" 2>"$e/err"; then
		printf 'check-gen: burrow-gen embed took %s\n' "$1" >&2
		exit 1
	fi
	got=$(sed "s|^burrow-gen: $e/x.c:1: ||" "$e/err")
	if [ "$got" != "$2" ]; then
		printf 'check-gen: burrow-gen embed on %s said\n%s\nwant\n%s\n' "$1" "$got" "$2" >&2
		exit 1
	fi
}
embed_fails 'BURROW_EMBED_FILE(s, "nope.txt");' 'pattern nope.txt: no matching files found'
embed_fails 'BURROW_EMBED_FILE(s, "*.txt");' 'invalid go:embed: multiple files for type string'
embed_fails 'BURROW_EMBED_BYTES(s, "a.txt b.txt");' 'invalid go:embed: multiple files for type []byte'
embed_fails 'BURROW_EMBED_FS(s, "../a.txt");' 'pattern ../a.txt: invalid pattern syntax'
embed_fails 'BURROW_EMBED_FS(s, "[");' 'pattern [: invalid pattern syntax'
embed_fails 'BURROW_EMBED_FS(s, "empty");' 'pattern empty: cannot embed directory empty: contains no embeddable files'
embed_fails 'BURROW_EMBED_FS(s, "bad");' 'pattern bad: cannot embed file bad/aux.txt: invalid name aux.txt'
embed_fails 'BURROW_EMBED_FS(s, "\"a.txt");' 'invalid quoted string in //go:embed: "a.txt'
embed_fails 'BURROW_EMBED_FS(s);' 'usage: BURROW_EMBED_FS(s, "pattern"...)'

printf 'check-gen: burrow-gen embed output is up to date and its errors are go build'"'"'s\n'

# burrow-gen from-go and its fixture. This needs neither Go nor libclang, only
# tools/from-go.c built against the library, which make check does first and
# burrow-gen does itself otherwise.
tool=
[ -n "${FROM_GO:-}" ] && tool="--tool $FROM_GO"
# shellcheck disable=SC2086
tools/burrow-gen from-go $tool tests/fromgo/inventory.go -o "$tmp/inventory_gen.c" \
	--header "$tmp/inventory_gen.h" 2>"$tmp/err" || {
	cat "$tmp/err" >&2
	exit 1
}
# The .c includes the header by its base name, the same in both places.
for f in inventory_gen.c inventory_gen.h; do
	if ! diff -u "tests/fromgo/$f" "$tmp/$f"; then
		printf 'check-gen: tests/fromgo/%s is not what burrow-gen from-go writes now.\n' "$f" >&2
		printf 'check-gen: regenerate with tools/burrow-gen from-go tests/fromgo/inventory.go -o tests/fromgo/inventory_gen.c --header tests/fromgo/inventory_gen.h\n' >&2
		exit 1
	fi
done
printf 'check-gen: burrow-gen from-go output is up to date\n'

# And what it refuses, each with nothing written.
fg="$tmp/fromgo"
mkdir -p "$fg"
fromgo_fails() {
	printf 'package p\n\n%s\n' "$1" >"$fg/x.go"
	# shellcheck disable=SC2086
	if tools/burrow-gen from-go $tool "$fg/x.go" -o "$fg/x.c" 2>"$fg/err"; then
		printf 'check-gen: burrow-gen from-go took %s\n' "$1" >&2
		exit 1
	fi
	got=$(sed "s|^$fg/||" "$fg/err")
	if [ "$got" != "$2" ] || [ -s "$fg/x.c" ]; then
		printf 'check-gen: burrow-gen from-go on %s said\n%s\nwant\n%s\n' "$1" "$got" "$2" >&2
		exit 1
	fi
}
fromgo_fails 'type C chan int' 'x.go:3:8: chan int: a channel has no descriptor to give it'
fromgo_fails 'type I interface{ M() }' 'x.go:3:8: interface{ M() }: an interface with methods has no descriptor to give it'
fromgo_fails 'type G[T any] struct{ x T }' 'x.go:3:6: G: generic types are not supported'
fromgo_fails 'type S struct{}' 'x.go:3:6: S: C has no empty structs'
fromgo_fails 'type S struct { A int; a int }' 'x.go:3:6: S: fields A and a are both a in C'
fromgo_fails 'type T struct { X struct{ Y int } }' 'x.go:3:19: struct{ Y int }: a struct needs a name of its own'
fromgo_fails 'func F(c chan int) {}' 'x.go:3:10: chan int: a channel has no descriptor to give it'
fromgo_fails 'type A B
type B A' 'x.go:3:6: A: invalid recursive type
x.go:4:6: B: invalid recursive type'

printf 'check-gen: burrow-gen from-go refuses what it cannot write\n'

# The same for burrow-gen tests, whose output for the fixture is checked in and
# built with the tests. The one line allowed to differ is the Go release it was
# generated with, and without Go there is nothing to generate with.
fixture=tools/gen-tests/testdata/strconv/fixture_test.go
golden=tools/gen-tests/testdata/strconv/gen_tests_fixture_test.c
if ! command -v go >/dev/null 2>&1; then
	printf 'check-gen: no go, skipping burrow-gen tests\n'
	exit 0
fi
tools/burrow-gen tests "$fixture" -o "$tmp/fixture_test.c" 2>"$tmp/err" || {
	cat "$tmp/err" >&2
	exit 1
}
# The comparison ignores white space as well, since that is clang-format's and
# the releases of it lay the same code out differently.
sed '/Go source: go1\./d' "$golden" | tr -d ' \t\n' >"$tmp/want.c"
sed '/Go source: go1\./d' "$tmp/fixture_test.c" | tr -d ' \t\n' >"$tmp/got.c"
if ! cmp -s "$tmp/want.c" "$tmp/got.c"; then
	diff -u "$golden" "$tmp/fixture_test.c" >&2 || true
	printf 'check-gen: %s is not what burrow-gen tests writes now.\n' "$golden" >&2
	printf 'check-gen: regenerate with tools/burrow-gen tests %s -o %s\n' "$fixture" "$golden" >&2
	exit 1
fi

printf 'check-gen: burrow-gen tests output is up to date\n'
