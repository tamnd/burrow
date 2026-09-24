// gen-tests is the Go half of burrow-gen tests. It reads one of Go's _test.go
// files, type checks it together with the package it tests, and writes the
// mechanical part of the C port to stdout: the struct types behind the test
// tables, the tables themselves, and every Test function, with the range loop
// over a table written out and its body left as Go in a comment.
//
// Names that belong to Go packages come out as markers, ⟪kind path Name⟫, and
// tools/burrow-gen turns them into C names with the rules tools/burrow-coverage
// uses, so there is one naming rule in the tree rather than two.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.
package main

import (
	"bytes"
	"fmt"
	"go/ast"
	"go/build"
	"go/constant"
	"go/importer"
	"go/parser"
	"go/printer"
	"go/token"
	"go/types"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"sort"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"
)

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: gen-tests FILE_test.go")
		os.Exit(2)
	}
	g, err := load(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, "gen-tests:", err)
		os.Exit(1)
	}
	os.Stdout.Write(g.generate())
	fmt.Fprintf(os.Stderr, "gen-tests: %d tables, %d loops, %d tests to translate by hand, %d values to fill in by hand\n",
		len(g.tables), g.loops, g.skipped, g.byHand)
}

// gen is one file being translated.
type gen struct {
	fset    *token.FileSet
	file    *ast.File
	src     []byte
	path    string // the file as the header names it, src/strconv/quote_test.go
	base    string // quote_test.go
	pkgPath string // the import path of the package under test
	pkg     *types.Package
	info    *types.Info

	structs map[*types.TypeName]string // struct types declared in this file, by C name
	order   []*types.TypeName          // the same, in source order
	anon    []anonStruct
	inits   map[types.Object]ast.Expr // what the file's own package vars start as
	tables  []*table
	byName  map[types.Object]*table

	out     bytes.Buffer
	loops   int
	skipped int
	byHand  int
}

// table is a slice or array literal a test ranges over.
type table struct {
	obj   types.Object // the Go variable
	name  string       // its C name
	elem  types.Type   // the element type
	ctype string       // the C element type
	dims  string       // [2] when each row is itself an array
	lit   *ast.CompositeLit
	used  bool // referenced by generated code, so it needs no unused marker
	local *ast.FuncDecl
}

func load(name string) (*gen, error) {
	abs, err := filepath.Abs(name)
	if err != nil {
		return nil, err
	}
	dir := filepath.Dir(abs)
	bp, err := build.Default.ImportDir(dir, build.ImportComment)
	if err != nil {
		return nil, err
	}
	g := &gen{
		fset:    token.NewFileSet(),
		base:    filepath.Base(abs),
		structs: map[*types.TypeName]string{},
		byName:  map[types.Object]*table{},
		inits:   map[types.Object]ast.Expr{},
	}
	g.path = g.base
	if root := build.Default.GOROOT; root != "" {
		if rel, err := filepath.Rel(filepath.Join(root, "src"), abs); err == nil && !strings.HasPrefix(rel, "..") {
			g.path = "src/" + filepath.ToSlash(rel)
			g.pkgPath = filepath.ToSlash(filepath.Dir(rel))
		}
	}
	if g.pkgPath == "" {
		g.pkgPath = bp.Name
	}

	// The file goes in with the rest of its package when it is an internal
	// test, and with the other external ones, importing the package, when it
	// is an external one. A file the default build leaves out, for a tag or
	// an experiment, goes in all the same, since it is the one asked for.
	head, err := parser.ParseFile(g.fset, abs, nil, parser.PackageClauseOnly)
	if err != nil {
		return nil, err
	}
	external := strings.HasSuffix(head.Name.Name, "_test")
	var files []string
	if external {
		files = append(files, bp.XTestGoFiles...)
	} else {
		files = append(files, bp.GoFiles...)
		files = append(files, bp.TestGoFiles...)
	}
	if !slices.Contains(files, g.base) {
		files = append(files, g.base)
	}
	var parsed []*ast.File
	for _, f := range files {
		p := filepath.Join(dir, f)
		af, err := parser.ParseFile(g.fset, p, nil, parser.ParseComments)
		if err != nil {
			return nil, err
		}
		if f == g.base {
			g.file = af
			g.src, _ = os.ReadFile(p)
		}
		parsed = append(parsed, af)
	}

	g.info = &types.Info{
		Types: map[ast.Expr]types.TypeAndValue{},
		Defs:  map[*ast.Ident]types.Object{},
		Uses:  map[*ast.Ident]types.Object{},
	}
	conf := types.Config{
		Importer: importer.ForCompiler(g.fset, "source", nil),
		// Anything that does not check just stays unknown, and whatever it
		// touches is left for a person.
		Error: func(error) {},
	}
	pkgPath := g.pkgPath
	if external {
		pkgPath += "_test"
	}
	g.pkg, _ = conf.Check(pkgPath, g.fset, parsed, g.info)
	return g, nil
}

// ----------------------------------------------------------------- output

func (g *gen) printf(format string, args ...any) { fmt.Fprintf(&g.out, format, args...) }

