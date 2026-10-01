#!/bin/sh
# Regenerates tests/zip_test_gen.h for tests/zip_test.c. It runs Go's own
# archive/zip tests on a copy of the package and then writes out, as C:
#
#   - the tables the tests loop over, tests in reader_test.go and writeTests in
#     writer_test.go, the errors named by which of the package's errors they
#     are and the sources of the archives that are made in code by the name of
#     the function that makes them;
#   - the archives and headers the tests spell out as byte or string literals,
#     taken from the source of the tests, and the two that come from hex dumps,
#     rZipBytes and biggestZipBytes;
#   - every file in testdata and testdata/zip64, decoded where it is obscured
#     with base64, and compressed with gzip, which the C test undoes with
#     burrow's own reader. The zip64 goldens are gzip already and go in as
#     they are.
#
# The copy of the package cannot import Go's internal packages from outside
# GOROOT, so it gets small stand-ins for the three it uses.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/zip_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
goroot=$(go env GOROOT)
src="$goroot/src/archive/zip"

mkdir -p "$tmp/zip"
cp -R "$src/testdata" "$tmp/zip/"
for f in reader.go register.go struct.go writer.go reader_test.go \
	writer_test.go zip_test.go zip64_test.go zip64_sparse_test.go; do
	cp "$src/$f" "$tmp/zip/"
done
cd "$tmp/zip"

cat > go.mod <<'EOF'
module zip

go 1.27
EOF

# The stand-ins for the internal packages.
perl -0pi -e 's/\t"internal\/godebug"\n//' reader.go
perl -0pi -e 's/\t"internal\/testenv"\n//' zip_test.go
perl -0pi -e 's/\t"internal\/obscuretestdata"\n//' reader_test.go

cat > shim.go <<'GO'
package zip

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
package zip

import (
	"encoding/base64"
	"io"
	"os"
)

