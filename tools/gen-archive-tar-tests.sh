#!/bin/sh
# Regenerates tests/tar_test_gen.h for tests/tar_test.c. It runs Go's own
# archive/tar tests on a copy of the package, with a call added in front of the
# loop of each table-driven test that writes the table out as C, so the C
# tests go through the same vectors and expect the same results:
#
#   - the tables of strconv_test.go, reader_test.go, tar_test.go and
#     writer_test.go, with their headers, sparse maps, file operations and
#     errors, the errors named by which of the package's errors they are;
#   - every file in testdata, decoded where it is obscured with base64 and
#     decompressed where it is bzip2, and then compressed with gzip, which the
#     C test undoes with burrow's own reader.
#
# The copy of the package cannot import Go's internal packages from outside
# GOROOT, so it gets small stand-ins for the three it uses.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/tar_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
goroot=$(go env GOROOT)
src="$goroot/src/archive/tar"

mkdir -p "$tmp/tar"
cp -R "$src/testdata" "$tmp/tar/"
for f in common.go format.go reader.go strconv.go writer.go stat_actime1.go \
	stat_actime2.go stat_unix.go reader_test.go strconv_test.go tar_test.go \
	writer_test.go; do
	cp "$src/$f" "$tmp/tar/"
done
cd "$tmp/tar"

cat > go.mod <<'EOF'
module tar

go 1.27
EOF

# The stand-ins for the internal packages.
perl -0pi -e 's/\t"internal\/godebug"\n//' common.go
perl -0pi -e 's/\t"internal\/testenv"\n//' tar_test.go
perl -0pi -e 's/\t"internal\/obscuretestdata"\n//' reader_test.go writer_test.go

cat > shim.go <<'GO'
package tar

import (
	"os"
	"strings"
)

type godebugSetting struct{ name string }

var godebug = struct{ New func(string) *godebugSetting }{
	func(name string) *godebugSetting { return &godebugSetting{name} },
}

func (s *godebugSetting) Value() string {
	v := ""
	for _, kv := range strings.Split(os.Getenv("GODEBUG"), ",") {
		if k, x, ok := strings.Cut(kv, "="); ok && k == s.name {
			v = x
		}
	}
	return v
}

func (s *godebugSetting) IncNonDefault() {}
GO

cat > shim_test.go <<'GO'
package tar

import (
	"encoding/base64"
	"io"
	"os"
	"runtime"
	"testing"
)

var testenv = struct{ MustHaveSymlink func(testing.TB) }{
	func(t testing.TB) {
		if runtime.GOOS == "windows" {
			t.Skip("no symlinks")
		}
	},
}

var obscuretestdata = struct {
	DecodeToTempFile func(string) (string, error)
}{
	func(name string) (string, error) {
		f, err := os.Open(name)
		if err != nil {
			return "", err
		}
		defer f.Close()
		tmp, err := os.CreateTemp("", "obscuretestdata-decoded-")
		if err != nil {
			return "", err
		}
		defer tmp.Close()
		_, err = io.Copy(tmp, base64.NewDecoder(base64.StdEncoding, f))
		return tmp.Name(), err
	},
}
GO

