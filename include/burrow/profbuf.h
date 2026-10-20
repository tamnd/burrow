/* The log a CPU profile is written into, from a signal handler, and read out of
 * by the goroutine that turns it into a profile.
 *
 * One writer and one reader, and no lock between them. The writer is whatever
 * thread the profiling signal landed on, which may have been stopped anywhere,
 * so a write never blocks, never allocates and never takes a lock the stopped
 * thread could be holding. When the log is full the write is counted instead
 * of kept, and the count comes out of the reader as a record of its own. The
 * caller serialises its writers, the way Go's cpuProfile.add does with
 * prof.signalLock.
 *
 * Each record is a length word, a time stamp, hdrsize header words and a stack
 * of pcs, plus one tag pointer kept beside it. A record of overflow has a zero
 * header, the time of the first write lost and a stack of one word, the count.
 *
 * Internal, for runtime_set_cpu_profile_rate, runtime/pprof and the tests.
 *
 * Derived from Go's src/runtime/profbuf.go.
 * Go source: go1.27.1.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_PROFBUF_H
#define BURROW_PROFBUF_H

#include "burrow/core.h"
#include "burrow/own.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct burrow__ProfBuf burrow__ProfBuf;

/* Go's profBufReadMode. */
typedef enum burrow__ProfBufReadMode {
    BURROW_PROF_BUF_BLOCKING,
    BURROW_PROF_BUF_NON_BLOCKING
} burrow__ProfBufReadMode;

/* What a read hands back. data and tags point into the buffer and stay valid
 * until the next read, which is what tells the writer it may reuse them. Both
 * are NULL when there is nothing, and eof is true once the buffer is closed and
 * everything in it has been read. */
typedef struct burrow__ProfBufRead {
    BURROW_BORROWS(1) const uint64_t *data;
    Int ndata;
    BURROW_BORROWS(1) void *const *tags;
    Int ntags;
    bool eof;
} burrow__ProfBufRead;

/* A buffer with room for bufwords words of records and tags tag pointers, each
 * rounded up to a power of two, and records with hdrsize header words. Throws
 * for sizes of 1<<28 and over, as Go's newProfBuf does. NULL when out of
 * memory. */
BURROW_OWNS(ret) burrow__ProfBuf *burrow__prof_buf_new(Int hdrsize, Int bufwords,
                                                       Int tags);

/* Frees a buffer nobody is writing to or reading from any more. NULL is
 * fine. */
void burrow__prof_buf_free(BURROW_OWNS(1) burrow__ProfBuf *b);

/* Adds a record. tag_ptr is where to read the tag from, or NULL for none, hdr
 * has at most hdrsize words and any it leaves out are zero. Safe in a signal
 * handler. Throws if hdr is longer than hdrsize. */
void burrow__prof_buf_write(burrow__ProfBuf *b, void *const *tag_ptr, int64_t now,
                            const uint64_t *hdr, Int nhdr, const uintptr_t *stk,
                            Int nstk);

/* Marks the end of the log. A reader gets what is left and then eof. Throws if
 * the buffer is already closed. */
void burrow__prof_buf_close(burrow__ProfBuf *b);

/* The next run of whole records. A blocking read waits until the buffer is half
 * full, closed, or has overflowed. A non-blocking one returns at once, with
 * nothing if there is nothing. NULL reads as closed and empty. */
burrow__ProfBufRead burrow__prof_buf_read(burrow__ProfBuf *b,
                                          burrow__ProfBufReadMode mode);

/* Whether a blocking reader is asleep in the buffer right now. Not in Go: Go's
 * tests find that out from the reader's goroutine status, which in Go is
 * syscall while it sleeps, and in burrow is running, because a note is slept on
 * without letting go of the P. */
bool burrow__prof_buf_reader_sleeping(burrow__ProfBuf *b);

/* Go's runtime_pprof_readProfile: the next part of the CPU profile's log,
 * without waiting. eof once the profile has stopped and all of it is read, at
 * which point the log is freed. Called by one reader at a time. */
burrow__ProfBufRead burrow__read_cpu_profile(void);

/* The functions a CPU profile sample is put under when its stack could not be
 * walked, Go's _System, _ExternalCode, _LostExternalCode and
 * _LostSIGPROFDuringAtomic64. Only their addresses mean anything, and calling
 * one throws. */
void burrow__prof_system(void);
void burrow__prof_external_code(void);
void burrow__prof_lost_external_code(void);
void burrow__prof_lost_sigprof_during_atomic64(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_PROFBUF_H */
