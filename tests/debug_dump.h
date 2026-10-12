/* What tests/elf_test.c and tests/macho_test.c share: the shape of the files
 * tools/gen-debug-tests.sh writes, the DWARF half of the dump it makes with
 * Go's packages, and the loops that compare a dump with Go's, whole and for
 * each damaged copy of a file. The includer includes the burrow headers first.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_DEBUG_DUMP_H
#define BURROW_TESTS_DEBUG_DUMP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct DebugDamage {
    int pos; /* the byte to change, or where to cut the file when xor_ is 0 */
    int xor_;
    const char *hash;
} DebugDamage;

typedef struct DebugFile {
    const char *name;
    bool gz;
    const char *const *data;
    const char *const *dump;
    const DebugDamage *damage;
} DebugFile;

typedef struct DebugName {
    int kind;
    long long value;
    const char *str;
    const char *go;
} DebugName;

/* ------------------------------------------------------------------ helpers */

static Slice gen_bytes(Alloc *a, const char *const *lines) {
    Int n = 0;
    for (const char *const *l = lines; *l != NULL; l++)
        n += (Int)strlen(*l);
    Byte *p = (Byte *)mem_alloc(a, (size_t)n + 1, 1);
    if (p == NULL)
        abort();
    Int off = 0;
    for (const char *const *l = lines; *l != NULL; l++) {
        size_t k = strlen(*l);
        memcpy(p + off, *l, k);
        off += (Int)k;
    }
    Error err = BURROW_NO_ERROR;
    Slice out = base64_encoding_decode_string(base64_std_encoding, a,
                                              str_from_bytes(p, n), &err);
    if (BURROW_FAILED(err))
        abort();
    return out;
}

/* The ELF file of a test file, gunzipped if it has to be. */
static Slice file_bytes(Alloc *a, const DebugFile *f) {
    Slice raw = gen_bytes(a, f->data);
    if (!f->gz)
        return raw;
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, raw);
    GzipReader *zr = gzip_new_reader(a, bytes_reader_as_io_reader(br), &err);
    if (BURROW_FAILED(err))
        abort();
    Slice out = io_read_all(a, gzip_reader_as_io_reader(zr), &err);
    if (BURROW_FAILED(err))
        abort();
    return out;
}

static Str errs(Alloc *a, Error err) {
    if (BURROW_OK(err))
        return BURROW_STR_EMPTY;
    return fmt_sprintf_v(a, " err=%q", error_text(err));
}

static Str blob(Alloc *a, Slice b, Error err) {
    Sha256Sum256Ret h = sha256_sum256(b);
    Str hx = hex_encode_to_string(a, (Slice){h.a, 8, 8, TYPE_BYTE});
    return fmt_sprintf_v(a, "len=%d sha=%s%s", b.len, hx, errs(a, err));
}

/* --------------------------------------------------------------------- dump */

/* The dump below is the one in tools/gen-debug-tests.sh, line for line. */

static Str dump_val(Alloc *a, Any v) {
    if (v.t == NULL)
        return BURROW_S("nil");
    if (v.t == TYPE_INT64)
        return fmt_sprintf_v(a, "i%d", *(int64_t *)v.data);
    if (v.t == TYPE_UINT64)
        return fmt_sprintf_v(a, "u%#x", *(uint64_t *)v.data);
    if (v.t == TYPE_STRING)
        return fmt_sprintf_v(a, "%q", *(Str *)v.data);
    if (v.t == TYPE_BYTES)
        return fmt_sprintf_v(a, "b%s", hex_encode_to_string(a, *(Slice *)v.data));
    if (v.t == TYPE_BOOL)
        return fmt_sprintf_v(a, "%t", *(bool *)v.data);
    if (v.t == TYPE_DWARF_OFFSET)
        return fmt_sprintf_v(a, "o%#x", *(DwarfOffset *)v.data);
    return BURROW_S("?");
}

