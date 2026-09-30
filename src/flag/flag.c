/* flag, command line flag parsing. burrow/flag.h has the tour.
 *
 * Go's flag.go, in Go's order. Where Go keeps its flags in maps and sorts them
 * every time it walks them, a set here keeps two arrays sorted by name, which
 * is the order every walk wants and a binary search away from any lookup.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/flag.h"

#include "burrow/core.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LIT(s) ((Str){(const Byte *)("" s), (Int)(sizeof(s) - 1)})

BURROW_SENTINEL_ERROR(flag_err_help, "flag: help requested");

/* What a value's set returns when strconv could not parse its argument, and
 * when the number did not fit, in place of strconv's own error, which would
 * repeat the argument the message already quotes. */
static const Str err_parse__text = {(const Byte *)"parse error", 11};
static const Error err_parse = {&burrow_sentinel_error_vt, &err_parse__text};
static const Str err_range__text = {(const Byte *)"value out of range", 18};
static const Error err_range = {&burrow_sentinel_error_vt, &err_range__text};

/* Go's numError. */
static Error num_error(Error err) {
    if (errors_is(err, strconv_err_syntax))
        return err_parse;
    if (errors_is(err, strconv_err_range))
        return err_range;
    return err;
}

/* ------------------------------------------------------------ the values
 *
 * Each builtin value's data is the variable itself, as Go's are the variable
 * converted to a named type, so the zero value print_defaults compares against
 * is a zeroed variable of self_type. */

static Str bool_string(void *self, Alloc *a) {
    (void)a;
    return strconv_format_bool(*(bool *)self);
}

static Error bool_set(void *self, Str s) {
    Error err;
    bool v = strconv_parse_bool(s, &err);
    *(bool *)self = v;
    return BURROW_OK(err) ? BURROW_NO_ERROR : err_parse;
}

static Any bool_get(void *self) {
    return BURROW_ANY(TYPE_BOOL, self);
}

static bool bool_is_bool_flag(void *self) {
    (void)self;
    return true;
}

static const FlagValueVT bool_vt = {TYPE_BOOL, bool_string, bool_set, bool_get,
                                    bool_is_bool_flag};

static Str int_string(void *self, Alloc *a) {
    return strconv_format_int(a, (int64_t)*(Int *)self, 10);
}

static Error int_set(void *self, Str s) {
    Error err;
    int64_t v = strconv_parse_int(s, 0, (Int)sizeof(Int) * 8, &err);
    *(Int *)self = (Int)v;
    return BURROW_OK(err) ? BURROW_NO_ERROR : num_error(err);
}

static Any int_get(void *self) {
    return BURROW_ANY(TYPE_INT, self);
}

static const FlagValueVT int_vt = {TYPE_INT, int_string, int_set, int_get, NULL};

static Str int64_string(void *self, Alloc *a) {
    return strconv_format_int(a, *(int64_t *)self, 10);
}

static Error int64_set(void *self, Str s) {
    Error err;
    int64_t v = strconv_parse_int(s, 0, 64, &err);
    *(int64_t *)self = v;
    return BURROW_OK(err) ? BURROW_NO_ERROR : num_error(err);
}

static Any int64_get(void *self) {
    return BURROW_ANY(TYPE_INT64, self);
}

static const FlagValueVT int64_vt = {TYPE_INT64, int64_string, int64_set, int64_get,
                                     NULL};

static Str uint_string(void *self, Alloc *a) {
    return strconv_format_uint(a, (uint64_t)*(Uint *)self, 10);
}

static Error uint_set(void *self, Str s) {
    Error err;
    uint64_t v = strconv_parse_uint(s, 0, (Int)sizeof(Uint) * 8, &err);
    *(Uint *)self = (Uint)v;
    return BURROW_OK(err) ? BURROW_NO_ERROR : num_error(err);
}

static Any uint_get(void *self) {
    return BURROW_ANY(TYPE_UINT, self);
}

static const FlagValueVT uint_vt = {TYPE_UINT, uint_string, uint_set, uint_get, NULL};

static Str uint64_string(void *self, Alloc *a) {
    return strconv_format_uint(a, *(uint64_t *)self, 10);
}

static Error uint64_set(void *self, Str s) {
    Error err;
    uint64_t v = strconv_parse_uint(s, 0, 64, &err);
    *(uint64_t *)self = v;
    return BURROW_OK(err) ? BURROW_NO_ERROR : num_error(err);
}

static Any uint64_get(void *self) {
    return BURROW_ANY(TYPE_UINT64, self);
}

static const FlagValueVT uint64_vt = {TYPE_UINT64, uint64_string, uint64_set,
                                      uint64_get, NULL};

static Str string_string(void *self, Alloc *a) {
    (void)a;
    return *(Str *)self;
}

static Error string_set(void *self, Str s) {
    *(Str *)self = s;
    return BURROW_NO_ERROR;
}

static Any string_get(void *self) {
    return BURROW_ANY(TYPE_STRING, self);
}

static const FlagValueVT string_vt = {TYPE_STRING, string_string, string_set,
                                      string_get, NULL};

