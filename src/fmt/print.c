/* Derived from Go's src/fmt/print.go and src/internal/fmtsort/sort.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file.
 *
 * Where Go asks reflect about a value, this asks the value's descriptor, so
 * the walk is the same one: a struct is its fields, a pointer at the top level
 * is & and what it points at, and a method is found by name. Three things
 * differ because C does.
 *
 * A method is looked up in the descriptor's method list, and every method
 * there takes a pointer receiver, so the method set of T and of *T are the
 * same set. Go would skip a pointer method when handed a T.
 *
 * A method on a nil pointer is not called. Go calls it and prints <nil> when it
 * panics, which in C would be a crash rather than a panic, so the <nil> is
 * printed without making the call.
 *
 * An error is a vtable rather than a pointer to a type, so what %T and %#v show
 * for one comes from its self_type when it has one, and is Go's answer for
 * errors.New when it does not. */

#include "burrow/fmt.h"

#include "format.h"

#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdio.h>
#include <string.h>

#define LIT(lit) BURROW_S(lit)

/* Descriptors hand out const pointers to values the walk only reads, and the
 * walk passes them on as void * because method thunks take one. */
static void *unconst(const void *p) {
    return (void *)(uintptr_t)p;
}

static const Str comma_space = BURROW_S_INIT(", ");
static const Str nil_angle = BURROW_S_INIT("<nil>");
static const Str nil_paren = BURROW_S_INIT("(nil)");
static const Str percent_bang = BURROW_S_INIT("%!");

/* ------------------------------------------------------------------ printer */

typedef struct Pp {
    FmtBuf buf;
    Fmt fmt;

    /* Whether the format used argument indexes like [3]. */
    bool reordered;
    /* Whether the most recent index was a good one. */
    bool good_arg_num;
    /* Set while a panic from a method is being printed, so that a second one
     * is not caught again. */
    bool panicking;
    /* Set while printing a bad verb, so that the argument is printed plainly
     * and cannot recurse into the same method that just failed. */
    bool erroring;
    /* Whether %w is allowed, which is only inside fmt_errorf. */
    bool wrap_errs;

    /* The argument indexes of the %w verbs, for fmt_errorf. */
    Int *wrapped;
    Int nwrapped;
    Int capwrapped;
    Int wrapped_inline[4];
} Pp;

static void pp_init(Pp *p, Byte *stack, Int cap) {
    memset(p, 0, sizeof *p);
    p->buf.p = stack;
    p->buf.cap = cap;
    p->fmt.buf = &p->buf;
    p->wrapped = p->wrapped_inline;
    p->capwrapped = (Int)(sizeof p->wrapped_inline / sizeof p->wrapped_inline[0]);
}

static void pp_free(Pp *p) {
    burrow__fmt_buf_free(&p->buf);
    if (p->wrapped != p->wrapped_inline)
        mem_free(heap_allocator(), p->wrapped, (size_t)p->capwrapped * sizeof(Int),
                 _Alignof(Int));
}

static void pp_add_wrapped(Pp *p, Int arg_num) {
    if (p->nwrapped == p->capwrapped) {
        Int cap = p->capwrapped * 2;
        Int *w = (Int *)mem_alloc_nozero(heap_allocator(), (size_t)cap * sizeof(Int),
                                         _Alignof(Int));
        if (w == NULL)
            return;
        memcpy(w, p->wrapped, (size_t)p->nwrapped * sizeof(Int));
        if (p->wrapped != p->wrapped_inline)
            mem_free(heap_allocator(), p->wrapped, (size_t)p->capwrapped * sizeof(Int),
                     _Alignof(Int));
        p->wrapped = w;
        p->capwrapped = cap;
    }
    p->wrapped[p->nwrapped++] = arg_num;
}

static void write_str(Pp *p, Str s) {
    fmt_buf_write_str(&p->buf, s);
}

static void write_byte(Pp *p, Byte c) {
    fmt_buf_write_byte(&p->buf, c);
}

static void write_rune(Pp *p, Rune r) {
    burrow__fmt_buf_write_rune(&p->buf, r);
}

/* ------------------------------------------------------------------ State */

static Int state_write(void *self, Slice b, Error *err) {
    Pp *p = (Pp *)self;
    fmt_buf_write(&p->buf, (const Byte *)b.p, b.len);
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    return b.len;
}

static Int state_width(void *self, bool *ok) {
    Pp *p = (Pp *)self;
    if (ok != NULL)
        *ok = p->fmt.f.wid_present;
    return p->fmt.wid;
}

static Int state_precision(void *self, bool *ok) {
    Pp *p = (Pp *)self;
    if (ok != NULL)
        *ok = p->fmt.f.prec_present;
    return p->fmt.prec;
}

static bool state_flag(void *self, Int c) {
    Pp *p = (Pp *)self;
    switch (c) {
    case '-':
        return p->fmt.f.minus;
    case '+':
        return p->fmt.f.plus || p->fmt.f.plus_v;
    case '#':
        return p->fmt.f.sharp || p->fmt.f.sharp_v;
    case ' ':
        return p->fmt.f.space;
    case '0':
        return p->fmt.f.zero;
    default:
        return false;
    }
}

static const FmtStateVT pp_state_vt = {
    NULL, state_write, state_width, state_precision, state_flag,
};

const Type burrow_type_FmtState = {
    BURROW_S_INIT("State"),
    BURROW_S_INIT("fmt"),
    KIND_INTERFACE,
    (uint32_t)sizeof(FmtState),
    (uint16_t)_Alignof(FmtState),
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

static Int state_writer_write(void *self, Slice b, Error *err) {
    FmtState *st = (FmtState *)self;
    return st->vt->write(st->data, b, err);
}

static const IoWriterVT state_writer_vt = {NULL, state_writer_write};

IoWriter fmt_state_writer(FmtState *st) {
    return (IoWriter){&state_writer_vt, st};
}

Str fmt_format_string(Alloc *a, FmtState state, Rune verb) {
    Byte tmp[64];
    Int n = 0;
    tmp[n++] = '%';
    static const char flags[] = " +-#0";
    for (const char *c = flags; *c != '\0'; c++)
        if (state.vt->flag(state.data, *c))
            tmp[n++] = (Byte)*c;
    Slice b = slice_from(tmp, n, (Int)sizeof tmp, TYPE_BYTE);
    bool ok = false;
    Int w = state.vt->width(state.data, &ok);
    if (ok)
        b = strconv_append_int(a, b, (int64_t)w, 10);
    Int prec = state.vt->precision(state.data, &ok);
    if (ok) {
        b = slice_append(a, b, ".", 1);
        b = strconv_append_int(a, b, (int64_t)prec, 10);
    }
    b = utf8_append_rune(a, b, verb);
    if (b.p == (void *)tmp)
        return str_from_slice(a, b);
    return str_from_bytes((const Byte *)b.p, b.len);
}

/* ------------------------------------------------------------------ types */

static void write_type(FmtBuf *b, const Type *t);

/* A named type is its package name, which is the last element of its path, a
 * dot and its name. Everything else is spelled out from its parts. */
static void write_type(FmtBuf *b, const Type *t) {
    if (t == NULL) {
        fmt_buf_write_str(b, LIT("<nil>"));
        return;
    }
    if (t->name.len > 0) {
        if (t->pkg_path.len > 0) {
            Int i = t->pkg_path.len;
            while (i > 0 && t->pkg_path.p[i - 1] != '/')
                i--;
            fmt_buf_write(b, t->pkg_path.p + i, t->pkg_path.len - i);
            fmt_buf_write_byte(b, '.');
        }
        fmt_buf_write_str(b, t->name);
        return;
    }
    switch ((int)t->kind) {
    case KIND_SLICE:
        fmt_buf_write_str(b, LIT("[]"));
        write_type(b, t->elem);
        return;
    case KIND_ARRAY: {
        Byte num[24];
        Int n = 0;
        uint32_t len = t->len;
        do {
            num[n++] = (Byte)('0' + len % 10);
            len /= 10;
        } while (len > 0);
        fmt_buf_write_byte(b, '[');
        while (n > 0)
            fmt_buf_write_byte(b, num[--n]);
        fmt_buf_write_byte(b, ']');
        write_type(b, t->elem);
        return;
    }
    case KIND_POINTER:
        fmt_buf_write_byte(b, '*');
        write_type(b, t->elem);
        return;
    case KIND_MAP:
        fmt_buf_write_str(b, LIT("map["));
        write_type(b, t->key);
        fmt_buf_write_byte(b, ']');
        write_type(b, t->elem);
        return;
    case KIND_CHAN:
        fmt_buf_write_str(b, LIT("chan "));
        write_type(b, t->elem);
        return;
    case KIND_FUNC: {
        Int nin = type_num_in(t), nout = type_num_out(t);
        fmt_buf_write_str(b, LIT("func("));
        for (Int i = 0; i < nin; i++) {
            if (i > 0)
                fmt_buf_write_str(b, LIT(", "));
            write_type(b, type_in(t, i));
        }
        fmt_buf_write_byte(b, ')');
        if (nout == 1) {
            fmt_buf_write_byte(b, ' ');
            write_type(b, type_out(t, 0));
        } else if (nout > 1) {
            fmt_buf_write_str(b, LIT(" ("));
            for (Int i = 0; i < nout; i++) {
                if (i > 0)
                    fmt_buf_write_str(b, LIT(", "));
                write_type(b, type_out(t, i));
            }
            fmt_buf_write_byte(b, ')');
        }
        return;
    }
    case KIND_STRUCT:
        if (t->nfield == 0) {
            fmt_buf_write_str(b, LIT("struct {}"));
            return;
        }
        fmt_buf_write_str(b, LIT("struct {"));
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (i > 0)
                fmt_buf_write_byte(b, ';');
            fmt_buf_write_byte(b, ' ');
            if (!field_is_embedded(f)) {
                fmt_buf_write_str(b, f->name);
                fmt_buf_write_byte(b, ' ');
            }
            write_type(b, f->type);
            if (f->tag.len > 0) {
                Byte tmp[128];
                Slice q = strconv_append_quote(
                    heap_allocator(), slice_from(tmp, 0, (Int)sizeof tmp, TYPE_BYTE),
                    f->tag);
                fmt_buf_write_byte(b, ' ');
                fmt_buf_write(b, (const Byte *)q.p, q.len);
                if (q.p != (void *)tmp)
                    mem_free(heap_allocator(), q.p, (size_t)q.cap, 1);
            }
        }
        fmt_buf_write_str(b, LIT(" }"));
        return;
    case KIND_INTERFACE:
        fmt_buf_write_str(b, LIT("interface {}"));
        return;
    default:
        fmt_buf_write_str(b, kind_name(t->kind));
        return;
    }
}

