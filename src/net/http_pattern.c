/* Derived from Go's src/net/http/pattern.go, the patterns ServeMux routes by,
 * and cleanPath from server.go.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_routing.h"

#include "http_internal.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/strings.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <string.h>

static Error pt_error(const Str *text) {
    return (Error){&burrow_sentinel_error_vt, text};
}

static const Str pt_text_empty = BURROW_S_INIT("empty pattern");
static const Str pt_text_missing_slash = BURROW_S_INIT("host/path missing /");
static const Str pt_text_host_brace =
    BURROW_S_INIT("host contains '{' (missing initial '/'?)");
static const Str pt_text_unclean =
    BURROW_S_INIT("non-CONNECT pattern with unclean path can never match");
static const Str pt_text_wild_start =
    BURROW_S_INIT("bad wildcard segment (must start with '{')");
static const Str pt_text_wild_end =
    BURROW_S_INIT("bad wildcard segment (must end with '}')");
static const Str pt_text_dollar_end = BURROW_S_INIT("{$} not at end");
static const Str pt_text_multi_end = BURROW_S_INIT("{...} wildcard not at end");
static const Str pt_text_empty_wild = BURROW_S_INIT("empty wildcard");

Str burrow__http_relationship_string(burrow__HttpRelationship r) {
    switch (r) {
    case BURROW__HTTP_EQUIVALENT:
        return BURROW_S("equivalent");
    case BURROW__HTTP_MORE_GENERAL:
        return BURROW_S("moreGeneral");
    case BURROW__HTTP_MORE_SPECIFIC:
        return BURROW_S("moreSpecific");
    case BURROW__HTTP_DISJOINT:
        return BURROW_S("disjoint");
    case BURROW__HTTP_OVERLAPS:
        return BURROW_S("overlaps");
    default:
        return BURROW_S("unknown");
    }
}

burrow__HttpSegment burrow__http_pattern_last_segment(const burrow__HttpPattern *p) {
    return p->segments[p->nsegments - 1];
}

/* isValidWildcardName. A Go identifier. */
static bool pt_valid_wildcard_name(Str s) {
    if (s.len == 0)
        return false;
    for (Int i = 0; i < s.len;) {
        Int size = 0;
        Rune c = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        if (!unicode_is_letter(c) && c != '_' && (i == 0 || !unicode_is_digit(c)))
            return false;
        i += size;
    }
    return true;
}

Str burrow__http_path_unescape(Alloc *a, Str path) {
    Error err;
    Str u = url_path_unescape(a, path, &err);
    if (BURROW_FAILED(err))
        /* Escaped wrongly, so it is taken as it is. */
        return path;
    return u;
}

/* x followed by y, or the empty string when a says no. */
static Str pt_concat(Alloc *a, Str x, Str y) {
    Int n = x.len + y.len;
    Byte *p = (Byte *)mem_alloc(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_S("");
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, n);
}

Str burrow__http_clean_path(Alloc *a, Str p) {
    if (p.len == 0)
        return BURROW_S("/");
    if (p.p[0] != '/') {
        p = pt_concat(a, BURROW_S("/"), p);
        if (p.len == 0)
            return p;
    }
    Str np = path_clean(a, p);
    if (np.len == 0)
        return np;
    /* path_clean drops a trailing slash everywhere but the root, so it goes
     * back on. */
    if (p.p[p.len - 1] == '/' && !str_eq(np, BURROW_S("/"))) {
        /* The common case, where p is already the answer. */
        if (p.len == np.len + 1 && strings_has_prefix(p, np))
            np = p;
        else
            np = pt_concat(a, np, BURROW_S("/"));
    }
    return np;
}

