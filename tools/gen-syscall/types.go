//go:build ignore

// The types: include/burrow/syscall/ztypes.h, the structs and the other named
// types syscall declares, laid out the way Go lays them out for each system
// and architecture, and tests/syscall_ztypes_check.inc, which checks that the
// C compiler puts every field where Go does.
//
// Most of these come from Go's ztypes files, which cgo -godefs made from the
// system's own headers, with the padding the system's C compiler adds written
// out as fields. So the C structs here are the system's structs field for
// field. A type with a field C can't spell the way Go does, a string, a slice,
// a func, a map, a channel or an interface, is left out, as is one burrow.h
// writes by hand.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package main

import (
	"bytes"
	"fmt"
	"go/types"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

// Hand-written types a generated one may use, which syscall.h declares before
// it includes ztypes.h.
var earlyTypes = map[string]bool{"Errno": true, "Signal": true}

// Words C and C++ keep for themselves, and lower case macros the system
// headers define, which a field can't be called. Such a field gets an
// underscore after its name.
var reservedField = map[string]bool{}

func init() {
	for _, w := range strings.Fields(`auto break case char const continue default do double
		else enum extern float for goto if inline int long register restrict return short
		signed sizeof static struct switch typedef union unsigned void volatile while bool
		true false alignas alignof and asm catch class compl delete explicit export friend
		mutable namespace new noexcept not operator or private protected public template
		this throw try typeid typename using virtual xor
		errno major minor makedev unix linux interface small near far min max`) {
		reservedField[w] = true
	}
}

// One generated type on one platform.
type ctype struct {
	name     string          // the Go name
	isStruct bool            // a struct, which gets a forward typedef
	def      string          // the C definition
	deps     map[string]bool // the Go names that have to be defined first
	check    string          // the size and offset checks
}

// typeGen turns the types of one platform into C.
type typeGen struct {
	p       *platform
	skip    map[string]bool   // Go names syscall.h writes by hand
	done    map[string]*ctype // nil for a type that can't be written
	working map[string]bool   // the types being worked out, for a pointer back to one
	why     map[string]string // why a type was left out, for -v
}

func basicC(b *types.Basic) string {
	switch b.Kind() {
	case types.Bool:
		return "bool"
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
	case types.Float32:
		return "float"
	case types.Float64:
		return "double"
	case types.UnsafePointer:
		return "void *"
	}
	return ""
}

// An empty struct with no name, which a pointer to is a void *.
func isEmptyStruct(t types.Type) bool {
	s, ok := types.Unalias(t).(*types.Struct)
	return ok && s.NumFields() == 0
}

// decl writes a C declaration of inner with type t, as in "uint8_t
// inner[4]". ptr says inner is a pointer, so a struct it points at only has
// to be declared, not defined. Whatever has to be defined first goes in deps.
// The second result is false, with the reason, for a type C can't spell.
func (g *typeGen) decl(t types.Type, inner string, ptr bool, deps map[string]bool, indent string) (string, string) {
	t = types.Unalias(t)
	switch t := t.(type) {
	case *types.Basic:
		c := basicC(t)
		if c == "" {
			return "", t.String()
		}
		if strings.HasSuffix(c, "*") {
			return c + inner, ""
		}
		return c + " " + inner, ""
	case *types.Named:
		obj := t.Obj()
		if obj.Pkg() == nil {
			return "", obj.Name()
		}
		if obj.Pkg().Path() != "syscall" {
			return "", obj.Pkg().Path() + "." + obj.Name()
		}
		name := obj.Name()
		if !obj.Exported() {
			// An unexported type is written as what it is.
			if _, ok := t.Underlying().(*types.Struct); ok {
				return "", "unexported struct " + name
			}
			return g.decl(t.Underlying(), inner, ptr, deps, indent)
		}
		if earlyTypes[name] {
			return "Syscall" + name + " " + inner, ""
		}
		if g.skip[name] {
			return "", "hand-written " + name
		}
		_, isStruct := t.Underlying().(*types.Struct)
		if !(ptr && isStruct && g.working[name]) {
			if c := g.typeOf(name); c == nil {
				return "", "uses " + name
			}
		}
		if !ptr || !isStruct {
			deps[name] = true
		}
		return "Syscall" + name + " " + inner, ""
	case *types.Array:
		if t.Len() == 0 {
			return "", "zero length array"
		}
		if strings.HasPrefix(inner, "*") {
			inner = "(" + inner + ")"
		}
		return g.decl(t.Elem(), fmt.Sprintf("%s[%d]", inner, t.Len()), false, deps, indent)
	case *types.Pointer:
		if isEmptyStruct(t.Elem()) {
			return "void *" + inner, ""
		}
		return g.decl(t.Elem(), "*"+inner, true, deps, indent)
	case *types.Struct:
		body, why := g.fields(t, indent+"    ", deps, nil)
		if why != "" {
			return "", why
		}
		return "struct {\n" + body + indent + "} " + inner, ""
	}
	return "", t.String()
}

// The C name of field i of s.
func fieldName(f *types.Var, blank *int) string {
	if f.Name() == "_" {
		*blank++
		return fmt.Sprintf("pad_%d", *blank-1)
	}
	n := strings.ToLower(strings.Join(words(f.Name()), "_"))
	if reservedField[n] {
		n += "_"
	}
	return n
}

// fields writes the fields of s, one to a line. names, when it is not nil,
// gets the C name of each field, or "" for one that is left out.
func (g *typeGen) fields(s *types.Struct, indent string, deps map[string]bool, names *[]string) (string, string) {
	var b strings.Builder
	blank := 0
	seen := map[string]bool{}
	for i := 0; i < s.NumFields(); i++ {
		f := s.Field(i)
		if g.p.sizes.Sizeof(f.Type()) == 0 {
			// C has no zero sized field. Go uses them to mark a struct, and
			// one at the end makes Go pad the struct, which the check finds.
			// The exception is an array of nothing at the end of a type of
			// its own, such as InotifyEvent's Name, where the bytes that
			// follow the struct start. That is C's flexible array member.
			if i == s.NumFields()-1 {
				if names == nil || flexibleLast(s) == nil {
					return "", "zero sized last field " + f.Name()
				}
				n := fieldName(f, &blank)
				if seen[n] {
					return "", "two fields are " + n
				}
				seen[n] = true
				d, why := g.decl(flexibleLast(s).Elem(), n+"[]", false, deps, indent)
				if why != "" {
					return "", "field " + f.Name() + ": " + why
				}
				b.WriteString(indent + d + ";\n")
				*names = append(*names, n)
				continue
			}
			if names != nil {
				*names = append(*names, "")
			}
			continue
		}
		n := fieldName(f, &blank)
		if seen[n] {
			return "", "two fields are " + n
		}
		seen[n] = true
		d, why := g.decl(f.Type(), n, false, deps, indent)
		if why != "" {
			return "", "field " + f.Name() + ": " + why
		}
		b.WriteString(indent + d + ";\n")
		if names != nil {
			*names = append(*names, n)
		}
	}
	if len(seen) == 0 {
		return "", "no fields"
	}
	return b.String(), ""
}

// flexibleLast is the last field of s when it is an array with no elements
// that comes after other fields, and nil otherwise.
func flexibleLast(s *types.Struct) *types.Array {
	if s.NumFields() < 2 {
		return nil
	}
	a, ok := s.Field(s.NumFields() - 1).Type().Underlying().(*types.Array)
	if !ok || a.Len() != 0 {
		return nil
	}
	return a
}

// typeOf works out the C for the exported type name, or nil if it can't be
// written.
func (g *typeGen) typeOf(name string) *ctype {
	if c, ok := g.done[name]; ok {
		return c
	}
	tn, ok := g.p.pkg.Scope().Lookup(name).(*types.TypeName)
	if !ok || tn.IsAlias() {
		g.done[name] = nil
		return nil
	}
	g.working[name] = true
	defer delete(g.working, name)
	cn := "Syscall" + name
	c := &ctype{name: name, deps: map[string]bool{}}
	var why string
	var check strings.Builder
	t := tn.Type()
	if s, ok := t.Underlying().(*types.Struct); ok && s.NumFields() == 0 {
		// Go uses an empty struct, such as SID, for something only the
		// system knows the layout of and a pointer is all anyone has, which
		// C says with a struct that is declared and never defined.
		c.isStruct = true
	} else if ok {
		c.isStruct = true
		var names []string
		var body string
		body, why = g.fields(s, "    ", c.deps, &names)
		if why == "" {
			c.def = "struct " + cn + " {\n" + body + "};\n"
			var vars []*types.Var
			for i := 0; i < s.NumFields(); i++ {
				vars = append(vars, s.Field(i))
			}
			offs := g.p.sizes.Offsetsof(vars)
			size := g.p.sizes.Sizeof(t)
			if flexibleLast(s) != nil {
				// Go pads the struct so a pointer to the empty array does
				// not point past it, and C does not.
				al := g.p.sizes.Alignof(t)
				size = (offs[len(offs)-1] + al - 1) / al * al
			}
			fmt.Fprintf(&check, "CHECK_SIZE(%s, %d);\n", cn, size)
			for i, n := range names {
				if n != "" {
					fmt.Fprintf(&check, "CHECK_OFFSET(%s, %s, %d);\n", cn, n, offs[i])
				}
			}
		}
	} else {
		var d string
		d, why = g.decl(t.Underlying(), cn, false, c.deps, "")
		if why == "" {
			c.def = "typedef " + d + ";\n"
			fmt.Fprintf(&check, "CHECK_SIZE(%s, %d);\n", cn, g.p.sizes.Sizeof(t))
		}
	}
	delete(c.deps, name)
	if why != "" {
		g.why[name] = why
		g.done[name] = nil
		return nil
	}
	c.check = check.String()
	g.done[name] = c
	return c
}

// The types of one platform that can be written, by Go name.
func typesOf(p *platform, skip map[string]bool) (map[string]*ctype, map[string]string) {
	g := &typeGen{p: p, skip: skip, done: map[string]*ctype{}, working: map[string]bool{}, why: map[string]string{}}
	out := map[string]*ctype{}
	scope := p.pkg.Scope()
	for _, name := range scope.Names() {
		tn, ok := scope.Lookup(name).(*types.TypeName)
		if !ok || !tn.Exported() || earlyTypes[name] || skip[name] {
			continue
		}
		switch tn.Type().Underlying().(type) {
		case *types.Interface, *types.Signature:
			continue
		}
		if c := g.typeOf(name); c != nil {
			out[name] = c
		}
	}
	return out, g.why
}

// The Go names of the types syscall.h writes by hand.
func handWritten(path string) map[string]bool {
	data, err := os.ReadFile(path)
	if err != nil {
		die("%v", err)
	}
	out := map[string]bool{}
	re := regexp.MustCompile(`(?m)^(?:typedef [^;{]*\b|\} )Syscall(\w+);`)
	for _, m := range re.FindAllSubmatch(data, -1) {
		out[string(m[1])] = true
	}
	return out
}

// typeWriter builds ztypes.h and the check one system at a time.
type typeWriter struct {
	h, c    bytes.Buffer
	skip    map[string]bool
	verbose bool
}

func newTypeWriter(root, ver string, verbose bool) *typeWriter {
	w := &typeWriter{skip: handWritten(filepath.Join(root, "include/burrow/syscall.h")), verbose: verbose}
	fmt.Fprintf(&w.h, banner, ver)
	fmt.Fprintf(&w.h, `
/* syscall's types for the system this is built for: the structs the system
 * calls take, such as SyscallStat_t, SyscallRawSockaddrInet6 and
 * SyscallTimespec, and the other named types, such as SyscallHandle on
 * Windows. Each is laid out the way Go lays it out on that system and
 * architecture, which is the way the system's own C compiler does, padding
 * included. A Linux architecture Go has no table for, Cosmopolitan and wasip1
 * get the types of linux/arm64.
 *
 * A type is SyscallX for Go's X, and a field is its Go name in lower case
 * with an underscore between the words, so Stat_t's Blksize is blksize. A
 * field Go calls _ is pad_0, pad_1 and so on, and one that is a C keyword has
 * an underscore after it. The types Go writes with a string, a slice, a func
 * or an interface in them are not here. */

#ifndef BURROW_SYSCALL_ZTYPES_H
#define BURROW_SYSCALL_ZTYPES_H

`)
	fmt.Fprintf(&w.c, banner, ver)
	fmt.Fprintf(&w.c, `
/* Included by tests/syscall_ztypes_test.c inside a function. CHECK_SIZE(T, n)
 * checks that sizeof(T) is n, and CHECK_OFFSET(T, f, n) that field f of T is
 * at offset n, the numbers Go gives for the architecture. */
`)
	return w
}

// A variant of a type: the architectures that have the same text for it.
type variant struct {
	archs []string
	text  string
}

// variants groups the architectures by text, with the fallback's group last.
// fb is false when the fallback architecture is not in any of them.
func variants(plats []*platform, text func(p *platform) (string, bool), fallback string) (vs []*variant, fb bool) {
	byText := map[string]*variant{}
	var fbv *variant
	for _, p := range plats {
		t, ok := text(p)
		if !ok {
			continue
		}
		v := byText[t]
		if v == nil {
			v = &variant{text: t}
			byText[t] = v
			vs = append(vs, v)
		}
		v.archs = append(v.archs, p.goarch)
		if p.goarch == fallback {
			fbv = v
		}
	}
	if fbv != nil {
		var out []*variant
		for _, v := range vs {
			if v != fbv {
				out = append(out, v)
			}
		}
		vs = append(out, fbv)
	}
	return vs, fbv != nil
}

// writeVariants writes text that differs between architectures under #if,
// and text every architecture has the same without one. With useElse, the
// fallback architecture's text is the #else, and the architectures that don't
// have the type at all get an empty branch of their own before it, so an
// architecture that is not in the list is the only other one to get it.
func writeVariants(b *bytes.Buffer, goos string, plats []*platform, vs []*variant, fb, useElse bool) {
	if len(vs) == 0 {
		return
	}
	if len(vs) == 1 && len(vs[0].archs) == len(plats) {
		b.WriteString(vs[0].text)
		return
	}
	if fb && useElse {
		has := map[string]bool{}
		for _, v := range vs {
			for _, a := range v.archs {
				has[a] = true
			}
		}
		var missing []string
		for _, p := range plats {
			if !has[p.goarch] {
				missing = append(missing, p.goarch)
			}
		}
		if len(missing) > 0 {
			last := vs[len(vs)-1]
			vs = append(append(vs[:len(vs)-1:len(vs)-1], &variant{archs: missing}), last)
		}
	}
	// Two architectures under one condition have to agree.
	seen := map[string]*variant{}
	for _, v := range vs {
		for _, a := range v.archs {
			c := archCond(a)
			if o := seen[c]; o != nil && o != v {
				die("%s: the architectures under %s disagree", goos, c)
			}
			seen[c] = v
		}
	}
	for i, v := range vs {
		switch {
		case i == 0:
			fmt.Fprintf(b, "#if %s\n", condFor(v.archs))
		case i == len(vs)-1 && fb && useElse:
			b.WriteString("#else\n")
		default:
			fmt.Fprintf(b, "#elif %s\n", condFor(v.archs))
		}
		b.WriteString(v.text)
	}
	b.WriteString("#endif\n")
}

// system writes the branch for systems[si], whose architectures are plats.
func (w *typeWriter) system(si int, plats []*platform) {
	sys := systems[si]
	all := map[string]map[string]*ctype{} // arch to Go name to type
	names := map[string]bool{}
	for _, p := range plats {
		ts, why := typesOf(p, w.skip)
		all[p.goarch] = ts
		for n := range ts {
			names[n] = true
		}
		if w.verbose {
			var left []string
			for n, r := range why {
				left = append(left, n+": "+r)
			}
			sort.Strings(left)
			for _, l := range left {
				fmt.Fprintf(os.Stderr, "gen-syscall: %s/%s: left out %s\n", p.goos, p.goarch, l)
			}
		}
	}

	// The order to define them in: each after the ones it needs, on any
	// architecture.
	var sorted []string
	for n := range names {
		sorted = append(sorted, n)
	}
	sort.Strings(sorted)
	var order []string
	state := map[string]int{}
	var visit func(n string)
	visit = func(n string) {
		switch state[n] {
		case 1:
			die("%s: %s needs itself", sys.goos, n)
		case 2:
			return
		}
		state[n] = 1
		var deps []string
		for _, p := range plats {
			if c := all[p.goarch][n]; c != nil {
				for d := range c.deps {
					deps = append(deps, d)
				}
			}
		}
		sort.Strings(deps)
		for _, d := range deps {
			if names[d] {
				visit(d)
			}
		}
		state[n] = 2
		order = append(order, n)
	}
	for _, n := range sorted {
		visit(n)
	}

	for _, b := range []*bytes.Buffer{&w.h, &w.c} {
		switch {
		case si == 0:
			fmt.Fprintf(b, "#if %s\n", sys.cond)
		case si == len(systems)-1:
			fmt.Fprintf(b, "#else\n")
		default:
			fmt.Fprintf(b, "#elif %s\n", sys.cond)
		}
	}
	what := sys.goos
	if si == len(systems)-1 {
		what += ", and everything not above"
	}
	fmt.Fprintf(&w.h, "\n/* %s */\n\n", what)
	fmt.Fprintf(&w.c, "\n/* %s */\n\n", sys.goos)

	// Every struct is declared first, so one can point at another whatever
	// the order.
	for _, n := range sorted {
		for _, p := range plats {
			if c := all[p.goarch][n]; c != nil && c.isStruct {
				fmt.Fprintf(&w.h, "typedef struct Syscall%s Syscall%s;\n", n, n)
				break
			}
		}
	}
	w.h.WriteString("\n")

	fallback := fallbackArch[sys.goos]
	for _, n := range order {
		vs, fb := variants(plats, func(p *platform) (string, bool) {
			c := all[p.goarch][n]
			if c == nil || c.def == "" {
				return "", false
			}
			return c.def, true
		}, fallback)
		var b bytes.Buffer
		writeVariants(&b, sys.goos, plats, vs, fb, true)
		if b.Len() > 0 {
			w.h.Write(b.Bytes())
			w.h.WriteString("\n")
		}

		// The checks are only for the architectures Go has numbers for.
		vs, _ = variants(plats, func(p *platform) (string, bool) {
			c := all[p.goarch][n]
			if c == nil || c.check == "" {
				return "", false
			}
			return c.check, true
		}, "")
		b.Reset()
		writeVariants(&b, sys.goos, plats, vs, false, false)
		w.c.Write(b.Bytes())
	}
	w.c.WriteString("\n")
}

// finish writes the header to path and the check to tests/ under root.
func (w *typeWriter) finish(root, path string) {
	fmt.Fprintf(&w.h, "#endif\n\n#endif /* BURROW_SYSCALL_ZTYPES_H */\n")
	if err := os.WriteFile(path, w.h.Bytes(), 0o644); err != nil {
		die("%v", err)
	}
	w.c.WriteString("#endif\n")
	if err := os.WriteFile(filepath.Join(root, "tests/syscall_ztypes_check.inc"), w.c.Bytes(), 0o644); err != nil {
		die("%v", err)
	}
}
