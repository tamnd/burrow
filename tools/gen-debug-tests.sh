#!/bin/sh
# Regenerates tests/debug_test_gen.h, which tests/elf_test.c and
# tests/dwarf_test.c read, or with the argument macho tests/macho_test_gen.h,
# which tests/macho_test.c reads. It holds Go's ELF test files, the ELF ones from
# debug/dwarf's testdata, and a dump of what Go's debug/elf and debug/dwarf make
# of each of them: the headers, sections, segments, symbols, dynamic tags and
# versions, and then every DWARF entry, range, type and line table row. The C
# tests make the same dump and have to get the same text. Each file is also
# damaged in a few dozen ways, and for those only a hash of Go's dump is kept,
# along with the String and GoString of every named ELF and DWARF constant.
# The macho one is the same for Go's Mach-O test files and the Mach-O ones in
# debug/dwarf's testdata, through debug/macho, each read as a universal binary
# and as a plain one. The pe one, tests/pe_test_gen.h, is the same again for
# Go's PE test files and the PE one in debug/dwarf's testdata, through debug/pe.
# The plan9obj one, tests/plan9obj_test_gen.h, does Go's two Plan 9 a.out files
# through debug/plan9obj, with the damage aimed at the symbol table as well.
# The gosym one, tests/gosym_test_gen.h, does Go's Go 1.15 pclntab, the one of
# pclinetest, which it builds from Go's testdata for linux/amd64, and symbol
# tables in each of the older formats made up here, through debug/gosym.
# The xcoff one, tests/xcoff_test_gen.h, does Go's two AIX executables and two
# AIX big archives through internal/xcoff, which debug/buildinfo reads AIX
# binaries with. The damage goes at the section headers, symbols, loader
# section and the decimal fields of the archive headers.
#
# The Go packages are copied out of GOSRC, a Go source tree that defaults to
# the one the go command has, into a scratch module and built from there, so
# the dump comes from the release named in the headers even when the go
# command is a different one. GOSRC has to have the testdata directories.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:-elf}
case $mode in
elf) out="$root/tests/debug_test_gen.h" ;;
macho) out="$root/tests/macho_test_gen.h" ;;
pe) out="$root/tests/pe_test_gen.h" ;;
plan9obj) out="$root/tests/plan9obj_test_gen.h" ;;
gosym) out="$root/tests/gosym_test_gen.h" ;;
xcoff) out="$root/tests/xcoff_test_gen.h" ;;
buildinfo) out="$root/tests/buildinfo_test_gen.h" ;;
*)
	echo "usage: $0 [elf|macho|pe|plan9obj|gosym|xcoff|buildinfo]" >&2
	exit 2
	;;
esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
gosrc=${GOSRC:-$(go env GOROOT)/src}

