/* Derived from Go's src/debug/elf/file_test.go, elf_test.go and
 * symbols_test.go. Go source: go1.27.1.
 *
 * Go's tests check a few fields of each file in its testdata against tables
 * written by hand. Here every file goes through a dump of everything debug/elf
 * and debug/dwarf can say about it, which tools/gen-debug-tests.sh made with
 * Go's packages, and the dump this makes has to be the same text. The files
 * are damaged a few dozen ways each too, and then the dumps have to hash the
 * same. TestNames checks String and GoString of every named constant and the
 * numbers around them. The rest are Go's tests that build their own input.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/compress/gzip.h"
#include "burrow/crypto/sha256.h"
#include "burrow/debug/dwarf.h"
#include "burrow/debug/elf.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/strings.h"

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

#include "debug_test_gen.h"

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

static const DebugFile *find_file(const char *name) {
    for (size_t i = 0; i < sizeof debug_files / sizeof debug_files[0]; i++)
        if (strcmp(debug_files[i].name, name) == 0)
            return &debug_files[i];
    abort();
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

static void dump_syms(Alloc *a, IoWriter w, const char *label, Slice syms, Error err) {
    fmt_fprintf_v(w, "%s n=%d%s\n", label, syms.len, errs(a, err));
    for (Int i = 0; i < syms.len; i++) {
        const ElfSymbol *s = (const ElfSymbol *)syms.p + i;
        fmt_fprintf_v(w,
                      "  %d %q info=%d %s %s other=%d %s hasver=%t vi=%d hidden=%t "
                      "section=%s value=%#x size=%#x version=%q library=%q\n",
                      i, s->name, s->info, elf_sym_bind_string(elf_st_bind(s->info), a),
                      elf_sym_type_string(elf_st_type(s->info), a), s->other,
                      elf_sym_vis_string(elf_st_visibility(s->other), a),
                      s->has_version, elf_version_index_index(s->version_index),
                      elf_version_index_is_hidden(s->version_index),
                      elf_section_index_string(s->section, a), s->value, s->size,
                      s->version, s->library);
    }
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

static const ElfDynTag dyn_value_tags[] = {
    ELF_DT_NEEDED, ELF_DT_FLAGS,  ELF_DT_FLAGS_1, ELF_DT_VERNEEDNUM,
    ELF_DT_STRSZ,  ELF_DT_PLTGOT, ELF_DT_INIT};

static const ElfDynTag dyn_string_tags[] = {ELF_DT_NEEDED, ELF_DT_SONAME, ELF_DT_RPATH,
                                            ELF_DT_RUNPATH};

static void dump(Alloc *a, IoWriter w, Slice data) {
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, data);
    ElfFile *f = elf_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (BURROW_FAILED(err) || f == NULL) {
        fmt_fprintf_v(w, "newfile err=%q\n", error_text(err));
        return;
    }
    const ElfFileHeader *h = &f->file_header;
    fmt_fprintf_v(w,
                  "header class=%s data=%s version=%s osabi=%s abiversion=%d "
                  "byteorder=%s type=%s machine=%s entry=%#x\n",
                  elf_class_string(h->cls, a), elf_data_string(h->data, a),
                  elf_version_string(h->version, a), elf_osabi_string(h->osabi, a),
                  h->abi_version, h->byte_order.vt->string(h->byte_order.data),
                  elf_type_string(h->type, a), elf_machine_string(h->machine, a),
                  h->entry);
    for (Int i = 0; i < f->sections.len; i++) {
        ElfSection *s = ((ElfSection **)f->sections.p)[i];
        const ElfSectionHeader *sh = &s->section_header;
        fmt_fprintf_v(w,
                      "section %d %q type=%s flags=%s addr=%#x offset=%#x size=%#x "
                      "link=%d info=%d addralign=%#x entsize=%#x filesize=%#x\n",
                      i, sh->name, elf_section_type_string(sh->type, a),
                      elf_section_flag_string(sh->flags, a), sh->addr, sh->offset,
                      sh->size, sh->link, sh->info, sh->addralign, sh->entsize,
                      sh->file_size);
        err = BURROW_NO_ERROR;
        Slice b = elf_section_data(s, a, &err);
        fmt_fprintf_v(w, "  data %s\n", blob(a, b, err));
        IoReadSeeker rs = elf_section_open(s, a);
        err = BURROW_NO_ERROR;
        b = io_read_all(a, io_read_seeker_as_io_reader(rs), &err);
        fmt_fprintf_v(w, "  open %s\n", blob(a, b, err));
        elf_reader_free(rs);
    }
    for (Int i = 0; i < f->progs.len; i++) {
        ElfProg *p = ((ElfProg **)f->progs.p)[i];
        const ElfProgHeader *ph = &p->prog_header;
        fmt_fprintf_v(w,
                      "prog %d type=%s flags=%s off=%#x vaddr=%#x paddr=%#x filesz=%#x "
                      "memsz=%#x align=%#x\n",
                      i, elf_prog_type_string(ph->type, a),
                      elf_prog_flag_string(ph->flags, a), ph->off, ph->vaddr, ph->paddr,
                      ph->filesz, ph->memsz, ph->align);
        IoReadSeeker rs = elf_prog_open(p, a);
        err = BURROW_NO_ERROR;
        Slice b = io_read_all(a, io_read_seeker_as_io_reader(rs), &err);
        fmt_fprintf_v(w, "  open %s\n", blob(a, b, err));
        elf_reader_free(rs);
    }
    err = BURROW_NO_ERROR;
    Slice syms = elf_file_symbols(f, a, &err);
    dump_syms(a, w, "symbols", syms, err);
    err = BURROW_NO_ERROR;
    syms = elf_file_dynamic_symbols(f, a, &err);
    dump_syms(a, w, "dynsyms", syms, err);
    err = BURROW_NO_ERROR;
    Slice imp = elf_file_imported_symbols(f, a, &err);
    fmt_fprintf_v(w, "imported n=%d%s\n", imp.len, errs(a, err));
    for (Int i = 0; i < imp.len; i++) {
        const ElfImportedSymbol *s = (const ElfImportedSymbol *)imp.p + i;
        fmt_fprintf_v(w, "  %q %q %q\n", s->name, s->version, s->library);
    }
    err = BURROW_NO_ERROR;
    Slice libs = elf_file_imported_libraries(f, a, &err);
    dump_strings(a, w, BURROW_S("libraries"), libs, err);
    for (size_t i = 0; i < sizeof dyn_string_tags / sizeof dyn_string_tags[0]; i++) {
        err = BURROW_NO_ERROR;
        Slice ss = elf_file_dyn_string(f, a, dyn_string_tags[i], &err);
        Str label =
            fmt_sprintf_v(a, "dynstring %s", elf_dyn_tag_string(dyn_string_tags[i], a));
        dump_strings(a, w, label, ss, err);
    }
    for (size_t i = 0; i < sizeof dyn_value_tags / sizeof dyn_value_tags[0]; i++) {
        err = BURROW_NO_ERROR;
        Slice vs = elf_file_dyn_value(f, a, dyn_value_tags[i], &err);
        fmt_fprintf_v(w, "dynvalue %s n=%d%s", elf_dyn_tag_string(dyn_value_tags[i], a),
                      vs.len, errs(a, err));
        for (Int k = 0; k < vs.len; k++)
            fmt_fprintf_v(w, " %#x", ((uint64_t *)vs.p)[k]);
        fmt_fprintf_v(w, "\n");
    }
    err = BURROW_NO_ERROR;
    Slice vers = elf_file_dynamic_versions(f, &err);
    fmt_fprintf_v(w, "versions n=%d%s\n", vers.len, errs(a, err));
    for (Int i = 0; i < vers.len; i++) {
        const ElfDynamicVersion *v = (const ElfDynamicVersion *)vers.p + i;
        fmt_fprintf_v(w, "  %q index=%d flags=%d deps=%d\n", v->name, v->index,
                      (Int)v->flags, v->deps.len);
        for (Int k = 0; k < v->deps.len; k++)
            fmt_fprintf_v(w, "    %q\n", ((Str *)v->deps.p)[k]);
    }
    err = BURROW_NO_ERROR;
    Slice needs = elf_file_dynamic_version_needs(f, &err);
    fmt_fprintf_v(w, "needs n=%d%s\n", needs.len, errs(a, err));
    for (Int i = 0; i < needs.len; i++) {
        const ElfDynamicVersionNeed *n = (const ElfDynamicVersionNeed *)needs.p + i;
        fmt_fprintf_v(w, "  %q n=%d\n", n->name, n->needs.len);
        for (Int k = 0; k < n->needs.len; k++) {
            const ElfDynamicVersionDep *dep =
                (const ElfDynamicVersionDep *)n->needs.p + k;
            fmt_fprintf_v(w, "    %q index=%d flags=%d\n", dep->dep, dep->index,
                          (Int)dep->flags);
        }
    }
    err = BURROW_NO_ERROR;
    DwarfData *d = elf_file_dwarf(f, a, &err);
    if (BURROW_FAILED(err) || d == NULL) {
        fmt_fprintf_v(w, "dwarf err=%q\n", error_text(err));
    } else {
        dump_dwarf(a, w, d);
    }
    elf_file_free(f);
}

/* ---------------------------------------------------------------- the tests */

