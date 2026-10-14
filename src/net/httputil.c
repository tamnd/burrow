/* Derived from Go's src/net/http/httputil/httputil.go, the chunked encoding
 * from net/http's internal package made public.
 * Go source: go1.27.1.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/httputil.h"

#include "http_internal.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"

#include <stddef.h>

/* The same error as net/http's, which errors_is tells by its text pointer. */
const Error httputil_err_line_too_long = {&burrow_sentinel_error_vt,
                                          &burrow__http_line_too_long_text};

IoReader httputil_new_chunked_reader(Alloc *a, IoReader r) {
    HttpChunkedReader *cr = burrow__http_new_chunked_reader(a, r);
    if (cr == NULL)
        return (IoReader){NULL, NULL};
    return burrow__http_chunked_reader_as_io_reader(cr);
}

void httputil_chunked_reader_free(Alloc *a, IoReader r) {
    burrow__http_chunked_reader_free(a, (HttpChunkedReader *)r.data);
}

IoWriteCloser httputil_new_chunked_writer(Alloc *a, IoWriter w) {
    HttpChunkedWriter *cw =
        (HttpChunkedWriter *)mem_alloc(a, sizeof *cw, _Alignof(HttpChunkedWriter));
    if (cw == NULL)
        return (IoWriteCloser){NULL, NULL};
    cw->wire = w;
    cw->flush = NULL;
    return burrow__http_chunked_writer_as_io_write_closer(cw);
}

void httputil_chunked_writer_free(Alloc *a, IoWriteCloser w) {
    if (w.data != NULL)
        mem_free(a, w.data, sizeof(HttpChunkedWriter), _Alignof(HttpChunkedWriter));
}
