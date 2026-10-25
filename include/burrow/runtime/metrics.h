/* Derived from Go's src/runtime/metrics: doc.go, description.go, histogram.go,
 * sample.go and value.go, and the bucket boundaries and the readers in
 * src/runtime/metrics.go and src/runtime/histogram.go.
 * Go source: go1.27.3.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* runtime/metrics: the numbers the runtime keeps about itself, each one read by
 * a name that carries its unit.
 *
 * Package metrics provides a stable interface to access implementation-defined
 * metrics exported by the Go runtime. This package is similar to existing
 * functions like [runtime.ReadMemStats] and [runtime/debug.ReadGCStats], but
 * significantly more general.
 *
 * The set of metrics defined by this package may evolve as the runtime itself
 * evolves, and also enables variation across Go implementations, whose relevant
 * metric sets may not intersect.
 *
 * # Interface
 *
 * Metrics are designated by a string key, rather than, for example, a field
 * name in a struct. The full list of supported metrics is always available in
 * the slice of Descriptions returned by [All]. Each [Description] also includes
 * useful information about the metric.
 *
 * Thus, users of this API are encouraged to sample supported metrics defined by
 * the slice returned by All to remain compatible across Go versions. Of course,
 * situations arise where reading specific metrics is critical. For these cases,
 * users are encouraged to use build tags, and although metrics may be
 * deprecated and removed, users should consider this to be an exceptional and
 * rare event, coinciding with a very large change in a particular Go
 * implementation.
 *
 * Each metric key also has a "kind" (see [ValueKind]) that describes the format
 * of the metric's value. In the interest of not breaking users of this package,
 * the "kind" for a given metric is guaranteed not to change. If it must change,
 * then a new metric will be introduced with a new key and a new "kind."
 *
 * # Metric key format
 *
 * As mentioned earlier, metric keys are strings. Their format is simple and
 * well-defined, designed to be both human and machine readable. It is split
 * into two components, separated by a colon: a rooted path and a unit. The
 * choice to include the unit in the key is motivated by compatibility: if a
 * metric's unit changes, its semantics likely did also, and a new key should be
 * introduced.
 *
 * For more details on the precise definition of the metric key's path and unit
 * formats, see the documentation of the Name field of the Description struct.
 *
 * # A note about floats
 *
 * This package supports metrics whose values have a floating-point
 * representation. In order to improve ease-of-use, this package promises to
 * never produce the following classes of floating-point values: NaN, infinity.
 *
 * # Supported metrics
 *
 * Below is the full list of supported metrics, ordered lexicographically.
 *
 *     /cgo/go-to-c-calls:calls
 *         Count of calls made from Go to C by the current process.
 *
 *     /cpu/classes/gc/mark/assist:cpu-seconds
 *         Estimated total CPU time goroutines spent performing GC
 *         tasks to assist the GC and prevent it from falling behind the
 *         application. This metric is an overestimate, and not directly
 *         comparable to system CPU time measurements. Compare only with
 *         other /cpu/classes metrics.
 *
 *     /cpu/classes/gc/mark/dedicated:cpu-seconds
 *         Estimated total CPU time spent performing GC tasks on processors
 *         (as defined by GOMAXPROCS) dedicated to those tasks. This metric
 *         is an overestimate, and not directly comparable to system CPU
 *         time measurements. Compare only with other /cpu/classes metrics.
 *
 *     /cpu/classes/gc/mark/idle:cpu-seconds
 *         Estimated total CPU time spent performing GC tasks on spare CPU
 *         resources that the Go scheduler could not otherwise find a use
 *         for. This should be subtracted from the total GC CPU time to
 *         obtain a measure of compulsory GC CPU time. This metric is an
 *         overestimate, and not directly comparable to system CPU time
 *         measurements. Compare only with other /cpu/classes metrics.
 *
 *     /cpu/classes/gc/pause:cpu-seconds
 *         Estimated total CPU time spent with the application paused by
 *         the GC. Even if only one thread is running during the pause,
 *         this is computed as GOMAXPROCS times the pause latency because
 *         nothing else can be executing. This is the exact sum of samples
 *         in /sched/pauses/total/gc:seconds if each sample is multiplied
 *         by GOMAXPROCS at the time it is taken. This metric is an
 *         overestimate, and not directly comparable to system CPU time
 *         measurements. Compare only with other /cpu/classes metrics.
 *
 *     /cpu/classes/gc/total:cpu-seconds
 *         Estimated total CPU time spent performing GC tasks. This metric
 *         is an overestimate, and not directly comparable to system CPU
 *         time measurements. Compare only with other /cpu/classes metrics.
 *         Sum of all metrics in /cpu/classes/gc.
 *
 *     /cpu/classes/idle:cpu-seconds
 *         Estimated total available CPU time not spent executing
 *         any Go or Go runtime code. In other words, the part of
 *         /cpu/classes/total:cpu-seconds that was unused. This metric is
 *         an overestimate, and not directly comparable to system CPU time
 *         measurements. Compare only with other /cpu/classes metrics.
 *
 *     /cpu/classes/scavenge/assist:cpu-seconds
 *         Estimated total CPU time spent returning unused memory to the
 *         underlying platform in response eagerly to memory pressure. This
 *         metric is an overestimate, and not directly comparable to system
 *         CPU time measurements. Compare only with other /cpu/classes
 *         metrics.
 *
 *     /cpu/classes/scavenge/background:cpu-seconds
 *         Estimated total CPU time spent performing background tasks to
 *         return unused memory to the underlying platform. This metric is
 *         an overestimate, and not directly comparable to system CPU time
 *         measurements. Compare only with other /cpu/classes metrics.
 *
 *     /cpu/classes/scavenge/total:cpu-seconds
 *         Estimated total CPU time spent performing tasks that return
 *         unused memory to the underlying platform. This metric is an
 *         overestimate, and not directly comparable to system CPU time
 *         measurements. Compare only with other /cpu/classes metrics.
 *         Sum of all metrics in /cpu/classes/scavenge.
 *
 *     /cpu/classes/total:cpu-seconds
 *         Estimated total available CPU time for user Go code or the Go
 *         runtime, as defined by GOMAXPROCS. In other words, GOMAXPROCS
 *         integrated over the wall-clock duration this process has been
 *         executing for. This metric is an overestimate, and not directly
 *         comparable to system CPU time measurements. Compare only with
 *         other /cpu/classes metrics. Sum of all metrics in /cpu/classes.
 *
 *     /cpu/classes/user:cpu-seconds
 *         Estimated total CPU time spent running user Go code. This may
 *         also include some small amount of time spent in the Go runtime.
 *         This metric is an overestimate, and not directly comparable
 *         to system CPU time measurements. Compare only with other
 *         /cpu/classes metrics.
 *
 *     /gc/cleanups/executed:cleanups
 *         Approximate total count of cleanup functions (created
 *         by runtime.AddCleanup) executed by the runtime. Subtract
 *         /gc/cleanups/queued:cleanups to approximate cleanup queue
 *         length. Useful for detecting slow cleanups holding up the queue.
 *
 *     /gc/cleanups/queued:cleanups
 *         Approximate total count of cleanup functions (created by
 *         runtime.AddCleanup) queued by the runtime for execution.
 *         Subtract from /gc/cleanups/executed:cleanups to approximate
 *         cleanup queue length. Useful for detecting slow cleanups holding
 *         up the queue.
 *
 *     /gc/cycles/automatic:gc-cycles
 *         Count of completed GC cycles generated by the Go runtime.
 *
 *     /gc/cycles/forced:gc-cycles
 *         Count of completed GC cycles forced by the application.
 *
 *     /gc/cycles/total:gc-cycles
 *         Count of all completed GC cycles.
 *
 *     /gc/finalizers/executed:finalizers
 *         Total count of finalizer functions (created by
 *         runtime.SetFinalizer) executed by the runtime. Subtract
 *         /gc/finalizers/queued:finalizers to approximate finalizer queue
 *         length. Useful for detecting finalizers overwhelming the queue,
 *         either by being too slow, or by there being too many of them.
 *
 *     /gc/finalizers/queued:finalizers
 *         Total count of finalizer functions (created by
 *         runtime.SetFinalizer) and queued by the runtime for execution.
 *         Subtract from /gc/finalizers/executed:finalizers to approximate
 *         finalizer queue length. Useful for detecting slow finalizers
 *         holding up the queue.
 *
 *     /gc/gogc:percent
 *         Heap size target percentage configured by the user, otherwise
 *         100. This value is set by the GOGC environment variable, and the
 *         runtime/debug.SetGCPercent function.
 *
 *     /gc/gomemlimit:bytes
 *         Go runtime memory limit configured by the user, otherwise
 *         math.MaxInt64. This value is set by the GOMEMLIMIT environment
 *         variable, and the runtime/debug.SetMemoryLimit function.
 *
 *     /gc/heap/allocs-by-size:bytes
 *         Distribution of heap allocations by approximate size.
 *         Bucket counts increase monotonically. Note that this does not
 *         include tiny objects as defined by /gc/heap/tiny/allocs:objects,
 *         only tiny blocks.
 *
 *     /gc/heap/allocs:bytes
 *         Cumulative sum of memory allocated to the heap by the
 *         application.
 *
 *     /gc/heap/allocs:objects
 *         Cumulative count of heap allocations triggered by the
 *         application. Note that this does not include tiny objects as
 *         defined by /gc/heap/tiny/allocs:objects, only tiny blocks.
 *
 *     /gc/heap/frees-by-size:bytes
 *         Distribution of freed heap allocations by approximate size.
 *         Bucket counts increase monotonically. Note that this does not
 *         include tiny objects as defined by /gc/heap/tiny/allocs:objects,
 *         only tiny blocks.
 *
 *     /gc/heap/frees:bytes
 *         Cumulative sum of heap memory freed by the garbage collector.
 *
 *     /gc/heap/frees:objects
 *         Cumulative count of heap allocations whose storage was freed
 *         by the garbage collector. Note that this does not include tiny
 *         objects as defined by /gc/heap/tiny/allocs:objects, only tiny
 *         blocks.
 *
 *     /gc/heap/goal:bytes
 *         Heap size target for the end of the GC cycle.
 *
 *     /gc/heap/live:bytes
 *         Heap memory occupied by live objects that were marked by the
 *         previous GC.
 *
 *     /gc/heap/objects:objects
 *         Number of objects, live or unswept, occupying heap memory.
 *
 *     /gc/heap/tiny/allocs:objects
 *         Count of small allocations that are packed together into blocks.
 *         These allocations are counted separately from other allocations
 *         because each individual allocation is not tracked by the
 *         runtime, only their block. Each block is already accounted for
 *         in allocs-by-size and frees-by-size.
 *
 *     /gc/limiter/last-enabled:gc-cycle
 *         GC cycle the last time the GC CPU limiter was enabled.
 *         This metric is useful for diagnosing the root cause of an
 *         out-of-memory error, because the limiter trades memory for CPU
 *         time when the GC's CPU time gets too high. This is most likely
 *         to occur with use of SetMemoryLimit. The first GC cycle is cycle
 *         1, so a value of 0 indicates that it was never enabled.
 *
 *     /gc/pauses:seconds
 *         Deprecated. Prefer the identical /sched/pauses/total/gc:seconds.
 *
 *     /gc/scan/globals:bytes
 *         The total amount of global variable space that is scannable.
 *
 *     /gc/scan/heap:bytes
 *         The total amount of heap space that is scannable.
 *
 *     /gc/scan/stack:bytes
 *         The number of bytes of stack that were scanned last GC cycle.
 *
 *     /gc/scan/total:bytes
 *         The total amount space that is scannable. Sum of all metrics in
 *         /gc/scan.
 *
 *     /gc/stack/starting-size:bytes
 *         The stack size of new goroutines.
 *
 *     /godebug/non-default-behavior/allowmultiplevcs:events
 *         The number of non-default behaviors executed by the cmd/go
 *         package due to a non-default GODEBUG=allowmultiplevcs=...
 *         setting.
 *
 *     /godebug/non-default-behavior/containermaxprocs:events
 *         The number of non-default behaviors executed by the runtime
 *         package due to a non-default GODEBUG=containermaxprocs=...
 *         setting.
 *
 *     /godebug/non-default-behavior/cryptocustomrand:events
 *         The number of non-default behaviors executed by the crypto
 *         package due to a non-default GODEBUG=cryptocustomrand=...
 *         setting.
 *
 *     /godebug/non-default-behavior/embedfollowsymlinks:events
 *         The number of non-default behaviors executed by the cmd/go
 *         package due to a non-default GODEBUG=embedfollowsymlinks=...
 *         setting.
 *
 *     /godebug/non-default-behavior/execerrdot:events
 *         The number of non-default behaviors executed by the os/exec
 *         package due to a non-default GODEBUG=execerrdot=... setting.
 *
 *     /godebug/non-default-behavior/fips140ems:events
 *         The number of non-default behaviors executed by the crypto/tls
 *         package due to a non-default GODEBUG=fips140ems=... setting.
 *
 *     /godebug/non-default-behavior/gocachehash:events
 *         The number of non-default behaviors executed by the cmd/go
 *         package due to a non-default GODEBUG=gocachehash=... setting.
 *
 *     /godebug/non-default-behavior/gocachetest:events
 *         The number of non-default behaviors executed by the cmd/go
 *         package due to a non-default GODEBUG=gocachetest=... setting.
 *
 *     /godebug/non-default-behavior/gocacheverify:events
 *         The number of non-default behaviors executed by the cmd/go
 *         package due to a non-default GODEBUG=gocacheverify=... setting.
 *
 *     /godebug/non-default-behavior/gotestjsonbuildtext:events
 *         The number of non-default behaviors executed by the cmd/go
 *         package due to a non-default GODEBUG=gotestjsonbuildtext=...
 *         setting.
 *
 *     /godebug/non-default-behavior/htmlmetacontenturlescape:events
 *         The number of non-default behaviors executed by
 *         the html/template package due to a non-default
 *         GODEBUG=htmlmetacontenturlescape=... setting.
 *
 *     /godebug/non-default-behavior/http2client:events
 *         The number of non-default behaviors executed by the net/http
 *         package due to a non-default GODEBUG=http2client=... setting.
 *
 *     /godebug/non-default-behavior/http2server:events
 *         The number of non-default behaviors executed by the net/http
 *         package due to a non-default GODEBUG=http2server=... setting.
 *
 *     /godebug/non-default-behavior/httpcookiemaxnum:events
 *         The number of non-default behaviors executed by the net/http
 *         package due to a non-default GODEBUG=httpcookiemaxnum=...
 *         setting.
 *
 *     /godebug/non-default-behavior/httplaxcontentlength:events
 *         The number of non-default behaviors executed by the net/http
 *         package due to a non-default GODEBUG=httplaxcontentlength=...
 *         setting.
 *
 *     /godebug/non-default-behavior/httpmuxgo121:events
 *         The number of non-default behaviors executed by the net/http
 *         package due to a non-default GODEBUG=httpmuxgo121=... setting.
 *
 *     /godebug/non-default-behavior/httpservecontentkeepheaders:events
 *         The number of non-default behaviors executed
 *         by the net/http package due to a non-default
 *         GODEBUG=httpservecontentkeepheaders=... setting.
 *
 *     /godebug/non-default-behavior/httpservecontentmaxranges:events
 *         The number of non-default behaviors executed
 *         by the net/http package due to a non-default
 *         GODEBUG=httpservecontentmaxranges=... setting.
 *
 *     /godebug/non-default-behavior/installgoroot:events
 *         The number of non-default behaviors executed by the go/build
 *         package due to a non-default GODEBUG=installgoroot=... setting.
 *
 *     /godebug/non-default-behavior/multipartmaxheaders:events
 *         The number of non-default behaviors executed by
 *         the mime/multipart package due to a non-default
 *         GODEBUG=multipartmaxheaders=... setting.
 *
 *     /godebug/non-default-behavior/multipartmaxparts:events
 *         The number of non-default behaviors executed by
 *         the mime/multipart package due to a non-default
 *         GODEBUG=multipartmaxparts=... setting.
 *
 *     /godebug/non-default-behavior/multipathtcp:events
 *         The number of non-default behaviors executed by the net package
 *         due to a non-default GODEBUG=multipathtcp=... setting.
 *
 *     /godebug/non-default-behavior/netedns0:events
 *         The number of non-default behaviors executed by the net package
 *         due to a non-default GODEBUG=netedns0=... setting.
 *
 *     /godebug/non-default-behavior/panicnil:events
 *         The number of non-default behaviors executed by the runtime
 *         package due to a non-default GODEBUG=panicnil=... setting.
 *
 *     /godebug/non-default-behavior/randautoseed:events
 *         The number of non-default behaviors executed by the math/rand
 *         package due to a non-default GODEBUG=randautoseed=... setting.
 *
 *     /godebug/non-default-behavior/randseednop:events
 *         The number of non-default behaviors executed by the math/rand
 *         package due to a non-default GODEBUG=randseednop=... setting.
 *
 *     /godebug/non-default-behavior/rsa1024min:events
 *         The number of non-default behaviors executed by the crypto/rsa
 *         package due to a non-default GODEBUG=rsa1024min=... setting.
 *
 *     /godebug/non-default-behavior/tarinsecurepath:events
 *         The number of non-default behaviors executed by the archive/tar
 *         package due to a non-default GODEBUG=tarinsecurepath=...
 *         setting.
 *
 *     /godebug/non-default-behavior/tlsmaxrsasize:events
 *         The number of non-default behaviors executed by the crypto/tls
 *         package due to a non-default GODEBUG=tlsmaxrsasize=... setting.
 *
 *     /godebug/non-default-behavior/tlssha1:events
 *         The number of non-default behaviors executed by the crypto/tls
 *         package due to a non-default GODEBUG=tlssha1=... setting.
 *
 *     /godebug/non-default-behavior/updatemaxprocs:events
 *         The number of non-default behaviors executed by the runtime
 *         package due to a non-default GODEBUG=updatemaxprocs=... setting.
 *
 *     /godebug/non-default-behavior/urlmaxqueryparams:events
 *         The number of non-default behaviors executed by the net/url
 *         package due to a non-default GODEBUG=urlmaxqueryparams=...
 *         setting.
 *
 *     /godebug/non-default-behavior/urlstrictcolons:events
 *         The number of non-default behaviors executed by the net/url
 *         package due to a non-default GODEBUG=urlstrictcolons=...
 *         setting.
 *
 *     /godebug/non-default-behavior/winreadlinkvolume:events
 *         The number of non-default behaviors executed by the os package
 *         due to a non-default GODEBUG=winreadlinkvolume=... setting.
 *
 *     /godebug/non-default-behavior/winsymlink:events
 *         The number of non-default behaviors executed by the os package
 *         due to a non-default GODEBUG=winsymlink=... setting.
 *
 *     /godebug/non-default-behavior/x509negativeserial:events
 *         The number of non-default behaviors executed by the crypto/x509
 *         package due to a non-default GODEBUG=x509negativeserial=...
 *         setting.
 *
 *     /godebug/non-default-behavior/x509rsacrt:events
 *         The number of non-default behaviors executed by the crypto/x509
 *         package due to a non-default GODEBUG=x509rsacrt=... setting.
 *
 *     /godebug/non-default-behavior/x509sha256skid:events
 *         The number of non-default behaviors executed by the crypto/x509
 *         package due to a non-default GODEBUG=x509sha256skid=... setting.
 *
 *     /godebug/non-default-behavior/x509sslcertoverrideplatform:events
 *         The number of non-default behaviors executed by
 *         the crypto/x509 package due to a non-default
 *         GODEBUG=x509sslcertoverrideplatform=... setting.
 *
 *     /godebug/non-default-behavior/x509usefallbackroots:events
 *         The number of non-default behaviors executed by the crypto/x509
 *         package due to a non-default GODEBUG=x509usefallbackroots=...
 *         setting.
 *
 *     /godebug/non-default-behavior/x509usepolicies:events
 *         The number of non-default behaviors executed by the crypto/x509
 *         package due to a non-default GODEBUG=x509usepolicies=...
 *         setting.
 *
 *     /godebug/non-default-behavior/zipinsecurepath:events
 *         The number of non-default behaviors executed by the archive/zip
 *         package due to a non-default GODEBUG=zipinsecurepath=...
 *         setting.
 *
 *     /memory/classes/heap/free:bytes
 *         Memory that is completely free and eligible to be returned to
 *         the underlying system, but has not been. This metric is the
 *         runtime's estimate of free address space that is backed by
 *         physical memory.
 *
 *     /memory/classes/heap/objects:bytes
 *         Memory occupied by live objects and dead objects that have not
 *         yet been marked free by the garbage collector.
 *
 *     /memory/classes/heap/released:bytes
 *         Memory that is completely free and has been returned to the
 *         underlying system. This metric is the runtime's estimate of free
 *         address space that is still mapped into the process, but is not
 *         backed by physical memory.
 *
 *     /memory/classes/heap/stacks:bytes
 *         Memory allocated from the heap that is reserved for stack space,
 *         whether or not it is currently in-use. Currently, this
 *         represents all stack memory for goroutines. It also includes all
 *         OS thread stacks in non-cgo programs. Note that stacks may be
 *         allocated differently in the future, and this may change.
 *
 *     /memory/classes/heap/unused:bytes
 *         Memory that is reserved for heap objects but is not currently
 *         used to hold heap objects.
 *
 *     /memory/classes/metadata/mcache/free:bytes
 *         Memory that is reserved for runtime mcache structures, but not
 *         in-use.
 *
 *     /memory/classes/metadata/mcache/inuse:bytes
 *         Memory that is occupied by runtime mcache structures that are
 *         currently being used.
 *
 *     /memory/classes/metadata/mspan/free:bytes
 *         Memory that is reserved for runtime mspan structures, but not
 *         in-use.
 *
 *     /memory/classes/metadata/mspan/inuse:bytes
 *         Memory that is occupied by runtime mspan structures that are
 *         currently being used.
 *
 *     /memory/classes/metadata/other:bytes
 *         Memory that is reserved for or used to hold runtime metadata.
 *
 *     /memory/classes/os-stacks:bytes
 *         Stack memory allocated by the underlying operating system.
 *         In non-cgo programs this metric is currently zero. This may
 *         change in the future.In cgo programs this metric includes
 *         OS thread stacks allocated directly from the OS. Currently,
 *         this only accounts for one stack in c-shared and c-archive build
 *         modes, and other sources of stacks from the OS are not measured.
 *         This too may change in the future.
 *
 *     /memory/classes/other:bytes
 *         Memory used by execution trace buffers, structures for debugging
 *         the runtime, finalizer and profiler specials, and more.
 *
 *     /memory/classes/profiling/buckets:bytes
 *         Memory that is used by the stack trace hash map used for
 *         profiling.
 *
 *     /memory/classes/total:bytes
 *         All memory mapped by the Go runtime into the current process
 *         as read-write. Note that this does not include memory mapped
 *         by code called via cgo or via the syscall package. Sum of all
 *         metrics in /memory/classes.
 *
 *     /sched/gomaxprocs:threads
 *         The current runtime.GOMAXPROCS setting, or the number of
 *         operating system threads that can execute user-level Go code
 *         simultaneously.
 *
 *     /sched/goroutines-created:goroutines
 *         Count of goroutines created since program start.
 *
 *     /sched/goroutines/not-in-go:goroutines
 *         Approximate count of goroutines running or blocked in
 *         a system call or cgo call. Not guaranteed to add up to
 *         /sched/goroutines:goroutines with other goroutine metrics.
 *
 *     /sched/goroutines/runnable:goroutines
 *         Approximate count of goroutines ready to execute,
 *         but not executing. Not guaranteed to add up to
 *         /sched/goroutines:goroutines with other goroutine metrics.
 *
 *     /sched/goroutines/running:goroutines
 *         Approximate count of goroutines executing. Always less than or
 *         equal to /sched/gomaxprocs:threads. Not guaranteed to add up to
 *         /sched/goroutines:goroutines with other goroutine metrics.
 *
 *     /sched/goroutines/waiting:goroutines
 *         Approximate count of goroutines waiting on a resource
 *         (I/O or sync primitives). Not guaranteed to add up to
 *         /sched/goroutines:goroutines with other goroutine metrics.
 *
 *     /sched/goroutines:goroutines
 *         Count of live goroutines.
 *
 *     /sched/latencies:seconds
 *         Distribution of the time goroutines have spent in the scheduler
 *         in a runnable state before actually running. Bucket counts
 *         increase monotonically.
 *
 *     /sched/pauses/stopping/gc:seconds
 *         Distribution of individual GC-related stop-the-world stopping
 *         latencies. This is the time it takes from deciding to stop the
 *         world until all Ps are stopped. This is a subset of the total
 *         GC-related stop-the-world time (/sched/pauses/total/gc:seconds).
 *         During this time, some threads may be executing. Bucket counts
 *         increase monotonically.
 *
 *     /sched/pauses/stopping/other:seconds
 *         Distribution of individual non-GC-related stop-the-world
 *         stopping latencies. This is the time it takes from deciding
 *         to stop the world until all Ps are stopped. This is a
 *         subset of the total non-GC-related stop-the-world time
 *         (/sched/pauses/total/other:seconds). During this time, some
 *         threads may be executing. Bucket counts increase monotonically.
 *
 *     /sched/pauses/total/gc:seconds
 *         Distribution of individual GC-related stop-the-world pause
 *         latencies. This is the time from deciding to stop the world
 *         until the world is started again. Some of this time is spent
 *         getting all threads to stop (this is measured directly in
 *         /sched/pauses/stopping/gc:seconds), during which some threads
 *         may still be running. Bucket counts increase monotonically.
 *
 *     /sched/pauses/total/other:seconds
 *         Distribution of individual non-GC-related stop-the-world
 *         pause latencies. This is the time from deciding to stop the
 *         world until the world is started again. Some of this time
 *         is spent getting all threads to stop (measured directly in
 *         /sched/pauses/stopping/other:seconds). Bucket counts increase
 *         monotonically.
 *
 *     /sched/threads/total:threads
 *         The current count of live threads that are owned by the Go
 *         runtime.
 *
 *     /sync/mutex/wait/total:seconds
 *         Approximate cumulative time goroutines have spent blocked on a
 *         sync.Mutex, sync.RWMutex, or runtime-internal lock. This metric
 *         is useful for identifying global changes in lock contention.
 *         Collect a mutex or block profile using the runtime/pprof package
 *         for more detailed contention data. */

