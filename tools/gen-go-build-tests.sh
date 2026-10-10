#!/bin/sh
# Regenerates tests/go_build_test_gen.h for the go/build tests. It holds what
# Go's own tests read from disk, and the tables of the tests themselves:
#
#   - every file under src/go/build/testdata, which the tests write out to a
#     temporary directory before they import from it;
#   - readGoInfoTests, readCommentsTests, readFailuresTests and readEmbedTests,
#     out of read_test.go;
#   - shouldBuildTests, matchFileTests and expandSrcDirTests, and the cases of
#     TestShellSafety, out of build_test.go.
#
# GOROOT says which Go to read, and defaults to what go env says.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
goroot=${GOROOT:-$(go env GOROOT)}
out="$root/tests/go_build_test_gen.h"

python3 - "$goroot" > "$out" <<'PY'
import os
import re
import sys

goroot = sys.argv[1]
pkg = os.path.join(goroot, "src", "go", "build")
tdir = os.path.join(pkg, "testdata")
gover = open(os.path.join(goroot, "VERSION")).readline().strip()


def lit(b):
    """b as C string literals, one line of the text to each."""
    lines, cur = [], ""
    for i, c in enumerate(b):
        if c == 10:
            s = "\\n"
        elif c == 9:
            s = "\\t"
        elif c == 34:
            s = '\\"'
        elif c == 92:
            s = "\\\\"
        elif c == 63 and i + 1 < len(b) and b[i + 1] == 63:
            s = "\\?"
        elif 32 <= c < 127:
            s = chr(c)
        else:
            s = "\\%03o" % c
        # an octal escape must not run into a digit after it
        if s.startswith("\\") and s[1:].isdigit() and i + 1 < len(b):
            nxt = b[i + 1]
            if 48 <= nxt <= 55:
                cur += s
                lines.append('"' + cur + '"')
                cur = ""
                continue
        cur += s
        if c == 10 or len(cur) >= 70:
            lines.append('"' + cur + '"')
            cur = ""
    if cur or not lines:
        lines.append('"' + cur + '"')
    return lines


def c_str(b, indent):
    return ("\n" + " " * indent).join(lit(b))


ESC = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11,
       "\\": 92, '"': 34, "'": 39}


def go_tokens(body):
    """Go source as tokens: ("s", bytes) for a string literal, ("i", name) for
    a word and ("p", c) for anything else, comments and space skipped."""
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c in " \t\r\n":
            i += 1
        elif body.startswith("//", i):
            i = body.index("\n", i)
        elif c == "`":
            j = body.index("`", i + 1)
            out.append(("s", body[i + 1 : j].encode()))
            i = j + 1
        elif c == '"':
            s, i = bytearray(), i + 1
            while body[i] != '"':
                if body[i] != "\\":
                    s += body[i].encode()
                    i += 1
                    continue
                e = body[i + 1]
                if e in ESC:
                    s.append(ESC[e])
                    i += 2
                elif e == "x":
                    s.append(int(body[i + 2 : i + 4], 16))
                    i += 4
                elif e in "01234567":
                    s.append(int(body[i + 1 : i + 4], 8))
                    i += 4
                elif e == "u":
                    s += chr(int(body[i + 2 : i + 6], 16)).encode()
                    i += 6
                elif e == "U":
                    s += chr(int(body[i + 2 : i + 10], 16)).encode()
                    i += 10
                else:
                    raise ValueError("escape " + e)
            out.append(("s", bytes(s)))
            i += 1
        elif c.isalnum() or c == "_":
            j = i
            while j < len(body) and (body[j].isalnum() or body[j] == "_"):
                j += 1
            out.append(("i", body[i:j]))
            i = j
        else:
            out.append(("p", c))
            i += 1
    return out


CONSTS = {"quote": b"`"}


class Parser:
    def __init__(self, toks):
        self.toks, self.pos = toks, 0

    def peek(self, k=0):
        return self.toks[self.pos + k]

    def take(self, want=None):
        t = self.toks[self.pos]
        assert want is None or t[1] == want, (t, want)
        self.pos += 1
        return t

    def value(self):
        """A string expression, a bool, a name, or a composite literal as a
        list of values or a dict of fields."""
        kind, v = self.peek()
        if kind == "p" and v == "{":
            return self.composite()
        if kind == "i" and v in ("true", "false"):
            self.take()
            return v == "true"
        if kind == "i" and v == "map":
            while self.peek()[1] != "{":
                self.take()
            return self.composite()
        if kind == "i" and v not in CONSTS:
            self.take()
            return v
        b = b""
        while True:
            kind, v = self.take()
            b += CONSTS[v] if kind == "i" else v
            if self.peek()[1] != "+":
                return b
            self.take("+")

    def composite(self):
        self.take("{")
        fields, items = {}, []
        while self.peek()[1] != "}":
            if self.peek(1)[1] == ":":
                k = self.take()[1]
                self.take(":")
                fields[k.decode() if isinstance(k, bytes) else k] = self.value()
            else:
                items.append(self.value())
            if self.peek()[1] == ",":
                self.take()
        self.take("}")
        return fields if fields else items


def table(src, start):
    """The values of the composite literal that follows start in src."""
    at = src.index(start)
    at = src.index("}{", at) + 1 if "struct" in start else src.index("{", at + len(start) - 1)
    p = Parser(go_tokens(src[at:]))
    return p.composite()


