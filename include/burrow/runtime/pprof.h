/* runtime/pprof: profiles in the format the pprof tool reads.
 *
 * A profile is a set of call stacks, each with a count. This is the part of
 * Go's runtime/pprof that a program fills in itself: profiling labels, which
 * tag the work a goroutine does, and named profiles, where a program records
 * the stack that made each of some kind of object and later writes them all
 * out. A profile of open files looks like this:
 *
 *     PprofProfile *files = pprof_new_profile(BURROW_S("example.com/files"));
 *     ...
 *     pprof_profile_add(files, BURROW_ANY(TYPE_UNSAFE_POINTER, &f), 1);
 *     ...
 *     pprof_profile_remove(files, BURROW_ANY(TYPE_UNSAFE_POINTER, &f));
 *     ...
 *     Error err = pprof_profile_write_to(files, w, 0);
 *
 * With debug 0 the profile is written as a gzipped profile.proto, which is what
 * the pprof tool and internal/profile read. With debug 1 it is the older text
 * form, one block per stack with the function names written next to the
 * addresses, for a person to read.
 *
 * Labels go on a context, and pprof_do runs a function with them set on the
 * goroutine, so that goroutines it starts get them too:
 *
 *     PprofLabelSet l = pprof_labels_v(a, 2, BURROW_S("worker"), BURROW_S("7"));
 *     pprof_do(a, ctx, l, BURROW_FN(PprofDoFunc, work, env));
 *
 * Of the profiles Go's runtime fills in, "goroutine", "threadcreate", "heap"
 * and "allocs" are here, and pprof_lookup says more, and so is the CPU
 * profiler, which pprof_start_cpu_profile starts. "block" and "mutex" are not
 * here yet.
 *
 * The stacks are what runtime_callers gives and the names are what
 * runtime_func_for_pc gives, so a static function shows up under the nearest
 * global function before it, and there are no file names or line numbers yet.
 * burrow/runtime.h says why.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package runtime/pprof */

#ifndef BURROW_RUNTIME_PPROF_H
#define BURROW_RUNTIME_PPROF_H

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------- labels */

/* One profiling label, a key and its value. Go's label.Label. */
typedef struct PprofLabel {
    Str key;
    Str value;
} PprofLabel;
extern const Type burrow_type_PprofLabel;
#define TYPE_PPROF_LABEL TYPE_OF(PprofLabel)

/* LabelSet is a set of labels. list holds a PprofLabel for each, sorted by key
 * with no key twice. */
typedef struct PprofLabelSet {
    Slice list;
} PprofLabelSet;

/* Labels takes an even number of strings representing key-value pairs and
 * makes a LabelSet containing them. A label overwrites a prior label with the
 * same key. Currently only the CPU and goroutine profiles utilize any labels
 * information. See https://golang.org/issue/23458 for details.
 *
 * args is a slice of Str. The list is allocated from a and the strings are not
 * copied. Panics with "uneven number of arguments to pprof.Labels" when there
 * is a key without a value, and when a has no memory left. */
BURROW_OWNS(ret) PprofLabelSet pprof_labels(Alloc *a, Slice args);

/* The same, with the n strings as arguments. */
BURROW_OWNS(ret) PprofLabelSet pprof_labels_v(Alloc *a, int n, ...);

/* WithLabels returns a new context.Context with the given labels added. A label
 * overwrites a prior label with the same key.
 *
 * The new context and its merged set of labels come from a and are never given
 * back, because goroutines can go on holding the labels after the context is
 * gone. Use an allocator that outlives every goroutine that might, such as
 * gc_allocator, or an arena that is freed once those goroutines are done. */
BURROW_OWNS(ret) Context pprof_with_labels(Alloc *a, Context ctx, PprofLabelSet labels);

/* Label returns the value of the label with the given key on ctx, and a boolean
 * indicating whether that label exists. value may be NULL. */
bool pprof_label(Context ctx, Str key, Str *value);

