/* Derived from Go's src/internal/fuzz/encoding.go.
 * Go source: go1.27.1.
 *
 * The file format a fuzz corpus entry is kept in: a version line, then one
 * value per line, each written as the Go conversion that would make it, such
 * as int(-23) or []byte("hello"). Go reads a line with go/parser and then
 * looks at the tree. There is no go/parser here yet, so corpus_scan.c and
 * corpus_parse.c carry the part of it that ParseExprFrom reaches, errors and
 * all, and this file makes the same decisions Go makes about the tree.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "corpus.h"
#include "corpus_syntax.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

static const char corpus_version[] = "go test fuzz v1";

/* ------------------------------------------------------------------ values */

static void *corpus_must_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size, align);
    if (p == NULL)
        panic_str(BURROW_S("testing: out of memory"));
    return p;
}

Any burrow__testing_value_copy(const Type *t, const void *src) {
    Alloc *h = heap_allocator();
    void *dst = corpus_must_alloc(h, t->size, t->align);
    if (t == TYPE_STRING) {
        Str s = *(const Str *)src;
        Str d = {0};
        if (s.len > 0) {
            Byte *p = (Byte *)corpus_must_alloc(h, (size_t)s.len, 1);
            memcpy(p, s.p, (size_t)s.len);
            d = str_from_bytes(p, s.len);
        }
        *(Str *)dst = d;
    } else if (t == TYPE_BYTES) {
        const Slice *s = (const Slice *)src;
        Slice d = slice_nil(TYPE_UINT8);
        if (s->len > 0) {
            d = slice_make(h, TYPE_UINT8, s->len, s->len);
            memcpy(d.p, s->p, (size_t)s->len);
        }
        *(Slice *)dst = d;
    } else {
        memcpy(dst, src, t->size);
    }
    return (Any){t, dst};
}

void burrow__testing_value_free(Any v) {
    Alloc *h = heap_allocator();
    if (v.t == TYPE_STRING) {
        Str *s = (Str *)v.data;
        if (s->len > 0)
            mem_free(h, (void *)(uintptr_t)s->p, (size_t)s->len, 1);
    } else if (v.t == TYPE_BYTES) {
        Slice *s = (Slice *)v.data;
        if (s->p != NULL)
            mem_free(h, s->p, (size_t)s->cap, 1);
    }
    mem_free(h, v.data, v.t->size, v.t->align);
}

void burrow__testing_values_free(Any *vals, Int n) {
    for (Int i = 0; i < n; i++)
        burrow__testing_value_free(vals[i]);
    if (vals != NULL)
        mem_free(heap_allocator(), vals, (size_t)n * sizeof(Any), _Alignof(Any));
}

/* ----------------------------------------------------------------- marshal */