w = sys.stdout.write
w("/* What Go's go/build tests read, from src/go/build of %s.\n" % gover)
w(" * Generated by tools/gen-go-build-tests.sh; do not edit. */\n\n")

w("typedef struct GbtFile {\n    const char *name;\n    const char *data;\n")
w("    long long len;\n} GbtFile;\n\n")
w("/* The files under testdata, by their slash separated paths. */\n")
w("static const GbtFile gbt_files[] = {\n")
names = []
for dirpath, dirs, files in os.walk(tdir):
    dirs.sort()
    for f in files:
        names.append(os.path.relpath(os.path.join(dirpath, f), pkg).replace(os.sep, "/"))
for name in sorted(names):
    data = open(os.path.join(pkg, name), "rb").read()
    w('    {"%s",\n     %s,\n     %d},\n' % (name, c_str(data, 5), len(data)))
w("};\n\n")

# read_test.go
src = open(os.path.join(pkg, "read_test.go")).read()
w("typedef struct GbtRead {\n    const char *in;\n    long long len;\n")
w("    const char *err;\n} GbtRead;\n\n")
for var in ("readGoInfoTests", "readCommentsTests", "readFailuresTests"):
    rows = table(src, "var %s = []readTest{" % var)
    cname = re.sub(r"([A-Z])", lambda m: "_" + m.group(1).lower(), var)
    w("static const GbtRead gbt_%s[] = {\n" % cname)
    for row in rows:
        inp, err = row
        w("    {%s,\n     %d,\n     %s},\n" % (c_str(inp, 5), len(inp), c_str(err, 5)))
    assert len(rows) >= 6, var
    w("};\n\n")

rows = table(src, "var readEmbedTests = []struct {")
w("typedef struct GbtEmbed {\n    const char *in;\n    const char *out;\n} GbtEmbed;\n\n")
w("/* readEmbedTests, with the space in each out turned into one newline, as the\n")
w(" * test does with strings.Fields. */\n")
w("static const GbtEmbed gbt_read_embed_tests[] = {\n")
for inp, outp in rows:
    outp = b"\n".join(outp.split())
    w("    {%s,\n     %s},\n" % (c_str(inp, 5), c_str(outp, 5)))
assert len(rows) >= 10, "readEmbedTests"
w("};\n\n")

# build_test.go
src = open(os.path.join(pkg, "build_test.go")).read()
rows = table(src, "var shouldBuildTests = []struct {")
w("typedef struct GbtShouldBuild {\n    const char *name;\n    const char *content;\n")
w("    const char *tags[4];\n    int ntags;\n    bool binary_only;\n")
w("    bool should_build;\n} GbtShouldBuild;\n\n")
w("static const GbtShouldBuild gbt_should_build_tests[] = {\n")
for r in rows:
    tags = r.get("tags", {})
    if isinstance(tags, list):
        tags = {}
    assert all(v is True for v in tags.values()) and len(tags) <= 4
    w('    {"%s",\n     %s,\n     {%s},\n     %d,\n     %s,\n     %s},\n'
      % (r["name"].decode(), c_str(r["content"], 5),
         ", ".join('"%s"' % k for k in sorted(tags)) or "NULL", len(tags),
         "true" if r.get("binaryOnly", False) else "false",
         "true" if r.get("shouldBuild", False) else "false"))
assert len(rows) >= 20, "shouldBuildTests"
w("};\n\n")

rows = table(src, "var matchFileTests = []struct {")
w("typedef struct GbtMatchFile {\n    const char *ctxt;\n    const char *name;\n")
w("    const char *data;\n    bool match;\n} GbtMatchFile;\n\n")
w("/* matchFileTests, with ctxtP9 as \"plan9\" and ctxtAndroid as \"android\",\n")
w(" * both for GOARCH=arm. */\n")
w("static const GbtMatchFile gbt_match_file_tests[] = {\n")
for ctxt, name, data, match in rows:
    goos = {"ctxtP9": "plan9", "ctxtAndroid": "android"}[ctxt]
    w('    {"%s", %s, %s, %s},\n'
      % (goos, c_str(name, 5), c_str(data, 5), "true" if match else "false"))
assert len(rows) >= 10, "matchFileTests"
w("};\n\n")

rows = table(src, "var expandSrcDirTests = []struct {")
w("typedef struct GbtExpand {\n    const char *input;\n    const char *expected;\n")
w("} GbtExpand;\n\n")
w("static const GbtExpand gbt_expand_src_dir_tests[] = {\n")
for inp, exp in rows:
    w("    {%s, %s},\n" % (c_str(inp, 5), c_str(exp, 5)))
assert len(rows) >= 8, "expandSrcDirTests"
w("};\n\n")

fn = src[src.index("func TestShellSafety("):]
rows = table(fn, "tests := []struct {")
w("typedef struct GbtShellSafety {\n    const char *input;\n    const char *srcdir;\n")
w("    const char *expected;\n    bool result;\n} GbtShellSafety;\n\n")
w("static const GbtShellSafety gbt_shell_safety_tests[] = {\n")
for inp, srcdir, exp, result in rows:
    w("    {%s, %s, %s, %s},\n" % (c_str(inp, 5), c_str(srcdir, 5), c_str(exp, 5),
                                 "true" if result else "false"))
assert len(rows) >= 6, "TestShellSafety"
w("};\n")
PY
