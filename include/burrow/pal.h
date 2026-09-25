/* The platform abstraction layer: the only place in burrow that talks to an
 * operating system.
 *
 * Go's runtime keeps its system calls in one place per platform and builds
 * everything else on top of them. That is why the same os package works on
 * Linux, macOS and Windows without a single build tag above the runtime, and it
 * is what this header is for. Everything above here, which is all of burrow, is
 * portable C that calls these functions and never a kernel.
 *
 * Three rules, from docs/design/10-packages-os.md:
 *
 *   1. No feature detection at build time. Optional kernel facilities are
 *      probed once at runtime and cached. One binary runs on an old kernel and
 *      a new one, and there is no configure step.
 *   2. No libc where a syscall will do, on Linux. On macOS libSystem is the
 *      supported interface and we use it. On Windows it is Win32 only, never
 *      the CRT's POSIX emulation, which has the wrong semantics for nearly
 *      everything it emulates.
 *   3. One file per platform per group, never an #ifdef forest inside a
 *      function. Where two platforms want the same code the file is shared and
 *      named for the family rather than copied, which is what src/pal/vm_posix.c
 *      is. Either way the whole file is under one #if and a reader can tell
 *      what runs on their machine by looking at the top of it.
 *
 * The conventions everything below follows are written out under the include
 * block, and tools/check-pal.sh is what keeps the first sentence above true.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_PAL_H
#define BURROW_PAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "burrow/own.h"
#include "burrow/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What crosses this boundary is C and nothing else. No Str, no Slice, no Error,
 * no allocator. Paths are NUL terminated UTF-8. Handles are int64_t, which
 * holds a POSIX file descriptor and a Win32 HANDLE both, and -1 means there
 * isn't one. Buffers come from the caller because nothing in here allocates,
 * which is what lets the PAL be called from a signal handler and from a
 * goroutine that is in the middle of growing its own stack.
 *
 * Failure is a PalErrno, out through the last parameter, which may be NULL if
 * you only want to know whether it worked. The code is one of ours rather than
 * the platform's, because making three platforms report the same thing the same
 * way is most of what this layer is for. A backend translates on the way out
 * and a raw errno never escapes. The native code is deliberately dropped: the
 * syscall package is where a caller goes for the number their platform actually
 * returned, and it does not come through here.
 *
 * Not every entry point has a backend yet. The ones that don't are declared
 * anyway, because the shape of the boundary is a decision worth writing down
 * once rather than discovering package by package, and because a call to a
 * missing one is a link error that names it. Today the time, memory, random,
 * machine query, poll, thread, stdout capture, signal and file groups are
 * implemented and in use, and process, dynamic loading, user and net land with
 * the packages that need them. Each group below says where it
 * stands. */

/* ------------------------------------------------------------------ failure
 *
 * Zero is success and every other value is a reason. The numbers are ours and
 * they are not any platform's: a backend that forgets to translate hands back a
 * Linux errno, and a Linux errno that happens to be 2 would silently read as
 * PAL_ENOENT here, which is why nothing in this enum starts at 1.
 *
 * The set is the one Go's os and net packages need in order to answer
 * ErrNotExist, ErrExist, ErrPermission, ErrDeadlineExceeded and the handful of
 * net errors that callers actually branch on. A backend that meets something
 * outside the set reports PAL_EOTHER, which is honest rather than a wrong
 * guess, and the fix is to add the case here rather than to widen a mapping. */
typedef int32_t PalErrno;

enum {
    PAL_OK = 0,

    PAL_EPERM = 1000,    /* not the owner */
    PAL_ENOENT,          /* no such file or directory */
    PAL_ESRCH,           /* no such process */
    PAL_EINTR,           /* interrupted by a signal */
    PAL_EIO,             /* input or output error */
    PAL_EBADF,           /* not an open descriptor */
    PAL_ECHILD,          /* no child processes */
    PAL_EDEADLK,         /* would deadlock */
    PAL_EAGAIN,          /* would block, try again */
    PAL_ENOMEM,          /* out of memory */
    PAL_EACCES,          /* permission denied */
    PAL_EFAULT,          /* bad address */
    PAL_EBUSY,           /* resource busy */
    PAL_EEXIST,          /* already exists */
    PAL_EXDEV,           /* cross device link */
    PAL_ENODEV,          /* no such device */
    PAL_ENOTDIR,         /* not a directory */
    PAL_EISDIR,          /* is a directory */
    PAL_EINVAL,          /* invalid argument */
    PAL_ENFILE,          /* system descriptor table full */
    PAL_EMFILE,          /* process descriptor table full */
    PAL_ENOTTY,          /* not a terminal */
    PAL_EFBIG,           /* file too large */
    PAL_ENOSPC,          /* no space left on device */
    PAL_ESPIPE,          /* not seekable */
    PAL_EROFS,           /* read only file system */
    PAL_EMLINK,          /* too many links */
    PAL_EPIPE,           /* broken pipe */
    PAL_ERANGE,          /* result out of range */
    PAL_ENAMETOOLONG,    /* name too long */
    PAL_ENOSYS,          /* not implemented by this kernel */
    PAL_ENOTEMPTY,       /* directory not empty */
    PAL_ELOOP,           /* too many symbolic links */
    PAL_ENOTSUP,         /* not supported on this platform */
    PAL_EOVERFLOW,       /* does not fit in the type it has to go in */
    PAL_ECANCELED,       /* cancelled before it happened */
    PAL_ETIMEDOUT,       /* timed out */
    PAL_EADDRINUSE,      /* address already in use */
    PAL_EADDRNOTAVAIL,   /* address not available here */
    PAL_ENETDOWN,        /* network is down */
    PAL_ENETUNREACH,     /* network unreachable */
    PAL_ECONNABORTED,    /* connection aborted locally */
    PAL_ECONNRESET,      /* connection reset by peer */
    PAL_ENOBUFS,         /* no buffer space */
    PAL_EISCONN,         /* already connected */
    PAL_ENOTCONN,        /* not connected */
    PAL_ECONNREFUSED,    /* connection refused */
    PAL_EHOSTUNREACH,    /* host unreachable */
    PAL_EALREADY,        /* already in progress */
    PAL_EINPROGRESS,     /* in progress, not finished */
    PAL_EPROTONOSUPPORT, /* protocol not supported */
    PAL_EAFNOSUPPORT,    /* address family not supported */
    PAL_EHOSTNOTFOUND,   /* name resolution found nothing */
    PAL_ETRYAGAIN,       /* name resolution failed for now, retry */
    PAL_EOTHER           /* something outside the set above */
};

/* The message for a code, as a NUL terminated ASCII string that lives forever.
 *
 * This is for a caller that has to say something and has no Error in hand, and
 * for the tests. The os package does not use it: an os error names the
 * operation and the path as well, so it builds its own message and only uses
 * the code to decide which sentinel it wraps. An unknown code gives "unknown
 * error" rather than an empty string. */
