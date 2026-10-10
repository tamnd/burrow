#!/bin/sh
# The go/printer gate: burrow's gofmt (tools/gofmt.c) and Go's own over the
# whole of $GOROOT/src, which have to agree byte for byte. Most files there are
# already formatted, and for those it is enough that both say so in their -l
# lists, since the output of each is then the file itself. The files gofmt
# would change are formatted by both and compared in full. The errors each
# prints for the files that do not parse have to match as well.
#
#     tools/check-gofmt.sh BURROW_GOFMT [DIR]
#
# BURROW_GOFMT is the built tools/gofmt.c, which make gofmt-gate builds and
# passes. DIR defaults to $GOROOT/src, and GOROOT to what go env says. The Go
# release should be the one burrow tracks, since gofmt's output changes now
# and then between releases.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 BURROW_GOFMT [DIR]" >&2
    exit 2
fi
bin=$1
goroot=${GOROOT:-$(go env GOROOT)}
dir=${2:-$goroot/src}
gofmt=$goroot/bin/gofmt

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Both exit 2 when a file does not parse, and $GOROOT/src has some on purpose.
"$gofmt" -l "$dir" > "$tmp/go.list" 2> "$tmp/go.err" || true
"$bin" -l "$dir" > "$tmp/bx.list" 2> "$tmp/bx.err" || true

fail=0
if ! cmp -s "$tmp/go.list" "$tmp/bx.list"; then
    echo "gofmt -l lists differ (- go, + burrow):"
    diff "$tmp/go.list" "$tmp/bx.list" | sed -n 's/^</-/p; s/^>/+/p' | head -100
    fail=1
fi
if ! cmp -s "$tmp/go.err" "$tmp/bx.err"; then
    echo "errors differ (- go, + burrow):"
    diff "$tmp/go.err" "$tmp/bx.err" | sed -n 's/^</-/p; s/^>/+/p' | head -100
    fail=1
fi

changed=0
while IFS= read -r f; do
    changed=$((changed + 1))
    "$gofmt" "$f" > "$tmp/go.out" 2> /dev/null || true
    "$bin" "$f" > "$tmp/bx.out" 2> /dev/null || true
    if ! cmp -s "$tmp/go.out" "$tmp/bx.out"; then
        echo "formatted differently: $f"
        diff "$tmp/go.out" "$tmp/bx.out" | head -20
        fail=1
    fi
done < "$tmp/go.list"

files=$(find "$dir" -name '*.go' ! -name '.*' | wc -l | tr -d ' ')
errs=$(wc -l < "$tmp/go.err" | tr -d ' ')
if [ $fail -ne 0 ]; then
    echo "FAIL	gofmt	$files files under $dir"
    exit 1
fi
printf 'ok\tgofmt\t%s files, %s that gofmt changes, %s error lines, against %s\n' \
    "$files" "$changed" "$errs" "$("$goroot/bin/go" version | cut -d' ' -f3)"
