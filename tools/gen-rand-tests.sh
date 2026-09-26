#!/bin/sh
# Regenerates tests/rand_test_gen.h from Go's math/rand and math/rand/v2 tests:
# the regressGolden tables both packages check their seeded sequences against,
# and the PCG and ChaCha8 outputs and encodings from pcg_test.go and
# chacha8_test.go. Nothing runs Go here. The tables are read out of the test
# sources as they are, so what the C tests check is exactly what Go's do.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
src=$(go env GOROOT)/src/math/rand
out="$root/tests/rand_test_gen.h"

python3 - "$src" "$out" <<'PY'
import re, struct, sys

src, out = sys.argv[1], sys.argv[2]

def block(text, start):
    """The body of the composite literal that starts at the line start."""
    i = text.index(start)
    i = text.index("{", i) + 1
    depth = 1
    j = i
    while depth:
        c = text[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif c == '"':
            j += 1
            while text[j] != '"':
                j += 2 if text[j] == "\\" else 1
        j += 1
    return text[i:j - 1]

def f64_bits(s):
    return struct.unpack("<Q", struct.pack("<d", float(s)))[0]

def f32_bits(s):
    return struct.unpack("<I", struct.pack("<f", float(s)))[0]

def golden(path, prefix):
    text = open(path).read()
    body = block(text, "\nvar regressGolden = []any{")
    rows, arrays = [], []
    for line in body.split("\n"):
        line = line.strip()
        if not line:
            continue
        m = re.match(r"(.*?),\s*// (\w+)\((.*)\)$", line)
        if m is None:
            raise SystemExit("unexpected line: " + repr(line))
        val, name = m.group(1), m.group(2)
        k = len(rows)
        if val.startswith("float64("):
            rows.append('{"%s", RG_F64, UINT64_C(0x%016x), NULL, 0}' % (name, f64_bits(val[8:-1])))
        elif val.startswith("float32("):
            rows.append('{"%s", RG_F32, UINT64_C(0x%08x), NULL, 0}' % (name, f32_bits(val[8:-1])))
        elif re.match(r"u?int(32|64)\(", val):
            kind = val[:val.index("(")]
            n = int(val[len(kind) + 1:-1])
            rows.append('{"%s", RG_%s, UINT64_C(0x%016x), NULL, 0}' % (name, kind.upper(), n & (2**64 - 1)))
        elif val.startswith("[]int{") or val.startswith("[]byte{"):
            items = [x.strip() for x in val[val.index("{") + 1:-1].split(",") if x.strip()]
            nums = [int(x, 0) for x in items]
            kind = "RG_INTS" if val.startswith("[]int") else "RG_BYTES"
            arr = "%s_%d" % (prefix, k)
            arrays.append("static const int64_t %s[] = {%s};" % (arr, ", ".join(str(x) for x in nums) or "0"))
            rows.append('{"%s", %s, 0, %s, %d}' % (name, kind, arr, len(nums)))
        else:
            raise SystemExit("unexpected golden: " + line)
    return arrays, rows

def go_string(lit):
    """The bytes of a Go interpreted string literal, quotes included."""
    s = lit[1:-1]
    b = bytearray()
    i = 0
    simple = {"a": 7, "b": 8, "f": 12, "n": 10, "r": 13, "t": 9, "v": 11, "\\": 92, '"': 34, "'": 39}
    while i < len(s):
        c = s[i]
        if c != "\\":
            b += c.encode()
            i += 1
            continue
        e = s[i + 1]
        if e in simple:
            b.append(simple[e])
            i += 2
        elif e == "x":
            b.append(int(s[i + 2:i + 4], 16))
            i += 4
        elif e == "u":
            b += chr(int(s[i + 2:i + 6], 16)).encode()
            i += 6
        elif e == "U":
            b += chr(int(s[i + 2:i + 10], 16)).encode()
            i += 10
        elif e in "01234567":
            b.append(int(s[i + 1:i + 4], 8))
            i += 4
        else:
            raise SystemExit("unexpected escape: " + e)
    return bytes(b)

def c_bytes(b):
    return "{" + ", ".join("0x%02x" % x for x in b) + "}"

v1_arrays, v1_rows = golden(src + "/regress_test.go", "rg1")
v2_arrays, v2_rows = golden(src + "/v2/regress_test.go", "rg2")

cc = open(src + "/v2/chacha8_test.go").read()
cc_out = re.findall(r"0x[0-9a-f]+", block(cc, "var chacha8output = []uint64{"))
cc_hash = re.search(r'chacha8hash, _ = hex.DecodeString\("([0-9a-f]+)"\)', cc).group(1)
cc_len = int(re.search(r"var chacha8outlen = (\d+)", cc).group(1))
cc_seed = go_string(re.search(r'var chacha8seed = \[32\]byte\(\[\]byte\(("[^"]*")\)\)', cc).group(1))

def strings(name):
    return [go_string(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"', block(cc, "var %s = []string{" % name))]

marshal = strings("chacha8marshal")
marshalread = strings("chacha8marshalread")

pcg = open(src + "/v2/pcg_test.go").read()
body = pcg[pcg.index("func TestPCG(t *testing.T)"):]
pcg_want = re.findall(r"0x[0-9a-f]+", block(body, "want := []uint64{"))

def enc_table(name, encs):
    lines = []
    for i, e in enumerate(encs):
        lines.append("static const Byte %s_%d[] = %s;" % (name, i, c_bytes(e)))
    lines.append("static const RandEnc %s[] = {" % name)
    for i, e in enumerate(encs):
        lines.append("    {%s_%d, %d}," % (name, i, len(e)))
    lines.append("};")
    return lines

o = []
o.append("""/* What Go's math/rand and math/rand/v2 tests say: the regressGolden tables
 * from both regress_test.go files, which pin the values every method gives for
 * a fixed seed, and the PCG and ChaCha8 tables from v2's pcg_test.go and
 * chacha8_test.go. Regenerate it with tools/gen-rand-tests.sh rather than
 * editing it.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */
""")
o += v1_arrays
o.append("static const RandGolden rand_v1_golden[] = {")
o += ["    %s," % r for r in v1_rows]
o.append("};")
o.append("")
o += v2_arrays
o.append("static const RandGolden rand_v2_golden[] = {")
o += ["    %s," % r for r in v2_rows]
o.append("};")
o.append("")
o.append("static const uint64_t pcg_want[] = {%s};" % ", ".join("UINT64_C(%s)" % x for x in pcg_want))
o.append("")
o.append("static const Byte chacha8_seed[32] = %s;" % c_bytes(cc_seed))
o.append("#define CHACHA8_OUTLEN %d" % cc_len)
o.append('static const char chacha8_hash[] = "%s";' % cc_hash)
o.append("static const uint64_t chacha8_output[] = {%s};" % ", ".join("UINT64_C(%s)" % x for x in cc_out))
o += enc_table("chacha8_marshal", marshal)
o += enc_table("chacha8_marshalread", marshalread)
open(out, "w").write("\n".join(o) + "\n")
PY
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
