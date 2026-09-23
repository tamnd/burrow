#!/bin/sh
# Every file scope name in burrow's own source has to be unique across files.
#
# A static function in one file and a static function with the same name in
# another are two different functions when the files are compiled one at a time,
# which is how the Makefile builds them. They are one name defined twice when the
# files are pasted into a single burrow.c, which is how the amalgamation builds
# them, and the amalgamation is the build most people will use. So the rule is
# that a static function, a static object, a struct tag or a typedef name is
# written once in the whole tree, and this is what keeps it true.
#
# Macros are not checked. The amalgamation takes back every macro a file defines
# at the end of that file, so two files can both say STACK_FLOOR and mean
# different things by it, which is often clearer than making one of them longer.
#
# Two files that can never be in the same build are allowed to share names. That
# is a file named for its platform, futex_posix.c beside futex_windows.c, where
# the whole of each is inside a guard for the platform it is named after. The
# suffixes that mean this are the list in ALTERNATES below, and sharing names is
# the point of them: the two files are one file written twice.
#
# What it will miss: a declaration that does not name itself on its first line,
# like a static table of an unnamed struct type. Compiling the amalgamation
# catches those, and this is here so that the common case fails in a second on
# any machine rather than in CI on one.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

ALTERNATES="posix windows linux bsd darwin completion readiness"

sources=$(git ls-files --cached --others --exclude-standard \
	'src/*.c' 'src/**/*.c' 2>/dev/null || true)
[ -n "$sources" ] || exit 0

# shellcheck disable=SC2086
awk -v alternates="$ALTERNATES" '
BEGIN {
    n = split(alternates, a, " ")
    for (i = 1; i <= n; i++) alt[a[i]] = 1
}

# The same stripping as tools/check-globals.sh, for the same reason: a brace in
# a comment or a string is not a brace.
function strip(s,   out, i, c, q) {
    out = ""
    i = 1
    while (i <= length(s)) {
        c = substr(s, i, 1)
        if (incomment) {
            if (substr(s, i, 2) == "*/") { incomment = 0; i += 2; continue }
            i++
            continue
        }
        if (substr(s, i, 2) == "/*") { incomment = 1; i += 2; continue }
        if (substr(s, i, 2) == "//") break
        if (c == "\"" || c == "'"'"'") {
            q = c
            i++
            while (i <= length(s) && substr(s, i, 1) != q) {
                if (substr(s, i, 1) == "\\") i++
                i++
            }
            i++
            continue
        }
        out = out c
        i++
    }
    return out
}

# Which build a file belongs to. A platform alternate belongs to its directory
# and stem, so futex_posix.c and futex_windows.c land in the same place, and
# every other file is a place of its own.
function group(f,   base, stem, tag, k) {
    base = f
    sub(/\.c$/, "", base)
    k = match(base, /_[a-z0-9]+$/)
    if (k > 0) {
        tag = substr(base, k + 1)
        stem = substr(base, 1, k - 1)
        if (tag in alt) return stem
    }
    return f
}

function note(name, f,   g) {
    if (name == "" || name in seen_here) return
    seen_here[name] = 1
    g = group(f)
    if (!((name, g) in pair)) {
        pair[name, g] = 1
        groups[name]++
    }
    files[name] = files[name] " " f
}

FNR == 1 {
    incomment = 0; depth = 0; incont = 0; intypedef = 0
    for (k in seen_here) delete seen_here[k]
}

{
    raw = $0
    line = strip(raw)

    wascont = incont
    incont = (raw ~ /\\[ \t]*$/) && (wascont || line ~ /^[ \t]*#/)
    if (wascont || line ~ /^[ \t]*#/) next

    before = depth
    depth += gsub(/\{/, "{", line)
    depth -= gsub(/\}/, "}", line)

    # The name at the end of a typedef whose body ran over several lines.
    if (intypedef && before > 0 && depth == 0) {
        intypedef = 0
        if (match(line, /[A-Za-z_][A-Za-z0-9_]*[ \t]*;/)) {
            name = substr(line, RSTART, RLENGTH)
            sub(/[ \t]*;$/, "", name)
            note(name, FILENAME)
        }
        next
    }
    if (before != 0) next

    # A struct, union or enum tag, with or without a typedef around it.
    if (match(line, /(struct|union|enum)[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*\{/)) {
        tag = substr(line, RSTART, RLENGTH)
        sub(/^(struct|union|enum)[ \t]+/, "", tag)
        sub(/[ \t]*\{$/, "", tag)
        note(tag, FILENAME)
    }

    if (line ~ /^typedef([^A-Za-z0-9_]|$)/) {
        if (depth > 0) { intypedef = 1; next }
        # A function pointer typedef names itself inside the first parens.
        if (match(line, /\([ \t]*\*[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\)/)) {
            name = substr(line, RSTART, RLENGTH)
            gsub(/[()* \t]/, "", name)
        } else if (match(line, /[A-Za-z_][A-Za-z0-9_]*[ \t]*(\[[^]]*\])?[ \t]*;/)) {
            name = substr(line, RSTART, RLENGTH)
            sub(/[ \t]*(\[[^]]*\])?[ \t]*;$/, "", name)
        } else {
            name = ""
        }
        note(name, FILENAME)
        next
    }

    if (line !~ /^static([^A-Za-z0-9_]|$)/) next

    # The declarator ends at the first paren for a function, and otherwise at
    # the first bracket, equals sign or semicolon. The name is the identifier
    # just before that.
    lhs = line
    p = index(lhs, "(")
    if (p > 0) {
        lhs = substr(lhs, 1, p - 1)
    } else {
        sub(/[\[=;].*$/, "", lhs)
    }
    sub(/[ \t]+$/, "", lhs)
    if (match(lhs, /[A-Za-z_][A-Za-z0-9_]*$/)) {
        name = substr(lhs, RSTART, RLENGTH)
        # A line that stops at a type, like the head of a static table of an
        # unnamed struct, has no name on it yet.
        if (name !~ /^(struct|union|enum|const|static|inline|void|int|char|bool)$/)
            note(name, FILENAME)
    }
}

END {
    bad = 0
    for (name in groups) {
        if (groups[name] < 2) continue
        printf "%s is defined at file scope in more than one file:%s\n", name, files[name]
        bad = 1
    }
    if (bad) {
        print ""
        print "The amalgamation puts every file in one translation unit, so a file scope"
        print "name has to be unique across the tree. Prefix it with the file it is in."
        exit 1
    }
}
' $sources
