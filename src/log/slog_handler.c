/* log/slog: the text, JSON, default, multi and discard handlers.
 *
 * Go's commonHandler is SlogCommon here, and the text, JSON and default
 * handlers are all one of those with a kind saying which. A handler made by
 * with_attrs or with_group is one allocation from the allocator the first one
 * was made in: the struct, its group names, its preformatted attributes and
 * its key prefix, laid out one after the other. They are built in an arena
 * first and copied in once their sizes are known.
 *
 * Each record is formatted into an arena that lives for the call, which is
 * also the arena a ReplaceAttr function is handed, and written with one call
 * to the writer under a mutex that every handler made from the same first one
 * shares.
 *
 * Derived from Go's src/log/slog/handler.go, text_handler.go, json_handler.go
 * and multi_handler.go. Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/log/slog.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/encoding.h"
#include "burrow/encoding/json.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "slog_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------- common */

enum { SLOGH_TEXT, SLOGH_JSON, SLOGH_DEFAULT };

typedef struct SlogCommon {
    Alloc *a;    /* what this one was allocated from */
    size_t size; /* and how much */
    int kind;
    bool json;
    SlogHandlerOptions opts;
    Slice preformatted; /* bytes, attributes already formatted by with_attrs */
    Str group_prefix;   /* text only, the prefix for the groups already open */
    Str *groups;        /* every group from with_group */
    Int ngroups;
    Int n_open_groups; /* how many of them preformatted has opened */
    SyncMutex *mu;     /* the first handler's mu_own, over w */
    SyncMutex mu_own;
    IoWriter w;
    burrow__SlogOutputFunc output; /* the default handler's */
    void *output_ctx;
} SlogCommon;

struct SlogTextHandler {
    SlogCommon c;
};

struct SlogJSONHandler {
    SlogCommon c;
};

static const SlogHandlerVT slogh_text_vt;
static const SlogHandlerVT slogh_json_vt;
static const SlogHandlerVT slogh_default_vt;

static SlogHandler slogh_as_handler(SlogCommon *c) {
    SlogHandler h = {NULL, NULL};
    if (c == NULL)
        return h;
    switch (c->kind) {
    case SLOGH_TEXT:
        h.vt = &slogh_text_vt;
        break;
    case SLOGH_JSON:
        h.vt = &slogh_json_vt;
        break;
    default:
        h.vt = &slogh_default_vt;
        break;
    }
    h.data = c;
    return h;
}

/* A handler like t, with the attributes, prefix and groups given, plus one more
 * group when name is not NULL. Everything is copied into one allocation from
 * t->a, or from the heap for the default handler, which is static. The first
 * handler of a family has t->mu NULL and gets a mutex of its own. */
static SlogCommon *slogh_pack(const SlogCommon *t, Slice pfa, Str prefix,
                              const Str *name, Int n_open) {
    Int ngroups = t->ngroups + (name != NULL ? 1 : 0);
    size_t bytes = (size_t)pfa.len + (size_t)prefix.len;
    for (Int i = 0; i < t->ngroups; i++)
        bytes += (size_t)t->groups[i].len;
    if (name != NULL)
        bytes += (size_t)name->len;
    size_t size = sizeof(SlogCommon) + (size_t)ngroups * sizeof(Str) + bytes;

    Alloc *a = t->a != NULL ? t->a : heap_allocator();
    SlogCommon *c = mem_alloc(a, size, alignof(SlogCommon));
    if (c == NULL)
        return NULL;
    memcpy(c, t, sizeof *c);
    c->a = a;
    memset(&c->mu_own, 0, sizeof c->mu_own);
    c->size = size;
    c->mu = t->mu != NULL ? t->mu : &c->mu_own;
    c->groups = (Str *)(void *)(c + 1);
    c->ngroups = ngroups;
    c->n_open_groups = n_open;

    Byte *p = (Byte *)(void *)(c->groups + ngroups);
    for (Int i = 0; i < ngroups; i++) {
        Str g = i < t->ngroups ? t->groups[i] : *name;
        if (g.len > 0)
            memcpy(p, g.p, (size_t)g.len);
        c->groups[i] = str_from_bytes(p, g.len);
        p += g.len;
    }
    if (pfa.len > 0)
        memcpy(p, pfa.p, (size_t)pfa.len);
    c->preformatted = slice_from(p, pfa.len, pfa.len, TYPE_BYTE);
    p += pfa.len;
    if (prefix.len > 0)
        memcpy(p, prefix.p, (size_t)prefix.len);
    c->group_prefix = str_from_bytes(p, prefix.len);
    return c;
}

static SlogCommon *slogh_new(Alloc *a, int kind, IoWriter w,
                             const SlogHandlerOptions *opts) {
    SlogCommon t;
    memset(&t, 0, sizeof t);
    t.a = a;
    t.kind = kind;
    t.json = kind == SLOGH_JSON;
    if (opts != NULL)
        t.opts = *opts;
    t.preformatted = slice_nil(TYPE_BYTE);
    t.w = w;
    return slogh_pack(&t, t.preformatted, BURROW_STR_EMPTY, NULL, 0);
}

