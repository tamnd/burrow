/* golang.org/x/net/http/httpguts, the copy Go vendors.
 *
 * Small pieces of the HTTP grammar that net/http and its HTTP/2 side share:
 * what a token is, which header names and values are valid, and which headers
 * may not be trailers. It is an internal of burrow's for the same reason it
 * is one of Go's, which is that only net/http uses it.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package xnet/httpguts */

#ifndef BURROW_SRC_XNET_HTTPGUTS_H
#define BURROW_SRC_XNET_HTTPGUTS_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"

#include <stdbool.h>
#include <stdint.h>

/* ValidTrailerHeader: whether name may be sent as a trailer. Go puts name in
 * canonical form first and looks it up. This does the same without making the
 * canonical form, so it needs no memory. */
bool burrow__httpguts_valid_trailer_header(Str name);

/* IsTokenRune: whether r may be part of a token in RFC 7230's grammar. */
bool burrow__httpguts_is_token_rune(int32_t r);

/* HeaderValuesContainsToken: whether any of the n values, each a comma list,
 * has token in it, ignoring ASCII case and optional white space. */
bool burrow__httpguts_header_values_contains_token(const Str *values, Int n, Str token);

/* ValidHeaderFieldName: whether v is a token, which is what an HTTP/1.x header
 * name is. HTTP/2 also rules out upper case, and that is for the caller. */
bool burrow__httpguts_valid_header_field_name(Str v);

/* ValidHostHeader: whether every byte of h may appear in a Host header. Like
 * Go's, this is more lenient than the grammar. */
bool burrow__httpguts_valid_host_header(Str h);

/* ValidHeaderFieldValue: whether v has no control bytes other than space and
 * tab. */
bool burrow__httpguts_valid_header_field_value(Str v);

/* PunycodeHostPort: "host" or "host:port" with the host in IDNA Punycode. An
 * ASCII v comes back as it is. Otherwise the result is in memory from a, and
 * so is the error when the host can't be converted, which gives an empty
 * result. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, v) BURROW_OWNS(err) Str
burrow__httpguts_punycode_host_port(Alloc *a, Str v, Error *err);

#endif