func (g *gen) generate() []byte {
	g.findTables()

	var body bytes.Buffer
	saved := g.out
	g.out = bytes.Buffer{}
	tests, others := g.emitFuncs()
	body = g.out
	g.out = saved

	g.header()
	g.emitStructs()
	g.emitTables()
	g.out.Write(body.Bytes())

	if len(others) > 0 {
		g.printf("/* Not generated, and still to port by hand:\n")
		for _, o := range others {
			g.printf(" *     %s\n", o)
		}
		g.printf(" */\n\n")
	}
	if len(tests) == 0 {
		g.printf("#define TESTS(X)\n\n")
	} else {
		g.printf("#define TESTS(X) \\\n")
		for i, t := range tests {
			sep := " \\"
			if i == len(tests)-1 {
				sep = ""
			}
			g.printf("    X(%s)%s\n", t, sep)
		}
		g.printf("\n")
	}
	g.printf("TESTING_MAIN(TESTS)\n")
	out := g.out.Bytes()
	if regexp.MustCompile(`\b(INFINITY|NAN)\b`).Match(out) {
		out = bytes.Replace(out, []byte("#include \"check.h\""), []byte("#include <math.h>\n\n#include \"check.h\""), 1)
	}
	return out
}

func (g *gen) header() {
	// Go's files carry a year, and a file that is not Go's, like the fixture
	// in testdata, gets no Go copyright line at all.
	year := ""
	if m := regexp.MustCompile(`Copyright (\d{4}) The Go Authors`).FindSubmatch(g.src); m != nil {
		year = string(m[1])
	}
	g.printf("/* Derived from Go's %s.\n", g.path)
	g.printf(" * Go source: %s.\n", runtime.Version())
	g.printf(" *\n")
	g.printf(" * Generated by burrow-gen tests and then translated by hand: the tables are\n")
	g.printf(" * mechanical, and every test that still skips has Go left in it to port.\n")
	g.printf(" *\n")
	if year != "" {
		g.printf(" * Copyright %s The Go Authors. All rights reserved.\n", year)
	}
	g.printf(" * Copyright 2026 The burrow Authors. All rights reserved.\n")
	g.printf(" * Use of this source code is governed by a BSD-style licence that can be found\n")
	g.printf(" * in the LICENSE file. */\n\n")
	g.printf("#include \"check.h\"\n\n")
	// One header per package, named for its import path, as in
	// docs/design/08-naming-abi.md.
	g.printf("#include \"burrow/%s.h\"\n\n", g.pkgPath)
	g.printf("#define S BURROW_S\n")
	g.printf("#define SI BURROW_S_INIT\n\n")
}

// ----------------------------------------------------------------- tables

func (g *gen) findTables() {
	for _, d := range g.file.Decls {
		switch d := d.(type) {
		case *ast.GenDecl:
			if d.Tok != token.VAR {
				continue
			}
			for _, s := range d.Specs {
				vs := s.(*ast.ValueSpec)
				for i, n := range vs.Names {
					if i < len(vs.Values) && len(vs.Names) == len(vs.Values) {
						if obj := g.info.Defs[n]; obj != nil {
							g.inits[obj] = vs.Values[i]
						}
						g.addTable(n, vs.Values[i], nil)
					}
				}
			}
		case *ast.FuncDecl:
			if d.Body == nil || !isTest(d) {
				continue
			}
			for _, st := range d.Body.List {
				switch st := st.(type) {
				case *ast.AssignStmt:
					if st.Tok == token.DEFINE && len(st.Lhs) == 1 && len(st.Rhs) == 1 {
						if n, ok := st.Lhs[0].(*ast.Ident); ok {
							g.addTable(n, st.Rhs[0], d)
						}
					}
				case *ast.DeclStmt:
					if gd, ok := st.Decl.(*ast.GenDecl); ok && gd.Tok == token.VAR {
						for _, s := range gd.Specs {
							vs := s.(*ast.ValueSpec)
							for i, n := range vs.Names {
								if i < len(vs.Values) {
									g.addTable(n, vs.Values[i], d)
								}
							}
						}
					}
				}
			}
		}
	}
}

func (g *gen) addTable(n *ast.Ident, v ast.Expr, fn *ast.FuncDecl) {
	lit, ok := v.(*ast.CompositeLit)
	if !ok {
		return
	}
	obj := g.info.Defs[n]
	if obj == nil {
		return
	}
	var elem types.Type
	switch t := obj.Type().Underlying().(type) {
	case *types.Slice:
		elem = t.Elem()
	case *types.Array:
		elem = t.Elem()
	default:
		return
	}
	// A []byte is a value, not a table.
	if isByte(elem) {
		return
	}
	t := &table{obj: obj, elem: elem, lit: lit, local: fn}
	t.name = n.Name
	if fn != nil {
		t.name = snake(strings.TrimPrefix(fn.Name.Name, "Test")) + "_" + n.Name
	}
	if st, ok := elem.Underlying().(*types.Struct); ok {
		if named, ok := types.Unalias(elem).(*types.Named); ok && g.declaredHere(named.Obj()) {
			t.ctype = g.structName(named.Obj())
		} else if _, ok := types.Unalias(elem).(*types.Named); !ok {
			// An anonymous struct gets a name from the table.
			t.ctype = camel(t.name) + "Case"
			g.anon = append(g.anon, anonStruct{t.ctype, st})
		}
	}
	if a, ok := elem.Underlying().(*types.Array); ok && t.ctype == "" {
		t.ctype = g.cType(a.Elem())
		t.dims = fmt.Sprintf("[%d]", a.Len())
	}
	if t.ctype == "" {
		t.ctype = g.cType(elem)
	}
	g.tables = append(g.tables, t)
	g.byName[obj] = t
}

type anonStruct struct {
	name string
	st   *types.Struct
}

func (g *gen) declaredHere(o types.Object) bool {
	return o.Pos().IsValid() && g.fset.File(o.Pos()) == g.fset.File(g.file.Pos())
}