# vendor DIR PKG copies the non-test Go files of $gosrc/DIR into $tmp/PKG and
# points their imports at the other copies.
vendor() {
	mkdir -p "$tmp/$2"
	for f in "$gosrc/$1"/*.go; do
		case $f in *_test.go) continue ;; esac
		sed -e 's#"debug/dwarf"#"gen/dwarf"#' -e 's#"internal/zstd"#"gen/zstd"#' \
			-e 's#"internal/saferio"#"gen/saferio"#' -e 's#"internal/abi"#"gen/abi"#' \
			-e 's#"debug/elf"#"gen/elf"#' -e 's#"debug/macho"#"gen/macho"#' \
			-e 's#"debug/pe"#"gen/pe"#' -e 's#"debug/plan9obj"#"gen/plan9obj"#' \
			-e 's#"internal/xcoff"#"gen/xcoff"#' -e 's#"runtime/debug"#"gen/rtdebug"#' \
			-e '/^\/\/go:linkname /d' "$f" > "$tmp/$2/$(basename "$f")"
	done
}
vendor debug/elf elf
vendor debug/macho macho
vendor debug/pe pe
vendor debug/plan9obj plan9obj
vendor debug/gosym gosym
vendor internal/xcoff xcoff
vendor debug/dwarf dwarf
vendor internal/zstd zstd
vendor internal/saferio saferio
vendor debug/buildinfo buildinfo
# Of runtime/debug, debug/buildinfo needs only the BuildInfo of mod.go, and
# not the part that asks the runtime for its own.
mkdir "$tmp/rtdebug"
awk '/^\/\/ exported from runtime\./ { skip = 1 } skip && /^}/ { skip = 0; next } !skip' \
	"$gosrc/runtime/debug/mod.go" | sed '/^	"runtime"$/d' > "$tmp/rtdebug/mod.go"

mkdir "$tmp/elfdata" "$tmp/dwarfdata"
for f in "$gosrc/debug/elf/testdata"/*; do
	case $f in *.c) continue ;; esac
	cp "$f" "$tmp/elfdata/"
done
for f in "$gosrc/debug/dwarf/testdata"/*.elf "$gosrc/debug/dwarf/testdata"/*.elf4 \
	"$gosrc/debug/dwarf/testdata"/*.elf5; do
	cp "$f" "$tmp/dwarfdata/"
done
mkdir "$tmp/machodata" "$tmp/machodwarf"
for f in "$gosrc/debug/macho/testdata"/*.base64; do
	cp "$f" "$tmp/machodata/"
done
for f in "$gosrc/debug/dwarf/testdata"/*.macho "$gosrc/debug/dwarf/testdata"/*.macho4; do
	cp "$f" "$tmp/machodwarf/"
done
mkdir "$tmp/pedata" "$tmp/pedwarf"
for f in "$gosrc/debug/pe/testdata"/*; do
	case $f in *.c) continue ;; esac
	[ -f "$f" ] && cp "$f" "$tmp/pedata/"
done
cp "$gosrc/debug/dwarf/testdata/line-gcc-win.bin" "$tmp/pedwarf/"
mkdir "$tmp/plan9data"
for f in "$gosrc/debug/plan9obj/testdata"/*; do
	case $f in *.c) continue ;; esac
	cp "$f" "$tmp/plan9data/"
done
mkdir "$tmp/xcoffdata"
for f in "$gosrc/internal/xcoff/testdata"/*; do
	case $f in *.c) continue ;; esac
	cp "$f" "$tmp/xcoffdata/"
done
mkdir "$tmp/bidata" "$tmp/bibuild"
cp -R "$gosrc/debug/buildinfo/testdata/." "$tmp/bidata/"
cp "$gosrc/debug/buildinfo/buildinfo_test.go" "$tmp/bidata/"
if [ "$mode" = buildinfo ]; then
	# The hello program of Go's TestReadFile, with a module it depends on
	# that is replaced by a directory, for each kind of binary debug/buildinfo
	# reads. The builds use the go command there is, so the record says its
	# release, and -ldflags with a space in it, so a setting has to be quoted.
	mkdir -p "$tmp/hello/dep" "$tmp/gopath/src/example.com/m"
	printf 'module example.com/m\n\ngo 1.18\n\nrequire example.com/dep v1.0.0\n\nreplace example.com/dep v1.0.0 => ./dep\n' \
		> "$tmp/hello/go.mod"
	printf 'package main\n\nimport _ "example.com/dep"\n\nfunc main() {}\n' > "$tmp/hello/hello.go"
	printf 'module example.com/dep\n\ngo 1.18\n' > "$tmp/hello/dep/go.mod"
	printf 'package dep\n' > "$tmp/hello/dep/dep.go"
	printf 'package main\n\nfunc main() {}\n' > "$tmp/gopath/src/example.com/m/hello.go"
	for t in linux/amd64/exe linux/amd64/pie linux/386/exe linux/arm/exe linux/ppc64/exe \
		windows/amd64/exe windows/386/exe darwin/amd64/exe darwin/arm64/exe aix/ppc64/exe \
		plan9/386/exe plan9/amd64/exe plan9/arm/exe; do
		os=${t%%/*}
		bm=${t##*/}
		arch=${t#*/}
		arch=${arch%/*}
		(cd "$tmp/hello" && GOOS=$os GOARCH=$arch CGO_ENABLED=0 GO111MODULE=on GOFLAGS='' \
			GOTOOLCHAIN=local GOPROXY=off GOCACHE="$tmp/gocache" go build -buildmode="$bm" \
			-trimpath -buildvcs=false "-ldflags=-buildid= -s" -o "$tmp/bibuild/${os}_${arch}_$bm" .)
	done
	(cd "$tmp/gopath/src/example.com/m" && GOOS=linux GOARCH=amd64 CGO_ENABLED=0 GO111MODULE=off \
		GOPATH="$tmp/gopath" GOFLAGS='' GOTOOLCHAIN=local GOCACHE="$tmp/gocache" go build -trimpath \
		"-ldflags=-buildid= -s" -o "$tmp/bibuild/gopath_linux_amd64_exe" .)
	rm -rf "$tmp/gocache"
fi
mkdir "$tmp/gosymdata" "$tmp/abi"
cp "$gosrc/debug/gosym/testdata/pcln115.gz" "$tmp/gosymdata/"
if [ "$mode" = gosym ]; then
	mkdir "$tmp/pclinetest"
	for f in main.go pclinetest.h pclinetest.s; do
		cp "$gosrc/debug/gosym/testdata/$f" "$tmp/pclinetest/"
	done
	printf 'module pclinetest\n\ngo 1.22\n' > "$tmp/pclinetest/go.mod"
	(cd "$tmp/pclinetest" && GOOS=linux GOARCH=amd64 CGO_ENABLED=0 \
		go build -trimpath -ldflags=-buildid= -o "$tmp/gosymdata/pclinetest" .)
fi
# debug/gosym takes the magic numbers of the pclntab from internal/abi, which
# a module outside the Go tree cannot import.
cat > "$tmp/abi/abi.go" <<'GO'
package abi

type PCLnTabMagic uint32

const (
	Go12PCLnTabMagic  PCLnTabMagic = 0xfffffffb
	Go116PCLnTabMagic PCLnTabMagic = 0xfffffffa
	Go118PCLnTabMagic PCLnTabMagic = 0xfffffff0
	Go120PCLnTabMagic PCLnTabMagic = 0xfffffff1
)
GO
# What the dump needs to see of a LineTable and a Table.
cat > "$tmp/gosym/export.go" <<'GO'
package gosym

import (
	"encoding/binary"
	"fmt"
)

func (t *LineTable) Info() string {
	t.isGo12()
	s := fmt.Sprintf("version=%d quantum=%d ptrsize=%d textstart=%#x nfunctab=%d nfiletab=%d tabs=%d,%d,%d,%d,%d,%d",
		t.version, t.quantum, t.ptrsize, t.textStart, t.nfunctab, t.nfiletab,
		len(t.funcnametab), len(t.cutab), len(t.funcdata), len(t.functab),
		len(t.filetab), len(t.pctab))
	if t.version >= ver12 {
		if t.binary == binary.BigEndian {
			s += " big"
		} else {
			s += " little"
		}
	}
	return s
}

func (t *Table) Go12() bool { return t.go12line != nil }
GO
cp "$gosrc/debug/elf/elf.go" "$tmp/elf.go.txt"
cp "$gosrc/debug/macho/macho.go" "$tmp/macho.go.txt"
cp "$gosrc/debug/dwarf/const.go" "$tmp/const.go.txt"

cat > "$tmp/main.go" <<'GO'
package main

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"go/ast"
	"go/constant"
	"go/parser"
	"go/token"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"gen/dwarf"
	"gen/elf"
)

func blob(b []byte, err error) string {
	h := sha256.Sum256(b)
	return fmt.Sprintf("len=%d sha=%x%s", len(b), h[:8], errs(err))
}

func errs(err error) string {
	if err == nil {
		return ""
	}
	return fmt.Sprintf(" err=%q", err.Error())
}

func val(v any) string {
	switch x := v.(type) {
	case nil:
		return "nil"
	case int64:
		return fmt.Sprintf("i%d", x)
	case uint64:
		return fmt.Sprintf("u%#x", x)
	case string:
		return fmt.Sprintf("%q", x)
	case []byte:
		return fmt.Sprintf("b%x", x)
	case bool:
		return fmt.Sprintf("%t", x)
	case dwarf.Offset:
		return fmt.Sprintf("o%#x", x)
	}
	return fmt.Sprintf("?%T", v)
}

func dumpSyms(w *bytes.Buffer, label string, syms []elf.Symbol, err error) {
	fmt.Fprintf(w, "%s n=%d%s\n", label, len(syms), errs(err))
	for i, s := range syms {
		fmt.Fprintf(w, "  %d %q info=%d %s %s other=%d %s hasver=%t vi=%d hidden=%t section=%s value=%#x size=%#x version=%q library=%q\n",
			i, s.Name, s.Info, elf.ST_BIND(s.Info).String(), elf.ST_TYPE(s.Info).String(),
			s.Other, elf.ST_VISIBILITY(s.Other).String(), s.HasVersion,
			s.VersionIndex.Index(), s.VersionIndex.IsHidden(), s.Section.String(),
			s.Value, s.Size, s.Version, s.Library)
	}
}

func dumpStrings(w *bytes.Buffer, label string, ss []string, err error) {
	fmt.Fprintf(w, "%s n=%d%s\n", label, len(ss), errs(err))
	for _, s := range ss {
		fmt.Fprintf(w, "  %q\n", s)
	}
}

func isTypeTag(t dwarf.Tag) bool {
	switch t {
	case dwarf.TagArrayType, dwarf.TagBaseType, dwarf.TagClassType, dwarf.TagStructType,
		dwarf.TagUnionType, dwarf.TagConstType, dwarf.TagVolatileType,
		dwarf.TagRestrictType, dwarf.TagEnumerationType, dwarf.TagPointerType,
		dwarf.TagSubroutineType, dwarf.TagTypedef, dwarf.TagUnspecifiedType,
		dwarf.TagReferenceType, dwarf.TagRvalueReferenceType, dwarf.TagPtrToMemberType:
		return true
	}
	return false
}

func dumpType(w *bytes.Buffer, d *dwarf.Data, off dwarf.Offset) {
	t, err := d.Type(off)
	if err != nil {
		fmt.Fprintf(w, "  type %#x err=%q\n", off, err.Error())
		return
	}
	kind := strings.TrimPrefix(fmt.Sprintf("%T", t), "*dwarf.")
	fmt.Fprintf(w, "  type %#x %s size=%d name=%q str=%q", off, kind, t.Size(),
		t.Common().Name, t.String())
	if b, ok := t.(interface{ Basic() *dwarf.BasicType }); ok {
		bt := b.Basic()
		fmt.Fprintf(w, " bits=%d,%d,%d", bt.BitSize, bt.BitOffset, bt.DataBitOffset)
	}
	switch t := t.(type) {
	case *dwarf.StructType:
		fmt.Fprintf(w, " kind=%q sname=%q incomplete=%t defn=%q", t.Kind, t.StructName,
			t.Incomplete, t.Defn())
		for _, f := range t.Field {
			fmt.Fprintf(w, " field=%q,%q,%d,%d,%d,%d,%d", f.Name, f.Type.String(),
				f.ByteOffset, f.ByteSize, f.BitOffset, f.DataBitOffset, f.BitSize)
		}
	case *dwarf.ArrayType:
		fmt.Fprintf(w, " stride=%d count=%d", t.StrideBitSize, t.Count)
	case *dwarf.EnumType:
		fmt.Fprintf(w, " ename=%q", t.EnumName)
		for _, v := range t.Val {
			fmt.Fprintf(w, " val=%q,%d", v.Name, v.Val)
		}
	case *dwarf.FuncType:
		fmt.Fprintf(w, " params=%d", len(t.ParamType))
	case *dwarf.QualType:
		fmt.Fprintf(w, " qual=%q", t.Qual)
	case *dwarf.UnsupportedType:
		fmt.Fprintf(w, " tag=%s", t.Tag.String())
	}
	fmt.Fprintf(w, "\n")
}

func dumpLines(w *bytes.Buffer, d *dwarf.Data, cu *dwarf.Entry) {
	lr, err := d.LineReader(cu)
	if err != nil {
		fmt.Fprintf(w, "lines %#x err=%q\n", cu.Offset, err.Error())
		return
	}
	if lr == nil {
		fmt.Fprintf(w, "lines %#x none\n", cu.Offset)
		return
	}
	fmt.Fprintf(w, "lines %#x\n", cu.Offset)
	var rows []dwarf.LineEntry
	var mid dwarf.LineReaderPos
	var e dwarf.LineEntry
	for {
		if len(rows) == 3 {
			mid = lr.Tell()
		}
		err := lr.Next(&e)
		if err != nil {
			fmt.Fprintf(w, "  end err=%q\n", err.Error())
			break
		}
		rows = append(rows, e)
		file := "nil"
		if e.File != nil {
			file = fmt.Sprintf("%q", e.File.Name)
		}
		fmt.Fprintf(w, "  row %#x op=%d file=%s line=%d col=%d stmt=%t bb=%t pe=%t eb=%t isa=%d disc=%d end=%t\n",
			e.Address, e.OpIndex, file, e.Line, e.Column, e.IsStmt, e.BasicBlock,
			e.PrologueEnd, e.EpilogueBegin, e.ISA, e.Discriminator, e.EndSequence)
	}
	files := lr.Files()
	fmt.Fprintf(w, "  files n=%d\n", len(files))
	for _, f := range files {
		if f == nil {
			fmt.Fprintf(w, "    nil\n")
			continue
		}
		fmt.Fprintf(w, "    %q mtime=%d len=%d\n", f.Name, f.Mtime, f.Length)
	}
	if len(rows) > 3 {
		lr.Seek(mid)
		same := 0
		for i := 3; i < len(rows); i++ {
			if lr.Next(&e) != nil {
				break
			}
			if e.Address == rows[i].Address && e.Line == rows[i].Line {
				same++
			}
		}
		fmt.Fprintf(w, "  tellseek %d of %d\n", same, len(rows)-3)
	}
	lr.Reset()
	for _, row := range rows {
		if row.EndSequence {
			continue
		}
		var got dwarf.LineEntry
		err := lr.SeekPC(row.Address, &got)
		if err != nil {
			fmt.Fprintf(w, "  seekpc %#x err=%q\n", row.Address, err.Error())
			continue
		}
		fmt.Fprintf(w, "  seekpc %#x %#x line=%d\n", row.Address, got.Address, got.Line)
	}
}

func dumpDWARF(w *bytes.Buffer, d *dwarf.Data) {
	r := d.Reader()
	var cus []*dwarf.Entry
	var lows []uint64
	for {
		e, err := r.Next()
		if err != nil {
			fmt.Fprintf(w, "next err=%q\n", err.Error())
			break
		}
		if e == nil {
			break
		}
		fmt.Fprintf(w, "entry %#x %s children=%t", e.Offset, e.Tag.String(), e.Children)
		for _, f := range e.Field {
			fmt.Fprintf(w, " %s:%s=%s", f.Attr.String(), f.Class.String(), val(f.Val))
		}
		fmt.Fprintf(w, "\n")
		if e.Tag == dwarf.TagCompileUnit {
			cus = append(cus, e)
			fmt.Fprintf(w, "  unit asize=%d order=%s\n", r.AddressSize(), r.ByteOrder().String())
		}
		rs, err := d.Ranges(e)
		if len(rs) > 0 || err != nil {
			fmt.Fprintf(w, "  ranges")
			for _, x := range rs {
				fmt.Fprintf(w, " %#x-%#x", x[0], x[1])
			}
			fmt.Fprintf(w, "%s\n", errs(err))
			if e.Tag == dwarf.TagCompileUnit && len(rs) > 0 {
				lows = append(lows, rs[0][0])
			}
		}
		if isTypeTag(e.Tag) {
			dumpType(w, d, e.Offset)
		}
		if off, ok := e.Val(dwarf.AttrType).(dwarf.Offset); ok {
			dumpType(w, d, off)
		}
	}
	for _, cu := range cus {
		dumpLines(w, d, cu)
	}
	r2 := d.Reader()
	for _, pc := range lows {
		e, err := r2.SeekPC(pc)
		if err != nil {
			fmt.Fprintf(w, "seekpc %#x err=%q\n", pc, err.Error())
			continue
		}
		fmt.Fprintf(w, "seekpc %#x %#x\n", pc, e.Offset)
	}
	for _, cu := range cus {
		r2.Seek(cu.Offset)
		e, err := r2.Next()
		if err != nil || e == nil {
			fmt.Fprintf(w, "seek %#x failed%s\n", cu.Offset, errs(err))
			continue
		}
		r2.SkipChildren()
		e2, err := r2.Next()
		next := "nil"
		if e2 != nil {
			next = fmt.Sprintf("%#x", e2.Offset)
		}
		fmt.Fprintf(w, "seek %#x %#x next=%s%s\n", cu.Offset, e.Offset, next, errs(err))
	}
}

var dynValueTags = []elf.DynTag{elf.DT_NEEDED, elf.DT_FLAGS, elf.DT_FLAGS_1,
	elf.DT_VERNEEDNUM, elf.DT_STRSZ, elf.DT_PLTGOT, elf.DT_INIT}

func dump(w *bytes.Buffer, data []byte) {
	f, err := elf.NewFile(bytes.NewReader(data))
	if err != nil {
		fmt.Fprintf(w, "newfile err=%q\n", err.Error())
		return
	}
	defer f.Close()
	h := f.FileHeader
	fmt.Fprintf(w, "header class=%s data=%s version=%s osabi=%s abiversion=%d byteorder=%s type=%s machine=%s entry=%#x\n",
		h.Class.String(), h.Data.String(), h.Version.String(), h.OSABI.String(),
		h.ABIVersion, h.ByteOrder.String(), h.Type.String(), h.Machine.String(), h.Entry)
	for i, s := range f.Sections {
		sh := s.SectionHeader
		fmt.Fprintf(w, "section %d %q type=%s flags=%s addr=%#x offset=%#x size=%#x link=%d info=%d addralign=%#x entsize=%#x filesize=%#x\n",
			i, sh.Name, sh.Type.String(), sh.Flags.String(), sh.Addr, sh.Offset, sh.Size,
			sh.Link, sh.Info, sh.Addralign, sh.Entsize, sh.FileSize)
		b, err := s.Data()
		fmt.Fprintf(w, "  data %s\n", blob(b, err))
		b, err = io.ReadAll(s.Open())
		fmt.Fprintf(w, "  open %s\n", blob(b, err))
	}
	for i, p := range f.Progs {
		ph := p.ProgHeader
		fmt.Fprintf(w, "prog %d type=%s flags=%s off=%#x vaddr=%#x paddr=%#x filesz=%#x memsz=%#x align=%#x\n",
			i, ph.Type.String(), ph.Flags.String(), ph.Off, ph.Vaddr, ph.Paddr, ph.Filesz,
			ph.Memsz, ph.Align)
		b, err := io.ReadAll(p.Open())
		fmt.Fprintf(w, "  open %s\n", blob(b, err))
	}
	syms, err := f.Symbols()
	dumpSyms(w, "symbols", syms, err)
	syms, err = f.DynamicSymbols()
	dumpSyms(w, "dynsyms", syms, err)
	imp, err := f.ImportedSymbols()
	fmt.Fprintf(w, "imported n=%d%s\n", len(imp), errs(err))
	for _, s := range imp {
		fmt.Fprintf(w, "  %q %q %q\n", s.Name, s.Version, s.Library)
	}
	libs, err := f.ImportedLibraries()
	dumpStrings(w, "libraries", libs, err)
	for _, tag := range []elf.DynTag{elf.DT_NEEDED, elf.DT_SONAME, elf.DT_RPATH, elf.DT_RUNPATH} {
		ss, err := f.DynString(tag)
		dumpStrings(w, "dynstring "+tag.String(), ss, err)
	}
	for _, tag := range dynValueTags {
		vs, err := f.DynValue(tag)
		fmt.Fprintf(w, "dynvalue %s n=%d%s", tag.String(), len(vs), errs(err))
		for _, v := range vs {
			fmt.Fprintf(w, " %#x", v)
		}
		fmt.Fprintf(w, "\n")
	}
	vers, err := f.DynamicVersions()
	fmt.Fprintf(w, "versions n=%d%s\n", len(vers), errs(err))
	for _, v := range vers {
		fmt.Fprintf(w, "  %q index=%d flags=%d deps=%d\n", v.Name, v.Index, v.Flags, len(v.Deps))
		for _, d := range v.Deps {
			fmt.Fprintf(w, "    %q\n", d)
		}
	}
	needs, err := f.DynamicVersionNeeds()
	fmt.Fprintf(w, "needs n=%d%s\n", len(needs), errs(err))
	for _, n := range needs {
		fmt.Fprintf(w, "  %q n=%d\n", n.Name, len(n.Needs))
		for _, d := range n.Needs {
			fmt.Fprintf(w, "    %q index=%d flags=%d\n", d.Dep, d.Index, d.Flags)
		}
	}
	d, err := f.DWARF()
	if err != nil {
		fmt.Fprintf(w, "dwarf err=%q\n", err.Error())
		return
	}
	dumpDWARF(w, d)
}

// safeDump is dump, or false if Go panics, which the C side does too, so that
// damage is left out.
func safeDump(data []byte) (out []byte, ok bool) {
	defer func() {
		if recover() != nil {
			ok = false
		}
	}()
	var w bytes.Buffer
	dump(&w, data)
	return w.Bytes(), true
}

func lcg(x uint64) uint64 { return x*6364136223846793005 + 1442695040888963407 }

type damage struct {
	pos  int // byte to change, or where to cut when xor is 0
	xor  byte
	hash string
}

func damages(data []byte, seed uint64) []damage {
	var ds []damage
	x := seed
	n := len(data)
	add := func(pos int, xor byte) {
		b := append([]byte(nil), data...)
		if xor == 0 {
			b = b[:pos]
		} else {
			b[pos] ^= xor
		}
		out, ok := safeDump(b)
		if !ok {
			return
		}
		ds = append(ds, damage{pos, xor, fmt.Sprintf("%x", sha256.Sum256(out))[:16]})
	}
	// The file header, then the section headers, then anywhere.
	for i := 0; i < 12; i++ {
		x = lcg(x)
		add(int(x>>33)%min(64, n), byte(1<<((x>>20)%8)))
	}
	shoff := 0
	if n > 0x30 && data[4] == 2 && data[5] == 1 {
		shoff = int(uint64(data[0x28]) | uint64(data[0x29])<<8 | uint64(data[0x2a])<<16)
	} else if n > 0x24 && data[4] == 1 && data[5] == 1 {
		shoff = int(uint32(data[0x20]) | uint32(data[0x21])<<8 | uint32(data[0x22])<<16)
	}
	if shoff > 0 && shoff < n {
		for i := 0; i < 12; i++ {
			x = lcg(x)
			add(shoff+int(x>>33)%(n-shoff), byte(1<<((x>>20)%8)))
		}
	}
	for i := 0; i < 16; i++ {
		x = lcg(x)
		xor := byte(x >> 56)
		if xor == 0 {
			xor = 0x80
		}
		add(int(x>>33)%n, xor)
	}
	for _, cut := range []int{n / 2, n - 1, n * 3 / 4, 100} {
		if cut > 0 && cut < n {
			add(cut, 0)
		}
	}
	return ds
}

type file struct {
	name   string
	gz     bool
	raw    []byte // as in testdata
	data   []byte // the ELF file
	dump   []byte
	damage []damage
}

func load(dir string) []*file {
	ents, err := os.ReadDir(dir)
	if err != nil {
		panic(err)
	}
	var fs []*file
	for _, de := range ents {
		raw, err := os.ReadFile(filepath.Join(dir, de.Name()))
		if err != nil {
			panic(err)
		}
		f := &file{name: de.Name(), raw: raw, data: raw}
		if strings.HasSuffix(de.Name(), ".gz") {
			zr, err := gzip.NewReader(bytes.NewReader(raw))
			if err != nil {
				panic(err)
			}
			f.data, err = io.ReadAll(zr)
			if err != nil {
				panic(err)
			}
			f.gz = true
		}
		fs = append(fs, f)
	}
	return fs
}

// The constant types with String and GoString, in the order of the C test's
// table.
var elfKinds = []struct {
	name string
	f    func(int64) (string, string)
}{
	{"Version", func(v int64) (string, string) { x := elf.Version(v); return x.String(), x.GoString() }},
	{"Class", func(v int64) (string, string) { x := elf.Class(v); return x.String(), x.GoString() }},
	{"Data", func(v int64) (string, string) { x := elf.Data(v); return x.String(), x.GoString() }},
	{"OSABI", func(v int64) (string, string) { x := elf.OSABI(v); return x.String(), x.GoString() }},
	{"Type", func(v int64) (string, string) { x := elf.Type(v); return x.String(), x.GoString() }},
	{"Machine", func(v int64) (string, string) { x := elf.Machine(v); return x.String(), x.GoString() }},
	{"SectionIndex", func(v int64) (string, string) { x := elf.SectionIndex(v); return x.String(), x.GoString() }},
	{"SectionType", func(v int64) (string, string) { x := elf.SectionType(v); return x.String(), x.GoString() }},
	{"SectionFlag", func(v int64) (string, string) { x := elf.SectionFlag(v); return x.String(), x.GoString() }},
	{"CompressionType", func(v int64) (string, string) { x := elf.CompressionType(v); return x.String(), x.GoString() }},
	{"ProgType", func(v int64) (string, string) { x := elf.ProgType(v); return x.String(), x.GoString() }},
	{"ProgFlag", func(v int64) (string, string) { x := elf.ProgFlag(v); return x.String(), x.GoString() }},
	{"DynTag", func(v int64) (string, string) { x := elf.DynTag(v); return x.String(), x.GoString() }},
	{"DynFlag", func(v int64) (string, string) { x := elf.DynFlag(v); return x.String(), x.GoString() }},
	{"DynFlag1", func(v int64) (string, string) { x := elf.DynFlag1(v); return x.String(), x.GoString() }},
	{"NType", func(v int64) (string, string) { x := elf.NType(v); return x.String(), x.GoString() }},
	{"SymBind", func(v int64) (string, string) { x := elf.SymBind(v); return x.String(), x.GoString() }},
	{"SymType", func(v int64) (string, string) { x := elf.SymType(v); return x.String(), x.GoString() }},
	{"SymVis", func(v int64) (string, string) { x := elf.SymVis(v); return x.String(), x.GoString() }},
	{"R_X86_64", func(v int64) (string, string) { x := elf.R_X86_64(v); return x.String(), x.GoString() }},
	{"R_AARCH64", func(v int64) (string, string) { x := elf.R_AARCH64(v); return x.String(), x.GoString() }},
	{"R_ALPHA", func(v int64) (string, string) { x := elf.R_ALPHA(v); return x.String(), x.GoString() }},
	{"R_ARM", func(v int64) (string, string) { x := elf.R_ARM(v); return x.String(), x.GoString() }},
	{"R_386", func(v int64) (string, string) { x := elf.R_386(v); return x.String(), x.GoString() }},
	{"R_MIPS", func(v int64) (string, string) { x := elf.R_MIPS(v); return x.String(), x.GoString() }},
	{"R_LARCH", func(v int64) (string, string) { x := elf.R_LARCH(v); return x.String(), x.GoString() }},
	{"R_PPC", func(v int64) (string, string) { x := elf.R_PPC(v); return x.String(), x.GoString() }},
	{"R_PPC64", func(v int64) (string, string) { x := elf.R_PPC64(v); return x.String(), x.GoString() }},
	{"R_RISCV", func(v int64) (string, string) { x := elf.R_RISCV(v); return x.String(), x.GoString() }},
	{"R_390", func(v int64) (string, string) { x := elf.R_390(v); return x.String(), x.GoString() }},
	{"R_SPARC", func(v int64) (string, string) { x := elf.R_SPARC(v); return x.String(), x.GoString() }},
	{"dwarf.Attr", func(v int64) (string, string) { x := dwarf.Attr(v); return x.String(), x.GoString() }},
	{"dwarf.Tag", func(v int64) (string, string) { x := dwarf.Tag(v); return x.String(), x.GoString() }},
	{"dwarf.Class", func(v int64) (string, string) { x := dwarf.Class(v); return x.String(), x.GoString() }},
}

// constValues reads the typed constants in a Go file, by type name.
func constValues(path string) map[string][]int64 {
	fset := token.NewFileSet()
	af, err := parser.ParseFile(fset, path, nil, 0)
	if err != nil {
		panic(err)
	}
	vals := map[string][]int64{}
	for _, decl := range af.Decls {
		gd, ok := decl.(*ast.GenDecl)
		if !ok || gd.Tok != token.CONST {
			continue
		}
		for _, spec := range gd.Specs {
			vs := spec.(*ast.ValueSpec)
			id, ok := vs.Type.(*ast.Ident)
			if !ok || len(vs.Values) != 1 {
				continue
			}
			v, ok := eval(vs.Values[0])
			if ok {
				vals[id.Name] = append(vals[id.Name], v)
			}
		}
	}
	return vals
}

func eval(e ast.Expr) (int64, bool) {
	switch e := e.(type) {
	case *ast.BasicLit:
		v := constant.MakeFromLiteral(e.Value, e.Kind, 0)
		return constant.Int64Val(constant.ToInt(v))
	case *ast.ParenExpr:
		return eval(e.X)
	case *ast.UnaryExpr:
		x, ok := eval(e.X)
		if ok && e.Op == token.SUB {
			return -x, true
		}
	case *ast.BinaryExpr:
		x, ok1 := eval(e.X)
		y, ok2 := eval(e.Y)
		if !ok1 || !ok2 {
			return 0, false
		}
		switch e.Op {
		case token.SHL:
			return x << uint(y), true
		case token.OR:
			return x | y, true
		case token.ADD:
			return x + y, true
		}
	}
	return 0, false
}

// names gives the values to try for each kind: every named one, the ones
// next to it, the small ones, and for the flags some unions.
func names() [][3]any {
	vals := constValues("elf.go.txt")
	for k, v := range constValues("const.go.txt") {
		vals["dwarf."+k] = v
	}
	var rows [][3]any
	for ki, k := range elfKinds {
		set := map[int64]bool{}
		width := int64(1) << 32
		switch k.name {
		case "Version", "Class", "Data", "OSABI":
			width = 256
		case "Type", "Machine":
			width = 1 << 16
		}
		for v := int64(0); v < 40; v++ {
			set[v] = true
		}
		named := vals[k.name]
		for _, v := range named {
			set[v] = true
			set[v+1] = true
			if v > 0 {
				set[v-1] = true
			}
		}
		if strings.Contains(k.name, "Flag") {
			for i, v := range named {
				if i+1 < len(named) {
					set[v|named[i+1]] = true
				}
				set[v|1<<30] = true
			}
		}
		set[width-1] = true
		var vs []int64
		for v := range set {
			if v >= 0 && v < width {
				vs = append(vs, v)
			}
		}
		sort.Slice(vs, func(i, j int) bool { return vs[i] < vs[j] })
		for _, v := range vs {
			s, g := k.f(v)
			rows = append(rows, [3]any{ki, v, [2]string{s, g}})
		}
	}
	return rows
}

func cstr(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '?':
			// Keeps trigraphs out.
			b.WriteString("\\?")
		case c >= 0x20 && c < 0x7f:
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func writeBase64(w io.Writer, name string, b []byte) {
	s := base64.StdEncoding.EncodeToString(b)
	fmt.Fprintf(w, "static const char *const %s[] = {\n", name)
	for len(s) > 0 {
		n := min(72, len(s))
		fmt.Fprintf(w, "    \"%s\",\n", s[:n])
		s = s[n:]
	}
	fmt.Fprintf(w, "    NULL,\n};\n\n")
}

func main() {
	if len(os.Args) > 1 && os.Args[1] == "macho" {
		machoMain()
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "pe" {
		peMain()
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "plan9obj" {
		plan9Main()
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "gosym" {
		gosymMain()
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "xcoff" {
		xcoffMain()
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "buildinfo" {
		biMain()
		return
	}
	var fs []*file
	for _, f := range load("elfdata") {
		f.name = "elf/" + f.name
		fs = append(fs, f)
	}
	for _, f := range load("dwarfdata") {
		f.name = "dwarf/" + f.name
		fs = append(fs, f)
	}
	for i, f := range fs {
		out, ok := safeDump(f.data)
		if !ok {
			panic("Go panics on " + f.name)
		}
		f.dump = out
		f.damage = damages(f.data, uint64(i)+1)
	}

	w := os.Stdout
	fmt.Fprintf(w, "/* Generated by tools/gen-debug-tests.sh from Go's debug/elf and debug/dwarf\n * and their testdata. Do not edit. */\n\n")
	fmt.Fprintf(w, "#ifndef BURROW_TESTS_DEBUG_TEST_GEN_H\n#define BURROW_TESTS_DEBUG_TEST_GEN_H\n\n")
	for i, f := range fs {
		fmt.Fprintf(w, "/* %s, %d bytes. */\n", f.name, len(f.raw))
		writeBase64(w, fmt.Sprintf("debug_file_%d", i), f.raw)
		fmt.Fprintf(w, "static const char *const debug_dump_%d[] = {\n", i)
		for _, line := range strings.SplitAfter(string(f.dump), "\n") {
			if line == "" {
				continue
			}
			fmt.Fprintf(w, "    %s,\n", cstr(strings.TrimSuffix(line, "\n")))
		}
		fmt.Fprintf(w, "    NULL,\n};\n\n")
		fmt.Fprintf(w, "static const DebugDamage debug_damage_%d[] = {\n", i)
		for _, d := range f.damage {
			fmt.Fprintf(w, "    {%d, %d, \"%s\"},\n", d.pos, d.xor, d.hash)
		}
		fmt.Fprintf(w, "    {-1, 0, NULL},\n};\n\n")
	}
	fmt.Fprintf(w, "static const DebugFile debug_files[] = {\n")
	for i, f := range fs {
		fmt.Fprintf(w, "    {%s, %t, debug_file_%d, debug_dump_%d, debug_damage_%d},\n", cstr(f.name), f.gz, i, i, i)
	}
	fmt.Fprintf(w, "};\n\n")

	fmt.Fprintf(w, "static const DebugName debug_names[] = {\n")
	for _, r := range names() {
		ss := r[2].([2]string)
		fmt.Fprintf(w, "    {%d, %dLL, %s, %s},\n", r[0], r[1], cstr(ss[0]), cstr(ss[1]))
	}
	fmt.Fprintf(w, "};\n\n#endif\n")
}
GO

cat > "$tmp/macho.go" <<'GO'
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"

	"gen/macho"
)

