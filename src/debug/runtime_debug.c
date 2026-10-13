/* Derived from Go's src/runtime/debug/mod.go. Go source: go1.27.1.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/runtime/debug.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEBUG_BUILD_SETTING_FIELDS(F, T)                                               \
    F(T, Str, key, Key, "")                                                            \
    F(T, Str, value, Value, "")

BURROW_STRUCT_AS_DEFINE(DebugBuildSetting, DEBUG_BUILD_SETTING_FIELDS);

DebugBuildInfo *debug_read_build_info(Alloc *a, bool *ok) {
    (void)a;
    if (ok != NULL)
        *ok = false;
    return NULL;
}

/* quoteKey and quoteValue: whether String has to quote a key or a value. */
static bool rtd_quote_key(Str key) {
    return key.len == 0 || strings_contains_any(key, BURROW_S("= \t\r\n\"`"));
}

static bool rtd_quote_value(Str value) {
    return strings_contains_any(value, BURROW_S(" \t\r\n\"`"));
}

static void rtd_put(StringsBuilder *b, Str s) {
    strings_builder_write_string(b, s, NULL);
}

static void rtd_put_byte(StringsBuilder *b, Byte c) {
    (void)strings_builder_write_byte(b, c);
}

static void rtd_format_mod(StringsBuilder *b, Str word, const DebugModule *m) {
    rtd_put(b, word);
    rtd_put_byte(b, '\t');
    rtd_put(b, m->path);
    rtd_put_byte(b, '\t');
    rtd_put(b, m->version);
    if (m->replace == NULL) {
        rtd_put_byte(b, '\t');
        rtd_put(b, m->sum);
    } else {
        rtd_put_byte(b, '\n');
        rtd_format_mod(b, BURROW_S("=>"), m->replace);
    }
    rtd_put_byte(b, '\n');
}

/* Writes s, quoted as strconv.Quote does when quote is set. */
static void rtd_put_quoted(StringsBuilder *b, Alloc *tmp, Str s, bool quote) {
    if (!quote) {
        rtd_put(b, s);
        return;
    }
    Slice q = strconv_append_quote(tmp, slice_make(tmp, TYPE_BYTE, 0, 0), s);
    rtd_put(b, (Str){(const Byte *)q.p, q.len});
}

Str debug_build_info_string(const DebugBuildInfo *bi, Alloc *a) {
    StringsBuilder b = STRINGS_BUILDER(a);
    if (bi->go_version.len > 0) {
        rtd_put(&b, BURROW_S("go\t"));
        rtd_put(&b, bi->go_version);
        rtd_put_byte(&b, '\n');
    }
    if (bi->path.len > 0) {
        rtd_put(&b, BURROW_S("path\t"));
        rtd_put(&b, bi->path);
        rtd_put_byte(&b, '\n');
    }
    const DebugModule *m = &bi->main;
    if (m->path.len > 0 || m->version.len > 0 || m->sum.len > 0 || m->replace != NULL)
        rtd_format_mod(&b, BURROW_S("mod"), m);
    for (Int i = 0; i < bi->deps.len; i++)
        rtd_format_mod(&b, BURROW_S("dep"), ((DebugModule *const *)bi->deps.p)[i]);
    Arena ar;
    arena_init(&ar, a, 0);
    Alloc *tmp = arena_allocator(&ar);
    const DebugBuildSetting *ss = (const DebugBuildSetting *)bi->settings.p;
    for (Int i = 0; i < bi->settings.len; i++) {
        rtd_put(&b, BURROW_S("build\t"));
        rtd_put_quoted(&b, tmp, ss[i].key, rtd_quote_key(ss[i].key));
        rtd_put_byte(&b, '=');
        rtd_put_quoted(&b, tmp, ss[i].value, rtd_quote_value(ss[i].value));
        rtd_put_byte(&b, '\n');
    }
    arena_free(&ar);
    return strings_builder_string(&b);
}

/* ------------------------------------------------------------- parsing */

static DebugBuildInfo *rtd_new(Alloc *a) {
    DebugBuildInfo *bi =
        (DebugBuildInfo *)mem_alloc(a, sizeof *bi, _Alignof(DebugBuildInfo));
    if (bi == NULL)
        return NULL;
    bi->parent = a;
    arena_init(&bi->arena, a, 0);
    bi->a = arena_allocator(&bi->arena);
    bi->deps = slice_make(bi->a, TYPE_UNSAFE_POINTER, 0, 0);
    bi->settings = slice_make(bi->a, TYPE_DEBUG_BUILD_SETTING, 0, 0);
    return bi;
}

void debug_build_info_free(DebugBuildInfo *bi) {
    if (bi == NULL || bi->parent == NULL)
        return;
    Alloc *a = bi->parent;
    arena_free(&bi->arena);
    mem_free(a, bi, sizeof *bi, _Alignof(DebugBuildInfo));
}

/* strings.Split(s, "\t") for the at most three columns a line can have: the
 * number of columns comes back, and the first three go in elem. */
static Int rtd_split(Str s, Str elem[3]) {
    Int n = 0;
    for (;;) {
        bool found = false;
        Str after = BURROW_STR_EMPTY;
        Str before = strings_cut(s, BURROW_S("\t"), &after, &found);
        if (n < 3)
            elem[n] = before;
        n++;
        if (!found)
            return n;
        s = after;
    }
}

/* readModuleLine. */
static Error rtd_module_line(Str line, DebugModule *m) {
    Str elem[3] = {BURROW_STR_EMPTY, BURROW_STR_EMPTY, BURROW_STR_EMPTY};
    Int n = rtd_split(line, elem);
    if (n != 2 && n != 3)
        return fmt_errorf_v("expected 2 or 3 columns; got %d", n);
    m->path = elem[0];
    m->version = elem[1];
    m->sum = n == 3 ? elem[2] : BURROW_STR_EMPTY;
    m->replace = NULL;
    return BURROW_NO_ERROR;
}

