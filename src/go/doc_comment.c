/* go/doc/comment: the parser.
 *
 * Derived from Go's src/go/doc/comment/parse.go and std.go.
 * Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/doc/comment.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

BURROW_NORETURN static void dc_oom(void) {
    panic_str(S("go/doc/comment: out of memory"));
}

static void *dc_alloc(Alloc *a, size_t n, size_t align) {
    void *p = mem_alloc(a, n, align);
    if (p == NULL)
        dc_oom();
    return p;
}

static void dc_append(Alloc *a, Slice *s, const void *v) {
    Slice next = slice_append(a, *s, v, 1);
    if (next.len != s->len + 1)
        dc_oom();
    *s = next;
}

static Str dc_sub(Str s, Int lo, Int hi) {
    if (lo == hi)
        return BURROW_STR_EMPTY; /* s.p can be NULL, and NULL + 0 is undefined */
    return str_from_bytes(s.p + lo, hi - lo);
}

static Str dc_clone(Alloc *a, Str s) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Str c = str_clone(a, s);
    if (c.len != s.len)
        dc_oom();
    return c;
}

/* ------------------------------------------------------------------ std */

/* The standard library's packages with a one element import path, sorted, as
 * GOEXPERIMENT=none go list std gives them for go1.27.1. */
static const Str dc_std_pkgs[] = {
    BURROW_S_INIT("bufio"),    BURROW_S_INIT("bytes"),   BURROW_S_INIT("cmp"),
    BURROW_S_INIT("context"),  BURROW_S_INIT("crypto"),  BURROW_S_INIT("embed"),
    BURROW_S_INIT("encoding"), BURROW_S_INIT("errors"),  BURROW_S_INIT("expvar"),
    BURROW_S_INIT("flag"),     BURROW_S_INIT("fmt"),     BURROW_S_INIT("hash"),
    BURROW_S_INIT("html"),     BURROW_S_INIT("image"),   BURROW_S_INIT("io"),
    BURROW_S_INIT("iter"),     BURROW_S_INIT("log"),     BURROW_S_INIT("maps"),
    BURROW_S_INIT("math"),     BURROW_S_INIT("mime"),    BURROW_S_INIT("net"),
    BURROW_S_INIT("os"),       BURROW_S_INIT("path"),    BURROW_S_INIT("plugin"),
    BURROW_S_INIT("reflect"),  BURROW_S_INIT("regexp"),  BURROW_S_INIT("runtime"),
    BURROW_S_INIT("slices"),   BURROW_S_INIT("sort"),    BURROW_S_INIT("strconv"),
    BURROW_S_INIT("strings"),  BURROW_S_INIT("structs"), BURROW_S_INIT("sync"),
    BURROW_S_INIT("syscall"),  BURROW_S_INIT("testing"), BURROW_S_INIT("time"),
    BURROW_S_INIT("unicode"),  BURROW_S_INIT("unique"),  BURROW_S_INIT("unsafe"),
    BURROW_S_INIT("uuid"),     BURROW_S_INIT("weak"),
};

#define DC_NSTD ((Int)(sizeof(dc_std_pkgs) / sizeof(dc_std_pkgs[0])))

/* The list, for the test that checks it against the Go it came from. */
Int burrow__comment_std_pkgs(const Str **list);
Int burrow__comment_std_pkgs(const Str **list) {
    *list = dc_std_pkgs;
    return DC_NSTD;
}