BURROW_STATIC(ret) const char *pal_errno_string(PalErrno e);

/* The invalid handle, which is what a failed open gives back. It is -1 on both
 * families: POSIX says so, and Win32's INVALID_HANDLE_VALUE is (HANDLE)-1,
 * which is the same bits once it is an int64_t. */
#define PAL_INVALID_HANDLE ((int64_t)-1)

/* ---------------------------------------------------------------------- time
 *
 * Two clocks, because the two questions are different and confusing them is the
 * oldest bug in timekeeping. The monotonic clock answers how long something
 * took and never goes backwards. The wall clock answers what time it is and
 * goes wherever ntp and the user put it.
 *
 * Neither can fail, so neither takes a PalErrno. There is no system that can
 * answer one call and not the next, and a caller that has to test the time
 * before using it writes worse code for no benefit. A platform with no clock at
 * all returns zero, which is wrong in a way that is obvious rather than in a
 * way that has to be handled at every call site. */

/* Nanoseconds since some arbitrary point that does not change while the process
 * runs. Only differences mean anything. On Linux this is CLOCK_MONOTONIC, on
 * macOS mach_absolute_time scaled by the timebase, on Windows
 * QueryPerformanceCounter scaled by the frequency. Whether it keeps counting
 * while the machine is asleep is the platform's business and not promised here,
 * which matches Go. */
int64_t pal_clock_monotonic(void);

/* Nanoseconds since 1970-01-01 UTC, leap seconds smeared or ignored the way the
 * platform does it. This is the one that jumps. */
int64_t pal_clock_realtime(void);

/* Sleep for at least ns, or return early if a signal arrives. It does not
 * report which happened, because every caller in burrow is a loop around a
 * deadline check and none of them care. A negative or zero ns returns at once
 * after letting the scheduler run, which is what a yield is. */
void pal_nanosleep(int64_t ns);

/* Read a zoneinfo database entry into buf and return its length, or -1.
 *
 * name is a zone name like "Europe/London" or NULL for the system's own zone.
 * The bytes are the TZif file as it is on disk, because the time package parses
 * that format itself on every platform, including Windows, where the backend
 * has to build the equivalent from the registry rather than read a file.
 *
 * Returns the number of bytes written. A buffer too small is PAL_ERANGE and
 * nothing is written. Not implemented yet, and it arrives with time. */
int64_t pal_tz_load(const char *name, void *buf, int64_t cap, PalErrno *err);

/* -------------------------------------------------------------------- memory
 *
 * Reserve and commit as two separate steps, which is Win32's model, because it
 * is the one that can be emulated on POSIX and not the other way round. A
 * goroutine stack reserves its maximum and commits a page, and grows by
 * committing more of a range nobody else can take.
 *
 * Every address and length here has to be a multiple of pal_page_size. The
 * backends do not round for you: a caller that rounds is a caller that knows
 * what it asked for, and a layer that rounds silently hands back more than was
 * asked for and is then asked to release less than it gave. */

/* Reserve address space without backing it with anything. The pages cannot be
 * touched until pal_vm_commit says so. Returns NULL on failure.
 *
 * On POSIX this is mmap PROT_NONE, which counts against the address space limit
 * and not against memory. On Windows it is VirtualAlloc MEM_RESERVE. */
BURROW_OWNS(ret) void *pal_vm_reserve(int64_t bytes, PalErrno *err);

/* Back a range inside a reservation with memory and make it readable and
 * writable. Committing an already committed range is not an error, which is
 * what makes a grow loop simple to write. */
bool pal_vm_commit(void *addr, int64_t bytes, PalErrno *err);

/* Give the memory behind a range back to the system, keeping the reservation so
 * that nothing else can take the addresses. Reading the range afterwards
 * faults. This is how a stack shrinks and how a freed span stops counting
 * against the process. */
bool pal_vm_decommit(void *addr, int64_t bytes, PalErrno *err);

/* Drop a reservation entirely. addr must be exactly what pal_vm_reserve
 * returned, and bytes exactly what it was asked for. Windows needs the first
 * and POSIX needs the second, so both are required on both. */
bool pal_vm_release(void *addr, int64_t bytes, PalErrno *err);

/* Turn a committed range into a guard: any access to it faults. This is the
 * page below a goroutine stack, and it is what turns a stack overflow into a
 * signal the runtime can name rather than into somebody else's corrupted
 * memory. */
bool pal_vm_guard(void *addr, int64_t bytes, PalErrno *err);

/* Map a file, or part of one, into memory. prot is PAL_PROT_*. Returns NULL on
 * failure. Unmap with pal_munmap and not with pal_vm_release, because on
 * Windows the two are different calls and the difference is not hideable.
 * A shared mapping and the file see each other's writes. A zero length is
 * PAL_EINVAL, as it is for mmap(2), and so is asking for a mapping that is
 * both writable and executable, which some systems refuse anyway and none
 * should be asked for. */
BURROW_OWNS(ret) void *pal_mmap(int64_t fd, int64_t off, int64_t len, uint32_t prot,
                                PalErrno *err);
bool pal_munmap(void *addr, int64_t len, PalErrno *err);

enum {
    PAL_PROT_READ = 1u << 0,
    PAL_PROT_WRITE = 1u << 1,
    PAL_PROT_EXEC = 1u << 2,
    /* Writes stay in this process and do not reach the file. */
    PAL_PROT_PRIVATE = 1u << 3
};

/* ----------------------------------------------------------- what we're on
 *
 * Queries about the machine. None of them can fail in a way worth reporting, so
 * each one has a documented answer for the case where the platform will not
 * say, and none of them take a PalErrno. */

/* The page size, which every address and length in the memory group has to be a
 * multiple of. Always a power of two. A platform that will not say gives 4096,
 * which is right nearly everywhere and is the smallest granularity that is safe
 * to assume when it is wrong.
 *
 * Cached after the first call, so calling it in a loop is a load. */
int64_t pal_page_size(void);

/* How many hardware threads the process may run on, which is what Go's
 * NumCPU answers and what the scheduler sizes itself from. It is the
 * affinity limited count where the platform can tell us one, so a container
 * pinned to two cores sees two rather than the ninety six on the host.
 *
 * Asked every time rather than cached, because the mask can be narrowed under a
 * running process and an answer from startup would quietly go stale. Nothing
 * asks this in a loop.
 *
 * Never less than one, including on a platform that will not say. */
int64_t pal_cpu_count(void);

/* The processor's name, the one a benchmark run prints on its cpu: line, into
 * buf as a NUL terminated string cut to fit. Returns its length, which is zero
 * when neither the processor nor the system will say. */
int64_t pal_cpu_name(char *buf, int64_t cap);

