// Command errors writes include/burrow/syscall/zerrors.h and
// src/syscall/zerrors.c from Go's own syscall tables: the E constants and the
// message table behind Errno.Error, for every system and architecture burrow
// builds for. tools/gen-syscall-errors.sh runs it.
//
// It reads the zerrors_GOOS_GOARCH.go files that Go generates from the system
// headers with mkerrors.sh, plus types_windows.go for the ERROR_ codes, and
// parses them rather than importing them, since one program can only import
// syscall for the system it runs on. Architectures whose constants and
// messages are the same are written once, under one #if.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.
package main

import (
	"bytes"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// The Go architectures burrow has a macro for. mips and mipsle are not here,
// and the others that are missing have no zerrors file.
var archMacro = map[string]string{
	"386":      "BURROW_ARCH_386",
	"amd64":    "BURROW_ARCH_AMD64",
	"arm":      "BURROW_ARCH_ARM",
	"arm64":    "BURROW_ARCH_ARM64",
	"loong64":  "BURROW_ARCH_LOONG64",
	"mips64":   "BURROW_ARCH_MIPS64",
	"mips64le": "BURROW_ARCH_MIPS64",
	"ppc64":    "BURROW_ARCH_PPC64",
	"ppc64le":  "BURROW_ARCH_PPC64",
	"riscv64":  "BURROW_ARCH_RISCV64",
	"s390x":    "BURROW_ARCH_S390X",
}

// The systems, in the order their #if branches are written, and the macros
// that pick each one.
var systems = []struct{ goos, cond string }{
	{"windows", "defined(BURROW_OS_WINDOWS)"},
	{"darwin", "defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)"},
	{"freebsd", "defined(BURROW_OS_FREEBSD)"},
	{"netbsd", "defined(BURROW_OS_NETBSD)"},
	{"openbsd", "defined(BURROW_OS_OPENBSD)"},
	{"dragonfly", "defined(BURROW_OS_DRAGONFLY)"},
	{"solaris", "defined(BURROW_OS_SOLARIS)"},
	{"aix", "defined(BURROW_OS_AIX)"},
	{"linux", "defined(BURROW_OS_LINUX)"},
}

// linux/arm64 is the asm-generic numbering, which is the one every newer Linux
// port uses, so it is what a Linux architecture with no table of its own gets,
// and what Cosmopolitan and wasip1 get as well.
const fallback = "linux/arm64"

type constant struct {
	name  string
	value uint64
}

type table struct {
	consts []constant
	errors map[uint64]string
	base   uint64 // what errors is indexed from, APPLICATION_ERROR on Windows
}

func (t *table) key() string {
	var b strings.Builder
	for _, c := range t.consts {
		fmt.Fprintf(&b, "%s=%d;", c.name, c.value)
	}
	for _, k := range sortedKeys(t.errors) {
		fmt.Fprintf(&b, "%d:%q;", k, t.errors[k])
	}
	fmt.Fprintf(&b, "base=%d", t.base)
	return b.String()
}

func sortedKeys(m map[uint64]string) []uint64 {
	var ks []uint64
	for k := range m {
		ks = append(ks, k)
	}
	sort.Slice(ks, func(i, j int) bool { return ks[i] < ks[j] })
	return ks
}

func die(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "gen-syscall: "+format+"\n", args...)
	os.Exit(1)
}

func parse(path string) *ast.File {
	f, err := parser.ParseFile(token.NewFileSet(), path, nil, 0)
	if err != nil {
		die("%v", err)
	}
	return f
}

func intLit(e ast.Expr) (uint64, bool) {
	lit, ok := e.(*ast.BasicLit)
	if !ok || lit.Kind != token.INT {
		return 0, false
	}
	v, err := strconv.ParseUint(lit.Value, 0, 64)
	if err != nil {
		die("%v", err)
	}
	return v, true
}

func strLit(e ast.Expr) string {
	lit, ok := e.(*ast.BasicLit)
	if !ok || lit.Kind != token.STRING {
		die("an errors entry that is not a string")
	}
	s, err := strconv.Unquote(lit.Value)
	if err != nil {
		die("%v", err)
	}
	return s
}

func errorsTable(f *ast.File) []*ast.KeyValueExpr {
	var out []*ast.KeyValueExpr
	ast.Inspect(f, func(n ast.Node) bool {
		vs, ok := n.(*ast.ValueSpec)
		if !ok || len(vs.Names) != 1 || vs.Names[0].Name != "errors" {
			return true
		}
		for _, e := range vs.Values[0].(*ast.CompositeLit).Elts {
			out = append(out, e.(*ast.KeyValueExpr))
		}
		return false
	})
	if out == nil {
		die("no errors table")
	}
	return out
}