static void TestDump(TestingT *t) {
    for (size_t i = 0; i < sizeof debug_files / sizeof debug_files[0]; i++) {
        const DebugFile *df = &debug_files[i];
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

static void TestDamage(TestingT *t) {
    int n = 0;
    for (size_t i = 0; i < sizeof debug_files / sizeof debug_files[0]; i++) {
        const DebugFile *df = &debug_files[i];
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
    CHECK(n > 1000);
}

/* -------------------------------------------------------------------- names */

#define NAME_KINDS(X)                                                                  \
    X(elf_version, ElfVersion)                                                         \
    X(elf_class, ElfClass)                                                             \
    X(elf_data, ElfData)                                                               \
    X(elf_osabi, ElfOSABI)                                                             \
    X(elf_type, ElfType)                                                               \
    X(elf_machine, ElfMachine)                                                         \
    X(elf_section_index, ElfSectionIndex)                                              \
    X(elf_section_type, ElfSectionType)                                                \
    X(elf_section_flag, ElfSectionFlag)                                                \
    X(elf_compression_type, ElfCompressionType)                                        \
    X(elf_prog_type, ElfProgType)                                                      \
    X(elf_prog_flag, ElfProgFlag)                                                      \
    X(elf_dyn_tag, ElfDynTag)                                                          \
    X(elf_dyn_flag, ElfDynFlag)                                                        \
    X(elf_dyn_flag1, ElfDynFlag1)                                                      \
    X(elf_n_type, ElfNType)                                                            \
    X(elf_sym_bind, ElfSymBind)                                                        \
    X(elf_sym_type, ElfSymType)                                                        \
    X(elf_sym_vis, ElfSymVis)                                                          \
    X(elf_r_x8664, ElfR_X86_64)                                                        \
    X(elf_r_aarch64, ElfR_AARCH64)                                                     \
    X(elf_r_alpha, ElfR_ALPHA)                                                         \
    X(elf_r_arm, ElfR_ARM)                                                             \
    X(elf_r386, ElfR_386)                                                              \
    X(elf_r_mips, ElfR_MIPS)                                                           \
    X(elf_r_larch, ElfR_LARCH)                                                         \
    X(elf_r_ppc, ElfR_PPC)                                                             \
    X(elf_r_ppc64, ElfR_PPC64)                                                         \
    X(elf_r_riscv, ElfR_RISCV)                                                         \
    X(elf_r390, ElfR_390)                                                              \
    X(elf_r_sparc, ElfR_SPARC)                                                         \
    X(dwarf_attr, DwarfAttr)                                                           \
    X(dwarf_tag, DwarfTag)                                                             \
    X(dwarf_class, DwarfClass)

#define NAME_WRAP(f, T)                                                                \
    static Str f##_str(long long v, Alloc *a) {                                        \
        return f##_string((T)v, a);                                                    \
    }                                                                                  \
    static Str f##_go(long long v, Alloc *a) {                                         \
        return f##_go_string((T)v, a);                                                 \
    }
NAME_KINDS(NAME_WRAP)

typedef struct NameKind {
    const char *name;
    Str (*str)(long long v, Alloc *a);
    Str (*go)(long long v, Alloc *a);
} NameKind;

#define NAME_ENTRY(f, T) {#T, f##_str, f##_go},
static const NameKind name_kinds[] = {NAME_KINDS(NAME_ENTRY)};

static void TestNames(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof debug_names / sizeof debug_names[0]; i++) {
        const DebugName *n = &debug_names[i];
        const NameKind *k = &name_kinds[n->kind];
        Str s = k->str(n->value, a);
        if (!str_eq(s, str_from_cstr(n->str)))
            testing_t_errorf_v(t, "%s(%d).String() = %q, want %q", k->name,
                               (int64_t)n->value, s, n->str);
        Str g = k->go(n->value, a);
        if (!str_eq(g, str_from_cstr(n->go)))
            testing_t_errorf_v(t, "%s(%d).GoString() = %q, want %q", k->name,
                               (int64_t)n->value, g, n->go);
    }
    arena_free(&ar);
}