static bool dc_is_std_pkg(Str path) {
    Int lo = 0, hi = DC_NSTD;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        int c = str_cmp(dc_std_pkgs[mid], path);
        if (c == 0)
            return true;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

Str comment_default_lookup_package(Str name, bool *ok) {
    *ok = dc_is_std_pkg(name);
    return *ok ? name : BURROW_STR_EMPTY;
}

/* --------------------------------------------------------------- bytes */

/* The byte sets of Go's 128-bit masks. */
static bool dc_alnum(Byte c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool dc_in(Byte c, const char *set) {
    return c != 0 && c < 0x80 && strchr(set, c) != NULL;
}

/* isHost reports whether c is a byte that can appear in a URL host, like
 * www.example.com or user@[::1]:8080. */
static bool dc_is_host(Byte c) {
    return dc_alnum(c) || dc_in(c, "_@-.[]:");
}

/* isPunct reports whether c is a punctuation byte that can appear inside a
 * path but not at the end. */
static bool dc_is_punct(Byte c) {
    return dc_in(c, ".,:;?!");
}

/* isPath reports whether c is a (non-punctuation) path byte. */
static bool dc_is_path(Byte c) {
    return dc_alnum(c) || dc_in(c, "$'()*+&#=@~_/-[]{}%");
}

/* isIdentASCII reports whether c is an ASCII identifier byte. */
static bool dc_is_ident_ascii(Byte c) {
    return dc_alnum(c) || c == '_';
}

static bool dc_import_path_ok(Byte c) {
    return dc_alnum(c) || dc_in(c, "-.~_+");
}

/* ------------------------------------------------------------- strings */

/* indented reports whether line is indented (starts with a leading space or
 * tab). */
static bool dc_indented(Str line) {
    return line.len > 0 && (line.p[0] == ' ' || line.p[0] == '\t');
}

/* isBlank reports whether s is a blank line. */
static bool dc_is_blank(Str s) {
    return s.len == 0 || (s.len == 1 && s.p[0] == '\n');
}

/* commonPrefix returns the longest common prefix of a and b. */
static Str dc_common_prefix(Str a, Str b) {
    Int i = 0;
    while (i < a.len && i < b.len && a.p[i] == b.p[i])
        i++;
    return dc_sub(a, 0, i);
}

/* leadingSpace returns the longest prefix of s consisting of spaces and
 * tabs. */
static Str dc_leading_space(Str s) {
    Int i = 0;
    while (i < s.len && (s.p[i] == ' ' || s.p[i] == '\t'))
        i++;
    return dc_sub(s, 0, i);
}

static Str dc_join(Alloc *a, const Str *lines, Int n, bool final_nl) {
    Int len = 0;
    for (Int i = 0; i < n; i++)
        len += lines[i].len + 1;
    if (!final_nl && n > 0)
        len--;
    if (len == 0)
        return BURROW_STR_EMPTY;
    Byte *buf = (Byte *)dc_alloc(a, (size_t)len, 1);
    Int off = 0;
    for (Int i = 0; i < n; i++) {
        if (lines[i].len > 0)
            memcpy(buf + off, lines[i].p, (size_t)lines[i].len);
        off += lines[i].len;
        if (i + 1 < n || final_nl)
            buf[off++] = '\n';
    }
    return str_from_bytes(buf, len);
}

/* A list of lines, lines[0:n]. */
typedef struct DcLines {
    Str *p;
    Int n;
} DcLines;

/* unindent removes any common space/tab prefix from each line in lines,
 * returning a copy of lines in which those prefixes have been trimmed from
 * each line. It also replaces any lines containing only spaces with blank
 * lines (empty strings). */
static DcLines dc_unindent(Alloc *a, DcLines lines) {
    /* Trim leading and trailing blank lines. */
    while (lines.n > 0 && dc_is_blank(lines.p[0])) {
        lines.p++;
        lines.n--;
    }
    while (lines.n > 0 && dc_is_blank(lines.p[lines.n - 1]))
        lines.n--;
    if (lines.n == 0)
        return (DcLines){NULL, 0};

    /* Compute and remove common indentation. */
    Str prefix = dc_leading_space(lines.p[0]);
    for (Int i = 1; i < lines.n; i++) {
        if (!dc_is_blank(lines.p[i]))
            prefix = dc_common_prefix(prefix, dc_leading_space(lines.p[i]));
    }

    DcLines out = {(Str *)dc_alloc(a, (size_t)lines.n * sizeof(Str), _Alignof(Str)),
                   lines.n};
    for (Int i = 0; i < lines.n; i++) {
        Str line = strings_trim_prefix(lines.p[i], prefix);
        if (strings_trim_space(line).len == 0)
            line = BURROW_STR_EMPTY;
        out.p[i] = line;
    }
    while (out.n > 0 && out.p[0].len == 0) {
        out.p++;
        out.n--;
    }
    while (out.n > 0 && out.p[out.n - 1].len == 0)
        out.n--;
    return out;
}

/* ident checks whether s begins with a Go identifier. If so, it returns the
 * identifier, which is a prefix of s, and true. */
static bool dc_ident(Str s, Str *id) {
    /* Scan [\pL_][\pL_0-9]* */
    Int n = 0;
    while (n < s.len) {
        Byte c = s.p[n];
        if (c < 0x80) {
            if (dc_is_ident_ascii(c) && (n > 0 || c < '0' || c > '9')) {
                n++;
                continue;
            }
            break;
        }
        Int nr = 0;
        Rune r = utf8_decode_rune_in_string(dc_sub(s, n, s.len), &nr);
        if (unicode_is_letter(r)) {
            n += nr;
            continue;
        }
        break;
    }
    *id = dc_sub(s, 0, n);
    return n > 0;
}

bool burrow__comment_ident(Str s, Str *id);
bool burrow__comment_ident(Str s, Str *id) {
    return dc_ident(s, id);
}

/* isName reports whether s is a capitalized Go identifier (like Name). */
static bool dc_is_name(Str s) {
    Str t = BURROW_STR_EMPTY;
    if (!dc_ident(s, &t) || t.len != s.len)
        return false;
    Int n = 0;
    return unicode_is_upper(utf8_decode_rune_in_string(s, &n));
}

static bool dc_valid_import_path_elem(Str elem) {
    if (elem.len == 0 || elem.p[0] == '.' || elem.p[elem.len - 1] == '.')
        return false;
    for (Int i = 0; i < elem.len; i++) {
        if (!dc_import_path_ok(elem.p[i]))
            return false;
    }
    return true;
}

/* validImportPath reports whether path is a valid import path. It is a lightly
 * edited copy of golang.org/x/mod/module.CheckImportPath. */
static bool dc_valid_import_path(Str path) {
    if (!utf8_valid_string(path))
        return false;
    if (path.len == 0)
        return false;
    if (path.p[0] == '-')
        return false;
    if (strings_contains(path, S("//")))
        return false;
    if (path.p[path.len - 1] == '/')
        return false;
    Int elem_start = 0;
    for (Int i = 0; i < path.len; i++) {
        if (path.p[i] == '/') {
            if (!dc_valid_import_path_elem(dc_sub(path, elem_start, i)))
                return false;
            elem_start = i + 1;
        }
    }
    return dc_valid_import_path_elem(dc_sub(path, elem_start, path.len));
}

/* isScheme reports whether s is a recognized URL scheme. Note that if strings
 * of new length (beyond 3-7) are added here, the fast path at the top of
 * autoURL will need updating. */
static bool dc_is_scheme(Str s) {
    static const Str schemes[] = {
        BURROW_S_INIT("file"), BURROW_S_INIT("ftp"),   BURROW_S_INIT("gopher"),
        BURROW_S_INIT("http"), BURROW_S_INIT("https"), BURROW_S_INIT("mailto"),
        BURROW_S_INIT("nntp"),
    };
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        if (str_eq(s, schemes[i]))
            return true;
    }
    return false;
}

/* autoURL checks whether s begins with a URL that should be hyperlinked. If
 * so, it returns the URL, which is a prefix of s, and true. The caller should
 * skip over the first len(url) bytes of s before further processing. */
static bool dc_auto_url(Str s, Str *url) {
    *url = BURROW_STR_EMPTY;
    /* Find the ://. Fast path to pick off non-URL, since we call this at every
     * position in the string. The shortest possible URL is ftp://x, 7 bytes. */
    Int i = 0;
    if (s.len < 7)
        return false;
    if (s.p[3] == ':')
        i = 3;
    else if (s.p[4] == ':')
        i = 4;
    else if (s.p[5] == ':')
        i = 5;
    else if (s.p[6] == ':')
        i = 6;
    else
        return false;
    if (i + 3 > s.len || memcmp(s.p + i, "://", 3) != 0)
        return false;

    /* Check valid scheme. */
    if (!dc_is_scheme(dc_sub(s, 0, i)))
        return false;

    /* Scan host part. Must have at least one byte, and must start and end in
     * non-punctuation. */
    i += 3;
    if (i >= s.len || !dc_is_host(s.p[i]) || dc_is_punct(s.p[i]))
        return false;
    i++;
    Int end = i;
    while (i < s.len && dc_is_host(s.p[i])) {
        if (!dc_is_punct(s.p[i]))
            end = i + 1;
        i++;
    }
    i = end;

    /* At this point we are definitely returning a URL (scheme://host). We just
     * have to find the longest path we can add to it. Heuristics abound. We
     * allow parens, braces, and brackets, but only if they match (#5043,
     * #22285). We allow .,:;?! in the path but not at the end, to avoid
     * end-of-sentence punctuation (#18139, #16565). The stack of closers
     * lives in a buffer as long as the rest of s, which is as deep as it can
     * get. */
    Byte small[64];
    Byte *stk = small;
    Alloc *ha = NULL;
    size_t stk_cap = sizeof(small);
    if ((size_t)(s.len - i) > stk_cap) {
        ha = heap_allocator();
        stk_cap = (size_t)(s.len - i);
        stk = (Byte *)dc_alloc(ha, stk_cap, 1);
    }
    Int depth = 0;
    end = i;
    for (; i < s.len; i++) {
        Byte c = s.p[i];
        if (dc_is_punct(c))
            continue;
        if (!dc_is_path(c))
            break;
        bool stop = false;
        switch (c) {
        case '(':
            stk[depth++] = ')';
            break;
        case '{':
            stk[depth++] = '}';
            break;
        case '[':
            stk[depth++] = ']';
            break;
        case ')':
        case '}':
        case ']':
            if (depth == 0 || stk[depth - 1] != c)
                stop = true;
            else
                depth--;
            break;
        default:
            break;
        }
        if (stop)
            break;
        if (depth == 0)
            end = i + 1;
    }
    if (ha != NULL)
        mem_free(ha, stk, stk_cap, 1);

    *url = dc_sub(s, 0, end);
    return true;
}

bool burrow__comment_auto_url(Str s, Str *url);
bool burrow__comment_auto_url(Str s, Str *url) {
    return dc_auto_url(s, url);
}

/* ---------------------------------------------------------------- nodes */

static CommentBase *dc_node(Alloc *a, CommentKind kind, size_t size) {
    CommentBase *n = (CommentBase *)dc_alloc(a, size, _Alignof(max_align_t));
    n->kind = (Int)kind;
    return n;
}

static CommentText dc_plain(Alloc *a, Str text) {
    CommentPlain *p =
        (CommentPlain *)dc_node(a, COMMENT_KIND_PLAIN, sizeof(CommentPlain));
    p->text = text;
    return &p->node;
}

static CommentText dc_italic(Alloc *a, Str text) {
    CommentItalic *p =
        (CommentItalic *)dc_node(a, COMMENT_KIND_ITALIC, sizeof(CommentItalic));
    p->text = text;
    return &p->node;
}

/* A list of one text. */
static Slice dc_one_text(Alloc *a, CommentText t) {
    Slice s = slice_nil(TYPE_COMMENT_TEXT);
    dc_append(a, &s, &t);
    return s;
}

/* --------------------------------------------------------------- parser */

/* parseDoc is parsing state for a single doc comment. */
typedef struct DcParse {
    CommentParser *p;
    Alloc *a;
    CommentDoc *doc;
    Map *links; /* Str to CommentLinkDef * */
    DcLines lines;
} DcParse;

/* lookupPkg is called to look up the pkg in [pkg], [pkg.Name], and
 * [pkg.Name.Recv]. If pkg has a slash, it is assumed to be the full import path
 * and is returned with ok = true.
 *
 * Otherwise, pkg is probably a simple package name like "rand" (not
 * "crypto/rand" or "math/rand"). d.LookupPackage provides a way for the caller
 * to allow resolving such names with reference to the imports in the
 * surrounding package.
 *
 * There is one collision between these two cases: single-element standard
 * library names like "math" are full import paths but don't contain slashes.
 * We let d.LookupPackage have the first chance to resolve it, in case there's a
 * different package imported as math, and otherwise we refer to a built-in list
 * of single-element standard library package names. */
static bool dc_lookup_pkg(DcParse *d, Str pkg, Str *import_path) {
    *import_path = BURROW_STR_EMPTY;
    if (strings_contains(pkg, S("/"))) { /* assume a full import path */
        if (dc_valid_import_path(pkg)) {
            *import_path = pkg;
            return true;
        }
        return false;
    }
    if (!BURROW_FUNC_IS_NIL(d->p->lookup_package)) {
        /* Give LookupPackage a chance. */
        bool ok = false;
        Str path = BURROW_CALLF(d->p->lookup_package, pkg, &ok);
        if (ok) {
            *import_path = dc_clone(d->a, path);
            return true;
        }
    }
    bool ok = false;
    *import_path = comment_default_lookup_package(pkg, &ok);
    return ok;
}

static bool dc_lookup_sym(DcParse *d, Str recv, Str name) {
    if (BURROW_FUNC_IS_NIL(d->p->lookup_sym))
        return false;
    return BURROW_CALLF(d->p->lookup_sym, recv, name);
}

/* A span represents a single span of comment lines (lines[start:end]) of an
 * identified kind (code, heading, paragraph, and so on). */
typedef enum DcSpanKind {
    DC_SPAN_NONE = 0,
    DC_SPAN_CODE,
    DC_SPAN_HEADING,
    DC_SPAN_LIST,
    DC_SPAN_OLD_HEADING,
    DC_SPAN_PARA
} DcSpanKind;

typedef struct DcSpan {
    Int start;
    Int end;
    DcSpanKind kind;
} DcSpan;

/* listMarker parses the line as beginning with a list marker. If it can do
 * that, it returns the numeric marker ("" for a bullet list), the rest of the
 * line, and true. Otherwise, it returns false. */
static bool dc_list_marker(Str line, Str *num, Str *rest) {
    *num = BURROW_STR_EMPTY;
    *rest = BURROW_STR_EMPTY;
    line = strings_trim_space(line);
    if (line.len == 0)
        return false;

    /* Can we find a marker? */
    Int n = 0;
    Rune r = utf8_decode_rune_in_string(line, &n);
    Str nm = BURROW_STR_EMPTY, after = BURROW_STR_EMPTY;
    if (r == 0x2022 || r == '*' || r == '+' || r == '-') {
        after = dc_sub(line, n, line.len);
    } else if ('0' <= line.p[0] && line.p[0] <= '9') {
        n = 1;
        while (n < line.len && '0' <= line.p[n] && line.p[n] <= '9')
            n++;
        if (n >= line.len || (line.p[n] != '.' && line.p[n] != ')'))
            return false;
        nm = dc_sub(line, 0, n);
        after = dc_sub(line, n + 1, line.len);
    } else {
        return false;
    }

    if (!dc_indented(after) || strings_trim_space(after).len == 0)
        return false;

    *num = nm;
    *rest = after;
    return true;
}

/* isList reports whether the line is the first line of a list, meaning starts
 * with a list marker after any indentation. (The caller is responsible for
 * checking the line is indented, as appropriate.) */
static bool dc_is_list(Str line) {
    Str num = BURROW_STR_EMPTY, rest = BURROW_STR_EMPTY;
    return dc_list_marker(line, &num, &rest);
}

/* isHeading reports whether line is a new-style section heading. */
static bool dc_is_heading(Str line) {
    return line.len >= 2 && line.p[0] == '#' &&
           (line.p[1] == ' ' || line.p[1] == '\t') &&
           !str_eq(strings_trim_space(line), S("#"));
}

/* isOldHeading reports whether line is an old-style section heading. line is
 * all[off]. */
static bool dc_is_old_heading(Str line, const Str *all, Int nall, Int off) {
    if (off <= 0 || all[off - 1].len != 0 || off + 2 >= nall || all[off + 1].len != 0 ||
        dc_leading_space(all[off + 2]).len != 0)
        return false;

    line = strings_trim_space(line);

    /* a heading must start with an uppercase letter */
    Int n = 0;
    Rune r = utf8_decode_rune_in_string(line, &n);
    if (!unicode_is_letter(r) || !unicode_is_upper(r))
        return false;

    /* it must end in a letter or digit: */
    r = utf8_decode_last_rune_in_string(line, &n);
    if (!unicode_is_letter(r) && !unicode_is_digit(r))
        return false;

    /* exclude lines with illegal characters. we allow "()," The two runes
     * after ^ are a degree sign and a section sign. */
    if (strings_contains_any(line, S(";:!?+*/=[]{}_^\302\260&\302\247~%#@<\">\\")))
        return false;

    /* allow "'" for possessive "'s" only */
    for (Str b = line;;) {
        bool ok = false;
        (void)strings_cut(b, S("'"), &b, &ok);
        if (!ok)
            break;
        if (!str_eq(b, S("s")) && !strings_has_prefix(b, S("s ")))
            return false; /* ' not followed by s and then end-of-word */
    }

    /* allow "." when followed by non-space */
    for (Str b = line;;) {
        bool ok = false;
        (void)strings_cut(b, S("."), &b, &ok);
        if (!ok)
            break;
        if (b.len == 0 || strings_has_prefix(b, S(" ")))
            return false; /* not followed by non-space */
    }

    return true;
}

bool burrow__comment_is_old_heading(Str line, const Str *all, Int nall, Int off);
bool burrow__comment_is_old_heading(Str line, const Str *all, Int nall, Int off) {
    return dc_is_old_heading(line, all, nall, off);
}

static void dc_span_append(Alloc *a, DcSpan **spans, Int *n, Int *cap, DcSpan s) {
    if (*n == *cap) {
        Int nc = *cap == 0 ? 8 : *cap * 2;
        DcSpan *p =
            (DcSpan *)mem_realloc(a, *spans, (size_t)*cap * sizeof(DcSpan),
                                  (size_t)nc * sizeof(DcSpan), _Alignof(DcSpan));
        if (p == NULL)
            dc_oom();
        *spans = p;
        *cap = nc;
    }
    (*spans)[(*n)++] = s;
}

static Int dc_parse_spans(Alloc *a, DcLines l, DcSpan **out, Int *out_cap) {
    const Str *lines = l.p;
    Int nlines = l.n;
    DcSpan *spans = NULL;
    Int n = 0, cap = 0;

    /* The loop may process a line twice: once as unindented and again forced
     * indented. So the maximum expected number of iterations is 2*len(lines).
     * The repeating logic can be subtle, though, and to protect against
     * introduction of infinite loops in future changes, we watch to see that
     * we are not looping too much. A panic is better than a quiet infinite
     * loop. */
    Int watchdog = 2 * nlines;

    Int i = 0;
    Int force_indent = 0;
    for (;;) {
        /* Skip blank lines. */
        while (i < nlines && lines[i].len == 0)
            i++;
        if (i >= nlines)
            break;
        if (--watchdog < 0)
            panic_str(S("go/doc/comment: internal error: not making progress"));

        DcSpanKind kind = DC_SPAN_NONE;
        Int start = i;
        Int end = 0;
        if (i < force_indent || dc_indented(lines[i])) {
            /* Indented (or force indented). Ends before next unindented.
             * (Blank lines are OK.) If this is an unindented list that we are
             * heuristically treating as indented, then accept unindented list
             * item lines up to the first blank lines. The heuristic is
             * disabled at blank lines to contain its effect to non-gofmt'ed
             * sections of the comment. */
            bool unindented_list_ok = dc_is_list(lines[i]) && i < force_indent;
            i++;
            while (i < nlines &&
                   (lines[i].len == 0 || i < force_indent || dc_indented(lines[i]) ||
                    (unindented_list_ok && dc_is_list(lines[i])))) {
                if (lines[i].len == 0)
                    unindented_list_ok = false;
                i++;
            }

            /* Drop trailing blank lines. */
            end = i;
            while (end > start && lines[end - 1].len == 0)
                end--;

            /* If indented lines are followed (without a blank line) by an
             * unindented line ending in a brace, take that one line too. This
             * fixes the common mistake of pasting in something like
             *
             * func main() {
             *	fmt.Println("hello, world")
             * }
             *
             * and forgetting to indent it. The heuristic will never trigger on
             * a gofmt'ed comment, because any gofmt'ed code block or list
             * would be followed by a blank line or end of comment. */
            if (end < nlines && strings_has_prefix(lines[end], S("}")))
                end++;

            if (dc_is_list(lines[start]))
                kind = DC_SPAN_LIST;
            else
                kind = DC_SPAN_CODE;
        } else {
            /* Unindented. Ends at next blank or indented line. */
            i++;
            while (i < nlines && lines[i].len != 0 && !dc_indented(lines[i]))
                i++;
            end = i;

            /* If unindented lines are followed (without a blank line) by an
             * indented line that would start a code block, check whether the
             * final unindented lines should be left for the indented section.
             * This can happen for the common mistakes of unindented code or
             * unindented lists. The heuristic will never trigger on a gofmt'ed
             * comment, because any gofmt'ed code block would have a blank line
             * preceding it after the unindented lines. */
            if (i < nlines && lines[i].len != 0 && !dc_is_list(lines[i])) {
                if (dc_is_list(lines[i - 1])) {
                    /* If the final unindented line looks like a list item,
                     * this may be the first indented line wrap of a mistakenly
                     * unindented list. Leave all the unindented list items. */
                    force_indent = end;
                    end--;
                    while (end > start && dc_is_list(lines[end - 1]))
                        end--;
                } else if (strings_has_suffix(lines[i - 1], S("{")) ||
                           strings_has_suffix(lines[i - 1], S("\\"))) {
                    /* If the final unindented line ended in { or \ it is
                     * probably the start of a misindented code block. Give the
                     * user a single line fix. Often that's enough; if not, the
                     * user can fix the others themselves. */
                    force_indent = end;
                    end--;
                }

                if (start == end && force_indent > start) {
                    i = start;
                    continue;
                }
            }

            /* Span is either paragraph or heading. */
            if (end - start == 1 && dc_is_heading(lines[start]))
                kind = DC_SPAN_HEADING;
            else if (end - start == 1 &&
                     dc_is_old_heading(lines[start], lines, nlines, start))
                kind = DC_SPAN_OLD_HEADING;
            else
                kind = DC_SPAN_PARA;
        }

        dc_span_append(a, &spans, &n, &cap, (DcSpan){start, end, kind});
        i = end;
    }

    *out = spans;
    *out_cap = cap;
    return n;
}

static CommentBlock dc_heading_of(Alloc *a, Str text) {
    CommentHeading *h =
        (CommentHeading *)dc_node(a, COMMENT_KIND_HEADING, sizeof(CommentHeading));
    h->text = dc_one_text(a, dc_plain(a, text));
    return &h->node;
}

/* code returns a code block built from the lines. */
static CommentBlock dc_code(DcParse *d, DcLines lines) {
    DcLines body = dc_unindent(d->a, lines);
    CommentCode *c =
        (CommentCode *)dc_node(d->a, COMMENT_KIND_CODE, sizeof(CommentCode));
    /* The final "" Go appends gives the final \n from the join. */
    c->text = dc_join(d->a, body.p, body.n, true);
    return &c->node;
}

/* parseLink parses a single link definition line:
 *
 *	[text]: url
 *
 * It returns the link definition and whether the line was well formed. */
static bool dc_parse_link(Str line, Str *text, Str *url) {
    if (line.len == 0 || line.p[0] != '[')
        return false;
    Int i = strings_index(line, S("]:"));
    if (i < 0 || i + 3 >= line.len || (line.p[i + 2] != ' ' && line.p[i + 2] != '\t'))
        return false;

    Str t = dc_sub(line, 1, i);
    Str u = strings_trim_space(dc_sub(line, i + 3, line.len));
    Int j = strings_index(u, S("://"));
    if (j < 0 || !dc_is_scheme(dc_sub(u, 0, j)))
        return false;

    /* Line has right form and has valid scheme://. That's good enough for us -
     * we are not as picky about the characters beyond the :// as we are when
     * extracting inline URLs from text. */
    *text = t;
    *url = u;
    return true;
}

/* paragraph returns a paragraph block built from the lines. If the lines are
 * link definitions, paragraph adds them to d and returns NULL. */
static CommentBlock dc_paragraph(DcParse *d, const Str *lines, Int n) {
    /* Is this a block of known links? Handle. */
    for (Int i = 0; i < n; i++) {
        Str text = BURROW_STR_EMPTY, url = BURROW_STR_EMPTY;
        if (!dc_parse_link(lines[i], &text, &url)) {
            CommentParagraph *p = (CommentParagraph *)dc_node(
                d->a, COMMENT_KIND_PARAGRAPH, sizeof(CommentParagraph));
            p->text = dc_one_text(d->a, dc_plain(d->a, dc_join(d->a, lines, n, false)));
            return &p->node;
        }
    }
    for (Int i = 0; i < n; i++) {
        CommentLinkDef *def = (CommentLinkDef *)dc_alloc(d->a, sizeof(CommentLinkDef),
                                                         _Alignof(CommentLinkDef));
        (void)dc_parse_link(lines[i], &def->text, &def->url);
        dc_append(d->a, &d->doc->links, &def);
        if (map_get(d->links, &def->text) == NULL) {
            if (!map_set(d->links, &def->text, &def))
                dc_oom();
        }
    }
    return NULL;
}

/* The lines of a list item, gathered before they become its paragraph. */
typedef struct DcText {
    Str *p;
    Int n;
    Int cap;
} DcText;

static void dc_flush_item(DcParse *d, CommentListItem *item, DcText *text) {
    if (item != NULL) {
        CommentBlock para = dc_paragraph(d, text->p, text->n);
        if (para != NULL)
            dc_append(d->a, &item->content, &para);
    }
    text->n = 0;
}

/* list returns a list built from the indented lines, using force_blank_before
 * as the value of the List's force_blank_before field. */
static CommentBlock dc_list(DcParse *d, DcLines lines, bool force_blank_before) {
    Str num = BURROW_STR_EMPTY, rest = BURROW_STR_EMPTY;
    (void)dc_list_marker(lines.p[0], &num, &rest);
    CommentList *list =
        (CommentList *)dc_node(d->a, COMMENT_KIND_LIST, sizeof(CommentList));
    list->items = slice_nil(TYPE_COMMENT_LIST_ITEM_PTR);
    list->force_blank_before = force_blank_before;
    CommentListItem *item = NULL;
    /* No item has more lines than the list. */
    DcText text = {(Str *)dc_alloc(d->a, (size_t)lines.n * sizeof(Str), _Alignof(Str)),
                   0, lines.n};

    for (Int k = 0; k < lines.n; k++) {
        Str line = lines.p[k];
        Str n = BURROW_STR_EMPTY, after = BURROW_STR_EMPTY;
        if (dc_list_marker(line, &n, &after) && (n.len != 0) == (num.len != 0)) {
            /* start new list item */
            dc_flush_item(d, item, &text);

            item = (CommentListItem *)dc_alloc(d->a, sizeof(CommentListItem),
                                               _Alignof(CommentListItem));
            item->number = n;
            item->content = slice_nil(TYPE_COMMENT_BLOCK);
            dc_append(d->a, &list->items, &item);
            line = after;
        }
        line = strings_trim_space(line);
        if (line.len == 0) {
            list->force_blank_between = true;
            dc_flush_item(d, item, &text);
            continue;
        }
        text.p[text.n++] = strings_trim_space(line);
    }
    dc_flush_item(d, item, &text);
    return &list->node;
}

/* ------------------------------------------------------------------ text */

/* A growing byte buffer in an allocator, Go's strings.Builder. */
typedef struct DcBuf {
    Alloc *a;
    Byte *p;
    Int len;
    Int cap;
} DcBuf;

static void dc_buf_write(DcBuf *b, const void *p, Int n) {
    if (n <= 0)
        return;
    if (b->len + n > b->cap) {
        Int nc = b->cap == 0 ? 64 : b->cap;
        while (nc < b->len + n)
            nc *= 2;
        Byte *q = (Byte *)mem_realloc(b->a, b->p, (size_t)b->cap, (size_t)nc, 1);
        if (q == NULL)
            dc_oom();
        b->p = q;
        b->cap = nc;
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static void dc_buf_str(DcBuf *b, Str s) {
    dc_buf_write(b, s.p, s.len);
}

/* parseText parses s as text and returns the result of appending those parsed
 * Text elements to out. parseText does not handle explicit links like
 * [math.Sin] or [Go home page]: those are handled by parseLinkedText. If
 * auto_link is true, then parseText recognizes URLs and words from d.Words and
 * converts those to links as appropriate. */
static void dc_parse_text(DcParse *d, Slice *out, Str s, bool auto_link, DcBuf *w) {
    Alloc *a = d->a;
    Int wrote = 0;
    w->len = 0;
#define DC_WRITE_UNTIL(i)                                                              \
    do {                                                                               \
        dc_buf_str(w, dc_sub(s, wrote, (i)));                                          \
        wrote = (i);                                                                   \
    } while (0)
#define DC_FLUSH(i)                                                                    \
    do {                                                                               \
        DC_WRITE_UNTIL(i);                                                             \
        if (w->len > 0) {                                                              \
            CommentText pt = dc_plain(a, dc_clone(a, str_from_bytes(w->p, w->len)));   \
            dc_append(a, out, &pt);                                                    \
            w->len = 0;                                                                \
        }                                                                              \
    } while (0)
    for (Int i = 0; i < s.len;) {
        Str t = dc_sub(s, i, s.len);
        if (auto_link) {
            Str url = BURROW_STR_EMPTY, id = BURROW_STR_EMPTY;
            if (dc_auto_url(t, &url)) {
                DC_FLUSH(i);
                /* Note: The old comment parser would look up the URL in words
                 * and replace the target with words[URL] if it was non-empty.
                 * That would allow creating links that display as one URL but
                 * when clicked go to a different URL. Not sure what the point
                 * of that is, so we're not doing that lookup here. */
                CommentLink *l =
                    (CommentLink *)dc_node(a, COMMENT_KIND_LINK, sizeof(CommentLink));
                l->auto_ = true;
                l->text = dc_one_text(a, dc_plain(a, url));
                l->url = url;
                CommentText lt = &l->node;
                dc_append(a, out, &lt);
                i += url.len;
                wrote = i;
                continue;
            }
            if (dc_ident(t, &id)) {
                Str *target = (Str *)map_get(d->p->words, &id);
                if (target == NULL) {
                    i += id.len;
                    continue;
                }
                DC_FLUSH(i);
                CommentText it = dc_italic(a, id);
                if (target->len == 0) {
                    dc_append(a, out, &it);
                } else {
                    CommentLink *l = (CommentLink *)dc_node(a, COMMENT_KIND_LINK,
                                                            sizeof(CommentLink));
                    l->auto_ = true;
                    l->text = dc_one_text(a, it);
                    l->url = dc_clone(a, *target);
                    CommentText lt = &l->node;
                    dc_append(a, out, &lt);
                }
                i += id.len;
                wrote = i;
                continue;
            }
        }
        if (strings_has_prefix(t, S("``"))) {
            if (t.len >= 3 && t.p[2] == '`') {
                /* Do not convert `` inside ```, in case people are mistakenly
                 * writing Markdown. Go's loop compares t[i], with i the index
                 * into s, and this does the same. */
                i += 3;
                while (i < t.len && t.p[i] == '`')
                    i++;
                continue;
            }
            DC_WRITE_UNTIL(i);
            dc_buf_str(w, S("\342\200\234")); /* left double quotation mark */
            i += 2;
            wrote = i;
        } else if (strings_has_prefix(t, S("''"))) {
            DC_WRITE_UNTIL(i);
            dc_buf_str(w, S("\342\200\235")); /* right double quotation mark */
            i += 2;
            wrote = i;
        } else {
            i++;
        }
    }
    DC_FLUSH(s.len);
#undef DC_FLUSH
#undef DC_WRITE_UNTIL
}

/* If text is of the form before.Name, where Name is a capitalized Go
 * identifier, then splitDocName returns before, name, true. Otherwise it
 * returns text, "", false. */
static bool dc_split_doc_name(Str text, Str *before, Str *name) {
    Int i = strings_last_index(text, S("."));
    Str nm = dc_sub(text, i + 1, text.len);
    if (!dc_is_name(nm)) {
        *before = text;
        *name = BURROW_STR_EMPTY;
        return false;
    }
    *before = i >= 0 ? dc_sub(text, 0, i) : BURROW_STR_EMPTY;
    *name = nm;
    return true;
}

static bool dc_link_edge(Rune r) {
    return unicode_is_punct(r) || r == ' ' || r == '\t' || r == '\n';
}

/* docLink parses text, which was found inside [ ] brackets, as a doc link if
 * possible, returning the DocLink or else NULL. The before and after strings
 * are the text before the [ and after the ] on the same line. Doc links must
 * be preceded and followed by punctuation, spaces, tabs, or the start or end
 * of a line. */
static CommentDocLink *dc_doc_link(DcParse *d, Str text, Str before, Str after) {
    Int n = 0;
    if (before.len > 0 && !dc_link_edge(utf8_decode_last_rune_in_string(before, &n)))
        return NULL;
    if (after.len > 0 && !dc_link_edge(utf8_decode_rune_in_string(after, &n)))
        return NULL;
    text = strings_trim_prefix(text, S("*"));
    Str pkg = BURROW_STR_EMPTY, name = BURROW_STR_EMPTY, recv = BURROW_STR_EMPTY;
    bool ok = dc_split_doc_name(text, &pkg, &name);
    if (ok) {
        Str p2 = BURROW_STR_EMPTY;
        (void)dc_split_doc_name(pkg, &p2, &recv);
        pkg = p2;
    }
    if (pkg.len != 0) {
        Str path = BURROW_STR_EMPTY;
        if (!dc_lookup_pkg(d, pkg, &path))
            return NULL;
        pkg = path;
    } else if (!dc_lookup_sym(d, recv, name)) {
        return NULL;
    }
    CommentDocLink *link =
        (CommentDocLink *)dc_node(d->a, COMMENT_KIND_DOC_LINK, sizeof(CommentDocLink));
    link->import_path = pkg;
    link->recv = recv;
    link->name = name;
    return link;
}

/* parseLinkedText parses text that is allowed to contain explicit links, such
 * as [math.Sin] or [Go home page], into a slice of Text items.
 *
 * A "pkg" is only assumed to be a full import path if it starts with a domain
 * name (a path element with a dot) or is one of the packages from the standard
 * library ("[os]", "[encoding/json]", and so on). To avoid problems with maps,
 * generics, and array types, doc links must be both preceded and followed by
 * punctuation, spaces, tabs, or the start or end of a line. An example problem
 * would be treating map[ast.Expr]TypeAndValue as containing a link. */
static Slice dc_parse_linked_text(DcParse *d, Str text, DcBuf *w) {
    Alloc *a = d->a;
    Slice out = slice_nil(TYPE_COMMENT_TEXT);
    Int wrote = 0;

    Int start = -1;
    /* buf never holds more than text does. */
    Byte *buf = text.len > 0 ? (Byte *)dc_alloc(a, (size_t)text.len, 1) : NULL;
    Int nbuf = 0;
    for (Int i = 0; i < text.len; i++) {
        Byte c = text.p[i];
        if (c == '\n' || c == '\t')
            c = ' ';
        if (c == '[') {
            start = i;
        } else if (c == ']') {
            if (start >= 0) {
                Str key = str_from_bytes(buf, nbuf);
                CommentLinkDef **def = (CommentLinkDef **)map_get(d->links, &key);
                CommentDocLink *dl = NULL;
                if (def == NULL)
                    dl = dc_doc_link(d, dc_sub(text, start + 1, i),
                                     dc_sub(text, 0, start),
                                     dc_sub(text, i + 1, text.len));
                if (def != NULL) {
                    (*def)->used = true;
                    if (wrote < start)
                        dc_parse_text(d, &out, dc_sub(text, wrote, start), true, w);
                    CommentLink *l = (CommentLink *)dc_node(a, COMMENT_KIND_LINK,
                                                            sizeof(CommentLink));
                    l->text = slice_nil(TYPE_COMMENT_TEXT);
                    dc_parse_text(d, &l->text, dc_sub(text, start + 1, i), false, w);
                    l->url = (*def)->url;
                    CommentText lt = &l->node;
                    dc_append(a, &out, &lt);
                    wrote = i + 1;
                } else if (dl != NULL) {
                    if (wrote < start)
                        dc_parse_text(d, &out, dc_sub(text, wrote, start), true, w);
                    dl->text = slice_nil(TYPE_COMMENT_TEXT);
                    dc_parse_text(d, &dl->text, dc_sub(text, start + 1, i), false, w);
                    CommentText lt = &dl->node;
                    dc_append(a, &out, &lt);
                    wrote = i + 1;
                }
            }
            start = -1;
            nbuf = 0;
        }
        if (start >= 0 && i != start)
            buf[nbuf++] = c;
    }

    if (wrote < text.len)
        dc_parse_text(d, &out, dc_sub(text, wrote, text.len), true, w);
    return out;
}

/* The one Plain of a paragraph the first pass made. */
static Str dc_first_plain(CommentParagraph *p) {
    return ((CommentPlain *)BURROW_AT(CommentText, p->text, 0))->text;
}

CommentDoc *comment_parser_parse(CommentParser *p, Alloc *a, Str text) {
    CommentParser zero = {0};
    if (p == NULL)
        p = &zero;
    text = dc_clone(a, text);

    Int nraw = 1;
    for (Int i = 0; i < text.len; i++) {
        if (text.p[i] == '\n')
            nraw++;
    }
    DcLines raw = {(Str *)dc_alloc(a, (size_t)nraw * sizeof(Str), _Alignof(Str)), 0};
    Int from = 0;
    for (Int i = 0; i <= text.len; i++) {
        if (i == text.len || text.p[i] == '\n') {
            raw.p[raw.n++] = dc_sub(text, from, i);
            from = i + 1;
        }
    }

    DcParse d = {p, a, NULL, NULL, dc_unindent(a, raw)};
    d.doc = (CommentDoc *)dc_alloc(a, sizeof(CommentDoc), _Alignof(CommentDoc));
    d.doc->content = slice_nil(TYPE_COMMENT_BLOCK);
    d.doc->links = slice_nil(TYPE_COMMENT_LINK_DEF_PTR);
    d.links = map_make(a, TYPE_STRING, TYPE_COMMENT_LINK_DEF_PTR, 0);
    if (d.links == NULL)
        dc_oom();
    const Str *lines = d.lines.p;

    /* First pass: break into block structure and collect known links. The
     * text is all recorded as Plain for now. */
    DcSpan *spans = NULL;
    Int spans_cap = 0;
    Int nspans = dc_parse_spans(a, d.lines, &spans, &spans_cap);
    DcSpan prev = {0, 0, DC_SPAN_NONE};
    for (Int k = 0; k < nspans; k++) {
        DcSpan s = spans[k];
        DcLines sl = {d.lines.p + s.start, s.end - s.start};
        CommentBlock b = NULL;
        switch ((int)s.kind) {
        case DC_SPAN_LIST:
            b = dc_list(&d, sl, prev.end < s.start);
            break;
        case DC_SPAN_CODE:
            b = dc_code(&d, sl);
            break;
        case DC_SPAN_OLD_HEADING:
            b = dc_heading_of(a, strings_trim_space(lines[s.start]));
            break;
        case DC_SPAN_HEADING:
            b = dc_heading_of(
                a, strings_trim_space(dc_sub(lines[s.start], 1, lines[s.start].len)));
            break;
        case DC_SPAN_PARA:
            b = dc_paragraph(&d, sl.p, sl.n);
            break;
        default:
            panic_str(S("go/doc/comment: internal error: unknown span kind"));
        }
        if (b != NULL)
            dc_append(a, &d.doc->content, &b);
        prev = s;
    }
    if (spans != NULL)
        mem_free(a, spans, (size_t)spans_cap * sizeof(DcSpan), _Alignof(DcSpan));

    /* Second pass: interpret all the Plain text now that we know the links. */
    DcBuf w = {a, NULL, 0, 0};
    for (Int k = 0; k < d.doc->content.len; k++) {
        CommentBlock b = BURROW_AT(CommentBlock, d.doc->content, k);
        if (b->kind == COMMENT_KIND_PARAGRAPH) {
            CommentParagraph *para = (CommentParagraph *)b;
            para->text = dc_parse_linked_text(&d, dc_first_plain(para), &w);
        } else if (b->kind == COMMENT_KIND_LIST) {
            CommentList *list = (CommentList *)b;
            for (Int i = 0; i < list->items.len; i++) {
                CommentListItem *item = BURROW_AT(CommentListItem *, list->items, i);
                for (Int j = 0; j < item->content.len; j++) {
                    CommentParagraph *para =
                        (CommentParagraph *)BURROW_AT(CommentBlock, item->content, j);
                    para->text = dc_parse_linked_text(&d, dc_first_plain(para), &w);
                }
            }
        }
    }
    if (w.p != NULL)
        mem_free(a, w.p, (size_t)w.cap, 1);

    return d.doc;
}

/* ---------------------------------------------------------------- lists */

bool comment_list_blank_before(CommentList *l) {
    return l->force_blank_before || comment_list_blank_between(l);
}

bool comment_list_blank_between(CommentList *l) {
    if (l->force_blank_between)
        return true;
    for (Int i = 0; i < l->items.len; i++) {
        CommentListItem *item = BURROW_AT(CommentListItem *, l->items, i);
        if (item->content.len != 1) {
            /* Unreachable for parsed comments today, since the only way to get
             * multiple item.Content is multiple paragraphs, which must have
             * been separated by a blank line. */
            return true;
        }
    }
    return false;
}
