#!/bin/sh
# How big a Go package is, measured from the Go tree on this machine.
#
#     tools/inventory.sh                  every public package, then the total
#     tools/inventory.sh strings bufio    just those
#     tools/inventory.sh -m net/...       the same, as a markdown table
#     tools/inventory.sh -t linux-amd64   declarations for one platform only
#     tools/inventory.sh -l bufio         the declarations themselves
#
# For each package it prints four numbers. Decls is the count of exported
# declarations in the union of $GOROOT/api/go1.txt to the newest go1.N.txt. Some
# lines there only exist on some platforms, and those count if they exist on
# one of the targets, which are the three burrow releases binaries for unless -t
# names others. A declaration that is on all three counts once. LOC and Test are the lines in the
# package's own .go files, not counting its subpackages or testdata, split on
# whether the name ends in _test.go. Deps is how many standard packages it
# imports directly or not, which is a fair guide to how late it can be ported.
#
# A public package is one that `go list std` prints and that is not under
# internal, vendor or cmd. docs/design/01-scope.md was measured with this, and
# running it with no arguments on the same Go release gives the same table.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

markdown=0
list=0
targets="linux-amd64 darwin-arm64 windows-amd64"
while [ $# -gt 0 ]; do
	case $1 in
	-m) markdown=1 ;;
	-l) list=1 ;;
	-t)
		[ $# -ge 2 ] || {
			echo "inventory: -t needs a list like linux-amd64,windows-386" >&2
			exit 2
		}
		targets=$(echo "$2" | tr ',' ' ')
		shift
		;;
	*) break ;;
	esac
	shift
done

command -v go >/dev/null 2>&1 || {
	echo "inventory: needs a Go toolchain on PATH" >&2
	exit 2
}

goroot=$(go env GOROOT)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The package list and each package's dependency count in one go list call,
# which is much faster than one call per package.
if [ $# -eq 0 ]; then
	set -- std
fi
go list -e -f '{{.ImportPath}} {{len .Deps}}' "$@" |
	grep -Ev '^(internal|vendor|cmd)/|/internal/|/internal |/vendor/' >"$tmp/pkgs" || true
if [ ! -s "$tmp/pkgs" ]; then
	echo "inventory: no public standard packages matched $*" >&2
	exit 1
fi

# One line per exported declaration. A line like
#     pkg syscall (linux-amd64-cgo), const AF_INET = 2
# names the platform after the package. It is kept if the platform, with any
# -cgo taken off, is a target, and then the parenthesis goes so the same line
# from two targets sorts together.
cat "$goroot"/api/go1*.txt |
	awk -v targets="$targets" '
	BEGIN { n = split(targets, t, " "); for (i = 1; i <= n; i++) want[t[i]] = 1 }
	$1 != "pkg" { next }
	{
	    pkg = $2
	    if (pkg ~ /,$/) {
	        sub(/,$/, "", pkg)
	        rest = substr($0, length("pkg " pkg ", ") + 1)
	    } else {
	        plat = $3
	        gsub(/[(),]/, "", plat)
	        sub(/-cgo$/, "", plat)
	        if (!(plat in want)) next
	        rest = substr($0, length("pkg " pkg " " $3 " ") + 1)
	    }
	    print pkg, rest
	}' |
	sort -u >"$tmp/api"

if [ "$list" = 1 ]; then
	awk 'NR == FNR { want[$1] = 1; next } $1 in want' "$tmp/pkgs" "$tmp/api"
	exit 0
fi

awk '{ n[$1]++ } END { for (p in n) print p, n[p] }' "$tmp/api" >"$tmp/decls"

while read -r pkg deps; do
	dir="$goroot/src/$pkg"
	loc=0
	test=0
	for f in "$dir"/*.go; do
		[ -f "$f" ] || continue
		lines=$(wc -l <"$f")
		case $f in
		*_test.go) test=$((test + lines)) ;;
		*) loc=$((loc + lines)) ;;
		esac
	done
	decls=$(awk -v p="$pkg" '$1 == p { print $2 }' "$tmp/decls")
	echo "$pkg ${decls:-0} $loc $test $deps"
done <"$tmp/pkgs" >"$tmp/rows"

awk -v markdown="$markdown" '
function num(n,   s) {
    s = sprintf("%d", n)
    while (s ~ /[0-9][0-9][0-9][0-9]/) sub(/[0-9][0-9][0-9]([,]|$)/, ",&", s)
    return s
}
BEGIN {
    if (markdown) {
        print "| Package | Decls | LOC | Test | Deps |"
        print "| --- | ---: | ---: | ---: | ---: |"
    } else {
        printf "%-28s %8s %9s %9s %5s\n", "package", "decls", "loc", "test", "deps"
    }
}
{
    if (markdown)
        printf "| `%s` | %s | %s | %s | %d |\n", $1, num($2), num($3), num($4), $5
    else
        printf "%-28s %8s %9s %9s %5d\n", $1, num($2), num($3), num($4), $5
    n++; d += $2; l += $3; t += $4
}
END {
    if (n < 2) exit
    total = sprintf("%d packages", n)
    if (markdown)
        printf "| **%s** | **%s** | **%s** | **%s** | |\n", total, num(d), num(l), num(t)
    else
        printf "%-28s %8s %9s %9s\n", total, num(d), num(l), num(t)
}
' "$tmp/rows"