/* -------------------------------------------------------- Go's other tests */

static ElfFile *must_file(TestingT *t, Alloc *a, const char *name) {
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, file_bytes(a, find_file(name)));
    ElfFile *f = elf_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (BURROW_FAILED(err) || f == NULL)
        testing_t_fatalf_v(t, "%s: %s", name, error_text(err));
    return f;
}

static const Byte compressed_want[] = {
    182, 0, 0, 0,  4, 0,   0,  0, 0, 0, 8,   1,   0,   0,   0,   0,   1,   0, 0,
    0,   0, 0, 0,  0, 0,   0,  0, 0, 0, 0,   0,   0,   0,   27,  0,   0,   0, 0,
    0,   0, 0, 0,  0, 0,   0,  2, 8, 7, 0,   0,   0,   0,   2,   1,   8,   0, 0,
    0,   0, 2, 2,  7, 0,   0,  0, 0, 2, 4,   7,   0,   0,   0,   0,   2,   1, 6,
    0,   0, 0, 0,  2, 2,   5,  0, 0, 0, 0,   3,   4,   5,   105, 110, 116, 0, 2,
    8,   5, 0, 0,  0, 0,   2,  8, 7, 0, 0,   0,   0,   4,   8,   114, 0,   0, 0,
    2,   1, 6, 0,  0, 0,   0,  5, 0, 0, 0,   0,   1,   4,   0,   0,   0,   0, 0,
    0,   0, 0, 27, 0, 0,   0,  0, 0, 0, 0,   1,   156, 179, 0,   0,   0,   6, 0,
    0,   0, 0, 1,  4, 87,  0,  0, 0, 2, 145, 108, 6,   0,   0,   0,   0,   1, 4,
    179, 0, 0, 0,  2, 145, 96, 0, 4, 8, 108, 0,   0,   0,   0,
};