/* parsePattern without the offset in its errors, which is left in *off. */
static Error pt_parse(Alloc *a, Str s, burrow__HttpPattern *p, Int *off) {
    Str method = s;
    Str rest = BURROW_S("");
    bool found = false;
    Int i = strings_index_any(s, BURROW_S(" \t"));
    if (i >= 0) {
        method = str_from_bytes(s.p, i);
        rest = strings_trim_left(str_from_bytes(s.p + i + 1, s.len - i - 1),
                                 BURROW_S(" \t"));
        found = true;
    }
    if (!found) {
        rest = method;
        method = BURROW_S("");
    }
    if (method.len > 0 && !burrow__http_is_token(method))
        return fmt_errorf_v("invalid method %q", method);
    p->str = s;
    p->method = method;

    if (found)
        *off = method.len + 1;
    i = strings_index_byte(rest, '/');
    if (i < 0)
        return pt_error(&pt_text_missing_slash);
    p->host = str_from_bytes(rest.p, i);
    rest = str_from_bytes(rest.p + i, rest.len - i);
    Int j = strings_index_byte(p->host, '{');
    if (j >= 0) {
        *off += j;
        return pt_error(&pt_text_host_brace);
    }
    /* From here rest is the path. */
    *off += i;

    /* Paths are cleaned before they are matched, so an unclean one can only
     * ever match a CONNECT, which is not cleaned. */
    if (method.len > 0 && !str_eq(method, BURROW_S("CONNECT"))) {
        Str clean = burrow__http_clean_path(a, rest);
        if (clean.len == 0)
            return burrow_err_out_of_memory;
        if (!str_eq(rest, clean))
            return pt_error(&pt_text_unclean);
    }

    /* There are at most as many segments as slashes. */
    Int max = 0;
    for (Int k = 0; k < rest.len; k++)
        if (rest.p[k] == '/')
            max++;
    p->segments = (burrow__HttpSegment *)mem_alloc(a, (size_t)max * sizeof *p->segments,
                                                   _Alignof(burrow__HttpSegment));
    if (p->segments == NULL)
        return burrow_err_out_of_memory;
    p->nsegments = 0;

    while (rest.len > 0) {
        /* rest starts with '/'. */
        rest = str_from_bytes(rest.p + 1, rest.len - 1);
        *off = s.len - rest.len;
        burrow__HttpSegment *seg = &p->segments[p->nsegments];
        memset(seg, 0, sizeof *seg);
        if (rest.len == 0) {
            /* A trailing slash. */
            seg->wild = true;
            seg->multi = true;
            p->nsegments++;
            break;
        }
        i = strings_index_byte(rest, '/');
        if (i < 0)
            i = rest.len;
        Str sg = str_from_bytes(rest.p, i);
        rest = str_from_bytes(rest.p + i, rest.len - i);
        Int brace = strings_index_byte(sg, '{');
        if (brace < 0) {
            /* A literal. */
            seg->s = burrow__http_path_unescape(a, sg);
            p->nsegments++;
            continue;
        }
        /* A wildcard. */
        if (brace != 0)
            return pt_error(&pt_text_wild_start);
        if (sg.p[sg.len - 1] != '}')
            return pt_error(&pt_text_wild_end);
        Str name = str_from_bytes(sg.p + 1, sg.len - 2);
        if (str_eq(name, BURROW_S("$"))) {
            if (rest.len != 0)
                return pt_error(&pt_text_dollar_end);
            seg->s = BURROW_S("/");
            p->nsegments++;
            break;
        }
        bool multi = false;
        name = strings_cut_suffix(name, BURROW_S("..."), &multi);
        if (multi && rest.len != 0)
            return pt_error(&pt_text_multi_end);
        if (name.len == 0)
            return pt_error(&pt_text_empty_wild);
        if (!pt_valid_wildcard_name(name))
            return fmt_errorf_v("bad wildcard name %q", name);
        for (Int k = 0; k < p->nsegments; k++)
            if (p->segments[k].wild && str_eq(p->segments[k].s, name))
                return fmt_errorf_v("duplicate wildcard name %q", name);
        seg->s = name;
        seg->wild = true;
        seg->multi = multi;
        p->nsegments++;
    }
    return BURROW_NO_ERROR;
}

