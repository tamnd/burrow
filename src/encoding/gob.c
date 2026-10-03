/* Derived from Go's src/encoding/gob/type.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/gob.h"

#include "gob_internal.h"

#include "burrow/atomic.h"
#include "burrow/fmt.h"
#include "burrow/lock.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/stack.h"
#include "burrow/strconv.h"
#include "burrow/thread.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- descriptors */

static const Field gob_common_type_fields[] = {
    {{(const Byte *)"Name", 4},
     {NULL, 0},
     &burrow_type_Str,
     (uint32_t)offsetof(GobCommonType, name)},
    {{(const Byte *)"Id", 2},
     {NULL, 0},
     &burrow_type_int32_t,
     (uint32_t)offsetof(GobCommonType, id)},
};

const Type burrow_type_GobCommonType = {
    {(const Byte *)"CommonType", 10},
    {(const Byte *)"encoding/gob", 12},
    KIND_STRUCT,
    (uint32_t)sizeof(GobCommonType),
    (uint16_t)_Alignof(GobCommonType),
    2,
    0,
    gob_common_type_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

#define GOB_IFACE_TYPE(cname, gonm)                                                    \
    const Type burrow_type_##cname = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"encoding/gob", 12},                                            \
        KIND_INTERFACE,                                                                \
        (uint32_t)sizeof(cname),                                                       \
        (uint16_t)_Alignof(cname),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

GOB_IFACE_TYPE(GobGobEncoder, "GobEncoder");
GOB_IFACE_TYPE(GobGobDecoder, "GobDecoder");

/* The unnamed slices registerBasics registers. Any other descriptor of the
 * same slice type is the same type to gob, through burrow__gob_canon. */
#define GOB_SLICE_DESC(cname, elem)                                                    \
    static const Type cname = {                                                        \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        KIND_SLICE,                                                                    \
        (uint32_t)sizeof(Slice),                                                       \
        (uint16_t)_Alignof(Slice),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        (elem),                                                                        \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

GOB_SLICE_DESC(gob_ints, &burrow_type_Int);
GOB_SLICE_DESC(gob_int8s, &burrow_type_int8_t);
GOB_SLICE_DESC(gob_int16s, &burrow_type_int16_t);
GOB_SLICE_DESC(gob_int32s, &burrow_type_int32_t);
GOB_SLICE_DESC(gob_int64s, &burrow_type_int64_t);
GOB_SLICE_DESC(gob_uints, &burrow_type_Uint);
GOB_SLICE_DESC(gob_uint16s, &burrow_type_uint16_t);
GOB_SLICE_DESC(gob_uint32s, &burrow_type_uint32_t);
GOB_SLICE_DESC(gob_uint64s, &burrow_type_uint64_t);
GOB_SLICE_DESC(gob_float32s, &burrow_type_float);
GOB_SLICE_DESC(gob_float64s, &burrow_type_double);
GOB_SLICE_DESC(gob_complex64s, &burrow_type_Complex64);
GOB_SLICE_DESC(gob_complex128s, &burrow_type_Complex128);
GOB_SLICE_DESC(gob_uintptrs, &burrow_type_Uintptr);
GOB_SLICE_DESC(gob_bools, &burrow_type_bool);
GOB_SLICE_DESC(gob_strings, &burrow_type_Str);

/* What an error made by errors_new holds, which is Go's *errors.errorString
 * and which nobody can register. */
static const Type gob_error_string_desc = {
    {(const Byte *)"errorString", 11},
    {(const Byte *)"errors", 6},
    KIND_STRUCT,
    1,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* ------------------------------------------------------------------ state
 *
 * All of it is under gob_lock, which is Go's typeLock and its sync.Maps in
 * one. Nothing here is ever freed, as nothing in Go's is. */

static burrow__Lock gob_lock;
static bool gob_inited;
static GobType **gob_ids;
static Int gob_nids;
static Int gob_capids;
static GobType *gob_builtin[GOB_FIRST_USER_ID];
static Map *gob_types;
static Map *gob_uts;
static Map *gob_infos;
static Map *gob_name_to_type;
static Map *gob_type_to_name;
static Map *gob_canon_ptr;
static Map *gob_canon_key;
static Map *gob_ptrs;

static void *gob_pget(Map *m, const void *k) {
    uintptr_t key = (uintptr_t)k;
    uintptr_t *v = (uintptr_t *)map_get(m, &key);
    return v != NULL ? (void *)*v : NULL;
}

static bool gob_pset(Map *m, const void *k, const void *v) {
    uintptr_t key = (uintptr_t)k;
    uintptr_t val = (uintptr_t)v;
    return map_set(m, &key, &val);
}

static void gob_pdel(Map *m, const void *k) {
    uintptr_t key = (uintptr_t)k;
    map_del(m, &key);
}

static void *gob_sget(Map *m, Str k) {
    uintptr_t *v = (uintptr_t *)map_get(m, &k);
    return v != NULL ? (void *)*v : NULL;
}

/* The key is copied, since a map keeps the Str and not the bytes. */
static bool gob_sset(Map *m, Str k, const void *v) {
    Byte *p = (Byte *)mem_alloc(heap_allocator(), (size_t)(k.len > 0 ? k.len : 1), 1);
    if (p == NULL)
        return false;
    if (k.len > 0)
        memcpy(p, k.p, (size_t)k.len);
    Str key = {p, k.len};
    uintptr_t val = (uintptr_t)v;
    return map_set(m, &key, &val);
}

static Str gob_strdup(Str s) {
    if (s.len == 0)
        return (Str){NULL, 0};
    Byte *p = (Byte *)mem_alloc(heap_allocator(), (size_t)s.len, 1);
    if (p == NULL)
        return (Str){NULL, 0};
    memcpy(p, s.p, (size_t)s.len);
    return (Str){p, s.len};
}

/* ------------------------------------------------------------ type strings */

typedef struct GobBuf {
    Alloc *a;
    Byte *p;
    Int len;
    Int cap;
    bool failed;
} GobBuf;

static void gob_buf_add(GobBuf *b, const void *p, Int n) {
    if (b->failed || n <= 0)
        return;
    if (b->len + n > b->cap) {
        Int nc = b->cap == 0 ? 64 : b->cap * 2;
        while (nc < b->len + n)
            nc *= 2;
        Byte *np = (Byte *)mem_realloc(b->a, b->p, (size_t)b->cap, (size_t)nc, 1);
        if (np == NULL) {
            b->failed = true;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static void gob_buf_str(GobBuf *b, Str s) {
    gob_buf_add(b, s.p, s.len);
}

static void gob_buf_cstr(GobBuf *b, const char *s) {
    gob_buf_add(b, s, (Int)strlen(s));
}

static void gob_buf_int(GobBuf *b, int64_t v) {
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    gob_buf_add(b, tmp, n);
}

static Str gob_buf_done(GobBuf *b) {
    if (b->failed)
        return (Str){NULL, 0};
    return (Str){b->p, b->len};
}

Str burrow__gob_type_name(const Type *t) {
    if (t == NULL || t == TYPE_ANY)
        return (Str){NULL, 0};
    return t->name;
}

/* The last element of a package path, which is its name as far as String()
 * is concerned. */
static Str gob_pkg_name(Str path) {
    Int i = path.len;
    while (i > 0 && path.p[i - 1] != '/')
        i--;
    return (Str){path.p + i, path.len - i};
}

static void gob_type_string(GobBuf *b, const Type *t, int depth) {
    if (t == NULL) {
        gob_buf_cstr(b, "<nil>");
        return;
    }
    if (depth > 64) {
        gob_buf_cstr(b, "...");
        return;
    }
    if (t->name.len > 0) {
        if (t->pkg_path.len > 0) {
            gob_buf_str(b, gob_pkg_name(t->pkg_path));
            gob_buf_cstr(b, ".");
        }
        gob_buf_str(b, t->name);
        return;
    }
    switch ((int)t->kind) {
    case KIND_POINTER:
        gob_buf_cstr(b, "*");
        gob_type_string(b, t->elem, depth + 1);
        return;
    case KIND_SLICE:
        gob_buf_cstr(b, "[]");
        gob_type_string(b, t->elem, depth + 1);
        return;
    case KIND_ARRAY:
        gob_buf_cstr(b, "[");
        gob_buf_int(b, (int64_t)t->len);
        gob_buf_cstr(b, "]");
        gob_type_string(b, t->elem, depth + 1);
        return;
    case KIND_MAP:
        gob_buf_cstr(b, "map[");
        gob_type_string(b, t->key, depth + 1);
        gob_buf_cstr(b, "]");
        gob_type_string(b, t->elem, depth + 1);
        return;
    case KIND_CHAN:
        gob_buf_cstr(b, "chan ");
        gob_type_string(b, t->elem, depth + 1);
        return;
    case KIND_FUNC:
        gob_buf_cstr(b, "func()");
        return;
    case KIND_INTERFACE:
        gob_buf_cstr(b, "interface {}");
        return;
    case KIND_STRUCT:
        if (t->nfield == 0) {
            gob_buf_cstr(b, "struct {}");
            return;
        }
        gob_buf_cstr(b, "struct {");
        for (Int i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            gob_buf_cstr(b, i == 0 ? " " : "; ");
            if (!field_is_embedded(f)) {
                gob_buf_str(b, f->name);
                gob_buf_cstr(b, " ");
            }
            gob_type_string(b, f->type, depth + 1);
            if (f->tag.len > 0) {
                gob_buf_cstr(b, " ");
                Str q = strconv_quote(heap_allocator(), f->tag);
                gob_buf_str(b, q);
                mem_free(heap_allocator(), (void *)(uintptr_t)q.p, (size_t)q.len, 1);
            }
        }
        gob_buf_cstr(b, " }");
        return;
    default:
        gob_buf_str(b, kind_name(t->kind));
        return;
    }
}

/* ------------------------------------------------------------ stack room
 *
 * Go's stack grows to fit a value however deep it is and a C stack does not,
 * so each level of nesting asks whether there is room for the next one. On a
 * goroutine the stack's bounds are one read away. On a thread of the
 * system's own they cost a system call on some systems, so they are only
 * looked up once nesting gets past a depth that any thread has room for. */

enum { GOB_THREAD_CHECK_AFTER = 32 };

/* Where the stack is now. A local's address will not do under
 * AddressSanitizer, which can put locals on a fake stack in the heap, so the
 * frame address is used wherever there is one. */
static uintptr_t gob_stack_here(void) {
#if defined(__GNUC__) || defined(__clang__)
    return (uintptr_t)__builtin_frame_address(0);
#elif defined(_MSC_VER)
    return (uintptr_t)_AddressOfReturnAddress();
#else
    volatile char probe = 0;
    return (uintptr_t)&probe;
#endif
}

bool burrow__gob_stack_low(uintptr_t *floor, int depth) {
    /* 0 is not looked at yet, 1 is bounds nobody would give, which falls
     * back on a count, and 2 is an OS thread whose bounds are left until the
     * nesting gets deep enough to be worth the call. */
    if (*floor == 0) {
        burrow__Stack *s = burrow__stack_current();
        /* A goroutine on a Windows fiber has no bounds of its own here, and
         * the fiber's are the thread's while it runs, so it asks the same way
         * a thread does. */
        if (s != NULL && s->lo != NULL)
            *floor = (uintptr_t)s->lo + GOB_STACK_MARGIN;
        else
            *floor = 2;
    }
    if (*floor == 2) {
        if (depth < GOB_THREAD_CHECK_AFTER)
            return false;
        void *lo = NULL;
        if (!burrow__thread_stack_limit(&lo))
            lo = NULL;
        *floor = lo == NULL ? 1 : (uintptr_t)lo + GOB_STACK_MARGIN;
    }
    if (*floor == 1)
        return depth >= GOB_MAX_DEPTH;
    return gob_stack_here() < *floor;
}

Str burrow__gob_type_string(Alloc *a, const Type *t) {
    GobBuf b = {a, NULL, 0, 0, false};
    gob_type_string(&b, t, 0);
    return gob_buf_done(&b);
}

/* a followed by b, from the error allocator. */
static Str gob_cat(Str a, Str b) {
    return fmt_sprintf_v(error_allocator(), "%s%s", a, b);
}

/* A value for fmt's %s that is a type's String(). The bytes come from the
 * error allocator, which is where the error they go into lives. */
static Str gob_tstr(const Type *t) {
    return burrow__gob_type_string(error_allocator(), t);
}

/* ------------------------------------------------------------- identity */

/* A string that is the same for two descriptors of one type and differs for
 * two types, as far as a descriptor can say. Named types are told apart by
 * package path and name, which is what makes them different in Go. */
static void gob_type_key(GobBuf *b, const Type *t, int depth) {
    if (t == NULL) {
        gob_buf_cstr(b, "<nil>");
        return;
    }
    if (t->name.len > 0 || depth > 32) {
        gob_buf_str(b, t->pkg_path);
        gob_buf_cstr(b, ".");
        gob_buf_str(b, t->name);
        if (depth > 32 || t->name.len == 0) {
            char tmp[32];
            int n = snprintf(tmp, sizeof tmp, "#%p", (const void *)t);
            gob_buf_add(b, tmp, n);
        }
        return;
    }
    switch ((int)t->kind) {
    case KIND_POINTER:
        gob_buf_cstr(b, "*");
        gob_type_key(b, t->elem, depth + 1);
        return;
    case KIND_SLICE:
        gob_buf_cstr(b, "[]");
        gob_type_key(b, t->elem, depth + 1);
        return;
    case KIND_ARRAY:
        gob_buf_cstr(b, "[");
        gob_buf_int(b, (int64_t)t->len);
        gob_buf_cstr(b, "]");
        gob_type_key(b, t->elem, depth + 1);
        return;
    case KIND_MAP:
        gob_buf_cstr(b, "map[");
        gob_type_key(b, t->key, depth + 1);
        gob_buf_cstr(b, "]");
        gob_type_key(b, t->elem, depth + 1);
        return;
    case KIND_CHAN:
        gob_buf_cstr(b, "chan ");
        gob_type_key(b, t->elem, depth + 1);
        return;
    case KIND_STRUCT:
        gob_buf_cstr(b, "struct{");
        for (Int i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            gob_buf_str(b, f->name);
            gob_buf_cstr(b, " ");
            gob_type_key(b, f->type, depth + 1);
            gob_buf_cstr(b, " ");
            gob_buf_int(b, f->tag.len);
            gob_buf_cstr(b, ":");
            gob_buf_str(b, f->tag);
            gob_buf_cstr(b, ";");
        }
        gob_buf_cstr(b, "}");
        return;
    default: {
        /* Unnamed functions and interfaces and anything else: only the
         * descriptor itself can say which type it is. */
        char tmp[48];
        int n = snprintf(tmp, sizeof tmp, "%d#%p", (int)t->kind, (const void *)t);
        gob_buf_add(b, tmp, n);
        return;
    }
    }
}

static const Type *gob_canon_l(const Type *t) {
    if (t == NULL || t->name.len > 0)
        return t;
    const Type *c = (const Type *)gob_pget(gob_canon_ptr, t);
    if (c != NULL)
        return c;
    GobBuf b = {heap_allocator(), NULL, 0, 0, false};
    gob_type_key(&b, t, 0);
    Str key = gob_buf_done(&b);
    c = NULL;
    if (key.len > 0) {
        c = (const Type *)gob_sget(gob_canon_key, key);
        if (c == NULL && gob_sset(gob_canon_key, key, t))
            c = t;
    }
    mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
    if (c == NULL)
        return t;
    (void)gob_pset(gob_canon_ptr, t, c);
    return c;
}

static bool gob_init_l(void);

const Type *burrow__gob_canon(const Type *t) {
    if (t == NULL || t->name.len > 0)
        return t;
    burrow__lock(&gob_lock);
    const Type *c = gob_init_l() ? gob_canon_l(t) : t;
    burrow__unlock(&gob_lock);
    return c;
}

static const Type *gob_ptr_to_l(const Type *t) {
    t = gob_canon_l(t);
    const Type *p = (const Type *)gob_pget(gob_ptrs, t);
    if (p != NULL)
        return p;
    Type *np = BURROW_NEW(heap_allocator(), Type);
    if (np == NULL)
        return NULL;
    memset(np, 0, sizeof *np);
    np->kind = KIND_POINTER;
    np->size = (uint32_t)sizeof(void *);
    np->align = (uint16_t)_Alignof(void *);
    np->elem = t;
    const Type *c = gob_canon_l(np);
    (void)gob_pset(gob_ptrs, t, c);
    return c;
}

const Type *burrow__gob_ptr_to(const Type *t) {
    burrow__lock(&gob_lock);
    const Type *p = gob_init_l() ? gob_ptr_to_l(t) : NULL;
    burrow__unlock(&gob_lock);
    return p;
}

/* ------------------------------------------------------------- gob types */

static GobType *gob_new_type(GobKind kind, Str name) {
    GobType *g = BURROW_NEW(heap_allocator(), GobType);
    if (g == NULL)
        return NULL;
    memset(g, 0, sizeof *g);
    g->kind = kind;
    g->name = gob_strdup(name);
    return g;
}

/* setTypeId. */
static bool gob_set_type_id(GobType *t) {
    if (t->id != 0)
        return true;
    if (gob_nids >= gob_capids) {
        Int nc = gob_capids == 0 ? 128 : gob_capids * 2;
        GobType **n = (GobType **)mem_realloc(
            heap_allocator(), gob_ids, (size_t)gob_capids * sizeof *n,
            (size_t)nc * sizeof *n, _Alignof(GobType *));
        if (n == NULL)
            return false;
        memset(n + gob_capids, 0, (size_t)(nc - gob_capids) * sizeof *n);
        gob_ids = n;
        gob_capids = nc;
    }
    t->id = (int32_t)gob_nids;
    gob_ids[gob_nids++] = t;
    return true;
}

static bool gob_add_field(GobType *st, Str name, int32_t id) {
    if (st->nfield == st->capfield) {
        Int nc = st->capfield == 0 ? 4 : st->capfield * 2;
        GobFieldType *n = (GobFieldType *)mem_realloc(
            heap_allocator(), st->field, (size_t)st->capfield * sizeof *n,
            (size_t)nc * sizeof *n, _Alignof(GobFieldType));
        if (n == NULL)
            return false;
        st->field = n;
        st->capfield = nc;
    }
    st->field[st->nfield].name = gob_strdup(name);
    st->field[st->nfield].id = id;
    st->nfield++;
    return true;
}

static const GobType *gob_id_to_type_l(int32_t id) {
    if (id < 0 || id >= gob_nids)
        return NULL;
    return gob_ids[id];
}

const GobType *burrow__gob_id_to_type(int32_t id) {
    burrow__lock(&gob_lock);
    const GobType *t = gob_init_l() ? gob_id_to_type_l(id) : NULL;
    burrow__unlock(&gob_lock);
    return t;
}

const GobType *burrow__gob_builtin_id_to_type(int32_t id) {
    if (id < 0 || id >= GOB_FIRST_USER_ID)
        return NULL;
    burrow__lock(&gob_lock);
    const GobType *t = gob_init_l() ? gob_builtin[id] : NULL;
    burrow__unlock(&gob_lock);
    return t;
}

/* safeString. seen is the ids already on the way down. */
typedef struct GobSeen {
    int32_t *ids;
    Int n;
    Int cap;
} GobSeen;

static bool gob_seen(GobSeen *s, int32_t id) {
    for (Int i = 0; i < s->n; i++)
        if (s->ids[i] == id)
            return true;
    return false;
}

static void gob_see(GobSeen *s, int32_t id) {
    if (s->n == s->cap) {
        Int nc = s->cap == 0 ? 8 : s->cap * 2;
        int32_t *n =
            (int32_t *)mem_realloc(heap_allocator(), s->ids, (size_t)s->cap * sizeof *n,
                                   (size_t)nc * sizeof *n, _Alignof(int32_t));
        if (n == NULL)
            return;
        s->ids = n;
        s->cap = nc;
    }
    s->ids[s->n++] = id;
}

static void gob_safe_string(GobBuf *b, const GobType *t, GobSeen *seen);

static void gob_id_safe_string(GobBuf *b, int32_t id, GobSeen *seen) {
    const GobType *t = id == 0 ? NULL : gob_id_to_type_l(id);
    if (t == NULL) {
        /* A nil gobType's safeString is a nil dereference in Go. The only
         * way here is a stream that named a type it never described. */
        gob_buf_cstr(b, "<nil>");
        return;
    }
    gob_safe_string(b, t, seen);
}

static void gob_safe_string(GobBuf *b, const GobType *t, GobSeen *seen) {
    switch (t->kind) {
    case GOB_COMMON:
    case GOB_GOB_ENCODER:
        gob_buf_str(b, t->name);
        return;
    case GOB_ARRAY:
        if (gob_seen(seen, t->id)) {
            gob_buf_str(b, t->name);
            return;
        }
        gob_see(seen, t->id);
        gob_buf_cstr(b, "[");
        gob_buf_int(b, t->len);
        gob_buf_cstr(b, "]");
        gob_id_safe_string(b, t->elem, seen);
        return;
    case GOB_SLICE:
        if (gob_seen(seen, t->id)) {
            gob_buf_str(b, t->name);
            return;
        }
        gob_see(seen, t->id);
        gob_buf_cstr(b, "[]");
        gob_id_safe_string(b, t->elem, seen);
        return;
    case GOB_MAP:
        if (gob_seen(seen, t->id)) {
            gob_buf_str(b, t->name);
            return;
        }
        gob_see(seen, t->id);
        gob_buf_cstr(b, "map[");
        gob_id_safe_string(b, t->key, seen);
        gob_buf_cstr(b, "]");
        gob_id_safe_string(b, t->elem, seen);
        return;
    case GOB_STRUCT:
        if (gob_seen(seen, t->id)) {
            gob_buf_str(b, t->name);
            return;
        }
        gob_see(seen, t->id);
        gob_buf_str(b, t->name);
        gob_buf_cstr(b, " = struct { ");
        for (Int i = 0; i < t->nfield; i++) {
            gob_buf_str(b, t->field[i].name);
            gob_buf_cstr(b, " ");
            gob_id_safe_string(b, t->field[i].id, seen);
            gob_buf_cstr(b, "; ");
        }
        gob_buf_cstr(b, "}");
        return;
    default:
        return;
    }
}

Str burrow__gob_type_str(Alloc *a, const GobType *t) {
    GobBuf b = {a, NULL, 0, 0, false};
    GobSeen seen = {NULL, 0, 0};
    burrow__lock(&gob_lock);
    gob_safe_string(&b, t, &seen);
    burrow__unlock(&gob_lock);
    mem_free(heap_allocator(), seen.ids, (size_t)seen.cap * sizeof *seen.ids,
             _Alignof(int32_t));
    return gob_buf_done(&b);
}

/* ------------------------------------------------------------ user types */

bool burrow__gob_is_exported(Str name) {
    if (name.len == 0)
        return false;
    Int size = 0;
    Rune r = utf8_decode_rune_in_string(name, &size);
    return unicode_is_upper(r);
}

bool burrow__gob_is_sent(const Field *f) {
    if (!burrow__gob_is_exported(f->name))
        return false;
    const Type *t = f->type;
    for (int i = 0; t != NULL && t->kind == KIND_POINTER && i < 100; i++)
        t = t->elem;
    return t != NULL && t->kind != KIND_CHAN && t->kind != KIND_FUNC;
}

enum { GOB_SHAPE_MARSHAL, GOB_SHAPE_UNMARSHAL };

static const Method *gob_method(const Type *t, Str name, int shape) {
    if (t == NULL || t->nmethod == 0)
        return NULL;
    const Method *m = type_method_by_name(t, name);
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return NULL;
    const Type *f = m->ftype;
    if (type_num_in(f) != 2 || type_num_out(f) != 1 ||
        type_in(f, 0) != &burrow_type_EncodingAllocArg)
        return NULL;
    if (shape == GOB_SHAPE_MARSHAL)
        return type_in(f, 1) == &burrow_type_EncodingErrorArg &&
                       type_out(f, 0) == TYPE_BYTES
                   ? m
                   : NULL;
    return type_in(f, 1) == TYPE_BYTES && type_out(f, 0) == TYPE_ERROR ? m : NULL;
}

static const GobUserType *gob_user_type_l(const Type *rt, Error *err) {
    const GobUserType *cached = (const GobUserType *)gob_pget(gob_uts, rt);
    if (cached != NULL)
        return cached;
    const Type *user = gob_canon_l(rt);
    cached = (const GobUserType *)gob_pget(gob_uts, user);
    if (cached != NULL) {
        (void)gob_pset(gob_uts, rt, cached);
        return cached;
    }
    GobUserType *ut = BURROW_NEW(heap_allocator(), GobUserType);
    if (ut == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    memset(ut, 0, sizeof *ut);
    ut->user = user;
    ut->base = user;
    const Type *slowpoke = user;
    for (;;) {
        const Type *pt = ut->base;
        if (pt->kind != KIND_POINTER)
            break;
        ut->base = gob_canon_l(pt->elem);
        if (ut->base == slowpoke) {
            *err =
                errors_new(error_allocator(),
                           gob_cat(BURROW_S("can't represent recursive pointer type "),
                                   gob_tstr(ut->base)));
            mem_free(heap_allocator(), ut, sizeof *ut, _Alignof(GobUserType));
            return NULL;
        }
        if (ut->indir % 2 == 0)
            slowpoke = gob_canon_l(slowpoke->elem);
        ut->indir++;
    }
    /* implementsInterface, with every method on the base type: the first
     * level that implements it is the base, or the pointer to it. */
    int indir = ut->indir > 0 ? ut->indir - 1 : 0;
    const Method *m = gob_method(ut->base, BURROW_S("GobEncode"), GOB_SHAPE_MARSHAL);
    if (m != NULL) {
        ut->external_enc = GOB_X_GOB;
    } else {
        m = gob_method(ut->base, BURROW_S("MarshalBinary"), GOB_SHAPE_MARSHAL);
        if (m != NULL)
            ut->external_enc = GOB_X_BINARY;
    }
    if (m != NULL) {
        ut->enc_method = m;
        ut->enc_indir = indir;
    }
    m = gob_method(ut->base, BURROW_S("GobDecode"), GOB_SHAPE_UNMARSHAL);
    if (m != NULL) {
        ut->external_dec = GOB_X_GOB;
    } else {
        m = gob_method(ut->base, BURROW_S("UnmarshalBinary"), GOB_SHAPE_UNMARSHAL);
        if (m != NULL)
            ut->external_dec = GOB_X_BINARY;
    }
    if (m != NULL) {
        ut->dec_method = m;
        ut->dec_indir = indir;
    }
    if (!gob_pset(gob_uts, user, ut) || (rt != user && !gob_pset(gob_uts, rt, ut))) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    return ut;
}

const GobUserType *burrow__gob_user_type(const Type *t, Error *err) {
    *err = BURROW_NO_ERROR;
    burrow__lock(&gob_lock);
    const GobUserType *ut = NULL;
    if (gob_init_l())
        ut = gob_user_type_l(t, err);
    else
        *err = burrow_err_out_of_memory;
    burrow__unlock(&gob_lock);
    return ut;
}

static GobEncPlan *gob_enc_plan_l(const GobUserType *ut, Error *err) {
    const Type *t = ut->base;
    GobEncPlan *plan = (GobEncPlan *)mem_alloc(
        heap_allocator(), sizeof(GobEncPlan) + (size_t)t->nfield * sizeof(GobEncField),
        _Alignof(GobEncPlan));
    if (plan == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    plan->n = 0;
    for (uint16_t i = 0; i < t->nfield; i++) {
        const Field *f = &t->fields[i];
        if (!burrow__gob_is_sent(f))
            continue;
        const GobUserType *fut = gob_user_type_l(f->type, err);
        if (fut == NULL) {
            mem_free(heap_allocator(), plan, 0, _Alignof(GobEncPlan));
            return NULL;
        }
        GobEncField *ef = &plan->f[plan->n];
        ef->type = f->type;
        ef->ut = fut;
        ef->offset = f->offset;
        ef->wire = (int32_t)plan->n;
        plan->n++;
    }
    return plan;
}

const GobEncPlan *burrow__gob_enc_plan(const GobUserType *ut, Error *err) {
    GobUserType *m = (GobUserType *)(uintptr_t)ut;
    const GobEncPlan *plan = burrow__atomic_load_acquire_ptr(&m->enc_plan);
    if (plan != NULL)
        return plan;
    *err = BURROW_NO_ERROR;
    burrow__lock(&gob_lock);
    plan = m->enc_plan;
    if (plan == NULL) {
        GobEncPlan *np = gob_init_l() ? gob_enc_plan_l(ut, err) : NULL;
        if (np == NULL && BURROW_OK(*err))
            *err = burrow_err_out_of_memory;
        if (np != NULL)
            burrow__atomic_store_release_ptr(&m->enc_plan, np);
        plan = np;
    }
    burrow__unlock(&gob_lock);
    return plan;
}

const GobUserType *burrow__gob_elem_ut(const GobUserType *ut, Error *err) {
    GobUserType *m = (GobUserType *)(uintptr_t)ut;
    const GobUserType *eut = burrow__atomic_load_acquire_ptr(&m->elem_ut);
    if (eut != NULL)
        return eut;
    eut = burrow__gob_user_type(ut->base->elem, err);
    if (eut != NULL)
        burrow__atomic_store_release_ptr(&m->elem_ut, (void *)(uintptr_t)eut);
    return eut;
}

/* --------------------------------------------------------- newTypeObject */

static GobType *gob_get_type_l(Str name, const GobUserType *ut, const Type *rt,
                               Error *err);

/* getBaseType. */
static GobType *gob_get_base_type_l(Str name, const Type *rt, Error *err) {
    const GobUserType *ut = gob_user_type_l(rt, err);
    if (ut == NULL)
        return NULL;
    return gob_get_type_l(name, ut, ut->base, err);
}

static GobType *gob_oom(Error *err) {
    *err = burrow_err_out_of_memory;
    return NULL;
}

static GobType *gob_new_type_object_l(Str name, const GobUserType *ut, const Type *rt,
                                      Error *err) {
    if (ut->external_enc != 0) {
        GobType *g = gob_new_type(GOB_GOB_ENCODER, name);
        if (g == NULL || !gob_set_type_id(g))
            return gob_oom(err);
        return g;
    }
    switch ((int)rt->kind) {
    case KIND_BOOL:
        return gob_ids[GOB_T_BOOL];
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        return gob_ids[GOB_T_INT];
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return gob_ids[GOB_T_UINT];
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        return gob_ids[GOB_T_FLOAT];
    case KIND_COMPLEX64:
    case KIND_COMPLEX128:
        return gob_ids[GOB_T_COMPLEX];
    case KIND_STRING:
        return gob_ids[GOB_T_STRING];
    case KIND_INTERFACE:
        return gob_ids[GOB_T_INTERFACE];
    case KIND_ARRAY: {
        GobType *at = gob_new_type(GOB_ARRAY, name);
        if (at == NULL || !gob_pset(gob_types, rt, at))
            return gob_oom(err);
        GobType *t0 = gob_get_base_type_l((Str){NULL, 0}, rt->elem, err);
        if (t0 == NULL || !gob_set_type_id(at)) {
            gob_pdel(gob_types, rt);
            return BURROW_FAILED(*err) ? NULL : gob_oom(err);
        }
        at->elem = t0->id;
        at->len = (int64_t)rt->len;
        return at;
    }
    case KIND_MAP: {
        GobType *mt = gob_new_type(GOB_MAP, name);
        if (mt == NULL || !gob_pset(gob_types, rt, mt))
            return gob_oom(err);
        GobType *t0 = gob_get_base_type_l((Str){NULL, 0}, rt->key, err);
        GobType *t1 =
            t0 != NULL ? gob_get_base_type_l((Str){NULL, 0}, rt->elem, err) : NULL;
        if (t1 == NULL || !gob_set_type_id(mt)) {
            gob_pdel(gob_types, rt);
            return BURROW_FAILED(*err) ? NULL : gob_oom(err);
        }
        mt->key = t0->id;
        mt->elem = t1->id;
        return mt;
    }
    case KIND_SLICE: {
        if (rt->elem->kind == KIND_UINT8)
            return gob_ids[GOB_T_BYTES];
        GobType *st = gob_new_type(GOB_SLICE, name);
        if (st == NULL || !gob_pset(gob_types, rt, st))
            return gob_oom(err);
        GobType *t0 =
            gob_get_base_type_l(burrow__gob_type_name(rt->elem), rt->elem, err);
        if (t0 == NULL || !gob_set_type_id(st) || !gob_set_type_id(t0)) {
            gob_pdel(gob_types, rt);
            return BURROW_FAILED(*err) ? NULL : gob_oom(err);
        }
        st->elem = t0->id;
        return st;
    }
    case KIND_STRUCT: {
        GobType *st = gob_new_type(GOB_STRUCT, name);
        if (st == NULL || !gob_set_type_id(st) || !gob_pset(gob_types, rt, st))
            return gob_oom(err);
        for (Int i = 0; i < rt->nfield; i++) {
            const Field *f = &rt->fields[i];
            if (!burrow__gob_is_sent(f))
                continue;
            /* An error here leaves rt in types, as Go's does: the err its
             * deferred delete looks at is not the one this loop sets. */
            const GobUserType *fut = gob_user_type_l(f->type, err);
            if (fut == NULL)
                return NULL;
            Str tname = burrow__gob_type_name(fut->base);
            bool owned = tname.len == 0;
            if (owned)
                tname = burrow__gob_type_string(heap_allocator(), fut->base);
            GobType *gt = gob_get_base_type_l(tname, f->type, err);
            if (owned && tname.p != NULL)
                mem_free(heap_allocator(), (void *)(uintptr_t)tname.p,
                         (size_t)tname.len, 1);
            if (gt == NULL)
                return NULL;
            if (!gob_set_type_id(gt) || !gob_add_field(st, f->name, gt->id))
                return gob_oom(err);
        }
        return st;
    }
    default: {
        Str s =
            gob_cat(BURROW_S("gob NewTypeObject can't handle type: "), gob_tstr(rt));
        *err = errors_new(error_allocator(), s);
        return NULL;
    }
    }
}

static GobType *gob_get_type_l(Str name, const GobUserType *ut, const Type *rt,
                               Error *err) {
    rt = gob_canon_l(rt);
    GobType *t = (GobType *)gob_pget(gob_types, rt);
    if (t != NULL)
        return t;
    t = gob_new_type_object_l(name, ut, rt, err);
    if (t != NULL && !gob_pset(gob_types, rt, t))
        return gob_oom(err);
    return t;
}

/* ----------------------------------------------------------- getTypeInfo */

static GobTypeInfo *gob_type_info_l(const GobUserType *ut, Error *err) {
    const Type *rt = ut->external_enc != 0 ? ut->user : ut->base;
    GobTypeInfo *info = (GobTypeInfo *)gob_pget(gob_infos, rt);
    if (info != NULL)
        return info;
    GobType *gt = gob_get_base_type_l(burrow__gob_type_name(rt), rt, err);
    if (gt == NULL)
        return NULL;
    info = BURROW_NEW(heap_allocator(), GobTypeInfo);
    if (info == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    memset(info, 0, sizeof *info);
    info->id = gt->id;
    if (ut->external_enc != 0) {
        GobType *u = gob_get_type_l(burrow__gob_type_name(rt), ut, rt, err);
        if (u == NULL)
            return NULL;
        GobType *g = gob_ids[u->id];
        switch (ut->external_enc) {
        case GOB_X_GOB:
            info->wire.t[GOB_W_GOB_ENCODER] = g;
            break;
        case GOB_X_BINARY:
            info->wire.t[GOB_W_BINARY_MARSHALER] = g;
            break;
        default:
            info->wire.t[GOB_W_TEXT_MARSHALER] = g;
            break;
        }
    } else {
        GobType *t = gob_ids[info->id];
        switch ((int)rt->kind) {
        case KIND_ARRAY:
            info->wire.t[GOB_W_ARRAY] = t;
            break;
        case KIND_MAP:
            info->wire.t[GOB_W_MAP] = t;
            break;
        case KIND_SLICE:
            if (rt->elem->kind != KIND_UINT8)
                info->wire.t[GOB_W_SLICE] = t;
            break;
        case KIND_STRUCT:
            info->wire.t[GOB_W_STRUCT] = t;
            break;
        default:
            break;
        }
    }
    if (!gob_pset(gob_infos, rt, info)) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    return info;
}

const GobTypeInfo *burrow__gob_type_info(const GobUserType *ut, Error *err) {
    *err = BURROW_NO_ERROR;
    burrow__lock(&gob_lock);
    const GobTypeInfo *info = NULL;
    if (gob_init_l())
        info = gob_type_info_l(ut, err);
    else
        *err = burrow_err_out_of_memory;
    burrow__unlock(&gob_lock);
    return info;
}

/* ------------------------------------------------------------- compileEnc
 *
 * The encoder here walks a value's descriptor as it goes rather than
 * compiling a program for it first, which comes out the same except for the
 * errors compiling finds. Those Go reports before it writes a byte of the
 * value, even for a part of the type the value does not use, so this finds
 * them the same way, once per type. */

typedef struct GobEncCheck {
    const void **items;
    Int n;
    Int cap;
} GobEncCheck;

static bool gob_check_has(const GobEncCheck *c, const void *p) {
    for (Int i = 0; i < c->n; i++)
        if (c->items[i] == p)
            return true;
    return false;
}

static bool gob_check_add(GobEncCheck *c, const void *p) {
    if (c->n == c->cap) {
        Int nc = c->cap == 0 ? 8 : c->cap * 2;
        const void **n = (const void **)mem_realloc(
            heap_allocator(), c->items, (size_t)c->cap * sizeof *n,
            (size_t)nc * sizeof *n, _Alignof(const void *));
        if (n == NULL)
            return false;
        c->items = n;
        c->cap = nc;
    }
    c->items[c->n++] = p;
    return true;
}

static void gob_check_free(GobEncCheck *c) {
    mem_free(heap_allocator(), c->items, (size_t)c->cap * sizeof *c->items,
             _Alignof(const void *));
}

static Error gob_enc_engine_l(const GobUserType *ut, GobEncCheck *building);

static Error gob_enc_op_for_l(const Type *rt, GobEncCheck *in_progress,
                              GobEncCheck *building) {
    Error err = BURROW_NO_ERROR;
    const GobUserType *ut = gob_user_type_l(rt, &err);
    if (ut == NULL)
        return err;
    if (ut->external_enc != 0)
        return BURROW_NO_ERROR;
    rt = gob_canon_l(rt);
    if (gob_check_has(in_progress, rt))
        return BURROW_NO_ERROR;
    const Type *t = ut->base;
    if (t->kind >= KIND_BOOL && t->kind <= KIND_COMPLEX128)
        return BURROW_NO_ERROR;
    if (t->kind == KIND_STRING)
        return BURROW_NO_ERROR;
    if (!gob_check_add(in_progress, rt))
        return burrow_err_out_of_memory;
    switch ((int)t->kind) {
    case KIND_SLICE:
        if (t->elem->kind == KIND_UINT8)
            return BURROW_NO_ERROR;
        return gob_enc_op_for_l(t->elem, in_progress, building);
    case KIND_ARRAY:
        return gob_enc_op_for_l(t->elem, in_progress, building);
    case KIND_MAP:
        err = gob_enc_op_for_l(t->key, in_progress, building);
        if (BURROW_FAILED(err))
            return err;
        return gob_enc_op_for_l(t->elem, in_progress, building);
    case KIND_STRUCT: {
        const GobUserType *sut = gob_user_type_l(t, &err);
        if (sut == NULL)
            return err;
        err = gob_enc_engine_l(sut, building);
        if (BURROW_FAILED(err))
            return err;
        /* mustGetTypeInfo, which panics where the rest reports. */
        if (gob_type_info_l(sut, &err) == NULL)
            panic_str(gob_cat(BURROW_S("getTypeInfo: "), error_text(err)));
        return BURROW_NO_ERROR;
    }
    case KIND_INTERFACE:
        return BURROW_NO_ERROR;
    default:
        return burrow__gob_errorf("can't happen: encode type %s", gob_tstr(rt));
    }
}

static Error gob_compile_enc_l(const GobUserType *ut, GobEncCheck *building) {
    const Type *srt = ut->base;
    const Type *rt = ut->external_enc != 0 ? ut->user : ut->base;
    GobEncCheck seen = {NULL, 0, 0};
    Error err = BURROW_NO_ERROR;
    if (ut->external_enc == 0 && srt->kind == KIND_STRUCT) {
        Int n = 0;
        for (Int i = 0; i < srt->nfield && BURROW_OK(err); i++) {
            const Field *f = &srt->fields[i];
            if (!burrow__gob_is_sent(f))
                continue;
            err = gob_enc_op_for_l(f->type, &seen, building);
            n++;
        }
        if (BURROW_OK(err) && srt->nfield > 0 && n == 0)
            err = burrow__gob_errorf("type %s has no exported fields", gob_tstr(rt));
    } else {
        err = gob_enc_op_for_l(rt, &seen, building);
    }
    gob_check_free(&seen);
    return err;
}

/* getEncEngine and buildEncEngine. */
static Error gob_enc_engine_l(const GobUserType *ut, GobEncCheck *building) {
    Error err = BURROW_NO_ERROR;
    GobTypeInfo *info = gob_type_info_l(ut, &err);
    if (info == NULL)
        return err;
    if (info->enc_ok || gob_check_has(building, info))
        return BURROW_NO_ERROR;
    if (!gob_check_add(building, info))
        return burrow_err_out_of_memory;
    err = gob_compile_enc_l(ut, building);
    if (BURROW_OK(err))
        info->enc_ok = true;
    return err;
}

Error burrow__gob_check_enc(const GobUserType *ut) {
    burrow__lock(&gob_lock);
    Error err = BURROW_NO_ERROR;
    if (!gob_init_l()) {
        err = burrow_err_out_of_memory;
    } else {
        GobEncCheck building = {NULL, 0, 0};
        err = gob_enc_engine_l(ut, &building);
        gob_check_free(&building);
    }
    burrow__unlock(&gob_lock);
    return err;
}

/* ----------------------------------------------------------- registration */

static void gob_panicf_unlock(Str msg) {
    burrow__unlock(&gob_lock);
    panic_str(msg);
}

static void gob_register_name_l(Str name, const Type *t) {
    if (name.len == 0)
        gob_panicf_unlock(BURROW_S("attempt to register empty name"));
    Error err = BURROW_NO_ERROR;
    const GobUserType *ut = gob_user_type_l(t, &err);
    if (ut == NULL)
        gob_panicf_unlock(error_text(err));
    const Type *have = (const Type *)gob_sget(gob_name_to_type, name);
    Str *n = (Str *)gob_pget(gob_type_to_name, ut->base);
    /* Go stores the name and deletes it again when the type already has
     * another one. Not storing it comes to the same thing. */
    if (have == NULL && (n == NULL || str_eq(*n, name))) {
        if (!gob_sset(gob_name_to_type, name, ut->user))
            gob_panicf_unlock(error_text(burrow_err_out_of_memory));
    } else if (have != NULL && have != ut->user) {
        gob_panicf_unlock(fmt_sprintf_v(
            error_allocator(), "gob: registering duplicate types for %q: %s != %s",
            name, burrow__gob_type_string(error_allocator(), have),
            burrow__gob_type_string(error_allocator(), ut->user)));
    }
    if (n == NULL) {
        Str *copy = BURROW_NEW(heap_allocator(), Str);
        if (copy == NULL)
            gob_panicf_unlock(error_text(burrow_err_out_of_memory));
        *copy = gob_strdup(name);
        if (!gob_pset(gob_type_to_name, ut->base, copy))
            gob_panicf_unlock(error_text(burrow_err_out_of_memory));
    } else if (!str_eq(*n, name)) {
        gob_panicf_unlock(fmt_sprintf_v(
            error_allocator(), "gob: registering duplicate names for %s: %q != %q",
            burrow__gob_type_string(error_allocator(), ut->user), *n, name));
    }
}

/* Register's choice of name. It comes from the error allocator, which
 * keeps it, since a panic out of registering would leak a heap copy. */
static Str gob_register_name_for(const Type *t) {
    Str name = burrow__gob_type_name(t);
    if (name.len == 0)
        return burrow__gob_type_string(error_allocator(), t);
    if (t->pkg_path.len == 0)
        return name;
    return fmt_sprintf_v(error_allocator(), "%s.%s", t->pkg_path, name);
}

void gob_register_name(Str name, Any v) {
    burrow__lock(&gob_lock);
    if (!gob_init_l())
        gob_panicf_unlock(error_text(burrow_err_out_of_memory));
    if (v.t == NULL)
        gob_panicf_unlock(BURROW_S("gob: Register of nil value"));
    gob_register_name_l(name, v.t);
    burrow__unlock(&gob_lock);
}

void gob_register(Any v) {
    if (v.t == NULL)
        panic_str(BURROW_S("gob: Register of nil value"));
    gob_register_name(gob_register_name_for(v.t), v);
}

bool burrow__gob_name_of(const Type *base, Str *name) {
    burrow__lock(&gob_lock);
    const Str *n = gob_init_l() ? (const Str *)gob_pget(gob_type_to_name, base) : NULL;
    if (n != NULL)
        *name = *n;
    burrow__unlock(&gob_lock);
    return n != NULL;
}

const Type *burrow__gob_type_of_name(Str name) {
    burrow__lock(&gob_lock);
    const Type *t =
        gob_init_l() ? (const Type *)gob_sget(gob_name_to_type, name) : NULL;
    burrow__unlock(&gob_lock);
    return t;
}

/* ------------------------------------------------------------- interfaces */

const Type *burrow__gob_iface_elem(const Type *t, void *p, void **vp) {
    if (t == TYPE_ANY) {
        const Any *a = (const Any *)p;
        if (a->t == NULL || a->data == NULL)
            return NULL;
        *vp = a->data;
        return a->t;
    }
    if (t == TYPE_ERROR) {
        const Error *err = (const Error *)p;
        if (err->vt == NULL)
            return NULL;
        if (err->vt->self_type == NULL || err->data == NULL) {
            *vp = (void *)(uintptr_t)&gob_error_string_desc;
            return &gob_error_string_desc;
        }
        *vp = (void *)(uintptr_t)err->data;
        return err->vt->self_type;
    }
    Iface *v = (Iface *)p;
    if (v->vt == NULL || v->vt->self_type == NULL)
        return NULL;
    if (t->size > sizeof(Iface))
        *vp = (Byte *)p + sizeof(void *);
    else
        *vp = v->data;
    return v->vt->self_type;
}

/* ------------------------------------------------------------ bootstrap */

static GobType *gob_bootstrap(GobKind kind, const char *name) {
    GobType *t = gob_new_type(kind, str_from_cstr(name));
    if (t == NULL || !gob_set_type_id(t))
        return NULL;
    return t;
}

static bool gob_fields(GobType *t, int n, const char *const *names,
                       const int32_t *ids) {
    for (int i = 0; i < n; i++)
        if (!gob_add_field(t, str_from_cstr(names[i]), ids[i]))
            return false;
    return true;
}

/* The types Go's init makes in order: the builtins, then wireType and what it
 * is made of, which come out as ids 16 to 24. */
static bool gob_bootstrap_all(void) {
    static const char *const common[] = {
        "bool",       "int",        "uint",       "float",      "bytes",
        "string",     "complex",    "interface",  "_reserved7", "_reserved6",
        "_reserved5", "_reserved4", "_reserved3", "_reserved2", "_reserved1",
    };
    gob_nids = 1;
    for (int i = 0; i < 15; i++)
        if (gob_bootstrap(GOB_COMMON, common[i]) == NULL)
            return false;
    GobType *wire = gob_bootstrap(GOB_STRUCT, "wireType");
    GobType *array = gob_bootstrap(GOB_STRUCT, "arrayType");
    GobType *common_t = gob_bootstrap(GOB_STRUCT, "CommonType");
    GobType *slice = gob_bootstrap(GOB_STRUCT, "sliceType");
    GobType *strct = gob_bootstrap(GOB_STRUCT, "structType");
    GobType *field = gob_bootstrap(GOB_STRUCT, "fieldType");
    GobType *fields = gob_bootstrap(GOB_SLICE, "[]gob.fieldType");
    GobType *map = gob_bootstrap(GOB_STRUCT, "mapType");
    GobType *genc = gob_bootstrap(GOB_STRUCT, "gobEncoderType");
    if (genc == NULL)
        return false;
    fields->elem = field->id;
    static const char *const wire_n[] = {
        "ArrayT",      "SliceT",           "StructT",       "MapT",
        "GobEncoderT", "BinaryMarshalerT", "TextMarshalerT"};
    const int32_t wire_i[] = {array->id, slice->id, strct->id, map->id,
                              genc->id,  genc->id,  genc->id};
    static const char *const array_n[] = {"CommonType", "Elem", "Len"};
    const int32_t array_i[] = {common_t->id, GOB_T_INT, GOB_T_INT};
    static const char *const common_n[] = {"Name", "Id"};
    const int32_t common_i[] = {GOB_T_STRING, GOB_T_INT};
    static const char *const slice_n[] = {"CommonType", "Elem"};
    const int32_t slice_i[] = {common_t->id, GOB_T_INT};
    static const char *const struct_n[] = {"CommonType", "Field"};
    const int32_t struct_i[] = {common_t->id, fields->id};
    static const char *const map_n[] = {"CommonType", "Key", "Elem"};
    const int32_t map_i[] = {common_t->id, GOB_T_INT, GOB_T_INT};
    static const char *const genc_n[] = {"CommonType"};
    const int32_t genc_i[] = {common_t->id};
    if (!gob_fields(wire, 7, wire_n, wire_i) ||
        !gob_fields(array, 3, array_n, array_i) ||
        !gob_fields(common_t, 2, common_n, common_i) ||
        !gob_fields(slice, 2, slice_n, slice_i) ||
        !gob_fields(strct, 2, struct_n, struct_i) ||
        !gob_fields(field, 2, common_n, common_i) ||
        !gob_fields(map, 3, map_n, map_i) || !gob_fields(genc, 1, genc_n, genc_i))
        return false;
    for (Int i = 0; i < gob_nids; i++)
        gob_builtin[i] = gob_ids[i];
    /* idToTypeSlice[:firstUserId]: the ids up to 64 stay unused. */
    while (gob_nids < GOB_FIRST_USER_ID) {
        if (gob_nids >= gob_capids) {
            GobType *pad = gob_new_type(GOB_COMMON, (Str){NULL, 0});
            if (pad == NULL || !gob_set_type_id(pad))
                return false;
            gob_ids[pad->id] = NULL;
        } else {
            gob_ids[gob_nids++] = NULL;
        }
    }
    return gob_pset(gob_types, &burrow_type_GobCommonType, common_t);
}

static bool gob_register_basics_l(void) {
    static const Type *const basics[] = {
        &burrow_type_Int,
        &burrow_type_int8_t,
        &burrow_type_int16_t,
        &burrow_type_int32_t,
        &burrow_type_int64_t,
        &burrow_type_Uint,
        &burrow_type_uint8_t,
        &burrow_type_uint16_t,
        &burrow_type_uint32_t,
        &burrow_type_uint64_t,
        &burrow_type_float,
        &burrow_type_double,
        &burrow_type_Complex64,
        &burrow_type_Complex128,
        &burrow_type_Uintptr,
        &burrow_type_bool,
        &burrow_type_Str,
        &burrow_type_Bytes,
        &gob_ints,
        &gob_int8s,
        &gob_int16s,
        &gob_int32s,
        &gob_int64s,
        &gob_uints,
        &burrow_type_Bytes,
        &gob_uint16s,
        &gob_uint32s,
        &gob_uint64s,
        &gob_float32s,
        &gob_float64s,
        &gob_complex64s,
        &gob_complex128s,
        &gob_uintptrs,
        &gob_bools,
        &gob_strings,
    };
    for (size_t i = 0; i < sizeof basics / sizeof basics[0]; i++)
        gob_register_name_l(gob_register_name_for(basics[i]), basics[i]);
    return true;
}

static bool gob_init_l(void) {
    if (gob_inited)
        return true;
    Alloc *h = heap_allocator();
    const Type *p = TYPE_OF(Uintptr);
    gob_types = map_make(h, p, p, 64);
    gob_uts = map_make(h, p, p, 64);
    gob_infos = map_make(h, p, p, 64);
    gob_name_to_type = map_make(h, TYPE_OF(Str), p, 64);
    gob_type_to_name = map_make(h, p, p, 64);
    gob_canon_ptr = map_make(h, p, p, 64);
    gob_canon_key = map_make(h, TYPE_OF(Str), p, 64);
    gob_ptrs = map_make(h, p, p, 16);
    if (gob_types == NULL || gob_uts == NULL || gob_infos == NULL ||
        gob_name_to_type == NULL || gob_type_to_name == NULL || gob_canon_ptr == NULL ||
        gob_canon_key == NULL || gob_ptrs == NULL)
        return false;
    if (!gob_bootstrap_all())
        return false;
    gob_inited = true;
    /* Canonical first, so that anybody's []int is the one registered here. */
    return gob_register_basics_l();
}
