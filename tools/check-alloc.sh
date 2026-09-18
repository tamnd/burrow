#!/bin/sh
# Nothing in burrow's own source may ignore a failed allocation.
#
# This is the half of the failure policy that a test cannot reach. The policy
# says a refused allocation comes back as NULL, turns into
# burrow_err_out_of_memory at the first function with an Error to return, and
# never ends the process. All of that rests on somebody having looked at the
# NULL, and a call site that forgets is not wrong at compile time. It is a
# dereference of NULL later, in a build where memory ran out, which is the build
# nobody is running under a debugger.
#
# So every call to an allocating function in src/ has to do one of three things
# with the result:
#
#   1. Assign it to a variable and test that variable against NULL within the
#      next few lines, which is what nearly every call site already does.
#   2. Assign it to a variable and return that variable, which hands the NULL to
#      a caller who is under this same rule.
#   3. Return the call directly, for the same reason.
#
# Anything else, including calling one of these and dropping the result on the
# floor, is a failure. So is dereferencing the pointer before the test, which
# is the version of this bug that looks checked in review because the check is
# right there, two lines under the crash.
#
# The window in rule 1 is six lines on purpose. A NULL test twenty lines below
# the allocation is a NULL test that has already been walked past, and keeping
# the two next to each other is worth more than the occasional rewrite it costs.
#
# What it will miss: a pointer handed to another function before the test. That
# one cannot be decided by looking at the line, since a good few functions in
# this library take a NULL on purpose and are supposed to.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

sources=$(git ls-files --cached --others --exclude-standard \
	'src/*.c' 'src/**/*.c' 2>/dev/null || true)
[ -n "$sources" ] || exit 0

status=0

# shellcheck disable=SC2086
awk '
# Comments and string literals go, for the same reason as in the other checkers.
# A NULL inside a message is not a NULL test and a call inside a comment is not
# a call.
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

# A whole file is read before anything is decided, because the answer to "was
# this checked" is usually on a line that has not been read yet. ENDFILE would
# be the obvious hook and it is a gawk extension, so the previous file is dealt
# with on the first line of the next one instead.
FNR == 1 { scan(); file = FILENAME; incomment = 0; nlines = 0; delete done }

{ nlines++; text[nlines] = strip($0); raw[nlines] = $0 }

END { scan(); exit bad ? 1 : 0 }

function scan(   i) {
    for (i = 1; i <= nlines; i++) {
        if (text[i] ~ /(^|[^a-zA-Z0-9_])(mem_alloc|mem_alloc_nozero|mem_alloc_array|mem_realloc|BURROW_NEW|BURROW_NEW_N)[ \t]*\(/ ||
            text[i] ~ /->(alloc|alloc_zeroed|realloc)[ \t]*\(/)
            one(i)
    }
    nlines = 0
}

# A definition is not a call. It has the name at the start of the line with a
# return type in front of it, no semicolon, and nothing assigned.
function isdef(s) {
    return s ~ /^[a-zA-Z_][^;=]*\)[ \t]*\{?[ \t]*$/ && s !~ /return/
}

function one(i,   stmt, s, j, lhs, name, k, first) {
    if (isdef(text[i])) return

    # Find where the statement starts, which is not always the line the call is
    # on. A wrapped assignment puts the name and the = above and the call below,
    # and looking only downwards from the call finds no = and calls a perfectly
    # good line a fault.
    s = i
    while (s > 1 && text[s - 1] !~ /[;{}:][ \t]*$/ && text[s - 1] !~ /^[ \t]*$/)
        s--
    if (s in done) return
    done[s] = 1

    # Then gather forward to the semicolon, since the call can wrap either way.
    stmt = text[s]
    j = s
    while (stmt !~ /;/ && j < nlines) {
        j++
        stmt = stmt " " text[j]
    }
    i = s

    # Returned straight out, so the caller is the one under the rule now.
    if (stmt ~ /(^|[^a-zA-Z0-9_])return[ \t(]/) return

    # A call inside the condition of an if or a while is checked by
    # construction, since what follows only runs when it worked.
    if (stmt ~ /^[ \t]*(if|while)[ \t]*\(/) return

    # Otherwise it has to land somewhere with a name on it.
    lhs = stmt
    if (!sub(/=[^=].*$/, "", lhs)) {
        report(i, "the result is not used")
        return
    }
    name = lhs
    gsub(/[^a-zA-Z0-9_]+$/, "", name)
    sub(/^.*[^a-zA-Z0-9_]/, "", name)
    if (name == "") {
        report(i, "the result goes somewhere this cannot follow")
        return
    }

    first = 1
    for (k = j + 1; k <= nlines && k <= j + 6; k++) {
        if (text[k] ~ /^[ \t]*$/) continue
        if (tests(text[k], name)) return

        # Handing the pointer straight back is a check, but only if it is the
        # very next thing. Anywhere further down there is room for a line that
        # touched it on the way, and a return six lines below a dereference does
        # not make the dereference safe.
        if (first && handsback(text[k], name)) return
        first = 0

        if (derefs(text[k], name)) {
            report(i, name " is used before it is checked")
            return
        }
    }
    report(i, name " is never checked against NULL")
}

# Only the forms that are certainly a dereference. Passing the pointer to
# another function is not one of them, because plenty of functions here take a
# NULL on purpose, so this says nothing about those.
function derefs(s, name) {
    if (s ~ ("(^|[^a-zA-Z0-9_])" name "[ \t]*(\\[|->|\\.)")) return 1
    if (s ~ ("\\*[ \t]*" name "([^a-zA-Z0-9_]|$)")) return 1
    return 0
}

function tests(s, name) {
    if (s ~ ("(^|[^a-zA-Z0-9_])" name "[ \t]*[!=]=[ \t]*NULL")) return 1
    if (s ~ ("(^|[^a-zA-Z0-9_])NULL[ \t]*[!=]=[ \t]*" name "([^a-zA-Z0-9_]|$)")) return 1
    if (s ~ ("![ \t]*" name "([^a-zA-Z0-9_]|$)")) return 1
    return 0
}

function handsback(s, name) {
    return s ~ ("^[ \t]*return[ \t]+" name "[ \t]*;")
}

function report(i, why) {
    printf "%s:%d: unchecked allocation, %s\n", file, i, why
    printf "%s:%d:   %s\n", file, i, raw[i]
    bad = 1
}
' $sources || status=1

if [ "$status" -ne 0 ]; then
	echo "" >&2
	echo "A failed allocation has to go somewhere. See docs/design/05-memory.md section 7." >&2
	exit 1
fi

echo "ok	alloc	$(printf '%s\n' "$sources" | wc -l | tr -d ' ') files"
