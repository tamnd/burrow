/* Derived from Go's src/debug/gosym/symtab.go and pclntab.go. Go source:
 * go1.27.1.
 *
 * Go reads the line tables by slicing and indexing them freely and lets a
 * malformed table panic, then recovers in each of the functions that read one
 * and gives -1, "", 0 or no functions. This does the same. Every read checks
 * its bounds the way Go's slicing does, against the length or the capacity as
 * Go would, and panics, and the same functions recover. So a damaged table
 * gives what it gives in Go, down to which fields a half read header leaves
 * set.
 *
 * Go also keeps two caches of the names it has read out of a table. A name
 * comes out the same with or without them, so they are not kept here.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/debug/gosym.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The formats, in the order Go numbers them. */
enum {
    GOSYM_VER_UNKNOWN,
    GOSYM_VER11,
    GOSYM_VER12,
    GOSYM_VER116,
    GOSYM_VER118,
    GOSYM_VER120,
};

/* ---------------------------------------------------------------- helpers */

BURROW_NORETURN static void gosym_nil_panic(void) {
    runtime_panic(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

BURROW_NORETURN static void gosym_bounds_panic(void) {
    runtime_panic(BURROW_S("runtime error: slice bounds out of range"));
}

/* b[lo:], which Go checks against the length. */
static Slice gosym_from(Slice b, uint64_t lo) {
    if (lo > (uint64_t)b.len)
        gosym_bounds_panic();
    if (lo == 0)
        return b;
    Slice out = {(Byte *)b.p + lo, b.len - (Int)lo, b.cap - (Int)lo, TYPE_BYTE};
    return out;
}

/* b[:hi], which Go checks against the capacity. */
static Slice gosym_to(Slice b, uint64_t hi) {
    if (hi > (uint64_t)b.cap)
        gosym_bounds_panic();
    b.len = (Int)hi;
    return b;
}

/* b[i:] for an index Go computes as an int32, which is out of range when it
 * comes out negative. */
static Slice gosym_from32(Slice b, int32_t i) {
    if (i < 0)
        gosym_bounds_panic();
    return gosym_from(b, (uint64_t)i);
}

static uint32_t gosym_u32(const GosymLineTable *t, Slice b) {
    if (b.len < 4)
        gosym_bounds_panic();
    const Byte *p = (const Byte *)b.p;
    if (t->big_endian)
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static uint64_t gosym_u64(const GosymLineTable *t, Slice b) {
    if (b.len < 8)
        gosym_bounds_panic();
    uint64_t lo = gosym_u32(t, b);
    uint64_t hi = gosym_u32(t, gosym_from(b, 4));
    if (t->big_endian)
        return lo << 32 | hi;
    return hi << 32 | lo;
}

/* LineTable.uintptr. */
static uint64_t gosym_uintptr(const GosymLineTable *t, Slice b) {
    if (t->ptrsize == 4)
        return gosym_u32(t, b);
    return gosym_u64(t, b);
}

/* stringFrom: the NUL terminated string at off in arr. With no NUL Go slices
 * arr[off:off+uint32(-1)], which is out of range for any table smaller than
 * 4GB, so that panics here the same way. The result points into arr. */
static Str gosym_string_from(Slice arr, uint32_t off) {
    Slice b = gosym_from(arr, off);
    const Byte *p = (const Byte *)b.p;
    const Byte *nul = b.len > 0 ? (const Byte *)memchr(p, 0, (size_t)b.len) : NULL;
    uint32_t i = nul != NULL ? (uint32_t)(nul - p) : UINT32_MAX;
    uint32_t hi = off + i;
    if (hi < off || hi > (uint64_t)arr.cap)
        gosym_bounds_panic();
    return str_from_bytes((const Byte *)arr.p + off, (Int)(hi - off));
}

/* Runs fn(ctx) and says whether it finished, or panicked and was recovered,
 * which is the defer and recover the functions below have in Go. */
static bool gosym_recover(void (*fn)(void *ctx), void *ctx) {
    volatile bool ok = false;
    BURROW_TRY {
        fn(ctx);
        ok = true;
    }
    BURROW_CATCH(p) {
        (void)p;
    }
    BURROW_TRY_END;
    return ok;
}

/* ------------------------------------------------------------- type info */

#define GOSYM_SYM_FIELDS(F, T)                                                         \
    F(T, uint64_t, value, Value, "")                                                   \
    F(T, uint8_t, type, Type, "")                                                      \
    F(T, Str, name, Name, "")                                                          \
    F(T, uint64_t, go_type, GoType, "")                                                \
    F(T, UnsafePointer, func, Func, "")

/* The pointers are ones the descriptors have no type for, so they are
 * unsafe.Pointer here. */
#define GOSYM_FUNC_FIELDS(F, T)                                                        \
    F(T, uint64_t, entry, Entry, "")                                                   \
    F(T, UnsafePointer, sym, Sym, "")                                                  \
    F(T, uint64_t, end, End, "")                                                       \
    F(T, Int, frame_size, FrameSize, "")                                               \
    F(T, UnsafePointer, line_table, LineTable, "")                                     \
    F(T, UnsafePointer, obj, Obj, "")

BURROW_STRUCT_AS_DEFINE(GosymSym, GOSYM_SYM_FIELDS);
BURROW_STRUCT_AS_DEFINE(GosymFunc, GOSYM_FUNC_FIELDS);

/* An Obj is two slices, which have no descriptors either, so it has a size
 * and no fields. */
const Type burrow_type_GosymObj = {
    {(const Byte *)"Obj", 3},
    {(const Byte *)"debug/gosym", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(GosymObj),
    (uint16_t)_Alignof(GosymObj),
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

/* ------------------------------------------------------------------ errors */

static Int gosym_put(Byte *p, Int n, Str s) {
    if (p != NULL && s.len > 0)
        memcpy(p + n, s.p, (size_t)s.len);
    return n + s.len;
}

/* v in decimal, or in hex with 0x in front, and a minus sign first if it is
 * negative, which is Go's %d and %#x. */
static Int gosym_put_int(Byte *p, Int n, int64_t v, bool hex) {
    uint64_t u = (uint64_t)v;
    if (v < 0) {
        n = gosym_put(p, n, BURROW_S("-"));
        u = 0 - u;
    }
    if (hex)
        n = gosym_put(p, n, BURROW_S("0x"));
    unsigned base = hex ? 16U : 10U;
    Byte digits[24];
    Int nd = 0;
    do {
        digits[nd++] = (Byte) "0123456789abcdef"[u % base];
        u /= base;
    } while (u != 0);
    while (nd > 0) {
        if (p != NULL)
            p[n] = digits[nd - 1];
        n++;
        nd--;
    }
    return n;
}

/* UnknownFileError. The name first, so that errors_as gives back a Str *, and
 * the text after it, built once since the message slot cannot allocate. */
typedef struct GosymFileErrorBox {
    GosymUnknownFileError name;
    Str message;
} GosymFileErrorBox;

static const Str gosym_unknown_file = BURROW_S_INIT("unknown file: ");

static Int gosym_file_error_put(Byte *p, GosymUnknownFileError e) {
    return gosym_put(p, gosym_put(p, 0, gosym_unknown_file), e);
}

Str gosym_unknown_file_error_error(GosymUnknownFileError e, Alloc *a) {
    Int n = gosym_file_error_put(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    gosym_file_error_put(p, e);
    return str_from_bytes(p, n);
}

static Str gosym_file_error_m_error(GosymUnknownFileError *self) {
    return gosym_unknown_file_error_error(*self, error_allocator());
}

#define GOSYM_SIG_ERROR(IN, OUT) OUT(Str)
#define GOSYM_FILE_ERROR_METHODS(M, T)                                                 \
    M(T, Error, gosym_file_error_m_error, GOSYM_SIG_ERROR)

BURROW_METHODS_DEFINE(GosymUnknownFileError, GOSYM_FILE_ERROR_METHODS);

const Type burrow_type_GosymUnknownFileError = {
    {(const Byte *)"UnknownFileError", 16},
    {(const Byte *)"debug/gosym", 11},
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    (uint16_t)(sizeof(burrow__methods_GosymUnknownFileError) /
               sizeof(burrow__methods_GosymUnknownFileError[0])),
    NULL,
    burrow__methods_GosymUnknownFileError,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str gosym_file_error_message(const void *self) {
    return ((const GosymFileErrorBox *)self)->message;
}

static bool gosym_file_error_is(const void *self, Error target);
static Error gosym_file_error_clone(const void *self, Alloc *a);

static const ErrorVT gosym_file_error_vt = {
    &burrow_type_GosymUnknownFileError,
    gosym_file_error_message,
    NULL,
    NULL,
    gosym_file_error_is,
    NULL,
    gosym_file_error_clone,
};

/* Go compares two of them with ==, which for a string is the text. */
static bool gosym_file_error_is(const void *self, Error target) {
    if (target.vt != &gosym_file_error_vt || target.data == NULL)
        return false;
    return str_eq(((const GosymFileErrorBox *)self)->name,
                  ((const GosymFileErrorBox *)target.data)->name);
}

/* UnknownFileError(file), in one allocation: the box, the name and the text. */
static Error gosym_file_error_build(Alloc *a, Str file) {
    Int tlen = gosym_file_error_put(NULL, file);
    size_t size = sizeof(GosymFileErrorBox) + (size_t)file.len + (size_t)tlen;
    GosymFileErrorBox *b =
        (GosymFileErrorBox *)mem_alloc_nozero(a, size, _Alignof(GosymFileErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->name = str_from_bytes(p, gosym_put(p, 0, file));
    p += file.len;
    gosym_file_error_put(p, b->name);
    b->message = str_from_bytes(p, tlen);
    return (Error){&gosym_file_error_vt, b};
}

static Error gosym_file_error_clone(const void *self, Alloc *a) {
    return gosym_file_error_build(a, ((const GosymFileErrorBox *)self)->name);
}

/* UnknownLineError, with the public struct first. */
typedef struct GosymLineErrorBox {
    GosymUnknownLineError e;
    Str message;
} GosymLineErrorBox;

static Int gosym_line_error_put(Byte *p, const GosymUnknownLineError *e) {
    Int n = gosym_put(p, 0, BURROW_S("no code at "));
    n = gosym_put(p, n, e->file);
    n = gosym_put(p, n, BURROW_S(":"));
    return gosym_put_int(p, n, e->line, false);
}

Str gosym_unknown_line_error_error(const GosymUnknownLineError *e, Alloc *a) {
    if (e == NULL)
        gosym_nil_panic();
    Int n = gosym_line_error_put(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    gosym_line_error_put(p, e);
    return str_from_bytes(p, n);
}

static Str gosym_line_error_m_error(GosymUnknownLineError *self) {
    return gosym_unknown_line_error_error(self, error_allocator());
}

#define GOSYM_LINE_ERROR_METHODS(M, T)                                                 \
    M(T, Error, gosym_line_error_m_error, GOSYM_SIG_ERROR)

BURROW_METHODS_DEFINE(GosymUnknownLineError, GOSYM_LINE_ERROR_METHODS);

const Type burrow_type_GosymUnknownLineError = {
    {(const Byte *)"UnknownLineError", 16},
    {(const Byte *)"debug/gosym", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(GosymUnknownLineError),
    (uint16_t)_Alignof(GosymUnknownLineError),
    0,
    (uint16_t)(sizeof(burrow__methods_GosymUnknownLineError) /
               sizeof(burrow__methods_GosymUnknownLineError[0])),
    NULL,
    burrow__methods_GosymUnknownLineError,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str gosym_line_error_message(const void *self) {
    return ((const GosymLineErrorBox *)self)->message;
}

static Error gosym_line_error_clone(const void *self, Alloc *a);

static const ErrorVT gosym_line_error_vt = {
    &burrow_type_GosymUnknownLineError,
    gosym_line_error_message,
    NULL,
    NULL,
    NULL,
    NULL,
    gosym_line_error_clone,
};

/* &UnknownLineError{file, line}: the box, the file's name and the text. */
static Error gosym_line_error_build(Alloc *a, Str file, Int line) {
    GosymUnknownLineError e = {file, line};
    Int tlen = gosym_line_error_put(NULL, &e);
    size_t size = sizeof(GosymLineErrorBox) + (size_t)file.len + (size_t)tlen;
    GosymLineErrorBox *b =
        (GosymLineErrorBox *)mem_alloc_nozero(a, size, _Alignof(GosymLineErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.file = str_from_bytes(p, gosym_put(p, 0, file));
    b->e.line = line;
    p += file.len;
    gosym_line_error_put(p, &b->e);
    b->message = str_from_bytes(p, tlen);
    return (Error){&gosym_line_error_vt, b};
}

static Error gosym_line_error_clone(const void *self, Alloc *a) {
    const GosymUnknownLineError *e = (const GosymUnknownLineError *)self;
    return gosym_line_error_build(a, e->file, e->line);
}

/* DecodingError, with the public struct first. */
typedef struct GosymDecodingErrorBox {
    GosymDecodingError e;
    Str message;
} GosymDecodingErrorBox;

static Int gosym_decoding_error_put(Byte *p, const GosymDecodingError *e) {
    Int n = gosym_put(p, 0, e->msg);
    if (e->has_val) {
        n = gosym_put(p, n, BURROW_S(" '"));
        n = gosym_put(p, n, e->val);
        n = gosym_put(p, n, BURROW_S("'"));
    }
    n = gosym_put(p, n, BURROW_S(" at byte "));
    return gosym_put_int(p, n, e->off, true);
}

Str gosym_decoding_error_error(const GosymDecodingError *e, Alloc *a) {
    if (e == NULL)
        gosym_nil_panic();
    Int n = gosym_decoding_error_put(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    gosym_decoding_error_put(p, e);
    return str_from_bytes(p, n);
}

static Str gosym_decoding_error_m_error(GosymDecodingError *self) {
    return gosym_decoding_error_error(self, error_allocator());
}

#define GOSYM_DECODING_ERROR_METHODS(M, T)                                             \
    M(T, Error, gosym_decoding_error_m_error, GOSYM_SIG_ERROR)

BURROW_METHODS_DEFINE(GosymDecodingError, GOSYM_DECODING_ERROR_METHODS);

const Type burrow_type_GosymDecodingError = {
    {(const Byte *)"DecodingError", 13},
    {(const Byte *)"debug/gosym", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(GosymDecodingError),
    (uint16_t)_Alignof(GosymDecodingError),
    0,
    (uint16_t)(sizeof(burrow__methods_GosymDecodingError) /
               sizeof(burrow__methods_GosymDecodingError[0])),
    NULL,
    burrow__methods_GosymDecodingError,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Str gosym_decoding_error_message(const void *self) {
    return ((const GosymDecodingErrorBox *)self)->message;
}

static Error gosym_decoding_error_clone(const void *self, Alloc *a);

static const ErrorVT gosym_decoding_error_vt = {
    &burrow_type_GosymDecodingError,
    gosym_decoding_error_message,
    NULL,
    NULL,
    NULL,
    NULL,
    gosym_decoding_error_clone,
};

/* One allocation: the box, then the message, the value and the text. */
static Error gosym_decoding_error_build(Alloc *a, Int off, Str msg, Str val,
                                        bool has_val) {
    if (!has_val)
        val = BURROW_STR_EMPTY;
    GosymDecodingError e = {off, msg, val, has_val};
    Int tlen = gosym_decoding_error_put(NULL, &e);
    size_t size = sizeof(GosymDecodingErrorBox) + (size_t)msg.len + (size_t)val.len +
                  (size_t)tlen;
    GosymDecodingErrorBox *b = (GosymDecodingErrorBox *)mem_alloc_nozero(
        a, size, _Alignof(GosymDecodingErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.off = off;
    b->e.has_val = has_val;
    b->e.msg = str_from_bytes(p, msg.len);
    p += gosym_put(p, 0, msg);
    b->e.val = str_from_bytes(p, val.len);
    p += gosym_put(p, 0, val);
    gosym_decoding_error_put(p, &b->e);
    b->message = str_from_bytes(p, tlen);
    return (Error){&gosym_decoding_error_vt, b};
}

static Error gosym_decoding_error_clone(const void *self, Alloc *a) {
    const GosymDecodingError *e = (const GosymDecodingError *)self;
    return gosym_decoding_error_build(a, e->off, e->msg, e->val, e->has_val);
}

/* &DecodingError{off, msg, nil}. */
static Error gosym_decoding_error_nil(Int off, const char *msg) {
    return gosym_decoding_error_build(error_allocator(), off, str_from_cstr(msg),
                                      BURROW_STR_EMPTY, false);
}

/* &DecodingError{off, msg, v}, where v is an int or a byte, which %v prints
 * in decimal. */
static Error gosym_decoding_error_int(Int off, const char *msg, int64_t v) {
    Byte buf[24];
    Str val = str_from_bytes(buf, gosym_put_int(buf, 0, v, false));
    return gosym_decoding_error_build(error_allocator(), off, str_from_cstr(msg), val,
                                      true);
}

/* ------------------------------------------------------- the old line table */

/* Go adds and subtracts lines as ints and lets them wrap. */
static Int gosym_add(Int x, Int y) {
    return (Int)((Uint)x + (Uint)y);
}

static Int gosym_sub(Int x, Int y) {
    return (Int)((Uint)x - (Uint)y);
}

/* LineTable.parse, for the tables from before Go 1.2: a byte code for each
 * step, which moves the line up or down or the pc on. It stops at target_pc,
 * at target_line or at the end, and gives what is left of the table. */
static Slice gosym_parse(const GosymLineTable *t, uint64_t target_pc, Int target_line,
                         uint64_t *pc_out, Int *line_out) {
    Slice b = t->data;
    uint64_t pc = t->pc;
    Int line = t->line;
    while (pc <= target_pc && line != target_line && b.len > 0) {
        Byte code = ((const Byte *)b.p)[0];
        b = gosym_from(b, 1);
        if (code == 0) {
            if (b.len < 4) {
                b.len = 0;
            } else {
                const Byte *p = (const Byte *)b.p;
                uint32_t val = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                               (uint32_t)p[2] << 8 | p[3];
                b = gosym_from(b, 4);
                line = gosym_add(line, (Int)(Uint)val);
            }
        } else if (code <= 64) {
            line = gosym_add(line, code);
        } else if (code <= 128) {
            line = gosym_sub(line, code - 64);
        } else {
            pc += (uint64_t)(code - 128);
            continue;
        }
        pc++;
    }
    *pc_out = pc;
    *line_out = line;
    return b;
}

/* LineTable.slice: the rest of the table from pc on, as a table of its own.
 * It goes in a, the allocator of the GosymTable that asks for it. */
static GosymLineTable *gosym_slice(const GosymLineTable *t, uint64_t pc, Alloc *a);

/* ----------------------------------------------------------- the pclntab */

#define GOSYM_GO12_MAGIC 0xfffffffbU
#define GOSYM_GO116_MAGIC 0xfffffffaU
#define GOSYM_GO118_MAGIC 0xfffffff0U
#define GOSYM_GO120_MAGIC 0xfffffff1U

/* offset(word) in parsePclnTab. Go computes 8+word*ptrsize as a uint32. */
static uint64_t gosym_offset(const GosymLineTable *t, uint32_t word) {
    return gosym_uintptr(t, gosym_from(t->data, (uint32_t)(8U + word * t->ptrsize)));
}

static Slice gosym_data(const GosymLineTable *t, uint32_t word) {
    return gosym_from(t->data, gosym_offset(t, word));
}

/* LineTable.functabFieldSize. */
static uint64_t gosym_field_size(const GosymLineTable *t) {
    if (t->version >= GOSYM_VER118)
        return 4;
    return t->ptrsize;
}

/* The functab holds nfunctab pairs and one more pc. */
static uint64_t gosym_functab_size(const GosymLineTable *t) {
    return ((uint64_t)t->nfunctab * 2 + 1) * gosym_field_size(t);
}

/* The body of parsePclnTab, which runs with the lock held, the version set to
 * Go 1.1 and a recover around it. Go sets the version and the fields one at a
 * time, so a table that ends too soon is left with some of them set, and so
 * is this one. */
static void gosym_parse_pclntab_body(void *ctx) {
    GosymLineTable *t = (GosymLineTable *)ctx;
    const Byte *p = (const Byte *)t->data.p;
    if (t->data.len < 16 || p[4] != 0 || p[5] != 0 ||
        (p[6] != 1 && p[6] != 2 && p[6] != 4) || (p[7] != 4 && p[7] != 8))
        return;
    uint32_t le =
        (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
    uint32_t be =
        (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    int version = 0;
    if (le == GOSYM_GO12_MAGIC) {
        t->big_endian = false;
        version = GOSYM_VER12;
    } else if (be == GOSYM_GO12_MAGIC) {
        t->big_endian = true;
        version = GOSYM_VER12;
    } else if (le == GOSYM_GO116_MAGIC) {
        t->big_endian = false;
        version = GOSYM_VER116;
    } else if (be == GOSYM_GO116_MAGIC) {
        t->big_endian = true;
        version = GOSYM_VER116;
    } else if (le == GOSYM_GO118_MAGIC) {
        t->big_endian = false;
        version = GOSYM_VER118;
    } else if (be == GOSYM_GO118_MAGIC) {
        t->big_endian = true;
        version = GOSYM_VER118;
    } else if (le == GOSYM_GO120_MAGIC) {
        t->big_endian = false;
        version = GOSYM_VER120;
    } else if (be == GOSYM_GO120_MAGIC) {
        t->big_endian = true;
        version = GOSYM_VER120;
    } else {
        return;
    }
    t->version = version;
    t->quantum = p[6];
    t->ptrsize = p[7];

    if (version == GOSYM_VER118 || version == GOSYM_VER120) {
        t->nfunctab = (uint32_t)gosym_offset(t, 0);
        t->nfiletab = (uint32_t)gosym_offset(t, 1);
        /* The start of the text, not the one in the table, which may not have
         * been relocated. */
        t->text_start = t->pc;
        t->funcnametab = gosym_data(t, 3);
        t->cutab = gosym_data(t, 4);
        t->filetab = gosym_data(t, 5);
        t->pctab = gosym_data(t, 6);
        t->funcdata = gosym_data(t, 7);
        t->functab = gosym_data(t, 7);
        t->functab = gosym_to(t->functab, gosym_functab_size(t));
    } else if (version == GOSYM_VER116) {
        t->nfunctab = (uint32_t)gosym_offset(t, 0);
        t->nfiletab = (uint32_t)gosym_offset(t, 1);
        t->funcnametab = gosym_data(t, 2);
        t->cutab = gosym_data(t, 3);
        t->filetab = gosym_data(t, 4);
        t->pctab = gosym_data(t, 5);
        t->funcdata = gosym_data(t, 6);
        t->functab = gosym_data(t, 6);
        t->functab = gosym_to(t->functab, gosym_functab_size(t));
    } else {
        t->nfunctab = (uint32_t)gosym_uintptr(t, gosym_from(t->data, 8));
        t->funcdata = t->data;
        t->funcnametab = t->data;
        t->functab = gosym_from(t->data, 8U + t->ptrsize);
        t->pctab = t->data;
        uint64_t size = gosym_functab_size(t);
        uint32_t fileoff = gosym_u32(t, gosym_from(t->functab, size));
        t->functab = gosym_to(t->functab, size);
        t->filetab = gosym_from(t->data, fileoff);
        t->nfiletab = gosym_u32(t, t->filetab);
        t->filetab = gosym_to(t->filetab, (uint32_t)(t->nfiletab * 4U));
    }
}

/* LineTable.parsePclnTab, which works out the format the first time and
 * remembers it. */
static void gosym_parse_pclntab(GosymLineTable *t) {
    if (t == NULL)
        gosym_nil_panic();
    sync_mutex_lock(&t->mu);
    if (t->version == GOSYM_VER_UNKNOWN) {
        /* A table that panics while it is read is taken for Go 1.1's. */
        t->version = GOSYM_VER11;
        (void)gosym_recover(gosym_parse_pclntab_body, t);
    }
    sync_mutex_unlock(&t->mu);
}

/* LineTable.isGo12. */
static bool gosym_is_go12(GosymLineTable *t) {
    gosym_parse_pclntab(t);
    return t->version >= GOSYM_VER12;
}

/* funcTab.uint. */
static uint64_t gosym_ft_uint(const GosymLineTable *t, Slice b) {
    if (gosym_field_size(t) == 4)
        return gosym_u32(t, b);
    return gosym_u64(t, b);
}

/* funcTab.pc: the pc of the i'th function. */
static uint64_t gosym_ft_pc(const GosymLineTable *t, uint64_t i) {
    uint64_t u = gosym_ft_uint(t, gosym_from(t->functab, 2 * i * gosym_field_size(t)));
    if (t->version >= GOSYM_VER118)
        u += t->text_start;
    return u;
}

/* funcTab.funcOff: where the i'th function's _func is in funcdata. */
static uint64_t gosym_ft_func_off(const GosymLineTable *t, uint64_t i) {
    return gosym_ft_uint(t, gosym_from(t->functab, (2 * i + 1) * gosym_field_size(t)));
}

/* LineTable.funcData: the i'th function's _func. */
static Slice gosym_func_data(const GosymLineTable *t, uint32_t i) {
    return gosym_from(t->funcdata, gosym_ft_func_off(t, i));
}

/* funcData.entryPC. */
static uint64_t gosym_entry_pc(const GosymLineTable *t, Slice f) {
    if (t->version >= GOSYM_VER118)
        return (uint64_t)gosym_u32(t, f) + t->text_start;
    return gosym_uintptr(t, f);
}

/* funcData.field: the n'th field of the _func, after the entry. */
enum {
    GOSYM_FIELD_NAME_OFF = 1,
    GOSYM_FIELD_DEFERRETURN = 3,
    GOSYM_FIELD_PCFILE = 5,
    GOSYM_FIELD_PCLN = 6,
    GOSYM_FIELD_CU_OFFSET = 8,
};

static uint32_t gosym_field(const GosymLineTable *t, Slice f, uint32_t n) {
    uint32_t sz0 = t->version >= GOSYM_VER118 ? 4U : t->ptrsize;
    return gosym_u32(t, gosym_from(f, (uint32_t)(sz0 + (n - 1) * 4U)));
}

/* LineTable.findFunc: the _func of the function that has pc, through out, or
 * false for a pc outside all of them. */
static bool gosym_find_func(const GosymLineTable *t, uint64_t pc, Slice *out) {
    if (pc < gosym_ft_pc(t, 0) || pc >= gosym_ft_pc(t, t->nfunctab))
        return false;
    /* sort.Search for the first function that starts after pc. */
    int64_t i = 0;
    int64_t j = t->nfunctab;
    while (i < j) {
        int64_t h = (int64_t)((uint64_t)(i + j) >> 1);
        if (!(gosym_ft_pc(t, (uint64_t)h) > pc))
            i = h + 1;
        else
            j = h;
    }
    *out = gosym_func_data(t, (uint32_t)(i - 1));
    return true;
}

/* LineTable.readvarint. Go shifts by a uint32 that keeps growing, and a shift
 * of 32 or more gives 0. */
static uint32_t gosym_readvarint(Slice *pp) {
    uint32_t v = 0;
    Slice p = *pp;
    for (uint32_t shift = 0;; shift += 7) {
        if (p.len == 0)
            gosym_bounds_panic();
        Byte b = ((const Byte *)p.p)[0];
        p = gosym_from(p, 1);
        if (shift < 32)
            v |= ((uint32_t)b & 0x7FU) << shift;
        if ((b & 0x80) == 0)
            break;
    }
    *pp = p;
    return v;
}

/* LineTable.step: the next pc and value of a pc-value table, or false at its
 * end. */
static bool gosym_step(const GosymLineTable *t, Slice *p, uint64_t *pc, int32_t *val,
                       bool first) {
    uint32_t uvdelta = gosym_readvarint(p);
    if (uvdelta == 0 && !first)
        return false;
    if ((uvdelta & 1) != 0)
        uvdelta = ~(uvdelta >> 1);
    else
        uvdelta >>= 1;
    uint32_t pcdelta = gosym_readvarint(p) * t->quantum;
    *pc += pcdelta;
    *val = (int32_t)((uint32_t)*val + uvdelta);
    return true;
}

/* LineTable.pcvalue: the value the table at off gives for targetpc, in the
 * function that starts at entry, or -1. */
static int32_t gosym_pcvalue(const GosymLineTable *t, uint32_t off, uint64_t entry,
                             uint64_t targetpc) {
    Slice p = gosym_from(t->pctab, off);
    int32_t val = -1;
    uint64_t pc = entry;
    while (gosym_step(t, &p, &pc, &val, pc == entry)) {
        if (targetpc < pc)
            return val;
    }
    return -1;
}

static bool gosym_has_cutab(const GosymLineTable *t) {
    return t->version == GOSYM_VER116 || t->version == GOSYM_VER118 ||
           t->version == GOSYM_VER120;
}

/* LineTable.findFileLine: the first pc of the function at entry that is in
 * file filenum on line line, or 0. */
static uint64_t gosym_find_file_line(const GosymLineTable *t, uint64_t entry,
                                     uint32_t filetab, uint32_t linetab,
                                     int32_t filenum, int32_t line, Slice cutab) {
    if (filetab == 0 || linetab == 0)
        return 0;
    Slice fp = gosym_from(t->pctab, filetab);
    Slice fl = gosym_from(t->pctab, linetab);
    int32_t file_val = -1;
    uint64_t file_pc = entry;
    int32_t line_val = -1;
    uint64_t line_pc = entry;
    uint64_t file_start_pc = file_pc;
    while (gosym_step(t, &fp, &file_pc, &file_val, file_pc == entry)) {
        int32_t file_index = file_val;
        if (gosym_has_cutab(t)) {
            int32_t at = (int32_t)((uint32_t)file_val * 4U);
            file_index = (int32_t)gosym_u32(t, gosym_from32(cutab, at));
        }
        if (file_index == filenum && file_start_pc < file_pc) {
            /* The file is the one from file_start_pc up to file_pc, so look
             * for the line there. */
            uint64_t line_start_pc = line_pc;
            while (line_pc < file_pc &&
                   gosym_step(t, &fl, &line_pc, &line_val, line_pc == entry)) {
                if (line_val == line) {
                    if (file_start_pc <= line_start_pc)
                        return line_start_pc;
                    if (file_start_pc < line_pc)
                        return file_start_pc;
                }
                line_start_pc = line_pc;
            }
        }
        file_start_pc = file_pc;
    }
    return 0;
}

typedef struct GosymPCCtx {
    GosymLineTable *t;
    uint64_t pc;
    Int line;
    Str file;
} GosymPCCtx;

static void gosym_go12_pc_to_line_body(void *ctx) {
    GosymPCCtx *c = (GosymPCCtx *)ctx;
    Slice f = {NULL, 0, 0, TYPE_BYTE};
    if (!gosym_find_func(c->t, c->pc, &f)) {
        c->line = -1;
        return;
    }
    uint64_t entry = gosym_entry_pc(c->t, f);
    uint32_t linetab = gosym_field(c->t, f, GOSYM_FIELD_PCLN);
    c->line = gosym_pcvalue(c->t, linetab, entry, c->pc);
}

/* LineTable.go12PCToLine. */
static Int gosym_go12_pc_to_line(GosymLineTable *t, uint64_t pc) {
    GosymPCCtx c = {t, pc, 0, BURROW_STR_EMPTY};
    if (!gosym_recover(gosym_go12_pc_to_line_body, &c))
        return -1;
    return c.line;
}

static void gosym_go12_pc_to_file_body(void *ctx) {
    GosymPCCtx *c = (GosymPCCtx *)ctx;
    const GosymLineTable *t = c->t;
    Slice f = {NULL, 0, 0, TYPE_BYTE};
    if (!gosym_find_func(t, c->pc, &f))
        return;
    uint64_t entry = gosym_entry_pc(t, f);
    uint32_t filetab = gosym_field(t, f, GOSYM_FIELD_PCFILE);
    int32_t fno = gosym_pcvalue(t, filetab, entry, c->pc);
    if (t->version == GOSYM_VER12) {
        if (fno <= 0)
            return;
        int32_t at = (int32_t)(4U * (uint32_t)fno);
        c->file =
            gosym_string_from(t->funcdata, gosym_u32(t, gosym_from32(t->filetab, at)));
        return;
    }
    /* From Go 1.16 on, 0 is a file too. */
    if (fno < 0)
        return;
    uint32_t cuoff = gosym_field(t, f, GOSYM_FIELD_CU_OFFSET);
    uint32_t at = (uint32_t)((cuoff + (uint32_t)fno) * 4U);
    uint32_t fnoff = gosym_u32(t, gosym_from(t->cutab, at));
    if (fnoff != UINT32_MAX)
        c->file = gosym_string_from(t->filetab, fnoff);
}

/* LineTable.go12PCToFile. The name points into the table's bytes. */
static Str gosym_go12_pc_to_file(GosymLineTable *t, uint64_t pc) {
    GosymPCCtx c = {t, pc, 0, BURROW_STR_EMPTY};
    if (!gosym_recover(gosym_go12_pc_to_file_body, &c))
        return BURROW_STR_EMPTY;
    return c.file;
}

BURROW_NORETURN static void gosym_oom_panic(void) {
    runtime_panic(BURROW_S("debug/gosym: out of memory"));
}

typedef struct GosymFileMapCtx {
    GosymLineTable *t;
    Map *m;
} GosymFileMapCtx;

static void gosym_file_map_body(void *ctx) {
    GosymFileMapCtx *c = (GosymFileMapCtx *)ctx;
    const GosymLineTable *t = c->t;
    c->m = map_make(t->a, TYPE_STRING, TYPE_UINT32, 0);
    if (c->m == NULL)
        gosym_oom_panic();
    if (t->version == GOSYM_VER12) {
        for (uint32_t i = 1; i < t->nfiletab; i++) {
            uint32_t off = gosym_u32(t, gosym_from(t->filetab, (uint32_t)(4U * i)));
            Str s = gosym_string_from(t->funcdata, off);
            if (!map_set(c->m, &s, &i))
                gosym_oom_panic();
        }
        return;
    }
    uint32_t pos = 0;
    for (uint32_t i = 0; i < t->nfiletab; i++) {
        Str s = gosym_string_from(t->filetab, pos);
        if (!map_set(c->m, &s, &pos))
            gosym_oom_panic();
        pos += (uint32_t)(s.len + 1);
    }
}

/* LineTable.initFileMap: the map from each file's name to its number, made
 * once. False is a table that panicked while it was read, which leaves no
 * map, so the next call tries again, as Go's does. */
static bool gosym_init_file_map(GosymLineTable *t) {
    sync_mutex_lock(&t->mu);
    bool ok = true;
    if (t->file_map == NULL) {
        GosymFileMapCtx c = {t, NULL};
        ok = gosym_recover(gosym_file_map_body, &c);
        if (ok)
            t->file_map = c.m;
        else
            map_free(c.m);
    }
    sync_mutex_unlock(&t->mu);
    return ok;
}

typedef struct GosymLineToPCCtx {
    GosymLineTable *t;
    Str file;
    Int line;
    uint64_t pc;
} GosymLineToPCCtx;

static void gosym_go12_line_to_pc_body(void *ctx) {
    GosymLineToPCCtx *c = (GosymLineToPCCtx *)ctx;
    GosymLineTable *t = c->t;
    if (!gosym_init_file_map(t))
        return;
    uint32_t filenum = 0;
    if (!map_get2(t->file_map, &c->file, &filenum))
        return;
    /* Every function, since nothing says which ones have code from a file. */
    Slice cutab = {NULL, 0, 0, TYPE_BYTE};
    for (uint32_t i = 0; i < t->nfunctab; i++) {
        Slice f = gosym_func_data(t, i);
        uint64_t entry = gosym_entry_pc(t, f);
        uint32_t filetab = gosym_field(t, f, GOSYM_FIELD_PCFILE);
        uint32_t linetab = gosym_field(t, f, GOSYM_FIELD_PCLN);
        if (gosym_has_cutab(t)) {
            uint32_t cu = gosym_field(t, f, GOSYM_FIELD_CU_OFFSET);
            /* A function with no compilation unit, which the linker made. */
            if (cu == UINT32_MAX)
                continue;
            cutab = gosym_from(t->cutab, (uint32_t)(cu * 4U));
        }
        uint64_t pc = gosym_find_file_line(t, entry, filetab, linetab, (int32_t)filenum,
                                           (int32_t)c->line, cutab);
        if (pc != 0) {
            c->pc = pc;
            return;
        }
    }
}

/* LineTable.go12LineToPC. */
static uint64_t gosym_go12_line_to_pc(GosymLineTable *t, Str file, Int line) {
    GosymLineToPCCtx c = {t, file, line, 0};
    if (!gosym_recover(gosym_go12_line_to_pc_body, &c))
        return 0;
    return c.pc;
}

typedef struct GosymMapFilesCtx {
    GosymLineTable *t;
    Map *files;
    GosymObj *obj;
} GosymMapFilesCtx;

static void gosym_go12_map_files_body(void *ctx) {
    GosymMapFilesCtx *c = (GosymMapFilesCtx *)ctx;
    if (!gosym_init_file_map(c->t))
        return;
    MapIter it = map_iter(c->t->file_map);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        if (!map_set(c->files, k, &c->obj))
            gosym_oom_panic();
    }
}

/* LineTable.go12MapFiles: every file of the table, each to obj. */
static void gosym_go12_map_files(GosymLineTable *t, Map *files, GosymObj *obj) {
    GosymMapFilesCtx c = {t, files, obj};
    (void)gosym_recover(gosym_go12_map_files_body, &c);
}

typedef struct GosymFuncsCtx {
    GosymLineTable *t;
    Alloc *a;
    Slice funcs;
} GosymFuncsCtx;

static void gosym_go12_funcs_body(void *ctx) {
    GosymFuncsCtx *c = (GosymFuncsCtx *)ctx;
    GosymLineTable *t = c->t;
    uint64_t n = t->nfunctab;
    if (n == 0)
        return;
    GosymFunc *funcs =
        (GosymFunc *)mem_alloc(c->a, (size_t)n * sizeof *funcs, _Alignof(GosymFunc));
    GosymSym *syms =
        (GosymSym *)mem_alloc(c->a, (size_t)n * sizeof *syms, _Alignof(GosymSym));
    if (funcs == NULL || syms == NULL)
        gosym_oom_panic();
    for (uint64_t i = 0; i < n; i++) {
        GosymFunc *f = &funcs[i];
        f->entry = gosym_ft_pc(t, i);
        f->end = gosym_ft_pc(t, i + 1);
        Slice info = gosym_func_data(t, (uint32_t)i);
        f->line_table = t;
        f->frame_size = (Int)gosym_field(t, info, GOSYM_FIELD_DEFERRETURN);
        GosymSym *s = &syms[i];
        s->value = f->entry;
        s->type = 'T';
        s->name = gosym_string_from(t->funcnametab,
                                    gosym_field(t, info, GOSYM_FIELD_NAME_OFF));
        s->go_type = 0;
        s->func = f;
        s->go_version = t->version;
        f->sym = s;
    }
    c->funcs = (Slice){funcs, (Int)n, (Int)n, TYPE_GOSYM_FUNC};
}

/* LineTable.go12Funcs: a function for each entry of the functab, in ar, or
 * none for a table that panics while it is read. */
static Slice gosym_go12_funcs(GosymLineTable *t, Arena *ar) {
    Slice none = {NULL, 0, 0, TYPE_GOSYM_FUNC};
    /* A functab too short for nfunctab panics in Go before the end, once it
     * has made the two lists. Their size comes from nfunctab, which the table
     * says, so this stops before making them. */
    uint64_t n = t->nfunctab;
    if (n > 0 && gosym_functab_size(t) > (uint64_t)t->functab.len)
        return none;
    ArenaMark mark = arena_mark(ar);
    GosymFuncsCtx c = {t, arena_allocator(ar), none};
    if (!gosym_recover(gosym_go12_funcs_body, &c)) {
        arena_release(ar, mark);
        return none;
    }
    return c.funcs;
}

/* ------------------------------------------------------- LineTable's API */

GosymLineTable *gosym_new_line_table(Alloc *a, Slice data, uint64_t text) {
    GosymLineTable *t =
        (GosymLineTable *)mem_alloc(a, sizeof *t, _Alignof(GosymLineTable));
    if (t == NULL)
        return NULL;
    t->data = data;
    t->pc = text;
    t->line = 0;
    arena_init(&t->arena, a, 0);
    t->a = arena_allocator(&t->arena);
    t->parent = a;
    return t;
}

void gosym_line_table_free(GosymLineTable *t) {
    /* The tables a GosymTable slices off have no parent and are the table's
     * to free. */
    if (t == NULL || t->parent == NULL)
        return;
    Alloc *a = t->parent;
    arena_free(&t->arena);
    mem_free(a, t, sizeof *t, _Alignof(GosymLineTable));
}

static GosymLineTable *gosym_slice(const GosymLineTable *t, uint64_t pc, Alloc *a) {
    uint64_t start = 0;
    Int line = 0;
    Slice data = gosym_parse(t, pc, -1, &start, &line);
    GosymLineTable *s =
        (GosymLineTable *)mem_alloc(a, sizeof *s, _Alignof(GosymLineTable));
    if (s == NULL)
        gosym_oom_panic();
    s->data = data;
    s->pc = start;
    s->line = line;
    s->a = a;
    return s;
}

Int gosym_line_table_pc_to_line(GosymLineTable *t, uint64_t pc) {
    if (gosym_is_go12(t))
        return gosym_go12_pc_to_line(t, pc);
    uint64_t end = 0;
    Int line = 0;
    (void)gosym_parse(t, pc, -1, &end, &line);
    return line;
}

uint64_t gosym_line_table_line_to_pc(GosymLineTable *t, Int line, uint64_t maxpc) {
    if (gosym_is_go12(t))
        return 0;
    uint64_t pc = 0;
    Int line1 = 0;
    (void)gosym_parse(t, maxpc, line, &pc, &line1);
    if (line1 != line)
        return 0;
    /* The pc is one past the line's last step. */
    return pc - 1;
}

/* ------------------------------------------------------ the symbol table */

static const Byte gosym_le_symtab[] = {0xFD, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00};
static const Byte gosym_be_symtab[] = {0xFF, 0xFF, 0xFF, 0xFD, 0x00, 0x00, 0x00};
static const Byte gosym_old_le_symtab[] = {0xFE, 0xFF, 0xFF, 0xFF, 0x00, 0x00};

/* sym, an entry as walksymtab reads it. name points into the table. */
typedef struct GosymRawSym {
    uint64_t value;
    uint64_t gotype;
    Byte typ;
    Byte *name;
    Int name_len;
} GosymRawSym;

typedef void (*GosymWalkFn)(void *ctx, const GosymRawSym *s);

static bool gosym_has_prefix(Slice data, const Byte *prefix, size_t n) {
    return (size_t)data.len >= n && memcmp(data.p, prefix, n) == 0;
}

static uint64_t gosym_order_uint(bool big, const Byte *p, Int n) {
    uint64_t v = 0;
    for (Int i = 0; i < n; i++) {
        Int k = big ? i : n - 1 - i;
        v = v << 8 | p[k];
    }
    return v;
}

static Error gosym_eof(Int off) {
    return gosym_decoding_error_nil(off, "unexpected EOF");
}

/* walksymtab: calls fn for each entry of the symbol table in data, in the
 * format of Go 1.0, of the time between it and Go 1.1, or of Go 1.1 and 1.2.
 * An empty table is fine. */
static Error gosym_walk(Slice data, GosymWalkFn fn, void *ctx) {
    if (data.len == 0)
        return BURROW_NO_ERROR;
    bool big = true;
    bool new_table = false;
    if (gosym_has_prefix(data, gosym_old_le_symtab, sizeof gosym_old_le_symtab)) {
        /* Go 1.0's format, little endian, from before Go 1.1. */
        data = gosym_from(data, 6);
        big = false;
    } else if (gosym_has_prefix(data, gosym_be_symtab, sizeof gosym_be_symtab)) {
        new_table = true;
    } else if (gosym_has_prefix(data, gosym_le_symtab, sizeof gosym_le_symtab)) {
        new_table = true;
        big = false;
    }
    Int ptrsz = 0;
    if (new_table) {
        if (data.len < 8)
            return gosym_eof(data.len);
        ptrsz = ((const Byte *)data.p)[7];
        if (ptrsz != 4 && ptrsz != 8)
            return gosym_decoding_error_int(7, "invalid pointer size", ptrsz);
        data = gosym_from(data, 8);
    }
    /* The entry before carries over: a new table entry without a Go type
     * keeps the one before it. */
    GosymRawSym s = {0, 0, 0, NULL, 0};
    Byte *p = (Byte *)data.p;
    Int n = data.len;
    while (n >= 4) {
        Byte typ = 0;
        if (new_table) {
            /* The type, the value and the Go type. */
            typ = p[0] & 0x3F;
            bool wide_value = (p[0] & 0x40) != 0;
            bool go_type = (p[0] & 0x80) != 0;
            if (typ < 26)
                typ = (Byte)(typ + 'A');
            else
                typ = (Byte)(typ + 'a' - 26);
            s.typ = typ;
            p++;
            n--;
            if (wide_value) {
                if (n < ptrsz)
                    return gosym_eof(data.len);
                s.value = gosym_order_uint(big, p, ptrsz);
                p += ptrsz;
                n -= ptrsz;
            } else {
                s.value = 0;
                unsigned shift = 0;
                while (n > 0 && (p[0] & 0x80) != 0) {
                    if (shift < 64)
                        s.value |= (uint64_t)(p[0] & 0x7F) << shift;
                    shift += 7;
                    p++;
                    n--;
                }
                if (n == 0)
                    return gosym_eof(data.len);
                if (shift < 64)
                    s.value |= (uint64_t)p[0] << shift;
                p++;
                n--;
            }
            if (go_type) {
                if (n < ptrsz)
                    return gosym_eof(data.len);
                s.gotype = gosym_order_uint(big, p, ptrsz);
                p += ptrsz;
                n -= ptrsz;
            }
        } else {
            /* The value and the type. */
            s.value = gosym_order_uint(big, p, 4);
            if (n < 5)
                return gosym_eof(data.len);
            typ = p[4];
            if ((typ & 0x80) == 0)
                return gosym_decoding_error_int(data.len - n + 4, "bad symbol type",
                                                typ);
            typ &= 0x7F;
            s.typ = typ;
            p += 5;
            n -= 5;
        }

        /* The name, up to a NUL, or for a path up to two NULs on an even
         * byte. Without them it runs to the end. */
        Int i = 0;
        Int nnul = 0;
        for (i = 0; i < n; i++) {
            if (p[i] == 0) {
                nnul = 1;
                break;
            }
        }
        if (typ == 'z' || typ == 'Z') {
            p += i + nnul;
            n -= i + nnul;
            for (i = 0; i + 2 <= n; i += 2) {
                if (p[i] == 0 && p[i + 1] == 0) {
                    nnul = 2;
                    break;
                }
            }
        }
        if (n < i + nnul)
            return gosym_eof(data.len);
        s.name = p;
        s.name_len = i;
        p += i + nnul;
        n -= i + nnul;

        if (!new_table) {
            if (n < 4)
                return gosym_eof(data.len);
            /* The Go type. */
            s.gotype = gosym_order_uint(big, p, 4);
            p += 4;
            n -= 4;
        }
        fn(ctx, &s);
    }
    return BURROW_NO_ERROR;
}

static void gosym_count(void *ctx, const GosymRawSym *s) {
    (void)s;
    (*(Int *)ctx)++;
}

/* ------------------------------------------------------------ NewTable */

static void *gosym_alloc(Alloc *a, size_t n, size_t size, size_t align) {
    if (n == 0)
        return NULL;
    void *p = mem_alloc(a, n * size, align);
    if (p == NULL)
        gosym_oom_panic();
    return p;
}

static Str gosym_copy(Alloc *a, const Byte *p, Int n) {
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *q = (Byte *)gosym_alloc(a, (size_t)n, 1, 1);
    memcpy(q, p, (size_t)n);
    return str_from_bytes(q, n);
}

typedef struct GosymBuild {
    GosymTable *t;
    Slice symtab;
    GosymLineTable *pcln;
    Int n;
    Arena tmp;
    Map *fname;
    Int nf;
    Int nz;
    Byte lasttyp;
    Error err;
} GosymBuild;

/* The length of the path a 'z' or 'Z' symbol names, through the file names
 * of the 'f' symbols before it, and how many of its codes have one. */
static Int gosym_path_len(GosymBuild *b, const GosymRawSym *s, Int *ncodes) {
    Int n = 0;
    Byte last = 0;
    Int i = 0;
    for (; i + 2 <= s->name_len; i += 2) {
        uint16_t code = (uint16_t)(s->name[i] << 8 | s->name[i + 1]);
        Str elt = BURROW_STR_EMPTY;
        if (!map_get2(b->fname, &code, &elt))
            break;
        if (n > 0 && last != '/') {
            n++;
            last = '/';
        }
        n += elt.len;
        if (elt.len > 0)
            last = elt.p[elt.len - 1];
    }
    *ncodes = i / 2;
    return n;
}

/* The second walk of NewTable, which fills in each symbol. */
static void gosym_add_sym(void *ctx, const GosymRawSym *s) {
    GosymBuild *b = (GosymBuild *)ctx;
    GosymTable *t = b->t;
    GosymSym *ts = &((GosymSym *)t->syms.p)[t->syms.len++];
    ts->type = s->typ;
    ts->value = s->value;
    ts->go_type = s->gotype;
    ts->go_version = b->pcln->version;
    if (s->typ == 'z' || s->typ == 'Z') {
        if (b->lasttyp != 'z' && b->lasttyp != 'Z')
            b->nz++;
        Int ncodes = 0;
        Int n = gosym_path_len(b, s, &ncodes);
        Byte *p = (Byte *)gosym_alloc(t->a, (size_t)n, 1, 1);
        Int w = 0;
        for (Int k = 0; k < ncodes; k++) {
            uint16_t code = (uint16_t)(s->name[2 * k] << 8 | s->name[2 * k + 1]);
            Str elt = BURROW_STR_EMPTY;
            (void)map_get2(b->fname, &code, &elt);
            if (w > 0 && p[w - 1] != '/')
                p[w++] = '/';
            if (elt.len > 0) {
                memcpy(p + w, elt.p, (size_t)elt.len);
                w += elt.len;
            }
        }
        ts->name = p != NULL ? str_from_bytes(p, w) : BURROW_STR_EMPTY;
        /* A code with no file name is an error Go makes and walksymtab
         * drops, so the symbol keeps what it has of its name and the rest of
         * this is skipped. */
        if (2 * ncodes < s->name_len)
            return;
    } else {
        /* "·" becomes ".", in the table itself. */
        Byte *nb = s->name;
        Int w = 0;
        for (Int i = 0; i < s->name_len; i++) {
            if (nb[i] == 0xc2 && i + 1 < s->name_len && nb[i + 1] == 0xb7) {
                i++;
                nb[i] = '.';
            }
            nb[w] = nb[i];
            w++;
        }
        ts->name = gosym_copy(t->a, nb, w);
    }
    if (s->typ == 'T' || s->typ == 't' || s->typ == 'L' || s->typ == 'l') {
        b->nf++;
    } else if (s->typ == 'f') {
        uint16_t code = (uint16_t)s->value;
        if (!map_set(b->fname, &code, &ts->name))
            gosym_oom_panic();
    }
    b->lasttyp = s->typ;
}

static bool gosym_is_text(Byte c) {
    return c == 'T' || c == 't' || c == 'L' || c == 'l';
}

static bool gosym_is_path(Byte c) {
    return c == 'Z' || c == 'z';
}

/* funcs[lo:], for an Obj. */
static Slice gosym_funcs_from(Slice funcs, Int lo) {
    if (funcs.p == NULL)
        return funcs;
    funcs.p = (GosymFunc *)funcs.p + lo;
    funcs.len -= lo;
    funcs.cap -= lo;
    return funcs;
}

/* The rest of NewTable, after the symbols have been counted. */
static void gosym_build_body(void *ctx) {
    GosymBuild *b = (GosymBuild *)ctx;
    GosymTable *t = b->t;
    GosymLineTable *pcln = b->pcln;
    b->fname = map_make(arena_allocator(&b->tmp), TYPE_UINT16, TYPE_STRING, 0);
    if (b->fname == NULL)
        gosym_oom_panic();
    t->syms.p = gosym_alloc(t->a, (size_t)b->n, sizeof(GosymSym), _Alignof(GosymSym));
    t->syms.cap = b->n;
    b->err = gosym_walk(b->symtab, gosym_add_sym, b);
    if (BURROW_FAILED(b->err))
        return;

    GosymFunc *funcs = (GosymFunc *)gosym_alloc(t->a, (size_t)b->nf, sizeof(GosymFunc),
                                                _Alignof(GosymFunc));
    t->funcs = (Slice){funcs, 0, b->nf, TYPE_GOSYM_FUNC};
    t->files = map_make(t->a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    if (t->files == NULL)
        gosym_oom_panic();

    GosymObj *obj = NULL;
    GosymObj *objs = NULL;
    if (t->go12line != NULL) {
        /* All the functions are in one Obj. */
        objs = (GosymObj *)gosym_alloc(t->a, 1, sizeof(GosymObj), _Alignof(GosymObj));
        t->objs = (Slice){objs, 1, 1, TYPE_GOSYM_OBJ};
        obj = &objs[0];
        gosym_go12_map_files(t->go12line, t->files, obj);
    } else {
        objs = (GosymObj *)gosym_alloc(t->a, (size_t)b->nz, sizeof(GosymObj),
                                       _Alignof(GosymObj));
        t->objs = (Slice){objs, 0, b->nz, TYPE_GOSYM_OBJ};
    }

    /* The functions, with their frame sizes, parameters and locals, and
     * where each object file starts. */
    GosymSym *syms = (GosymSym *)t->syms.p;
    Int nsyms = t->syms.len;
    Int lastf = 0;
    for (Int i = 0; i < nsyms; i++) {
        GosymSym *sym = &syms[i];
        if (gosym_is_path(sym->type)) {
            /* Go 1.2 has the files elsewhere. */
            if (t->go12line != NULL)
                continue;
            /* The end of the object before, and the start of a new one. */
            if (obj != NULL)
                obj->funcs = gosym_funcs_from(t->funcs, lastf);
            lastf = t->funcs.len;
            obj = &objs[t->objs.len++];
            Int end = i + 1;
            while (end < nsyms && gosym_is_path(syms[end].type))
                end++;
            obj->paths = (Slice){syms + i, end - i, t->syms.cap - i, TYPE_GOSYM_SYM};
            i = end - 1;
            /* The files at the top of the stack. */
            Int depth = 0;
            for (Int j = 0; j < obj->paths.len; j++) {
                const GosymSym *s = &((GosymSym *)obj->paths.p)[j];
                if (s->name.len == 0) {
                    depth--;
                } else {
                    if (depth == 0 && !map_set(t->files, &s->name, &obj))
                        gosym_oom_panic();
                    depth++;
                }
            }
        } else if (gosym_is_text(sym->type)) {
            if (t->funcs.len > 0)
                funcs[t->funcs.len - 1].end = sym->value;
            if (str_eq(sym->name, BURROW_S("runtime.etext")) ||
                str_eq(sym->name, BURROW_S("etext")))
                continue;
            /* The parameters and locals that follow. */
            Int np = 0;
            Int na = 0;
            Int end = i + 1;
            for (; end < nsyms; end++) {
                Byte c = syms[end].type;
                if (gosym_is_text(c) || gosym_is_path(c))
                    break;
                if (c == 'p')
                    np++;
                else if (c == 'a')
                    na++;
            }
            GosymFunc *fn = &funcs[t->funcs.len++];
            sym->func = fn;
            fn->params = (Slice){
                gosym_alloc(t->a, (size_t)np, sizeof(GosymSym *), _Alignof(GosymSym *)),
                0, np, TYPE_UNSAFE_POINTER};
            fn->locals = (Slice){
                gosym_alloc(t->a, (size_t)na, sizeof(GosymSym *), _Alignof(GosymSym *)),
                0, na, TYPE_UNSAFE_POINTER};
            fn->sym = sym;
            fn->entry = sym->value;
            fn->obj = obj;
            if (t->go12line != NULL) {
                /* One table for all of them, which finds a function itself. */
                fn->line_table = t->go12line;
            } else {
                fn->line_table = gosym_slice(pcln, fn->entry, t->a);
                pcln = fn->line_table;
            }
            for (Int j = i; j < end; j++) {
                GosymSym *s = &syms[j];
                if (s->type == 'm')
                    fn->frame_size = (Int)s->value;
                else if (s->type == 'p')
                    ((GosymSym **)fn->params.p)[fn->params.len++] = s;
                else if (s->type == 'a')
                    ((GosymSym **)fn->locals.p)[fn->locals.len++] = s;
            }
            i = end - 1;
        }
    }

    if (t->go12line != NULL && b->nf == 0)
        t->funcs = gosym_go12_funcs(t->go12line, &t->arena);
    if (obj != NULL)
        obj->funcs = gosym_funcs_from(t->funcs, lastf);
}

GosymTable *gosym_new_table(Alloc *a, Slice symtab, GosymLineTable *pcln, Error *err) {
    Int n = 0;
    Error e = gosym_walk(symtab, gosym_count, &n);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    bool go12 = gosym_is_go12(pcln);
    GosymTable *t = (GosymTable *)mem_alloc(a, sizeof *t, _Alignof(GosymTable));
    if (t == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    arena_init(&t->arena, a, 0);
    t->a = arena_allocator(&t->arena);
    t->parent = a;
    t->syms = (Slice){NULL, 0, 0, TYPE_GOSYM_SYM};
    t->funcs = (Slice){NULL, 0, 0, TYPE_GOSYM_FUNC};
    t->objs = (Slice){NULL, 0, 0, TYPE_GOSYM_OBJ};
    if (go12)
        t->go12line = pcln;

    GosymBuild b;
    memset(&b, 0, sizeof b);
    b.t = t;
    b.symtab = symtab;
    b.pcln = pcln;
    b.n = n;
    b.err = BURROW_NO_ERROR;
    arena_init(&b.tmp, a, 0);
    bool ok = gosym_recover(gosym_build_body, &b);
    arena_free(&b.tmp);
    if (!ok || BURROW_FAILED(b.err)) {
        gosym_table_free(t);
        BURROW_OUT(err, ok ? b.err : burrow_err_out_of_memory);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return t;
}

void gosym_table_free(GosymTable *t) {
    if (t == NULL)
        return;
    Alloc *a = t->parent;
    arena_free(&t->arena);
    mem_free(a, t, sizeof *t, _Alignof(GosymTable));
}

/* ------------------------------------------------------------ lookups */

GosymFunc *gosym_table_pc_to_func(GosymTable *t, uint64_t pc) {
    GosymFunc *funcs = (GosymFunc *)t->funcs.p;
    Int n = t->funcs.len;
    while (n > 0) {
        Int m = n / 2;
        GosymFunc *fn = &funcs[m];
        if (pc < fn->entry) {
            n = m;
        } else if (fn->entry <= pc && pc < fn->end) {
            return fn;
        } else {
            funcs += m + 1;
            n -= m + 1;
        }
    }
    return NULL;
}

typedef struct GosymStackEnt {
    Str path;
    Int start;
    Int offset;
    Int prev;
} GosymStackEnt;

/* Obj.lineFromAline: the file and the line in it of the line aline, which
 * counts lines across all the files the object was made of. The 'z'
 * symbols push and pop the files that include each other. */
static Str gosym_line_from_aline(Alloc *a, const GosymObj *o, Int aline, Int *line) {
    if (o == NULL)
        gosym_nil_panic();
    const GosymSym *paths = (const GosymSym *)o->paths.p;
    /* Entry 0 is the bottom, which has no path. Each push takes a new one. */
    GosymStackEnt small[64];
    GosymStackEnt *st = small;
    size_t cap = (size_t)o->paths.len + 1;
    if (cap > sizeof small / sizeof small[0]) {
        st = (GosymStackEnt *)mem_alloc(a, cap * sizeof *st, _Alignof(GosymStackEnt));
        if (st == NULL)
            gosym_oom_panic();
    }
    st[0] = (GosymStackEnt){BURROW_STR_EMPTY, 0, 0, -1};
    Int tos = 0;
    Int used = 1;
    Str file = BURROW_STR_EMPTY;
    Int l = 0;
    bool malformed = false;
    for (Int i = 0; i < o->paths.len; i++) {
        const GosymSym *s = &paths[i];
        Int val = (Int)s->value;
        if (val > aline)
            break;
        if (val == 1) {
            /* A new stack. */
            st[used] = (GosymStackEnt){s->name, val, 0, 0};
            tos = used++;
        } else if (s->name.len == 0) {
            /* A pop. */
            if (tos == 0) {
                malformed = true;
                break;
            }
            Int prev = st[tos].prev;
            st[prev].offset = gosym_add(st[prev].offset, gosym_sub(val, st[tos].start));
            tos = prev;
        } else {
            /* A push. */
            st[used] = (GosymStackEnt){s->name, val, 0, tos};
            tos = used++;
        }
    }
    if (malformed) {
        file = BURROW_S("<malformed symbol table>");
    } else if (tos != 0) {
        file = st[tos].path;
        l = gosym_add(gosym_sub(gosym_sub(aline, st[tos].start), st[tos].offset), 1);
    }
    if (st != small)
        mem_free(a, st, cap * sizeof *st, _Alignof(GosymStackEnt));
    *line = l;
    return file;
}

/* Obj.alineFromLine, the other way. */
static Int gosym_aline_from_line(const GosymObj *o, Str path, Int line, Error *err) {
    if (line < 1) {
        *err = gosym_line_error_build(error_allocator(), path, line);
        return 0;
    }
    const GosymSym *paths = (const GosymSym *)o->paths.p;
    for (Int i = 0; i < o->paths.len; i++) {
        /* The file. */
        if (!str_eq(paths[i].name, path))
            continue;
        /* The line, in the file at this depth. */
        Int depth = 0;
        Int incstart = 0;
        line = gosym_add(line, (Int)paths[i].value);
        for (Int j = i; j < o->paths.len; j++) {
            const GosymSym *s = &paths[j];
            Int val = (Int)s->value;
            if (depth == 1 && val >= line)
                return gosym_sub(line, 1);
            if (s->name.len == 0) {
                depth--;
                if (depth == 0)
                    break;
                if (depth == 1)
                    line = gosym_add(line, gosym_sub(val, incstart));
            } else {
                if (depth == 1)
                    incstart = val;
                depth++;
            }
        }
        *err = gosym_line_error_build(error_allocator(), path, line);
        return 0;
    }
    *err = gosym_file_error_build(error_allocator(), path);
    return 0;
}

Str gosym_table_pc_to_line(GosymTable *t, uint64_t pc, Int *line, GosymFunc **fn) {
    Str file = BURROW_STR_EMPTY;
    Int l = 0;
    GosymFunc *f = gosym_table_pc_to_func(t, pc);
    if (f != NULL) {
        if (t->go12line != NULL) {
            file = gosym_go12_pc_to_file(t->go12line, pc);
            l = gosym_go12_pc_to_line(t->go12line, pc);
        } else {
            Int aline = gosym_line_table_pc_to_line(f->line_table, pc);
            file = gosym_line_from_aline(t->parent, f->obj, aline, &l);
        }
    }
    if (line != NULL)
        *line = l;
    if (fn != NULL)
        *fn = f;
    return file;
}

uint64_t gosym_table_line_to_pc(GosymTable *t, Str file, Int line, GosymFunc **fn,
                                Error *err) {
    if (fn != NULL)
        *fn = NULL;
    void *found = NULL;
    if (!map_get2(t->files, &file, &found)) {
        BURROW_OUT(err, gosym_file_error_build(error_allocator(), file));
        return 0;
    }
    const GosymObj *obj = (const GosymObj *)found;
    if (t->go12line != NULL) {
        uint64_t pc = gosym_go12_line_to_pc(t->go12line, file, line);
        if (pc == 0) {
            BURROW_OUT(err, gosym_line_error_build(error_allocator(), file, line));
            return 0;
        }
        if (fn != NULL)
            *fn = gosym_table_pc_to_func(t, pc);
        BURROW_OUT(err, BURROW_NO_ERROR);
        return pc;
    }
    Error e = BURROW_NO_ERROR;
    Int abs = gosym_aline_from_line(obj, file, line, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    for (Int i = 0; i < obj->funcs.len; i++) {
        GosymFunc *f = &((GosymFunc *)obj->funcs.p)[i];
        uint64_t pc = gosym_line_table_line_to_pc(f->line_table, abs, f->end);
        if (pc != 0) {
            if (fn != NULL)
                *fn = f;
            BURROW_OUT(err, BURROW_NO_ERROR);
            return pc;
        }
    }
    BURROW_OUT(err, gosym_line_error_build(error_allocator(), file, line));
    return 0;
}

/* The symbols LookupSym and SymByAddr look at: text, data and bss. */
static bool gosym_is_tdb(Byte c) {
    return gosym_is_text(c) || c == 'D' || c == 'd' || c == 'B' || c == 'b';
}

GosymSym *gosym_table_lookup_sym(GosymTable *t, Str name) {
    for (Int i = 0; i < t->syms.len; i++) {
        GosymSym *s = &((GosymSym *)t->syms.p)[i];
        if (gosym_is_tdb(s->type) && str_eq(s->name, name))
            return s;
    }
    return NULL;
}

GosymFunc *gosym_table_lookup_func(GosymTable *t, Str name) {
    for (Int i = 0; i < t->funcs.len; i++) {
        GosymFunc *f = &((GosymFunc *)t->funcs.p)[i];
        if (str_eq(f->sym->name, name))
            return f;
    }
    return NULL;
}

GosymSym *gosym_table_sym_by_addr(GosymTable *t, uint64_t addr) {
    for (Int i = 0; i < t->syms.len; i++) {
        GosymSym *s = &((GosymSym *)t->syms.p)[i];
        if (gosym_is_tdb(s->type) && s->value == addr)
            return s;
    }
    return NULL;
}

/* -------------------------------------------------------------- names */

/* Go masks the bytes inside brackets with '_' before it looks for a dot or a
 * slash, unless the brackets do not balance, and then it uses the name as it
 * is. This says which, and gosym_index skips the masked bytes the same way
 * without making the masked copy. */
static bool gosym_balanced(Str name) {
    Int n = 0;
    for (Int i = 0; i < name.len; i++) {
        if (name.p[i] == '[') {
            n++;
        } else if (name.p[i] == ']') {
            n--;
            if (n < 0)
                return false;
        }
    }
    return n == 0;
}

/* The first, or with last the last, c at from or after it, or -1. With mask,
 * a c inside brackets does not count. */
static Int gosym_index(Str name, bool mask, Byte c, Int from, bool last) {
    Int depth = 0;
    Int at = -1;
    for (Int i = 0; i < name.len; i++) {
        Byte b = name.p[i];
        if (b == '[') {
            depth++;
        } else if (b == ']') {
            depth--;
        } else if (i >= from && b == c && (!mask || depth == 0)) {
            at = i;
            if (!last)
                break;
        }
    }
    return at;
}

/* The prefixes Go checks contain no brackets, so they are the same in the
 * masked name as in the name. */
static bool gosym_str_has_prefix(Str s, const char *prefix) {
    size_t n = strlen(prefix);
    return (size_t)s.len >= n && memcmp(s.p, prefix, n) == 0;
}

bool gosym_sym_static(const GosymSym *s) {
    if (s == NULL)
        gosym_nil_panic();
    return s->type >= 'a';
}

Str gosym_sym_package_name(const GosymSym *s) {
    if (s == NULL)
        gosym_nil_panic();
    Str name = s->name;
    bool mask = gosym_balanced(name);
    /* From Go 1.20 on, a name that starts "go:" or "type:" is one the compiler
     * made, which is in no package. Before, the prefixes ended in a dot. */
    if (s->go_version >= GOSYM_VER120 &&
        (gosym_str_has_prefix(name, "go:") || gosym_str_has_prefix(name, "type:")))
        return BURROW_STR_EMPTY;
    if (s->go_version <= GOSYM_VER118 &&
        (gosym_str_has_prefix(name, "go.") || gosym_str_has_prefix(name, "type.")))
        return BURROW_STR_EMPTY;
    Int pathend = gosym_index(name, mask, '/', 0, true);
    if (pathend < 0)
        pathend = 0;
    Int i = gosym_index(name, mask, '.', pathend, false);
    if (i != -1)
        return str_from_bytes(name.p, i);
    return BURROW_STR_EMPTY;
}

Str gosym_sym_receiver_name(const GosymSym *s) {
    if (s == NULL)
        gosym_nil_panic();
    Str name = s->name;
    bool mask = gosym_balanced(name);
    Int pathend = gosym_index(name, mask, '/', 0, true);
    if (pathend < 0)
        pathend = 0;
    /* The first and the last dot after the path. */
    Int l = gosym_index(name, mask, '.', pathend, false);
    Int r = gosym_index(name, mask, '.', pathend, true);
    if (l == -1 || r == -1 || l == r)
        return BURROW_STR_EMPTY;
    return str_from_bytes(name.p + l + 1, r - l - 1);
}

Str gosym_sym_base_name(const GosymSym *s) {
    if (s == NULL)
        gosym_nil_panic();
    Str name = s->name;
    Int i = gosym_index(name, gosym_balanced(name), '.', 0, true);
    if (i != -1)
        return str_from_bytes(name.p + i + 1, name.len - i - 1);
    return name;
}

static const GosymSym *gosym_func_sym(const GosymFunc *f) {
    if (f == NULL)
        gosym_nil_panic();
    return f->sym;
}

bool gosym_func_static(const GosymFunc *f) {
    return gosym_sym_static(gosym_func_sym(f));
}

Str gosym_func_package_name(const GosymFunc *f) {
    return gosym_sym_package_name(gosym_func_sym(f));
}

Str gosym_func_receiver_name(const GosymFunc *f) {
    return gosym_sym_receiver_name(gosym_func_sym(f));
}

Str gosym_func_base_name(const GosymFunc *f) {
    return gosym_sym_base_name(gosym_func_sym(f));
}