/* What Go's reflect would call the type of an error, which is a pointer to the
 * struct behind it, or *errors.errorString for one from errors_new. */
static void write_error_type(FmtBuf *b, const Error *e) {
    const Type *self = e->vt->self_type;
    if (self == NULL) {
        fmt_buf_write_str(b, LIT("*errors.errorString"));
        return;
    }
    if (self->kind == KIND_STRUCT)
        fmt_buf_write_byte(b, '*');
    write_type(b, self);
}

static void write_arg_type(FmtBuf *b, const Type *t, const void *d) {
    if (t == TYPE_ERROR)
        write_error_type(b, (const Error *)d);
    else
        write_type(b, t);
}

/* The type as a Str, in a buffer the caller hands in and then frees. */
static Str arg_type_string(FmtBuf *b, Byte *stack, Int cap, const Type *t,
                           const void *d) {
    memset(b, 0, sizeof *b);
    b->p = stack;
    b->cap = cap;
    write_arg_type(b, t, d);
    return (Str){b->p, b->len};
}

/* ----------------------------------------------------------------- values */

/* What an interface value holds, looked through: an Any's value, a non-nil
 * error as itself, any other interface as its concrete type when its vtable
 * says what that is. A nil comes back as a NULL type. */
static void unwrap(const Type **tp, void **dp) {
    for (;;) {
        const Type *t = *tp;
        if (t == NULL)
            return;
        if (t == TYPE_ANY) {
            Any a = *(const Any *)*dp;
            *tp = a.t;
            *dp = a.data;
            continue;
        }
        if (t == TYPE_ERROR) {
            if (((const Error *)*dp)->vt == NULL)
                *tp = NULL;
            return;
        }
        if (t->kind == KIND_INTERFACE) {
            Iface v = *(const Iface *)*dp;
            if (v.vt == NULL) {
                *tp = NULL;
            } else if (v.vt->self_type != NULL) {
                *tp = v.vt->self_type;
                *dp = v.data;
            }
        }
        return;
    }
}

static int64_t value_int(const Type *t, const void *d) {
    switch ((int)t->kind) {
    case KIND_INT8:
        return *(const int8_t *)d;
    case KIND_INT16:
        return *(const int16_t *)d;
    case KIND_INT32:
        return *(const int32_t *)d;
    case KIND_INT64:
        return *(const int64_t *)d;
    default:
        return t->size == 4 ? *(const int32_t *)d : *(const int64_t *)d;
    }
}

static uint64_t value_uint(const Type *t, const void *d) {
    switch ((int)t->kind) {
    case KIND_UINT8:
        return *(const uint8_t *)d;
    case KIND_UINT16:
        return *(const uint16_t *)d;
    case KIND_UINT32:
        return *(const uint32_t *)d;
    case KIND_UINT64:
        return *(const uint64_t *)d;
    default:
        return t->size == 4 ? *(const uint32_t *)d : *(const uint64_t *)d;
    }
}

static double value_float(const Type *t, const void *d) {
    if (t->kind == KIND_FLOAT32)
        return (double)*(const float *)d;
    return *(const double *)d;
}

static Complex128 value_complex(const Type *t, const void *d) {
    if (t->kind == KIND_COMPLEX64) {
        const Complex64 *c = (const Complex64 *)d;
        return (Complex128){(double)c->re, (double)c->im};
    }
    return *(const Complex128 *)d;
}

static bool kind_is_int(Kind k) {
    return k >= KIND_INT && k <= KIND_INT64;
}

static bool kind_is_uint(Kind k) {
    return k >= KIND_UINT && k <= KIND_UINTPTR;
}

/* The address a pointer shaped value holds, and whether it has one. */
static bool value_pointer(const Type *t, const void *d, uint64_t *u) {
    uintptr_t v = 0;
    switch ((int)t->kind) {
    case KIND_CHAN:
    case KIND_FUNC:
    case KIND_MAP:
    case KIND_POINTER:
    case KIND_UNSAFE_POINTER:
        /* A function value's first word is the function, and every other one
         * of these is a single pointer. */
        memcpy(&v, d, sizeof v);
        break;
    case KIND_SLICE:
        v = (uintptr_t)((const Slice *)d)->p;
        break;
    case KIND_INTERFACE:
        if (t != TYPE_ERROR)
            return false;
        v = (uintptr_t)((const Error *)d)->data;
        break;
    default:
        return false;
    }
    *u = (uint64_t)v;
    return true;
}

/* ------------------------------------------------------------------ verbs */

static void print_arg(Pp *p, const Type *t, void *d, Rune verb);
static void print_value(Pp *p, const Type *t, void *d, Rune verb, Int depth, bool can);

static void bad_verb(Pp *p, const Type *t, void *d, Rune verb, bool is_value) {
    p->erroring = true;
    write_str(p, percent_bang);
    write_rune(p, verb);
    write_byte(p, '(');
    if (t != NULL) {
        write_arg_type(&p->buf, t, d);
        write_byte(p, '=');
        if (is_value)
            print_value(p, t, d, 'v', 0, true);
        else
            print_arg(p, t, d, 'v');
    } else {
        write_str(p, nil_angle);
    }
    write_byte(p, ')');
    p->erroring = false;
}

static void fmt_bool(Pp *p, const Type *t, void *d, bool v, Rune verb, bool is_value) {
    switch (verb) {
    case 't':
    case 'v':
        burrow__fmt_boolean(&p->fmt, v);
        break;
    default:
        bad_verb(p, t, d, verb, is_value);
    }
}

/* %#x, or %x, of an unsigned value, whatever the sharp flag says. */
static void fmt_0x64(Pp *p, uint64_t v, bool leading0x) {
    bool sharp = p->fmt.f.sharp;
    p->fmt.f.sharp = leading0x;
    burrow__fmt_integer(&p->fmt, v, 16, false, 'v', FMT_LDIGITS);
    p->fmt.f.sharp = sharp;
}

