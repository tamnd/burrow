#!/bin/sh
# Regenerates tests/encoding_gob_test_gen.h: what Go's encoding/gob does with a
# list of values and streams, most of them taken from Go's own gob tests. The
# generator is a test file laid over the package with go test -overlay, so it
# can use the package's test types, T0, ET1, Bug0Outer and the rest, by name.
#
# For a value, it records the bytes of a stream holding the value twice, and
# whether those bytes are the same on every run, which they are not when a
# map with more than one entry is in the value. Then it decodes the stream into
# a target twice and records each error and a dump of each result. For a raw
# stream, from Go's badDataTests or made by cutting short or corrupting one of
# the encoded streams, it records the same for the decoding.
#
# The C types, descriptors, value builders and dumps come from the json
# generators' tools/gen-json-common/common.go, with its package clause
# rewritten. The generated header says how many cases went in.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/encoding_gob_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pkg=$(go env GOROOT)/src/encoding/gob

sed -e '/^\/\/go:build/d' -e 's/^package json$/package gob/' \
    "$root/tools/gen-json-common/common.go" > "$tmp/zz_gen_common_test.go"

cat > "$tmp/zz_gen_test.go" <<'GO'
package gob

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"math"
	"os"
	"reflect"
	"regexp"
	"strings"
	"testing"
)

var (
	anyType           = reflect.TypeFor[any]()
	jsontextValueType = reflect.TypeFor[struct{ zzNever [0]func() }]()
)

func knownType(reflect.Type) (string, string, bool)   { return "", "", false }
func knownValue(*builder, reflect.Value, string) bool { return false }
func typeName(t reflect.Type) string                  { return t.Name() }

// Types of the generator's own, for what Go's tests do not cover.
type GenInner struct{ A, B int }
type GenEmbed struct {
	GenInner
	X int
}
type GenEmbedPtr struct {
	*GenInner
	X int
}
type GenLeft struct{ Dup, L int }
type GenRight struct{ Dup, R int }
type GenAmbig struct {
	GenLeft
	GenRight
}
type GenFlat struct{ A, B, X, Dup, L, R int }
type GenUnexp struct {
	a int
	B int
}
type GenNarrow struct {
	A int8
	B uint8
	C float32
}
type GenWide struct {
	A int
	B uint
	C float64
}
type GenOther struct{ Z int }
type GenRec struct {
	V    int
	Next *GenRec
}
type GenAll struct {
	B   bool
	I   int
	I8  int8
	I16 int16
	I32 int32
	I64 int64
	U   uint
	U8  uint8
	U16 uint16
	U32 uint32
	U64 uint64
	UP  uintptr
	F32 float32
	F64 float64
	S   string
	By  []byte
	Is  []int
	Ss  []string
	A   [3]int16
	M   map[string]int
	P   *int
	PP  **string
	Ptr *GenInner
	I0  any
}
type GenEmpty struct{}
type GenMaps struct {
	M1 map[int]string
	M2 map[string][]int
	M3 map[string]*GenInner
}
type GenSlices struct {
	S  [][]int
	SP []*GenInner
	SA [][2]string
	SI []any
}
type GenNamedSlice []int
type GenNamedHolder struct{ N GenNamedSlice }

type zzCase struct {
	name string
	in   any    // what to encode, twice
	raw  string // or the stream, in hex
	isRaw bool
	out  any    // a pointer to what to decode into, or nil to throw it away
}

func ip(i int) *int { return &i }

func zzHex(v any) string {
	var b bytes.Buffer
	NewEncoder(&b).Encode(v)
	return hex.EncodeToString(b.Bytes())
}

