/* Why a goroutine is waiting, which is what a traceback prints in the brackets
 * after its number in place of the bare "waiting".
 *
 * Go's waitReason has a value for every place its runtime parks, most of them
 * in the garbage collector. This has the ones burrow parks for, with Go's
 * strings, so that a dump of a burrow program reads like a dump of a Go one.
 * The order is burrow's own. Nothing stores these anywhere but in a G, so the
 * numbers never have to match Go's.
 *
 * Internal, for the scheduler and the packages built on it.
 *
 * Derived from Go's src/runtime/runtime2.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_WAITREASON_H
#define BURROW_WAITREASON_H

#include "burrow/own.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A waitReason explains why a goroutine has been stopped. See gopark. */
typedef enum burrow__WaitReason {
    BURROW_WAIT_ZERO = 0,                /* "" */
    BURROW_WAIT_IO,                      /* "IO wait" */
    BURROW_WAIT_CHAN_RECEIVE_NIL_CHAN,   /* "chan receive (nil chan)" */
    BURROW_WAIT_CHAN_SEND_NIL_CHAN,      /* "chan send (nil chan)" */
    BURROW_WAIT_SELECT,                  /* "select" */
    BURROW_WAIT_SELECT_NO_CASES,         /* "select (no cases)" */
    BURROW_WAIT_CHAN_RECEIVE,            /* "chan receive" */
    BURROW_WAIT_CHAN_SEND,               /* "chan send" */
    BURROW_WAIT_SEMACQUIRE,              /* "semacquire" */
    BURROW_WAIT_SLEEP,                   /* "sleep" */
    BURROW_WAIT_SYNC_COND_WAIT,          /* "sync.Cond.Wait" */
    BURROW_WAIT_SYNC_MUTEX_LOCK,         /* "sync.Mutex.Lock" */
    BURROW_WAIT_SYNC_RW_MUTEX_R_LOCK,    /* "sync.RWMutex.RLock" */
    BURROW_WAIT_SYNC_RW_MUTEX_LOCK,      /* "sync.RWMutex.Lock" */
    BURROW_WAIT_SYNC_WAIT_GROUP_WAIT,    /* "sync.WaitGroup.Wait" */
    BURROW_WAIT_COROUTINE,               /* "coroutine" */
    BURROW_WAIT_SYNCTEST_RUN,            /* "synctest.Run" */
    BURROW_WAIT_SYNCTEST_WAIT,           /* "synctest.Wait" */
    BURROW_WAIT_SYNCTEST_CHAN_RECEIVE,   /* "chan receive (durable)" */
    BURROW_WAIT_SYNCTEST_CHAN_SEND,      /* "chan send (durable)" */
    BURROW_WAIT_SYNCTEST_SELECT,         /* "select (durable)" */
    BURROW_WAIT_SYNCTEST_WAIT_GROUP_WAIT /* "sync.WaitGroup.Wait (durable)" */
} burrow__WaitReason;

/* The string in the comment beside r, or "unknown wait reason" for a value
 * that is not in the list. Go's waitReason.String. */
BURROW_STATIC(ret) const char *burrow__wait_reason_string(burrow__WaitReason r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_WAITREASON_H */
