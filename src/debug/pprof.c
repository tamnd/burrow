/* runtime/pprof: profiling labels, and the Profile type with the count
 * profiles it writes.
 *
 * Derived from Go's src/runtime/pprof: label.go, runtime.go, pprof.go,
 * pprof_rusage.go and pprof_norusage.go. Go source: go1.27.1.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "pprof_internal.h"

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/defer.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mprof.h"
#include "burrow/mstats.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/profbuf.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/syscall.h"
#include "burrow/text/tabwriter.h"
#include "burrow/time.h"
#include "burrow/trace.h"
#include "burrow/type.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define S_(lit) BURROW_S(lit)

static BURROW_NORETURN void ppr_out_of_memory(void) {
    panic_str(S_("runtime/pprof: out of memory"));
}

/* ----------------------------------------------------------------- labels */

#define PPROF_LABEL_FIELDS(F, T)                                                       \
    F(T, Str, key, Key, "")                                                            \
    F(T, Str, value, Value, "")
BURROW_STRUCT_AS_DEFINE(PprofLabel, PPROF_LABEL_FIELDS);

/* labelContextKey is the type of contextKeys used for profiler labels. */
static const Type ppr_label_key_desc = {
    {(const Byte *)"labelContextKey", 15},
    {(const Byte *)"runtime/pprof", 13},
    KIND_INT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x70706c6bU, /* "pplk" */
    NULL,
};

static const Int ppr_label_key_value = 0;

static const Any ppr_label_key = {&ppr_label_key_desc,
                                  (void *)(uintptr_t)&ppr_label_key_value};

/* What the context holds under the key: a *labelMap. The Any points at the
 * PprLabelMap itself, and its type is this one so that nobody else's value can
 * be mistaken for it. */
