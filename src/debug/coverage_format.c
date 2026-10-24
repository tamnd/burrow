/* internal/coverage: the meta-data and counter data files, and the encoders
 * and decoders for them.
 *
 * Derived from Go's src/internal/coverage/defs.go, stringtab/stringtab.go,
 * uleb128/uleb128.go, slicereader/slicereader.go, slicewriter/slicewriter.go,
 * encodemeta/encode.go, encodemeta/encodefile.go, decodemeta/decode.go,
 * decodemeta/decodefile.go, encodecounter/encode.go and
 * decodecounter/decodecounterfile.go. Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "coverage_internal.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/encoding/binary.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/hash.h"
#include "burrow/hash/fnv.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define COV_S(lit) BURROW_S(lit)

/* Not in Go: running out of memory panics, as it does there. */
static void *cov_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size == 0 ? 1 : size, align);
    if (p == NULL)
        panic_str(COV_S("coverage: out of memory"));
    return p;
}

#define COV_NEW(a, T) ((T *)cov_alloc((a), sizeof(T), _Alignof(T)))

static Slice cov_bytes(const void *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static Slice cov_str_bytes(Str s) {
    return cov_bytes(s.p, s.len);
}

/* ------------------------------------------------------------------- defs */

const Byte burrow__cov_meta_magic[4] = {'\x00', '\x63', '\x76', '\x6d'};
const Byte burrow__cov_counter_magic[4] = {'\x00', '\x63', '\x77', '\x6d'};

Str burrow__cov_counter_mode_string(CovCounterMode cm) {
    if (cm == COV_CTR_MODE_SET)
        return COV_S("set");
    if (cm == COV_CTR_MODE_COUNT)
        return COV_S("count");
    if (cm == COV_CTR_MODE_ATOMIC)
        return COV_S("atomic");
    if (cm == COV_CTR_MODE_REG_ONLY)
        return COV_S("regonly");
    if (cm == COV_CTR_MODE_TEST_MAIN)
        return COV_S("testmain");
    return COV_S("<invalid>");
}

CovCounterMode burrow__cov_parse_counter_mode(Str mode) {
    if (str_eq(mode, COV_S("set")))
        return COV_CTR_MODE_SET;
    if (str_eq(mode, COV_S("count")))
        return COV_CTR_MODE_COUNT;
    if (str_eq(mode, COV_S("atomic")))
        return COV_CTR_MODE_ATOMIC;
    if (str_eq(mode, COV_S("regonly")))
        return COV_CTR_MODE_REG_ONLY;
    if (str_eq(mode, COV_S("testmain")))
        return COV_CTR_MODE_TEST_MAIN;
    return COV_CTR_MODE_INVALID;
}

Str burrow__cov_counter_granularity_string(CovCounterGranularity cm) {
    if (cm == COV_CTR_GRANULARITY_PER_BLOCK)
        return COV_S("perblock");
    if (cm == COV_CTR_GRANULARITY_PER_FUNC)
        return COV_S("perfunc");
    return COV_S("<invalid>");
}

const Type burrow__cov_coverable_unit_type = {
    {(const Byte *)"CoverableUnit", 13},
    {(const Byte *)"internal/coverage", 17},
    KIND_STRUCT,
    (uint32_t)sizeof(CovCoverableUnit),
    (uint16_t)_Alignof(CovCoverableUnit),
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

/* [][]byte */
const Type burrow__cov_blob_type = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_BYTE,
    NULL,
    0,
    0x63766273U, /* "cvbs" */
    NULL,
};

/* ---------------------------------------------------------------- uleb128 */

Slice burrow__cov_append_uleb128(Alloc *a, Slice b, uint64_t v) {
    if (b.elem == NULL)
        b.elem = TYPE_BYTE;
    for (;;) {
        Byte c = (Byte)(v & 0x7f);
        v >>= 7;
        if (v != 0)
            c |= 0x80;
        b = BURROW_APPEND(Byte, a, b, c);
        if ((c & 0x80) == 0)
            break;
    }
    return b;
}

/* Not in Go: how many bytes AppendUleb128 would add for v. */
static uint32_t cov_uleb128_len(uint64_t v) {
    uint32_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------ slicereader */

CovSliceReader *burrow__cov_new_slice_reader(Alloc *a, Slice b, bool readonly) {
    CovSliceReader *r = COV_NEW(a, CovSliceReader);
    r->b = b;
    r->readonly = readonly;
    r->a = a;
    return r;
}

Int burrow__cov_slice_reader_read(CovSliceReader *r, Slice b, Error *err) {
    (void)err;
    Int amt = b.len;
    Slice toread = slice_sub(r->b, (Int)r->off, r->b.len);
    if (toread.len < amt)
        amt = toread.len;
    if (amt > 0)
        memmove(b.p, toread.p, (size_t)amt);
    r->off += amt;
    return amt;
}

int64_t burrow__cov_slice_reader_seek(CovSliceReader *r, int64_t offset, int whence,
                                      Error *err) {
    int64_t n = (int64_t)r->b.len;
    if (whence == BURROW_IO_SEEK_START) {
        if (offset < 0 || offset > n) {
            BURROW_OUT(err, fmt_errorf_v("invalid seek: new offset %d (out of range [0 %d]",
                                         offset, n));
            return 0;
        }
        r->off = offset;
        return offset;
    }
    if (whence == BURROW_IO_SEEK_CURRENT) {
        int64_t newoff = r->off + offset;
        if (newoff < 0 || newoff > n) {
            BURROW_OUT(err, fmt_errorf_v("invalid seek: new offset %d (out of range [0 %d]",
                                         newoff, n));
            return 0;
        }
        r->off = newoff;
        return r->off;
    }
    if (whence == BURROW_IO_SEEK_END) {
        int64_t newoff = n + offset;
        if (newoff < 0 || newoff > n) {
            BURROW_OUT(err, fmt_errorf_v("invalid seek: new offset %d (out of range [0 %d]",
                                         newoff, n));
            return 0;
        }
        r->off = newoff;
        return r->off;
    }
    /* other modes are not supported */
    int64_t w = whence;
    BURROW_OUT(err, fmt_errorf_v("unsupported seek mode %d", w));
    return 0;
}

int64_t burrow__cov_slice_reader_offset(const CovSliceReader *r) {
    return r->off;
}

/* r.b[r.off], with Go's check. */
static Byte cov_slr_byte(CovSliceReader *r) {
    if (r->off < 0 || r->off >= (int64_t)r->b.len)
        runtime_index_out_of_range((Int)r->off, r->b.len);
    return ((const Byte *)r->b.p)[r->off];
}

uint8_t burrow__cov_slice_reader_read_uint8(CovSliceReader *r) {
    uint8_t rv = cov_slr_byte(r);
    r->off += 1;
    return rv;
}

uint32_t burrow__cov_slice_reader_read_uint32(CovSliceReader *r) {
    Int end = (Int)r->off + 4;
    uint32_t rv = binary_little_endian_uint32(slice_sub3(r->b, (Int)r->off, end, end));
    r->off += 4;
    return rv;
}

uint64_t burrow__cov_slice_reader_read_uint64(CovSliceReader *r) {
    Int end = (Int)r->off + 8;
    uint64_t rv = binary_little_endian_uint64(slice_sub3(r->b, (Int)r->off, end, end));
    r->off += 8;
    return rv;
}

uint64_t burrow__cov_slice_reader_read_uleb128(CovSliceReader *r) {
    uint64_t value = 0;
    unsigned shift = 0;

    for (;;) {
        Byte b = cov_slr_byte(r);
        r->off++;
        /* Not in Go: a shift of 64 or more gives zero in Go and is undefined
         * in C, so it is spelled out. */
        if (shift < 64)
            value |= ((uint64_t)(b & 0x7F) << shift);
        if ((b & 0x80) == 0)
            break;
        shift += 7;
    }
    return value;
}

Str burrow__cov_slice_reader_read_string(CovSliceReader *r, int64_t len) {
    Slice b = slice_sub(r->b, (Int)r->off, (Int)(r->off + len));
    r->off += len;
    Str s = str_from_bytes(b.p, b.len);
    if (r->readonly)
        return s; /* backed by RO memory, ok to make unsafe string */
    return strings_clone(r->a, s);
}

static const Type cov_slice_reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"internal/coverage/slicereader", 29},
    KIND_STRUCT,
    (uint32_t)sizeof(CovSliceReader),
    (uint16_t)_Alignof(CovSliceReader),
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

static Int cov_slr_vt_read(void *self, Slice p, Error *err) {
    return burrow__cov_slice_reader_read((CovSliceReader *)self, p, err);
}

static const IoReaderVT cov_slice_reader_vt = {&cov_slice_reader_desc, cov_slr_vt_read};

IoReader burrow__cov_slice_reader_as_io_reader(CovSliceReader *r) {
    return (IoReader){&cov_slice_reader_vt, r};
}

/* Not in Go: binary.Read of n bytes from a slicereader. io.ReadFull would wait
 * forever on one that has run out, since its Read gives no bytes and no error
 * at the end, so this says io_err_unexpected_eof instead, or io_eof if there
 * was nothing to read at all. */
static Error cov_slr_read_full(CovSliceReader *r, Byte *buf, Int n) {
    Int got = burrow__cov_slice_reader_read(r, cov_bytes(buf, n), NULL);
    if (got == n)
        return BURROW_NO_ERROR;
    return got == 0 ? io_eof : io_err_unexpected_eof;
}

/* ------------------------------------------------------------ slicewriter */

Int burrow__cov_write_seeker_write(CovWriteSeeker *sws, Slice p, Error *err) {
    (void)err;
    if (sws->payload.elem == NULL)
        sws->payload.elem = TYPE_BYTE;
    Int amt = p.len;
    Slice towrite = slice_sub(sws->payload, (Int)sws->off, sws->payload.len);
    if (towrite.len < amt) {
        sws->payload = slice_append(sws->a, sws->payload, NULL, amt - towrite.len);
        towrite = slice_sub(sws->payload, (Int)sws->off, sws->payload.len);
    }
    if (amt > 0)
        memmove(towrite.p, p.p, (size_t)amt);
    sws->off += amt;
    return amt;
}

int64_t burrow__cov_write_seeker_seek(CovWriteSeeker *sws, int64_t offset, int whence,
                                      Error *err) {
    int64_t n = (int64_t)sws->payload.len;
    if (whence == BURROW_IO_SEEK_START) {
        if (sws->off != offset && (offset < 0 || offset > n)) {
            BURROW_OUT(err, fmt_errorf_v("invalid seek: new offset %d (out of range [0 %d]",
                                         offset, n));
            return 0;
        }
        sws->off = offset;
        return offset;
    }
    if (whence == BURROW_IO_SEEK_CURRENT) {
        int64_t newoff = sws->off + offset;
        if (newoff != sws->off && (newoff < 0 || newoff > n)) {
            BURROW_OUT(err, fmt_errorf_v("invalid seek: new offset %d (out of range [0 %d]",
                                         newoff, n));
            return 0;
        }
        sws->off += offset;
        return sws->off;
    }
    if (whence == BURROW_IO_SEEK_END) {
        int64_t newoff = n + offset;
        if (newoff != sws->off && (newoff < 0 || newoff > n)) {
            BURROW_OUT(err, fmt_errorf_v("invalid seek: new offset %d (out of range [0 %d]",
                                         newoff, n));
            return 0;
        }
        sws->off = newoff;
        return sws->off;
    }
    /* other modes not supported */
    int64_t w = whence;
    BURROW_OUT(err, fmt_errorf_v("unsupported seek mode %d", w));
    return 0;
}

Slice burrow__cov_write_seeker_bytes_written(CovWriteSeeker *sws) {
    if (sws->payload.elem == NULL)
        sws->payload.elem = TYPE_BYTE;
    return sws->payload;
}

Int burrow__cov_write_seeker_read(CovWriteSeeker *sws, Slice p, Error *err) {
    (void)err;
    Int amt = p.len;
    Slice toread = slice_sub(sws->payload, (Int)sws->off, sws->payload.len);
    if (toread.len < amt)
        amt = toread.len;
    if (amt > 0)
        memmove(p.p, toread.p, (size_t)amt);
    sws->off += amt;
    return amt;
}

static const Type cov_write_seeker_desc = {
    {(const Byte *)"WriteSeeker", 11},
    {(const Byte *)"internal/coverage/slicewriter", 29},
    KIND_STRUCT,
    (uint32_t)sizeof(CovWriteSeeker),
    (uint16_t)_Alignof(CovWriteSeeker),
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

static Int cov_sws_vt_write(void *self, Slice p, Error *err) {
    return burrow__cov_write_seeker_write((CovWriteSeeker *)self, p, err);
}

static int64_t cov_sws_vt_seek(void *self, int64_t offset, int whence, Error *err) {
    return burrow__cov_write_seeker_seek((CovWriteSeeker *)self, offset, whence, err);
}

static const IoWriteSeekerVT cov_write_seeker_vt = {
    {&cov_write_seeker_desc, cov_sws_vt_write},
    {&cov_write_seeker_desc, cov_sws_vt_seek},
};

IoWriteSeeker burrow__cov_write_seeker_as_io_write_seeker(CovWriteSeeker *sws) {
    return (IoWriteSeeker){&cov_write_seeker_vt, sws};
}

/* -------------------------------------------------------------- stringtab */

void burrow__cov_strtab_init_writer(CovStrtabWriter *stw, Alloc *a) {
    stw->a = a;
    stw->stab = map_make(a, TYPE_STRING, TYPE_UINT32, 0);
    stw->strs = slice_make(a, TYPE_STRING, 0, 0);
    stw->tmp = slice_make(a, TYPE_BYTE, 64, 64);
    stw->frozen = false;
}

uint32_t burrow__cov_strtab_nentries(const CovStrtabWriter *stw) {
    return (uint32_t)stw->strs.len;
}

uint32_t burrow__cov_strtab_lookup(CovStrtabWriter *stw, Str s) {
    const uint32_t *got = map_get(stw->stab, &s);
    if (got != NULL)
        return *got;
    if (stw->frozen)
        panic_str(COV_S("internal error: string table previously frozen"));
    uint32_t idx = (uint32_t)stw->strs.len;
    Str c = strings_clone(stw->a, s);
    map_set(stw->stab, &c, &idx);
    stw->strs = BURROW_APPEND(Str, stw->a, stw->strs, c);
    return idx;
}

uint32_t burrow__cov_strtab_size(CovStrtabWriter *stw) {
    uint32_t rval = 0;
    rval += cov_uleb128_len((uint64_t)stw->strs.len);
    for (Int i = 0; i < stw->strs.len; i++) {
        Str s = BURROW_AT(Str, stw->strs, i);
        uint64_t slen = (uint64_t)s.len;
        rval += cov_uleb128_len(slen) + (uint32_t)slen;
    }
    return rval;
}

static Error cov_strtab_wr128(CovStrtabWriter *stw, IoWriter w, uint64_t v) {
    stw->tmp.len = 0;
    stw->tmp = burrow__cov_append_uleb128(stw->a, stw->tmp, v);
    Error err = BURROW_NO_ERROR;
    Int nw = w.vt->write(w.data, stw->tmp, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("writing string table: %v", err);
    if (nw != stw->tmp.len)
        return fmt_errorf_v("short write emitting stringtab uleb");
    return BURROW_NO_ERROR;
}

Error burrow__cov_strtab_write(CovStrtabWriter *stw, IoWriter w) {
    Error err = cov_strtab_wr128(stw, w, (uint64_t)stw->strs.len);
    if (BURROW_FAILED(err))
        return err;
    for (Int i = 0; i < stw->strs.len; i++) {
        Str s = BURROW_AT(Str, stw->strs, i);
        err = cov_strtab_wr128(stw, w, (uint64_t)s.len);
        if (BURROW_FAILED(err))
            return err;
        Int nw = w.vt->write(w.data, cov_str_bytes(s), &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("writing string table: %v", err);
        if (nw != s.len)
            return fmt_errorf_v("short write emitting stringtab");
    }
    return BURROW_NO_ERROR;
}

void burrow__cov_strtab_freeze(CovStrtabWriter *stw) {
    stw->frozen = true;
}

CovStrtabReader *burrow__cov_new_strtab_reader(Alloc *a, CovSliceReader *r) {
    CovStrtabReader *str = COV_NEW(a, CovStrtabReader);
    str->r = r;
    str->strs = slice_make(a, TYPE_STRING, 0, 0);
    return str;
}

void burrow__cov_strtab_reader_read(CovStrtabReader *str) {
    Int num_entries = (Int)burrow__cov_slice_reader_read_uleb128(str->r);
    str->strs = slice_make(str->r->a, TYPE_STRING, 0, num_entries);
    for (Int idx = 0; idx < num_entries; idx++) {
        uint64_t slen = burrow__cov_slice_reader_read_uleb128(str->r);
        Str s = burrow__cov_slice_reader_read_string(str->r, (int64_t)slen);
        str->strs = BURROW_APPEND(Str, str->r->a, str->strs, s);
    }
}

Int burrow__cov_strtab_reader_entries(const CovStrtabReader *str) {
    return str->strs.len;
}

Str burrow__cov_strtab_reader_get(const CovStrtabReader *str, uint32_t idx) {
    if ((Int)idx >= str->strs.len)
        runtime_index_out_of_range((Int)idx, str->strs.len);
    return BURROW_AT(Str, str->strs, (Int)idx);
}

/* ------------------------------------------------------------- encodemeta */

struct CovMetaDataBuilder {
    CovStrtabWriter stab;
    Slice funcs; /* funcDesc: the encoded bytes, a Slice of Slices */
    Slice tmp;   /* temp work slice */
    Hash h;
    uint32_t pkgpath;
    uint32_t pkgname;
    uint32_t modpath;
    Error werr;
    Alloc *a;
};

static void cov_hash_str(Hash h, Str s) {
    hash_write(h, cov_str_bytes(s), NULL);
}

CovMetaDataBuilder *burrow__cov_new_meta_data_builder(Alloc *a, Str pkgpath, Str pkgname,
                                                      Str modulepath, Error *err) {
    if (pkgpath.len == 0) {
        BURROW_OUT(err, fmt_errorf_v("invalid empty package path"));
        return NULL;
    }
    CovMetaDataBuilder *x = COV_NEW(a, CovMetaDataBuilder);
    x->a = a;
    x->tmp = slice_make(a, TYPE_BYTE, 0, 256);
    x->funcs = slice_make(a, &burrow__cov_blob_type, 0, 0);
    x->h = fnv_new128a(a);
    burrow__cov_strtab_init_writer(&x->stab, a);
    burrow__cov_strtab_lookup(&x->stab, BURROW_STR_EMPTY);
    x->pkgpath = burrow__cov_strtab_lookup(&x->stab, pkgpath);
    x->pkgname = burrow__cov_strtab_lookup(&x->stab, pkgname);
    x->modpath = burrow__cov_strtab_lookup(&x->stab, modulepath);
    cov_hash_str(x->h, pkgpath);
    cov_hash_str(x->h, pkgname);
    cov_hash_str(x->h, modulepath);
    return x;
}

static void cov_h32(uint32_t x, Hash h) {
    Byte tmp[4];
    binary_little_endian_put_uint32(cov_bytes(tmp, 4), x);
    hash_write(h, cov_bytes(tmp, 4), NULL);
}

/* hashFuncDesc incorporates a given function 'f' into the hash 'h'. */
static void cov_hash_func_desc(Hash h, const CovFuncDesc *f) {
    cov_hash_str(h, f->funcname);
    cov_hash_str(h, f->srcfile);
    for (Int i = 0; i < f->units.len; i++) {
        CovCoverableUnit u = BURROW_AT(CovCoverableUnit, f->units, i);
        cov_h32(u.st_line, h);
        cov_h32(u.st_col, h);
        cov_h32(u.en_line, h);
        cov_h32(u.en_col, h);
        cov_h32(u.nx_stmts, h);
    }
    uint32_t lit = 0;
    if (f->lit)
        lit = 1;
    cov_h32(lit, h);
}

uint64_t burrow__cov_meta_data_builder_add_func(CovMetaDataBuilder *b, CovFuncDesc f) {
    cov_hash_func_desc(b->h, &f);
    Alloc *a = b->a;
    b->tmp.len = 0;
    b->tmp = burrow__cov_append_uleb128(a, b->tmp, (uint64_t)f.units.len);
    b->tmp = burrow__cov_append_uleb128(a, b->tmp, burrow__cov_strtab_lookup(&b->stab, f.funcname));
    b->tmp = burrow__cov_append_uleb128(a, b->tmp, burrow__cov_strtab_lookup(&b->stab, f.srcfile));
    for (Int i = 0; i < f.units.len; i++) {
        CovCoverableUnit u = BURROW_AT(CovCoverableUnit, f.units, i);
        b->tmp = burrow__cov_append_uleb128(a, b->tmp, u.st_line);
        b->tmp = burrow__cov_append_uleb128(a, b->tmp, u.st_col);
        b->tmp = burrow__cov_append_uleb128(a, b->tmp, u.en_line);
        b->tmp = burrow__cov_append_uleb128(a, b->tmp, u.en_col);
        b->tmp = burrow__cov_append_uleb128(a, b->tmp, u.nx_stmts);
    }
    uint64_t lit = 0;
    if (f.lit)
        lit = 1;
    b->tmp = burrow__cov_append_uleb128(a, b->tmp, lit);
    Slice encoded = slice_make(a, TYPE_BYTE, b->tmp.len, b->tmp.len);
    slice_copy(encoded, b->tmp);
    uint64_t rv = (uint64_t)b->funcs.len;
    b->funcs = BURROW_APPEND(Slice, a, b->funcs, encoded);
    return rv;
}

static void cov_report_write_error(CovMetaDataBuilder *b, Error err) {
    if (BURROW_OK(b->werr))
        b->werr = err;
}

static void cov_builder_wr_uint32(CovMetaDataBuilder *b, IoWriteSeeker w, uint32_t v) {
    Byte tmp[4];
    binary_little_endian_put_uint32(cov_bytes(tmp, 4), v);
    Error err = BURROW_NO_ERROR;
    Int nw = w.vt->writer.write(w.data, cov_bytes(tmp, 4), &err);
    if (BURROW_FAILED(err))
        cov_report_write_error(b, err);
    else if (nw != 4)
        cov_report_write_error(b, fmt_errorf_v("short write"));
}

static int64_t cov_emit_func_offsets(CovMetaDataBuilder *b, IoWriteSeeker w, int64_t off) {
    Int n_funcs = b->funcs.len;
    int64_t foff = COV_META_HEADER_SIZE + (int64_t)burrow__cov_strtab_size(&b->stab) +
                   (int64_t)n_funcs * 4;
    for (Int idx = 0; idx < n_funcs; idx++) {
        cov_builder_wr_uint32(b, w, (uint32_t)foff);
        foff += (int64_t)BURROW_AT(Slice, b->funcs, idx).len;
    }
    return off + ((int64_t)b->funcs.len * 4);
}

static int64_t cov_emit_func(IoWriteSeeker w, int64_t off, Slice f, Error *errp) {
    Int ew = f.len;
    Error err = BURROW_NO_ERROR;
    Int nw = w.vt->writer.write(w.data, f, &err);
    if (BURROW_FAILED(err)) {
        *errp = err;
        return 0;
    }
    if (ew != nw) {
        *errp = fmt_errorf_v("short write emitting coverage meta-data");
        return 0;
    }
    return off + ew;
}

Error burrow__cov_meta_data_builder_emit(CovMetaDataBuilder *b, IoWriteSeeker w,
                                         Byte digest[16]) {
    /* Emit header.  Length will initially be zero, we'll back-patch it later. */
    Slice sum = hash_sum(b->a, b->h, slice_make(b->a, TYPE_BYTE, 0, 16));
    memset(digest, 0, 16);
    memcpy(digest, sum.p, (size_t)(sum.len < 16 ? sum.len : 16));
    /* hash and length initially zero, will be back-patched */
    Byte mh[COV_META_HEADER_SIZE] = {0};
    binary_little_endian_put_uint32(cov_bytes(mh + 4, 4), b->pkgname);
    binary_little_endian_put_uint32(cov_bytes(mh + 8, 4), b->pkgpath);
    binary_little_endian_put_uint32(cov_bytes(mh + 12, 4), b->modpath);
    memcpy(mh + 16, digest, 16);
    binary_little_endian_put_uint32(cov_bytes(mh + 36, 4), burrow__cov_strtab_nentries(&b->stab));
    binary_little_endian_put_uint32(cov_bytes(mh + 40, 4), (uint32_t)b->funcs.len);
    Error err = BURROW_NO_ERROR;
    (void)w.vt->writer.write(w.data, cov_bytes(mh, COV_META_HEADER_SIZE), &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("error writing meta-file header: %v", err);
    int64_t off = COV_META_HEADER_SIZE;

    /* Write function offsets section */
    off = cov_emit_func_offsets(b, w, off);

    /* Check for any errors up to this point. */
    if (BURROW_FAILED(b->werr))
        return b->werr;

    /* Write string table. */
    err = burrow__cov_strtab_write(&b->stab, io_write_seeker_as_io_writer(w));
    if (BURROW_FAILED(err))
        return err;
    off += (int64_t)burrow__cov_strtab_size(&b->stab);

    /* Write functions */
    for (Int i = 0; i < b->funcs.len; i++) {
        off = cov_emit_func(w, off, BURROW_AT(Slice, b->funcs, i), &err);
        if (BURROW_FAILED(err))
            return err;
    }

    /* Back-patch the length. */
    uint32_t total_length = (uint32_t)off;
    (void)w.vt->seeker.seek(w.data, 0, BURROW_IO_SEEK_START, &err);
    if (BURROW_FAILED(err))
        return err;
    cov_builder_wr_uint32(b, w, total_length);
    if (BURROW_FAILED(b->werr))
        return b->werr;
    return BURROW_NO_ERROR;
}

void burrow__cov_hash_func_desc(const CovFuncDesc *f, Byte digest[16]) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h = fnv_new128a(a);
    cov_hash_func_desc(h, f);
    Slice sum = hash_sum(a, h, slice_make(a, TYPE_BYTE, 0, 16));
    memset(digest, 0, 16);
    memcpy(digest, sum.p, (size_t)(sum.len < 16 ? sum.len : 16));
    arena_free(&ar);
}

struct CovMetaFileWriter {
    CovStrtabWriter stab;
    Str mfname;
    BufioWriter *w;
    Alloc *a;
};

CovMetaFileWriter *burrow__cov_new_meta_file_writer(Alloc *a, Str mfname, IoWriter w) {
    CovMetaFileWriter *r = COV_NEW(a, CovMetaFileWriter);
    r->a = a;
    r->mfname = strings_clone(a, mfname);
    r->w = bufio_new_writer(a, w);
    burrow__cov_strtab_init_writer(&r->stab, a);
    burrow__cov_strtab_lookup(&r->stab, BURROW_STR_EMPTY);
    return r;
}

Error burrow__cov_meta_file_writer_write(CovMetaFileWriter *m, const Byte final_hash[16],
                                         Slice blobs, CovCounterMode mode,
                                         CovCounterGranularity granularity) {
    uint64_t mhsz = COV_META_FILE_HEADER_SIZE;
    uint32_t st_size = burrow__cov_strtab_size(&m->stab);
    uint64_t st_offset = mhsz + (uint64_t)(16 * blobs.len);
    uint64_t preamble_length = st_offset + (uint64_t)st_size;

    /* Compute total size */
    uint64_t tlen = preamble_length;
    for (Int i = 0; i < blobs.len; i++)
        tlen += (uint64_t)BURROW_AT(Slice, blobs, i).len;

    /* Emit header */
    Byte mh[COV_META_FILE_HEADER_SIZE] = {0};
    memcpy(mh, burrow__cov_meta_magic, 4);
    binary_little_endian_put_uint32(cov_bytes(mh + 4, 4), COV_META_FILE_VERSION);
    binary_little_endian_put_uint64(cov_bytes(mh + 8, 8), tlen);
    binary_little_endian_put_uint64(cov_bytes(mh + 16, 8), (uint64_t)blobs.len);
    memcpy(mh + 24, final_hash, 16);
    binary_little_endian_put_uint32(cov_bytes(mh + 40, 4), (uint32_t)st_offset);
    binary_little_endian_put_uint32(cov_bytes(mh + 44, 4), st_size);
    mh[48] = mode;
    mh[49] = granularity;
    Error err = BURROW_NO_ERROR;
    (void)bufio_writer_write(m->w, cov_bytes(mh, COV_META_FILE_HEADER_SIZE), &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("error writing %s: %v", m->mfname, err);

    /* Emit package offsets section followed by package lengths section. */
    uint64_t off = preamble_length;
    Byte buf[8];
    for (Int i = 0; i < blobs.len; i++) {
        binary_little_endian_put_uint64(cov_bytes(buf, 8), off);
        (void)bufio_writer_write(m->w, cov_bytes(buf, 8), &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("error writing %s: %v", m->mfname, err);
        off += (uint64_t)BURROW_AT(Slice, blobs, i).len;
    }
    for (Int i = 0; i < blobs.len; i++) {
        uint64_t bl = (uint64_t)BURROW_AT(Slice, blobs, i).len;
        binary_little_endian_put_uint64(cov_bytes(buf, 8), bl);
        (void)bufio_writer_write(m->w, cov_bytes(buf, 8), &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("error writing %s: %v", m->mfname, err);
    }

    /* Emit string table */
    err = burrow__cov_strtab_write(&m->stab, bufio_writer_as_io_writer(m->w));
    if (BURROW_FAILED(err))
        return err;

    /* Now emit blobs themselves. */
    for (Int i = 0; i < blobs.len; i++) {
        (void)bufio_writer_write(m->w, BURROW_AT(Slice, blobs, i), &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("error writing %s: %v", m->mfname, err);
    }

    /* Flush writer, and we're done. */
    err = bufio_writer_flush(m->w);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("error writing %s: %v", m->mfname, err);
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------- decodemeta */

static uint32_t cov_le32(const Byte *p) {
    return binary_little_endian_uint32(cov_bytes(p, 4));
}

static uint64_t cov_le64(const Byte *p) {
    return binary_little_endian_uint64(cov_bytes(p, 8));
}

CovMetaDataDecoder *burrow__cov_new_meta_data_decoder(Alloc *a, Slice b, bool readonly,
                                                      Error *err) {
    CovSliceReader *slr = burrow__cov_new_slice_reader(a, b, readonly);
    CovMetaDataDecoder *x = COV_NEW(a, CovMetaDataDecoder);
    x->r = slr;

    /* readHeader */
    Byte h[COV_META_HEADER_SIZE];
    Error e = cov_slr_read_full(slr, h, COV_META_HEADER_SIZE);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    x->hdr.length = cov_le32(h);
    x->hdr.pkg_name = cov_le32(h + 4);
    x->hdr.pkg_path = cov_le32(h + 8);
    x->hdr.module_path = cov_le32(h + 12);
    memcpy(x->hdr.meta_hash, h + 16, 16);
    x->hdr.num_files = cov_le32(h + 36);
    x->hdr.num_funcs = cov_le32(h + 40);

    /* readStringTable */
    /* Seek to the correct location to read the string table. */
    int64_t string_table_location = (int64_t)(uint32_t)(COV_META_HEADER_SIZE +
                                                        4 * x->hdr.num_funcs);
    (void)burrow__cov_slice_reader_seek(slr, string_table_location, BURROW_IO_SEEK_START,
                                        &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }

    /* Read the table itself. */
    x->strtab = burrow__cov_new_strtab_reader(a, slr);
    burrow__cov_strtab_reader_read(x->strtab);
    return x;
}

Str burrow__cov_meta_data_decoder_package_path(CovMetaDataDecoder *d) {
    return burrow__cov_strtab_reader_get(d->strtab, d->hdr.pkg_path);
}

Str burrow__cov_meta_data_decoder_package_name(CovMetaDataDecoder *d) {
    return burrow__cov_strtab_reader_get(d->strtab, d->hdr.pkg_name);
}

Str burrow__cov_meta_data_decoder_module_path(CovMetaDataDecoder *d) {
    return burrow__cov_strtab_reader_get(d->strtab, d->hdr.module_path);
}

uint32_t burrow__cov_meta_data_decoder_num_funcs(const CovMetaDataDecoder *d) {
    return d->hdr.num_funcs;
}

Error burrow__cov_meta_data_decoder_read_func(CovMetaDataDecoder *d, uint32_t fidx,
                                              CovFuncDesc *f) {
    if (fidx >= d->hdr.num_funcs)
        return fmt_errorf_v("illegal function index");

    /* Seek to the correct location to read the function offset and read it. */
    int64_t func_offset_location = (int64_t)(uint32_t)(COV_META_HEADER_SIZE + 4 * fidx);
    Error err = BURROW_NO_ERROR;
    (void)burrow__cov_slice_reader_seek(d->r, func_offset_location, BURROW_IO_SEEK_START,
                                        &err);
    if (BURROW_FAILED(err))
        return err;
    uint32_t foff = burrow__cov_slice_reader_read_uint32(d->r);

    /* Check assumptions */
    if (foff < (uint32_t)func_offset_location || foff > d->hdr.length)
        return fmt_errorf_v("malformed func offset %d", foff);

    /* Seek to the correct location to read the function. */
    int64_t floc = (int64_t)foff;
    (void)burrow__cov_slice_reader_seek(d->r, floc, BURROW_IO_SEEK_START, &err);
    if (BURROW_FAILED(err))
        return err;

    /* Preamble containing number of units, file, and function. */
    uint32_t num_units = (uint32_t)burrow__cov_slice_reader_read_uleb128(d->r);
    uint32_t fnameidx = (uint32_t)burrow__cov_slice_reader_read_uleb128(d->r);
    uint32_t fileidx = (uint32_t)burrow__cov_slice_reader_read_uleb128(d->r);

    f->srcfile = burrow__cov_strtab_reader_get(d->strtab, fileidx);
    f->funcname = burrow__cov_strtab_reader_get(d->strtab, fnameidx);

    /* Now the units */
    f->units.len = 0;
    if (f->units.elem == NULL || f->units.cap < (Int)num_units)
        f->units = slice_make(d->r->a, &burrow__cov_coverable_unit_type, 0, (Int)num_units);
    for (uint32_t k = 0; k < num_units; k++) {
        CovCoverableUnit u = {0, 0, 0, 0, 0, 0};
        u.st_line = (uint32_t)burrow__cov_slice_reader_read_uleb128(d->r);
        u.st_col = (uint32_t)burrow__cov_slice_reader_read_uleb128(d->r);
        u.en_line = (uint32_t)burrow__cov_slice_reader_read_uleb128(d->r);
        u.en_col = (uint32_t)burrow__cov_slice_reader_read_uleb128(d->r);
        u.nx_stmts = (uint32_t)burrow__cov_slice_reader_read_uleb128(d->r);
        f->units = slice_append(d->r->a, f->units, &u, 1);
    }
    uint64_t lit = burrow__cov_slice_reader_read_uleb128(d->r);
    f->lit = lit != 0;
    return BURROW_NO_ERROR;
}

struct CovMetaFileReader {
    OsFile *f;
    CovMetaFileHeader hdr;
    uint64_t *pkg_offsets;
    uint64_t *pkg_lengths;
    CovStrtabReader *strtab;
    BufioReader *file_rdr;
    Slice file_view;
    Alloc *a;
};

static uint64_t cov_rd_uint64(CovMetaFileReader *r, Error *errp) {
    Byte tmp[8];
    Error err = BURROW_NO_ERROR;
    Int n = bufio_reader_read(r->file_rdr, cov_bytes(tmp, 8), &err);
    if (BURROW_FAILED(err)) {
        *errp = err;
        return 0;
    }
    if (n != 8) {
        *errp = fmt_errorf_v("premature end of file on read");
        return 0;
    }
    return cov_le64(tmp);
}

static Error cov_read_file_header(CovMetaFileReader *r) {
    Error err = BURROW_NO_ERROR;

    r->file_rdr = bufio_new_reader(r->a, os_file_as_io_reader(r->f));

    /* Read file header. */
    Byte h[COV_META_FILE_HEADER_SIZE];
    (void)io_read_full(bufio_reader_as_io_reader(r->file_rdr),
                       cov_bytes(h, COV_META_FILE_HEADER_SIZE), &err);
    if (BURROW_FAILED(err))
        return err;
    memcpy(r->hdr.magic, h, 4);
    r->hdr.version = cov_le32(h + 4);
    r->hdr.total_length = cov_le64(h + 8);
    r->hdr.entries = cov_le64(h + 16);
    memcpy(r->hdr.meta_file_hash, h + 24, 16);
    r->hdr.str_tab_offset = cov_le32(h + 40);
    r->hdr.str_tab_length = cov_le32(h + 44);
    r->hdr.cmode = h[48];
    r->hdr.cgranularity = h[49];

    /* Verify magic string */
    const Byte *m = r->hdr.magic;
    const Byte *g = burrow__cov_meta_magic;
    if (m[0] != g[0] || m[1] != g[1] || m[2] != g[2] || m[3] != g[3])
        return fmt_errorf_v("invalid meta-data file magic string");

    /* Vet the version. If this is a meta-data file from the future, we won't
     * be able to read it. */
    if (r->hdr.version > COV_META_FILE_VERSION) {
        uint32_t version = r->hdr.version;
        uint32_t want = COV_META_FILE_VERSION;
        return fmt_errorf_v("meta-data file with an unknown version %d (expected %d)",
                            version, want);
    }

    /* Read package offsets for good measure */
    r->pkg_offsets = (uint64_t *)cov_alloc(r->a, (size_t)r->hdr.entries * sizeof(uint64_t),
                                           _Alignof(uint64_t));
    for (uint64_t i = 0; i < r->hdr.entries; i++) {
        r->pkg_offsets[i] = cov_rd_uint64(r, &err);
        if (BURROW_FAILED(err))
            return err;
        if (r->pkg_offsets[i] > r->hdr.total_length) {
            uint64_t po = r->pkg_offsets[i];
            uint64_t tl = r->hdr.total_length;
            return fmt_errorf_v("insane pkg offset %d: %d > totlen %d", i, po, tl);
        }
    }
    r->pkg_lengths = (uint64_t *)cov_alloc(r->a, (size_t)r->hdr.entries * sizeof(uint64_t),
                                           _Alignof(uint64_t));
    for (uint64_t i = 0; i < r->hdr.entries; i++) {
        r->pkg_lengths[i] = cov_rd_uint64(r, &err);
        if (BURROW_FAILED(err))
            return err;
        if (r->pkg_lengths[i] > r->hdr.total_length) {
            uint64_t pl = r->pkg_lengths[i];
            uint64_t tl = r->hdr.total_length;
            return fmt_errorf_v("insane pkg length %d: %d > totlen %d", i, pl, tl);
        }
    }

    /* Read string table. */
    Slice b = slice_make(r->a, TYPE_BYTE, (Int)r->hdr.str_tab_length,
                         (Int)r->hdr.str_tab_length);
    Int nr = bufio_reader_read(r->file_rdr, b, &err);
    if (BURROW_FAILED(err))
        return err;
    if (nr != (Int)r->hdr.str_tab_length)
        return fmt_errorf_v("error: short read on string table");
    CovSliceReader *slr = burrow__cov_new_slice_reader(r->a, b, false /* not readonly */);
    r->strtab = burrow__cov_new_strtab_reader(r->a, slr);
    burrow__cov_strtab_reader_read(r->strtab);

    return BURROW_NO_ERROR;
}

CovMetaFileReader *burrow__cov_new_meta_file_reader(Alloc *a, OsFile *f, Slice file_view,
                                                    Error *err) {
    CovMetaFileReader *r = COV_NEW(a, CovMetaFileReader);
    r->f = f;
    r->file_view = file_view;
    r->a = a;

    Error e = cov_read_file_header(r);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    return r;
}

uint64_t burrow__cov_meta_file_reader_num_packages(const CovMetaFileReader *r) {
    return r->hdr.entries;
}

CovCounterMode burrow__cov_meta_file_reader_counter_mode(const CovMetaFileReader *r) {
    return r->hdr.cmode;
}

CovCounterGranularity
burrow__cov_meta_file_reader_counter_granularity(const CovMetaFileReader *r) {
    return r->hdr.cgranularity;
}

void burrow__cov_meta_file_reader_file_hash(const CovMetaFileReader *r, Byte out[16]) {
    memcpy(out, r->hdr.meta_file_hash, 16);
}

CovMetaDataDecoder *burrow__cov_meta_file_reader_get_package_decoder(CovMetaFileReader *r,
                                                                     uint32_t pk_idx,
                                                                     Slice payloadbuf,
                                                                     Slice *payload,
                                                                     Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice pp = burrow__cov_meta_file_reader_get_package_payload(r, pk_idx, payloadbuf, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    CovMetaDataDecoder *mdd =
        burrow__cov_new_meta_data_decoder(r->a, pp, r->file_view.p != NULL, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    if (payload != NULL)
        *payload = pp;
    return mdd;
}

Slice burrow__cov_meta_file_reader_get_package_payload(CovMetaFileReader *r,
                                                       uint32_t pk_idx, Slice payloadbuf,
                                                       Error *err) {
    Slice nil = {NULL, 0, 0, TYPE_BYTE};

    /* Determine correct offset/length. */
    if ((uint64_t)pk_idx >= r->hdr.entries) {
        BURROW_OUT(err, fmt_errorf_v("GetPackagePayload: illegal pkg index %d", pk_idx));
        return nil;
    }
    uint64_t off = r->pkg_offsets[pk_idx];
    uint64_t len = r->pkg_lengths[pk_idx];

    if (r->file_view.p != NULL)
        return slice_sub(r->file_view, (Int)off, (Int)(off + len));

    Slice payload = slice_sub(payloadbuf, 0, 0);
    if (payload.elem == NULL)
        payload.elem = TYPE_BYTE;
    if (payload.cap < (Int)len)
        payload = slice_make(r->a, TYPE_BYTE, 0, (Int)len);
    payload = slice_append(r->a, payload, NULL, (Int)len);
    Error e = BURROW_NO_ERROR;
    (void)os_file_seek(r->f, (int64_t)off, BURROW_IO_SEEK_START, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return nil;
    }
    (void)io_read_full(os_file_as_io_reader(r->f), payload, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return nil;
    }
    return payload;
}

/* ---------------------------------------------------------- encodecounter */

struct CovCoverageDataWriter {
    CovStrtabWriter *stab;
    BufioWriter *w;
    CovCounterSegmentHeader csh;
    Slice tmp;
    CovCounterFlavor cflavor;
    uint32_t segs;
    Alloc *a;
};

CovCoverageDataWriter *burrow__cov_new_coverage_data_writer(Alloc *a, IoWriter w,
                                                            CovCounterFlavor flav) {
    CovCoverageDataWriter *r = COV_NEW(a, CovCoverageDataWriter);
    r->a = a;
    r->stab = COV_NEW(a, CovStrtabWriter);
    r->w = bufio_new_writer(a, w);
    r->tmp = slice_make(a, TYPE_BYTE, 64, 64);
    r->cflavor = flav;
    burrow__cov_strtab_init_writer(r->stab, a);
    burrow__cov_strtab_lookup(r->stab, BURROW_STR_EMPTY);
    return r;
}

static Error cov_write_header(CovCoverageDataWriter *cfw, const Byte meta_file_hash[16]);

Error burrow__cov_coverage_data_writer_write(CovCoverageDataWriter *cfw,
                                             const Byte meta_file_hash[16], Map *args,
                                             CovCounterVisitor visitor) {
    Error err = cov_write_header(cfw, meta_file_hash);
    if (BURROW_FAILED(err))
        return err;
    return burrow__cov_coverage_data_writer_append_segment(cfw, args, visitor);
}

static Error cov_pad_to_four_byte_boundary(CovWriteSeeker *ws) {
    Int sz = burrow__cov_write_seeker_bytes_written(ws).len;
    static const Byte zeros[4] = {0, 0, 0, 0};
    uint32_t rem = (uint32_t)sz % 4;
    if (rem != 0) {
        Slice pad = cov_bytes(zeros, (Int)(4 - rem));
        Error err = BURROW_NO_ERROR;
        Int nw = burrow__cov_write_seeker_write(ws, pad, &err);
        if (BURROW_FAILED(err))
            return err;
        if (nw != pad.len)
            return fmt_errorf_v("error: short write");
    }
    return BURROW_NO_ERROR;
}

/* binary.Write of a CounterSegmentHeader. */
static Error cov_write_csh(CovWriteSeeker *ws, const CovCounterSegmentHeader *csh) {
    Byte b[COV_COUNTER_SEGMENT_HEADER_SIZE];
    binary_little_endian_put_uint64(cov_bytes(b, 8), csh->fcn_entries);
    binary_little_endian_put_uint32(cov_bytes(b + 8, 4), csh->str_tab_len);
    binary_little_endian_put_uint32(cov_bytes(b + 12, 4), csh->args_len);
    Error err = BURROW_NO_ERROR;
    (void)burrow__cov_write_seeker_write(ws, cov_bytes(b, COV_COUNTER_SEGMENT_HEADER_SIZE),
                                         &err);
    return err;
}

static Error cov_patch_segment_header(CovCoverageDataWriter *cfw, CovWriteSeeker *ws) {
    Error err = BURROW_NO_ERROR;
    /* record position */
    int64_t off = burrow__cov_write_seeker_seek(ws, 0, BURROW_IO_SEEK_CURRENT, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("error seeking in patchSegmentHeader: %v", err);
    /* seek back to start so that we can update the segment header */
    (void)burrow__cov_write_seeker_seek(ws, 0, BURROW_IO_SEEK_START, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("error seeking in patchSegmentHeader: %v", err);
    err = cov_write_csh(ws, &cfw->csh);
    if (BURROW_FAILED(err))
        return err;
    /* ... and finally return to the original offset. */
    (void)burrow__cov_write_seeker_seek(ws, off, BURROW_IO_SEEK_START, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("error seeking in patchSegmentHeader: %v", err);
    return BURROW_NO_ERROR;
}

static Error cov_wr_uleb128(CovCoverageDataWriter *cfw, CovWriteSeeker *ws, uint64_t v) {
    cfw->tmp.len = 0;
    cfw->tmp = burrow__cov_append_uleb128(cfw->a, cfw->tmp, v);
    Error err = BURROW_NO_ERROR;
    (void)burrow__cov_write_seeker_write(ws, cfw->tmp, &err);
    return err;
}

static Error cov_write_segment_preamble(CovCoverageDataWriter *cfw, Map *args,
                                        CovWriteSeeker *ws) {
    Error err = cov_write_csh(ws, &cfw->csh);
    if (BURROW_FAILED(err))
        return err;
    uint32_t hdrsz = (uint32_t)burrow__cov_write_seeker_bytes_written(ws).len;

    /* Write string table and args to a byte slice (since we need to capture
     * offsets at various points), then emit the slice once we are done. */
    burrow__cov_strtab_freeze(cfw->stab);
    err = burrow__cov_strtab_write(cfw->stab, (IoWriter){&cov_write_seeker_vt.writer, ws});
    if (BURROW_FAILED(err))
        return err;
    cfw->csh.str_tab_len = (uint32_t)burrow__cov_write_seeker_bytes_written(ws).len - hdrsz;

    Slice akeys = slice_make(cfw->a, TYPE_STRING, 0, map_len(args));
    if (args != NULL) {
        MapIter it = map_iter(args);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v))
            akeys = BURROW_APPEND(Str, cfw->a, akeys, *(const Str *)k);
    }
    slices_sort(akeys);

    /* Count of arg pairs. */
    err = cov_wr_uleb128(cfw, ws, (uint64_t)map_len(args));
    if (BURROW_FAILED(err))
        return err;
    /* Arg pairs themselves. */
    for (Int i = 0; i < akeys.len; i++) {
        Str k = BURROW_AT(Str, akeys, i);
        uint64_t ki = burrow__cov_strtab_lookup(cfw->stab, k);
        err = cov_wr_uleb128(cfw, ws, ki);
        if (BURROW_FAILED(err))
            return err;
        Str v = *(const Str *)map_get(args, &k);
        uint64_t vi = burrow__cov_strtab_lookup(cfw->stab, v);
        err = cov_wr_uleb128(cfw, ws, vi);
        if (BURROW_FAILED(err))
            return err;
    }
    err = cov_pad_to_four_byte_boundary(ws);
    if (BURROW_FAILED(err))
        return err;
    cfw->csh.args_len = (uint32_t)burrow__cov_write_seeker_bytes_written(ws).len -
                        (cfw->csh.str_tab_len + hdrsz);

    return BURROW_NO_ERROR;
}

typedef struct CovEmitter {
    CovCoverageDataWriter *cfw;
    CovWriteSeeker *ws;
} CovEmitter;

static Error cov_wrval(CovCoverageDataWriter *cfw, CovWriteSeeker *ws, uint32_t val) {
    Byte ctrb[4];
    Slice buf = {NULL, 0, 0, TYPE_BYTE};
    Int towr = 0;
    if (cfw->cflavor == COV_CTR_RAW) {
        binary_little_endian_put_uint32(cov_bytes(ctrb, 4), val);
        buf = cov_bytes(ctrb, 4);
        towr = 4;
    } else if (cfw->cflavor == COV_CTR_ULEB128) {
        cfw->tmp.len = 0;
        cfw->tmp = burrow__cov_append_uleb128(cfw->a, cfw->tmp, val);
        buf = cfw->tmp;
        towr = buf.len;
    } else {
        panic_str(COV_S("internal error: bad counter flavor"));
    }
    Error err = BURROW_NO_ERROR;
    Int sz = burrow__cov_write_seeker_write(ws, buf, &err);
    if (BURROW_FAILED(err))
        return err;
    if (sz != towr)
        return fmt_errorf_v("writing counters: short write");
    return BURROW_NO_ERROR;
}

/* Write out entries for each live function. */
static Error cov_emitter(void *env, uint32_t pkid, uint32_t funcid, Slice counters) {
    CovEmitter *e = (CovEmitter *)env;
    e->cfw->csh.fcn_entries++;
    Error err = cov_wrval(e->cfw, e->ws, (uint32_t)counters.len);
    if (BURROW_FAILED(err))
        return err;

    err = cov_wrval(e->cfw, e->ws, pkid);
    if (BURROW_FAILED(err))
        return err;

    err = cov_wrval(e->cfw, e->ws, funcid);
    if (BURROW_FAILED(err))
        return err;
    for (Int i = 0; i < counters.len; i++) {
        err = cov_wrval(e->cfw, e->ws, BURROW_AT(uint32_t, counters, i));
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

static Error cov_write_counters(CovCoverageDataWriter *cfw, CovCounterVisitor visitor,
                                CovWriteSeeker *ws) {
    /* Notes:
     * - this version writes everything little-endian, which means a call is
     *   needed to encode every value (expensive)
     * - we may want to move to a model in which we just blast out all
     *   counters, or possibly mmap the file and do the write implicitly. */
    CovEmitter e = {cfw, ws};
    return visitor.visit_funcs(visitor.self, cov_emitter, &e);
}

static Error cov_write_bytes(CovCoverageDataWriter *cfw, Slice b) {
    if (b.len == 0)
        return BURROW_NO_ERROR;
    Error err = BURROW_NO_ERROR;
    Int nw = bufio_writer_write(cfw->w, b, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("error writing counter data: %v", err);
    if (b.len != nw)
        return fmt_errorf_v("error writing counter data: short write");
    return BURROW_NO_ERROR;
}

static Error cov_write_footer(CovCoverageDataWriter *cfw) {
    cfw->segs++;
    Byte cf[COV_COUNTER_FILE_FOOTER_SIZE] = {0};
    memcpy(cf, burrow__cov_counter_magic, 4);
    binary_little_endian_put_uint32(cov_bytes(cf + 8, 4), cfw->segs);
    Error err = BURROW_NO_ERROR;
    (void)bufio_writer_write(cfw->w, cov_bytes(cf, COV_COUNTER_FILE_FOOTER_SIZE), &err);
    return err;
}

Error burrow__cov_coverage_data_writer_append_segment(CovCoverageDataWriter *cfw,
                                                      Map *args,
                                                      CovCounterVisitor visitor) {
    cfw->stab = COV_NEW(cfw->a, CovStrtabWriter);
    burrow__cov_strtab_init_writer(cfw->stab, cfw->a);
    burrow__cov_strtab_lookup(cfw->stab, BURROW_STR_EMPTY);

    if (args != NULL) {
        MapIter it = map_iter(args);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            burrow__cov_strtab_lookup(cfw->stab, *(const Str *)k);
            burrow__cov_strtab_lookup(cfw->stab, *(const Str *)v);
        }
    }

    CovWriteSeeker *ws = COV_NEW(cfw->a, CovWriteSeeker);
    ws->a = cfw->a;
    Error err = cov_write_segment_preamble(cfw, args, ws);
    if (BURROW_FAILED(err))
        return err;
    err = cov_write_counters(cfw, visitor, ws);
    if (BURROW_FAILED(err))
        return err;
    err = cov_patch_segment_header(cfw, ws);
    if (BURROW_FAILED(err))
        return err;
    err = cov_write_bytes(cfw, burrow__cov_write_seeker_bytes_written(ws));
    if (BURROW_FAILED(err))
        return err;
    err = cov_write_footer(cfw);
    if (BURROW_FAILED(err))
        return err;
    err = bufio_writer_flush(cfw->w);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("write error: %v", err);
    cfw->stab = NULL;
    return BURROW_NO_ERROR;
}

static Error cov_write_header(CovCoverageDataWriter *cfw, const Byte meta_file_hash[16]) {
    /* Emit file header. */
    Byte ch[COV_COUNTER_FILE_HEADER_SIZE] = {0};
    memcpy(ch, burrow__cov_counter_magic, 4);
    binary_little_endian_put_uint32(cov_bytes(ch + 4, 4), COV_COUNTER_FILE_VERSION);
    memcpy(ch + 8, meta_file_hash, 16);
    ch[24] = cfw->cflavor;
    ch[25] = 0; /* BigEndian: false */
    Error err = BURROW_NO_ERROR;
    (void)bufio_writer_write(cfw->w, cov_bytes(ch, COV_COUNTER_FILE_HEADER_SIZE), &err);
    return err;
}

/* ---------------------------------------------------------- decodecounter */

struct CovCounterDataReader {
    CovStrtabReader *stab;
    Map *args;    /* string to string */
    Slice osargs; /* of Str */
    Str goarch;   /* GOARCH setting from run that produced counter data */
    Str goos;     /* GOOS setting from run that produced counter data */
    IoReadSeeker mr;
    CovCounterFileHeader hdr;
    CovCounterFileFooter ftr;
    CovCounterSegmentHeader shdr;
    Byte u32b[4];
    Byte u8b[1];
    uint32_t fcn_count;
    uint32_t seg_count;
    Alloc *a;
};

static Int cov_mr_read(CovCounterDataReader *cdr, Slice p, Error *err) {
    return cdr->mr.vt->reader.read(cdr->mr.data, p, err);
}

static int64_t cov_mr_seek(CovCounterDataReader *cdr, int64_t off, int whence, Error *err) {
    return cdr->mr.vt->seeker.seek(cdr->mr.data, off, whence, err);
}

static Error cov_mr_read_full(CovCounterDataReader *cdr, Byte *b, Int n) {
    Error err = BURROW_NO_ERROR;
    (void)io_read_full(io_read_seeker_as_io_reader(cdr->mr), cov_bytes(b, n), &err);
    return err;
}

static bool cov_check_magic(const Byte v[4]) {
    const Byte *g = burrow__cov_counter_magic;
    return v[0] == g[0] && v[1] == g[1] && v[2] == g[2] && v[3] == g[3];
}

static Error cov_read_footer(CovCounterDataReader *cdr) {
    int64_t ftr_size = COV_COUNTER_FILE_FOOTER_SIZE;
    Error err = BURROW_NO_ERROR;
    (void)cov_mr_seek(cdr, -ftr_size, BURROW_IO_SEEK_END, &err);
    if (BURROW_FAILED(err))
        return err;
    Byte b[COV_COUNTER_FILE_FOOTER_SIZE];
    err = cov_mr_read_full(cdr, b, COV_COUNTER_FILE_FOOTER_SIZE);
    if (BURROW_FAILED(err))
        return err;
    memcpy(cdr->ftr.magic, b, 4);
    cdr->ftr.num_segments = cov_le32(b + 8);
    if (!cov_check_magic(cdr->ftr.magic))
        return fmt_errorf_v("invalid magic string (not a counter data file)");
    if (cdr->ftr.num_segments == 0)
        return fmt_errorf_v("invalid counter data file (no segments)");
    return BURROW_NO_ERROR;
}

static Error cov_read_string_table(CovCounterDataReader *cdr) {
    Slice b = slice_make(cdr->a, TYPE_BYTE, (Int)cdr->shdr.str_tab_len,
                         (Int)cdr->shdr.str_tab_len);
    Error err = BURROW_NO_ERROR;
    Int nr = cov_mr_read(cdr, b, &err);
    if (BURROW_FAILED(err))
        return err;
    if (nr != (Int)cdr->shdr.str_tab_len)
        return fmt_errorf_v("error: short read on string table");
    CovSliceReader *slr = burrow__cov_new_slice_reader(cdr->a, b, false /* not readonly */);
    cdr->stab = burrow__cov_new_strtab_reader(cdr->a, slr);
    burrow__cov_strtab_reader_read(cdr->stab);
    return BURROW_NO_ERROR;
}

static Str cov_sget(CovCounterDataReader *cdr, CovSliceReader *slr, Error *err) {
    uint64_t kidx = burrow__cov_slice_reader_read_uleb128(slr);
    if (kidx >= (uint64_t)burrow__cov_strtab_reader_entries(cdr->stab)) {
        *err = fmt_errorf_v("malformed string table ref");
        return BURROW_STR_EMPTY;
    }
    return burrow__cov_strtab_reader_get(cdr->stab, (uint32_t)kidx);
}

static Error cov_read_args(CovCounterDataReader *cdr) {
    Slice b = slice_make(cdr->a, TYPE_BYTE, (Int)cdr->shdr.args_len,
                         (Int)cdr->shdr.args_len);
    Error err = BURROW_NO_ERROR;
    Int nr = cov_mr_read(cdr, b, &err);
    if (BURROW_FAILED(err))
        return err;
    if (nr != (Int)cdr->shdr.args_len)
        return fmt_errorf_v("error: short read on args table");
    CovSliceReader *slr = burrow__cov_new_slice_reader(cdr->a, b, false /* not readonly */);
    uint64_t nents = burrow__cov_slice_reader_read_uleb128(slr);
    cdr->args = map_make(cdr->a, TYPE_STRING, TYPE_STRING, (Int)nents);
    for (uint64_t i = 0; i < nents; i++) {
        Error errk = BURROW_NO_ERROR;
        Str k = cov_sget(cdr, slr, &errk);
        if (BURROW_FAILED(errk))
            return errk;
        Error errv = BURROW_NO_ERROR;
        Str v = cov_sget(cdr, slr, &errv);
        if (BURROW_FAILED(errv))
            return errv;
        if (map_get(cdr->args, &k) != NULL)
            return fmt_errorf_v("malformed args table");
        map_set(cdr->args, &k, &v);
    }
    Str argc_key = COV_S("argc");
    const Str *argcs = map_get(cdr->args, &argc_key);
    if (argcs != NULL) {
        Int argc = strconv_atoi(*argcs, &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("malformed argc in counter data file args section");
        cdr->osargs = slice_make(cdr->a, TYPE_STRING, 0, argc);
        for (Int i = 0; i < argc; i++) {
            Str key = fmt_sprintf_v(cdr->a, "argv%d", i);
            const Str *arg = map_get(cdr->args, &key);
            Str s = arg != NULL ? *arg : BURROW_STR_EMPTY;
            cdr->osargs = BURROW_APPEND(Str, cdr->a, cdr->osargs, s);
        }
    }
    Str goos_key = COV_S("GOOS");
    const Str *goos = map_get(cdr->args, &goos_key);
    if (goos != NULL)
        cdr->goos = *goos;
    Str goarch_key = COV_S("GOARCH");
    const Str *goarch = map_get(cdr->args, &goarch_key);
    if (goarch != NULL)
        cdr->goarch = *goarch;
    return BURROW_NO_ERROR;
}

/* readSegmentPreamble reads and consumes the segment header, segment string
 * table, and segment args table. */
static Error cov_read_segment_preamble(CovCounterDataReader *cdr) {
    /* Read segment header. */
    Byte h[COV_COUNTER_SEGMENT_HEADER_SIZE];
    Error err = cov_mr_read_full(cdr, h, COV_COUNTER_SEGMENT_HEADER_SIZE);
    if (BURROW_FAILED(err))
        return err;
    cdr->shdr.fcn_entries = cov_le64(h);
    cdr->shdr.str_tab_len = cov_le32(h + 8);
    cdr->shdr.args_len = cov_le32(h + 12);

    /* Read string table and args. */
    err = cov_read_string_table(cdr);
    if (BURROW_FAILED(err))
        return err;
    err = cov_read_args(cdr);
    if (BURROW_FAILED(err))
        return err;
    /* Seek past any padding to bring us up to a 4-byte boundary. */
    int64_t of = cov_mr_seek(cdr, 0, BURROW_IO_SEEK_CURRENT, &err);
    if (BURROW_FAILED(err))
        return err;
    int64_t rem = of % 4;
    if (rem != 0) {
        int64_t pad = 4 - rem;
        (void)cov_mr_seek(cdr, pad, BURROW_IO_SEEK_CURRENT, &err);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

CovCounterDataReader *burrow__cov_new_counter_data_reader(Alloc *a, Str fn,
                                                          IoReadSeeker rs, Error *err) {
    (void)fn;
    CovCounterDataReader *cdr = COV_NEW(a, CovCounterDataReader);
    cdr->mr = rs;
    cdr->a = a;
    cdr->osargs = slice_make(a, TYPE_STRING, 0, 0);
    /* Read header */
    Byte h[COV_COUNTER_FILE_HEADER_SIZE];
    Error e = cov_mr_read_full(cdr, h, COV_COUNTER_FILE_HEADER_SIZE);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    memcpy(cdr->hdr.magic, h, 4);
    cdr->hdr.version = cov_le32(h + 4);
    memcpy(cdr->hdr.meta_hash, h + 8, 16);
    cdr->hdr.cflavor = h[24];
    cdr->hdr.big_endian = h[25] != 0;
    if (!cov_check_magic(cdr->hdr.magic)) {
        BURROW_OUT(err, fmt_errorf_v("invalid magic string: not a counter data file"));
        return NULL;
    }
    if (cdr->hdr.version > COV_COUNTER_FILE_VERSION) {
        uint32_t reader = COV_COUNTER_FILE_VERSION;
        uint32_t data = cdr->hdr.version;
        BURROW_OUT(err, fmt_errorf_v("version data incompatibility: reader is %d data is %d",
                                     reader, data));
        return NULL;
    }

    /* Read footer. */
    e = cov_read_footer(cdr);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    /* Seek back to just past the file header. */
    int64_t hsz = COV_COUNTER_FILE_HEADER_SIZE;
    (void)cov_mr_seek(cdr, hsz, BURROW_IO_SEEK_START, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    /* Read preamble for first segment. */
    e = cov_read_segment_preamble(cdr);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    return cdr;
}

Slice burrow__cov_counter_data_reader_os_args(CovCounterDataReader *cdr) {
    return cdr->osargs;
}

Str burrow__cov_counter_data_reader_goos(CovCounterDataReader *cdr) {
    return cdr->goos;
}

Str burrow__cov_counter_data_reader_goarch(CovCounterDataReader *cdr) {
    return cdr->goarch;
}

uint32_t burrow__cov_counter_data_reader_num_segments(const CovCounterDataReader *cdr) {
    return cdr->ftr.num_segments;
}

bool burrow__cov_counter_data_reader_begin_next_segment(CovCounterDataReader *cdr,
                                                        Error *err) {
    if (cdr->seg_count >= cdr->ftr.num_segments)
        return false;
    cdr->seg_count++;
    cdr->fcn_count = 0;
    /* Seek past footer from last segment. */
    int64_t ftr_size = COV_COUNTER_FILE_FOOTER_SIZE;
    Error e = BURROW_NO_ERROR;
    (void)cov_mr_seek(cdr, ftr_size, BURROW_IO_SEEK_CURRENT, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return false;
    }
    /* Read preamble for this segment. */
    e = cov_read_segment_preamble(cdr);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return false;
    }
    return true;
}

uint32_t burrow__cov_counter_data_reader_num_functions_in_segment(
    const CovCounterDataReader *cdr) {
    return (uint32_t)cdr->shdr.fcn_entries;
}

static uint32_t cov_rdu32(CovCounterDataReader *cdr, Error *err) {
    if (cdr->hdr.cflavor == COV_CTR_ULEB128) {
        unsigned shift = 0;
        uint64_t value = 0;
        for (;;) {
            (void)cov_mr_read(cdr, cov_bytes(cdr->u8b, 1), err);
            if (BURROW_FAILED(*err))
                return 0;
            Byte b = cdr->u8b[0];
            /* Not in Go: the shift is kept in range, as in the slicereader. */
            if (shift < 64)
                value |= ((uint64_t)(b & 0x7F) << shift);
            if ((b & 0x80) == 0)
                break;
            shift += 7;
        }
        return (uint32_t)value;
    }
    if (cdr->hdr.cflavor == COV_CTR_RAW) {
        Int n = cov_mr_read(cdr, cov_bytes(cdr->u32b, 4), err);
        if (BURROW_FAILED(*err))
            return 0;
        if (n != 4) {
            *err = io_eof;
            return 0;
        }
        if (cdr->hdr.big_endian)
            return binary_big_endian_uint32(cov_bytes(cdr->u32b, 4));
        return binary_little_endian_uint32(cov_bytes(cdr->u32b, 4));
    }
    panic_str(COV_S("internal error: unknown counter flavor"));
}

bool burrow__cov_counter_data_reader_next_func(CovCounterDataReader *cdr, CovFuncPayload *p,
                                               Error *err) {
    if (cdr->fcn_count >= (uint32_t)cdr->shdr.fcn_entries)
        return false;
    cdr->fcn_count++;
    /* Go picks its rdu32 here, before reading anything. */
    if (cdr->hdr.cflavor != COV_CTR_ULEB128 && cdr->hdr.cflavor != COV_CTR_RAW)
        panic_str(COV_S("internal error: unknown counter flavor"));

    Error e = BURROW_NO_ERROR;
    uint32_t nc = cov_rdu32(cdr, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return false;
    }

    /* Read package and func indices. */
    p->pkg_idx = cov_rdu32(cdr, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return false;
    }
    p->func_idx = cov_rdu32(cdr, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return false;
    }
    if (p->counters.elem == NULL || p->counters.cap < 1024)
        p->counters = slice_make(cdr->a, TYPE_UINT32, 0, 1024);
    p->counters.len = 0;
    for (uint32_t i = 0; i < nc; i++) {
        uint32_t v = cov_rdu32(cdr, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return false;
        }
        p->counters = BURROW_APPEND(uint32_t, cdr->a, p->counters, v);
    }
    return true;
}