func dumpMachoFile(w *bytes.Buffer, f *macho.File) {
	h := f.FileHeader
	fmt.Fprintf(w, "header magic=%#x cpu=%s subcpu=%#x type=%s ncmd=%d cmdsz=%d flags=%#x byteorder=%s\n",
		h.Magic, h.Cpu.String(), h.SubCpu, h.Type.String(), h.Ncmd, h.Cmdsz, h.Flags,
		f.ByteOrder.String())
	for i, l := range f.Loads {
		fmt.Fprintf(w, "load %d raw %s\n", i, blob(l.Raw(), nil))
		switch l := l.(type) {
		case *macho.Segment:
			sh := l.SegmentHeader
			fmt.Fprintf(w, "  segment cmd=%s len=%d %q addr=%#x memsz=%#x offset=%#x filesz=%#x maxprot=%#x prot=%#x nsect=%d flag=%#x\n",
				sh.Cmd.String(), sh.Len, sh.Name, sh.Addr, sh.Memsz, sh.Offset, sh.Filesz,
				sh.Maxprot, sh.Prot, sh.Nsect, sh.Flag)
			b, err := l.Data()
			fmt.Fprintf(w, "  data %s\n", blob(b, err))
			b, err = io.ReadAll(l.Open())
			fmt.Fprintf(w, "  open %s\n", blob(b, err))
			p := make([]byte, 16)
			n, err := l.ReadAt(p, 8)
			fmt.Fprintf(w, "  readat n=%d %x%s\n", n, p[:n], errs(err))
		case *macho.Dylib:
			fmt.Fprintf(w, "  dylib %q time=%d current=%#x compat=%#x\n", l.Name, l.Time,
				l.CurrentVersion, l.CompatVersion)
		case *macho.Symtab:
			c := l.SymtabCmd
			fmt.Fprintf(w, "  symtab cmd=%d len=%d symoff=%#x nsyms=%d stroff=%#x strsize=%d syms=%d\n",
				c.Cmd, c.Len, c.Symoff, c.Nsyms, c.Stroff, c.Strsize, len(l.Syms))
		case *macho.Dysymtab:
			c := l.DysymtabCmd
			fmt.Fprintf(w, "  dysymtab")
			for _, v := range []uint32{uint32(c.Cmd), c.Len, c.Ilocalsym, c.Nlocalsym,
				c.Iextdefsym, c.Nextdefsym, c.Iundefsym, c.Nundefsym, c.Tocoffset, c.Ntoc,
				c.Modtaboff, c.Nmodtab, c.Extrefsymoff, c.Nextrefsyms, c.Indirectsymoff,
				c.Nindirectsyms, c.Extreloff, c.Nextrel, c.Locreloff, c.Nlocrel} {
				fmt.Fprintf(w, " %d", v)
			}
			fmt.Fprintf(w, "\n  indirect n=%d", len(l.IndirectSyms))
			for _, x := range l.IndirectSyms {
				fmt.Fprintf(w, " %#x", x)
			}
			fmt.Fprintf(w, "\n")
		case *macho.Rpath:
			fmt.Fprintf(w, "  rpath %q\n", l.Path)
		case macho.LoadBytes:
			fmt.Fprintf(w, "  bytes\n")
		}
	}
	for i, s := range f.Sections {
		sh := s.SectionHeader
		fmt.Fprintf(w, "section %d %q seg=%q addr=%#x size=%#x offset=%#x align=%d reloff=%#x nreloc=%d flags=%#x\n",
			i, sh.Name, sh.Seg, sh.Addr, sh.Size, sh.Offset, sh.Align, sh.Reloff, sh.Nreloc,
			sh.Flags)
		b, err := s.Data()
		fmt.Fprintf(w, "  data %s\n", blob(b, err))
		b, err = io.ReadAll(s.Open())
		fmt.Fprintf(w, "  open %s\n", blob(b, err))
		p := make([]byte, 16)
		n, err := s.ReadAt(p, 4)
		fmt.Fprintf(w, "  readat n=%d %x%s\n", n, p[:n], errs(err))
		for _, r := range s.Relocs {
			fmt.Fprintf(w, "  reloc addr=%#x value=%#x type=%d len=%d pcrel=%t extern=%t scattered=%t\n",
				r.Addr, r.Value, r.Type, r.Len, r.Pcrel, r.Extern, r.Scattered)
		}
	}
	if f.Symtab != nil {
		fmt.Fprintf(w, "symbols n=%d\n", len(f.Symtab.Syms))
		for i, s := range f.Symtab.Syms {
			fmt.Fprintf(w, "  %d %q type=%#x sect=%d desc=%#x value=%#x\n", i, s.Name, s.Type,
				s.Sect, s.Desc, s.Value)
		}
	}
	fmt.Fprintf(w, "dysymtab %t\n", f.Dysymtab != nil)
	for _, name := range machoSegmentNames {
		if s := f.Segment(name); s != nil {
			fmt.Fprintf(w, "segment %q addr=%#x\n", name, s.Addr)
		} else {
			fmt.Fprintf(w, "segment %q nil\n", name)
		}
	}
	for _, name := range machoSectionNames {
		if s := f.Section(name); s != nil {
			fmt.Fprintf(w, "section %q addr=%#x\n", name, s.Addr)
		} else {
			fmt.Fprintf(w, "section %q nil\n", name)
		}
	}
	imp, err := f.ImportedSymbols()
	dumpStrings(w, "imported", imp, err)
	libs, err := f.ImportedLibraries()
	dumpStrings(w, "libraries", libs, err)
	d, err := f.DWARF()
	if err != nil {
		fmt.Fprintf(w, "dwarf err=%q\n", err.Error())
		return
	}
	dumpDWARF(w, d)
}

var machoSegmentNames = []string{"__TEXT", "__DATA", "__LINKEDIT", "__PAGEZERO", "__DWARF", ""}
var machoSectionNames = []string{"__text", "__data", "__cstring", "__debug_info", "__nl_symbol_ptr", ""}

func dumpMacho(w *bytes.Buffer, data []byte) {
	ff, err := macho.NewFatFile(bytes.NewReader(data))
	if err != nil {
		fmt.Fprintf(w, "fat err=%q notfat=%t\n", err.Error(), err == macho.ErrNotFat)
	} else {
		fmt.Fprintf(w, "fat magic=%#x arches=%d\n", ff.Magic, len(ff.Arches))
		for i, a := range ff.Arches {
			h := a.FatArchHeader
			fmt.Fprintf(w, "arch %d cpu=%s subcpu=%#x offset=%#x size=%#x align=%d\n", i,
				h.Cpu.String(), h.SubCpu, h.Offset, h.Size, h.Align)
			dumpMachoFile(w, a.File)
		}
	}
	f, err := macho.NewFile(bytes.NewReader(data))
	if err != nil {
		fmt.Fprintf(w, "newfile err=%q\n", err.Error())
		return
	}
	dumpMachoFile(w, f)
}

func safeMachoDump(data []byte) (out []byte, ok bool) {
	defer func() {
		if recover() != nil {
			ok = false
		}
	}()
	var w bytes.Buffer
	dumpMacho(&w, data)
	return w.Bytes(), true
}

func machoDamages(data []byte, seed uint64) []damage {
	var ds []damage
	x := seed
	n := len(data)
	add := func(pos int, xor byte) {
		b := append([]byte(nil), data...)
		if xor == 0 {
			b = b[:pos]
		} else {
			b[pos] ^= xor
		}
		out, ok := safeMachoDump(b)
		if !ok {
			return
		}
		ds = append(ds, damage{pos, xor, fmt.Sprintf("%x", sha256.Sum256(out))[:16]})
	}
	// The file header, then the load commands after it, then anywhere.
	for i := 0; i < 12; i++ {
		x = lcg(x)
		add(int(x>>33)%min(32, n), byte(1<<((x>>20)%8)))
	}
	if n > 32 {
		cmds := min(n, 32+int(uint32(data[20])|uint32(data[21])<<8|uint32(data[22])<<16))
		for i := 0; i < 24; i++ {
			x = lcg(x)
			add(28+int(x>>33)%(cmds-28), byte(1<<((x>>20)%8)))
		}
	}
	for i := 0; i < 16; i++ {
		x = lcg(x)
		xor := byte(x >> 56)
		if xor == 0 {
			xor = 0x80
		}
		add(int(x>>33)%n, xor)
	}
	for _, cut := range []int{n / 2, n - 1, n * 3 / 4, 100, 30} {
		if cut > 0 && cut < n {
			add(cut, 0)
		}
	}
	return ds
}

var machoKinds = []struct {
	name string
	f    func(int64) (string, string)
}{
	{"Type", func(v int64) (string, string) { x := macho.Type(v); return x.String(), x.GoString() }},
	{"Cpu", func(v int64) (string, string) { x := macho.Cpu(v); return x.String(), x.GoString() }},
	{"LoadCmd", func(v int64) (string, string) { x := macho.LoadCmd(v); return x.String(), x.GoString() }},
	{"RelocTypeGeneric", func(v int64) (string, string) { x := macho.RelocTypeGeneric(v); return x.String(), x.GoString() }},
	{"RelocTypeX86_64", func(v int64) (string, string) { x := macho.RelocTypeX86_64(v); return x.String(), x.GoString() }},
	{"RelocTypeARM", func(v int64) (string, string) { x := macho.RelocTypeARM(v); return x.String(), x.GoString() }},
	{"RelocTypeARM64", func(v int64) (string, string) { x := macho.RelocTypeARM64(v); return x.String(), x.GoString() }},
}