static const Type ppr_label_map_desc = {
    {(const Byte *)"labelMap", 8},
    {(const Byte *)"runtime/pprof", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(PprLabelMap),
    (uint16_t)_Alignof(PprLabelMap),
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

static const PprLabelMap *ppr_context_labels(Context ctx) {
    Any v = context_value(ctx, ppr_label_key);
    if (v.t != &ppr_label_map_desc)
        return NULL;
    return (const PprLabelMap *)v.data;
}

/* labelValue: the labels on ctx, as a list that is empty when there are
 * none. */
static Slice ppr_label_value(Context ctx) {
    const PprLabelMap *labels = ppr_context_labels(ctx);
    if (labels == NULL)
        return slice_nil(TYPE_PPROF_LABEL);
    return labels->list;
}

static int ppr_compare_strs(void *env, const void *a, const void *b) {
    (void)env;
    return (int)strings_compare(*(const Str *)a, *(const Str *)b);
}

/* String satisfies Stringer and returns key, value pairs in a consistent
 * order. */
Str burrow__ppr_label_map_string(const PprLabelMap *l, Alloc *a) {
    if (l == NULL)
        return BURROW_STR_EMPTY;
    Slice key_vals = slice_make(a, TYPE_STRING, 0, l->list.len);
    if (l->list.len > 0 && key_vals.p == NULL)
        ppr_out_of_memory();
    const PprofLabel *list = (const PprofLabel *)l->list.p;
    for (Int i = 0; i < l->list.len; i++) {
        Str kv = fmt_sprintf_v(a, "%q:%q", list[i].key, list[i].value);
        key_vals = slice_append(a, key_vals, &kv, 1);
    }
    slices_sort_func(key_vals, BURROW_FN(SlicesCmpFunc, ppr_compare_strs, NULL));
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_byte(&b, '{');
    strings_builder_write_string(&b, strings_join(a, key_vals, S_(", ")), NULL);
    strings_builder_write_byte(&b, '}');
    return strings_builder_string(&b);
}

static Slice ppr_append_label(Alloc *a, Slice list, const PprofLabel *l) {
    Int n = list.len;
    list = slice_append(a, list, l, 1);
    if (list.len != n + 1)
        ppr_out_of_memory();
    return list;
}

static Slice ppr_merge_label_sets(Alloc *a, Slice left, Slice right) {
    if (left.len == 0)
        return right;
    if (right.len == 0)
        return left;
    const PprofLabel *l_list = (const PprofLabel *)left.p;
    const PprofLabel *r_list = (const PprofLabel *)right.p;
    Int l = 0, r = 0;
    Slice result = slice_make(a, TYPE_PPROF_LABEL, 0, right.len);
    if (result.p == NULL)
        ppr_out_of_memory();
    while (l < left.len && r < right.len) {
        Int c = strings_compare(l_list[l].key, r_list[r].key);
        if (c < 0) { /* left key < right key */
            result = ppr_append_label(a, result, &l_list[l]);
            l++;
        } else if (c > 0) { /* right key < left key */
            result = ppr_append_label(a, result, &r_list[r]);
            r++;
        } else { /* keys are equal, right value overwrites left value */
            result = ppr_append_label(a, result, &r_list[r]);
            l++;
            r++;
        }
    }
    /* Append the remaining elements */
    for (; l < left.len; l++)
        result = ppr_append_label(a, result, &l_list[l]);
    for (; r < right.len; r++)
        result = ppr_append_label(a, result, &r_list[r]);
    return result;
}

Context pprof_with_labels(Alloc *a, Context ctx, PprofLabelSet labels) {
    Slice parent = ppr_label_value(ctx);
    PprLabelMap *m =
        (PprLabelMap *)mem_alloc(a, sizeof(PprLabelMap), _Alignof(PprLabelMap));
    if (m == NULL)
        ppr_out_of_memory();
    m->list = ppr_merge_label_sets(a, parent, labels.list);
    Context out =
        context_with_value(a, ctx, ppr_label_key, BURROW_ANY(&ppr_label_map_desc, m));
    if (out.vt == NULL)
        ppr_out_of_memory();
    return out;
}

static int ppr_compare_label_keys(void *env, const void *a, const void *b) {
    (void)env;
    return (int)strings_compare(((const PprofLabel *)a)->key,
                                ((const PprofLabel *)b)->key);
}

PprofLabelSet pprof_labels(Alloc *a, Slice args) {
    if (args.len % 2 != 0)
        panic_str(S_("uneven number of arguments to pprof.Labels"));
    const Str *s = (const Str *)args.p;
    Slice list = slice_make(a, TYPE_PPROF_LABEL, 0, args.len / 2);
    if (args.len > 0 && list.p == NULL)
        ppr_out_of_memory();
    bool sorted_no_dupes = true;
    for (Int i = 0; i + 1 < args.len; i += 2) {
        PprofLabel l = {s[i], s[i + 1]};
        list = ppr_append_label(a, list, &l);
        sorted_no_dupes =
            sorted_no_dupes && (i < 2 || strings_compare(s[i], s[i - 2]) > 0);
    }
    if (!sorted_no_dupes) {
        /* slow path: keys are unsorted, contain duplicates, or both */
        slices_sort_stable_func(list,
                                BURROW_FN(SlicesCmpFunc, ppr_compare_label_keys, NULL));
        PprofLabel *l = (PprofLabel *)list.p;
        Int n = 0;
        for (Int i = 0; i < list.len; i++) {
            if (i == 0 || !str_eq(l[i].key, l[i - 1].key))
                l[n++] = l[i];
            else
                l[n - 1] = l[i];
        }
        list = slice_sub(list, 0, n);
    }
    return (PprofLabelSet){list};
}

PprofLabelSet pprof_labels_v(Alloc *a, int n, ...) {
    Str stack[16];
    Str *args = stack;
    if (n < 0)
        n = 0;
    if ((size_t)n > sizeof stack / sizeof stack[0]) {
        args = (Str *)mem_alloc_array(a, (size_t)n, sizeof(Str), _Alignof(Str));
        if (args == NULL)
            ppr_out_of_memory();
    }
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        args[i] = va_arg(ap, Str);
    va_end(ap);
    PprofLabelSet set = pprof_labels(a, slice_from(args, n, n, TYPE_STRING));
    if (args != stack)
        mem_free(a, args, (size_t)n * sizeof(Str), _Alignof(Str));
    return set;
}

bool pprof_label(Context ctx, Str key, Str *value) {
    Slice ctx_labels = ppr_label_value(ctx);
    const PprofLabel *l = (const PprofLabel *)ctx_labels.p;
    for (Int i = 0; i < ctx_labels.len; i++) {
        if (str_eq(l[i].key, key)) {
            if (value != NULL)
                *value = l[i].value;
            return true;
        }
    }
    if (value != NULL)
        *value = BURROW_STR_EMPTY;
    return false;
}

void pprof_for_labels(Context ctx, PprofLabelFunc f) {
    Slice ctx_labels = ppr_label_value(ctx);
    const PprofLabel *l = (const PprofLabel *)ctx_labels.p;
    for (Int i = 0; i < ctx_labels.len; i++) {
        if (!f.f(f.env, l[i].key, l[i].value))
            break;
    }
}

void pprof_set_goroutine_labels(Context ctx) {
    burrow__set_prof_label((void *)(uintptr_t)ppr_context_labels(ctx));
}

static void ppr_restore_labels(void *env) {
    pprof_set_goroutine_labels(*(const Context *)env);
}

void pprof_do(Alloc *a, Context ctx, PprofLabelSet labels, PprofDoFunc f) {
    Context parent = ctx;
    BURROW_SCOPE {
        BURROW_DEFER(ppr_restore_labels, &parent);
        Context with = pprof_with_labels(a, parent, labels);
        pprof_set_goroutine_labels(with);
        f.f(f.env, with);
    }
    BURROW_SCOPE_END;
}

/* --------------------------------------------------------------- profiles */

/* The stack Add recorded for one value, and the copy of the value that is the
 * key it is under, so that Remove can give both back. */
typedef struct PprStack {
    Any key;
    Int n;
    Uintptr pc[32];
} PprStack;

struct PprofProfile {
    Str name;
    SyncMutex mu;
    Map *m; /* of Any to PprStack pointers */
    Int (*count)(void);
    Error (*write)(IoWriter w, Int debug);
};

/* profiles records all registered profiles. */
static SyncMutex ppr_profiles_mu;
static Map *ppr_profiles; /* of Str to PprofProfile pointers */

static Int ppr_count_goroutine(void);
static Error ppr_write_goroutine(IoWriter w, Int debug);
static Int ppr_count_thread_create(void);
static Error ppr_write_thread_create(IoWriter w, Int debug);
static Int ppr_count_heap(void);
static Error ppr_write_heap(IoWriter w, Int debug);
static Error ppr_write_alloc(IoWriter w, Int debug);
static Int ppr_count_block(void);
static Error ppr_write_block(IoWriter w, Int debug);
static Int ppr_count_mutex(void);
static Error ppr_write_mutex(IoWriter w, Int debug);

static PprofProfile ppr_goroutine_profile = {
    .name = BURROW_S_INIT("goroutine"),
    .count = ppr_count_goroutine,
    .write = ppr_write_goroutine,
};

static PprofProfile ppr_threadcreate_profile = {
    .name = BURROW_S_INIT("threadcreate"),
    .count = ppr_count_thread_create,
    .write = ppr_write_thread_create,
};

static PprofProfile ppr_heap_profile = {
    .name = BURROW_S_INIT("heap"),
    .count = ppr_count_heap,
    .write = ppr_write_heap,
};

static PprofProfile ppr_allocs_profile = {
    .name = BURROW_S_INIT("allocs"),
    .count = ppr_count_heap, /* identical to heap profile */
    .write = ppr_write_alloc,
};

static PprofProfile ppr_block_profile = {
    .name = BURROW_S_INIT("block"),
    .count = ppr_count_block,
    .write = ppr_write_block,
};

static PprofProfile ppr_mutex_profile = {
    .name = BURROW_S_INIT("mutex"),
    .count = ppr_count_mutex,
    .write = ppr_write_mutex,
};

static void ppr_lock_profiles(void) {
    sync_mutex_lock(&ppr_profiles_mu);
    if (ppr_profiles == NULL) {
        /* Initial built-in profiles. */
        PprofProfile *builtin[] = {&ppr_goroutine_profile, &ppr_threadcreate_profile,
                                   &ppr_heap_profile,      &ppr_allocs_profile,
                                   &ppr_block_profile,     &ppr_mutex_profile};
        Map *m = map_make(heap_allocator(), TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
        bool ok = m != NULL;
        for (size_t i = 0; ok && i < sizeof builtin / sizeof builtin[0]; i++) {
            void *pv = builtin[i];
            ok = map_set(m, &builtin[i]->name, &pv);
        }
        if (!ok) {
            map_free(m);
            sync_mutex_unlock(&ppr_profiles_mu);
            ppr_out_of_memory();
        }
        ppr_profiles = m;
    }
}

static void ppr_unlock_profiles(void) {
    sync_mutex_unlock(&ppr_profiles_mu);
}

static BURROW_NORETURN void ppr_panic_name(const char *msg, Str name) {
    Str text = fmt_sprintf_v(heap_allocator(), "%s%s", msg, name);
    panic_str(text);
}

PprofProfile *pprof_new_profile(Str name) {
    ppr_lock_profiles();
    if (name.len == 0) {
        ppr_unlock_profiles();
        panic_str(S_("pprof: NewProfile with empty name"));
    }
    if (map_get(ppr_profiles, &name) != NULL) {
        ppr_unlock_profiles();
        ppr_panic_name("pprof: NewProfile name already in use: ", name);
    }
    Alloc *heap = heap_allocator();
    PprofProfile *p =
        (PprofProfile *)mem_alloc(heap, sizeof(PprofProfile), _Alignof(PprofProfile));
    Str copy = str_clone(heap, name);
    Map *m = map_make(heap, TYPE_ANY, TYPE_UNSAFE_POINTER, 0);
    if (p == NULL || copy.len == 0 || m == NULL) {
        ppr_unlock_profiles();
        ppr_out_of_memory();
    }
    p->name = copy;
    p->m = m;
    void *pv = p;
    if (!map_set(ppr_profiles, &p->name, &pv)) {
        ppr_unlock_profiles();
        ppr_out_of_memory();
    }
    ppr_unlock_profiles();
    return p;
}

PprofProfile *pprof_lookup(Str name) {
    ppr_lock_profiles();
    void **p = (void **)map_get(ppr_profiles, &name);
    PprofProfile *out = p != NULL ? (PprofProfile *)*p : NULL;
    ppr_unlock_profiles();
    return out;
}

static int ppr_compare_profiles(void *env, const void *a, const void *b) {
    (void)env;
    return (int)strings_compare((*(PprofProfile *const *)a)->name,
                                (*(PprofProfile *const *)b)->name);
}

Slice pprof_profiles(Alloc *a) {
    ppr_lock_profiles();
    Slice all = slice_make(a, TYPE_UNSAFE_POINTER, 0, map_len(ppr_profiles));
    MapIter it = map_iter(ppr_profiles);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        Int n = all.len;
        all = slice_append(a, all, v, 1);
        if (all.len != n + 1) {
            ppr_unlock_profiles();
            ppr_out_of_memory();
        }
    }
    ppr_unlock_profiles();

    slices_sort_func(all, BURROW_FN(SlicesCmpFunc, ppr_compare_profiles, NULL));
    return all;
}

Str pprof_profile_name(const PprofProfile *p) {
    return p->name;
}

Int pprof_profile_count(PprofProfile *p) {
    sync_mutex_lock(&p->mu);
    Int n = p->count != NULL ? p->count() : map_len(p->m);
    sync_mutex_unlock(&p->mu);
    return n;
}

static void ppr_free_stack(PprStack *s) {
    Alloc *heap = heap_allocator();
    if (s->key.t != NULL && s->key.data != NULL)
        mem_free(heap, s->key.data, s->key.t->size, s->key.t->align);
    mem_free(heap, s, sizeof(PprStack), _Alignof(PprStack));
}

void pprof_profile_add(PprofProfile *p, Any value, Int skip) {
    if (p->name.len == 0)
        panic_str(S_("pprof: use of uninitialized Profile"));
    if (p->write != NULL)
        ppr_panic_name("pprof: Add called on built-in Profile ", p->name);

    Alloc *heap = heap_allocator();
    PprStack *stk = (PprStack *)mem_alloc(heap, sizeof(PprStack), _Alignof(PprStack));
    if (stk == NULL)
        ppr_out_of_memory();
    stk->n = runtime_callers(skip + 1, slice_from(stk->pc, 32, 32, TYPE_UINTPTR));
    if (stk->n == 0) {
        /* The value for skip is too large, and there's no stack trace to
         * record. */
        stk->pc[0] = (Uintptr)burrow__pprof_lost_profile_event;
        stk->n = 1;
    }
    /* A nil value, typed or not, has nothing to copy and is its own key. */
    stk->key = value.data != NULL ? any_box(heap, value) : value;
    if (stk->key.t != value.t) {
        mem_free(heap, stk, sizeof(PprStack), _Alignof(PprStack));
        ppr_out_of_memory();
    }

    sync_mutex_lock(&p->mu);
    if (map_get(p->m, &stk->key) != NULL) {
        sync_mutex_unlock(&p->mu);
        ppr_free_stack(stk);
        panic_str(S_("pprof: Profile.Add of duplicate value"));
    }
    void *sv = stk;
    bool ok = map_set(p->m, &stk->key, &sv);
    sync_mutex_unlock(&p->mu);
    if (!ok) {
        ppr_free_stack(stk);
        ppr_out_of_memory();
    }
}

void pprof_profile_remove(PprofProfile *p, Any value) {
    sync_mutex_lock(&p->mu);
    void **sv = (void **)map_get(p->m, &value);
    PprStack *stk = sv != NULL ? (PprStack *)*sv : NULL;
    if (stk != NULL)
        map_del(p->m, &value);
    sync_mutex_unlock(&p->mu);
    if (stk != NULL)
        ppr_free_stack(stk);
}

/* printStackRecord prints the function + source line information for a single
 * stack trace.
 *
 * Go's CallersFrames leaves out an address it has no function for, and only
 * prints the line with no name when there is nothing at all. burrow has no
 * name for anything outside its own symbol table, which is most of a C
 * program, so such a frame is printed as that line rather than dropped. */
static void ppr_print_stack_record(IoWriter w, Slice stk, bool all_frames) {
    bool show = all_frames;
    const Uintptr *pcs = (const Uintptr *)stk.p;
    Int n = stk.len > 0 ? stk.len : 1;
    for (Int i = 0; i < n; i++) {
        RuntimeFrame frame;
        if (stk.len > 0)
            burrow__ppr_frame(pcs[i], &frame);
        else
            memset(&frame, 0, sizeof frame);
        Str name = frame.function;
        if (name.len == 0) {
            show = true;
            fmt_fprintf_v(w, "#\t%#x\n", frame.pc);
        } else if (!str_eq(name, S_("runtime.goexit")) &&
                   (show || !burrow__ppr_alloc_frame(name))) {
            /* Hide runtime.goexit and any runtime functions at the beginning.
             * This is useful mainly for allocation traces. */
            show = true;
            fmt_fprintf_v(w, "#\t%#x\t%s+%#x\t%s:%d\n", frame.pc, name,
                          frame.pc - frame.entry, frame.file, frame.line);
        }
    }
    if (!show) {
        /* We didn't print anything; do it again, and this time include runtime
         * functions. */
        ppr_print_stack_record(w, stk, true);
        return;
    }
    fmt_fprintf_v(w, "\n");
}

typedef struct PprKeyOrder {
    Map *count;
} PprKeyOrder;

static Int ppr_count_of(Map *count, const Str *key) {
    const Int *c = (const Int *)map_get(count, key);
    return c != NULL ? *c : 0;
}

/* keysByCount sorts keys with higher counts first, breaking ties by key string
 * order. */
static int ppr_compare_keys(void *env, const void *a, const void *b) {
    const PprKeyOrder *o = (const PprKeyOrder *)env;
    const Str *ki = (const Str *)a;
    const Str *kj = (const Str *)b;
    Int ci = ppr_count_of(o->count, ki);
    Int cj = ppr_count_of(o->count, kj);
    if (ci != cj)
        return ci > cj ? -1 : 1;
    return (int)strings_compare(*ki, *kj);
}

/* printCountProfile prints a countProfile at the specified debug level. The
 * profile will be in compressed proto format unless debug is nonzero.
 *
 * Go's countProfile is an interface. Here it is n stacks, each a slice of
 * Uintptr, and labels, which is NULL or n label maps that may each be
 * NULL. */
static Error ppr_print_count_profile(Alloc *a, IoWriter w, Int debug, Str name,
                                     const Slice *stacks,
                                     const PprLabelMap *const *labels, Int n) {
    /* Build count of each stack. */
    StringsBuilder buf = STRINGS_BUILDER(a);
    IoWriter bw = strings_builder_as_io_writer(&buf);
    Map *count = map_make(a, TYPE_STRING, TYPE_INT, n);
    Map *index = map_make(a, TYPE_STRING, TYPE_INT, n);
    Slice keys = slice_make(a, TYPE_STRING, 0, n);
    if (count == NULL || index == NULL || (n > 0 && keys.p == NULL))
        ppr_out_of_memory();
    for (Int i = 0; i < n; i++) {
        strings_builder_reset(&buf);
        fmt_fprintf_v(bw, "@");
        const Uintptr *pcs = (const Uintptr *)stacks[i].p;
        for (Int j = 0; j < stacks[i].len; j++)
            fmt_fprintf_v(bw, " %#x", pcs[j]);
        if (labels != NULL && labels[i] != NULL) {
            strings_builder_write_string(&buf, S_("\n# labels: "), NULL);
            strings_builder_write_string(
                &buf, burrow__ppr_label_map_string(labels[i], a), NULL);
        }
        Str k = str_clone(a, strings_builder_string(&buf));
        if (k.len == 0)
            ppr_out_of_memory();
        Int c = ppr_count_of(count, &k);
        if (c == 0) {
            if (!map_set(index, &k, &i))
                ppr_out_of_memory();
            Int nk = keys.len;
            keys = slice_append(a, keys, &k, 1);
            if (keys.len != nk + 1)
                ppr_out_of_memory();
        }
        c++;
        if (!map_set(count, &k, &c))
            ppr_out_of_memory();
    }

    PprKeyOrder order = {count};
    slices_sort_func(keys, BURROW_FN(SlicesCmpFunc, ppr_compare_keys, &order));
    const Str *key = (const Str *)keys.p;

    if (debug > 0) {
        /* Print debug profile in legacy format */
        TabwriterWriter *tw = tabwriter_new_writer(a, w, 1, 8, 1, '\t', 0);
        if (tw == NULL)
            ppr_out_of_memory();
        IoWriter tww = tabwriter_writer_as_io_writer(tw);
        fmt_fprintf_v(tww, "%s profile: total %d\n", name, n);
        for (Int i = 0; i < keys.len; i++) {
            fmt_fprintf_v(tww, "%d %s\n", ppr_count_of(count, &key[i]), key[i]);
            Int idx = *(const Int *)map_get(index, &key[i]);
            ppr_print_stack_record(tww, stacks[idx], false);
        }
        Error err = tabwriter_writer_flush(tw);
        tabwriter_writer_free(tw);
        return err;
    }

    /* Output profile in protobuf form. */
    PprBuilder *b = burrow__ppr_new_builder(w);
    burrow__ppr_value_type(b, PPR_TAG_PROFILE_PERIOD_TYPE, name, S_("count"));
    burrow__ppr_int64_opt(b, PPR_TAG_PROFILE_PERIOD, 1);
    burrow__ppr_value_type(b, PPR_TAG_PROFILE_SAMPLE_TYPE, name, S_("count"));

    int64_t values[1] = {0};
    Slice locs = slice_nil(TYPE_UINT64);
    for (Int i = 0; i < keys.len; i++) {
        values[0] = (int64_t)ppr_count_of(count, &key[i]);
        Int idx = *(const Int *)map_get(index, &key[i]);
        /* For count profiles, all stack addresses are return PCs, which is
         * what appendLocsForStack expects. */
        locs = burrow__ppr_append_locs_for_stack(b, slice_sub(locs, 0, 0), stacks[idx]);
        burrow__ppr_sample(b, slice_from(values, 1, 1, TYPE_INT64), locs,
                           labels != NULL ? labels[idx] : NULL);
    }
    return burrow__ppr_build(b);
}

/* Orders pointers to stacks, as slices.Compare orders the stacks. */
static int ppr_compare_stacks(void *env, const void *a, const void *b) {
    (void)env;
    const Slice *x = *(const Slice *const *)a;
    const Slice *y = *(const Slice *const *)b;
    const Uintptr *xp = (const Uintptr *)x->p;
    const Uintptr *yp = (const Uintptr *)y->p;
    for (Int i = 0; i < x->len && i < y->len; i++) {
        if (xp[i] != yp[i])
            return xp[i] < yp[i] ? -1 : 1;
    }
    if (x->len != y->len)
        return x->len < y->len ? -1 : 1;
    return 0;
}

static void ppr_free_arena(void *env) {
    arena_free((Arena *)env);
}

Error pprof_profile_write_to(PprofProfile *p, IoWriter w, Int debug) {
    if (p->name.len == 0)
        panic_str(S_("pprof: use of zero Profile"));
    if (p->write != NULL)
        return p->write(w, debug);

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    BURROW_SCOPE {
        BURROW_DEFER(ppr_free_arena, &ar);

        /* Obtain consistent snapshot under lock; then process without lock. */
        sync_mutex_lock(&p->mu);
        Int n = map_len(p->m);
        size_t cap = (size_t)(n > 0 ? n : 1);
        Slice *all = (Slice *)mem_alloc_array(a, cap, sizeof(Slice), _Alignof(Slice));
        Slice **order =
            (Slice **)mem_alloc_array(a, cap, sizeof(Slice *), _Alignof(Slice *));
        Uintptr *pcs =
            (Uintptr *)mem_alloc_array(a, cap * 32, sizeof(Uintptr), _Alignof(Uintptr));
        if (all == NULL || order == NULL || pcs == NULL) {
            sync_mutex_unlock(&p->mu);
            ppr_out_of_memory();
        }
        Int i = 0;
        MapIter it = map_iter(p->m);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            const PprStack *stk = *(PprStack *const *)v;
            Uintptr *dst = pcs + i * 32;
            memcpy(dst, stk->pc, (size_t)stk->n * sizeof(Uintptr));
            all[i] = slice_from(dst, stk->n, stk->n, TYPE_UINTPTR);
            order[i] = &all[i];
            i++;
        }
        sync_mutex_unlock(&p->mu);

        /* Map order is non-deterministic; make output deterministic. */
        slices_sort_func(slice_from(order, n, n, TYPE_UNSAFE_POINTER),
                         BURROW_FN(SlicesCmpFunc, ppr_compare_stacks, NULL));
        Slice *sorted =
            (Slice *)mem_alloc_array(a, cap, sizeof(Slice), _Alignof(Slice));
        if (sorted == NULL)
            ppr_out_of_memory();
        for (Int j = 0; j < n; j++)
            sorted[j] = *order[j];

        err = ppr_print_count_profile(a, w, debug, p->name, sorted, NULL, n);
    }
    BURROW_SCOPE_END;
    return err;
}

/* ------------------------------------------------------- runtime profiles */

/* The records a runtime profile hands over, gathered for printCountProfile.
 * Go sizes its slice with a first call that only counts and retries if the
 * profile grew in between. The runtime here calls back once per record
 * instead, so the slice just grows. */
typedef struct PprRecords {
    Alloc *a;
    burrow__StackRecord *r;
    Int n;
    Int cap;
} PprRecords;

static void ppr_collect(void *env, const burrow__StackRecord *r) {
    PprRecords *rs = (PprRecords *)env;
    if (rs->n == rs->cap) {
        Int cap = rs->cap > 0 ? 2 * rs->cap : 16;
        burrow__StackRecord *grown = (burrow__StackRecord *)mem_alloc_array(
            rs->a, (size_t)cap, sizeof(burrow__StackRecord),
            _Alignof(burrow__StackRecord));
        if (grown == NULL)
            ppr_out_of_memory();
        if (rs->n > 0)
            memcpy(grown, rs->r, (size_t)rs->n * sizeof(burrow__StackRecord));
        rs->r = grown;
        rs->cap = cap;
    }
    rs->r[rs->n++] = *r;
}

static Error ppr_write_runtime_profile(IoWriter w, Int debug, Str name,
                                       void (*fetch)(PprRecords *rs)) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    BURROW_SCOPE {
        BURROW_DEFER(ppr_free_arena, &ar);

        PprRecords rs = {a, NULL, 0, 0};
        fetch(&rs);

        size_t cap = (size_t)(rs.n > 0 ? rs.n : 1);
        Slice *stacks =
            (Slice *)mem_alloc_array(a, cap, sizeof(Slice), _Alignof(Slice));
        const PprLabelMap **labels = (const PprLabelMap **)mem_alloc_array(
            a, cap, sizeof(PprLabelMap *), _Alignof(PprLabelMap *));
        if (stacks == NULL || labels == NULL)
            ppr_out_of_memory();
        for (Int i = 0; i < rs.n; i++) {
            Int n = rs.r[i].n;
            stacks[i] = slice_from(rs.r[i].pcs, n, n, TYPE_UINTPTR);
            labels[i] = (const PprLabelMap *)rs.r[i].labels;
        }

        err = ppr_print_count_profile(a, w, debug, name, stacks, labels, rs.n);
    }
    BURROW_SCOPE_END;
    return err;
}

