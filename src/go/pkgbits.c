/* internal/pkgbits: reading and writing the Unified IR bitstream.
 *
 * Derived from Go's src/internal/pkgbits/decoder.go, encoder.go, version.go,
 * syncmarker_string.go and support.go. Go source: go1.27.3.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "pkgbits.h"

#include "burrow/core.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/binary.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PB_S(lit) BURROW_S(lit)

/* ---------------------------------------------------------------- support */

static void pb_assert(bool b) {
    if (!b)
        panic_str(PB_S("assertion failed"));
}

/* panicf: panics with an error. */
static BURROW_NORETURN void pb_panic_err(Error e) {
    Error *box = mem_alloc(error_allocator(), sizeof *box, _Alignof(Error));
    if (box == NULL)
        panic_str(error_text(e));
    *box = e;
    panic(BURROW_ANY(TYPE_ERROR, box));
}

/* Not in Go: running out of memory panics, as it does there. */
static void *pb_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size == 0 ? 1 : size, align);
    if (p == NULL)
        panic_str(PB_S("pkgbits: out of memory"));
    return p;
}

/* --------------------------------------------------------------- versions */

static const PbVersion pb_introduced[PB_NUM_FIELDS] = {
    [PB_FIELD_FLAGS] = PB_V1,
    [PB_FIELD_ALIAS_TYPE_PARAM_NAMES] = PB_V2,
    [PB_FIELD_COMPACT_COMP_LITERALS] = PB_V3,
    [PB_FIELD_GENERIC_METHODS] = PB_V4,
    [PB_FIELD_PRESERVE_METHOD_ORDER] = PB_V5,
};

static const PbVersion pb_removed[PB_NUM_FIELDS] = {
    [PB_FIELD_HAS_INIT] = PB_V2,
    [PB_FIELD_DERIVED_FUNC_INSTANCE] = PB_V2,
    [PB_FIELD_DERIVED_INFO_NEEDED] = PB_V2,
};

bool burrow__pb_version_has(PbVersion v, PbField f) {
    return pb_introduced[f] <= v && (v < pb_removed[f] || pb_removed[f] == PB_V0);
}

/* ----------------------------------------------------------- sync markers */

static const char pb_sync_marker_name[] =
    "EOFBoolInt64Uint64StringValueValRelocsRelocUseRelocPublicPosPosBaseObjectObject1"
    "PkgPkgDefMethodTypeTypeIdxTypeParamNamesSignatureParamsParamCodeObjSymLocalIdent"
    "SelectorPrivateFuncExtVarExtTypeExtPragmaExprListExprsExprExprTypeAssignOpFuncLit"
    "CompLitDeclFuncBodyOpenScopeCloseScopeCloseAnotherScopeDeclNamesDeclNameStmts"
    "BlockStmtIfStmtForStmtSwitchStmtRangeStmtCaseClauseCommClauseSelectStmtDecls"
    "LabeledStmtUseObjLocalAddLocalLinknameStmt1StmtsEndLabelOptLabelMultiExprRType"
    "ConvRTTI";

static const uint16_t pb_sync_marker_index[] = {
    0,   3,   7,   12,  18,  24,  29,  32,  38,  43,  51,  57,  60,  67,
    73,  80,  83,  89,  95,  99,  106, 120, 129, 135, 140, 147, 150, 160,
    168, 175, 182, 188, 195, 201, 209, 214, 218, 226, 232, 234, 241, 248,
    252, 260, 269, 279, 296, 305, 313, 318, 327, 333, 340, 350, 359, 369,
    379, 389, 394, 405, 416, 424, 432, 437, 445, 450, 458, 467, 472, 480};

Str burrow__pb_sync_marker_string(Alloc *a, PbSyncMarker i) {
    Int idx = (Int)i - 1;
    Int n = (Int)(sizeof pb_sync_marker_index / sizeof pb_sync_marker_index[0]);
    if (i < 1 || idx >= n - 1)
        return fmt_sprintf_v(a, "SyncMarker(%d)", (int64_t)i);
    return str_from_bytes(pb_sync_marker_name + pb_sync_marker_index[idx],
                          pb_sync_marker_index[idx + 1] - pb_sync_marker_index[idx]);
}

/* ---------------------------------------------------------------- decoding */

/* binary.Read of a little-endian uint32 from input at *pos. */
static uint32_t pb_read_uint32(Str input, Int *pos) {
    pb_assert(input.len - *pos >= 4);
    uint32_t x = binary_little_endian_uint32(
        (Slice){(void *)(uintptr_t)(input.p + *pos), 4, 4, TYPE_BYTE});
    *pos += 4;
    return x;
}