// machoNames is names for debug/macho. The relocation types are ints made
// with iota, so they get negative numbers and the int32 limits instead.
func machoNames() [][3]any {
	vals := constValues("macho.go.txt")
	vals["Cpu"] = append(vals["Cpu"], 0x1000007, 0x100000c, 0x1000012, 0x1000000)
	var rows [][3]any
	for ki, k := range machoKinds {
		set := map[int64]bool{}
		for v := int64(0); v < 40; v++ {
			set[v] = true
		}
		for _, v := range vals[k.name] {
			set[v] = true
			set[v+1] = true
			if v > 0 {
				set[v-1] = true
			}
		}
		lo, hi := int64(0), int64(1)<<32
		if strings.HasPrefix(k.name, "Reloc") {
			lo, hi = -1<<31, 1<<31
			set[-1] = true
			set[-100] = true
			set[lo] = true
		}
		set[hi-1] = true
		var vs []int64
		for v := range set {
			if v >= lo && v < hi {
				vs = append(vs, v)
			}
		}
		sort.Slice(vs, func(i, j int) bool { return vs[i] < vs[j] })
		for _, v := range vs {
			s, g := k.f(v)
			rows = append(rows, [3]any{ki, v, [2]string{s, g}})
		}
	}
	return rows
}

func machoMain() {
	var fs []*file
	for _, f := range load("machodata") {
		b, err := io.ReadAll(base64.NewDecoder(base64.StdEncoding, bytes.NewReader(f.raw)))
		if err != nil {
			panic(err)
		}
		f.name = "macho/" + strings.TrimSuffix(f.name, ".base64")
		f.raw, f.data = b, b
		fs = append(fs, f)
	}
	for _, f := range load("machodwarf") {
		f.name = "dwarf/" + f.name
		fs = append(fs, f)
	}
	for i, f := range fs {
		out, ok := safeMachoDump(f.data)
		if !ok {
			panic("Go panics on " + f.name)
		}
		f.dump = out
		f.damage = machoDamages(f.data, uint64(i)+1)
	}

	w := os.Stdout
	fmt.Fprintf(w, "/* Generated by tools/gen-debug-tests.sh macho from Go's debug/macho and\n * debug/dwarf and their testdata. Do not edit. */\n\n")
	fmt.Fprintf(w, "#ifndef BURROW_TESTS_MACHO_TEST_GEN_H\n#define BURROW_TESTS_MACHO_TEST_GEN_H\n\n")
	for i, f := range fs {
		fmt.Fprintf(w, "/* %s, %d bytes. */\n", f.name, len(f.raw))
		writeBase64(w, fmt.Sprintf("macho_file_%d", i), f.raw)
		fmt.Fprintf(w, "static const char *const macho_dump_%d[] = {\n", i)
		for _, line := range strings.SplitAfter(string(f.dump), "\n") {
			if line == "" {
				continue
			}
			fmt.Fprintf(w, "    %s,\n", cstr(strings.TrimSuffix(line, "\n")))
		}
		fmt.Fprintf(w, "    NULL,\n};\n\n")
		fmt.Fprintf(w, "static const DebugDamage macho_damage_%d[] = {\n", i)
		for _, d := range f.damage {
			fmt.Fprintf(w, "    {%d, %d, \"%s\"},\n", d.pos, d.xor, d.hash)
		}
		fmt.Fprintf(w, "    {-1, 0, NULL},\n};\n\n")
	}
	fmt.Fprintf(w, "static const DebugFile macho_files[] = {\n")
	for i, f := range fs {
		fmt.Fprintf(w, "    {%s, false, macho_file_%d, macho_dump_%d, macho_damage_%d},\n", cstr(f.name), i, i, i)
	}
	fmt.Fprintf(w, "};\n\n")

	fmt.Fprintf(w, "static const DebugName macho_names[] = {\n")
	for _, r := range machoNames() {
		ss := r[2].([2]string)
		fmt.Fprintf(w, "    {%d, %dLL, %s, %s},\n", r[0], r[1], cstr(ss[0]), cstr(ss[1]))
	}
	fmt.Fprintf(w, "};\n\n#endif\n")
}
GO

cat > "$tmp/pe.go" <<'GO'
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strings"

	"gen/pe"
)

func dumpOptional(w *bytes.Buffer, oh any) {
	var dd [16]pe.DataDirectory
	switch h := oh.(type) {
	case nil:
		fmt.Fprintf(w, "optional nil\n")
		return
	case *pe.OptionalHeader32:
		fmt.Fprintf(w, "optional32 magic=%#x linker=%d.%d code=%#x idata=%#x udata=%#x entry=%#x codebase=%#x database=%#x image=%#x\n",
			h.Magic, h.MajorLinkerVersion, h.MinorLinkerVersion, h.SizeOfCode,
			h.SizeOfInitializedData, h.SizeOfUninitializedData, h.AddressOfEntryPoint,
			h.BaseOfCode, h.BaseOfData, h.ImageBase)
		fmt.Fprintf(w, "  align=%#x/%#x os=%d.%d image=%d.%d subsystem=%d.%d win32=%d size=%#x headers=%#x sum=%#x\n",
			h.SectionAlignment, h.FileAlignment, h.MajorOperatingSystemVersion,
			h.MinorOperatingSystemVersion, h.MajorImageVersion, h.MinorImageVersion,
			h.MajorSubsystemVersion, h.MinorSubsystemVersion, h.Win32VersionValue,
			h.SizeOfImage, h.SizeOfHeaders, h.CheckSum)
		fmt.Fprintf(w, "  subsystem=%d dll=%#x stack=%#x/%#x heap=%#x/%#x loader=%#x ndd=%d\n",
			h.Subsystem, h.DllCharacteristics, h.SizeOfStackReserve, h.SizeOfStackCommit,
			h.SizeOfHeapReserve, h.SizeOfHeapCommit, h.LoaderFlags, h.NumberOfRvaAndSizes)
		dd = h.DataDirectory
	case *pe.OptionalHeader64:
		fmt.Fprintf(w, "optional64 magic=%#x linker=%d.%d code=%#x idata=%#x udata=%#x entry=%#x codebase=%#x image=%#x\n",
			h.Magic, h.MajorLinkerVersion, h.MinorLinkerVersion, h.SizeOfCode,
			h.SizeOfInitializedData, h.SizeOfUninitializedData, h.AddressOfEntryPoint,
			h.BaseOfCode, h.ImageBase)
		fmt.Fprintf(w, "  align=%#x/%#x os=%d.%d image=%d.%d subsystem=%d.%d win32=%d size=%#x headers=%#x sum=%#x\n",
			h.SectionAlignment, h.FileAlignment, h.MajorOperatingSystemVersion,
			h.MinorOperatingSystemVersion, h.MajorImageVersion, h.MinorImageVersion,
			h.MajorSubsystemVersion, h.MinorSubsystemVersion, h.Win32VersionValue,
			h.SizeOfImage, h.SizeOfHeaders, h.CheckSum)
		fmt.Fprintf(w, "  subsystem=%d dll=%#x stack=%#x/%#x heap=%#x/%#x loader=%#x ndd=%d\n",
			h.Subsystem, h.DllCharacteristics, h.SizeOfStackReserve, h.SizeOfStackCommit,
			h.SizeOfHeapReserve, h.SizeOfHeapCommit, h.LoaderFlags, h.NumberOfRvaAndSizes)
		dd = h.DataDirectory
	}
	for i, d := range dd {
		if d.VirtualAddress != 0 || d.Size != 0 {
			fmt.Fprintf(w, "  dd %d va=%#x size=%#x\n", i, d.VirtualAddress, d.Size)
		}
	}
}

var peSectionNames = []string{".text", ".data", ".bss", ".idata", ".debug_info", ".rdata", ""}

func dumpPEFile(w *bytes.Buffer, f *pe.File) {
	h := f.FileHeader
	fmt.Fprintf(w, "header machine=%#x nsec=%d time=%d symptr=%#x nsyms=%d optsize=%d chars=%#x\n",
		h.Machine, h.NumberOfSections, h.TimeDateStamp, h.PointerToSymbolTable,
		h.NumberOfSymbols, h.SizeOfOptionalHeader, h.Characteristics)
	dumpOptional(w, f.OptionalHeader)
	for i, s := range f.Sections {
		sh := s.SectionHeader
		fmt.Fprintf(w, "section %d %q vsize=%#x va=%#x size=%#x offset=%#x relptr=%#x lineptr=%#x nrel=%d nline=%d chars=%#x\n",
			i, sh.Name, sh.VirtualSize, sh.VirtualAddress, sh.Size, sh.Offset,
			sh.PointerToRelocations, sh.PointerToLineNumbers, sh.NumberOfRelocations,
			sh.NumberOfLineNumbers, sh.Characteristics)
		b, err := s.Data()
		fmt.Fprintf(w, "  data %s\n", blob(b, err))
		b, err = io.ReadAll(s.Open())
		fmt.Fprintf(w, "  open %s\n", blob(b, err))
		p := make([]byte, 16)
		n, err := s.ReadAt(p, 4)
		fmt.Fprintf(w, "  readat n=%d %x%s\n", n, p[:n], errs(err))
		for _, r := range s.Relocs {
			fmt.Fprintf(w, "  reloc va=%#x sym=%d type=%d\n", r.VirtualAddress,
				r.SymbolTableIndex, r.Type)
		}
	}
	st := f.StringTable
	fmt.Fprintf(w, "strings %s\n", blob(st, nil))
	for _, off := range []uint32{0, 3, 4, 5, 8, uint32(len(st)) + 3, uint32(len(st)) + 4,
		uint32(len(st)) + 5} {
		s, err := st.String(off)
		fmt.Fprintf(w, "  string %d %q%s\n", off, s, errs(err))
	}
	fmt.Fprintf(w, "coff n=%d\n", len(f.COFFSymbols))
	for i := range f.COFFSymbols {
		s := &f.COFFSymbols[i]
		name, err := s.FullName(st)
		fmt.Fprintf(w, "  %d %x %q%s value=%#x sect=%d type=%#x class=%d naux=%d\n", i,
			s.Name, name, errs(err), s.Value, s.SectionNumber, s.Type, s.StorageClass,
			s.NumberOfAuxSymbols)
	}
	idx := []int{-1, 0, len(f.COFFSymbols)}
	for i, s := range f.COFFSymbols {
		if s.StorageClass == 3 {
			idx = append(idx, i)
		}
	}
	for _, i := range idx {
		a, err := f.COFFSymbolReadSectionDefAux(i)
		if err != nil {
			fmt.Fprintf(w, "aux %d err=%q\n", i, err.Error())
			continue
		}
		fmt.Fprintf(w, "aux %d %+v\n", i, *a)
	}
	fmt.Fprintf(w, "symbols n=%d\n", len(f.Symbols))
	for i, s := range f.Symbols {
		fmt.Fprintf(w, "  %d %q value=%#x sect=%d type=%#x class=%d\n", i, s.Name, s.Value,
			s.SectionNumber, s.Type, s.StorageClass)
	}
	for _, name := range peSectionNames {
		if s := f.Section(name); s != nil {
			fmt.Fprintf(w, "section %q va=%#x\n", name, s.VirtualAddress)
		} else {
			fmt.Fprintf(w, "section %q nil\n", name)
		}
	}
	imp, err := f.ImportedSymbols()
	dumpStrings(w, "imported", imp, err)
	libs, err := f.ImportedLibraries()
	dumpStrings(w, "libraries", libs, err)
	d, err := f.DWARF()
	if err != nil {
		fmt.Fprintf(w, "dwarf err=%q\n", err.Error())
		return
	}
	// The DWARF dump is the one the ELF and Mach-O tests check line by line,
	// so only its hash is kept here.
	var dw bytes.Buffer
	dumpDWARF(&dw, d)
	fmt.Fprintf(w, "dwarf %s\n", blob(dw.Bytes(), nil))
}

func dumpPE(w *bytes.Buffer, data []byte) {
	f, err := pe.NewFile(bytes.NewReader(data))
	if err != nil {
		fmt.Fprintf(w, "newfile err=%q\n", err.Error())
		return
	}
	dumpPEFile(w, f)
}

func safePEDump(data []byte) (out []byte, ok bool) {
	defer func() {
		if recover() != nil {
			ok = false
		}
	}()
	var w bytes.Buffer
	dumpPE(&w, data)
	return w.Bytes(), true
}

func peDamages(data []byte, seed uint64) []damage {
	var ds []damage
	x := seed
	n := len(data)
	add := func(pos int, xor byte) {
		b := append([]byte(nil), data...)
		if xor == 0 {
			b = b[:pos]
		} else {
			b[pos] ^= xor
		}
		out, ok := safePEDump(b)
		if !ok {
			return
		}
		ds = append(ds, damage{pos, xor, fmt.Sprintf("%x", sha256.Sum256(out))[:16]})
	}
	// The COFF header, which is after the MS-DOS stub in an image, then the
	// optional header and the section headers after it, then anywhere.
	base := 0
	if n >= 0x40 && data[0] == 'M' && data[1] == 'Z' {
		base = int(binary.LittleEndian.Uint32(data[0x3c:])) + 4
	}
	if base+20 <= n {
		for i := 0; i < 12; i++ {
			x = lcg(x)
			add(base+int(x>>33)%20, byte(1<<((x>>20)%8)))
		}
		nsec := int(binary.LittleEndian.Uint16(data[base+2:]))
		optsz := int(binary.LittleEndian.Uint16(data[base+16:]))
		end := min(n, base+20+optsz+40*nsec)
		if end > base+20 {
			for i := 0; i < 24; i++ {
				x = lcg(x)
				add(base+20+int(x>>33)%(end-base-20), byte(1<<((x>>20)%8)))
			}
		}
	}
	for i := 0; i < 16; i++ {
		x = lcg(x)
		xor := byte(x >> 56)
		if xor == 0 {
			xor = 0x80
		}
		add(int(x>>33)%n, xor)
	}
	for _, cut := range []int{n / 2, n - 1, n * 3 / 4, 100, 30} {
		if cut > 0 && cut < n {
			add(cut, 0)
		}
	}
	return ds
}

func peMain() {
	var fs []*file
	for _, f := range load("pedata") {
		f.name = "pe/" + f.name
		fs = append(fs, f)
	}
	for _, f := range load("pedwarf") {
		f.name = "dwarf/" + f.name
		fs = append(fs, f)
	}
	for i, f := range fs {
		out, ok := safePEDump(f.data)
		if !ok {
			panic("Go panics on " + f.name)
		}
		f.dump = out
		f.damage = peDamages(f.data, uint64(i)+1)
	}

	w := os.Stdout
	fmt.Fprintf(w, "/* Generated by tools/gen-debug-tests.sh pe from Go's debug/pe and\n * debug/dwarf and their testdata. Do not edit. */\n\n")
	fmt.Fprintf(w, "#ifndef BURROW_TESTS_PE_TEST_GEN_H\n#define BURROW_TESTS_PE_TEST_GEN_H\n\n")
	for i, f := range fs {
		fmt.Fprintf(w, "/* %s, %d bytes. */\n", f.name, len(f.raw))
		writeBase64(w, fmt.Sprintf("pe_file_%d", i), f.raw)
		fmt.Fprintf(w, "static const char *const pe_dump_%d[] = {\n", i)
		for _, line := range strings.SplitAfter(string(f.dump), "\n") {
			if line == "" {
				continue
			}
			fmt.Fprintf(w, "    %s,\n", cstr(strings.TrimSuffix(line, "\n")))
		}
		fmt.Fprintf(w, "    NULL,\n};\n\n")
		fmt.Fprintf(w, "static const DebugDamage pe_damage_%d[] = {\n", i)
		for _, d := range f.damage {
			fmt.Fprintf(w, "    {%d, %d, \"%s\"},\n", d.pos, d.xor, d.hash)
		}
		fmt.Fprintf(w, "    {-1, 0, NULL},\n};\n\n")
	}
	fmt.Fprintf(w, "static const DebugFile pe_files[] = {\n")
	for i, f := range fs {
		fmt.Fprintf(w, "    {%s, false, pe_file_%d, pe_dump_%d, pe_damage_%d},\n", cstr(f.name), i, i, i)
	}
	fmt.Fprintf(w, "};\n\n#endif\n")
}
GO

