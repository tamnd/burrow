/* The dump that tools/gen-json-common/common.go writes for a Go value, made
 * here for a C one, so the json tests can compare what Go and C unmarshalled
 * without either side knowing the other's layout. Shared by the v1 and v2
 * json tests. The includer defines LEN and cstr first.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_JSON_DUMP_H
#define BURROW_TESTS_JSON_DUMP_H

static void put_hex(JsonBuf *b, uint64_t v, int digits) {
    static const char hexd[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--)
        jsonbuf_byte(b, (Byte)hexd[(v >> (4 * i)) & 15]);
}

static void put_dec(JsonBuf *b, uint64_t u, bool neg) {
    Byte tmp[24];
    int k = 0;
    if (neg)
        jsonbuf_byte(b, '-');
    do {
        tmp[k++] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u > 0);
    while (k > 0)
        jsonbuf_byte(b, tmp[--k]);
}

typedef struct DumpPath {
    const void *p[256];
    int n;
} DumpPath;

static void dump(JsonBuf *b, const Type *t, const void *p, DumpPath *path);

static int cmp_str(const void *x, const void *y) {
    const Str *a = (const Str *)x, *c = (const Str *)y;
    Int n = a->len < c->len ? a->len : c->len;
    int r = n > 0 ? memcmp(a->p, c->p, (size_t)n) : 0;
    if (r != 0)
        return r;
    return a->len < c->len ? -1 : a->len > c->len ? 1 : 0;
}

static void dump(JsonBuf *b, const Type *t, const void *p, DumpPath *path) {
    switch ((int)t->kind) {
    case KIND_BOOL:
        jsonbuf_str(b, *(const bool *)p ? cstr("true") : cstr("false"));
        return;
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64: {
        int64_t v = t->size == 1   ? *(const int8_t *)p
                    : t->size == 2 ? *(const int16_t *)p
                    : t->size == 4 ? *(const int32_t *)p
                                   : *(const int64_t *)p;
        put_dec(b, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, v < 0);
        return;
    }
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR: {
        uint64_t v = t->size == 1   ? *(const uint8_t *)p
                     : t->size == 2 ? *(const uint16_t *)p
                     : t->size == 4 ? *(const uint32_t *)p
                                    : *(const uint64_t *)p;
        put_dec(b, v, false);
        return;
    }
    case KIND_FLOAT32: {
        uint32_t bits;
        memcpy(&bits, p, 4);
        jsonbuf_byte(b, 'f');
        put_hex(b, bits, 8);
        return;
    }
    case KIND_FLOAT64: {
        uint64_t bits;
        memcpy(&bits, p, 8);
        jsonbuf_byte(b, 'd');
        put_hex(b, bits, 16);
        return;
    }
    case KIND_STRING: {
        Str s = *(const Str *)p;
        jsonbuf_byte(b, 's');
        for (Int i = 0; i < s.len; i++)
            put_hex(b, s.p[i], 2);
        return;
    }
    case KIND_SLICE: {
        const Slice *s = (const Slice *)p;
        if (s->p == NULL) {
            jsonbuf_str(b, cstr("nil"));
            return;
        }
        jsonbuf_byte(b, '[');
        for (Int i = 0; i < s->len; i++) {
            if (i > 0)
                jsonbuf_byte(b, ',');
            dump(b, t->elem, (const Byte *)s->p + (size_t)i * t->elem->size, path);
        }
        jsonbuf_byte(b, ']');
        return;
    }
    case KIND_ARRAY:
        jsonbuf_byte(b, '[');
        for (uint32_t i = 0; i < t->len; i++) {
            if (i > 0)
                jsonbuf_byte(b, ',');
            dump(b, t->elem, (const Byte *)p + (size_t)i * t->elem->size, path);
        }
        jsonbuf_byte(b, ']');
        return;
    case KIND_POINTER: {
        const void *v = *(void *const *)p;
        if (v == NULL) {
            jsonbuf_str(b, cstr("nil"));
            return;
        }
        for (int i = 0; i < path->n; i++) {
            if (path->p[i] == v) {
                jsonbuf_str(b, cstr("cycle"));
                return;
            }
        }
        if (path->n == (int)LEN(path->p)) {
            jsonbuf_str(b, cstr("deep"));
            return;
        }
        path->p[path->n++] = v;
        jsonbuf_byte(b, '&');
        dump(b, t->elem, v, path);
        path->n--;
        return;
    }
    case KIND_MAP: {
        Map *m = *(Map *const *)p;
        if (m == NULL) {
            jsonbuf_str(b, cstr("nil"));
            return;
        }
        Int n = map_len(m);
        Str *ents = (Str *)mem_alloc(
            heap_allocator(), (size_t)(n > 0 ? n : 1) * sizeof(Str), _Alignof(Str));
        Int i = 0;
        MapIter it = map_iter(m);
        const void *k;
        void *v;
        while (i < n && map_next(&it, &k, &v)) {
            JsonBuf e = {NULL, 0, 0, heap_allocator(), true, false};
            dump(&e, t->key, k, path);
            jsonbuf_byte(&e, ':');
            dump(&e, t->elem, v, path);
            ents[i++] = (Str){e.p, e.len};
        }
        qsort(ents, (size_t)i, sizeof(Str), cmp_str);
        jsonbuf_str(b, cstr("map["));
        for (Int j = 0; j < i; j++) {
            if (j > 0)
                jsonbuf_byte(b, ',');
            jsonbuf_str(b, ents[j]);
            mem_free(heap_allocator(), (void *)(uintptr_t)ents[j].p,
                     (size_t)ents[j].len, 1);
        }
        jsonbuf_byte(b, ']');
        mem_free(heap_allocator(), ents, (size_t)(n > 0 ? n : 1) * sizeof(Str),
                 _Alignof(Str));
        return;
    }
    case KIND_STRUCT:
        jsonbuf_byte(b, '{');
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (i > 0)
                jsonbuf_byte(b, ',');
            jsonbuf_str(b, f->name);
            jsonbuf_byte(b, ':');
            dump(b, f->type, (const Byte *)p + f->offset, path);
        }
        jsonbuf_byte(b, '}');
        return;
    case KIND_INTERFACE: {
        const Any *a = (const Any *)p;
        if (a->t == NULL) {
            jsonbuf_str(b, cstr("nil"));
            return;
        }
        jsonbuf_byte(b, '(');
        burrow__jsonv2_put_type(b, a->t);
        jsonbuf_byte(b, ')');
        dump(b, a->t, a->data, path);
        return;
    }
    case KIND_CHAN:
    case KIND_FUNC:
        jsonbuf_str(b, *(void *const *)p == NULL ? cstr("nil") : cstr("?"));
        return;
    default:
        return;
    }
}

#endif /* BURROW_TESTS_JSON_DUMP_H */