PbPkgDecoder burrow__pb_new_pkg_decoder(Alloc *a, Str pkg_path, Str input) {
    PbPkgDecoder pr = {0};
    pr.pkg_path = pkg_path;
    pr.a = a;

    Int pos = 0;
    pr.version = pb_read_uint32(input, &pos);

    if (pr.version >= PB_NUM_VERSIONS)
        pb_panic_err(fmt_errorf_v("cannot decode %q, export data version %d is greater "
                                  "than maximum supported version %d",
                                  pkg_path, (int64_t)pr.version,
                                  (int64_t)(PB_NUM_VERSIONS - 1)));

    if (burrow__pb_version_has(pr.version, PB_FIELD_FLAGS)) {
        uint32_t flags = pb_read_uint32(input, &pos);
        pr.sync = (flags & 1) != 0; /* flagSyncMarkers */
    }

    for (Int i = 0; i < PB_NUM_RELOCS; i++)
        pr.elem_ends_ends[i] = pb_read_uint32(input, &pos);

    /* Not in Go: the ends are copied out, since the input need not be
     * aligned for uint32. */
    Int n = (Int)pr.elem_ends_ends[PB_NUM_RELOCS - 1];
    pb_assert((input.len - pos) / 4 >= n);
    uint32_t *ends = pb_alloc(a, (size_t)n * sizeof *ends, _Alignof(uint32_t));
    for (Int i = 0; i < n; i++)
        ends[i] = pb_read_uint32(input, &pos);
    pr.elem_ends = ends;
    pr.nelem_ends = n;

    pr.elem_data = str_from_bytes(input.p + pos, input.len - pos);

    const Int fingerprint_size = 8;
    if (n == 0)
        panic_str(PB_S("runtime error: index out of range [-1]"));
    pb_assert(pr.elem_data.len - fingerprint_size == (Int)pr.elem_ends[n - 1]);

    return pr;
}

Str burrow__pb_pkg_decoder_pkg_path(const PbPkgDecoder *pr) {
    return pr->pkg_path;
}

bool burrow__pb_pkg_decoder_sync_markers(const PbPkgDecoder *pr) {
    return pr->sync;
}

Int burrow__pb_num_elems(const PbPkgDecoder *pr, PbSectionKind k) {
    Int count = (Int)pr->elem_ends_ends[k];
    if (k > 0)
        count -= (Int)pr->elem_ends_ends[k - 1];
    return count;
}

Int burrow__pb_total_elems(const PbPkgDecoder *pr) {
    return pr->nelem_ends;
}

void burrow__pb_fingerprint(const PbPkgDecoder *pr, Byte fp[8]) {
    memcpy(fp, pr->elem_data.p + pr->elem_data.len - 8, 8);
}

Int burrow__pb_abs_idx(const PbPkgDecoder *pr, PbSectionKind k, PbIndex idx) {
    Int abs_idx = (Int)idx;
    if (k > 0)
        abs_idx += (Int)pr->elem_ends_ends[k - 1];
    if (abs_idx >= (Int)pr->elem_ends_ends[k] || abs_idx < 0) {
        /* %v of the array, as Go prints it. */
        Alloc *a = error_allocator();
        Str ends = PB_S("[");
        for (Int i = 0; i < PB_NUM_RELOCS; i++)
            ends = fmt_sprintf_v(a, i == 0 ? "%s%d" : "%s %d", ends,
                                 (int64_t)pr->elem_ends_ends[i]);
        pb_panic_err(fmt_errorf_v("%d:%d is out of bounds; %s]", (int64_t)k,
                                  (int64_t)idx, ends));
    }
    return abs_idx;
}

Str burrow__pb_data_idx(const PbPkgDecoder *pr, PbSectionKind k, PbIndex idx) {
    Int abs_idx = burrow__pb_abs_idx(pr, k, idx);

    uint32_t start = 0;
    if (abs_idx > 0)
        start = pr->elem_ends[abs_idx - 1];
    uint32_t end = pr->elem_ends[abs_idx];

    if (start > end || (Int)end > pr->elem_data.len)
        panic_str(PB_S("runtime error: slice bounds out of range"));
    return str_from_bytes(pr->elem_data.p + start, (Int)(end - start));
}

Str burrow__pb_string_idx(const PbPkgDecoder *pr, PbIndex idx) {
    return burrow__pb_data_idx(pr, PB_SECTION_STRING, idx);
}