/* countThreadCreate returns the size of the current ThreadCreateProfile. */
static Int ppr_count_thread_create(void) {
    return burrow__thread_count();
}

static void ppr_fetch_thread_create(PprRecords *rs) {
    burrow__thread_create_profile(ppr_collect, rs);
}

/* writeThreadCreate writes the current runtime ThreadCreateProfile to w. */
static Error ppr_write_thread_create(IoWriter w, Int debug) {
    return ppr_write_runtime_profile(w, debug, S_("threadcreate"),
                                     ppr_fetch_thread_create);
}

/* countHeap returns the number of records in the heap profile. */
static Int ppr_count_heap(void) {
    return runtime_mem_profile(NULL, 0, true, NULL);
}

/* The records pprof_memProfileInternal copies out, into room made for them
 * before, as the copying is done under the profile's lock. */
typedef struct PprMemRecords {
    burrow__MemProfileRecord *r;
    Int n;
} PprMemRecords;

static void ppr_collect_mem(void *env, const burrow__MemProfileRecord *r) {
    PprMemRecords *rs = (PprMemRecords *)env;
    rs->r[rs->n++] = *r;
}

static int64_t ppr_in_use_bytes(const burrow__MemProfileRecord *r) {
    return (r->alloc_objects - r->free_objects) * r->object_size;
}

