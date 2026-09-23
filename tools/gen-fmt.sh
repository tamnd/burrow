#!/bin/sh
# Regenerates tests/fmt_gen.h by running Go's fmt over the cases in its own
# fmt_test.go. It copies that file out of GOROOT into a scratch module, drops
# the one internal package it imports, adds the generator and runs it. The Go on
# PATH should be the release the port follows.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
src=$(go env GOROOT)/src/fmt
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir "$work/f"
printf 'module fmtgen\n\ngo 1.27\n' >"$work/go.mod"
sed -e '/"internal\/race"/d' -e 's/race\.Enabled/false/g' "$src/fmt_test.go" >"$work/f/fmt_test.go"
# What the file borrows from the rest of the package, which the generator never
# calls: a variable from scan_test.go and two functions from export_test.go.
cat >"$work/f/shim_test.go" <<'SHIM'
package fmt_test

var f float64

func IsSpace(r rune) bool { return r == ' ' }

func Parsenum(s string, start, end int) (int, bool, int) { return 0, false, 0 }
SHIM
cp "$root/tools/gen-fmt/gen_test.go" "$work/f/gen_test.go"
out="$root/tests/fmt_gen.h"
(cd "$work" && OUT="$out" go test ./f -run '^TestGenFmt$' -count=1 -v | grep cases)
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
