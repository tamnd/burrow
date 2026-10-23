#!/usr/bin/env python3
# A function that takes `Error *err` writes it on every return.
#
# That includes BURROW_NO_ERROR on success, and callers lean on it: Go code
# reads the err that comes back without having set it first, and the C that
# follows it does the same with an `Error err;` it never initialised. A
# function that returns early on success without writing err hands its caller
# whatever was on the stack, and the caller takes that for a failure, or worse,
# for no failure when there was one. This is how TestServeTLS came to hang
# (#597).
#
# So this follows every return in every function in src/ with an `Error *err`
# parameter, and the fall off the end of a void one, back to one of:
#
#   - a write, `*err = ...` or `BURROW_OUT(err, ...)`;
#   - a call that is passed err, which writes it in turn;
#   - an `if (err != NULL)` whose other branch is the write, since a NULL err
#     needs nothing;
#   - a macro defined in the same file that does one of the above with err.
#
# A function whose first use of err reads *err is carrying an earlier error
# through, the sticky error style, and is left alone.
#
# It works on the source as written, without the preprocessor, so it checks
# the house style rather than proving anything. It doesn't look inside
# callees, it counts a write inside a loop as maybe not done, and it gives up
# on a function it can't parse, which it reports. A function it gets wrong, or
# one that is right not to write err, goes in tools/errout-baseline.txt with
# the reason. So do the ones nobody has fixed yet; that list should only get
# shorter.
#
# Usage: tools/check-errout.py [-l] [file.c ...]
# With no files it checks every file in src/. -l lists every finding, baseline
# or not, in the baseline's format.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

import glob
import os
import re
import sys

TOKEN = re.compile(
    r"""(?P<ws>\s+)
      | (?P<id>[A-Za-z_]\w*)
      | (?P<num>\.?\d(?:[\w.]|[eEpP][+-])*)
      | (?P<op>->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|\.\.\.|[^\s\w])
    """,
    re.X,
)

IDENT = re.compile(r"[A-Za-z_]\w*$")

# Conditions under which err is NULL, or isn't.
ISNULL = (["!", "err"], ["err", "==", "NULL"], ["NULL", "==", "err"])
NONNULL = (["err"], ["err", "!=", "NULL"], ["NULL", "!=", "err"])


def strip(src):
    """Blanks comments, string and character literals and preprocessor lines,
    keeping the newlines so that line numbers stay right."""
    out = []
    i, n = 0, len(src)
    bol = True
    while i < n:
        c = src[i]
        if bol and c in " \t":
            out.append(c)
            i += 1
            continue
        if bol and c == "#":
            while i < n and src[i] != "\n":
                if src.startswith("\\\n", i):
                    out.append("\n")
                    i += 1
                i += 1
            continue
        bol = False
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(" " + "\n" * src.count("\n", i, j))
            i = j
            continue
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(" 0 " + "\n" * src.count("\n", i, j))
            i = j + 1
            continue
        if c == "\n":
            bol = True
        out.append(c)
        i += 1
    return "".join(out)


def tokens(src):
    line = 1
    toks = []
    for m in TOKEN.finditer(src):
        text = m.group()
        if m.lastgroup != "ws":
            toks.append((text, line))
        line += text.count("\n")
    return toks


class ParseError(Exception):
    pass


class Parser:
    """Splits a function body into statements. The statements are tuples:

    ("block", line, [stmt])         ("return", line, toks)
    ("expr", line, toks)            ("goto", line, label)
    ("if", line, cond, then, else)  ("break", line), ("continue", line)
    ("while"|"for"|"switch", line, head, body)
    ("do", line, body, cond)        ("case", line, is_default)
    ("label", line, name)
    """

    def __init__(self, toks, i):
        self.t = toks
        self.i = i

    def peek(self, k=0):
        j = self.i + k
        return self.t[j][0] if j < len(self.t) else ""

    def line(self):
        return self.t[min(self.i, len(self.t) - 1)][1]

    def take(self, want=None):
        if self.i >= len(self.t):
            raise ParseError("end of file")
        text, line = self.t[self.i]
        if want is not None and text != want:
            raise ParseError("want %r at line %d, got %r" % (want, line, text))
        self.i += 1
        return text

    def group(self):
        self.take("(")
        depth = 1
        start = self.i
        while depth:
            text = self.take()
            depth += text == "("
            depth -= text == ")"
        return [t for t, _ in self.t[start : self.i - 1]]

    def until_semi(self):
        out = []
        depth = 0
        while True:
            line = self.line()
            text = self.take()
            if text in ("(", "[", "{"):
                depth += 1
            elif text in (")", "]", "}"):
                depth -= 1
                if depth < 0:
                    raise ParseError("unbalanced %r at line %d" % (text, line))
            elif text == ";" and depth == 0:
                return out
            out.append(text)

    def block(self):
        line = self.line()
        self.take("{")
        body = []
        while self.peek() != "}":
            body.append(self.stmt())
        self.take("}")
        return ("block", line, body)

    def stmt(self):
        text = self.peek()
        line = self.line()
        if text == "{":
            return self.block()
        if text == "if":
            self.take()
            cond = self.group()
            then = self.stmt()
            other = None
            if self.peek() == "else":
                self.take()
                other = self.stmt()
            return ("if", line, cond, then, other)
        if text in ("while", "for", "switch"):
            self.take()
            head = self.group()
            return (text, line, head, self.stmt())
        if text == "do":
            self.take()
            body = self.stmt()
            self.take("while")
            cond = self.group()
            self.take(";")
            return ("do", line, body, cond)
        if text == "return":
            self.take()
            return ("return", line, self.until_semi())
        if text == "goto":
            self.take()
            label = self.take()
            self.take(";")
            return ("goto", line, label)
        if text in ("break", "continue"):
            self.take()
            self.take(";")
            return (text, line)
        if text == "case":
            depth = 0
            while not (self.peek() == ":" and depth == 0):
                t = self.take()
                depth += t in ("(", "[", "{")
                depth -= t in (")", "]", "}")
            self.take(":")
            return ("case", line, False)
        if text == "default" and self.peek(1) == ":":
            self.take()
            self.take()
            return ("case", line, True)
        if IDENT.match(text) and self.peek(1) == ":":
            self.take()
            self.take()
            return ("label", line, text)
        return ("expr", line, self.until_semi())


