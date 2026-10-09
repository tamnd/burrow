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

void burrow__http2_outflow_set_conn_flow(Http2Outflow *f, Http2Outflow *cf) {
    f->conn = cf;
}

int32_t burrow__http2_outflow_available(const Http2Outflow *f) {
    int32_t n = f->n;
    if (f->conn != NULL && f->conn->n < n)
        n = f->conn->n;
    return n;
}

void burrow__http2_outflow_take(Http2Outflow *f, int32_t n) {
    if (n > burrow__http2_outflow_available(f))
        panic_str(BURROW_S("internal error: took too much"));
    f->n -= n;
    if (f->conn != NULL)
        f->conn->n -= n;
}

bool burrow__http2_outflow_add(Http2Outflow *f, int32_t n) {
    /* Go's int32 addition wraps, and the test below is how it sees that it
     * did. C's would be undefined, so the sum is made in uint32_t. */
    int32_t sum = (int32_t)((uint32_t)f->n + (uint32_t)n);
    if ((sum > n) == (f->n > 0)) {
        f->n = sum;
        return true;
    }
    return false;
}