func (g *gen) structName(o *types.TypeName) string {
	if n, ok := g.structs[o]; ok {
		return n
	}
	n := camel(o.Name())
	g.structs[o] = n
	g.order = append(g.order, o)
	return n
}

// emitStructs writes the struct types in source order, except that a struct
// another one holds goes first, since C wants it complete by then.
func (g *gen) emitStructs() {
	sort.SliceStable(g.order, func(i, j int) bool { return g.order[i].Pos() < g.order[j].Pos() })
	done := map[string]bool{}
	var emit func(name string, st *types.Struct)
	emit = func(name string, st *types.Struct) {
		if done[name] {
			return
		}
		done[name] = true
		for i := 0; i < st.NumFields(); i++ {
			ft := st.Field(i).Type()
			if s, ok := ft.Underlying().(*types.Slice); ok {
				ft = s.Elem()
			}
			if n, ok := types.Unalias(ft).(*types.Named); ok && g.declaredHere(n.Obj()) {
				if inner, ok := n.Underlying().(*types.Struct); ok {
					emit(g.structName(n.Obj()), inner)
				}
			}
		}
		g.emitStruct(name, st)
	}
	for i := 0; i < len(g.order); i++ {
		o := g.order[i]
		emit(g.structs[o], o.Type().Underlying().(*types.Struct))
	}
	for _, a := range g.anon {
		emit(a.name, a.st)
	}
}

func (g *gen) emitStruct(name string, st *types.Struct) {
	g.printf("typedef struct %s {\n", name)
	for i := 0; i < st.NumFields(); i++ {
		f := st.Field(i)
		fname := cIdent(f.Name())
		ft := f.Type()
		if s, ok := ft.Underlying().(*types.Slice); ok && !isByte(s.Elem()) {
			g.printf("    const %s *%s;\n", g.cType(s.Elem()), fname)
			g.printf("    Int %s_len;\n", fname)
			continue
		}
		if a, ok := ft.Underlying().(*types.Array); ok {
			g.printf("    %s %s[%d];\n", g.cType(a.Elem()), fname, a.Len())
			continue
		}
		if sig, ok := ft.Underlying().(*types.Signature); ok {
			g.printf("    void (*%s)(void); /* %s */\n", fname, commentSafe(types.TypeString(sig, g.qualifier)))
			continue
		}
		g.printf("    %s %s;\n", g.cType(ft), fname)
	}
	g.printf("} %s;\n\n", name)
}

func (g *gen) emitTables() {
	for _, t := range g.tables {
		unused := ""
		if !t.used {
			unused = " BURROW_UNUSED"
		}
		g.printf("static const %s %s[]%s%s = {\n", t.ctype, t.name, t.dims, unused)
		for _, e := range t.lit.Elts {
			if kv, ok := e.(*ast.KeyValueExpr); ok {
				// An array with its indices written out.
				e = kv.Value
			}
			g.printf("    %s,\n", g.value(e, t.elem))
		}
		g.printf("};\n\n")
	}
}

// ------------------------------------------------------------------ types

func (g *gen) qualifier(p *types.Package) string {
	if p == g.pkg {
		return ""
	}
	return p.Name()
}

// cType is the C spelling of a Go type that a table field holds.
func (g *gen) cType(t types.Type) string {
	if isError(t) {
		return "const Error *"
	}
	if isByteSlice(t) {
		return "Str"
	}
	switch u := types.Unalias(t).(type) {
	case *types.Basic:
		return basicC(u)
	case *types.Named:
		if g.declaredHere(u.Obj()) {
			if _, ok := u.Underlying().(*types.Struct); ok {
				return g.structName(u.Obj())
			}
			if b, ok := u.Underlying().(*types.Basic); ok {
				return basicC(b)
			}
		}
		if u.Obj().Pkg() != nil {
			return marker("type", u.Obj().Pkg().Path(), u.Obj().Name())
		}
	case *types.Pointer:
		return "const " + g.cType(u.Elem()) + " *"
	}
	g.byHand++
	return "void * /* by hand: " + commentSafe(types.TypeString(t, g.qualifier)) + " */"
}

func basicC(b *types.Basic) string {
	switch b.Name() {
	case "byte":
		return "Byte"
	case "rune":
		return "Rune"
	}
	switch b.Kind() {
	case types.Bool, types.UntypedBool:
		return "bool"
	case types.Int, types.UntypedInt:
		return "Int"
	case types.Int8:
		return "int8_t"
	case types.Int16:
		return "int16_t"
	case types.Int32, types.UntypedRune:
		return "int32_t"
	case types.Int64:
		return "int64_t"
	case types.Uint:
		return "Uint"
	case types.Uint8:
		return "uint8_t"
	case types.Uint16:
		return "uint16_t"
	case types.Uint32:
		return "uint32_t"
	case types.Uint64:
		return "uint64_t"
	case types.Uintptr:
		return "Uintptr"
	case types.Float32:
		return "float"
	case types.Float64, types.UntypedFloat:
		return "double"
	case types.String, types.UntypedString:
		return "Str"
	}
	return "void * /* by hand: " + b.Name() + " */"
}

func isString(t types.Type) bool {
	b, ok := t.Underlying().(*types.Basic)
	return ok && b.Info()&types.IsString != 0
}

func isTableLit(e ast.Expr) bool {
	_, ok := e.(*ast.CompositeLit)
	return ok
}