burrow__HttpPattern *burrow__http_parse_pattern(Alloc *a, Str s, Error *err) {
    if (s.len == 0) {
        *err = pt_error(&pt_text_empty);
        return NULL;
    }
    burrow__HttpPattern *p =
        (burrow__HttpPattern *)mem_alloc(a, sizeof *p, _Alignof(burrow__HttpPattern));
    if (p == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    memset(p, 0, sizeof *p);
    /* The pattern keeps its own copy, so that the caller's can go. */
    Str own = str_clone(a, s);
    if (own.len != s.len) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    Int off = 0;
    Error e = pt_parse(a, own, p, &off);
    if (BURROW_FAILED(e)) {
        if (e.vt == burrow_err_out_of_memory.vt &&
            e.data == burrow_err_out_of_memory.data)
            *err = e;
        else
            *err = fmt_errorf_v("at offset %d: %w", off, e);
        return NULL;
    }
    *err = BURROW_NO_ERROR;
    return p;
}

/* -------------------------------------------------------------- comparing */

burrow__HttpRelationship burrow__http_inverse_relationship(burrow__HttpRelationship r) {
    switch (r) {
    case BURROW__HTTP_MORE_SPECIFIC:
        return BURROW__HTTP_MORE_GENERAL;
    case BURROW__HTTP_MORE_GENERAL:
        return BURROW__HTTP_MORE_SPECIFIC;
    case BURROW__HTTP_EQUIVALENT:
    case BURROW__HTTP_DISJOINT:
    case BURROW__HTTP_OVERLAPS:
    default:
        return r;
    }
}

/* combineRelationships. The relationship of two patterns overall, given those
 * of the two parts they were split into. More general one way and equivalent
 * the other is more general, and more general one way and more specific the
 * other is an overlap. */
static burrow__HttpRelationship pt_combine(burrow__HttpRelationship r1,
                                           burrow__HttpRelationship r2) {
    switch (r1) {
    case BURROW__HTTP_EQUIVALENT:
        return r2;
    case BURROW__HTTP_DISJOINT:
        return BURROW__HTTP_DISJOINT;
    case BURROW__HTTP_OVERLAPS:
        if (r2 == BURROW__HTTP_DISJOINT)
            return BURROW__HTTP_DISJOINT;
        return BURROW__HTTP_OVERLAPS;
    case BURROW__HTTP_MORE_GENERAL:
    case BURROW__HTTP_MORE_SPECIFIC:
        if (r2 == BURROW__HTTP_EQUIVALENT)
            return r1;
        if (r2 == burrow__http_inverse_relationship(r1))
            return BURROW__HTTP_OVERLAPS;
        return r2;
    default:
        panic_str(BURROW_S("unknown relationship"));
    }
}

burrow__HttpRelationship
burrow__http_pattern_compare_methods(const burrow__HttpPattern *p1,
                                     const burrow__HttpPattern *p2) {
    if (str_eq(p1->method, p2->method))
        return BURROW__HTTP_EQUIVALENT;
    /* The empty method matches them all. */
    if (p1->method.len == 0)
        return BURROW__HTTP_MORE_GENERAL;
    if (p2->method.len == 0)
        return BURROW__HTTP_MORE_SPECIFIC;
    /* GET matches HEAD as well. */
    if (str_eq(p1->method, BURROW_S("GET")) && str_eq(p2->method, BURROW_S("HEAD")))
        return BURROW__HTTP_MORE_GENERAL;
    if (str_eq(p2->method, BURROW_S("GET")) && str_eq(p1->method, BURROW_S("HEAD")))
        return BURROW__HTTP_MORE_SPECIFIC;
    return BURROW__HTTP_DISJOINT;
}

/* compareSegments. */
static burrow__HttpRelationship pt_compare_segments(burrow__HttpSegment s1,
                                                    burrow__HttpSegment s2) {
    if (s1.multi && s2.multi)
        return BURROW__HTTP_EQUIVALENT;
    if (s1.multi)
        return BURROW__HTTP_MORE_GENERAL;
    if (s2.multi)
        return BURROW__HTTP_MORE_SPECIFIC;
    if (s1.wild && s2.wild)
        return BURROW__HTTP_EQUIVALENT;
    if (s1.wild) {
        /* A single wildcard does not match a trailing slash. */
        if (str_eq(s2.s, BURROW_S("/")))
            return BURROW__HTTP_DISJOINT;
        return BURROW__HTTP_MORE_GENERAL;
    }
    if (s2.wild) {
        if (str_eq(s1.s, BURROW_S("/")))
            return BURROW__HTTP_DISJOINT;
        return BURROW__HTTP_MORE_SPECIFIC;
    }
    /* Two literals. */
    if (str_eq(s1.s, s2.s))
        return BURROW__HTTP_EQUIVALENT;
    return BURROW__HTTP_DISJOINT;
}

burrow__HttpRelationship
burrow__http_pattern_compare_paths(const burrow__HttpPattern *p1,
                                   const burrow__HttpPattern *p2) {
    bool multi1 = burrow__http_pattern_last_segment(p1).multi;
    bool multi2 = burrow__http_pattern_last_segment(p2).multi;
    /* Without a multi at the end, a pattern only matches paths with as many
     * segments as it has. */
    if (p1->nsegments != p2->nsegments && !multi1 && !multi2)
        return BURROW__HTTP_DISJOINT;

    /* The segments the two have in the same places. */
    burrow__HttpRelationship rel = BURROW__HTTP_EQUIVALENT;
    Int n = p1->nsegments < p2->nsegments ? p1->nsegments : p2->nsegments;
    for (Int i = 0; i < n; i++) {
        rel = pt_combine(rel, pt_compare_segments(p1->segments[i], p2->segments[i]));
        if (rel == BURROW__HTTP_DISJOINT)
            return rel;
    }
    if (p1->nsegments == p2->nsegments)
        return rel;
    /* One is longer, and the two can only both match something if the shorter
     * ends in a multi, which is more general than whatever the longer has
     * left. */
    if (p1->nsegments < p2->nsegments && multi1)
        return pt_combine(rel, BURROW__HTTP_MORE_GENERAL);
    if (p2->nsegments < p1->nsegments && multi2)
        return pt_combine(rel, BURROW__HTTP_MORE_SPECIFIC);
    return BURROW__HTTP_DISJOINT;
}

burrow__HttpRelationship
burrow__http_pattern_compare_paths_and_methods(const burrow__HttpPattern *p1,
                                               const burrow__HttpPattern *p2) {
    burrow__HttpRelationship mrel = burrow__http_pattern_compare_methods(p1, p2);
    /* Disjoint methods make the paths not matter. */
    if (mrel == BURROW__HTTP_DISJOINT)
        return BURROW__HTTP_DISJOINT;
    return pt_combine(mrel, burrow__http_pattern_compare_paths(p1, p2));
}

bool burrow__http_pattern_conflicts_with(const burrow__HttpPattern *p1,
                                         const burrow__HttpPattern *p2) {
    /* With different hosts, either one has a host and wins, or both do and
     * they never match the same request. */
    if (!str_eq(p1->host, p2->host))
        return false;
    burrow__HttpRelationship rel =
        burrow__http_pattern_compare_paths_and_methods(p1, p2);
    return rel == BURROW__HTTP_EQUIVALENT || rel == BURROW__HTTP_OVERLAPS;
}

/* ---------------------------------------------------------- describing */

/* writeSegment. */
static void pt_write_segment(StringsBuilder *b, burrow__HttpSegment s) {
    (void)strings_builder_write_byte(b, '/');
    if (!s.multi && !str_eq(s.s, BURROW_S("/")))
        (void)strings_builder_write_string(b, s.s, NULL);
}

/* writeMatchingPath. */
static void pt_write_matching_path(StringsBuilder *b, const burrow__HttpSegment *segs,
                                   Int n) {
    for (Int i = 0; i < n; i++)
        pt_write_segment(b, segs[i]);
}

Str burrow__http_common_path(Alloc *a, const burrow__HttpPattern *p1,
                             const burrow__HttpPattern *p2) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Int n = p1->nsegments < p2->nsegments ? p1->nsegments : p2->nsegments;
    for (Int i = 0; i < n; i++) {
        if (p1->segments[i].wild)
            pt_write_segment(&b, p2->segments[i]);
        else
            pt_write_segment(&b, p1->segments[i]);
    }
    if (p1->nsegments > n)
        pt_write_matching_path(&b, p1->segments + n, p1->nsegments - n);
    else if (p2->nsegments > n)
        pt_write_matching_path(&b, p2->segments + n, p2->nsegments - n);
    return strings_builder_string(&b);
}