PbDecoder burrow__pb_new_decoder(PbPkgDecoder *pr, PbSectionKind k, PbIndex idx,
                                 PbSyncMarker marker) {
    PbDecoder r = burrow__pb_new_decoder_raw(pr, k, idx);
    burrow__pb_sync(&r, marker);
    return r;
}

PbDecoder burrow__pb_temp_decoder(PbPkgDecoder *pr, PbSectionKind k, PbIndex idx,
                                  PbSyncMarker marker) {
    PbDecoder r = burrow__pb_temp_decoder_raw(pr, k, idx);
    burrow__pb_sync(&r, marker);
    return r;
}

void burrow__pb_retire_decoder(PbPkgDecoder *pr, PbDecoder *d) {
    pr->scratch = d->relocs;
    pr->scratch_cap = d->relocs_cap;
    d->relocs = NULL;
    d->nrelocs = 0;
    d->relocs_cap = 0;
}

/* The reference table at the start of every element, into relocs, which has
 * room for it. */
static void pb_read_relocs(PbDecoder *r) {
    for (Int i = 0; i < r->nrelocs; i++) {
        burrow__pb_sync(r, PB_SYNC_RELOC);
        PbSectionKind kind = (PbSectionKind)burrow__pb_len(r);
        PbIndex idx = (PbIndex)burrow__pb_len(r);
        r->relocs[i] = (PbRefTableEntry){kind, idx};
    }
}

PbDecoder burrow__pb_new_decoder_raw(PbPkgDecoder *pr, PbSectionKind k, PbIndex idx) {
    PbDecoder r = {0};
    r.common = pr;
    r.k = k;
    r.idx = idx;

    r.data = burrow__pb_data_idx(pr, k, idx);
    burrow__pb_sync(&r, PB_SYNC_RELOCS);
    Int l = burrow__pb_len(&r);
    pb_assert(l <= r.data.len); /* Not in Go: a bound before allocating */
    r.relocs = pb_alloc(pr->a, (size_t)l * sizeof *r.relocs, _Alignof(PbRefTableEntry));
    r.nrelocs = l;
    r.relocs_cap = l;
    pb_read_relocs(&r);

    return r;
}

PbDecoder burrow__pb_temp_decoder_raw(PbPkgDecoder *pr, PbSectionKind k, PbIndex idx) {
    PbDecoder r = {0};
    r.common = pr;
    r.k = k;
    r.idx = idx;

    r.data = burrow__pb_data_idx(pr, k, idx);
    burrow__pb_sync(&r, PB_SYNC_RELOCS);
    Int l = burrow__pb_len(&r);
    if (pr->scratch != NULL && pr->scratch_cap >= l) {
        r.relocs = pr->scratch;
        r.relocs_cap = pr->scratch_cap;
        pr->scratch = NULL;
        pr->scratch_cap = 0;
    } else {
        pb_assert(l <= r.data.len); /* Not in Go: a bound before allocating */
        r.relocs =
            pb_alloc(pr->a, (size_t)l * sizeof *r.relocs, _Alignof(PbRefTableEntry));
        r.relocs_cap = l;
    }
    r.nrelocs = l;
    pb_read_relocs(&r);

    return r;
}

static BURROW_NORETURN void pb_decoding_error(Str err) {
    pb_panic_err(fmt_errorf_v("unexpected decoding error: %s", err));
}

/* readUvarint, with Go's errors. */
static uint64_t pb_raw_uvarint(PbDecoder *r) {
    uint64_t x = 0;
    unsigned s = 0;
    for (int i = 0; i < 10; i++) { /* binary.MaxVarintLen64 */
        if (r->off >= r->data.len)
            pb_decoding_error(i > 0 ? PB_S("unexpected EOF") : PB_S("EOF"));
        Byte b = r->data.p[r->off++];
        if (b < 0x80) {
            if (i == 9 && b > 1)
                pb_decoding_error(
                    PB_S("pkgbits: readUvarint overflows a 64-bit integer"));
            return x | (uint64_t)b << s;
        }
        x |= (uint64_t)(b & 0x7f) << s;
        s += 7;
    }
    pb_decoding_error(PB_S("pkgbits: readUvarint overflows a 64-bit integer"));
}

static int64_t pb_raw_varint(PbDecoder *r) {
    uint64_t ux = pb_raw_uvarint(r);

    /* Zig-zag decode. */
    int64_t x = (int64_t)(ux >> 1);
    if (ux & 1)
        x = ~x;
    return x;
}