// stringValue works out a string the type checker could not, because it is
// built from the file's variables or with strings.Repeat, which is how Go's
// tests make their long inputs.
func (g *gen) stringValue(e ast.Expr) (string, bool) {
	if tv, ok := g.info.Types[e]; ok && tv.Value != nil && tv.Value.Kind() == constant.String {
		return constant.StringVal(tv.Value), true
	}
	switch e := e.(type) {
	case *ast.ParenExpr:
		return g.stringValue(e.X)
	case *ast.Ident:
		if init, ok := g.inits[g.info.Uses[e]]; ok {
			return g.stringValue(init)
		}
	case *ast.BinaryExpr:
		if e.Op == token.ADD {
			a, ok1 := g.stringValue(e.X)
			b, ok2 := g.stringValue(e.Y)
			if ok1 && ok2 {
				return a + b, true
			}
		}
	case *ast.CallExpr:
		var fn *ast.Ident
		switch f := e.Fun.(type) {
		case *ast.Ident:
			fn = f
		case *ast.SelectorExpr:
			fn = f.Sel
		}
		if fn == nil {
			return "", false
		}
		if tv, ok := g.info.Types[e.Fun]; ok && tv.IsType() && len(e.Args) == 1 {
			return g.stringValue(e.Args[0])
		}
		obj := g.info.Uses[fn]
		if obj == nil || obj.Pkg() == nil {
			return "", false
		}
		if (obj.Pkg().Path() == "strings" || obj.Pkg().Path() == "bytes") && obj.Name() == "Repeat" && len(e.Args) == 2 {
			s, ok := g.stringValue(e.Args[0])
			tv, ok2 := g.info.Types[e.Args[1]]
			if ok && ok2 && tv.Value != nil {
				if n, exact := constant.Int64Val(tv.Value); exact && n >= 0 && int64(len(s))*n <= 1<<20 {
					return strings.Repeat(s, int(n)), true
				}
			}
		}
	}
	return "", false
}

func isError(t types.Type) bool {
	return types.Identical(t, types.Universe.Lookup("error").Type())
}

func isByte(t types.Type) bool {
	b, ok := t.Underlying().(*types.Basic)
	return ok && b.Kind() == types.Uint8
}

func isByteSlice(t types.Type) bool {
	s, ok := t.Underlying().(*types.Slice)
	return ok && isByte(s.Elem())
}

// ----------------------------------------------------------------- values

// value is the C initializer for a Go expression of type t.
func (g *gen) value(e ast.Expr, t types.Type) string {
	if p, ok := e.(*ast.ParenExpr); ok {
		return g.value(p.X, t)
	}
	if tv, ok := g.info.Types[e]; ok && tv.Value != nil {
		if s, ok := g.constant(tv.Value, t); ok {
			return s
		}
	}
	// A conversion is its operand, as the type the table wants.
	if c, ok := e.(*ast.CallExpr); ok && len(c.Args) == 1 {
		if tv, ok := g.info.Types[c.Fun]; ok && tv.IsType() && !isByteSlice(tv.Type) {
			return g.value(c.Args[0], t)
		}
	}
	// One of the file's own variables is whatever it was set to, as long as
	// nothing in the file assigns to it later, which Go's tests do not do to
	// the variables their tables use.
	if id, ok := e.(*ast.Ident); ok {
		if init, ok := g.inits[g.info.Uses[id]]; ok && !isTableLit(init) {
			return g.value(init, t)
		}
	}
	if isString(t) || isByteSlice(t) {
		if str, ok := g.stringValue(e); ok {
			return "SI(" + cString(str) + ")"
		}
	}
	if isError(t) {
		if isNil(e) {
			return "NULL"
		}
		if ref, ok := g.ref(e); ok {
			return "&" + ref
		}
		return g.handZero(e, "NULL")
	}
	switch u := t.Underlying().(type) {
	case *types.Struct:
		if lit, ok := e.(*ast.CompositeLit); ok {
			return g.structValue(lit, t)
		}
	case *types.Slice:
		if isByte(u.Elem()) {
			if isNil(e) {
				return "{0}"
			}
			if c, ok := e.(*ast.CallExpr); ok && len(c.Args) == 1 {
				if tv, ok := g.info.Types[c.Args[0]]; ok && tv.Value != nil && tv.Value.Kind() == constant.String {
					return "SI(" + cString(constant.StringVal(tv.Value)) + ")"
				}
			}
			if lit, ok := e.(*ast.CompositeLit); ok {
				var b []byte
				for _, el := range lit.Elts {
					tv, ok := g.info.Types[el]
					if !ok || tv.Value == nil {
						return g.handZero(e, "{0}")
					}
					n, _ := constant.Int64Val(tv.Value)
					b = append(b, byte(n))
				}
				return "SI(" + cString(string(b)) + ")"
			}
			return g.handZero(e, "{0}")
		}
	case *types.Array:
		if lit, ok := e.(*ast.CompositeLit); ok {
			var vals []string
			for _, el := range lit.Elts {
				if kv, ok := el.(*ast.KeyValueExpr); ok {
					el = kv.Value
				}
				vals = append(vals, g.value(el, u.Elem()))
			}
			return "{" + strings.Join(vals, ", ") + "}"
		}
	case *types.Pointer:
		if isNil(e) {
			return "NULL"
		}
		// &T{...} is a compound literal, which at file scope lives as long
		// as the table does.
		if a, ok := e.(*ast.UnaryExpr); ok && a.Op == token.AND {
			if lit, ok := a.X.(*ast.CompositeLit); ok {
				if _, ok := u.Elem().Underlying().(*types.Struct); ok {
					return "&(const " + g.cType(u.Elem()) + ")" + g.structValue(lit, u.Elem())
				}
			}
		}
	case *types.Signature:
		if isNil(e) {
			return "NULL"
		}
		if ref, ok := g.ref(e); ok {
			return "(void (*)(void))" + ref
		}
	case *types.Basic:
		if u.Info()&types.IsFloat != 0 {
			if s, ok := g.mathCall(e, u.Kind() == types.Float32); ok {
				return s
			}
		}
	}
	if isNil(e) {
		return "{0}"
	}
	return g.handZero(e, zeroFor(t))
}