Str burrow__http_difference_path(Alloc *a, const burrow__HttpPattern *p1,
                                 const burrow__HttpPattern *p2) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Int n = p1->nsegments < p2->nsegments ? p1->nsegments : p2->nsegments;
    for (Int i = 0; i < n; i++) {
        burrow__HttpSegment s1 = p1->segments[i];
        burrow__HttpSegment s2 = p2->segments[i];
        if (s1.multi && s2.multi) {
            /* From here both match the same paths, so the difference was
             * earlier. */
            (void)strings_builder_write_byte(&b, '/');
            return strings_builder_string(&b);
        }
        if (s1.multi && !s2.multi) {
            /* A trailing slash tells them apart, unless s2 is "{$}", when any
             * segment does, and the wildcard's name if it has one is the
             * nicest. */
            (void)strings_builder_write_byte(&b, '/');
            if (str_eq(s2.s, BURROW_S("/"))) {
                if (s1.s.len > 0)
                    (void)strings_builder_write_string(&b, s1.s, NULL);
                else
                    (void)strings_builder_write_string(&b, BURROW_S("x"), NULL);
            }
            return strings_builder_string(&b);
        }
        if ((!s1.multi && s2.multi) || s2.wild) {
            /* p2 matches whatever goes here, so what p1 has will do. */
            pt_write_segment(&b, s1);
        } else if (s1.wild && !s2.wild) {
            /* Anything but the literal works. The wildcard's name, unless that
             * is the literal, which then gets an x on the end. */
            if (!str_eq(s1.s, s2.s)) {
                pt_write_segment(&b, s1);
            } else {
                (void)strings_builder_write_byte(&b, '/');
                (void)strings_builder_write_string(&b, s2.s, NULL);
                (void)strings_builder_write_byte(&b, 'x');
            }
        } else {
            /* Two literals, and since the patterns overlap they are the same
             * one. */
            if (!str_eq(s1.s, s2.s))
                panic_str(fmt_sprintf_v(error_allocator(), "literals differ: %q and %q",
                                        s1.s, s2.s));
            pt_write_segment(&b, s1);
        }
    }
    if (p1->nsegments > n)
        /* p1 is longer and p2 does not end in a multi, so anything matching
         * the rest of p1 will do. */
        pt_write_matching_path(&b, p1->segments + n, p1->nsegments - n);
    else if (p2->nsegments > n)
        pt_write_matching_path(&b, p2->segments + n, p2->nsegments - n);
    return strings_builder_string(&b);
}

