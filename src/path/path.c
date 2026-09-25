/* Derived from Go's src/path/path.go and src/path/match.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/path.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdarg.h>
#include <string.h>

BURROW_SENTINEL_ERROR(path_err_bad_pattern, "syntax error in pattern");

static Str path_str(const Byte *p, Int n) {
    Str s = {p, n};
    return s;
}

static Int path_last_slash(Str s) {
    for (Int i = s.len - 1; i >= 0; i--) {
        if (s.p[i] == '/')
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ Clean */

/* Go's lazybuf: the output is read from s for as long as it is a prefix of s,
 * and only copied into a buffer from a once it differs. The output is never
 * longer than the input it came from, so the buffer can also be s itself when
 * the caller owns it, which is how Join cleans without a second allocation. */
typedef struct PathBuf {
    Alloc *a;
    Str s;
    Byte *buf;
    Int w;
    bool oom;
} PathBuf;

static Byte pathbuf_index(const PathBuf *b, Int i) {
    return b->buf != NULL ? b->buf[i] : b->s.p[i];
}

static void pathbuf_append(PathBuf *b, Byte c) {
    if (b->buf == NULL) {
        if (b->w < b->s.len && b->s.p[b->w] == c) {
            b->w++;
            return;
        }
        if (b->oom)
            return;
        b->buf = (Byte *)mem_alloc_nozero(b->a, (size_t)b->s.len, 1);
        if (b->buf == NULL) {
            b->oom = true;
            return;
        }
        memcpy(b->buf, b->s.p, (size_t)b->w);
    }
    b->buf[b->w] = c;
    b->w++;
}

static Str path_clean_into(Alloc *a, Str path, Byte *own) {
    if (path.len == 0)
        return BURROW_S(".");

    bool rooted = path.p[0] == '/';
    Int n = path.len;
    const Byte *p = path.p;

    PathBuf out = {a, path, own, 0, false};
    Int r = 0, dotdot = 0;
    if (rooted) {
        pathbuf_append(&out, '/');
        r = 1;
        dotdot = 1;
    }

    while (r < n && !out.oom) {
        if (p[r] == '/' || (p[r] == '.' && (r + 1 == n || p[r + 1] == '/'))) {
            /* An empty element or a . element. */
            r++;
        } else if (p[r] == '.' && p[r + 1] == '.' && (r + 2 == n || p[r + 2] == '/')) {
            /* A .. element: remove back to the last slash. */
            r += 2;
            if (out.w > dotdot) {
                /* Can backtrack. */
                out.w--;
                while (out.w > dotdot && pathbuf_index(&out, out.w) != '/')
                    out.w--;
            } else if (!rooted) {
                /* Cannot backtrack, but not rooted, so append a .. element. */
                if (out.w > 0)
                    pathbuf_append(&out, '/');
                pathbuf_append(&out, '.');
                pathbuf_append(&out, '.');
                dotdot = out.w;
            }
        } else {
            /* A real path element. Add a slash if needed. */
            if ((rooted && out.w != 1) || (!rooted && out.w != 0))
                pathbuf_append(&out, '/');
            for (; r < n && p[r] != '/'; r++)
                pathbuf_append(&out, p[r]);
        }
    }

    if (out.oom)
        return BURROW_STR_EMPTY;
    if (out.w == 0)
        return BURROW_S(".");
    return path_str(out.buf != NULL ? out.buf : p, out.w);
}

Str path_clean(Alloc *a, Str path) {
    return path_clean_into(a, path, NULL);
}

/* -------------------------------------------------------------- the rest */

Str path_split(Str path, Str *file) {
    Int i = path_last_slash(path);
    if (file != NULL)
        *file = path_str(path.p + i + 1, path.len - i - 1);
    return path_str(path.p, i + 1);
}