static PbIndex pb_raw_reloc(PbDecoder *r, PbSectionKind k, Int idx) {
    if (idx < 0 || idx >= r->nrelocs)
        panic_str(PB_S("runtime error: index out of range"));
    PbRefTableEntry e = r->relocs[idx];
    pb_assert(e.kind == k);
    return e.idx;
}

void burrow__pb_sync(PbDecoder *r, PbSyncMarker want) {
    if (!r->common->sync)
        return;

    Int pos = r->off;
    PbSyncMarker have = (PbSyncMarker)pb_raw_uvarint(r);
    uint64_t nwriter = pb_raw_uvarint(r);
    pb_assert(nwriter <= (uint64_t)r->data.len); /* Not in Go: a bound */
    Int *writer_pcs =
        pb_alloc(r->common->a, (size_t)nwriter * sizeof(Int), _Alignof(Int));
    for (uint64_t i = 0; i < nwriter; i++)
        writer_pcs[i] = (Int)pb_raw_uvarint(r);

    if (have == want)
        return;

    Alloc *a = r->common->a;
    fmt_printf_v("export data desync: package %q, section %d, index %d, offset %d\n",
                 r->common->pkg_path, (int64_t)r->k, (int64_t)r->idx, pos);

    fmt_printf_v("\nfound %s, written at:\n", burrow__pb_sync_marker_string(a, have));
    if (nwriter == 0)
        fmt_printf_v("\t[stack trace unavailable; recompile package %q with "
                     "-d=syncframes]\n",
                     r->common->pkg_path);
    for (uint64_t i = 0; i < nwriter; i++)
        fmt_printf_v("\t%s\n",
                     burrow__pb_string_idx(
                         r->common, pb_raw_reloc(r, PB_SECTION_STRING, writer_pcs[i])));

    /* Not in Go: there is no stack to print for the reader. */
    fmt_printf_v("\nexpected %s, reading at:\n",
                 burrow__pb_sync_marker_string(a, want));

    /* We already printed a stack trace for the reader, so now we can simply
     * exit. Printing a second one with panic or base.Fatalf would just be
     * noise. */
    os_exit(1);
}

bool burrow__pb_bool(PbDecoder *r) {
    burrow__pb_sync(r, PB_SYNC_BOOL);
    if (r->off >= r->data.len)
        pb_decoding_error(PB_S("EOF"));
    Byte x = r->data.p[r->off++];
    pb_assert(x < 2);
    return x != 0;
}

int64_t burrow__pb_int64(PbDecoder *r) {
    burrow__pb_sync(r, PB_SYNC_INT64);
    return pb_raw_varint(r);
}

uint64_t burrow__pb_uint64(PbDecoder *r) {
    burrow__pb_sync(r, PB_SYNC_UINT64);
    return pb_raw_uvarint(r);
}

Int burrow__pb_len(PbDecoder *r) {
    uint64_t x = burrow__pb_uint64(r);
    Int v = (Int)x;
    pb_assert(v >= 0 && (uint64_t)v == x);
    return v;
}

Int burrow__pb_int(PbDecoder *r) {
    int64_t x = burrow__pb_int64(r);
    Int v = (Int)x;
    pb_assert((int64_t)v == x);
    return v;
}

Uint burrow__pb_uint(PbDecoder *r) {
    uint64_t x = burrow__pb_uint64(r);
    Uint v = (Uint)x;
    pb_assert((uint64_t)v == x);
    return v;
}

Int burrow__pb_code(PbDecoder *r, PbSyncMarker mark) {
    burrow__pb_sync(r, mark);
    return burrow__pb_len(r);
}

PbIndex burrow__pb_reloc(PbDecoder *r, PbSectionKind k) {
    burrow__pb_sync(r, PB_SYNC_USE_RELOC);
    return pb_raw_reloc(r, k, burrow__pb_len(r));
}

Str burrow__pb_string(PbDecoder *r) {
    burrow__pb_sync(r, PB_SYNC_STRING);
    return burrow__pb_string_idx(r->common, burrow__pb_reloc(r, PB_SECTION_STRING));
}

Slice burrow__pb_strings(PbDecoder *r) {
    Int n = burrow__pb_len(r);
    pb_assert(n <= r->data.len); /* Not in Go: a bound before allocating */
    Slice res = slice_make(r->common->a, TYPE_STRING, n, n);
    if (n > 0 && res.p == NULL)
        panic_str(PB_S("pkgbits: out of memory"));
    for (Int i = 0; i < n; i++)
        BURROW_AT(Str, res, i) = burrow__pb_string(r);
    return res;
}