func zeroFor(t types.Type) string {
	if isError(t) {
		return "NULL"
	}
	switch u := t.Underlying().(type) {
	case *types.Basic:
		if u.Info()&types.IsString != 0 {
			return "{0}"
		}
		if u.Info()&types.IsBoolean != 0 {
			return "false"
		}
		return "0"
	case *types.Struct:
		return "{0}"
	case *types.Slice:
		if isByte(u.Elem()) {
			return "{0}"
		}
	}
	return "NULL"
}

// handZero is a value nobody could translate mechanically. It compiles as the
// zero value and says in a comment what Go had.
func (g *gen) handZero(e ast.Expr, zero string) string {
	g.byHand++
	return "/* by hand: " + commentSafe(g.goSource(e)) + " */ " + zero
}

// structValue is a struct literal. One of the file's own structs keeps Go's
// shape, positional or keyed. One from a package is always keyed, with the C
// names of its fields, since burrow's struct need not list them in Go's order.
func (g *gen) structValue(lit *ast.CompositeLit, t types.Type) string {
	st := t.Underlying().(*types.Struct)
	var foreign *types.TypeName
	if n, ok := types.Unalias(t).(*types.Named); ok && !g.declaredHere(n.Obj()) && n.Obj().Pkg() != nil {
		foreign = n.Obj()
	}
	var parts []string
	keyed := len(lit.Elts) > 0
	for _, e := range lit.Elts {
		if _, ok := e.(*ast.KeyValueExpr); !ok {
			keyed = false
		}
	}
	if foreign != nil {
		keyed = true
	}
	for i, e := range lit.Elts {
		var f *types.Var
		val := e
		if kv, ok := e.(*ast.KeyValueExpr); ok {
			key, _ := kv.Key.(*ast.Ident)
			for j := 0; j < st.NumFields() && key != nil; j++ {
				if st.Field(j).Name() == key.Name {
					f = st.Field(j)
				}
			}
			val = kv.Value
		} else if i < st.NumFields() {
			f = st.Field(i)
		}
		if f == nil {
			parts = append(parts, g.handZero(e, "0"))
			continue
		}
		prefix := ""
		if foreign != nil {
			prefix = "." + marker("field", foreign.Pkg().Path(), foreign.Name()+"."+f.Name()) + " = "
		} else if keyed {
			prefix = "." + cIdent(f.Name()) + " = "
		}
		if foreign != nil && isError(f.Type()) {
			// burrow's own structs hold an Error by value, and a copy of a
			// package's sentinel is not a constant a static table can hold.
			if isNil(val) {
				parts = append(parts, prefix+"{0}")
			} else {
				parts = append(parts, prefix+g.handZero(val, "{0}"))
			}
			continue
		}
		if s, ok := f.Type().Underlying().(*types.Slice); ok && !isByte(s.Elem()) {
			if foreign != nil {
				// burrow's own structs hold a Slice, and what goes in one
				// is not something a table can say.
				parts = append(parts, prefix+g.handZero(val, "{0}"))
				continue
			}
			ptr, n := g.sliceValue(val, s.Elem())
			if keyed {
				parts = append(parts, prefix+ptr, "."+cIdent(f.Name())+"_len = "+n)
			} else {
				parts = append(parts, ptr, n)
			}
			continue
		}
		parts = append(parts, prefix+g.value(val, f.Type()))
	}
	return "{" + strings.Join(parts, ", ") + "}"
}

// sliceValue is a slice field: a compound literal for the elements and the
// count, which a static table can hold because it lives at file scope.
func (g *gen) sliceValue(e ast.Expr, elem types.Type) (string, string) {
	if isNil(e) {
		return "NULL", "0"
	}
	lit, ok := e.(*ast.CompositeLit)
	if !ok {
		return g.handZero(e, "NULL"), "0"
	}
	if len(lit.Elts) == 0 {
		return "NULL", "0"
	}
	var vals []string
	for _, el := range lit.Elts {
		vals = append(vals, g.value(el, elem))
	}
	return "(const " + g.cType(elem) + "[]){" + strings.Join(vals, ", ") + "}", strconv.Itoa(len(lit.Elts))
}

// ref is a reference to a package level variable or function, as a marker
// for burrow-gen to turn into its C name.
func (g *gen) ref(e ast.Expr) (string, bool) {
	var id *ast.Ident
	switch e := e.(type) {
	case *ast.Ident:
		id = e
	case *ast.SelectorExpr:
		id = e.Sel
	default:
		return "", false
	}
	obj := g.info.Uses[id]
	if obj == nil || obj.Pkg() == nil || obj.Parent() != obj.Pkg().Scope() {
		return "", false
	}
	if g.declaredHere(obj) {
		return "", false
	}
	switch obj.(type) {
	case *types.Var:
		return marker("var", obj.Pkg().Path(), obj.Name()), true
	case *types.Func:
		return marker("func", obj.Pkg().Path(), obj.Name()), true
	}
	return "", false
}