static int ppr_compare_in_use(void *env, const void *a, const void *b) {
    (void)env;
    int64_t x = ppr_in_use_bytes(*(const burrow__MemProfileRecord *const *)a);
    int64_t y = ppr_in_use_bytes(*(const burrow__MemProfileRecord *const *)b);
    return x < y ? -1 : x > y ? 1 : 0;
}

/* addMaxRSS. Go has it on unix and Windows. Here it is on the systems whose
 * getrusage burrow's syscall has. */
static void ppr_add_max_rss(IoWriter w) {
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_FREEBSD) ||                          \
    defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
    Uintptr rss_to_bytes = 1;
#else
    Uintptr rss_to_bytes = 1024;
#endif
    /* Zeroed first because the call goes straight to the kernel, so
     * MemorySanitizer never sees it fill the struct in. */
    SyscallRusage rusage = {0};
    Error err = syscall_getrusage(SYSCALL_RUSAGE_SELF, &rusage);
    if (BURROW_OK(err))
        fmt_fprintf_v(w, "# MaxRSS = %d\n", (Uintptr)rusage.maxrss * rss_to_bytes);
#else
    (void)w;
#endif
}

/* Go prints an array with %d as its elements in brackets. */
static void ppr_print_pauses(IoWriter w, const char *name,
                             const RuntimeMemStatsPauses *p) {
    fmt_fprintf_v(w, "# %s = [", name);
    for (size_t i = 0; i < sizeof p->v / sizeof p->v[0]; i++)
        fmt_fprintf_v(w, i == 0 ? "%d" : " %d", p->v[i]);
    fmt_fprintf_v(w, "]\n");
}