static void slogh_free(void *self) {
    SlogCommon *c = self;
    if (c != NULL)
        mem_free(c->a, c, c->size, alignof(SlogCommon));
}

static bool slogh_enabled(const SlogCommon *h, SlogLevel l) {
    SlogLevel min = SLOG_LEVEL_INFO;
    if (h->opts.level.vt != NULL)
        min = h->opts.level.vt->level(h->opts.level.data);
    return l >= min;
}

static Str slogh_attr_sep(const SlogCommon *h) {
    return h->json ? BURROW_S(",") : BURROW_S(" ");
}

static bool slogh_ends_open(const SlogCommon *h, Slice pfa) {
    return h->json && pfa.len > 0 && ((const Byte *)pfa.p)[pfa.len - 1] == '{';
}

/* ------------------------------------------------------------------ state */

/* Go's handleState. groups is the list ReplaceAttr is shown, and is only kept
 * when there is a ReplaceAttr to show it to. */
typedef struct SlogState {
    const SlogCommon *h;
    Alloc *a;
    Slice buf;
    Str sep;
    Slice prefix;
    Slice groups; /* of Str */
    bool track;
} SlogState;

static void slogh_state_init(SlogState *s, const SlogCommon *h, Alloc *a, Slice buf,
                             Str sep) {
    memset(s, 0, sizeof *s);
    s->h = h;
    s->a = a;
    s->buf = buf;
    s->sep = sep;
    s->prefix = slice_nil(TYPE_BYTE);
    s->groups = slice_nil(TYPE_STRING);
    if (h->opts.replace_attr != NULL) {
        s->track = true;
        if (h->n_open_groups > 0)
            s->groups = slice_append(a, s->groups, h->groups, h->n_open_groups);
    }
}

static void slogh_put(SlogState *s, Str x) {
    if (x.len > 0)
        s->buf = slice_append(s->a, s->buf, x.p, x.len);
}

static void slogh_putc(SlogState *s, Byte c) {
    s->buf = slice_append(s->a, s->buf, &c, 1);
}

static Str slogh_bytes_str(Slice b) {
    return str_from_bytes(b.p, b.len);
}

/* safeSet: printable ASCII apart from the quote and the backslash. */
static bool slogh_safe(Byte b) {
    return b >= 0x20 && b < 0x80 && b != '"' && b != '\\';
}

static const char slogh_hex[] = "0123456789abcdef";

/* appendEscapedJSONString. Go 1.27 builds with the JSONv2 experiment on, which
 * writes a bad byte as U+FFFD itself rather than as the escape. */
static void slogh_put_json_escaped(SlogState *s, Str x) {
    const Byte *p = x.p;
    Int start = 0;
    for (Int i = 0; i < x.len;) {
        Byte b = p[i];
        if (b < 0x80) {
            if (slogh_safe(b)) {
                i++;
                continue;
            }
            slogh_put(s, str_from_bytes(p + start, i - start));
            slogh_putc(s, '\\');
            switch (b) {
            case '\\':
            case '"':
                slogh_putc(s, b);
                break;
            case '\n':
                slogh_putc(s, 'n');
                break;
            case '\r':
                slogh_putc(s, 'r');
                break;
            case '\t':
                slogh_putc(s, 't');
                break;
            default:
                slogh_put(s, BURROW_S("u00"));
                slogh_putc(s, (Byte)slogh_hex[b >> 4]);
                slogh_putc(s, (Byte)slogh_hex[b & 0xF]);
                break;
            }
            i++;
            start = i;
            continue;
        }
        Int size = 0;
        Rune c = utf8_decode_rune_in_string(str_from_bytes(p + i, x.len - i), &size);
        if (c == UTF8_RUNE_ERROR && size == 1) {
            slogh_put(s, str_from_bytes(p + start, i - start));
            slogh_put(s, BURROW_S("\xef\xbf\xbd"));
            i += size;
            start = i;
            continue;
        }
        if (c == 0x2028 || c == 0x2029) {
            slogh_put(s, str_from_bytes(p + start, i - start));
            slogh_put(s, BURROW_S("\\u202"));
            slogh_putc(s, (Byte)slogh_hex[c & 0xF]);
            i += size;
            start = i;
            continue;
        }
        i += size;
    }
    slogh_put(s, str_from_bytes(p + start, x.len - start));
}

/* needsQuoting, for the text handler. */
static bool slogh_needs_quoting(Str x) {
    if (x.len == 0)
        return true;
    const Byte *p = x.p;
    for (Int i = 0; i < x.len;) {
        Byte b = p[i];
        if (b < 0x80) {
            if (b != '\\' && (b == ' ' || b == '=' || !slogh_safe(b)))
                return true;
            i++;
            continue;
        }
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(p + i, x.len - i), &size);
        if (r == UTF8_RUNE_ERROR || unicode_is_space(r) || !unicode_is_print(r))
            return true;
        i += size;
    }
    return false;
}