static BigInt *pb_big_int(PbDecoder *r) {
    Alloc *a = r->common->a;
    BigInt *v = BURROW_NEW(a, BigInt);
    if (v == NULL)
        panic_str(PB_S("pkgbits: out of memory"));
    *v = BIG_INT(a);
    Str s = burrow__pb_string(r);
    big_int_set_bytes(v, (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE});
    if (burrow__pb_bool(r))
        big_int_neg(v, v);
    return v;
}

static BigFloat *pb_big_float(PbDecoder *r) {
    Alloc *a = r->common->a;
    BigFloat *v = BURROW_NEW(a, BigFloat);
    if (v == NULL)
        panic_str(PB_S("pkgbits: out of memory"));
    *v = BIG_FLOAT(a);
    big_float_set_prec(v, 512);
    Str s = burrow__pb_string(r);
    pb_assert(BURROW_OK(big_float_unmarshal_text(
        v, (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE})));
    return v;
}

static ConstantValue pb_scalar(PbDecoder *r) {
    Alloc *a = r->common->a;
    Int tag = burrow__pb_code(r, PB_SYNC_VAL);
    switch (tag) {
    case PB_VAL_BOOL:
        return constant_make_bool(burrow__pb_bool(r));
    case PB_VAL_STRING:
        return constant_make_string(a, burrow__pb_string(r));
    case PB_VAL_INT64:
        return constant_make_int64(burrow__pb_int64(r));
    case PB_VAL_BIG_INT:
        return constant_make(a, BURROW_ANY(TYPE_BIG_INT, pb_big_int(r)));
    case PB_VAL_BIG_RAT: {
        BigInt *num = pb_big_int(r);
        BigInt *denom = pb_big_int(r);
        BigRat q = BIG_RAT(a);
        big_rat_set_frac(&q, num, denom);
        return constant_make(a, BURROW_ANY(TYPE_BIG_RAT, &q));
    }
    case PB_VAL_BIG_FLOAT:
        return constant_make(a, BURROW_ANY(TYPE_BIG_FLOAT, pb_big_float(r)));
    default:
        pb_panic_err(fmt_errorf_v("unexpected scalar tag: %d", tag));
    }
}

ConstantValue burrow__pb_value(PbDecoder *r) {
    burrow__pb_sync(r, PB_SYNC_VALUE);
    bool is_complex = burrow__pb_bool(r);
    ConstantValue val = pb_scalar(r);
    if (is_complex) {
        Alloc *a = r->common->a;
        val =
            constant_binary_op(a, val, TOKEN_ADD, constant_make_imag(a, pb_scalar(r)));
    }
    return val;
}

Str burrow__pb_peek_pkg_path(PbPkgDecoder *pr, PbIndex idx) {
    Str path;
    {
        PbDecoder r = burrow__pb_temp_decoder(pr, PB_SECTION_PKG, idx, PB_SYNC_PKG_DEF);
        path = burrow__pb_string(&r);
        burrow__pb_retire_decoder(pr, &r);
    }
    if (path.len == 0)
        path = pr->pkg_path;
    return path;
}

Str burrow__pb_peek_obj(PbPkgDecoder *pr, PbIndex idx, Str *name, Int *tag) {
    PbIndex ridx;
    Int rcode;
    {
        PbDecoder r =
            burrow__pb_temp_decoder(pr, PB_SECTION_NAME, idx, PB_SYNC_OBJECT1);
        burrow__pb_sync(&r, PB_SYNC_SYM);
        burrow__pb_sync(&r, PB_SYNC_PKG);
        ridx = burrow__pb_reloc(&r, PB_SECTION_PKG);
        *name = burrow__pb_string(&r);
        rcode = burrow__pb_code(&r, PB_SYNC_CODE_OBJ);
        burrow__pb_retire_decoder(pr, &r);
    }

    Str path = burrow__pb_peek_pkg_path(pr, ridx);
    pb_assert(name->len != 0);

    *tag = rcode;
    return path;
}

PbVersion burrow__pb_decoder_version(const PbDecoder *r) {
    return r->common->version;
}

/* ---------------------------------------------------------------- encoding */

PbPkgEncoder burrow__pb_new_pkg_encoder(Alloc *a, PbVersion version, Int sync_frames) {
    PbPkgEncoder pw = {0};
    pw.version = version;
    pw.strings_idx = map_make(a, TYPE_STRING, TYPE_INT32, 0);
    if (pw.strings_idx == NULL)
        panic_str(PB_S("pkgbits: out of memory"));
    pw.sync_frames = sync_frames;
    pw.a = a;
    for (Int i = 0; i < PB_NUM_RELOCS; i++)
        pw.elems[i] = slice_nil(TYPE_STRING);
    return pw;
}