/* burrow:package runtime/metrics */

#ifndef BURROW_RUNTIME_METRICS_H
#define BURROW_RUNTIME_METRICS_H

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Not in Go: where it differs from Go.
 *
 * Read takes the allocator the histograms come from:
 *
 *     MetricsSample s[2] = {
 *         {.name = BURROW_S("/sched/goroutines:goroutines")},
 *         {.name = BURROW_S("/gc/heap/allocs-by-size:bytes")},
 *     };
 *     metrics_read(a, slice_from(s, 2, 2, TYPE_METRICS_SAMPLE));
 *     uint64_t n = metrics_value_uint64(s[0].value);
 *     MetricsFloat64Histogram *h = metrics_value_float64_histogram(s[1].value);
 *
 * A histogram is made the first time a sample needs one and reused when the
 * same sample is read again, as in Go, so reading the same samples over and
 * over allocates once. Nothing frees it, so read into an arena or keep the
 * samples. Running out of memory panics.
 *
 * Every metric in the list is there and has the kind the list gives it, but
 * burrow's runtime does not keep every number Go's does, and a metric it has
 * no number for reads zero. These are the ones with real values:
 *
 *   - /sched/gomaxprocs:threads, /sched/threads/total:threads,
 *     /sched/goroutines-created:goroutines, and the goroutine counts, which
 *     come from looking at each goroutine's state. not-in-go counts the ones
 *     in a system call.
 *   - /gc/stack/starting-size:bytes, the stack every goroutine starts with
 *     and keeps, since stacks do not grow here.
 *   - /gc/heap/allocs:bytes and /gc/heap/allocs:objects, which count what went
 *     through heap_allocator while testing measured a benchmark. The bytes
 *     also count what the collector handed out.
 *   - /gc/heap/live:bytes, /memory/classes/heap/objects:bytes and the GC cycle
 *     counts, while the collector from burrow/mem/gc.h is running.
 *     /gc/heap/live:bytes is what is in use in its heap now rather than what
 *     the last cycle marked.
 *   - /gc/gogc:percent is always 100 and /gc/gomemlimit:bytes is always
 *     math.MaxInt64, Go's defaults, since there is no GOGC or GOMEMLIMIT.
 *
 * These are the numbers runtime_read_mem_stats reports, so the other
 * /memory/classes metrics are the fields of RuntimeMemStats that Go's runtime
 * relates them to, and /memory/classes/total:bytes is their sum. The rest read
 * zero: the CPU time classes, the cgo calls, the scan sizes, the finalizers and
 * cleanups, the frees, the by-size histograms, the pause and latency
 * histograms, the mutex wait time and the GODEBUG counts. A histogram that
 * reads zero still has Go's buckets. */