# The call that writes each table out, in front of the loop over it.
tests="TestFitsInBase256 TestParseNumeric TestFormatNumeric TestFitsInOctal
TestParsePAXTime TestFormatPAXTime TestParsePAXRecord TestFormatPAXRecord
TestReader TestPartialRead TestMergePAX TestParsePAX TestReadOldGNUSparseMap
TestReadGNUSparsePAXHeaders TestFileReader TestSparseEntries
TestHeaderRoundTrip TestHeaderAllowedFormats TestWriter TestSplitUSTARPath
TestFileWriter"
TESTS=$(echo $tests) perl -i -ne '
	BEGIN { %want = map { $_ => 1 } split / /, $ENV{TESTS}; }
	if (/^func (Test\w+)\(/) { $fn = $1; }
	if ($want{$fn} && /^\tfor (_|i), v := range vectors \{/) {
		print "\tgenDump(\"$fn\", vectors)\n";
		delete $want{$fn};
	}
	print;
	END { die "no loop found in: " . join(" ", sort keys %want) . "\n" if %want; }
' strconv_test.go reader_test.go tar_test.go writer_test.go

cat > gen_test.go <<'GO'
package tar

import (
	"bytes"
	"compress/bzip2"
	"compress/gzip"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"
	"time"
	"unsafe"
)

// Strings longer than this go in as gzip, and longer than bigString not at
// all, which leaves the C test to make them itself.
const (
	zipString = 2048
	bigString = 1 << 20
)

type emitter struct {
	types  strings.Builder
	data   strings.Builder
	named  map[reflect.Type]string
	used   map[string]bool
	test   string
	serial int
}

var gen = &emitter{named: map[reflect.Type]string{}, used: map[string]bool{}}

var dumps []string

var cKeywords = map[string]bool{
	"auto": true, "break": true, "case": true, "char": true, "const": true,
	"default": true, "do": true, "double": true, "else": true, "enum": true,
	"float": true, "for": true, "if": true, "int": true, "long": true,
	"return": true, "short": true, "signed": true, "sizeof": true,
	"static": true, "struct": true, "switch": true, "union": true,
	"void": true, "while": true,
}

func cField(name string) string {
	if cKeywords[name] {
		return name + "_"
	}
	return name
}

func (e *emitter) fresh(prefix string) string {
	e.serial++
	return fmt.Sprintf("%s_%d", prefix, e.serial)
}

var (
	typeError  = reflect.TypeOf((*error)(nil)).Elem()
	typeTime   = reflect.TypeOf(time.Time{})
	typeString = reflect.TypeOf("")
	typeBytes  = reflect.TypeOf([]byte(nil))
	typeMap    = reflect.TypeOf(map[string]string(nil))
)

// The C type for t. hint names an anonymous struct after where it is.
func (e *emitter) ctype(t reflect.Type, hint string) string {
	switch {
	case t == typeError:
		return "GErr"
	case t == typeTime:
		return "GTime"
	case t == typeString || t == typeBytes:
		return "GStr"
	case t == typeMap:
		return "GMap"
	}
	if n, ok := e.named[t]; ok {
		return n
	}
	switch t.Kind() {
	case reflect.Bool:
		return "bool"
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
		reflect.Uint8, reflect.Uint16, reflect.Uint32:
		return "int64_t"
	case reflect.Interface:
		return "GAny"
	case reflect.Pointer:
		if t.Elem().Kind() != reflect.Struct {
			panic("pointer to " + t.String())
		}
		return "const " + e.ctype(t.Elem(), hint) + " *"
	case reflect.Slice:
		elem := e.ctype(t.Elem(), hint+"_e")
		name := "GSlice_" + strings.NewReplacer("const ", "", " *", "_ptr").Replace(elem)
		if !e.used[name] {
			e.used[name] = true
			fmt.Fprintf(&e.types, "typedef struct {\n    const %s *p;\n    int64_t n;\n    bool nil;\n} %s;\n\n",
				strings.TrimPrefix(elem, "const "), name)
		}
		e.named[t] = name
		return name
	case reflect.Struct:
		name := hint
		switch t.Name() {
		case "":
		case "Header", "sparseEntry":
			name = "G" + t.Name()
		default:
			name = e.test + "_" + t.Name()
		}
		for e.used[name] {
			name += "_"
		}
		e.used[name] = true
		e.named[t] = name
		var b strings.Builder
		fmt.Fprintf(&b, "typedef struct %s {\n", name)
		for i := 0; i < t.NumField(); i++ {
			f := t.Field(i)
			ft := e.ctype(f.Type, name+"_"+f.Name)
			fmt.Fprintf(&b, "    %s%s%s;\n", ft, map[bool]string{true: "", false: " "}[strings.HasSuffix(ft, "*")], cField(f.Name))
		}
		fmt.Fprintf(&b, "} %s;\n\n", name)
		e.types.WriteString(b.String())
		return name
	}
	panic("cannot emit " + t.String())
}

// open makes v one that can be read whole, fields not exported included.
func open(v reflect.Value) reflect.Value {
	if v.CanAddr() {
		return reflect.NewAt(v.Type(), unsafe.Pointer(v.UnsafeAddr())).Elem()
	}
	nv := reflect.New(v.Type()).Elem()
	nv.Set(v)
	return nv
}

func cString(b []byte) string {
	var s strings.Builder
	s.WriteByte('"')
	col := 0
	for i, c := range b {
		if col > 72 {
			s.WriteString("\"\n        \"")
			col = 0
		}
		switch {
		case c == '"' || c == '\\':
			s.WriteByte('\\')
			s.WriteByte(c)
			col += 2
		case c == '?':
			// No trigraphs.
			s.WriteString("\\077")
			col += 4
		case c >= 0x20 && c < 0x7f:
			s.WriteByte(c)
			col++
		default:
			fmt.Fprintf(&s, "\\%03o", c)
			col += 4
		}
		_ = i
	}
	s.WriteByte('"')
	return s.String()
}

// A byte array for data too long for one string literal.
func (e *emitter) blob(b []byte) string {
	name := e.fresh("gblob")
	fmt.Fprintf(&e.data, "static const unsigned char %s[%d] = {", name, len(b)+1)
	for i, c := range b {
		if i%16 == 0 {
			e.data.WriteString("\n   ")
		}
		fmt.Fprintf(&e.data, " %d,", c)
	}
	e.data.WriteString(" 0};\n\n")
	return name
}

func gz(b []byte) []byte {
	var buf bytes.Buffer
	zw, _ := gzip.NewWriterLevel(&buf, gzip.BestCompression)
	zw.Write(b)
	zw.Close()
	return buf.Bytes()
}

func (e *emitter) str(b []byte, isNil bool) string {
	switch {
	case isNil:
		return "{NULL, 0, 0, true}"
	case len(b) > bigString:
		return fmt.Sprintf("{NULL, %d, -1, false}", len(b))
	case len(b) > zipString:
		z := gz(b)
		return fmt.Sprintf("{(const char *)%s, %d, %d, false}", e.blob(z), len(b), len(z))
	}
	return fmt.Sprintf("{%s, %d, 0, false}", cString(b), len(b))
}

var knownErrors = []struct {
	err  error
	name string
}{
	{ErrHeader, "GERR_HEADER"},
	{ErrWriteTooLong, "GERR_WRITE_TOO_LONG"},
	{ErrFieldTooLong, "GERR_FIELD_TOO_LONG"},
	{ErrWriteAfterClose, "GERR_WRITE_AFTER_CLOSE"},
	{ErrInsecurePath, "GERR_INSECURE_PATH"},
	{errMissData, "GERR_MISS_DATA"},
	{errUnrefData, "GERR_UNREF_DATA"},
	{errWriteHole, "GERR_WRITE_HOLE"},
	{errSparseTooLong, "GERR_SPARSE_TOO_LONG"},
	{io.EOF, "GERR_EOF"},
	{io.ErrUnexpectedEOF, "GERR_UNEXPECTED_EOF"},
}

func errName(err error) string {
	if err == nil {
		return "GERR_NIL"
	}
	for _, k := range knownErrors {
		if err == k.err {
			return k.name
		}
	}
	if _, ok := err.(headerError); ok {
		return "GERR_HEADER_ERROR"
	}
	return "GERR_OTHER"
}

// The C initializer for v, which is of type t.
func (e *emitter) value(v reflect.Value, hint string) string {
	t := v.Type()
	switch {
	case t == typeError:
		if v.IsNil() {
			return "GERR_NIL"
		}
		return errName(open(v).Interface().(error))
	case t == typeTime:
		tm := open(v).Interface().(time.Time)
		if tm.IsZero() {
			return "{0, 0, true}"
		}
		return fmt.Sprintf("{%dLL, %d, false}", tm.Unix(), tm.Nanosecond())
	case t == typeString:
		return e.str([]byte(v.String()), false)
	case t == typeBytes:
		return e.str(v.Bytes(), v.IsNil())
	case t == typeMap:
		if v.IsNil() {
			return "{NULL, 0, true}"
		}
		m := open(v).Interface().(map[string]string)
		keys := make([]string, 0, len(m))
		for k := range m {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		if len(keys) == 0 {
			return "{NULL, 0, false}"
		}
		name := e.fresh("gmap")
		var b strings.Builder
		fmt.Fprintf(&b, "static const GStr %s[] = {\n", name)
		for _, k := range keys {
			fmt.Fprintf(&b, "    %s,\n    %s,\n", e.str([]byte(k), false), e.str([]byte(m[k]), false))
		}
		b.WriteString("};\n\n")
		e.data.WriteString(b.String())
		return fmt.Sprintf("{%s, %d, false}", name, len(keys))
	}
	switch t.Kind() {
	case reflect.Bool:
		return fmt.Sprint(v.Bool())
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		if v.Int() == -1<<63 {
			return "INT64_MIN"
		}
		return fmt.Sprintf("%dLL", v.Int())
	case reflect.Uint8, reflect.Uint16, reflect.Uint32:
		return fmt.Sprintf("%dLL", v.Uint())
	case reflect.Interface:
		if v.IsNil() {
			return "{NULL, NULL}"
		}
		c := open(open(v).Elem())
		ct := e.ctype(c.Type(), e.test+"_any")
		name := e.fresh("gany")
		val := e.value(c, hint)
		fmt.Fprintf(&e.data, "static const %s %s = %s;\n\n", ct, name, val)
		return fmt.Sprintf("{%q, &%s}", ct, name)
	case reflect.Pointer:
		if v.IsNil() {
			return "NULL"
		}
		c := v.Elem()
		ct := e.ctype(c.Type(), hint)
		name := e.fresh("gptr")
		val := e.value(c, hint)
		fmt.Fprintf(&e.data, "static const %s %s = %s;\n\n", ct, name, val)
		return "&" + name
	case reflect.Slice:
		if v.IsNil() {
			return "{NULL, 0, true}"
		}
		if v.Len() == 0 {
			return "{NULL, 0, false}"
		}
		et := e.ctype(t.Elem(), hint+"_e")
		items := make([]string, v.Len())
		for i := range items {
			items[i] = e.value(v.Index(i), hint)
		}
		name := e.fresh("gslice")
		fmt.Fprintf(&e.data, "static const %s %s[] = {\n", strings.TrimPrefix(et, "const "), name)
		for _, it := range items {
			fmt.Fprintf(&e.data, "    %s,\n", it)
		}
		e.data.WriteString("};\n\n")
		return fmt.Sprintf("{%s, %d, false}", name, v.Len())
	case reflect.Struct:
		e.ctype(t, hint)
		var parts []string
		for i := 0; i < t.NumField(); i++ {
			parts = append(parts, fmt.Sprintf(".%s = %s", cField(t.Field(i).Name),
				e.value(v.Field(i), hint+"_"+t.Field(i).Name)))
		}
		return "{" + strings.Join(parts, ", ") + "}"
	}
	panic("cannot emit " + t.String())
}

func genDump(test string, vectors any) {
	gen.test = test
	v := open(reflect.ValueOf(vectors))
	// Anything reached from the table has to be readable, so the table is
	// copied into memory reflect can hand out.
	cp := reflect.MakeSlice(v.Type(), v.Len(), v.Len())
	reflect.Copy(cp, v)
	et := gen.ctype(v.Type().Elem(), test+"_v")
	items := make([]string, cp.Len())
	for i := range items {
		items[i] = gen.value(cp.Index(i), test)
	}
	var b strings.Builder
	fmt.Fprintf(&b, "static const %s gen_%s[] = {\n", et, test)
	for _, it := range items {
		fmt.Fprintf(&b, "    %s,\n", it)
	}
	b.WriteString("};\n\n")
	gen.data.WriteString(b.String())
	dumps = append(dumps, test)
}

// Every file in testdata as the tests read it.
func dumpTestdata() {
	ents, err := os.ReadDir("testdata")
	if err != nil {
		panic(err)
	}
	var rows []string
	for _, ent := range ents {
		name := "testdata/" + ent.Name()
		b, err := os.ReadFile(name)
		if err != nil {
			panic(err)
		}
		if strings.HasSuffix(name, ".base64") {
			name = strings.TrimSuffix(name, ".base64")
			if b, err = io.ReadAll(base64.NewDecoder(base64.StdEncoding, bytes.NewReader(b))); err != nil {
				panic(err)
			}
		}
		if strings.HasSuffix(name, ".bz2") {
			name = strings.TrimSuffix(name, ".bz2")
			if b, err = io.ReadAll(bzip2.NewReader(bytes.NewReader(b))); err != nil {
				panic(err)
			}
		}
		z := gz(b)
		rows = append(rows, fmt.Sprintf("    {%q, (const char *)%s, %d, %d},", filepath.ToSlash(name), gen.blob(z), len(z), len(b)))
	}
	fmt.Fprintf(&gen.data, "static const GFile gen_testdata[] = {\n%s\n};\n\n", strings.Join(rows, "\n"))
}

func TestMain(m *testing.M) {
	code := m.Run()
	if code != 0 {
		os.Exit(code)
	}
	dumpTestdata()
	var b strings.Builder
	b.WriteString(`/* Generated by tools/gen-archive-tar-tests.sh; do not edit.
 *
 * The tables of Go's archive/tar tests, written out by the tests themselves,
 * and the files in its testdata compressed with gzip. */

#ifndef BURROW_TESTS_TAR_TEST_GEN_H
#define BURROW_TESTS_TAR_TEST_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A string or []byte. zn is the length of p when it is gzip, 0 when it is
 * the bytes themselves, and -1 when it was too long to put here. */
typedef struct {
    const char *p;
    int64_t n;
    int64_t zn;
    bool nil;
} GStr;

typedef struct {
    int64_t sec;
    int64_t nsec;
    bool zero;
} GTime;

/* A map[string]string, key and value after key and value, sorted by key. */
typedef struct {
    const GStr *kv;
    int64_t n;
    bool nil;
} GMap;

/* An interface value, its C type's name and where it is. */
typedef struct {
    const char *type;
    const void *v;
} GAny;

/* Which error, by the variable it is, or by type for a headerError. */
typedef enum {
    GERR_NIL,
`)
	for _, k := range knownErrors {
		fmt.Fprintf(&b, "    %s,\n", k.name)
	}
	b.WriteString(`    GERR_HEADER_ERROR,
    GERR_OTHER,
} GErr;

typedef struct {
    const char *name;
    const char *gz;
    int64_t gz_len;
    int64_t len;
} GFile;

`)
	b.WriteString(gen.types.String())
	b.WriteString(gen.data.String())
	b.WriteString("#endif /* BURROW_TESTS_TAR_TEST_GEN_H */\n")
	if err := os.WriteFile(os.Getenv("GEN_OUT"), []byte(b.String()), 0o644); err != nil {
		panic(err)
	}
	os.Exit(0)
}

var _ = errors.New
GO

GOFLAGS=-mod=mod GEN_OUT="$out" go test -count=1 . >"$tmp/log" 2>&1 || {
	cat "$tmp/log"
	exit 1
}
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