bool burrow__pb_pkg_encoder_sync_markers(const PbPkgEncoder *pw) {
    return pw->sync_frames >= 0;
}

static Slice pb_append(Alloc *a, Slice s, const void *p, Int n) {
    if (n == 0)
        return s;
    Slice t = slice_append(a, s, p, n);
    if (t.p == NULL)
        panic_str(PB_S("pkgbits: out of memory"));
    return t;
}

static Slice pb_append_uint32(Alloc *a, Slice s, uint32_t x) {
    Byte b[4];
    binary_little_endian_put_uint32((Slice){b, 4, 4, TYPE_BYTE}, x);
    return pb_append(a, s, b, 4);
}

void burrow__pb_dump_to(PbPkgEncoder *pw, IoWriter out, Byte fp[8]) {
    /* Not in Go: the data is put together first and then written, which comes
     * to the same bytes as writing through a MultiWriter with the hash. */
    Alloc *a = pw->a;
    Slice buf = slice_nil(TYPE_BYTE);

    buf = pb_append_uint32(a, buf, pw->version);

    if (burrow__pb_version_has(pw->version, PB_FIELD_FLAGS)) {
        uint32_t flags = 0;
        if (burrow__pb_pkg_encoder_sync_markers(pw))
            flags |= 1; /* flagSyncMarkers */
        buf = pb_append_uint32(a, buf, flags);
    }

    uint32_t sum = 0;
    for (Int k = 0; k < PB_NUM_RELOCS; k++) {
        sum += (uint32_t)pw->elems[k].len;
        buf = pb_append_uint32(a, buf, sum);
    }

    sum = 0;
    for (Int k = 0; k < PB_NUM_RELOCS; k++) {
        for (Int i = 0; i < pw->elems[k].len; i++) {
            sum += (uint32_t)BURROW_AT(Str, pw->elems[k], i).len;
            buf = pb_append_uint32(a, buf, sum);
        }
    }

    for (Int k = 0; k < PB_NUM_RELOCS; k++) {
        for (Int i = 0; i < pw->elems[k].len; i++) {
            Str elem = BURROW_AT(Str, pw->elems[k], i);
            buf = pb_append(a, buf, elem.p, elem.len);
        }
    }

    Sha256Sum256Ret h = sha256_sum256(buf);
    memcpy(fp, h.a, 8);
    buf = pb_append(a, buf, fp, 8);

    Error err = BURROW_NO_ERROR;
    if (buf.len > 0)
        (void)out.vt->write(out.data, buf, &err);
    pb_assert(BURROW_OK(err));
}

PbIndex burrow__pb_encoder_string_idx(PbPkgEncoder *pw, Str s) {
    int32_t *found = map_get(pw->strings_idx, &s);
    if (found != NULL) {
        pb_assert(str_eq(BURROW_AT(Str, pw->elems[PB_SECTION_STRING], *found), s));
        return (PbIndex)*found;
    }

    int32_t idx = (int32_t)pw->elems[PB_SECTION_STRING].len;
    /* Not in Go: s is copied, since the encoder keeps it. */
    Str c = str_clone(pw->a, s);
    if (s.len > 0 && c.p == NULL)
        panic_str(PB_S("pkgbits: out of memory"));
    pw->elems[PB_SECTION_STRING] =
        pb_append(pw->a, pw->elems[PB_SECTION_STRING], &c, 1);
    if (!map_set(pw->strings_idx, &c, &idx))
        panic_str(PB_S("pkgbits: out of memory"));
    return (PbIndex)idx;
}

PbEncoder *burrow__pb_new_encoder(PbPkgEncoder *pw, PbSectionKind k,
                                  PbSyncMarker marker) {
    PbEncoder *e = burrow__pb_new_encoder_raw(pw, k);
    burrow__pb_encoder_sync(e, marker);
    return e;
}

PbEncoder *burrow__pb_new_encoder_raw(PbPkgEncoder *pw, PbSectionKind k) {
    PbIndex idx = (PbIndex)pw->elems[k].len;
    Str placeholder = BURROW_STR_EMPTY;
    pw->elems[k] = pb_append(pw->a, pw->elems[k], &placeholder, 1);

    PbEncoder *e = pb_alloc(pw->a, sizeof *e, _Alignof(PbEncoder));
    *e = (PbEncoder){0};
    e->p = pw;
    e->k = k;
    e->idx = idx;
    e->data = slice_nil(TYPE_BYTE);
    return e;
}