static void slogh_append_string(SlogState *s, Str x) {
    if (s->h->json) {
        slogh_putc(s, '"');
        slogh_put_json_escaped(s, x);
        slogh_putc(s, '"');
    } else if (slogh_needs_quoting(x)) {
        s->buf = strconv_append_quote(s->a, s->buf, x);
    } else {
        slogh_put(s, x);
    }
}

static void slogh_append_two_strings(SlogState *s, Str x, Str y) {
    if (s->h->json) {
        slogh_putc(s, '"');
        slogh_put_json_escaped(s, x);
        slogh_put_json_escaped(s, y);
        slogh_putc(s, '"');
    } else if (!slogh_needs_quoting(x) && !slogh_needs_quoting(y)) {
        slogh_put(s, x);
        slogh_put(s, y);
    } else {
        Slice xy = slice_nil(TYPE_BYTE);
        xy = slice_append(s->a, xy, x.p, x.len);
        xy = slice_append(s->a, xy, y.p, y.len);
        s->buf = strconv_append_quote(s->a, s->buf, slogh_bytes_str(xy));
    }
}

static void slogh_append_error(SlogState *s, Error err) {
    slogh_append_string(s, fmt_sprintf_v(s->a, "!ERROR:%v", err));
}

static void slogh_append_key(SlogState *s, Str key) {
    slogh_put(s, s->sep);
    if (s->prefix.len > 0)
        slogh_append_two_strings(s, slogh_bytes_str(s->prefix), key);
    else
        slogh_append_string(s, key);
    slogh_putc(s, s->h->json ? ':' : '=');
    s->sep = slogh_attr_sep(s->h);
}

static void slogh_open_group(SlogState *s, Str name) {
    if (s->h->json) {
        slogh_append_key(s, name);
        slogh_putc(s, '{');
        s->sep = BURROW_STR_EMPTY;
    } else {
        if (name.len > 0)
            s->prefix = slice_append(s->a, s->prefix, name.p, name.len);
        s->prefix = BURROW_APPEND(Byte, s->a, s->prefix, '.');
    }
    if (s->track)
        s->groups = BURROW_APPEND(Str, s->a, s->groups, name);
}

static void slogh_close_group(SlogState *s, Str name) {
    if (s->h->json)
        slogh_putc(s, '}');
    else
        s->prefix.len -= name.len + 1;
    s->sep = slogh_attr_sep(s->h);
    if (s->track)
        s->groups.len--;
}

static void slogh_open_groups(SlogState *s) {
    for (Int i = s->h->n_open_groups; i < s->h->ngroups; i++)
        slogh_open_group(s, s->h->groups[i]);
}

/* appendRFC3339Millis: RFC3339Nano rounded to the millisecond, with the
 * trailing zeros kept, which takes adding a tenth of a millisecond so that
 * there is a fourth digit to drop. */
static void slogh_append_rfc3339_millis(SlogState *s, Time t) {
    enum { prefix_len = sizeof "2006-01-02T15:04:05.000" - 1 };
    Int n = s->buf.len;
    t = time_add(time_truncate(t, TIME_MILLISECOND), TIME_MILLISECOND / 10);
    s->buf = time_append_format(t, s->a, s->buf, TIME_RFC3339_NANO);
    Byte *p = s->buf.p;
    Int at = n + prefix_len;
    memmove(p + at, p + at + 1, (size_t)(s->buf.len - at - 1));
    s->buf.len--;
}

static void slogh_append_json_time(SlogState *s, Time t) {
    Int y = time_year(t);
    if (y < 0 || y >= 10000) {
        slogh_append_error(
            s, errors_new(s->a, BURROW_S("time.Time year outside of range [0,9999]")));
        return;
    }
    slogh_putc(s, '"');
    s->buf = time_append_format(t, s->a, s->buf, TIME_RFC3339_NANO);
    slogh_putc(s, '"');
}

static void slogh_append_time(SlogState *s, Time t) {
    if (s->h->json)
        slogh_append_json_time(s, t);
    else
        slogh_append_rfc3339_millis(s, t);
}

/* appendJSONMarshal: v through an encoder with HTML escaping off, less the
 * newline the encoder ends with. */
static Error slogh_json_marshal(SlogState *s, Any v) {
    BytesBuffer bb = BYTES_BUFFER(s->a);
    JsonEncoder *e = json_new_encoder(s->a, bytes_buffer_as_io_writer(&bb));
    if (e == NULL)
        return errors_new(error_allocator(), BURROW_S("slog: out of memory"));
    json_encoder_set_escape_html(e, false);
    Error err = json_encoder_encode(e, v);
    if (BURROW_OK(err)) {
        Slice bs = bytes_buffer_bytes(&bb);
        if (bs.len > 0)
            s->buf = slice_append(s->a, s->buf, bs.p, bs.len - 1);
    } else {
        /* The encoder's error can point into the encoder, so it is
         * formatted before the encoder goes. */
        slogh_append_error(s, err);
    }
    json_encoder_free(e);
    bytes_buffer_free(&bb);
    return BURROW_NO_ERROR;
}