/* ValueKind is a tag for a metric [Value] which indicates its type. */
typedef Int MetricsValueKind;
extern const Type burrow_type_MetricsValueKind;
#define TYPE_METRICS_VALUE_KIND TYPE_OF(MetricsValueKind)

/* KindBad indicates that the Value has no type and should not be used. */
#define METRICS_KIND_BAD ((MetricsValueKind)0)

/* KindUint64 indicates that the type of the Value is a uint64. */
#define METRICS_KIND_UINT64 ((MetricsValueKind)1)

/* KindFloat64 indicates that the type of the Value is a float64. */
#define METRICS_KIND_FLOAT64 ((MetricsValueKind)2)

/* KindFloat64Histogram indicates that the type of the Value is a
 * *Float64Histogram. */
#define METRICS_KIND_FLOAT64_HISTOGRAM ((MetricsValueKind)3)

/* Value represents a metric value returned by the runtime.
 *
 * scalar contains scalar values for scalar Kinds, and pointer contains
 * non-scalar values. */
#define METRICS_VALUE_FIELDS(F, T)                                                     \
    F(T, MetricsValueKind, kind, kind, "")                                             \
    F(T, uint64_t, scalar, scalar, "")                                                 \
    F(T, UnsafePointer, pointer, pointer, "")