// A Unix zerrors file: E constants written as Errno(0x2), and an errors table
// keyed by number.
func unixTable(path string) *table {
	f := parse(path)
	t := &table{errors: map[uint64]string{}}
	seen := map[string]bool{}
	ast.Inspect(f, func(n ast.Node) bool {
		vs, ok := n.(*ast.ValueSpec)
		if !ok || len(vs.Names) != 1 || len(vs.Values) != 1 {
			return true
		}
		name := vs.Names[0].Name
		call, ok := vs.Values[0].(*ast.CallExpr)
		if !ok || len(call.Args) != 1 || !strings.HasPrefix(name, "E") {
			return true
		}
		if fn, ok := call.Fun.(*ast.Ident); !ok || fn.Name != "Errno" {
			return true
		}
		v, ok := intLit(call.Args[0])
		if !ok {
			die("%s: %s is not a number", path, name)
		}
		if seen[name] {
			die("%s: %s twice", path, name)
		}
		seen[name] = true
		t.consts = append(t.consts, constant{name, v})
		return true
	})
	for _, kv := range errorsTable(f) {
		k, ok := intLit(kv.Key)
		if !ok {
			die("%s: an errors key that is not a number", path)
		}
		t.errors[k] = strLit(kv.Value)
	}
	sort.Slice(t.consts, func(i, j int) bool { return t.consts[i].name < t.consts[j].name })
	return t
}

// Windows: the ERROR_ and WSA codes from types_windows.go, ENOENT and ENOTDIR
// as aliases of two of them, and the invented E constants, which count up from
// APPLICATION_ERROR with iota and index the errors table from there.
func windowsTable(dir string) *table {
	t := &table{errors: map[uint64]string{}}
	codes := map[string]uint64{}
	for _, d := range parse(filepath.Join(dir, "types_windows.go")).Decls {
		g, ok := d.(*ast.GenDecl)
		if !ok || g.Tok != token.CONST {
			continue
		}
		for _, s := range g.Specs {
			vs := s.(*ast.ValueSpec)
			if len(vs.Names) != 1 || len(vs.Values) != 1 || vs.Type == nil {
				continue
			}
			name := vs.Names[0].Name
			if id, ok := vs.Type.(*ast.Ident); !ok || id.Name != "Errno" || !ast.IsExported(name) {
				continue
			}
			v, ok := intLit(vs.Values[0])
			if !ok {
				die("types_windows.go: %s is not a number", name)
			}
			codes[name] = v
			t.consts = append(t.consts, constant{name, v})
		}
	}

	f := parse(filepath.Join(dir, "zerrors_windows.go"))
	var appErr uint64
	for _, d := range f.Decls {
		g, ok := d.(*ast.GenDecl)
		if !ok || g.Tok != token.CONST {
			continue
		}
		counting := false
		for i, s := range g.Specs {
			vs := s.(*ast.ValueSpec)
			name := vs.Names[0].Name
			switch {
			case name == "APPLICATION_ERROR":
				appErr = 1 << 29
				if b, ok := vs.Values[0].(*ast.BinaryExpr); !ok || b.Op != token.SHL {
					die("APPLICATION_ERROR is not 1 << 29 any more")
				}
			case len(vs.Values) == 1:
				if id, ok := vs.Values[0].(*ast.Ident); ok {
					v, ok := codes[id.Name]
					if !ok {
						die("%s is %s, which types_windows.go does not have", name, id.Name)
					}
					t.consts = append(t.consts, constant{name, v})
					continue
				}
				if _, ok := vs.Values[0].(*ast.BinaryExpr); !ok || i != 0 {
					die("%s is not the first of an iota block", name)
				}
				counting = true
				fallthrough
			default:
				if !counting || appErr == 0 {
					die("%s has no value", name)
				}
				t.consts = append(t.consts, constant{name, appErr + uint64(i)})
			}
		}
	}
	byName := map[string]uint64{}
	for _, c := range t.consts {
		byName[c.name] = c.value
	}
	t.base = appErr
	for _, kv := range errorsTable(f) {
		b, ok := kv.Key.(*ast.BinaryExpr)
		if !ok || b.Op != token.SUB {
			die("zerrors_windows.go: an errors key that is not NAME - APPLICATION_ERROR")
		}
		v, ok := byName[b.X.(*ast.Ident).Name]
		if !ok {
			die("zerrors_windows.go: no constant %s", b.X.(*ast.Ident).Name)
		}
		t.errors[v-appErr] = strLit(kv.Value)
	}
	sort.Slice(t.consts, func(i, j int) bool { return t.consts[i].name < t.consts[j].name })
	return t
}

type group struct {
	goos  string
	archs []string
	t     *table
}

func (g *group) cond(sysCond string) string {
	if g.archs == nil {
		return sysCond
	}
	seen := map[string]bool{}
	var ms []string
	for _, a := range g.archs {
		m := archMacro[a]
		if !seen[m] {
			seen[m] = true
			ms = append(ms, "defined("+m+")")
		}
	}
	return "(" + sysCond + ") && (" + strings.Join(ms, " || ") + ")"
}