static bool slogh_has_method(const Type *t, const char *name) {
    return t != NULL && type_method_by_name(t, str_from_cstr(name)) != NULL;
}

static Error slogh_append_json_value(SlogState *s, SlogValue v) {
    switch (v.kind) {
    case SLOG_KIND_STRING:
        slogh_append_string(s, v.u.s);
        break;
    case SLOG_KIND_INT64:
        s->buf = strconv_append_int(s->a, s->buf, v.u.i, 10);
        break;
    case SLOG_KIND_UINT64:
        s->buf = strconv_append_uint(s->a, s->buf, v.u.u, 10);
        break;
    case SLOG_KIND_FLOAT64:
        /* json is particular about floats, so ask it, as Go does. */
        return slogh_json_marshal(s, (Any){TYPE_FLOAT64, &v.u.f});
    case SLOG_KIND_BOOL:
        s->buf = strconv_append_bool(s->a, s->buf, v.u.b);
        break;
    case SLOG_KIND_DURATION:
        s->buf = strconv_append_int(s->a, s->buf, v.u.d, 10);
        break;
    case SLOG_KIND_TIME:
        slogh_append_time(s, v.u.t);
        break;
    case SLOG_KIND_ANY: {
        Any x = v.u.any;
        if (x.t == TYPE_ERROR) {
            const Error *e = x.data;
            if (e == NULL || e->vt == NULL)
                return slogh_json_marshal(s, (Any){NULL, NULL});
            const Type *self = e->vt->self_type;
            if (!slogh_has_method(self, "MarshalJSON")) {
                slogh_append_string(s, error_text(*e));
                break;
            }
            /* An Any only ever reads through its pointer. */
            return slogh_json_marshal(s, (Any){self, (void *)(uintptr_t)e->data});
        }
        return slogh_json_marshal(s, x);
    }
    case SLOG_KIND_GROUP:
    case SLOG_KIND_LOG_VALUER:
    default:
        panic_str(
            fmt_sprintf_v(error_allocator(), "bad kind: %s", slog_kind_string(v.kind)));
    }
    return BURROW_NO_ERROR;
}

static bool slogh_byte_slice(Any x, Str *out) {
    const Type *t = x.t;
    if (t == NULL || t->kind != KIND_SLICE || t->elem == NULL ||
        t->elem->kind != KIND_UINT8 || x.data == NULL)
        return false;
    const Slice *bs = x.data;
    *out = str_from_bytes(bs->p, bs->len);
    return true;
}

static Error slogh_append_text_value(SlogState *s, SlogValue v) {
    switch (v.kind) {
    case SLOG_KIND_STRING:
        slogh_append_string(s, v.u.s);
        break;
    case SLOG_KIND_TIME:
        slogh_append_time(s, v.u.t);
        break;
    case SLOG_KIND_ANY: {
        Any x = v.u.any;
        Error err = BURROW_NO_ERROR;
        Str bs = BURROW_STR_EMPTY;
        if (encoding_is_text_appender(x)) {
            Slice b = encoding_append_text(s->a, x, slice_nil(TYPE_BYTE), &err);
            if (BURROW_FAILED(err))
                return err;
            slogh_append_string(s, slogh_bytes_str(b));
        } else if (encoding_is_text_marshaler(x)) {
            Slice b = encoding_marshal_text(s->a, x, &err);
            if (BURROW_FAILED(err))
                return err;
            slogh_append_string(s, slogh_bytes_str(b));
        } else if (slogh_byte_slice(x, &bs)) {
            s->buf = strconv_append_quote(s->a, s->buf, bs);
        } else {
            slogh_append_string(s, fmt_sprintf_v(s->a, "%+v", x));
        }
        break;
    }
    case SLOG_KIND_BOOL:
    case SLOG_KIND_DURATION:
    case SLOG_KIND_FLOAT64:
    case SLOG_KIND_INT64:
    case SLOG_KIND_UINT64:
    case SLOG_KIND_GROUP:
    case SLOG_KIND_LOG_VALUER:
    default:
        s->buf = burrow__slog_value_append(v, s->a, s->buf);
        break;
    }
    return BURROW_NO_ERROR;
}

/* Whether v holds a nil pointer, which is what Go's recover in appendValue
 * checks for before it calls the panic a panic. */
static bool slogh_nil_pointer(const SlogValue *v) {
    if (v->kind != SLOG_KIND_ANY)
        return false;
    Any x = v->u.any;
    return x.t != NULL && x.t->kind == KIND_POINTER &&
           (x.data == NULL || *(void *const *)x.data == NULL);
}

/* appendValue, with its recover. The work goes through s, which lives in the
 * caller's frame, so nothing the try block changes is a local of this one. */