// mathCall covers the float values Go can only write as calls.
func (g *gen) mathCall(e ast.Expr, f32 bool) (string, bool) {
	neg := false
	if u, ok := e.(*ast.UnaryExpr); ok && u.Op == token.SUB {
		neg = true
		e = u.X
	}
	c, ok := e.(*ast.CallExpr)
	if !ok {
		return "", false
	}
	var fn *ast.Ident
	switch f := c.Fun.(type) {
	case *ast.Ident:
		fn = f // math's own tests call NaN() unqualified
	case *ast.SelectorExpr:
		fn = f.Sel
	default:
		return "", false
	}
	obj := g.info.Uses[fn]
	if obj == nil || obj.Pkg() == nil || obj.Pkg().Path() != "math" {
		return "", false
	}
	switch obj.Name() {
	case "NaN":
		return floatC(math.NaN(), f32), true
	case "Inf":
		if len(c.Args) == 1 {
			if tv, ok := g.info.Types[c.Args[0]]; ok && tv.Value != nil {
				if constant.Sign(tv.Value) < 0 {
					neg = !neg
				}
				if neg {
					return floatC(math.Inf(-1), f32), true
				}
				return floatC(math.Inf(1), f32), true
			}
		}
	case "Copysign":
		if len(c.Args) == 2 {
			a, ok1 := g.info.Types[c.Args[0]]
			b, ok2 := g.info.Types[c.Args[1]]
			if ok1 && ok2 && a.Value != nil && b.Value != nil {
				x, _ := constant.Float64Val(a.Value)
				y, _ := constant.Float64Val(b.Value)
				v := math.Copysign(x, y)
				if neg {
					v = -v
				}
				return floatC(v, f32), true
			}
		}
	case "Float64frombits", "Float32frombits":
		if len(c.Args) == 1 {
			if tv, ok := g.info.Types[c.Args[0]]; ok && tv.Value != nil {
				n, _ := constant.Uint64Val(tv.Value)
				var v float64
				if obj.Name() == "Float32frombits" {
					v = float64(math.Float32frombits(uint32(n)))
				} else {
					v = math.Float64frombits(n)
				}
				if neg {
					v = -v
				}
				return floatC(v, f32), true
			}
		}
	}
	return "", false
}

func (g *gen) constant(v constant.Value, t types.Type) (string, bool) {
	b, ok := t.Underlying().(*types.Basic)
	if !ok {
		if isByteSlice(t) && v.Kind() == constant.String {
			return "SI(" + cString(constant.StringVal(v)) + ")", true
		}
		return "", false
	}
	switch {
	case b.Info()&types.IsBoolean != 0:
		if constant.BoolVal(v) {
			return "true", true
		}
		return "false", true
	case b.Info()&types.IsString != 0:
		if v.Kind() != constant.String {
			return "", false
		}
		return "SI(" + cString(constant.StringVal(v)) + ")", true
	case b.Info()&types.IsFloat != 0:
		f, _ := constant.Float64Val(constant.ToFloat(v))
		return floatC(f, b.Kind() == types.Float32), true
	case b.Info()&types.IsInteger != 0:
		v = constant.ToInt(v)
		if v.Kind() != constant.Int {
			return "", false
		}
		if b.Name() == "rune" || b.Kind() == types.UntypedRune {
			if n, ok := constant.Int64Val(v); ok && n >= 0 {
				return runeC(n), true
			}
		}
		return intC(v, b), true
	}
	return "", false
}

// runeC is a rune the way a person would write it in C: a character literal
// for ASCII and hex for the rest, which is how Unicode tables read.
func runeC(n int64) string {
	switch n {
	case '\a':
		return `'\a'`
	case '\b':
		return `'\b'`
	case '\f':
		return `'\f'`
	case '\n':
		return `'\n'`
	case '\r':
		return `'\r'`
	case '\t':
		return `'\t'`
	case '\v':
		return `'\v'`
	case '\\':
		return `'\\'`
	case '\'':
		return `'\''`
	}
	if n >= 0x20 && n < 0x7f {
		return "'" + string(rune(n)) + "'"
	}
	return fmt.Sprintf("0x%x", n)
}

func intC(v constant.Value, b *types.Basic) string {
	unsigned := b.Info()&types.IsUnsigned != 0
	if unsigned {
		n, _ := constant.Uint64Val(v)
		if n > math.MaxInt32 {
			return fmt.Sprintf("UINT64_C(%d)", n)
		}
		return strconv.FormatUint(n, 10)
	}
	n, _ := constant.Int64Val(v)
	switch {
	case n == math.MinInt64:
		return "INT64_MIN"
	case n > math.MaxInt32 || n < math.MinInt32:
		return fmt.Sprintf("INT64_C(%d)", n)
	case n == math.MinInt32:
		return "INT32_MIN"
	}
	return strconv.FormatInt(n, 10)
}

func floatC(f float64, f32 bool) string {
	// INFINITY and NAN are floats, and a double from one needs the cast or
	// -Wdouble-promotion stops the build.
	cast := "(double)"
	if f32 {
		cast = ""
	}
	switch {
	case math.IsInf(f, 1):
		return cast + "INFINITY"
	case math.IsInf(f, -1):
		return "-" + cast + "INFINITY"
	case math.IsNaN(f):
		return cast + "NAN"
	}
	bits := 64
	if f32 {
		bits = 32
	}
	s := strconv.FormatFloat(f, 'g', -1, bits)
	if !strings.ContainsAny(s, ".eEn") {
		s += ".0"
	}
	if f == 0 && math.Signbit(f) {
		s = "-0.0"
	}
	if f32 {
		s += "f"
	}
	return s
}

