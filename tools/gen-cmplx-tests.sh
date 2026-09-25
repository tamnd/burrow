#!/bin/sh
# Regenerates tests/cmplx_test_gen.h from Go's math/cmplx package, the same way
# tools/gen-math-tests.sh does for math: it copies the package and the tables
# from the top of cmath_test.go into a scratch module, adds the generator and
# runs it as amd64, where the Go compiler never fuses a multiply and an add.
# Go's own math has assembly for Exp, Log and Hypot on amd64 that differs from
# its portable code in the last bit, and burrow's math is the portable code,
# so the copy of cmplx imports a copy of math with the assembly switched off.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
src=$(go env GOROOT)/src/math/cmplx
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir "$work/cmplx" "$work/math"
printf 'module cgen\n\ngo 1.27\n' >"$work/go.mod"
for f in "$src"/../*.go; do
	case $(basename "$f") in
	*_test.go | *_asm.go | *_s390x.go | exp_amd64.go) continue ;;
	esac
	sed '/^\/\/go:build/d' "$f" >"$work/math/$(basename "$f")"
done
for f in "$src"/*.go; do
	case $(basename "$f") in
	*_test.go) continue ;;
	esac
	sed 's|^import "math"$|import "cgen/math"|; s|^\t"math"$|\t"cgen/math"|' "$f" >"$work/cmplx/$(basename "$f")"
done

# The inputs and the expected answers, which is everything above the first
# function in cmath_test.go.
{
	printf 'package cmplx\n\nimport "cgen/math"\n\n'
	awk '/^import \(/ {skip = 1; next} skip && /^\)/ {skip = 0; next} skip {next}
	     /^import / {next} /^package / {next} /^func / {nextfile} {print}' "$src/cmath_test.go"
} >"$work/cmplx/tables_test.go"

# A list of the tables by name, for the generator to walk.
{
	printf 'package cmplx\n\nvar tables = []table{\n'
	awk '/^func / {nextfile} /^var [a-zA-Z0-9]+ = \[\]/ && $2 != "vc26" {printf "\t{\"%s\", %s},\n", $2, $2}' \
		"$src/cmath_test.go"
	printf '}\n'
} >"$work/cmplx/list_test.go"

cp "$root/tools/gen-cmplx-tests/gen_test.go" "$work/cmplx/gen_test.go"
out="$root/tests/cmplx_test_gen.h"
(cd "$work" && OUT="$out" GOARCH=amd64 go test ./cmplx -run '^TestGenCmplx$' -count=1 >/dev/null)
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