static void fmt_integer(Pp *p, const Type *t, void *d, uint64_t v, bool is_signed,
                        Rune verb, bool is_value) {
    Fmt *f = &p->fmt;
    switch (verb) {
    case 'v':
        if (f->f.sharp_v && !is_signed)
            fmt_0x64(p, v, true);
        else
            burrow__fmt_integer(f, v, 10, is_signed, verb, FMT_LDIGITS);
        break;
    case 'd':
        burrow__fmt_integer(f, v, 10, is_signed, verb, FMT_LDIGITS);
        break;
    case 'b':
        burrow__fmt_integer(f, v, 2, is_signed, verb, FMT_LDIGITS);
        break;
    case 'o':
    case 'O':
        burrow__fmt_integer(f, v, 8, is_signed, verb, FMT_LDIGITS);
        break;
    case 'x':
        burrow__fmt_integer(f, v, 16, is_signed, verb, FMT_LDIGITS);
        break;
    case 'X':
        burrow__fmt_integer(f, v, 16, is_signed, verb, FMT_UDIGITS);
        break;
    case 'c':
        burrow__fmt_c(f, v);
        break;
    case 'q':
        burrow__fmt_qc(f, v);
        break;
    case 'U':
        burrow__fmt_unicode(f, v);
        break;
    default:
        bad_verb(p, t, d, verb, is_value);
    }
}

/* size is 32 or 64, and says which float the shortest form has to round trip
 * through, which is why a float32 prints as 0.1 and not 0.10000000149011612. */
static void fmt_float(Pp *p, const Type *t, void *d, double v, Int size, Rune verb,
                      bool is_value) {
    Fmt *f = &p->fmt;
    switch (verb) {
    case 'v':
        burrow__fmt_float(f, v, size, 'g', -1);
        break;
    case 'b':
    case 'g':
    case 'G':
    case 'x':
    case 'X':
        burrow__fmt_float(f, v, size, verb, -1);
        break;
    case 'f':
    case 'e':
    case 'E':
        burrow__fmt_float(f, v, size, verb, 6);
        break;
    case 'F':
        burrow__fmt_float(f, v, size, 'f', 6);
        break;
    default:
        bad_verb(p, t, d, verb, is_value);
    }
}

static void fmt_complex(Pp *p, const Type *t, void *d, Complex128 v, Int size,
                        Rune verb, bool is_value) {
    switch (verb) {
    case 'v':
    case 'b':
    case 'g':
    case 'G':
    case 'x':
    case 'X':
    case 'f':
    case 'F':
    case 'e':
    case 'E': {
        bool old_plus = p->fmt.f.plus;
        write_byte(p, '(');
        fmt_float(p, t, d, v.re, size / 2, verb, is_value);
        /* The imaginary part always has a sign. */
        p->fmt.f.plus = true;
        fmt_float(p, t, d, v.im, size / 2, verb, is_value);
        write_str(p, LIT("i)"));
        p->fmt.f.plus = old_plus;
        break;
    }
    default:
        bad_verb(p, t, d, verb, is_value);
    }
}

static void fmt_string(Pp *p, const Type *t, void *d, Str v, Rune verb, bool is_value) {
    Fmt *f = &p->fmt;
    switch (verb) {
    case 'v':
        if (f->f.sharp_v)
            burrow__fmt_q(f, v);
        else
            burrow__fmt_s(f, v);
        break;
    case 's':
        burrow__fmt_s(f, v);
        break;
    case 'x':
        burrow__fmt_sbx(f, v.p, v.len, FMT_LDIGITS);
        break;
    case 'X':
        burrow__fmt_sbx(f, v.p, v.len, FMT_UDIGITS);
        break;
    case 'q':
        burrow__fmt_q(f, v);
        break;
    default:
        bad_verb(p, t, d, verb, is_value);
    }
}

/* Go's []byte with no name, which is what a Slice of TYPE_BYTE comes in as. */
static const Type bytes_type = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_uint8_t,
    NULL,
    0,
    0,
    NULL,
};

static void fmt_bytes(Pp *p, const Byte *v, Int n, bool is_nil, Rune verb,
                      Str type_string, void *d) {
    Fmt *f = &p->fmt;
    switch (verb) {
    case 'v':
    case 'd':
        if (f->f.sharp_v) {
            write_str(p, type_string);
            if (is_nil) {
                write_str(p, nil_paren);
                return;
            }
            write_byte(p, '{');
            for (Int i = 0; i < n; i++) {
                if (i > 0)
                    write_str(p, comma_space);
                fmt_0x64(p, v[i], true);
            }
            write_byte(p, '}');
        } else {
            write_byte(p, '[');
            for (Int i = 0; i < n; i++) {
                if (i > 0)
                    write_byte(p, ' ');
                burrow__fmt_integer(f, v[i], 10, false, verb, FMT_LDIGITS);
            }
            write_byte(p, ']');
        }
        break;
    case 's':
        burrow__fmt_bs(f, v, n);
        break;
    case 'x':
        burrow__fmt_sbx(f, v, n, FMT_LDIGITS);
        break;
    case 'X':
        burrow__fmt_sbx(f, v, n, FMT_UDIGITS);
        break;
    case 'q':
        burrow__fmt_q(f, (Str){v, n});
        break;
    default:
        print_value(p, &bytes_type, d, verb, 0, true);
    }
}

static void fmt_pointer(Pp *p, const Type *t, void *d, Rune verb, bool is_value) {
    uint64_t u;
    if (!value_pointer(t, d, &u)) {
        bad_verb(p, t, d, verb, is_value);
        return;
    }
    switch (verb) {
    case 'v':
        if (p->fmt.f.sharp_v) {
            write_byte(p, '(');
            write_arg_type(&p->buf, t, d);
            write_str(p, LIT(")("));
            if (u == 0)
                write_str(p, LIT("nil"));
            else
                fmt_0x64(p, u, true);
            write_byte(p, ')');
        } else {
            if (u == 0)
                burrow__fmt_pad_string(&p->fmt, nil_angle);
            else
                fmt_0x64(p, u, !p->fmt.f.sharp);
        }
        break;
    case 'p':
        fmt_0x64(p, u, !p->fmt.f.sharp);
        break;
    case 'b':
    case 'o':
    case 'd':
    case 'x':
    case 'X':
        fmt_integer(p, t, d, u, false, verb, is_value);
        break;
    default:
        bad_verb(p, t, d, verb, is_value);
    }
}

/* ---------------------------------------------------------------- methods */

/* A method with Go's signature for String, Error or GoString. */
static bool is_string_method(const Method *m) {
    return m != NULL && m->ftype != NULL && type_num_in(m->ftype) == 0 &&
           type_num_out(m->ftype) == 1 && type_out(m->ftype, 0) == TYPE_STRING;
}

static bool is_format_method(const Method *m) {
    return m != NULL && m->ftype != NULL && type_num_in(m->ftype) == 2 &&
           type_num_out(m->ftype) == 0 && type_in(m->ftype, 0) == TYPE_FMT_STATE &&
           type_in(m->ftype, 1) != NULL && type_in(m->ftype, 1)->kind == KIND_INT32;
}

/* Where a value's methods are and what they are called on. For a pointer that
 * is the type it points at, called on what it points at. */
typedef struct Recv {
    const Type *t;
    void *recv;
} Recv;

static bool method_holder(const Type *t, void *d, Recv *r) {
    if (t == TYPE_ERROR) {
        const Error *e = (const Error *)d;
        if (e->vt->self_type == NULL || e->vt->self_type->nmethod == 0)
            return false;
        r->t = e->vt->self_type;
        r->recv = unconst(e->data);
        return true;
    }
    if (t->nmethod > 0) {
        r->t = t;
        r->recv = d;
        return true;
    }
    if (t->kind == KIND_POINTER && t->elem != NULL && t->elem->nmethod > 0) {
        r->t = t->elem;
        r->recv = *(void **)d;
        return true;
    }
    return false;
}

static const Method *find_method(const Recv *r, Str name, bool format) {
    const Method *m = type_method_by_name(r->t, name);
    if (format ? !is_format_method(m) : !is_string_method(m))
        return NULL;
    return m;
}