BURROW_STRUCT_AS_DECL(MetricsValue, METRICS_VALUE_FIELDS);
#define TYPE_METRICS_VALUE TYPE_OF(MetricsValue)

/* Kind returns the tag representing the kind of value this is. */
MetricsValueKind metrics_value_kind(MetricsValue v);

/* Uint64 returns the internal uint64 value for the metric.
 *
 * If v.Kind() != KindUint64, this method panics. */
uint64_t metrics_value_uint64(MetricsValue v);

/* Float64 returns the internal float64 value for the metric.
 *
 * If v.Kind() != KindFloat64, this method panics. */
double metrics_value_float64(MetricsValue v);

/* The []uint64 and []float64 a Float64Histogram holds. */
BURROW_SLICE_TYPE_DECL(MetricsUint64Slice, uint64_t);
BURROW_SLICE_TYPE_DECL(MetricsFloat64Slice, double);

/* Float64Histogram represents a distribution of float64 values.
 *
 * Counts contains the weights for each histogram bucket.
 *
 * Given N buckets, Count[n] is the weight of the range [bucket[n],
 * bucket[n+1]), for 0 <= n < N.
 *
 * Buckets contains the boundaries of the histogram buckets, in increasing
 * order.
 *
 * Buckets[0] is the inclusive lower bound of the minimum bucket while
 * Buckets[len(Buckets)-1] is the exclusive upper bound of the maximum bucket.
 * Hence, there are len(Buckets)-1 counts. Furthermore, len(Buckets) != 1,
 * always, since at least two boundaries are required to describe one bucket
 * (and 0 boundaries are used to describe 0 buckets).
 *
 * Buckets[0] is permitted to have value -Inf and Buckets[len(Buckets)-1] is
 * permitted to have value Inf.
 *
 * For a given metric name, the value of Buckets is guaranteed not to change
 * between calls until program exit.
 *
 * This slice value is permitted to alias with other Float64Histograms' Buckets
 * fields, so the values within should only ever be read. If they need to be
 * modified, the user must make a copy. */