BURROW_NOINLINE static void slogh_append_value(SlogState *s, const SlogValue *v) {
    BURROW_TRY {
        Error err = s->h->json ? slogh_append_json_value(s, *v)
                               : slogh_append_text_value(s, *v);
        if (BURROW_FAILED(err))
            slogh_append_error(s, err);
    }
    BURROW_CATCH(p) {
        if (slogh_nil_pointer(v))
            slogh_append_string(s, BURROW_S("<nil>"));
        else
            slogh_append_string(s, fmt_sprintf_v(s->a, "!PANIC: %v", p));
    }
    BURROW_TRY_END;
}

static bool slogh_append_attrs(SlogState *s, Slice as);

static bool slogh_source_empty(const SlogSource *src) {
    return src == NULL ||
           (src->function.len == 0 && src->file.len == 0 && src->line == 0);
}

static bool slogh_append_attr(SlogState *s, SlogAttr a) {
    a.value = slog_value_resolve(a.value);
    SlogReplaceAttrFunc rep = s->h->opts.replace_attr;
    if (rep != NULL && a.value.kind != SLOG_KIND_GROUP) {
        Slice gs = s->track ? s->groups : slice_nil(TYPE_STRING);
        a = rep(s->h->opts.replace_attr_ctx, s->a, gs, a);
        a.value = slog_value_resolve(a.value);
    }
    if (burrow__slog_attr_is_empty(a))
        return false;
    if (a.value.kind == SLOG_KIND_ANY && a.value.u.any.t == TYPE_SLOG_SOURCE) {
        const SlogSource *src = a.value.u.any.data;
        if (slogh_source_empty(src))
            return false;
        if (s->h->json)
            a.value = burrow__slog_source_group(s->a, src);
        else
            a.value =
                slog_string_value(fmt_sprintf_v(s->a, "%s:%d", src->file, src->line));
    }
    if (a.value.kind == SLOG_KIND_GROUP) {
        Slice attrs = a.value.u.group;
        if (attrs.len > 0) {
            Int pos = s->buf.len;
            if (a.key.len > 0)
                slogh_open_group(s, a.key);
            if (!slogh_append_attrs(s, attrs)) {
                s->buf.len = pos;
                return false;
            }
            if (a.key.len > 0)
                slogh_close_group(s, a.key);
        }
    } else {
        slogh_append_key(s, a.key);
        slogh_append_value(s, &a.value);
    }
    return true;
}

static bool slogh_append_attrs(SlogState *s, Slice as) {
    bool non_empty = false;
    const SlogAttr *p = as.p;
    for (Int i = 0; i < as.len; i++)
        if (slogh_append_attr(s, p[i]))
            non_empty = true;
    return non_empty;
}

static void slogh_append_non_builtins(SlogState *s, const SlogRecord *r) {
    const SlogCommon *h = s->h;
    Slice pfa = h->preformatted;
    if (pfa.len > 0) {
        slogh_put(s, s->sep);
        s->buf = slice_append(s->a, s->buf, pfa.p, pfa.len);
        s->sep = slogh_ends_open(h, pfa) ? BURROW_STR_EMPTY : slogh_attr_sep(h);
    }
    Int n_open = h->n_open_groups;
    if (slog_record_num_attrs(r) > 0) {
        if (h->group_prefix.len > 0)
            s->prefix =
                slice_append(s->a, s->prefix, h->group_prefix.p, h->group_prefix.len);
        Int pos = s->buf.len;
        slogh_open_groups(s);
        n_open = h->ngroups;
        bool empty = true;
        for (Int i = 0; i < r->nfront; i++)
            if (slogh_append_attr(s, r->front[i]))
                empty = false;
        const SlogAttr *back = r->back.p;
        for (Int i = 0; i < r->back.len; i++)
            if (slogh_append_attr(s, back[i]))
                empty = false;
        if (empty) {
            s->buf.len = pos;
            n_open = h->n_open_groups;
        }
    }
    if (h->json) {
        for (Int i = 0; i < n_open; i++)
            slogh_putc(s, '}');
        slogh_putc(s, '}');
    }
}