/* A method panicked. Go prints the panic in place of the value, unless the
 * receiver was nil, and a second panic while printing the first is let go. */
static void catch_panic(Pp *p, Any err, Rune verb, Str method) {
    if (p->panicking)
        panic(err);

    FmtFlags old = p->fmt.f;
    /* For this output we want default behaviour. */
    burrow__fmt_clearflags(&p->fmt);

    write_str(p, percent_bang);
    write_rune(p, verb);
    write_str(p, LIT("(PANIC="));
    write_str(p, method);
    write_str(p, LIT(" method: "));
    p->panicking = true;
    print_arg(p, err.t, err.data, 'v');
    p->panicking = false;
    write_byte(p, ')');

    p->fmt.f = old;
}

static void call_method(Pp *p, const Method *m, void *recv, void **args, void **rets,
                        Rune verb, Str name) {
    BURROW_TRY {
        method_call(m, recv, args, rets);
    }
    BURROW_CATCH(err) {
        catch_panic(p, err, verb, name);
    }
    BURROW_TRY_END;
}

/* A String, Error or GoString method, then the result formatted for verb. A
 * panic prints instead of the result. */
static void call_string_method(Pp *p, const Type *t, void *d, const Method *m,
                               void *recv, Rune verb, Str name, bool as_s) {
    volatile bool ok = false;
    Str out = BURROW_STR_EMPTY;
    void *rets[1] = {&out};
    BURROW_TRY {
        method_call(m, recv, NULL, rets);
        ok = true;
    }
    BURROW_CATCH(err) {
        catch_panic(p, err, verb, name);
    }
    BURROW_TRY_END;
    if (!ok)
        return;
    if (as_s)
        burrow__fmt_s(&p->fmt, out);
    else
        fmt_string(p, t, d, out, verb, false);
}

static bool is_error(const Type *t, void *d) {
    if (t == TYPE_ERROR)
        return true;
    Recv r;
    return method_holder(t, d, &r) && find_method(&r, LIT("Error"), false) != NULL;
}

static bool handle_methods(Pp *p, const Type *t, void *d, Rune verb) {
    if (p->erroring)
        return false;
    if (verb == 'w') {
        /* It is invalid to use %w other than with Errorf or with a non-error
         * argument. */
        if (!is_error(t, d) || !p->wrap_errs) {
            bad_verb(p, t, d, verb, false);
            return true;
        }
        /* If the arg is a Formatter, pass 'v' as the verb to it. */
        verb = 'v';
    }

    Recv r;
    bool has = method_holder(t, d, &r);

    /* A Formatter is in charge of everything. */
    if (has) {
        const Method *m = find_method(&r, LIT("Format"), true);
        if (m != NULL) {
            if (r.recv == NULL) {
                write_str(p, nil_angle);
                return true;
            }
            FmtState st = {&pp_state_vt, p};
            Rune v = verb;
            void *args[2] = {&st, &v};
            call_method(p, m, r.recv, args, NULL, verb, LIT("Format"));
            return true;
        }
    }

    if (p->fmt.f.sharp_v) {
        if (has) {
            const Method *m = find_method(&r, LIT("GoString"), false);
            if (m != NULL) {
                if (r.recv == NULL) {
                    write_str(p, nil_angle);
                    return true;
                }
                call_string_method(p, t, d, m, r.recv, verb, LIT("GoString"), true);
                return true;
            }
        }
        return false;
    }

    switch (verb) {
    case 'v':
    case 's':
    case 'x':
    case 'X':
    case 'q':
        break;
    default:
        return false;
    }

    /* An error first and then a Stringer, which is the order of Go's type
     * switch, so a type with both prints its Error. */
    if (t == TYPE_ERROR) {
        fmt_string(p, t, d, error_text(*(const Error *)d), verb, false);
        return true;
    }
    if (!has)
        return false;
    static const char *const names[] = {"Error", "String"};
    for (int i = 0; i < 2; i++) {
        Str name = str_from_cstr(names[i]);
        const Method *m = find_method(&r, name, false);
        if (m == NULL)
            continue;
        if (r.recv == NULL) {
            write_str(p, nil_angle);
            return true;
        }
        call_string_method(p, t, d, m, r.recv, verb, name, false);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ sorting
 *
 * internal/fmtsort, which puts a map's keys in order so that printing a map
 * gives the same text every time. */

static int cmp_int(int64_t a, int64_t b) {
    return (a > b) - (a < b);
}

static int cmp_uint(uint64_t a, uint64_t b) {
    return (a > b) - (a < b);
}

/* cmp.Compare for floats, where a NaN is less than everything else and equal to
 * another NaN. */
static int cmp_float(double a, double b) {
    bool an = a != a, bn = b != b;
    if (an || bn)
        return an && bn ? 0 : (an ? -1 : 1);
    return (a > b) - (a < b);
}

static int compare(const Type *t, const void *a, const void *b);

static int nil_compare(bool anil, bool bnil, bool *ok) {
    *ok = true;
    if (anil)
        return bnil ? 0 : -1;
    if (bnil)
        return 1;
    *ok = false;
    return 0;
}

static int compare(const Type *t, const void *a, const void *b) {
    switch ((int)t->kind) {
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        return cmp_int(value_int(t, a), value_int(t, b));
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return cmp_uint(value_uint(t, a), value_uint(t, b));
    case KIND_STRING: {
        Str x = *(const Str *)a, y = *(const Str *)b;
        Int n = x.len < y.len ? x.len : y.len;
        int c = n > 0 ? memcmp(x.p, y.p, (size_t)n) : 0;
        if (c != 0)
            return c < 0 ? -1 : 1;
        return cmp_int(x.len, y.len);
    }
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        return cmp_float(value_float(t, a), value_float(t, b));
    case KIND_COMPLEX64:
    case KIND_COMPLEX128: {
        Complex128 x = value_complex(t, a), y = value_complex(t, b);
        int c = cmp_float(x.re, y.re);
        return c != 0 ? c : cmp_float(x.im, y.im);
    }
    case KIND_BOOL: {
        bool x = *(const bool *)a, y = *(const bool *)b;
        return x == y ? 0 : (x ? 1 : -1);
    }
    case KIND_POINTER:
    case KIND_UNSAFE_POINTER:
    case KIND_CHAN: {
        uintptr_t x, y;
        memcpy(&x, a, sizeof x);
        memcpy(&y, b, sizeof y);
        return cmp_uint(x, y);
    }
    case KIND_STRUCT:
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            int c = compare(f->type, (const Byte *)a + f->offset,
                            (const Byte *)b + f->offset);
            if (c != 0)
                return c;
        }
        return 0;
    case KIND_ARRAY:
        for (uint32_t i = 0; i < t->len; i++) {
            size_t off = (size_t)i * t->elem->size;
            int c = compare(t->elem, (const Byte *)a + off, (const Byte *)b + off);
            if (c != 0)
                return c;
        }
        return 0;
    case KIND_INTERFACE: {
        const Type *ta = t, *tb = t;
        void *da = unconst(a), *db = unconst(b);
        unwrap(&ta, &da);
        unwrap(&tb, &db);
        bool ok;
        int c = nil_compare(ta == NULL, tb == NULL, &ok);
        if (ok)
            return c;
        /* Different types have no order but a stable one, so by descriptor. */
        if (ta != tb)
            return cmp_uint((uintptr_t)ta, (uintptr_t)tb);
        if (ta == TYPE_ERROR)
            return cmp_uint((uintptr_t)((const Error *)da)->data,
                            (uintptr_t)((const Error *)db)->data);
        return compare(ta, da, db);
    }
    default: {
        Byte stack[64];
        FmtBuf mb = {stack, 0, (Int)sizeof stack, false, false};
        fmt_buf_write_str(&mb, LIT("bad type in compare: "));
        write_type(&mb, t);
        Str msg = str_clone(heap_allocator(), (Str){mb.p, mb.len});
        burrow__fmt_buf_free(&mb);
        panic_str(msg);
    }
    }
}

typedef struct KeyValue {
    const void *key;
    void *value;
} KeyValue;

/* A stable merge sort, since keys that compare equal keep the order the map
 * gave them in Go too, and that is what slices.SortStableFunc promises. */
static void sort_map_entries(const Type *kt, KeyValue *v, KeyValue *tmp, Int n) {
    if (n < 2)
        return;
    if (n <= 8) {
        for (Int i = 1; i < n; i++) {
            KeyValue x = v[i];
            Int j = i;
            while (j > 0 && compare(kt, x.key, v[j - 1].key) < 0) {
                v[j] = v[j - 1];
                j--;
            }
            v[j] = x;
        }
        return;
    }
    Int mid = n / 2;
    sort_map_entries(kt, v, tmp, mid);
    sort_map_entries(kt, v + mid, tmp, n - mid);
    Int i = 0, j = mid, k = 0;
    while (i < mid && j < n)
        tmp[k++] = compare(kt, v[j].key, v[i].key) < 0 ? v[j++] : v[i++];
    while (i < mid)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, (size_t)n * sizeof *v);
}