Str burrow__http_describe_conflict(Alloc *a, const burrow__HttpPattern *p1,
                                   const burrow__HttpPattern *p2) {
    burrow__HttpRelationship mrel = burrow__http_pattern_compare_methods(p1, p2);
    burrow__HttpRelationship prel = burrow__http_pattern_compare_paths(p1, p2);
    burrow__HttpRelationship rel = pt_combine(mrel, prel);
    if (rel == BURROW__HTTP_EQUIVALENT)
        return fmt_sprintf_v(a, "%s matches the same requests as %s", p1->str, p2->str);
    if (rel != BURROW__HTTP_OVERLAPS)
        panic_str(BURROW_S("describeConflict called with non-conflicting patterns"));
    if (prel == BURROW__HTTP_OVERLAPS)
        return fmt_sprintf_v(a,
                             "%[1]s and %[2]s both match some paths, like %[3]q.\n"
                             "But neither is more specific than the other.\n"
                             "%[1]s matches %[4]q, but %[2]s doesn't.\n"
                             "%[2]s matches %[5]q, but %[1]s doesn't.",
                             p1->str, p2->str, burrow__http_common_path(a, p1, p2),
                             burrow__http_difference_path(a, p1, p2),
                             burrow__http_difference_path(a, p2, p1));
    if (mrel == BURROW__HTTP_MORE_GENERAL && prel == BURROW__HTTP_MORE_SPECIFIC)
        return fmt_sprintf_v(
            a, "%s matches more methods than %s, but has a more specific path pattern",
            p1->str, p2->str);
    if (mrel == BURROW__HTTP_MORE_SPECIFIC && prel == BURROW__HTTP_MORE_GENERAL)
        return fmt_sprintf_v(
            a, "%s matches fewer methods than %s, but has a more general path pattern",
            p1->str, p2->str);
    return fmt_sprintf_v(a,
                         "bug: unexpected way for two patterns %s and %s to conflict: "
                         "methods %s, paths %s",
                         p1->str, p2->str, burrow__http_relationship_string(mrel),
                         burrow__http_relationship_string(prel));
}