/* Go's test seeks with math/rand. This takes its numbers from the generators'
 * LCG instead, which is as good at landing everywhere. */
static void TestCompressedSection(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ElfFile *f = must_file(t, a, "elf/compressed-64.obj");
    ElfSection *sec = elf_file_section(f, BURROW_S(".debug_info"));
    CHECK(sec != NULL);
    Error err = BURROW_NO_ERROR;
    Slice b = elf_section_data(sec, a, &err);
    Int n = (Int)sizeof compressed_want;
    if (BURROW_FAILED(err) || b.len != n ||
        memcmp(b.p, compressed_want, (size_t)n) != 0)
        testing_t_fatalf_v(t, "Data = %d bytes, err %q", b.len, error_text(err));

    Byte buf[sizeof compressed_want];
    bool have[sizeof compressed_want] = {0};
    Int count = 0;
    IoReadSeeker rs = elf_section_open(sec, a);
    IoReader r = io_read_seeker_as_io_reader(rs);
    IoSeeker sk = io_read_seeker_as_io_seeker(rs);
    int64_t pos = BURROW_CALL(sk, seek, 0, BURROW_IO_SEEK_END, &err);
    if (pos != n || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "want seek end %d, got %d error %q", n, pos,
                           error_text(err));
    Int got = BURROW_CALL(r, read, (Slice){buf, n, n, TYPE_BYTE}, &err);
    if (got != 0 || !errors_is(err, io_eof))
        testing_t_fatalf_v(t, "want EOF with 0 bytes, got %q with %d bytes",
                           error_text(err), got);
    uint64_t x = 1;
    while (count < n) {
        x = x * 6364136223846793005U + 1442695040888963407U;
        int whence = (int)((x >> 33) % 3);
        int64_t target = (int64_t)((x >> 20) % (uint64_t)n);
        int64_t offset = target;
        if (whence == BURROW_IO_SEEK_CURRENT)
            offset = target - pos;
        else if (whence == BURROW_IO_SEEK_END)
            offset = target - n;
        err = BURROW_NO_ERROR;
        pos = BURROW_CALL(sk, seek, offset, whence, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "seek: %s", error_text(err));
        if (pos != target)
            testing_t_fatalf_v(t, "want position %d, got %d", target, pos);
        int64_t end = pos + 16 > n ? n : pos + 16;
        Slice dst = {buf + pos, (Int)(end - pos), (Int)(end - pos), TYPE_BYTE};
        Int k = io_read_full(r, dst, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "read: %s", error_text(err));
        for (Int i = 0; i < k; i++) {
            if (!have[pos]) {
                have[pos] = true;
                count++;
            }
            pos++;
        }
    }
    if (memcmp(buf, compressed_want, sizeof buf) != 0)
        testing_t_errorf_v(t, "the bytes read after seeking are not the section's");
    elf_reader_free(rs);
    arena_free(&ar);
}

