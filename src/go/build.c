/* go/build: finding Go packages and the files that make them up.
 *
 * Derived from Go's src/go/build/build.go and gc.go, with what it takes from
 * internal/buildcfg (the tool tags), internal/syslist (the known systems),
 * internal/platform (where cgo works) and internal/goroot (gccgo's standard
 * packages).
 * Go source: go1.27.1.
 *
 * Go works out its Default context and the default tool tags once, when the
 * program starts. Nothing here is kept between calls, so build_default looks
 * at the environment each time, and the check in importGo for tags that have
 * been changed compares with what the environment gives now.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/build.h"

#include "build_internal.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/go/ast.h"
#include "burrow/go/build/constraint.h"
#include "burrow/go/doc.h"
#include "burrow/go/token.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/os/exec.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/path/filepath.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

/* ---------------------------------------------------------------- helpers */

BURROW_NORETURN static void gb_oom(void) {
    panic_str(S("go/build: out of memory"));
}

static void *gb_alloc(Alloc *a, size_t n) {
    void *p = mem_alloc(a, n, BURROW_ALIGN_MAX);
    if (p == NULL)
        gb_oom();
    return p;
}

#define GB_NEW(a, T) ((T *)gb_alloc((a), sizeof(T)))

/* Appends *v to *s, giving a nil s the element type elem first. */
static void gb_append(Alloc *a, Slice *s, const Type *elem, const void *v) {
    if (s->elem == NULL)
        *s = slice_nil(elem);
    Slice next = slice_append(a, *s, v, 1);
    if (next.len != s->len + 1)
        gb_oom();
    *s = next;
}

static void gb_append_str(Alloc *a, Slice *s, Str v) {
    gb_append(a, s, TYPE_STRING, &v);
}

static void gb_append_slice(Alloc *a, Slice *s, Slice t) {
    if (t.len == 0)
        return;
    if (s->elem == NULL)
        *s = slice_nil(t.elem);
    Slice next = slice_append_slice(a, *s, t);
    if (next.len != s->len + t.len)
        gb_oom();
    *s = next;
}

/* s[lo:hi]. An empty s may have a NULL p, which stays as it is, since NULL + 0
 * is undefined; any other s keeps its pointer, so offsets into it still work. */
static Str gb_sub(Str s, Int lo, Int hi) {
    if (s.p == NULL)
        return s;
    return str_from_bytes(s.p + lo, hi - lo);
}

static Str gb_from(Str s, Int lo) {
    return gb_sub(s, lo, s.len);
}

/* The n Strs after n joined end to end, in a. */
static Str gb_cat(Alloc *a, int n, ...) {
    va_list ap;
    va_start(ap, n);
    Int total = 0;
    for (int i = 0; i < n; i++)
        total += va_arg(ap, Str).len;
    va_end(ap);
    if (total == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)total, 1);
    if (p == NULL)
        gb_oom();
    Int at = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        Str s = va_arg(ap, Str);
        if (s.len > 0)
            memcpy(p + at, s.p, (size_t)s.len);
        at += s.len;
    }
    va_end(ap);
    return str_from_bytes(p, total);
}

/* The n Strs after n as a Slice of Str. */
static Slice gb_strs(Alloc *a, int n, ...) {
    Slice s = slice_nil(TYPE_STRING);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        gb_append_str(a, &s, va_arg(ap, Str));
    va_end(ap);
    return s;
}

static Str gb_at(Slice s, Int i) {
    return BURROW_AT(Str, s, i);
}

static bool gb_contains(Slice list, Str s) {
    for (Int i = 0; i < list.len; i++) {
        if (str_eq(gb_at(list, i), s))
            return true;
    }
    return false;
}

static bool gb_slices_equal(Slice x, Slice y) {
    if (x.len != y.len)
        return false;
    for (Int i = 0; i < x.len; i++) {
        if (!str_eq(gb_at(x, i), gb_at(y, i)))
            return false;
    }
    return true;
}

static bool gb_in(const Str *list, size_t n, Str s) {
    for (size_t i = 0; i < n; i++) {
        if (str_eq(list[i], s))
            return true;
    }
    return false;
}

static Map *gb_map(Alloc *a, const Type *key, const Type *val) {
    Map *m = map_make(a, key, val, 0);
    if (m == NULL)
        gb_oom();
    return m;
}

static void gb_set_true(Map *m, Str key) {
    bool t = true;
    if (!map_set(m, &key, &t))
        gb_oom();
}

/* The Str keys of m, sorted. */
static Slice gb_sorted_keys(Alloc *a, Map *m) {
    Slice all = slice_nil(TYPE_STRING);
    MapIter it = map_iter(m);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v))
        gb_append_str(a, &all, *(const Str *)k);
    sort_strings(all);
    return all;
}

static bool gb_is_eof(Error err) {
    return errors_is(err, io_eof);
}

/* ------------------------------------------------------------------ types */

static const Type gb_position_desc = {
    {(const Byte *)"Position", 8},
    {(const Byte *)"go/token", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(TokenPosition),
    (uint16_t)_Alignof(TokenPosition),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x67627073U, /* "gbps" */
    NULL,
};

/* []token.Position, what the position maps hold. */
static const Type gb_positions_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &gb_position_desc,
    NULL,
    0,
    0,
    NULL,
};

