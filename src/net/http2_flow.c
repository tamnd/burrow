/* net/http/internal/http2's flow.go: the flow control windows.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http2.h"

#include "burrow/panic.h"

#include <stdint.h>

void burrow__http2_inflow_init(Http2Inflow *f, int32_t n) {
    f->avail = n;
}

int32_t burrow__http2_inflow_add(Http2Inflow *f, Int n) {
    if (n < 0)
        panic_str(BURROW_S("negative update"));
    int64_t unsent = (int64_t)f->unsent + (int64_t)n;
    /* "A sender MUST NOT allow a flow-control window to exceed 2^31-1
     * octets." RFC 7540 section 6.9.1. */
    if (unsent + (int64_t)f->avail > INT32_MAX)
        panic_str(BURROW_S("flow control update exceeds maximum window size"));
    f->unsent = (int32_t)unsent;
    /* Fewer than inflowMinRefresh bytes that would not at least double the
     * window wait for a later update. */
    if (f->unsent < HTTP2_INFLOW_MIN_REFRESH && f->unsent < f->avail)
        return 0;
    f->avail += f->unsent;
    f->unsent = 0;
    return (int32_t)unsent;
}

bool burrow__http2_inflow_take(Http2Inflow *f, uint32_t n) {
    if (n > (uint32_t)f->avail)
        return false;
    f->avail -= (int32_t)n;
    return true;
}

bool burrow__http2_take_inflows(Http2Inflow *f1, Http2Inflow *f2, uint32_t n) {
    if (n > (uint32_t)f1->avail || n > (uint32_t)f2->avail)
        return false;
    f1->avail -= (int32_t)n;
    f2->avail -= (int32_t)n;
    return true;
}

void burrow__http2_conn_outflow_init(Http2ConnOutflow *f) {
    f->initial = HTTP2_INITIAL_WINDOW_SIZE; /* initial stream window size */
    f->n = HTTP2_INITIAL_WINDOW_SIZE;       /* current connection window size */
}

bool burrow__http2_conn_outflow_change_initial_window_size(Http2ConnOutflow *f,
                                                           int64_t size) {
    if (size > HTTP2_MAX_FLOW_WINDOW) {
        f->flow_err = true;
        return false;
    }
    f->initial = (int32_t)size;
    return true;
}

bool burrow__http2_conn_outflow_add(Http2ConnOutflow *f, int32_t n) {
    int64_t sum = (int64_t)f->n + (int64_t)n;
    if (sum > HTTP2_MAX_FLOW_WINDOW) {
        f->flow_err = true;
        return false;
    }
    f->n = (int32_t)sum;
    return true;
}

int32_t burrow__http2_outflow_available(Http2Outflow *f, bool *ok) {
    *ok = true;
    if (f->conn == NULL)
        return HTTP2_MAX_FLOW_WINDOW; /* only happens in tests */
    if (f->conn->flow_err) {
        /* Block all sending once any stream observes a flow control error. */
        *ok = false;
        return 0;
    }
    int64_t n = (int64_t)f->conn->initial + (int64_t)f->delta;
    if (n > HTTP2_MAX_FLOW_WINDOW) {
        f->conn->flow_err = true;
        *ok = false;
        return 0;
    }
    return (int32_t)n < f->conn->n ? (int32_t)n : f->conn->n;
}

void burrow__http2_outflow_take(Http2Outflow *f, int32_t n) {
    if (f->conn == NULL)
        return; /* only happens in tests */
    bool ok = false;
    if (n > burrow__http2_outflow_available(f, &ok))
        panic_str(BURROW_S("internal error: took too much"));
    f->delta -= n;
    f->conn->n -= n;
}

bool burrow__http2_outflow_add(Http2Outflow *f, int32_t n) {
    if (f->conn == NULL)
        return true; /* only happens in tests */
    int64_t avail = (int64_t)f->conn->initial + (int64_t)f->delta;
    if (avail > HTTP2_MAX_FLOW_WINDOW) {
        /* An earlier change to the initial window pushed this stream over the
         * limit. This is a connection-level flow control error. */
        f->conn->flow_err = true;
        return false;
    }
    if (avail + (int64_t)n > HTTP2_MAX_FLOW_WINDOW) {
        /* This update would push the stream over the limit. This is a
         * stream-level flow control error. */
        return false;
    }
    /* Go's int32 addition wraps where C's would be undefined, so the sum is
     * made in uint32_t. */
    f->delta = (int32_t)((uint32_t)f->delta + (uint32_t)n);
    return true;
}
