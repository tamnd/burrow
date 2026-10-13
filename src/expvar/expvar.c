/* expvar. See burrow/expvar.h.
 *
 * The map is Go's: a sync.Map for the entries, so that a get takes no lock,
 * and a sorted key list under an RWMutex for the walks. What Go leaves to its
 * collector, the key copies and the Ints and Floats that add makes, goes on a
 * list here and comes back in expvar_map_free.
 *
 * Derived from Go's src/expvar/expvar.go. Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/expvar.h"

#include "burrow/encoding/json.h"
#include "burrow/log.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mstats.h"
#include "burrow/os.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include "../net/http_internal.h"
#include "expvar_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- descriptors */

static const Type ev_int_desc = {
    {(const Byte *)"Int", 3},
    {(const Byte *)"expvar", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(ExpvarInt),
    (uint16_t)_Alignof(ExpvarInt),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6576696eU, /* "evin" */
    NULL,
};

static const Type ev_float_desc = {
    {(const Byte *)"Float", 5},
    {(const Byte *)"expvar", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(ExpvarFloat),
    (uint16_t)_Alignof(ExpvarFloat),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6576666cU, /* "evfl" */
    NULL,
};

static const Type ev_string_desc = {
    {(const Byte *)"String", 6},
    {(const Byte *)"expvar", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(ExpvarString),
    (uint16_t)_Alignof(ExpvarString),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x65767374U, /* "evst" */
    NULL,
};

static const Type ev_func_desc = {
    {(const Byte *)"Func", 4},
    {(const Byte *)"expvar", 6},
    KIND_FUNC,
    (uint32_t)sizeof(ExpvarFunc),
    (uint16_t)_Alignof(ExpvarFunc),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6576666eU, /* "evfn" */
    NULL,
};

static const Type ev_map_desc = {
    {(const Byte *)"Map", 3},
    {(const Byte *)"expvar", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(ExpvarMap),
    (uint16_t)_Alignof(ExpvarMap),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x65766d70U, /* "evmp" */
    NULL,
};

const Type *const TYPE_EXPVAR_INT = &ev_int_desc;
const Type *const TYPE_EXPVAR_FLOAT = &ev_float_desc;
const Type *const TYPE_EXPVAR_STRING = &ev_string_desc;
const Type *const TYPE_EXPVAR_FUNC = &ev_func_desc;
const Type *const TYPE_EXPVAR_MAP = &ev_map_desc;

/* What the sync.Map holds for each key: a Var, as two pointers. */
static const Field ev_var_fields[] = {
    {{(const Byte *)"vt", 2},
     {NULL, 0},
     TYPE_OF(UnsafePointer),
     (uint32_t)offsetof(ExpvarVar, vt)},
    {{(const Byte *)"data", 4},
     {NULL, 0},
     TYPE_OF(UnsafePointer),
     (uint32_t)offsetof(ExpvarVar, data)},
};

static const Type ev_var_desc = {
    {(const Byte *)"Var", 3},
    {(const Byte *)"expvar", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(ExpvarVar),
    (uint16_t)_Alignof(ExpvarVar),
    2,
    0,
    ev_var_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x65767672U, /* "evvr" */
    NULL,
};

/* Something a map made and gives back in expvar_map_free: a key copy, or an
 * Int or Float that add made. */
typedef struct EvOwned {
    void *p;
    size_t size;
    size_t align;
} EvOwned;

static const Type ev_owned_desc = {
    {(const Byte *)"evOwned", 7},
    {(const Byte *)"expvar", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(EvOwned),
    (uint16_t)_Alignof(EvOwned),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x65766f77U, /* "evow" */
    NULL,
};

/* os.Args as encoding/json sees it, a []string. */
BURROW_SLICE_TYPE(ExpvarStrSlice, Str);

/* --------------------------------------------------------------- JSON text */

/* appendJSONQuote, which escapes the way encoding/json does with HTML
 * escaping on. */
static Slice ev_append_json_quote(Alloc *a, Slice b, Str s) {
    static const char hex[] = "0123456789abcdef";
    Byte q = '"';
    b = slice_append(a, b, &q, 1);
    Int i = 0;
    while (i < s.len) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        i += size;
        if (r < ' ' || r == '\\' || r == '"' || r == '<' || r == '>' || r == '&' ||
            r == 0x2028 || r == 0x2029) {
            Byte e[6];
            Int n = 2;
            e[0] = '\\';
            switch (r) {
            case '\\':
            case '"':
                e[1] = (Byte)r;
                break;
            case '\n':
                e[1] = 'n';
                break;
            case '\r':
                e[1] = 'r';
                break;
            case '\t':
                e[1] = 't';
                break;
            default:
                e[1] = 'u';
                e[2] = (Byte)hex[(r >> 12) & 0xf];
                e[3] = (Byte)hex[(r >> 8) & 0xf];
                e[4] = (Byte)hex[(r >> 4) & 0xf];
                e[5] = (Byte)hex[r & 0xf];
                n = 6;
                break;
            }
            b = slice_append(a, b, e, n);
        } else if (r < UTF8_RUNE_SELF) {
            Byte c = (Byte)r;
            b = slice_append(a, b, &c, 1);
        } else {
            b = utf8_append_rune(a, b, r);
        }
    }
    return slice_append(a, b, &q, 1);
}

static Slice ev_append_str(Alloc *a, Slice b, Str s) {
    return slice_append(a, b, s.p, s.len);
}

static Str ev_bytes_str(Slice b) {
    return str_from_bytes(b.p, b.len);
}

Str expvar_var_string(ExpvarVar v, Alloc *a) {
    if (v.vt == NULL)
        return BURROW_S("null");
    return v.vt->string(v.data, a);
}

/* The switch in appendJSONMayExpand: nil, a jsonVar, or String. */
static Slice ev_append_var(Alloc *a, Slice b, ExpvarVar v) {
    if (v.vt == NULL)
        return ev_append_str(a, b, BURROW_S("null"));
    if (v.vt->append_json != NULL)
        return v.vt->append_json(v.data, a, b);
    return ev_append_str(a, b, v.vt->string(v.data, a));
}

/* --------------------------------------------------------------------- Int */

int64_t expvar_int_value(ExpvarInt *v) {
    return sync_atomic_int64_load(&v->i);
}

static Slice ev_int_append_json(void *self, Alloc *a, Slice b) {
    ExpvarInt *v = self;
    return strconv_append_int(a, b, sync_atomic_int64_load(&v->i), 10);
}

Str expvar_int_string(ExpvarInt *v, Alloc *a) {
    return ev_bytes_str(ev_int_append_json(v, a, slice_nil(TYPE_BYTE)));
}

void expvar_int_add(ExpvarInt *v, int64_t delta) {
    sync_atomic_int64_add(&v->i, delta);
}

void expvar_int_set(ExpvarInt *v, int64_t value) {
    sync_atomic_int64_store(&v->i, value);
}

static Str ev_int_string(void *self, Alloc *a) {
    return expvar_int_string(self, a);
}

static const ExpvarVarVT ev_int_vt = {&ev_int_desc, ev_int_string, ev_int_append_json};

ExpvarVar expvar_int_as_var(ExpvarInt *v) {
    return (ExpvarVar){&ev_int_vt, v};
}

/* ------------------------------------------------------------------- Float */

static double ev_from_bits(uint64_t u) {
    double f = 0;
    memcpy(&f, &u, sizeof f);
    return f;
}

static uint64_t ev_to_bits(double f) {
    uint64_t u = 0;
    memcpy(&u, &f, sizeof u);
    return u;
}

double expvar_float_value(ExpvarFloat *v) {
    return ev_from_bits(sync_atomic_uint64_load(&v->f));
}

static Slice ev_float_append_json(void *self, Alloc *a, Slice b) {
    ExpvarFloat *v = self;
    return strconv_append_float(a, b, ev_from_bits(sync_atomic_uint64_load(&v->f)), 'g',
                                -1, 64);
}

Str expvar_float_string(ExpvarFloat *v, Alloc *a) {
    return ev_bytes_str(ev_float_append_json(v, a, slice_nil(TYPE_BYTE)));
}

void expvar_float_add(ExpvarFloat *v, double delta) {
    for (;;) {
        uint64_t cur = sync_atomic_uint64_load(&v->f);
        uint64_t nxt = ev_to_bits(ev_from_bits(cur) + delta);
        if (sync_atomic_uint64_compare_and_swap(&v->f, cur, nxt))
            return;
    }
}

void expvar_float_set(ExpvarFloat *v, double value) {
    sync_atomic_uint64_store(&v->f, ev_to_bits(value));
}

static Str ev_float_string(void *self, Alloc *a) {
    return expvar_float_string(self, a);
}

static const ExpvarVarVT ev_float_vt = {&ev_float_desc, ev_float_string,
                                        ev_float_append_json};

ExpvarVar expvar_float_as_var(ExpvarFloat *v) {
    return (ExpvarVar){&ev_float_vt, v};
}

/* ------------------------------------------------------------------ String */

Str expvar_string_value(ExpvarString *v, Alloc *a) {
    sync_rw_mutex_r_lock(&v->mu);
    Str s = str_clone(a, v->s);
    sync_rw_mutex_r_unlock(&v->mu);
    return s;
}

static Slice ev_string_append_json(void *self, Alloc *a, Slice b) {
    ExpvarString *v = self;
    sync_rw_mutex_r_lock(&v->mu);
    b = ev_append_json_quote(a, b, v->s);
    sync_rw_mutex_r_unlock(&v->mu);
    return b;
}

Str expvar_string_string(ExpvarString *v, Alloc *a) {
    return ev_bytes_str(ev_string_append_json(v, a, slice_nil(TYPE_BYTE)));
}

static void ev_free_str(Alloc *a, Str s) {
    if (s.len > 0)
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

bool expvar_string_set(ExpvarString *v, Str value) {
    Str c = str_clone(heap_allocator(), value);
    if (c.len != value.len)
        return false;
    sync_rw_mutex_lock(&v->mu);
    Str old = v->s;
    v->s = c;
    sync_rw_mutex_unlock(&v->mu);
    ev_free_str(heap_allocator(), old);
    return true;
}

void expvar_string_free(ExpvarString *v) {
    ev_free_str(heap_allocator(), v->s);
    v->s = BURROW_STR_EMPTY;
}

static Str ev_string_string(void *self, Alloc *a) {
    return expvar_string_string(self, a);
}

static const ExpvarVarVT ev_string_vt = {&ev_string_desc, ev_string_string,
                                         ev_string_append_json};

ExpvarVar expvar_string_as_var(ExpvarString *v) {
    return (ExpvarVar){&ev_string_vt, v};
}

/* -------------------------------------------------------------------- Func */

Any expvar_func_value(ExpvarFunc f, Alloc *a) {
    return f.f(f.env, a);
}

Str expvar_func_string(ExpvarFunc f, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Slice b = json_marshal(a, f.f(f.env, a), &err);
    if (BURROW_FAILED(err))
        return BURROW_STR_EMPTY;
    return ev_bytes_str(b);
}

static Str ev_func_string(void *self, Alloc *a) {
    const ExpvarFunc *f = self;
    return expvar_func_string(*f, a);
}

static const ExpvarVarVT ev_func_vt = {&ev_func_desc, ev_func_string, NULL};

ExpvarVar expvar_func_as_var(ExpvarFunc *f) {
    return (ExpvarVar){&ev_func_vt, f};
}

/* --------------------------------------------------------------------- Map */

static Alloc *ev_map_alloc(const ExpvarMap *v) {
    return v->a != NULL ? v->a : heap_allocator();
}

static void ev_map_setup(void *env) {
    ExpvarMap *v = env;
    Alloc *a = ev_map_alloc(v);
    v->m = SYNC_MAP(a, TYPE_STRING, &ev_var_desc);
    v->keys = slice_nil(TYPE_STRING);
    v->owned = slice_nil(&ev_owned_desc);
}

/* The zero map is ready to use, and this is what makes it so. */
static void ev_map_need(ExpvarMap *v) {
    sync_once_do(&v->once, BURROW_FN(Func, ev_map_setup, v));
}

/* append(*s, elem), giving back the old array when it moved. Go leaves that
 * to the collector, and nothing can still be reading it since both lists are
 * only read under keys_mu. Returns false, with *s as it was, on failure. */
static bool ev_grow(Alloc *a, Slice *s, const void *elem) {
    Slice old = *s;
    Slice grown = slice_append(a, old, elem, 1);
    if (grown.len != old.len + 1)
        return false;
    if (grown.p != old.p && old.cap > 0)
        mem_free(a, old.p, (size_t)old.cap * old.elem->size, old.elem->align);
    *s = grown;
    return true;
}

/* Remembers p, a key copy or something add made, for expvar_map_free. Called
 * with keys_mu held for writing. Failure only costs the memory. */
static void ev_map_own(ExpvarMap *v, void *p, size_t size, size_t align) {
    EvOwned o = {p, size, align};
    (void)ev_grow(ev_map_alloc(v), &v->owned, &o);
}

/* slices.BinarySearch over the sorted keys. */
static Int ev_search(Slice keys, Str key, bool *found) {
    const Str *k = keys.p;
    Int lo = 0, hi = keys.len;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        if (strings_compare(k[mid], key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = lo < keys.len && str_eq(k[lo], key);
    return lo;
}

ExpvarMap *expvar_map_init(ExpvarMap *v) {
    ev_map_need(v);
    sync_rw_mutex_lock(&v->keys_mu);
    v->keys.len = 0;
    (void)sync_map_clear(&v->m);
    sync_rw_mutex_unlock(&v->keys_mu);
    return v;
}

/* addKey, the insertion into the sorted key list. *stored gets the key the
 * list holds, the map's own copy. Returns false when it could not make one. */
static bool ev_map_add_key(ExpvarMap *v, Str key, Str *stored) {
    Alloc *a = ev_map_alloc(v);
    sync_rw_mutex_lock(&v->keys_mu);
    bool found = false;
    Int i = ev_search(v->keys, key, &found);
    if (found) {
        *stored = ((const Str *)v->keys.p)[i];
        sync_rw_mutex_unlock(&v->keys_mu);
        return true;
    }
    Str c = str_clone(a, key);
    if (c.len != key.len) {
        sync_rw_mutex_unlock(&v->keys_mu);
        return false;
    }
    Int n = v->keys.len;
    if (!ev_grow(a, &v->keys, &c)) {
        ev_free_str(a, c);
        sync_rw_mutex_unlock(&v->keys_mu);
        return false;
    }
    Str *k = v->keys.p;
    memmove(k + i + 1, k + i, (size_t)(n - i) * sizeof *k);
    k[i] = c;
    if (c.len > 0)
        ev_map_own(v, (void *)(uintptr_t)c.p, (size_t)c.len, 1);
    *stored = c;
    sync_rw_mutex_unlock(&v->keys_mu);
    return true;
}

ExpvarVar expvar_map_get(ExpvarMap *v, Str key) {
    ev_map_need(v);
    ExpvarVar av = {NULL, NULL};
    (void)sync_map_load(&v->m, &key, &av);
    return av;
}

bool expvar_map_set(ExpvarMap *v, Str key, ExpvarVar av) {
    ev_map_need(v);
    /* The map's key has to be the map's copy, which ev_map_add_key makes, so
     * a new key goes into the list before the entry goes in, the other way
     * round from Go. A walk in between finds no entry and writes null for it,
     * which a reader cannot tell from a Set with a nil Var. */
    Str stored = BURROW_STR_EMPTY;
    if (!ev_map_add_key(v, key, &stored))
        return false;
    return sync_map_store(&v->m, &stored, &av);
}

/* Add and AddFloat: the Var under key, or a new zero one of vt's kind, size
 * bytes, when there is none. *ok is false when that could not be made. */
static ExpvarVar ev_map_add_var(ExpvarMap *v, Str key, const ExpvarVarVT *vt,
                                size_t size, size_t align, bool *ok) {
    ExpvarVar cur = {NULL, NULL};
    *ok = true;
    if (sync_map_load(&v->m, &key, &cur))
        return cur;
    Alloc *a = ev_map_alloc(v);
    void *p = mem_alloc(a, size, align);
    if (p == NULL) {
        *ok = false;
        return cur;
    }
    Str stored = BURROW_STR_EMPTY;
    if (!ev_map_add_key(v, key, &stored)) {
        mem_free(a, p, size, align);
        *ok = false;
        return cur;
    }
    ExpvarVar fresh = {vt, p};
    bool loaded = false;
    if (!sync_map_load_or_store(&v->m, &stored, &fresh, &cur, &loaded)) {
        mem_free(a, p, size, align);
        *ok = false;
        return cur;
    }
    if (loaded) {
        mem_free(a, p, size, align);
    } else {
        sync_rw_mutex_lock(&v->keys_mu);
        ev_map_own(v, p, size, align);
        sync_rw_mutex_unlock(&v->keys_mu);
    }
    return cur;
}

bool expvar_map_add(ExpvarMap *v, Str key, int64_t delta) {
    ev_map_need(v);
    bool ok = false;
    ExpvarVar i =
        ev_map_add_var(v, key, &ev_int_vt, sizeof(ExpvarInt), _Alignof(ExpvarInt), &ok);
    if (i.vt == &ev_int_vt)
        expvar_int_add(i.data, delta);
    return ok;
}

bool expvar_map_add_float(ExpvarMap *v, Str key, double delta) {
    ev_map_need(v);
    bool ok = false;
    ExpvarVar f = ev_map_add_var(v, key, &ev_float_vt, sizeof(ExpvarFloat),
                                 _Alignof(ExpvarFloat), &ok);
    if (f.vt == &ev_float_vt)
        expvar_float_add(f.data, delta);
    return ok;
}

void expvar_map_delete(ExpvarMap *v, Str key) {
    ev_map_need(v);
    sync_rw_mutex_lock(&v->keys_mu);
    bool found = false;
    Int i = ev_search(v->keys, key, &found);
    if (found) {
        Str *k = v->keys.p;
        memmove(k + i, k + i + 1, (size_t)(v->keys.len - i - 1) * sizeof *k);
        v->keys.len--;
        sync_map_delete(&v->m, &key);
    }
    sync_rw_mutex_unlock(&v->keys_mu);
}

void expvar_map_do(ExpvarMap *v, ExpvarDoFunc f) {
    ev_map_need(v);
    sync_rw_mutex_r_lock(&v->keys_mu);
    const Str *k = v->keys.p;
    for (Int i = 0; i < v->keys.len; i++) {
        ExpvarVar av = {NULL, NULL};
        (void)sync_map_load(&v->m, &k[i], &av);
        f.f(f.env, (ExpvarKeyValue){k[i], av});
    }
    sync_rw_mutex_r_unlock(&v->keys_mu);
}

typedef struct EvMapJSON {
    Alloc *a;
    Slice b;
    bool expand;
    bool first;
} EvMapJSON;

static void ev_map_json_entry(void *env, ExpvarKeyValue kv) {
    EvMapJSON *j = env;
    if (!j->first) {
        Byte sep[2] = {',', j->expand ? '\n' : ' '};
        j->b = slice_append(j->a, j->b, sep, 2);
    }
    j->first = false;
    j->b = ev_append_json_quote(j->a, j->b, kv.key);
    j->b = ev_append_str(j->a, j->b, BURROW_S(": "));
    j->b = ev_append_var(j->a, j->b, kv.value);
}

/* appendJSONMayExpand. expand puts every entry on its own line, which is how
 * the handler writes the published variables. */
static Slice ev_map_append_json_may_expand(ExpvarMap *v, Alloc *a, Slice b,
                                           bool expand) {
    Byte nl = '\n';
    b = ev_append_str(a, b, BURROW_S("{"));
    if (expand)
        b = slice_append(a, b, &nl, 1);
    EvMapJSON j = {a, b, expand, true};
    expvar_map_do(v, BURROW_FN(ExpvarDoFunc, ev_map_json_entry, &j));
    b = j.b;
    if (expand)
        b = slice_append(a, b, &nl, 1);
    b = ev_append_str(a, b, BURROW_S("}"));
    if (expand)
        b = slice_append(a, b, &nl, 1);
    return b;
}

static Slice ev_map_append_json(void *self, Alloc *a, Slice b) {
    return ev_map_append_json_may_expand(self, a, b, false);
}

Str expvar_map_string(ExpvarMap *v, Alloc *a) {
    return ev_bytes_str(ev_map_append_json(v, a, slice_nil(TYPE_BYTE)));
}

static Str ev_map_string(void *self, Alloc *a) {
    return expvar_map_string(self, a);
}

static const ExpvarVarVT ev_map_vt = {&ev_map_desc, ev_map_string, ev_map_append_json};

ExpvarVar expvar_map_as_var(ExpvarMap *v) {
    return (ExpvarVar){&ev_map_vt, v};
}

void expvar_map_free(ExpvarMap *v) {
    ev_map_need(v);
    Alloc *a = ev_map_alloc(v);
    sync_map_free(&v->m);
    const EvOwned *own = v->owned.p;
    for (Int i = 0; i < v->owned.len; i++)
        mem_free(a, own[i].p, own[i].size, own[i].align);
    if (v->owned.cap > 0)
        mem_free(a, v->owned.p, (size_t)v->owned.cap * sizeof(EvOwned),
                 _Alignof(EvOwned));
    if (v->keys.cap > 0)
        mem_free(a, v->keys.p, (size_t)v->keys.cap * sizeof(Str), _Alignof(Str));
    ev_map_setup(v);
}

/* --------------------------------------------------------------- published */

static ExpvarMap ev_vars;
static SyncOnce ev_vars_once;

static Any ev_cmdline(void *env, Alloc *a) {
    (void)env;
    Slice *args = BURROW_NEW(a, Slice);
    if (args == NULL)
        return (Any){NULL, NULL};
    *args = os_args();
    return (Any){TYPE_OF(ExpvarStrSlice), args};
}

static Any ev_memstats(void *env, Alloc *a) {
    (void)env;
    RuntimeMemStats *m = BURROW_NEW(a, RuntimeMemStats);
    if (m == NULL)
        return (Any){NULL, NULL};
    runtime_read_mem_stats(m);
    return (Any){TYPE_RUNTIME_MEM_STATS, m};
}

static ExpvarFunc ev_cmdline_func = {ev_cmdline, NULL};
static ExpvarFunc ev_memstats_func = {ev_memstats, NULL};

static void ev_publish(Str name, ExpvarVar v);

/* The two variables Go's init publishes. */
static void ev_vars_setup(void *env) {
    (void)env;
    ev_publish(BURROW_S("cmdline"), expvar_func_as_var(&ev_cmdline_func));
    ev_publish(BURROW_S("memstats"), expvar_func_as_var(&ev_memstats_func));
}

static ExpvarMap *ev_need_vars(void) {
    sync_once_do(&ev_vars_once, BURROW_FN(Func, ev_vars_setup, NULL));
    return &ev_vars;
}

static void ev_publish(Str name, ExpvarVar v) {
    ExpvarMap *m = &ev_vars;
    ev_map_need(m);
    Str stored = BURROW_STR_EMPTY;
    bool loaded = true;
    if (!sync_map_load(&m->m, &name, NULL)) {
        if (!ev_map_add_key(m, name, &stored))
            runtime_panic(BURROW_S("expvar: out of memory publishing a variable"));
        if (!sync_map_load_or_store(&m->m, &stored, &v, NULL, &loaded))
            runtime_panic(BURROW_S("expvar: out of memory publishing a variable"));
    }
    if (loaded)
        log_panicln_v(BURROW_S("Reuse of exported var name:"), name);
}

void expvar_publish(Str name, ExpvarVar v) {
    (void)ev_need_vars();
    ev_publish(name, v);
}

ExpvarVar expvar_get(Str name) {
    return expvar_map_get(ev_need_vars(), name);
}

/* Publishes a new variable that new_var has made, size bytes of it. The list
 * of published variables owns it from then on, which is all Go's collector
 * would do with it, since a published variable is never unpublished. */
static void *ev_new_var(Str name, size_t size, size_t align,
                        ExpvarVar (*as_var)(void *p)) {
    void *p = mem_alloc(heap_allocator(), size, align);
    if (p == NULL)
        return NULL;
    memset(p, 0, size);
    expvar_publish(name, as_var(p));
    ExpvarMap *m = &ev_vars;
    sync_rw_mutex_lock(&m->keys_mu);
    ev_map_own(m, p, size, align);
    sync_rw_mutex_unlock(&m->keys_mu);
    return p;
}

static ExpvarVar ev_int_var(void *p) {
    return expvar_int_as_var(p);
}

static ExpvarVar ev_float_var(void *p) {
    return expvar_float_as_var(p);
}

static ExpvarVar ev_map_var(void *p) {
    return expvar_map_as_var(expvar_map_init(p));
}

static ExpvarVar ev_string_var(void *p) {
    return expvar_string_as_var(p);
}

ExpvarInt *expvar_new_int(Str name) {
    return ev_new_var(name, sizeof(ExpvarInt), _Alignof(ExpvarInt), ev_int_var);
}

ExpvarFloat *expvar_new_float(Str name) {
    return ev_new_var(name, sizeof(ExpvarFloat), _Alignof(ExpvarFloat), ev_float_var);
}

ExpvarMap *expvar_new_map(Str name) {
    return ev_new_var(name, sizeof(ExpvarMap), _Alignof(ExpvarMap), ev_map_var);
}

ExpvarString *expvar_new_string(Str name) {
    return ev_new_var(name, sizeof(ExpvarString), _Alignof(ExpvarString),
                      ev_string_var);
}

void expvar_do(ExpvarDoFunc f) {
    expvar_map_do(ev_need_vars(), f);
}

/* ----------------------------------------------------------------- handler */

static void ev_serve(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    http_header_set(http_response_writer_header(w), BURROW_S("Content-Type"),
                    BURROW_S("application/json; charset=utf-8"));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice b = ev_map_append_json_may_expand(ev_need_vars(), arena_allocator(&ar),
                                            slice_nil(TYPE_BYTE), true);
    (void)http_response_writer_write(w, b, NULL);
    arena_free(&ar);
}

static HttpHandlerFunc ev_handler_func = {ev_serve, NULL};

HttpHandler expvar_handler(void) {
    return http_handler_func_as_handler(&ev_handler_func);
}

static void ev_register(void *env) {
    (void)env;
    if (burrow__http_godebug_mux121())
        http_handle_func(BURROW_S("/debug/vars"), ev_handler_func);
    else
        http_handle_func(BURROW_S("GET /debug/vars"), ev_handler_func);
}

static SyncOnce ev_register_once;

void burrow__expvar_remove_all(void) {
    ExpvarMap *m = ev_need_vars();
    sync_rw_mutex_lock(&m->keys_mu);
    const Str *k = m->keys.p;
    for (Int i = 0; i < m->keys.len; i++)
        sync_map_delete(&m->m, &k[i]);
    m->keys.len = 0;
    sync_rw_mutex_unlock(&m->keys_mu);
}

void expvar_register(void) {
    (void)ev_need_vars();
    sync_once_do(&ev_register_once, BURROW_FN(Func, ev_register, NULL));
}