def functions(toks):
    """Yields (name, line, void, index of the body's '{') for each function
    definition with an `Error *err` parameter."""
    depth = 0
    i = 0
    n = len(toks)
    while i < n:
        text = toks[i][0]
        if text == "{":
            depth += 1
        elif text == "}":
            depth -= 1
        elif depth == 0 and text == "(" and i > 0 and IDENT.match(toks[i - 1][0]):
            j = i + 1
            d = 1
            while j < n and d:
                d += toks[j][0] == "("
                d -= toks[j][0] == ")"
                j += 1
            params = [t for t, _ in toks[i + 1 : j - 1]]
            if j < n and toks[j][0] == "{" and has_err_param(params):
                void = i > 1 and toks[i - 2][0] == "void"
                yield toks[i - 1][0], toks[i - 1][1], void, j
            i = j
            continue
        i += 1


def has_err_param(params):
    depth = 0
    cur = []
    for t in params + [","]:
        if t == "," and depth == 0:
            if cur == ["Error", "*", "err"]:
                return True
            cur = []
            continue
        depth += t in ("(", "[")
        depth -= t in (")", "]")
        cur.append(t)
    return False


def passes_err(toks, k):
    """Reports whether the err at toks[k] is written or handed on."""
    prev = toks[k - 1] if k else ""
    nxt = toks[k + 1] if k + 1 < len(toks) else ""
    if prev == "*" and nxt == "=" and (k < 2 or toks[k - 2] in ("(", ",", ")")):
        return True
    if prev in ("(", ",") and nxt in (")", ","):
        return True
    # Stored in a variable or a field, which something else writes through.
    return prev == "=" and nxt in (";", ",", "")


def writer_macros(src):
    """The function-like macros in src that write err or hand it on, like
    `#define FAIL(m) return fail(err, m)`."""
    out = set()
    pat = re.compile(
        r"^[ \t]*#[ \t]*define[ \t]+(\w+)\(([^)]*)\)((?:[^\n]*\\\n)*[^\n]*)", re.M
    )
    for m in pat.finditer(src):
        params = m.group(2).replace(" ", "").split(",")
        if "err" in params:
            continue
        toks = [t for t, _ in tokens(strip(m.group(3).replace("\\\n", " ")))]
        if any(t == "err" and passes_err(toks, k) for k, t in enumerate(toks)):
            out.add(m.group(1))
    return out


def noreturn_names(root):
    names = {"abort", "exit", "_Exit", "longjmp", "_longjmp"}
    pat = re.compile(
        r"BURROW_NORETURN\s+(?:static\s+)?(?:inline\s+)?"
        r"\w+\s*\(?\s*\*?\s*(\w+)\s*\)?\s*\("
    )
    paths = glob.glob(os.path.join(root, "include/burrow/**/*.h"), recursive=True)
    paths += glob.glob(os.path.join(root, "src/**/*.[ch]"), recursive=True)
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as f:
            names.update(m.group(1) for m in pat.finditer(f.read()))
    # The _v forms are macros over the Slice forms.
    return names | {n + "_v" for n in names}


