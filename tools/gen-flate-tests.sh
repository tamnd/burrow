#!/bin/sh
# Regenerates tests/flate_test_gen.h: DEFLATE streams made by Go's
# compress/flate writer, for the reader tests to decode until this library has
# a writer of its own. Go's reader tests make these on the fly with NewWriter.
# Here Go makes them once, and the header records what it made.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/flate_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/main.go" <<'GO'
package main

import (
	"bytes"
	"compress/flate"
	"encoding/base64"
	"fmt"
	"os"
	"runtime"
	"strings"
)

var w = new(strings.Builder)

// A stream as base64, in lines the test joins and decodes, because as a C
// array of bytes the header would be four times the size, and one string
// literal would be longer than C promises to allow.
func arr(name string, b []byte) {
	s := base64.StdEncoding.EncodeToString(b)
	fmt.Fprintf(w, "static const char *const %s[] = {", name)
	for len(s) > 72 {
		fmt.Fprintf(w, "\n    \"%s\",", s[:72])
		s = s[72:]
	}
	fmt.Fprintf(w, "\n    \"%s\",\n    NULL,\n};\n", s)
}

func deflate(level int, dict []byte, flush bool, p []byte) []byte {
	var buf bytes.Buffer
	var fw *flate.Writer
	if dict != nil {
		fw, _ = flate.NewWriterDict(&buf, level, dict)
	} else {
		fw, _ = flate.NewWriter(&buf, level)
	}
	fw.Write(p)
	if flush {
		fw.Flush()
	}
	fw.Close()
	return buf.Bytes()
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

func main() {
	w.WriteString(`/* DEFLATE streams from Go's compress/flate writer, ` + runtime.Version() + `. The
 * reader tests decode them. Regenerate it with tools/gen-flate-tests.sh rather
 * than editing it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

`)
	// TestReset and TestResetDict.
	ss := []string{"lorem ipsum izzle fo rizzle", "the quick brown fox jumped over"}
	for i, s := range ss {
		arr(fmt.Sprintf("reset_%d", i), deflate(1, nil, false, []byte(s)))
		arr(fmt.Sprintf("reset_dict_%d", i), deflate(flate.DefaultCompression, []byte("the lorem fox"), false, []byte(s)))
	}

	// TestReaderEarlyEOF: data[i] = byte(i) at level 5, with and without a
	// Flush after the data.
	sizes := []int{1, 2, 3, 4, 5, 6, 7, 8, 100, 1000, 10000, 100000, 128, 1024, 16384, 131072,
		32768, 65536, 98304}
	data := make([]byte, 131072)
	for i := range data {
		data[i] = byte(i)
	}
	for i, sz := range sizes {
		arr(fmt.Sprintf("early_%d_flush", i), deflate(5, nil, true, data[:sz]))
		arr(fmt.Sprintf("early_%d", i), deflate(5, nil, false, data[:sz]))
	}
	w.WriteString("typedef struct FlateEarly {\n    Int size;\n    const char *const *flushed;\n    const char *const *plain;\n} FlateEarly;\n\n")
	w.WriteString("static const FlateEarly flate_early[] = {\n")
	for i, sz := range sizes {
		fmt.Fprintf(w, "    {%d, early_%d_flush, early_%d},\n", sz, i, i)
	}
	w.WriteString("};\n\n")

	// Whole streams at every level: the Gettysburg Address, which is short
	// enough to store, and 34000 bytes of words, which is longer than the
	// window.
	gb, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	fmt.Fprintf(w, "#define GETTYSBURG_LEN %d\n", len(gb))
	arr("gettysburg", gb)
	levels := []int{-2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9}
	names := []string{}
	for _, l := range levels {
		n := fmt.Sprintf("gettysburg_l%d", l+2)
		arr(n, deflate(l, nil, false, gb))
		names = append(names, fmt.Sprintf("    {%d, 0, %s},\n", l, n))
	}
	wd := words(34000)
	for _, l := range []int{-2, 1, 6, 9} {
		n := fmt.Sprintf("words_l%d", l+2)
		arr(n, deflate(l, nil, false, wd))
		names = append(names, fmt.Sprintf("    {%d, 1, %s},\n", l, n))
	}
	arr("words_dict", deflate(6, wd[:5000], false, wd[5000:]))
	names = append(names, "    {6, 2, words_dict},\n")
	w.WriteString("typedef struct FlateStream {\n    int level;\n    int input; /* 0 gettysburg, 1 words, 2 words after a dictionary */\n    const char *const *b64;\n} FlateStream;\n\n")
	w.WriteString("static const FlateStream flate_streams[] = {\n")
	for _, n := range names {
		w.WriteString(n)
	}
	w.WriteString("};\n")
	os.Stdout.WriteString(w.String())
}
GO
(cd "$tmp" && go mod init gen >/dev/null 2>&1 && go run . "$(go env GOROOT)/src/compress/testdata/gettysburg.txt") > "$out"
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
