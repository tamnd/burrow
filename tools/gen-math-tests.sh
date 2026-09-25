#!/bin/sh
# Regenerates tests/math_test_gen.h from Go's math package. It copies the
# package out of GOROOT into a scratch module with every assembly version
# switched off, so what runs is the portable Go the C port follows, adds the
# tables from the top of all_test.go and the generator, and runs it as amd64.
# That last part matters: the Go compiler fuses a multiply and an add into one
# instruction on arm64 and some other machines, which changes the last bit of
# many results, and it never does on amd64. On an Apple Silicon Mac Rosetta
# runs the amd64 build. The Go on PATH should be the release the port follows.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
src=$(go env GOROOT)/src/math
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir "$work/gm"
printf 'module mgen\n\ngo 1.27\n' >"$work/go.mod"
for f in "$src"/*.go; do
	case $(basename "$f") in
	*_test.go | *_asm.go | *_s390x.go | exp_amd64.go) continue ;;
	esac
	# The portable fallbacks are tagged for the machines without assembly.
	sed '/^\/\/go:build/d' "$f" >"$work/gm/$(basename "$f")"
done
cp "$src/export_test.go" "$work/gm/export_test.go"

# The inputs and the expected answers, which is everything above the first
# function in all_test.go and huge_test.go.
{
	printf 'package math_test\n\nimport (\n\t. "mgen/gm"\n\t"unsafe"\n)\n\n'
	awk '/^import \(/ {skip = 1; next} skip && /^\)/ {skip = 0; next} skip {next}
	     /^package / {next} /^func / {nextfile} {print}' "$src/all_test.go" "$src/huge_test.go"
} >"$work/gm/tables_test.go"

# A list of the tables by name, for the generator to walk.
{
	printf 'package math_test\n\nvar tables = []table{\n'
	awk '/^func / {nextfile} /^var [a-zA-Z0-9]+ = / && $2 != "nan" {printf "\t{\"%s\", %s},\n", $2, $2}
	     /^\t[a-zA-Z0-9]+ += append/ {printf "\t{\"%s\", %s},\n", $1, $1}' "$src/all_test.go" "$src/huge_test.go"
	printf '}\n'
} >"$work/gm/list_test.go"

cp "$root/tools/gen-math-tests/gen_test.go" "$work/gm/gen_test.go"
out="$root/tests/math_test_gen.h"
(cd "$work" && OUT="$out" GOARCH=amd64 go test ./gm -run '^TestGenMath$' -count=1 >/dev/null)
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