cat > "$tmp/plan9obj.go" <<'GO'
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strings"

	"gen/plan9obj"
)

var plan9SectionNames = []string{"text", "data", "syms", "spsz", "pcsz", "bss", ""}

func dumpPlan9(w *bytes.Buffer, data []byte) {
	f, err := plan9obj.NewFile(bytes.NewReader(data))
	if err != nil {
		fmt.Fprintf(w, "newfile err=%q\n", err.Error())
		return
	}
	h := f.FileHeader
	fmt.Fprintf(w, "header magic=%#x bss=%#x entry=%#x ptrsize=%d load=%#x hdrsize=%d\n",
		h.Magic, h.Bss, h.Entry, h.PtrSize, h.LoadAddress, h.HdrSize)
	for i, s := range f.Sections {
		fmt.Fprintf(w, "section %d %q size=%#x offset=%#x\n", i, s.Name, s.Size, s.Offset)
		b, err := s.Data()
		fmt.Fprintf(w, "  data %s\n", blob(b, err))
		b, err = io.ReadAll(s.Open())
		fmt.Fprintf(w, "  open %s\n", blob(b, err))
		var buf [8]byte
		n, err := s.ReadAt(buf[:], 4)
		fmt.Fprintf(w, "  readat n=%d % x%s\n", n, buf[:n], errs(err))
	}
	for _, name := range plan9SectionNames {
		if s := f.Section(name); s != nil {
			fmt.Fprintf(w, "section %q offset=%#x\n", name, s.Offset)
		} else {
			fmt.Fprintf(w, "section %q nil\n", name)
		}
	}
	syms, err := f.Symbols()
	fmt.Fprintf(w, "symbols n=%d%s\n", len(syms), errs(err))
	for _, s := range syms {
		fmt.Fprintf(w, "  %#x %d %q\n", s.Value, s.Type, s.Name)
	}
}

func safePlan9Dump(data []byte) (out []byte, ok bool) {
	defer func() {
		if recover() != nil {
			ok = false
		}
	}()
	var w bytes.Buffer
	dumpPlan9(&w, data)
	return w.Bytes(), true
}

// plan9Entry is where one entry of a symbol table is in the file.
type plan9Entry struct {
	value int // the offset of the value
	typ   int // the offset of the type byte
	name  int // the offset of the name
	t     byte
}

// plan9Entries walks the symbol table the way walksymtab does and gives where
// each entry is, so the damage can aim at types, file codes and paths.
func plan9Entries(data []byte) []plan9Entry {
	if len(data) < 40 {
		return nil
	}
	hdr, ptrsz := 32, 4
	if binary.BigEndian.Uint32(data)&plan9obj.Magic64 != 0 {
		hdr, ptrsz = 40, 8
	}
	text := int(binary.BigEndian.Uint32(data[4:]))
	dat := int(binary.BigEndian.Uint32(data[8:]))
	syms := int(binary.BigEndian.Uint32(data[16:]))
	start := hdr + text + dat
	if start+syms > len(data) {
		return nil
	}
	var es []plan9Entry
	p := start
	end := start + syms
	for end-p >= 4 && end-p >= ptrsz+1 {
		e := plan9Entry{value: p, typ: p + ptrsz, t: data[p+ptrsz] & 0x7f}
		p += ptrsz + 1
		e.name = p
		i := bytes.IndexByte(data[p:end], 0)
		if i < 0 {
			break
		}
		p += i + 1
		if e.t == 'z' || e.t == 'Z' {
			for p+2 <= end && !(data[p] == 0 && data[p+1] == 0) {
				p += 2
			}
			p += 2
		}
		es = append(es, e)
	}
	return es
}

func plan9Damages(data []byte, seed uint64) []damage {
	var ds []damage
	x := seed
	n := len(data)
	add := func(pos int, xor byte) {
		if pos < 0 || pos >= n || (xor == 0 && pos == 0) {
			return
		}
		b := append([]byte(nil), data...)
		if xor == 0 {
			b = b[:pos]
		} else {
			b[pos] ^= xor
		}
		out, ok := safePlan9Dump(b)
		if !ok {
			return
		}
		ds = append(ds, damage{pos, xor, fmt.Sprintf("%x", sha256.Sum256(out))[:16]})
	}
	// The header, then the symbol table, aiming at the type letters, the
	// codes of the file names and the codes in the paths that use them.
	for i := 0; i < 24; i++ {
		x = lcg(x)
		add(int(x>>33)%40, byte(1<<((x>>20)%8)))
	}
	es := plan9Entries(data)
	var files, paths []plan9Entry
	for _, e := range es {
		switch e.t {
		case 'f':
			files = append(files, e)
		case 'z', 'Z':
			paths = append(paths, e)
		}
	}
	if len(es) > 0 {
		for _, t := range []byte{'z', 'Z', 'f', 'z', 'Z', 'f', 'T'} {
			x = lcg(x)
			e := es[int(x>>33)%len(es)]
			if xor := data[e.typ] ^ t; xor != 0 {
				add(e.typ, xor)
			}
		}
		// The top bit of a type is dropped, so this one changes nothing.
		x = lcg(x)
		add(es[int(x>>33)%len(es)].typ, 0x80)
		for i := 0; i < 16; i++ {
			x = lcg(x)
			e := es[int(x>>33)%len(es)]
			add(e.name+int(x>>20)%3, byte(x>>40)|1)
		}
	}
	for i := 0; i < 6 && len(files) > 0; i++ {
		x = lcg(x)
		e := files[int(x>>33)%len(files)]
		ptrsz := e.typ - e.value
		add(e.value+ptrsz-1, byte(1<<((x>>20)%8)))
	}
	for i := 0; i < 6 && len(paths) > 0; i++ {
		x = lcg(x)
		e := paths[int(x>>33)%len(paths)]
		add(e.name+1+int(x>>20)%4, byte(1<<((x>>24)%8)))
	}
	for i := 0; i < 12; i++ {
		x = lcg(x)
		xor := byte(x >> 56)
		if xor == 0 {
			xor = 0x80
		}
		add(int(x>>33)%n, xor)
	}
	cuts := []int{n / 2, n - 1, n * 3 / 4, 100, 36, 30, 3}
	if len(es) > 2 {
		e := es[len(es)/2]
		cuts = append(cuts, e.value+1, e.typ, e.name+1)
		e = es[len(es)-1]
		cuts = append(cuts, e.typ, e.name+1)
	}
	for _, cut := range cuts {
		if cut > 0 && cut < n {
			add(cut, 0)
		}
	}
	return ds
}

func plan9Main() {
	fs := load("plan9data")
	for i, f := range fs {
		f.name = "plan9obj/" + f.name
		out, ok := safePlan9Dump(f.data)
		if !ok {
			panic("Go panics on " + f.name)
		}
		f.dump = out
		f.damage = plan9Damages(f.data, uint64(i)+1)
	}

	w := os.Stdout
	fmt.Fprintf(w, "/* Generated by tools/gen-debug-tests.sh plan9obj from Go's debug/plan9obj\n * and its testdata. Do not edit. */\n\n")
	fmt.Fprintf(w, "#ifndef BURROW_TESTS_PLAN9OBJ_TEST_GEN_H\n#define BURROW_TESTS_PLAN9OBJ_TEST_GEN_H\n\n")
	for i, f := range fs {
		fmt.Fprintf(w, "/* %s, %d bytes. */\n", f.name, len(f.raw))
		writeBase64(w, fmt.Sprintf("plan9obj_file_%d", i), f.raw)
		fmt.Fprintf(w, "static const char *const plan9obj_dump_%d[] = {\n", i)
		for _, line := range strings.SplitAfter(string(f.dump), "\n") {
			if line == "" {
				continue
			}
			fmt.Fprintf(w, "    %s,\n", cstr(strings.TrimSuffix(line, "\n")))
		}
		fmt.Fprintf(w, "    NULL,\n};\n\n")
		fmt.Fprintf(w, "static const DebugDamage plan9obj_damage_%d[] = {\n", i)
		for _, d := range f.damage {
			fmt.Fprintf(w, "    {%d, %d, \"%s\"},\n", d.pos, d.xor, d.hash)
		}
		fmt.Fprintf(w, "    {-1, 0, NULL},\n};\n\n")
	}
	fmt.Fprintf(w, "static const DebugFile plan9obj_files[] = {\n")
	for i, f := range fs {
		fmt.Fprintf(w, "    {%s, false, plan9obj_file_%d, plan9obj_dump_%d, plan9obj_damage_%d},\n", cstr(f.name), i, i, i)
	}
	fmt.Fprintf(w, "};\n\n#endif\n")
}
GO

cat > "$tmp/gosym.go" <<'GO'
package main

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	stdelf "debug/elf"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"

	"gen/gosym"
)

// A test file for debug/gosym is a container for the three things NewTable
// and NewLineTable take: the text address, eight bytes little-endian, then
// the length of the symbol table, four bytes, then the symbol table, then the
// pclntab to the end.
func gosymContainer(text uint64, symtab, pcln []byte) []byte {
	b := make([]byte, 12, 12+len(symtab)+len(pcln))
	binary.LittleEndian.PutUint64(b, text)
	binary.LittleEndian.PutUint32(b[8:], uint32(len(symtab)))
	b = append(b, symtab...)
	return append(b, pcln...)
}

// gosymList prints a hash of all the lines and then some of them: the first
// dozen, every 128th and the last.
func gosymList(w *bytes.Buffer, label string, lines []string) {
	h := sha256.New()
	for _, l := range lines {
		io.WriteString(h, l)
		io.WriteString(h, "\n")
	}
	fmt.Fprintf(w, "%s n=%d sha=%x\n", label, len(lines), h.Sum(nil)[:8])
	for i, l := range lines {
		if i < 12 || i%128 == 0 || i == len(lines)-1 {
			fmt.Fprintf(w, "  %s\n", l)
		}
	}
}

func gosymTry(f func()) (panicked bool) {
	defer func() {
		if recover() != nil {
			panicked = true
		}
	}()
	f()
	return false
}

func dumpGosym(w *bytes.Buffer, data []byte) {
	if len(data) < 12 {
		fmt.Fprintf(w, "short %d\n", len(data))
		return
	}
	text := binary.LittleEndian.Uint64(data)
	l := int(binary.LittleEndian.Uint32(data[8:]))
	if l > len(data)-12 {
		fmt.Fprintf(w, "short %d symtab=%d\n", len(data), l)
		return
	}
	symtab := make([]byte, l)
	copy(symtab, data[12:])
	pcln := data[12+l:]
	fmt.Fprintf(w, "text=%#x symtab=%d pcln=%d\n", text, l, len(pcln))
	lt := gosym.NewLineTable(pcln, text)
	fmt.Fprintf(w, "linetable %s\n", lt.Info())
	for _, pc := range []uint64{text - 1, text, text + 1, text + 2, text + 5, text + 16,
		text + 64, text + 0x100, text + 0x1000} {
		fmt.Fprintf(w, "lt pc %#x line=%d\n", pc, lt.PCToLine(pc))
	}
	for _, line := range []int{-1, 0, 1, 2, 3, 5, 10, 20, 50, 100} {
		fmt.Fprintf(w, "lt line %d pc=%#x\n", line, lt.LineToPC(line, text+0x10000))
	}

	var tab *gosym.Table
	var err error
	if gosymTry(func() { tab, err = gosym.NewTable(symtab, lt) }) {
		fmt.Fprintf(w, "newtable panic\n")
		return
	}
	if err != nil {
		fmt.Fprintf(w, "newtable err=%q\n", err.Error())
		return
	}
	fmt.Fprintf(w, "table go12=%t\n", tab.Go12())
	fi := func(f *gosym.Func) int {
		for i := range tab.Funcs {
			if f == &tab.Funcs[i] {
				return i
			}
		}
		return -1
	}
	si := func(s *gosym.Sym) int {
		for i := range tab.Syms {
			if s == &tab.Syms[i] {
				return i
			}
		}
		return -1
	}
	oi := func(o *gosym.Obj) int {
		for i := range tab.Objs {
			if o == &tab.Objs[i] {
				return i
			}
		}
		return -1
	}

	var lines []string
	for i := range tab.Syms {
		s := &tab.Syms[i]
		lines = append(lines, fmt.Sprintf("%#x %d %q gotype=%#x func=%d static=%t pkg=%q recv=%q base=%q",
			s.Value, s.Type, s.Name, s.GoType, fi(s.Func), s.Static(), s.PackageName(),
			s.ReceiverName(), s.BaseName()))
	}
	gosymList(w, "syms", lines)

	lines = nil
	for i := range tab.Funcs {
		f := &tab.Funcs[i]
		var ps, ls []string
		for _, s := range f.Params {
			ps = append(ps, s.Name)
		}
		for _, s := range f.Locals {
			ls = append(ls, s.Name)
		}
		desc := "nil"
		switch {
		case f.LineTable == nil:
		case f.LineTable == lt:
			desc = "shared"
		default:
			desc = fmt.Sprintf("pc=%#x line=%d len=%d", f.LineTable.PC, f.LineTable.Line,
				len(f.LineTable.Data))
		}
		lines = append(lines, fmt.Sprintf("%#x-%#x %q frame=%d params=%q locals=%q obj=%d %s",
			f.Entry, f.End, f.Sym.Name, f.FrameSize, ps, ls, oi(f.Obj), desc))
	}
	gosymList(w, "funcs", lines)

	lines = nil
	for i := range tab.Objs {
		o := &tab.Objs[i]
		first := -1
		if len(o.Funcs) > 0 {
			first = fi(&o.Funcs[0])
		}
		var ps []string
		for _, s := range o.Paths {
			ps = append(ps, fmt.Sprintf("%#x:%s", s.Value, s.Name))
		}
		lines = append(lines, fmt.Sprintf("funcs=%d first=%d paths=%q", len(o.Funcs), first, ps))
	}
	gosymList(w, "objs", lines)

	var files []string
	for name := range tab.Files {
		files = append(files, name)
	}
	sort.Strings(files)
	lines = nil
	for _, name := range files {
		lines = append(lines, fmt.Sprintf("%q obj=%d", name, oi(tab.Files[name])))
	}
	gosymList(w, "files", lines)

	// PCToFunc, SymByAddr and PCToLine at the edges of some functions.
	pcs := []uint64{0, text - 1, text, ^uint64(0)}
	n := len(tab.Funcs)
	for i := range tab.Funcs {
		if i < 4 || i%256 == 0 || i == n-1 {
			f := &tab.Funcs[i]
			pcs = append(pcs, f.Entry, f.Entry+1, f.End-1, f.End)
		}
	}
	type fileLine struct {
		file string
		line int
	}
	var back []fileLine
	for _, pc := range pcs {
		name := "nil"
		if s := tab.SymByAddr(pc); s != nil {
			name = strconv.Quote(s.Name)
		}
		var file string
		var line int
		var fn *gosym.Func
		where := "panic"
		if !gosymTry(func() { file, line, fn = tab.PCToLine(pc) }) {
			where = fmt.Sprintf("%q:%d fn=%d", file, line, fi(fn))
			if fn != nil && len(back) < 3 {
				back = append(back, fileLine{file, line})
			}
		}
		fmt.Fprintf(w, "pc %#x func=%d sym=%s line %s\n", pc, fi(tab.PCToFunc(pc)), name, where)
	}

	// LineToPC for some of the files, a file that is not there, and the lines
	// PCToLine gave above.
	var tries []fileLine
	if len(files) > 0 {
		for _, k := range []int{0, len(files) / 2, len(files) - 1} {
			for _, line := range []int{0, 1, 9, 40} {
				tries = append(tries, fileLine{files[k], line})
			}
		}
	}
	tries = append(tries, fileLine{"nope.go", 1})
	tries = append(tries, back...)
	for _, tr := range tries {
		var pc uint64
		var fn *gosym.Func
		var err error
		if gosymTry(func() { pc, fn, err = tab.LineToPC(tr.file, tr.line) }) {
			fmt.Fprintf(w, "ltp %q:%d panic\n", tr.file, tr.line)
			continue
		}
		fmt.Fprintf(w, "ltp %q:%d pc=%#x fn=%d%s\n", tr.file, tr.line, pc, fi(fn), errs(err))
	}

	names := []string{"main.main", "runtime.main", "nope"}
	for i := 0; i < 2 && i < len(tab.Syms); i++ {
		names = append(names, tab.Syms[i].Name)
	}
	for i := 0; i < 2 && i < len(tab.Funcs); i++ {
		names = append(names, tab.Funcs[i].Sym.Name)
	}
	for _, name := range names {
		fmt.Fprintf(w, "lookup %q sym=%d func=%d\n", name, si(tab.LookupSym(name)),
			fi(tab.LookupFunc(name)))
	}

	// NewTable rewrote the names in symtab, so a second one sees them as they
	// are now.
	var tab2 *gosym.Table
	if gosymTry(func() { tab2, err = gosym.NewTable(symtab, lt) }) {
		fmt.Fprintf(w, "again panic\n")
		return
	}
	if err != nil {
		fmt.Fprintf(w, "again err=%q\n", err.Error())
		return
	}
	lines = nil
	for i := range tab2.Syms {
		lines = append(lines, strconv.Quote(tab2.Syms[i].Name))
	}
	gosymList(w, "again", lines)
}