Str path_join(Alloc *a, Slice elem) {
    const Str *e = (const Str *)elem.p;
    Int size = 0;
    for (Int i = 0; i < elem.len; i++) {
        if (e[i].len > BURROW_INT_MAX - size - elem.len)
            panic_str(BURROW_S("path: Join output length overflow"));
        size += e[i].len;
    }
    if (size == 0)
        return BURROW_STR_EMPTY;

    Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)(size + elem.len - 1), 1);
    if (buf == NULL)
        return BURROW_STR_EMPTY;
    Int n = 0;
    for (Int i = 0; i < elem.len; i++) {
        if (n > 0 || e[i].len > 0) {
            if (n > 0)
                buf[n++] = '/';
            if (e[i].len > 0) {
                memcpy(buf + n, e[i].p, (size_t)e[i].len);
                n += e[i].len;
            }
        }
    }
    Str r = path_clean_into(a, path_str(buf, n), buf);
    if (r.p != buf)
        mem_free(a, buf, (size_t)(size + elem.len - 1), 1);
    return r;
}

Str path_join_v(Alloc *a, int n, ...) {
    Str stack[16];
    Str *e = stack;
    va_list ap;

    if (n <= 0)
        return BURROW_STR_EMPTY;
    if ((size_t)n > sizeof stack / sizeof stack[0]) {
        e = (Str *)mem_alloc_nozero(a, (size_t)n * sizeof(Str), _Alignof(Str));
        if (e == NULL)
            return BURROW_STR_EMPTY;
    }
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        e[i] = va_arg(ap, Str);
    va_end(ap);
    Str r = path_join(a, slice_from(e, n, n, TYPE_STRING));
    if (e != stack)
        mem_free(a, e, (size_t)n * sizeof(Str), _Alignof(Str));
    return r;
}

Str path_ext(Str path) {
    for (Int i = path.len - 1; i >= 0 && path.p[i] != '/'; i--) {
        if (path.p[i] == '.')
            return path_str(path.p + i, path.len - i);
    }
    return BURROW_STR_EMPTY;
}

Str path_base(Str path) {
    if (path.len == 0)
        return BURROW_S(".");
    /* Strip trailing slashes. */
    while (path.len > 0 && path.p[path.len - 1] == '/')
        path.len--;
    /* Find the last element. */
    Int i = path_last_slash(path);
    if (i >= 0)
        path = path_str(path.p + i + 1, path.len - i - 1);
    /* If empty now, it had only slashes. */
    if (path.len == 0)
        return BURROW_S("/");
    return path;
}

bool path_is_abs(Str path) {
    return path.len > 0 && path.p[0] == '/';
}

Str path_dir(Alloc *a, Str path) {
    return path_clean(a, path_split(path, NULL));
}

/* ------------------------------------------------------------------ Match */

static Str path_tail(Str s, Int i) {
    return path_str(s.p + i, s.len - i);
}

/* The next segment of pattern: a run of stars, then everything up to the
 * next star that is not inside a class. */
static bool path_scan_chunk(Str *pattern, Str *chunk) {
    Str p = *pattern;
    bool star = false;
    while (p.len > 0 && p.p[0] == '*') {
        p = path_tail(p, 1);
        star = true;
    }
    bool inrange = false;
    for (Int i = 0; i < p.len; i++) {
        switch (p.p[i]) {
        case '\\':
            /* The error check is done in path_match_chunk. */
            if (i + 1 < p.len)
                i++;
            break;
        case '[':
            inrange = true;
            break;
        case ']':
            inrange = false;
            break;
        case '*':
            if (!inrange) {
                *chunk = path_str(p.p, i);
                *pattern = path_tail(p, i);
                return star;
            }
            break;
        default:
            break;
        }
    }
    *chunk = p;
    *pattern = BURROW_STR_EMPTY;
    return star;
}

/* One character of a class, a backslash escape taken into account. */
static bool path_get_esc(Str *chunk, Rune *r) {
    Str c = *chunk;
    if (c.len == 0 || c.p[0] == '-' || c.p[0] == ']')
        return false;
    if (c.p[0] == '\\') {
        c = path_tail(c, 1);
        if (c.len == 0)
            return false;
    }
    Int n;
    *r = utf8_decode_rune_in_string(c, &n);
    bool ok = !(*r == UTF8_RUNE_ERROR && n == 1);
    *chunk = path_tail(c, n);
    return ok && chunk->len > 0;
}