func zzCases() []zzCase {
	rec := &GenRec{1, &GenRec{2, &GenRec{3, nil}}}
	s := "deep"
	sp := &s
	all := GenAll{true, -1, -128, 32767, -2147483648, math.MinInt64, 1, 255, 65535, 4294967295,
		math.MaxUint64, 12345, 1.5, math.Pi, "all", []byte("bytes"), []int{1, -1},
		[]string{"a", ""}, [3]int16{1, -2, 3}, map[string]int{"k": 1}, ip(7), &sp,
		&GenInner{1, 2}, 42}
	cs := []zzCase{
		// TestSingletons and the singletons of TestDebugSingleton.
		{name: "int", in: 17, out: new(int)},
		{name: "float32", in: float32(17.5), out: new(float32)},
		{name: "string", in: "bike shed", out: new(string)},
		{name: "strings", in: []string{"bike", "shed", "paint", "color"}, out: new([]string)},
		{name: "map", in: map[string]int{"seven": 7, "twelve": 12}, out: new(map[string]int)},
		{name: "array bug", in: [7]int{4, 55, 0, 0, 0, 0, 0}, out: new([7]int)},
		{name: "array", in: [7]int{4, 55, 1, 44, 22, 66, 1234}, out: new([7]int)},
		{name: "int into float32", in: 172, out: new(float32)},
		{name: "bool", in: true, out: new(bool)},
		{name: "uint", in: uint(10), out: new(uint)},
		{name: "float64", in: 3.2, out: new(float64)},
		{name: "int array", in: [3]int{11, 22, 33}, out: new([3]int)},
		{name: "float32 slice", in: []float32{0.5, 0.25, 0.125}, out: new([]float32)},
		{name: "bytes", in: []byte("hello"), out: new([]byte)},
		{name: "empty bytes", in: []byte{}, out: new([]byte)},
		{name: "zero int", in: 0, out: new(int)},
		{name: "negative", in: -1234567, out: new(int)},
		{name: "min int64", in: int64(math.MinInt64), out: new(int64)},
		{name: "max uint64", in: uint64(math.MaxUint64), out: new(uint64)},
		// Go's NaN has a payload the C builder does not write, so this one
		// goes in as Go's stream. A float64 is a builtin type and takes no
		// type id to encode.
		{name: "nan", raw: zzHex(math.NaN()), isRaw: true, out: new(float64)},
		{name: "inf", in: math.Inf(-1), out: new(float64)},
		{name: "negative zero", in: math.Copysign(0, -1), out: new(float64)},
		{name: "empty string", in: "", out: new(string)},
		{name: "int16 slice", in: []int16{-1, 2, -300}, out: new([]int16)},
		{name: "uint32 slice", in: []uint32{1, 1 << 31}, out: new([]uint32)},
		{name: "bool slice", in: []bool{true, false, true}, out: new([]bool)},
		{name: "uintptr slice", in: []uintptr{1, 2}, out: new([]uintptr)},
		{name: "string array", in: [2]string{"x", "yz"}, out: new([2]string)},
		{name: "int map", in: map[int]string{1: "one", 2: "two", 3: "three"}, out: new(map[int]string)},
		{name: "empty map", in: map[string]int{}, out: new(map[string]int)},
		{name: "pointer to int", in: ip(99), out: new(*int)},
		{name: "pointer into int", in: ip(99), out: new(int)},
		{name: "int into pointer", in: 99, out: new(**int)},

		// Range and type errors for single values.
		{name: "int into int8", in: 300, out: new(int8)},
		{name: "negative into int8", in: -129, out: new(int8)},
		{name: "int into int16", in: 1 << 20, out: new(int16)},
		{name: "int into int32", in: 1 << 40, out: new(int32)},
		{name: "uint into uint8", in: uint(256), out: new(uint8)},
		{name: "uint into uint16", in: uint(1 << 16), out: new(uint16)},
		{name: "uint into uint32", in: uint(1 << 32), out: new(uint32)},
		{name: "float64 into float32", in: 1e300, out: new(float32)},
		{name: "inf into float32", in: math.Inf(1), out: new(float32)},
		{name: "int into uint", in: 5, out: new(uint)},
		{name: "uint into int", in: uint(5), out: new(int)},
		{name: "string into int", in: "x", out: new(int)},
		{name: "int into string", in: 5, out: new(string)},
		{name: "bytes into ints", in: []byte{1, 2, 3}, out: new([]int)},
		{name: "ints into bytes", in: []int{1, 2, 3}, out: new([]byte)},
		{name: "ints into strings", in: []int{1, 2, 3}, out: new([]string)},
		{name: "array length", in: [3]int{1, 2, 3}, out: new([4]int)},
		{name: "array into slice", in: [3]int{1, 2, 3}, out: new([]int)},
		{name: "map key type", in: map[string]int{"a": 1}, out: new(map[int]int)},
		{name: "int into struct", in: 5, out: new(T0)},
		{name: "struct into int", in: T0{1, 2, 3, 4}, out: new(int)},
		{name: "int into any", in: 5, out: new(any)},
		{name: "int slice into int", in: []int{1}, out: new(int)},
		{name: "named slice", in: GenNamedSlice{1, 2}, out: new(GenNamedSlice)},
		{name: "named slice field", in: GenNamedHolder{GenNamedSlice{3, 4}}, out: new(GenNamedHolder)},

		// TestAutoIndirection.
		{name: "t1 into t0", in: T1{17, ip(177), func() **int { p := ip(1777); return &p }(), nil}, out: new(T0)},
		{name: "t0 into t1", in: T0{17, 177, 1777, 17777}, out: new(T1)},
		{name: "t0 into t2", in: T0{17, 177, 1777, 17777}, out: new(T2)},
		// TestReorderedFields and TestIgnoredFields.
		{name: "reordered", in: RT0{17, "hello", 3.14159}, out: new(RT1)},
		{name: "ignored fields", in: IT0{A: 17, B: "hello", C: 3.14159, Ignore_d: []int{1, 2, 3},
			Ignore_e: [3]float64{1, 2, 3}, Ignore_f: true, Ignore_g: "pay no attention",
			Ignore_h: []byte("to the curtain"), Ignore_i: &RT1{3.1, "hi", 7, "hello"},
			Ignore_m: map[string]int{"one": 1}}, out: new(RT0)},
		// TestEncoderDecoder and its type mismatches.
		{name: "et0", in: &ET0{7, "hello"}, out: new(ET0)},
		{name: "et1", in: &ET1{7, &ET2{"wow"}, &ET1{8, nil, nil}}, out: new(ET1)},
		{name: "et1 into et3", in: &ET1{7, &ET2{"wow"}, nil}, out: new(ET3)},
		{name: "et1 into et4", in: &ET1{7, &ET2{"wow"}, nil}, out: new(ET4)},
		{name: "et2 into et1", in: ET2{"x"}, out: new(ET1)},
		{name: "rt0 into other", in: RT0{17, "hello", 3.14159}, out: new(GenOther)},
		{name: "empty into other", in: GenEmpty{}, out: new(GenOther)},
		{name: "other into empty", in: GenOther{5}, out: new(GenEmpty)},
		// TestDecodeIntoNothing.
		{name: "struct into empty", in: &struct{ A int }{23}, out: new(struct{})},
		{name: "struct into nothing", in: &struct{ A int }{23}},
		{name: "string into nothing", in: "hello, world"},
		{name: "slice into nothing", in: []int{1, 2, 3, 4}},
		{name: "interface into nothing", in: &Struct0{&NewType0{"value0"}}},
		{name: "interfaces into nothing", in: []any{"hi", &NewType0{"value1"}, 23}},
		{name: "map into nothing", in: map[string][]int{"a": {1}}},
		{name: "array into nothing", in: [2]GenInner{{1, 2}, {3, 4}}},
		{name: "recursive into nothing", in: rec},
		// Interfaces.
		{name: "interface", in: &Struct0{&NewType0{"value0"}}, out: new(Struct0)},
		{name: "nil interface", in: &Struct0{nil}, out: new(Struct0)},
		{name: "interface int", in: &Struct0{17}, out: new(Struct0)},
		{name: "interface slice", in: []any{"hi", &NewType0{"value1"}, 23}, out: new([]any)},
		{name: "nested interfaces", in: &Bug0Outer{&Bug0Outer{&Bug0Inner{7}}}, out: new(Bug0Outer)},
		{name: "interface map", in: map[string]any{"a": 1, "b": "two"}, out: new(map[string]any)},
		{name: "interface into struct", in: &Struct0{&NewType0{"x"}}, out: new(NewType0)},
		// Bugs.
		{name: "map bug1", in: Bug1StructMap{"val1": {"elem1", 1}, "val2": {"elem2", 2}}, out: new(Bug1StructMap)},
		{name: "chan func ignored", in: Bug2{A: 23}, out: new(Bug2)},
		{name: "pointer slices", in: []*Bug3{{1, nil}, {2, nil}}, out: new([]*Bug3)},
		{name: "recursive slices", in: &Bug3{1, []*Bug3{{2, []*Bug3{{3, nil}}}}}, out: new(Bug3)},
		{name: "no exported fields", in: Bug4Public{"name", Bug4Secret{1}}, out: new(Bug4Public)},
		// Embedding, and fields the decoder does not have.
		{name: "embedded", in: GenEmbed{GenInner{1, 2}, 3}, out: new(GenEmbed)},
		{name: "embedded into flat", in: GenEmbed{GenInner{1, 2}, 3}, out: new(GenFlat)},
		{name: "flat into embedded", in: GenFlat{1, 2, 3, 4, 5, 6}, out: new(GenEmbed)},
		{name: "flat into embedded pointer", in: GenFlat{1, 2, 3, 4, 5, 6}, out: new(GenEmbedPtr)},
		{name: "embedded pointer", in: GenEmbedPtr{&GenInner{1, 2}, 3}, out: new(GenEmbedPtr)},
		{name: "nil embedded pointer", in: GenEmbedPtr{nil, 3}, out: new(GenEmbedPtr)},
		{name: "flat into ambiguous", in: GenFlat{1, 2, 3, 4, 5, 6}, out: new(GenAmbig)},
		{name: "ambiguous", in: GenAmbig{GenLeft{1, 2}, GenRight{3, 4}}, out: new(GenFlat)},
		{name: "unexported", in: GenUnexp{1, 2}, out: new(GenUnexp)},
		{name: "narrow", in: GenWide{1, 2, 3}, out: new(GenNarrow)},
		{name: "narrow int overflow", in: GenWide{1000, 2, 3}, out: new(GenNarrow)},
		{name: "narrow uint overflow", in: GenWide{1, 1000, 3}, out: new(GenNarrow)},
		{name: "narrow float overflow", in: GenWide{1, 2, 1e200}, out: new(GenNarrow)},
		{name: "wide", in: GenNarrow{-1, 255, 0.5}, out: new(GenWide)},
		{name: "all", in: all, out: new(GenAll)},
		{name: "all zero", in: GenAll{}, out: new(GenAll)},
		{name: "all into other", in: all, out: new(GenOther)},
		{name: "recursive", in: rec, out: new(GenRec)},
		{name: "maps", in: GenMaps{map[int]string{1: "a"}, map[string][]int{"b": {1, 2}}, map[string]*GenInner{"c": {1, 2}}}, out: new(GenMaps)},
		{name: "slices", in: GenSlices{[][]int{{1}, {2, 3}}, []*GenInner{{1, 2}}, [][2]string{{"a", "b"}}, []any{1, "s"}}, out: new(GenSlices)},
		{name: "empty struct", in: GenEmpty{}, out: new(GenEmpty)},
	}
	for i, t := range badDataTests {
		cs = append(cs, zzCase{name: fmt.Sprintf("bad data %d", i), raw: t.input, isRaw: true, out: t.data})
	}
	return cs
}