func safeGosymDump(data []byte) (out []byte, ok bool) {
	defer func() {
		if recover() != nil {
			ok = false
		}
	}()
	var w bytes.Buffer
	dumpGosym(&w, data)
	return w.Bytes(), true
}

// gosymDamages damages the text address and the length, the symbol table,
// and with pcln set the pclntab, mostly its header and the function table
// after it. Each damaged copy is a new slice as long as the file, so a cut one
// keeps the capacity the C test gives it too.
func gosymDamages(data []byte, seed uint64, pcln bool) []damage {
	var ds []damage
	x := seed
	n := len(data)
	l := int(binary.LittleEndian.Uint32(data[8:]))
	p := 12 + l
	add := func(pos int, xor byte) {
		if pos < 0 || pos >= n || (xor == 0 && pos == 0) {
			return
		}
		b := make([]byte, n)
		copy(b, data)
		if xor == 0 {
			b = b[:pos]
		} else {
			b[pos] ^= xor
		}
		out, ok := safeGosymDump(b)
		if !ok {
			return
		}
		ds = append(ds, damage{pos, xor, fmt.Sprintf("%x", sha256.Sum256(out))[:16]})
	}
	set := func(pos int, v byte) {
		if pos < n && data[pos] != v {
			add(pos, data[pos]^v)
		}
	}
	for i := 0; i < 6; i++ {
		x = lcg(x)
		add(int(x>>33)%12, byte(1<<((x>>20)%8)))
	}
	if l > 0 {
		for i := 0; i < 24; i++ {
			x = lcg(x)
			add(12+int(x>>33)%l, byte(x>>40)|1)
		}
		for i := 0; i < 6; i++ {
			x = lcg(x)
			add(12+int(x>>33)%l, 0)
		}
		add(12+l/2, 0)
	}
	if m := n - p; pcln && m > 0 {
		// The magic as each of the others, then the quantum and the pointer
		// size.
		for _, v := range []byte{0xfb, 0xfa, 0xf0, 0xf1, 0xfe} {
			set(p, v)
		}
		for _, v := range []byte{1, 2, 3, 4} {
			set(p+6, v)
		}
		for _, v := range []byte{0, 4, 8} {
			set(p+7, v)
		}
		for i := 0; i < 24; i++ {
			x = lcg(x)
			add(p+int(x>>33)%min(m, 72), byte(1<<((x>>20)%8)))
		}
		for i := 0; i < 16; i++ {
			x = lcg(x)
			add(p+int(x>>33)%min(m, 4096), byte(x>>40)|1)
		}
		for i := 0; i < 12; i++ {
			x = lcg(x)
			add(p+int(x>>33)%m, byte(x>>40)|1)
		}
		for _, cut := range []int{m / 2, m - 1, m / 8, 72, 40, 16, 15, 8} {
			if cut > 0 && cut < m {
				add(p+cut, 0)
			}
		}
	}
	add(11, 0)
	add(4, 0)
	return ds
}

// A gosymEnt is a symbol for the symbol tables made up here, in the old
// formats and the newer one.
type gosymEnt struct {
	typ    byte
	value  uint64
	name   string
	codes  []uint16 // the path of a 'z' or 'Z' symbol
	gotype uint64
}

func gosymEnts(early bool) []gosymEnt {
	var es []gosymEnt
	if early {
		// A function before any path, which has no Obj.
		es = append(es, gosymEnt{'T', 0xff0, "main.early", nil, 0})
	}
	return append(es, []gosymEnt{
		{'f', 1, "/", nil, 0},
		{'f', 2, "tmp", nil, 0},
		{'f', 3, "a.go", nil, 0},
		{'f', 4, "b.go", nil, 0},
		{'f', 5, "c.go", nil, 0},
		{'f', 6, "pkg", nil, 0},
		{'Z', 1, "", []uint16{1, 2, 3}, 0},
		{'z', 100, "", []uint16{1, 2, 4}, 0},
		{'z', 180, "", nil, 0},
		{'z', 300, "", []uint16{2, 5}, 0},
		{'z', 340, "", nil, 0},
		{'T', 0x1000, "main.main", nil, 0x4000},
		{'m', 0x28, ".frame", nil, 0},
		{'p', 8, "x", nil, 0x4010},
		{'p', 16, "y", nil, 0x4010},
		{'a', 8, "tmp", nil, 0x4020},
		{'t', 0x1030, "main.helper·1", nil, 0},
		{'m', 0x10, ".frame", nil, 0},
		{'D', 0x5000, "main.data", nil, 0x4030},
		{'B', 0x6000, "main.bss", nil, 0},
		{'T', 0x1050, "pkg.(*T).Method", nil, 0},
		{'l', 0x1060, "pkg.T[...].Gen", nil, 0},
		{'Z', 1, "", []uint16{6, 3}, 0},
		{'T', 0x1080, "runtime.main", nil, 0},
		{'a', 24, "n", nil, 0},
		{'L', 0x10a0, "pkg.(*T[...]).M", nil, 0},
		{'T', 0x10c0, "type:.eq.main.T", nil, 0},
		{'T', 0x1100, "runtime.etext", nil, 0},
		{'d', 0x5008, "github.com/x/y.z", nil, 0},
		{'U', 0, "undef", nil, 0},
	}...)
}

func gosymName(e gosymEnt) []byte {
	b := append([]byte(e.name), 0)
	if e.typ == 'z' || e.typ == 'Z' {
		for _, c := range e.codes {
			b = binary.BigEndian.AppendUint16(b, c)
		}
		b = append(b, 0, 0)
	}
	return b
}

// gosymOldSymtab writes the symbols in the format of Go 1.0, big-endian, or
// little-endian after the prefix of the one between Go 1.0 and Go 1.1.
func gosymOldSymtab(es []gosymEnt, order binary.AppendByteOrder) []byte {
	var b []byte
	if order == binary.LittleEndian {
		b = []byte{0xfe, 0xff, 0xff, 0xff, 0x00, 0x00}
	}
	for _, e := range es {
		b = order.AppendUint32(b, uint32(e.value))
		b = append(b, e.typ|0x80)
		b = append(b, gosymName(e)...)
		b = order.AppendUint32(b, uint32(e.gotype))
	}
	return b
}

// gosymNewSymtab writes the symbols in the format of Go 1.1, with every
// third value fixed-width and the rest varints.
func gosymNewSymtab(es []gosymEnt, order binary.AppendByteOrder, ptrsz int) []byte {
	b := []byte{0xff, 0xff, 0xff, 0xfd, 0x00, 0x00, 0x00, byte(ptrsz)}
	if order == binary.LittleEndian {
		b[0], b[3] = 0xfd, 0xff
	}
	put := func(v uint64) {
		if ptrsz == 8 {
			b = order.AppendUint64(b, v)
		} else {
			b = order.AppendUint32(b, uint32(v))
		}
	}
	for i, e := range es {
		c := e.typ - 'A'
		if e.typ >= 'a' {
			c = e.typ - 'a' + 26
		}
		wide := i%3 == 0
		if wide {
			c |= 0x40
		}
		if e.gotype != 0 {
			c |= 0x80
		}
		b = append(b, c)
		if wide {
			put(e.value)
		} else {
			b = binary.AppendUvarint(b, e.value)
		}
		if e.gotype != 0 {
			put(e.gotype)
		}
		b = append(b, gosymName(e)...)
	}
	return b
}

// gosymOldLines makes a line table in the format before Go 1.2 that covers
// pcs bytes of text.
func gosymOldLines(seed uint64, pcs int) []byte {
	var b []byte
	x := seed
	for pc := 0; pc < pcs; {
		x = lcg(x)
		v := byte(x >> 40)
		switch (x >> 33) % 10 {
		case 6:
			b = append(b, 129+v%16)
			pc += int(v%16) + 1
		case 7:
			b = append(b, 65+v%4)
			pc++
		case 8:
			b = append(b, 0, 0, 0, 0, v%24)
			pc++
		default:
			b = append(b, 1+v%6)
			pc++
		}
	}
	return b
}

func gosymGzip(b []byte) []byte {
	var buf bytes.Buffer
	zw, err := gzip.NewWriterLevel(&buf, gzip.BestCompression)
	if err != nil {
		panic(err)
	}
	zw.Write(b)
	zw.Close()
	return buf.Bytes()
}

func gosymMain() {
	zr, err := gzip.NewReader(bytes.NewReader(must(os.ReadFile("gosymdata/pcln115.gz"))))
	if err != nil {
		panic(err)
	}
	pcln115 := must(io.ReadAll(zr))

	ef, err := stdelf.Open("gosymdata/pclinetest")
	if err != nil {
		panic(err)
	}
	textSec := ef.Section(".text")
	textdat := must(textSec.Data())
	pclntab := must(ef.Section(".gopclntab").Data())
	var gosymtab []byte
	if s := ef.Section(".gosymtab"); s != nil {
		gosymtab = must(s.Data())
	}

	// The functions of the Go 1.15 table, some of them, as symbols for a
	// table that has both.
	tab115, err := gosym.NewTable(nil, gosym.NewLineTable(pcln115, 0x1001000))
	if err != nil {
		panic(err)
	}
	var picked []*gosym.Func
	for i := range tab115.Funcs {
		f := &tab115.Funcs[i]
		if i < 3 || f.Name == "main.main" || f.Name == "runtime.main" {
			picked = append(picked, f)
		}
	}
	var es115 []gosymEnt
	for _, e := range gosymEnts(false)[:11] {
		es115 = append(es115, e)
	}
	for i, f := range picked {
		es115 = append(es115, gosymEnt{'T', f.Entry, f.Name, nil, 0},
			gosymEnt{'m', uint64(8 * (i + 1)), ".frame", nil, 0},
			gosymEnt{'p', 8, "arg", nil, 0})
	}
	es115 = append(es115, gosymEnt{'D', 0x2000000, "main.data", nil, 0})

	old := gosymOldLines(7, 0x110)
	fs := []*file{
		{name: "gosym/pcln115", data: gosymContainer(0x1001000, nil, pcln115)},
		{name: "gosym/pclinetest", data: gosymContainer(textSec.Addr, gosymtab, pclntab)},
		{name: "gosym/go115-symtab", data: gosymContainer(0x1001000,
			gosymNewSymtab(es115, binary.LittleEndian, 8), pcln115)},
		{name: "gosym/old-be", data: gosymContainer(0x1000,
			gosymOldSymtab(gosymEnts(false), binary.BigEndian), old)},
		{name: "gosym/old-le", data: gosymContainer(0x1000,
			gosymOldSymtab(gosymEnts(false), binary.LittleEndian), old)},
		{name: "gosym/new-be4", data: gosymContainer(0x1000,
			gosymNewSymtab(gosymEnts(true), binary.BigEndian, 4), old)},
		{name: "gosym/new-le8", data: gosymContainer(0x1000,
			gosymNewSymtab(gosymEnts(false), binary.LittleEndian, 8), old)},
	}
	for i, f := range fs {
		f.gz = true
		f.raw = gosymGzip(f.data)
		out, ok := safeGosymDump(f.data)
		if !ok {
			panic("Go panics on " + f.name)
		}
		f.dump = out
		// The pclntab of go115-symtab is pcln115's, damaged already.
		f.damage = gosymDamages(f.data, uint64(i)+1, f.name != "gosym/go115-symtab")
	}

	// The two functions in pclinetest.s that TestPCLine reads, and the text
	// they are in.
	tab, err := gosym.NewTable(gosymtab, gosym.NewLineTable(pclntab, textSec.Addr))
	if err != nil {
		panic(err)
	}
	lo, hi := ^uint64(0), uint64(0)
	for _, name := range []string{"main.linefrompc", "main.pcfromline"} {
		f := tab.LookupFunc(name)
		lo, hi = min(lo, f.Entry), max(hi, f.End)
	}

	w := os.Stdout
	fmt.Fprintf(w, "/* Generated by tools/gen-debug-tests.sh gosym from Go's debug/gosym and\n * its testdata, with pclinetest built by %s. Do not edit. */\n\n", runtime.Version())
	fmt.Fprintf(w, "#ifndef BURROW_TESTS_GOSYM_TEST_GEN_H\n#define BURROW_TESTS_GOSYM_TEST_GEN_H\n\n")
	for i, f := range fs {
		fmt.Fprintf(w, "/* %s, %d bytes, %d gzipped. */\n", f.name, len(f.data), len(f.raw))
		writeBase64(w, fmt.Sprintf("gosym_file_%d", i), f.raw)
		fmt.Fprintf(w, "static const char *const gosym_dump_%d[] = {\n", i)
		for _, line := range strings.SplitAfter(string(f.dump), "\n") {
			if line == "" {
				continue
			}
			fmt.Fprintf(w, "    %s,\n", cstr(strings.TrimSuffix(line, "\n")))
		}
		fmt.Fprintf(w, "    NULL,\n};\n\n")
		fmt.Fprintf(w, "static const DebugDamage gosym_damage_%d[] = {\n", i)
		for _, d := range f.damage {
			fmt.Fprintf(w, "    {%d, %d, \"%s\"},\n", d.pos, d.xor, d.hash)
		}
		fmt.Fprintf(w, "    {-1, 0, NULL},\n};\n\n")
	}
	fmt.Fprintf(w, "static const DebugFile gosym_files[] = {\n")
	for i, f := range fs {
		fmt.Fprintf(w, "    {%s, true, gosym_file_%d, gosym_dump_%d, gosym_damage_%d},\n", cstr(f.name), i, i, i)
	}
	fmt.Fprintf(w, "};\n\n")
	fmt.Fprintf(w, "/* The text of main.linefrompc and main.pcfromline in pclinetest. */\n")
	fmt.Fprintf(w, "#define GOSYM_PCLINE_TEXT_ADDR UINT64_C(%#x)\n\n", lo)
	writeBase64(w, "gosym_pcline_text", textdat[lo-textSec.Addr:hi-textSec.Addr])
	fmt.Fprintf(w, "#endif\n")
}

func must[T any](v T, err error) T {
	if err != nil {
		panic(err)
	}
	return v
}
GO

cat > "$tmp/xcoff.go" <<'GO'
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"

	"gen/xcoff"
)

// Anything bigger than this that Go would allocate in one go is left out of
// the dump, and the C dump leaves out the same.
const xcoffBig = 1 << 24

var xcoffSectionNames = []string{".text", ".data", ".bss", ".loader", ".dwinfo",
	".dwabrev", ".dwabrevxx", ".debug", "", ".nosuch"}