static uint32_t corpus_f32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static uint64_t corpus_f64_bits(double f) {
    uint64_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* The bits of math.NaN() and of float32(math.NaN()). A NaN with other bits is
 * written out as its bits so that a failure depending on them comes back. */
#define CORPUS_NAN64 0x7ff8000000000001ULL
#define CORPUS_NAN32 0x7fc00000U

static bool corpus_valid_rune(int32_t r) {
    return (r >= 0 && r < 0xd800) || (r > 0xdfff && r <= 0x10ffff);
}

/* marshalCorpusFile. */
Str burrow__testing_corpus_marshal(Alloc *a, const Any *vals, Int n) {
    if (n == 0)
        panic_str(BURROW_S("must have at least one value to marshal"));
    Str out = fmt_sprintf_v(a, "%s\n", corpus_version);
    for (Int i = 0; i < n; i++) {
        Any v = vals[i];
        const Type *t = v.t;
        Str line;
        if (t == TYPE_INT || t == TYPE_INT8 || t == TYPE_INT16 || t == TYPE_INT64 ||
            t == TYPE_UINT || t == TYPE_UINT16 || t == TYPE_UINT32 ||
            t == TYPE_UINT64 || t == TYPE_BOOL) {
            line = fmt_sprintf_v(a, "%T(%v)\n", v, v);
        } else if (t == TYPE_FLOAT32) {
            float f = *(const float *)v.data;
            uint32_t bits = corpus_f32_bits(f);
            if (f != f && bits != CORPUS_NAN32)
                line = fmt_sprintf_v(a, "math.Float32frombits(0x%x)\n", bits);
            else
                line = fmt_sprintf_v(a, "%T(%v)\n", v, v);
        } else if (t == TYPE_FLOAT64) {
            double f = *(const double *)v.data;
            uint64_t bits = corpus_f64_bits(f);
            if (f != f && bits != CORPUS_NAN64)
                line = fmt_sprintf_v(a, "math.Float64frombits(0x%x)\n", bits);
            else
                line = fmt_sprintf_v(a, "%T(%v)\n", v, v);
        } else if (t == TYPE_STRING) {
            line = fmt_sprintf_v(a, "string(%q)\n", v);
        } else if (t == TYPE_INT32) {
            /* Not every int32 has a rune literal, and %q would turn the ones
             * that do not into U+FFFD, so those are written as numbers. */
            if (corpus_valid_rune(*(const int32_t *)v.data))
                line = fmt_sprintf_v(a, "rune(%q)\n", v);
            else
                line = fmt_sprintf_v(a, "int32(%v)\n", v);
        } else if (t == TYPE_UINT8) {
            line = fmt_sprintf_v(a, "byte(%q)\n", v);
        } else if (t == TYPE_BYTES) {
            line = fmt_sprintf_v(a, "[]byte(%q)\n", v);
        } else {
            panic_str(fmt_sprintf_v(a, "unsupported type: %T", v));
        }
        out = fmt_sprintf_v(a, "%s%s", out, line);
    }
    return out;
}

/* ------------------------------------------------------------------ values */

static Str corpus_errorf(Alloc *a, const char *msg) {
    return str_clone(a, str_from_cstr(msg));
}

static Any corpus_box(const Type *t, const void *v) {
    return burrow__testing_value_copy(t, v);
}

/* parseInt, with int wrapping to the width of Int as Go's does. */
static bool corpus_parse_int(Alloc *a, Str val, const char *typ, Any *out, Str *err) {
    Error e = {0};
    if (strcmp(typ, "int") == 0) {
        Int v = (Int)strconv_parse_int(val, 0, 64, &e);
        if (!BURROW_OK(e))
            goto fail;
        *out = corpus_box(TYPE_INT, &v);
    } else if (strcmp(typ, "int8") == 0) {
        int8_t v = (int8_t)strconv_parse_int(val, 0, 8, &e);
        if (!BURROW_OK(e))
            goto fail;
        *out = corpus_box(TYPE_INT8, &v);
    } else if (strcmp(typ, "int16") == 0) {
        int16_t v = (int16_t)strconv_parse_int(val, 0, 16, &e);
        if (!BURROW_OK(e))
            goto fail;
        *out = corpus_box(TYPE_INT16, &v);
    } else if (strcmp(typ, "int32") == 0 || strcmp(typ, "rune") == 0) {
        int32_t v = (int32_t)strconv_parse_int(val, 0, 32, &e);
        if (!BURROW_OK(e))
            goto fail;
        *out = corpus_box(TYPE_INT32, &v);
    } else {
        int64_t v = strconv_parse_int(val, 0, 64, &e);
        if (!BURROW_OK(e))
            goto fail;
        *out = corpus_box(TYPE_INT64, &v);
    }
    return true;
fail:
    *err = str_clone(a, error_text(e));
    return false;
}

static bool corpus_parse_uint_bits(Alloc *a, Str val, Int bits, uint64_t *out,
                                   Str *err) {
    Error e = {0};
    *out = strconv_parse_uint(val, 0, bits, &e);
    if (!BURROW_OK(e)) {
        *err = str_clone(a, error_text(e));
        return false;
    }
    return true;
}

/* parseUint. */
static bool corpus_parse_uint(Alloc *a, Str val, const char *typ, Any *out, Str *err) {
    uint64_t v;
    if (strcmp(typ, "uint") == 0) {
        if (!corpus_parse_uint_bits(a, val, 64, &v, err))
            return false;
        Uint u = (Uint)v;
        *out = corpus_box(TYPE_UINT, &u);
    } else if (strcmp(typ, "uint8") == 0 || strcmp(typ, "byte") == 0) {
        if (!corpus_parse_uint_bits(a, val, 8, &v, err))
            return false;
        uint8_t u = (uint8_t)v;
        *out = corpus_box(TYPE_UINT8, &u);
    } else if (strcmp(typ, "uint16") == 0) {
        if (!corpus_parse_uint_bits(a, val, 16, &v, err))
            return false;
        uint16_t u = (uint16_t)v;
        *out = corpus_box(TYPE_UINT16, &u);
    } else if (strcmp(typ, "uint32") == 0) {
        if (!corpus_parse_uint_bits(a, val, 32, &v, err))
            return false;
        uint32_t u = (uint32_t)v;
        *out = corpus_box(TYPE_UINT32, &u);
    } else {
        if (!corpus_parse_uint_bits(a, val, 64, &v, err))
            return false;
        *out = corpus_box(TYPE_UINT64, &v);
    }
    return true;
}

static bool corpus_unquote(Alloc *a, Str lit, Str *out, Str *err) {
    Error e = {0};
    *out = strconv_unquote(a, lit, &e);
    if (!BURROW_OK(e)) {
        *err = str_clone(a, error_text(e));
        return false;
    }
    return true;
}

/* parseCorpusValue, over the tree the parser above built. */
static bool corpus_value(Alloc *a, const CorpusNode *call, Any *out, Str *err) {
    if (call->kind != CN_CALL) {
        *err = corpus_errorf(a, "expected call expression");
        return false;
    }
    if (call->nargs != 1) {
        *err = fmt_sprintf_v(a, "expected call expression with 1 argument; got %d",
                             call->nargs);
        return false;
    }
    const CorpusNode *fun = call->x;
    const CorpusNode *arg = call->args[0];

    if (fun->kind == CN_ARRAY_TYPE) {
        if (fun->y != NULL) {
            *err = corpus_errorf(a, "expected []byte or primitive type");
            return false;
        }
        const CorpusNode *elt = fun->x;
        if (elt->kind != CN_IDENT || !str_eq(elt->name, BURROW_S("byte"))) {
            *err = corpus_errorf(a, "expected []byte");
            return false;
        }
        if (arg->kind != CN_BASIC_LIT || arg->tok != CT_STRING) {
            *err = corpus_errorf(a, "string literal required for type []byte");
            return false;
        }
        Str s;
        if (!corpus_unquote(a, arg->name, &s, err))
            return false;
        Slice b = slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_UINT8);
        *out = corpus_box(TYPE_BYTES, &b);
        return true;
    }

    const char *typ;
    char name[16];
    if (fun->kind == CN_SELECTOR) {
        const CorpusNode *x = fun->x;
        if (x->kind != CN_IDENT || !str_eq(x->name, BURROW_S("math"))) {
            *err = corpus_errorf(a, "invalid selector type");
            return false;
        }
        if (str_eq(fun->y->name, BURROW_S("Float64frombits"))) {
            typ = "float64-bits";
        } else if (str_eq(fun->y->name, BURROW_S("Float32frombits"))) {
            typ = "float32-bits";
        } else {
            *err = corpus_errorf(a, "invalid selector type");
            return false;
        }
    } else {
        if (fun->kind != CN_IDENT) {
            *err = corpus_errorf(a, "expected []byte or primitive type");
            return false;
        }
        if (str_eq(fun->name, BURROW_S("bool"))) {
            if (arg->kind != CN_IDENT) {
                *err = corpus_errorf(a, "malformed bool");
                return false;
            }
            bool v;
            if (str_eq(arg->name, BURROW_S("true"))) {
                v = true;
            } else if (str_eq(arg->name, BURROW_S("false"))) {
                v = false;
            } else {
                *err = corpus_errorf(a, "true or false required for type bool");
                return false;
            }
            *out = corpus_box(TYPE_BOOL, &v);
            return true;
        }
        /* Anything longer than the longest name below is none of them. */
        if (fun->name.len >= (Int)sizeof name) {
            *err = corpus_errorf(a, "expected []byte or primitive type");
            return false;
        }
        memcpy(name, fun->name.p, (size_t)fun->name.len);
        name[fun->name.len] = '\0';
        typ = name;
    }

    Str val;
    CorpusTok kind;
    if (arg->kind == CN_UNARY) {
        const CorpusNode *x = arg->x;
        if (x->kind == CN_BASIC_LIT) {
            if (arg->tok != CT_SUB) {
                *err = fmt_sprintf_v(a, "unsupported operation on int/float: %s",
                                     burrow__testing_corpus_tok_string(arg->tok));
                return false;
            }
            val = fmt_sprintf_v(a, "-%s", x->name);
            kind = x->tok;
        } else if (x->kind == CN_IDENT) {
            if (!str_eq(x->name, BURROW_S("Inf"))) {
                *err = corpus_errorf(a, "expected operation on int or float type");
                return false;
            }
            val = arg->tok == CT_SUB ? BURROW_S("-Inf") : BURROW_S("+Inf");
            kind = CT_FLOAT;
        } else {
            *err = corpus_errorf(a, "expected operation on int or float type");
            return false;
        }
    } else if (arg->kind == CN_BASIC_LIT) {
        val = arg->name;
        kind = arg->tok;
    } else if (arg->kind == CN_IDENT) {
        if (!str_eq(arg->name, BURROW_S("NaN"))) {
            *err = corpus_errorf(a, "literal value required for primitive type");
            return false;
        }
        val = BURROW_S("NaN");
        kind = CT_FLOAT;
    } else {
        *err = corpus_errorf(a, "literal value required for primitive type");
        return false;
    }

    if (strcmp(typ, "string") == 0) {
        if (kind != CT_STRING) {
            *err = corpus_errorf(a, "string literal value required for type string");
            return false;
        }
        Str s;
        if (!corpus_unquote(a, val, &s, err))
            return false;
        *out = corpus_box(TYPE_STRING, &s);
        return true;
    }
    if (strcmp(typ, "byte") == 0 || strcmp(typ, "rune") == 0) {
        bool is_rune = typ[0] == 'r';
        if (kind == CT_INT)
            return is_rune ? corpus_parse_int(a, val, typ, out, err)
                           : corpus_parse_uint(a, val, typ, out, err);
        if (kind != CT_CHAR) {
            *err = corpus_errorf(a, "character literal required for byte/rune types");
            return false;
        }
        if (val.len < 2) {
            *err =
                corpus_errorf(a, "malformed character literal, missing single quotes");
            return false;
        }
        Error e = {0};
        bool multibyte;
        Str tail;
        Rune code = strconv_unquote_char(str_from_bytes(val.p + 1, val.len - 2), '\'',
                                         &multibyte, &tail, &e);
        if (!BURROW_OK(e)) {
            *err = str_clone(a, error_text(e));
            return false;
        }
        if (is_rune) {
            int32_t r = code;
            *out = corpus_box(TYPE_INT32, &r);
            return true;
        }
        if (code >= 256) {
            *err = corpus_errorf(a, "can only encode single byte to a byte type");
            return false;
        }
        uint8_t b = (uint8_t)code;
        *out = corpus_box(TYPE_UINT8, &b);
        return true;
    }
    if (strcmp(typ, "int") == 0 || strcmp(typ, "int8") == 0 ||
        strcmp(typ, "int16") == 0 || strcmp(typ, "int32") == 0 ||
        strcmp(typ, "int64") == 0) {
        if (kind != CT_INT) {
            *err = corpus_errorf(a, "integer literal required for int types");
            return false;
        }
        return corpus_parse_int(a, val, typ, out, err);
    }
    if (strcmp(typ, "uint") == 0 || strcmp(typ, "uint8") == 0 ||
        strcmp(typ, "uint16") == 0 || strcmp(typ, "uint32") == 0 ||
        strcmp(typ, "uint64") == 0) {
        if (kind != CT_INT) {
            *err = corpus_errorf(a, "integer literal required for uint types");
            return false;
        }
        return corpus_parse_uint(a, val, typ, out, err);
    }
    if (strcmp(typ, "float32") == 0 || strcmp(typ, "float64") == 0) {
        bool f32 = typ[5] == '3';
        if (kind != CT_FLOAT && kind != CT_INT) {
            *err = corpus_errorf(
                a, f32 ? "float or integer literal required for float32 type"
                       : "float or integer literal required for float64 type");
            return false;
        }
        Error e = {0};
        double v = strconv_parse_float(val, f32 ? 32 : 64, &e);
        if (!BURROW_OK(e)) {
            *err = str_clone(a, error_text(e));
            return false;
        }
        if (f32) {
            float f = (float)v;
            *out = corpus_box(TYPE_FLOAT32, &f);
        } else {
            *out = corpus_box(TYPE_FLOAT64, &v);
        }
        return true;
    }
    if (strcmp(typ, "float32-bits") == 0) {
        if (kind != CT_INT) {
            *err = corpus_errorf(
                a, "integer literal required for math.Float32frombits type");
            return false;
        }
        uint64_t bits;
        if (!corpus_parse_uint_bits(a, val, 32, &bits, err))
            return false;
        uint32_t b32 = (uint32_t)bits;
        float f;
        memcpy(&f, &b32, sizeof f);
        *out = corpus_box(TYPE_FLOAT32, &f);
        return true;
    }
    if (strcmp(typ, "float64-bits") == 0) {
        /* FLOAT is let through here and then fails in the parse, as in Go. */
        if (kind != CT_FLOAT && kind != CT_INT) {
            *err = corpus_errorf(
                a, "integer literal required for math.Float64frombits type");
            return false;
        }
        uint64_t bits;
        if (!corpus_parse_uint_bits(a, val, 64, &bits, err))
            return false;
        double f;
        memcpy(&f, &bits, sizeof f);
        *out = corpus_box(TYPE_FLOAT64, &f);
        return true;
    }
    *err = corpus_errorf(a, "expected []byte or primitive type");
    return false;
}

