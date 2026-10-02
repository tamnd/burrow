/* testing/iotest, readers and writers that misbehave on purpose, for testing
 * code that reads and writes.
 *
 *     IoReader r = iotest_one_byte_reader(a, strings_reader_as_io_reader(&sr));
 *     Error err = iotest_test_reader(r, BURROW_B("hello"));
 *
 * Each wrapper is one small allocation from a, holding the reader or writer
 * it wraps, which has to outlive it. iotest_reader_free and
 * iotest_writer_free give one back, or free the arena they came from.
 * A wrapper is nil when a refuses.
 *
 * Go's NewReadLogger and NewWriteLogger are not here yet. They print through
 * package log, and will come with it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package testing/iotest */

#ifndef BURROW_TESTING_IOTEST_H
#define BURROW_TESTING_IOTEST_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* iotest.ErrTimeout, "timeout", what iotest_timeout_reader returns. */
extern const Error iotest_err_timeout;

/* iotest.OneByteReader: every Read that asks for something reads one byte
 * from r. */
BURROW_OWNS(ret) IoReader iotest_one_byte_reader(Alloc *a, IoReader r);

/* iotest.HalfReader: every Read reads half of what it asks for from r,
 * rounded up. */
BURROW_OWNS(ret) IoReader iotest_half_reader(Alloc *a, IoReader r);

/* iotest.DataErrReader: the error at the end, usually io_eof, comes back
 * with the last of the data rather than from the Read after it. */
BURROW_OWNS(ret) IoReader iotest_data_err_reader(Alloc *a, IoReader r);

/* iotest.TimeoutReader: the second Read returns iotest_err_timeout and no
 * data. Every other Read reads from r. */
BURROW_OWNS(ret) IoReader iotest_timeout_reader(Alloc *a, IoReader r);

/* iotest.ErrReader: every Read returns 0 and err. */
BURROW_OWNS(ret) IoReader iotest_err_reader(Alloc *a, Error err);

/* iotest.TruncateWriter: writes the first n bytes to w and then throws the
 * rest away, while reporting every write as complete. */
BURROW_OWNS(ret) IoWriter iotest_truncate_writer(Alloc *a, IoWriter w, int64_t n);

/* Give back a wrapper made above. Nothing happens to the reader or writer
 * it wraps. A nil one is fine. */
void iotest_reader_free(Alloc *a, IoReader r);
void iotest_writer_free(Alloc *a, IoWriter w);

/* iotest.TestReader: reads r to the end in reads of different sizes and
 * checks it gives content. When r's type also has Seek or ReadAt, those are
 * checked too. The error is nil when everything was right, and otherwise
 * says what was wrong, sometimes over several lines. */
BURROW_STATIC(ret) Error iotest_test_reader(IoReader r, Slice content);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TESTING_IOTEST_H */