/* ------------------------------------------------------------------ values */

/* Go's getField: an interface field is looked through to what it holds, and a
 * nil one stays as the interface so that it prints as <nil>. The walk down
 * print_value's interface case does the same thing, so this is just the
 * address of the field. */
static void *field_at(void *d, const Field *f) {
    return (Byte *)d + f->offset;
}

/* An error's value, for when none of its methods was used. errors.New is Go's
 * *errors.errorString, a pointer to a struct holding the message, so that is
 * what is printed for an error with no self_type. */
static void print_error_value(Pp *p, void *d, Rune verb, Int depth, bool can) {
    const Error *e = (const Error *)d;
    const Type *self = e->vt->self_type;
    if (self != NULL && self->kind != KIND_STRUCT) {
        print_value(p, self, unconst(e->data), verb, depth, can);
        return;
    }
    if (depth > 0) {
        fmt_pointer(p, TYPE_ERROR, d, verb, true);
        return;
    }
    write_byte(p, '&');
    if (self != NULL) {
        print_value(p, self, unconst(e->data), verb, depth + 1, can);
        return;
    }
    Str msg = error_text(*e);
    if (p->fmt.f.sharp_v)
        write_str(p, LIT("errors.errorString"));
    write_byte(p, '{');
    if (p->fmt.f.plus_v || p->fmt.f.sharp_v)
        write_str(p, LIT("s:"));
    print_value(p, TYPE_STRING, &msg, verb, depth + 1, false);
    write_byte(p, '}');
}

static void print_value(Pp *p, const Type *t, void *d, Rune verb, Int depth, bool can) {
    /* Handle values with special methods if not already handled by printArg
     * (depth == 0). */
    if (depth > 0 && t != NULL && can && handle_methods(p, t, d, verb))
        return;

    if (t == NULL) {
        if (depth == 0) {
            write_str(p, LIT("<invalid reflect.Value>"));
        } else if (verb == 'v') {
            write_str(p, nil_angle);
        } else {
            bad_verb(p, NULL, NULL, verb, true);
        }
        return;
    }

    switch ((int)t->kind) {
    case KIND_BOOL:
        fmt_bool(p, t, d, *(const bool *)d, verb, true);
        return;
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        fmt_integer(p, t, d, (uint64_t)value_int(t, d), true, verb, true);
        return;
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        fmt_integer(p, t, d, value_uint(t, d), false, verb, true);
        return;
    case KIND_FLOAT32:
        fmt_float(p, t, d, value_float(t, d), 32, verb, true);
        return;
    case KIND_FLOAT64:
        fmt_float(p, t, d, value_float(t, d), 64, verb, true);
        return;
    case KIND_COMPLEX64:
        fmt_complex(p, t, d, value_complex(t, d), 64, verb, true);
        return;
    case KIND_COMPLEX128:
        fmt_complex(p, t, d, value_complex(t, d), 128, verb, true);
        return;
    case KIND_STRING:
        fmt_string(p, t, d, *(const Str *)d, verb, true);
        return;
    case KIND_MAP: {
        Map *m = *(Map **)d;
        if (p->fmt.f.sharp_v) {
            write_type(&p->buf, t);
            if (m == NULL) {
                write_str(p, nil_paren);
                return;
            }
            write_byte(p, '{');
        } else {
            write_str(p, LIT("map["));
        }
        Int n = m == NULL ? 0 : map_len(m);
        if (n > 0) {
            KeyValue *kv = (KeyValue *)mem_alloc_nozero(
                heap_allocator(), (size_t)n * 2 * sizeof(KeyValue), _Alignof(KeyValue));
            if (kv == NULL) {
                p->buf.failed = true;
                return;
            }
            Int got = 0;
            const void *k;
            void *v;
            for (MapIter it = map_iter(m); got < n && map_next(&it, &k, &v);) {
                kv[got].key = k;
                kv[got].value = v;
                got++;
            }
            const Type *kt = t->key != NULL ? t->key : map_key_type(m);
            const Type *vt = t->elem != NULL ? t->elem : map_val_type(m);
            sort_map_entries(kt, kv, kv + n, got);
            for (Int i = 0; i < got; i++) {
                if (i > 0) {
                    if (p->fmt.f.sharp_v)
                        write_str(p, comma_space);
                    else
                        write_byte(p, ' ');
                }
                print_value(p, kt, unconst(kv[i].key), verb, depth + 1, can);
                write_byte(p, ':');
                print_value(p, vt, kv[i].value, verb, depth + 1, can);
            }
            mem_free(heap_allocator(), kv, (size_t)n * 2 * sizeof(KeyValue),
                     _Alignof(KeyValue));
        }
        if (p->fmt.f.sharp_v)
            write_byte(p, '}');
        else
            write_byte(p, ']');
        return;
    }
    case KIND_STRUCT:
        if (p->fmt.f.sharp_v)
            write_type(&p->buf, t);
        write_byte(p, '{');
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (i > 0) {
                if (p->fmt.f.sharp_v)
                    write_str(p, comma_space);
                else
                    write_byte(p, ' ');
            }
            if ((p->fmt.f.plus_v || p->fmt.f.sharp_v) && f->name.len > 0) {
                write_str(p, f->name);
                write_byte(p, ':');
            }
            print_value(p, f->type, field_at(d, f), verb, depth + 1,
                        can && field_is_exported(f));
        }
        write_byte(p, '}');
        return;
    case KIND_INTERFACE: {
        const Type *et = t;
        void *ed = d;
        unwrap(&et, &ed);
        if (et == NULL) {
            if (p->fmt.f.sharp_v) {
                write_type(&p->buf, t);
                write_str(p, nil_paren);
            } else {
                write_str(p, nil_angle);
            }
        } else if (et == TYPE_ERROR) {
            if (can && handle_methods(p, et, ed, verb))
                return;
            print_error_value(p, ed, verb, depth + 1, can);
        } else if (et->kind == KIND_INTERFACE) {
            /* An interface whose vtable does not say what is behind it. */
            write_byte(p, '?');
            write_type(&p->buf, et);
            write_byte(p, '?');
        } else {
            print_value(p, et, ed, verb, depth + 1, can);
        }
        return;
    }
    case KIND_ARRAY:
    case KIND_SLICE: {
        const Slice *s = t->kind == KIND_SLICE ? (const Slice *)d : NULL;
        const Type *et = t->elem;
        if (et == NULL && s != NULL)
            et = s->elem;
        Byte *base = s != NULL ? (Byte *)s->p : (Byte *)d;
        Int n = s != NULL ? s->len : (Int)t->len;
        bool is_nil = s != NULL && s->p == NULL;
        switch (verb) {
        case 's':
        case 'q':
        case 'x':
        case 'X':
            /* Handle byte and uint8 slices and arrays special for the above
             * verbs. */
            if (et != NULL && et->kind == KIND_UINT8) {
                Byte stack[64];
                FmtBuf tb;
                Str ts = arg_type_string(&tb, stack, (Int)sizeof stack, t, d);
                fmt_bytes(p, base, n, is_nil, verb, ts, d);
                burrow__fmt_buf_free(&tb);
                return;
            }
            break;
        default:
            break;
        }
        if (et == NULL)
            n = 0;
        if (p->fmt.f.sharp_v) {
            write_type(&p->buf, t);
            if (is_nil) {
                write_str(p, nil_paren);
                return;
            }
            write_byte(p, '{');
            for (Int i = 0; i < n; i++) {
                if (i > 0)
                    write_str(p, comma_space);
                print_value(p, et, base + (size_t)i * et->size, verb, depth + 1, can);
            }
            write_byte(p, '}');
        } else {
            write_byte(p, '[');
            for (Int i = 0; i < n; i++) {
                if (i > 0)
                    write_byte(p, ' ');
                print_value(p, et, base + (size_t)i * et->size, verb, depth + 1, can);
            }
            write_byte(p, ']');
        }
        return;
    }
    case KIND_POINTER: {
        /* pointer to array or slice or struct? ok at top level but not
         * embedded (avoid loops) */
        void *to = *(void **)d;
        if (depth == 0 && to != NULL && t->elem != NULL) {
            switch ((int)t->elem->kind) {
            case KIND_ARRAY:
            case KIND_SLICE:
            case KIND_STRUCT:
            case KIND_MAP:
                write_byte(p, '&');
                print_value(p, t->elem, to, verb, depth + 1, can);
                return;
            default:
                break;
            }
        }
        fmt_pointer(p, t, d, verb, true);
        return;
    }
    case KIND_CHAN:
    case KIND_FUNC:
    case KIND_UNSAFE_POINTER:
        fmt_pointer(p, t, d, verb, true);
        return;
    default:
        write_byte(p, '?');
        write_type(&p->buf, t);
        write_byte(p, '?');
        return;
    }
}