static void TestNobitsSection(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ElfFile *f = must_file(t, a, "elf/gcc-amd64-linux-exec");
    Str want = BURROW_S("unexpected read from SHT_NOBITS section");
    ElfSection *bss = elf_file_section(f, BURROW_S(".bss"));
    CHECK(bss != NULL);
    Error err = BURROW_NO_ERROR;
    (void)elf_section_data(bss, a, &err);
    if (BURROW_OK(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "bss.Data() got error %q, want error %q", error_text(err),
                           want);
    IoReadSeeker rs = elf_section_open(bss, a);
    Byte p[1];
    err = BURROW_NO_ERROR;
    (void)BURROW_CALL(io_read_seeker_as_io_reader(rs), read,
                      (Slice){p, 1, 1, TYPE_BYTE}, &err);
    if (BURROW_OK(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "r.Read(p) got error %q, want error %q", error_text(err),
                           want);
    elf_reader_free(rs);
    arena_free(&ar);
}

static void put16(Byte *p, uint32_t v) {
    p[0] = (Byte)v;
    p[1] = (Byte)(v >> 8);
}

static void put32(Byte *p, uint32_t v) {
    put16(p, v & 0xffff);
    put16(p + 2, v >> 16);
}

typedef struct Sect32 {
    uint32_t name, type, flags, off, size, link, info, addralign, entsize;
} Sect32;

static Byte *put_sect32(Byte *p, Sect32 s) {
    uint32_t v[10] = {s.name, s.type, s.flags, 0,           s.off,
                      s.size, s.link, s.info,  s.addralign, s.entsize};
    for (Int i = 0; i < 10; i++)
        put32(p + 4 * i, v[i]);
    return p + 40;
}

static Byte *put_cstr(Byte *p, const char *s) {
    size_t k = strlen(s) + 1;
    memcpy(p, s, k);
    return p + k;
}

/* A file with at least 0xff00 sections, which is too big to keep, so it is
 * made here the way Go's test makes it. */
static void TestLargeNumberOfSections(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Int size = 0x55AF1C;
    Byte *data = (Byte *)mem_alloc(a, (size_t)size, 1);
    CHECK(data != NULL);
    if (data == NULL)
        return;
    memset(data, 0, (size_t)size);
    static const Byte ident[] = {0x7F, 'E', 'L', 'F', 0x01, 0x01, 0x01};
    memcpy(data, ident, sizeof ident);
    put16(data + 16, 1);        /* Type */
    put16(data + 18, 3);        /* Machine */
    put32(data + 20, 1);        /* Version */
    put32(data + 32, 0x2DD44C); /* Shoff */
    put16(data + 40, 0x34);     /* Ehsize */
    put16(data + 46, 0x28);     /* Shentsize */
    put16(data + 48, 0);        /* Shnum */
    put16(data + 50, 0xFFFF);   /* Shstrndx */
    /* Section [65295] is zeros but for its last 48 bytes. */
    Byte *p = data + 0x17E944 - 48;
    for (uint32_t i = 0; i < 12; i++, p += 4)
        put32(p, 0xFF00 | i);
    char name[32];
    p = put_cstr(p + 1, "y.c");
    for (int i = 1; i <= 65288; i++) {
        snprintf(name, sizeof name, "var_%04x", (unsigned)i);
        p = put_cstr(p, name);
    }
    p++;
    static const char *const first[] = {".symtab", ".strtab", ".shstrtab",
                                        ".text",   ".data",   ".bss"};
    for (size_t i = 0; i < sizeof first / sizeof first[0]; i++)
        p = put_cstr(p, first[i]);
    for (int i = 1; i <= 65288; i++) {
        snprintf(name, sizeof name, "section_%04x", (unsigned)i);
        p = put_cstr(p, name);
    }
    p = put_cstr(p, ".comment");
    p = put_cstr(p, ".note.GNU-stack");
    p = put_cstr(p, ".symtab_shndx");
    CHECK(p == data + 0x2DD44C);

    const uint32_t write = (uint32_t)ELF_SHF_WRITE, alloc = (uint32_t)ELF_SHF_ALLOC;
    p = put_sect32(p, (Sect32){.size = 0xFF12, .link = 0xFF11});
    p = put_sect32(p, (Sect32){.name = 0x1B,
                               .type = ELF_SHT_PROGBITS,
                               .flags = alloc | (uint32_t)ELF_SHF_EXECINSTR,
                               .off = 0x34,
                               .addralign = 1});
    p = put_sect32(p, (Sect32){.name = 0x21,
                               .type = ELF_SHT_PROGBITS,
                               .flags = write | alloc,
                               .off = 0x34,
                               .addralign = 1});
    p = put_sect32(p, (Sect32){.name = 0x27,
                               .type = ELF_SHT_NOBITS,
                               .flags = write | alloc,
                               .off = 0x34,
                               .addralign = 1});
    for (uint32_t i = 0; i < 65288; i++)
        p = put_sect32(p, (Sect32){.name = 0x2C + i * 13,
                                   .type = ELF_SHT_PROGBITS,
                                   .flags = write | alloc,
                                   .off = 0x34 + i * 4,
                                   .size = 4,
                                   .addralign = 4});
    p = put_sect32(
        p, (Sect32){.name = 0x0CF394,
                    .type = ELF_SHT_PROGBITS,
                    .flags = (uint32_t)ELF_SHF_MERGE | (uint32_t)ELF_SHF_STRINGS,
                    .off = 0x03FC54,
                    .size = 0x27,
                    .addralign = 1,
                    .entsize = 1});
    p = put_sect32(p, (Sect32){.name = 0x0CF39D,
                               .type = ELF_SHT_PROGBITS,
                               .off = 0x03FC7B,
                               .addralign = 1});
    p = put_sect32(p, (Sect32){.name = 0x01,
                               .type = ELF_SHT_SYMTAB,
                               .off = 0x03FC7C,
                               .size = 0x0FF0A0,
                               .link = 0xFF10,
                               .info = 2,
                               .addralign = 4,
                               .entsize = 0x10});
    p = put_sect32(p, (Sect32){.name = 0x0CF3AD,
                               .type = ELF_SHT_SYMTAB_SHNDX,
                               .off = 0x13ED1C,
                               .size = 0x03FC28,
                               .link = 0xFF0E,
                               .addralign = 4,
                               .entsize = 4});
    p = put_sect32(p, (Sect32){.name = 0x09,
                               .type = ELF_SHT_STRTAB,
                               .off = 0x17E944,
                               .size = 0x08F74D,
                               .addralign = 1});
    p = put_sect32(p, (Sect32){.name = 0x11,
                               .type = ELF_SHT_STRTAB,
                               .off = 0x20E091,
                               .size = 0x0CF3BB,
                               .addralign = 1});
    CHECK(p == data + size);

    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, (Slice){data, size, size, TYPE_BYTE});
    ElfFile *f = elf_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (BURROW_FAILED(err) || f == NULL)
        testing_t_fatalf_v(t, "cannot create file from data: %s", error_text(err));
    const ElfFileHeader *h = &f->file_header;
    CHECK(h->cls == ELF_ELFCLASS32 && h->data == ELF_ELFDATA2LSB &&
          h->version == ELF_EV_CURRENT && h->osabi == ELF_ELFOSABI_NONE &&
          h->abi_version == 0 && h->type == ELF_ET_REL && h->machine == ELF_EM_386 &&
          h->entry == 0);
    if (f->sections.len != 65298)
        testing_t_errorf_v(t, "len(Sections) = %d, want %d", f->sections.len, 65298);
    if (f->sections.len > 10) {
        const ElfSectionHeader *sh =
            &((ElfSection **)f->sections.p)[10]->section_header;
        CHECK(str_eq(sh->name, BURROW_S("section_0007")));
        CHECK(sh->type == ELF_SHT_PROGBITS);
        CHECK(sh->flags == (ElfSectionFlag)(ELF_SHF_WRITE + ELF_SHF_ALLOC));
        CHECK(sh->addr == 0 && sh->offset == 0x4c && sh->size == 4 && sh->link == 0 &&
              sh->info == 0 && sh->addralign == 4 && sh->entsize == 0 &&
              sh->file_size == 4);
    }
    elf_file_free(f);
    arena_free(&ar);
}

static void TestIssue10996(TestingT *t) {
    static const char data[] = "\177ELF\002\001\0010000000000000"
                               "\0010000000000000000000\000"
                               "\000\000\000\000\000\000\00000000000\000\000\000\000"
                               "0000";
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Int n = (Int)sizeof data - 1;
    BytesReader *br =
        bytes_new_reader(a, (Slice){(void *)(uintptr_t)data, n, n, TYPE_BYTE});
    (void)elf_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "opening invalid ELF file unexpectedly succeeded");
    arena_free(&ar);
}