/* commonHandler.handle. */
static Error slogh_handle(const SlogCommon *h, SlogRecord r) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    SlogState s;
    slogh_state_init(&s, h, arena_allocator(&ar), slice_nil(TYPE_BYTE),
                     BURROW_STR_EMPTY);
    if (h->json)
        slogh_putc(&s, '{');

    /* The built in attributes are not in any group. */
    bool track = s.track;
    s.track = false;
    SlogReplaceAttrFunc rep = h->opts.replace_attr;
    if (!time_is_zero(r.time)) {
        Time val = time_round(r.time, 0);
        if (rep == NULL) {
            slogh_append_key(&s, SLOG_TIME_KEY);
            slogh_append_time(&s, val);
        } else {
            slogh_append_attr(&s, slog_time(SLOG_TIME_KEY, val));
        }
    }
    SlogLevel level = r.level;
    if (rep == NULL) {
        slogh_append_key(&s, SLOG_LEVEL_KEY);
        slogh_append_string(&s, slog_level_string(level, s.a));
    } else {
        slogh_append_attr(&s, slog_any(SLOG_LEVEL_KEY, (Any){TYPE_SLOG_LEVEL, &level}));
    }
    SlogSource src;
    memset(&src, 0, sizeof src);
    if (h->opts.add_source) {
        if (!slog_record_source(&r, &src))
            memset(&src, 0, sizeof src);
        slogh_append_attr(&s, slog_any(SLOG_SOURCE_KEY, (Any){TYPE_SLOG_SOURCE, &src}));
    }
    if (rep == NULL) {
        slogh_append_key(&s, SLOG_MESSAGE_KEY);
        slogh_append_string(&s, r.message);
    } else {
        slogh_append_attr(&s, slog_string(SLOG_MESSAGE_KEY, r.message));
    }
    s.track = track;
    slogh_append_non_builtins(&s, &r);
    slogh_putc(&s, '\n');

    Error err = BURROW_NO_ERROR;
    sync_mutex_lock(h->mu);
    h->w.vt->write(h->w.data, s.buf, &err);
    sync_mutex_unlock(h->mu);
    arena_free(&ar);
    return err;
}

/* commonHandler.withAttrs. Go hands back the same handler when every attribute
 * is an empty group, and this makes a copy instead, so that what with_attrs
 * returns can always be freed. */
static SlogCommon *slogh_with_attrs(const SlogCommon *h, Slice as) {
    if (burrow__slog_count_empty_groups(as) == as.len)
        return slogh_pack(h, h->preformatted, h->group_prefix, NULL, h->n_open_groups);

    Arena ar;
    arena_init(&ar, NULL, 0);
    SlogState s;
    Alloc *a = arena_allocator(&ar);
    Slice buf = slice_nil(TYPE_BYTE);
    if (h->preformatted.len > 0)
        buf = slice_append(a, buf, h->preformatted.p, h->preformatted.len);
    slogh_state_init(&s, h, a, buf, BURROW_STR_EMPTY);
    if (h->group_prefix.len > 0)
        s.prefix = slice_append(s.a, s.prefix, h->group_prefix.p, h->group_prefix.len);
    if (h->preformatted.len > 0)
        s.sep =
            slogh_ends_open(h, h->preformatted) ? BURROW_STR_EMPTY : slogh_attr_sep(h);
    Int pos = s.buf.len;
    slogh_open_groups(&s);
    SlogCommon *c;
    if (!slogh_append_attrs(&s, as)) {
        s.buf.len = pos;
        c = slogh_pack(h, s.buf, h->group_prefix, NULL, h->n_open_groups);
    } else {
        c = slogh_pack(h, s.buf, slogh_bytes_str(s.prefix), NULL, h->ngroups);
    }
    arena_free(&ar);
    return c;
}

static SlogCommon *slogh_with_group(const SlogCommon *h, Str name) {
    return slogh_pack(h, h->preformatted, h->group_prefix, &name, h->n_open_groups);
}

/* The vtable functions, shared by the text and JSON handlers. */

static bool slogh_vt_enabled(void *self, Context ctx, SlogLevel level) {
    (void)ctx;
    return slogh_enabled(self, level);
}

static Error slogh_vt_handle(void *self, Context ctx, SlogRecord r) {
    (void)ctx;
    return slogh_handle(self, r);
}

static SlogHandler slogh_vt_with_attrs(void *self, Slice attrs) {
    return slogh_as_handler(slogh_with_attrs(self, attrs));
}

static SlogHandler slogh_vt_with_group(void *self, Str name) {
    return slogh_as_handler(slogh_with_group(self, name));
}

static const SlogHandlerVT slogh_text_vt = {
    NULL,
    slogh_vt_enabled,
    slogh_vt_handle,
    slogh_vt_with_attrs,
    slogh_vt_with_group,
    slogh_free,
};

static const SlogHandlerVT slogh_json_vt = {
    NULL,
    slogh_vt_enabled,
    slogh_vt_handle,
    slogh_vt_with_attrs,
    slogh_vt_with_group,
    slogh_free,
};

void slog_handler_free(SlogHandler h) {
    if (h.vt != NULL && h.vt->free != NULL)
        h.vt->free(h.data);
}

/* ------------------------------------------------------------------- text */

SlogTextHandler *slog_new_text_handler(Alloc *a, IoWriter w,
                                       const SlogHandlerOptions *opts) {
    return (SlogTextHandler *)(void *)slogh_new(a, SLOGH_TEXT, w, opts);
}

SlogHandler slog_text_handler_as_handler(SlogTextHandler *h) {
    return slogh_as_handler(h != NULL ? &h->c : NULL);
}

bool slog_text_handler_enabled(SlogTextHandler *h, Context ctx, SlogLevel level) {
    (void)ctx;
    return slogh_enabled(&h->c, level);
}

