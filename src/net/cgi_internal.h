/* What the cgi package's two sides share with its tests.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_NET_CGI_INTERNAL_H
#define BURROW_NET_CGI_INTERNAL_H

#include "burrow/core.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/net/http.h"

/* cgi's response, the ResponseWriter cgi_serve hands the handler. */
typedef struct burrow__CgiResponse burrow__CgiResponse;

/* newResponse. A response to req that writes to out through a bufio writer,
 * all of it made in a. NULL when a says no. */
burrow__CgiResponse *burrow__cgi_new_response(Alloc *a, HttpRequest *req, IoWriter out);

/* The response as the writer a handler takes. */
HttpResponseWriter burrow__cgi_response_writer(burrow__CgiResponse *r);

/* response.writeCGIHeader, with p the start of the body to sniff. */
void burrow__cgi_write_cgi_header(burrow__CgiResponse *r, Slice p);

/* removeLeadingDuplicates. Drops each "key=value" of env that a later one with
 * the same key overrides, in place, and gives the new length. */
Int burrow__cgi_remove_leading_duplicates(Str *env, Int n);

#endif /* BURROW_NET_CGI_INTERNAL_H */