/* The debug != 0 half of writeHeapInternal. */
static Error ppr_write_heap_text(Alloc *a, IoWriter w,
                                 const burrow__MemProfileRecord *p, Int n,
                                 const RuntimeMemStats *s) {
    size_t cap = (size_t)(n > 0 ? n : 1);
    const burrow__MemProfileRecord **order =
        (const burrow__MemProfileRecord **)mem_alloc_array(
            a, cap, sizeof(burrow__MemProfileRecord *),
            _Alignof(burrow__MemProfileRecord *));
    if (order == NULL)
        ppr_out_of_memory();
    for (Int i = 0; i < n; i++)
        order[i] = &p[i];
    slices_sort_func(slice_from(order, n, n, TYPE_UNSAFE_POINTER),
                     BURROW_FN(SlicesCmpFunc, ppr_compare_in_use, NULL));

    TabwriterWriter *tw = tabwriter_new_writer(a, w, 1, 8, 1, '\t', 0);
    if (tw == NULL)
        ppr_out_of_memory();
    w = tabwriter_writer_as_io_writer(tw);

    int64_t alloc_bytes = 0, alloc_objects = 0, free_bytes = 0, free_objects = 0;
    for (Int i = 0; i < n; i++) {
        const burrow__MemProfileRecord *r = &p[i];
        alloc_bytes += r->alloc_objects * r->object_size;
        alloc_objects += r->alloc_objects;
        free_bytes += r->free_objects * r->object_size;
        free_objects += r->free_objects;
    }

    /* Technically the rate is MemProfileRate not 2*MemProfileRate, but
     * early versions of the C++ heap profiler reported 2*MemProfileRate,
     * so that's what pprof has come to expect. */
    Int rate = 2 * runtime_mem_profile_rate();

    /* pprof reads a profile with alloc == inuse as being a "2-column"
     * profile (objects and bytes, not distinguishing alloc from inuse),
     * but then such a profile can't be merged using pprof *.prof with
     * other 4-column profiles where alloc != inuse. The easiest way to
     * avoid this bug is to adjust allocBytes so it's never == inuseBytes.
     * pprof doesn't use these header values anymore except for checking
     * equality. */
    int64_t in_use_bytes = alloc_bytes - free_bytes;
    if (in_use_bytes == alloc_bytes)
        alloc_bytes++;

    fmt_fprintf_v(w, "heap profile: %d: %d [%d: %d] @ heap/%d\n",
                  alloc_objects - free_objects, in_use_bytes, alloc_objects,
                  alloc_bytes, rate);

    for (Int i = 0; i < n; i++) {
        const burrow__MemProfileRecord *r = order[i];
        fmt_fprintf_v(w, "%d: %d [%d: %d] @", r->alloc_objects - r->free_objects,
                      ppr_in_use_bytes(r), r->alloc_objects,
                      r->alloc_objects * r->object_size);
        for (Int j = 0; j < r->nstack; j++)
            fmt_fprintf_v(w, " %#x", r->stack[j]);
        fmt_fprintf_v(w, "\n");
        ppr_print_stack_record(
            w,
            slice_from((void *)(uintptr_t)r->stack, r->nstack, r->nstack, TYPE_UINTPTR),
            false);
    }

    /* Print memstats information too. Pprof will ignore, but useful for
     * people */
    fmt_fprintf_v(w, "\n# runtime.MemStats\n");
    fmt_fprintf_v(w, "# Alloc = %d\n", s->alloc);
    fmt_fprintf_v(w, "# TotalAlloc = %d\n", s->total_alloc);
    fmt_fprintf_v(w, "# Sys = %d\n", s->sys);
    fmt_fprintf_v(w, "# Lookups = %d\n", s->lookups);
    fmt_fprintf_v(w, "# Mallocs = %d\n", s->mallocs);
    fmt_fprintf_v(w, "# Frees = %d\n", s->frees);

    fmt_fprintf_v(w, "# HeapAlloc = %d\n", s->heap_alloc);
    fmt_fprintf_v(w, "# HeapSys = %d\n", s->heap_sys);
    fmt_fprintf_v(w, "# HeapIdle = %d\n", s->heap_idle);
    fmt_fprintf_v(w, "# HeapInuse = %d\n", s->heap_inuse);
    fmt_fprintf_v(w, "# HeapReleased = %d\n", s->heap_released);
    fmt_fprintf_v(w, "# HeapObjects = %d\n", s->heap_objects);

    fmt_fprintf_v(w, "# Stack = %d / %d\n", s->stack_inuse, s->stack_sys);
    fmt_fprintf_v(w, "# MSpan = %d / %d\n", s->m_span_inuse, s->m_span_sys);
    fmt_fprintf_v(w, "# MCache = %d / %d\n", s->m_cache_inuse, s->m_cache_sys);
    fmt_fprintf_v(w, "# BuckHashSys = %d\n", s->buck_hash_sys);
    fmt_fprintf_v(w, "# GCSys = %d\n", s->gc_sys);
    fmt_fprintf_v(w, "# OtherSys = %d\n", s->other_sys);

    fmt_fprintf_v(w, "# NextGC = %d\n", s->next_gc);
    fmt_fprintf_v(w, "# LastGC = %d\n", s->last_gc);
    ppr_print_pauses(w, "PauseNs", &s->pause_ns);
    ppr_print_pauses(w, "PauseEnd", &s->pause_end);
    fmt_fprintf_v(w, "# NumGC = %d\n", s->num_gc);
    fmt_fprintf_v(w, "# NumForcedGC = %d\n", s->num_forced_gc);
    fmt_fprintf_v(w, "# GCCPUFraction = %v\n", s->gccpu_fraction);
    fmt_fprintf_v(w, "# DebugGC = %v\n", s->debug_gc);

    /* Also flush out MaxRSS on supported platforms. */
    ppr_add_max_rss(w);

    Error err = tabwriter_writer_flush(tw);
    tabwriter_writer_free(tw);
    return err;
}

