//go:build ignore

// The constants: include/burrow/syscall/zconst.h, every constant syscall
// declares other than the E and SIG ones zerrors.h already has, and
// tests/syscall_zconst_check.inc, which compares them with the system's own
// headers.
//
// A constant with the same value on every architecture of a system is
// written once for the system, and the rest go under an #if for the
// architectures that share them.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package main

import (
	"bytes"
	"fmt"
	"go/constant"
	"go/token"
	"go/types"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"unicode"
)

// ------------------------------------------------------------- the values

// The C type a Go basic type is written as.
func cType(t types.Type) string {
	if n, ok := t.(*types.Named); ok && n.Obj().Pkg() != nil && n.Obj().Pkg().Path() == "syscall" {
		switch n.Obj().Name() {
		case "Errno":
			return "SyscallErrno"
		case "Signal":
			return "SyscallSignal"
		}
	}
	b, ok := t.Underlying().(*types.Basic)
	if !ok {
		die("constant of type %s", t)
	}
	switch b.Kind() {
	case types.Int:
		return "Int"
	case types.Int8:
		return "int8_t"
	case types.Int16:
		return "int16_t"
	case types.Int32:
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
	}
	die("constant of type %s", t)
	return ""
}

// A C integer literal for v. Hex, as Go's tables write them, with LL or ULL
// when the value does not fit an int, so it keeps its value and its sign
// whatever int is.
func intLit(v constant.Value) string {
	if constant.Sign(v) < 0 {
		u, ok := constant.Int64Val(constant.UnaryOp(token.SUB, v, 0))
		if !ok {
			die("constant %s too small", v)
		}
		if u > 0x7fffffff {
			return fmt.Sprintf("(-%#xLL)", u)
		}
		return fmt.Sprintf("(-%#x)", u)
	}
	u, ok := constant.Uint64Val(v)
	if !ok {
		die("constant %s too big", v)
	}
	switch {
	case u > 0x7fffffffffffffff:
		return fmt.Sprintf("%#xULL", u)
	case u > 0x7fffffff:
		return fmt.Sprintf("%#xLL", u)
	}
	return fmt.Sprintf("%#x", u)
}

func cString(s string) string {
	var b strings.Builder
	b.WriteString("BURROW_S(\"")
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c >= 0x20 && c < 0x7f:
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteString("\")")
	return b.String()
}

// What a constant is defined as in C.
func cValue(c *types.Const) string {
	v := c.Val()
	switch v.Kind() {
	case constant.Bool:
		return strconv.FormatBool(constant.BoolVal(v))
	case constant.String:
		return cString(constant.StringVal(v))
	case constant.Int:
	default:
		die("%s: constant of kind %s", c.Name(), v.Kind())
	}
	lit := intLit(v)
	if b, ok := c.Type().(*types.Basic); ok && b.Info()&types.IsUntyped != 0 {
		return lit
	}
	return "((" + cType(c.Type()) + ")" + lit + ")"
}

// The exported constants of p, Go name to C value.
func constsOf(p *platform) map[string]string {
	out := map[string]string{}
	scope := p.pkg.Scope()
	for _, name := range scope.Names() {
		c, ok := scope.Lookup(name).(*types.Const)
		if !ok || !c.Exported() {
			continue
		}
		out[name] = cValue(c)
	}
	return out
}

// The SYSCALL_ names the headers already define, which zerrors.h and
// syscall.h have, so they are not written twice.
func defined(paths ...string) map[string]bool {
	re := regexp.MustCompile(`(?m)^#define (SYSCALL_\w+)`)
	out := map[string]bool{}
	for _, p := range paths {
		data, err := os.ReadFile(p)
		if err != nil {
			die("%v", err)
		}
		for _, m := range re.FindAllSubmatch(data, -1) {
			out[string(m[1])] = true
		}
	}
	return out
}

// ------------------------------------------------------------ the output

type archGroup struct {
	archs  []string
	consts map[string]string
}

func (g *archGroup) key() string {
	var b strings.Builder
	for _, k := range sortedNames(g.consts) {
		b.WriteString(k + "=" + g.consts[k] + "\n")
	}
	return b.String()
}

func (g *archGroup) cond() string { return condFor(g.archs) }

func sortedNames(m map[string]string) []string {
	ks := make([]string, 0, len(m))
	for k := range m {
		ks = append(ks, k)
	}
	sort.Slice(ks, func(i, j int) bool { return cName(ks[i]) < cName(ks[j]) })
	return ks
}

func writeDefines(b *bytes.Buffer, consts map[string]string, skip map[string]bool) {
	for _, k := range sortedNames(consts) {
		cn := cName(k)
		if skip[cn] {
			continue
		}
		fmt.Fprintf(b, "#define %s %s\n", cn, consts[k])
	}
}

// constWriter builds zconst.h and the check one system at a time.
type constWriter struct {
	h          bytes.Buffer
	skip       map[string]bool
	checkNames map[string]bool   // every name with no lower case on any system
	names      map[string]string // C name to Go name, to catch two that collide
}

