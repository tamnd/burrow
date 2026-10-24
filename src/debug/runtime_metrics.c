/* runtime/metrics: the list of metrics, and the readers that fill in a sample.
 *
 * Go's list is in description.go and its readers are in the runtime, in
 * metrics.go, which reads Go's own heap and scheduler. Here the readers read
 * what burrow's runtime keeps, through runtime_read_mem_stats and the
 * scheduler, and the ones with nothing to read give zero. The header says
 * which is which.
 *
 * Derived from Go's src/runtime/metrics/description.go, sample.go and
 * value.go, and from src/runtime/metrics.go and histogram.go.
 * Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/runtime/metrics.h"

#include "burrow/atomic.h"
#include "burrow/exectrace.h"
#include "burrow/func.h"
#include "burrow/math.h"
#include "burrow/mstats.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/sched.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include <stdint.h>
#include <string.h>

BURROW_NAMED_TYPE_DEFINE(MetricsValueKind, Int, KIND_INT);
BURROW_STRUCT_AS_DEFINE(MetricsValue, METRICS_VALUE_FIELDS);
BURROW_SLICE_TYPE_DEFINE(MetricsUint64Slice, uint64_t);
BURROW_SLICE_TYPE_DEFINE(MetricsFloat64Slice, double);
BURROW_STRUCT_AS_DEFINE(MetricsFloat64Histogram, METRICS_FLOAT64_HISTOGRAM_FIELDS);
BURROW_STRUCT_AS_DEFINE(MetricsDescription, METRICS_DESCRIPTION_FIELDS);
BURROW_STRUCT_AS_DEFINE(MetricsSample, METRICS_SAMPLE_FIELDS);

/* ----------------------------------------------------------------- value.go */

MetricsValueKind metrics_value_kind(MetricsValue v) {
    return v.kind;
}

uint64_t metrics_value_uint64(MetricsValue v) {
    if (v.kind != METRICS_KIND_UINT64)
        panic_str(BURROW_S("called Uint64 on non-uint64 metric value"));
    return v.scalar;
}

double metrics_value_float64(MetricsValue v) {
    if (v.kind != METRICS_KIND_FLOAT64)
        panic_str(BURROW_S("called Float64 on non-float64 metric value"));
    return math_float64frombits(v.scalar);
}

MetricsFloat64Histogram *metrics_value_float64_histogram(MetricsValue v) {
    if (v.kind != METRICS_KIND_FLOAT64_HISTOGRAM)
        panic_str(
            BURROW_S("called Float64Histogram on non-Float64Histogram metric value"));
    return (MetricsFloat64Histogram *)v.pointer;
}

/* ----------------------------------------------------------- description.go */

/* The GODEBUG settings that description.go's init adds a metric for, which are
 * the ones in internal/godebugs' table that are not opaque, as name and
 * package. They sort between /gc/ and /memory/, so both lists below have them
 * as one run in the middle. */
#define METRICS_GODEBUGS(X)                                                            \
    X("allowmultiplevcs", "cmd/go")                                                    \
    X("containermaxprocs", "runtime")                                                  \
    X("cryptocustomrand", "crypto")                                                    \
    X("embedfollowsymlinks", "cmd/go")                                                 \
    X("execerrdot", "os/exec")                                                         \
    X("fips140ems", "crypto/tls")                                                      \
    X("gocachehash", "cmd/go")                                                         \
    X("gocachetest", "cmd/go")                                                         \
    X("gocacheverify", "cmd/go")                                                       \
    X("gotestjsonbuildtext", "cmd/go")                                                 \
    X("htmlmetacontenturlescape", "html/template")                                     \
    X("http2client", "net/http")                                                       \
    X("http2server", "net/http")                                                       \
    X("httpcookiemaxnum", "net/http")                                                  \
    X("httplaxcontentlength", "net/http")                                              \
    X("httpmuxgo121", "net/http")                                                      \
    X("httpservecontentkeepheaders", "net/http")                                       \
    X("installgoroot", "go/build")                                                     \
    X("multipartmaxheaders", "mime/multipart")                                         \
    X("multipartmaxparts", "mime/multipart")                                           \
    X("multipathtcp", "net")                                                           \
    X("netedns0", "net")                                                               \
    X("panicnil", "runtime")                                                           \
    X("randautoseed", "math/rand")                                                     \
    X("randseednop", "math/rand")                                                      \
    X("rsa1024min", "crypto/rsa")                                                      \
    X("tarinsecurepath", "archive/tar")                                                \
    X("tlsmaxrsasize", "crypto/tls")                                                   \
    X("tlssha1", "crypto/tls")                                                         \
    X("updatemaxprocs", "runtime")                                                     \
    X("urlmaxqueryparams", "net/url")                                                  \
    X("urlstrictcolons", "net/url")                                                    \
    X("winreadlinkvolume", "os")                                                       \
    X("winsymlink", "os")                                                              \
    X("x509negativeserial", "crypto/x509")                                             \
    X("x509rsacrt", "crypto/x509")                                                     \
    X("x509sha256skid", "crypto/x509")                                                 \
    X("x509sslcertoverrideplatform", "crypto/x509")                                    \
    X("x509usefallbackroots", "crypto/x509")                                           \
    X("x509usepolicies", "crypto/x509")                                                \
    X("zipinsecurepath", "archive/zip")

/* One of the descriptions description.go's init adds, with Go's sentence. */
#define METRICS_GODEBUG_DESC(name, pkg)                                                \
    {                                                                                  \
        BURROW_S_INIT("/godebug/non-default-behavior/" name ":events"),                \
        BURROW_S_INIT("The number of non-default behaviors executed by the " pkg       \
                      " package due to a non-default GODEBUG=" name "=... setting."),  \
        METRICS_KIND_UINT64,                                                           \
        true,                                                                          \
    },