static Str float64_string(void *self, Alloc *a) {
    return strconv_format_float(a, *(double *)self, 'g', -1, 64);
}

static Error float64_set(void *self, Str s) {
    Error err;
    double v = strconv_parse_float(s, 64, &err);
    *(double *)self = v;
    return BURROW_OK(err) ? BURROW_NO_ERROR : num_error(err);
}

static Any float64_get(void *self) {
    return BURROW_ANY(TYPE_FLOAT64, self);
}

static const FlagValueVT float64_vt = {TYPE_FLOAT64, float64_string, float64_set,
                                       float64_get, NULL};

static Str duration_value_string(void *self, Alloc *a) {
    return duration_string(*(Duration *)self, a);
}

static Error duration_set(void *self, Str s) {
    Error err;
    Duration v = time_parse_duration(s, &err);
    *(Duration *)self = v;
    return BURROW_OK(err) ? BURROW_NO_ERROR : err_parse;
}

static Any duration_get(void *self) {
    return BURROW_ANY(TYPE_DURATION, self);
}

static const FlagValueVT duration_vt = {TYPE_DURATION, duration_value_string,
                                        duration_set, duration_get, NULL};

/* Go's textValue. The data is a TextBox the set made, holding the variable and
 * the allocator its UnmarshalText gets. No self_type, since Go's textValue is a
 * struct whose zero value prints as nothing. */
typedef struct TextBox {
    Any p;
    Alloc *a;
} TextBox;

static Str text_string(void *self, Alloc *a) {
    TextBox *b = (TextBox *)self;
    if (b == NULL || !encoding_is_text_marshaler(b->p))
        return BURROW_STR_EMPTY;
    Error err;
    Slice s = encoding_marshal_text(a, b->p, &err);
    if (!BURROW_OK(err))
        return BURROW_STR_EMPTY;
    return str_from_bytes(s.p, s.len);
}

static Error text_set(void *self, Str s) {
    TextBox *b = (TextBox *)self;
    return encoding_unmarshal_text(
        b->a, b->p, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE));
}

static Any text_get(void *self) {
    return ((TextBox *)self)->p;
}

static const FlagValueVT text_vt = {NULL, text_string, text_set, text_get, NULL};

/* Go's funcValue and boolFuncValue. The data is the FlagFunc, in storage the
 * set made. Neither is a Getter, in Go or here. */
static Str func_string(void *self, Alloc *a) {
    (void)self;
    (void)a;
    return BURROW_STR_EMPTY;
}

static Error func_set(void *self, Str s) {
    FlagFunc *fn = (FlagFunc *)self;
    return fn->f(fn->env, s);
}

static const FlagValueVT func_vt = {NULL, func_string, func_set, NULL, NULL};
static const FlagValueVT bool_func_vt = {NULL, func_string, func_set, NULL,
                                         bool_is_bool_flag};

/* ---------------------------------------------------------------- output */

/* Standard error as a writer, through stdio so that it interleaves with what
 * the program prints there itself. */
static Int stderr_write(void *self, Slice p, Error *err) {
    (void)self;
    size_t n = fwrite(p.p, 1, (size_t)p.len, stderr);
    if (err)
        *err = BURROW_NO_ERROR;
    return (Int)n;
}

static const IoWriterVT stderr_vt = {NULL, stderr_write};

IoWriter flag_flag_set_output(FlagFlagSet *f) {
    if (f->output.vt == NULL)
        return (IoWriter){&stderr_vt, NULL};
    return f->output;
}

static void out_str(FlagFlagSet *f, Str s) {
    IoWriter w = flag_flag_set_output(f);
    (void)w.vt->write(
        w.data, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE), NULL);
}

/* ------------------------------------------------------------ the set */

/* A flag and what the set made for it, in one allocation with the name, the
 * usage and the default's text after it. */
typedef struct FlagNode {
    Flag flag;
    size_t size;
    void *store; /* made by the set, or NULL */
    size_t store_size;
    size_t store_align;
} FlagNode;

static Alloc *set_alloc(FlagFlagSet *f) {
    if (f->a == NULL)
        f->a = heap_allocator();
    return f->a;
}

BURROW_NORETURN static void out_of_memory(void) {
    panic_str(LIT("flag: out of memory"));
}

static void *alloc_or_panic(FlagFlagSet *f, size_t size, size_t align) {
    void *p = mem_alloc(set_alloc(f), size, align);
    if (p == NULL)
        out_of_memory();
    memset(p, 0, size);
    return p;
}

/* The index of name in a sorted list, or where it would go, and whether it is
 * there. */