static void dump_strings(Alloc *a, IoWriter w, Str label, Slice ss, Error err) {
    fmt_fprintf_v(w, "%s n=%d%s\n", label, ss.len, errs(a, err));
    for (Int i = 0; i < ss.len; i++)
        fmt_fprintf_v(w, "  %q\n", ((Str *)ss.p)[i]);
}

static bool is_type_tag(DwarfTag t) {
    switch (t) {
    case DWARF_TAG_ARRAY_TYPE:
    case DWARF_TAG_BASE_TYPE:
    case DWARF_TAG_CLASS_TYPE:
    case DWARF_TAG_STRUCT_TYPE:
    case DWARF_TAG_UNION_TYPE:
    case DWARF_TAG_CONST_TYPE:
    case DWARF_TAG_VOLATILE_TYPE:
    case DWARF_TAG_RESTRICT_TYPE:
    case DWARF_TAG_ENUMERATION_TYPE:
    case DWARF_TAG_POINTER_TYPE:
    case DWARF_TAG_SUBROUTINE_TYPE:
    case DWARF_TAG_TYPEDEF:
    case DWARF_TAG_UNSPECIFIED_TYPE:
    case DWARF_TAG_REFERENCE_TYPE:
    case DWARF_TAG_RVALUE_REFERENCE_TYPE:
    case DWARF_TAG_PTR_TO_MEMBER_TYPE:
        return true;
    default:
        return false;
    }
}

/* What %T says of each type struct in Go, without the "*dwarf.". */
static const char *type_kind_name(DwarfTypeKind k) {
    switch (k) {
    case DWARF_TYPE_KIND_COMMON:
        return "CommonType";
    case DWARF_TYPE_KIND_ADDR:
        return "AddrType";
    case DWARF_TYPE_KIND_ARRAY:
        return "ArrayType";
    case DWARF_TYPE_KIND_BASIC:
        return "BasicType";
    case DWARF_TYPE_KIND_BOOL:
        return "BoolType";
    case DWARF_TYPE_KIND_CHAR:
        return "CharType";
    case DWARF_TYPE_KIND_COMPLEX:
        return "ComplexType";
    case DWARF_TYPE_KIND_DOT_DOT_DOT:
        return "DotDotDotType";
    case DWARF_TYPE_KIND_ENUM:
        return "EnumType";
    case DWARF_TYPE_KIND_FLOAT:
        return "FloatType";
    case DWARF_TYPE_KIND_FUNC:
        return "FuncType";
    case DWARF_TYPE_KIND_INT:
        return "IntType";
    case DWARF_TYPE_KIND_PTR:
        return "PtrType";
    case DWARF_TYPE_KIND_QUAL:
        return "QualType";
    case DWARF_TYPE_KIND_STRUCT:
        return "StructType";
    case DWARF_TYPE_KIND_TYPEDEF:
        return "TypedefType";
    case DWARF_TYPE_KIND_UCHAR:
        return "UcharType";
    case DWARF_TYPE_KIND_UINT:
        return "UintType";
    case DWARF_TYPE_KIND_UNSPECIFIED:
        return "UnspecifiedType";
    case DWARF_TYPE_KIND_UNSUPPORTED:
        return "UnsupportedType";
    case DWARF_TYPE_KIND_VOID:
        return "VoidType";
    default:
        return "?";
    }
}

static bool kind_is_basic(DwarfTypeKind k) {
    switch (k) {
    case DWARF_TYPE_KIND_ADDR:
    case DWARF_TYPE_KIND_BASIC:
    case DWARF_TYPE_KIND_BOOL:
    case DWARF_TYPE_KIND_CHAR:
    case DWARF_TYPE_KIND_COMPLEX:
    case DWARF_TYPE_KIND_FLOAT:
    case DWARF_TYPE_KIND_INT:
    case DWARF_TYPE_KIND_UCHAR:
    case DWARF_TYPE_KIND_UINT:
    case DWARF_TYPE_KIND_UNSPECIFIED:
        return true;
    case DWARF_TYPE_KIND_COMMON:
    case DWARF_TYPE_KIND_ARRAY:
    case DWARF_TYPE_KIND_DOT_DOT_DOT:
    case DWARF_TYPE_KIND_ENUM:
    case DWARF_TYPE_KIND_FUNC:
    case DWARF_TYPE_KIND_PTR:
    case DWARF_TYPE_KIND_QUAL:
    case DWARF_TYPE_KIND_STRUCT:
    case DWARF_TYPE_KIND_TYPEDEF:
    case DWARF_TYPE_KIND_UNSUPPORTED:
    case DWARF_TYPE_KIND_VOID:
    default:
        return false;
    }
}