func newConstWriter(root, ver string) *constWriter {
	w := &constWriter{
		skip: defined(filepath.Join(root, "include/burrow/syscall/zerrors.h"),
			filepath.Join(root, "include/burrow/syscall.h")),
		checkNames: map[string]bool{},
		names:      map[string]string{},
	}
	fmt.Fprintf(&w.h, banner, ver)
	fmt.Fprintf(&w.h, `
/* syscall's constants for the system this is built for, other than the E and
 * SIG ones in zerrors.h: the address families, the open and socket flags, the
 * ioctl requests, the SYS_ system call numbers, the Sizeof values of the
 * structs and the rest, with the system's own values, which Go took from its
 * headers. A Linux architecture Go has no table for, Cosmopolitan and wasip1
 * get the values of linux/arm64.
 *
 * Go's names with no lower case are kept as they are after the SYSCALL_, and
 * the others are split into words like every other name in burrow, so
 * SizeofSockaddrInet4 is SYSCALL_SIZEOF_SOCKADDR_INET4. A constant Go gives a
 * type has that type here too. */

#ifndef BURROW_SYSCALL_ZCONST_H
#define BURROW_SYSCALL_ZCONST_H

`)
	return w
}

// system writes the branch for systems[si], whose architectures are plats.
func (w *constWriter) system(si int, plats []*platform) {
	sys := systems[si]
	all := make([]map[string]string, len(plats))
	for i, p := range plats {
		all[i] = constsOf(p)
		for k := range all[i] {
			cn := cName(k)
			if other, ok := w.names[cn]; ok && other != k {
				die("%s and %s are both %s", other, k, cn)
			}
			w.names[cn] = k
			if k == strings.ToUpper(k) && !w.skip[cn] {
				w.checkNames[k] = true
			}
		}
	}

	// The constants every architecture has with the same value.
	common := map[string]string{}
	for k, v := range all[0] {
		same := true
		for _, c := range all[1:] {
			if cv, ok := c[k]; !ok || cv != v {
				same = false
				break
			}
		}
		if same {
			common[k] = v
		}
	}

	// The rest, grouped by architectures that agree on all of it.
	var groups []*archGroup
	byKey := map[string]*archGroup{}
	var fb *archGroup
	for i, p := range plats {
		rest := map[string]string{}
		for k, v := range all[i] {
			if _, ok := common[k]; !ok {
				rest[k] = v
			}
		}
		g := &archGroup{consts: rest}
		k := g.key()
		if old := byKey[k]; old != nil {
			g = old
		} else {
			byKey[k] = g
			groups = append(groups, g)
		}
		g.archs = append(g.archs, p.goarch)
		if fallbackArch[sys.goos] == p.goarch {
			fb = g
		}
	}
	// Two architectures under one condition have to agree.
	condGroup := map[string]*archGroup{}
	for _, g := range groups {
		for _, a := range g.archs {
			c := archCond(a)
			if o := condGroup[c]; o != nil && o != g {
				die("%s: the architectures under %s disagree", sys.goos, c)
			}
			condGroup[c] = g
		}
	}

	h := &w.h
	switch {
	case si == 0:
		fmt.Fprintf(h, "#if %s\n", sys.cond)
	case si == len(systems)-1:
		fmt.Fprintf(h, "#else\n")
	default:
		fmt.Fprintf(h, "#elif %s\n", sys.cond)
	}
	what := sys.goos
	if si == len(systems)-1 {
		what += ", and everything not above"
	}
	fmt.Fprintf(h, "\n/* %s */\n", what)
	writeDefines(h, common, w.skip)

	if len(groups) > 1 || (len(groups) == 1 && len(groups[0].consts) > 0) {
		var order []*archGroup
		for _, g := range groups {
			if g != fb {
				order = append(order, g)
			}
		}
		if fb != nil {
			order = append(order, fb)
		}
		for i, g := range order {
			var labels []string
			for _, a := range g.archs {
				labels = append(labels, sys.goos+"/"+a)
			}
			switch {
			case i == 0:
				fmt.Fprintf(h, "\n#if %s\n", g.cond())
			case g == fb:
				fmt.Fprintf(h, "#else\n")
			default:
				fmt.Fprintf(h, "#elif %s\n", g.cond())
			}
			label := strings.Join(labels, ", ")
			if g == fb {
				label += ", and any other architecture"
			}
			fmt.Fprintf(h, "\n/* %s */\n", label)
			writeDefines(h, g.consts, w.skip)
			fmt.Fprintf(h, "\n")
		}
		fmt.Fprintf(h, "#endif\n")
	}
	fmt.Fprintf(h, "\n")
}

// finish writes the header to path and the check to tests/ under root.
func (w *constWriter) finish(root, ver, path string) {
	fmt.Fprintf(&w.h, "#endif\n\n#endif /* BURROW_SYSCALL_ZCONST_H */\n")
	if err := os.WriteFile(path, w.h.Bytes(), 0o644); err != nil {
		die("%v", err)
	}

	// The check: each name is compared with the system's macro of the same
	// name, where the system has one and so does burrow for this system.
	var c bytes.Buffer
	fmt.Fprintf(&c, banner, ver)
	fmt.Fprintf(&c, `
/* Included by tests/syscall_zconst_test.c inside a function. CHECK_CONST(n)
 * compares SYSCALL_n with the system's n. */
`)
	var cn []string
	for k := range w.checkNames {
		cn = append(cn, k)
	}
	sort.Strings(cn)
	for _, k := range cn {
		if !unicode.IsUpper(rune(k[0])) {
			continue
		}
		fmt.Fprintf(&c, "#if defined(%s) && defined(SYSCALL_%s)\nCHECK_CONST(%s);\n#endif\n", k, k, k)
	}
	check := filepath.Join(root, "tests/syscall_zconst_check.inc")
	if err := os.WriteFile(check, c.Bytes(), 0o644); err != nil {
		die("%v", err)
	}
}