static Int search(Flag **list, Int n, Str name, bool *found) {
    Int lo = 0;
    Int hi = n;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        Str m = list[mid]->name;
        /* Names mostly differ in their first byte, which saves a call. */
        int c = m.len > 0 && name.len > 0 && m.p[0] != name.p[0]
                    ? (m.p[0] < name.p[0] ? -1 : 1)
                    : str_cmp(m, name);
        if (c == 0) {
            *found = true;
            return mid;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = false;
    return lo;
}

static void insert(FlagFlagSet *f, Flag ***list, Int *n, Int *cap, Flag *fl) {
    bool found;
    Int i = search(*list, *n, fl->name, &found);
    if (found)
        return;
    if (*n == *cap) {
        Int nc = *cap == 0 ? 8 : *cap * 2;
        Flag **grown = mem_realloc(set_alloc(f), *list, (size_t)*cap * sizeof(Flag *),
                                   (size_t)nc * sizeof(Flag *), _Alignof(Flag *));
        if (grown == NULL)
            out_of_memory();
        *list = grown;
        *cap = nc;
    }
    memmove(*list + i + 1, *list + i, (size_t)(*n - i) * sizeof(Flag *));
    (*list)[i] = fl;
    (*n)++;
}

/* flag_command_line, before anything has pointed it elsewhere. */
static void command_line_usage(void *env);
static FlagFlagSet command_line = {
    {command_line_usage, NULL},
    NULL,
    {NULL, 0},
    false,
    NULL,
    0,
    0,
    NULL,
    0,
    0,
    {NULL, 0, 0, NULL},
    FLAG_EXIT_ON_ERROR,
    {NULL, NULL},
    NULL,
    0,
    0,
};

FlagFlagSet *flag_command_line = &command_line;

/* os.Args, which the os package will own once there is one. Read once, the
 * first time something needs it, and kept until the program ends. */
static SyncOnce args_once;
static Str *os_args;
static Int os_nargs;

static void load_args(void *env) {
    (void)env;
    Alloc *a = heap_allocator();
    int64_t cap = 4096;
    char *buf = NULL;
    int64_t n = 0;
    for (;;) {
        buf = mem_alloc(a, (size_t)cap, 1);
        if (buf == NULL)
            return;
        PalErrno e = 0;
        n = pal_args(buf, cap, &e);
        if (n >= 0)
            break;
        mem_free(a, buf, (size_t)cap, 1);
        buf = NULL;
        if (e != PAL_ERANGE)
            return;
        cap *= 2;
    }
    Int count = 0;
    for (int64_t i = 0; i < n; i++)
        if (buf[i] == '\0')
            count++;
    if (count == 0)
        return;
    Str *args = mem_alloc(a, (size_t)count * sizeof(Str), _Alignof(Str));
    if (args == NULL)
        return;
    int64_t start = 0;
    Int k = 0;
    for (int64_t i = 0; i < n; i++) {
        if (buf[i] == '\0') {
            args[k++] = str_from_bytes(buf + start, (Int)(i - start));
            start = i + 1;
        }
    }
    os_args = args;
    os_nargs = count;
    /* Go's init gives CommandLine the program's name. */
    command_line.name = str_clone(a, args[0]);
}

static void need_args(void) {
    sync_once_do(&args_once, BURROW_FN(Func, load_args, NULL));
}

/* The one set whose name comes from outside is the command line's, and it
 * arrives the first time anything asks. */
static void need_name(FlagFlagSet *f) {
    if (f == &command_line)
        need_args();
}

FlagFlagSet *flag_new_flag_set(Alloc *a, Str name, FlagErrorHandling error_handling) {
    if (a == NULL)
        a = heap_allocator();
    FlagFlagSet *f = mem_alloc(a, sizeof *f, _Alignof(FlagFlagSet));
    if (f == NULL)
        return NULL;
    memset(f, 0, sizeof *f);
    f->a = a;
    f->name = str_clone(a, name);
    f->error_handling = error_handling;
    return f;
}

void flag_flag_set_init(FlagFlagSet *f, Str name, FlagErrorHandling error_handling) {
    need_name(f);
    Alloc *a = set_alloc(f);
    Str old = f->name;
    f->name = str_clone(a, name);
    if (old.len > 0)
        mem_free(a, (void *)(uintptr_t)old.p, (size_t)old.len, 1);
    f->error_handling = error_handling;
}

void flag_flag_set_destroy(FlagFlagSet *f) {
    Alloc *a = set_alloc(f);
    for (Int i = 0; i < f->nformal; i++) {
        FlagNode *n = (FlagNode *)(void *)f->formal[i];
        if (n->store != NULL)
            mem_free(a, n->store, n->store_size, n->store_align);
        mem_free(a, n, n->size, _Alignof(FlagNode));
    }
    mem_free(a, f->formal, (size_t)f->capformal * sizeof(Flag *), _Alignof(Flag *));
    mem_free(a, f->actual, (size_t)f->capactual * sizeof(Flag *), _Alignof(Flag *));
    for (Int i = 0; i < 2 * f->nundef; i++)
        mem_free(a, (void *)(uintptr_t)f->undef[i].p, (size_t)f->undef[i].len, 1);
    mem_free(a, f->undef, (size_t)f->capundef * 2 * sizeof(Str), _Alignof(Str));
    if (f->name.len > 0)
        mem_free(a, (void *)(uintptr_t)f->name.p, (size_t)f->name.len, 1);
    Func usage = f->usage;
    memset(f, 0, sizeof *f);
    f->a = a;
    f->usage = usage;
}

void flag_flag_set_free(FlagFlagSet *f) {
    if (f == NULL)
        return;
    Alloc *a = set_alloc(f);
    flag_flag_set_destroy(f);
    mem_free(a, f, sizeof *f, _Alignof(FlagFlagSet));
}

Str flag_flag_set_name(FlagFlagSet *f) {
    need_name(f);
    return f->name;
}

FlagErrorHandling flag_flag_set_error_handling(FlagFlagSet *f) {
    return f->error_handling;
}

void flag_flag_set_set_output(FlagFlagSet *f, IoWriter output) {
    f->output = output;
}

void flag_flag_set_visit_all(FlagFlagSet *f, FlagVisitFunc fn) {
    for (Int i = 0; i < f->nformal; i++)
        fn.f(fn.env, f->formal[i]);
}

void flag_flag_set_visit(FlagFlagSet *f, FlagVisitFunc fn) {
    for (Int i = 0; i < f->nactual; i++)
        fn.f(fn.env, f->actual[i]);
}

Flag *flag_flag_set_lookup(FlagFlagSet *f, Str name) {
    bool found;
    Int i = search(f->formal, f->nformal, name, &found);
    return found ? f->formal[i] : NULL;
}

/* Go's FlagSet.set. file and line are where it was called from, for the panic
 * a later definition of an unknown name gets. */
Error burrow__flag_set_at(FlagFlagSet *f, Str name, Str value, const char *file,
                          Int line) {
    Flag *fl = flag_flag_set_lookup(f, name);
    if (fl == NULL) {
        Alloc *a = set_alloc(f);
        if (f->nundef == f->capundef) {
            Int nc = f->capundef == 0 ? 4 : f->capundef * 2;
            Str *grown = mem_realloc(a, f->undef, (size_t)f->capundef * 2 * sizeof(Str),
                                     (size_t)nc * 2 * sizeof(Str), _Alignof(Str));
            if (grown == NULL)
                out_of_memory();
            f->undef = grown;
            f->capundef = nc;
        }
        Str pos = fmt_sprintf_v(a, "%s:%d", file ? file : "?", file ? line : 0);
        /* Go's undef is a map, so a second Set of the same name replaces the
         * first one's position. */
        Int i = 0;
        for (; i < f->nundef; i++)
            if (str_eq(f->undef[2 * i], name))
                break;
        if (i < f->nundef) {
            Str old = f->undef[2 * i + 1];
            mem_free(a, (void *)(uintptr_t)old.p, (size_t)old.len, 1);
        } else {
            f->undef[2 * i] = str_clone(a, name);
            f->nundef++;
        }
        f->undef[2 * i + 1] = pos;
        return fmt_errorf_v("no such flag -%s", name);
    }
    Error err = fl->value.vt->set(fl->value.data, value);
    if (!BURROW_OK(err))
        return err;
    insert(f, &f->actual, &f->nactual, &f->capactual, fl);
    return BURROW_NO_ERROR;
}

Error(flag_flag_set_set)(FlagFlagSet *f, Str name, Str value) {
    return burrow__flag_set_at(f, name, value, NULL, 0);
}

/* ----------------------------------------------------------- the usage */

/* A zeroed value of t, told to print itself, and whether that panicked, with
 * the panic's text in *panicked. The text is copied into a inside the catch
 * block, since what the panic value points at is gone after it. The TRY is in
 * a function of its own so that nothing it writes is a local of the function
 * holding the jump buffer. */
static bool zero_string(const FlagValueVT *vt, void *zero, Alloc *a, Str *out,
                        Str *panicked) {
    volatile bool ok = false;
    BURROW_TRY {
        *out = vt->string(zero, a);
        ok = true;
    }
    BURROW_CATCH(p) {
        *panicked = str_clone(a, panic_text(p));
    }
    BURROW_TRY_END;
    return ok;
}

/* Go's %v of a reflect.Type: the package's name, which is the last element of
 * its path, a dot, and the type's name. */
static Str type_text(Alloc *a, const Type *t) {
    Str pkg = t->pkg_path;
    for (Int i = pkg.len; i > 0; i--) {
        if (pkg.p[i - 1] == '/') {
            pkg = str_from_bytes(pkg.p + i, pkg.len - i);
            break;
        }
    }
    if (pkg.len == 0)
        return t->name;
    return fmt_sprintf_v(a, "%s.%s", pkg, t->name);
}

/* Go's isZeroValue: whether value is what a zeroed flag of this type prints.
 * *errmsg is set instead when printing it panicked. */
static bool is_zero_value(Alloc *a, Flag *fl, Str value, Str *errmsg) {
    const FlagValueVT *vt = fl->value.vt;
    const Type *t = vt->self_type;
    if (t == NULL)
        return value.len == 0;
    size_t size = t->size > 0 ? t->size : 1;
    size_t align = t->align > 0 ? t->align : 1;
    void *zero = mem_alloc(a, size, align);
    if (zero == NULL)
        out_of_memory();
    memset(zero, 0, size);
    Str s = BURROW_STR_EMPTY;
    Str p = BURROW_STR_EMPTY;
    if (!zero_string(vt, zero, a, &s, &p)) {
        *errmsg =
            fmt_sprintf_v(a, "panic calling String method on zero %s for flag %s: %s",
                          type_text(a, t), fl->name, p);
        return false;
    }
    return str_eq(s, value);
}

Str flag_unquote_usage(Alloc *a, const Flag *f, Str *usage) {
    Str u = f->usage;
    for (Int i = 0; i < u.len; i++) {
        if (u.p[i] != '`')
            continue;
        for (Int j = i + 1; j < u.len; j++) {
            if (u.p[j] == '`') {
                Str name = str_from_bytes(u.p + i + 1, j - i - 1);
                if (usage) {
                    Int n = u.len - 2;
                    Byte *b = NULL;
                    if (n > 0) {
                        b = mem_alloc(a, (size_t)n, 1);
                        if (b == NULL)
                            out_of_memory();
                        memcpy(b, u.p, (size_t)i);
                        memcpy(b + i, name.p, (size_t)name.len);
                        memcpy(b + i + name.len, u.p + j + 1, (size_t)(u.len - j - 1));
                    }
                    *usage = str_from_bytes(b, n);
                }
                return name;
            }
        }
        break; /* Only one back quote; use type name. */
    }
    if (usage)
        *usage = str_clone(a, u);

    const FlagValueVT *vt = f->value.vt;
    if (vt->is_bool_flag != NULL) {
        if (vt->is_bool_flag(f->value.data))
            return BURROW_STR_EMPTY;
    } else if (vt == &duration_vt) {
        return LIT("duration");
    } else if (vt == &float64_vt) {
        return LIT("float");
    } else if (vt == &int_vt || vt == &int64_vt) {
        return LIT("int");
    } else if (vt == &string_vt) {
        return LIT("string");
    } else if (vt == &uint_vt || vt == &uint64_vt) {
        return LIT("uint");
    }
    return LIT("value");
}

void flag_flag_set_print_defaults(FlagFlagSet *f) {
    Arena ar;
    arena_init(&ar, NULL, 1024);
    Alloc *a = arena_allocator(&ar);
    Str *errs = NULL;
    Int nerrs = 0;
    for (Int i = 0; i < f->nformal; i++) {
        Flag *fl = f->formal[i];
        /* Two spaces before -, which is what lines the tabs up with the
         * name. */
        Str usage;
        Str name = flag_unquote_usage(a, fl, &usage);
        Str line = fmt_sprintf_v(a, "  -%s", fl->name);
        if (name.len > 0)
            line = fmt_sprintf_v(a, "%s %s", line, name);
        /* Boolean flags of one ASCII letter are so common we treat them
         * specially, putting their usage on the same line. */
        if (line.len <= 4) /* space, space, '-', 'x'. */
            line = fmt_sprintf_v(a, "%s\t", line);
        else
            /* Four spaces before the tab triggers good alignment for both 4-
             * and 8-space tab stops. */
            line = fmt_sprintf_v(a, "%s\n    \t", line);
        /* A local rather than a call in the arguments, which MSVC warns about
         * once for every type the argument macro could have picked. */
        Str indented = strings_replace_all(a, usage, LIT("\n"), LIT("\n    \t"));
        line = fmt_sprintf_v(a, "%s%s", line, indented);

        /* Print the default value only if it differs from the zero value. */
        Str errmsg = BURROW_STR_EMPTY;
        bool zero = is_zero_value(a, fl, fl->def_value, &errmsg);
        if (errmsg.len > 0) {
            Str *grown = mem_realloc(a, errs, (size_t)nerrs * sizeof(Str),
                                     (size_t)(nerrs + 1) * sizeof(Str), _Alignof(Str));
            if (grown == NULL)
                out_of_memory();
            errs = grown;
            errs[nerrs++] = errmsg;
        } else if (!zero) {
            if (fl->value.vt == &string_vt)
                line = fmt_sprintf_v(a, "%s (default %q)", line, fl->def_value);
            else
                line = fmt_sprintf_v(a, "%s (default %s)", line, fl->def_value);
        }
        out_str(f, line);
        out_str(f, LIT("\n"));
    }
    if (nerrs > 0) {
        out_str(f, LIT("\n"));
        for (Int i = 0; i < nerrs; i++) {
            out_str(f, errs[i]);
            out_str(f, LIT("\n"));
        }
    }
    arena_free(&ar);
}

/* Go's defaultUsage, what a set without a usage of its own calls. */
static void default_usage(FlagFlagSet *f) {
    need_name(f);
    if (f->name.len == 0) {
        out_str(f, LIT("Usage:\n"));
    } else {
        Arena ar;
        arena_init(&ar, NULL, 256);
        out_str(f, fmt_sprintf_v(arena_allocator(&ar), "Usage of %s:\n", f->name));
        arena_free(&ar);
    }
    flag_flag_set_print_defaults(f);
}

/* Go's Usage variable, as the package starts it off. */
static void usage_default(void *env) {
    (void)env;
    need_args();
    Str prog = os_nargs > 0 ? os_args[0] : BURROW_STR_EMPTY;
    Arena ar;
    arena_init(&ar, NULL, 256);
    out_str(flag_command_line,
            fmt_sprintf_v(arena_allocator(&ar), "Usage of %s:\n", prog));
    arena_free(&ar);
    flag_print_defaults();
}

Func flag_usage = {usage_default, NULL};

/* Go's commandLineUsage, which reads flag_usage when it runs rather than when
 * the set is made, so that assigning flag_usage later takes effect. */
static void command_line_usage(void *env) {
    (void)env;
    if (flag_usage.f != NULL)
        flag_usage.f(flag_usage.env);
}

static void call_usage(FlagFlagSet *f) {
    if (f->usage.f == NULL)
        default_usage(f);
    else
        f->usage.f(f->usage.env);
}

/* --------------------------------------------------------- definitions */

/* Go's FlagSet.sprintf: the message to the output and back to the caller, in
 * the error arena, where it outlives a panic. */
static Str say(FlagFlagSet *f, Str msg) {
    out_str(f, msg);
    out_str(f, LIT("\n"));
    return msg;
}

/* The checks at the top of Go's FlagSet.Var, made before any storage for the
 * value is allocated, so a panic here leaks nothing. */
static void check_name(FlagFlagSet *f, Str name) {
    Alloc *ea = error_allocator();
    /* Flag must not begin "-" or contain "=". */
    if (strings_has_prefix(name, LIT("-")))
        panic_str(say(f, fmt_sprintf_v(ea, "flag %q begins with -", name)));
    if (strings_index_byte(name, '=') >= 0)
        panic_str(say(f, fmt_sprintf_v(ea, "flag %q contains =", name)));

    if (flag_flag_set_lookup(f, name) != NULL) {
        need_name(f);
        Str msg = f->name.len == 0
                      ? fmt_sprintf_v(ea, "flag redefined: %s", name)
                      : fmt_sprintf_v(ea, "%s flag redefined: %s", f->name, name);
        panic_str(
            say(f, msg)); /* Happens only if flags are declared with identical names */
    }
    for (Int i = 0; i < f->nundef; i++)
        if (str_eq(f->undef[2 * i], name))
            panic_str(fmt_sprintf_v(ea, "flag %s set at %s before being defined", name,
                                    f->undef[2 * i + 1]));
}

/* The rest of Go's FlagSet.Var, after check_name, with the storage the set made
 * for the value, if any, which the set frees with the flag. */
static void define(FlagFlagSet *f, FlagValue value, Str name, Str usage, void *store,
                   size_t store_size, size_t store_align) {
    Arena ar;
    arena_init(&ar, NULL, 256);
    Str def = value.vt->string(value.data, arena_allocator(&ar));

    size_t size =
        sizeof(FlagNode) + (size_t)name.len + (size_t)usage.len + (size_t)def.len;
    FlagNode *n = alloc_or_panic(f, size, _Alignof(FlagNode));
    Byte *text = (Byte *)(n + 1);
    if (name.len > 0)
        memcpy(text, name.p, (size_t)name.len);
    if (usage.len > 0)
        memcpy(text + name.len, usage.p, (size_t)usage.len);
    if (def.len > 0)
        memcpy(text + name.len + usage.len, def.p, (size_t)def.len);
    arena_free(&ar);

    n->size = size;
    n->store = store;
    n->store_size = store_size;
    n->store_align = store_align;
    n->flag.name = str_from_bytes(text, name.len);
    n->flag.usage = str_from_bytes(text + name.len, usage.len);
    n->flag.value = value;
    n->flag.def_value = str_from_bytes(text + name.len + usage.len, def.len);
    insert(f, &f->formal, &f->nformal, &f->capformal, &n->flag);
}

void flag_flag_set_var(FlagFlagSet *f, FlagValue value, Str name, Str usage) {
    check_name(f, name);
    define(f, value, name, usage, NULL, 0, 0);
}

/* NOLINTBEGIN(bugprone-macro-parentheses) */
/* The _var forms, which store the default and hand the variable to define, and
 * the others, which make the variable first. */
#define FLAG_TYPE(fname, T, vt)                                                        \
    void flag_flag_set_##fname##_var(FlagFlagSet *f, T *p, Str name, T value,          \
                                     Str usage) {                                      \
        *p = value;                                                                    \
        check_name(f, name);                                                           \
        define(f, (FlagValue){&(vt), p}, name, usage, NULL, 0, 0);                     \
    }                                                                                  \
    T *flag_flag_set_##fname(FlagFlagSet *f, Str name, T value, Str usage) {           \
        check_name(f, name);                                                           \
        T *p = alloc_or_panic(f, sizeof(T), _Alignof(T));                              \
        *p = value;                                                                    \
        define(f, (FlagValue){&(vt), p}, name, usage, p, sizeof(T), _Alignof(T));      \
        return p;                                                                      \
    }                                                                                  \
    void flag_##fname##_var(T *p, Str name, T value, Str usage) {                      \
        flag_flag_set_##fname##_var(flag_command_line, p, name, value, usage);         \
    }                                                                                  \
    T *flag_##fname(Str name, T value, Str usage) {                                    \
        return flag_flag_set_##fname(flag_command_line, name, value, usage);           \
    }

