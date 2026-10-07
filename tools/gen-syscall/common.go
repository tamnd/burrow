//go:build ignore

// The parts of the syscall table generator that consts.go and types.go share:
// the systems and architectures, the C names, and type checking syscall for
// one of them. main.go has the order things happen in.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package main

import (
	"fmt"
	"go/ast"
	"go/build"
	"go/importer"
	"go/parser"
	"go/token"
	"go/types"
	"os"
	"path/filepath"
	"regexp"
	"strings"
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
	fmt.Fprintf(os.Stderr, "gen-syscall: "+format+"\n", args...)
	os.Exit(1)
}

// ---------------------------------------------------------------- C names

// The words of a Go name, the same split tools/burrow-coverage makes, so the
// C name here is the one the coverage report looks for.
var acronyms = map[string]bool{}

func init() {
	for _, w := range strings.Fields("URL HTTP HTTPS TLS TCP UDP IP DNS ID API CPU IO EOF ASN1 DER PEM JSON XML " +
		"UTF8 UTF16 RSA ECDSA GCM CBC SHA MD5 CRC FS DB SQL RPC MIME SMTP URI UUID " +
		"PKCS OID SAN CA OCSP SCT ALPN SNI QUIC HPACK GZIP ZIP RW ICMP") {
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

// condFor is the #if condition for a set of architectures of one system.
func condFor(archs []string) string {
	in := map[string]bool{}
	for _, a := range archs {
		in[a] = true
	}
	seen := map[string]bool{}
	var cs []string
	for _, a := range archs {
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

// ------------------------------------------------------- reading syscall

// One system and architecture, with syscall type checked for it.
type platform struct {
	goos, goarch string
	pkg          *types.Package
	sizes        types.Sizes
	fset         *token.FileSet
	files        []*ast.File // syscall's files for this platform
	info         *types.Info // the types of everything in them
}

// The architectures with Go's register ABI, from internal/buildcfg.
var regabi = map[string]bool{
	"amd64": true, "arm64": true, "loong64": true, "ppc64": true, "ppc64le": true, "riscv64": true, "s390x": true,
}

var hostTags []string

func load(goos, goarch string) *platform {
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
	sizes := types.SizesFor("gc", goarch)
	if sizes == nil {
		die("no sizes for %s", goarch)
	}
	// syscall is type checked here rather than imported, so the types of the
	// expressions in its function bodies are there for calls.go. What it
	// imports comes from source too.
	bp, err := build.Default.Import("syscall", "", 0)
	if err != nil {
		die("%s/%s: %v", goos, goarch, err)
	}
	fset := token.NewFileSet()
	var files []*ast.File
	for _, name := range bp.GoFiles {
		f, err := parser.ParseFile(fset, filepath.Join(bp.Dir, name), nil, parser.ParseComments)
		if err != nil {
			die("%s/%s: %v", goos, goarch, err)
		}
		files = append(files, f)
	}
	info := &types.Info{
		Types: map[ast.Expr]types.TypeAndValue{},
		Defs:  map[*ast.Ident]types.Object{},
		Uses:  map[*ast.Ident]types.Object{},
	}
	conf := types.Config{Importer: importer.ForCompiler(fset, "source", nil), Sizes: sizes}
	pkg, err := conf.Check("syscall", fset, files, info)
	if err != nil {
		die("%s/%s: %v", goos, goarch, err)
	}
	return &platform{goos, goarch, pkg, sizes, fset, files, info}
}

const banner = `/* Derived from Go's src/syscall, the zerrors, zsysnum and ztypes files and
 * the rest of the package.
 * Go source: %s.
 *
 * Generated by tools/gen-syscall-tables.sh from the Go tables. Do not edit.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */
`

// readFile is os.ReadFile that gives up on an error.
func readFile(path string) []byte {
	data, err := os.ReadFile(path)
	if err != nil {
		die("%v", err)
	}
	return data
}

// The BURROW_OS_ macros each GOOS defines.
var osMacros = map[string][]string{
	"linux":     {"BURROW_OS_LINUX"},
	"android":   {"BURROW_OS_ANDROID", "BURROW_OS_LINUX"},
	"darwin":    {"BURROW_OS_DARWIN"},
	"ios":       {"BURROW_OS_IOS"},
	"windows":   {"BURROW_OS_WINDOWS"},
	"freebsd":   {"BURROW_OS_FREEBSD"},
	"openbsd":   {"BURROW_OS_OPENBSD"},
	"netbsd":    {"BURROW_OS_NETBSD"},
	"dragonfly": {"BURROW_OS_DRAGONFLY"},
	"illumos":   {"BURROW_OS_SOLARIS"},
	"solaris":   {"BURROW_OS_SOLARIS"},
	"aix":       {"BURROW_OS_AIX"},
	"wasip1":    {"BURROW_OS_WASI"},
}

// The BURROW_OS_ and BURROW_ARCH_ macros defined for goos/goarch, or nil
// for a platform these tables do not know, which keeps every branch.
func targetMacros(goos, goarch string) map[string]bool {
	oses, ok := osMacros[goos]
	arch, ok2 := archMacro[goarch]
	if !ok || !ok2 {
		return nil
	}
	m := map[string]bool{arch: true}
	for _, o := range oses {
		m[o] = true
	}
	return m
}

var (
	platformMacro = regexp.MustCompile(`^BURROW_(?:OS|ARCH)_[A-Z0-9]+$`)
	definedRE     = regexp.MustCompile(`\bdefined\s*\(\s*(\w+)\s*\)|\bdefined\s+(\w+)`)
	condToken     = regexp.MustCompile(`D_\w+|\w+|\|\||&&|!|\(|\)|\S`)
	directive     = regexp.MustCompile(`^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$`)
)

// evalCond decides an #if condition that only asks which platform this is,
// with ok false for anything else.
func evalCond(cond string, macros map[string]bool) (v, ok bool) {
	cond = definedRE.ReplaceAllStringFunc(cond, func(s string) string {
		m := definedRE.FindStringSubmatch(s)
		return " D_" + m[1] + m[2] + " "
	})
	toks := condToken.FindAllString(cond, -1)
	for _, t := range toks {
		if strings.HasPrefix(t, "D_") {
			if !platformMacro.MatchString(t[2:]) {
				return false, false
			}
		} else if t != "||" && t != "&&" && t != "!" && t != "(" && t != ")" {
			return false, false
		}
	}
	// or := and ("||" and)*, and := not ("&&" not)*, not := "!" not | atom
	i := 0
	var or, and, not func() (bool, bool)
	or = func() (bool, bool) {
		v, ok := and()
		for ok && i < len(toks) && toks[i] == "||" {
			i++
			w, ok2 := and()
			v, ok = v || w, ok2
		}
		return v, ok
	}
	and = func() (bool, bool) {
		v, ok := not()
		for ok && i < len(toks) && toks[i] == "&&" {
			i++
			w, ok2 := not()
			v, ok = v && w, ok2
		}
		return v, ok
	}
	not = func() (bool, bool) {
		if i >= len(toks) {
			return false, false
		}
		t := toks[i]
		i++
		switch {
		case t == "!":
			v, ok := not()
			return !v, ok
		case t == "(":
			v, ok := or()
			if !ok || i >= len(toks) || toks[i] != ")" {
				return false, false
			}
			i++
			return v, true
		case strings.HasPrefix(t, "D_"):
			return macros[t[2:]], true
		}
		return false, false
	}
	v, ok = or()
	return v, ok && i == len(toks)
}

// selectTarget blanks the lines of code in #if branches the platform the
// macros are for cannot take. A condition that asks anything other than
// which platform this is keeps both its branches.
func selectTarget(code []byte, macros map[string]bool) []byte {
	if macros == nil {
		return code
	}
	lines := strings.Split(string(code), "\n")
	// Each level: whether the enclosing level is live, whether a branch here
	// was surely taken, and whether this branch is live.
	type level struct{ outer, taken, live bool }
	var stack []level
	live := true
	for i := 0; i < len(lines); i++ {
		m := directive.FindStringSubmatch(lines[i])
		if m == nil {
			if !live {
				lines[i] = ""
			}
			continue
		}
		// A condition can go on past a backslash at the end of the line.
		kw, rest := m[1], strings.TrimSpace(m[2])
		lines[i] = ""
		for strings.HasSuffix(rest, "\\") && i+1 < len(lines) {
			i++
			rest = strings.TrimSuffix(rest, "\\") + " " + strings.TrimSpace(lines[i])
			lines[i] = ""
		}
		switch kw {
		case "if", "ifdef", "ifndef":
			if kw == "ifdef" {
				rest = "defined(" + rest + ")"
			} else if kw == "ifndef" {
				rest = "!defined(" + rest + ")"
			}
			v, ok := evalCond(rest, macros)
			stack = append(stack, level{live, ok && v, live && (!ok || v)})
		case "elif":
			if len(stack) > 0 {
				top := &stack[len(stack)-1]
				if top.taken {
					top.live = false
				} else {
					v, ok := evalCond(rest, macros)
					top.live = top.outer && (!ok || v)
					top.taken = ok && v
				}
			}
		case "else":
			if len(stack) > 0 {
				top := &stack[len(stack)-1]
				top.live = top.outer && !top.taken
				top.taken = true
			}
		case "endif":
			if len(stack) > 0 {
				stack = stack[:len(stack)-1]
			}
		}
		if len(stack) > 0 {
			live = stack[len(stack)-1].live
		} else {
			live = true
		}
	}
	return []byte(strings.Join(lines, "\n"))
}
