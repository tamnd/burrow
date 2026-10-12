/* Derived from Go's src/debug/plan9obj/file.go and plan9obj.go. Go source:
 * go1.27.1.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/debug/plan9obj.h"

#include "objfile_internal.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

static void plan9obj_nil_panic(void) {
    runtime_panic(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

static Slice plan9obj_bytes(void *p, Int n) {
    Slice s = {p, n, n, TYPE_BYTE};
    return s;
}

static uint16_t plan9obj_u16(const Byte *p) {
    return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

static uint32_t plan9obj_u32(const Byte *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t plan9obj_u64(const Byte *p) {
    return (uint64_t)plan9obj_u32(p) << 32 | plan9obj_u32(p + 4);
}

BURROW_SENTINEL_ERROR(plan9obj_err_no_symbols, "no symbol section");

/* Symbols makes a new one of these each time in Go, so nothing can compare
 * against it; one is enough. */
#define PLAN9OBJ_CANNOT_LOAD "cannot load symbol section"
static const Str plan9obj_cannot_load__text = {(const Byte *)PLAN9OBJ_CANNOT_LOAD,
                                               (Int)(sizeof PLAN9OBJ_CANNOT_LOAD - 1)};
static const Error plan9obj_err_cannot_load = {&burrow_sentinel_error_vt,
                                               &plan9obj_cannot_load__text};

/* formatError{len(data), "unexpected EOF", nil}. */
static Error plan9obj_eof_error(Int off) {
    return fmt_errorf_v("unexpected EOF in record at byte %#x", off);
}

#define PLAN9OBJ_SYM_FIELDS(F, T)                                                      \
    F(T, uint64_t, value, Value, "")                                                   \
    F(T, Rune, type, Type, "")                                                         \
    F(T, Str, name, Name, "")

BURROW_STRUCT_AS_DEFINE(Plan9objSym, PLAN9OBJ_SYM_FIELDS);

/* --------------------------------------------------------------- sections */

Slice plan9obj_section_data(Plan9objSection *s, Alloc *a, Error *err) {
    if (s == NULL)
        plan9obj_nil_panic();
    return burrow__saferio_read_data_at(a, io_section_reader_as_io_reader_at(&s->sr),
                                        s->section_header.size, 0, err);
}

IoReadSeeker plan9obj_section_open(Plan9objSection *s, Alloc *a) {
    if (s == NULL)
        plan9obj_nil_panic();
    return burrow__objfile_open(a, &s->sr);
}

Int plan9obj_section_read_at(Plan9objSection *s, Slice p, int64_t off, Error *err) {
    if (s == NULL || s->reader_at.vt == NULL)
        plan9obj_nil_panic();
    return s->reader_at.vt->read_at(s->reader_at.data, p, off, err);
}

void plan9obj_reader_free(IoReadSeeker r) {
    burrow__objfile_reader_free(r);
}

/* ------------------------------------------------------------------ files */

static void plan9obj_file_release(Plan9objFile *f) {
    Alloc *a = f->a;
    arena_free(&f->arena);
    mem_free(a, f, sizeof *f, _Alignof(Plan9objFile));
}