FLAG_TYPE(bool, bool, bool_vt)
FLAG_TYPE(int, Int, int_vt)
FLAG_TYPE(int64, int64_t, int64_vt)
FLAG_TYPE(uint, Uint, uint_vt)
FLAG_TYPE(uint64, uint64_t, uint64_vt)
FLAG_TYPE(string, Str, string_vt)
FLAG_TYPE(float64, double, float64_vt)
FLAG_TYPE(duration, Duration, duration_vt)

#undef FLAG_TYPE
/* NOLINTEND(bugprone-macro-parentheses) */

/* Go's newTextValue. */
void flag_flag_set_text_var(FlagFlagSet *f, Any p, Str name, Any value, Str usage) {
    Alloc *ea = error_allocator();
    if (p.t == NULL || p.data == NULL)
        panic_str(LIT("variable value type must be a pointer"));
    if (value.t != p.t) {
        Str have = value.t ? type_text(ea, value.t) : LIT("<nil>");
        Str want = type_text(ea, p.t);
        panic_str(fmt_sprintf_v(
            ea, "default type does not match variable type: %s != %s", have, want));
    }
    if (value.data != p.data)
        memmove(p.data, value.data, p.t->size);
    check_name(f, name);
    TextBox *b = alloc_or_panic(f, sizeof *b, _Alignof(TextBox));
    b->p = p;
    b->a = set_alloc(f);
    define(f, (FlagValue){&text_vt, b}, name, usage, b, sizeof *b, _Alignof(TextBox));
}

