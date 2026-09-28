#!/bin/sh
# Regenerates tests/flate_writer_test_gen.h: what Go's compress/flate writer
# makes of a set of inputs at every level, for the writer tests to match byte
# for byte, and the huffman bit writer cases of huffman_bit_writer_test.go with
# their expected output.
#
# Go may fuse a multiply and an add into one instruction on arm64 and not on
# amd64, which changes the size estimates the writer picks blocks with. The
# numbers here are amd64 ones, so on another machine the program is built for
# linux/amd64 and run on GEN_SSH, an amd64 host that can be reached with ssh.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/flate_writer_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
goroot=$(go env GOROOT)

mkdir "$tmp/testdata"
cp "$goroot"/src/compress/flate/testdata/huffman-* "$goroot"/src/compress/flate/testdata/null-long-match.* \
	"$goroot/src/compress/testdata/gettysburg.txt" "$tmp/testdata/"

# writeBlockTests and ml, as they are in Go's test.
{
	echo 'package main'
	echo
	echo 'type token uint32'
	echo
	echo 'type huffTest struct {'
	echo '	tokens      []token'
	echo '	input       string'
	echo '	want        string'
	echo '	wantNoInput string'
	echo '}'
	echo
	grep '^const ml = ' "$goroot/src/compress/flate/huffman_bit_writer_test.go"
	echo
	sed -n '/^var writeBlockTests = /,/^}/p' "$goroot/src/compress/flate/huffman_bit_writer_test.go"
} > "$tmp/tests.go"

cat > "$tmp/main.go" <<'GO'
package main

import (
	"bytes"
	"compress/flate"
	"crypto/sha256"
	"embed"
	"encoding/base64"
	"fmt"
	"os"
	"runtime"
	"strings"
)

//go:embed testdata
var testdata embed.FS

var w = new(strings.Builder)

func read(name string) []byte {
	b, err := testdata.ReadFile(name)
	if err != nil {
		panic(err)
	}
	return b
}

func sum(b []byte) string {
	h := sha256.Sum256(b)
	return fmt.Sprintf("%x", h)
}

// Bytes as base64, in lines the test joins and decodes.
func arr(name string, b []byte) {
	s := base64.StdEncoding.EncodeToString(b)
	fmt.Fprintf(w, "static const char *const %s[] = {", name)
	for len(s) > 72 {
		fmt.Fprintf(w, "\n    \"%s\",", s[:72])
		s = s[72:]
	}
	fmt.Fprintf(w, "\n    \"%s\",\n    NULL,\n};\n", s)
}

// The same words the C test makes with flate_words.
func words(n int) []byte {
	vocab := strings.Fields("the quick brown fox jumped over lazy dog and a of to in " +
		"is that it was for on are as with his they at be this from I have or by " +
		"one had not but what all were when we there can an your which their said " +
		"if do will each about how up out them then she many some so these would")
	var b []byte
	x := uint64(1)
	for len(b) < n {
		x = x*6364136223846793005 + 1442695040888963407
		b = append(b, vocab[(x>>33)%uint64(len(vocab))]...)
		if x>>60 == 0 {
			b = append(b, '\n')
		} else {
			b = append(b, ' ')
		}
	}
	return b[:n]
}

// Bytes from the same generator, seeded, the top byte of each step.
func noise(n int, seed uint64, mask byte) []byte {
	b := make([]byte, n)
	x := seed
	for i := range b {
		x = x*6364136223846793005 + 1442695040888963407
		b[i] = byte(x>>56) & mask
	}
	return b
}

// The inputs, numbered as flate_input in the C test numbers them.
func input(id int) []byte {
	switch id {
	case 0:
		return read("testdata/gettysburg.txt")
	case 1:
		return words(34000)
	case 2:
		return words(300000)
	case 3:
		return noise(70000, 7, 0xff)
	case 4:
		return noise(65535*3+500, 1, 7)
	case 5:
		b := make([]byte, 100000)
		for i := range b {
			b[i] = byte(i * i & 0xff)
		}
		return b
	case 6:
		return make([]byte, 100000)
	case 7:
		b := make([]byte, 131072)
		for i := range b {
			b[i] = byte(i % 128)
		}
		return b
	case 8:
		// Text and noise in turn, 5000 bytes of each.
		wd := words(100000)
		nz := noise(100000, 3, 0xff)
		var b []byte
		for i := 0; i < 100000; i += 5000 {
			b = append(b, wd[i:i+5000]...)
			b = append(b, nz[i:i+5000]...)
		}
		return b
	case 9:
		return nil
	case 10:
		return []byte("hello, hello, hello, hello\n")
	}
	panic("no such input")
}

const numInputs = 11

var smallDict = []byte(strings.Repeat("we are the world - how are you?", 3))

// Writes in to a new writer at level the way pattern says, as the C test's
// flate_pattern does, and gives what came out.
func run(level, pattern int, in []byte) []byte {
	var buf bytes.Buffer
	var fw *flate.Writer
	var err error
	switch pattern {
	case 4:
		fw, err = flate.NewWriterDict(&buf, level, words(34000))
	case 5:
		fw, err = flate.NewWriterDict(&buf, level, smallDict)
	default:
		fw, err = flate.NewWriter(&buf, level)
	}
	if err != nil {
		panic(err)
	}
	chunk := len(in)
	flush := false
	switch pattern {
	case 1:
		chunk = 787
	case 2:
		chunk = 81761
	case 3:
		chunk, flush = 10000, true
	case 6:
		chunk = 1
	}
	if chunk == 0 {
		chunk = 1
	}
	for i := 0; i < len(in); i += chunk {
		fw.Write(in[i:min(i+chunk, len(in))])
		if flush {
			fw.Flush()
		}
	}
	if len(in) == 0 {
		fw.Write(in)
	}
	fw.Close()
	return buf.Bytes()
}