/* Go's allDesc, after init has put the GODEBUG metrics in. */
static const MetricsDescription met_all_desc[] = {
    {
        BURROW_S_INIT("/cgo/go-to-c-calls:calls"),
        BURROW_S_INIT("Count of calls made from Go to C by the current process."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/gc/mark/assist:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total CPU time goroutines spent performing GC tasks to "
            "assist the GC and prevent it from falling behind the application. This "
            "metric is an overestimate, and not directly comparable to system CPU "
            "time measurements. Compare only with other /cpu/classes metrics."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/gc/mark/dedicated:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total CPU time spent performing GC tasks on processors (as "
            "defined by GOMAXPROCS) dedicated to those tasks. This metric is an "
            "overestimate, and not directly comparable to system CPU time "
            "measurements. Compare only with other /cpu/classes metrics."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/gc/mark/idle:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total CPU time spent performing GC tasks on spare CPU "
            "resources that the Go scheduler could not otherwise find a use for. "
            "This should be subtracted from the total GC CPU time to obtain a "
            "measure of compulsory GC CPU time. This metric is an overestimate, and "
            "not directly comparable to system CPU time measurements. Compare only "
            "with other /cpu/classes metrics."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/gc/pause:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total CPU time spent with the application paused by the GC. "
            "Even if only one thread is running during the pause, this is computed "
            "as GOMAXPROCS times the pause latency because nothing else can be "
            "executing. This is the exact sum of samples in "
            "/sched/pauses/total/gc:seconds if each sample is multiplied by "
            "GOMAXPROCS at the time it is taken. This metric is an overestimate, "
            "and not directly comparable to system CPU time measurements. Compare "
            "only with other /cpu/classes metrics."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/gc/total:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total CPU time spent performing GC tasks. This metric is an "
            "overestimate, and not directly comparable to system CPU time "
            "measurements. Compare only with other /cpu/classes metrics. Sum of all "
            "metrics in /cpu/classes/gc."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/idle:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total available CPU time not spent executing any Go or Go "
            "runtime code. In other words, the part of "
            "/cpu/classes/total:cpu-seconds that was unused. This metric is an "
            "overestimate, and not directly comparable to system CPU time "
            "measurements. Compare only with other /cpu/classes metrics."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/scavenge/assist:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total CPU time spent returning unused memory to the "
            "underlying platform in response eagerly to memory pressure. This "
            "metric is an overestimate, and not directly comparable to system CPU "
            "time measurements. Compare only with other /cpu/classes metrics."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/scavenge/background:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total CPU time spent performing background tasks to return "
            "unused memory to the underlying platform. This metric is an "
            "overestimate, and not directly comparable to system CPU time "
            "measurements. Compare only with other /cpu/classes metrics."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/scavenge/total:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total CPU time spent performing tasks that return unused "
            "memory to the underlying platform. This metric is an overestimate, and "
            "not directly comparable to system CPU time measurements. Compare only "
            "with other /cpu/classes metrics. Sum of all metrics in "
            "/cpu/classes/scavenge."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/total:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total available CPU time for user Go code or the Go runtime, "
            "as defined by GOMAXPROCS. In other words, GOMAXPROCS integrated over "
            "the wall-clock duration this process has been executing for. This "
            "metric is an overestimate, and not directly comparable to system CPU "
            "time measurements. Compare only with other /cpu/classes metrics. Sum "
            "of all metrics in /cpu/classes."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/cpu/classes/user:cpu-seconds"),
        BURROW_S_INIT(
            "Estimated total CPU time spent running user Go code. This may also "
            "include some small amount of time spent in the Go runtime. This metric "
            "is an overestimate, and not directly comparable to system CPU time "
            "measurements. Compare only with other /cpu/classes metrics."),
        METRICS_KIND_FLOAT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/cleanups/executed:cleanups"),
        BURROW_S_INIT(
            "Approximate total count of cleanup functions (created by "
            "runtime.AddCleanup) executed by the runtime. Subtract "
            "/gc/cleanups/queued:cleanups to approximate cleanup queue length. "
            "Useful for detecting slow cleanups holding up the queue."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/cleanups/queued:cleanups"),
        BURROW_S_INIT(
            "Approximate total count of cleanup functions (created by "
            "runtime.AddCleanup) queued by the runtime for execution. Subtract from "
            "/gc/cleanups/executed:cleanups to approximate cleanup queue length. "
            "Useful for detecting slow cleanups holding up the queue."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/cycles/automatic:gc-cycles"),
        BURROW_S_INIT("Count of completed GC cycles generated by the Go runtime."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/cycles/forced:gc-cycles"),
        BURROW_S_INIT("Count of completed GC cycles forced by the application."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/cycles/total:gc-cycles"),
        BURROW_S_INIT("Count of all completed GC cycles."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/finalizers/executed:finalizers"),
        BURROW_S_INIT(
            "Total count of finalizer functions (created by runtime.SetFinalizer) "
            "executed by the runtime. Subtract /gc/finalizers/queued:finalizers to "
            "approximate finalizer queue length. Useful for detecting finalizers "
            "overwhelming the queue, either by being too slow, or by there being "
            "too many of them."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/finalizers/queued:finalizers"),
        BURROW_S_INIT(
            "Total count of finalizer functions (created by runtime.SetFinalizer) "
            "and queued by the runtime for execution. Subtract from "
            "/gc/finalizers/executed:finalizers to approximate finalizer queue "
            "length. Useful for detecting slow finalizers holding up the queue."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/gogc:percent"),
        BURROW_S_INIT(
            "Heap size target percentage configured by the user, otherwise 100. "
            "This value is set by the GOGC environment variable, and the "
            "runtime/debug.SetGCPercent function."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/gc/gomemlimit:bytes"),
        BURROW_S_INIT("Go runtime memory limit configured by the user, otherwise "
                      "math.MaxInt64. This value is set by the GOMEMLIMIT environment "
                      "variable, and the runtime/debug.SetMemoryLimit function."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/gc/heap/allocs-by-size:bytes"),
        BURROW_S_INIT(
            "Distribution of heap allocations by approximate size. Bucket counts "
            "increase monotonically. Note that this does not include tiny objects "
            "as defined by /gc/heap/tiny/allocs:objects, only tiny blocks."),
        METRICS_KIND_FLOAT64_HISTOGRAM,
        true,
    },
    {
        BURROW_S_INIT("/gc/heap/allocs:bytes"),
        BURROW_S_INIT(
            "Cumulative sum of memory allocated to the heap by the application."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/heap/allocs:objects"),
        BURROW_S_INIT(
            "Cumulative count of heap allocations triggered by the application. "
            "Note that this does not include tiny objects as defined by "
            "/gc/heap/tiny/allocs:objects, only tiny blocks."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/heap/frees-by-size:bytes"),
        BURROW_S_INIT(
            "Distribution of freed heap allocations by approximate size. Bucket "
            "counts increase monotonically. Note that this does not include tiny "
            "objects as defined by /gc/heap/tiny/allocs:objects, only tiny blocks."),
        METRICS_KIND_FLOAT64_HISTOGRAM,
        true,
    },
    {
        BURROW_S_INIT("/gc/heap/frees:bytes"),
        BURROW_S_INIT("Cumulative sum of heap memory freed by the garbage collector."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/heap/frees:objects"),
        BURROW_S_INIT(
            "Cumulative count of heap allocations whose storage was freed by the "
            "garbage collector. Note that this does not include tiny objects as "
            "defined by /gc/heap/tiny/allocs:objects, only tiny blocks."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/heap/goal:bytes"),
        BURROW_S_INIT("Heap size target for the end of the GC cycle."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/gc/heap/live:bytes"),
        BURROW_S_INIT(
            "Heap memory occupied by live objects that were marked by the previous "
            "GC."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/gc/heap/objects:objects"),
        BURROW_S_INIT("Number of objects, live or unswept, occupying heap memory."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/gc/heap/tiny/allocs:objects"),
        BURROW_S_INIT(
            "Count of small allocations that are packed together into blocks. These "
            "allocations are counted separately from other allocations because each "
            "individual allocation is not tracked by the runtime, only their block. "
            "Each block is already accounted for in allocs-by-size and "
            "frees-by-size."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/gc/limiter/last-enabled:gc-cycle"),
        BURROW_S_INIT(
            "GC cycle the last time the GC CPU limiter was enabled. This metric is "
            "useful for diagnosing the root cause of an out-of-memory error, "
            "because the limiter trades memory for CPU time when the GC's CPU time "
            "gets too high. This is most likely to occur with use of "
            "SetMemoryLimit. The first GC cycle is cycle 1, so a value of 0 "
            "indicates that it was never enabled."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/gc/pauses:seconds"),
        BURROW_S_INIT(
            "Deprecated. Prefer the identical /sched/pauses/total/gc:seconds."),
        METRICS_KIND_FLOAT64_HISTOGRAM,
        true,
    },
    {
        BURROW_S_INIT("/gc/scan/globals:bytes"),
        BURROW_S_INIT("The total amount of global variable space that is scannable."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/gc/scan/heap:bytes"),
        BURROW_S_INIT("The total amount of heap space that is scannable."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/gc/scan/stack:bytes"),
        BURROW_S_INIT("The number of bytes of stack that were scanned last GC cycle."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/gc/scan/total:bytes"),
        BURROW_S_INIT("The total amount space that is scannable. Sum of all metrics in "
                      "/gc/scan."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/gc/stack/starting-size:bytes"),
        BURROW_S_INIT("The stack size of new goroutines."),
        METRICS_KIND_UINT64,
        false,
    },
    /* clang-format off */
    METRICS_GODEBUGS(METRICS_GODEBUG_DESC)
    /* clang-format on */
    {
        BURROW_S_INIT("/memory/classes/heap/free:bytes"),
        BURROW_S_INIT(
            "Memory that is completely free and eligible to be returned to the "
            "underlying system, but has not been. This metric is the runtime's "
            "estimate of free address space that is backed by physical memory."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/heap/objects:bytes"),
        BURROW_S_INIT(
            "Memory occupied by live objects and dead objects that have not yet "
            "been marked free by the garbage collector."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/heap/released:bytes"),
        BURROW_S_INIT(
            "Memory that is completely free and has been returned to the underlying "
            "system. This metric is the runtime's estimate of free address space "
            "that is still mapped into the process, but is not backed by physical "
            "memory."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/heap/stacks:bytes"),
        BURROW_S_INIT(
            "Memory allocated from the heap that is reserved for stack space, "
            "whether or not it is currently in-use. Currently, this represents all "
            "stack memory for goroutines. It also includes all OS thread stacks in "
            "non-cgo programs. Note that stacks may be allocated differently in the "
            "future, and this may change."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/heap/unused:bytes"),
        BURROW_S_INIT(
            "Memory that is reserved for heap objects but is not currently used to "
            "hold heap objects."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/metadata/mcache/free:bytes"),
        BURROW_S_INIT(
            "Memory that is reserved for runtime mcache structures, but not in-use."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/metadata/mcache/inuse:bytes"),
        BURROW_S_INIT("Memory that is occupied by runtime mcache structures that are "
                      "currently being used."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/metadata/mspan/free:bytes"),
        BURROW_S_INIT(
            "Memory that is reserved for runtime mspan structures, but not in-use."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/metadata/mspan/inuse:bytes"),
        BURROW_S_INIT(
            "Memory that is occupied by runtime mspan structures that are currently "
            "being used."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/metadata/other:bytes"),
        BURROW_S_INIT("Memory that is reserved for or used to hold runtime metadata."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/os-stacks:bytes"),
        BURROW_S_INIT(
            "Stack memory allocated by the underlying operating system. In non-cgo "
            "programs this metric is currently zero. This may change in the "
            "future.In cgo programs this metric includes OS thread stacks allocated "
            "directly from the OS. Currently, this only accounts for one stack in "
            "c-shared and c-archive build modes, and other sources of stacks from "
            "the OS are not measured. This too may change in the future."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/other:bytes"),
        BURROW_S_INIT(
            "Memory used by execution trace buffers, structures for debugging the "
            "runtime, finalizer and profiler specials, and more."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/profiling/buckets:bytes"),
        BURROW_S_INIT(
            "Memory that is used by the stack trace hash map used for profiling."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/memory/classes/total:bytes"),
        BURROW_S_INIT(
            "All memory mapped by the Go runtime into the current process as "
            "read-write. Note that this does not include memory mapped by code "
            "called via cgo or via the syscall package. Sum of all metrics in "
            "/memory/classes."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/sched/gomaxprocs:threads"),
        BURROW_S_INIT(
            "The current runtime.GOMAXPROCS setting, or the number of operating "
            "system threads that can execute user-level Go code simultaneously."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/sched/goroutines-created:goroutines"),
        BURROW_S_INIT("Count of goroutines created since program start."),
        METRICS_KIND_UINT64,
        true,
    },
    {
        BURROW_S_INIT("/sched/goroutines/not-in-go:goroutines"),
        BURROW_S_INIT(
            "Approximate count of goroutines running or blocked in a system call or "
            "cgo call. Not guaranteed to add up to /sched/goroutines:goroutines "
            "with other goroutine metrics."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/sched/goroutines/runnable:goroutines"),
        BURROW_S_INIT(
            "Approximate count of goroutines ready to execute, but not executing. "
            "Not guaranteed to add up to /sched/goroutines:goroutines with other "
            "goroutine metrics."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/sched/goroutines/running:goroutines"),
        BURROW_S_INIT(
            "Approximate count of goroutines executing. Always less than or equal "
            "to /sched/gomaxprocs:threads. Not guaranteed to add up to "
            "/sched/goroutines:goroutines with other goroutine metrics."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/sched/goroutines/waiting:goroutines"),
        BURROW_S_INIT(
            "Approximate count of goroutines waiting on a resource (I/O or sync "
            "primitives). Not guaranteed to add up to /sched/goroutines:goroutines "
            "with other goroutine metrics."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/sched/goroutines:goroutines"),
        BURROW_S_INIT("Count of live goroutines."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/sched/latencies:seconds"),
        BURROW_S_INIT(
            "Distribution of the time goroutines have spent in the scheduler in a "
            "runnable state before actually running. Bucket counts increase "
            "monotonically."),
        METRICS_KIND_FLOAT64_HISTOGRAM,
        true,
    },
    {
        BURROW_S_INIT("/sched/pauses/stopping/gc:seconds"),
        BURROW_S_INIT(
            "Distribution of individual GC-related stop-the-world stopping "
            "latencies. This is the time it takes from deciding to stop the world "
            "until all Ps are stopped. This is a subset of the total GC-related "
            "stop-the-world time (/sched/pauses/total/gc:seconds). During this "
            "time, some threads may be executing. Bucket counts increase "
            "monotonically."),
        METRICS_KIND_FLOAT64_HISTOGRAM,
        true,
    },
    {
        BURROW_S_INIT("/sched/pauses/stopping/other:seconds"),
        BURROW_S_INIT(
            "Distribution of individual non-GC-related stop-the-world stopping "
            "latencies. This is the time it takes from deciding to stop the world "
            "until all Ps are stopped. This is a subset of the total non-GC-related "
            "stop-the-world time (/sched/pauses/total/other:seconds). During this "
            "time, some threads may be executing. Bucket counts increase "
            "monotonically."),
        METRICS_KIND_FLOAT64_HISTOGRAM,
        true,
    },
    {
        BURROW_S_INIT("/sched/pauses/total/gc:seconds"),
        BURROW_S_INIT(
            "Distribution of individual GC-related stop-the-world pause latencies. "
            "This is the time from deciding to stop the world until the world is "
            "started again. Some of this time is spent getting all threads to stop "
            "(this is measured directly in /sched/pauses/stopping/gc:seconds), "
            "during which some threads may still be running. Bucket counts increase "
            "monotonically."),
        METRICS_KIND_FLOAT64_HISTOGRAM,
        true,
    },
    {
        BURROW_S_INIT("/sched/pauses/total/other:seconds"),
        BURROW_S_INIT(
            "Distribution of individual non-GC-related stop-the-world pause "
            "latencies. This is the time from deciding to stop the world until the "
            "world is started again. Some of this time is spent getting all threads "
            "to stop (measured directly in /sched/pauses/stopping/other:seconds). "
            "Bucket counts increase monotonically."),
        METRICS_KIND_FLOAT64_HISTOGRAM,
        true,
    },
    {
        BURROW_S_INIT("/sched/threads/total:threads"),
        BURROW_S_INIT(
            "The current count of live threads that are owned by the Go runtime."),
        METRICS_KIND_UINT64,
        false,
    },
    {
        BURROW_S_INIT("/sync/mutex/wait/total:seconds"),
        BURROW_S_INIT(
            "Approximate cumulative time goroutines have spent blocked on a "
            "sync.Mutex, sync.RWMutex, or runtime-internal lock. This metric is "
            "useful for identifying global changes in lock contention. Collect a "
            "mutex or block profile using the runtime/pprof package for more "
            "detailed contention data."),
        METRICS_KIND_FLOAT64,
        true,
    },
};

Slice metrics_all(void) {
    Int n = (Int)(sizeof met_all_desc / sizeof met_all_desc[0]);
    return slice_from((void *)(uintptr_t)met_all_desc, n, n, TYPE_METRICS_DESCRIPTION);
}

/* --------------------------------------------------------------- metrics.go */

/* Go's statDep: which group of numbers a reader needs. */
#define MET_DEP_MEM 1U
#define MET_DEP_SCHED 2U

/* Go's schedStatsAggregate. */
typedef struct MetSchedStats {
    uint64_t total;
    uint64_t running;
    uint64_t runnable;
    uint64_t non_go;
    uint64_t waiting;
    uint64_t created;
    uint64_t threads;
} MetSchedStats;

/* Go's statAggregate, which holds the numbers one Read has looked at so far, so
 * that each group is read at most once however many samples need it. Here the
 * memory numbers are runtime_read_mem_stats', where Go has heapStats and
 * sysStats. */
typedef struct MetAggregate {
    unsigned ensured;
    RuntimeMemStats mem;
    MetSchedStats sched;
} MetAggregate;

static void met_count_g(burrow__G *g, void *arg) {
    MetSchedStats *s = (MetSchedStats *)arg;
    uint32_t st = burrow__atomic_load_acquire_u32(&g->status) & ~(uint32_t)BURROW_GSCAN;
    if (st == (uint32_t)BURROW_GIDLE || st == (uint32_t)BURROW_GDEAD)
        return;
    s->total++;
    if (st == (uint32_t)BURROW_GRUNNING)
        s->running++;
    else if (st == (uint32_t)BURROW_GRUNNABLE)
        s->runnable++;
    else if (st == (uint32_t)BURROW_GSYSCALL)
        s->non_go++;
}

/* Go's schedStatsAggregate.compute. Go counts the running and runnable ones
 * from the Ps and their run queues and the rest from gcount. Here every
 * goroutine is in the scheduler's list with its state, so it is one walk. */
static void met_sched_compute(MetSchedStats *s) {
    memset(s, 0, sizeof *s);
    s->threads = (uint64_t)burrow__thread_count();
    s->created = burrow__goroutines_created();
    burrow__sched_walk_gs(met_count_g, s);
    if (s->total < s->running + s->runnable + s->non_go)
        s->waiting = 0;
    else
        s->waiting = s->total - (s->running + s->runnable + s->non_go);
}

/* Go's statAggregate.ensure. */
static void met_ensure(MetAggregate *in, unsigned deps) {
    unsigned missing = deps & ~in->ensured;
    if ((missing & MET_DEP_MEM) != 0)
        runtime_read_mem_stats(&in->mem);
    if ((missing & MET_DEP_SCHED) != 0)
        met_sched_compute(&in->sched);
    in->ensured |= missing;
}

/* Go's sizeClassBuckets and timeHistBuckets, made once by met_init_buckets the
 * way initMetrics makes them. */
#define MET_NUM_SIZE_CLASSES 68
#define MET_TIME_HIST_MIN_BUCKET_BITS 9
#define MET_TIME_HIST_MAX_BUCKET_BITS 48
#define MET_TIME_HIST_SUB_BUCKET_BITS 2
#define MET_TIME_HIST_NUM_SUB_BUCKETS (1 << MET_TIME_HIST_SUB_BUCKET_BITS)
#define MET_TIME_HIST_NUM_BUCKETS                                                      \
    (MET_TIME_HIST_MAX_BUCKET_BITS - MET_TIME_HIST_MIN_BUCKET_BITS + 1)
#define MET_TIME_HIST_TOTAL_BUCKETS                                                    \
    (MET_TIME_HIST_NUM_BUCKETS * MET_TIME_HIST_NUM_SUB_BUCKETS + 2)

/* internal/runtime/gc's SizeClassToSize. */
static const uint16_t met_size_class_to_size[MET_NUM_SIZE_CLASSES] = {
    0,     8,     16,    24,    32,    48,    64,    80,    96,    112,   128,   144,
    160,   176,   192,   208,   224,   240,   256,   288,   320,   352,   384,   416,
    448,   480,   512,   576,   640,   704,   768,   896,   1024,  1152,  1280,  1408,
    1536,  1792,  2048,  2304,  2688,  3072,  3200,  3456,  4096,  4864,  5376,  6144,
    6528,  6784,  6912,  8192,  9472,  9728,  10240, 10880, 12288, 13568, 14336, 16384,
    18432, 19072, 20480, 21760, 24576, 27264, 28672, 32768};

static double met_size_class_buckets[MET_NUM_SIZE_CLASSES + 1];
static double met_time_hist_buckets[MET_TIME_HIST_TOTAL_BUCKETS + 1];
static SyncOnce met_buckets_once;

/* Go's timeHistogramMetricsBuckets. */
static void met_time_histogram_metrics_buckets(double *b) {
    Int n = MET_TIME_HIST_TOTAL_BUCKETS + 1;
    /* Underflow bucket. */
    b[0] = math_inf(-1);

    for (int j = 0; j < MET_TIME_HIST_NUM_SUB_BUCKETS; j++) {
        /* No bucket bit for the first few buckets. Just sub-bucket bits after
         * the min bucket bit. */
        uint64_t bucket_nanos = (uint64_t)j << (MET_TIME_HIST_MIN_BUCKET_BITS - 1 -
                                                MET_TIME_HIST_SUB_BUCKET_BITS);
        /* Convert nanoseconds to seconds via a division. These values will all
         * be exactly representable by a float64. */
        b[j + 1] = (double)bucket_nanos / 1e9;
    }
    /* Generate the rest of the buckets. It's easier to reason about if we cut
     * out the 0'th bucket. */
    for (int i = MET_TIME_HIST_MIN_BUCKET_BITS; i < MET_TIME_HIST_MAX_BUCKET_BITS;
         i++) {
        for (int j = 0; j < MET_TIME_HIST_NUM_SUB_BUCKETS; j++) {
            /* Set the bucket bit. */
            uint64_t bucket_nanos = (uint64_t)1 << (i - 1);
            /* Set the sub-bucket bits. */
            bucket_nanos |= (uint64_t)j << (i - 1 - MET_TIME_HIST_SUB_BUCKET_BITS);
            /* The index for this bucket is going to be the (i+1)'th bucket
             * (note that we're starting from zero, but handled the first bucket
             * earlier, so we need to compensate), and the j'th sub bucket. Add
             * 1 because we left space for -Inf. */
            int bucket_index = (i - MET_TIME_HIST_MIN_BUCKET_BITS + 1) *
                                   MET_TIME_HIST_NUM_SUB_BUCKETS +
                               j + 1;
            /* Convert nanoseconds to seconds via a division. These values will
             * all be exactly representable by a float64. */
            b[bucket_index] = (double)bucket_nanos / 1e9;
        }
    }
    /* Overflow bucket. */
    b[n - 2] = (double)((uint64_t)1 << (MET_TIME_HIST_MAX_BUCKET_BITS - 1)) / 1e9;
    b[n - 1] = math_inf(1);
}

static void met_init_buckets(void *env) {
    (void)env;
    /* Skip size class 0 which is a stand-in for large objects, but large
     * objects are tracked separately (and they actually get placed in the last
     * bucket, not the first). */
    met_size_class_buckets[0] = 1; /* The smallest allocation is 1 byte in size. */
    for (int i = 1; i < MET_NUM_SIZE_CLASSES; i++) {
        /* Size classes have an inclusive upper-bound and exclusive lower bound
         * (e.g. 48-byte size class is (32, 48]) whereas we want and inclusive
         * lower-bound and exclusive upper-bound (e.g. 48-byte size class is
         * [33, 49)). We can achieve this by shifting all bucket boundaries up
         * by 1.
         *
         * Also, a float64 can precisely represent integers with value up to
         * 2^53 and size classes are relatively small (nowhere near 2^48 even)
         * so this will give us exact boundaries. */
        met_size_class_buckets[i] = (double)(met_size_class_to_size[i] + 1);
    }
    met_size_class_buckets[MET_NUM_SIZE_CLASSES] = math_inf(1);

    met_time_histogram_metrics_buckets(met_time_hist_buckets);
}

/* Go's metricValue.float64HistOrInit. The counts are zeroed each time, since
 * no reader here has a count to put in them. */
static MetricsFloat64Histogram *met_float64_hist_or_init(MetricsValue *v, Alloc *a,
                                                         double *buckets, Int n) {
    MetricsFloat64Histogram *hist;
    if (v->kind == METRICS_KIND_FLOAT64_HISTOGRAM && v->pointer != NULL) {
        hist = (MetricsFloat64Histogram *)v->pointer;
    } else {
        hist = (MetricsFloat64Histogram *)mem_alloc(a, sizeof *hist,
                                                    _Alignof(MetricsFloat64Histogram));
        if (hist == NULL)
            panic_str(BURROW_S("runtime/metrics: out of memory"));
        memset(hist, 0, sizeof *hist);
        v->kind = METRICS_KIND_FLOAT64_HISTOGRAM;
        v->pointer = hist;
    }
    hist->buckets = slice_from(buckets, n, n, TYPE_FLOAT64);
    if (hist->counts.len != n - 1) {
        hist->counts = slice_make(a, TYPE_UINT64, n - 1, n - 1);
        if (hist->counts.p == NULL)
            panic_str(BURROW_S("runtime/metrics: out of memory"));
    } else {
        memset(hist->counts.p, 0, (size_t)(n - 1) * sizeof(uint64_t));
    }
    return hist;
}

typedef void (*MetCompute)(MetAggregate *in, MetricsValue *out, Alloc *a);

/* Go's metricData. */
typedef struct MetData {
    Str name;
    unsigned deps;
    MetCompute compute;
} MetData;

static void met_uint64(MetricsValue *out, uint64_t v) {
    out->kind = METRICS_KIND_UINT64;
    out->scalar = v;
}

static void met_float64(MetricsValue *out, double v) {
    out->kind = METRICS_KIND_FLOAT64;
    out->scalar = math_float64bits(v);
}

/* Go's compute0, the reader for a GODEBUG count. */
static void met_compute0(MetAggregate *in, MetricsValue *out, Alloc *a) {
    (void)in;
    (void)a;
    met_uint64(out, 0);
}

/* The readers for the numbers burrow does not keep, by kind. */
static void met_zero_float64(MetAggregate *in, MetricsValue *out, Alloc *a) {
    (void)in;
    (void)a;
    met_float64(out, 0);
}

static void met_size_class_hist(MetAggregate *in, MetricsValue *out, Alloc *a) {
    (void)in;
    (void)met_float64_hist_or_init(out, a, met_size_class_buckets,
                                   MET_NUM_SIZE_CLASSES + 1);
}

static void met_time_hist(MetAggregate *in, MetricsValue *out, Alloc *a) {
    (void)in;
    (void)met_float64_hist_or_init(out, a, met_time_hist_buckets,
                                   MET_TIME_HIST_TOTAL_BUCKETS + 1);
}

/* A reader of one number from the aggregate, as Go writes each one inline. */
#define MET_READER(fn, expr)                                                           \
    static void fn(MetAggregate *in, MetricsValue *out, Alloc *a) {                    \
        (void)in;                                                                      \
        (void)a;                                                                       \
        met_uint64(out, (expr));                                                       \
    }

/* x - y for the pairs of fields Go subtracts, which cannot go below zero in
 * Go's numbers and must not wrap here if one of them is not kept. */
static uint64_t met_sub(uint64_t x, uint64_t y) {
    return x > y ? x - y : 0;
}

MET_READER(met_gc_cycles_automatic, met_sub(in->mem.num_gc, in->mem.num_forced_gc))
MET_READER(met_gc_cycles_forced, in->mem.num_forced_gc)
MET_READER(met_gc_cycles_total, in->mem.num_gc)
MET_READER(met_gogc, 100)
MET_READER(met_gomemlimit, (uint64_t)INT64_MAX)
MET_READER(met_heap_allocs_bytes, in->mem.total_alloc)
MET_READER(met_heap_allocs_objects, in->mem.mallocs)
MET_READER(met_heap_frees_objects, in->mem.frees)
MET_READER(met_heap_goal, in->mem.next_gc)
MET_READER(met_heap_live, in->mem.heap_alloc)
MET_READER(met_heap_objects, in->mem.heap_objects)
MET_READER(met_stack_starting_size, (uint64_t)BURROW_GOROUTINE_STACK)
MET_READER(met_mem_heap_free, met_sub(in->mem.heap_idle, in->mem.heap_released))
MET_READER(met_mem_heap_objects, in->mem.heap_alloc)
MET_READER(met_mem_heap_released, in->mem.heap_released)
MET_READER(met_mem_heap_stacks, in->mem.stack_inuse)
MET_READER(met_mem_heap_unused, met_sub(in->mem.heap_inuse, in->mem.heap_alloc))
MET_READER(met_mem_mcache_free, met_sub(in->mem.m_cache_sys, in->mem.m_cache_inuse))
MET_READER(met_mem_mcache_inuse, in->mem.m_cache_inuse)
MET_READER(met_mem_mspan_free, met_sub(in->mem.m_span_sys, in->mem.m_span_inuse))
MET_READER(met_mem_mspan_inuse, in->mem.m_span_inuse)
MET_READER(met_mem_metadata_other, in->mem.gc_sys)
MET_READER(met_mem_os_stacks, met_sub(in->mem.stack_sys, in->mem.stack_inuse))
MET_READER(met_mem_other, in->mem.other_sys)
MET_READER(met_mem_profiling_buckets, in->mem.buck_hash_sys)
MET_READER(met_gomaxprocs, (uint64_t)burrow__gomaxprocs())
MET_READER(met_goroutines_created, in->sched.created)
MET_READER(met_goroutines_not_in_go, in->sched.non_go)
MET_READER(met_goroutines_runnable, in->sched.runnable)
MET_READER(met_goroutines_running, in->sched.running)
MET_READER(met_goroutines_waiting, in->sched.waiting)
MET_READER(met_goroutines, in->sched.total)
MET_READER(met_threads, in->sched.threads)

/* Go's sum is over heapStats and sysStats. This is the sum of the classes
 * above, so that they add up to it the way the package doc says they do. */
static void met_mem_total(MetAggregate *in, MetricsValue *out, Alloc *a) {
    (void)a;
    const RuntimeMemStats *m = &in->mem;
    uint64_t heap = met_sub(m->heap_idle, m->heap_released) + m->heap_alloc +
                    m->heap_released + m->stack_inuse +
                    met_sub(m->heap_inuse, m->heap_alloc);
    uint64_t meta = m->m_cache_sys + m->m_span_sys + m->gc_sys;
    uint64_t rest =
        met_sub(m->stack_sys, m->stack_inuse) + m->other_sys + m->buck_hash_sys;
    met_uint64(out, heap + meta + rest);
}

#define MET_GODEBUG_DATA(name, pkg)                                                    \
    {BURROW_S_INIT("/godebug/non-default-behavior/" name ":events"), 0, met_compute0},

/* Go's metrics map, as a list in the order of the names, so that a name is
 * found by a binary search. */
static const MetData met_data[] = {
    {BURROW_S_INIT("/cgo/go-to-c-calls:calls"), 0, met_compute0},
    {BURROW_S_INIT("/cpu/classes/gc/mark/assist:cpu-seconds"), 0, met_zero_float64},
    {BURROW_S_INIT("/cpu/classes/gc/mark/dedicated:cpu-seconds"), 0, met_zero_float64},
    {BURROW_S_INIT("/cpu/classes/gc/mark/idle:cpu-seconds"), 0, met_zero_float64},
    {BURROW_S_INIT("/cpu/classes/gc/pause:cpu-seconds"), 0, met_zero_float64},
    {BURROW_S_INIT("/cpu/classes/gc/total:cpu-seconds"), 0, met_zero_float64},
    {BURROW_S_INIT("/cpu/classes/idle:cpu-seconds"), 0, met_zero_float64},
    {BURROW_S_INIT("/cpu/classes/scavenge/assist:cpu-seconds"), 0, met_zero_float64},
    {BURROW_S_INIT("/cpu/classes/scavenge/background:cpu-seconds"), 0,
     met_zero_float64},
    {BURROW_S_INIT("/cpu/classes/scavenge/total:cpu-seconds"), 0, met_zero_float64},
    {BURROW_S_INIT("/cpu/classes/total:cpu-seconds"), 0, met_zero_float64},
    {BURROW_S_INIT("/cpu/classes/user:cpu-seconds"), 0, met_zero_float64},
    {BURROW_S_INIT("/gc/cleanups/executed:cleanups"), 0, met_compute0},
    {BURROW_S_INIT("/gc/cleanups/queued:cleanups"), 0, met_compute0},
    {BURROW_S_INIT("/gc/cycles/automatic:gc-cycles"), MET_DEP_MEM,
     met_gc_cycles_automatic},
    {BURROW_S_INIT("/gc/cycles/forced:gc-cycles"), MET_DEP_MEM, met_gc_cycles_forced},
    {BURROW_S_INIT("/gc/cycles/total:gc-cycles"), MET_DEP_MEM, met_gc_cycles_total},
    {BURROW_S_INIT("/gc/finalizers/executed:finalizers"), 0, met_compute0},
    {BURROW_S_INIT("/gc/finalizers/queued:finalizers"), 0, met_compute0},
    {BURROW_S_INIT("/gc/gogc:percent"), 0, met_gogc},
    {BURROW_S_INIT("/gc/gomemlimit:bytes"), 0, met_gomemlimit},
    {BURROW_S_INIT("/gc/heap/allocs-by-size:bytes"), 0, met_size_class_hist},
    {BURROW_S_INIT("/gc/heap/allocs:bytes"), MET_DEP_MEM, met_heap_allocs_bytes},
    {BURROW_S_INIT("/gc/heap/allocs:objects"), MET_DEP_MEM, met_heap_allocs_objects},
    {BURROW_S_INIT("/gc/heap/frees-by-size:bytes"), 0, met_size_class_hist},
    {BURROW_S_INIT("/gc/heap/frees:bytes"), 0, met_compute0},
    {BURROW_S_INIT("/gc/heap/frees:objects"), MET_DEP_MEM, met_heap_frees_objects},
    {BURROW_S_INIT("/gc/heap/goal:bytes"), MET_DEP_MEM, met_heap_goal},
    {BURROW_S_INIT("/gc/heap/live:bytes"), MET_DEP_MEM, met_heap_live},
    {BURROW_S_INIT("/gc/heap/objects:objects"), MET_DEP_MEM, met_heap_objects},
    {BURROW_S_INIT("/gc/heap/tiny/allocs:objects"), 0, met_compute0},
    {BURROW_S_INIT("/gc/limiter/last-enabled:gc-cycle"), 0, met_compute0},
    {BURROW_S_INIT("/gc/pauses:seconds"), 0, met_time_hist},
    {BURROW_S_INIT("/gc/scan/globals:bytes"), 0, met_compute0},
    {BURROW_S_INIT("/gc/scan/heap:bytes"), 0, met_compute0},
    {BURROW_S_INIT("/gc/scan/stack:bytes"), 0, met_compute0},
    {BURROW_S_INIT("/gc/scan/total:bytes"), 0, met_compute0},
    {BURROW_S_INIT("/gc/stack/starting-size:bytes"), 0, met_stack_starting_size},
    /* clang-format off */
    METRICS_GODEBUGS(MET_GODEBUG_DATA)
    /* clang-format on */
    {BURROW_S_INIT("/memory/classes/heap/free:bytes"), MET_DEP_MEM, met_mem_heap_free},
    {BURROW_S_INIT("/memory/classes/heap/objects:bytes"), MET_DEP_MEM,
     met_mem_heap_objects},
    {BURROW_S_INIT("/memory/classes/heap/released:bytes"), MET_DEP_MEM,
     met_mem_heap_released},
    {BURROW_S_INIT("/memory/classes/heap/stacks:bytes"), MET_DEP_MEM,
     met_mem_heap_stacks},
    {BURROW_S_INIT("/memory/classes/heap/unused:bytes"), MET_DEP_MEM,
     met_mem_heap_unused},
    {BURROW_S_INIT("/memory/classes/metadata/mcache/free:bytes"), MET_DEP_MEM,
     met_mem_mcache_free},
    {BURROW_S_INIT("/memory/classes/metadata/mcache/inuse:bytes"), MET_DEP_MEM,
     met_mem_mcache_inuse},
    {BURROW_S_INIT("/memory/classes/metadata/mspan/free:bytes"), MET_DEP_MEM,
     met_mem_mspan_free},
    {BURROW_S_INIT("/memory/classes/metadata/mspan/inuse:bytes"), MET_DEP_MEM,
     met_mem_mspan_inuse},
    {BURROW_S_INIT("/memory/classes/metadata/other:bytes"), MET_DEP_MEM,
     met_mem_metadata_other},
    {BURROW_S_INIT("/memory/classes/os-stacks:bytes"), MET_DEP_MEM, met_mem_os_stacks},
    {BURROW_S_INIT("/memory/classes/other:bytes"), MET_DEP_MEM, met_mem_other},
    {BURROW_S_INIT("/memory/classes/profiling/buckets:bytes"), MET_DEP_MEM,
     met_mem_profiling_buckets},
    {BURROW_S_INIT("/memory/classes/total:bytes"), MET_DEP_MEM, met_mem_total},
    {BURROW_S_INIT("/sched/gomaxprocs:threads"), 0, met_gomaxprocs},
    {BURROW_S_INIT("/sched/goroutines-created:goroutines"), MET_DEP_SCHED,
     met_goroutines_created},
    {BURROW_S_INIT("/sched/goroutines/not-in-go:goroutines"), MET_DEP_SCHED,
     met_goroutines_not_in_go},
    {BURROW_S_INIT("/sched/goroutines/runnable:goroutines"), MET_DEP_SCHED,
     met_goroutines_runnable},
    {BURROW_S_INIT("/sched/goroutines/running:goroutines"), MET_DEP_SCHED,
     met_goroutines_running},
    {BURROW_S_INIT("/sched/goroutines/waiting:goroutines"), MET_DEP_SCHED,
     met_goroutines_waiting},
    {BURROW_S_INIT("/sched/goroutines:goroutines"), MET_DEP_SCHED, met_goroutines},
    {BURROW_S_INIT("/sched/latencies:seconds"), 0, met_time_hist},
    {BURROW_S_INIT("/sched/pauses/stopping/gc:seconds"), 0, met_time_hist},
    {BURROW_S_INIT("/sched/pauses/stopping/other:seconds"), 0, met_time_hist},
    {BURROW_S_INIT("/sched/pauses/total/gc:seconds"), 0, met_time_hist},
    {BURROW_S_INIT("/sched/pauses/total/other:seconds"), 0, met_time_hist},
    {BURROW_S_INIT("/sched/threads/total:threads"), MET_DEP_SCHED, met_threads},
    {BURROW_S_INIT("/sync/mutex/wait/total:seconds"), 0, met_zero_float64},
};

#define MET_NUM_DATA ((Int)(sizeof met_data / sizeof met_data[0]))

static const MetData *met_lookup(Str name) {
    Int lo = 0, hi = MET_NUM_DATA;
    while (lo < hi) {
        Int m = (Int)((Uint)(lo + hi) >> 1);
        Int c = strings_compare(name, met_data[m].name);
        if (c == 0)
            return &met_data[m];
        if (c < 0)
            hi = m;
        else
            lo = m + 1;
    }
    return NULL;
}

Slice burrow__metrics_read_names(Alloc *a) {
    Slice list = slice_make(a, TYPE_STRING, MET_NUM_DATA, MET_NUM_DATA);
    if (list.p == NULL)
        return list;
    for (Int i = 0; i < MET_NUM_DATA; i++)
        ((Str *)list.p)[i] = met_data[i].name;
    return list;
}

/* Go's readMetrics and readMetricsLocked. Go holds metricsLock for the whole
 * read, since its readers share agg and the map. Here the aggregate is on the
 * stack and the list does not change, so nothing is shared but the buckets,
 * which are made once and only read after that. */
void metrics_read(Alloc *a, Slice m) {
    if (m.len == 0)
        return;
    sync_once_do(&met_buckets_once, BURROW_FN(Func, met_init_buckets, NULL));

    MetAggregate agg;
    memset(&agg, 0, sizeof agg);

    MetricsSample *samples = (MetricsSample *)m.p;
    for (Int i = 0; i < m.len; i++) {
        MetricsSample *sample = &samples[i];
        const MetData *data = met_lookup(sample->name);
        if (data == NULL) {
            sample->value.kind = METRICS_KIND_BAD;
            continue;
        }
        /* Ensure we have all the stats we need. agg is populated lazily. */
        met_ensure(&agg, data->deps);

        /* Compute the value based on the stats we have. */
        data->compute(&agg, &sample->value, a);
    }
}