PbIndex burrow__pb_flush(PbEncoder *w) {
    Alloc *a = w->p->a;

    /* Back up the data so we write the relocations at the front. */
    Slice tmp = w->data;
    w->data = slice_nil(TYPE_BYTE);

    if (w->encoding_reloc_header)
        panic_str(PB_S("encodingRelocHeader already true; recursive flush?"));
    w->encoding_reloc_header = true;
    burrow__pb_encoder_sync(w, PB_SYNC_RELOCS);
    burrow__pb_encoder_len(w, w->nrelocs);
    for (Int i = 0; i < w->nrelocs; i++) {
        PbRefTableEntry r_ent = w->relocs[i];
        burrow__pb_encoder_sync(w, PB_SYNC_RELOC);
        burrow__pb_encoder_len(w, (Int)r_ent.kind);
        burrow__pb_encoder_len(w, (Int)r_ent.idx);
    }

    Slice sb = pb_append(a, slice_nil(TYPE_BYTE), w->data.p, w->data.len);
    sb = pb_append(a, sb, tmp.p, tmp.len);
    w->data = slice_nil(TYPE_BYTE);
    BURROW_AT(Str, w->p->elems[w->k], w->idx) = str_from_bytes(sb.p, sb.len);

    return w->idx;
}

static void pb_raw_uvarint_enc(PbEncoder *w, uint64_t x) {
    Byte buf[10]; /* binary.MaxVarintLen64 */
    Int n = binary_put_uvarint((Slice){buf, 10, 10, TYPE_BYTE}, x);
    w->data = pb_append(w->p->a, w->data, buf, n);
}

static void pb_raw_varint_enc(PbEncoder *w, int64_t x) {
    /* Zig-zag encode. */
    uint64_t ux = (uint64_t)x << 1;
    if (x < 0)
        ux = ~ux;

    pb_raw_uvarint_enc(w, ux);
}

static Int pb_raw_reloc_enc(PbEncoder *w, PbSectionKind k, PbIndex idx) {
    uint64_t key = (uint64_t)(uint32_t)k << 32 | (uint32_t)idx;
    if (w->reloc_map != NULL) {
        uint32_t *i = map_get(w->reloc_map, &key);
        if (i != NULL)
            return (Int)*i;
    } else {
        w->reloc_map = map_make(w->p->a, TYPE_UINT64, TYPE_UINT32, 0);
        if (w->reloc_map == NULL)
            panic_str(PB_S("pkgbits: out of memory"));
    }

    Int i = w->nrelocs;
    uint32_t u = (uint32_t)i;
    if (!map_set(w->reloc_map, &key, &u))
        panic_str(PB_S("pkgbits: out of memory"));
    if (w->nrelocs == w->relocs_cap) {
        Int cap = w->relocs_cap == 0 ? 4 : w->relocs_cap * 2;
        PbRefTableEntry *grown =
            pb_alloc(w->p->a, (size_t)cap * sizeof *grown, _Alignof(PbRefTableEntry));
        if (w->nrelocs > 0)
            memcpy(grown, w->relocs, (size_t)w->nrelocs * sizeof *grown);
        w->relocs = grown;
        w->relocs_cap = cap;
    }
    w->relocs[w->nrelocs++] = (PbRefTableEntry){k, idx};
    return i;
}

void burrow__pb_encoder_sync(PbEncoder *w, PbSyncMarker m) {
    if (!burrow__pb_pkg_encoder_sync_markers(w->p))
        return;

    /* Not in Go: no frames are written, since C has no runtime.Callers. */
    pb_raw_uvarint_enc(w, (uint64_t)m);
    pb_raw_uvarint_enc(w, 0);
}

bool burrow__pb_encoder_bool(PbEncoder *w, bool b) {
    burrow__pb_encoder_sync(w, PB_SYNC_BOOL);
    Byte x = b ? 1 : 0;
    w->data = pb_append(w->p->a, w->data, &x, 1);
    return b;
}

void burrow__pb_encoder_int64(PbEncoder *w, int64_t x) {
    burrow__pb_encoder_sync(w, PB_SYNC_INT64);
    pb_raw_varint_enc(w, x);
}

void burrow__pb_encoder_uint64(PbEncoder *w, uint64_t x) {
    burrow__pb_encoder_sync(w, PB_SYNC_UINT64);
    pb_raw_uvarint_enc(w, x);
}

void burrow__pb_encoder_len(PbEncoder *w, Int x) {
    pb_assert(x >= 0);
    burrow__pb_encoder_uint64(w, (uint64_t)x);
}