/* Go's func(key, value string) bool, what pprof_for_labels calls. */
BURROW_FUNC(PprofLabelFunc, bool, Str key, Str value);

/* ForLabels invokes f with each label set on the context. The function f should
 * return true to continue iteration or false to stop iteration early. */
void pprof_for_labels(Context ctx, PprofLabelFunc f);

/* SetGoroutineLabels sets the current goroutine's labels to match ctx. A new
 * goroutine inherits the labels of the goroutine that created it. This is a
 * lower-level API than pprof_do, which should be used instead when possible.
 *
 * Code that runs before runtime_main, outside any goroutine, has one set of
 * labels per thread instead. */
void pprof_set_goroutine_labels(Context ctx);

/* Go's func(context.Context), what pprof_do calls. */
BURROW_FUNC(PprofDoFunc, void, Context ctx);

/* Do calls f with a copy of the parent context with the given labels added to
 * the parent's label map. Goroutines spawned while executing f will inherit
 * the augmented label-set. Each key/value pair in labels is inserted into the
 * label map in the order provided, overriding any previous value for the same
 * key. The augmented label map will be set for the duration of the call to f
 * and restored once f returns.
 *
 * The context is made with pprof_with_labels on a, so the same goes for its
 * memory. The labels are restored when f panics too. */
void pprof_do(Alloc *a, Context ctx, PprofLabelSet labels, PprofDoFunc f);

/* --------------------------------------------------------------- profiles */

/* A Profile is a collection of stack traces showing the call sequences that
 * led to instances of a particular event, such as allocation. Packages can
 * create and maintain their own profiles; the most common use is for tracking
 * resources that must be explicitly closed, such as files or network
 * connections.
 *
 * A Profile's methods can be called from multiple goroutines simultaneously.
 *
 * A profile lives as long as the program, as in Go, where nothing can remove
 * one from the list once it is in it. */
typedef struct PprofProfile PprofProfile;

/* NewProfile creates a new profile with the given name. If a profile with that
 * name already exists, NewProfile panics. The convention is to use a
 * 'import/path.' prefix to create separate name spaces for each package. For
 * compatibility with various tools that read pprof data, profile names should
 * not contain spaces.
 *
 * The name is copied. Panics with "pprof: NewProfile with empty name" for an
 * empty one. */
BURROW_STATIC(ret) PprofProfile *pprof_new_profile(Str name);

/* Lookup returns the profile with the given name, or NULL if no such profile
 * exists.
 *
 * The predefined profiles are:
 *
 *     goroutine    - stack traces of all current goroutines
 *     heap         - a sampling of memory allocations of live objects
 *     allocs       - a sampling of all past memory allocations
 *     threadcreate - stack traces that led to the creation of new OS threads
 *
 * The heap and allocs profiles are the same profile, which burrow/mprof.h
 * describes, and differ only in which of its values the pprof tool shows
 * first: memory in use for heap and memory allocated for allocs. Go's are as
 * of the last garbage collection. Here they are as of now, since a free is
 * counted when mem_free is called.
 *
 * A goroutine's stack in the goroutine profile is read from where it last
 * stopped, on amd64 and arm64 outside Windows. Elsewhere, and for a goroutine
 * that is still running after the profile has waited twenty milliseconds for it
 * to stop, the stack is the function the goroutine was started with. The
 * debug=2 form of the profile is runtime_stack with all set, which reads the
 * stacks the same way. */
BURROW_STATIC(ret) PprofProfile *pprof_lookup(Str name);

/* Profiles returns a slice of all the known profiles, sorted by name. The
 * elements are PprofProfile pointers and the slice comes from a. */
BURROW_OWNS(ret) Slice pprof_profiles(Alloc *a);

/* Name returns this profile's name, which can be passed to pprof_lookup to
 * reobtain the profile. */
BURROW_BORROWS(ret, p) Str pprof_profile_name(const PprofProfile *p);

