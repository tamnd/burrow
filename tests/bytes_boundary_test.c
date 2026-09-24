/* Derived from Go's src/bytes/boundary_test.go.
 * Go source: go1.27.1.
 *
 * Go builds these on Linux only, with mmap and mprotect. The pal's reserve and
 * commit do the same everywhere: three pages are reserved and only the middle
 * one committed, so reading a byte either side of it faults.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bytes.h"

#include "burrow/burrow.h"
#include "burrow/pal.h"

#include "check.h"

#include <string.h>

/* This file tests the situation where byte operations are checking data very
 * near to a page boundary. We want to make sure those operations do not read
 * across the boundary and cause a page fault where they shouldn't. */

static Byte *dangerous_page(TestingT *t, Int *n) {
    int64_t pagesize = pal_page_size();
    PalErrno err = 0;
    Byte *b = (Byte *)pal_vm_reserve(3 * pagesize, &err);
    if (b == NULL)
        testing_t_fatalf_v(t, "reserve failed %s", pal_errno_string(err));
    if (!pal_vm_commit(b + pagesize, pagesize, &err))
        testing_t_fatalf_v(t, "commit failed %s", pal_errno_string(err));
    *n = (Int)pagesize;
    return b + pagesize;
}

static void release(Byte *page, Int n) {
    pal_vm_release(page - n, 3 * (int64_t)n, NULL);
}

static Slice sl(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

static void TestEqualNearPageBoundary(TestingT *t) {
    Int n;
    Byte *b = dangerous_page(t, &n);
    memset(b, 'A', (size_t)n);
    for (Int i = 0; i <= n; i++) {
        bytes_equal(sl(b, i), sl(b + n - i, i));
        bytes_equal(sl(b + n - i, i), sl(b, i));
    }
    release(b, n);
}

static void TestIndexByteNearPageBoundary(TestingT *t) {
    Int n;
    Byte *b = dangerous_page(t, &n);
    memset(b, 0, (size_t)n);
    for (Int i = 0; i < n; i++) {
        Int idx = bytes_index_byte(sl(b + i, n - i), 1);
        if (idx != -1)
            testing_t_fatalf_v(t, "IndexByte(b[%d:])=%d, want -1\n", i, idx);
    }
    release(b, n);
}

static void TestIndexNearPageBoundary(TestingT *t) {
    Int qn, bn;
    Byte *qpage = dangerous_page(t, &qn);
    Byte *bpage = dangerous_page(t, &bn);
    memset(qpage, 0, (size_t)qn);
    memset(bpage, 0, (size_t)bn);
    /* Only worry about when we're near the end of a page. */
    Byte *q = qpage;
    if (qn > 64) {
        q = qpage + qn - 64;
        qn = 64;
    }
    Byte *b = bpage;
    Int n = bn;
    if (n > 256) {
        b = bpage + n - 256;
        n = 256;
    }
    for (Int j = 1; j < qn; j++) {
        q[j - 1] = 1; /* difference is only found on the last byte */
        for (Int i = 0; i < n; i++) {
            Int idx = bytes_index(sl(b + i, n - i), sl(q, j));
            if (idx != -1)
                testing_t_fatalf_v(t, "Index(b[%d:], q[:%d])=%d, want -1\n", i, j, idx);
        }
        q[j - 1] = 0;
    }

    /* Test differing alignments and sizes of q which always end on a page
     * boundary. */
    q[qn - 1] = 1; /* difference is only found on the last byte */
    for (Int j = 0; j < qn; j++) {
        for (Int i = 0; i < n; i++) {
            Int idx = bytes_index(sl(b + i, n - i), sl(q + j, qn - j));
            if (idx != -1)
                testing_t_fatalf_v(t, "Index(b[%d:], q[%d:])=%d, want -1\n", i, j, idx);
        }
    }
    q[qn - 1] = 0;
    /* Both are one page, and qn was cut down to 64. */
    release(qpage, bn);
    release(bpage, bn);
}

static void TestCountNearPageBoundary(TestingT *t) {
    Int n;
    Byte *b = dangerous_page(t, &n);
    memset(b, 0, (size_t)n);
    static const Byte one = 1, zero = 0;
    Slice sep1 = slice_from((void *)(uintptr_t)&one, 1, 1, TYPE_BYTE);
    Slice sep0 = slice_from((void *)(uintptr_t)&zero, 1, 1, TYPE_BYTE);
    for (Int i = 0; i < n; i++) {
        Int c = bytes_count(sl(b + i, n - i), sep1);
        if (c != 0)
            testing_t_fatalf_v(t, "Count(b[%d:], {1})=%d, want 0\n", i, c);
        c = bytes_count(sl(b, i), sep0);
        if (c != i)
            testing_t_fatalf_v(t, "Count(b[:%d], {0})=%d, want %d\n", i, c, i);
    }
    release(b, n);
}

#define TESTS(X)                                                                       \
    X(TestEqualNearPageBoundary)                                                       \
    X(TestIndexByteNearPageBoundary)                                                   \
    X(TestIndexNearPageBoundary)                                                       \
    X(TestCountNearPageBoundary)

TESTING_MAIN(TESTS)
