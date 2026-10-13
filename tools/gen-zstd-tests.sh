#!/bin/sh
# Regenerates tests/zstd_test_gen.h: zstd streams and what Go's internal/zstd
# reader makes of each of them. The streams are the ones in Go's tests and
# testdata, ones zstd(1) made from text, noise and long runs at several levels
# and block sizes, and damaged copies of the small ones. tests/zstd_test.c
# reads every stream and has to get the same bytes and the same error.
#
# internal/zstd cannot be imported from outside the standard library, so its
# files are copied out of GOSRC, a Go source tree that defaults to the one the
# go command has, into a scratch module. zstd(1) is only needed here.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/zstd_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
gosrc=${GOSRC:-$(go env GOROOT)/src}

mkdir "$tmp/zstd" "$tmp/testdata" "$tmp/made"
for f in "$gosrc/internal/zstd"/*.go; do
	case $f in *_test.go) continue ;; esac
	cp "$f" "$tmp/zstd/"
done
cp "$gosrc/internal/zstd/zstd_test.go" "$tmp/zstd_test.go.txt"
cp "$gosrc/internal/zstd/fuzz_test.go" "$tmp/fuzz_test.go.txt"
cp "$gosrc/internal/zstd/testdata/"*.zst "$tmp/testdata/"
head -c 65536 "$gosrc/testdata/Isaac.Newton-Opticks.txt" > "$tmp/made/opticks"

cat > "$tmp/main.go" <<'GO'
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"gen/zstd"
)

// strs evaluates the string literals, joined with +, in the composite
// literal elements of the variable called name in a Go file.
func strs(path, name string) [][]string {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, path, nil, 0)
	if err != nil {
		panic(err)
	}
	var out [][]string
	ast.Inspect(f, func(n ast.Node) bool {
		vs, ok := n.(*ast.ValueSpec)
		if !ok || len(vs.Names) != 1 || vs.Names[0].Name != name {
			return true
		}
		for _, elt := range vs.Values[0].(*ast.CompositeLit).Elts {
			if cl, ok := elt.(*ast.CompositeLit); ok {
				var row []string
				for _, e := range cl.Elts {
					row = append(row, eval(e))
				}
				out = append(out, row)
			} else {
				out = append(out, []string{eval(elt)})
			}
		}
		return false
	})
	return out
}

func eval(e ast.Expr) string {
	switch e := e.(type) {
	case *ast.BasicLit:
		s, err := strconv.Unquote(e.Value)
		if err != nil {
			panic(err)
		}
		return s
	case *ast.BinaryExpr:
		return eval(e.X) + eval(e.Y)
	case *ast.ParenExpr:
		return eval(e.X)
	}
	panic(fmt.Sprintf("cannot evaluate %T", e))
}

func lcg(x uint64) uint64 { return x*6364136223846793005 + 1442695040888963407 }

func words(n int) []byte {
	vocab := strings.Fields("the quick brown fox jumped over lazy dog and a of to in " +
		"is that it was for on are as with his they at be this from I have or by")
	var b []byte
	x := uint64(7)
	for len(b) < n {
		x = lcg(x)
		b = append(b, vocab[(x>>33)%uint64(len(vocab))]...)
		if x>>60 == 0 {
			b = append(b, '\n')
		} else {
			b = append(b, ' ')
		}
	}
	return b[:n]
}

func noise(n int, seed uint64) []byte {
	b := make([]byte, n)
	x := seed
	for i := range b {
		x = lcg(x)
		b[i] = byte(x >> 56)
	}
	return b
}

func runs(n int, seed uint64) []byte {
	var b []byte
	x := seed
	for len(b) < n {
		x = lcg(x)
		c := byte(x >> 56)
		k := 1 + int((x>>33)%3000)
		for i := 0; i < k; i++ {
			b = append(b, c)
		}
	}
	return b[:n]
}

func compress(in []byte, args ...string) []byte {
	cmd := exec.Command("zstd", append([]string{"-q", "-c"}, args...)...)
	cmd.Stdin = bytes.NewReader(in)
	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		panic(err)
	}
	return out.Bytes()
}

type stream struct {
	name string
	data []byte
	want []byte // what it should decompress to, when that is known
	bad  bool   // Go's test wants an error
}

type result struct {
	n   int
	sum string
	err string
}

func decode(b []byte) (res result, ok bool) {
	defer func() {
		if recover() != nil {
			ok = false
		}
	}()
	got, err := io.ReadAll(zstd.NewReader(bytes.NewReader(b)))
	res.n = len(got)
	res.sum = fmt.Sprintf("%x", sha256.Sum256(got))
	if err != nil {
		res.err = err.Error()
	}
	return res, true
}

func cstr(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '?':
			b.WriteString("\\?")
		case c >= 0x20 && c < 0x7f:
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func main() {
	var ss []stream
	for _, t := range strs("zstd_test.go.txt", "tests") {
		ss = append(ss, stream{name: t[0], data: []byte(t[2]), want: []byte(t[1])})
	}
	for i, t := range strs("fuzz_test.go.txt", "badStrings") {
		ss = append(ss, stream{name: fmt.Sprintf("badStrings#%d", i), data: []byte(t[0]), bad: true})
	}
	files, _ := filepath.Glob("testdata/*.zst")
	sort.Strings(files)
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			panic(err)
		}
		ss = append(ss, stream{name: filepath.Base(f), data: b})
	}
	opticks, err := os.ReadFile("made/opticks")
	if err != nil {
		panic(err)
	}
	inputs := []struct {
		name string
		b    []byte
	}{
		{"empty", nil},
		{"one byte", []byte("x")},
		{"words", words(60000)},
		{"noise", noise(20000, 1)},
		{"runs", runs(300000, 2)},
		{"opticks", opticks},
	}
	for _, in := range inputs {
		for _, args := range [][]string{{"-1"}, {"-3"}, {"-19"}, {"--ultra", "-22"},
			{"-3", "--no-check"}, {"-3", "-B4096"}, {"-9", "--long=24"}} {
			if len(in.b) < 1000 && len(args) > 1 {
				continue
			}
			ss = append(ss, stream{
				name: in.name + " zstd " + strings.Join(args, " "),
				data: compress(in.b, args...),
				want: in.b,
			})
		}
	}
	// Two frames and a skippable frame between them.
	two := compress([]byte("first frame\n"), "-3")
	two = append(two, 0x5e, 0x2a, 0x4d, 0x18, 3, 0, 0, 0, 'a', 'b', 'c')
	two = append(two, compress([]byte("second frame\n"), "-3")...)
	ss = append(ss, stream{name: "two frames", data: two, want: []byte("first frame\nsecond frame\n")})

	// Damaged copies of the small streams: bits flipped, bytes changed, cut
	// short, and something after the end.
	n := len(ss)
	x := uint64(99)
	for i := 0; i < n; i++ {
		s := ss[i]
		if s.bad || len(s.data) < 8 || len(s.data) > 4096 {
			continue
		}
		for j := 0; j < 24; j++ {
			x = lcg(x)
			b := append([]byte(nil), s.data...)
			pos := int(x>>33) % len(b)
			var what string
			if j < 12 {
				b[pos] ^= byte(1 << ((x >> 20) % 8))
				what = fmt.Sprintf("bit at %d", pos)
			} else {
				b[pos] = byte(x >> 56)
				what = fmt.Sprintf("byte at %d", pos)
			}
			ss = append(ss, stream{name: s.name + ", " + what, data: b})
		}
		for _, cut := range []int{1, 4, 5, 6, len(s.data) / 2, len(s.data) - 1} {
			if cut < len(s.data) {
				ss = append(ss, stream{name: fmt.Sprintf("%s, cut at %d", s.name, cut), data: s.data[:cut]})
			}
		}
		ss = append(ss, stream{name: s.name + ", junk after", data: append(append([]byte(nil), s.data...), "junk"...)})
	}

	w := os.Stdout
	fmt.Fprintf(w, "/* Generated by tools/gen-zstd-tests.sh from Go's internal/zstd. Do not\n * edit. */\n\n")
	fmt.Fprintf(w, "#ifndef BURROW_TESTS_ZSTD_TEST_GEN_H\n#define BURROW_TESTS_ZSTD_TEST_GEN_H\n\n")
	var rows []string
	for i, s := range ss {
		res, ok := decode(s.data)
		if !ok {
			continue
		}
		if s.want != nil && (res.err != "" || res.sum != fmt.Sprintf("%x", sha256.Sum256(s.want))) {
			panic("Go's reader got " + s.name + " wrong")
		}
		if s.bad && res.err == "" {
			panic("Go's reader read " + s.name)
		}
		b64 := base64.StdEncoding.EncodeToString(s.data)
		fmt.Fprintf(w, "/* %s, %d bytes. */\n", strings.ReplaceAll(s.name, "*/", "* /"), len(s.data))
		fmt.Fprintf(w, "static const char *const zstd_stream_%d[] = {\n", i)
		for len(b64) > 0 {
			k := min(72, len(b64))
			fmt.Fprintf(w, "    \"%s\",\n", b64[:k])
			b64 = b64[k:]
		}
		fmt.Fprintf(w, "    NULL,\n};\n\n")
		rows = append(rows, fmt.Sprintf("    {%s, zstd_stream_%d, %d, \"%s\", %s},\n",
			cstr(s.name), i, res.n, res.sum, cstr(res.err)))
	}
	fmt.Fprintf(w, "static const ZstdCase zstd_cases[] = {\n")
	for _, r := range rows {
		fmt.Fprint(w, r)
	}
	fmt.Fprintf(w, "};\n\n#endif\n")
}
GO

cd "$tmp"
cat > go.mod <<'MOD'
module gen

go 1.22
MOD
go run . > "$out"
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
