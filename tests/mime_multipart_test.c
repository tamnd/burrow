/* Derived from Go's src/mime/multipart/multipart_test.go, formdata_test.go and
 * writer_test.go. Go source: go1.27.1.
 *
 * tests/mime_multipart_test_gen.h, from tools/gen-mime-multipart-tests.sh, holds
 * bodies with a transcript of what Go's reader, ReadForm and Writer made of
 * them. The replays here write the same transcript from burrow and compare. The
 * tests that need a reader with a trick in it, or bodies of many megabytes, are
 * ported by hand below.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mime.h"
#include "burrow/mime/multipart.h"
#include "burrow/pal.h"
#include "burrow/sort.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Woverlength-strings"

typedef struct QStr {
    const char *p;
    long long n;
} QStr;

#define QS(x) {x, (long long)sizeof(x) - 1}

typedef struct MpReadCase {
    QStr body;
    QStr boundary;
    bool expand; /* [longline] in body stands for Go's longLine */
    QStr want[3];
    QStr want_slow; /* NULL when it is want[0] */
    /* What Windows file names change: NULL when it is the answer above, and
     * for the one byte reader, when it is the Windows answer for NextPart. */
    QStr want_win[4];
} MpReadCase;

typedef struct MpFormCase {
    QStr body;
    QStr boundary;
    long long max_memory;
    const char *godebug;
    QStr want;
    QStr want_win; /* NULL when Windows file names change nothing */
} MpFormCase;

typedef struct MpOp {
    char op;
    QStr a;
    QStr b;
} MpOp;

typedef struct MpWriterCase {
    int n;
    MpOp ops[32];
    QStr want;
} MpWriterCase;

#include "mime_multipart_test_gen.h"

static Str qstr(QStr q) {
    return (Str){(const Byte *)q.p, (Int)q.n};
}

static Str errs(Alloc *a, Error err);

static Str mk(const void *p, Int n) {
    return (Str){(const Byte *)p, n};
}

#define OUT(b, a, ...)                                                                 \
    strings_builder_write_string((b), fmt_sprintf_v((a), __VA_ARGS__), NULL)

