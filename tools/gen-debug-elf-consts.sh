#!/bin/sh
# Regenerates the constants of debug/elf, and the names their String and
# GoString methods give, from src/debug/elf/elf.go:
#
#   - include/burrow/debug/elf_const.h, the named integer types, their
#     constants and the String and GoString functions of each;
#   - src/debug/elf_string.c, the name tables Go keeps next to each const
#     block, the stringName and flagName that read them, and the type
#     descriptors.
#
# The tables are copied as Go has them rather than worked out from the
# constants, since they leave some constants out and the order they are in
# decides which name a value with two of them gets.
#
# The C names are the ones tools/burrow-coverage expects, since it is loaded to
# spell them. GOROOT says which Go to read, and defaults to what go env says.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
goroot=${GOROOT:-$(go env GOROOT)}

python3 - "$root" "$goroot" <<'PY'
import importlib.machinery
import importlib.util
import os
import re
import sys
import textwrap

root, goroot = sys.argv[1], sys.argv[2]
gover = open(os.path.join(goroot, "VERSION")).readline().strip()

loader = importlib.machinery.SourceFileLoader("burrow_coverage", os.path.join(root, "tools", "burrow-coverage"))
spec = importlib.util.spec_from_loader("burrow_coverage", loader)
cov = importlib.util.module_from_spec(spec)
loader.exec_module(cov)

PKG = "debug/elf"

LICENCE = """ * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */
"""

# The C type each Go underlying type becomes, and the Kind of its descriptor.
CTYPES = {
    "byte": ("uint8_t", "KIND_UINT8"),
    "uint16": ("uint16_t", "KIND_UINT16"),
    "uint32": ("uint32_t", "KIND_UINT32"),
    "int": ("Int", "KIND_INT"),
}


def mapped(text):
    d = cov.Decl(PKG, text)
    cov.map_decl(d)
    return d.cname


def const_name(name):
    return mapped("const " + name)


def type_cname(name):
    return mapped("type %s int" % name)


def method_cname(typ, method):
    return mapped("method (%s) %s() string" % (typ, method))


def type_macro(typ):
    return "TYPE_" + cov.type_snake(PKG, typ).upper()


def comment_text(text):
    """A Go comment as the text of a C one."""
    text = text.strip()
    if text.startswith("//"):
        text = text[2:]
    elif text.startswith("/*"):
        text = text[2:]
        if text.endswith("*/"):
            text = text[:-2]
    text = " ".join(text.split())
    if "/*" in text or "*/" in text:
        sys.exit("comment would end early: " + text)
    return text


def split_comment(rest):
    """The comment after a constant's value, or None."""
    rest = rest.strip()
    if not rest:
        return None
    if not (rest.startswith("//") or rest.startswith("/*")):
        sys.exit("cannot read: " + rest)
    return comment_text(rest)


src = open(os.path.join(goroot, "src", "debug", "elf", "elf.go")).read()
lines = src.split("\n")

# The items of the file in order: ("type", name, underlying, doc),
# ("const", typ or None, doc, entries) and ("single", name, value, doc).
items = []
tables = {}
methods = {}
doc = []
i = 0
while i < len(lines):
    line = lines[i]
    if line.startswith("//"):
        doc.append(comment_text(line))
        i += 1
        continue
    if line.startswith("/*") and line.rstrip().endswith("*/") and i > 50:
        doc.append(comment_text(line))
        i += 1
        continue
    m = re.match(r"^type (\w+) (byte|uint16|uint32|int)$", line)
    if m:
        items.append(("type", m.group(1), m.group(2), " ".join(doc)))
        doc = []
        i += 1
        continue
    if line == "const (":
        entries = []
        typ = None
        i += 1
        while lines[i] != ")":
            text = lines[i].strip()
            i += 1
            if not text:
                continue
            # A comment that goes on over more lines than one.
            if "/*" in text and "*/" not in text:
                while "*/" not in text:
                    text += " " + lines[i].strip()
                    i += 1
            if text.startswith("//") or text.startswith("/*"):
                entries.append((None, None, None, comment_text(text)))
                continue
            cm = re.match(r"^(\w+)\s+(?:(\w+)\s+)?=\s*(0x[0-9A-Fa-f]+|\d+)(.*)$", text)
            if cm is None:
                sys.exit("cannot read: " + text)
            if cm.group(2):
                if typ is not None and typ != cm.group(2):
                    sys.exit("two types in one block: " + text)
                typ = cm.group(2)
            entries.append((cm.group(1), cm.group(3), int(cm.group(3), 0), split_comment(cm.group(4))))
        items.append(("const", typ, " ".join(doc), entries))
        doc = []
        i += 1
        continue
    m = re.match(r'^const (\w+) = (0x[0-9A-Fa-f]+|\d+|"[^"]*")$', line)
    if m:
        items.append(("single", m.group(1), m.group(2), " ".join(doc)))
        doc = []
        i += 1
        continue
    m = re.match(r"^var (\w+) = \[\]intName\{$", line)
    if m:
        rows = []
        i += 1
        while lines[i] != "}":
            text = lines[i].strip()
            i += 1
            if not text or text.startswith("//") or text.startswith("/*"):
                continue
            rm = re.match(r'^\{(0x[0-9A-Fa-f]+|\d+), "([^"]*)"\},$', text)
            if rm is None:
                sys.exit("cannot read: " + text)
            rows.append((rm.group(1), int(rm.group(1), 0), rm.group(2)))
        tables[m.group(1)] = rows
        doc = []
        i += 1
        continue
    m = re.match(r"^func \(i (\w+)\) (String|GoString)\(\) string\s+\{ return (stringName|flagName)\(uint32\(i\), (\w+), (true|false)\) \}$", line)
    if m:
        want = "true" if m.group(2) == "GoString" else "false"
        if m.group(5) != want:
            sys.exit("unexpected goSyntax: " + line)
        prev = methods.setdefault(m.group(1), (m.group(3), m.group(4)))
        if prev != (m.group(3), m.group(4)):
            sys.exit("String and GoString differ: " + line)
    doc = []
    i += 1