/* Instruction set extensions the crypto code can use, as bits of
 * pal_cpu_features. They are the fields of Go's internal/cpu that its hashes
 * check, X86.HasSHA and the rest, and a bit is only ever set on the machine it
 * names. */
#define PAL_CPU_X86_SSSE3 (UINT32_C(1) << 0)
#define PAL_CPU_X86_SSE41 (UINT32_C(1) << 1)
#define PAL_CPU_X86_SHA (UINT32_C(1) << 2)
#define PAL_CPU_ARM64_SHA1 (UINT32_C(1) << 8)
#define PAL_CPU_ARM64_SHA2 (UINT32_C(1) << 9)
#define PAL_CPU_ARM64_SHA512 (UINT32_C(1) << 10)
#define PAL_CPU_ARM64_SHA3 (UINT32_C(1) << 11)

/* The PAL_CPU_ bits this processor has, found on the first call and kept.
 *
 * GODEBUG turns them off the way it does in Go, so GODEBUG=cpu.sha2=off runs
 * the portable SHA-256 on an arm64 machine and cpu.all=off runs the portable
 * code for everything. The names are Go's: ssse3, sse41 and sha on x86, and
 * sha1, sha2, sha512 and sha3 on arm64. The variable is read once, with the
 * features, so setting it after the first call does nothing. */
uint32_t pal_cpu_features(void);

/* The machine's name into buf, NUL terminated, returning its length or -1.
 * A buffer too small is PAL_ERANGE. Not implemented yet, it arrives with os. */
int64_t pal_hostname(char *buf, int64_t cap, PalErrno *err);

/* ------------------------------------------------------------------- random
 *
 * The system generator, which is getrandom on Linux, getentropy on the BSDs and
 * macOS, and RtlGenRandom on Windows. It is the seed for the runtime's own
 * generator and the source under crypto/rand, so it has to be the real one and
 * not a fallback that looks like it.
 *
 * Returns false and says why if the system will not give the bytes. A caller
 * that can carry on with a weaker source has to decide that for itself, because
 * a layer that quietly substitutes one is how a key ends up predictable. */
bool pal_random_bytes(void *buf, int64_t n, PalErrno *err);

/* ------------------------------------------------------------------ threads
 *
 * OS threads, which in Go's terms is an M. Goroutines are not here: they are
 * stacks and a scheduler, both of which are portable C above this line.
 *
 * The whole group is implemented and in use. src/runtime/thread.c and
 * src/runtime/note.c are both portable C over it on every platform. */

/* Start a thread running fn(arg) on a stack of at least stack_bytes, or 0 for
 * the platform's default. Returns a handle, or PAL_INVALID_HANDLE.
 *
 * A size below the platform's minimum is raised to the minimum rather than
 * refused, because the minimum is 16 kilobytes on macOS and 128 on glibc arm64
 * and a caller asking for a small stack means small rather than exactly that.
 * It is also rounded up to a whole page, because macOS refuses one that is not.
 *
 * The thread is joinable. A thread that is neither joined nor detached leaks
 * its handle on Windows and its stack on POSIX, so the runtime accounts for
 * every one it starts, including on the way out.
 *
 * This is the one call in the group that does real work to exist. A platform
 * thread entry point has room for exactly one pointer and this takes two, so
 * the two are handed over in a small structure on the starter's own stack and
 * the starter waits on pal_futex_wait until the new thread has copied them out.
 * That costs one handshake per thread, which is nothing next to what starting a
 * thread costs anyway, and it is what keeps the layer free of allocation. */
int64_t pal_thread_create(void (*fn)(void *), void *arg, int64_t stack_bytes,
                          PalErrno *err);

/* Wait for a thread to finish and release its handle. */
bool pal_thread_join(int64_t thread, PalErrno *err);

/* Say the thread will never be joined, so the system can release what it holds
 * as soon as the thread returns. The handle is dead afterwards. */
bool pal_thread_detach(int64_t thread, PalErrno *err);

/* An identifier for the calling thread that is unique among the threads running
 * right now. It is not stable across a thread's death and another's birth, and
 * it is not the same number the debugger shows. Never fails. */
int64_t pal_thread_self(void);

/* Give up the rest of this thread's time slice. Never fails, and on a machine
 * with one processor it is the only way the thread being waited on gets to
 * run. */
void pal_thread_yield(void);

/* Where the calling thread's own stack begins and ends: lo is the lowest
 * address on it and hi is one past the highest.
 *
 * Only ever ask about the thread you are on. Every system underneath answers
 * for the current thread and several of them answer for no other, and a caller
 * asking about another thread is asking about a stack that is moving.
 *
 * False means the platform has no way to ask, which is the honest answer on a
 * system nobody has written this for rather than a failure. The stack walker is
 * the only caller and it does less rather than guessing. */
bool pal_thread_stack_bounds(void **lo, void **hi);

/* Sleep until the 32 bit word at addr stops being expect, or until somebody
 * wakes it, or until timeout_ns passes. A negative timeout waits forever and a
 * zero one does not wait at all.
 *
 * This is Linux's futex and it is emulated everywhere else, because the three
 * native spellings are not three spellings of the same thing. Linux has the
 * real one. Windows has WaitOnAddress, which lives in synchronization.lib and
 * burrow links nothing, so a note is not worth a link line. macOS has
 * __ulock_wait, which is private, undocumented and has changed shape between
 * releases, and Go does not use it either. So src/pal/futex_posix.c and
 * src/pal/futex_windows.c build the same primitive out of a fixed table of
 * locks and condition variables, keyed by the address. Nothing allocates and
 * the caller's word stays the caller's word, which is the part that matters:
 * an uncontended wait or wake still never enters this layer at all, because
 * the caller checks its own word first.
 *
 * The caller must treat a wake as advice and re-check the word. All three
 * backends may return early and a lock written on the assumption that they do
 * not is a lock that deadlocks once a month.
 *
 * True means the wait ended normally, which covers being woken, being told the
 * word had already changed, and waking for no reason anybody can name. False
 * means it did not: PAL_ETIMEDOUT when the time ran out, PAL_EINTR when a
 * signal arrived, PAL_EINVAL for a null address. A caller loops on the first
 * two. Nothing here reports a word that already differed as a failure, because
 * the caller is going to look at the word again in either case and an error out
 * of a successful early return is a branch nobody wants to write. */
bool pal_futex_wait(uint32_t *addr, uint32_t expect, int64_t timeout_ns, PalErrno *err);