static Error ppr_write_heap_internal(IoWriter w, Int debug, Str default_sample_type) {
    RuntimeMemStats mem_stats;
    if (debug != 0) {
        /* Read mem stats first, so that our other allocations do not appear in
         * the statistics. */
        runtime_read_mem_stats(&mem_stats);
    }

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    BURROW_SCOPE {
        BURROW_DEFER(ppr_free_arena, &ar);

        /* Find out how many records there are (the call
         * pprof_memProfileInternal(nil, true) below), allocate that many
         * records, and get the data. There's a race, more records might be
         * added between the two calls, so allocate a few extra records for
         * safety and also try again if we're very unlucky. The loop should
         * only execute one iteration in the common case. */
        PprMemRecords rs = {NULL, 0};
        bool ok;
        Int n = burrow__mem_profile(0, true, ppr_collect_mem, &rs, &ok);
        for (;;) {
            /* Allocate room for a slightly bigger profile, in case a few more
             * entries have been added since the call to MemProfile. */
            rs.r = (burrow__MemProfileRecord *)mem_alloc_array(
                a, (size_t)(n + 50), sizeof(burrow__MemProfileRecord),
                _Alignof(burrow__MemProfileRecord));
            if (rs.r == NULL)
                ppr_out_of_memory();
            rs.n = 0;
            n = burrow__mem_profile(n + 50, true, ppr_collect_mem, &rs, &ok);
            if (ok)
                break;
            /* Profile grew; try again. */
        }
        const burrow__MemProfileRecord *p = rs.r;

        if (debug == 0)
            err = burrow__ppr_write_heap_proto(w, p, n, runtime_mem_profile_rate(),
                                               default_sample_type);
        else
            err = ppr_write_heap_text(a, w, p, n, &mem_stats);
    }
    BURROW_SCOPE_END;
    return err;
}

Error pprof_write_heap_profile(IoWriter w) {
    return ppr_write_heap(w, 0);
}

/* writeHeap writes the current runtime heap profile to w. */
static Error ppr_write_heap(IoWriter w, Int debug) {
    return ppr_write_heap_internal(w, debug, S_(""));
}

/* writeAlloc writes the current runtime heap profile to w with the total
 * allocation space as the default sample type. */
static Error ppr_write_alloc(IoWriter w, Int debug) {
    return ppr_write_heap_internal(w, debug, S_("alloc_space"));
}