static void dump_type(Alloc *a, IoWriter w, DwarfData *d, DwarfOffset off) {
    Error err = BURROW_NO_ERROR;
    DwarfType t = dwarf_data_type(d, off, &err);
    if (BURROW_FAILED(err) || t == NULL) {
        fmt_fprintf_v(w, "  type %#x err=%q\n", off, error_text(err));
        return;
    }
    fmt_fprintf_v(w, "  type %#x %s size=%d name=%q str=%q", off,
                  type_kind_name(t->type_kind), dwarf_type_size(t), t->name,
                  dwarf_type_string(t, a));
    if (kind_is_basic(t->type_kind)) {
        const DwarfBasicType *bt = (const DwarfBasicType *)t;
        fmt_fprintf_v(w, " bits=%d,%d,%d", bt->bit_size, bt->bit_offset,
                      bt->data_bit_offset);
    }
    switch (t->type_kind) {
    case DWARF_TYPE_KIND_STRUCT: {
        DwarfStructType *st = (DwarfStructType *)t;
        fmt_fprintf_v(w, " kind=%q sname=%q incomplete=%t defn=%q", st->kind,
                      st->struct_name, st->incomplete, dwarf_struct_type_defn(st, a));
        for (Int i = 0; i < st->field.len; i++) {
            const DwarfStructField *f = ((DwarfStructField **)st->field.p)[i];
            Str ts = f->type != NULL ? dwarf_type_string(f->type, a) : BURROW_S("nil");
            fmt_fprintf_v(w, " field=%q,%q,%d,%d,%d,%d,%d", f->name, ts, f->byte_offset,
                          f->byte_size, f->bit_offset, f->data_bit_offset, f->bit_size);
        }
        break;
    }
    case DWARF_TYPE_KIND_ARRAY: {
        const DwarfArrayType *at = (const DwarfArrayType *)t;
        fmt_fprintf_v(w, " stride=%d count=%d", at->stride_bit_size, at->count);
        break;
    }
    case DWARF_TYPE_KIND_ENUM: {
        const DwarfEnumType *et = (const DwarfEnumType *)t;
        fmt_fprintf_v(w, " ename=%q", et->enum_name);
        for (Int i = 0; i < et->val.len; i++) {
            const DwarfEnumValue *v = ((DwarfEnumValue **)et->val.p)[i];
            fmt_fprintf_v(w, " val=%q,%d", v->name, v->val);
        }
        break;
    }
    case DWARF_TYPE_KIND_FUNC:
        fmt_fprintf_v(w, " params=%d", ((const DwarfFuncType *)t)->param_type.len);
        break;
    case DWARF_TYPE_KIND_QUAL:
        fmt_fprintf_v(w, " qual=%q", ((const DwarfQualType *)t)->qual);
        break;
    case DWARF_TYPE_KIND_UNSUPPORTED:
        fmt_fprintf_v(w, " tag=%s",
                      dwarf_tag_string(((const DwarfUnsupportedType *)t)->tag, a));
        break;
    case DWARF_TYPE_KIND_COMMON:
    case DWARF_TYPE_KIND_ADDR:
    case DWARF_TYPE_KIND_BASIC:
    case DWARF_TYPE_KIND_BOOL:
    case DWARF_TYPE_KIND_CHAR:
    case DWARF_TYPE_KIND_COMPLEX:
    case DWARF_TYPE_KIND_DOT_DOT_DOT:
    case DWARF_TYPE_KIND_FLOAT:
    case DWARF_TYPE_KIND_INT:
    case DWARF_TYPE_KIND_PTR:
    case DWARF_TYPE_KIND_TYPEDEF:
    case DWARF_TYPE_KIND_UCHAR:
    case DWARF_TYPE_KIND_UINT:
    case DWARF_TYPE_KIND_UNSPECIFIED:
    case DWARF_TYPE_KIND_VOID:
    default:
        break;
    }
    fmt_fprintf_v(w, "\n");
}

