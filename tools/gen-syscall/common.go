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
	"go/build"
	"go/importer"
	"go/token"
	"go/types"
	"os"
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
	fset := token.NewFileSet()
	pkg, err := importer.ForCompiler(fset, "source", nil).Import("syscall")
	if err != nil {
		die("%s/%s: %v", goos, goarch, err)
	}
	sizes := types.SizesFor("gc", goarch)
	if sizes == nil {
		die("no sizes for %s", goarch)
	}
	return &platform{goos, goarch, pkg, sizes}
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