static void define_func(FlagFlagSet *f, const FlagValueVT *vt, Str name, Str usage,
                        FlagFunc fn) {
    check_name(f, name);
    FlagFunc *box = alloc_or_panic(f, sizeof *box, _Alignof(FlagFunc));
    *box = fn;
    define(f, (FlagValue){vt, box}, name, usage, box, sizeof *box, _Alignof(FlagFunc));
}

void flag_flag_set_func(FlagFlagSet *f, Str name, Str usage, FlagFunc fn) {
    define_func(f, &func_vt, name, usage, fn);
}

void flag_flag_set_bool_func(FlagFlagSet *f, Str name, Str usage, FlagFunc fn) {
    define_func(f, &bool_func_vt, name, usage, fn);
}

/* -------------------------------------------------------------- parsing */

/* Go's failf: the message, then the usage, then the message as an error. */
static Error failf(FlagFlagSet *f, Str msg) {
    say(f, msg);
    call_usage(f);
    return errors_new(error_allocator(), msg);
}

static Str arg0(Slice args) {
    return ((Str *)args.p)[0];
}

/* Go's parseOne: one flag off the front of the arguments. True when there was
 * one, and false with *err set when there was a mistake, or with no error when
 * the flags have ended. */
static bool parse_one(FlagFlagSet *f, Error *err) {
    *err = BURROW_NO_ERROR;
    if (f->args.len == 0)
        return false;
    Str s = arg0(f->args);
    if (s.len < 2 || s.p[0] != '-')
        return false;
    Int minuses = 1;
    if (s.p[1] == '-') {
        minuses++;
        if (s.len == 2) { /* "--" terminates the flags */
            f->args = slice_sub(f->args, 1, f->args.len);
            return false;
        }
    }
    Str name = str_from_bytes(s.p + minuses, s.len - minuses);
    if (name.len == 0 || name.p[0] == '-' || name.p[0] == '=') {
        *err = failf(f, fmt_sprintf_v(error_allocator(), "bad flag syntax: %s", s));
        return false;
    }

    /* It's a flag. Does it have an argument? */
    f->args = slice_sub(f->args, 1, f->args.len);
    bool has_value = false;
    Str value = BURROW_STR_EMPTY;
    for (Int i = 1; i < name.len; i++) { /* equals cannot be first */
        if (name.p[i] == '=') {
            value = str_from_bytes(name.p + i + 1, name.len - i - 1);
            has_value = true;
            name = str_from_bytes(name.p, i);
            break;
        }
    }

    Flag *fl = flag_flag_set_lookup(f, name);
    if (fl == NULL) {
        if (str_eq(name, LIT("help")) || str_eq(name, LIT("h"))) {
            /* special case for nice help message. */
            call_usage(f);
            *err = flag_err_help;
            return false;
        }
        *err = failf(f, fmt_sprintf_v(error_allocator(),
                                      "flag provided but not defined: -%s", name));
        return false;
    }

    const FlagValueVT *vt = fl->value.vt;
    if (vt->is_bool_flag != NULL && vt->is_bool_flag(fl->value.data)) {
        /* special case: doesn't need an arg */
        if (has_value) {
            Error e = vt->set(fl->value.data, value);
            if (!BURROW_OK(e)) {
                *err = failf(f, fmt_sprintf_v(error_allocator(),
                                              "invalid boolean value %q for -%s: %v",
                                              value, name, e));
                return false;
            }
        } else {
            Error e = vt->set(fl->value.data, LIT("true"));
            if (!BURROW_OK(e)) {
                *err = failf(f, fmt_sprintf_v(error_allocator(),
                                              "invalid boolean flag %s: %v", name, e));
                return false;
            }
        }
    } else {
        /* It must have a value, which might be the next argument. */
        if (!has_value && f->args.len > 0) {
            /* value is the next arg */
            has_value = true;
            value = arg0(f->args);
            f->args = slice_sub(f->args, 1, f->args.len);
        }
        if (!has_value) {
            *err = failf(f, fmt_sprintf_v(error_allocator(),
                                          "flag needs an argument: -%s", name));
            return false;
        }
        Error e = vt->set(fl->value.data, value);
        if (!BURROW_OK(e)) {
            *err = failf(f, fmt_sprintf_v(error_allocator(),
                                          "invalid value %q for flag -%s: %v", value,
                                          name, e));
            return false;
        }
    }
    insert(f, &f->actual, &f->nactual, &f->capactual, fl);
    return true;
}

