#!/bin/sh
# Regenerates tests/strconv_float_gen.h by running Go's strconv over the test
# inputs. It copies internal/strconv out of GOROOT into a scratch module, since
# a package under internal cannot be imported from outside, adds the generator
# and the ParseComplex table, which Go keeps inside a test function, and runs
# it. The Go on PATH should be the release the port follows.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
src=$(go env GOROOT)/src/internal/strconv
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir "$work/sc"
printf 'module scgen\n\ngo 1.27\n' >"$work/go.mod"
# The package, and of its tests only the tables the generator reads.
for f in "$src"/*.go; do
	case $(basename "$f") in
	pow10gen.go) continue ;;
	export_test.go | atof_test.go | ftoa_test.go) ;;
	*_test.go) continue ;;
	esac
	sed 's#\. "internal/strconv"#. "scgen/sc"#' "$f" >"$work/sc/$(basename "$f")"
done
sed -i.bak 's/:= parseFloatPrefix(/:= ParseFloatPrefix(/' "$work/sc/atof_test.go"

# The ParseComplex cases, lifted out of TestParseComplex into a variable.
{
	printf 'package strconv_test\n\nimport (\n\t"math"\n\t. "scgen/sc"\n)\n\n'
	awk '/^var \($/ {p = 1} p {print} p && /^\)$/ {exit}' "$src/atoc_test.go"
	printf '\ntype atocCase struct {\n\tin  string\n\tout complex128\n\terr error\n}\n\n'
	awk '/tests := \[\]atocTest\{/ {p = 1; print "var atocTests = []atocCase{"; next}
	     p && /^\t}$/ {print "}"; exit}
	     p {sub(/^\t/, ""); print}' "$src/atoc_test.go"
} >"$work/sc/atoc_table_test.go"

cp "$root/tools/gen-strconv-float/gen_test.go" "$work/sc/gen_float_test.go"
out="$root/tests/strconv_float_gen.h"
(cd "$work" && OUT="$out" go test ./sc -run '^TestGenFloat$' -count=1 >/dev/null)
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