/* The types Go's printArg switches on directly, which have no methods and so
 * need no lookup. */
static bool is_basic(const Type *t) {
    return t->name.len > 0 && t->pkg_path.len == 0 && t->nmethod == 0 &&
           t->kind != KIND_INTERFACE && t->kind != KIND_STRUCT;
}

static void print_arg(Pp *p, const Type *t, void *d, Rune verb) {
    unwrap(&t, &d);

    if (t == NULL) {
        switch (verb) {
        case 'T':
        case 'v':
            burrow__fmt_pad_string(&p->fmt, nil_angle);
            break;
        default:
            bad_verb(p, NULL, NULL, verb, false);
        }
        return;
    }

    /* Special processing considerations. %T (the value's type) and %p (its
     * address) are special; we always do them first. */
    switch (verb) {
    case 'T': {
        Byte stack[64];
        FmtBuf tb;
        Str ts = arg_type_string(&tb, stack, (Int)sizeof stack, t, d);
        burrow__fmt_s(&p->fmt, ts);
        burrow__fmt_buf_free(&tb);
        return;
    }
    case 'p':
        fmt_pointer(p, t, d, 'p', false);
        return;
    default:
        break;
    }

    if (is_basic(t)) {
        switch ((int)t->kind) {
        case KIND_BOOL:
            fmt_bool(p, t, d, *(const bool *)d, verb, false);
            return;
        case KIND_FLOAT32:
            fmt_float(p, t, d, value_float(t, d), 32, verb, false);
            return;
        case KIND_FLOAT64:
            fmt_float(p, t, d, value_float(t, d), 64, verb, false);
            return;
        case KIND_COMPLEX64:
            fmt_complex(p, t, d, value_complex(t, d), 64, verb, false);
            return;
        case KIND_COMPLEX128:
            fmt_complex(p, t, d, value_complex(t, d), 128, verb, false);
            return;
        case KIND_STRING:
            fmt_string(p, t, d, *(const Str *)d, verb, false);
            return;
        default:
            if (kind_is_int(t->kind)) {
                fmt_integer(p, t, d, (uint64_t)value_int(t, d), true, verb, false);
                return;
            }
            if (kind_is_uint(t->kind)) {
                fmt_integer(p, t, d, value_uint(t, d), false, verb, false);
                return;
            }
            break;
        }
    }

    /* []byte, unnamed, is a case of its own in Go's switch. */
    if (t->kind == KIND_SLICE && t->name.len == 0) {
        const Slice *s = (const Slice *)d;
        const Type *et = t->elem != NULL ? t->elem : s->elem;
        if (et == &burrow_type_uint8_t) {
            fmt_bytes(p, (const Byte *)s->p, s->len, s->p == NULL, verb, LIT("[]byte"),
                      d);
            return;
        }
    }

    /* If the type is not simple, it might have methods. */
    if (handle_methods(p, t, d, verb))
        return;
    if (t == TYPE_ERROR)
        print_error_value(p, d, verb, 0, true);
    else
        print_value(p, t, d, verb, 0, true);
}

/* ------------------------------------------------------------------ formats */

/* Whether x is too big to be a width or a precision. */
static bool too_large(Int x) {
    const Int max = 1000000;
    return x > max || x < -max;
}

/* The number at s[start:end], if there is one, and where it stopped. */
static Int parsenum(Str s, Int start, Int end, bool *isnum, Int *newi) {
    Int num = 0;
    *isnum = false;
    if (start >= end) {
        *newi = end;
        return 0;
    }
    Int i;
    for (i = start; i < end && '0' <= s.p[i] && s.p[i] <= '9'; i++) {
        if (too_large(num)) {
            *isnum = false;
            *newi = end;
            return 0; /* Overflow; crazy long number most likely. */
        }
        num = num * 10 + (Int)(s.p[i] - '0');
        *isnum = true;
    }
    *newi = i;
    return num;
}

static Any arg_at(Slice a, Int i) {
    return ((const Any *)a.p)[i];
}

/* The width or precision from an operand, for *. */
static Int int_from_arg(Slice a, Int *arg_num, bool *is_int) {
    Int num = 0;
    *is_int = false;
    if (*arg_num < a.len) {
        Any v = arg_at(a, *arg_num);
        const Type *t = v.t;
        void *d = v.data;
        unwrap(&t, &d);
        if (t != NULL && kind_is_int(t->kind)) {
            int64_t n = value_int(t, d);
            if ((int64_t)(Int)n == n) {
                num = (Int)n;
                *is_int = true;
            }
        } else if (t != NULL && kind_is_uint(t->kind)) {
            uint64_t n = value_uint(t, d);
            if ((int64_t)n >= 0 && (uint64_t)(Int)n == n) {
                num = (Int)n;
                *is_int = true;
            }
        }
        *arg_num += 1;
        if (too_large(num)) {
            num = 0;
            *is_int = false;
        }
    }
    return num;
}

/* An argument index like [3] at the start of format, as a zero based index,
 * with how many bytes it took. */
static Int parse_arg_number(Str format, Int *wid, bool *ok) {
    /* There must be at least 3 bytes: [n]. */
    *ok = false;
    *wid = 1;
    if (format.len < 3)
        return 0;
    /* Find closing bracket. */
    for (Int i = 1; i < format.len; i++) {
        if (format.p[i] == ']') {
            bool isnum;
            Int newi;
            Int width = parsenum(format, 1, i, &isnum, &newi);
            if (!isnum || newi != i) {
                *wid = i + 1;
                return 0;
            }
            *wid = i + 1;
            *ok = true;
            return width - 1; /* arg numbers are one-indexed and skip paren. */
        }
    }
    return 0;
}

/* The argument number to use next, after any index at format[i]. */
static Int arg_number(Pp *p, Int arg_num, Str format, Int *i, Int num_args,
                      bool *found) {
    *found = false;
    if (format.len <= *i || format.p[*i] != '[')
        return arg_num;
    p->reordered = true;
    Int wid;
    bool ok;
    Int index = parse_arg_number((Str){format.p + *i, format.len - *i}, &wid, &ok);
    *i += wid;
    if (ok && 0 <= index && index < num_args) {
        *found = true;
        return index;
    }
    p->good_arg_num = false;
    *found = ok;
    return arg_num;
}

static void bad_arg_num(Pp *p, Rune verb) {
    write_str(p, percent_bang);
    write_rune(p, verb);
    write_str(p, LIT("(BADINDEX)"));
}

static void missing_arg(Pp *p, Rune verb) {
    write_str(p, percent_bang);
    write_rune(p, verb);
    write_str(p, LIT("(MISSING)"));
}

