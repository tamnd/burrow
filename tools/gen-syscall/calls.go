//go:build ignore

// The functions: include/burrow/syscall/zsyscall.h, src/syscall/zsyscall.h and
// src/syscall/zsyscall.c, the wrappers Go's mksyscall.pl writes from the //sys
// lines into the zsyscall files.
//
// Rather than read the //sys lines again and redo what mksyscall.pl does with
// them, this translates the functions mksyscall.pl wrote, statement by
// statement, from the type checked zsyscall file of each system and
// architecture. They are all the same few shapes: turn the arguments into
// uintptrs, make the call, turn the results back, and turn a nonzero errno
// into an error. So an architecture that passes a 64-bit argument in two
// halves, or pads one to an even register, or uses another call number, does
// it here because Go's file does, and nothing about it is written down twice.
//
// An exported function becomes a public one, syscall_chdir for Chdir, and an
// unexported one becomes burrow__syscall_ and its name, for the hand-written
// functions that wrap it. A function syscall.h already declares is left to it,
// and so is one with a type C can't spell, which -v reports.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package main

import (
	"bytes"
	"fmt"
	"go/ast"
	"go/constant"
	"go/token"
	"go/types"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// The systems whose functions are written, with their #if condition. Linux's
// covers Cosmopolitan and wasip1, as the #else of the tables does, and there a
// system call fails with ENOSYS. The rest are on the design's Tertiary list
// and come when someone asks.
var callSystems = []struct{ goos, cond string }{
	{"darwin", "defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)"},
	{"freebsd", "defined(BURROW_OS_FREEBSD)"},
	{"linux", "defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)"},
}

// The libSystem functions that are variadic, and how many parameters come
// before the "...". Apple's arm64 passes the rest on the stack, so the call
// has to know.
var libcVariadic = map[string]int{"open": 2, "openat": 3, "fcntl": 2, "ioctl": 2}

// The C name of a syscall function.
func funcName(name string) string {
	n := strings.ToLower(strings.Join(words(name), "_"))
	if ast.IsExported(name) {
		return "syscall_" + n
	}
	return "burrow__syscall_" + n
}

// The C name of a parameter. A C keyword or a lower case macro of the system
// headers gets an underscore after it, as a field does.
func paramName(name string) string {
	if reservedField[name] {
		return name + "_"
	}
	return name
}

// One translated function on one platform.
type cfunc struct {
	name  string // the Go name
	proto string // the C declaration, with no semicolon
	def   string // the C definition
	libc  map[string]bool
}

// callGen translates the zsyscall functions of one platform.
type callGen struct {
	p    *platform
	tg   *typeGen
	hand map[string]bool   // Go names of the types syscall.h writes
	libc map[string]string // libc_X to the symbol, from cgo_import_dynamic
	why  map[string]string
}

var errorType = types.Universe.Lookup("error").Type()

// ctype is the C declaration of name with Go type t, or the reason there is
// none. A string is a Str, a slice a Slice, and a pointer to an
// array a pointer to its first element.
func (g *callGen) ctype(t types.Type, name string) (string, string) {
	sp := func(c string) string {
		if name == "" || strings.HasSuffix(c, "*") {
			return c + name
		}
		return c + " " + name
	}
	switch u := types.Unalias(t).(type) {
	case *types.Basic:
		if u.Kind() == types.String {
			return sp("Str"), ""
		}
	case *types.Slice:
		if _, why := g.ctype(u.Elem(), ""); why != "" {
			return "", why
		}
		return sp("Slice"), ""
	case *types.Pointer:
		if a, ok := types.Unalias(u.Elem()).(*types.Array); ok {
			return g.ctype(types.NewPointer(a.Elem()), name)
		}
		if n, ok := types.Unalias(u.Elem()).(*types.Named); ok && g.hand[n.Obj().Name()] && n.Obj().Pkg() != nil && n.Obj().Pkg().Path() == "syscall" {
			return "Syscall" + n.Obj().Name() + " *" + name, ""
		}
	case *types.Named:
		if types.Identical(u, errorType) {
			return sp("Error"), ""
		}
		if g.hand[u.Obj().Name()] && u.Obj().Pkg() != nil && u.Obj().Pkg().Path() == "syscall" {
			return sp("Syscall" + u.Obj().Name()), ""
		}
	}
	d, why := g.tg.decl(t, name, false, map[string]bool{}, "")
	if why != "" {
		return "", why
	}
	return strings.TrimRight(d, " "), ""
}

// castType is the C type of t, for a cast.
func (g *callGen) castType(t types.Type) (string, string) {
	c, why := g.ctype(t, "")
	return strings.TrimSpace(c), why
}

// A function being translated.
type trans struct {
	g        *callGen
	decls    []string // declarations, at the top
	inits    []string // what has to happen before anything can jump to done
	b        strings.Builder
	names    map[types.Object]string
	used     map[string]bool // C names taken, since Go may reuse one in a block
	cstrs    []string
	usesDone bool
	libc     map[string]bool
	why      string
}

func (t *trans) fail(format string, args ...any) string {
	if t.why == "" {
		t.why = fmt.Sprintf(format, args...)
	}
	return "0"
}

func (t *trans) typeOf(e ast.Expr) types.Type {
	return t.g.p.info.Types[e].Type
}

func (t *trans) objOf(id *ast.Ident) types.Object {
	if o := t.g.p.info.Uses[id]; o != nil {
		return o
	}
	return t.g.p.info.Defs[id]
}

// declare puts a local for v at the top of the function.
func (t *trans) declare(v types.Object) string {
	if n, ok := t.names[v]; ok {
		return n
	}
	n := v.Name()
	for i := 2; t.used[n]; i++ {
		n = fmt.Sprintf("%s_%d", v.Name(), i)
	}
	t.used[n] = true
	c, why := t.g.ctype(v.Type(), n)
	if why != "" {
		t.fail("local %s: %s", n, why)
		return n
	}
	zero := "0"
	if _, ok := types.Unalias(v.Type()).(*types.Pointer); ok {
		zero = "NULL"
	} else if b, ok := types.Unalias(v.Type()).(*types.Basic); ok && b.Kind() == types.UnsafePointer {
		zero = "NULL"
	}
	t.decls = append(t.decls, c+" = "+zero+";")
	t.names[v] = n
	return n
}

func (t *trans) expr(e ast.Expr) string {
	if tv, ok := t.g.p.info.Types[e]; ok && tv.Value != nil {
		if c, ok := e.(*ast.CallExpr); !ok || !t.g.p.info.Types[c.Fun].IsType() {
			return t.constant(e, tv)
		}
	}
	switch e := e.(type) {
	case *ast.Ident:
		o := t.objOf(e)
		switch o := o.(type) {
		case *types.Var:
			if n, ok := t.names[o]; ok {
				return n
			}
			if o.Name() == "_zero" && o.Parent() == t.g.p.pkg.Scope() {
				return "burrow__syscall_zero"
			}
			return t.fail("variable %s", o.Name())
		case *types.Nil:
			return "NULL"
		}
		return t.fail("identifier %s", e.Name)
	case *ast.ParenExpr:
		return "(" + t.expr(e.X) + ")"
	case *ast.UnaryExpr:
		switch e.Op {
		case token.AND:
			return "&" + t.expr(e.X)
		case token.SUB:
			return "-" + t.expr(e.X)
		case token.XOR:
			return "~" + t.expr(e.X)
		}
		return t.fail("unary %s", e.Op)
	case *ast.StarExpr:
		return "(*" + t.expr(e.X) + ")"
	case *ast.BinaryExpr:
		return t.binary(e)
	case *ast.IndexExpr:
		xt := types.Unalias(t.typeOf(e.X)).Underlying()
		s, ok := xt.(*types.Slice)
		if !ok {
			return t.fail("index of %s", xt)
		}
		et, why := t.g.castType(types.NewPointer(s.Elem()))
		if why != "" {
			return t.fail("%s", why)
		}
		return "((" + et + ")" + t.expr(e.X) + ".p)[" + t.expr(e.Index) + "]"
	case *ast.CallExpr:
		return t.call(e, nil)
	}
	return t.fail("expression %T", e)
}

// constant writes the value of a constant expression. A syscall constant
// that zconst.h has, which is any exported one, is written by its name, so
// the code reads like Go's and is the same on the architectures where only
// the number differs.
func (t *trans) constant(e ast.Expr, tv types.TypeAndValue) string {
	if id, ok := e.(*ast.Ident); ok {
		if c, ok := t.objOf(id).(*types.Const); ok && c.Exported() && c.Pkg() == t.g.p.pkg {
			return cName(c.Name())
		}
	}
	switch tv.Value.Kind() {
	case constant.Int:
		s := tv.Value.ExactString()
		if v, ok := constant.Int64Val(tv.Value); ok && (v > 1<<31-1 || v < -1<<31) {
			s += "LL"
		} else if !ok {
			s += "ULL"
		}
		return s
	case constant.Bool:
		return tv.Value.String()
	case constant.String:
		return "BURROW_S(" + tv.Value.ExactString() + ")"
	}
	return t.fail("constant %s", tv.Value)
}

func (t *trans) binary(e *ast.BinaryExpr) string {
	// err != nil and err == nil.
	if id, ok := e.Y.(*ast.Ident); ok && id.Name == "nil" {
		if xt := t.typeOf(e.X); xt != nil && types.Identical(xt, errorType) {
			x := t.expr(e.X)
			if e.Op == token.NEQ {
				return "BURROW_FAILED(" + x + ")"
			}
			return "BURROW_OK(" + x + ")"
		}
	}
	// runtime.GOOS == "ios".
	if sel, ok := e.X.(*ast.SelectorExpr); ok && sel.Sel.Name == "GOOS" {
		if lit, ok := e.Y.(*ast.BasicLit); ok && lit.Value == `"ios"` && e.Op == token.EQL {
			return "BURROW__SYSCALL_IOS"
		}
		return t.fail("runtime.GOOS")
	}
	op := e.Op.String()
	if e.Op == token.AND_NOT {
		return "(" + t.expr(e.X) + " & ~" + t.expr(e.Y) + ")"
	}
	return "(" + t.expr(e.X) + " " + op + " " + t.expr(e.Y) + ")"
}

// call writes a call. outs, for a call with more than one result, are what
// the results after the first go to, "NULL" for one that is thrown away.
func (t *trans) call(e *ast.CallExpr, outs []string) string {
	ftv := t.g.p.info.Types[e.Fun]
	if ftv.IsType() {
		if len(e.Args) != 1 {
			return t.fail("conversion")
		}
		// A conversion to the type the value already has, like uintptr(r0),
		// is left out, since clang-tidy calls the cast redundant.
		if at := t.g.p.info.Types[e.Args[0]]; at.Type != nil && types.Identical(at.Type, ftv.Type) {
			return t.expr(e.Args[0])
		}
		to, why := t.g.castType(ftv.Type)
		if why != "" {
			return t.fail("conversion to %s", why)
		}
		return "(" + to + ")(" + t.expr(e.Args[0]) + ")"
	}
	if ftv.IsBuiltin() {
		id, _ := ast.Unparen(e.Fun).(*ast.Ident)
		switch {
		case id != nil && id.Name == "len":
			return t.expr(e.Args[0]) + ".len"
		case id != nil && id.Name == "panic":
			return "runtime_panic(" + t.expr(e.Args[0]) + ")"
		}
		return t.fail("builtin")
	}
	if sel, ok := e.Fun.(*ast.SelectorExpr); ok {
		// abi.FuncPCABI0(libc_X_trampoline) is the libSystem function X,
		// which is a number in the table zsyscall.c has.
		if sel.Sel.Name == "FuncPCABI0" && len(e.Args) == 1 {
			if id, ok := e.Args[0].(*ast.Ident); ok && strings.HasPrefix(id.Name, "libc_") && strings.HasSuffix(id.Name, "_trampoline") {
				fn := strings.TrimSuffix(id.Name, "_trampoline")
				if t.g.libc[fn] == "" {
					return t.fail("no library name for %s", fn)
				}
				t.libc[fn] = true
				return "(Uintptr)" + libcEnum(fn)
			}
		}
		return t.fail("call of %s", sel.Sel.Name)
	}
	id, ok := e.Fun.(*ast.Ident)
	if !ok {
		return t.fail("call of %T", e.Fun)
	}
	fn, ok := t.objOf(id).(*types.Func)
	if !ok || fn.Pkg() != t.g.p.pkg {
		return t.fail("call of %s", id.Name)
	}
	var args []string
	for _, a := range e.Args {
		args = append(args, t.expr(a))
	}
	switch id.Name {
	case "errnoErr":
		return "syscall_errno_as_error(" + args[0] + ", error_allocator())"
	}
	sig := fn.Type().(*types.Signature)
	n := sig.Results().Len()
	if n > 1 {
		for len(outs) < n-1 {
			outs = append(outs, "NULL")
		}
		args = append(args, outs...)
	}
	return funcName(id.Name) + "(" + strings.Join(args, ", ") + ")"
}

func (t *trans) out(lhs ast.Expr, define bool) string {
	id, ok := lhs.(*ast.Ident)
	if !ok {
		return "&" + t.expr(lhs)
	}
	if id.Name == "_" {
		return "NULL"
	}
	o := t.objOf(id)
	if define {
		if _, seen := t.names[o]; !seen {
			t.declare(o)
		}
	}
	return "&" + t.names[o]
}

func (t *trans) stmts(list []ast.Stmt, indent string, top bool) {
	for i, s := range list {
		t.stmt(s, indent, top && i == len(list)-1)
	}
}

func (t *trans) line(indent, s string) {
	t.b.WriteString(indent + s + "\n")
}

func (t *trans) stmt(s ast.Stmt, indent string, last bool) {
	switch s := s.(type) {
	case *ast.DeclStmt:
		gd, ok := s.Decl.(*ast.GenDecl)
		if !ok || gd.Tok != token.VAR {
			t.fail("declaration")
			return
		}
		for _, sp := range gd.Specs {
			vs := sp.(*ast.ValueSpec)
			if len(vs.Values) != 0 {
				t.fail("var with a value")
				return
			}
			for _, n := range vs.Names {
				t.declare(t.objOf(n))
			}
		}
	case *ast.AssignStmt:
		define := s.Tok == token.DEFINE
		if s.Tok != token.ASSIGN && !define {
			t.fail("assignment %s", s.Tok)
			return
		}
		if len(s.Rhs) == 1 && len(s.Lhs) > 1 {
			call, ok := s.Rhs[0].(*ast.CallExpr)
			if !ok {
				t.fail("tuple assignment")
				return
			}
			if id, ok := call.Fun.(*ast.Ident); ok && id.Name == "BytePtrFromString" && len(s.Lhs) == 2 {
				// A string becomes a C string in a buffer on the stack,
				// or on the heap when it does not fit, freed at the end.
				p := t.expr(s.Lhs[0])
				h := p + "s"
				t.decls = append(t.decls, "burrow__SyscallCString "+h+";")
				t.inits = append(t.inits, h+".heap = NULL;")
				t.cstrs = append(t.cstrs, h)
				t.line(indent, fmt.Sprintf("%s = burrow__syscall_cstring(&%s, %s, %s);", p, h, t.expr(call.Args[0]), t.out(s.Lhs[1], define)))
				return
			}
			var outs []string
			for _, l := range s.Lhs[1:] {
				outs = append(outs, t.out(l, define))
			}
			c := t.call(call, outs)
			if id, ok := s.Lhs[0].(*ast.Ident); ok && id.Name == "_" {
				t.line(indent, c+";")
				return
			}
			first := t.out(s.Lhs[0], define)
			t.line(indent, strings.TrimPrefix(first, "&")+" = "+c+";")
			return
		}
		if len(s.Lhs) != len(s.Rhs) {
			t.fail("assignment")
			return
		}
		for i := range s.Lhs {
			l := strings.TrimPrefix(t.out(s.Lhs[i], define), "&")
			t.line(indent, l+" = "+t.expr(s.Rhs[i])+";")
		}
	case *ast.ExprStmt:
		call, ok := s.X.(*ast.CallExpr)
		if !ok {
			t.fail("expression statement")
			return
		}
		t.line(indent, t.call(call, nil)+";")
	case *ast.IfStmt:
		if s.Init != nil {
			t.fail("if with a statement")
			return
		}
		t.line(indent, "if ("+t.cond(s.Cond)+") {")
		t.stmts(s.Body.List, indent+"    ", false)
		for s.Else != nil {
			switch e := s.Else.(type) {
			case *ast.BlockStmt:
				t.line(indent, "} else {")
				t.stmts(e.List, indent+"    ", false)
				s = &ast.IfStmt{}
			case *ast.IfStmt:
				t.line(indent, "} else if ("+t.cond(e.Cond)+") {")
				t.stmts(e.Body.List, indent+"    ", false)
				s = e
			}
		}
		t.line(indent, "}")
	case *ast.ReturnStmt:
		if len(s.Results) != 0 {
			t.fail("return with results")
			return
		}
		if !last {
			t.usesDone = true
			t.line(indent, "goto done;")
		}
	default:
		t.fail("statement %T", s)
	}
}

// cond writes the condition of an if, without the parentheses a binary
// expression would have twice.
func (t *trans) cond(e ast.Expr) string {
	c := t.expr(e)
	if _, ok := ast.Unparen(e).(*ast.BinaryExpr); ok && strings.HasPrefix(c, "(") && strings.HasSuffix(c, ")") {
		return c[1 : len(c)-1]
	}
	return c
}

// translate makes the C for fd, or says why it can't.
func (g *callGen) translate(fd *ast.FuncDecl) (*cfunc, string) {
	fn, ok := g.p.info.Defs[fd.Name].(*types.Func)
	if !ok {
		return nil, "no object"
	}
	sig := fn.Type().(*types.Signature)
	t := &trans{g: g, names: map[types.Object]string{}, used: map[string]bool{}, libc: map[string]bool{}}

	var params []string
	for i := 0; i < sig.Params().Len(); i++ {
		v := sig.Params().At(i)
		n := paramName(v.Name())
		c, why := g.ctype(v.Type(), n)
		if why != "" {
			return nil, "parameter " + v.Name() + ": " + why
		}
		params = append(params, c)
		t.names[v] = n
		t.used[n] = true
	}

	// The first result that is not the error is what the function returns,
	// and the rest go out through pointers, the error last.
	res := sig.Results()
	var vals []*types.Var
	var errv *types.Var
	for i := 0; i < res.Len(); i++ {
		v := res.At(i)
		if i == res.Len()-1 && types.Identical(v.Type(), errorType) {
			errv = v
			continue
		}
		vals = append(vals, v)
	}
	ret := "void"
	var all []*types.Var
	all = append(all, vals...)
	if errv != nil {
		all = append(all, errv)
	}
	for i, v := range all {
		if v.Name() == "" {
			return nil, "unnamed result"
		}
		local := v.Name() + "_"
		c, why := g.ctype(v.Type(), local)
		if why != "" {
			return nil, "result " + v.Name() + ": " + why
		}
		zero := "0"
		if v == errv {
			zero = "BURROW_NO_ERROR"
		}
		t.decls = append(t.decls, c+" = "+zero+";")
		t.names[v] = local
		t.used[local] = true
		if i == 0 {
			ret, _ = g.castType(v.Type())
			continue
		}
		pc, _ := g.ctype(v.Type(), "*"+paramName(v.Name()))
		params = append(params, pc)
	}
	if len(params) == 0 {
		params = []string{"void"}
	}
	sep := " "
	if strings.HasSuffix(ret, "*") {
		sep = ""
	}
	proto := ret + sep + funcName(fd.Name.Name) + "(" + strings.Join(params, ", ") + ")"

	t.stmts(fd.Body.List, "    ", true)
	if t.why != "" {
		return nil, t.why
	}

	var b strings.Builder
	b.WriteString(proto + " {\n")
	for _, d := range t.decls {
		b.WriteString("    " + d + "\n")
	}
	for _, s := range t.inits {
		b.WriteString("    " + s + "\n")
	}
	b.WriteString(t.b.String())
	if t.usesDone {
		b.WriteString("done:\n")
	}
	for _, h := range t.cstrs {
		b.WriteString("    burrow__syscall_cstring_free(&" + h + ");\n")
	}
	for i, v := range all {
		if i == 0 {
			continue
		}
		fmt.Fprintf(&b, "    BURROW_OUT(%s, %s);\n", paramName(v.Name()), t.names[v])
	}
	if len(all) > 0 {
		fmt.Fprintf(&b, "    return %s;\n", t.names[all[0]])
	}
	b.WriteString("}\n")
	// An Error the caller owns, as the annotations check wants said.
	decl := proto
	switch {
	case ret == "Error", ret == "Str", ret == "Slice", strings.HasSuffix(ret, "*"):
		decl = "BURROW_OWNS(ret) " + proto
	}
	return &cfunc{name: fd.Name.Name, proto: decl, def: b.String(), libc: t.libc}, ""
}

func libcEnum(fn string) string {
	return "BURROW__SYSCALL_" + strings.ToUpper(fn)
}

var importDynamic = regexp.MustCompile(`^//go:cgo_import_dynamic (libc_\w+) (\S+) `)

// callsOf translates the zsyscall functions of one platform, by Go name.
func callsOf(p *platform, hand map[string]bool, handFuncs map[string]bool) (map[string]*cfunc, map[string]string, map[string]string) {
	g := &callGen{
		p:    p,
		tg:   &typeGen{p: p, skip: hand, done: map[string]*ctype{}, working: map[string]bool{}, why: map[string]string{}},
		hand: hand,
		libc: map[string]string{},
		why:  map[string]string{},
	}
	var zfiles []*ast.File
	for _, f := range p.files {
		name := filepath.Base(p.fset.Position(f.Pos()).Filename)
		if !strings.HasPrefix(name, "zsyscall_") {
			continue
		}
		zfiles = append(zfiles, f)
		for _, cg := range f.Comments {
			for _, c := range cg.List {
				if m := importDynamic.FindStringSubmatch(c.Text); m != nil {
					g.libc[m[1]] = m[2]
				}
			}
		}
	}
	out := map[string]*cfunc{}
	for _, f := range zfiles {
		for _, d := range f.Decls {
			fd, ok := d.(*ast.FuncDecl)
			if !ok || fd.Body == nil || fd.Recv != nil {
				continue
			}
			if handFuncs[funcName(fd.Name.Name)] {
				g.why[fd.Name.Name] = "syscall.h has it"
				continue
			}
			c, why := g.translate(fd)
			if why != "" {
				g.why[fd.Name.Name] = why
				continue
			}
			out[fd.Name.Name] = c
		}
	}
	return out, g.why, g.libc
}

// The C names of the functions syscall.h declares by hand.
func handFunctions(path string) map[string]bool {
	data, err := os.ReadFile(path)
	if err != nil {
		die("%v", err)
	}
	out := map[string]bool{}
	re := regexp.MustCompile(`\b(syscall_\w+)\(`)
	for _, m := range re.FindAllSubmatch(data, -1) {
		out[string(m[1])] = true
	}
	return out
}

// callWriter builds the three files one system at a time.
type callWriter struct {
	pub, priv, c bytes.Buffer
	hand         map[string]bool
	handFuncs    map[string]bool
	verbose      bool
	n            int // systems written so far
	count        map[string]int
}

func newCallWriter(root, ver string, verbose bool) *callWriter {
	w := &callWriter{
		hand:      handWritten(filepath.Join(root, "include/burrow/syscall.h")),
		handFuncs: handFunctions(filepath.Join(root, "include/burrow/syscall.h")),
		verbose:   verbose,
		count:     map[string]int{},
	}
	fmt.Fprintf(&w.pub, banner, ver)
	fmt.Fprintf(&w.pub, `
/* syscall's functions for the system this is built for, the ones Go
 * generates from its //sys lines, such as syscall_chdir, syscall_fchmod and
 * syscall_getpid. syscall.h includes this, and says how the rest of the
 * package is spelled.
 *
 * Each is the C name of Go's function, and takes what Go's takes in the same
 * order: a string is a Str, a slice is a Slice, and a pointer to a Go struct
 * is a pointer to the struct in ztypes.h. The first result comes back, the
 * others go out through pointers, which may be NULL, and the error is an Errno
 * made with syscall_errno_as_error, from error_allocator, as the last of them.
 * A function with only an error returns it.
 *
 * Each makes the call the way Go's does on the system and architecture:
 * Linux and FreeBSD by number through syscall(2), macOS through libSystem. On
 * Cosmopolitan and wasip1 they are here, with Linux's names, and fail with
 * ENOSYS. A function only some architectures have is only declared there. */

#ifndef BURROW_SYSCALL_ZSYSCALL_H
#define BURROW_SYSCALL_ZSYSCALL_H

`)
	fmt.Fprintf(&w.priv, banner, ver)
	fmt.Fprintf(&w.priv, `
/* The unexported functions of syscall's zsyscall files, burrow__syscall_ and
 * the Go name, which the hand-written functions call the way Go's do. On
 * macOS, also the numbers of the libSystem functions they reach, which
 * src/syscall/zsyscall.c has the names of. */

#ifndef BURROW_SRC_SYSCALL_ZSYSCALL_H
#define BURROW_SRC_SYSCALL_ZSYSCALL_H

#include "burrow/syscall.h"

`)
	fmt.Fprintf(&w.c, banner, ver)
	fmt.Fprintf(&w.c, `
/* The functions of Go's zsyscall files, translated, for the system this is
 * built for. tools/gen-syscall/calls.go says how. */

#include "burrow/syscall.h"

#include "burrow/runtime.h"

#include "internal.h"

`)
	return w
}

func (w *callWriter) system(goos string, plats []*platform) {
	var sys struct{ goos, cond string }
	for _, s := range callSystems {
		if s.goos == goos {
			sys = s
		}
	}
	if sys.goos == "" {
		return
	}
	all := map[string]map[string]*cfunc{}
	libcs := map[string]map[string]string{}
	names := map[string]bool{}
	for _, p := range plats {
		fs, why, libc := callsOf(p, w.hand, w.handFuncs)
		all[p.goarch] = fs
		libcs[p.goarch] = libc
		for n := range fs {
			names[n] = true
		}
		if w.verbose {
			var left []string
			for n, r := range why {
				left = append(left, n+": "+r)
			}
			sort.Strings(left)
			for _, l := range left {
				fmt.Fprintf(os.Stderr, "gen-syscall: %s/%s: left out func %s\n", p.goos, p.goarch, l)
			}
		}
	}
	var sorted []string
	for n := range names {
		sorted = append(sorted, n)
	}
	sort.Strings(sorted)

	kw := "#if"
	if w.n > 0 {
		kw = "#elif"
	}
	w.n++
	for _, b := range []*bytes.Buffer{&w.pub, &w.priv, &w.c} {
		fmt.Fprintf(b, "%s %s\n\n/* %s */\n\n", kw, sys.cond, sys.goos)
	}

	// The libSystem functions, numbered the same on every architecture.
	used := map[string]bool{}
	for _, p := range plats {
		for _, f := range all[p.goarch] {
			for l := range f.libc {
				used[l] = true
			}
		}
	}
	if len(used) > 0 {
		var ls []string
		for l := range used {
			ls = append(ls, l)
		}
		sort.Strings(ls)
		w.priv.WriteString("enum {\n")
		for _, l := range ls {
			w.priv.WriteString("    " + libcEnum(l) + ",\n")
		}
		w.priv.WriteString("    BURROW__SYSCALL_NLIBC\n};\n\n")
		w.priv.WriteString(`/* A libSystem function: its name, and how many parameters come before the
 * "..." if it is variadic, or -1. */
typedef struct burrow__SyscallLibc {
    const char *name;
    int32_t nfixed;
} burrow__SyscallLibc;

extern const burrow__SyscallLibc burrow__syscall_libc[BURROW__SYSCALL_NLIBC];

/* Where each one is, once it has been looked up, in src/syscall/call.c. */
extern void *burrow__syscall_libc_cache[BURROW__SYSCALL_NLIBC];

`)
		vs, fb := variants(plats, func(p *platform) (string, bool) {
			var b strings.Builder
			b.WriteString("const burrow__SyscallLibc burrow__syscall_libc[BURROW__SYSCALL_NLIBC] = {\n")
			for _, l := range ls {
				sym := libcs[p.goarch][l]
				if sym == "" {
					sym = strings.TrimPrefix(l, "libc_")
				}
				nf := -1
				if v, ok := libcVariadic[sym]; ok {
					nf = v
				}
				fmt.Fprintf(&b, "    [%s] = {%s, %d},\n", libcEnum(l), strconv.Quote(sym), nf)
			}
			b.WriteString("};\n")
			return b.String(), true
		}, fallbackArch[goos])
		writeVariants(&w.c, goos, plats, vs, fb, true)
		w.c.WriteString("\n")
	}

	fallback := fallbackArch[goos]
	for _, n := range sorted {
		hb := &w.pub
		if !ast.IsExported(n) {
			hb = &w.priv
		}
		vs, fb := variants(plats, func(p *platform) (string, bool) {
			f := all[p.goarch][n]
			if f == nil {
				return "", false
			}
			return f.proto + ";\n", true
		}, fallback)
		writeVariants(hb, goos, plats, vs, fb, true)

		vs, fb = variants(plats, func(p *platform) (string, bool) {
			f := all[p.goarch][n]
			if f == nil {
				return "", false
			}
			return f.def, true
		}, fallback)
		var b bytes.Buffer
		writeVariants(&b, goos, plats, vs, fb, true)
		w.c.Write(b.Bytes())
		w.c.WriteString("\n")
		w.count[goos]++
	}
	w.pub.WriteString("\n")
	w.priv.WriteString("\n")
}

func (w *callWriter) finish(root string) {
	if w.n > 0 {
		for _, b := range []*bytes.Buffer{&w.pub, &w.priv, &w.c} {
			b.WriteString("#endif\n")
		}
	}
	w.pub.WriteString("\n#endif /* BURROW_SYSCALL_ZSYSCALL_H */\n")
	w.priv.WriteString("\n#endif /* BURROW_SRC_SYSCALL_ZSYSCALL_H */\n")
	for path, b := range map[string]*bytes.Buffer{
		"include/burrow/syscall/zsyscall.h": &w.pub,
		"src/syscall/zsyscall.h":            &w.priv,
		"src/syscall/zsyscall.c":            &w.c,
	} {
		if err := os.WriteFile(filepath.Join(root, path), b.Bytes(), 0o644); err != nil {
			die("%v", err)
		}
	}
	var keys []string
	for k := range w.count {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		fmt.Fprintf(os.Stderr, "gen-syscall: %s: %d functions\n", k, w.count[k])
	}
}