// cString is a C string literal with exactly the bytes of s. Printable UTF-8
// stays as it is, so a table reads the way Go's did.
func cString(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	hex := false // the last thing written was a \x escape
	for i := 0; i < len(s); {
		r, n := utf8.DecodeRuneInString(s[i:])
		if hex && i < len(s) && isHexDigit(s[i]) {
			b.WriteString(`" "`)
		}
		hex = false
		switch {
		case r == utf8.RuneError && n == 1:
			fmt.Fprintf(&b, `\x%02x`, s[i])
			hex = true
		case r == '"':
			b.WriteString(`\"`)
		case r == '\\':
			b.WriteString(`\\`)
		case r == '?' && i > 0 && s[i-1] == '?':
			// The second of two question marks, so no trigraph can start.
			b.WriteString(`\?`)
		case r == '\a':
			b.WriteString(`\a`)
		case r == '\b':
			b.WriteString(`\b`)
		case r == '\f':
			b.WriteString(`\f`)
		case r == '\n':
			b.WriteString(`\n`)
		case r == '\r':
			b.WriteString(`\r`)
		case r == '\t':
			b.WriteString(`\t`)
		case r == '\v':
			b.WriteString(`\v`)
		case r < 0x80 && r >= 0x20 && r != 0x7f:
			b.WriteRune(r)
		case r >= 0x80 && unicode.IsPrint(r):
			b.WriteString(s[i : i+n])
		default:
			for j := 0; j < n; j++ {
				fmt.Fprintf(&b, `\x%02x`, s[i+j])
			}
			hex = true
		}
		i += n
	}
	b.WriteByte('"')
	return b.String()
}

func isHexDigit(c byte) bool {
	return '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F'
}

func isNil(e ast.Expr) bool {
	id, ok := e.(*ast.Ident)
	return ok && id.Name == "nil"
}

// -------------------------------------------------------------- functions

func isTest(d *ast.FuncDecl) bool {
	n := d.Name.Name
	if d.Recv != nil || !strings.HasPrefix(n, "Test") || n == "TestMain" {
		return false
	}
	if len(n) > 4 {
		r, _ := utf8.DecodeRuneInString(n[4:])
		if unicode.IsLower(r) {
			return false
		}
	}
	return d.Type.Params != nil && len(d.Type.Params.List) == 1
}

// emitFuncs writes every Test function and gives back their names, and the
// names of everything else in the file that a person still has to port.
func (g *gen) emitFuncs() ([]string, []string) {
	var tests, others []string
	for _, d := range g.file.Decls {
		fd, ok := d.(*ast.FuncDecl)
		if !ok {
			continue
		}
		if !isTest(fd) {
			name := fd.Name.Name
			if fd.Recv != nil && len(fd.Recv.List) == 1 {
				name = "(" + g.goSource(fd.Recv.List[0].Type) + ") " + name
			}
			others = append(others, name)
			continue
		}
		tests = append(tests, fd.Name.Name)
		g.emitTest(fd)
	}
	return tests, others
}

func (g *gen) emitTest(fd *ast.FuncDecl) {
	tparam := "t"
	if names := fd.Type.Params.List[0].Names; len(names) == 1 {
		tparam = names[0].Name
	}
	g.printf("static void %s(TestingT *%s) {\n", fd.Name.Name, tparam)

	// The first loop over a table is written out, and the rest of the body
	// around it stays Go.
	var loop ast.Stmt
	var tab *table
	var index, value string
	var body *ast.BlockStmt
	for _, st := range fd.Body.List {
		if t, i, v, b := g.tableLoop(st); t != nil {
			loop, tab, index, value, body = st, t, i, v, b
			break
		}
	}
	// Tables this test refers to, so a table no loop uses is still marked
	// as used by the test it belongs to.
	voided := map[*table]bool{}
	ast.Inspect(fd.Body, func(n ast.Node) bool {
		if id, ok := n.(*ast.Ident); ok {
			if t, ok := g.byName[g.info.Uses[id]]; ok && t != tab && !voided[t] {
				voided[t] = true
				g.printf("    (void)%s;\n", t.name)
			}
		}
		return true
	})

	if loop == nil {
		g.printf("    /* Go, from %s:%d:\n", g.base, g.fset.Position(fd.Pos()).Line)
		g.goComment(fd.Body.List, "     *")
		g.printf("     */\n")
		g.printf("    testing_t_skip_v(%s, \"burrow-gen tests: not table driven, port by hand\");\n", tparam)
		g.printf("}\n\n")
		g.skipped++
		return
	}

	g.loops++
	tab.used = true
	var before, after []ast.Stmt
	seen := false
	for _, st := range fd.Body.List {
		switch {
		case st == loop:
			seen = true
		case !seen:
			if !g.isTableDecl(st, tab) {
				before = append(before, st)
			}
		default:
			after = append(after, st)
		}
	}
	if len(before) > 0 {
		g.printf("    /* Go, before the loop:\n")
		g.goComment(before, "     *")
		g.printf("     */\n")
	}
	g.printf("    for (size_t %s = 0; %s < sizeof %s / sizeof %s[0]; %s++) {\n", index, index, tab.name, tab.name, index)
	if value != "" {
		if tab.dims != "" {
			g.printf("        const %s *%s = %s[%s];\n", tab.ctype, value, tab.name, index)
		} else {
			g.printf("        const %s *%s = &%s[%s];\n", tab.ctype, value, tab.name, index)
		}
		g.printf("        (void)%s;\n", value)
		g.printf("        /* Go, with %s as a pointer now:\n", value)
	} else {
		// The body reaches the table through the index, which is in the Go
		// below, so this is the only use the compiler sees.
		g.printf("        (void)%s[%s];\n", tab.name, index)
		g.printf("        /* Go:\n")
	}
	g.goComment(body.List, "         *")
	g.printf("         */\n")
	g.printf("    }\n")
	if len(after) > 0 {
		g.printf("    /* Go, after the loop:\n")
		g.goComment(after, "     *")
		g.printf("     */\n")
	}
	g.printf("    testing_t_skip_v(%s, \"burrow-gen tests: the loop body is still Go\");\n", tparam)
	g.printf("}\n\n")
	g.skipped++
}