/* Count returns the number of execution stacks currently in the profile. */
Int pprof_profile_count(PprofProfile *p);

/* Add adds the current execution stack to the profile, associated with value.
 * Add stores value in an internal map, so value must be suitable for use as a
 * map key and will not be garbage collected until the corresponding call to
 * pprof_profile_remove. Add panics if the profile already contains a stack for
 * value.
 *
 * The skip parameter has the same meaning as runtime_caller's skip and controls
 * where the stack trace begins. Passing skip=0 begins the trace in the function
 * calling Add. For example, given this execution stack:
 *
 *     Add
 *     called from rpc.NewClient
 *     called from mypkg.Run
 *     called from main.main
 *
 * Passing skip=0 begins the stack trace at the call to Add inside
 * rpc.NewClient. Passing skip=1 begins the stack trace at the call to NewClient
 * inside mypkg.Run.
 *
 * What value points at is copied, the way Go copies a value into an interface,
 * so BURROW_ANY_OF on a temporary is fine. Anything it points to in turn is
 * not. */
void pprof_profile_add(PprofProfile *p, Any value, Int skip);

/* Remove removes the execution stack associated with value from the profile.
 * It is a no-op if the value is not in the profile. */
void pprof_profile_remove(PprofProfile *p, Any value);

/* WriteTo writes a pprof-formatted snapshot of the profile to w. If a write to
 * w returns an error, WriteTo returns that error. Otherwise, WriteTo returns
 * no error.
 *
 * The debug parameter enables additional output. Passing debug=0 writes the
 * gzip-compressed protocol buffer described in
 * https://github.com/google/pprof/tree/main/proto#overview. Passing debug=1
 * writes the legacy text format with comments translating addresses to
 * function names and line numbers, so that a programmer can read the profile
 * without tools.
 *
 * The predefined profiles may assign meaning to other debug values; for
 * example, when printing the "goroutine" profile, debug=2 means to print the
 * goroutine stacks in the same form that a Go program uses when dying due to
 * an unrecovered panic. */
BURROW_STATIC(ret) Error pprof_profile_write_to(PprofProfile *p, IoWriter w, Int debug);

/* WriteHeapProfile is shorthand for Lookup("heap").WriteTo(w, 0). It is
 * preserved for backwards compatibility. */
BURROW_STATIC(ret) Error pprof_write_heap_profile(IoWriter w);

/* StartCPUProfile enables CPU profiling for the current process. While
 * profiling, the profile will be buffered and written to w.
 * StartCPUProfile returns an error if profiling is already enabled.
 *
 *     Error err = pprof_start_cpu_profile(w);
 *     ...
 *     pprof_stop_cpu_profile();
 *
 * The samples come from SIGPROF, a hundred a second of CPU time the process
 * uses. The handler goes in the first time and stays, so a program that has its
 * own SIGPROF handler loses it. On Windows and wasip1 there is no SIGPROF and
 * the profile has no samples. A goroutine's stack is walked from where the
 * signal stopped it, so it has every frame only where its frames have frame
 * pointers. A sample taken while a thread is in the scheduler, or on a thread
 * that is not running a goroutine, is the pc it stopped at and a frame named
 * burrow__prof_system or burrow__prof_external_code.
 *
 * While runtime_main is running, the profile is written by a goroutine, as in
 * Go. Called when it is not, before it or between two runs of it, the profile
 * is written by a thread of its own instead, and keeps going across any run of
 * runtime_main in between. That needs threads, so on wasip1 it fails there. w
 * is written from the goroutine or the thread until pprof_stop_cpu_profile
 * returns. */
BURROW_STATIC(ret) Error pprof_start_cpu_profile(IoWriter w);

/* StopCPUProfile stops the current CPU profile, if any. StopCPUProfile only
 * returns after all the writes for the profile have completed. */
void pprof_stop_cpu_profile(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_RUNTIME_PPROF_H */