/* ParseExprFrom followed by parseCorpusValue. The tree lives in an arena
 * that goes when the line is done with, so the error text is copied out. */
static bool corpus_line(Alloc *a, Str line, Any *out, Str *err) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *tmp = arena_allocator(&ar);
    Str why;
    CorpusNode *expr = burrow__testing_corpus_parse_expr(tmp, line, &why);
    bool ok = expr != NULL && corpus_value(tmp, expr, out, &why);
    if (!ok)
        *err = str_clone(a, why);
    arena_free(&ar);
    return ok;
}

static bool corpus_is_space(Byte c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' ||
           c == 0x85 || c == 0xa0;
}

/* bytes.TrimSpace. The two Latin-1 spaces count only as the runes U+0085 and
 * U+00A0, so they are looked for in their UTF-8 form. */
static Str corpus_trim(Str s) {
    for (;;) {
        if (s.len > 0 && s.p[0] < 0x80 && corpus_is_space(s.p[0])) {
            s = str_from_bytes(s.p + 1, s.len - 1);
        } else if (s.len > 1 && s.p[0] == 0xc2 && (s.p[1] == 0x85 || s.p[1] == 0xa0)) {
            s = str_from_bytes(s.p + 2, s.len - 2);
        } else {
            break;
        }
    }
    for (;;) {
        if (s.len > 0 && s.p[s.len - 1] < 0x80 && corpus_is_space(s.p[s.len - 1])) {
            s.len--;
        } else if (s.len > 1 && s.p[s.len - 2] == 0xc2 &&
                   (s.p[s.len - 1] == 0x85 || s.p[s.len - 1] == 0xa0)) {
            s.len -= 2;
        } else {
            break;
        }
    }
    return s;
}