/* The rows are kept as bytes, which need not be aligned for a DwarfLineEntry. */
static DwarfLineEntry row_at(Slice rows, Int i) {
    DwarfLineEntry e;
    memcpy(&e, (const Byte *)rows.p + i * (Int)sizeof e, sizeof e);
    return e;
}

static void dump_lines(Alloc *a, IoWriter w, DwarfData *d, const DwarfEntry *cu) {
    Error err = BURROW_NO_ERROR;
    DwarfLineReader *lr = dwarf_data_line_reader(d, a, cu, &err);
    if (BURROW_FAILED(err)) {
        fmt_fprintf_v(w, "lines %#x err=%q\n", cu->offset, error_text(err));
        return;
    }
    if (lr == NULL) {
        fmt_fprintf_v(w, "lines %#x none\n", cu->offset);
        return;
    }
    fmt_fprintf_v(w, "lines %#x\n", cu->offset);
    Slice rows = slice_make(a, TYPE_BYTE, 0, 0);
    Int nrows = 0;
    DwarfLineReaderPos mid = {0};
    DwarfLineEntry e = {0};
    for (;;) {
        if (nrows == 3)
            mid = dwarf_line_reader_tell(lr);
        err = dwarf_line_reader_next(lr, &e);
        if (BURROW_FAILED(err)) {
            fmt_fprintf_v(w, "  end err=%q\n", error_text(err));
            break;
        }
        Byte *row = (Byte *)&e;
        rows = slice_append(a, rows, row, (Int)sizeof e);
        nrows++;
        Str file = BURROW_S("nil");
        if (e.file != NULL)
            file = fmt_sprintf_v(a, "%q", e.file->name);
        fmt_fprintf_v(
            w,
            "  row %#x op=%d file=%s line=%d col=%d stmt=%t bb=%t pe=%t eb=%t "
            "isa=%d disc=%d end=%t\n",
            e.address, e.op_index, file, e.line, e.column, e.is_stmt, e.basic_block,
            e.prologue_end, e.epilogue_begin, e.isa, e.discriminator, e.end_sequence);
    }
    Slice files = dwarf_line_reader_files(lr);
    fmt_fprintf_v(w, "  files n=%d\n", files.len);
    for (Int i = 0; i < files.len; i++) {
        const DwarfLineFile *f = ((DwarfLineFile **)files.p)[i];
        if (f == NULL) {
            fmt_fprintf_v(w, "    nil\n");
            continue;
        }
        fmt_fprintf_v(w, "    %q mtime=%d len=%d\n", f->name, f->mtime, f->length);
    }
    if (nrows > 3) {
        dwarf_line_reader_seek(lr, mid);
        Int same = 0;
        for (Int i = 3; i < nrows; i++) {
            if (BURROW_FAILED(dwarf_line_reader_next(lr, &e)))
                break;
            DwarfLineEntry want = row_at(rows, i);
            if (e.address == want.address && e.line == want.line)
                same++;
        }
        fmt_fprintf_v(w, "  tellseek %d of %d\n", same, nrows - 3);
    }
    dwarf_line_reader_reset(lr);
    for (Int i = 0; i < nrows; i++) {
        DwarfLineEntry want = row_at(rows, i);
        if (want.end_sequence)
            continue;
        DwarfLineEntry got = {0};
        err = dwarf_line_reader_seek_pc(lr, want.address, &got);
        if (BURROW_FAILED(err)) {
            fmt_fprintf_v(w, "  seekpc %#x err=%q\n", want.address, error_text(err));
            continue;
        }
        fmt_fprintf_v(w, "  seekpc %#x %#x line=%d\n", want.address, got.address,
                      got.line);
    }
}