Error slog_text_handler_handle(SlogTextHandler *h, Context ctx, SlogRecord r) {
    (void)ctx;
    return slogh_handle(&h->c, r);
}

SlogHandler slog_text_handler_with_attrs(SlogTextHandler *h, Slice attrs) {
    return slogh_as_handler(slogh_with_attrs(&h->c, attrs));
}

SlogHandler slog_text_handler_with_group(SlogTextHandler *h, Str name) {
    return slogh_as_handler(slogh_with_group(&h->c, name));
}

/* ------------------------------------------------------------------- json */

SlogJSONHandler *slog_new_json_handler(Alloc *a, IoWriter w,
                                       const SlogHandlerOptions *opts) {
    return (SlogJSONHandler *)(void *)slogh_new(a, SLOGH_JSON, w, opts);
}

SlogHandler slog_json_handler_as_handler(SlogJSONHandler *h) {
    return slogh_as_handler(h != NULL ? &h->c : NULL);
}

bool slog_json_handler_enabled(SlogJSONHandler *h, Context ctx, SlogLevel level) {
    (void)ctx;
    return slogh_enabled(&h->c, level);
}

Error slog_json_handler_handle(SlogJSONHandler *h, Context ctx, SlogRecord r) {
    (void)ctx;
    return slogh_handle(&h->c, r);
}

SlogHandler slog_json_handler_with_attrs(SlogJSONHandler *h, Slice attrs) {
    return slogh_as_handler(slogh_with_attrs(&h->c, attrs));
}

SlogHandler slog_json_handler_with_group(SlogJSONHandler *h, Str name) {
    return slogh_as_handler(slogh_with_group(&h->c, name));
}

/* ---------------------------------------------------------------- default */

/* defaultHandler: "LEVEL msg k=v", handed to output rather than written, with
 * whatever the record knows about where it came from. Enabled at and above
 * the level slog_set_log_logger_level last set. */

static bool slogh_default_enabled(void *self, Context ctx, SlogLevel level) {
    (void)self;
    (void)ctx;
    return level >= slog_level_var_level(&burrow__slog_log_logger_level);
}

static Error slogh_default_handle(void *self, Context ctx, SlogRecord r) {
    (void)ctx;
    const SlogCommon *h = self;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice buf = slice_nil(TYPE_BYTE);
    Str level = slog_level_string(r.level, a);
    buf = slice_append(a, buf, level.p, level.len);
    buf = BURROW_APPEND(Byte, a, buf, ' ');
    if (r.message.len > 0)
        buf = slice_append(a, buf, r.message.p, r.message.len);
    SlogState s;
    slogh_state_init(&s, h, a, buf, BURROW_S(" "));
    slogh_append_non_builtins(&s, &r);
    Error err =
        h->output(h->output_ctx, r.source.file, r.source.line, slogh_bytes_str(s.buf));
    arena_free(&ar);
    return err;
}

static const SlogHandlerVT slogh_default_vt = {
    NULL,
    slogh_default_enabled,
    slogh_default_handle,
    slogh_vt_with_attrs,
    slogh_vt_with_group,
    slogh_free,
};

/* The handler slog_default starts with, writing through log's standard
 * logger. Static, so its vtable has no free. */
static Error slogh_std_output(void *ctx, Str file, Int line, Str s) {
    (void)ctx;
    return burrow__log_output_site(NULL, file, line, s);
}

static SlogCommon slogh_std = {
    .kind = SLOGH_DEFAULT,
    .mu = &slogh_std.mu_own,
    .output = slogh_std_output,
};

static const SlogHandlerVT slogh_std_vt = {
    NULL,
    slogh_default_enabled,
    slogh_default_handle,
    slogh_vt_with_attrs,
    slogh_vt_with_group,
    NULL,
};

SlogHandler burrow__slog_std_handler(void) {
    SlogHandler h = {&slogh_std_vt, &slogh_std};
    return h;
}

bool burrow__slog_is_default_handler(SlogHandler h) {
    return h.vt == &slogh_std_vt || h.vt == &slogh_default_vt;
}

SlogHandler burrow__slog_new_default_handler(Alloc *a, burrow__SlogOutputFunc output,
                                             void *ctx) {
    SlogCommon *c = slogh_new(a, SLOGH_DEFAULT, (IoWriter){NULL, NULL}, NULL);
    if (c != NULL) {
        c->output = output;
        c->output_ctx = ctx;
    }
    return slogh_as_handler(c);
}

/* ------------------------------------------------------------------ multi */

struct SlogMultiHandler {
    Alloc *a;
    SlogHandler *hs; /* in a */
    Int n;
    Int room;  /* how many hs was allocated with, which n can be short of */
    bool owns; /* made by with_attrs or with_group, so the handlers are ours */
};

