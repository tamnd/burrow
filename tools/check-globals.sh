#!/bin/sh
# Every mutable global in burrow's own source has to be in tools/globals.txt.
#
# Reentrancy is a requirement here rather than a preference, and the way that
# stays true over 180 packages is not discipline, it is a list. docs/design/
# 03-c-dialect.md section 5 says global mutable state is enumerated, justified
# and individually synchronised. This is the enumeration, and this script is what
# stops it from being a paragraph somebody wrote once.
#
# It fails in both directions. A mutable global that is not in the list fails,
# which is the case everybody expects. A line in the list whose global is gone
# also fails, which is the case that matters more, because a list that can rot is
# a list nobody reads and then nobody maintains.
#
# What counts as mutable is the C rule and not a guess: the object itself has to
# be writable. A const object is not mutable. A pointer to const is, because the
# pointer can be repointed, and that is state. A pointer that is itself const is
# not.
#
# What it will miss: a global declared through a macro, and a function pointer
# variable at file scope, which is written like a function declaration and cannot
# be told from one by looking at the line. Neither exists in this tree and both
# would be caught by the list going stale the moment anything else in the file
# changed.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

LIST=tools/globals.txt

# --others so that a global you have written but not staged is still checked,
# since that is exactly when somebody runs this.
sources=$(git ls-files --cached --others --exclude-standard \
	'src/*.c' 'src/**/*.c' 'include/**/*.h' 2>/dev/null || true)
[ -n "$sources" ] || exit 0

found=$(mktemp)
listed=$(mktemp)
cats=$(mktemp)
trap 'rm -f "$found" "$listed" "$cats"' EXIT INT TERM

# shellcheck disable=SC2086
awk '
# Comments and string literals go first. A brace inside either one is not a
# brace, and getting that wrong means every declaration after it is judged at
# the wrong nesting depth.
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

FNR == 1 { incomment = 0; depth = 0; incont = 0 }

{
    raw = $0
    line = strip(raw)

    # A preprocessor directive can run over several lines, and the lines after
    # the first do not look like directives. A multi line macro is where most of
    # the things that look like a declaration and are not live.
    wascont = incont
    incont = (raw ~ /\\[ \t]*$/) && (wascont || line ~ /^[ \t]*#/)
    if (wascont || line ~ /^[ \t]*#/) next

    # Depth entering the line is what decides file scope, because an initialiser
    # opens a brace on the very line the declaration is on.
    before = depth
    depth += gsub(/\{/, "{", line)
    depth -= gsub(/\}/, "}", line)
    if (before != 0) next

    if (line ~ /(^|[^a-zA-Z0-9_])typedef([^a-zA-Z0-9_]|$)/) next
    if (line ~ /^[ \t]*extern([^a-zA-Z0-9_]|$)/) next   # a declaration; the definition is elsewhere
    # A tag declared ahead of its definition, struct T; with nothing after the
    # tag, declares no object. Neither does an enum body, whose = gives a
    # constant its value.
    if (line ~ /^[ \t]*(struct|union|enum)[ \t]+[A-Za-z_][a-zA-Z0-9_]*[ \t]*;/) next
    if (line ~ /^[ \t]*enum([ \t]+[A-Za-z_][a-zA-Z0-9_]*)?[ \t]*\{/) next
    if (line !~ /[;=]/) next
    if (line !~ /^[ \t]*[A-Za-z_]/) next

    # Everything up to the first = , or to the ; when there is no initialiser.
    lhs = line
    sub(/=.*$/, "", lhs)
    sub(/;.*$/, "", lhs)

    # A paren on the left of the = means a function, or a function pointer that
    # is written like one. An initialiser is allowed to call something, so this
    # is decided on the declarator and not on the whole line.
    if (index(lhs, "(") > 0) next

    sub(/\[[^]]*\][ \t]*$/, "", lhs)   # an array is still one object
    sub(/[ \t]+$/, "", lhs)
    if (lhs !~ /[A-Za-z_][a-zA-Z0-9_]*$/) next

    name = lhs
    sub(/^.*[^a-zA-Z0-9_]/, "", name)
    decl = substr(lhs, 1, length(lhs) - length(name))

    # The C rule. const before any * qualifies the object; after the last * it
    # qualifies the pointer, which is the same thing when there is no pointer.
    tail = decl
    if (index(decl, "*") > 0) {
        while (index(tail, "*") > 0)
            tail = substr(tail, index(tail, "*") + 1)
    }
    if (tail ~ /(^|[^a-zA-Z0-9_])const([^a-zA-Z0-9_]|$)/) next

    printf "%s\t%s\n", FILENAME, name
}
' $sources | sort > "$found"

awk -F '\t' '
$1 == "category" { print $2 > CATS; next }
$1 == "global"   { print $2 "\t" $3 }
' CATS="$cats" "$LIST" | sort > "$listed"

status=0

# A global in the tree and not in the list.
while IFS="$(printf '\t')" read -r file name; do
	[ -n "$file" ] || continue
	if ! grep -qxF "$(printf '%s\t%s' "$file" "$name")" "$listed"; then
		printf '%s: %s is a mutable global and is not in %s\n' "$file" "$name" "$LIST" >&2
		status=1
	fi
done < "$found"

# A line in the list whose global has gone.
while IFS="$(printf '\t')" read -r file name; do
	[ -n "$file" ] || continue
	if ! grep -qxF "$(printf '%s\t%s' "$file" "$name")" "$found"; then
		printf '%s: %s is listed in %s and no longer exists\n' "$file" "$name" "$LIST" >&2
		status=1
	fi
done < "$listed"

# Every entry has to name a category that the list itself declares, and every
# entry has to say how the thing is synchronised. A blank last column is a
# global nobody thought about.
awk -F '\t' -v cats="$cats" '
BEGIN { while ((getline c < cats) > 0) known[c] = 1 }
$1 == "global" {
    if (NF < 5 || $5 == "") {
        printf "%s:%d: %s says nothing about how it is synchronised\n", FILENAME, FNR, $3 > "/dev/stderr"
        bad = 1
    }
    if (!($4 in known)) {
        printf "%s:%d: %s is in category \"%s\", which this file does not declare\n", FILENAME, FNR, $3, $4 > "/dev/stderr"
        bad = 1
    }
}
END { exit bad ? 1 : 0 }
' "$LIST" || status=1

if [ "$status" -ne 0 ]; then
	echo "" >&2
	echo "Global mutable state is enumerated. See docs/design/03-c-dialect.md section 5." >&2
	exit 1
fi

echo "ok	globals	$(wc -l < "$found" | tr -d ' ') enumerated"
