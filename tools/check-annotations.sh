#!/bin/sh
# Every function that hands back a pointer has to say whose pointer it is.
#
# burrow's answer to ownership is one annotation per declaration, in the header,
# where the caller reads it. That only works if the annotations are actually
# there and actually name real parameters, and neither of those survives on
# discipline. A declaration added without one is not wrong at compile time, it
# is just silent, and silence is exactly what the annotations exist to remove.
#
# Two rules, both structural:
#
#   1. A declaration whose return type carries a pointer needs an OWNS, a
#      BORROWS or a STATIC naming ret. Str and Slice carry a pointer inside
#      them, Error and Any are interface values with a receiver pointer, and
#      anything spelled with a star is obvious.
#
#   2. Every name inside an annotation is either ret, something under ret, or a
#      parameter of that same declaration. This is what catches the annotation
#      that was right until somebody renamed the parameter.
#
# What it cannot check is whether the annotation is true. That needs the memory
# to exist, so it happens in tests/lifetime_test.c at runtime instead.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

headers=$(git ls-files --cached --others --exclude-standard \
	'include/burrow/*.h' 'include/burrow/**/*.h' 2>/dev/null || true)
[ -n "$headers" ] || exit 0

# shellcheck disable=SC2086
awk '
# Strip comments and string literals, which are the only places the punctuation
# below means something other than what it looks like.
function strip(s,   out, i, c) {
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
        if (c == "\"") {
            i++
            while (i <= length(s) && substr(s, i, 1) != "\"") {
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

FNR == 1 { incomment = 0; indirective = 0; depth = 0; decl = "" }

{
    line = strip($0)

    # Preprocessor lines are not declarations, and one of them can run over
    # several lines with a backslash. Following the continuation matters: the
    # second line of a multi line #if does not start with a # and otherwise
    # arrives here looking like code.
    if (indirective || line ~ /^[ \t]*#/) {
        indirective = (line ~ /\\[ \t]*$/)
        next
    }

    # Everything inside a struct, a union, an enum or a function body is not a
    # declaration this cares about, and the brace depth is how that is known.
    n = split(line, ch, "")
    buf = ""
    for (i = 1; i <= n; i++) {
        c = ch[i]
        if (c == "}") { if (depth > 0) depth--; decl = ""; continue }
        if (depth > 0) { if (c == "{") depth++; continue }
        if (c == "{") {
            # extern "C" { wraps the whole header and its brace is not a scope
            # anything here cares about. Without this the depth never comes back
            # to zero and the checker passes every file by looking at none of it,
            # which is the most expensive kind of green. The string literal is
            # already gone by here, so what is left to match is the keyword and
            # the space that was in front of the quote.
            kw = decl
            gsub(/[ \t]+$/, "", kw)
            if (kw == "extern") { decl = ""; continue }
            depth++
            check(decl, FILENAME, start)
            decl = ""
            continue
        }
        if (c == ";") { check(decl, FILENAME, start); decl = ""; continue }
        if (decl == "" && c ~ /[ \t]/) continue
        if (decl == "") start = FNR
        decl = decl c
    }
    # A declaration that spans lines needs a separator where the newline was.
    if (decl != "") decl = decl " "
}

function check(d, file, ln,   ann, rest, name, params, i, j, parts, tok, plist, np, pname, want, m, arr) {
    gsub(/[ \t]+/, " ", d)
    sub(/^ /, "", d)
    sub(/ $/, "", d)
    if (d == "") return

    # Peel the annotations off the front and remember every name in them.
    delete named
    nnamed = 0
    while (match(d, /^BURROW_(OWNS|BORROWS|RETAINS|STATIC)\(/)) {
        kind = d
        sub(/\(.*/, "", kind)
        sub(/^BURROW_/, "", kind)
        # Find the closing paren of this annotation.
        rest = substr(d, RLENGTH + 1)
        level = 1
        for (i = 1; i <= length(rest); i++) {
            c = substr(rest, i, 1)
            if (c == "(") level++
            else if (c == ")") { level--; if (level == 0) break }
        }
        ann = substr(rest, 1, i - 1)
        d = substr(rest, i + 1)
        sub(/^ /, "", d)
        seen[kind] = 1
        np = split(ann, parts, ",")
        for (j = 1; j <= np; j++) {
            tok = parts[j]
            gsub(/^ +| +$/, "", tok)
            if (tok == "") continue
            named[++nnamed] = tok
        }
        if (kind != "RETAINS") hasret = hasret || (ann ~ /(^|[ ,])ret([ ,.]|$)/)
    }

    # Only function declarations from here on.
    if (d !~ /^[A-Za-z_].*\(.*\)$/) { hasret = 0; return }
    if (d ~ /^typedef/) { hasret = 0; return }

    # The return type is everything before the function name, and the name is
    # the last identifier before the first open paren.
    head = d
    sub(/\(.*/, "", head)
    if (head !~ /[A-Za-z_]$/) { hasret = 0; return }   # a function pointer typedef
    name = head
    sub(/.*[^A-Za-z0-9_]/, "", name)
    ret = head
    sub(/[A-Za-z0-9_]+$/, "", ret)
    gsub(/^ +| +$/, "", ret)
    sub(/^static inline /, "", ret)
    sub(/^static /, "", ret)

    # The parameter list, for rule two.
    params = d
    sub(/^[^(]*\(/, "", params)
    sub(/\)$/, "", params)
    delete plist
    np = split(params, arr, ",")
    for (i = 1; i <= np; i++) {
        pname = arr[i]
        gsub(/\[[^]]*\]/, "", pname)
        gsub(/^ +| +$/, "", pname)
        if (pname !~ /[A-Za-z0-9_]$/) continue
        sub(/.*[^A-Za-z0-9_]/, "", pname)
        plist[pname] = 1
    }

    for (i = 1; i <= nnamed; i++) {
        tok = named[i]
        sub(/\..*/, "", tok)
        if (tok == "ret") continue
        if (tok in plist) continue
        printf "%s:%d: %s: annotation names %s, which is not a parameter\n", file, ln, name, tok
        bad = 1
    }

    if (ret ~ /(^|[ *])(Str|Slice|Error|Any)$/ || ret ~ /\*$/) {
        if (!hasret) {
            printf "%s:%d: %s returns %s and says nothing about who owns it\n", file, ln, name, ret
            bad = 1
        }
    }
    hasret = 0
    delete seen
}

END { exit bad ? 1 : 0 }
' $headers || status=1

status=${status:-0}
if [ "$status" -eq 0 ]; then
	echo "ok	annotations	$(printf '%s\n' "$headers" | wc -l | tr -d ' ') headers"
fi
exit "$status"