/* Wake at most n waiters on addr, returning how many were woken, or -1. n may
 * be INT64_MAX for all of them. Waking an address nobody waits on is free and
 * is not an error, which is what lets an unlock path skip the bookkeeping that
 * would tell it whether the call was needed.
 *
 * The count is how many waiters this call released, not how many are running:
 * on Linux it is what the kernel reported and on the emulated backends it is
 * how many queued waiters were marked. Either way it is a number for a test or
 * a statistic to read, and a caller that branches on it is a caller that has
 * assumed a waiter cannot have given up a nanosecond earlier.
 *
 * Nothing this call touches belongs to the caller except the address, which it
 * compares and never reads through. A wake that is still in flight when the
 * word it names is freed is therefore looking at this layer's own memory, which
 * is what lets a note live in a stack frame the sleeper has already left. */
int64_t pal_futex_wake(uint32_t *addr, int64_t n, PalErrno *err);

/* ----------------------------------------------------------- stdout capture
 *
 * What testing needs to run an example, and implemented. For a while
 * everything the process writes to its standard output goes into a pipe
 * instead, and a thread the capture starts hands each piece to sink as it
 * arrives, so a writer never blocks on a full pipe.
 *
 * Go swaps os.Stdout for the write end of a pipe, which catches everything Go
 * code prints. C code prints through the stdout stream and straight to
 * descriptor 1, and both have to be caught, so this moves the descriptor under
 * the stream instead of replacing the stream. The stream is flushed on the way
 * in and on the way out, so nothing written before the capture ends up in it
 * and nothing written during it is left behind in a buffer.
 *
 * On Windows the stream belongs to the C runtime, which keeps its own table of
 * descriptors on top of the handles, so that backend moves the runtime's
 * descriptor 1 with its own calls. It is the one place this layer uses them,
 * because it is the one place the thing being moved is the runtime's.
 *
 * sink runs on the capture's thread only, and never after
 * pal_stdout_capture_end has returned. One capture at a time: a second begin
 * before the first has ended captures into the second and loses the first. */
typedef struct PalStdoutCapture {
    int64_t saved;
    int64_t read;
    int64_t thread;
    void (*sink)(void *env, const void *p, int64_t n);
    void *env;
} PalStdoutCapture;

bool pal_stdout_capture_begin(PalStdoutCapture *c,
                              void (*sink)(void *env, const void *p, int64_t n),
                              void *env, PalErrno *err);
bool pal_stdout_capture_end(PalStdoutCapture *c, PalErrno *err);

/* -------------------------------------------------------------------- files
 *
 * The os package's floor. Paths are NUL terminated UTF-8 on every platform,
 * including Windows, where the backend converts to UTF-16 itself: a path that
 * cannot be spelled in the process's ANSI code page still has to work, and the
 * conversion has to happen in exactly one place for that to be true.
 *
 * A path containing a NUL is rejected before it gets here, by the caller that
 * turned a Str into a char *, because it cannot be detected afterwards.
 *
 * Descriptors are close on exec without being asked, because the alternative is
 * a race that cannot be closed in a threaded process and because Go does the
 * same. A caller that wants a descriptor to survive an exec passes it in the
 * PalSpawn fds list, which dups it into place in the child.
 *
 * Implemented. The first caller is the testing package, which reads a fuzz
 * target's seeds out of testdata/fuzz, and the os package is the second.
 *
 * On Windows a path goes to UTF-16 in a buffer on the stack, because nothing
 * here allocates, and that buffer holds PAL_WPATH_MAX units. A longer path is
 * PAL_ENAMETOOLONG. Win32 itself stops at 260 units unless the process has
 * long paths turned on, so this is the larger of the two limits by a distance,
 * and a path that needs more than four thousand characters is one the os
 * package would have to prefix with \\?\ anyway. */

#define PAL_WPATH_MAX 4096

enum {
    PAL_O_RDONLY = 0,
    PAL_O_WRONLY = 1,
    PAL_O_RDWR = 2,
    PAL_O_ACCMODE = 3,

    PAL_O_APPEND = 1u << 2,
    PAL_O_CREATE = 1u << 3,
    PAL_O_EXCL = 1u << 4,
    PAL_O_TRUNC = 1u << 5,
    PAL_O_SYNC = 1u << 6,
    PAL_O_NONBLOCK = 1u << 7,
    PAL_O_DIRECTORY = 1u << 8,
    PAL_O_NOFOLLOW = 1u << 9
};

enum { PAL_SEEK_SET = 0, PAL_SEEK_CUR = 1, PAL_SEEK_END = 2 };

/* File type and permission bits, numbered as POSIX numbers them because that is
 * what the mode argument to open and mkdir means everywhere and because a
 * second numbering would have to be translated on both sides.
 *
 * Windows has none of this and the backend synthesises it: a read only
 * attribute becomes 0444, everything else becomes 0666, a directory gets
 * PAL_S_IFDIR, and a reparse point gets PAL_S_IFLNK. That is what Go does and it
 * is the only mapping that makes os.FileMode mean anything on Windows. */
enum {
    PAL_S_IFMT = 0xf000,
    PAL_S_IFIFO = 0x1000,
    PAL_S_IFCHR = 0x2000,
    PAL_S_IFDIR = 0x4000,
    PAL_S_IFBLK = 0x6000,
    PAL_S_IFREG = 0x8000,
    PAL_S_IFLNK = 0xa000,
    PAL_S_IFSOCK = 0xc000,

    PAL_S_ISUID = 04000,
    PAL_S_ISGID = 02000,
    PAL_S_ISVTX = 01000,
    PAL_S_IRWXU = 0700,
    PAL_S_IRWXG = 0070,
    PAL_S_IRWXO = 0007
};

/* What a stat answers. Fixed layout, no pointers, safe to memcpy and to put on
 * a stack. The times are split into seconds and nanoseconds rather than being
 * one int64_t of nanoseconds, because a file's modification time can be outside
 * the range that holds, and a stat that cannot describe a file from 1901 is a
 * stat that fails on a tarball. */
typedef struct PalStat {
    int64_t size;
    uint32_t mode; /* PAL_S_* */
    uint32_t uid;
    uint32_t gid;
    uint64_t dev;
    uint64_t ino;
    uint64_t rdev;
    uint64_t nlink;
    int64_t blocks;  /* 512 byte blocks actually used, 0 where unknown */
    int64_t blksize; /* preferred IO size, 0 where unknown */
    int64_t atime_sec;
    int64_t mtime_sec;
    int64_t ctime_sec; /* the inode change time, not the creation time */
    int64_t btime_sec; /* creation time, 0 where the platform will not say */
    int32_t atime_nsec;
    int32_t mtime_nsec;
    int32_t ctime_nsec;
    int32_t btime_nsec;
} PalStat;

/* The longest single path component, in bytes of UTF-8.
 *
 * NTFS and every Unix filesystem stop at 255 characters. A character is at most
 * three UTF-8 bytes when it comes from one UTF-16 unit and four when it comes
 * from two, so 255 units can never exceed 765 bytes and this is that with room
 * for the NUL. It is a component and not a path: a full path has no useful
 * limit on any of the three platforms and is never stored in a fixed buffer
 * here. */