static void TestDynValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ElfFile *f = must_file(t, a, "elf/gcc-amd64-linux-exec");
    Error err = BURROW_NO_ERROR;
    Slice vals = elf_file_dyn_value(f, a, ELF_DT_VERNEEDNUM, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "DynValue(DT_VERNEEDNUM): got unexpected error %s",
                           error_text(err));
    if (vals.len != 1 || ((uint64_t *)vals.p)[0] != 1)
        testing_t_errorf_v(t, "DynValue(DT_VERNEEDNUM): got %d values, want [1]",
                           vals.len);
    arena_free(&ar);
}

/* Corrupt DWARF has to say it is bad DWARF, and not that it is bad zlib. */
static void TestIssue59208(TestingT *t) {
    static const Byte z[] = {
        0x78, 0x9c, 0x5c, 0x4d, 0xb9, 0xd,  0x80, 0x30, 0xc,  0x3c, 0x7,  0x27,
        0xdc, 0xe,  0xc,  0x46, 0x4b, 0x8b, 0x14, 0x51, 0x20, 0x16, 0xa1, 0x67,
        0x8b, 0x2c, 0x88, 0xec, 0x44, 0xc2, 0xe2, 0x8a, 0xdc, 0x1b, 0x59, 0x0,
        0x28, 0xc,  0x34, 0x9,  0x7f, 0x22, 0x96, 0xa0, 0x13, 0x67, 0x27, 0xa1,
        0x53, 0xea, 0x4e, 0x47, 0x58, 0x7a, 0x98, 0x8d, 0x26, 0xcd, 0xfb, 0x71,
        0x21, 0x31, 0x87, 0x7f, 0xca, 0xf3, 0x1b, 0x7a, 0x21, 0xfa, 0x3f, 0x23,
        0x4f, 0x3,  0x50, 0x7a, 0xb9, 0xda, 0xfc, 0xae, 0xc3, 0x35, 0x77, 0x1b,
        0x94, 0xd5, 0x82, 0x37, 0x0,  0x0,  0xff, 0xff, 0x65, 0xfb, 0x7,  0x6e};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = file_bytes(a, find_file("elf/compressed-64.obj"));
    ElfFile *f = must_file(t, a, "elf/compressed-64.obj");
    ElfSection *sec = elf_file_section(f, BURROW_S(".debug_info"));
    CHECK(sec != NULL);
    if (sec == NULL)
        return;
    Slice dn = slice_make(a, TYPE_BYTE, data.len, data.len);
    Int zoff = (Int)sec->section_header.offset + (Int)sec->compression_offset;
    Int end = (Int)(sec->section_header.offset + sec->section_header.file_size);
    memcpy(dn.p, data.p, (size_t)zoff);
    memcpy((Byte *)dn.p + zoff, z, sizeof z);
    memcpy((Byte *)dn.p + end, (Byte *)data.p + end, (size_t)(data.len - end));
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, dn);
    ElfFile *nf = elf_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (BURROW_FAILED(err) || nf == NULL)
        testing_t_fatalf_v(t, "%s", error_text(err));
    (void)elf_file_dwarf(nf, a, &err);
    if (BURROW_OK(err) ||
        !strings_contains(error_text(err), BURROW_S("decoding dwarf section info")))
        testing_t_errorf_v(t, "DWARF = %q; want %q", error_text(err),
                           "decoding dwarf section info");
    arena_free(&ar);
}

