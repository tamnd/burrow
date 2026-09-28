/* Derived from Go's src/mime/type_test.go and type_unix_test.go. Go source:
 * go1.27.1.
 *
 * tests/mime_type_test_gen.h, from tools/gen-mime-type-tests.sh, is a script of
 * steps on the extension table with what Go's mime package answered at each
 * one. Go's own test cases are in it, the built in table is asked about every
 * extension and type it has, and 200 globs2 and mime.types files built from
 * pieces are loaded and probed. This replays the script against burrow and
 * checks every answer, errors included.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mime.h"
#include "burrow/pal.h"

#include "../src/mime/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct QStr {
    const char *p;
    long long n;
} QStr;

#define QS(x) {x, (long long)sizeof(x) - 1}

typedef enum MimeTypeOp {
    MT_RESET_EMPTY,
    MT_RESET_BUILTIN,
    MT_GLOBS,
    MT_GLOBS_MISSING,
    MT_TYPES,
    MT_SET,
    MT_ADD,
    MT_TYPE,
    MT_EXTS,
} MimeTypeOp;

typedef struct MimeTypeStep {
    MimeTypeOp op;
    QStr a;
    QStr b;
    QStr want;
    const char *err;
} MimeTypeStep;

#include "mime_type_test_gen.h"

static Str qstr(QStr q) {
    return (Str){(const Byte *)q.p, (Int)q.n};
}

static bool err_is(Error err, const char *want) {
    if (!BURROW_FAILED(err))
        return want[0] == '\0';
    Str s = error_text(err);
    size_t n = strlen(want);
    return s.len == (Int)n && n > 0 && memcmp(s.p, want, n) == 0;
}

static Str err_text(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : (Str){NULL, 0};
}

static const char *temp_root(void) {
    const char *names[] = {"TMPDIR", "TEMP", "TMP"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *v = getenv(names[i]);
        if (v != NULL && v[0] != '\0')
            return v;
    }
    return "/tmp";
}

static void write_file(TestingT *t, const char *path, Str text) {
    PalErrno e = PAL_OK;
    int64_t fd = pal_open(path, PAL_O_WRONLY | PAL_O_CREATE | PAL_O_TRUNC, 0600, &e);
    if (fd < 0)
        testing_t_fatalf_v(t, "open %s: %s", path, pal_errno_string(e));
    Int off = 0;
    while (off < text.len) {
        int64_t n = pal_write(fd, text.p + off, text.len - off, &e);
        if (n <= 0)
            testing_t_fatalf_v(t, "write %s: %s", path, pal_errno_string(e));
        off += n;
    }
    pal_close(fd, NULL);
}

/* The extensions as the generator writes them, each with a newline after it. */
static Str joined(Alloc *a, Slice l) {
    StringsBuilder b = STRINGS_BUILDER(a);
    const Str *p = (const Str *)l.p;
    for (Int i = 0; i < l.len; i++) {
        strings_builder_write_string(&b, p[i], NULL);
        strings_builder_write_byte(&b, '\n');
    }
    return strings_builder_string(&b);
}

static void TestTypeScript(TestingT *t) {
    char path[512];
    uint32_t r = 0;
    pal_random_bytes(&r, sizeof r, NULL);
    snprintf(path, sizeof path, "%s/burrow-mime-%08x", temp_root(), (unsigned)r);
    char missing[600];
    snprintf(missing, sizeof missing, "%s-missing", path);

    Arena arena;
    arena_init(&arena, heap_allocator(), 0);
    Alloc *a = arena_allocator(&arena);
    for (int i = 0; i < MIME_TYPE_STEP_COUNT; i++) {
        const MimeTypeStep *s = &mime_type_steps[i];
        Str x = qstr(s->a), y = qstr(s->b), want = qstr(s->want);
        switch (s->op) {
        case MT_RESET_EMPTY:
        case MT_RESET_BUILTIN:
            burrow__mime_types_reset(s->op == MT_RESET_BUILTIN);
            break;
        case MT_GLOBS:
        case MT_TYPES:
            write_file(t, path, x);
            if (s->op == MT_TYPES) {
                burrow__mime_load_types_file(path);
            } else if (burrow__mime_load_globs_file(path) != (s->err[0] == '\0')) {
                testing_t_errorf_v(t, "step %d: loading globs2 %q: ok is wrong", i, x);
            }
            pal_unlink(path, NULL);
            break;
        case MT_GLOBS_MISSING:
            if (burrow__mime_load_globs_file(missing))
                testing_t_errorf_v(t, "step %d: loading a missing globs2 file worked",
                                   i);
            break;
        case MT_SET:
        case MT_ADD: {
            Error err = s->op == MT_SET ? burrow__mime_set_extension_type(x, y)
                                        : mime_add_extension_type(x, y);
            if (!err_is(err, s->err))
                testing_t_errorf_v(t, "step %d: %s(%q, %q) = %q, want %q", i,
                                   s->op == MT_SET ? "setExtensionType"
                                                   : "AddExtensionType",
                                   x, y, err_text(err), str_from_cstr(s->err));
            break;
        }
        case MT_TYPE: {
            Str got = mime_type_by_extension(x);
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "step %d: TypeByExtension(%q) = %q, want %q", i,
                                   x, got, want);
            break;
        }
        case MT_EXTS: {
            Error err;
            Slice l = mime_extensions_by_type(a, x, &err);
            Str got = joined(a, l);
            if (!str_eq(got, want) || !err_is(err, s->err))
                testing_t_errorf_v(
                    t, "step %d: ExtensionsByType(%q) = %q, %q, want %q, %q", i, x, got,
                    err_text(err), want, str_from_cstr(s->err));
            arena_reset(&arena);
            break;
        }
        default:
            testing_t_fatalf_v(t, "step %d: unknown op", i);
        }
    }
    arena_free(&arena);
    burrow__mime_types_reset(true);
}

/* The table is read under a lock and written under it, so lookups and adds
 * from many threads at once have to see whole entries. */
typedef struct Racer {
    int id;
} Racer;

static void race(void *env) {
    Racer *r = env;
    char ext[32], typ[64];
    for (int i = 0; i < 200; i++) {
        snprintf(ext, sizeof ext, ".r%d_%d", r->id, i);
        snprintf(typ, sizeof typ, "application/x-r%d-%d", r->id, i);
        mime_add_extension_type(str_from_cstr(ext), str_from_cstr(typ));
        Str got = mime_type_by_extension(str_from_cstr(ext));
        if (!str_eq(got, str_from_cstr(typ)))
            abort();
        if (mime_type_by_extension(BURROW_S(".html")).len == 0)
            abort();
    }
}

static void TestTypeConcurrent(TestingT *t) {
    (void)t;
    burrow__mime_types_reset(true);
    enum { N = 8 };
    Racer rs[N];
    int64_t th[N];
    for (int i = 0; i < N; i++) {
        rs[i].id = i;
        th[i] = pal_thread_create(race, &rs[i], 0, NULL);
    }
    for (int i = 0; i < N; i++)
        pal_thread_join(th[i], NULL);
    burrow__mime_types_reset(true);
}

#define TESTS(X) X(TestTypeScript) X(TestTypeConcurrent)
TESTING_MAIN(TESTS)
