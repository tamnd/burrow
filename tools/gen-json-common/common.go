//go:build goexperiment.jsonv2

// The half of the json test generators that both tools/gen-jsonv2-tests.sh
// and tools/gen-json-tests.sh share. Each script lays this file over the
// package it generates for, next to its own file, which has to define
// knownType, knownValue and typeName. It walks Go types and values with
// reflect and writes the C types, Type descriptors and builder functions for
// them, and the dumps both sides compare values by.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package json

import (
	"fmt"
	"math"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"time"
)

var timeType = reflect.TypeFor[time.Time]()

var (
	genTypeIDs   = map[reflect.Type]int{}
	genTypeOrder []reflect.Type
	genTypeDefs  strings.Builder
	genTypeDescs strings.Builder
	genTypeInit  strings.Builder
	genFuncs     strings.Builder
	genCases     strings.Builder
	genKept      int
	genSkipped   int
)

func lit(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '?':
			b.WriteString(`\077`)
		case c >= ' ' && c <= '~':
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func qs(s string) string   { return "QS(" + lit(s) + ")" }
func strLit(s string) string { return fmt.Sprintf("{(const Byte *)%s, %d}", lit(s), len(s)) }

var builtins = map[reflect.Kind][2]string{
	reflect.Bool:    {"bool", "bool"},
	reflect.Int:     {"Int", "Int"},
	reflect.Int8:    {"int8_t", "int8_t"},
	reflect.Int16:   {"int16_t", "int16_t"},
	reflect.Int32:   {"int32_t", "int32_t"},
	reflect.Int64:   {"int64_t", "int64_t"},
	reflect.Uint:    {"Uint", "Uint"},
	reflect.Uint8:   {"uint8_t", "uint8_t"},
	reflect.Uint16:  {"uint16_t", "uint16_t"},
	reflect.Uint32:  {"uint32_t", "uint32_t"},
	reflect.Uint64:  {"uint64_t", "uint64_t"},
	reflect.Uintptr: {"Uintptr", "Uintptr"},
	reflect.Float32: {"float", "float"},
	reflect.Float64: {"double", "double"},
	reflect.String:  {"Str", "Str"},
}

var kindNames = map[reflect.Kind]string{
	reflect.Bool: "KIND_BOOL", reflect.Int: "KIND_INT", reflect.Int8: "KIND_INT8",
	reflect.Int16: "KIND_INT16", reflect.Int32: "KIND_INT32", reflect.Int64: "KIND_INT64",
	reflect.Uint: "KIND_UINT", reflect.Uint8: "KIND_UINT8", reflect.Uint16: "KIND_UINT16",
	reflect.Uint32: "KIND_UINT32", reflect.Uint64: "KIND_UINT64", reflect.Uintptr: "KIND_UINTPTR",
	reflect.Float32: "KIND_FLOAT32", reflect.Float64: "KIND_FLOAT64", reflect.String: "KIND_STRING",
	reflect.Slice: "KIND_SLICE", reflect.Array: "KIND_ARRAY", reflect.Pointer: "KIND_POINTER",
	reflect.Map: "KIND_MAP", reflect.Struct: "KIND_STRUCT", reflect.Interface: "KIND_INTERFACE",
	reflect.Chan: "KIND_CHAN", reflect.Func: "KIND_FUNC",
}


// Whether the C side can have t.
func supported(t reflect.Type, seen map[reflect.Type]bool) bool {
	if seen[t] {
		return true
	}
	seen[t] = true
	if t == jsontextValueType {
		return true
	}
	if _, _, ok := knownType(t); ok {
		return true
	}
	if t.Kind() != reflect.Interface && (t.NumMethod() > 0 || reflect.PointerTo(t).NumMethod() > 0) {
		return false
	}
	switch t.Kind() {
	case reflect.Complex64, reflect.Complex128, reflect.UnsafePointer:
		return false
	case reflect.Interface:
		return t == anyType
	case reflect.Func:
		return t.String() == "func()"
	case reflect.Chan:
		return t.ChanDir() == reflect.BothDir && supported(t.Elem(), seen)
	case reflect.Slice, reflect.Array, reflect.Pointer:
		return supported(t.Elem(), seen)
	case reflect.Map:
		return supported(t.Key(), seen) && supported(t.Elem(), seen)
	case reflect.Struct:
		for i := 0; i < t.NumField(); i++ {
			f := t.Field(i)
			looks := typeName(f.Type) == f.Name ||
				(f.Type.Kind() == reflect.Pointer && f.Type.Name() == "" && typeName(f.Type.Elem()) == f.Name)
			if looks != f.Anonymous {
				return false
			}
			if !supported(f.Type, seen) {
				return false
			}
		}
	}
	return true
}

// The C spelling of t and the address of its descriptor.
func ctype(t reflect.Type) (string, string) {
	if t == anyType {
		return "Any", "&burrow_type_Any"
	}
	if t == jsontextValueType {
		return "JsontextValue", "&burrow_type_JsontextValue"
	}
	if c, d, ok := knownType(t); ok {
		return c, d
	}
	if b, ok := builtins[t.Kind()]; ok && t.PkgPath() == "" && t.Name() != "" {
		return b[0], "&burrow_type_" + b[1]
	}
	id, ok := genTypeIDs[t]
	if !ok {
		id = len(genTypeOrder)
		genTypeIDs[t] = id
		genTypeOrder = append(genTypeOrder, t)
		defineType(t, id)
	}
	return fmt.Sprintf("G%d", id), fmt.Sprintf("&gt%d", id)
}

func defineType(t reflect.Type, id int) {
	name, pkg := "", ""
	if t.Name() != "" {
		name = typeName(t)
		pkg = t.PkgPath()
		if pkg == "encoding/json/v2" {
			pkg = "encoding/json"
		}
	}
	var def strings.Builder
	elem, key, length := "NULL", "NULL", 0
	fields, nfield := "NULL", 0
	switch t.Kind() {
	// These need nothing complete, so their typedefs go out before the
	// types they point at, which may be the struct being defined.
	case reflect.Slice:
		fmt.Fprintf(&genTypeDefs, "typedef Slice G%d;\n", id)
		_, elem = ctype(t.Elem())
	case reflect.Pointer, reflect.Chan:
		fmt.Fprintf(&genTypeDefs, "typedef void *G%d;\n", id)
		_, elem = ctype(t.Elem())
	case reflect.Func:
		fmt.Fprintf(&genTypeDefs, "typedef void *G%d;\n", id)
	case reflect.Map:
		fmt.Fprintf(&genTypeDefs, "typedef Map *G%d;\n", id)
		_, key = ctype(t.Key())
		_, elem = ctype(t.Elem())
	case reflect.Array:
		var ec string
		ec, elem = ctype(t.Elem())
		length = t.Len()
		n := length
		if n == 0 {
			n = 1
		}
		fmt.Fprintf(&def, "typedef struct G%d {\n    %s v[%d];\n} G%d;\n", id, ec, n, id)
	case reflect.Struct:
		var fl strings.Builder
		fmt.Fprintf(&def, "typedef struct G%d {\n", id)
		if t.NumField() == 0 {
			def.WriteString("    char pad_;\n")
		}
		for i := 0; i < t.NumField(); i++ {
			f := t.Field(i)
			fc, fd := ctype(f.Type)
			fmt.Fprintf(&def, "    %s f%d;\n", fc, i)
			fmt.Fprintf(&fl, "    {%s, %s, %s, (uint32_t)offsetof(G%d, f%d)},\n", strLit(f.Name), strLit(string(f.Tag)), fd, id, i)
		}
		def.WriteString(fmt.Sprintf("} G%d;\n", id))
		nfield = t.NumField()
		if nfield > 0 {
			fmt.Fprintf(&genTypeDescs, "static const Field gf%d[] = {\n%s};\n", id, fl.String())
			fields = fmt.Sprintf("gf%d", id)
		}
	default:
		b := builtins[t.Kind()]
		fmt.Fprintf(&def, "typedef %s G%d;\n", b[0], id)
		fmt.Fprintf(&genTypeInit, "    gt%d.ops = burrow_type_%s.ops;\n", id, b[1])
	}
	genTypeDefs.WriteString(def.String())
	fmt.Fprintf(&genTypeDescs, "static Type gt%d = {%s, %s, %s, (uint32_t)sizeof(G%d), (uint16_t)_Alignof(G%d), %d, 0, %s, NULL, %s, %s, %d, 0, NULL};\n",
		id, strLit(name), strLit(pkg), kindNames[t.Kind()], id, id, nfield, fields, elem, key, length)
}

// Builds C statements that store v at the lvalue lv.
type builder struct {
	b    strings.Builder
	ptrs map[uintptr]int
	nvar  int
	depth int
	ok    bool
}

func (g *builder) tmp() string { g.nvar++; return fmt.Sprintf("v%d", g.nvar) }

func (g *builder) value(v reflect.Value, lv string) {
	// Cycles through maps, slices and interfaces have no pointer to spot
	// them by, and the cases that make them are the cycle tests.
	if !g.ok || g.depth > 100 {
		g.ok = false
		return
	}
	g.depth++
	defer func() { g.depth-- }()
	t := v.Type()
	if v.IsZero() && !(t.Kind() == reflect.Float32 || t.Kind() == reflect.Float64) {
		return
	}
	if knownValue(g, v, lv) {
		return
	}
	switch t.Kind() {
	case reflect.Bool:
		fmt.Fprintf(&g.b, "%s = true;\n", lv)
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		tc, _ := ctype(t)
		fmt.Fprintf(&g.b, "%s = (%s)(int64_t)UINT64_C(%d);\n", lv, tc, uint64(v.Int()))
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		tc, _ := ctype(t)
		fmt.Fprintf(&g.b, "%s = (%s)UINT64_C(%d);\n", lv, tc, v.Uint())
	case reflect.Float32, reflect.Float64:
		f := v.Float()
		tc, _ := ctype(t)
		switch {
		case f == 0 && !math.Signbit(f):
			return
		case math.IsNaN(f):
			fmt.Fprintf(&g.b, "%s = (%s)NAN;\n", lv, tc)
		case math.IsInf(f, 1):
			fmt.Fprintf(&g.b, "%s = (%s)INFINITY;\n", lv, tc)
		case math.IsInf(f, -1):
			fmt.Fprintf(&g.b, "%s = (%s)-INFINITY;\n", lv, tc)
		case f == 0:
			fmt.Fprintf(&g.b, "%s = (%s)-0.0;\n", lv, tc)
		default:
			fmt.Fprintf(&g.b, "%s = (%s)%s;\n", lv, tc, strconv.FormatFloat(f, 'x', -1, 64))
		}
	case reflect.String:
		s := v.String()
		fmt.Fprintf(&g.b, "%s = (Str){(const Byte *)%s, %d};\n", lv, lit(s), len(s))
	case reflect.Slice:
		ec, ed := ctype(t.Elem())
		s := g.tmp()
		fmt.Fprintf(&g.b, "{\nSlice %s = slice_make(a, %s, %d, %d);\n", s, ed, v.Len(), v.Len())
		for i := 0; i < v.Len(); i++ {
			g.value(v.Index(i), fmt.Sprintf("((%s *)%s.p)[%d]", ec, s, i))
		}
		fmt.Fprintf(&g.b, "%s = %s;\n}\n", lv, s)
	case reflect.Array:
		for i := 0; i < v.Len(); i++ {
			g.value(v.Index(i), fmt.Sprintf("%s.v[%d]", lv, i))
		}
	case reflect.Pointer:
		if n, ok := g.ptrs[v.Pointer()]; ok {
			fmt.Fprintf(&g.b, "%s = P[%d];\n", lv, n)
			return
		}
		ec, ed := ctype(t.Elem())
		n := len(g.ptrs)
		g.ptrs[v.Pointer()] = n
		fmt.Fprintf(&g.b, "P[%d] = gen_alloc(a, %s);\n", n, ed)
		g.value(v.Elem(), fmt.Sprintf("(*(%s *)P[%d])", ec, n))
		fmt.Fprintf(&g.b, "%s = P[%d];\n", lv, n)
	case reflect.Map:
		kc, kd := ctype(t.Key())
		vc, vd := ctype(t.Elem())
		m := g.tmp()
		fmt.Fprintf(&g.b, "{\nMap *%s = map_make(a, %s, %s, 0);\n", m, kd, vd)
		// In key order, so that the output does not change from run to run.
		// Pairs from MapRange rather than MapIndex on each key, which finds
		// nothing for a NaN key.
		type pair struct {
			k, v reflect.Value
			d    string
		}
		var pairs []pair
		for it := v.MapRange(); it.Next(); {
			var d strings.Builder
			dump(&d, it.Key(), map[uintptr]bool{})
			pairs = append(pairs, pair{it.Key(), it.Value(), d.String()})
		}
		sort.SliceStable(pairs, func(i, j int) bool { return pairs[i].d < pairs[j].d })
		for _, p := range pairs {
			k, e := g.tmp(), g.tmp()
			fmt.Fprintf(&g.b, "{\n%s %s;\n%s %s;\nmemset(&%s, 0, sizeof(%s));\nmemset(&%s, 0, sizeof(%s));\n", kc, k, vc, e, k, k, e, e)
			g.value(p.k, k)
			g.value(p.v, e)
			fmt.Fprintf(&g.b, "map_set(%s, &%s, &%s);\n}\n", m, k, e)
		}
		fmt.Fprintf(&g.b, "%s = %s;\n}\n", lv, m)
	case reflect.Struct:
		for i := 0; i < v.NumField(); i++ {
			g.value(v.Field(i), fmt.Sprintf("%s.f%d", lv, i))
		}
	case reflect.Interface:
		e := v.Elem()
		if !supported(e.Type(), map[reflect.Type]bool{}) {
			g.ok = false
			return
		}
		ec, ed := ctype(e.Type())
		b := g.tmp()
		fmt.Fprintf(&g.b, "{\n%s *%s = (%s *)gen_alloc(a, %s);\n", ec, b, ec, ed)
		g.value(e, "(*"+b+")")
		fmt.Fprintf(&g.b, "%s.t = %s;\n%s.data = %s;\n}\n", lv, ed, lv, b)
	default:
		g.ok = false
	}
}

// The dump of a value, which the C test makes the same way.
func dump(b *strings.Builder, v reflect.Value, path map[uintptr]bool) {
	t := v.Type()
	// A time.Time is the instant and its offset. The zone's name is left out,
	// since which zone a parse lands in can depend on the machine's Local.
	if t == timeType && v.CanInterface() {
		tm := v.Interface().(time.Time)
		_, off := tm.Zone()
		fmt.Fprintf(b, "T%d.%d%+d", tm.Unix(), tm.Nanosecond(), off)
		return
	}
	switch t.Kind() {
	case reflect.Bool:
		fmt.Fprintf(b, "%t", v.Bool())
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		fmt.Fprintf(b, "%d", v.Int())
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		fmt.Fprintf(b, "%d", v.Uint())
	case reflect.Float32:
		fmt.Fprintf(b, "f%08x", math.Float32bits(float32(v.Float())))
	case reflect.Float64:
		fmt.Fprintf(b, "d%016x", math.Float64bits(v.Float()))
	case reflect.String:
		fmt.Fprintf(b, "s%x", v.String())
	case reflect.Slice:
		if v.IsNil() {
			b.WriteString("nil")
			return
		}
		fallthrough
	case reflect.Array:
		b.WriteByte('[')
		for i := 0; i < v.Len(); i++ {
			if i > 0 {
				b.WriteByte(',')
			}
			dump(b, v.Index(i), path)
		}
		b.WriteByte(']')
	case reflect.Pointer:
		if v.IsNil() {
			b.WriteString("nil")
			return
		}
		if path[v.Pointer()] {
			b.WriteString("cycle")
			return
		}
		path[v.Pointer()] = true
		b.WriteByte('&')
		dump(b, v.Elem(), path)
		delete(path, v.Pointer())
	case reflect.Map:
		if v.IsNil() {
			b.WriteString("nil")
			return
		}
		var ents []string
		it := v.MapRange()
		for it.Next() {
			var e strings.Builder
			dump(&e, it.Key(), path)
			e.WriteByte(':')
			dump(&e, it.Value(), path)
			ents = append(ents, e.String())
		}
		sort.Strings(ents)
		b.WriteString("map[" + strings.Join(ents, ",") + "]")
	case reflect.Struct:
		b.WriteByte('{')
		for i := 0; i < v.NumField(); i++ {
			if i > 0 {
				b.WriteByte(',')
			}
			b.WriteString(t.Field(i).Name + ":")
			dump(b, v.Field(i), path)
		}
		b.WriteByte('}')
	case reflect.Interface:
		if v.IsNil() {
			b.WriteString("nil")
			return
		}
		b.WriteString("(" + v.Elem().Type().String() + ")")
		dump(b, v.Elem(), path)
	case reflect.Chan, reflect.Func:
		if v.IsNil() {
			b.WriteString("nil")
		} else {
			b.WriteString("?")
		}
	}
}

func errText(err error) string {
	if err == nil {
		return "NULL"
	}
	return lit(err.Error())
}

// A C function that builds v in *out, or false when it cannot.
func builderFor(v reflect.Value) (string, bool) {
	if !supported(v.Type(), map[reflect.Type]bool{}) {
		return "", false
	}
	tc, _ := ctype(v.Type())
	g := &builder{ptrs: map[uintptr]int{}, ok: true}
	g.value(v, "(*o)")
	if !g.ok {
		return "", false
	}
	id := genKept
	np := len(g.ptrs)
	if np == 0 {
		np = 1
	}
	fmt.Fprintf(&genFuncs, "static void mk%d(Alloc *a, void *out) {\n%s *o = (%s *)out;\nvoid *P[%d];\n(void)a;\n(void)o;\n(void)P;\n%s}\n\n",
		id, tc, tc, np, g.b.String())
	return fmt.Sprintf("mk%d", id), true
}