#define PAL_NAME_MAX 767

/* One directory entry. type is a PAL_S_IF* value, or 0 when the platform can
 * name the file but not say what it is without a second stat, which is what
 * DT_UNKNOWN means on Linux. */
typedef struct PalDirEntry {
    char name[PAL_NAME_MAX + 1];
    int32_t name_len;
    uint32_t type;
    uint64_t ino;
} PalDirEntry;

int64_t pal_open(const char *path, uint32_t flags, uint32_t mode, PalErrno *err);
bool pal_close(int64_t fd, PalErrno *err);

/* Short reads and writes are normal and are not errors. read returning 0 is end
 * of file. Every one of these returns the count, or -1. */
int64_t pal_read(int64_t fd, void *buf, int64_t n, PalErrno *err);
int64_t pal_pread(int64_t fd, void *buf, int64_t n, int64_t off, PalErrno *err);
int64_t pal_write(int64_t fd, const void *buf, int64_t n, PalErrno *err);
int64_t pal_pwrite(int64_t fd, const void *buf, int64_t n, int64_t off, PalErrno *err);

int64_t pal_seek(int64_t fd, int64_t off, int32_t whence, PalErrno *err);
bool pal_fsync(int64_t fd, PalErrno *err);
bool pal_ftruncate(int64_t fd, int64_t size, PalErrno *err);

bool pal_stat(const char *path, PalStat *out, PalErrno *err);
bool pal_lstat(const char *path, PalStat *out, PalErrno *err);
bool pal_fstat(int64_t fd, PalStat *out, PalErrno *err);

bool pal_unlink(const char *path, PalErrno *err);
bool pal_rename(const char *from, const char *to, PalErrno *err);
bool pal_mkdir(const char *path, uint32_t mode, PalErrno *err);
bool pal_rmdir(const char *path, PalErrno *err);

/* A directory being read, which is a descriptor opened with PAL_O_DIRECTORY
 * and a buffer for what the platform hands back.
 *
 * Every platform returns directory entries in batches, and the batch has to
 * live somewhere between calls. It lives here, in memory the caller owns,
 * because the alternative is a table in the PAL keyed by descriptor, which is
 * both global state and an allocation. Set fd, zero the rest, and hand the
 * same PalDir to every call for that descriptor:
 *
 *     PalDir d = {.fd = fd};
 *     PalDirEntry e;
 *     PalErrno err = PAL_OK;
 *     while (pal_readdir(&d, &e, &err))
 *         use(e.name, e.name_len);
 *     if (err != PAL_OK)
 *         fail(err);
 *
 * It is eight kilobytes, which is fine in a goroutine and is why it is not
 * inside PalDirEntry. Closing the descriptor is the caller's, with pal_close,
 * and nothing else needs undoing. */
typedef struct PalDir {
    int64_t fd;
    int32_t pos;
    int32_t len;
    uint32_t state; /* the backend's own, zero to start */
    uint32_t pad;
    uint64_t buf[1024];
} PalDir;

/* One entry per call. Returns false at the end of the directory with no error
 * set, and false with an error set when something went wrong, so a loop tests
 * the error and not the bool. "." and ".." are filtered out here, because no
 * caller has ever wanted them and every one of them would filter them itself.
 * The order is whatever the filesystem gives. */
bool pal_readdir(PalDir *d, PalDirEntry *out, PalErrno *err);

bool pal_link(const char *from, const char *to, PalErrno *err);
bool pal_symlink(const char *target, const char *path, PalErrno *err);

/* The link's target into buf. Not NUL terminated, because a symlink target is
 * bytes and may legitimately contain anything but a NUL. Returns the length, or
 * -1. A target that does not fit is PAL_ERANGE rather than a truncated path,
 * for the same reason type_qualified_name refuses to truncate a type name. */
int64_t pal_readlink(const char *path, char *buf, int64_t cap, PalErrno *err);

bool pal_chmod(const char *path, uint32_t mode, PalErrno *err);

/* -1 for either id leaves it alone, which is how POSIX spells it. Windows
 * reports PAL_ENOTSUP, which is what Go's os.Chown does there. */
bool pal_chown(const char *path, int64_t uid, int64_t gid, PalErrno *err);

/* Times in nanoseconds since the Unix epoch. */
bool pal_utimes(const char *path, int64_t atime_ns, int64_t mtime_ns, PalErrno *err);

int64_t pal_dup(int64_t fd, PalErrno *err);

/* out[0] is the read end and out[1] is the write end. flags takes
 * PAL_O_NONBLOCK and nothing else. */
bool pal_pipe(int64_t out[2], uint32_t flags, PalErrno *err);

/* ------------------------------------------------------------------ process
 *
 * A process id is the kernel's on POSIX. On Windows it is the process handle,
 * because that is what waiting and killing need and a number the system can
 * give to someone else once the process is gone is not safe to hold. It is
 * closed by the pal_wait that reaps the child. */

enum {
    /* Put the child in its own process group, so that a signal to ours does not
     * reach it and so that it can be killed as a group. */
    PAL_SPAWN_SETPGID = 1u << 0,
    /* Give the child its own session and no controlling terminal. */
    PAL_SPAWN_SETSID = 1u << 1
};

/* Everything a spawn needs, in one struct, because a function with nine
 * parameters is a function whose call sites are wrong.
 *
 * argv and envp are NUL terminated arrays of NUL terminated UTF-8, both ending
 * in a NULL pointer. envp NULL means inherit ours. dir NULL means inherit ours.
 *
 * fds is the child's descriptor table: fds[i] in the parent becomes descriptor
 * i in the child, and PAL_INVALID_HANDLE leaves that slot closed. Descriptors
 * above nfds are closed in the child, which is the only behaviour that is safe
 * in a threaded process where another thread may be opening a file right
 * now.
 *
 * Windows has no descriptor numbers. There the first three slots become the
 * child's standard handles, and handles in any slot after them are inherited
 * under the values they have here, for the child to be told about some other
 * way, which is what Go does too. Nothing else is inherited.
 *
 * A relative path is taken relative to dir when dir is set, as it is in
 * Go. */
typedef struct PalSpawn {
    const char *path;
    const char *const *argv;
    const char *const *envp;
    const char *dir;
    const int64_t *fds;
    int32_t nfds;
    uint32_t flags;
} PalSpawn;

/* Start the process and return its id, or -1.
 *
 * A failure in the child between the fork and the exec is reported here, in the
 * parent, as though the spawn itself had failed, because a caller cannot be
 * asked to wait on a process that never existed. That is what the pipe in the
 * POSIX backend is for. */
int64_t pal_spawn(const PalSpawn *req, PalErrno *err);