class Checker:
    """Walks a body with a state: True when err has been written on the way
    here, False when it hasn't, and None when nothing gets here."""

    def __init__(self, noreturn, macros):
        self.noreturn = noreturn
        self.macros = macros

    def writes(self, toks):
        for k, t in enumerate(toks):
            if t == "err" and passes_err(toks, k):
                return True
            if t in self.macros and k + 1 < len(toks) and toks[k + 1] == "(":
                return True
        return False

    def stops(self, toks):
        calls = (t for k, t in enumerate(toks) if toks[k + 1 : k + 2] == ["("])
        return any(t in self.noreturn for t in calls)

    @staticmethod
    def meet(states):
        states = [s for s in states if s is not None]
        return all(states) if states else None

    def loop(self, kind, head, body, w):
        save, self.brk = self.brk, []
        if kind == "switch":
            self.sw.append(w)
            after = self.walk(body, None)
            self.sw.pop()
        else:
            after = self.walk(body, w)
        exits, self.brk = self.brk, save
        if kind == "switch":
            outs = exits + [after]
            cases = body[2] if body[0] == "block" else []
            if not any(c[0] == "case" and c[2] for c in cases):
                outs.append(w)
        elif (kind == "for" and ";" in head and head[head.index(";") + 1] == ";") or (
            kind == "while" and head in (["1"], ["true"])
        ):
            # No condition: only a break gets out.
            outs = exits
        else:
            # The body may not run at all.
            outs = exits + [w]
        return self.meet(outs)

    def walk(self, s, w):
        kind = s[0]
        if kind == "block":
            for c in s[2]:
                w = self.walk(c, w)
            return w
        if kind == "expr":
            if w is None or self.stops(s[2]):
                return None
            return w or self.writes(s[2])
        if kind == "if":
            cond = s[2]
            if w is not None:
                w = w or self.writes(cond)
            # A NULL err needs nothing written.
            nonnull = w is not None and cond in NONNULL
            isnull = w is not None and cond in ISNULL
            a = self.walk(s[3], True if isnull else w)
            b = True if nonnull else w
            if s[4] is not None:
                b = self.walk(s[4], b)
            return self.meet([a, b])
        if kind in ("while", "for", "switch"):
            if w is not None:
                w = w or self.writes(s[2])
            return self.loop(kind, s[2], s[3], w)
        if kind == "do":
            save, self.brk = self.brk, []
            after = self.walk(s[2], w)
            exits, self.brk = self.brk, save
            if after is not None:
                after = after or self.writes(s[3])
            return self.meet(exits + [after])
        if kind == "return":
            if w is False and not self.writes(s[2]) and not self.stops(s[2]):
                self.bad.append(s[1])
            return None
        if kind == "goto":
            if w is not None:
                self.gotos.setdefault(s[2], []).append(w)
            return None
        if kind == "break":
            if w is not None:
                self.brk.append(w)
            return None
        if kind == "continue":
            return None
        if kind == "case":
            return self.meet([self.sw[-1] if self.sw else False, w])
        if kind == "label":
            return self.meet(self.seen.get(s[2], []) + [w])
        raise AssertionError(kind)

    def run(self, body, void):
        """Returns the lines of the returns that don't write err, with None
        for falling off the end."""
        self.seen = {}
        # Go round until the labels settle, so that a label sees the gotos
        # that come after it.
        for _ in range(8):
            self.bad = []
            self.gotos = {}
            self.brk = []
            self.sw = []
            end = self.walk(body, False)
            if self.gotos == self.seen:
                break
            self.seen = self.gotos
        if void and end is False:
            self.bad.append(None)
        return self.bad


def carries_err(toks, start):
    """Reports whether the first use of err in the body reads *err."""
    for k in range(start, len(toks) - 1):
        if toks[k][0] == "err":
            deref = toks[k - 1][0] == "*" and toks[k - 2][0] in ("(", "!", ",")
            return deref and toks[k + 1][0] != "="
    return False


def check_file(path, noreturn):
    """Returns (function, message) for each finding in path."""
    with open(path, encoding="utf-8", errors="replace") as f:
        src = f.read()
    toks = tokens(strip(src))
    checker = Checker(noreturn, writer_macros(src))
    out = []
    for name, line, void, start in functions(toks):
        if carries_err(toks, start):
            continue
        try:
            body = Parser(toks, start).block()
        except ParseError as e:
            out.append((name, "%s:%d: %s: not checked: %s" % (path, line, name, e)))
            continue
        for b in checker.run(body, void):
            where = "falls off the end" if b is None else "returns at line %d" % b
            msg = "%s:%d: %s %s without writing *err" % (path, line, name, where)
            out.append((name, msg))
    return out


def read_baseline(path):
    entries = set()
    if not os.path.exists(path):
        return entries
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                entries.add(tuple(line.split()[:2]))
    return entries


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    list_all = "-l" in argv
    files = [os.path.relpath(os.path.abspath(a), root) for a in argv if a != "-l"]
    os.chdir(root)
    whole = not files
    if whole:
        files = sorted(glob.glob("src/**/*.c", recursive=True))
    noreturn = noreturn_names(root)
    baseline = read_baseline("tools/errout-baseline.txt")
    found = set()
    failed = False
    for path in files:
        for name, msg in check_file(path, noreturn):
            if list_all and (path, name) not in found:
                print(path, name)
            found.add((path, name))
            if not list_all and (path, name) not in baseline:
                print(msg)
                failed = True
    if list_all:
        return 0
    if whole:
        for found_ok in sorted(baseline - found):
            print("tools/errout-baseline.txt: %s %s is fine now; remove it" % found_ok)
            failed = True
    sys.stdout.flush()
    if failed:
        print(
            "check-errout: write *err on every return; "
            "BURROW_OUT(err, BURROW_NO_ERROR) at the top is the easy way",
            file=sys.stderr,
        )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