/* unmarshalCorpusFile. */
bool burrow__testing_corpus_unmarshal(Alloc *a, Str data, Any **out, Int *n, Str *err) {
    *out = NULL;
    *n = 0;
    if (data.len == 0) {
        *err = corpus_errorf(a, "cannot unmarshal empty string");
        return false;
    }
    const Byte *nl = memchr(data.p, '\n', (size_t)data.len);
    if (nl == NULL) {
        *err = corpus_errorf(a, "must include version and at least one value");
        return false;
    }
    Str version = str_from_bytes(data.p, nl - data.p);
    if (version.len > 0 && version.p[version.len - 1] == '\r')
        version.len--;
    if (!str_eq(version, str_from_cstr(corpus_version))) {
        *err = fmt_sprintf_v(a, "unknown encoding version: %s", version);
        return false;
    }

    Alloc *h = heap_allocator();
    Any *vals = NULL;
    Int len = 0, cap = 0;
    const Byte *p = nl + 1;
    const Byte *end = data.p + data.len;
    for (;;) {
        const Byte *e = memchr(p, '\n', (size_t)(end - p));
        if (e == NULL)
            e = end;
        Str line = corpus_trim(str_from_bytes(p, e - p));
        if (line.len > 0) {
            Any v;
            Str why;
            if (!corpus_line(a, line, &v, &why)) {
                for (Int i = 0; i < len; i++)
                    burrow__testing_value_free(vals[i]);
                if (vals != NULL)
                    mem_free(h, vals, (size_t)cap * sizeof(Any), _Alignof(Any));
                *err = fmt_sprintf_v(a, "malformed line %q: %s", line, why);
                return false;
            }
            if (len == cap) {
                Int ncap = cap == 0 ? 4 : cap * 2;
                Any *nv = (Any *)mem_realloc(h, vals, (size_t)cap * sizeof(Any),
                                             (size_t)ncap * sizeof(Any), _Alignof(Any));
                if (nv == NULL)
                    panic_str(BURROW_S("testing: out of memory"));
                vals = nv;
                cap = ncap;
            }
            vals[len++] = v;
        }
        if (e == end)
            break;
        p = e + 1;
    }
    /* Trimmed to the length so that the caller frees what it was given. */
    if (len < cap) {
        Any *nv = len == 0
                      ? NULL
                      : (Any *)mem_realloc(h, vals, (size_t)cap * sizeof(Any),
                                           (size_t)len * sizeof(Any), _Alignof(Any));
        if (len == 0)
            mem_free(h, vals, (size_t)cap * sizeof(Any), _Alignof(Any));
        else if (nv == NULL)
            panic_str(BURROW_S("testing: out of memory"));
        vals = nv;
    }
    *out = vals;
    *n = len;
    return true;
}