enum {
    PAL_WAIT_NOHANG = 1u << 0,
    /* Report a process a signal ended as minus the signal's number, ours where
     * there is one of ours, and 256 further below nought when it left a core
     * behind. That lets a caller tell "exit status 139" from "signal:
     * segmentation fault" the way Go's ProcessState does. Windows has no
     * signals and ignores it. */
    PAL_WAIT_SIGNAL = 1u << 1
};

/* Wait for a child. Returns its id, 0 when PAL_WAIT_NOHANG and it is still
 * running, or -1. status gets the exit code, or 128 plus the signal number for
 * a process a signal ended, which is the shell's convention and Go's. */
int64_t pal_wait(int64_t pid, int32_t *status, uint32_t flags, PalErrno *err);

/* Signal numbers, ours, because the numbers differ between platforms and
 * Windows has none at all. The Windows backend maps what it can onto
 * TerminateProcess and GenerateConsoleCtrlEvent and reports PAL_ENOTSUP for the
 * rest rather than pretending. */
enum {
    PAL_SIGHUP = 1,
    PAL_SIGINT = 2,
    PAL_SIGQUIT = 3,
    /* The ones a crash raises. They are here so that pal_kill can send them and
     * pal_wait can name them. PAL_SIGSEGV and PAL_SIGBUS cannot be installed,
     * because a handler for a bad memory access is PAL_SIGFAULT's. */
    PAL_SIGILL = 4,
    PAL_SIGTRAP = 5,
    PAL_SIGABRT = 6,
    PAL_SIGBUS = 7,
    PAL_SIGFPE = 8,
    PAL_SIGKILL = 9,
    PAL_SIGUSR1 = 10,
    PAL_SIGSEGV = 11,
    PAL_SIGUSR2 = 12,
    PAL_SIGPIPE = 13,
    PAL_SIGALRM = 14,
    PAL_SIGTERM = 15,
    PAL_SIGCHLD = 17,
    PAL_SIGCONT = 18,
    PAL_SIGSTOP = 19,
    /* The one the runtime sends itself, to take a goroutine off a core it has
     * been on too long. Go uses SIGURG for this. */
    PAL_SIGPREEMPT = 23,
    /* Not a signal, and the number is outside the range the others come from so
     * that nobody reads it as one. It means a bad memory access, which is two
     * signals on POSIX and none at all on Windows.
     *
     * POSIX raises SIGSEGV for some of those and SIGBUS for others, and which
     * one a guard page gives you is not the same on Linux as it is on macOS.
     * Windows raises an access violation, which reaches a vectored exception
     * handler and is not a signal in any sense. A caller that had to know which
     * of those three it was on would be a caller this layer had failed, so
     * installing for this one installs for whatever the platform actually
     * raises. Only pal_signal_install takes it. pal_kill does not. */
    PAL_SIGFAULT = 64
};

bool pal_kill(int64_t pid, int32_t sig, PalErrno *err);
int64_t pal_getpid(void);

/* The handle behind standard input, output or error, for i 0, 1 or 2, which
 * is what a PalSpawn fds list wants when a child is to share ours. It is i
 * itself on POSIX and GetStdHandle on Windows. PAL_INVALID_HANDLE for any
 * other i, or when the process has no such handle. */
int64_t pal_std_handle(int i);

/* Ends the process now. Does not return and does not run anything on the way
 * out: an exit handler that runs while another thread holds a lock is how a
 * program hangs on the way to dying. */
BURROW_NORETURN void pal_exit(int32_t code);

/* The environment as a NULL terminated array of "KEY=VALUE". It borrows from
 * the process and is only valid until something changes the environment, which
 * is why the os package copies it once at startup and never reads this
 * again. */
BURROW_BORROWS(ret) const char *const *pal_environ(void);

bool pal_chdir(const char *path, PalErrno *err);

/* The working directory into buf, NUL terminated. Returns its length, or -1,
 * with PAL_ERANGE when it does not fit. */
int64_t pal_getcwd(char *buf, int64_t cap, PalErrno *err);

/* Resolve a program name against the search path, writing the full path to buf.
 * Returns its length, or -1. This is a PAL call rather than portable code over
 * PATH because Windows searches differently, applies PATHEXT, and looks in the
 * application directory first. */
int64_t pal_exec_lookup(const char *name, char *buf, int64_t cap, PalErrno *err);

/* ------------------------------------------------------------------ signals
 *
 * Running something of ours when the operating system interrupts a thread.
 *
 * Implemented, and PAL_SIGFAULT is in use: it is what turns a goroutine running
 * off the bottom of its stack into "fatal error: stack overflow" rather than
 * into a segmentation fault with nothing said. PAL_SIGPREEMPT is what the
 * scheduler will send itself to take a goroutine off a core it has held too
 * long, and that has no caller yet.
 *
 * This is the group with the least room to be clever in. A handler runs with
 * the thread stopped wherever it was, which may be in the middle of malloc, so
 * everything below is written to take a lock the handler could already be
 * holding exactly never. */

/* What a handler is called with and what it says back.
 *
 * sig is the PAL number it was installed for. info and ctx are the platform's
 * own two structures, opaque here, because the callers that want anything out
 * of them want one field each and there is a call below for the field the fault
 * handler wants. The preemption handler will want the interrupted program
 * counter out of ctx and will get a call of its own when it is written.
 *
 * True means the handler dealt with it and the interrupted instruction should
 * be resumed. False means this one was not ours, and the backend puts back
 * whatever disposition was there before burrow arrived and lets it happen
 * again, so a program with its own handler still gets it and a program with
 * none still dies the way it would have, core file and all.
 *
 * That return value is the whole reason this is not a void function. A POSIX
 * handler returns nothing and a Windows vectored handler returns one of three
 * codes, and a caller that had to know which it was writing is a caller this
 * layer had failed. */
typedef bool (*PalSignalHandler)(int32_t sig, void *info, void *ctx);

/* Install a handler. The handler runs on the signal stack pal_signal_stack_
 * install gave the thread, where the platform has such a thing, which is what
 * lets it run at all when the fault it is handling is a stack that ran out.
 *
 * Installing twice for the same signal replaces the handler. The disposition
 * kept for the forwarding above is the one that was there before the first
 * install, so a caller cannot lose the program's own handler by arming twice.
 *
 * Windows has no signals. PAL_SIGFAULT is a vectored exception handler there.
 * PAL_SIGINT is Ctrl-C and Ctrl-Break, and PAL_SIGTERM is closing the console,
 * logging off and shutting down, both through a console control handler, as
 * in Go's os/signal. That handler runs on a thread the system starts for it,
 * so info and ctx are NULL, and it has to be safe to run beside every other
 * thread. The rest report PAL_ENOTSUP. */
bool pal_signal_install(int32_t sig, PalSignalHandler handler, PalErrno *err);

/* Block or unblock a signal for the calling thread only. PAL_ENOTSUP on
 * Windows. */