var xcoffSectionTypes = []uint32{xcoff.STYP_TEXT, xcoff.STYP_DATA, xcoff.STYP_BSS,
	xcoff.STYP_LOADER, xcoff.STYP_DEBUG, xcoff.STYP_TYPCHK,
	xcoff.STYP_DWARF | xcoff.SSUBTYP_DWINFO, xcoff.STYP_DWARF | xcoff.SSUBTYP_DWLINE}

func xcoffSize(s *xcoff.Section) int64 {
	return s.ReaderAt.(*io.SectionReader).Size()
}

func xcoffCSectBig(f *xcoff.File, name string) bool {
	for _, s := range f.Symbols {
		if s.Name == name && s.AuxCSect.SymbolType == xcoff.XTY_SD {
			return uint64(s.AuxCSect.Length) > xcoffBig
		}
	}
	return false
}

func xcoffImportsBig(f *xcoff.File) bool {
	s := f.SectionByType(xcoff.STYP_LOADER)
	if s == nil {
		return false
	}
	var h [56]byte
	n := 32
	if f.TargetMachine == xcoff.U64_TOCMAGIC {
		n = 56
	}
	if _, err := s.ReadAt(h[:n], 0); err != nil {
		return false
	}
	istlen := binary.BigEndian.Uint32(h[12:])
	stlen := binary.BigEndian.Uint32(h[24:])
	if n == 56 {
		stlen = binary.BigEndian.Uint32(h[20:])
	}
	return istlen > xcoffBig || stlen > xcoffBig
}

func xcoffDWARFBig(f *xcoff.File) bool {
	for _, t := range []uint32{xcoff.SSUBTYP_DWABREV, xcoff.SSUBTYP_DWINFO,
		xcoff.SSUBTYP_DWLINE, xcoff.SSUBTYP_DWRNGES, xcoff.SSUBTYP_DWSTR} {
		if s := f.SectionByType(xcoff.STYP_DWARF | t); s != nil && xcoffSize(s) > xcoffBig {
			return true
		}
	}
	return false
}

func dumpXcoffFile(w *bytes.Buffer, f *xcoff.File) {
	fmt.Fprintf(w, "header magic=%#x\n", f.TargetMachine)
	for i, s := range f.Sections {
		fmt.Fprintf(w, "section %d %q va=%#x size=%#x type=%#x relptr=%#x nreloc=%d\n", i,
			s.Name, s.VirtualAddress, s.Size, s.Type, s.Relptr, s.Nreloc)
		var rl bytes.Buffer
		for _, r := range s.Relocs {
			if r.Symbol != nil {
				fmt.Fprintf(&rl, "%#x %q", r.VirtualAddress, r.Symbol.Name)
			} else {
				fmt.Fprintf(&rl, "%#x nil", r.VirtualAddress)
			}
			fmt.Fprintf(&rl, " %t %t %d %d\n", r.Signed, r.InstructionFixed, r.Length, r.Type)
		}
		fmt.Fprintf(w, "  relocs n=%d %s\n", len(s.Relocs), blob(rl.Bytes(), nil))
		if xcoffSize(s) > xcoffBig {
			fmt.Fprintf(w, "  data big\n")
		} else {
			b, err := s.Data()
			fmt.Fprintf(w, "  data %s\n", blob(b, err))
		}
		var buf [8]byte
		n, err := s.ReadAt(buf[:], 4)
		fmt.Fprintf(w, "  readat n=%d % x%s\n", n, buf[:n], errs(err))
	}
	fmt.Fprintf(w, "strtab %s\n", blob(f.StringTable, nil))
	fmt.Fprintf(w, "symbols n=%d\n", len(f.Symbols))
	for _, s := range f.Symbols {
		fmt.Fprintf(w, "  %q value=%#x sect=%d class=%d fcn=%d csect=%d/%d/%d\n", s.Name,
			s.Value, s.SectionNumber, s.StorageClass, s.AuxFcn.Size, s.AuxCSect.Length,
			s.AuxCSect.StorageMappingClass, s.AuxCSect.SymbolType)
	}
	for _, name := range xcoffSectionNames {
		if s := f.Section(name); s != nil {
			fmt.Fprintf(w, "section %q size=%#x\n", name, s.Size)
		} else {
			fmt.Fprintf(w, "section %q nil\n", name)
		}
	}
	for _, t := range xcoffSectionTypes {
		if s := f.SectionByType(t); s != nil {
			fmt.Fprintf(w, "type %#x %q\n", t, s.Name)
		} else {
			fmt.Fprintf(w, "type %#x nil\n", t)
		}
	}
	// CSect of the first dozen names of SD symbols, then of one that is not
	// there.
	var names []string
	for _, s := range f.Symbols {
		if len(names) == 12 {
			break
		}
		if s.AuxCSect.SymbolType == xcoff.XTY_SD && !slicesContains(names, s.Name) {
			names = append(names, s.Name)
		}
	}
	names = append(names, "nosuch")
	for _, name := range names {
		if xcoffCSectBig(f, name) {
			fmt.Fprintf(w, "csect %q big\n", name)
			continue
		}
		if b := f.CSect(name); b != nil {
			fmt.Fprintf(w, "csect %q %s\n", name, blob(b, nil))
		} else {
			fmt.Fprintf(w, "csect %q nil\n", name)
		}
	}
	if xcoffImportsBig(f) {
		fmt.Fprintf(w, "imports skipped\n")
	} else {
		syms, err := f.ImportedSymbols()
		fmt.Fprintf(w, "imported n=%d%s\n", len(syms), errs(err))
		for _, s := range syms {
			fmt.Fprintf(w, "  %q %q\n", s.Name, s.Library)
		}
		libs, err := f.ImportedLibraries()
		dumpStrings(w, "libraries", libs, err)
		dumpStrings(w, "libpaths", f.LibraryPaths, nil)
	}
	if xcoffDWARFBig(f) {
		fmt.Fprintf(w, "dwarf big\n")
		return
	}
	d, err := f.DWARF()
	if err != nil {
		fmt.Fprintf(w, "dwarf err=%q\n", err.Error())
		return
	}
	// The DWARF dump is the one the ELF and Mach-O tests check line by line,
	// so only its hash is kept here.
	var dw bytes.Buffer
	dumpDWARF(&dw, d)
	fmt.Fprintf(w, "dwarf %s\n", blob(dw.Bytes(), nil))
}

func slicesContains(ss []string, s string) bool {
	for _, x := range ss {
		if x == s {
			return true
		}
	}
	return false
}

func dumpXcoffArchive(w *bytes.Buffer, data []byte) {
	arch, err := xcoff.NewArchive(bytes.NewReader(data))
	if err != nil {
		fmt.Fprintf(w, "archive err=%q\n", err.Error())
		return
	}
	fmt.Fprintf(w, "archive members=%d\n", len(arch.Members))
	for i, m := range arch.Members {
		fmt.Fprintf(w, "member %d %q size=%d\n", i, m.Name, m.Size)
		f, err := arch.GetFile(m.Name)
		if err != nil {
			fmt.Fprintf(w, "newfile err=%q\n", err.Error())
			continue
		}
		dumpXcoffFile(w, f)
	}
	_, err = arch.GetFile("nosuch.o")
	fmt.Fprintf(w, "getfile%s\n", errs(err))
}

// Every file goes through NewFile and NewArchive both, and one of them says
// it is not the right kind.
func dumpXcoff(w *bytes.Buffer, data []byte) {
	f, err := xcoff.NewFile(bytes.NewReader(data))
	if err != nil {
		fmt.Fprintf(w, "newfile err=%q\n", err.Error())
	} else {
		dumpXcoffFile(w, f)
	}
	dumpXcoffArchive(w, data)
}

// xcoffChainEnds says whether NewArchive gets to the last member, or to an
// error, within 64 members. Damage that sends the chain round in a loop has
// NewArchive read members until it runs out of memory, so it is left out.
func xcoffChainEnds(data []byte) bool {
	if len(data) < 128 || string(data[:8]) != xcoff.AIAMAGBIG {
		return true
	}
	dec := func(b []byte) (int64, bool) {
		v, err := strconv.ParseInt(strings.TrimSpace(string(b)), 10, 64)
		return v, err == nil
	}
	off, ok := dec(data[68:88])
	if !ok || off == 0 {
		return true
	}
	last, ok := dec(data[88:108])
	if !ok {
		return true
	}
	for i := 0; i < 64; i++ {
		if off < 0 || off > int64(len(data))-112 {
			return true
		}
		h := data[off : off+112]
		if _, ok := dec(h[:20]); !ok {
			return true
		}
		namlen, ok := dec(h[108:112])
		if !ok || namlen < 0 {
			return true
		}
		fileoff := off + 112 + namlen
		if fileoff&1 != 0 {
			fileoff++
		}
		if fileoff+2 > int64(len(data)) || string(data[fileoff:fileoff+2]) != "`\n" {
			return true
		}
		if off == last {
			return true
		}
		if off, ok = dec(h[20:40]); !ok {
			return true
		}
	}
	return false
}

func safeXcoffDump(data []byte) (out []byte, ok bool) {
	if !xcoffChainEnds(data) {
		return nil, false
	}
	defer func() {
		if recover() != nil {
			ok = false
		}
	}()
	var w bytes.Buffer
	dumpXcoff(&w, data)
	return w.Bytes(), true
}

// xcoffTargets gives the bytes of the XCOFF file at base worth damaging: the
// file header, the fields of each section header, the loader header and its
// import IDs, the first relocations of each section, and the fields of the
// symbol entries.
func xcoffTargets(data []byte, base int) []int {
	var ts []int
	n := len(data)
	in := func(p int) {
		if p >= 0 && p < n {
			ts = append(ts, p)
		}
	}
	if base < 0 || base+24 > n {
		return ts
	}
	be := binary.BigEndian
	d := data[base:]
	is32 := be.Uint16(d) == xcoff.U802TOCMAGIC
	hdrsz, shsz, relsz := 24, 72, 14
	var symptr uint64
	var nsyms uint32
	if is32 {
		hdrsz, shsz, relsz = 20, 40, 10
		symptr, nsyms = uint64(be.Uint32(d[8:])), be.Uint32(d[12:])
	} else {
		symptr, nsyms = be.Uint64(d[8:]), be.Uint32(d[20:])
	}
	for i := 0; i < hdrsz; i++ {
		in(base + i)
	}
	opthdr := int(be.Uint16(d[16:]))
	nscns := int(be.Uint16(d[2:]))
	for i := 0; i < nscns; i++ {
		sh := base + hdrsz + opthdr + i*shsz
		if sh+shsz > n {
			break
		}
		h := data[sh : sh+shsz]
		var fields []int
		var scnptr, relptr uint64
		var nreloc int
		var typ uint32
		if is32 {
			fields = []int{0, 7, 15, 16, 19, 23, 27, 33, 39}
			scnptr, relptr = uint64(be.Uint32(h[20:])), uint64(be.Uint32(h[24:]))
			nreloc, typ = int(be.Uint16(h[32:])), be.Uint32(h[36:])
		} else {
			fields = []int{0, 7, 23, 24, 31, 39, 47, 59, 67}
			scnptr, relptr = be.Uint64(h[32:]), be.Uint64(h[40:])
			nreloc, typ = int(be.Uint32(h[56:])), be.Uint32(h[64:])
		}
		for _, f := range fields {
			in(sh + f)
		}
		if typ == xcoff.STYP_LOADER && scnptr < uint64(n) {
			lh := base + int(scnptr)
			for j := 0; j < 56; j++ {
				in(lh + j)
			}
			if lh+56 <= n {
				istlen := int(min(be.Uint32(data[lh+12:]), 64))
				impoff := be.Uint64(data[lh+24:])
				if is32 {
					impoff = uint64(be.Uint32(data[lh+20:]))
				}
				if impoff < uint64(n) {
					for j := 0; j < istlen; j++ {
						in(lh + int(impoff) + j)
					}
				}
			}
		}
		if relptr != 0 && relptr < uint64(n) {
			for j := 0; j < min(nreloc, 8); j++ {
				r := base + int(relptr) + j*relsz
				in(r + relsz - 3)
				in(r + relsz - 2)
			}
		}
	}
	if symptr < uint64(n) {
		for i := 0; i < int(min(nsyms, 200)); i++ {
			e := base + int(symptr) + i*18
			for _, f := range []int{7, 11, 13, 15, 16, 17} {
				in(e + f)
			}
		}
		st := base + int(symptr) + int(nsyms)*18
		for j := 0; j < 8; j++ {
			in(st + j)
		}
	}
	return ts
}

func xcoffDamages(data []byte, seed uint64) []damage {
	var ds []damage
	x := seed
	n := len(data)
	add := func(pos int, xor byte) {
		if pos < 0 || pos >= n || (xor == 0 && pos == 0) {
			return
		}
		b := append([]byte(nil), data...)
		if xor == 0 {
			b = b[:pos]
		} else {
			b[pos] ^= xor
		}
		out, ok := safeXcoffDump(b)
		if !ok {
			return
		}
		ds = append(ds, damage{pos, xor, fmt.Sprintf("%x", sha256.Sum256(out))[:16]})
	}
	var ts []int
	cuts := []int{n / 2, n - 1, n * 3 / 4, 100, 30, 3}
	if n >= 128 && string(data[:8]) == xcoff.AIAMAGBIG {
		// The decimal fields of the archive header and of each member header,
		// where a low bit turns one digit into another, and the files inside.
		for i := 8; i < 128; i++ {
			ts = append(ts, i)
		}
		off, _ := strconv.ParseInt(strings.TrimSpace(string(data[68:88])), 10, 64)
		for i := 0; i < 8 && off > 0 && off+114 <= int64(n); i++ {
			for j := 0; j < 114; j++ {
				ts = append(ts, int(off)+j)
			}
			namlen, _ := strconv.ParseInt(strings.TrimSpace(string(data[off+108:off+112])), 10, 64)
			fileoff := off + 112 + namlen
			if fileoff&1 != 0 {
				fileoff++
			}
			ts = append(ts, xcoffTargets(data, int(fileoff)+2)...)
			cuts = append(cuts, int(off)+50, int(fileoff)+1)
			next, err := strconv.ParseInt(strings.TrimSpace(string(data[off+20:off+40])), 10, 64)
			if err != nil || next == off {
				break
			}
			off = next
		}
	} else {
		ts = xcoffTargets(data, 0)
		if n >= 24 {
			symptr := int(binary.BigEndian.Uint32(data[8:]))
			if binary.BigEndian.Uint16(data) == xcoff.U64_TOCMAGIC {
				symptr = int(binary.BigEndian.Uint64(data[8:]))
			}
			cuts = append(cuts, symptr+9, symptr+100)
		}
	}
	for i := 0; i < 90 && len(ts) > 0; i++ {
		x = lcg(x)
		add(ts[int(x>>33)%len(ts)], byte(1<<((x>>20)%8)))
	}
	for i := 0; i < 16; i++ {
		x = lcg(x)
		xor := byte(x >> 56)
		if xor == 0 {
			xor = 0x80
		}
		add(int(x>>33)%n, xor)
	}
	for _, cut := range cuts {
		if cut > 0 && cut < n {
			add(cut, 0)
		}
	}
	return ds
}