/* Whether chunk matches the start of s, with what is left of s in *rest.
 * Returns -1 for a malformed chunk, 0 for no match and 1 for a match. The
 * whole chunk is read even after the match has failed, so that a malformed
 * one is always noticed. */
static int path_match_chunk(Str chunk, Str s, Str *rest) {
    bool failed = false;
    while (chunk.len > 0) {
        failed = failed || s.len == 0;
        switch (chunk.p[0]) {
        case '[': {
            /* A character class. */
            Rune r = 0;
            if (!failed) {
                Int n;
                r = utf8_decode_rune_in_string(s, &n);
                s = path_tail(s, n);
            }
            chunk = path_tail(chunk, 1);
            /* Possibly negated. */
            bool negated = false;
            if (chunk.len > 0 && chunk.p[0] == '^') {
                negated = true;
                chunk = path_tail(chunk, 1);
            }
            /* Parse all the ranges. */
            bool match = false;
            Int nrange = 0;
            for (;;) {
                if (chunk.len > 0 && chunk.p[0] == ']' && nrange > 0) {
                    chunk = path_tail(chunk, 1);
                    break;
                }
                Rune lo, hi;
                if (!path_get_esc(&chunk, &lo))
                    return -1;
                hi = lo;
                if (chunk.p[0] == '-') {
                    chunk = path_tail(chunk, 1);
                    if (!path_get_esc(&chunk, &hi))
                        return -1;
                }
                match = match || (lo <= r && r <= hi);
                nrange++;
            }
            failed = failed || match == negated;
            break;
        }
        case '?':
            if (!failed) {
                failed = s.p[0] == '/';
                Int n;
                utf8_decode_rune_in_string(s, &n);
                s = path_tail(s, n);
            }
            chunk = path_tail(chunk, 1);
            break;
        case '\\':
            chunk = path_tail(chunk, 1);
            if (chunk.len == 0)
                return -1;
            if (!failed) {
                failed = chunk.p[0] != s.p[0];
                s = path_tail(s, 1);
            }
            chunk = path_tail(chunk, 1);
            break;
        default:
            if (!failed) {
                failed = chunk.p[0] != s.p[0];
                s = path_tail(s, 1);
            }
            chunk = path_tail(chunk, 1);
            break;
        }
    }
    if (failed)
        return 0;
    *rest = s;
    return 1;
}

static bool path_bad_pattern(Error *err) {
    if (err != NULL)
        *err = path_err_bad_pattern;
    return false;
}

bool path_match(Str pattern, Str name, Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    while (pattern.len > 0) {
        Str chunk, t;
        bool star = path_scan_chunk(&pattern, &chunk);
        if (star && chunk.len == 0) {
            /* A trailing * matches the rest of the string unless it has a
             * slash. */
            return path_last_slash(name) < 0;
        }
        /* Look for a match at the current position. */
        int m = path_match_chunk(chunk, name, &t);
        /* If we're the last chunk, make sure we've exhausted the name,
         * otherwise we'll give a false result even if we could still match
         * using the star. */
        if (m == 1 && (t.len == 0 || pattern.len > 0)) {
            name = t;
            continue;
        }
        if (m < 0)
            return path_bad_pattern(err);
        if (star) {
            /* Look for a match skipping i+1 bytes. Cannot skip a slash. */
            bool next = false;
            for (Int i = 0; i < name.len && name.p[i] != '/'; i++) {
                m = path_match_chunk(chunk, path_tail(name, i + 1), &t);
                if (m == 1) {
                    /* If we're the last chunk, make sure we exhausted the
                     * name. */
                    if (pattern.len == 0 && t.len > 0)
                        continue;
                    name = t;
                    next = true;
                    break;
                }
                if (m < 0)
                    return path_bad_pattern(err);
            }
            if (next)
                continue;
        }
        /* Before returning false with no error, check that the rest of the
         * pattern is well formed. */
        while (pattern.len > 0) {
            path_scan_chunk(&pattern, &chunk);
            if (path_match_chunk(chunk, BURROW_STR_EMPTY, &t) < 0)
                return path_bad_pattern(err);
        }
        return false;
    }
    return name.len == 0;
}