// tableLoop is a loop over a table in either of the two ways Go's tests write
// one, "for i, tt := range tests" and "for i := 0; i < len(tests); i++", with
// the table, the index, the element if there is one, and the body.
func (g *gen) tableLoop(st ast.Stmt) (*table, string, string, *ast.BlockStmt) {
	name := func(e ast.Expr) string {
		if id, ok := e.(*ast.Ident); ok && id.Name != "_" {
			return id.Name
		}
		return ""
	}
	switch st := st.(type) {
	case *ast.RangeStmt:
		id, ok := st.X.(*ast.Ident)
		if !ok {
			return nil, "", "", nil
		}
		t, ok := g.byName[g.info.Uses[id]]
		if !ok {
			return nil, "", "", nil
		}
		index := name(st.Key)
		if index == "" {
			index = "i"
		}
		return t, index, name(st.Value), st.Body
	case *ast.ForStmt:
		init, ok := st.Init.(*ast.AssignStmt)
		if !ok || len(init.Lhs) != 1 || init.Tok != token.DEFINE {
			return nil, "", "", nil
		}
		index := name(init.Lhs[0])
		cond, ok := st.Cond.(*ast.BinaryExpr)
		if !ok || index == "" || cond.Op != token.LSS || name(cond.X) != index {
			return nil, "", "", nil
		}
		call, ok := cond.Y.(*ast.CallExpr)
		if !ok || name(call.Fun) != "len" || len(call.Args) != 1 {
			return nil, "", "", nil
		}
		id, ok := call.Args[0].(*ast.Ident)
		if !ok {
			return nil, "", "", nil
		}
		if t, ok := g.byName[g.info.Uses[id]]; ok {
			return t, index, "", st.Body
		}
	}
	return nil, "", "", nil
}

func (g *gen) isTableDecl(st ast.Stmt, t *table) bool {
	switch st := st.(type) {
	case *ast.AssignStmt:
		if len(st.Lhs) == 1 {
			if id, ok := st.Lhs[0].(*ast.Ident); ok {
				return g.info.Defs[id] == t.obj
			}
		}
	case *ast.DeclStmt:
		if gd, ok := st.Decl.(*ast.GenDecl); ok {
			for _, s := range gd.Specs {
				if vs, ok := s.(*ast.ValueSpec); ok {
					for _, n := range vs.Names {
						if g.info.Defs[n] == t.obj {
							return true
						}
					}
				}
			}
		}
	}
	return false
}

func (g *gen) goSource(n ast.Node) string {
	var b bytes.Buffer
	printer.Fprint(&b, g.fset, n)
	return b.String()
}

// goComment writes Go statements into a C comment, one line each, at the
// indent they had.
func (g *gen) goComment(stmts []ast.Stmt, lead string) {
	for _, st := range stmts {
		src := commentSafe(g.goSource(st))
		for _, line := range strings.Split(src, "\n") {
			line = strings.ReplaceAll(line, "\t", "    ")
			if strings.TrimSpace(line) == "" {
				g.printf("%s\n", lead)
				continue
			}
			g.printf("%s     %s\n", lead, line)
		}
	}
}

// ------------------------------------------------------------------- names

func marker(kind, path, name string) string {
	return "⟪" + kind + " " + path + " " + name + "⟫"
}

func commentSafe(s string) string {
	s = strings.ReplaceAll(s, "*/", "* /")
	return strings.ReplaceAll(s, "/*", "/ *")
}

var cKeywords = map[string]bool{
	"auto": true, "char": true, "const": true, "default": true, "do": true, "double": true,
	"enum": true, "extern": true, "float": true, "inline": true, "int": true, "long": true,
	"register": true, "restrict": true, "short": true, "signed": true, "sizeof": true,
	"static": true, "union": true, "unsigned": true, "void": true, "volatile": true,
	"while": true, "bool": true, "true": true, "false": true, "new": true, "delete": true,
	"class": true, "this": true, "template": true, "typename": true, "private": true,
	"public": true, "protected": true, "operator": true,
}

// cIdent is a Go field name that C, or C++ reading burrow's headers, can use.
func cIdent(s string) string {
	if cKeywords[s] {
		return s + "_"
	}
	return s
}

func camel(s string) string {
	parts := strings.Split(s, "_")
	for i, p := range parts {
		if p != "" {
			parts[i] = strings.ToUpper(p[:1]) + p[1:]
		}
	}
	return strings.Join(parts, "")
}

// snake is only for the names of tables that were local to a test, which are
// burrow's own and not part of any API, so it does not need burrow-coverage's
// rules for acronyms.
func snake(s string) string {
	var b strings.Builder
	for i, r := range s {
		if unicode.IsUpper(r) {
			if i > 0 {
				prev, _ := utf8.DecodeLastRuneInString(s[:i])
				next, _ := utf8.DecodeRuneInString(s[i+utf8.RuneLen(r):])
				if !unicode.IsUpper(prev) || unicode.IsLower(next) {
					b.WriteByte('_')
				}
			}
			b.WriteRune(unicode.ToLower(r))
			continue
		}
		b.WriteRune(r)
	}
	return b.String()
}