static SlogMultiHandler *slogm_new(Alloc *a, const SlogHandler *hs, Int n, bool owns) {
    size_t size = sizeof(SlogMultiHandler) + (size_t)n * sizeof(SlogHandler);
    SlogMultiHandler *m = mem_alloc(a, size, alignof(SlogMultiHandler));
    if (m == NULL)
        return NULL;
    m->a = a;
    m->hs = (SlogHandler *)(void *)(m + 1);
    m->n = n;
    m->room = n;
    m->owns = owns;
    if (n > 0)
        memcpy(m->hs, hs, (size_t)n * sizeof *hs);
    return m;
}

SlogMultiHandler *slog_new_multi_handler(Alloc *a, const SlogHandler *handlers, Int n) {
    return slogm_new(a, handlers, n, false);
}

bool slog_multi_handler_enabled(SlogMultiHandler *h, Context ctx, SlogLevel level) {
    for (Int i = 0; i < h->n; i++)
        if (h->hs[i].vt->enabled(h->hs[i].data, ctx, level))
            return true;
    return false;
}

Error slog_multi_handler_handle(SlogMultiHandler *h, Context ctx, SlogRecord r) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice errs = slice_nil(TYPE_ERROR);
    for (Int i = 0; i < h->n; i++) {
        SlogHandler c = h->hs[i];
        if (!c.vt->enabled(c.data, ctx, r.level))
            continue;
        Error err = c.vt->handle(c.data, ctx, slog_record_clone(r));
        if (BURROW_FAILED(err))
            errs = BURROW_APPEND(Error, a, errs, err);
    }
    Error err = errors_join(error_allocator(), errs);
    arena_free(&ar);
    return err;
}

/* WithAttrs and WithGroup: a new multi handler over what each handler makes.
 * When one of them says no, the ones already made are freed and the result is
 * a nil handler. */
static SlogHandler slogm_derive(SlogMultiHandler *h, const Slice *attrs,
                                const Str *name) {
    SlogHandler out = {NULL, NULL};
    SlogMultiHandler *m = slogm_new(h->a, h->hs, h->n, true);
    if (m == NULL)
        return out;
    for (Int i = 0; i < h->n; i++) {
        SlogHandler c = h->hs[i];
        m->hs[i] = attrs != NULL ? c.vt->with_attrs(c.data, *attrs)
                                 : c.vt->with_group(c.data, *name);
        if (m->hs[i].vt == NULL) {
            m->n = i;
            slog_handler_free(slog_multi_handler_as_handler(m));
            return out;
        }
    }
    return slog_multi_handler_as_handler(m);
}

SlogHandler slog_multi_handler_with_attrs(SlogMultiHandler *h, Slice attrs) {
    return slogm_derive(h, &attrs, NULL);
}

SlogHandler slog_multi_handler_with_group(SlogMultiHandler *h, Str name) {
    return slogm_derive(h, NULL, &name);
}

static void slogm_free(void *self) {
    SlogMultiHandler *m = self;
    if (m == NULL)
        return;
    if (m->owns)
        for (Int i = m->n - 1; i >= 0; i--)
            slog_handler_free(m->hs[i]);
    mem_free(m->a, m, sizeof *m + (size_t)m->room * sizeof(SlogHandler),
             alignof(SlogMultiHandler));
}

static bool slogm_vt_enabled(void *self, Context ctx, SlogLevel level) {
    return slog_multi_handler_enabled(self, ctx, level);
}

static Error slogm_vt_handle(void *self, Context ctx, SlogRecord r) {
    return slog_multi_handler_handle(self, ctx, r);
}

static SlogHandler slogm_vt_with_attrs(void *self, Slice attrs) {
    return slog_multi_handler_with_attrs(self, attrs);
}

static SlogHandler slogm_vt_with_group(void *self, Str name) {
    return slog_multi_handler_with_group(self, name);
}

static const SlogHandlerVT slogm_vt = {
    NULL,
    slogm_vt_enabled,
    slogm_vt_handle,
    slogm_vt_with_attrs,
    slogm_vt_with_group,
    slogm_free,
};

SlogHandler slog_multi_handler_as_handler(SlogMultiHandler *h) {
    SlogHandler out = {NULL, NULL};
    if (h != NULL) {
        out.vt = &slogm_vt;
        out.data = h;
    }
    return out;
}

/* ---------------------------------------------------------------- discard */

static bool slogd_enabled(void *self, Context ctx, SlogLevel level) {
    (void)self;
    (void)ctx;
    (void)level;
    return false;
}

static Error slogd_handle(void *self, Context ctx, SlogRecord r) {
    (void)self;
    (void)ctx;
    (void)r;
    return BURROW_NO_ERROR;
}

static SlogHandler slogd_with_attrs(void *self, Slice attrs) {
    (void)self;
    (void)attrs;
    return slog_discard_handler;
}

static SlogHandler slogd_with_group(void *self, Str name) {
    (void)self;
    (void)name;
    return slog_discard_handler;
}

static const SlogHandlerVT slogd_vt = {
    NULL, slogd_enabled, slogd_handle, slogd_with_attrs, slogd_with_group, NULL,
};

const SlogHandler slog_discard_handler = {&slogd_vt, NULL};