func xcoffMain() {
	fs := load("xcoffdata")
	for i, f := range fs {
		f.name = "xcoff/" + f.name
		out, ok := safeXcoffDump(f.data)
		if !ok {
			panic("Go panics on " + f.name)
		}
		f.dump = out
		f.damage = xcoffDamages(f.data, uint64(i)+1)
	}

	w := os.Stdout
	fmt.Fprintf(w, "/* Generated by tools/gen-debug-tests.sh xcoff from Go's internal/xcoff and\n * its testdata. Do not edit. */\n\n")
	fmt.Fprintf(w, "#ifndef BURROW_TESTS_XCOFF_TEST_GEN_H\n#define BURROW_TESTS_XCOFF_TEST_GEN_H\n\n")
	for i, f := range fs {
		fmt.Fprintf(w, "/* %s, %d bytes. */\n", f.name, len(f.raw))
		writeBase64(w, fmt.Sprintf("xcoff_file_%d", i), f.raw)
		fmt.Fprintf(w, "static const char *const xcoff_dump_%d[] = {\n", i)
		for _, line := range strings.SplitAfter(string(f.dump), "\n") {
			if line == "" {
				continue
			}
			fmt.Fprintf(w, "    %s,\n", cstr(strings.TrimSuffix(line, "\n")))
		}
		fmt.Fprintf(w, "    NULL,\n};\n\n")
		fmt.Fprintf(w, "static const DebugDamage xcoff_damage_%d[] = {\n", i)
		for _, d := range f.damage {
			fmt.Fprintf(w, "    {%d, %d, \"%s\"},\n", d.pos, d.xor, d.hash)
		}
		fmt.Fprintf(w, "    {-1, 0, NULL},\n};\n\n")
	}
	fmt.Fprintf(w, "static const DebugFile xcoff_files[] = {\n")
	for i, f := range fs {
		fmt.Fprintf(w, "    {%s, false, xcoff_file_%d, xcoff_dump_%d, xcoff_damage_%d},\n", cstr(f.name), i, i, i)
	}
	fmt.Fprintf(w, "};\n\n#endif\n")
}
GO

cat > "$tmp/buildinfo.go" <<'GO'
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"gen/buildinfo"
	debug "gen/rtdebug"
)

var biMagic = []byte("\xff Go buildinf:")

// biDump is what Go's debug/buildinfo reads from data, as BuildInfo.String
// gives it, or the error.
func biDump(data []byte) (out []byte, ok bool) {
	defer func() {
		if recover() != nil {
			ok = false
		}
	}()
	info, err := buildinfo.Read(bytes.NewReader(data))
	if err != nil {
		return []byte("error: " + err.Error() + "\n"), true
	}
	return []byte(info.String()), true
}

func biMustDump(name string, data []byte) []byte {
	out, ok := biDump(data)
	if !ok {
		panic("Go panics on " + name)
	}
	return out
}

// biMinimize zeroes every piece of a binary that debug/buildinfo can do
// without, so that a binary of a megabyte or two gzips to a few kilobytes:
// the headers, the build information and what leads to it stay.
func biMinimize(name string, data []byte) []byte {
	want := biMustDump(name, data)
	b := append([]byte(nil), data...)
	for _, size := range []int{1 << 16, 1 << 12, 1 << 9, 1 << 6} {
		for off := 0; off < len(b); off += size {
			end := min(off+size, len(b))
			if bytes.Count(b[off:end], []byte{0}) == end-off {
				continue
			}
			saved := append([]byte(nil), b[off:end]...)
			clear(b[off:end])
			if got, ok := biDump(b); !ok || !bytes.Equal(got, want) {
				copy(b[off:end], saved)
			}
		}
	}
	return b
}

// biFat makes a Mach-O universal binary of the two thin ones.
func biFat(thin ...[]byte) []byte {
	const align = 14
	be, le := binary.BigEndian, binary.LittleEndian
	hdr := make([]byte, 8+20*len(thin))
	be.PutUint32(hdr, 0xcafebabe)
	be.PutUint32(hdr[4:], uint32(len(thin)))
	out := hdr
	off := uint32(1 << align)
	for i, f := range thin {
		e := hdr[8+20*i:]
		be.PutUint32(e, le.Uint32(f[4:]))
		be.PutUint32(e[4:], le.Uint32(f[8:]))
		be.PutUint32(e[8:], off)
		be.PutUint32(e[12:], uint32(len(f)))
		be.PutUint32(e[16:], align)
		out = append(out, make([]byte, int(off)-len(out))...)
		out = append(out, f...)
		off = (off + uint32(len(f)) + 1<<align - 1) &^ (1<<align - 1)
	}
	return out
}

// biDamages changes the build information header and the strings after it
// a bit at a time, the header of the binary and anywhere else, and cuts the
// binary short in a few places.
func biDamages(data []byte, seed uint64) []damage {
	var ds []damage
	x := seed
	n := len(data)
	add := func(pos int, xor byte) {
		if pos < 0 || pos >= n || (xor == 0 && pos == 0) {
			return
		}
		b := append([]byte(nil), data...)
		if xor == 0 {
			b = b[:pos]
		} else {
			b[pos] ^= xor
		}
		out, ok := biDump(b)
		if !ok {
			return
		}
		ds = append(ds, damage{pos, xor, fmt.Sprintf("%x", sha256.Sum256(out))[:16]})
	}
	if m := bytes.Index(data, biMagic); m >= 0 {
		for i := 0; i < 48; i++ {
			add(m+i, byte(1<<(i%8)))
		}
		add(m+2, 'G'^'N')
		add(m+15, 2)
		for i := 0; i < 12 && m+32 < n; i++ {
			x = lcg(x)
			add(m+32+int(x>>33)%min(400, n-m-32), byte(1<<((x>>20)%8)))
		}
		for _, cut := range []int{m + 8, m + 20, m + 31, m + 33, m + 40, m + 100} {
			add(cut, 0)
		}
	}
	for i := 0; i < 8; i++ {
		x = lcg(x)
		add(int(x>>33)%min(64, n), byte(1<<((x>>20)%8)))
	}
	for i := 0; i < 16; i++ {
		x = lcg(x)
		xor := byte(x >> 56)
		if xor == 0 {
			xor = 0x80
		}
		add(int(x>>33)%n, xor)
	}
	for _, cut := range []int{n / 2, n - 1, n * 3 / 4, 100} {
		if cut > 0 && cut < n {
			add(cut, 0)
		}
	}
	return ds
}

func biBase64(name string) []byte {
	s, err := os.ReadFile(filepath.Join("bidata", name))
	if err != nil {
		panic(err)
	}
	b, err := base64.StdEncoding.DecodeString(string(s))
	if err != nil {
		panic(err)
	}
	return b
}

// biIssue57002 is the input of Go's FuzzIssue57002, from its test file.
func biIssue57002() []byte {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, "bidata/buildinfo_test.go", nil, 0)
	if err != nil {
		panic(err)
	}
	var out []byte
	for _, d := range f.Decls {
		fd, ok := d.(*ast.FuncDecl)
		if !ok || fd.Name.Name != "FuzzIssue57002" {
			continue
		}
		ast.Inspect(fd, func(n ast.Node) bool {
			cl, ok := n.(*ast.CompositeLit)
			if !ok || out != nil {
				return out == nil
			}
			for _, e := range cl.Elts {
				v, err := strconv.ParseUint(e.(*ast.BasicLit).Value, 0, 8)
				if err != nil {
					panic(err)
				}
				out = append(out, byte(v))
			}
			return false
		})
	}
	if out == nil {
		panic("no FuzzIssue57002 input")
	}
	return out
}

// biCorpus reads a file of Go's fuzzing corpus, one []byte value.
func biCorpus(name string) []byte {
	s, err := os.ReadFile(filepath.Join("bidata", name))
	if err != nil {
		panic(err)
	}
	lines := strings.Split(strings.TrimSpace(string(s)), "\n")
	v := strings.TrimSuffix(strings.TrimPrefix(lines[1], "[]byte("), ")")
	b, err := strconv.Unquote(v)
	if err != nil {
		panic(err)
	}
	return []byte(b)
}

// The inputs of the ParseBuildInfo cases: the seeds of Go's
// FuzzParseBuildInfoRoundTrip, a line for each way a line can be wrong, and
// then mixes of the lines.
var biParseSeeds = []string{
	"path\trsc.io/fortune\nmod\trsc.io/fortune\tv1.0.0\n",
	"path\tcmd/test2json",
	"go\t1.18\npath\texample.com/m\nmod\texample.com/m\t(devel)\t\nbuild\t-compiler=gc\n",
	"go\t1.18\npath\texample.com/m\nbuild\t-compiler=gc\n",
	"go 1.18\npath example.com/m\nbuild CRAZY_ENV=\"requires\\nescaping\"\n",
	"",
	"\n\n\n",
	"path\ta\tb\n",
	"path\tp\nmod\t\n",
	"\n\n\nmod\tx\n",
	"mod\ta\n",
	"mod\ta\tb\n",
	"mod\ta\tb\tc\td\n",
	"dep\tx\n",
	"dep\tx\tv1\th1:abc\n",
	"=>\ta\tb\tc\n",
	"mod\tm\tv\n=>\ta\tb\n",
	"dep\tx\tv\n=>\ta\tb\tc\n=>\td\te\tf\n",
	"mod\tm\tv\n=>\t../r\t\t\n",
	"mod\tm\tv\nmod\tn\tw\th\n",
	"dep\tx\tv\nmod\tm\tv\n=>\ta\tb\tc\ndep\ty\tw\ts\n=>\td\te\tf\n",
	"build\t\n",
	"build\t=x\n",
	"build\t\"k\n",
	"build\t\"k\"\n",
	"build\t\"k\"x\n",
	"build\t\"k\"\xff\n",
	"build\t`k`\t\n",
	"build\tk\n",
	"build\tk k=v\n",
	"build\t\"\"=v\n",
	"build\tk=\"v\n",
	"build\tk=a b\n",
	"build\tk=`raw`\n",
	"build\tk=`a\"b`\n",
	"build\tk=\"a\\tb\"\n",
	"build\t`k k`=v\n",
	"build\tk=\n",
	"build\tk=v=w\n",
	"build\t\"\\x\"=v\n",
	"build\t\"a=b\"=c\n",
	"build\tk=\u00e9\n",
	"build\tk=\"\\u00e9\"\n",
	"build\tk=\"\\xff\"\n",
	"build\tk=\"\u00e9\\n\\x00\"\n",
	"build\t\"\\t\"=\"\\\"\"\n",
	"build\tk=\"\\U0001f600 \\a\"\n",
	"build\tk=v\r\n",
	"build\tk=\xffv\n",
	"build\tk==\n",
	"build\t=\n",
	"build\tk=`\n",
	"path\tp\nbuild\tGOOS=linux\nbuild\t-ldflags=\"-s -w\"\nbuild\tvcs.modified=true\n",
	"dep\ta\tb\tc\n=>\tx\n",
	"unknown\tline\nmod\tm\tv\n",
	"mod\tm\tv\n\nbuild\tk=v\n",
}

// biParseLines are what the mixes are made of.
var biParseLines = []string{
	"path\texample.com/m", "path\t", "mod\texample.com/m\t(devel)\t", "mod\tm\tv1.0.0\th1:x=",
	"mod\tm", "dep\tgolang.org/x/text\tv0.3.0\th1:abc=", "dep\td\tv", "dep\t\t",
	"=>\t../text\t\t", "=>\tr\tv\ts", "=>\tr", "build\t-compiler=gc", "build\tGOOS=linux",
	"build\t-ldflags=\"-s -w\"", "build\tk=`v`", "build\t\"q k\"=v", "build\tk=a b",
	"build\t=v", "build\t", "go\tgo1.27.1", "", "\t", "x",
}

func biParseDump(s string) string {
	bi, err := debug.ParseBuildInfo(s)
	if err != nil {
		return "error: " + err.Error() + "\n"
	}
	s2 := bi.String()
	if _, err := debug.ParseBuildInfo(s2); err != nil {
		panic("String does not parse back: " + s2)
	}
	return "ok\n" + s2
}

func biMain() {
	type src struct {
		name string
		data []byte
		min  bool // minimised and gzipped
		gz   bool
	}
	srcs := []src{
		{"testdata/go117", biBase64("go117/go117.base64"), true, true},
		{"testdata/notgo", biBase64("notgo/notgo.base64"), false, true},
	}
	corpus, err := os.ReadDir("bidata/fuzz/FuzzRead")
	if err != nil {
		panic(err)
	}
	for _, de := range corpus {
		srcs = append(srcs, src{"testdata/fuzz/FuzzRead/" + de.Name(),
			biCorpus("fuzz/FuzzRead/" + de.Name()), false, false})
	}
	srcs = append(srcs, src{"issue57002", biIssue57002(), false, false})
	builds, err := os.ReadDir("bibuild")
	if err != nil {
		panic(err)
	}
	thin := map[string][]byte{}
	for _, de := range builds {
		b, err := os.ReadFile(filepath.Join("bibuild", de.Name()))
		if err != nil {
			panic(err)
		}
		b = biMinimize(de.Name(), b)
		thin[de.Name()] = b
		srcs = append(srcs, src{de.Name(), b, false, true})
		if de.Name() == "darwin_arm64_exe" {
			srcs = append(srcs, src{"darwin_fat_exe",
				biFat(thin["darwin_amd64_exe"], b), false, true})
		}
	}
	if len(builds) == 0 {
		panic("no builds")
	}

	var fs []*file
	for i, s := range srcs {
		data := s.data
		if s.min {
			data = biMinimize(s.name, data)
		}
		f := &file{name: s.name, gz: s.gz, data: data, raw: data}
		if s.gz {
			f.raw = gosymGzip(data)
		}
		f.dump = biMustDump(s.name, data)
		f.damage = biDamages(data, uint64(i)+1)
		fs = append(fs, f)
	}

	inputs := append([]string(nil), biParseSeeds...)
	x := uint64(7)
	for i := 0; i < 200; i++ {
		var b strings.Builder
		x = lcg(x)
		k := 1 + int(x>>33)%6
		for j := 0; j < k; j++ {
			x = lcg(x)
			b.WriteString(biParseLines[int(x>>33)%len(biParseLines)])
			x = lcg(x)
			if j < k-1 || x>>60 != 0 {
				b.WriteByte('\n')
			}
		}
		inputs = append(inputs, b.String())
	}

	w := os.Stdout
	fmt.Fprintf(w, "/* Generated by tools/gen-debug-tests.sh buildinfo from Go's debug/buildinfo,\n * runtime/debug and their testdata, and Go binaries built for it. Do not edit. */\n\n")
	fmt.Fprintf(w, "#ifndef BURROW_TESTS_BUILDINFO_TEST_GEN_H\n#define BURROW_TESTS_BUILDINFO_TEST_GEN_H\n\n")
	for i, f := range fs {
		fmt.Fprintf(w, "/* %s, %d bytes. */\n", f.name, len(f.data))
		writeBase64(w, fmt.Sprintf("buildinfo_file_%d", i), f.raw)
		fmt.Fprintf(w, "static const char *const buildinfo_dump_%d[] = {\n", i)
		for _, line := range strings.SplitAfter(string(f.dump), "\n") {
			if line == "" {
				continue
			}
			fmt.Fprintf(w, "    %s,\n", cstr(strings.TrimSuffix(line, "\n")))
		}
		fmt.Fprintf(w, "    NULL,\n};\n\n")
		fmt.Fprintf(w, "static const DebugDamage buildinfo_damage_%d[] = {\n", i)
		for _, d := range f.damage {
			fmt.Fprintf(w, "    {%d, %d, \"%s\"},\n", d.pos, d.xor, d.hash)
		}
		fmt.Fprintf(w, "    {-1, 0, NULL},\n};\n\n")
	}
	fmt.Fprintf(w, "static const DebugFile buildinfo_files[] = {\n")
	for i, f := range fs {
		fmt.Fprintf(w, "    {%s, %t, buildinfo_file_%d, buildinfo_dump_%d, buildinfo_damage_%d},\n",
			cstr(f.name), f.gz, i, i, i)
	}
	fmt.Fprintf(w, "};\n\n")
	fmt.Fprintf(w, "/* What runtime/debug's ParseBuildInfo makes of each input: \"ok\" and then\n * BuildInfo.String, or the error. */\n")
	fmt.Fprintf(w, "static const BuildinfoParseCase buildinfo_parse_cases[] = {\n")
	for _, in := range inputs {
		fmt.Fprintf(w, "    {%s, %d, %s},\n", cstr(in), len(in), cstr(biParseDump(in)))
	}
	fmt.Fprintf(w, "};\n\n#endif\n")
}
GO

cd "$tmp"
cat > go.mod <<'MOD'
module gen

go 1.22
MOD
go run . "$mode" > "$out"
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
echo "wrote $out"