#define METRICS_FLOAT64_HISTOGRAM_FIELDS(F, T)                                         \
    F(T, MetricsUint64Slice, counts, Counts, "")                                       \
    F(T, MetricsFloat64Slice, buckets, Buckets, "")

BURROW_STRUCT_AS_DECL(MetricsFloat64Histogram, METRICS_FLOAT64_HISTOGRAM_FIELDS);
#define TYPE_METRICS_FLOAT64_HISTOGRAM TYPE_OF(MetricsFloat64Histogram)

/* Float64Histogram returns the internal *Float64Histogram value for the metric.
 *
 * If v.Kind() != KindFloat64Histogram, this method panics. */
BURROW_BORROWS(ret, v) MetricsFloat64Histogram *
metrics_value_float64_histogram(MetricsValue v);

/* Description describes a runtime metric.
 *
 * Name is the full name of the metric which includes the unit.
 *
 * The format of the metric may be described by the following regular
 * expression.
 *
 *     ^(?P<name>/[^:]+):(?P<unit>[^:*\/]+(?:[*\/][^:*\/]+)*)$
 *
 * The format splits the name into two components, separated by a colon: a
 * path which always starts with a /, and a machine-parseable unit. The name
 * may contain any valid Unicode codepoint in between / characters, but by
 * convention will try to stick to lowercase characters and hyphens. An example
 * of such a path might be "/memory/heap/free".
 *
 * The unit is by convention a series of lowercase English unit names (singular
 * or plural) without prefixes delimited by '*' or '/'. The unit names may
 * contain any valid Unicode codepoint that is not a delimiter. Examples of
 * units might be "seconds", "bytes", "bytes/second", "cpu-seconds",
 * "byte*cpu-seconds", and "bytes/second/second".
 *
 * For histograms, multiple units may apply. For instance, the units of the
 * buckets and the count. By convention, for histograms, the units of the count
 * are always "samples" with the type of sample evident by the metric's name,
 * while the unit in the name specifies the buckets' unit.
 *
 * A complete name might look like "/memory/heap/free:bytes".
 *
 * Not in Go: the expression has \/ for each / that comes after a *, since in C
 * the two would end this comment.
 *
 * Description is an English language sentence describing the metric.
 *
 * Kind is the kind of value for this metric.
 *
 * The purpose of this field is to allow users to filter out metrics whose
 * values are types which their application may not understand.
 *
 * Cumulative is whether or not the metric is cumulative. If a cumulative metric
 * is just a single number, then it increases monotonically. If the metric is a
 * distribution, then each bucket count increases monotonically.
 *
 * This flag thus indicates whether or not it's useful to compute a rate from
 * this value. */