static Slice sl(const void *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static uint64_t fnv(Str s) {
    uint64_t h = 14695981039346656037ULL;
    for (Int i = 0; i < s.len; i++) {
        h ^= s.p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static Str body_str(Alloc *a, Str s) {
    if (s.len <= 200)
        return fmt_sprintf_v(a, "%q", s);
    return fmt_sprintf_v(a, "len %d fnv %016x", s.len, fnv(s));
}

/* The temporary directory of the form tests, while one runs. */
static char mp_dir[512];

/* Error text with the temporary file's name turned into TMPFILE, as the
 * generator does. */
static Str errs(Alloc *a, Error err) {
    if (BURROW_OK(err))
        return BURROW_S("nil");
    Str s = error_text(err);
    Str dir = str_from_cstr(mp_dir);
    Int i = dir.len > 0 ? strings_index(s, dir) : -1;
    if (i < 0)
        return s;
    Int j = i + dir.len;
    Str rest = {s.p + j, s.len - j};
    Str pre = BURROW_S("/multipart-");
#if defined(BURROW_OS_WINDOWS)
    pre = BURROW_S("\\multipart-");
#endif
    if (!strings_has_prefix(rest, pre))
        return s;
    j += pre.len;
    while (j < s.len && s.p[j] >= '0' && s.p[j] <= '9')
        j++;
    return fmt_sprintf_v(a, "%sTMPFILE%s", mk(s.p, i), mk(s.p + j, s.len - j));
}

static void dump_header(StringsBuilder *b, Alloc *a, TextprotoMIMEHeader h,
                        const char *indent) {
    Int n = map_len(h);
    Slice keys = slice_make(a, TYPE_STRING, n, n);
    Int i = 0;
    const void *k;
    void *v;
    for (MapIter it = map_iter(h); map_next(&it, &k, &v);)
        ((Str *)keys.p)[i++] = *(const Str *)k;
    sort_strings(keys);
    for (i = 0; i < n; i++) {
        Str key = ((Str *)keys.p)[i];
        Slice vs = *(const Slice *)map_get(h, &key);
        for (Int j = 0; j < vs.len; j++)
            OUT(b, a, "%sh %q: %q\n", str_from_cstr(indent), key, ((Str *)vs.p)[j]);
    }
}

typedef Int (*ReadFn)(void *self, Slice p, Error *err);

static Str read_all(Alloc *a, ReadFn fn, void *self, Error *err) {
    BytesBuffer out = BYTES_BUFFER(a);
    Byte buf[512];
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Int n = fn(self, sl(buf, 512), &e);
        bytes_buffer_write(&out, sl(buf, n), NULL);
        if (BURROW_FAILED(e)) {
            *err = e;
            Slice s = bytes_buffer_bytes(&out);
            return mk((const Byte *)s.p, s.len);
        }
    }
}

static Int part_read(void *self, Slice p, Error *err) {
    return multipart_part_read(self, p, err);
}

static Int file_read(void *self, Slice p, Error *err) {
    return multipart_file_read(self, p, err);
}

/* One byte at a time, like Go's slowReader. */
static Int one_byte_read(void *self, Slice p, Error *err) {
    BytesReader *r = self;
    if (p.len > 1)
        p.len = 1;
    return bytes_reader_read(r, p, err);
}

static const IoReaderVT one_byte_vt = {NULL, one_byte_read};

static Str dump_reader(Alloc *a, IoReader src, Str boundary, int mode) {
    MultipartReader *mr = multipart_new_reader(a, src, boundary);
    StringsBuilder b = STRINGS_BUILDER(a);
    for (int i = 0; i < 64; i++) {
        Error err = BURROW_NO_ERROR;
        MultipartPart *p = mode == 1 ? multipart_reader_next_raw_part(mr, &err)
                                     : multipart_reader_next_part(mr, &err);
        if (BURROW_FAILED(err)) {
            OUT(&b, a, "end %s\n", errs(a, err));
            multipart_reader_free(mr);
            return strings_builder_string(&b);
        }
        strings_builder_write_string(&b, BURROW_S("part\n"), NULL);
        dump_header(&b, a, multipart_part_header(p), "");
        OUT(&b, a, "name %q file %q\n", multipart_part_form_name(p),
            multipart_part_file_name(p));
        if (mode == 2) {
            Byte buf[7];
            Int n = multipart_part_read(p, sl(buf, 7), &err);
            OUT(&b, a, "read %s %s\n", body_str(a, mk(buf, n)), errs(a, err));
            continue;
        }
        Str data = read_all(a, part_read, p, &err);
        OUT(&b, a, "body %s\nerr %s\n", body_str(a, data), errs(a, err));
    }
    multipart_reader_free(mr);
    strings_builder_write_string(&b, BURROW_S("more\n"), NULL);
    return strings_builder_string(&b);
}

/* Go's longLine. */
static Str long_line(Alloc *a) {
    Int n = (1 << 20) / 8;
    Byte *p = mem_alloc_nozero(a, (size_t)n * 8, 1);
    for (Int i = 0; i < n; i++)
        memcpy(p + i * 8, "\n\n\r\r\r\n\r\0", 8);
    return mk(p, n * 8);
}

static Str case_body(Alloc *a, const MpReadCase *c) {
    Str body = qstr(c->body);
    if (!c->expand)
        return body;
    return strings_replace(a, body, BURROW_S("[longline]"), long_line(a), 1);
}

/* Prints the first difference of two transcripts. */
static void diff(TestingT *t, const char *what, int i, Str got, Str want) {
    Int k = 0;
    while (k < got.len && k < want.len && got.p[k] == want.p[k])
        k++;
    Int from = k > 80 ? k - 80 : 0;
    Str g = {got.p + from, got.len - from}, w = {want.p + from, want.len - from};
    if (g.len > 240)
        g.len = 240;
    if (w.len > 240)
        w.len = 240;
    testing_t_errorf_v(t, "%s case %d differs at byte %d:\n got: %q\nwant: %q",
                       str_from_cstr(what), i, k, g, w);
}

/* FileName drops a volume name and splits on backslashes on Windows, as Go's
 * filepath.Base does, so some cases have an answer of their own there. */
#if defined(BURROW_OS_WINDOWS)
static const bool on_windows = true;
#else
static const bool on_windows = false;
#endif

static Str pick(QStr want, QStr win) {
    return on_windows && win.p != NULL ? qstr(win) : qstr(want);
}

static void TestReaderScript(TestingT *t) {
    Arena arena;
    arena_init(&arena, heap_allocator(), 0);
    Alloc *a = arena_allocator(&arena);
    int n = (int)(sizeof mp_read_cases / sizeof mp_read_cases[0]);
    for (int i = 0; i < n; i++) {
        const MpReadCase *c = &mp_read_cases[i];
        Str body = case_body(a, c);
        for (int m = 0; m < 3; m++) {
            BytesReader br;
            bytes_reader_reset(&br, sl(body.p, body.len));
            Str got =
                dump_reader(a, bytes_reader_as_io_reader(&br), qstr(c->boundary), m);
            Str want = pick(c->want[m], c->want_win[m]);
            if (!str_eq(got, want)) {
                char what[32];
                snprintf(what, sizeof what, "mode %d", m);
                diff(t, what, i, got, want);
            }
        }
        BytesReader br;
        bytes_reader_reset(&br, sl(body.p, body.len));
        Str got = dump_reader(a, (IoReader){&one_byte_vt, &br}, qstr(c->boundary), 0);
        Str want = c->want_slow.p != NULL ? qstr(c->want_slow)
                                          : pick(c->want[0], c->want_win[0]);
        if (on_windows && c->want_win[3].p != NULL)
            want = qstr(c->want_win[3]);
        if (!str_eq(got, want))
            diff(t, "slow", i, got, want);
        arena_reset(&arena);
    }
    arena_free(&arena);
}

static const char *temp_root(void) {
    static char buf[400];
    PalErrno e = PAL_OK;
    if (pal_temp_dir(buf, (int64_t)sizeof buf, &e) < 0)
        return "/tmp";
    return buf;
}

static void make_dir(TestingT *t) {
    uint32_t r = 0;
    pal_random_bytes(&r, sizeof r, NULL);
#if defined(BURROW_OS_WINDOWS)
    snprintf(mp_dir, sizeof mp_dir, "%s\\burrow-mp-%08x", temp_root(), (unsigned)r);
#else
    snprintf(mp_dir, sizeof mp_dir, "%s/burrow-mp-%08x", temp_root(), (unsigned)r);
#endif
    PalErrno e = PAL_OK;
    if (!pal_mkdir(mp_dir, 0700, &e))
        testing_t_fatalf_v(t, "mkdir %s: %s", mp_dir, pal_errno_string(e));
}

static int count_dir(void) {
    PalErrno e = PAL_OK;
    int64_t fd = pal_open(mp_dir, PAL_O_RDONLY | PAL_O_DIRECTORY, 0, &e);
    if (fd < 0)
        return -1;
    static PalDir d;
    memset(&d, 0, sizeof d);
    d.fd = fd;
    PalDirEntry ent;
    int n = 0;
    while (pal_readdir(&d, &ent, &e))
        n++;
    pal_close(fd, NULL);
    return n;
}

static void remove_dir(void) {
    pal_rmdir(mp_dir, NULL);
    mp_dir[0] = 0;
}

static Str dump_form(Alloc *a, Str body, Str boundary, int64_t max_memory,
                     const char *godebug) {
    burrow__multipart_godebug_set(godebug);
    BytesReader br;
    bytes_reader_reset(&br, sl(body.p, body.len));
    MultipartReader *mr =
        multipart_new_reader(a, bytes_reader_as_io_reader(&br), boundary);
    burrow__multipart_reader_set_temp_dir(mr, mp_dir);
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = BURROW_NO_ERROR;
    MultipartForm *f = multipart_reader_read_form(mr, max_memory, &err);
    burrow__multipart_godebug_set(NULL);
    if (BURROW_FAILED(err)) {
        OUT(&b, a, "err %s\nleft %d\n", errs(a, err), count_dir());
        multipart_reader_free(mr);
        return strings_builder_string(&b);
    }
    for (int which = 0; which < 2; which++) {
        Map *m = which == 0 ? f->value : f->file;
        Int n = map_len(m);
        Slice keys = slice_make(a, TYPE_STRING, n, n);
        Int i = 0;
        const void *k;
        void *v;
        for (MapIter it = map_iter(m); map_next(&it, &k, &v);)
            ((Str *)keys.p)[i++] = *(const Str *)k;
        sort_strings(keys);
        for (i = 0; i < n; i++) {
            Str key = ((Str *)keys.p)[i];
            Slice vs = *(const Slice *)map_get(m, &key);
            if (which == 0) {
                OUT(&b, a, "value %q\n", key);
                for (Int j = 0; j < vs.len; j++)
                    OUT(&b, a, "  %s\n", body_str(a, ((Str *)vs.p)[j]));
                continue;
            }
            OUT(&b, a, "file %q\n", key);
            for (Int j = 0; j < vs.len; j++) {
                MultipartFileHeader *fh = ((MultipartFileHeader **)vs.p)[j];
                OUT(&b, a, "  filename %q size %d disk %t shared %t off %d\n",
                    fh->filename, fh->size, (bool)(fh->tmpfile != NULL), fh->tmpshared,
                    fh->tmpoff);
                dump_header(&b, a, fh->header, "  ");
                MultipartFile *file = multipart_file_header_open(fh, a, &err);
                if (file == NULL) {
                    OUT(&b, a, "  open %s\n", errs(a, err));
                    continue;
                }
                Str data = read_all(a, file_read, file, &err);
                OUT(&b, a, "  content %s %s\n", body_str(a, data), errs(a, err));
                Byte buf[5];
                Int got = multipart_file_read_at(file, sl(buf, 5), 1, &err);
                OUT(&b, a, "  readat %q %s\n", mk(buf, got), errs(a, err));
                int64_t pos = multipart_file_seek(file, -3, BURROW_IO_SEEK_END, &err);
                OUT(&b, a, "  seek %d %s\n", pos, errs(a, err));
                got = multipart_file_read(file, sl(buf, 5), &err);
                OUT(&b, a, "  read %q %s\n", mk(buf, got), errs(a, err));
                multipart_file_close(file);
            }
        }
    }
    OUT(&b, a, "files %d\n", count_dir());
    OUT(&b, a, "removeall %s\n", errs(a, multipart_form_remove_all(f)));
    OUT(&b, a, "left %d\n", count_dir());
    multipart_form_free(f);
    multipart_reader_free(mr);
    return strings_builder_string(&b);
}

static void TestFormScript(TestingT *t) {
    make_dir(t);
    Arena arena;
    arena_init(&arena, heap_allocator(), 0);
    Alloc *a = arena_allocator(&arena);
    int n = (int)(sizeof mp_form_cases / sizeof mp_form_cases[0]);
    for (int i = 0; i < n; i++) {
        const MpFormCase *c = &mp_form_cases[i];
        Str got = dump_form(a, qstr(c->body), qstr(c->boundary), (int64_t)c->max_memory,
                            c->godebug);
        Str want = pick(c->want, c->want_win);
        if (!str_eq(got, want))
            diff(t, "form", i, got, want);
        arena_reset(&arena);
    }
    arena_free(&arena);
    remove_dir();
}

static Str run_writer(Alloc *a, const MpWriterCase *c) {
    BytesBuffer out = BYTES_BUFFER(a);
    MultipartWriter *w = multipart_new_writer(a, bytes_buffer_as_io_writer(&out));
    IoWriter parts[32];
    int nparts = 0;
    StringsBuilder b = STRINGS_BUILDER(a);
    for (int i = 0; i < c->n; i++) {
        const MpOp *o = &c->ops[i];
        Error err = BURROW_NO_ERROR;
        switch (o->op) {
        case 'B':
            err = multipart_writer_set_boundary(w, qstr(o->a));
            break;
        case 'W':
            err = multipart_writer_write_field(w, qstr(o->a), qstr(o->b));
            break;
        case 'F':
        case 'C':
        case 'H': {
            IoWriter p;
            if (o->op == 'F') {
                p = multipart_writer_create_form_file(w, qstr(o->a), qstr(o->b), &err);
            } else if (o->op == 'C') {
                p = multipart_writer_create_form_field(w, qstr(o->a), &err);
            } else {
                TextprotoMIMEHeader h = textproto_mime_header_make(a);
                Str rest = qstr(o->a);
                while (rest.len > 0) {
                    Int nl = strings_index_byte(rest, '\n');
                    Str line = nl < 0 ? rest : mk(rest.p, nl);
                    rest =
                        nl < 0 ? mk(NULL, 0) : mk(rest.p + nl + 1, rest.len - nl - 1);
                    Int sep = strings_index(line, BURROW_S(": "));
                    if (sep >= 0)
                        textproto_mime_header_add(
                            h, mk(line.p, sep),
                            mk(line.p + sep + 2, line.len - sep - 2));
                }
                p = multipart_writer_create_part(w, h, &err);
            }
            if (BURROW_OK(err))
                parts[nparts++] = p;
            break;
        }
        case 'D':
        case 'O': {
            if (nparts == 0)
                continue;
            IoWriter p = o->op == 'O' ? parts[0] : parts[nparts - 1];
            Str s = qstr(o->a);
            Int n = p.vt->write(p.data, sl(s.p, s.len), &err);
            OUT(&b, a, "n %d ", n);
            break;
        }
        case 'X':
            err = multipart_writer_close(w);
            break;
        case 'T':
            OUT(&b, a, "ct %q\n", multipart_writer_form_data_content_type(w, a));
            continue;
        default:
            break;
        }
        OUT(&b, a, "%c %s\n", (int32_t)o->op, errs(a, err));
    }
    Slice o = bytes_buffer_bytes(&out);
    OUT(&b, a, "boundary %q\nout %s\n", multipart_writer_boundary(w),
        body_str(a, mk((const Byte *)o.p, o.len)));
    multipart_writer_free(w);
    return strings_builder_string(&b);
}

static void TestWriterScript(TestingT *t) {
    Arena arena;
    arena_init(&arena, heap_allocator(), 0);
    Alloc *a = arena_allocator(&arena);
    int n = (int)(sizeof mp_writer_cases / sizeof mp_writer_cases[0]);
    for (int i = 0; i < n; i++) {
        Str got = run_writer(a, &mp_writer_cases[i]);
        if (!str_eq(got, qstr(mp_writer_cases[i].want)))
            diff(t, "writer", i, got, qstr(mp_writer_cases[i].want));
        arena_reset(&arena);
    }
    arena_free(&arena);
}

/* TestLineLimit: a reader that never sends a newline has to be given up on
 * before a megabyte. */
typedef struct Malicious {
    Int n;
} Malicious;

static Int malicious_read(void *self, Slice p, Error *err) {
    Malicious *m = self;
    m->n += p.len;
    *err = BURROW_NO_ERROR;
    if (m->n >= 1 << 20) {
        *err = io_eof;
        return 0;
    }
    memset(p.p, 'x', (size_t)p.len);
    return p.len;
}

static const IoReaderVT malicious_vt = {NULL, malicious_read};

static void TestLineLimit(TestingT *t) {
    Malicious m = {0};
    MultipartReader *r = multipart_new_reader(
        heap_allocator(), (IoReader){&malicious_vt, &m}, BURROW_S("fooBoundary"));
    Error err = BURROW_NO_ERROR;
    MultipartPart *p = multipart_reader_next_part(r, &err);
    if (p != NULL)
        testing_t_errorf_v(t, "unexpected part read");
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected an error");
    if (m.n >= 1 << 20)
        testing_t_errorf_v(t, "expected to read < %d bytes; read %d", 1 << 20, m.n);
    multipart_reader_free(r);
}

/* TestMultipartStreamReadahead: reading a part must not read past the boundary
 * after it. */
typedef struct Sentinel {
    bool read;
} Sentinel;

static Int sentinel_read(void *self, Slice p, Error *err) {
    ((Sentinel *)self)->read = true;
    *err = io_eof;
    return 0;
}

static const IoReaderVT sentinel_vt = {NULL, sentinel_read};

static void TestMultipartStreamReadahead(TestingT *t) {
    Str body1 =
        BURROW_S("\nThis is a multi-part message.  This line is "
                 "ignored.\n--MyBoundary\nfoo-bar: baz\n\nBody\n--MyBoundary\n");
    Str body2 = BURROW_S("foo-bar: bop\n\nBody 2\n--MyBoundary--\n");
    BytesReader r1, r2;
    bytes_reader_reset(&r1, sl(body1.p, body1.len));
    bytes_reader_reset(&r2, sl(body2.p, body2.len));
    Sentinel s = {false};
    IoReader rs[3] = {bytes_reader_as_io_reader(&r1),
                      {&sentinel_vt, &s},
                      bytes_reader_as_io_reader(&r2)};
    Alloc *a = heap_allocator();
    IoReader multi = io_multi_reader(a, rs, 3);
    MultipartReader *r = multipart_new_reader(a, multi, BURROW_S("MyBoundary"));
    const char *want[2][2] = {{"baz", "Body"}, {"bop", "Body 2"}};
    for (int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        MultipartPart *p = multipart_reader_next_part(r, &err);
        if (p == NULL)
            testing_t_fatalf_v(t, "Part %d: NextPart failed: %s", i, error_text(err));
        Str v =
            textproto_mime_header_get(multipart_part_header(p), BURROW_S("Foo-Bar"));
        if (!str_eq(v, str_from_cstr(want[i][0])))
            testing_t_errorf_v(t, "Part %d: Foo-Bar = %q", i, v);
        Arena ar;
        arena_init(&ar, a, 0);
        Str data = read_all(arena_allocator(&ar), part_read, p, &err);
        if (!str_eq(data, str_from_cstr(want[i][1])) || !errors_is(err, io_eof))
            testing_t_errorf_v(t, "Part %d body = %q, %s", i, data,
                               errs(arena_allocator(&ar), err));
        arena_free(&ar);
        if (i == 0 && s.read)
            testing_t_errorf_v(t, "Reader read past second boundary");
    }
    multipart_reader_free(r);
    io_multi_reader_free(a, multi);
}

/* TestNested: a part is a reader, so a multipart body inside one reads too. */
static void TestNested(TestingT *t) {
    const MpReadCase *c = NULL;
    for (size_t i = 0; i < sizeof mp_read_cases / sizeof mp_read_cases[0]; i++)
        if (str_eq(qstr(mp_read_cases[i].boundary),
                   BURROW_S("e89a8ff1c1e83553e304be640612")))
            c = &mp_read_cases[i];
    if (c == NULL)
        testing_t_fatalf_v(t, "nested-mime is not in the table");
    Alloc *a = heap_allocator();
    BytesReader br;
    bytes_reader_reset(&br, sl(c->body.p, (Int)c->body.n));
    MultipartReader *mr = multipart_new_reader(
        a, bytes_reader_as_io_reader(&br), BURROW_S("e89a8ff1c1e83553e304be640612"));
    Error err = BURROW_NO_ERROR;
    MultipartPart *p = multipart_reader_next_part(mr, &err);
    if (p == NULL)
        testing_t_fatalf_v(t, "error reading first section (alternative): %s",
                           error_text(err));
    MultipartReader *mr2 = multipart_new_reader(
        a, multipart_part_as_io_reader(p), BURROW_S("e89a8ff1c1e83553e004be640610"));
    const char *want[2] = {"*body*\r\n", "<b>body</b>\r\n"};
    Arena ar;
    arena_init(&ar, a, 0);
    for (int i = 0; i < 2; i++) {
        MultipartPart *q = multipart_reader_next_part(mr2, &err);
        if (q == NULL)
            testing_t_fatalf_v(t, "reading inner part %d: %s", i, error_text(err));
        Str got = read_all(arena_allocator(&ar), part_read, q, &err);
        if (!str_eq(got, str_from_cstr(want[i])) || !errors_is(err, io_eof))
            testing_t_fatalf_v(t, "reading inner part %d: got %q", i, got);
    }
    arena_free(&ar);
    if (multipart_reader_next_part(mr2, &err) != NULL || !errors_is(err, io_eof))
        testing_t_fatalf_v(t, "final inner NextPart is not io.EOF");
    multipart_reader_free(mr2);
    if (multipart_reader_next_part(mr, &err) == NULL)
        testing_t_fatalf_v(t, "error reading the image attachment at the end: %s",
                           error_text(err));
    if (multipart_reader_next_part(mr, &err) != NULL || !errors_is(err, io_eof))
        testing_t_fatalf_v(t, "final outer NextPart is not io.EOF");
    multipart_reader_free(mr);
}

/* TestParseAllSizes: bodies of every length up to 5 KiB round trip. */
static void TestParseAllSizes(TestingT *t) {
    enum { MAX = 5 << 10 };
    static Byte body[MAX];
    memset(body, 'a', sizeof body);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (Int size = 0; size < MAX; size++) {
        BytesBuffer buf = BYTES_BUFFER(a);
        MultipartWriter *w = multipart_new_writer(a, bytes_buffer_as_io_writer(&buf));
        Error err = BURROW_NO_ERROR;
        IoWriter part = multipart_writer_create_form_field(w, BURROW_S("f"), &err);
        part.vt->write(part.data, sl(body, size), &err);
        part = multipart_writer_create_form_field(w, BURROW_S("key"), &err);
        part.vt->write(part.data, sl("val", 3), &err);
        multipart_writer_close(w);
        BytesReader br;
        bytes_reader_reset(&br, bytes_buffer_bytes(&buf));
        MultipartReader *r = multipart_new_reader(a, bytes_reader_as_io_reader(&br),
                                                  multipart_writer_boundary(w));
        int nparts = 0;
        for (;;) {
            MultipartPart *p = multipart_reader_next_part(r, &err);
            if (p == NULL)
                break;
            Str got = read_all(a, part_read, p, &err);
            if (!errors_is(err, io_eof))
                testing_t_fatalf_v(t, "For size %d: %s", size, error_text(err));
            if (nparts == 0 && !str_eq(got, mk(body, size)))
                testing_t_fatalf_v(t, "For size %d, got unexpected len %d", size,
                                   got.len);
            nparts++;
        }
        if (!errors_is(err, io_eof) || nparts != 2)
            testing_t_fatalf_v(t, "For size %d, num parts = %d, %s", size, nparts,
                               error_text(err));
        multipart_reader_free(r);
        multipart_writer_free(w);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* TestReadForm_NoReadAfterEOF. */
typedef struct FailAfterErr {
    BytesReader r;
    bool saw_err;
    bool bad;
} FailAfterErr;

static Int fail_after_err_read(void *self, Slice p, Error *err) {
    FailAfterErr *f = self;
    if (f->saw_err)
        f->bad = true;
    Int n = bytes_reader_read(&f->r, p, err);
    f->saw_err = BURROW_FAILED(*err);
    return n;
}

static const IoReaderVT fail_after_err_vt = {NULL, fail_after_err_read};

static void TestReadFormNoReadAfterEOF(TestingT *t) {
    Str body =
        BURROW_S("\n-----------------------------8d345eef0d38dc9\nContent-Disposition: "
                 "form-data; name=\"version\"\n\n171\n-----------------------------"
                 "8d345eef0d38dc9--");
    FailAfterErr f = {.saw_err = false};
    bytes_reader_reset(&f.r, sl(body.p, body.len));
    MultipartReader *r =
        multipart_new_reader(heap_allocator(), (IoReader){&fail_after_err_vt, &f},
                             BURROW_S("---------------------------8d345eef0d38dc9"));
    Error err = BURROW_NO_ERROR;
    MultipartForm *form = multipart_reader_read_form(r, (int64_t)32 << 20, &err);
    if (form == NULL)
        testing_t_fatalf_v(t, "ReadForm: %s", error_text(err));
    if (f.bad)
        testing_t_errorf_v(t, "Read on Reader after previous read saw an error");
    multipart_form_free(form);
    multipart_reader_free(r);
}

static Str write_body(Alloc *a, void (*fill)(MultipartWriter *, Alloc *),
                      Str *boundary) {
    BytesBuffer *buf = mem_alloc(a, sizeof *buf, _Alignof(BytesBuffer));
    *buf = BYTES_BUFFER(a);
    MultipartWriter *w = multipart_new_writer(a, bytes_buffer_as_io_writer(buf));
    fill(w, a);
    multipart_writer_close(w);
    *boundary = strings_clone(a, multipart_writer_boundary(w));
    multipart_writer_free(w);
    Slice s = bytes_buffer_bytes(buf);
    return mk((const Byte *)s.p, s.len);
}

static void fill_large_name(MultipartWriter *fw, Alloc *a) {
    Str name = strings_repeat(a, BURROW_S("a"), 10 << 20);
    Error err;
    IoWriter w = multipart_writer_create_form_field(fw, name, &err);
    w.vt->write(w.data, sl("value", 5), &err);
}

static void fill_large_header(MultipartWriter *fw, Alloc *a) {
    TextprotoMIMEHeader h = textproto_mime_header_make(a);
    textproto_mime_header_set(h, BURROW_S("Content-Disposition"),
                              BURROW_S("form-data; name=\"a\""));
    textproto_mime_header_set(h, BURROW_S("X-Foo"),
                              strings_repeat(a, BURROW_S("a"), 10 << 20));
    Error err;
    IoWriter w = multipart_writer_create_part(fw, h, &err);
    w.vt->write(w.data, sl("value", 5), &err);
}

static void fill_many_parts(MultipartWriter *fw, Alloc *a) {
    for (int i = 0; i < 110000; i++) {
        Error err;
        IoWriter w = multipart_writer_create_form_field(fw, BURROW_S("f"), &err);
        w.vt->write(w.data, sl("v", 1), &err);
    }
}

static Str large_text;

static void fill_large_text(MultipartWriter *fw, Alloc *a) {
    Error err;
    IoWriter w = multipart_writer_create_form_field(fw, BURROW_S("largetext"), &err);
    w.vt->write(w.data, sl(large_text.p, large_text.len), &err);
}

static MultipartForm *read_body(Alloc *a, Str body, Str boundary, int64_t max_memory,
                                Error *err, MultipartReader **out) {
    BytesReader *br = mem_alloc(a, sizeof *br, _Alignof(BytesReader));
    bytes_reader_reset(br, sl(body.p, body.len));
    *out = multipart_new_reader(a, bytes_reader_as_io_reader(br), boundary);
    return multipart_reader_read_form(*out, max_memory, err);
}

/* TestReadForm_MetadataTooLarge. */
static void TestReadFormMetadataTooLarge(TestingT *t) {
    void (*fills[3])(MultipartWriter *, Alloc *) = {fill_large_name, fill_large_header,
                                                    fill_many_parts};
    for (int i = 0; i < 3; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Alloc *a = arena_allocator(&ar);
        Str boundary;
        Str body = write_body(a, fills[i], &boundary);
        Error err = BURROW_NO_ERROR;
        MultipartReader *r;
        MultipartForm *f = read_body(a, body, boundary, 0, &err, &r);
        if (f != NULL || !errors_is(err, multipart_err_message_too_large))
            testing_t_errorf_v(t, "case %d: ReadForm = %s, want ErrMessageTooLarge", i,
                               errs(a, err));
        multipart_form_free(f);
        multipart_reader_free(r);
        arena_free(&ar);
    }
}

/* TestReadForm_NonFileMaxMemory. */
static void TestReadFormNonFileMaxMemory(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    large_text = strings_repeat(a, BURROW_S("1"), 10 << 20);
    Str boundary;
    Str body = write_body(a, fill_large_text, &boundary);
    bool ok = false;
    for (int64_t mm = 0; mm < 256; mm += 16) {
        Error err = BURROW_NO_ERROR;
        MultipartReader *r;
        MultipartForm *f = read_body(a, body, boundary, mm, &err, &r);
        if (f == NULL) {
            multipart_reader_free(r);
            continue;
        }
        Str key = BURROW_S("largetext");
        Slice vs = *(const Slice *)map_get(f->value, &key);
        if (!str_eq(((Str *)vs.p)[0], large_text))
            testing_t_errorf_v(t, "largetext mismatch");
        if (mm < 128)
            testing_t_errorf_v(t, "ReadForm(%d): no error", mm);
        multipart_form_remove_all(f);
        multipart_form_free(f);
        multipart_reader_free(r);
        ok = true;
        break;
    }
    if (!ok)
        testing_t_errorf_v(t, "ReadForm(x) failed for x < 256, expect success");
    arena_free(&ar);
}

static void fill_ten_files(MultipartWriter *fw, Alloc *a) {
    for (int i = 0; i < 10; i++) {
        Str name = fmt_sprintf_v(a, "%d", i);
        Error err;
        IoWriter w = multipart_writer_create_form_file(fw, name, name, &err);
        w.vt->write(w.data, sl(name.p, name.len), &err);
    }
}

/* TestReadForm_ManyFiles_Combined and _Distinct. */
static void many_files(TestingT *t, bool distinct) {
    make_dir(t);
    burrow__multipart_godebug_set(distinct ? "multipartfiles=distinct" : "");
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str boundary;
    Str body = write_body(a, fill_ten_files, &boundary);
    BytesReader br;
    bytes_reader_reset(&br, sl(body.p, body.len));
    MultipartReader *r =
        multipart_new_reader(a, bytes_reader_as_io_reader(&br), boundary);
    burrow__multipart_reader_set_temp_dir(r, mp_dir);
    Error err = BURROW_NO_ERROR;
    MultipartForm *form = multipart_reader_read_form(r, 0, &err);
    burrow__multipart_godebug_set(NULL);
    if (form == NULL)
        testing_t_fatalf_v(t, "ReadForm: %s", error_text(err));
    for (int i = 0; i < 10; i++) {
        Str name = fmt_sprintf_v(a, "%d", i);
        const Slice *fhs = map_get(form->file, &name);
        if (fhs == NULL || fhs->len != 1)
            testing_t_fatalf_v(t, "form.File[%q] does not have 1 entry", name);
        MultipartFile *file =
            multipart_file_header_open(((MultipartFileHeader **)fhs->p)[0], a, &err);
        if (file == NULL)
            testing_t_fatalf_v(t, "form.File[%q].Open() = %s", name, error_text(err));
        Str got = read_all(a, file_read, file, &err);
        multipart_file_close(file);
        if (!str_eq(got, name) || !errors_is(err, io_eof))
            testing_t_fatalf_v(t, "read form.File[%q]: %q", name, got);
    }
    int want = distinct ? 10 : 1;
    if (count_dir() != want)
        testing_t_fatalf_v(t, "temp dir contains %d files; want %d", count_dir(), want);
    err = multipart_form_remove_all(form);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "form.RemoveAll() = %s", error_text(err));
    if (count_dir() != 0)
        testing_t_fatalf_v(t, "temp dir contains %d files; want 0", count_dir());
    multipart_form_free(form);
    multipart_reader_free(r);
    arena_free(&ar);
    remove_dir();
}

static void TestReadFormManyFilesCombined(TestingT *t) {
    many_files(t, false);
}

static void TestReadFormManyFilesDistinct(TestingT *t) {
    many_files(t, true);
}

/* TestReadFormLimits. */
static int lim_values, lim_files, lim_extra;

static void fill_limits(MultipartWriter *fw, Alloc *a) {
    Error err;
    for (int i = 0; i < lim_values; i++) {
        IoWriter w = multipart_writer_create_form_field(
            fw, fmt_sprintf_v(a, "field%d", i), &err);
        Str v = fmt_sprintf_v(a, "value %d", i);
        w.vt->write(w.data, sl(v.p, v.len), &err);
    }
    for (int i = 0; i < lim_files; i++) {
        TextprotoMIMEHeader h = textproto_mime_header_make(a);
        textproto_mime_header_set(
            h, BURROW_S("Content-Disposition"),
            fmt_sprintf_v(a, "form-data; name=\"file%d\"; filename=\"file%d\"", i, i));
        textproto_mime_header_set(h, BURROW_S("Content-Type"),
                                  BURROW_S("application/octet-stream"));
        for (int j = 0; j < lim_extra; j++)
            textproto_mime_header_set(h, fmt_sprintf_v(a, "k%d", j), BURROW_S("v"));
        IoWriter w = multipart_writer_create_part(fw, h, &err);
        Str v = fmt_sprintf_v(a, "value %d", i);
        w.vt->write(w.data, sl(v.p, v.len), &err);
    }
}

static void TestReadFormLimits(TestingT *t) {
    static const struct {
        int values, files, extra;
        bool too_large;
        const char *godebug;
    } tests[] = {
        {1000, 0, 0, false, ""},
        {1001, 0, 0, true, ""},
        {500, 500, 0, false, ""},
        {501, 500, 0, true, ""},
        {0, 1000, 0, false, ""},
        {0, 1001, 0, true, ""},
        {0, 1, 9998, false, ""},
        {0, 1, 10000, true, ""},
        {100, 0, 0, false, "multipartmaxparts=100"},
        {101, 0, 0, true, "multipartmaxparts=100"},
        {0, 2, 48, false, "multipartmaxheaders=100"},
        {0, 2, 50, true, "multipartmaxheaders=100"},
    };
    make_dir(t);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Alloc *a = arena_allocator(&ar);
        lim_values = tests[i].values;
        lim_files = tests[i].files;
        lim_extra = tests[i].extra;
        Str boundary;
        Str body = write_body(a, fill_limits, &boundary);
        burrow__multipart_godebug_set(tests[i].godebug);
        Error err = BURROW_NO_ERROR;
        MultipartReader *r;
        MultipartForm *f = read_body(a, body, boundary, 1 << 10, &err, &r);
        burrow__multipart_godebug_set(NULL);
        bool too_large = f == NULL && errors_is(err, multipart_err_message_too_large);
        if (too_large != tests[i].too_large || (f == NULL && !too_large))
            testing_t_errorf_v(t, "case %d: ReadForm = %s", (int)i, errs(a, err));
        if (f != NULL) {
            multipart_form_remove_all(f);
            multipart_form_free(f);
        }
        multipart_reader_free(r);
        arena_free(&ar);
    }
    remove_dir();
}

/* TestReadFormEndlessHeaderLine. */
static Int endless_read(void *self, Slice p, Error *err) {
    memset(p.p, 'X', (size_t)p.len);
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoReaderVT endless_vt = {NULL, endless_read};

static void TestReadFormEndlessHeaderLine(TestingT *t) {
    const char *prefixes[] = {"X-", "X-Header: ", "X-Header: foo\r\n  "};
    for (int i = 0; i < 3; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Alloc *a = arena_allocator(&ar);
        Str s = fmt_sprintf_v(a,
                              "--boundary\r\nContent-Disposition: form-data; "
                              "name=\"a\"\r\nContent-Type: text/plain\r\n%s",
                              str_from_cstr(prefixes[i]));
        BytesReader br;
        bytes_reader_reset(&br, sl(s.p, s.len));
        IoReader rs[2] = {bytes_reader_as_io_reader(&br), {&endless_vt, NULL}};
        IoReader multi = io_multi_reader(a, rs, 2);
        MultipartReader *r = multipart_new_reader(a, multi, BURROW_S("boundary"));
        Error err = BURROW_NO_ERROR;
        MultipartForm *f = multipart_reader_read_form(r, 1 << 20, &err);
        if (f != NULL || !errors_is(err, multipart_err_message_too_large))
            testing_t_fatalf_v(
                t, "case %d: ReadForm(1 << 20): %s, want ErrMessageTooLarge", i,
                errs(a, err));
        multipart_reader_free(r);
        arena_free(&ar);
    }
}

/* NewWriter makes a boundary of 60 random hex digits, a new one every time. */
static void TestWriterRandomBoundary(TestingT *t) {
    Alloc *a = heap_allocator();
    MultipartWriter *w1 = multipart_new_writer(a, io_discard);
    MultipartWriter *w2 = multipart_new_writer(a, io_discard);
    Str b1 = multipart_writer_boundary(w1), b2 = multipart_writer_boundary(w2);
    if (b1.len != 60 || b2.len != 60 || str_eq(b1, b2))
        testing_t_errorf_v(t, "boundaries %q and %q", b1, b2);
    for (Int i = 0; i < b1.len; i++)
        if (!strings_contains_rune(BURROW_S("0123456789abcdef"), b1.p[i]))
            testing_t_errorf_v(t, "boundary %q is not hex", b1);
    Arena ar;
    arena_init(&ar, a, 0);
    Str ct = multipart_writer_form_data_content_type(w1, arena_allocator(&ar));
    Map *params = NULL;
    Error err;
    Str mt = mime_parse_media_type(arena_allocator(&ar), ct, &params, &err);
    Str key = BURROW_S("boundary");
    const Str *got = params == NULL ? NULL : map_get(params, &key);
    if (!str_eq(mt, BURROW_S("multipart/form-data")) || got == NULL ||
        !str_eq(*got, b1))
        testing_t_errorf_v(t, "FormDataContentType = %q", ct);
    arena_free(&ar);
    multipart_writer_free(w1);
    multipart_writer_free(w2);
}

/* The part and file readers as the io interfaces. */
static void TestInterfaces(TestingT *t) {
    Str body = BURROW_S("--b\r\nContent-Disposition: form-data; name=\"f\"; "
                        "filename=\"x.txt\"\r\n\r\nhello\r\n--b--\r\n");
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    bytes_reader_reset(&br, sl(body.p, body.len));
    MultipartReader *r =
        multipart_new_reader(a, bytes_reader_as_io_reader(&br), BURROW_S("b"));
    Error err = BURROW_NO_ERROR;
    MultipartPart *p = multipart_reader_next_part(r, &err);
    IoReadCloser rc = multipart_part_as_io_read_closer(p);
    Byte buf[8];
    Int n = rc.vt->reader.read(rc.data, sl(buf, 2), &err);
    if (n != 2 || memcmp(buf, "he", 2) != 0)
        testing_t_errorf_v(t, "part read = %d", n);
    rc.vt->closer.close(rc.data);
    n = multipart_part_read(p, sl(buf, 8), &err);
    if (n != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "read after close = %d, %s", n, errs(a, err));
    multipart_reader_free(r);

    bytes_reader_reset(&br, sl(body.p, body.len));
    r = multipart_new_reader(a, bytes_reader_as_io_reader(&br), BURROW_S("b"));
    MultipartForm *f = multipart_reader_read_form(r, 1 << 20, &err);
    Str key = BURROW_S("f");
    MultipartFileHeader *fh =
        ((MultipartFileHeader **)((const Slice *)map_get(f->file, &key))->p)[0];
    MultipartFile *file = multipart_file_header_open(fh, a, &err);
    IoReadSeekCloser rsc = multipart_file_as_io_read_seek_closer(file);
    rsc.vt->seeker.seek(rsc.data, 1, BURROW_IO_SEEK_START, &err);
    n = rsc.vt->reader.read(rsc.data, sl(buf, 8), &err);
    if (n != 4 || memcmp(buf, "ello", 4) != 0)
        testing_t_errorf_v(t, "file read = %d", n);
    IoReaderAt ra = multipart_file_as_io_reader_at(file);
    n = ra.vt->read_at(ra.data, sl(buf, 3), 2, &err);
    if (n != 3 || memcmp(buf, "llo", 3) != 0)
        testing_t_errorf_v(t, "file read_at = %d", n);
    rsc.vt->closer.close(rsc.data);
    multipart_form_free(f);
    multipart_reader_free(r);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReaderScript)                                                                \
    X(TestFormScript)                                                                  \
    X(TestWriterScript)                                                                \
    X(TestLineLimit)                                                                   \
    X(TestMultipartStreamReadahead)                                                    \
    X(TestNested)                                                                      \
    X(TestParseAllSizes)                                                               \
    X(TestReadFormNoReadAfterEOF)                                                      \
    X(TestReadFormMetadataTooLarge)                                                    \
    X(TestReadFormNonFileMaxMemory)                                                    \
    X(TestReadFormManyFilesCombined)                                                   \
    X(TestReadFormManyFilesDistinct)                                                   \
    X(TestReadFormLimits)                                                              \
    X(TestReadFormEndlessHeaderLine)                                                   \
    X(TestWriterRandomBoundary)                                                        \
    X(TestInterfaces)

TESTING_MAIN(TESTS)
