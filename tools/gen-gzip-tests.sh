#!/bin/sh
# Regenerates tests/gzip_test_gen.h for tests/gzip_test.c. It has three things
# in it, all taken from Go itself:
#
#   - gunzipTests from Go's gunzip_test.go, the streams and what reading them
#     gives, copied out by a test added to a copy of the package;
#   - testdata/issue6550.gz.base64, the corrupt stream TestIssue6550 reads;
#   - the SHA-256 of what Go's writer makes of a set of inputs at every level,
#     with an empty header and a full one, for the C writer to match byte for
#     byte.
#
# The DEFLATE inside is compress/flate's, whose output can differ between
# amd64 and arm64 (tools/gen-flate-writer-tests.sh says why), so on another
# machine the test binary is built for linux/amd64 and run on GEN_SSH, an amd64
# host that can be reached with ssh.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/gzip_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
goroot=$(go env GOROOT)

mkdir -p "$tmp/gzip/testdata"
cp "$goroot/src/compress/gzip/gunzip.go" "$goroot/src/compress/gzip/gzip.go" \
	"$goroot/src/compress/gzip/gunzip_test.go" "$tmp/gzip/"
cp "$goroot/src/compress/gzip/testdata/issue6550.gz.base64" "$tmp/gzip/testdata/"
cp "$goroot/src/compress/testdata/gettysburg.txt" "$tmp/gzip/testdata/"

cat > "$tmp/gzip/gen_test.go" <<'GO'
package gzip

import (
	"bytes"
	"crypto/sha256"
	_ "embed"
	"fmt"
	"io"
	"os"
	"runtime"
	"strings"
	"testing"
	"time"
)

//go:embed testdata/gettysburg.txt
var gettysburg []byte

//go:embed testdata/issue6550.gz.base64
var issue6550 string

// The same words the C test makes with gzip_words.
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
func noise(n int, seed uint64) []byte {
	b := make([]byte, n)
	x := seed
	for i := range b {
		x = x*6364136223846793005 + 1442695040888963407
		b[i] = byte(x >> 56)
	}
	return b
}

// The inputs, numbered as gzip_input in the C test numbers them.
func input(id int) []byte {
	switch id {
	case 0:
		return gettysburg
	case 1:
		return words(34000)
	case 2:
		return noise(70000, 7)
	case 3:
		return nil
	}
	return []byte("hello, world\n")
}

// The headers, numbered as gzip_set_header in the C test numbers them.
func header(id int) Header {
	if id == 0 {
		return Header{OS: 255}
	}
	return Header{
		Comment: "Grüße aus Gettysburg",
		Extra:   []byte("ex\x00tra"),
		ModTime: time.Unix(1_000_000_000, 0),
		Name:    "gettysburg.txt",
		OS:      3,
	}
}

func cString(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 0x20 && c < 0x7f && c != '"' && c != '\\' && c != '?' {
			b.WriteByte(c)
		} else {
			fmt.Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func errName(err error) string {
	switch err {
	case nil:
		return "GZ_OK"
	case ErrChecksum:
		return "GZ_CHECKSUM"
	case ErrHeader:
		return "GZ_HEADER"
	case io.ErrUnexpectedEOF:
		return "GZ_UNEXPECTED_EOF"
	}
	panic(err)
}

func TestGen(t *testing.T) {
	var w strings.Builder
	w.WriteString(`/* From Go's compress/gzip, ` + runtime.Version() + ` on ` + runtime.GOARCH + `. The input and header
 * numbers are gzip_input and gzip_set_header in tests/gzip_test.c. Generated
 * by tools/gen-gzip-tests.sh; do not edit. */

enum { GZ_OK, GZ_CHECKSUM, GZ_HEADER, GZ_UNEXPECTED_EOF };

typedef struct GunzipTest {
    const char *name;
    const char *desc;
    const char *raw;
    int raw_len;
    const char *gzip;
    int gzip_len;
    int err;
} GunzipTest;

static const GunzipTest gunzip_tests[] = {
`)
	for _, tt := range gunzipTests {
		fmt.Fprintf(&w, "    {%s, %s, %s, %d, %s, %d, %s},\n", cString(tt.name), cString(tt.desc),
			cString(tt.raw), len(tt.raw), cString(string(tt.gzip)), len(tt.gzip), errName(tt.err))
	}
	w.WriteString("};\n\n/* testdata/issue6550.gz.base64, a line at a time. */\nstatic const char *const issue6550[] = {\n")
	for _, line := range strings.Split(strings.TrimSpace(issue6550), "\n") {
		for len(line) > 0 {
			n := min(len(line), 76)
			fmt.Fprintf(&w, "    %s,\n", cString(line[:n]))
			line = line[n:]
		}
	}
	w.WriteString(`    NULL,
};

typedef struct GzipVector {
    int input;
    int level;
    int header;
    long long len;
    const char *sha256;
} GzipVector;

static const GzipVector gzip_vectors[] = {
`)
	for in := 0; in < 5; in++ {
		for level := -2; level <= 9; level++ {
			for h := 0; h < 2; h++ {
				var buf bytes.Buffer
				zw, err := NewWriterLevel(&buf, level)
				if err != nil {
					t.Fatal(err)
				}
				zw.Header = header(h)
				if _, err := zw.Write(input(in)); err != nil {
					t.Fatal(err)
				}
				if err := zw.Close(); err != nil {
					t.Fatal(err)
				}
				zr, err := NewReader(bytes.NewReader(buf.Bytes()))
				if err != nil {
					t.Fatal(err)
				}
				got, err := io.ReadAll(zr)
				if err != nil || !bytes.Equal(got, input(in)) {
					t.Fatalf("input %d level %d header %d does not read back", in, level, h)
				}
				fmt.Fprintf(&w, "    {%d, %d, %d, %d, \"%x\"},\n", in, level, h, buf.Len(), sha256.Sum256(buf.Bytes()))
			}
		}
	}
	w.WriteString("};\n")
	os.Stdout.WriteString(w.String())
}
GO
cd "$tmp/gzip"
cat > go.mod <<'MOD'
module gzipgen

go 1.23
MOD
if [ "$(go env GOARCH)" = amd64 ]; then
	go test -run '^TestGen$' . | sed '/^ok/d;/^PASS$/d' > "$out"
else
	: "${GEN_SSH:?set GEN_SSH to an amd64 host to run the generator on}"
	GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go test -c -o gen .
	scp -q gen "$GEN_SSH:/tmp/burrow-gzip-gen"
	ssh "$GEN_SSH" '/tmp/burrow-gzip-gen -test.run "^TestGen$"; rm -f /tmp/burrow-gzip-gen' |
		sed '/^PASS$/d' > "$out"
fi
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