#define METRICS_DESCRIPTION_FIELDS(F, T)                                               \
    F(T, Str, name, Name, "")                                                          \
    F(T, Str, description, Description, "")                                            \
    F(T, MetricsValueKind, kind, Kind, "")                                             \
    F(T, bool, cumulative, Cumulative, "")

BURROW_STRUCT_AS_DECL(MetricsDescription, METRICS_DESCRIPTION_FIELDS);
#define TYPE_METRICS_DESCRIPTION TYPE_OF(MetricsDescription)

/* All returns a slice of containing metric descriptions for all supported
 * metrics.
 *
 * Not in Go: the slice holds MetricsDescription and is the library's own, so do
 * not write to it. */
BURROW_STATIC(ret) Slice metrics_all(void);

/* Sample captures a single metric sample.
 *
 * Name is the name of the metric sampled.
 *
 * It must correspond to a name in one of the metric descriptions returned by
 * All.
 *
 * Value is the value of the metric sample. */
#define METRICS_SAMPLE_FIELDS(F, T)                                                    \
    F(T, Str, name, Name, "")                                                          \
    F(T, MetricsValue, value, Value, "")

BURROW_STRUCT_AS_DECL(MetricsSample, METRICS_SAMPLE_FIELDS);
#define TYPE_METRICS_SAMPLE TYPE_OF(MetricsSample)