static const Type gb_directive_desc = {
    {(const Byte *)"Directive", 9},
    {(const Byte *)"go/build", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(BuildDirective),
    (uint16_t)_Alignof(BuildDirective),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x67626476U, /* "gbdv" */
    NULL,
};

const Type *const TYPE_BUILD_DIRECTIVE = &gb_directive_desc;

/* ----------------------------------------------------------------- errors */

static const Str gb_no_modules_text = BURROW_S_INIT("not using modules");
static const Error gb_err_no_modules = {&burrow_sentinel_error_vt, &gb_no_modules_text};
static const Str gb_multiple_go_build_text =
    BURROW_S_INIT("multiple //go:build comments");
static const Error gb_err_multiple_go_build = {&burrow_sentinel_error_vt,
                                               &gb_multiple_go_build_text};
static const Str gb_unclosed_quote_text = BURROW_S_INIT("unclosed quote");
static const Error gb_err_unclosed_quote = {&burrow_sentinel_error_vt,
                                            &gb_unclosed_quote_text};
static const Str gb_unfinished_escape_text = BURROW_S_INIT("unfinished escaping");
static const Error gb_err_unfinished_escape = {&burrow_sentinel_error_vt,
                                               &gb_unfinished_escape_text};
static const Str gb_arch_char_text =
    BURROW_S_INIT("architecture letter no longer used");
static const Error gb_err_arch_char = {&burrow_sentinel_error_vt, &gb_arch_char_text};

Error burrow__build_err_no_modules(void) {
    return gb_err_no_modules;
}

static bool gb_is_no_modules(Error err) {
    return err.vt == gb_err_no_modules.vt && err.data == gb_err_no_modules.data;
}

/* A NoGoError with the text of its message after it. */
typedef struct GbNoGoBox {
    BuildNoGoError e;
    Str text;
} GbNoGoBox;

static const Type gb_no_go_desc = {
    {(const Byte *)"NoGoError", 9},
    {(const Byte *)"go/build", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(BuildNoGoError),
    (uint16_t)_Alignof(BuildNoGoError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x67626e67U, /* "gbng" */
    NULL,
};

const Type *const TYPE_BUILD_NO_GO_ERROR = &gb_no_go_desc;

Str build_no_go_error_error(const BuildNoGoError *e, Alloc *a) {
    return gb_cat(a, 2, S("no buildable Go source files in "), e->dir);
}

static Error gb_no_go_error(Alloc *a, Str dir);

static Str gb_no_go_message(const void *self) {
    return ((const GbNoGoBox *)self)->text;
}

static Error gb_no_go_clone(const void *self, Alloc *a) {
    return gb_no_go_error(a, ((const GbNoGoBox *)self)->e.dir);
}

static const ErrorVT gb_no_go_vt = {
    .self_type = &gb_no_go_desc,
    .message = gb_no_go_message,
    .clone = gb_no_go_clone,
};

static Error gb_no_go_error(Alloc *a, Str dir) {
    GbNoGoBox *b = (GbNoGoBox *)mem_alloc(a, sizeof(GbNoGoBox), _Alignof(GbNoGoBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e.dir = str_clone(a, dir);
    b->text = build_no_go_error_error(&b->e, a);
    return (Error){&gb_no_go_vt, b};
}

typedef struct GbMultiBox {
    BuildMultiplePackageError e;
    Str text;
} GbMultiBox;

static const Type gb_multi_desc = {
    {(const Byte *)"MultiplePackageError", 20},
    {(const Byte *)"go/build", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(BuildMultiplePackageError),
    (uint16_t)_Alignof(BuildMultiplePackageError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x67626d70U, /* "gbmp" */
    NULL,
};

const Type *const TYPE_BUILD_MULTIPLE_PACKAGE_ERROR = &gb_multi_desc;

Str build_multiple_package_error_error(const BuildMultiplePackageError *e, Alloc *a) {
    Str pkg0 = gb_at(e->packages, 0);
    Str file0 = gb_at(e->files, 0);
    Str pkg1 = gb_at(e->packages, 1);
    Str file1 = gb_at(e->files, 1);
    return fmt_sprintf_v(a, "found packages %s (%s) and %s (%s) in %s", pkg0, file0,
                         pkg1, file1, e->dir);
}

static Error gb_multi_error(Alloc *a, Str dir, Str pkg0, Str pkg1, Str file0,
                            Str file1);

static Str gb_multi_message(const void *self) {
    return ((const GbMultiBox *)self)->text;
}

static Error gb_multi_clone(const void *self, Alloc *a) {
    const BuildMultiplePackageError *e = &((const GbMultiBox *)self)->e;
    return gb_multi_error(a, e->dir, gb_at(e->packages, 0), gb_at(e->packages, 1),
                          gb_at(e->files, 0), gb_at(e->files, 1));
}

static const ErrorVT gb_multi_vt = {
    .self_type = &gb_multi_desc,
    .message = gb_multi_message,
    .clone = gb_multi_clone,
};

static Error gb_multi_error(Alloc *a, Str dir, Str pkg0, Str pkg1, Str file0,
                            Str file1) {
    GbMultiBox *b =
        (GbMultiBox *)mem_alloc(a, sizeof(GbMultiBox), _Alignof(GbMultiBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e.dir = str_clone(a, dir);
    b->e.packages = gb_strs(a, 2, str_clone(a, pkg0), str_clone(a, pkg1));
    b->e.files = gb_strs(a, 2, str_clone(a, file0), str_clone(a, file1));
    b->text = build_multiple_package_error_error(&b->e, a);
    return (Error){&gb_multi_vt, b};
}

/* --------------------------------------------------------- system tables */

/* internal/syslist.KnownOS. */
static const Str gb_known_os[] = {
    BURROW_S_INIT("aix"),       BURROW_S_INIT("android"), BURROW_S_INIT("darwin"),
    BURROW_S_INIT("dragonfly"), BURROW_S_INIT("freebsd"), BURROW_S_INIT("hurd"),
    BURROW_S_INIT("illumos"),   BURROW_S_INIT("ios"),     BURROW_S_INIT("js"),
    BURROW_S_INIT("linux"),     BURROW_S_INIT("nacl"),    BURROW_S_INIT("netbsd"),
    BURROW_S_INIT("openbsd"),   BURROW_S_INIT("plan9"),   BURROW_S_INIT("solaris"),
    BURROW_S_INIT("wasip1"),    BURROW_S_INIT("windows"), BURROW_S_INIT("zos"),
};

/* internal/syslist.UnixOS, the systems the unix tag matches. */
static const Str gb_unix_os[] = {
    BURROW_S_INIT("aix"),       BURROW_S_INIT("android"), BURROW_S_INIT("darwin"),
    BURROW_S_INIT("dragonfly"), BURROW_S_INIT("freebsd"), BURROW_S_INIT("hurd"),
    BURROW_S_INIT("illumos"),   BURROW_S_INIT("ios"),     BURROW_S_INIT("linux"),
    BURROW_S_INIT("netbsd"),    BURROW_S_INIT("openbsd"), BURROW_S_INIT("solaris"),
};

/* internal/syslist.KnownArch. */
static const Str gb_known_arch[] = {
    BURROW_S_INIT("386"),       BURROW_S_INIT("amd64"),       BURROW_S_INIT("amd64p32"),
    BURROW_S_INIT("arm"),       BURROW_S_INIT("armbe"),       BURROW_S_INIT("arm64"),
    BURROW_S_INIT("arm64be"),   BURROW_S_INIT("loong64"),     BURROW_S_INIT("mips"),
    BURROW_S_INIT("mipsle"),    BURROW_S_INIT("mips64"),      BURROW_S_INIT("mips64le"),
    BURROW_S_INIT("mips64p32"), BURROW_S_INIT("mips64p32le"), BURROW_S_INIT("ppc"),
    BURROW_S_INIT("ppc64"),     BURROW_S_INIT("ppc64le"),     BURROW_S_INIT("riscv"),
    BURROW_S_INIT("riscv64"),   BURROW_S_INIT("s390"),        BURROW_S_INIT("s390x"),
    BURROW_S_INIT("sparc"),     BURROW_S_INIT("sparc64"),     BURROW_S_INIT("wasm"),
};

#define GB_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

static bool gb_known_os_has(Str s) {
    return gb_in(gb_known_os, GB_LEN(gb_known_os), s);
}

static bool gb_known_arch_has(Str s) {
    return gb_in(gb_known_arch, GB_LEN(gb_known_arch), s);
}

/* internal/platform: the GOOS/GOARCH pairs where cgo works. */
static const Str gb_cgo_pairs[] = {
    BURROW_S_INIT("aix/ppc64"),       BURROW_S_INIT("android/386"),
    BURROW_S_INIT("android/amd64"),   BURROW_S_INIT("android/arm"),
    BURROW_S_INIT("android/arm64"),   BURROW_S_INIT("darwin/amd64"),
    BURROW_S_INIT("darwin/arm64"),    BURROW_S_INIT("dragonfly/amd64"),
    BURROW_S_INIT("freebsd/386"),     BURROW_S_INIT("freebsd/amd64"),
    BURROW_S_INIT("freebsd/arm"),     BURROW_S_INIT("freebsd/arm64"),
    BURROW_S_INIT("freebsd/riscv64"), BURROW_S_INIT("illumos/amd64"),
    BURROW_S_INIT("ios/amd64"),       BURROW_S_INIT("ios/arm64"),
    BURROW_S_INIT("linux/386"),       BURROW_S_INIT("linux/amd64"),
    BURROW_S_INIT("linux/arm"),       BURROW_S_INIT("linux/arm64"),
    BURROW_S_INIT("linux/loong64"),   BURROW_S_INIT("linux/mips"),
    BURROW_S_INIT("linux/mips64"),    BURROW_S_INIT("linux/mips64le"),
    BURROW_S_INIT("linux/mipsle"),    BURROW_S_INIT("linux/ppc64"),
    BURROW_S_INIT("linux/ppc64le"),   BURROW_S_INIT("linux/riscv64"),
    BURROW_S_INIT("linux/s390x"),     BURROW_S_INIT("linux/sparc64"),
    BURROW_S_INIT("netbsd/386"),      BURROW_S_INIT("netbsd/amd64"),
    BURROW_S_INIT("netbsd/arm"),      BURROW_S_INIT("netbsd/arm64"),
    BURROW_S_INIT("openbsd/386"),     BURROW_S_INIT("openbsd/amd64"),
    BURROW_S_INIT("openbsd/arm"),     BURROW_S_INIT("openbsd/arm64"),
    BURROW_S_INIT("openbsd/riscv64"), BURROW_S_INIT("solaris/amd64"),
    BURROW_S_INIT("windows/386"),     BURROW_S_INIT("windows/amd64"),
    BURROW_S_INIT("windows/arm64"),
};

static bool gb_cgo_supported(Str goos, Str goarch) {
    for (size_t i = 0; i < GB_LEN(gb_cgo_pairs); i++) {
        Str pair = gb_cgo_pairs[i];
        if (pair.len == goos.len + 1 + goarch.len &&
            memcmp(pair.p, goos.p, (size_t)goos.len) == 0 && pair.p[goos.len] == '/' &&
            memcmp(pair.p + goos.len + 1, goarch.p, (size_t)goarch.len) == 0)
            return true;
    }
    return false;
}

/* The goexperiment.Flags fields in order, lower cased, which are the names
 * GOEXPERIMENT takes and the tags they set. */
static const Str gb_experiments[] = {
    BURROW_S_INIT("fieldtrack"),
    BURROW_S_INIT("preemptibleloops"),
    BURROW_S_INIT("staticlockranking"),
    BURROW_S_INIT("boringcrypto"),
    BURROW_S_INIT("regabiwrappers"),
    BURROW_S_INIT("regabiargs"),
    BURROW_S_INIT("heapminimum512kib"),
    BURROW_S_INIT("arenas"),
    BURROW_S_INIT("cgocheck2"),
    BURROW_S_INIT("loopvar"),
    BURROW_S_INIT("newinliner"),
    BURROW_S_INIT("dwarf5"),
    BURROW_S_INIT("jsonv2"),
    BURROW_S_INIT("greenteagc"),
    BURROW_S_INIT("randomizedheapbase64"),
    BURROW_S_INIT("runtimefreegc"),
    BURROW_S_INIT("sizespecializedmalloc"),
    BURROW_S_INIT("simd"),
    BURROW_S_INIT("runtimesecret"),
    BURROW_S_INIT("mapsplitgroup"),
};

enum {
    GB_EXP_REGABI_WRAPPERS = 4,
    GB_EXP_REGABI_ARGS = 5,
    GB_EXP_DWARF5 = 11,
    GB_EXP_JSONV2 = 12,
    GB_EXP_GREENTEAGC = 13,
    GB_EXP_RANDOMIZED_HEAP_BASE64 = 14,
    GB_EXP_SIZE_SPECIALIZED_MALLOC = 16,
    GB_NEXP = 20
};

static Str gb_env_or(Alloc *a, Str key, Str def) {
    Str s = os_getenv(a, key);
    return s.len == 0 ? def : s;
}

/* buildcfg.ParseGOEXPERIMENT, which fills on[]. An error leaves every
 * experiment off, as buildcfg does when it records the error. */
static void gb_parse_goexperiment(Alloc *a, Str goos, Str goarch, Str goexp,
                                  bool on[GB_NEXP]) {
    bool regabi = str_eq(goarch, S("amd64")) || str_eq(goarch, S("arm64")) ||
                  str_eq(goarch, S("loong64")) || str_eq(goarch, S("ppc64le")) ||
                  str_eq(goarch, S("ppc64")) || str_eq(goarch, S("riscv64")) ||
                  str_eq(goarch, S("s390x"));
    bool dwarf5 = !str_eq(goos, S("darwin")) && !str_eq(goos, S("ios")) &&
                  !str_eq(goos, S("aix"));
    memset(on, 0, GB_NEXP * sizeof(bool));
    on[GB_EXP_REGABI_WRAPPERS] = regabi;
    on[GB_EXP_REGABI_ARGS] = regabi;
    on[GB_EXP_DWARF5] = dwarf5;
    on[GB_EXP_RANDOMIZED_HEAP_BASE64] = true;
    on[GB_EXP_GREENTEAGC] = true;
    on[GB_EXP_JSONV2] = true;
    on[GB_EXP_SIZE_SPECIALIZED_MALLOC] = true;

    if (goexp.len > 0) {
        Slice parts = strings_split(a, goexp, S(","));
        for (Int i = 0; i < parts.len; i++) {
            Str f = gb_at(parts, i);
            if (f.len == 0)
                continue;
            if (str_eq(f, S("none"))) {
                memset(on, 0, GB_NEXP * sizeof(bool));
                continue;
            }
            bool val = true;
            if (strings_has_prefix(f, S("no"))) {
                f = gb_from(f, 2);
                val = false;
            }
            if (str_eq(f, S("regabi"))) {
                on[GB_EXP_REGABI_WRAPPERS] = val;
                on[GB_EXP_REGABI_ARGS] = val;
                continue;
            }
            size_t k = 0;
            while (k < GB_NEXP && !str_eq(gb_experiments[k], f))
                k++;
            if (k == GB_NEXP) {
                /* unknown GOEXPERIMENT */
                memset(on, 0, GB_NEXP * sizeof(bool));
                return;
            }
            on[k] = val;
        }
    }

    on[GB_EXP_REGABI_WRAPPERS] = regabi;
    on[GB_EXP_REGABI_ARGS] = regabi;
}

/* buildcfg.gogoarchTags, the tags for the GOAMD64 style level of goarch. */
static void gb_arch_tags(Alloc *a, Str goos, Str goarch, Slice *list) {
    if (str_eq(goarch, S("386"))) {
        gb_append_str(
            a, list, gb_cat(a, 3, goarch, S("."), gb_env_or(a, S("GO386"), S("sse2"))));
    } else if (str_eq(goarch, S("amd64"))) {
        Str v = gb_env_or(a, S("GOAMD64"), S("v1"));
        Int level = 1;
        if (v.len == 2 && v.p[0] == 'v' && v.p[1] >= '1' && v.p[1] <= '4')
            level = v.p[1] - '0';
        for (Int i = 1; i <= level; i++)
            gb_append_str(a, list, fmt_sprintf_v(a, "%s.v%d", goarch, i));
    } else if (str_eq(goarch, S("arm"))) {
        Str v = gb_env_or(a, S("GOARM"), S("7"));
        if (strings_has_suffix(v, S(",softfloat")))
            v = gb_sub(v, 0, v.len - 10);
        if (strings_has_suffix(v, S(",hardfloat")))
            v = gb_sub(v, 0, v.len - 10);
        Int version = 7;
        if (v.len == 1 && v.p[0] >= '5' && v.p[0] <= '7')
            version = v.p[0] - '0';
        for (Int i = 5; i <= version; i++)
            gb_append_str(a, list, fmt_sprintf_v(a, "%s.%d", goarch, i));
    } else if (str_eq(goarch, S("arm64"))) {
        Str v = gb_env_or(a, S("GOARM64"), S("v8.0"));
        for (;;) {
            if (strings_has_suffix(v, S(",lse"))) {
                v = gb_sub(v, 0, v.len - 4);
                continue;
            }
            if (strings_has_suffix(v, S(",crypto"))) {
                v = gb_sub(v, 0, v.len - 7);
                continue;
            }
            break;
        }
        bool valid =
            v.len == 4 && v.p[0] == 'v' && v.p[2] == '.' && v.p[3] >= '0' &&
            ((v.p[1] == '8' && v.p[3] <= '9') || (v.p[1] == '9' && v.p[3] <= '5'));
        Int major = valid ? v.p[1] - '0' : 8;
        Int minor = valid ? v.p[3] - '0' : 0;
        for (Int i = 0; i <= minor; i++)
            gb_append_str(a, list, fmt_sprintf_v(a, "%s.v%d.%d", goarch, major, i));
        if (major == 9) {
            for (Int i = 0; i <= minor + 5 && i <= 9; i++)
                gb_append_str(a, list,
                              fmt_sprintf_v(a, "%s.v%d.%d", goarch, (Int)8, i));
        }
    } else if (str_eq(goarch, S("mips")) || str_eq(goarch, S("mipsle")) ||
               str_eq(goarch, S("mips64")) || str_eq(goarch, S("mips64le"))) {
        bool is64 = goarch.len >= 6;
        Str v = gb_env_or(a, is64 ? S("GOMIPS64") : S("GOMIPS"), S("hardfloat"));
        if (!str_eq(v, S("hardfloat")) && !str_eq(v, S("softfloat")))
            v = S("hardfloat");
        gb_append_str(a, list, gb_cat(a, 3, goarch, S("."), v));
    } else if (str_eq(goarch, S("ppc64")) || str_eq(goarch, S("ppc64le"))) {
        Str v = gb_env_or(a, S("GOPPC64"), S("power8"));
        Int level = 8;
        if (str_eq(v, S("power9")))
            level = 9;
        else if (str_eq(v, S("power10")))
            level = 10;
        for (Int i = 8; i <= level; i++)
            gb_append_str(a, list, fmt_sprintf_v(a, "%s.power%d", goarch, i));
    } else if (str_eq(goarch, S("riscv64"))) {
        Str v = gb_env_or(a, S("GORISCV64"), S("rva20u64"));
        Int year = 20;
        if (str_eq(v, S("rva22u64")))
            year = 22;
        else if (str_eq(v, S("rva23u64")))
            year = 23;
        gb_append_str(a, list, S("riscv64.rva20u64"));
        if (year >= 22)
            gb_append_str(a, list, S("riscv64.rva22u64"));
        if (year >= 23)
            gb_append_str(a, list, S("riscv64.rva23u64"));
    } else if (str_eq(goarch, S("wasm"))) {
        gb_append_str(a, list, S("wasm.satconv"));
        gb_append_str(a, list, S("wasm.signext"));
    }
    (void)goos;
}

static Str gb_host_os(void) {
    return str_from_cstr(burrow_os_name());
}

static Str gb_host_arch(void) {
    return str_from_cstr(burrow_arch_name());
}

/* buildcfg.ToolTags for the GOOS and GOARCH in the environment. */
static Slice gb_default_tool_tags(Alloc *a) {
    Str goos = gb_env_or(a, S("GOOS"), gb_host_os());
    Str goarch = gb_env_or(a, S("GOARCH"), gb_host_arch());
    bool on[GB_NEXP];
    gb_parse_goexperiment(a, goos, goarch, os_getenv(a, S("GOEXPERIMENT")), on);
    Slice list = slice_nil(TYPE_STRING);
    for (size_t i = 0; i < GB_NEXP; i++) {
        if (on[i])
            gb_append_str(a, &list,
                          gb_cat(a, 2, S("goexperiment."), gb_experiments[i]));
    }
    gb_arch_tags(a, goos, goarch, &list);
    return list;
}

/* The release this is go1.27.1 of. */
enum { GB_GO_VERSION = 27 };

static Slice gb_default_release_tags(Alloc *a) {
    Slice list = slice_nil(TYPE_STRING);
    for (Int i = 1; i <= GB_GO_VERSION; i++)
        gb_append_str(a, &list, gb_cat(a, 2, S("go1."), strconv_itoa(a, i)));
    return list;
}

/* --------------------------------------------------------- context hooks */

static Str gb_join(BuildContext *ctxt, Alloc *a, Slice elem) {
    if (!BURROW_FUNC_IS_NIL(ctxt->join_path))
        return BURROW_CALLF(ctxt->join_path, a, elem);
    return filepath_join(a, elem);
}

static Str gb_join2(BuildContext *ctxt, Alloc *a, Str x, Str y) {
    return gb_join(ctxt, a, gb_strs(a, 2, x, y));
}

static Str gb_join3(BuildContext *ctxt, Alloc *a, Str x, Str y, Str z) {
    return gb_join(ctxt, a, gb_strs(a, 3, x, y, z));
}

static Slice gb_split_path_list(BuildContext *ctxt, Alloc *a, Str s) {
    if (!BURROW_FUNC_IS_NIL(ctxt->split_path_list))
        return BURROW_CALLF(ctxt->split_path_list, a, s);
    return filepath_split_list(a, s);
}

static bool gb_is_abs_path(BuildContext *ctxt, Str path) {
    if (!BURROW_FUNC_IS_NIL(ctxt->is_abs_path))
        return BURROW_CALLF(ctxt->is_abs_path, path);
    return filepath_is_abs(path);
}

static bool gb_is_dir(BuildContext *ctxt, Alloc *a, Str path) {
    if (!BURROW_FUNC_IS_NIL(ctxt->is_dir))
        return BURROW_CALLF(ctxt->is_dir, path);
    Error err = BURROW_NO_ERROR;
    OsFileInfo fi = os_stat(a, path, &err);
    return BURROW_OK(err) && fi.vt->is_dir(fi.data);
}

/* hasSubdir without the hook or symlinks. */
static Str gb_has_subdir_plain(Alloc *a, Str root, Str dir, bool *ok) {
    static const Byte sep_byte = FILEPATH_SEPARATOR;
    Str sep = str_from_bytes(&sep_byte, 1);
    root = filepath_clean(a, root);
    if (!strings_has_suffix(root, sep))
        root = gb_cat(a, 2, root, sep);
    dir = filepath_clean(a, dir);
    bool found = false;
    Str after = strings_cut_prefix(dir, root, &found);
    if (!found) {
        *ok = false;
        return BURROW_STR_EMPTY;
    }
    *ok = true;
    return filepath_to_slash(a, after);
}

static Str gb_eval_symlinks(Alloc *a, Str path) {
    Error err = BURROW_NO_ERROR;
    Str s = filepath_eval_symlinks(a, path, &err);
    return BURROW_OK(err) ? s : BURROW_STR_EMPTY;
}

static Str gb_has_subdir(BuildContext *ctxt, Alloc *a, Str root, Str dir, bool *ok) {
    if (!BURROW_FUNC_IS_NIL(ctxt->has_subdir))
        return BURROW_CALLF(ctxt->has_subdir, a, root, dir, ok);

    /* Try the paths as they are first, then with symlinks resolved, which
     * matters when GOPATH has a link in it. */
    Str rel = gb_has_subdir_plain(a, root, dir, ok);
    if (*ok)
        return rel;
    Str root_sym = gb_eval_symlinks(a, root);
    Str dir_sym = gb_eval_symlinks(a, dir);
    rel = gb_has_subdir_plain(a, root_sym, dir, ok);
    if (*ok)
        return rel;
    rel = gb_has_subdir_plain(a, root, dir_sym, ok);
    if (*ok)
        return rel;
    return gb_has_subdir_plain(a, root_sym, dir_sym, ok);
}

/* readDir: a Slice of FsDirEntry. */
static Slice gb_read_dir(BuildContext *ctxt, Alloc *a, Str path, Error *err) {
    if (!BURROW_FUNC_IS_NIL(ctxt->read_dir)) {
        Error ferr = BURROW_NO_ERROR;
        Slice fis = BURROW_CALLF(ctxt->read_dir, a, path, &ferr);
        if (BURROW_FAILED(ferr)) {
            *err = ferr;
            return slice_nil(TYPE_FS_DIR_ENTRY);
        }
        Slice des = slice_nil(TYPE_FS_DIR_ENTRY);
        for (Int i = 0; i < fis.len; i++) {
            FsDirEntry de = fs_file_info_to_dir_entry(a, BURROW_AT(FsFileInfo, fis, i));
            gb_append(a, &des, TYPE_FS_DIR_ENTRY, &de);
        }
        return des;
    }
    return os_read_dir(a, path, err);
}

/* An OsFile as an io.ReadCloser whose Close also frees it. */
static Int gb_os_read(void *self, Slice p, Error *err) {
    return os_file_read((OsFile *)self, p, err);
}

static Error gb_os_close(void *self) {
    OsFile *f = (OsFile *)self;
    Error err = os_file_close(f);
    os_file_free(f);
    return err;
}

static const IoReadCloserVT gb_os_rc_vt = {
    {NULL, gb_os_read},
    {NULL, gb_os_close},
};

static IoReadCloser gb_open_file(BuildContext *ctxt, Alloc *a, Str path, Error *err) {
    if (!BURROW_FUNC_IS_NIL(ctxt->open_file))
        return BURROW_CALLF(ctxt->open_file, a, path, err);
    Error oerr = BURROW_NO_ERROR;
    OsFile *f = os_open(a, path, &oerr);
    if (BURROW_FAILED(oerr)) {
        *err = oerr;
        return (IoReadCloser){0};
    }
    return (IoReadCloser){&gb_os_rc_vt, f};
}

static Error gb_close(IoReadCloser f) {
    if (f.vt == NULL)
        return BURROW_NO_ERROR;
    return f.vt->closer.close(f.data);
}

static bool gb_is_file(BuildContext *ctxt, Alloc *a, Str path) {
    Error err = BURROW_NO_ERROR;
    IoReadCloser f = gb_open_file(ctxt, a, path, &err);
    if (BURROW_FAILED(err))
        return false;
    (void)gb_close(f);
    return true;
}

/* The usable GOPATH entries: not empty, not GOROOT, and not starting with ~,
 * which is a sign of a path the shell never expanded. */
static Slice gb_gopath(BuildContext *ctxt, Alloc *a) {
    Slice all = slice_nil(TYPE_STRING);
    Slice list = gb_split_path_list(ctxt, a, ctxt->gopath);
    for (Int i = 0; i < list.len; i++) {
        Str p = gb_at(list, i);
        if (p.len == 0 || str_eq(p, ctxt->goroot))
            continue;
        if (strings_has_prefix(p, S("~")))
            continue;
        gb_append_str(a, &all, p);
    }
    return all;
}

Slice build_context_src_dirs(BuildContext *ctxt, Alloc *a) {
    Slice all = slice_nil(TYPE_STRING);
    if (ctxt->goroot.len > 0 && !str_eq(ctxt->compiler, S("gccgo"))) {
        Str dir = gb_join2(ctxt, a, ctxt->goroot, S("src"));
        if (gb_is_dir(ctxt, a, dir))
            gb_append_str(a, &all, dir);
    }
    Slice gopath = gb_gopath(ctxt, a);
    for (Int i = 0; i < gopath.len; i++) {
        Str dir = gb_join2(ctxt, a, gb_at(gopath, i), S("src"));
        if (gb_is_dir(ctxt, a, dir))
            gb_append_str(a, &all, dir);
    }
    return all;
}

/* ---------------------------------------------------------------- Default */

/* runtime.GOROOT(), which with no toolchain behind it is just $GOROOT. */
static Str gb_runtime_goroot(Alloc *a) {
    return os_getenv(a, S("GOROOT"));
}

static Str gb_default_gopath(Alloc *a) {
    Str host = gb_host_os();
    Str env = S("HOME");
    if (str_eq(host, S("windows")))
        env = S("USERPROFILE");
    else if (str_eq(host, S("plan9")))
        env = S("home");
    Str home = os_getenv(a, env);
    if (home.len == 0)
        return BURROW_STR_EMPTY;
    Str def = filepath_join_v(a, 2, home, S("go"));
    if (str_eq(filepath_clean(a, def), filepath_clean(a, gb_runtime_goroot(a))))
        return BURROW_STR_EMPTY;
    return def;
}

BuildContext build_default(Alloc *a) {
    BuildContext c;
    memset(&c, 0, sizeof c);
    c.goarch = gb_env_or(a, S("GOARCH"), gb_host_arch());
    c.goos = gb_env_or(a, S("GOOS"), gb_host_os());
    Str goroot = gb_runtime_goroot(a);
    if (goroot.len > 0)
        c.goroot = filepath_clean(a, goroot);
    c.gopath = gb_env_or(a, S("GOPATH"), gb_default_gopath(a));
    c.compiler = S("gc");
    c.tool_tags = gb_default_tool_tags(a);
    c.release_tags = gb_default_release_tags(a);

    /* cgo must be explicitly enabled for cross compilation builds. */
    Str env = os_getenv(a, S("CGO_ENABLED"));
    c.cgo_enabled = false;
    if (str_eq(env, S("1")))
        c.cgo_enabled = true;
    else if (!str_eq(env, S("0")) && str_eq(gb_host_arch(), c.goarch) &&
             str_eq(gb_host_os(), c.goos))
        c.cgo_enabled = gb_cgo_supported(c.goos, c.goarch);
    return c;
}

Str build_tool_dir(Alloc *a) {
    Str dir = gb_cat(a, 4, S("pkg/tool/"), gb_host_os(), S("_"), gb_host_arch());
    return filepath_join_v(a, 2, gb_runtime_goroot(a), dir);
}

/* ---------------------------------------------------------------- package */

bool build_package_is_command(const BuildPackage *p) {
    return str_eq(p->name, S("main"));
}

bool build_is_local_import(Str path) {
    return str_eq(path, S(".")) || str_eq(path, S("..")) ||
           strings_has_prefix(path, S("./")) || strings_has_prefix(path, S("../"));
}

Str build_arch_char(Str goarch, Error *err) {
    (void)goarch;
    *err = gb_err_arch_char;
    return S("?");
}

static Str gb_name_ext(Str name) {
    Int i = strings_last_index(name, S("."));
    if (i < 0)
        return BURROW_STR_EMPTY;
    return gb_from(name, i);
}

/* fileListForExt: the list in p that files with ext go in, or NULL. */
static Slice *gb_file_list_for_ext(BuildPackage *p, Str ext) {
    if (str_eq(ext, S(".c")))
        return &p->c_files;
    if (str_eq(ext, S(".cc")) || str_eq(ext, S(".cpp")) || str_eq(ext, S(".cxx")))
        return &p->cxx_files;
    if (str_eq(ext, S(".m")))
        return &p->m_files;
    if (str_eq(ext, S(".h")) || str_eq(ext, S(".hh")) || str_eq(ext, S(".hpp")) ||
        str_eq(ext, S(".hxx")))
        return &p->h_files;
    if (str_eq(ext, S(".f")) || str_eq(ext, S(".F")) || str_eq(ext, S(".for")) ||
        str_eq(ext, S(".f90")))
        return &p->f_files;
    if (str_eq(ext, S(".s")) || str_eq(ext, S(".S")) || str_eq(ext, S(".sx")))
        return &p->s_files;
    if (str_eq(ext, S(".swig")))
        return &p->swig_files;
    if (str_eq(ext, S(".swigcxx")))
        return &p->swig_cxx_files;
    if (str_eq(ext, S(".syso")))
        return &p->syso_files;
    return NULL;
}

static bool gb_known_ext(Str ext) {
    BuildPackage dummy;
    memset(&dummy, 0, sizeof dummy);
    return gb_file_list_for_ext(&dummy, ext) != NULL;
}

/* ------------------------------------------------------------ build tags */

static bool gb_match_tag(BuildContext *ctxt, Str name, Map *all_tags) {
    if (all_tags != NULL)
        gb_set_true(all_tags, name);

    if (ctxt->cgo_enabled && str_eq(name, S("cgo")))
        return true;
    if (str_eq(name, ctxt->goos) || str_eq(name, ctxt->goarch) ||
        str_eq(name, ctxt->compiler))
        return true;
    if (str_eq(ctxt->goos, S("android")) && str_eq(name, S("linux")))
        return true;
    if (str_eq(ctxt->goos, S("illumos")) && str_eq(name, S("solaris")))
        return true;
    if (str_eq(ctxt->goos, S("ios")) && str_eq(name, S("darwin")))
        return true;
    if (str_eq(name, S("unix")) && gb_in(gb_unix_os, GB_LEN(gb_unix_os), ctxt->goos))
        return true;
    if (str_eq(name, S("boringcrypto")))
        name = S("goexperiment.boringcrypto"); /* the old name for it */

    return gb_contains(ctxt->build_tags, name) || gb_contains(ctxt->tool_tags, name) ||
           gb_contains(ctxt->release_tags, name);
}

typedef struct GbTagEnv {
    BuildContext *ctxt;
    Map *all_tags;
} GbTagEnv;

static bool gb_tag_ok(void *env, Str tag) {
    GbTagEnv *e = (GbTagEnv *)env;
    return gb_match_tag(e->ctxt, tag, e->all_tags);
}

static bool gb_eval(BuildContext *ctxt, ConstraintExpr x, Map *all_tags) {
    GbTagEnv env = {ctxt, all_tags};
    return constraint_expr_eval(x, BURROW_FN(ConstraintTagFunc, gb_tag_ok, &env));
}

/* matchAuto: text as a //go:build expression if it has operators in it and as
 * a // +build line if not. */
static bool gb_match_auto(BuildContext *ctxt, Alloc *a, Str text, Map *all_tags) {
    if (strings_contains_any(text, S("&|()")))
        text = gb_cat(a, 2, S("//go:build "), text);
    else
        text = gb_cat(a, 2, S("// +build "), text);
    Error err = BURROW_NO_ERROR;
    ConstraintExpr x = constraint_parse(a, text, &err);
    if (BURROW_FAILED(err))
        return false;
    return gb_eval(ctxt, x, all_tags);
}

bool burrow__build_match_auto(BuildContext *ctxt, Alloc *a, Str text, Map *all_tags) {
    return gb_match_auto(ctxt, a, text, all_tags);
}

/* goodOSArchFile: whether the _GOOS, _GOARCH or _GOOS_GOARCH at the end of
 * name, before any _test, matches. */
static bool gb_good_os_arch_file(BuildContext *ctxt, Alloc *a, Str name,
                                 Map *all_tags) {
    Int dot = strings_index_byte(name, '.');
    if (dot >= 0)
        name = gb_sub(name, 0, dot);

    Int i = strings_index(name, S("_"));
    if (i < 0)
        return true;
    name = gb_from(name, i); /* what comes before the first _ does not count */

    Slice l = strings_split(a, name, S("_"));
    Int n = l.len;
    if (n > 0 && str_eq(gb_at(l, n - 1), S("test")))
        n--;
    if (n >= 2 && gb_known_os_has(gb_at(l, n - 2)) &&
        gb_known_arch_has(gb_at(l, n - 1))) {
        if (all_tags != NULL) {
            /* Recorded even when the arch fails to match and the && below never
             * gets to the OS. */
            gb_set_true(all_tags, gb_at(l, n - 2));
        }
        return gb_match_tag(ctxt, gb_at(l, n - 1), all_tags) &&
               gb_match_tag(ctxt, gb_at(l, n - 2), all_tags);
    }
    if (n >= 1 &&
        (gb_known_os_has(gb_at(l, n - 1)) || gb_known_arch_has(gb_at(l, n - 1))))
        return gb_match_tag(ctxt, gb_at(l, n - 1), all_tags);
    return true;
}

bool burrow__build_good_os_arch_file(BuildContext *ctxt, Alloc *a, Str name,
                                     Map *all_tags) {
    return gb_good_os_arch_file(ctxt, a, name, all_tags);
}

static bool gb_is_go_build_comment(Str line) {
    if (!strings_has_prefix(line, S("//go:build")))
        return false;
    line = strings_trim_space(line);
    Str rest = gb_from(line, 10);
    return rest.len == 0 || strings_trim_space(rest).len < rest.len;
}

/* parseFileHeader: content up to the end of its leading run of blank lines and
 * comments, with the //go:build line found in it, if any, and whether it has
 * a //go:binary-only-package line. */
static Str gb_parse_file_header(Str content, Str *go_build, bool *saw_binary_only,
                                Error *err) {
    Int end = 0;
    Str p = content;
    bool ended = false;         /* past the comments, so no more //go:build lines */
    bool in_slash_star = false; /* inside a block comment */
    *go_build = BURROW_STR_EMPTY;
    bool have_go_build = false;
    *saw_binary_only = false;

    while (p.len > 0) {
        Str line = p;
        Int i = strings_index_byte(line, '\n');
        if (i >= 0) {
            line = gb_sub(line, 0, i);
            p = gb_from(p, i + 1);
        } else {
            p = gb_from(p, p.len);
        }
        line = strings_trim_space(line);
        if (line.len == 0 && !ended) { /* a blank line */
            end = content.len - p.len;
            continue;
        }
        if (!strings_has_prefix(line, S("//"))) /* not a line comment */
            ended = true;

        if (!in_slash_star && gb_is_go_build_comment(line)) {
            if (have_go_build) {
                *err = gb_err_multiple_go_build;
                return BURROW_STR_EMPTY;
            }
            *go_build = line;
            have_go_build = true;
        }
        if (!in_slash_star && str_eq(line, S("//go:binary-only-package")))
            *saw_binary_only = true;

        /* Find where the comments end. */
        bool next_line = false;
        while (line.len > 0) {
            if (in_slash_star) {
                Int j = strings_index(line, S("*/"));
                if (j >= 0) {
                    in_slash_star = false;
                    line = strings_trim_space(gb_from(line, j + 2));
                    continue;
                }
                next_line = true;
                break;
            }
            if (strings_has_prefix(line, S("//"))) {
                next_line = true;
                break;
            }
            if (strings_has_prefix(line, S("/*"))) {
                in_slash_star = true;
                line = strings_trim_space(gb_from(line, 2));
                continue;
            }
            break;
        }
        if (!next_line && line.len > 0)
            break;
    }
    return gb_sub(content, 0, end);
}

/* shouldBuild: whether a file with this header is built, which is what its
 * //go:build line says, or with no such line what its // +build lines say
 * between them. */
static bool gb_should_build(BuildContext *ctxt, Alloc *a, Str content, Map *all_tags,
                            bool *binary_only, Error *err) {
    Str go_build = BURROW_STR_EMPTY;
    bool saw_binary_only = false;
    Error herr = BURROW_NO_ERROR;
    content = gb_parse_file_header(content, &go_build, &saw_binary_only, &herr);
    *binary_only = false;
    if (BURROW_FAILED(herr)) {
        *err = herr;
        return false;
    }

    bool should = true;
    if (go_build.len > 0) {
        Error perr = BURROW_NO_ERROR;
        ConstraintExpr x = constraint_parse(a, go_build, &perr);
        if (BURROW_FAILED(perr)) {
            *err = fmt_errorf_v("parsing //go:build line: %v", perr);
            return false;
        }
        should = gb_eval(ctxt, x, all_tags);
    } else {
        Str p = content;
        while (p.len > 0) {
            Str line = p;
            Int i = strings_index_byte(line, '\n');
            if (i >= 0) {
                line = gb_sub(line, 0, i);
                p = gb_from(p, i + 1);
            } else {
                p = gb_from(p, p.len);
            }
            line = strings_trim_space(line);
            if (!strings_has_prefix(line, S("//")) ||
                !strings_contains(line, S("+build")))
                continue;
            if (!constraint_is_plus_build(line))
                continue;
            Error perr = BURROW_NO_ERROR;
            ConstraintExpr x = constraint_parse(a, line, &perr);
            if (BURROW_OK(perr) && !gb_eval(ctxt, x, all_tags))
                should = false;
        }
    }
    *binary_only = saw_binary_only;
    return should;
}

bool burrow__build_should_build(BuildContext *ctxt, Alloc *a, Str content,
                                Map *all_tags, bool *binary_only, Error *err) {
    return gb_should_build(ctxt, a, content, all_tags, binary_only, err);
}

/* ------------------------------------------------------------------- cgo */

static const Str gb_safe_string = BURROW_S_INIT(
    "+-.,/0123456789=ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz:$@%! ~^");

/* safeCgoName: whether s is safe to hand to the shell, with only safe ASCII
 * characters in it. Other UTF-8 is let through. */
static bool gb_safe_cgo_name(Str s) {
    if (s.len == 0)
        return false;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c < UTF8_RUNE_SELF && strings_index_byte(gb_safe_string, c) < 0)
            return false;
    }
    return true;
}

/* expandSrcDir: str with ${SRCDIR} replaced by srcdir, and whether the result
 * is safe. */
static Str gb_expand_src_dir(Alloc *a, Str str, Str srcdir, bool *ok) {
    srcdir = filepath_to_slash(a, srcdir);
    Slice chunks = strings_split(a, str, S("${SRCDIR}"));
    if (chunks.len < 2) {
        *ok = gb_safe_cgo_name(str);
        return str;
    }
    bool good = true;
    for (Int i = 0; i < chunks.len; i++) {
        Str chunk = gb_at(chunks, i);
        good = good && (chunk.len == 0 || gb_safe_cgo_name(chunk));
    }
    good = good && (srcdir.len == 0 || gb_safe_cgo_name(srcdir));
    Str res = strings_join(a, chunks, srcdir);
    *ok = good && res.len != 0;
    return res;
}

Str burrow__build_expand_src_dir(Alloc *a, Str str, Str srcdir, bool *ok) {
    return gb_expand_src_dir(a, str, srcdir, ok);
}

/* makePathsAbsolute: the paths after -I and -L made absolute against src_dir,
 * whether joined to the flag or in the next argument. */
static void gb_make_paths_absolute(Alloc *a, Slice args, Str src_dir) {
    bool next_path = false;
    for (Int i = 0; i < args.len; i++) {
        Str arg = gb_at(args, i);
        if (next_path) {
            if (!filepath_is_abs(arg))
                BURROW_AT(Str, args, i) = filepath_join_v(a, 2, src_dir, arg);
            next_path = false;
        } else if (strings_has_prefix(arg, S("-I")) ||
                   strings_has_prefix(arg, S("-L"))) {
            if (arg.len == 2) {
                next_path = true;
            } else if (!filepath_is_abs(gb_from(arg, 2))) {
                BURROW_AT(Str, args, i) =
                    gb_cat(a, 2, gb_sub(arg, 0, 2),
                           filepath_join_v(a, 2, src_dir, gb_from(arg, 2)));
            }
        }
    }
}

/* splitQuoted: s split at spaces, where single or double quotes keep spaces in
 * and a backslash escapes the next character. */
static Slice gb_split_quoted(Alloc *a, Str s, Error *err) {
    Slice args = slice_nil(TYPE_STRING);
    /* Each byte of s is at most one rune, and a bad byte becomes U+FFFD. */
    Int cap = s.len * UTF8_UTF_MAX + 1;
    Byte *arg = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
    if (arg == NULL)
        gb_oom();
    bool escaped = false;
    bool quoted = false;
    Rune quote = 0;
    Int n = 0;
    Int at = 0;
    while (at < s.len) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(gb_from(s, at), &size);
        at += size;
        if (escaped) {
            escaped = false;
        } else if (r == '\\') {
            escaped = true;
            continue;
        } else if (quote != 0) {
            if (r == quote) {
                quote = 0;
                continue;
            }
        } else if (r == '"' || r == '\'') {
            quoted = true;
            quote = r;
            continue;
        } else if (unicode_is_space(r)) {
            if (quoted || n > 0) {
                quoted = false;
                gb_append_str(a, &args, str_clone(a, str_from_bytes(arg, n)));
                n = 0;
            }
            continue;
        }
        n += utf8_encode_rune(slice_from(arg + n, cap - n, cap - n, TYPE_BYTE), r);
    }
    if (quoted || n > 0)
        gb_append_str(a, &args, str_clone(a, str_from_bytes(arg, n)));
    if (quote != 0)
        *err = gb_err_unclosed_quote;
    else if (escaped)
        *err = gb_err_unfinished_escape;
    return args;
}

/* saveCgo: the #cgo lines of the comment above import "C" saved into di. */
static Error gb_save_cgo(BuildContext *ctxt, Alloc *a, Str filename, BuildPackage *di,
                         AstCommentGroup *cg) {
    Str text = ast_comment_group_text(cg, a);
    Slice lines = strings_split(a, text, S("\n"));
    for (Int li = 0; li < lines.len; li++) {
        Str orig = gb_at(lines, li);
        Str line = strings_trim_space(orig);
        if (line.len < 5 || !strings_has_prefix(line, S("#cgo")) ||
            (line.p[4] != ' ' && line.p[4] != '\t'))
            continue;

        /* #cgo nocallback and #cgo noescape are for cgo itself. */
        Slice fields = strings_fields(a, line);
        if (fields.len == 3 && (str_eq(gb_at(fields, 1), S("nocallback")) ||
                                str_eq(gb_at(fields, 1), S("noescape"))))
            continue;

        /* #cgo [GOOS/GOARCH...] LDFLAGS: stuff */
        Str argstr = BURROW_STR_EMPTY;
        bool ok = false;
        line = strings_cut(strings_trim_space(gb_from(line, 4)), S(":"), &argstr, &ok);
        if (!ok)
            return fmt_errorf_v("%s: invalid #cgo line: %s", filename, orig);

        Slice f = strings_fields(a, line);
        if (f.len < 1)
            return fmt_errorf_v("%s: invalid #cgo line: %s", filename, orig);

        Str verb = gb_at(f, f.len - 1);
        if (f.len > 1) {
            bool any = false;
            for (Int i = 0; i < f.len - 1; i++) {
                if (gb_match_auto(ctxt, a, gb_at(f, i), NULL)) {
                    any = true;
                    break;
                }
            }
            if (!any)
                continue;
        }

        Error serr = BURROW_NO_ERROR;
        Slice args = gb_split_quoted(a, argstr, &serr);
        if (BURROW_FAILED(serr))
            return fmt_errorf_v("%s: invalid #cgo line: %s", filename, orig);
        for (Int i = 0; i < args.len; i++) {
            bool safe = false;
            Str arg = gb_expand_src_dir(a, gb_at(args, i), di->dir, &safe);
            if (!safe)
                return fmt_errorf_v("%s: malformed #cgo argument: %s", filename, arg);
            BURROW_AT(Str, args, i) = arg;
        }

        if (str_eq(verb, S("CFLAGS")) || str_eq(verb, S("CPPFLAGS")) ||
            str_eq(verb, S("CXXFLAGS")) || str_eq(verb, S("FFLAGS")) ||
            str_eq(verb, S("LDFLAGS")))
            gb_make_paths_absolute(a, args, di->dir);

        Slice *dst = NULL;
        if (str_eq(verb, S("CFLAGS")))
            dst = &di->cgo_cflags;
        else if (str_eq(verb, S("CPPFLAGS")))
            dst = &di->cgo_cppflags;
        else if (str_eq(verb, S("CXXFLAGS")))
            dst = &di->cgo_cxxflags;
        else if (str_eq(verb, S("FFLAGS")))
            dst = &di->cgo_fflags;
        else if (str_eq(verb, S("LDFLAGS")))
            dst = &di->cgo_ldflags;
        else if (str_eq(verb, S("pkg-config")))
            dst = &di->cgo_pkg_config;
        else
            return fmt_errorf_v("%s: invalid #cgo verb: %s", filename, orig);
        gb_append_slice(a, dst, args);
    }
    return BURROW_NO_ERROR;
}

/* -------------------------------------------------------- import comments */

static Str gb_skip_space_or_comment(Str data) {
    while (data.len > 0) {
        Byte c = data.p[0];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            data = gb_from(data, 1);
            continue;
        }
        if (c == '/') {
            if (strings_has_prefix(data, S("//"))) {
                Int i = strings_index_byte(data, '\n');
                if (i < 0)
                    return BURROW_STR_EMPTY;
                data = gb_from(data, i + 1);
                continue;
            }
            if (strings_has_prefix(data, S("/*"))) {
                data = gb_from(data, 2);
                Int i = strings_index(data, S("*/"));
                if (i < 0)
                    return BURROW_STR_EMPTY;
                data = gb_from(data, i + 2);
                continue;
            }
        }
        break;
    }
    return data;
}

/* parseWord: the word at the start of data, after space and comments, and
 * the rest after it. No word gives two empty Strs. */
static Str gb_parse_word(Str data, Str *rest) {
    data = gb_skip_space_or_comment(data);
    Str r = data;
    for (;;) {
        Int size = 0;
        Rune c = utf8_decode_rune_in_string(r, &size);
        if (r.len > 0 && (unicode_is_letter(c) || ('0' <= c && c <= '9') || c == '_')) {
            r = gb_from(r, size);
            continue;
        }
        break;
    }
    Str word = gb_sub(data, 0, data.len - r.len);
    if (word.len == 0) {
        *rest = BURROW_STR_EMPTY;
        return BURROW_STR_EMPTY;
    }
    *rest = r;
    return word;
}

/* findImportComment: the quoted path of an import comment on the package
 * clause in data, as in package foo // import "x", and the line it is on, or
 * 0 for none. */
static Str gb_find_import_comment(Str data, Int *line) {
    *line = 0;
    Str word = gb_parse_word(data, &data);
    if (!str_eq(word, S("package")))
        return BURROW_STR_EMPTY;

    gb_parse_word(data, &data); /* the name */

    /* The comment has to be on the same line, so no newlines are skipped. */
    while (data.len > 0 && (data.p[0] == ' ' || data.p[0] == '\t' || data.p[0] == '\r'))
        data = gb_from(data, 1);

    Str comment = BURROW_STR_EMPTY;
    if (strings_has_prefix(data, S("//"))) {
        Str rest = gb_from(data, 2);
        Int i = strings_index_byte(rest, '\n');
        comment = i < 0 ? rest : gb_sub(rest, 0, i);
    } else if (strings_has_prefix(data, S("/*"))) {
        Str rest = gb_from(data, 2);
        Int i = strings_index(rest, S("*/"));
        if (i < 0)
            return BURROW_STR_EMPTY;
        comment = gb_sub(rest, 0, i);
        if (strings_index_byte(comment, '\n') >= 0)
            return BURROW_STR_EMPTY;
    }
    comment = strings_trim_space(comment);

    Str arg = BURROW_STR_EMPTY;
    word = gb_parse_word(comment, &arg);
    if (!str_eq(word, S("import")))
        return BURROW_STR_EMPTY;

    /* The newlines in data before arg. */
    const Byte *end = arg.p != NULL ? arg.p : word.p + word.len;
    Int upto = (Int)(end - data.p);
    Int n = 0;
    for (Int i = 0; i < upto; i++) {
        if (data.p[i] == '\n')
            n++;
    }
    *line = 1 + n;
    return strings_trim_space(arg);
}

/* ------------------------------------------------------------- matchFile */

static void gb_close_quiet(IoReadCloser f) {
    Error err = gb_close(f);
    (void)err;
}

/* matchFile: whether name in dir is part of the package, which gives the info
 * read from it, or NULL when it is not. A file that cannot be read gives an
 * error, with the info when the file got that far. binary_only and fset may be
 * NULL. */
static burrow__BuildFileInfo *gb_match_file(BuildContext *ctxt, Alloc *a, Str dir,
                                            Str name, Map *all_tags, bool *binary_only,
                                            TokenFileSet *fset, Error *err) {
    if (strings_has_prefix(name, S("_")) || strings_has_prefix(name, S(".")))
        return NULL;

    Int i = strings_last_index(name, S("."));
    if (i < 0)
        i = name.len;
    Str ext = gb_from(name, i);

    if (!str_eq(ext, S(".go")) && !gb_known_ext(ext))
        return NULL;

    if (!gb_good_os_arch_file(ctxt, a, name, all_tags) && !ctxt->use_all_files)
        return NULL;

    burrow__BuildFileInfo *info = GB_NEW(a, burrow__BuildFileInfo);
    info->name = gb_join2(ctxt, a, dir, name);
    info->fset = fset;
    info->parse_err = BURROW_NO_ERROR;
    if (str_eq(ext, S(".syso"))) {
        /* binary, no reading */
        return info;
    }

    Error oerr = BURROW_NO_ERROR;
    IoReadCloser f = gb_open_file(ctxt, a, info->name, &oerr);
    if (BURROW_FAILED(oerr)) {
        *err = oerr;
        return NULL;
    }

    Error rerr = BURROW_NO_ERROR;
    if (strings_has_suffix(name, S(".go"))) {
        rerr = burrow__build_read_go_info(a, io_read_closer_as_io_reader(f), info);
        if (strings_has_suffix(name, S("_test.go")))
            binary_only = NULL; /* //go:binary-only-package means nothing in a test */
    } else {
        binary_only = NULL; /* nor in a file that is not Go */
        info->header =
            burrow__build_read_comments(a, io_read_closer_as_io_reader(f), &rerr);
    }
    gb_close_quiet(f);
    if (BURROW_FAILED(rerr)) {
        *err = fmt_errorf_v("read %s: %v", info->name, rerr);
        return info;
    }

    bool saw_binary_only = false;
    Error serr = BURROW_NO_ERROR;
    bool ok = gb_should_build(ctxt, a, info->header, all_tags, &saw_binary_only, &serr);
    if (BURROW_FAILED(serr)) {
        *err = fmt_errorf_v("%s: %v", name, serr);
        return NULL;
    }
    if (!ok && !ctxt->use_all_files)
        return NULL;

    if (binary_only != NULL && saw_binary_only)
        *binary_only = true;

    return info;
}

bool build_context_match_file(BuildContext *ctxt, Alloc *a, Str dir, Str name,
                              Error *err) {
    Error merr = BURROW_NO_ERROR;
    burrow__BuildFileInfo *info =
        gb_match_file(ctxt, a, dir, name, NULL, NULL, NULL, &merr);
    *err = merr;
    return info != NULL;
}

/* -------------------------------------------------------------- importGo */

/* importGo: the package found by asking the go command, for module mode. It
 * says errNoModules when module mode does not apply. */
static Error gb_import_go(BuildContext *ctxt, Alloc *a, BuildPackage *p, Str path,
                          Str src_dir, BuildImportMode mode) {
    /* Module mode needs the go command's view of things, so a context with
     * anything changed that the go command would not know about stays out of
     * it. */
    if ((mode & BUILD_ALLOW_BINARY) != 0 || (mode & BUILD_IGNORE_VENDOR) != 0 ||
        !BURROW_FUNC_IS_NIL(ctxt->join_path) ||
        !BURROW_FUNC_IS_NIL(ctxt->split_path_list) ||
        !BURROW_FUNC_IS_NIL(ctxt->is_abs_path) || !BURROW_FUNC_IS_NIL(ctxt->is_dir) ||
        !BURROW_FUNC_IS_NIL(ctxt->has_subdir) || !BURROW_FUNC_IS_NIL(ctxt->read_dir) ||
        !BURROW_FUNC_IS_NIL(ctxt->open_file) ||
        !gb_slices_equal(ctxt->tool_tags, gb_default_tool_tags(a)) ||
        !gb_slices_equal(ctxt->release_tags, gb_default_release_tags(a)) ||
        ctxt->use_all_files)
        return gb_err_no_modules;

    /* Without a GOROOT there is no go command to ask. */
    if (ctxt->goroot.len == 0)
        return gb_err_no_modules;

    Str go111module = os_getenv(a, S("GO111MODULE"));
    if (str_eq(go111module, S("off")))
        return gb_err_no_modules;

    /* Packages under GOROOT/src are found the old way. */
    if (src_dir.len > 0) {
        Str abs_src_dir = BURROW_STR_EMPTY;
        if (filepath_is_abs(src_dir)) {
            abs_src_dir = src_dir;
        } else if (ctxt->dir.len > 0) {
            return fmt_errorf_v(
                "go/build: Dir is non-empty, so relative srcDir is not allowed: %v",
                src_dir);
        } else {
            Error aerr = BURROW_NO_ERROR;
            abs_src_dir = filepath_abs(a, src_dir, &aerr);
            if (BURROW_FAILED(aerr))
                return gb_err_no_modules;
        }
        bool ok = false;
        gb_has_subdir(ctxt, a, filepath_join_v(a, 2, ctxt->goroot, S("src")),
                      abs_src_dir, &ok);
        if (ok)
            return gb_err_no_modules;
    }

    /* So is a standard package. */
    Str stddir = gb_join3(ctxt, a, ctxt->goroot, S("src"), path);
    if (gb_is_dir(ctxt, a, stddir))
        return gb_err_no_modules;

    /* With GO111MODULE=auto, module mode only inside a module. */
    if (str_eq(go111module, S("auto"))) {
        Str parent = BURROW_STR_EMPTY;
        Error perr = BURROW_NO_ERROR;
        if (ctxt->dir.len == 0) {
            parent = os_getwd(a, &perr);
            if (BURROW_FAILED(perr))
                return gb_err_no_modules;
        } else {
            parent = filepath_abs(a, ctxt->dir, &perr);
            if (BURROW_FAILED(perr))
                return perr;
        }
        for (;;) {
            Error oerr = BURROW_NO_ERROR;
            IoReadCloser f =
                gb_open_file(ctxt, a, gb_join2(ctxt, a, parent, S("go.mod")), &oerr);
            if (BURROW_OK(oerr)) {
                Byte buf[100];
                Error rerr = BURROW_NO_ERROR;
                IoReader r = io_read_closer_as_io_reader(f);
                BURROW_CALL(r, read, slice_from(buf, 100, 100, TYPE_BYTE), &rerr);
                gb_close_quiet(f);
                if (BURROW_OK(rerr) || gb_is_eof(rerr))
                    break;
            }
            Str d = filepath_dir(a, parent);
            if (d.len >= parent.len)
                return gb_err_no_modules; /* the top, and no go.mod */
            parent = d;
        }
    }

    Str go_cmd = filepath_join_v(a, 3, ctxt->goroot, S("bin"), S("go"));
    Slice args =
        gb_strs(a, 8, S("list"), S("-e"), gb_cat(a, 2, S("-compiler="), ctxt->compiler),
                gb_cat(a, 2, S("-tags="), strings_join(a, ctxt->build_tags, S(","))),
                gb_cat(a, 2, S("-installsuffix="), ctxt->install_suffix),
                S("-f={{.Dir}}\n{{.ImportPath}}\n{{.Root}}\n{{.Goroot}}\n{{if "
                  ".Error}}{{.Error}}{{end}}\n"),
                S("--"), path);
    ExecCmd *cmd = exec_command(a, go_cmd, args);
    if (cmd == NULL)
        gb_oom();

    if (ctxt->dir.len > 0)
        cmd->dir = ctxt->dir;

    StringsBuilder stdout_b = STRINGS_BUILDER(a);
    StringsBuilder stderr_b = STRINGS_BUILDER(a);
    cmd->stdout_ = strings_builder_as_io_writer(&stdout_b);
    cmd->stderr_ = strings_builder_as_io_writer(&stderr_b);

    Str cgo = ctxt->cgo_enabled ? S("1") : S("0");
    Slice env = exec_cmd_environ(cmd, a);
    gb_append_str(a, &env, gb_cat(a, 2, S("GOOS="), ctxt->goos));
    gb_append_str(a, &env, gb_cat(a, 2, S("GOARCH="), ctxt->goarch));
    gb_append_str(a, &env, gb_cat(a, 2, S("GOROOT="), ctxt->goroot));
    gb_append_str(a, &env, gb_cat(a, 2, S("GOPATH="), ctxt->gopath));
    gb_append_str(a, &env, gb_cat(a, 2, S("CGO_ENABLED="), cgo));
    cmd->env = env;

    Error rerr = exec_cmd_run(cmd);
    if (BURROW_FAILED(rerr)) {
        Str errout = strings_builder_string(&stderr_b);
        Error out = fmt_errorf_v("go/build: go list %s: %v\n%s\n", path, rerr, errout);
        exec_cmd_free(cmd);
        return out;
    }
    exec_cmd_free(cmd);

    Str output = strings_builder_string(&stdout_b);
    Slice f = strings_split_n(a, output, S("\n"), 5);
    if (f.len != 5)
        return fmt_errorf_v("go/build: importGo %s: unexpected output:\n%s\n", path,
                            output);
    Str dir = gb_at(f, 0);
    Str err_str = strings_trim_space(gb_at(f, 4));
    if (err_str.len > 0 && dir.len == 0)
        return errors_new(error_allocator(), err_str);

    /* With a dir and an error, the error is for whatever is wrong in the
     * package, which the caller finds again when it reads the directory. */
    p->dir = dir;
    p->import_path = gb_at(f, 1);
    p->root = gb_at(f, 2);
    p->goroot = str_eq(gb_at(f, 3), S("true"));
    return BURROW_NO_ERROR;
}

Error burrow__build_import_go(BuildContext *ctxt, Alloc *a, BuildPackage *p, Str path,
                              Str src_dir, BuildImportMode mode) {
    return gb_import_go(ctxt, a, p, path, src_dir, mode);
}

static bool gb_has_go_files(BuildContext *ctxt, Alloc *a, Str dir) {
    Error err = BURROW_NO_ERROR;
    Slice ents = gb_read_dir(ctxt, a, dir, &err);
    for (Int i = 0; i < ents.len; i++) {
        FsDirEntry ent = BURROW_AT(FsDirEntry, ents, i);
        if (!ent.vt->is_dir(ent.data) &&
            strings_has_suffix(ent.vt->name(ent.data), S(".go")))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ gccgo */

static bool gb_stat_is_dir(Alloc *a, Str path, bool want_dir) {
    Error err = BURROW_NO_ERROR;
    OsFileInfo fi = os_stat(a, path, &err);
    return BURROW_OK(err) && fi.vt->is_dir(fi.data) == want_dir;
}

static Str gb_command_output(Alloc *a, Str bin, Str arg, bool *ok) {
    ExecCmd *cmd = exec_command_v(a, bin, 1, arg);
    if (cmd == NULL)
        gb_oom();
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_output(cmd, a, &err);
    exec_cmd_free(cmd);
    *ok = BURROW_OK(err);
    return out.len == 0 ? BURROW_STR_EMPTY
                        : str_from_bytes((const Byte *)out.p, out.len);
}

/* The directories gccgo keeps its .gox files in, or a nil Slice when gccgo is
 * not there to say. Go asks once, and this asks each time. */
static Slice gb_gccgo_dirs(Alloc *a) {
    Slice dirs_out = {0};
    Str gccgo = gb_env_or(a, S("GCCGO"), S("gccgo"));
    Error err = BURROW_NO_ERROR;
    Str bin = exec_look_path(a, gccgo, &err);
    if (BURROW_FAILED(err))
        return dirs_out;

    bool ok = false;
    Str all = gb_command_output(a, bin, S("-print-search-dirs"), &ok);
    if (!ok)
        return dirs_out;
    Str version = strings_trim_space(gb_command_output(a, bin, S("-dumpversion"), &ok));
    if (!ok)
        return dirs_out;
    Str machine = strings_trim_space(gb_command_output(a, bin, S("-dumpmachine"), &ok));
    if (!ok)
        return dirs_out;

    Slice entries = strings_split(a, all, S("\n"));
    Slice dirs = {0};
    for (Int i = 0; i < entries.len; i++) {
        bool found = false;
        Str after = strings_cut_prefix(gb_at(entries, i), S("libraries: ="), &found);
        if (found) {
            dirs = filepath_split_list(a, after);
            break;
        }
    }
    if (dirs.len == 0)
        return dirs_out;

    Slice last = {0};
    for (Int i = 0; i < dirs.len; i++) {
        Str dir = gb_at(dirs, i);
        Str go_dir = filepath_join_v(a, 3, dir, S("go"), version);
        if (gb_stat_is_dir(a, go_dir, true)) {
            gb_append_str(a, &dirs_out, go_dir);
            go_dir = filepath_join_v(a, 2, go_dir, machine);
            if (gb_stat_is_dir(a, go_dir, true))
                gb_append_str(a, &dirs_out, go_dir);
        }
        if (gb_stat_is_dir(a, dir, true))
            gb_append_str(a, &last, dir);
    }
    gb_append_slice(a, &dirs_out, last);
    return dirs_out;
}

/* goroot.IsStandardPackage for gccgo. */
static bool gb_gccgo_is_standard(Alloc *a, Str path) {
    Int i = strings_index(path, S("/"));
    if (i < 0)
        i = path.len;
    if (strings_contains(gb_sub(path, 0, i), S(".")))
        return false;
    if (str_eq(path, S("unsafe")))
        return true; /* unsafe has no .gox */

    Slice dirs = gb_gccgo_dirs(a);
    if (dirs.elem == NULL)
        return true; /* no gccgo to ask, so call it standard */

    for (Int k = 0; k < dirs.len; k++) {
        Str full = gb_cat(a, 2, filepath_join_v(a, 2, gb_at(dirs, k), path), S(".gox"));
        if (gb_stat_is_dir(a, full, false))
            return true;
    }
    return false;
}

/* ----------------------------------------------------------------- Import */

/* GODEBUG=installgoroot=all, the last setting of it winning. */
static bool gb_install_goroot_all(Alloc *a) {
    Str godebug = os_getenv(a, S("GODEBUG"));
    if (godebug.len == 0)
        return false;
    bool all = false;
    Slice parts = strings_split(a, godebug, S(","));
    for (Int i = 0; i < parts.len; i++) {
        Str kv = gb_at(parts, i);
        bool found = false;
        Str v = strings_cut_prefix(kv, S("installgoroot="), &found);
        if (found)
            all = str_eq(v, S("all"));
    }
    return all;
}

/* What an import keeps track of along the way. */
typedef struct GbImport {
    BuildContext *ctxt;
    Alloc *a;
    BuildPackage *p;
    Str path;
    Str src_dir;
    Str pkgtargetroot;
    Str pkga;
    Slice tried_vendor;
    Error bad_err;
    Map *bad_files;
} GbImport;

/* setPkga: where the package's .a file goes, for its import path. */
static void gb_set_pkga(GbImport *im) {
    BuildContext *ctxt = im->ctxt;
    Alloc *a = im->a;
    if (str_eq(ctxt->compiler, S("gccgo"))) {
        Str elem = BURROW_STR_EMPTY;
        Str dir = path_split(im->p->import_path, &elem);
        im->pkga =
            gb_cat(a, 6, im->pkgtargetroot, S("/"), dir, S("lib"), elem, S(".a"));
    } else if (str_eq(ctxt->compiler, S("gc"))) {
        im->pkga = gb_cat(a, 4, im->pkgtargetroot, S("/"), im->p->import_path, S(".a"));
    }
}

static bool gb_in_testdata(Str sub) {
    return strings_contains(sub, S("/testdata/")) ||
           strings_has_suffix(sub, S("/testdata")) ||
           strings_has_prefix(sub, S("testdata/")) || str_eq(sub, S("testdata"));
}

/* searchVendor: the vendor directories from src_dir up to root, the first
 * with path in it winning. */
static bool gb_search_vendor(GbImport *im, Str root, bool is_goroot) {
    BuildContext *ctxt = im->ctxt;
    Alloc *a = im->a;
    bool ok = false;
    Str sub = gb_has_subdir(ctxt, a, root, im->src_dir, &ok);
    if (!ok || !strings_has_prefix(sub, S("src/")) ||
        strings_contains(sub, S("/testdata/")))
        return false;
    for (;;) {
        Str vendor = gb_join3(ctxt, a, root, sub, S("vendor"));
        if (gb_is_dir(ctxt, a, vendor)) {
            Str dir = gb_join2(ctxt, a, vendor, im->path);
            if (gb_is_dir(ctxt, a, dir) && gb_has_go_files(ctxt, a, dir)) {
                im->p->dir = dir;
                im->p->import_path = strings_trim_prefix(
                    path_join_v(a, 3, sub, S("vendor"), im->path), S("src/"));
                im->p->goroot = is_goroot;
                im->p->root = root;
                gb_set_pkga(im);
                return true;
            }
            gb_append_str(a, &im->tried_vendor, dir);
        }
        Int i = strings_last_index(sub, S("/"));
        if (i < 0)
            break;
        sub = gb_sub(sub, 0, i);
    }
    return false;
}

static void gb_bad_go_file(GbImport *im, Str name, Error err) {
    if (BURROW_OK(im->bad_err))
        im->bad_err = err;
    bool *seen = (bool *)map_get(im->bad_files, &name);
    if (seen == NULL || !*seen) {
        gb_append_str(im->a, &im->p->invalid_go_files, name);
        gb_set_true(im->bad_files, name);
    }
}

static Map *gb_pos_map(Alloc *a) {
    return gb_map(a, TYPE_STRING, &gb_positions_desc);
}

static void gb_pos_add(Alloc *a, Map *m, Str key, TokenPosition pos) {
    Slice *list = (Slice *)map_get(m, &key);
    Slice cur = list != NULL ? *list : slice_nil(&gb_position_desc);
    gb_append(a, &cur, &gb_position_desc, &pos);
    if (!map_set(m, &key, &cur))
        gb_oom();
}

BuildPackage *build_context_import(BuildContext *ctxt, Alloc *a, Str path, Str src_dir,
                                   BuildImportMode mode, Error *err) {
    *err = BURROW_NO_ERROR;
    BuildPackage *p = GB_NEW(a, BuildPackage);
    p->import_path = path;
    if (path.len == 0) {
        *err = fmt_errorf_v("import %q: invalid import path", path);
        return p;
    }

    GbImport im;
    memset(&im, 0, sizeof im);
    im.ctxt = ctxt;
    im.a = a;
    im.p = p;
    im.path = path;
    im.src_dir = src_dir;
    im.bad_err = BURROW_NO_ERROR;

    Error pkgerr = BURROW_NO_ERROR;
    Str suffix = BURROW_STR_EMPTY;
    if (ctxt->install_suffix.len > 0)
        suffix = gb_cat(a, 2, S("_"), ctxt->install_suffix);
    if (str_eq(ctxt->compiler, S("gccgo")))
        im.pkgtargetroot =
            gb_cat(a, 5, S("pkg/gccgo_"), ctxt->goos, S("_"), ctxt->goarch, suffix);
    else if (str_eq(ctxt->compiler, S("gc")))
        im.pkgtargetroot =
            gb_cat(a, 5, S("pkg/"), ctxt->goos, S("_"), ctxt->goarch, suffix);
    else
        pkgerr = fmt_errorf_v("import %q: unknown compiler %q", path, ctxt->compiler);
    gb_set_pkga(&im);

    bool binary_only = false;
    if (build_is_local_import(path)) {
        im.pkga = BURROW_STR_EMPTY; /* a local import has no installed path */
        if (src_dir.len == 0) {
            *err =
                fmt_errorf_v("import %q: import relative to unknown directory", path);
            return p;
        }
        if (!gb_is_abs_path(ctxt, path))
            p->dir = gb_join2(ctxt, a, src_dir, path);
        /* A local path that is in GOROOT or GOPATH gets the import path it would
         * have there. */
        if (ctxt->goroot.len > 0) {
            Str root = gb_join2(ctxt, a, ctxt->goroot, S("src"));
            bool ok = false;
            Str sub = gb_has_subdir(ctxt, a, root, p->dir, &ok);
            if (ok && !gb_in_testdata(sub)) {
                p->goroot = true;
                p->import_path = sub;
                p->root = ctxt->goroot;
                gb_set_pkga(&im);
                goto found;
            }
        }
        Slice all = gb_gopath(ctxt, a);
        for (Int i = 0; i < all.len; i++) {
            Str root = gb_at(all, i);
            Str rootsrc = gb_join2(ctxt, a, root, S("src"));
            bool ok = false;
            Str sub = gb_has_subdir(ctxt, a, rootsrc, p->dir, &ok);
            if (ok && !gb_in_testdata(sub)) {
                /* A package earlier on the search path hides this one, which is
                 * then left with no import path. */
                if (ctxt->goroot.len > 0 && !str_eq(ctxt->compiler, S("gccgo"))) {
                    Str dir = gb_join3(ctxt, a, ctxt->goroot, S("src"), sub);
                    if (gb_is_dir(ctxt, a, dir)) {
                        p->conflict_dir = dir;
                        goto found;
                    }
                }
                for (Int j = 0; j < i; j++) {
                    Str dir = gb_join3(ctxt, a, gb_at(all, j), S("src"), sub);
                    if (gb_is_dir(ctxt, a, dir)) {
                        p->conflict_dir = dir;
                        goto found;
                    }
                }

                p->import_path = sub;
                p->root = root;
                gb_set_pkga(&im);
                goto found;
            }
        }
        /* Not in any tree: no import path. */
    } else {
        if (strings_has_prefix(path, S("/"))) {
            *err = fmt_errorf_v("import %q: cannot import absolute path", path);
            return p;
        }

        Error gerr = gb_import_go(ctxt, a, p, path, src_dir, mode);
        if (BURROW_OK(gerr))
            goto found;
        if (!gb_is_no_modules(gerr)) {
            *err = gerr;
            return p;
        }

        Slice gopath = gb_gopath(ctxt, a);
        Str tried_goroot = BURROW_STR_EMPTY;
        Slice tried_gopath = slice_nil(TYPE_STRING);

        /* Vendor directories first. */
        if ((mode & BUILD_IGNORE_VENDOR) == 0 && src_dir.len > 0) {
            if (!str_eq(ctxt->compiler, S("gccgo")) && ctxt->goroot.len > 0 &&
                gb_search_vendor(&im, ctxt->goroot, true))
                goto found;
            for (Int i = 0; i < gopath.len; i++) {
                if (gb_search_vendor(&im, gb_at(gopath, i), false))
                    goto found;
            }
        }

        /* Then GOROOT, unless this is a vendor path from outside GOROOT, which
         * looks in GOPATH first. */
        if (ctxt->goroot.len > 0) {
            bool goroot_first =
                src_dir.len == 0 || !strings_has_prefix(path, S("vendor/"));
            if (!goroot_first)
                gb_has_subdir(ctxt, a, ctxt->goroot, src_dir, &goroot_first);
            if (goroot_first) {
                Str dir = gb_join3(ctxt, a, ctxt->goroot, S("src"), path);
                if (!str_eq(ctxt->compiler, S("gccgo"))) {
                    bool is_dir = gb_is_dir(ctxt, a, dir);
                    binary_only =
                        !is_dir && (mode & BUILD_ALLOW_BINARY) != 0 &&
                        im.pkga.len > 0 &&
                        gb_is_file(ctxt, a, gb_join2(ctxt, a, ctxt->goroot, im.pkga));
                    if (is_dir || binary_only) {
                        p->dir = dir;
                        p->goroot = true;
                        p->root = ctxt->goroot;
                        goto found;
                    }
                }
                tried_goroot = dir;
            }
            if (str_eq(ctxt->compiler, S("gccgo")) && gb_gccgo_is_standard(a, path)) {
                /* gccgo keeps no sources for its standard library */
                p->dir = gb_join3(ctxt, a, ctxt->goroot, S("src"), path);
                p->goroot = true;
                p->root = ctxt->goroot;
                goto found;
            }
        }
        for (Int i = 0; i < gopath.len; i++) {
            Str root = gb_at(gopath, i);
            Str dir = gb_join3(ctxt, a, root, S("src"), path);
            bool is_dir = gb_is_dir(ctxt, a, dir);
            binary_only = !is_dir && (mode & BUILD_ALLOW_BINARY) != 0 &&
                          im.pkga.len > 0 &&
                          gb_is_file(ctxt, a, gb_join2(ctxt, a, root, im.pkga));
            if (is_dir || binary_only) {
                p->dir = dir;
                p->root = root;
                goto found;
            }
            gb_append_str(a, &tried_gopath, dir);
        }

        /* GOROOT last, when it was passed over above. */
        if (ctxt->goroot.len > 0 && tried_goroot.len == 0) {
            Str dir = gb_join3(ctxt, a, ctxt->goroot, S("src"), path);
            if (!str_eq(ctxt->compiler, S("gccgo"))) {
                bool is_dir = gb_is_dir(ctxt, a, dir);
                binary_only =
                    !is_dir && (mode & BUILD_ALLOW_BINARY) != 0 && im.pkga.len > 0 &&
                    gb_is_file(ctxt, a, gb_join2(ctxt, a, ctxt->goroot, im.pkga));
                if (is_dir || binary_only) {
                    p->dir = dir;
                    p->goroot = true;
                    p->root = ctxt->goroot;
                    goto found;
                }
            }
            tried_goroot = dir;
        }

        /* Not found: say everywhere that was looked. */
        Slice paths = slice_nil(TYPE_STRING);
        bool first = true;
        for (Int i = 0; i < im.tried_vendor.len; i++) {
            Str dir = gb_at(im.tried_vendor, i);
            gb_append_str(a, &paths,
                          first ? fmt_sprintf_v(a, "\t%s (vendor tree)", dir)
                                : fmt_sprintf_v(a, "\t%s", dir));
            first = false;
        }
        if (tried_goroot.len > 0)
            gb_append_str(a, &paths,
                          fmt_sprintf_v(a, "\t%s (from $GOROOT)", tried_goroot));
        else
            gb_append_str(a, &paths, S("\t($GOROOT not set)"));
        first = true;
        for (Int i = 0; i < tried_gopath.len; i++) {
            Str dir = gb_at(tried_gopath, i);
            gb_append_str(a, &paths,
                          first ? fmt_sprintf_v(a, "\t%s (from $GOPATH)", dir)
                                : fmt_sprintf_v(a, "\t%s", dir));
            first = false;
        }
        if (tried_gopath.len == 0)
            gb_append_str(
                a, &paths,
                S("\t($GOPATH not set. For more details see: 'go help gopath')"));
        Str list = strings_join(a, paths, S("\n"));
        *err = fmt_errorf_v("cannot find package %q in any of:\n%s", path, list);
        return p;
    }

found:
    if (p->root.len > 0) {
        p->src_root = gb_join2(ctxt, a, p->root, S("src"));
        p->pkg_root = gb_join2(ctxt, a, p->root, S("pkg"));
        p->bin_dir = gb_join2(ctxt, a, p->root, S("bin"));
        if (im.pkga.len > 0) {
            /* A GOROOT package has no .a file unless GODEBUG says otherwise. */
            p->pkg_target_root = gb_join2(ctxt, a, p->root, im.pkgtargetroot);
            if (!p->goroot ||
                (gb_install_goroot_all(a) && !str_eq(p->import_path, S("unsafe")) &&
                 !str_eq(p->import_path, S("builtin"))))
                p->pkg_obj = gb_join2(ctxt, a, p->root, im.pkga);
        }
    }

    /* A local import whose directory is not there is an error, and gccgo's
     * standard packages have no directory. */
    if (build_is_local_import(path) && !gb_is_dir(ctxt, a, p->dir)) {
        if (str_eq(ctxt->compiler, S("gccgo")) && p->goroot)
            return p;
        *err = fmt_errorf_v("cannot find package %q in:\n\t%s", p->import_path, p->dir);
        return p;
    }

    if ((mode & BUILD_FIND_ONLY) != 0) {
        *err = pkgerr;
        return p;
    }
    if (binary_only && (mode & BUILD_ALLOW_BINARY) != 0) {
        *err = pkgerr;
        return p;
    }

    if (str_eq(ctxt->compiler, S("gccgo")) && p->goroot)
        return p;

    Error derr = BURROW_NO_ERROR;
    Slice dirs = gb_read_dir(ctxt, a, p->dir, &derr);
    if (BURROW_FAILED(derr)) {
        *err = derr;
        return p;
    }

    im.bad_files = gb_map(a, TYPE_STRING, TYPE_BOOL);

    Slice s_upper_files = slice_nil(TYPE_STRING); /* .S and .sx, kept with cgo only */
    Str first_file = BURROW_STR_EMPTY;
    Str first_comment_file = BURROW_STR_EMPTY;
    Map *embed_pos = gb_pos_map(a);
    Map *test_embed_pos = gb_pos_map(a);
    Map *x_test_embed_pos = gb_pos_map(a);
    Map *import_pos = gb_pos_map(a);
    Map *test_import_pos = gb_pos_map(a);
    Map *x_test_import_pos = gb_pos_map(a);
    Map *all_tags = gb_map(a, TYPE_STRING, TYPE_BOOL);
    TokenFileSet *fset = token_new_file_set(a);
    if (fset == NULL)
        gb_oom();

    for (Int di = 0; di < dirs.len; di++) {
        FsDirEntry d = BURROW_AT(FsDirEntry, dirs, di);
        if (d.vt->is_dir(d.data))
            continue;
        if (d.vt->type(d.data) == FS_MODE_SYMLINK) {
            if (gb_is_dir(ctxt, a, gb_join2(ctxt, a, p->dir, d.vt->name(d.data))))
                continue; /* a link to a directory */
        }

        Str name = d.vt->name(d.data);
        Str ext = gb_name_ext(name);

        Error merr = BURROW_NO_ERROR;
        burrow__BuildFileInfo *info = gb_match_file(ctxt, a, p->dir, name, all_tags,
                                                    &p->binary_only, fset, &merr);
        if (BURROW_FAILED(merr) && strings_has_suffix(name, S(".go"))) {
            gb_bad_go_file(&im, name, merr);
            continue;
        }
        if (info == NULL) {
            if (strings_has_prefix(name, S("_")) || strings_has_prefix(name, S("."))) {
                /* not even listed as ignored */
            } else if (str_eq(ext, S(".go"))) {
                gb_append_str(a, &p->ignored_go_files, name);
            } else if (gb_file_list_for_ext(p, ext) != NULL) {
                gb_append_str(a, &p->ignored_other_files, name);
            }
            continue;
        }

        /* Everything but .go files goes straight on its list. */
        if (str_eq(ext, S(".S")) || str_eq(ext, S(".sx"))) {
            gb_append_str(a, &s_upper_files, name);
            continue;
        }
        if (!str_eq(ext, S(".go"))) {
            Slice *list = gb_file_list_for_ext(p, ext);
            if (list != NULL)
                gb_append_str(a, list, name);
            continue;
        }

        Str data = info->header;
        Str filename = info->name;

        if (BURROW_FAILED(info->parse_err))
            gb_bad_go_file(&im, name, info->parse_err);

        Str pkg = BURROW_STR_EMPTY;
        if (info->parsed != NULL) {
            pkg = info->parsed->name->name;
            if (str_eq(pkg, S("documentation"))) {
                gb_append_str(a, &p->ignored_go_files, name);
                continue;
            }
        }

        bool is_test = strings_has_suffix(name, S("_test.go"));
        bool is_x_test = false;
        if (is_test && strings_has_suffix(pkg, S("_test")) && !str_eq(p->name, pkg)) {
            is_x_test = true;
            pkg = gb_sub(pkg, 0, pkg.len - 5);
        }

        if (p->name.len == 0) {
            p->name = pkg;
            first_file = name;
        } else if (!str_eq(pkg, p->name)) {
            /* With an import, the go command reports this one, so it goes in
             * invalid_go_files along with everything else that is wrong. */
            gb_bad_go_file(&im, name,
                           gb_multi_error(error_allocator(), p->dir, p->name, pkg,
                                          first_file, name));
        }
        /* The doc of the first non-test file with one. */
        if (info->parsed != NULL && info->parsed->doc != NULL && p->doc.len == 0 &&
            !is_test && !is_x_test)
            p->doc = doc_synopsis(a, ast_comment_group_text(info->parsed->doc, a));

        if ((mode & BUILD_IMPORT_COMMENT) != 0) {
            Int line = 0;
            Str qcom = gb_find_import_comment(data, &line);
            if (line != 0) {
                Error uerr = BURROW_NO_ERROR;
                Str com = strconv_unquote(a, qcom, &uerr);
                if (BURROW_FAILED(uerr)) {
                    gb_bad_go_file(&im, name,
                                   fmt_errorf_v("%s:%d: cannot parse import comment",
                                                filename, line));
                } else if (p->import_comment.len == 0) {
                    p->import_comment = com;
                    first_comment_file = name;
                } else if (!str_eq(p->import_comment, com)) {
                    gb_bad_go_file(
                        &im, name,
                        fmt_errorf_v("found import comments %q (%s) and %q (%s) in %s",
                                     p->import_comment, first_comment_file, com, name,
                                     p->dir));
                }
            }
        }

        /* cgo, and the #cgo lines in the comment on import "C". */
        bool is_cgo = false;
        for (Int i = 0; i < info->imports.len; i++) {
            burrow__BuildFileImport *imp =
                &BURROW_AT(burrow__BuildFileImport, info->imports, i);
            if (!str_eq(imp->path, S("C")))
                continue;
            if (is_test) {
                gb_bad_go_file(
                    &im, name,
                    fmt_errorf_v("use of cgo in test %s not supported", filename));
                continue;
            }
            is_cgo = true;
            if (imp->doc != NULL) {
                Error cerr = gb_save_cgo(ctxt, a, filename, p, imp->doc);
                if (BURROW_FAILED(cerr))
                    gb_bad_go_file(&im, name, cerr);
            }
        }

        Slice *file_list = NULL;
        Map *import_map = NULL;
        Map *embed_map = NULL;
        Slice *directives = NULL;
        if (is_cgo) {
            gb_set_true(all_tags, S("cgo"));
            if (ctxt->cgo_enabled) {
                file_list = &p->cgo_files;
                import_map = import_pos;
                embed_map = embed_pos;
                directives = &p->directives;
            } else {
                /* Ignore imports and embeds from cgo files when cgo is off. */
                file_list = &p->ignored_go_files;
            }
        } else if (is_x_test) {
            file_list = &p->x_test_go_files;
            import_map = x_test_import_pos;
            embed_map = x_test_embed_pos;
            directives = &p->x_test_directives;
        } else if (is_test) {
            file_list = &p->test_go_files;
            import_map = test_import_pos;
            embed_map = test_embed_pos;
            directives = &p->test_directives;
        } else {
            file_list = &p->go_files;
            import_map = import_pos;
            embed_map = embed_pos;
            directives = &p->directives;
        }
        gb_append_str(a, file_list, name);
        if (import_map != NULL) {
            for (Int i = 0; i < info->imports.len; i++) {
                burrow__BuildFileImport *imp =
                    &BURROW_AT(burrow__BuildFileImport, info->imports, i);
                gb_pos_add(a, import_map, imp->path,
                           token_file_set_position(fset, imp->pos));
            }
        }
        if (embed_map != NULL) {
            for (Int i = 0; i < info->embeds.len; i++) {
                burrow__BuildFileEmbed *emb =
                    &BURROW_AT(burrow__BuildFileEmbed, info->embeds, i);
                gb_pos_add(a, embed_map, emb->pattern, emb->pos);
            }
        }
        if (directives != NULL)
            gb_append_slice(a, directives, info->directives);
    }

    p->all_tags = gb_sorted_keys(a, all_tags);

    p->embed_patterns = gb_sorted_keys(a, embed_pos);
    p->embed_pattern_pos = embed_pos;
    p->test_embed_patterns = gb_sorted_keys(a, test_embed_pos);
    p->test_embed_pattern_pos = test_embed_pos;
    p->x_test_embed_patterns = gb_sorted_keys(a, x_test_embed_pos);
    p->x_test_embed_pattern_pos = x_test_embed_pos;

    p->imports = gb_sorted_keys(a, import_pos);
    p->import_pos = import_pos;
    p->test_imports = gb_sorted_keys(a, test_import_pos);
    p->test_import_pos = test_import_pos;
    p->x_test_imports = gb_sorted_keys(a, x_test_import_pos);
    p->x_test_import_pos = x_test_import_pos;

    /* The .S files are only for cgo, which runs the C compiler on them, and
     * without it they are listed as ignored. */
    if (p->cgo_files.len > 0) {
        gb_append_slice(a, &p->s_files, s_upper_files);
        sort_strings(p->s_files);
    } else {
        gb_append_slice(a, &p->ignored_other_files, s_upper_files);
        sort_strings(p->ignored_other_files);
    }

    if (BURROW_FAILED(im.bad_err)) {
        *err = im.bad_err;
        return p;
    }
    if (p->go_files.len + p->cgo_files.len + p->test_go_files.len +
            p->x_test_go_files.len ==
        0) {
        *err = gb_no_go_error(error_allocator(), p->dir);
        return p;
    }
    *err = pkgerr;
    return p;
}

BuildPackage *build_context_import_dir(BuildContext *ctxt, Alloc *a, Str dir,
                                       BuildImportMode mode, Error *err) {
    return build_context_import(ctxt, a, S("."), dir, mode, err);
}

BuildPackage *build_import(Alloc *a, Str path, Str src_dir, BuildImportMode mode,
                           Error *err) {
    BuildContext ctxt = build_default(a);
    return build_context_import(&ctxt, a, path, src_dir, mode, err);
}

BuildPackage *build_import_dir(Alloc *a, Str dir, BuildImportMode mode, Error *err) {
    BuildContext ctxt = build_default(a);
    return build_context_import_dir(&ctxt, a, dir, mode, err);
}
