//go:build ignore

// Command consts writes include/burrow/syscall/zconst.h, every constant
// syscall declares other than the E and SIG ones zerrors.h already has, for
// every system and architecture burrow builds for, and
// tests/syscall_zconst_check.inc, which compares them with the system's own
// headers. tools/gen-syscall-consts.sh runs it.
//
// The constants come from type checking Go's syscall package from source once
// for each GOOS and GOARCH, so the values are the ones the Go compiler would
// use, including the ones written as expressions or with iota, and nothing
// has to be parsed by hand. A constant with the same value on every
// architecture of a system is written once for the system, and the rest go
// under an #if for the architectures that share them.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package main

import (
	"bytes"
	"fmt"
	"go/build"
	"go/constant"
	"go/importer"
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

// The Go architectures burrow has a macro for, as in errors.go. The two
// byte orders of mips64 and ppc64 share a macro and can differ, so they are
// told apart by BURROW_BIG_ENDIAN.
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

// The other byte order of an architecture whose macro covers both.
var otherOrder = map[string]string{
	"mips64": "mips64le", "mips64le": "mips64",
	"ppc64": "ppc64le", "ppc64le": "ppc64",
}

// archCond is the #if condition for one architecture.
func archCond(a string) string {
	m := "defined(" + archMacro[a] + ")"
	if _, ok := otherOrder[a]; !ok {
		return m
	}
	if strings.HasSuffix(a, "le") {
		return "(" + m + " && BURROW_LITTLE_ENDIAN)"
	}
	return "(" + m + " && BURROW_BIG_ENDIAN)"
}

// The systems in the order their #if branches are written, as in errors.go.
// Linux is last and is the #else, so Cosmopolitan and wasip1 get it too.
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

// The Windows architectures Go still supports. The others come from the
// zerrors files that exist for each system.
var windowsArchs = []string{"386", "amd64", "arm64"}

// The architecture a system's #else stands for, which is the one an
// architecture with no table of its own gets. For Linux it is arm64, the
// asm-generic numbering every newer port uses.
var fallbackArch = map[string]string{"linux": "arm64"}

func die(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "consts: "+format+"\n", args...)
	os.Exit(1)
}

// ---------------------------------------------------------------- C names

// The words of a Go name, the same split tools/burrow-coverage makes, so the
// C name here is the one the coverage report looks for.
var acronyms = map[string]bool{}

func init() {
	for _, w := range strings.Fields("URL HTTP HTTPS TLS TCP UDP IP DNS ID API CPU IO EOF ASN1 DER PEM JSON XML " +
		"UTF8 UTF16 RSA ECDSA GCM CBC SHA MD5 CRC FS DB SQL RPC MIME SMTP URI UUID " +
		"PKCS OID SAN CA OCSP SCT ALPN SNI QUIC HPACK GZIP ZIP RW") {
		acronyms[w] = true
	}
}

func isUpper(b byte) bool { return b >= 'A' && b <= 'Z' }
func isLower(b byte) bool { return b >= 'a' && b <= 'z' }
func isDigit(b byte) bool { return b >= '0' && b <= '9' }

func words(name string) []string {
	name = strings.ReplaceAll(name, "NaN", "Nan")
	var out []string
	for _, part := range strings.Split(name, "_") {
		i, n := 0, len(part)
		for i < n {
			if isUpper(part[i]) {
				j := i
				for j < n && isUpper(part[j]) {
					j++
				}
				k := j
				for k < n && isDigit(part[k]) {
					k++
				}
				run := part[i:j]
				if k < n && isLower(part[k]) && k == j && len(run) > 1 {
					if acronyms[run] {
						m := k
						for m < n && isLower(part[m]) {
							m++
						}
						tail := part[k:m]
						if tail == "s" || (m < n && isDigit(part[m])) {
							for m < n && isDigit(part[m]) {
								m++
							}
							out = append(out, part[i:m])
							i = m
						} else {
							out = append(out, run)
							i = j
						}
						continue
					}
					out = append(out, run[:len(run)-1])
					i = j - 1
					continue
				}
				if k < n && isLower(part[k]) && k == j {
					m := k
					for m < n && (isLower(part[m]) || isDigit(part[m])) {
						m++
					}
					out = append(out, part[i:m])
					i = m
					continue
				}
				out = append(out, part[i:k])
				i = k
			} else {
				m := i
				for m < n && !isUpper(part[m]) {
					m++
				}
				if len(out) > 0 && isDigit(part[i]) {
					out[len(out)-1] += part[i:m]
				} else {
					out = append(out, part[i:m])
				}
				i = m
			}
		}
	}
	var keep []string
	for _, w := range out {
		if w != "" {
			keep = append(keep, w)
		}
	}
	return keep
}

// The C name of a constant. One with no lower case is written as it is, and
// the rest are split into words.
func cName(name string) string {
	if name == strings.ToUpper(name) {
		return "SYSCALL_" + name
	}
	return "SYSCALL_" + strings.ToUpper(strings.Join(words(name), "_"))
}

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

// ------------------------------------------------------- reading syscall

type platform struct {
	goos, goarch string
	consts       map[string]string // Go name to C value
}

// The architectures with Go's register ABI, from internal/buildcfg.
var regabi = map[string]bool{
	"amd64": true, "arm64": true, "loong64": true, "ppc64": true, "ppc64le": true, "riscv64": true, "s390x": true,
}

var hostTags []string