static void dump_dwarf(Alloc *a, IoWriter w, DwarfData *d) {
    DwarfReader *r = dwarf_data_reader(d, a);
    Slice cus = slice_make(a, TYPE_UNSAFE_POINTER, 0, 0);
    Slice lows = slice_make(a, TYPE_UINT64, 0, 0);
    for (;;) {
        Error err = BURROW_NO_ERROR;
        DwarfEntry *e = dwarf_reader_next(r, &err);
        if (BURROW_FAILED(err)) {
            fmt_fprintf_v(w, "next err=%q\n", error_text(err));
            break;
        }
        if (e == NULL)
            break;
        fmt_fprintf_v(w, "entry %#x %s children=%t", e->offset,
                      dwarf_tag_string(e->tag, a), e->children);
        for (Int i = 0; i < e->field.len; i++) {
            const DwarfField *f = (const DwarfField *)e->field.p + i;
            fmt_fprintf_v(w, " %s:%s=%s", dwarf_attr_string(f->attr, a),
                          dwarf_class_string(f->cls, a), dump_val(a, f->val));
        }
        fmt_fprintf_v(w, "\n");
        if (e->tag == DWARF_TAG_COMPILE_UNIT) {
            cus = slice_append(a, cus, &e, 1);
            BinaryByteOrder bo = dwarf_reader_byte_order(r);
            fmt_fprintf_v(w, "  unit asize=%d order=%s\n", dwarf_reader_address_size(r),
                          bo.vt->string(bo.data));
        }
        Slice rs = dwarf_data_ranges(d, a, e, &err);
        if (rs.len > 0 || BURROW_FAILED(err)) {
            fmt_fprintf_v(w, "  ranges");
            for (Int i = 0; i < rs.len; i++) {
                const DwarfPCRange *x = (const DwarfPCRange *)rs.p + i;
                fmt_fprintf_v(w, " %#x-%#x", x->v[0], x->v[1]);
            }
            fmt_fprintf_v(w, "%s\n", errs(a, err));
            if (e->tag == DWARF_TAG_COMPILE_UNIT && rs.len > 0) {
                uint64_t lo = ((const DwarfPCRange *)rs.p)->v[0];
                lows = slice_append(a, lows, &lo, 1);
            }
        }
        if (is_type_tag(e->tag))
            dump_type(a, w, d, e->offset);
        DwarfOffset *ref = (DwarfOffset *)any_assert(
            dwarf_entry_val(e, DWARF_ATTR_TYPE), TYPE_DWARF_OFFSET);
        if (ref != NULL)
            dump_type(a, w, d, *ref);
    }
    DwarfEntry **cu = (DwarfEntry **)cus.p;
    Int ncu = cus.len;
    for (Int i = 0; i < ncu; i++)
        dump_lines(a, w, d, cu[i]);
    DwarfReader *r2 = dwarf_data_reader(d, a);
    const uint64_t *low = (const uint64_t *)lows.p;
    for (Int i = 0; i < lows.len; i++) {
        Error err = BURROW_NO_ERROR;
        DwarfEntry *e = dwarf_reader_seek_pc(r2, low[i], &err);
        if (BURROW_FAILED(err)) {
            fmt_fprintf_v(w, "seekpc %#x err=%q\n", low[i], error_text(err));
            continue;
        }
        fmt_fprintf_v(w, "seekpc %#x %#x\n", low[i], e->offset);
    }
    for (Int i = 0; i < ncu; i++) {
        Error err = BURROW_NO_ERROR;
        dwarf_reader_seek(r2, cu[i]->offset);
        DwarfEntry *e = dwarf_reader_next(r2, &err);
        if (BURROW_FAILED(err) || e == NULL) {
            fmt_fprintf_v(w, "seek %#x failed%s\n", cu[i]->offset, errs(a, err));
            continue;
        }
        dwarf_reader_skip_children(r2);
        DwarfEntry *e2 = dwarf_reader_next(r2, &err);
        Str next = BURROW_S("nil");
        if (e2 != NULL)
            next = fmt_sprintf_v(a, "%#x", e2->offset);
        fmt_fprintf_v(w, "seek %#x %#x next=%s%s\n", cu[i]->offset, e->offset, next,
                      errs(a, err));
    }
}