static Error plan9obj_read_file(Plan9objFile *f, IoReaderAt r, Alloc *fa) {
    IoSectionReader sr = io_new_section_reader(r, 0, INT64_MAX);
    /* Read and decode Plan 9 magic. */
    Byte magic[4];
    Error e = BURROW_NO_ERROR;
    r.vt->read_at(r.data, plan9obj_bytes(magic, 4), 0, &e);
    if (BURROW_FAILED(e))
        return e;
    switch (plan9obj_u32(magic)) {
    case PLAN9OBJ_MAGIC386:
    case PLAN9OBJ_MAGIC_AMD64:
    case PLAN9OBJ_MAGIC_ARM:
        break;
    default:
        return fmt_errorf_v("bad magic number '[%d %d %d %d]' in record at byte 0x0",
                            magic[0], magic[1], magic[2], magic[3]);
    }

    Byte ph[32];
    io_read_full(io_section_reader_as_io_reader(&sr), plan9obj_bytes(ph, 32), &e);
    if (BURROW_FAILED(e))
        return e;
    uint32_t text = plan9obj_u32(ph + 4);
    uint32_t data = plan9obj_u32(ph + 8);
    uint32_t syms = plan9obj_u32(ph + 16);
    uint32_t spsz = plan9obj_u32(ph + 24);
    uint32_t pcsz = plan9obj_u32(ph + 28);
    Plan9objFileHeader *h = &f->file_header;
    h->magic = plan9obj_u32(ph);
    h->bss = plan9obj_u32(ph + 12);
    h->entry = plan9obj_u32(ph + 20);
    h->ptr_size = 4;
    h->load_address = 0x1000;
    h->hdr_size = UINT64_C(4) * 8;

    if ((h->magic & PLAN9OBJ_MAGIC64) != 0) {
        Byte entry[8];
        io_read_full(io_section_reader_as_io_reader(&sr), plan9obj_bytes(entry, 8), &e);
        if (BURROW_FAILED(e))
            return e;
        h->entry = plan9obj_u64(entry);
        h->ptr_size = 8;
        h->load_address = 0x200000;
        h->hdr_size += 8;
    }

    static const char *const names[5] = {"text", "data", "syms", "spsz", "pcsz"};
    const uint32_t sizes[5] = {text, data, syms, spsz, pcsz};
    Plan9objSection **secs = (Plan9objSection **)mem_alloc(fa, 5 * sizeof *secs,
                                                           _Alignof(Plan9objSection *));
    Plan9objSection *s =
        (Plan9objSection *)mem_alloc(fa, 5 * sizeof *s, _Alignof(Plan9objSection));
    if (secs == NULL || s == NULL)
        return burrow_err_out_of_memory;
    f->sections = (Slice){secs, 5, 5, TYPE_UNSAFE_POINTER};
    uint32_t off = (uint32_t)h->hdr_size;
    for (int i = 0; i < 5; i++, s++) {
        s->section_header.name = str_from_cstr(names[i]);
        s->section_header.size = sizes[i];
        s->section_header.offset = off;
        off += sizes[i];
        s->sr = io_new_section_reader(r, (int64_t)s->section_header.offset,
                                      (int64_t)s->section_header.size);
        s->reader_at = io_section_reader_as_io_reader_at(&s->sr);
        secs[i] = s;
    }
    return BURROW_NO_ERROR;
}