static void do_printf(Pp *p, Str format, Slice a) {
    Int end = format.len;
    Int arg_num = 0;          /* we process one argument per non-trivial format */
    bool after_index = false; /* previous item in format was an index like [3]. */
    p->reordered = false;
    Int i = 0;
    while (i < end) {
        p->good_arg_num = true;
        Int lasti = i;
        while (i < end && format.p[i] != '%')
            i++;
        if (i > lasti)
            fmt_buf_write(&p->buf, format.p + lasti, i - lasti);
        if (i >= end)
            break; /* done processing format string */

        /* Process one verb. */
        i++;

        /* Do we have flags? */
        burrow__fmt_clearflags(&p->fmt);
        bool simple_done = false;
        for (; i < end; i++) {
            Byte c = format.p[i];
            bool flag = true;
            switch (c) {
            case '#':
                p->fmt.f.sharp = true;
                break;
            case '0':
                p->fmt.f.zero = true;
                break;
            case '+':
                p->fmt.f.plus = true;
                break;
            case '-':
                p->fmt.f.minus = true;
                break;
            case ' ':
                p->fmt.f.space = true;
                break;
            default:
                flag = false;
            }
            if (flag)
                continue;
            /* Fast path for common case of ascii lower case simple verbs
             * without precision or width or argument indices. */
            if ('a' <= c && c <= 'z' && arg_num < a.len) {
                if (c == 'w')
                    pp_add_wrapped(p, arg_num);
                if (c == 'v' || c == 'w') {
                    /* Go syntax */
                    p->fmt.f.sharp_v = p->fmt.f.sharp;
                    p->fmt.f.sharp = false;
                    /* Struct-field syntax */
                    p->fmt.f.plus_v = p->fmt.f.plus;
                    p->fmt.f.plus = false;
                }
                Any v = arg_at(a, arg_num);
                print_arg(p, v.t, v.data, (Rune)c);
                arg_num++;
                i++;
                simple_done = true;
            }
            break;
        }
        if (simple_done)
            continue;

        /* Do we have an explicit argument index? */
        arg_num = arg_number(p, arg_num, format, &i, a.len, &after_index);

        /* Do we have width? */
        if (i < end && format.p[i] == '*') {
            i++;
            p->fmt.wid = int_from_arg(a, &arg_num, &p->fmt.f.wid_present);

            if (!p->fmt.f.wid_present)
                write_str(p, LIT("%!(BADWIDTH)"));

            /* We have a negative width, so take its value and ensure that the
             * minus flag is set. */
            if (p->fmt.wid < 0) {
                p->fmt.wid = -p->fmt.wid;
                p->fmt.f.minus = true;
                p->fmt.f.zero = false; /* Do not pad with zeros to the right. */
            }
            after_index = false;
        } else {
            p->fmt.wid = parsenum(format, i, end, &p->fmt.f.wid_present, &i);
            if (after_index && p->fmt.f.wid_present) /* "%[3]2d" */
                p->good_arg_num = false;
        }

        /* Do we have precision? */
        if (i + 1 < end && format.p[i] == '.') {
            i++;
            if (after_index) /* "%[3].2d" */
                p->good_arg_num = false;
            arg_num = arg_number(p, arg_num, format, &i, a.len, &after_index);
            if (i < end && format.p[i] == '*') {
                i++;
                p->fmt.prec = int_from_arg(a, &arg_num, &p->fmt.f.prec_present);
                /* Negative precision arguments don't make sense. */
                if (p->fmt.prec < 0) {
                    p->fmt.prec = 0;
                    p->fmt.f.prec_present = false;
                }
                if (!p->fmt.f.prec_present)
                    write_str(p, LIT("%!(BADPREC)"));
                after_index = false;
            } else {
                p->fmt.prec = parsenum(format, i, end, &p->fmt.f.prec_present, &i);
                if (!p->fmt.f.prec_present) {
                    p->fmt.prec = 0;
                    p->fmt.f.prec_present = true;
                }
            }
        }

        if (!after_index)
            arg_num = arg_number(p, arg_num, format, &i, a.len, &after_index);

        if (i >= end) {
            write_str(p, LIT("%!(NOVERB)"));
            break;
        }

        Int size = 1;
        Rune verb = (Rune)format.p[i];
        if (verb >= UTF8_RUNE_SELF)
            verb = utf8_decode_rune_in_string((Str){format.p + i, end - i}, &size);
        i += size;

        if (verb == '%') {
            /* Percent does not absorb operands and ignores f.wid and f.prec. */
            write_byte(p, '%');
        } else if (!p->good_arg_num) {
            bad_arg_num(p, verb);
        } else if (arg_num >= a.len) {
            /* No argument left over to print for the current verb. */
            missing_arg(p, verb);
        } else {
            if (verb == 'w')
                pp_add_wrapped(p, arg_num);
            if (verb == 'v' || verb == 'w') {
                /* Go syntax */
                p->fmt.f.sharp_v = p->fmt.f.sharp;
                p->fmt.f.sharp = false;
                /* Struct-field syntax */
                p->fmt.f.plus_v = p->fmt.f.plus;
                p->fmt.f.plus = false;
            }
            Any v = arg_at(a, arg_num);
            print_arg(p, v.t, v.data, verb);
            arg_num++;
        }
    }

    /* Check for extra arguments unless the call accessed the arguments out of
     * order, in which case it's too expensive to detect if they've all been
     * used and arguably OK if they're not. */
    if (!p->reordered && arg_num < a.len) {
        burrow__fmt_clearflags(&p->fmt);
        write_str(p, LIT("%!(EXTRA "));
        for (Int k = arg_num; k < a.len; k++) {
            if (k > arg_num)
                write_str(p, comma_space);
            Any v = arg_at(a, k);
            const Type *t = v.t;
            void *d = v.data;
            unwrap(&t, &d);
            if (t == NULL) {
                write_str(p, nil_angle);
            } else {
                write_arg_type(&p->buf, t, d);
                write_byte(p, '=');
                print_arg(p, t, d, 'v');
            }
        }
        write_byte(p, ')');
    }
}

static void do_print(Pp *p, Slice a) {
    bool prev_string = false;
    for (Int k = 0; k < a.len; k++) {
        Any v = arg_at(a, k);
        const Type *t = v.t;
        void *d = v.data;
        unwrap(&t, &d);
        bool is_string = t != NULL && t->kind == KIND_STRING;
        /* Add a space between two non-string arguments. */
        if (k > 0 && !is_string && !prev_string)
            write_byte(p, ' ');
        print_arg(p, t, d, 'v');
        prev_string = is_string;
    }
}

/* doPrintln is like doPrint but always adds a space between arguments and a
 * newline after the last argument. */
static void do_println(Pp *p, Slice a) {
    for (Int k = 0; k < a.len; k++) {
        if (k > 0)
            write_byte(p, ' ');
        Any v = arg_at(a, k);
        print_arg(p, v.t, v.data, 'v');
    }
    write_byte(p, '\n');
}

/* --------------------------------------------------------------- entry points */

#define PP_STACK 256

typedef enum { DO_PRINTF, DO_PRINT, DO_PRINTLN } DoKind;

static void pp_run(Pp *p, DoKind k, Str format, Slice args) {
    switch (k) {
    case DO_PRINTF:
        do_printf(p, format, args);
        break;
    case DO_PRINT:
        do_print(p, args);
        break;
    case DO_PRINTLN:
    default:
        do_println(p, args);
        break;
    }
}

static Int to_writer(IoWriter w, DoKind k, Str format, Slice args, Error *err) {
    Byte stack[PP_STACK];
    Pp p;
    pp_init(&p, stack, (Int)sizeof stack);
    pp_run(&p, k, format, args);
    Error e = BURROW_NO_ERROR;
    Int n = 0;
    if (p.buf.failed)
        e = burrow_err_out_of_memory;
    else
        n = w.vt->write(w.data, slice_from(p.buf.p, p.buf.len, p.buf.len, TYPE_BYTE),
                        &e);
    pp_free(&p);
    if (err != NULL)
        *err = e;
    return n;
}

static Int to_stdout(DoKind k, Str format, Slice args, Error *err) {
    Byte stack[PP_STACK];
    Pp p;
    pp_init(&p, stack, (Int)sizeof stack);
    pp_run(&p, k, format, args);
    Error e = BURROW_NO_ERROR;
    Int n = 0;
    if (p.buf.failed) {
        e = burrow_err_out_of_memory;
    } else if (p.buf.len > 0) {
        n = (Int)fwrite(p.buf.p, 1, (size_t)p.buf.len, stdout);
        if (n < p.buf.len)
            e = io_err_short_write;
    }
    pp_free(&p);
    if (err != NULL)
        *err = e;
    return n;
}