/* The key and value of a build line, kv with "build\t" taken off. */
static Error rtd_build_line(Alloc *a, Str kv, DebugBuildSetting *s) {
    if (kv.len < 1)
        return fmt_errorf_v("build line missing '='");
    Str key = BURROW_STR_EMPTY;
    Str raw_value = BURROW_STR_EMPTY;
    Error e = BURROW_NO_ERROR;
    switch (kv.p[0]) {
    case '=':
        return fmt_errorf_v("build line with missing key");
    case '`':
    case '"': {
        Str raw_key = strconv_quoted_prefix(kv, &e);
        if (BURROW_FAILED(e))
            return fmt_errorf_v("invalid quoted key in build line");
        if (kv.len == raw_key.len)
            return fmt_errorf_v("build line missing '=' after quoted key");
        uint8_t c = kv.p[raw_key.len];
        if (c != '=')
            return fmt_errorf_v("unexpected character after quoted key: %q", c);
        key = strconv_unquote(a, raw_key, NULL);
        raw_value = (Str){kv.p + raw_key.len + 1, kv.len - raw_key.len - 1};
        break;
    }
    default: {
        bool ok = false;
        key = strings_cut(kv, BURROW_S("="), &raw_value, &ok);
        if (!ok)
            return fmt_errorf_v("build line missing '=' after key");
        if (rtd_quote_key(key))
            return fmt_errorf_v("unquoted key %q must be quoted", key);
        break;
    }
    }
    Str value = BURROW_STR_EMPTY;
    if (raw_value.len > 0) {
        switch (raw_value.p[0]) {
        case '`':
        case '"':
            value = strconv_unquote(a, raw_value, &e);
            if (BURROW_FAILED(e))
                return fmt_errorf_v("invalid quoted value in build line");
            break;
        default:
            value = raw_value;
            if (rtd_quote_value(value))
                return fmt_errorf_v("unquoted value %q must be quoted", value);
            break;
        }
    }
    s->key = key;
    s->value = value;
    return BURROW_NO_ERROR;
}

/* One line of the build info, which may change bi and last. */
static Error rtd_line(DebugBuildInfo *bi, Str line, DebugModule **last) {
    bool found = false;
    Str rest = strings_cut_prefix(line, BURROW_S("path\t"), &found);
    if (found) {
        bi->path = rest;
        return BURROW_NO_ERROR;
    }
    rest = strings_cut_prefix(line, BURROW_S("mod\t"), &found);
    if (found) {
        *last = &bi->main;
        return rtd_module_line(rest, &bi->main);
    }
    rest = strings_cut_prefix(line, BURROW_S("dep\t"), &found);
    if (found) {
        DebugModule *m =
            (DebugModule *)mem_alloc(bi->a, sizeof *m, _Alignof(DebugModule));
        if (m == NULL)
            return burrow_err_out_of_memory;
        bi->deps = slice_append(bi->a, bi->deps, &m, 1);
        *last = m;
        return rtd_module_line(rest, m);
    }
    rest = strings_cut_prefix(line, BURROW_S("=>\t"), &found);
    if (found) {
        Str elem[3] = {BURROW_STR_EMPTY, BURROW_STR_EMPTY, BURROW_STR_EMPTY};
        Int n = rtd_split(rest, elem);
        if (n != 3)
            return fmt_errorf_v("expected 3 columns for replacement; got %d", n);
        if (*last == NULL)
            return fmt_errorf_v("replacement with no module on previous line");
        DebugModule *r =
            (DebugModule *)mem_alloc(bi->a, sizeof *r, _Alignof(DebugModule));
        if (r == NULL)
            return burrow_err_out_of_memory;
        r->path = elem[0];
        r->version = elem[1];
        r->sum = elem[2];
        (*last)->replace = r;
        *last = NULL;
        return BURROW_NO_ERROR;
    }
    rest = strings_cut_prefix(line, BURROW_S("build\t"), &found);
    if (found) {
        DebugBuildSetting s = {BURROW_STR_EMPTY, BURROW_STR_EMPTY};
        Error e = rtd_build_line(bi->a, rest, &s);
        if (BURROW_FAILED(e))
            return e;
        bi->settings = slice_append(bi->a, bi->settings, &s, 1);
    }
    return BURROW_NO_ERROR;
}

DebugBuildInfo *debug_parse_build_info(Alloc *a, Str data, Error *err) {
    DebugBuildInfo *bi = rtd_new(a);
    if (bi == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    /* The strings of the result point into a copy of data. */
    if (data.len > 0) {
        Byte *p = (Byte *)mem_alloc_nozero(bi->a, (size_t)data.len, 1);
        if (p == NULL) {
            debug_build_info_free(bi);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        memcpy(p, data.p, (size_t)data.len);
        data.p = p;
    }
    DebugModule *last = NULL;
    Int line_num = 1;
    while (data.len > 0) {
        bool ok = false;
        Str rest = BURROW_STR_EMPTY;
        Str line = strings_cut(data, BURROW_S("\n"), &rest, &ok);
        if (!ok)
            break;
        data = rest;
        Error e = rtd_line(bi, line, &last);
        if (BURROW_FAILED(e)) {
            debug_build_info_free(bi);
            BURROW_OUT(err, fmt_errorf_v("could not parse Go build info: line %d: %w",
                                         line_num, e));
            return NULL;
        }
        line_num++;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return bi;
}