types = {name: under for kind, name, under, _ in [x for x in items if x[0] == "type"]}
for t in methods:
    if t not in types:
        sys.exit("methods on an unknown type " + t)


def wrap_comment(text, indent=""):
    body = textwrap.wrap(text, 84 - len(indent) - 3)
    if not body:
        return []
    out = [indent + "/* " + body[0]]
    for b in body[1:]:
        out.append(indent + " * " + b)
    out[-1] += " */"
    return out


def with_comment(text, comment, indent):
    """A constant's line with its comment after it, or above it when the two
    are too long for one line."""
    if not comment:
        return [text]
    if len(text) + len(comment) + 12 <= 88:
        return [text + " /* %s */" % comment]
    return wrap_comment(comment, indent) + [text]


def big(entries):
    return any(e[0] is not None and e[2] > 0x7FFFFFFF for e in entries)


# ------------------------------------------------------------------ header

hdr = []
for item in items:
    if item[0] == "type":
        _, name, under, tdoc = item
        cname = type_cname(name)
        ctype, _ = CTYPES[under]
        hdr.append("")
        hdr.extend(wrap_comment("elf.%s. %s" % (name, tdoc) if tdoc else "elf.%s." % name))
        hdr.append("typedef %s %s;" % (ctype, cname))
        hdr.append("extern const Type burrow_type_%s;" % cname)
        hdr.append("#define %s TYPE_OF(%s)" % (type_macro(name), cname))
        if name in methods:
            fn, _ = methods[name]
            hdr.append("")
            rows = tables[methods[name][1]]
            if fn == "stringName":
                hdr.extend(wrap_comment('"%s" and "elf.%s" for %s; see the top of the file for a value '
                                        "with no name." % (rows[0][2], rows[0][2], rows[0][2])))
            else:
                two = "%s+%s" % (rows[0][2], rows[1][2])
                hdr.extend(wrap_comment('"%s" and "elf.%s" for the two flags; see the top of the file '
                                        "for the rest." % (two, two.replace("+", "+elf."))))
            hdr.append("BURROW_OWNS(ret) Str %s(%s i, Alloc *a);" % (method_cname(name, "String"), cname))
            hdr.append("BURROW_OWNS(ret) Str %s(%s i, Alloc *a);" % (method_cname(name, "GoString"), cname))
        continue
    if item[0] == "single":
        _, name, value, sdoc = item
        hdr.append("")
        if sdoc:
            hdr.extend(wrap_comment(sdoc))
        if value.startswith('"'):
            hdr.append("#define %s BURROW_S(%s)" % (const_name(name), value))
        else:
            hdr.append("enum { %s = %s };" % (const_name(name), value))
        continue
    _, typ, cdoc, entries = item
    hdr.append("")
    if typ is None:
        hdr.extend(wrap_comment(cdoc if cdoc else "Untyped constants."))
    else:
        hdr.extend(wrap_comment("elf.%s's constants." % typ))
    if typ is not None and big(entries):
        ctyp = type_cname(typ)
        for name, lit, val, comment in entries:
            if name is None:
                hdr.extend(wrap_comment(comment))
                continue
            text = "#define %s ((%s)%s)" % (const_name(name), ctyp, lit)
            hdr.extend(with_comment(text, comment, ""))
        continue
    hdr.append("enum {")
    for name, lit, val, comment in entries:
        if name is None:
            hdr.extend(wrap_comment(comment, "    "))
            continue
        text = "    %s = %s," % (const_name(name), lit)
        hdr.extend(with_comment(text, comment, "    "))
    hdr.append("};")