bool pal_signal_mask(int32_t sig, bool block, PalErrno *err);

/* Gives the calling thread somewhere for a handler to run that is not the stack
 * it is already on.
 *
 * Once per thread, for every thread that wants a handler to be able to report a
 * stack overflow. The stack that overflowed is the one with no room left on it,
 * so a handler that has nowhere else to go is a handler that faults again and
 * takes the message with it.
 *
 * Calling it twice on one thread does nothing the second time and answers true.
 * It answers true on Windows too, where there is nothing to do: a vectored
 * handler runs on whatever stack faulted and the platform offers no way to say
 * otherwise. */
bool pal_signal_stack_install(PalErrno *err);

/* Gives back what the call above took. Call it before the thread ends or the
 * mapping lives as long as the process does. Safe on a thread that never
 * installed one. */
void pal_signal_stack_remove(void);

/* The address a bad memory access was trying to reach, out of the info pointer
 * a PAL_SIGFAULT handler was called with.
 *
 * It is an address and not a pointer: reading it is what faulted, so reading it
 * again would fault again. Compare it against something and nothing else.
 *
 * NULL means either the platform did not say or the access really was at
 * address zero, and this does not distinguish them, because the one caller
 * there is compares the answer against a range and zero is never in one. A
 * caller that needs to tell a null dereference from silence needs a different
 * call and can have one when it turns up.
 *
 * Only meaningful inside a PAL_SIGFAULT handler and only for the info pointer
 * that handler was handed. */
BURROW_BORROWS(ret) const void *pal_signal_fault_addr(const void *info);

/* ------------------------------------------------------------ dynamic loading
 *
 * For plugin and for the cgo-free paths that have to reach a system library at
 * runtime, which on macOS is how anything outside libSystem is reached at all.
 * Not implemented yet. */
int64_t pal_dl_open(const char *path, PalErrno *err);
BURROW_BORROWS(ret, handle) void *pal_dl_sym(int64_t handle, const char *name,
                                             PalErrno *err);
bool pal_dl_close(int64_t handle, PalErrno *err);

/* ---------------------------------------------------------------------- user
 *
 * What os/user needs, without the passwd parsing, which is portable and
 * belongs above this line. Not implemented yet. */
typedef struct PalUser {
    int64_t uid;
    int64_t gid;
    char name[PAL_NAME_MAX + 1];
    char home[1024];
} PalUser;

/* uid below zero means the user this process is running as. */
bool pal_user_lookup(int64_t uid, PalUser *out, PalErrno *err);

/* --------------------------------------------------------------------- net
 *
 * Sockets, with the addresses in a shape of ours rather than the platform's.
 *
 * A sockaddr is the part of the socket API that differs most between the three
 * platforms and the part that is most often got wrong, because it is a tagged
 * union with platform specific padding, a length field that exists on some
 * systems and not others, and a port in network byte order in the middle of it.
 * Converting once, in the backend, is the whole reason this layer exists.
 *
 * Not implemented yet. These arrive with the net package. */

enum { PAL_AF_INET = 1, PAL_AF_INET6 = 2, PAL_AF_UNIX = 3 };
enum { PAL_SOCK_STREAM = 1, PAL_SOCK_DGRAM = 2, PAL_SOCK_RAW = 3 };
enum { PAL_IPPROTO_TCP = 6, PAL_IPPROTO_UDP = 17 };

/* An address, in host byte order everywhere a number appears. addr holds four
 * bytes for IPv4 and sixteen for IPv6, most significant first, which is how an
 * address is written down and how net.IP stores it. */
typedef struct PalSockAddr {
    uint16_t family;
    uint16_t port;
    uint32_t scope_id; /* the IPv6 zone, 0 for none */
    uint8_t addr[16];
    char path[108]; /* AF_UNIX, NUL terminated, or a leading NUL for abstract */
} PalSockAddr;

/* A socket, always non blocking and always close on exec, because every
 * descriptor in burrow goes to the poller and a blocking one would park an OS
 * thread instead of a goroutine. */
int64_t pal_socket(int32_t family, int32_t type, int32_t protocol, PalErrno *err);

bool pal_bind(int64_t fd, const PalSockAddr *addr, PalErrno *err);
bool pal_listen(int64_t fd, int32_t backlog, PalErrno *err);

/* Returns the new descriptor, or -1 with PAL_EAGAIN when there is nothing
 * waiting, which is the normal answer on a non blocking listener and is what
 * sends the goroutine to the poller. */
int64_t pal_accept(int64_t fd, PalSockAddr *peer, PalErrno *err);

/* PAL_EINPROGRESS is the normal answer, not a failure: the caller waits for
 * writability and then reads PAL_SO_ERROR to find out how it went. */
bool pal_connect(int64_t fd, const PalSockAddr *addr, PalErrno *err);

/* addr NULL means a connected socket, which makes these send and recv. */
int64_t pal_sendto(int64_t fd, const void *buf, int64_t n, const PalSockAddr *addr,
                   PalErrno *err);
int64_t pal_recvfrom(int64_t fd, void *buf, int64_t n, PalSockAddr *from,
                     PalErrno *err);

/* Socket options, ours, for the same reason the signal numbers are: the level
 * and name pairs differ between platforms and a caller should not have to know
 * which. The set is what Go's net package actually sets, and it grows from
 * there rather than trying to cover everything now. */
enum {
    PAL_SO_REUSEADDR = 1,
    PAL_SO_REUSEPORT,
    PAL_SO_KEEPALIVE,
    PAL_SO_BROADCAST,
    PAL_SO_LINGER,
    PAL_SO_RCVBUF,
    PAL_SO_SNDBUF,
    PAL_SO_ERROR,
    PAL_TCP_NODELAY,
    PAL_TCP_KEEPIDLE,
    PAL_TCP_KEEPINTVL,
    PAL_TCP_KEEPCNT,
    PAL_IP_TTL,
    PAL_IPV6_V6ONLY,
    PAL_IPV6_HOPLIMIT
};

bool pal_getsockopt(int64_t fd, int32_t opt, int64_t *value, PalErrno *err);
bool pal_setsockopt(int64_t fd, int32_t opt, int64_t value, PalErrno *err);

enum { PAL_SHUT_RD = 0, PAL_SHUT_WR = 1, PAL_SHUT_RDWR = 2 };
bool pal_shutdown(int64_t fd, int32_t how, PalErrno *err);

/* One result from a name lookup. */
typedef struct PalAddrInfo {
    PalSockAddr addr;
    int32_t socktype;
    int32_t protocol;
} PalAddrInfo;

/* Resolve host and service into at most cap results, returning how many were
 * written, or -1. Nothing is allocated, which is the difference from the
 * platform call this sits on and the reason there is no free to pair with it.
 *
 * This is the blocking resolver, which runs on a thread of its own the way Go's
 * cgo resolver does. The pure Go resolver, which reads resolv.conf and speaks
 * DNS itself, is portable code above this line and does not come through
 * here. */