static Str to_string(Alloc *a, DoKind k, Str format, Slice args) {
    Byte stack[PP_STACK];
    Pp p;
    pp_init(&p, stack, (Int)sizeof stack);
    pp_run(&p, k, format, args);
    Str out = BURROW_STR_EMPTY;
    if (!p.buf.failed && p.buf.len > 0) {
        Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)p.buf.len, 1);
        if (b != NULL) {
            memcpy(b, p.buf.p, (size_t)p.buf.len);
            out = str_from_bytes(b, p.buf.len);
        }
    }
    pp_free(&p);
    return out;
}

static Slice to_slice(Alloc *a, Slice b, DoKind k, Str format, Slice args) {
    Byte stack[PP_STACK];
    Pp p;
    pp_init(&p, stack, (Int)sizeof stack);
    pp_run(&p, k, format, args);
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    if (!p.buf.failed && p.buf.len > 0)
        b = slice_append(a, b, p.buf.p, p.buf.len);
    pp_free(&p);
    return b;
}

Int fmt_fprintf(IoWriter w, Str format, Slice args, Error *err) {
    return to_writer(w, DO_PRINTF, format, args, err);
}

Int fmt_printf(Str format, Slice args, Error *err) {
    return to_stdout(DO_PRINTF, format, args, err);
}

Str fmt_sprintf(Alloc *a, Str format, Slice args) {
    return to_string(a, DO_PRINTF, format, args);
}

Slice fmt_appendf(Alloc *a, Slice b, Str format, Slice args) {
    return to_slice(a, b, DO_PRINTF, format, args);
}

Int fmt_fprint(IoWriter w, Slice args, Error *err) {
    return to_writer(w, DO_PRINT, BURROW_STR_EMPTY, args, err);
}

Int fmt_print(Slice args, Error *err) {
    return to_stdout(DO_PRINT, BURROW_STR_EMPTY, args, err);
}

Str fmt_sprint(Alloc *a, Slice args) {
    return to_string(a, DO_PRINT, BURROW_STR_EMPTY, args);
}

Slice fmt_append(Alloc *a, Slice b, Slice args) {
    return to_slice(a, b, DO_PRINT, BURROW_STR_EMPTY, args);
}

Int fmt_fprintln(IoWriter w, Slice args, Error *err) {
    return to_writer(w, DO_PRINTLN, BURROW_STR_EMPTY, args, err);
}

Int fmt_println(Slice args, Error *err) {
    return to_stdout(DO_PRINTLN, BURROW_STR_EMPTY, args, err);
}

Str fmt_sprintln(Alloc *a, Slice args) {
    return to_string(a, DO_PRINTLN, BURROW_STR_EMPTY, args);
}

Slice fmt_appendln(Alloc *a, Slice b, Slice args) {
    return to_slice(a, b, DO_PRINTLN, BURROW_STR_EMPTY, args);
}

/* ------------------------------------------------------------------ errorf */

/* One %w, which unwraps to what it wrapped. */
typedef struct WrapError {
    Str msg;
    Error err;
} WrapError;

/* Several, which unwrap to all of them. */
typedef struct WrapErrors {
    Str msg;
    Slice errs;
} WrapErrors;

static Str wrap_error_message(const void *self) {
    return ((const WrapError *)self)->msg;
}

static Error wrap_error_unwrap(const void *self) {
    return ((const WrapError *)self)->err;
}

static Error new_wrap_error(Alloc *a, Str msg, Error err);

static Error wrap_error_clone(const void *self, Alloc *a) {
    const WrapError *w = (const WrapError *)self;
    return new_wrap_error(a, w->msg, error_retain(a, w->err));
}

static const ErrorVT wrap_error_vt = {
    NULL, wrap_error_message, wrap_error_unwrap, NULL, NULL, NULL, wrap_error_clone,
};

static Str wrap_errors_message(const void *self) {
    return ((const WrapErrors *)self)->msg;
}

static Slice wrap_errors_unwrap_multi(const void *self) {
    return ((const WrapErrors *)self)->errs;
}

static Error new_wrap_errors(Alloc *a, Str msg, Slice errs);

static Error wrap_errors_clone(const void *self, Alloc *a) {
    const WrapErrors *w = (const WrapErrors *)self;
    Slice errs = slice_make(a, TYPE_ERROR, w->errs.len, w->errs.len);
    if (w->errs.len > 0 && errs.p == NULL)
        return burrow_err_out_of_memory;
    for (Int i = 0; i < w->errs.len; i++)
        ((Error *)errs.p)[i] = error_retain(a, ((const Error *)w->errs.p)[i]);
    return new_wrap_errors(a, w->msg, errs);
}

static const ErrorVT wrap_errors_vt = {
    NULL, wrap_errors_message, NULL, wrap_errors_unwrap_multi, NULL,
    NULL, wrap_errors_clone,
};

/* The struct with the message after it, in one allocation, as errors_new
 * does. */
static void *alloc_with_text(Alloc *a, size_t size, size_t align, Str msg, Str *out) {
    Byte *base = (Byte *)mem_alloc(a, size + (size_t)msg.len, align);
    if (base == NULL)
        return NULL;
    if (msg.len > 0)
        memcpy(base + size, msg.p, (size_t)msg.len);
    *out = (Str){base + size, msg.len};
    return base;
}

static Error new_wrap_error(Alloc *a, Str msg, Error err) {
    Str text;
    WrapError *w = (WrapError *)alloc_with_text(a, sizeof(WrapError),
                                                _Alignof(WrapError), msg, &text);
    if (w == NULL)
        return burrow_err_out_of_memory;
    w->msg = text;
    w->err = err;
    return (Error){&wrap_error_vt, w};
}

static Error new_wrap_errors(Alloc *a, Str msg, Slice errs) {
    Str text;
    WrapErrors *w = (WrapErrors *)alloc_with_text(a, sizeof(WrapErrors),
                                                  _Alignof(WrapErrors), msg, &text);
    if (w == NULL)
        return burrow_err_out_of_memory;
    w->msg = text;
    w->errs = errs;
    return (Error){&wrap_errors_vt, w};
}

/* The operand at i as an Error, or a nil one when it is not an error. */
static Error arg_error(Slice a, Int i) {
    Any v = arg_at(a, i);
    const Type *t = v.t;
    void *d = v.data;
    unwrap(&t, &d);
    if (t != TYPE_ERROR)
        return BURROW_NO_ERROR;
    return *(const Error *)d;
}

Error fmt_errorf(Str format, Slice args) {
    Alloc *a = error_allocator();
    Byte stack[PP_STACK];
    Pp p;
    pp_init(&p, stack, (Int)sizeof stack);
    p.wrap_errs = true;
    do_printf(&p, format, args);
    if (p.buf.failed) {
        pp_free(&p);
        return burrow_err_out_of_memory;
    }
    Str s = {p.buf.p, p.buf.len};
    Error err;
    switch (p.nwrapped) {
    case 0:
        err = errors_new(a, s);
        break;
    case 1:
        err = new_wrap_error(a, s, arg_error(args, p.wrapped[0]));
        break;
    default: {
        if (p.reordered) {
            /* Sort, so that duplicates are next to each other. */
            for (Int i = 1; i < p.nwrapped; i++) {
                Int x = p.wrapped[i], j = i;
                while (j > 0 && p.wrapped[j - 1] > x) {
                    p.wrapped[j] = p.wrapped[j - 1];
                    j--;
                }
                p.wrapped[j] = x;
            }
        }
        Slice errs = slice_make(a, TYPE_ERROR, 0, p.nwrapped);
        for (Int i = 0; i < p.nwrapped; i++) {
            if (i > 0 && p.wrapped[i - 1] == p.wrapped[i])
                continue;
            Error e = arg_error(args, p.wrapped[i]);
            if (e.vt != NULL && errs.p != NULL)
                ((Error *)errs.p)[errs.len++] = e;
        }
        if (errs.len == 0)
            errs = slice_nil(TYPE_ERROR);
        err = new_wrap_errors(a, s, errs);
        break;
    }
    }
    pp_free(&p);
    return err;
}