Error flag_flag_set_parse(FlagFlagSet *f, Slice arguments) {
    f->parsed = true;
    f->args = arguments;
    for (;;) {
        Error err;
        if (parse_one(f, &err))
            continue;
        if (BURROW_OK(err))
            break;
        switch (f->error_handling) {
        case FLAG_CONTINUE_ON_ERROR:
            return err;
        case FLAG_EXIT_ON_ERROR:
            /* Go's os.Exit, after what a C program has buffered, which Go
             * programs do not have. */
            fflush(NULL);
            pal_exit(errors_is(err, flag_err_help) ? 0 : 2);
        case FLAG_PANIC_ON_ERROR: {
            /* In the error arena rather than this frame, which is gone by
             * the time a catch block reads it. */
            Error *box = mem_alloc(error_allocator(), sizeof *box, _Alignof(Error));
            if (box == NULL)
                out_of_memory();
            *box = err;
            panic(BURROW_ANY(TYPE_ERROR, box));
        }
        default:
            return err;
        }
    }
    return BURROW_NO_ERROR;
}

bool flag_flag_set_parsed(FlagFlagSet *f) {
    return f->parsed;
}

Str flag_flag_set_arg(FlagFlagSet *f, Int i) {
    if (i < 0 || i >= f->args.len)
        return BURROW_STR_EMPTY;
    return ((Str *)f->args.p)[i];
}