/* Read populates each [Value] field in the given slice of metric samples.
 *
 * Desired metrics should be present in the slice with the appropriate name.
 * The user of this API is encouraged to re-use the same slice between calls
 * for efficiency, but is not required to do so.
 *
 * Note that re-use has some caveats. Notably, Values should not be read or
 * manipulated while a Read with that value is outstanding; that is a data
 * race. This property includes pointer-typed Values (for example,
 * [Float64Histogram]) whose underlying storage will be reused by Read when
 * possible. To safely use such values in a concurrent setting, all data must be
 * deep-copied.
 *
 * It is safe to execute multiple Read calls concurrently, but their arguments
 * must share no underlying memory. When in doubt, create a new []Sample from
 * scratch, which is always safe, though may be inefficient.
 *
 * Sample values with names not appearing in [All] will have their Value
 * populated as KindBad to indicate that the name is unknown.
 *
 * Not in Go: m holds MetricsSample, and a histogram that has to be made comes
 * from a. */
void metrics_read(Alloc *a, Slice m);

/* Not in Go: for the tests. The names the runtime has a reader for, which is
 * Go's runtime_readMetricNames, in a Slice of Str made in a. */
BURROW_OWNS(ret) Slice burrow__metrics_read_names(Alloc *a);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_RUNTIME_METRICS_H */