static void TestNewFileShortReader(TestingT *t) {
    static const Byte four[] = {0x7f, 'E', 'L', 'F'};
    static const Byte fifteen[15] = {0};
    const struct {
        const char *name;
        const Byte *p;
        Int n;
    } tests[] = {
        {"empty", fifteen, 0},
        {"one byte", four, 1},
        {"four bytes", four, 4},
        {"fifteen bytes", fifteen, 15},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Slice b = {(void *)(uintptr_t)tests[i].p, tests[i].n, tests[i].n, TYPE_BYTE};
        (void)elf_new_file(a, bytes_reader_as_io_reader_at(bytes_new_reader(a, b)),
                           &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "%s: NewFile with short data: got nil error",
                               tests[i].name);
            continue;
        }
        if (errors_as(err, TYPE_ELF_FORMAT_ERROR) == NULL)
            testing_t_errorf_v(
                t, "%s: NewFile with short data: got %q, want an ElfFormatError",
                tests[i].name, error_text(err));
    }
    arena_free(&ar);
}

static void TestOpenEmptyFile(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("elftest"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Str name = fmt_sprintf_v(a, "%s/empty", dir);
    err = os_write_file(name, (Slice){NULL, 0, 0, TYPE_BYTE}, 0644);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    ElfFile *f = elf_open(a, name, &err);
    if (BURROW_OK(err)) {
        testing_t_errorf_v(t, "Open on empty file: got nil error, want non-nil");
        elf_file_close(f);
    } else if (errors_as(err, TYPE_ELF_FORMAT_ERROR) == NULL) {
        testing_t_errorf_v(t, "Open on empty file: got %q, want an ElfFormatError",
                           error_text(err));
    }
    (void)os_remove_all(dir);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestNames)                                                                       \
    X(TestDump)                                                                        \
    X(TestDamage)                                                                      \
    X(TestCompressedSection)                                                           \
    X(TestNobitsSection)                                                               \
    X(TestLargeNumberOfSections)                                                       \
    X(TestIssue10996)                                                                  \
    X(TestDynValue)                                                                    \
    X(TestIssue59208)                                                                  \
    X(TestNewFileShortReader)                                                          \
    X(TestOpenEmptyFile)

TESTING_MAIN(TESTS)
