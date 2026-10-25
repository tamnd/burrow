/* internal/coverage tests.
 *
 * Derived from Go's src/internal/coverage/slicereader/slr_test.go,
 * slicewriter/slw_test.go, test/roundtrip_test.go and test/counter_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "../src/debug/coverage_internal.h"

#include "burrow/core.h"
#include "burrow/encoding/binary.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/path/filepath.h"
#include "burrow/slice.h"
#include "burrow/testing.h"

#include <stdint.h>
#include <string.h>

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Arena ar;
static Alloc *a;

static void remove_tree(void *env) {
    (void)os_remove_all(*(Str *)env);
}

/* t.TempDir: a new directory, removed when the test ends. */
static Str temp_dir(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str *d = BURROW_NEW(a, Str);
    *d = os_mkdir_temp(a, S(""), S("burrow-coverage-test-*"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirTemp: %v", e);
    testing_t_cleanup(t, BURROW_FN(Func, remove_tree, d));
    return *d;
}

static Str join2(Str x, Str y) {
    Str parts[2] = {x, y};
    return filepath_join(a, slice_from(parts, 2, 2, TYPE_STRING));
}

static Slice bytes(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* &slicewriter.WriteSeeker{} */
static CovWriteSeeker *new_write_seeker(void) {
    CovWriteSeeker *ws = BURROW_NEW(a, CovWriteSeeker);
    ws->a = a;
    return ws;
}

/* Not in Go: an OsFile as an io.ReadSeeker, which Go gets by conversion. */
static IoReadSeekerVT os_rs_vt;

static IoReadSeeker os_rs(OsFile *f) {
    os_rs_vt.reader = *os_file_as_io_reader(f).vt;
    os_rs_vt.seeker = *os_file_as_io_seeker(f).vt;
    return (IoReadSeeker){&os_rs_vt, f};
}

/* ------------------------------------------------------------ slicereader */

static void TestSliceReader(TestingT *t) {
    Slice b = slice_make(a, TYPE_BYTE, 0, 0);

    Byte bt[8];
    uint32_t e32 = 1030507U;
    binary_little_endian_put_uint32(bytes(bt, 4), e32);
    b = slice_append(a, b, bt, 4);

    uint64_t e64 = 907050301U;
    binary_little_endian_put_uint64(bytes(bt, 8), e64);
    b = slice_append(a, b, bt, 8);

    b = burrow__cov_append_uleb128(a, b, e32);
    b = burrow__cov_append_uleb128(a, b, e64);
    b = burrow__cov_append_uleb128(a, b, 6);
    Str s1 = S("foobar");
    b = slice_append(a, b, s1.p, s1.len);
    b = burrow__cov_append_uleb128(a, b, 9);
    Str s2 = S("bazbasher");
    b = slice_append(a, b, s2.p, s2.len);

    for (int i = 0; i < 2; i++) {
        CovSliceReader *slr = burrow__cov_new_slice_reader(a, b, i == 0);
        uint32_t g32 = burrow__cov_slice_reader_read_uint32(slr);
        if (g32 != e32)
            testing_t_fatalf_v(t, "slr.ReadUint32() got %d want %d", g32, e32);
        uint64_t g64 = burrow__cov_slice_reader_read_uint64(slr);
        if (g64 != e64)
            testing_t_fatalf_v(t, "slr.ReadUint64() got %d want %d", g64, e64);
        g32 = (uint32_t)burrow__cov_slice_reader_read_uleb128(slr);
        if (g32 != e32)
            testing_t_fatalf_v(t, "slr.ReadULEB128() got %d want %d", g32, e32);
        g64 = burrow__cov_slice_reader_read_uleb128(slr);
        if (g64 != e64)
            testing_t_fatalf_v(t, "slr.ReadULEB128() got %d want %d", g64, e64);
        uint64_t len = burrow__cov_slice_reader_read_uleb128(slr);
        Str gs1 = burrow__cov_slice_reader_read_string(slr, (int64_t)len);
        if (!str_eq(gs1, s1))
            testing_t_fatalf_v(t, "readStr got %s want %s", gs1, s1);
        len = burrow__cov_slice_reader_read_uleb128(slr);
        Str gs2 = burrow__cov_slice_reader_read_string(slr, (int64_t)len);
        if (!str_eq(gs2, s2))
            testing_t_fatalf_v(t, "readStr got %s want %s", gs2, s2);
        Error err = BURROW_NO_ERROR;
        (void)burrow__cov_slice_reader_seek(slr, 4, BURROW_IO_SEEK_START, &err);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, err);
        int64_t off = burrow__cov_slice_reader_offset(slr);
        if (off != 4)
            testing_t_fatalf_v(t, "Offset() returned %d wanted 4", off);
        g64 = burrow__cov_slice_reader_read_uint64(slr);
        if (g64 != e64)
            testing_t_fatalf_v(t, "post-seek slr.ReadUint64() got %d want %d", g64, e64);
    }
}

/* ------------------------------------------------------------ slicewriter */

static void sleq(TestingT *t, Slice got, Slice want) {
    testing_t_helper(t);
    if (got.len != want.len)
        testing_t_fatalf_v(t, "bad length got %d want %d", got.len, want.len);
    for (Int i = 0; i < got.len; i++) {
        Byte g = BURROW_AT(Byte, got, i), w = BURROW_AT(Byte, want, i);
        if (g != w)
            testing_t_fatalf_v(t, "bad read at %d got %d want %d", i, g, w);
    }
}

static void wf(TestingT *t, CovWriteSeeker *ws, Slice p) {
    testing_t_helper(t);
    Error werr = BURROW_NO_ERROR;
    Int nw = burrow__cov_write_seeker_write(ws, p, &werr);
    if (BURROW_FAILED(werr))
        testing_t_fatalf_v(t, "unexpected write error: %v", werr);
    if (nw != p.len)
        testing_t_fatalf_v(t, "wrong amount written want %d got %d", p.len, nw);
}

static void rf(TestingT *t, CovWriteSeeker *ws, Slice p) {
    testing_t_helper(t);
    Slice b = slice_make(a, TYPE_BYTE, p.len, p.len);
    Error rerr = BURROW_NO_ERROR;
    Int nr = burrow__cov_write_seeker_read(ws, b, &rerr);
    if (BURROW_FAILED(rerr))
        testing_t_fatalf_v(t, "unexpected read error: %v", rerr);
    if (nr != p.len)
        testing_t_fatalf_v(t, "wrong amount read want %d got %d", p.len, nr);
    sleq(t, b, p);
}

static int64_t sk(TestingT *t, CovWriteSeeker *ws, int64_t offset, int whence) {
    testing_t_helper(t);
    Error err = BURROW_NO_ERROR;
    int64_t off = burrow__cov_write_seeker_seek(ws, offset, whence, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected seek error: %v", err);
    return off;
}

static void TestSliceWriter(TestingT *t) {
    Byte wp1[] = {1, 2};
    CovWriteSeeker *ws = new_write_seeker();
    Slice empty = bytes(NULL, 0);

    /* write some stuff */
    wf(t, ws, bytes(wp1, 2));
    /* check that BytesWritten returns what we wrote. */
    sleq(t, burrow__cov_write_seeker_bytes_written(ws), bytes(wp1, 2));
    /* offset is at end of slice, so reading should return zero bytes. */
    rf(t, ws, empty);

    /* write some more stuff */
    Byte wp2[] = {7, 8, 9};
    wf(t, ws, bytes(wp2, 3));
    /* check that BytesWritten returns what we expect. */
    Byte wpex[] = {1, 2, 7, 8, 9};
    sleq(t, burrow__cov_write_seeker_bytes_written(ws), bytes(wpex, 5));
    rf(t, ws, empty);

    /* seeks and reads. */
    Byte r27[] = {2, 7};
    sk(t, ws, 1, BURROW_IO_SEEK_START);
    rf(t, ws, bytes(r27, 2));
    sk(t, ws, -2, BURROW_IO_SEEK_CURRENT);
    rf(t, ws, bytes(r27, 2));
    sk(t, ws, -4, BURROW_IO_SEEK_END);
    rf(t, ws, bytes(r27, 2));
    int64_t off = sk(t, ws, 0, BURROW_IO_SEEK_END);
    sk(t, ws, off, BURROW_IO_SEEK_START);

    /* seek back and overwrite */
    sk(t, ws, 1, BURROW_IO_SEEK_START);
    Byte w911[] = {9, 11};
    wf(t, ws, bytes(w911, 2));
    Byte wpex2[] = {1, 9, 11, 8, 9};
    sleq(t, burrow__cov_write_seeker_bytes_written(ws), bytes(wpex2, 5));

    /* seeks on empty writer. */
    CovWriteSeeker *ws2 = new_write_seeker();
    sk(t, ws2, 0, BURROW_IO_SEEK_START);
    sk(t, ws2, 0, BURROW_IO_SEEK_CURRENT);
    sk(t, ws2, 0, BURROW_IO_SEEK_END);

    /* check for seek errors. */
    Error err = BURROW_NO_ERROR;
    (void)burrow__cov_write_seeker_seek(ws, -1, BURROW_IO_SEEK_START, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected error on invalid -1 seek");
    Int bw = burrow__cov_write_seeker_bytes_written(ws).len;
    err = BURROW_NO_ERROR;
    (void)burrow__cov_write_seeker_seek(ws, (int64_t)bw + 1, BURROW_IO_SEEK_START, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected error on invalid %d seek", bw);

    (void)burrow__cov_write_seeker_seek(ws, 0, BURROW_IO_SEEK_START, NULL);
    err = BURROW_NO_ERROR;
    (void)burrow__cov_write_seeker_seek(ws, -1, BURROW_IO_SEEK_CURRENT, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected error on invalid -1 seek");
    err = BURROW_NO_ERROR;
    (void)burrow__cov_write_seeker_seek(ws, (int64_t)bw + 1, BURROW_IO_SEEK_CURRENT, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected error on invalid %d seek", bw);

    err = BURROW_NO_ERROR;
    (void)burrow__cov_write_seeker_seek(ws, 1, BURROW_IO_SEEK_END, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected error on invalid 1 seek");
    int64_t bsamt = (int64_t)(-1 * bw - 1);
    err = BURROW_NO_ERROR;
    (void)burrow__cov_write_seeker_seek(ws, bsamt, BURROW_IO_SEEK_END, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected error on invalid %d seek", bsamt);

    /* bad seek mode */
    err = BURROW_NO_ERROR;
    (void)burrow__cov_write_seeker_seek(ws, -1, BURROW_IO_SEEK_START + 9, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected error on invalid seek mode");
}

/* -------------------------------------------------------------- roundtrip */

/* Not in Go: fmt.Sprintf("%+v", fd) for a coverage.FuncDesc, written out. */
static Str func_desc_string(CovFuncDesc f) {
    Slice b = slice_make(a, TYPE_BYTE, 0, 0);
    b = fmt_appendf_v(a, b, "{Funcname:%s Srcfile:%s Units:[", f.funcname, f.srcfile);
    for (Int i = 0; i < f.units.len; i++) {
        CovCoverableUnit u = BURROW_AT(CovCoverableUnit, f.units, i);
        if (i > 0)
            b = fmt_appendf_v(a, b, " ");
        b = fmt_appendf_v(a, b,
                          "{StLine:%d StCol:%d EnLine:%d EnCol:%d NxStmts:%d Parent:%d}",
                          u.st_line, u.st_col, u.en_line, u.en_col, u.nx_stmts,
                          u.parent);
    }
    bool lit = f.lit;
    b = fmt_appendf_v(a, b, "] Lit:%t}", lit);
    return str_from_bytes(b.p, b.len);
}

static Str cmp_func_desc(CovFuncDesc want, CovFuncDesc got) {
    Str swant = func_desc_string(want);
    Str sgot = func_desc_string(got);
    if (str_eq(swant, sgot))
        return S("");
    return fmt_sprintf_v(a, "wanted %q got %q", swant, sgot);
}

static Slice units_of(const CovCoverableUnit *u, Int n) {
    Slice s = slice_make(a, &burrow__cov_coverable_unit_type, 0, n);
    return slice_append(a, s, u, n);
}

static void TestMetaDataEmptyPackage(TestingT *t) {
    /* Make sure that encoding/decoding works properly with packages that don't
     * actually have any functions. */
    Str p = S("empty/package");
    Str pn = S("package");
    Str mp = S("m");
    Error err = BURROW_NO_ERROR;
    CovMetaDataBuilder *b = burrow__cov_new_meta_data_builder(a, p, pn, mp, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "making builder: %v", err);
    CovWriteSeeker *drws = new_write_seeker();
    Byte digest[16];
    (void)burrow__cov_meta_data_builder_emit(b, burrow__cov_write_seeker_as_io_write_seeker(drws),
                                             digest);
    (void)burrow__cov_write_seeker_seek(drws, 0, BURROW_IO_SEEK_START, NULL);
    CovMetaDataDecoder *dec = burrow__cov_new_meta_data_decoder(
        a, burrow__cov_write_seeker_bytes_written(drws), false, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "making decoder: %v", err);
    uint32_t nf = burrow__cov_meta_data_decoder_num_funcs(dec);
    if (nf != 0)
        testing_t_errorf_v(t, "dec.NumFuncs(): got %d want %d", nf, 0);
    Str pp = burrow__cov_meta_data_decoder_package_path(dec);
    if (!str_eq(pp, p))
        testing_t_errorf_v(t, "dec.PackagePath(): got %s want %s", pp, p);
    Str ppn = burrow__cov_meta_data_decoder_package_name(dec);
    if (!str_eq(ppn, pn))
        testing_t_errorf_v(t, "dec.PackageName(): got %s want %s", ppn, pn);
    Str pmp = burrow__cov_meta_data_decoder_module_path(dec);
    if (!str_eq(pmp, mp))
        testing_t_errorf_v(t, "dec.ModulePath(): got %s want %s", pmp, mp);
}

static void TestMetaDataEncoderDecoder(TestingT *t) {
    /* Test encode path. */
    Str pp = S("foo/bar/pkg");
    Str pn = S("pkg");
    Str mp = S("barmod");
    Error err = BURROW_NO_ERROR;
    CovMetaDataBuilder *b = burrow__cov_new_meta_data_builder(a, pp, pn, mp, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "making builder: %v", err);
    static const CovCoverableUnit u1[] = {
        {1, 2, 3, 4, 5, 0},
        {6, 7, 8, 9, 10, 0},
    };
    CovFuncDesc f1 = {S("func"), S("foo.go"), units_of(u1, NELEM(u1)), false};
    uint64_t idx = burrow__cov_meta_data_builder_add_func(b, f1);
    if (idx != 0)
        testing_t_errorf_v(t, "b.AddFunc(f1) got %d want %d", idx, 0);

    static const CovCoverableUnit u2[] = {
        {1, 2, 3, 4, 5, 0},
        {6, 7, 8, 9, 10, 0},
        {11, 12, 13, 14, 15, 0},
    };
    CovFuncDesc f2 = {S("xfunc"), S("bar.go"), units_of(u2, NELEM(u2)), false};
    idx = burrow__cov_meta_data_builder_add_func(b, f2);
    if (idx != 1)
        testing_t_errorf_v(t, "b.AddFunc(f2) got %d want %d", idx, 0);

    /* Emit into a writer. */
    CovWriteSeeker *drws = new_write_seeker();
    Byte digest[16];
    (void)burrow__cov_meta_data_builder_emit(b, burrow__cov_write_seeker_as_io_write_seeker(drws),
                                             digest);

    /* Test decode path. */
    (void)burrow__cov_write_seeker_seek(drws, 0, BURROW_IO_SEEK_START, NULL);
    CovMetaDataDecoder *dec = burrow__cov_new_meta_data_decoder(
        a, burrow__cov_write_seeker_bytes_written(drws), false, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NewCoverageMetaDataDecoder error: %v", err);
    uint32_t nf = burrow__cov_meta_data_decoder_num_funcs(dec);
    if (nf != 2)
        testing_t_errorf_v(t, "dec.NumFuncs(): got %d want %d", nf, 2);

    Str gotpp = burrow__cov_meta_data_decoder_package_path(dec);
    if (!str_eq(gotpp, pp))
        testing_t_errorf_v(t, "packagepath: got %s want %s", gotpp, pp);
    Str gotpn = burrow__cov_meta_data_decoder_package_name(dec);
    if (!str_eq(gotpn, pn))
        testing_t_errorf_v(t, "packagename: got %s want %s", gotpn, pn);

    CovFuncDesc cases[] = {f1, f2};
    for (uint32_t i = 0; i < (uint32_t)NELEM(cases); i++) {
        CovFuncDesc fn = {S(""), S(""), {NULL, 0, 0, NULL}, false};
        err = burrow__cov_meta_data_decoder_read_func(dec, i, &fn);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "err reading function %d: %v", i, err);
        Str res = cmp_func_desc(cases[i], fn);
        if (!str_is_empty(res))
            testing_t_errorf_v(t, "ReadFunc(%d): %s", i, res);
    }
}

/* createFuncs: a Slice of CovFuncDesc. */
static const Type func_desc_type = {
    {(const Byte *)"FuncDesc", 8},
    {(const Byte *)"internal/coverage", 17},
    KIND_STRUCT,
    (uint32_t)sizeof(CovFuncDesc),
    (uint16_t)_Alignof(CovFuncDesc),
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

static Slice create_funcs(int i) {
    Slice res = slice_make(a, &func_desc_type, 0, 0);
    uint32_t lc = 1;
    for (int fi = 0; fi < i + 1; fi++) {
        Slice units = slice_make(a, &burrow__cov_coverable_unit_type, 0, 0);
        for (int ui = 0; ui < (fi + 1) * (i + 1); ui++) {
            CovCoverableUnit u = {lc, lc + 1, lc + 2, lc + 3, lc + 4, 0};
            units = slice_append(a, units, &u, 1);
            lc += 5;
        }
        CovFuncDesc f = {fmt_sprintf_v(a, "func_%d_%d", i, fi),
                         fmt_sprintf_v(a, "foo_%d.go", i), units, false};
        res = slice_append(a, res, &f, 1);
    }
    return res;
}

static Slice create_blob(TestingT *t, int i) {
    Str nomodule = S("");
    Error err = BURROW_NO_ERROR;
    CovMetaDataBuilder *b =
        burrow__cov_new_meta_data_builder(a, S("foo/pkg"), S("pkg"), nomodule, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "making builder: %v", err);

    Slice funcs = create_funcs(i);
    for (Int k = 0; k < funcs.len; k++)
        (void)burrow__cov_meta_data_builder_add_func(b, BURROW_AT(CovFuncDesc, funcs, k));
    CovWriteSeeker *drws = new_write_seeker();
    Byte digest[16];
    (void)burrow__cov_meta_data_builder_emit(b, burrow__cov_write_seeker_as_io_write_seeker(drws),
                                             digest);
    return burrow__cov_write_seeker_bytes_written(drws);
}

static Slice create_meta_data_blobs(TestingT *t, int nb) {
    Slice res = slice_make(a, &burrow__cov_blob_type, 0, 0);
    for (int i = 0; i < nb; i++) {
        Slice blob = create_blob(t, i);
        res = slice_append(a, res, &blob, 1);
    }
    return res;
}

static void TestMetaDataWriterReader(TestingT *t) {
    Str d = temp_dir(t);

    /* Emit a meta-file... */
    Str mfpath = join2(d, S("covmeta.hash.0"));
    Error err = BURROW_NO_ERROR;
    OsFile *of = os_open_file(a, mfpath, OS_O_WRONLY | OS_O_CREATE | OS_O_TRUNC, 0666, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "opening covmeta: %v", err);
    /* t.Logf("meta-file path is %s", mfpath) */
    Slice blobs = create_meta_data_blobs(t, 7);
    CovCounterGranularity gran = COV_CTR_GRANULARITY_PER_BLOCK;
    CovMetaFileWriter *mfw =
        burrow__cov_new_meta_file_writer(a, mfpath, os_file_as_io_writer(of));
    static const Byte final_hash[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    err = burrow__cov_meta_file_writer_write(mfw, final_hash, blobs, COV_CTR_MODE_ATOMIC,
                                             gran);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "writing meta-file: %v", err);
    err = os_file_close(of);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "closing meta-file: %v", err);
    os_file_free(of);

    /* ... then read it back in, first time without setting fileView, second
     * time setting it. */
    for (int k = 0; k < 2; k++) {
        Slice file_view = {NULL, 0, 0, TYPE_BYTE};

        OsFile *inf = os_open(a, mfpath, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "open() on meta-file: %v", err);

        if (k != 0) {
            /* Use fileview to exercise different paths in reader. */
            OsFileInfo fi = os_stat(a, mfpath, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "stat() on meta-file: %v", err);
            Int size = (Int)fi.vt->size(fi.data);
            file_view = slice_make(a, TYPE_BYTE, size, size);
            (void)os_file_read(inf, file_view, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "read() on meta-file: %v", err);
            (void)os_file_seek(inf, 0, BURROW_IO_SEEK_START, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "seek() on meta-file: %v", err);
        }

        CovMetaFileReader *mfr = burrow__cov_new_meta_file_reader(a, inf, file_view, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "k=%d NewCoverageMetaFileReader failed with: %v", k, err);
        uint64_t np = burrow__cov_meta_file_reader_num_packages(mfr);
        if (np != 7)
            testing_t_fatalf_v(t, "k=%d wanted 7 packages got %d", k, np);
        CovCounterMode md = burrow__cov_meta_file_reader_counter_mode(mfr);
        CovCounterMode wmd = COV_CTR_MODE_ATOMIC;
        if (md != wmd)
            testing_t_fatalf_v(t, "k=%d wanted mode %d got %d", k, wmd, md);
        CovCounterGranularity rgran = burrow__cov_meta_file_reader_counter_granularity(mfr);
        CovCounterGranularity wgran = COV_CTR_GRANULARITY_PER_BLOCK;
        if (rgran != wgran)
            testing_t_fatalf_v(t, "k=%d wanted gran %d got %d", k, wgran, rgran);

        Slice payload = slice_make(a, TYPE_BYTE, 0, 0);
        for (int pi = 0; pi < (int)np; pi++) {
            CovMetaDataDecoder *pd = burrow__cov_meta_file_reader_get_package_decoder(
                mfr, (uint32_t)pi, payload, &payload, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "GetPackageDecoder(%d) failed with: %v", pi, err);
            Slice efuncs = create_funcs(pi);
            uint32_t nf = burrow__cov_meta_data_decoder_num_funcs(pd);
            if (efuncs.len != (Int)nf)
                testing_t_fatalf_v(t, "decoding pk %d wanted %d funcs got %d", pi, efuncs.len,
                                   nf);
            CovFuncDesc f = {S(""), S(""), {NULL, 0, 0, NULL}, false};
            for (int fi = 0; fi < (int)nf; fi++) {
                err = burrow__cov_meta_data_decoder_read_func(pd, (uint32_t)fi, &f);
                if (BURROW_FAILED(err))
                    testing_t_fatalf_v(t, "ReadFunc(%d) pk %d got error %v", fi, pi, err);
                Str res = cmp_func_desc(BURROW_AT(CovFuncDesc, efuncs, fi), f);
                if (!str_is_empty(res))
                    testing_t_errorf_v(t, "ReadFunc(%d) pk %d: %s", fi, pi, res);
            }
        }
        (void)os_file_close(inf);
        os_file_free(inf);
    }
}

static void TestMetaDataDecodeLitFlagIssue57942(TestingT *t) {
    /* Encode a package with a few functions. The funcs alternate between
     * regular functions and function literals. */
    Str pp = S("foo/bar/pkg");
    Str pn = S("pkg");
    Str mp = S("barmod");
    Error err = BURROW_NO_ERROR;
    CovMetaDataBuilder *b = burrow__cov_new_meta_data_builder(a, pp, pn, mp, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "making builder: %v", err);
    enum { NF = 6, NCU = 1 };
    uint32_t ln = 10;
    CovFuncDesc wantfds[NF];
    for (uint32_t fi = 0; fi < NF; fi++) {
        Str fis = fmt_sprintf_v(a, "%d", fi);
        CovCoverableUnit u = {ln + 1, 2, ln + 3, 4, fi + 2, 0};
        CovFuncDesc fd = {fmt_sprintf_v(a, "func%s", fis), fmt_sprintf_v(a, "foo%s.go", fis),
                          units_of(&u, NCU), (fi % 2) == 0};
        wantfds[fi] = fd;
        (void)burrow__cov_meta_data_builder_add_func(b, fd);
    }

    /* Emit into a writer. */
    CovWriteSeeker *drws = new_write_seeker();
    Byte digest[16];
    (void)burrow__cov_meta_data_builder_emit(b, burrow__cov_write_seeker_as_io_write_seeker(drws),
                                             digest);

    /* Decode the result. */
    (void)burrow__cov_write_seeker_seek(drws, 0, BURROW_IO_SEEK_START, NULL);
    CovMetaDataDecoder *dec = burrow__cov_new_meta_data_decoder(
        a, burrow__cov_write_seeker_bytes_written(drws), false, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "making decoder: %v", err);
    uint32_t nf = burrow__cov_meta_data_decoder_num_funcs(dec);
    if (nf != NF) {
        Int want = NF;
        testing_t_fatalf_v(t, "decoder number of functions: got %d want %d", nf, want);
    }
    CovFuncDesc fn = {S(""), S(""), {NULL, 0, 0, NULL}, false};
    for (uint32_t i = 0; i < (uint32_t)NF; i++) {
        err = burrow__cov_meta_data_decoder_read_func(dec, i, &fn);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "err reading function %d: %v", i, err);
        Str res = cmp_func_desc(wantfds[i], fn);
        if (!str_is_empty(res))
            testing_t_errorf_v(t, "ReadFunc(%d): %s", i, res);
    }
}

/* ---------------------------------------------------------------- counter */

typedef struct CtrVis {
    const CovFuncPayload *funcs;
    Int n;
} CtrVis;

static Error ctr_vis_visit_funcs(void *self, CovCounterVisitorFn f, void *env) {
    CtrVis *v = (CtrVis *)self;
    for (Int i = 0; i < v->n; i++) {
        CovFuncPayload fn = v->funcs[i];
        Error err = f(env, fn.pkg_idx, fn.func_idx, fn.counters);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

static CovCounterVisitor ctr_vis(CtrVis *v) {
    return (CovCounterVisitor){ctr_vis_visit_funcs, v};
}

static CovFuncPayload mkfunc(uint32_t p, uint32_t f, Slice c) {
    CovFuncPayload fp = {p, f, c};
    return fp;
}

/* Not in Go: fmt.Sprintf("%+v", fp) for a decodecounter.FuncPayload. */
static Str func_payload_string(CovFuncPayload fp) {
    Slice c = fp.counters;
    if (c.elem == NULL)
        c.elem = TYPE_UINT32;
    return fmt_sprintf_v(a, "{PkgIdx:%d FuncIdx:%d Counters:%v}", fp.pkg_idx, fp.func_idx,
                         c);
}

static bool is_dead(CovFuncPayload fp) {
    for (Int i = 0; i < fp.counters.len; i++)
        if (BURROW_AT(uint32_t, fp.counters, i) != 0)
            return false;
    return true;
}

static Map *str_map(const char *const *kv, Int n) {
    Map *m = map_make(a, TYPE_STRING, TYPE_STRING, n);
    for (Int i = 0; i + 1 < n; i += 2) {
        Str k = str_from_cstr(kv[i]);
        Str v = str_from_cstr(kv[i + 1]);
        map_set(m, &k, &v);
    }
    return m;
}

static void close_file(void *env) {
    (void)os_file_close((OsFile *)env);
    os_file_free((OsFile *)env);
}

static void TestCounterDataWriterReader(TestingT *t) {
    static const CovCounterFlavor flavors[] = {COV_CTR_RAW, COV_CTR_ULEB128};

    static uint32_t c0[] = {13, 14, 15};
    static uint32_t c1[] = {16, 17};
    static uint32_t c2[] = {18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 976543, 7};
    CovFuncPayload funcs[] = {
        mkfunc(0, 0, slice_from(c0, NELEM(c0), NELEM(c0), TYPE_UINT32)),
        mkfunc(0, 1, slice_from(c1, NELEM(c1), NELEM(c1), TYPE_UINT32)),
        mkfunc(1, 0, slice_from(c2, NELEM(c2), NELEM(c2), TYPE_UINT32)),
    };
    CtrVis write_visitor = {funcs, NELEM(funcs)};

    for (Int kf = 0; kf < NELEM(flavors); kf++) {
        CovCounterFlavor flav = flavors[kf];

        testing_t_logf_v(t, "testing flavor %d\n", flav);

        /* Open a counter data file in preparation for emitting data. */
        Str d = temp_dir(t);
        Str cfpath = join2(d, fmt_sprintf_v(a, "covcounters.hash.0.%d", kf));
        Error err = BURROW_NO_ERROR;
        OsFile *of =
            os_open_file(a, cfpath, OS_O_WRONLY | OS_O_CREATE | OS_O_TRUNC, 0666, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "opening covcounters: %v", err);

        /* Perform the encode and write. */
        CovCoverageDataWriter *cdfw =
            burrow__cov_new_coverage_data_writer(a, os_file_as_io_writer(of), flav);
        if (cdfw == NULL)
            testing_t_fatalf_v(t, "NewCoverageDataWriter failed");
        static const Byte final_hash[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0};
        static const char *const kv[] = {"argc",  "3",    "argv0", "arg0",
                                         "argv1", "arg1", "argv2", "arg_________2"};
        Map *args = str_map(kv, NELEM(kv));
        err = burrow__cov_coverage_data_writer_write(cdfw, final_hash, args,
                                                     ctr_vis(&write_visitor));
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "counter file Write failed: %v", err);
        err = os_file_close(of);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "closing covcounters: %v", err);
        os_file_free(of);
        cdfw = NULL;

        /* Decode the same file. */
        OsFile *inf = os_open(a, cfpath, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "reopening covcounters file: %v", err);
        testing_t_cleanup(t, BURROW_FN(Func, close_file, inf));
        CovCounterDataReader *cdr =
            burrow__cov_new_counter_data_reader(a, cfpath, os_rs(inf), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "opening covcounters for read: %v", err);
        Slice decoded_args = burrow__cov_counter_data_reader_os_args(cdr);
        Str a_want = S("[arg0 arg1 arg_________2]");
        Str a_got = fmt_sprintf_v(a, "%+v", decoded_args);
        if (!str_eq(a_want, a_got))
            testing_t_errorf_v(t, "reading decoded args, got %s want %s", a_got, a_want);
        for (Int i = 0; i < NELEM(funcs); i++) {
            if (is_dead(funcs[i]))
                continue;
            CovFuncPayload fp = {0, 0, {NULL, 0, 0, NULL}};
            bool ok = burrow__cov_counter_data_reader_next_func(cdr, &fp, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "reading func %d: %v", i, err);
            else if (!ok)
                testing_t_fatalf_v(t, "reading func %d: bad return", i);
            Str got = func_payload_string(fp);
            Str want = func_payload_string(funcs[i]);
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "cdr.NextFunc iter %d\ngot  %+v\nwant %+v", i, got,
                                   want);
        }
        CovFuncPayload dummy = {0, 0, {NULL, 0, 0, NULL}};
        bool ok = burrow__cov_counter_data_reader_next_func(cdr, &dummy, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "reading func after loop: %v", err);
        else if (ok)
            testing_t_fatalf_v(t, "reading func after loop: expected EOF");
    }
}

static void TestCounterDataAppendSegment(TestingT *t) {
    Str d = temp_dir(t);
    Str cfpath = join2(d, S("covcounters.hash2.0"));
    Error err = BURROW_NO_ERROR;
    OsFile *of = os_open_file(a, cfpath, OS_O_WRONLY | OS_O_CREATE | OS_O_TRUNC, 0666, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "opening covcounters: %v", err);

    enum { numSegments = 2 };

    /* Write a counter with multiple segments. */
    static const char *const kv[] = {"argc", "1", "argv0", "prog.exe"};
    Map *args = str_map(kv, NELEM(kv));
    CovFuncPayload *allfuncs[numSegments];
    Int nallfuncs[numSegments];
    Int nall = 0;
    Slice ctrs = slice_make(a, TYPE_UINT32, 0, 0);
    uint32_t q = 0;
    CovCoverageDataWriter *cdfw = NULL;
    for (int idx = 0; idx < numSegments; idx++) {
        Str k = fmt_sprintf_v(a, "seg%d", idx);
        Str x = S("x");
        map_set(args, &k, &x);
        q += 7;
        ctrs = slice_append(a, ctrs, &q, 1);
        CovFuncPayload *funcs =
            mem_alloc(a, sizeof(CovFuncPayload) * (size_t)(idx + 1), _Alignof(CovFuncPayload));
        if (funcs == NULL)
            testing_t_fatalf_v(t, "out of memory");
        for (int kk = 0; kk < idx + 1; kk++) {
            Slice c = slice_make(a, TYPE_UINT32, ctrs.len, ctrs.len);
            slice_copy(c, ctrs);
            funcs[kk] = mkfunc((uint32_t)idx, (uint32_t)kk, c);
        }
        allfuncs[nall] = funcs;
        nallfuncs[nall] = idx + 1;
        nall++;

        CtrVis write_visitor = {funcs, idx + 1};

        if (idx == 0) {
            /* Perform the encode and write. */
            cdfw = burrow__cov_new_coverage_data_writer(a, os_file_as_io_writer(of),
                                                        COV_CTR_RAW);
            if (cdfw == NULL)
                testing_t_fatalf_v(t, "NewCoverageDataWriter failed");
            static const Byte final_hash[16] = {1, 2};
            err = burrow__cov_coverage_data_writer_write(cdfw, final_hash, args,
                                                         ctr_vis(&write_visitor));
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "counter file Write failed: %v", err);
        } else {
            err = burrow__cov_coverage_data_writer_append_segment(cdfw, args,
                                                                  ctr_vis(&write_visitor));
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "counter file AppendSegment failed: %v", err);
        }
    }
    err = os_file_close(of);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "closing covcounters: %v", err);
    os_file_free(of);

    /* Read the result file. */
    OsFile *inf = os_open(a, cfpath, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "reopening covcounters file: %v", err);
    testing_t_cleanup(t, BURROW_FN(Func, close_file, inf));
    CovCounterDataReader *cdr =
        burrow__cov_new_counter_data_reader(a, cfpath, os_rs(inf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "opening covcounters for read: %v", err);
    uint32_t ns = burrow__cov_counter_data_reader_num_segments(cdr);
    if (ns != numSegments) {
        Int want = numSegments;
        testing_t_fatalf_v(t, "got %d segments want %d", ns, want);
    }
    if (nall != numSegments) {
        Int want = numSegments;
        testing_t_fatalf_v(t, "expected %d got %d", want, nall);
    }

    for (int sidx = 0; sidx < (int)ns; sidx++) {
        int64_t off = os_file_seek(inf, 0, BURROW_IO_SEEK_CURRENT, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Seek failed: %v", err);
        else
            testing_t_logf_v(t, "sidx=%d off=%d\n", sidx, off);

        if (sidx != 0) {
            bool ok = burrow__cov_counter_data_reader_begin_next_segment(cdr, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "BeginNextSegment failed: %v", err);
            else if (!ok)
                testing_t_fatalf_v(t, "BeginNextSegment return %v on iter %d", ok, sidx);
        }
        CovFuncPayload *funcs = allfuncs[sidx];
        for (Int i = 0; i < nallfuncs[sidx]; i++) {
            CovFuncPayload fp = {0, 0, {NULL, 0, 0, NULL}};
            bool ok = burrow__cov_counter_data_reader_next_func(cdr, &fp, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "reading func %d: %v", i, err);
            else if (!ok)
                testing_t_fatalf_v(t, "reading func %d: bad return", i);
            Str got = func_payload_string(fp);
            Str want = func_payload_string(funcs[i]);
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "cdr.NextFunc iter %d\ngot  %+v\nwant %+v", i, got,
                                   want);
        }
    }
}

#define TESTS(X)                                                                       \
    X(TestSliceReader)                                                                 \
    X(TestSliceWriter)                                                                 \
    X(TestMetaDataEmptyPackage)                                                        \
    X(TestMetaDataEncoderDecoder)                                                      \
    X(TestMetaDataWriterReader)                                                        \
    X(TestMetaDataDecodeLitFlagIssue57942)                                             \
    X(TestCounterDataWriterReader)                                                     \
    X(TestCounterDataAppendSegment)

static int coverage_internal_main(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(coverage_internal_main, TESTS)