void burrow__pb_encoder_int(PbEncoder *w, Int x) {
    burrow__pb_encoder_int64(w, (int64_t)x);
}

void burrow__pb_encoder_uint(PbEncoder *w, Uint x) {
    burrow__pb_encoder_uint64(w, (uint64_t)x);
}

void burrow__pb_encoder_reloc(PbEncoder *w, PbSectionKind k, PbIndex idx) {
    burrow__pb_encoder_sync(w, PB_SYNC_USE_RELOC);
    burrow__pb_encoder_len(w, pb_raw_reloc_enc(w, k, idx));
}

void burrow__pb_encoder_code(PbEncoder *w, PbSyncMarker mark, Int c) {
    burrow__pb_encoder_sync(w, mark);
    burrow__pb_encoder_len(w, c);
}

void burrow__pb_encoder_string(PbEncoder *w, Str s) {
    burrow__pb_encoder_string_ref(w, burrow__pb_encoder_string_idx(w->p, s));
}

void burrow__pb_encoder_string_ref(PbEncoder *w, PbIndex idx) {
    burrow__pb_encoder_sync(w, PB_SYNC_STRING);
    burrow__pb_encoder_reloc(w, PB_SECTION_STRING, idx);
}

void burrow__pb_encoder_strings(PbEncoder *w, Slice ss) {
    burrow__pb_encoder_len(w, ss.len);
    for (Int i = 0; i < ss.len; i++)
        burrow__pb_encoder_string(w, BURROW_AT(Str, ss, i));
}

static void pb_big_int_enc(PbEncoder *w, const BigInt *v) {
    Slice b = big_int_bytes(v, w->p->a);
    burrow__pb_encoder_string(w, str_from_bytes(b.p, b.len)); /* TODO: More efficient
                                                                 encoding. */
    burrow__pb_encoder_bool(w, big_int_sign(v) < 0);
}

static void pb_big_float_enc(PbEncoder *w, const BigFloat *v) {
    Slice b = big_float_append(v, w->p->a, slice_nil(TYPE_BYTE), 'p', -1);
    burrow__pb_encoder_string(w, str_from_bytes(b.p, b.len)); /* TODO: More efficient
                                                                 encoding. */
}

static void pb_scalar_enc(PbEncoder *w, ConstantValue val) {
    Alloc *a = w->p->a;
    Any v = constant_val(a, val);
    if (v.t == TYPE_BOOL) {
        burrow__pb_encoder_code(w, PB_SYNC_VAL, PB_VAL_BOOL);
        burrow__pb_encoder_bool(w, *(bool *)v.data);
    } else if (v.t == TYPE_STRING) {
        burrow__pb_encoder_code(w, PB_SYNC_VAL, PB_VAL_STRING);
        burrow__pb_encoder_string(w, *(Str *)v.data);
    } else if (v.t == TYPE_INT64) {
        burrow__pb_encoder_code(w, PB_SYNC_VAL, PB_VAL_INT64);
        burrow__pb_encoder_int64(w, *(int64_t *)v.data);
    } else if (v.t == TYPE_BIG_INT) {
        burrow__pb_encoder_code(w, PB_SYNC_VAL, PB_VAL_BIG_INT);
        pb_big_int_enc(w, v.data);
    } else if (v.t == TYPE_BIG_RAT) {
        burrow__pb_encoder_code(w, PB_SYNC_VAL, PB_VAL_BIG_RAT);
        pb_big_int_enc(w, big_rat_num(v.data));
        pb_big_int_enc(w, big_rat_denom(v.data));
    } else if (v.t == TYPE_BIG_FLOAT) {
        burrow__pb_encoder_code(w, PB_SYNC_VAL, PB_VAL_BIG_FLOAT);
        pb_big_float_enc(w, v.data);
    } else {
        pb_panic_err(fmt_errorf_v("unhandled %s (%s)", constant_value_string(val, a),
                                  constant_kind_string(constant_value_kind(val), a)));
    }
}

void burrow__pb_encoder_value(PbEncoder *w, ConstantValue val) {
    burrow__pb_encoder_sync(w, PB_SYNC_VALUE);
    if (burrow__pb_encoder_bool(w, constant_value_kind(val) == CONSTANT_COMPLEX)) {
        pb_scalar_enc(w, constant_real(val));
        pb_scalar_enc(w, constant_imag(val));
    } else {
        pb_scalar_enc(w, val);
    }
}

PbVersion burrow__pb_encoder_version(const PbEncoder *w) {
    return w->p->version;
}