header = """/* Derived from Go's src/debug/elf/elf.go.
 * Go source: %s.
 *
%s *
 * Generated by tools/gen-debug-elf-consts.sh from the Go source. Do not edit.
 *
 * The named integer types of debug/elf, their constants and the String and
 * GoString functions of each, which burrow/debug/elf.h includes. A type whose
 * constants all fit in an int has them as an enum, and one with a constant
 * that does not, such as SHT_HIUSER, has them as macros with the type's cast.
 *
 * String gives the name of a constant, and a value that is not one gets the
 * name of the largest smaller value with "+" and the difference after it, such
 * as "SHN_ABS+1", or the number when no value is smaller. The flag types, which
 * are SectionFlag, ProgFlag, DynFlag and DynFlag1, give the names of the flags
 * set joined with "+" instead, then "+0x" and the hex of any bits left over,
 * or "0x" and the hex of the value when no flag is set. GoString is the same
 * with "elf." in front of each name. A name straight from a table is static
 * and anything else is made in a, so a stays alive while the result is used.
 *
 * elf.go is derived in its turn from FreeBSD's ELF headers:
 *
 * Copyright (c) 1996-1998 John D. Polstra. All rights reserved.
 * Copyright (c) 2001 David E. O'Brien
 * Portions Copyright 2009 The Go Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#ifndef BURROW_DEBUG_ELF_CONST_H
#define BURROW_DEBUG_ELF_CONST_H

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
%s

#ifdef __cplusplus
}
#endif

#endif
""" % (gover, LICENCE.replace(" */\n", "\n"), "\n".join(hdr))

# ------------------------------------------------------------------ source

# The longest text either function can make, so the buffer it is made in can
# be a fixed size.
longest = 0
for typ, (fn, table) in methods.items():
    rows = tables[table]
    if fn == "stringName":
        n = max(len(r[2]) for r in rows) + len("elf.+4294967295")
    else:
        n = sum(len(r[2]) + len("elf.+") for r in rows) + len("+0xffffffff")
    longest = max(longest, n)
bufsize = 256
while bufsize < longest:
    bufsize *= 2


def table_cname(table):
    return "elf_" + cov.snake(table[0].upper() + table[1:])


out = []
for table, rows in tables.items():
    out.append("static const ElfIntName %s[] = {" % table_cname(table))
    for lit, val, name in rows:
        out.append('    {%s, BURROW_S_INIT("%s")},' % (lit, name))
    out.append("};")
    out.append("")

for item in items:
    if item[0] != "type":
        continue
    name = item[1]
    cname = type_cname(name)
    _, kind = CTYPES[item[2]]
    snake = cov.type_snake(PKG, name)
    if name in methods:
        fn, table = methods[name]
        tc = table_cname(table)
        cfn = "elf_string_name" if fn == "stringName" else "elf_flag_name"
        for meth, go in (("String", "false"), ("GoString", "true")):
            out.append("Str %s(%s i, Alloc *a) {" % (method_cname(name, meth), cname))
            out.append("    return %s((uint32_t)i, %s, ELF_NNAME(%s), %s, a);" % (cfn, tc, tc, go))
            out.append("}")
            out.append("")
        for meth in ("GoString", "String"):
            out.append("static Str %s_m_%s(%s *self) {" % (snake, cov.snake(meth), cname))
            out.append("    return %s(*self, error_allocator());" % method_cname(name, meth))
            out.append("}")
            out.append("")
        up = snake.upper()
        out.append("#define %s_SIG_STRING(IN, OUT) OUT(Str)" % up)
        out.append("#define %s_METHODS(M, T) \\" % up)
        out.append("    M(T, GoString, %s_m_go_string, %s_SIG_STRING) \\" % (snake, up))
        out.append("    M(T, String, %s_m_string, %s_SIG_STRING)" % (snake, up))
        out.append("")
        out.append("BURROW_METHODS_DEFINE(%s, %s_METHODS);" % (cname, up))
        out.append("")
        nmethod = "(uint16_t)(sizeof(burrow__methods_%s) / sizeof(burrow__methods_%s[0]))" % (cname, cname)
        mtab = "burrow__methods_%s" % cname
    else:
        nmethod = "0"
        mtab = "NULL"
    out.append("const Type burrow_type_%s = {" % cname)
    out.append('    {(const Byte *)"%s", %d},' % (name, len(name)))
    out.append('    {(const Byte *)"debug/elf", 9},')
    out.append("    %s," % kind)
    out.append("    (uint32_t)sizeof(%s)," % cname)
    out.append("    (uint16_t)_Alignof(%s)," % cname)
    out.append("    0,")
    out.append("    %s," % nmethod)
    out.append("    NULL,")
    out.append("    %s," % mtab)
    out.append("    NULL,")
    out.append("    NULL,")
    out.append("    0,")
    out.append("    0,")
    out.append("    NULL,")
    out.append("};")
    out.append("")