func main() {
	if len(os.Args) != 5 {
		die("usage: errors <goroot> <go version> <header> <source>")
	}
	dir := filepath.Join(os.Args[1], "src", "syscall")

	var groups []*group
	var fb *group
	for _, sys := range systems {
		if sys.goos == "windows" {
			groups = append(groups, &group{goos: "windows", t: windowsTable(dir)})
			continue
		}
		files, _ := filepath.Glob(filepath.Join(dir, "zerrors_"+sys.goos+"_*.go"))
		byKey := map[string]*group{}
		var mine []*group
		for _, path := range files {
			arch := strings.TrimSuffix(strings.TrimPrefix(filepath.Base(path), "zerrors_"+sys.goos+"_"), ".go")
			if archMacro[arch] == "" {
				continue
			}
			t := unixTable(path)
			k := t.key()
			g := byKey[k]
			if g == nil {
				g = &group{goos: sys.goos, t: t}
				byKey[k] = g
				mine = append(mine, g)
			}
			g.archs = append(g.archs, arch)
			if sys.goos+"/"+arch == fallback {
				fb = g
			}
		}
		if len(mine) == 0 {
			die("no zerrors files for %s", sys.goos)
		}
		// One table for the whole system needs no architecture test, and the
		// fallback is written last, as the #else.
		if len(mine) == 1 {
			mine[0].archs = nil
		}
		for _, g := range mine {
			if g != fb {
				groups = append(groups, g)
			}
		}
	}
	if fb == nil {
		die("no %s table", fallback)
	}

	sysCond := map[string]string{}
	for _, s := range systems {
		sysCond[s.goos] = s.cond
	}
	label := func(g *group) string {
		if g.archs == nil {
			return g.goos
		}
		var as []string
		for _, a := range g.archs {
			as = append(as, g.goos+"/"+a)
		}
		return strings.Join(as, ", ")
	}

	var h, c bytes.Buffer
	ver := os.Args[2]
	fmt.Fprintf(&h, `/* Derived from Go's src/syscall/zerrors_linux_amd64.go.
 * Go source: %s.
 *
 * The other systems come from their own zerrors files in the same place, and
 * the Windows ERROR_ and WSA codes from types_windows.go.
 *
 * Generated by tools/gen-syscall-errors.sh from the Go tables. Do not edit.
 *
 * syscall's E constants for the system this is built for, and on Windows its
 * ERROR_ and WSA codes too. The numbers are the system's own, so they differ
 * from one system to the next and, on Linux, between some architectures. A
 * Linux architecture Go has no table for, Cosmopolitan and wasip1 get the
 * numbers of linux/arm64, which are the generic ones.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SYSCALL_ZERRORS_H
#define BURROW_SYSCALL_ZERRORS_H

`, ver)
	fmt.Fprintf(&c, `/* Derived from Go's src/syscall/zerrors_linux_amd64.go.
 * Go source: %s.
 *
 * The other systems come from their own zerrors files in the same place, and
 * Windows from zerrors_windows.go, which has Go's own codes.
 *
 * Generated by tools/gen-syscall-errors.sh from the Go tables. Do not edit.
 *
 * The messages behind syscall.Errno.Error, indexed by number, from
 * burrow__syscall_errors_base on. That is APPLICATION_ERROR on Windows, where
 * only Go's invented codes have a message here and the rest come from the
 * system.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#include "internal.h"

`, ver)

	all := append(groups, fb)
	for i, g := range all {
		var line string
		switch {
		case i == 0:
			line = "#if " + g.cond(sysCond[g.goos])
		case g == fb:
			line = "#else"
		default:
			line = "#elif " + g.cond(sysCond[g.goos])
		}
		what := label(g)
		if g == fb {
			what += ", and everything not above"
		}
		fmt.Fprintf(&h, "%s\n\n/* %s */\n", line, what)
		fmt.Fprintf(&c, "%s\n\n/* %s */\n", line, what)
		for _, k := range g.t.consts {
			fmt.Fprintf(&h, "#define SYSCALL_%s ((SyscallErrno)%#x)\n", k.name, k.value)
		}
		fmt.Fprintf(&h, "\n")
		ks := sortedKeys(g.t.errors)
		fmt.Fprintf(&c, "const Uintptr burrow__syscall_errors_base = %#x;\n", g.t.base)
		fmt.Fprintf(&c, "const char *const burrow__syscall_errors[] = {\n")
		for _, k := range ks {
			fmt.Fprintf(&c, "    [%d] = %s,\n", k, strconv.Quote(g.t.errors[k]))
		}
		fmt.Fprintf(&c, "};\n")
		fmt.Fprintf(&c, "const Int burrow__syscall_nerrors = %d;\n\n", ks[len(ks)-1]+1)
	}
	fmt.Fprintf(&h, "#endif\n\n#endif /* BURROW_SYSCALL_ZERRORS_H */\n")
	fmt.Fprintf(&c, "#endif\n")

	if err := os.WriteFile(os.Args[3], h.Bytes(), 0o644); err != nil {
		die("%v", err)
	}
	if err := os.WriteFile(os.Args[4], c.Bytes(), 0o644); err != nil {
		die("%v", err)
	}
}