func main() {
	w.WriteString(`/* What Go's compress/flate writer makes, ` + runtime.Version() + ` on ` + runtime.GOARCH + `. The
 * writer tests match it. Regenerate it with tools/gen-flate-writer-tests.sh
 * rather than editing it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

`)
	w.WriteString("typedef struct FlateVector {\n    int input;\n    int level;\n    int pattern;\n    Int len;\n    const char *sha256;\n} FlateVector;\n\n")
	w.WriteString("static const FlateVector flate_vectors[] = {\n")
	for id := 0; id < numInputs; id++ {
		in := input(id)
		for pattern := 0; pattern <= 6; pattern++ {
			if pattern == 6 && len(in) > 40000 {
				continue
			}
			for level := -2; level <= 9; level++ {
				got := run(level, pattern, in)
				fmt.Fprintf(w, "    {%d, %d, %d, %d, \"%s\"},\n", id, level, pattern, len(got), sum(got))
			}
		}
	}
	w.WriteString("};\n\n")

	// huffman_bit_writer_test.go.
	names := map[string]string{}
	for i, t := range writeBlockTests {
		name := fmt.Sprintf("hbw_tokens_%d", i)
		fmt.Fprintf(w, "static const uint32_t %s[] = {", name)
		for j, tok := range t.tokens {
			if j%8 == 0 {
				w.WriteString("\n   ")
			}
			fmt.Fprintf(w, " 0x%x,", uint32(tok))
		}
		w.WriteString("\n};\n")
		if t.input != "" {
			in := "hbw_input_" + strings.TrimSuffix(strings.TrimPrefix(t.input, "testdata/"), ".in")
			in = strings.ReplaceAll(in, "-", "_")
			names[t.input] = in
			arr(in, read(t.input))
		}
	}
	w.WriteString("\ntypedef struct HuffTest {\n    const uint32_t *tokens;\n    Int ntokens;\n    const char *const *input; /* NULL for none */\n    const char *name;\n    /* wb, dyn and sync, with the input and without */\n    Int want_len[3];\n    const char *want[3];\n    Int want_noinput_len[3];\n    const char *want_noinput[3];\n} HuffTest;\n\n")
	w.WriteString("static const HuffTest huff_tests[] = {\n")
	for i, t := range writeBlockTests {
		in := "NULL"
		if t.input != "" {
			in = names[t.input]
		}
		name := t.wantNoInput[len("testdata/"):strings.Index(t.wantNoInput, ".%s")]
		fmt.Fprintf(w, "    {hbw_tokens_%d, %d, %s, \"%s\",\n", i, len(t.tokens), in, name)
		var lens, sums, nlens, nsums []string
		for _, typ := range []string{"wb", "dyn", "sync"} {
			if t.want != "" {
				b := read(fmt.Sprintf(t.want, typ))
				lens = append(lens, fmt.Sprint(len(b)))
				sums = append(sums, `"`+sum(b)+`"`)
			} else {
				lens = append(lens, "0")
				sums = append(sums, "NULL")
			}
			b := read(fmt.Sprintf(t.wantNoInput, typ))
			nlens = append(nlens, fmt.Sprint(len(b)))
			nsums = append(nsums, `"`+sum(b)+`"`)
		}
		fmt.Fprintf(w, "     {%s},\n     {%s},\n     {%s},\n     {%s}},\n",
			strings.Join(lens, ", "), strings.Join(sums, ", "), strings.Join(nlens, ", "), strings.Join(nsums, ", "))
	}
	w.WriteString("};\n\n")

	// TestBlockHuff: every huffman-*.in through writeBlockHuff, against its
	// .golden file.
	ents, _ := testdata.ReadDir("testdata")
	w.WriteString("typedef struct HuffGolden {\n    const char *const *input;\n    const char *name;\n    Int len;\n    const char *sha256;\n} HuffGolden;\n\n")
	var golds []string
	for _, e := range ents {
		n := e.Name()
		if !strings.HasPrefix(n, "huffman-") || !strings.HasSuffix(n, ".in") {
			continue
		}
		key := "testdata/" + n
		in, ok := names[key]
		if !ok {
			in = "hbw_input_" + strings.ReplaceAll(strings.TrimSuffix(n, ".in"), "-", "_")
			names[key] = in
			arr(in, read(key))
		}
		g := read("testdata/" + strings.TrimSuffix(n, ".in") + ".golden")
		golds = append(golds, fmt.Sprintf("    {%s, \"%s\", %d, \"%s\"},\n", in, strings.TrimSuffix(n, ".in"), len(g), sum(g)))
	}
	w.WriteString("static const HuffGolden huff_goldens[] = {\n")
	for _, g := range golds {
		w.WriteString(g)
	}
	w.WriteString("};\n")
	os.Stdout.WriteString(w.String())
}
GO
cd "$tmp"
go mod init gen >/dev/null 2>&1
if [ "$(go env GOARCH)" = amd64 ]; then
	go run . > "$out"
else
	: "${GEN_SSH:?set GEN_SSH to an amd64 host to run the generator on}"
	GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build -o gen .
	scp -q gen "$GEN_SSH:/tmp/burrow-flate-gen"
	ssh "$GEN_SSH" '/tmp/burrow-flate-gen; rm -f /tmp/burrow-flate-gen' > "$out"
fi
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