Plan9objFile *plan9obj_new_file(Alloc *a, IoReaderAt r, Error *err) {
    if (r.vt == NULL)
        plan9obj_nil_panic();
    Plan9objFile *f = (Plan9objFile *)mem_alloc(a, sizeof *f, _Alignof(Plan9objFile));
    if (f == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    f->a = a;
    arena_init(&f->arena, a, 0);
    f->sections.elem = TYPE_UNSAFE_POINTER;
    Error e = plan9obj_read_file(f, r, arena_allocator(&f->arena));
    if (BURROW_FAILED(e)) {
        plan9obj_file_release(f);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return f;
}

Plan9objFile *plan9obj_open(Alloc *a, Str name, Error *err) {
    Error e = BURROW_NO_ERROR;
    OsFile *file = os_open(a, name, &e);
    if (file == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    Plan9objFile *f = plan9obj_new_file(a, os_file_as_io_reader_at(file), &e);
    if (f == NULL) {
        os_file_close(file);
        os_file_free(file);
        BURROW_OUT(err, e);
        return NULL;
    }
    f->file = file;
    f->open = true;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return f;
}

Error plan9obj_file_close(Plan9objFile *f) {
    if (f == NULL)
        plan9obj_nil_panic();
    Error e = BURROW_NO_ERROR;
    if (f->open) {
        e = os_file_close(f->file);
        f->open = false;
    }
    return e;
}

void plan9obj_file_free(Plan9objFile *f) {
    if (f == NULL)
        return;
    OsFile *file = f->file;
    if (f->open)
        os_file_close(file);
    os_file_free(file);
    plan9obj_file_release(f);
}

Plan9objSection *plan9obj_file_section(Plan9objFile *f, Str name) {
    if (f == NULL)
        plan9obj_nil_panic();
    Plan9objSection **secs = (Plan9objSection **)f->sections.p;
    for (Int i = 0; i < f->sections.len; i++) {
        if (str_eq(secs[i]->section_header.name, name))
            return secs[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------- symbols */

/* sym, one entry as the table has it. */
typedef struct Plan9objRawSym {
    uint64_t value;
    Byte typ;
    const Byte *name;
    Int name_len;
} Plan9objRawSym;

/* The 'f' symbols seen so far, by their 16-bit code: Go's map[uint16]string,
 * open addressed. keys holds a code plus one, or 0 for a free slot. */
typedef struct Plan9objNames {
    uint32_t *keys;
    Str *vals;
    Int mask;
} Plan9objNames;

typedef struct Plan9objTable {
    Alloc *a;
    Int n;
    Int nf;
    Int cap;
    Plan9objSym *syms;
    Plan9objNames fname;
} Plan9objTable;

static Int plan9obj_slot(const Plan9objNames *m, uint16_t code) {
    Int i = (Int)(code * 40503U) & m->mask;
    while (m->keys[i] != 0 && m->keys[i] != (uint32_t)code + 1)
        i = (i + 1) & m->mask;
    return i;
}

static const Str *plan9obj_lookup(const Plan9objNames *m, uint16_t code) {
    if (m->keys == NULL)
        return NULL;
    Int i = plan9obj_slot(m, code);
    return m->keys[i] == 0 ? NULL : &m->vals[i];
}

typedef void (*Plan9objVisit)(Plan9objTable *t, const Plan9objRawSym *s);

/* walksymtab. Like Go's, it drops what fn says and only fails on a table that
 * ends in the middle of an entry. */
static Error plan9obj_walk(Slice data, Int ptrsz, Plan9objVisit fn, Plan9objTable *t) {
    const Byte *p = (const Byte *)data.p;
    Int n = data.len;
    Plan9objRawSym s = {0};
    while (n >= 4) {
        /* Symbol type, value. */
        if (n < ptrsz)
            return plan9obj_eof_error(data.len);
        /* Fixed-width value. */
        if (ptrsz == 8) {
            s.value = plan9obj_u64(p);
            p += 8;
            n -= 8;
        } else {
            s.value = plan9obj_u32(p);
            p += 4;
            n -= 4;
        }

        if (n < 1)
            return plan9obj_eof_error(data.len);
        Byte typ = p[0] & 0x7F;
        s.typ = typ;
        p++;
        n--;

        /* Name. */
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
            return plan9obj_eof_error(data.len);
        s.name = p;
        s.name_len = i;
        i += nnul;
        p += i;
        n -= i;

        fn(t, &s);
    }
    return BURROW_NO_ERROR;
}

static void plan9obj_count(Plan9objTable *t, const Plan9objRawSym *s) {
    t->n++;
    if (s->typ == 'f')
        t->nf++;
}

/* The path a 'z' symbol names: the 'f' names its codes stand for, joined with
 * a slash where one is missing. Go stops at the first code it has no name for
 * and keeps what it has so far, so this does too. */
static Str plan9obj_path(Plan9objTable *t, const Plan9objRawSym *s) {
    Int len = 0;
    Byte last = 0;
    Int end = 0;
    for (; end + 2 <= s->name_len; end += 2) {
        const Str *elt = plan9obj_lookup(&t->fname, plan9obj_u16(s->name + end));
        if (elt == NULL)
            break;
        if (len > 0 && last != '/') {
            len++;
            last = '/';
        }
        if (elt->len > 0) {
            len += elt->len;
            last = elt->p[elt->len - 1];
        }
    }
    if (len == 0)
        return BURROW_STR_EMPTY;
    Byte *b = (Byte *)mem_alloc_nozero(t->a, (size_t)len, 1);
    if (b == NULL)
        return BURROW_STR_EMPTY;
    Int at = 0;
    for (Int i = 0; i < end; i += 2) {
        const Str *elt = plan9obj_lookup(&t->fname, plan9obj_u16(s->name + i));
        if (at > 0 && b[at - 1] != '/')
            b[at++] = '/';
        if (elt->len > 0)
            memcpy(b + at, elt->p, (size_t)elt->len);
        at += elt->len;
    }
    return str_from_bytes(b, len);
}

static void plan9obj_add(Plan9objTable *t, const Plan9objRawSym *s) {
    /* The second walk sees what the first one counted. */
    if (t->syms == NULL || t->n >= t->cap)
        return;
    Plan9objSym *ts = &t->syms[t->n++];
    ts->type = (Rune)s->typ;
    ts->value = s->value;
    if (s->typ == 'z' || s->typ == 'Z') {
        ts->name = plan9obj_path(t, s);
    } else {
        ts->name = str_clone(t->a, str_from_bytes(s->name, s->name_len));
    }
    if (s->typ == 'f' && t->fname.keys != NULL) {
        Int i = plan9obj_slot(&t->fname, (uint16_t)s->value);
        t->fname.keys[i] = (uint32_t)(uint16_t)s->value + 1;
        t->fname.vals[i] = ts->name;
    }
}

/* newTable. The name table is made in ta and the symbols in a. */
static Slice plan9obj_new_table(Alloc *a, Alloc *ta, Slice symtab, Int ptrsz,
                                Error *err) {
    Plan9objTable t = {a, 0, 0, 0, NULL, {NULL, NULL, 0}};
    Error e = plan9obj_walk(symtab, ptrsz, plan9obj_count, &t);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (Slice){NULL, 0, 0, TYPE_PLAN9OBJ_SYM};
    }

    if (t.nf > 0) {
        Int size = 2;
        while (size < 2 * t.nf)
            size *= 2;
        t.fname.keys = (uint32_t *)mem_alloc(ta, (size_t)size * sizeof *t.fname.keys,
                                             _Alignof(uint32_t));
        t.fname.vals =
            (Str *)mem_alloc(ta, (size_t)size * sizeof *t.fname.vals, _Alignof(Str));
        t.fname.mask = size - 1;
        if (t.fname.keys == NULL || t.fname.vals == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return (Slice){NULL, 0, 0, TYPE_PLAN9OBJ_SYM};
        }
    }
    Int n = t.n;
    if (n > 0) {
        t.syms = (Plan9objSym *)mem_alloc(a, (size_t)n * sizeof *t.syms,
                                          _Alignof(Plan9objSym));
        if (t.syms == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return (Slice){NULL, 0, 0, TYPE_PLAN9OBJ_SYM};
        }
    }
    t.n = 0;
    t.cap = n;
    (void)plan9obj_walk(symtab, ptrsz, plan9obj_add, &t);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (Slice){t.syms, t.n, n, TYPE_PLAN9OBJ_SYM};
}

Slice plan9obj_file_symbols(Plan9objFile *f, Alloc *a, Error *err) {
    Plan9objSection *symtab_section = plan9obj_file_section(f, BURROW_S("syms"));
    if (symtab_section == NULL) {
        BURROW_OUT(err, plan9obj_err_no_symbols);
        return (Slice){NULL, 0, 0, TYPE_PLAN9OBJ_SYM};
    }

    /* The table itself is only needed while the symbols are made. */
    Arena tmp;
    arena_init(&tmp, a, 0);
    Alloc *ta = arena_allocator(&tmp);
    Error e = BURROW_NO_ERROR;
    Slice symtab = plan9obj_section_data(symtab_section, ta, &e);
    Slice syms = {NULL, 0, 0, TYPE_PLAN9OBJ_SYM};
    if (BURROW_FAILED(e))
        BURROW_OUT(err, plan9obj_err_cannot_load);
    else
        syms = plan9obj_new_table(a, ta, symtab, f->file_header.ptr_size, err);
    arena_free(&tmp);
    return syms;
}