Slice flag_flag_set_args(FlagFlagSet *f) {
    return f->args;
}

Int flag_flag_set_n_arg(FlagFlagSet *f) {
    return f->args.len;
}

Int flag_flag_set_n_flag(FlagFlagSet *f) {
    return f->nactual;
}

/* --------------------------------------------------------- CommandLine */

void flag_parse(void) {
    need_args();
    Slice args = {NULL, 0, 0, TYPE_STRING};
    if (os_nargs > 1)
        args = slice_from(os_args + 1, os_nargs - 1, os_nargs - 1, TYPE_STRING);
    /* Ignore errors; CommandLine is set for ExitOnError. */
    (void)flag_flag_set_parse(flag_command_line, args);
}

bool flag_parsed(void) {
    return flag_command_line->parsed;
}

Str flag_arg(Int i) {
    return flag_flag_set_arg(flag_command_line, i);
}

Slice flag_args(void) {
    return flag_command_line->args;
}

Int flag_n_arg(void) {
    return flag_command_line->args.len;
}

Int flag_n_flag(void) {
    return flag_command_line->nactual;
}

Flag *flag_lookup(Str name) {
    return flag_flag_set_lookup(flag_command_line, name);
}

Error(flag_set)(Str name, Str value) {
    return burrow__flag_set_at(flag_command_line, name, value, NULL, 0);
}

void flag_visit(FlagVisitFunc fn) {
    flag_flag_set_visit(flag_command_line, fn);
}

void flag_visit_all(FlagVisitFunc fn) {
    flag_flag_set_visit_all(flag_command_line, fn);
}

void flag_print_defaults(void) {
    flag_flag_set_print_defaults(flag_command_line);
}

void flag_text_var(Any p, Str name, Any value, Str usage) {
    flag_flag_set_text_var(flag_command_line, p, name, value, usage);
}

void flag_func(Str name, Str usage, FlagFunc fn) {
    flag_flag_set_func(flag_command_line, name, usage, fn);
}

void flag_bool_func(Str name, Str usage, FlagFunc fn) {
    flag_flag_set_bool_func(flag_command_line, name, usage, fn);
}

void flag_var(FlagValue value, Str name, Str usage) {
    flag_flag_set_var(flag_command_line, value, name, usage);
}