source = """/* Derived from Go's src/debug/elf/elf.go.
 * Go source: %s.
 *
 * Generated by tools/gen-debug-elf-consts.sh from the Go source. Do not edit.
 *
 * The tables of names Go keeps next to each const block of elf.go, in Go's
 * order, which is the order stringName and flagName look through them in, and
 * the String and GoString functions and type descriptors of the named integer
 * types.
 *
%s
#include "burrow/debug/elf.h"

#include "burrow/error.h"

#include <string.h>

typedef struct ElfIntName {
    uint32_t i;
    Str s;
} ElfIntName;

#define ELF_NNAME(t) ((Int)(sizeof(t) / sizeof((t)[0])))

/* Big enough for the longest name either function below can make. */
typedef struct ElfNameBuf {
    Byte p[%d];
    Int len;
} ElfNameBuf;

static void elf_name_put(ElfNameBuf *b, Str s) {
    memcpy(b->p + b->len, s.p, (size_t)s.len);
    b->len += s.len;
}

static void elf_name_uint(ElfNameBuf *b, uint32_t u, uint32_t base) {
    Byte digits[16];
    Int n = 0;
    do {
        digits[n++] = (Byte) "0123456789abcdef"[u %% base];
        u /= base;
    } while (u != 0);
    while (n > 0)
        b->p[b->len++] = digits[--n];
}

static Str elf_name_clone(Alloc *a, const ElfNameBuf *b) {
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)b->len, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, b->p, (size_t)b->len);
    return str_from_bytes(p, b->len);
}

/* stringName. */
static Str elf_string_name(uint32_t i, const ElfIntName *names, Int n, bool go_syntax,
                           Alloc *a) {
    ElfNameBuf b = {.len = 0};
    for (Int j = 0; j < n; j++) {
        if (names[j].i == i) {
            if (!go_syntax)
                return names[j].s;
            elf_name_put(&b, BURROW_S("elf."));
            elf_name_put(&b, names[j].s);
            return elf_name_clone(a, &b);
        }
    }
    /* Then the largest smaller value, which the tables being in order makes
     * the last one smaller. */
    for (Int j = n - 1; j >= 0; j--) {
        if (names[j].i < i) {
            if (go_syntax)
                elf_name_put(&b, BURROW_S("elf."));
            elf_name_put(&b, names[j].s);
            elf_name_put(&b, BURROW_S("+"));
            elf_name_uint(&b, i - names[j].i, 10);
            return elf_name_clone(a, &b);
        }
    }
    elf_name_uint(&b, i, 10);
    return elf_name_clone(a, &b);
}

/* flagName. */
static Str elf_flag_name(uint32_t i, const ElfIntName *names, Int n, bool go_syntax,
                         Alloc *a) {
    ElfNameBuf b = {.len = 0};
    for (Int j = 0; j < n; j++) {
        if ((names[j].i & i) == names[j].i) {
            if (b.len > 0)
                elf_name_put(&b, BURROW_S("+"));
            if (go_syntax)
                elf_name_put(&b, BURROW_S("elf."));
            elf_name_put(&b, names[j].s);
            i -= names[j].i;
        }
    }
    if (b.len == 0) {
        elf_name_put(&b, BURROW_S("0x"));
        elf_name_uint(&b, i, 16);
        return elf_name_clone(a, &b);
    }
    if (i != 0) {
        elf_name_put(&b, BURROW_S("+0x"));
        elf_name_uint(&b, i, 16);
    }
    return elf_name_clone(a, &b);
}

%s""" % (gover, LICENCE, bufsize, "\n".join(out))


def write(path, text):
    with open(os.path.join(root, path), "w") as f:
        f.write(text)


write("include/burrow/debug/elf_const.h", header)
write("src/debug/elf_string.c", source)
PY

for f in include/burrow/debug/elf_const.h src/debug/elf_string.c; do
	if command -v clang-format >/dev/null 2>&1; then
		clang-format -i "$root/$f"
	fi
done