func load(goos, goarch string) map[string]string {
	build.Default.GOOS = goos
	build.Default.GOARCH = goarch
	build.Default.CgoEnabled = false
	// The tool tags are worked out for the machine this runs on, and the
	// register ABI experiments are only on for some architectures. With them
	// on for one that has no register ABI, internal/abi has no IntArgRegs.
	if hostTags == nil {
		hostTags = build.Default.ToolTags
	}
	build.Default.ToolTags = nil
	for _, t := range hostTags {
		if strings.HasPrefix(t, "goexperiment.regabi") && !regabi[goarch] {
			continue
		}
		build.Default.ToolTags = append(build.Default.ToolTags, t)
	}
	fset := token.NewFileSet()
	pkg, err := importer.ForCompiler(fset, "source", nil).Import("syscall")
	if err != nil {
		die("%s/%s: %v", goos, goarch, err)
	}
	out := map[string]string{}
	scope := pkg.Scope()
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

func (g *archGroup) cond() string {
	in := map[string]bool{}
	for _, a := range g.archs {
		in[a] = true
	}
	seen := map[string]bool{}
	var cs []string
	for _, a := range g.archs {
		c := archCond(a)
		if o, ok := otherOrder[a]; ok && in[o] {
			// Both byte orders are here, so the macro alone says it.
			c = "defined(" + archMacro[a] + ")"
		}
		if !seen[c] {
			seen[c] = true
			cs = append(cs, c)
		}
	}
	return strings.Join(cs, " || ")
}

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

const banner = `/* Derived from Go's src/syscall, the zerrors, zsysnum and ztypes files and
 * the constants in the rest of the package.
 * Go source: %s.
 *
 * Generated by tools/gen-syscall-consts.sh from the Go tables. Do not edit.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */
`

func main() {
	if len(os.Args) != 5 {
		die("usage: consts <goroot> <go version> <burrow root> <header>")
	}
	dir := filepath.Join(os.Args[1], "src", "syscall")
	ver := os.Args[2]
	root := os.Args[3]
	skip := defined(filepath.Join(root, "include/burrow/syscall/zerrors.h"),
		filepath.Join(root, "include/burrow/syscall.h"))

	var h bytes.Buffer
	fmt.Fprintf(&h, banner, ver)
	fmt.Fprintf(&h, `
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

	// Every name the check compares, which is every name with no lower case
	// on any system.
	checkNames := map[string]bool{}
	names := map[string]string{} // C name to Go name, to catch two that collide

	for si, sys := range systems {
		var archs []string
		if sys.goos == "windows" {
			archs = windowsArchs
		} else {
			files, _ := filepath.Glob(filepath.Join(dir, "zerrors_"+sys.goos+"_*.go"))
			for _, f := range files {
				a := strings.TrimSuffix(strings.TrimPrefix(filepath.Base(f), "zerrors_"+sys.goos+"_"), ".go")
				if archMacro[a] != "" {
					archs = append(archs, a)
				}
			}
		}
		if len(archs) == 0 {
			die("no architectures for %s", sys.goos)
		}
		var plats []*platform
		for _, a := range archs {
			fmt.Fprintf(os.Stderr, "consts: %s/%s\n", sys.goos, a)
			plats = append(plats, &platform{sys.goos, a, load(sys.goos, a)})
		}
		for _, p := range plats {
			for k := range p.consts {
				cn := cName(k)
				if other, ok := names[cn]; ok && other != k {
					die("%s and %s are both %s", other, k, cn)
				}
				names[cn] = k
				if k == strings.ToUpper(k) && !skip[cn] {
					checkNames[k] = true
				}
			}
		}

		// The constants every architecture has with the same value.
		common := map[string]string{}
		for k, v := range plats[0].consts {
			same := true
			for _, p := range plats[1:] {
				if pv, ok := p.consts[k]; !ok || pv != v {
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
		for _, p := range plats {
			rest := map[string]string{}
			for k, v := range p.consts {
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

		switch {
		case si == 0:
			fmt.Fprintf(&h, "#if %s\n", sys.cond)
		case si == len(systems)-1:
			fmt.Fprintf(&h, "#else\n")
		default:
			fmt.Fprintf(&h, "#elif %s\n", sys.cond)
		}
		what := sys.goos
		if si == len(systems)-1 {
			what += ", and everything not above"
		}
		fmt.Fprintf(&h, "\n/* %s */\n", what)
		writeDefines(&h, common, skip)

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
				case len(order) == 1:
					fmt.Fprintf(&h, "\n#if %s\n", g.cond())
				case i == 0:
					fmt.Fprintf(&h, "\n#if %s\n", g.cond())
				case g == fb:
					fmt.Fprintf(&h, "#else\n")
				default:
					fmt.Fprintf(&h, "#elif %s\n", g.cond())
				}
				label := strings.Join(labels, ", ")
				if g == fb {
					label += ", and any other architecture"
				}
				fmt.Fprintf(&h, "\n/* %s */\n", label)
				writeDefines(&h, g.consts, skip)
				fmt.Fprintf(&h, "\n")
			}
			fmt.Fprintf(&h, "#endif\n")
		}
		fmt.Fprintf(&h, "\n")
	}
	fmt.Fprintf(&h, "#endif\n\n#endif /* BURROW_SYSCALL_ZCONST_H */\n")

	if err := os.WriteFile(os.Args[4], h.Bytes(), 0o644); err != nil {
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
	for k := range checkNames {
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