/* --------------------------------------------------------------- the checks */

typedef void (*DebugDumpFn)(Alloc *a, IoWriter w, Slice data);

/* The dump of each file has to be Go's, line for line. */
static void debug_check_dumps(TestingT *t, const DebugFile *files, size_t nfiles,
                              DebugDumpFn dump) {
    for (size_t i = 0; i < nfiles; i++) {
        const DebugFile *df = &files[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        BytesBuffer out = BYTES_BUFFER(a);
        dump(a, bytes_buffer_as_io_writer(&out), file_bytes(a, df));
        Slice got = bytes_buffer_bytes(&out);
        Int off = 0;
        int line = 0;
        for (const char *const *l = df->dump; *l != NULL; l++, line++) {
            Int end = off;
            while (end < got.len && ((Byte *)got.p)[end] != '\n')
                end++;
            Str g = str_from_bytes((Byte *)got.p + off, end - off);
            Str want = str_from_cstr(*l);
            if (off >= got.len || !str_eq(g, want)) {
                testing_t_errorf_v(t, "%s line %d:\n got %q\nwant %q", df->name,
                                   line + 1, off < got.len ? g : BURROW_S("<end>"),
                                   want);
                break;
            }
            off = end + 1;
        }
        if (off < got.len && df->dump[line] == NULL)
            testing_t_errorf_v(t, "%s: %d bytes more than Go", df->name, got.len - off);
        arena_free(&ar);
    }
}

/* The dump of each damaged copy has to hash the same as Go's. Gives the number
 * of copies. */
static int debug_check_damage(TestingT *t, const DebugFile *files, size_t nfiles,
                              DebugDumpFn dump) {
    int n = 0;
    for (size_t i = 0; i < nfiles; i++) {
        const DebugFile *df = &files[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Slice data = file_bytes(a, df);
        for (const DebugDamage *dm = df->damage; dm->hash != NULL; dm++, n++) {
            Arena dar;
            arena_init(&dar, NULL, 0);
            Alloc *da = arena_allocator(&dar);
            Slice b = slice_make(da, TYPE_BYTE, data.len, data.len);
            memcpy(b.p, data.p, (size_t)data.len);
            if (dm->xor_ == 0)
                b.len = dm->pos;
            else
                ((Byte *)b.p)[dm->pos] ^= (Byte)dm->xor_;
            BytesBuffer out = BYTES_BUFFER(da);
            dump(da, bytes_buffer_as_io_writer(&out), b);
            Sha256Sum256Ret h = sha256_sum256(bytes_buffer_bytes(&out));
            Str got = hex_encode_to_string(da, (Slice){h.a, 8, 8, TYPE_BYTE});
            if (!str_eq(got, str_from_cstr(dm->hash))) {
                testing_t_errorf_v(t, "%s with %s at %d: dump hashes to %s, want %s",
                                   df->name, dm->xor_ == 0 ? "a cut" : "a change",
                                   dm->pos, got, dm->hash);
                if (getenv("BURROW_DEBUG_DUMP") != NULL)
                    fwrite(bytes_buffer_bytes(&out).p, 1,
                           (size_t)bytes_buffer_bytes(&out).len, stderr);
            }
            arena_free(&dar);
        }
        arena_free(&ar);
    }
    return n;
}

#endif