/* countBlock returns the number of records in the blocking profile. */
static Int ppr_count_block(void) {
    return runtime_block_profile(NULL, 0, NULL);
}

/* countMutex returns the number of records in the mutex profile. */
static Int ppr_count_mutex(void) {
    return runtime_mutex_profile(NULL, 0, NULL);
}

/* The records pprof_blockProfileInternal and pprof_mutexProfileInternal copy
 * out, into room made for them before, as the copying is done under the
 * profile's lock. */
typedef struct PprBlockRecords {
    burrow__BlockProfileRecord *r;
    Int n;
} PprBlockRecords;

static void ppr_collect_block(void *env, const burrow__BlockProfileRecord *r) {
    PprBlockRecords *rs = (PprBlockRecords *)env;
    rs->r[rs->n++] = *r;
}

static int ppr_compare_cycles(void *env, const void *a, const void *b) {
    (void)env;
    int64_t x = (*(const burrow__BlockProfileRecord *const *)a)->cycles;
    int64_t y = (*(const burrow__BlockProfileRecord *const *)b)->cycles;
    return y < x ? -1 : y > x ? 1 : 0;
}

/* printCountCycleProfile outputs block profile records (for block or mutex
 * profiles) as the pprof-proto format output. Translations from cycle count to
 * time duration are done because The proto expects count and time
 * (nanoseconds) instead of count and the number of cycles for block,
 * contention profiles. */
static Error ppr_print_count_cycle_profile(IoWriter w, Str count_name, Str cycle_name,
                                           burrow__BlockProfileRecord *const *records,
                                           Int n) {
    /* Output profile in protobuf form. */
    PprBuilder *b = burrow__ppr_new_builder(w);
    burrow__ppr_value_type(b, PPR_TAG_PROFILE_PERIOD_TYPE, count_name, S_("count"));
    burrow__ppr_int64_opt(b, PPR_TAG_PROFILE_PERIOD, 1);
    burrow__ppr_value_type(b, PPR_TAG_PROFILE_SAMPLE_TYPE, count_name, S_("count"));
    burrow__ppr_value_type(b, PPR_TAG_PROFILE_SAMPLE_TYPE, cycle_name,
                           S_("nanoseconds"));

    double cpu_ghz = (double)burrow__ticks_per_second() / 1e9;

    int64_t values[2] = {0, 0};
    Slice locs = slice_nil(TYPE_UINT64);
    for (Int i = 0; i < n; i++) {
        const burrow__BlockProfileRecord *r = records[i];
        values[0] = r->count;
        values[1] = (int64_t)((double)r->cycles / cpu_ghz);
        /* For count profiles, all stack addresses are return PCs, which is
         * what appendLocsForStack expects. */
        locs = burrow__ppr_append_locs_for_stack(b, slice_sub(locs, 0, 0),
                                                 slice_from((void *)(uintptr_t)r->stack,
                                                            r->nstack, r->nstack,
                                                            TYPE_UINTPTR));
        burrow__ppr_sample(b, slice_from(values, 2, 2, TYPE_INT64), locs, NULL);
    }
    return burrow__ppr_build(b);
}

/* writeProfileInternal writes the current blocking or mutex profile depending
 * on the passed parameters. */
static Error ppr_write_profile_internal(
    IoWriter w, Int debug, Str name,
    Int (*runtime_profile)(Int size, burrow__BlockProfileFn fn, void *env, bool *ok)) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    BURROW_SCOPE {
        BURROW_DEFER(ppr_free_arena, &ar);

        PprBlockRecords rs = {NULL, 0};
        bool ok;
        Int n = runtime_profile(0, ppr_collect_block, &rs, &ok);
        for (;;) {
            rs.r = (burrow__BlockProfileRecord *)mem_alloc_array(
                a, (size_t)(n + 50), sizeof(burrow__BlockProfileRecord),
                _Alignof(burrow__BlockProfileRecord));
            if (rs.r == NULL)
                ppr_out_of_memory();
            rs.n = 0;
            n = runtime_profile(n + 50, ppr_collect_block, &rs, &ok);
            if (ok)
                break;
        }
        burrow__BlockProfileRecord **p = (burrow__BlockProfileRecord **)mem_alloc_array(
            a, (size_t)(n > 0 ? n : 1), sizeof(burrow__BlockProfileRecord *),
            _Alignof(burrow__BlockProfileRecord *));
        if (p == NULL)
            ppr_out_of_memory();
        for (Int i = 0; i < n; i++)
            p[i] = &rs.r[i];

        slices_sort_func(slice_from(p, n, n, TYPE_UNSAFE_POINTER),
                         BURROW_FN(SlicesCmpFunc, ppr_compare_cycles, NULL));

        if (debug <= 0) {
            err =
                ppr_print_count_cycle_profile(w, S_("contentions"), S_("delay"), p, n);
        } else {
            TabwriterWriter *tw = tabwriter_new_writer(a, w, 1, 8, 1, '\t', 0);
            if (tw == NULL)
                ppr_out_of_memory();
            IoWriter tww = tabwriter_writer_as_io_writer(tw);

            fmt_fprintf_v(tww, "--- %v:\n", name);
            fmt_fprintf_v(tww, "cycles/second=%v\n", burrow__ticks_per_second());
            if (str_eq(name, S_("mutex")))
                fmt_fprintf_v(tww, "sampling period=%d\n",
                              runtime_set_mutex_profile_fraction(-1));
            for (Int i = 0; i < n; i++) {
                const burrow__BlockProfileRecord *r = p[i];
                fmt_fprintf_v(tww, "%v %v @", r->cycles, r->count);
                for (Int j = 0; j < r->nstack; j++)
                    fmt_fprintf_v(tww, " %#x", r->stack[j]);
                fmt_fprintf_v(tww, "\n");
                ppr_print_stack_record(tww,
                                       slice_from((void *)(uintptr_t)r->stack,
                                                  r->nstack, r->nstack, TYPE_UINTPTR),
                                       true);
            }

            err = tabwriter_writer_flush(tw);
            tabwriter_writer_free(tw);
        }
    }
    BURROW_SCOPE_END;
    return err;
}

/* writeBlock writes the current blocking profile to w. */
static Error ppr_write_block(IoWriter w, Int debug) {
    return ppr_write_profile_internal(w, debug, S_("contention"),
                                      burrow__block_profile);
}

/* writeMutex writes the current mutex profile to w. */
static Error ppr_write_mutex(IoWriter w, Int debug) {
    return ppr_write_profile_internal(w, debug, S_("mutex"), burrow__mutex_profile);
}

/* countGoroutine returns the number of goroutines. */
static Int ppr_count_goroutine(void) {
    return runtime_numgoroutine();
}

/* The goroutine asking goes first, as in Go, which records its own stack before
 * it looks at any other. A thread outside the scheduler isn't a goroutine and
 * isn't in the profile. */
