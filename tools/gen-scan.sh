#!/bin/sh
# Regenerates tests/scan_gen.h by running Go's fmt over the cases in its own
# scan_test.go. It copies that file and fmt_test.go, which declares the renamed
# types the scan tables use, out of GOROOT into a scratch module, adds the
# generator and runs it. The Go on PATH should be the release the port follows.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
src=$(go env GOROOT)/src/fmt
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir "$work/f"
printf 'module scangen\n\ngo 1.27\n' >"$work/go.mod"
sed -e '/"internal\/race"/d' -e 's/race\.Enabled/false/g' "$src/fmt_test.go" >"$work/f/fmt_test.go"
cp "$src/scan_test.go" "$work/f/scan_test.go"
# Two functions from export_test.go that fmt_test.go calls and the generator
# never does.
cat >"$work/f/shim_test.go" <<'SHIM'
package fmt_test

func IsSpace(r rune) bool { return r == ' ' }

func Parsenum(s string, start, end int) (int, bool, int) { return 0, false, 0 }
SHIM
cp "$root/tools/gen-scan/gen_test.go" "$work/f/gen_test.go"
out="$root/tests/scan_gen.h"
(cd "$work" && OUT="$out" go test ./f -run '^TestGenScan$' -count=1 -v | grep cases)
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