// What gets registered before any case runs, in this order.
var zzRegister = []any{new(NewType0), new(Bug0Outer), new(Bug0Inner)}

// The streams cut short and corrupted to make more raw cases.
var zzMangle = []string{"all", "interface slice", "et1", "recursive", "maps", "slices", "ignored fields"}

var digits = regexp.MustCompile(`[0-9]+`)

type zzOut struct {
	err  [2]string
	dump [2]string
}

func zzDecode(data []byte, out any) (o zzOut) {
	d := NewDecoder(bytes.NewReader(data))
	for i := 0; i < 2; i++ {
		var p any
		if out != nil {
			p = reflect.New(reflect.TypeOf(out).Elem()).Interface()
		}
		// A panic out of Decode, which Go's reflect can give, is recorded
		// as its text after "panic: ".
		func() {
			defer func() {
				if r := recover(); r != nil {
					o.err[i] = lit(fmt.Sprint("panic: ", r))
				}
			}()
			o.err[i] = errText(d.Decode(p))
		}()
		if p != nil {
			var b strings.Builder
			dump(&b, reflect.ValueOf(p).Elem(), map[uintptr]bool{})
			o.dump[i] = lit(b.String())
		} else {
			o.dump[i] = `""`
		}
	}
	return o
}

func TestZZGen(t *testing.T) {
	for _, r := range zzRegister {
		Register(r)
	}
	var w strings.Builder
	var regs strings.Builder
	for _, r := range zzRegister {
		_, d := ctype(reflect.TypeOf(r))
		fmt.Fprintf(&regs, "    gob_register(BURROW_ANY(%s, NULL));\n", d)
	}
	streams := map[string][]byte{}
	outs := map[string]any{}
	// A mangled stream is kept only when its errors are ones not seen yet
	// for the stream it came from, which keeps every way decoding it fails
	// and drops the thousands of cases that fail the same way.
	seen := map[string]bool{}
	row := func(name, mk, typ string, data []byte, det int, encErr string, out any) {
		otyp := "NULL"
		if out != nil {
			_, otyp = ctype(reflect.TypeOf(out).Elem())
		}
		o := zzDecode(data, out)
		if i := strings.Index(name, " cut at "); i >= 0 || strings.Contains(name, " xor ") {
			if i < 0 {
				i = strings.Index(name, " byte ")
			}
			// Errors that differ only in a number count as the same, and a
			// stream whose corrupted length made a huge value is not kept.
			key := digits.ReplaceAllString(name[:i]+"\x00"+o.err[0]+"\x00"+o.err[1], "#")
			if seen[key] || len(o.dump[0])+len(o.dump[1]) > 1024 {
				return
			}
			seen[key] = true
		}
		fmt.Fprintf(&genCases, "    {%s, %s, %s, %s, %d, %s, %s, {%s, %s}, {%s, %s}},\n",
			lit(name), typ, mk, qs(string(data)), det, encErr, otyp, o.err[0], o.err[1], o.dump[0], o.dump[1])
		genKept++
	}
	for _, c := range zzCases() {
		if c.isRaw {
			data, err := hex.DecodeString(c.raw)
			if err != nil {
				t.Fatal(err)
			}
			row(c.name, "NULL", "NULL", data, 1, "NULL", c.out)
			continue
		}
		v := reflect.ValueOf(c.in)
		mk, ok := builderFor(v)
		if !ok {
			t.Fatalf("%s: cannot build %T in C", c.name, c.in)
		}
		if c.out != nil && !supported(reflect.TypeOf(c.out).Elem(), map[reflect.Type]bool{}) {
			t.Fatalf("%s: C has no %T", c.name, c.out)
		}
		_, typ := ctype(v.Type())
		enc := func() ([]byte, error) {
			var b bytes.Buffer
			e := NewEncoder(&b)
			if err := e.Encode(c.in); err != nil {
				return nil, err
			}
			err := e.Encode(c.in)
			return b.Bytes(), err
		}
		data, err := enc()
		det := 1
		for i := 0; i < 16 && err == nil; i++ {
			again, _ := enc()
			if !bytes.Equal(again, data) {
				det = 0
			}
		}
		if err != nil {
			fmt.Fprintf(&genCases, "    {%s, %s, %s, QS(\"\"), 1, %s, NULL, {NULL, NULL}, {\"\", \"\"}},\n",
				lit(c.name), typ, mk, errText(err))
			genKept++
			continue
		}
		streams[c.name] = data
		outs[c.name] = c.out
		row(c.name, mk, typ, data, det, "NULL", c.out)
	}
	// TestDecodeErrorMultipleTypes: two streams, one after the other, each
	// sending the type, so that the second sending is a duplicate. Made after
	// the rest, since encoding hands out type ids and the C side has to hand
	// out the same ones in the same order.
	{
		var b bytes.Buffer
		NewEncoder(&b).Encode(GenInner{1, 2})
		NewEncoder(&b).Encode(GenInner{3, 4})
		row("duplicate type", "NULL", "NULL", b.Bytes(), 1, "NULL", new(GenInner))
	}
	for _, name := range zzMangle {
		data, out := streams[name], outs[name]
		if data == nil {
			t.Fatalf("no stream %q", name)
		}
		for n := 0; n < len(data); n++ {
			row(fmt.Sprintf("%s cut at %d", name, n), "NULL", "NULL", data[:n], 1, "NULL", out)
		}
		for i := 0; i < len(data); i++ {
			for _, x := range []byte{0x01, 0x80, 0xff} {
				m := bytes.Clone(data)
				m[i] ^= x
				row(fmt.Sprintf("%s byte %d xor %02x", name, i, x), "NULL", "NULL", m, 1, "NULL", out)
			}
		}
	}

	w.WriteString(`/* What Go's encoding/gob does with the cases of tools/gen-gob-tests.sh,
 * go1.27.1. Generated by that script; do not edit. */

typedef struct QStr {
    const char *p;
    long long n;
} QStr;

#define QS(x) {x, (long long)sizeof(x) - 1}

typedef struct GCase {
    const char *name;
    const Type *in_type;
    void (*mk)(Alloc *a, void *out);
    QStr stream;
    int det;
    const char *enc_err;
    const Type *out_type;
    const char *dec_err[2];
    const char *dump[2];
} GCase;

`)
	fmt.Fprintf(&w, "/* %d cases. */\n\n", genKept)
	for i := range genTypeOrder {
		fmt.Fprintf(&w, "static Type gt%d;\n", i)
	}
	w.WriteString("\n")
	w.WriteString(genTypeDefs.String())
	w.WriteString("\n")
	w.WriteString(genTypeDescs.String())
	w.WriteString("\nstatic void gen_init(void) {\n")
	w.WriteString(genTypeInit.String())
	w.WriteString(regs.String())
	w.WriteString("}\n\n")
	w.WriteString(genFuncs.String())
	w.WriteString("static const GCase g_cases[] = {\n")
	w.WriteString(genCases.String())
	w.WriteString("};\n")
	os.Stdout.WriteString(w.String())
}
GO

printf '{"Replace":{"%s/zz_gen_test.go":"%s/zz_gen_test.go","%s/zz_gen_common_test.go":"%s/zz_gen_common_test.go"}}' \
    "$pkg" "$tmp" "$pkg" "$tmp" > "$tmp/overlay.json"
go test -overlay "$tmp/overlay.json" -run '^TestZZGen$' -count=1 -v encoding/gob > "$tmp/out.txt" ||
    { grep -m1 -B2 -A30 "^panic\|^fatal\|FAIL\|cannot\|undefined\|zz_gen" "$tmp/out.txt" >&2; exit 1; }
sed -n '/^\/\* What Go/,$p' "$tmp/out.txt" | sed '/^--- PASS/,$d' > "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
echo "wrote $out"