int64_t pal_getaddrinfo(const char *host, const char *service, int32_t family,
                        int32_t socktype, PalAddrInfo *out, int64_t cap, PalErrno *err);

enum {
    PAL_IFF_UP = 1u << 0,
    PAL_IFF_BROADCAST = 1u << 1,
    PAL_IFF_LOOPBACK = 1u << 2,
    PAL_IFF_POINTTOPOINT = 1u << 3,
    PAL_IFF_MULTICAST = 1u << 4,
    PAL_IFF_RUNNING = 1u << 5
};

typedef struct PalInterface {
    char name[PAL_NAME_MAX + 1];
    int32_t index;
    int32_t mtu;
    uint32_t flags;
    uint8_t hwaddr[32];
    int32_t hwaddr_len;
} PalInterface;

/* The machine's network interfaces into out, returning how many were written,
 * or -1. Addresses are not here: they are a second call with a different shape
 * and they arrive with net. */
int64_t pal_if_enumerate(PalInterface *out, int64_t cap, PalErrno *err);

/* --------------------------------------------------------------------- poll
 *
 * Readiness notification: epoll on Linux, kqueue on the BSDs and macOS, an IO
 * completion port on Windows. The first two answer when a descriptor is ready
 * and the third answers when an operation has finished, which is a real
 * difference and not one this layer hides. What it does hide is the shape of
 * the structs, the spelling of the calls, and the wakeup, which is a pipe on
 * one platform, an eventfd on another and a posted completion on the third and
 * is not something a caller should have to know about.
 *
 * Implemented, in src/pal/poll_linux.c, poll_bsd.c and poll_windows.c. */

enum { PAL_POLL_READ = 1u << 0, PAL_POLL_WRITE = 1u << 1 };

enum {
    PAL_POLL_READY_READ = 1u << 0,
    PAL_POLL_READY_WRITE = 1u << 1,
    /* The only thing reported was a failure, with no readiness in either
     * direction behind it. The read and write bits are set as well, because a
     * read that will now fail and a write that will now fail are both better
     * than a goroutine that waits forever, and this bit is how a caller tells
     * that apart from a descriptor that genuinely has something on it. */
    PAL_POLL_READY_ERROR = 1u << 2
};

/* An OVERLAPPED, laid out by hand so that this header does not have to drag
 * windows.h in behind it, and so that nothing above the PAL has to either.
 *
 * A completion backend needs the caller to submit its operations with one of
 * these attached, because that is how the kernel knows where to put the answer
 * and it is the only thing that comes back. So the shape of it is part of this
 * boundary whether or not the other two platforms have anything like it, and
 * pretending otherwise would only mean the caller declaring it instead and
 * nobody checking.
 *
 * The field names are lower case because these are burrow's names for somebody
 * else's structure, and nothing outside the Windows backend reads or writes
 * them. src/pal/poll_windows.c checks at compile time that this is the same
 * size, alignment and field order as the real one. */
typedef struct PalOverlapped {
    uintptr_t internal;
    uintptr_t internal_high;
    uint32_t offset;
    uint32_t offset_high;
    void *event;
} PalOverlapped;

/* What came back.
 *
 * user is whatever was handed to pal_poll_add on a readiness backend, which is
 * how the caller finds its own state without a table lookup. On a completion
 * backend it is the OVERLAPPED the caller submitted the operation with, because
 * that is what the kernel hands back and nothing else about the operation
 * reaches the port.
 *
 * bytes and status are the completion's own and are 0 on a readiness backend,
 * where there is no operation to have produced either. status is the kernel's
 * status word untranslated, for the same reason burrow__PollOp keeps it
 * untranslated: the caller turns it into an error with the operation in hand
 * and this layer does not have the operation. */
typedef struct PalPollEvent {
    void *user;
    uint32_t ready;
    uint32_t bytes;
    int32_t status;
} PalPollEvent;

/* Makes the poller and whatever it wakes up with, which is one object as far as
 * anything above here is concerned. The wakeup has to be registered in the set
 * on two of the three platforms, so there is no point in it being a second
 * call that a caller could forget.
 *
 * There is one poller per process and a second call gives PAL_EBUSY. That is
 * not a simplification, it is the rule: nothing in the PAL allocates, so a
 * poller has to be static storage, and a runtime with two netpollers would have
 * two answers to the question of which thread is asleep in the kernel. Go has
 * one and so does this. The handle that comes back is the poller's own
 * descriptor where a platform has one, so that a debugger and lsof agree with
 * each other, and is only ever meant to be passed back in here. */
int64_t pal_poll_create(PalErrno *err);

/* Register for reading and writing together, edge triggered, once for the life
 * of the descriptor. Both halves at once because that is one system call
 * instead of two and because a descriptor in burrow is always both.
 *
 * user is handed back in every event for this descriptor, which is how the
 * caller finds its own state without a table lookup. It may be NULL and it may
 * be anything else except the all ones pointer, which a readiness backend keeps
 * for its own wakeup. That reservation is the price of there being no table: a
 * value that means the wakeup has to come from the same space the caller's
 * values come from. A caller that is passing an index rather than a pointer, as
 * the runtime's netpoller is, gets that for free by never filling every bit.
 *
 * On a completion backend there is nothing to arm and this attaches the handle
 * to the port so that its completions have somewhere to arrive. user is not
 * used there, since the operation carries its own. */
bool pal_poll_add(int64_t poll, int64_t fd, void *user, PalErrno *err);

/* Undo it. On kqueue and on a completion port this does nothing, because
 * closing the descriptor is what takes it off and the caller is about to. */
bool pal_poll_del(int64_t poll, int64_t fd, PalErrno *err);

/* Ends a blocking pal_poll_wait early, from any thread, including one that is
 * not in the poller. Collapses: a thousand of these while one wait is asleep
 * cost one wakeup, because a wait that is about to return has already answered
 * all of them.
 *
 * The wakeup never comes back as an event. A wait that was going to block
 * consumes it and a wait that was only looking leaves it for the thread it was
 * meant for, and neither is something a caller has to arrange. */
bool pal_poll_break(int64_t poll, PalErrno *err);

/* Wait for up to cap events. timeout_ns below zero waits forever and 0 returns
 * at once, which is the non blocking poll the scheduler does when it is looking
 * for work. Returns how many events were written, which may be 0 on a timeout,
 * or -1. PAL_EINTR is not an error worth reporting here and comes back as 0
 * events, because every caller would loop on it. */
int64_t pal_poll_wait(int64_t poll, PalPollEvent *out, int64_t cap, int64_t timeout_ns,
                      PalErrno *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_PAL_H */