var testenv = struct {
	Builder   func() string
	CPUIsSlow func() bool
}{
	func() string { return "" },
	func() bool { return false },
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

cat > gen_test.go <<'GO'
package zip

import (
	"bytes"
	"compress/gzip"
	"encoding/base64"
	"errors"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"testing"
	"time"
	"unsafe"
)

// Strings longer than this go in as gzip, and longer than bigString not at
// all, which leaves the C test to make them itself.
const (
	zipString = 2048
	bigString = 1 << 16
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
	case reflect.Uint64:
		return "int64_t"
	case reflect.Func:
		return "const char *"
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
		case "ZipTest", "ZipTestFile", "WriteTest":
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
	{ErrFormat, "GERR_FORMAT"},
	{ErrAlgorithm, "GERR_ALGORITHM"},
	{ErrChecksum, "GERR_CHECKSUM"},
	{ErrInsecurePath, "GERR_INSECURE_PATH"},
	{errLongName, "GERR_LONG_NAME"},
	{errLongExtra, "GERR_LONG_EXTRA"},
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
			return "{0, 0, 0, \"\", false, true}"
		}
		zone, off := tm.Zone()
		return fmt.Sprintf("{%dLL, %d, %d, %q, %v, false}", tm.Unix(), tm.Nanosecond(), off, zone, tm.Location() == time.UTC)
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
	case reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		return fmt.Sprintf("%dLL", v.Uint())
	case reflect.Func:
		if v.IsNil() {
			return "NULL"
		}
		name := runtime.FuncForPC(v.Pointer()).Name()
		return fmt.Sprintf("%q", name[strings.LastIndex(name, ".")+1:])
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

func genDump(name string, vectors any) {
	gen.test = name
	v := open(reflect.ValueOf(vectors))
	// Anything reached from the table has to be readable, so the table is
	// copied into memory reflect can hand out.
	cp := reflect.MakeSlice(v.Type(), v.Len(), v.Len())
	reflect.Copy(cp, v)
	et := gen.ctype(v.Type().Elem(), name+"_v")
	items := make([]string, cp.Len())
	for i := range items {
		items[i] = gen.value(cp.Index(i), name)
	}
	var b strings.Builder
	fmt.Fprintf(&b, "static const %s gen_%s[] = {\n", et, name)
	for _, it := range items {
		fmt.Fprintf(&b, "    %s,\n", it)
	}
	b.WriteString("};\n\n")
	gen.data.WriteString(b.String())
}

// The value of a constant string or integer expression made of literals and
// +, or false when it is anything else.
func constOf(e ast.Expr) (string, int64, bool) {
	switch e := e.(type) {
	case *ast.BasicLit:
		switch e.Kind {
		case token.STRING:
			s, err := strconv.Unquote(e.Value)
			return s, 0, err == nil
		case token.INT:
			n, err := strconv.ParseInt(e.Value, 0, 64)
			return "", n, err == nil
		case token.CHAR:
			s, err := strconv.Unquote(e.Value)
			if err != nil || len([]rune(s)) != 1 {
				return "", 0, false
			}
			return "", int64([]rune(s)[0]), true
		}
	case *ast.BinaryExpr:
		if e.Op != token.ADD {
			return "", 0, false
		}
		x, _, ok1 := constOf(e.X)
		y, _, ok2 := constOf(e.Y)
		return x + y, 0, ok1 && ok2
	case *ast.ParenExpr:
		return constOf(e.X)
	}
	return "", 0, false
}

func isIdent(e ast.Expr, name string) bool {
	id, ok := e.(*ast.Ident)
	return ok && id.Name == name
}

func isSliceOf(e ast.Expr, elem string) bool {
	at, ok := e.(*ast.ArrayType)
	return ok && at.Len == nil && isIdent(at.Elt, elem)
}

// The []byte or []string a literal spells out, as a C declaration with %s for
// its name: []byte{...} with constant elements, []byte("..."), and
// []string{...} of constant strings.
func literalOf(e ast.Expr) (string, bool) {
	switch e := e.(type) {
	case *ast.CompositeLit:
		if isSliceOf(e.Type, "byte") {
			b := make([]byte, 0, len(e.Elts))
			for _, el := range e.Elts {
				_, n, ok := constOf(el)
				if !ok {
					return "", false
				}
				b = append(b, byte(n))
			}
			return "static const GStr %s = " + gen.str(b, false) + ";\n\n", true
		}
		if isSliceOf(e.Type, "string") {
			var items []string
			for _, el := range e.Elts {
				s, _, ok := constOf(el)
				if !ok {
					return "", false
				}
				items = append(items, "    "+gen.str([]byte(s), false)+",\n")
			}
			return "static const GStr %s[] = {\n" + strings.Join(items, "") + "};\n\n", true
		}
	case *ast.CallExpr:
		if isSliceOf(e.Fun, "byte") && len(e.Args) == 1 {
			if s, _, ok := constOf(e.Args[0]); ok {
				return "static const GStr %s = " + gen.str([]byte(s), false) + ";\n\n", true
			}
		}
	}
	return "", false
}

// Every x := literal in a test function, as gen_<Test>_<x>.
func dumpLiterals() {
	fset := token.NewFileSet()
	files, _ := filepath.Glob("*_test.go")
	sort.Strings(files)
	for _, file := range files {
		if file == "gen_test.go" || file == "shim_test.go" {
			continue
		}
		f, err := parser.ParseFile(fset, file, nil, 0)
		if err != nil {
			panic(err)
		}
		for _, d := range f.Decls {
			fn, ok := d.(*ast.FuncDecl)
			if !ok || !strings.HasPrefix(fn.Name.Name, "Test") || fn.Body == nil {
				continue
			}
			ast.Inspect(fn.Body, func(n ast.Node) bool {
				as, ok := n.(*ast.AssignStmt)
				if !ok || as.Tok != token.DEFINE || len(as.Lhs) != 1 || len(as.Rhs) != 1 {
					return true
				}
				id, ok := as.Lhs[0].(*ast.Ident)
				if !ok {
					return true
				}
				if c, ok := literalOf(as.Rhs[0]); ok {
					gen.data.WriteString(strings.Replace(c, "%s", "gen_"+fn.Name.Name+"_"+id.Name, 1))
				}
				return true
			})
		}
	}
}

// Every file in testdata and testdata/zip64 as the tests read it.
func dumpTestdata() {
	var rows []string
	for _, dir := range []string{"testdata", "testdata/zip64"} {
		ents, err := os.ReadDir(dir)
		if err != nil {
			panic(err)
		}
		for _, ent := range ents {
			if ent.IsDir() {
				continue
			}
			name := dir + "/" + ent.Name()
			b, err := os.ReadFile(name)
			if err != nil {
				panic(err)
			}
			if strings.HasSuffix(name, ".base64") {
				if b, err = io.ReadAll(base64.NewDecoder(base64.StdEncoding, bytes.NewReader(b))); err != nil {
					panic(err)
				}
			}
			z := gz(b)
			if strings.HasSuffix(name, ".zsparse") {
				z = b
				zr, err := gzip.NewReader(bytes.NewReader(b))
				if err != nil {
					panic(err)
				}
				if b, err = io.ReadAll(zr); err != nil {
					panic(err)
				}
			}
			rows = append(rows, fmt.Sprintf("    {%q, (const char *)%s, %d, %d},", name, gen.blob(z), len(z), len(b)))
		}
	}
	fmt.Fprintf(&gen.data, "static const GFile gen_testdata[] = {\n%s\n};\n\n", strings.Join(rows, "\n"))
}

func TestMain(m *testing.M) {
	code := m.Run()
	if code != 0 {
		os.Exit(code)
	}
	genDump("tests", tests)
	genDump("writeTests", writeTests)
	fmt.Fprintf(&gen.data, "static const GStr gen_rZipBytes = %s;\n\n", gen.str(rZipBytes(), false))
	fmt.Fprintf(&gen.data, "static const GStr gen_biggestZipBytes = %s;\n\n", gen.str(biggestZipBytes(), false))
	dumpLiterals()
	dumpTestdata()
	var b strings.Builder
	b.WriteString(`/* Generated by tools/gen-archive-zip-tests.sh; do not edit.
 *
 * The tables of Go's archive/zip tests, the archives its tests spell out in
 * their source, and the files in its testdata compressed with gzip. */

#ifndef BURROW_TESTS_ZIP_TEST_GEN_H
#define BURROW_TESTS_ZIP_TEST_GEN_H

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

/* A time.Time with the name and offset of its zone, utc when the location is
 * time.UTC rather than a fixed zone that happens to be at 0. */
typedef struct {
    int64_t sec;
    int64_t nsec;
    int64_t off;
    const char *zone;
    bool utc;
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

/* Which error, by the variable it is. */
typedef enum {
    GERR_NIL,
`)
	for _, k := range knownErrors {
		fmt.Fprintf(&b, "    %s,\n", k.name)
	}
	b.WriteString(`    GERR_OTHER,
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
	b.WriteString("#endif /* BURROW_TESTS_ZIP_TEST_GEN_H */\n")
	if err := os.WriteFile(os.Getenv("GEN_OUT"), []byte(b.String()), 0o644); err != nil {
		panic(err)
	}
	os.Exit(0)
}

var _ = errors.New
GO

GOFLAGS=-mod=mod GEN_OUT="$out" go test -short -count=1 . >"$tmp/log" 2>&1 || {
	cat "$tmp/log"
	exit 1
}
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