BURROW_NOINLINE static void ppr_fetch_goroutines(PprRecords *rs) {
    if (burrow__curg() != NULL) {
        burrow__StackRecord self;
        memset(&self, 0, sizeof self);
        self.labels = burrow__get_prof_label();
        self.n =
            (int32_t)runtime_callers(1, slice_from(self.pcs, BURROW_PROFILE_DEPTH,
                                                   BURROW_PROFILE_DEPTH, TYPE_UINTPTR));
        ppr_collect(rs, &self);
    }
    burrow__goroutine_profile(ppr_collect, rs);
}

static Error ppr_write_goroutine_stacks(IoWriter w) {
    /* We don't know how big the buffer needs to be to collect all the
     * goroutines. Start with 1 MB and try a few times, doubling each time. Give
     * up and use a truncated trace if 64 MB is not enough.
     *
     * runtime_stack drops a line that doesn't fit instead of filling the
     * buffer, so a buffer counts as big enough only when there is room left
     * for the longest line it could have dropped. */
    Alloc *heap = heap_allocator();
    Int size = 1 << 20;
    Byte *buf = NULL;
    Int n = 0;
    for (;;) {
        buf = (Byte *)mem_alloc(heap, (size_t)size, 1);
        if (buf == NULL)
            ppr_out_of_memory();
        n = runtime_stack(slice_from(buf, size, size, TYPE_BYTE), true);
        if (n <= size - BURROW_FRAME_TEXT_MAX)
            break;
        if (size >= 64 << 20) {
            /* Filled 64 MB - stop there. */
            break;
        }
        mem_free(heap, buf, (size_t)size, 1);
        size *= 2;
    }
    Error err = BURROW_NO_ERROR;
    w.vt->write(w.data, slice_from(buf, n, n, TYPE_BYTE), &err);
    mem_free(heap, buf, (size_t)size, 1);
    return err;
}

/* writeGoroutine writes the current runtime GoroutineProfile to w. */
static Error ppr_write_goroutine(IoWriter w, Int debug) {
    if (debug >= 2)
        return ppr_write_goroutine_stacks(w);
    return ppr_write_runtime_profile(w, debug, S_("goroutine"), ppr_fetch_goroutines);
}

/* -------------------------------------------------------------------- cpu */

typedef struct PprCpu {
    SyncMutex mu;
    bool profiling;
    Chan *done; /* of bool */
} PprCpu;

static PprCpu ppr_cpu;

BURROW_SENTINEL_ERROR(ppr_err_cpu_in_use, "cpu profiling already in use");

/* Not in Go, which ends the program when it cannot start a goroutine. */
BURROW_SENTINEL_ERROR(ppr_err_cpu_no_writer, "cpu profiling: cannot start the writer");

typedef struct PprCpuWriter {
    IoWriter w;
} PprCpuWriter;

static void ppr_profile_writer(void *env) {
    PprCpuWriter *pw = (PprCpuWriter *)env;
    PprBuilder *b = burrow__ppr_new_builder(pw->w);
    mem_free(heap_allocator(), pw, sizeof *pw, _Alignof(PprCpuWriter));
    Error err = BURROW_NO_ERROR;
    for (;;) {
        /* Go sleeps here only on darwin and ios, where its read cannot block.
         * burrow__read_cpu_profile never blocks, so this sleeps everywhere. */
        time_sleep(100 * TIME_MILLISECOND);
        burrow__ProfBufRead r = burrow__read_cpu_profile();
        Slice data =
            slice_from((void *)(uintptr_t)r.data, r.ndata, r.ndata, TYPE_UINT64);
        Slice tags = slice_from((void *)(uintptr_t)r.tags, r.ntags, r.ntags,
                                TYPE_UNSAFE_POINTER);
        Error e = burrow__ppr_add_cpu_data(b, data, tags);
        if (BURROW_FAILED(e) && BURROW_OK(err))
            err = e;
        if (r.eof)
            break;
    }
    if (BURROW_FAILED(err)) {
        /* The runtime should never produce an invalid or truncated profile.
         * It drops records that can't fit into its log buffers. */
        burrow__ppr_builder_free(b);
        ppr_panic_name("runtime/pprof: converting profile: ", error_text(err));
    }
    (void)burrow__ppr_build(b);
    bool done = true;
    chan_send(ppr_cpu.done, &done);
}

Error pprof_start_cpu_profile(IoWriter w) {
    /* The runtime routines allow a variable profiling rate, but in practice
     * operating systems cannot trigger signals at more than about 500 Hz, and
     * our processing of the signal is not cheap (mostly getting the stack
     * trace). 100 Hz is a reasonable choice: it is frequent enough to produce
     * useful data, rare enough not to bog down the system, and a nice round
     * number to make it easy to convert sample counts to seconds. Instead of
     * requiring each client to specify the frequency, we hard code it. */
    enum { hz = 100 };

    sync_mutex_lock(&ppr_cpu.mu);
    if (ppr_cpu.done == NULL) {
        ppr_cpu.done = chan_make(heap_allocator(), TYPE_BOOL, 0);
        if (ppr_cpu.done == NULL) {
            sync_mutex_unlock(&ppr_cpu.mu);
            ppr_out_of_memory();
        }
    }
    /* Double-check. */
    if (ppr_cpu.profiling) {
        sync_mutex_unlock(&ppr_cpu.mu);
        return ppr_err_cpu_in_use;
    }
    PprCpuWriter *pw =
        (PprCpuWriter *)mem_alloc(heap_allocator(), sizeof *pw, _Alignof(PprCpuWriter));
    if (pw == NULL) {
        sync_mutex_unlock(&ppr_cpu.mu);
        ppr_out_of_memory();
    }
    pw->w = w;
    ppr_cpu.profiling = true;
    runtime_set_cpu_profile_rate(hz);
    if (!go(BURROW_FN(Func, ppr_profile_writer, pw))) {
        /* Not in Go. Stop the profile again and read the log to its end, which
         * is what lets the runtime free it. */
        runtime_set_cpu_profile_rate(0);
        while (!burrow__read_cpu_profile().eof) {
        }
        mem_free(heap_allocator(), pw, sizeof *pw, _Alignof(PprCpuWriter));
        ppr_cpu.profiling = false;
        sync_mutex_unlock(&ppr_cpu.mu);
        return ppr_err_cpu_no_writer;
    }
    sync_mutex_unlock(&ppr_cpu.mu);
    return BURROW_NO_ERROR;
}

void pprof_stop_cpu_profile(void) {
    sync_mutex_lock(&ppr_cpu.mu);
    if (!ppr_cpu.profiling) {
        sync_mutex_unlock(&ppr_cpu.mu);
        return;
    }
    ppr_cpu.profiling = false;
    runtime_set_cpu_profile_rate(0);
    bool done;
    (void)chan_recv(ppr_cpu.done, &done);
    sync_mutex_unlock(&ppr_cpu.mu);
}
