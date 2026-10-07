/* net/http/internal/httpsfv, which Go bundles from golang.org/x/net.
 *
 * A parser for HTTP Structured Field Values, RFC 9651, that does not build a
 * tree. Each parse function walks its input and hands the pieces to a callback
 * as they are found, still in their wire form, and says whether the whole input
 * was well formed. The callback is called for the pieces before a mistake, so
 * what it was given is only good when the parse returns true. A nil callback is
 * allowed.
 *
 * Every Str a parse function gives back points into its input, except the one
 * burrow__httpsfv_parse_display_string makes, which is in memory from the
 * allocator it is given.
 *
 * The consume functions are the steps the parse functions are built from. Each
 * reads one piece from the front of s, and on success sets consumed to it and
 * rest to what follows. On failure consumed is empty and rest is s. They are
 * here because Go's tests call them.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/internal/httpsfv */

#ifndef BURROW_SRC_NET_HTTPSFV_H
#define BURROW_SRC_NET_HTTPSFV_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/time.h"

#include <stdbool.h>
#include <stdint.h>

/* func(member, param string) for a list and func(bareItem, param string) for an
 * inner list or an item. */
BURROW_FUNC(HttpsfvItemFunc, void, Str item, Str param);

/* func(key, val, param string) for a dictionary. */
BURROW_FUNC(HttpsfvDictFunc, void, Str key, Str val, Str param);

/* func(key, val string) for parameters. */
BURROW_FUNC(HttpsfvParamFunc, void, Str key, Str val);

/* ParseList: f gets each member and its parameters. A member that is an inner
 * list comes whole, parentheses and all. */
bool burrow__httpsfv_parse_list(Str s, HttpsfvItemFunc f);

/* ParseBareInnerList: an inner list without the parameters after its closing
 * parenthesis. f gets each item in it and the item's parameters. */
bool burrow__httpsfv_parse_bare_inner_list(Str s, HttpsfvItemFunc f);

/* ParseItem: f is called once, with the bare item and its parameters. */
bool burrow__httpsfv_parse_item(Str s, HttpsfvItemFunc f);

/* ParseDictionary: f gets each key, its value and its parameters. A key with
 * no value gets "?1", which is boolean true. */
bool burrow__httpsfv_parse_dictionary(Str s, HttpsfvDictFunc f);

/* ParseParameter: f gets each key and its value, "?1" when there is none. */
bool burrow__httpsfv_parse_parameter(Str s, HttpsfvParamFunc f);

/* ParseInteger and ParseDecimal. A decimal has to have a point in it. *out is
 * 0 when the result is false. */
bool burrow__httpsfv_parse_integer(Str s, int64_t *out);
bool burrow__httpsfv_parse_decimal(Str s, double *out);

/* ParseString: what is between the quotes. As in Go the escapes are left as
 * they are, so "a\"b" gives a\"b. */
BURROW_BORROWS(out, s) bool burrow__httpsfv_parse_string(Str s, Str *out);

/* ParseToken: s itself. */
BURROW_BORROWS(out, s) bool burrow__httpsfv_parse_token(Str s, Str *out);

/* ParseByteSequence: what is between the colons, which is still base64, as in
 * Go. Go's is a copy, and this one points into s. */
BURROW_BORROWS(out, s) bool burrow__httpsfv_parse_byte_sequence(Str s, Str *out);

/* ParseBoolean. */
bool burrow__httpsfv_parse_boolean(Str s, bool *out);

/* ParseDate: the time that many seconds after the Unix epoch, in the local
 * zone, the same as time.Unix gives. */
bool burrow__httpsfv_parse_date(Str s, Time *out);

/* ParseDisplayString: the text with its %xx escapes decoded, which is checked
 * to be UTF-8, in memory from a. When that memory can't be had the result is
 * false and err is burrow_err_out_of_memory, which is the only error. */
BURROW_OWNS(out) BURROW_OWNS(err) bool
burrow__httpsfv_parse_display_string(Alloc *a, Str s, Str *out, Error *err);

bool burrow__httpsfv_consume_bare_inner_list(Str s, HttpsfvItemFunc f, Str *consumed,
                                             Str *rest);
bool burrow__httpsfv_consume_item(Str s, HttpsfvItemFunc f, Str *consumed, Str *rest);
bool burrow__httpsfv_consume_parameter(Str s, HttpsfvParamFunc f, Str *consumed,
                                       Str *rest);
bool burrow__httpsfv_consume_key(Str s, Str *consumed, Str *rest);
bool burrow__httpsfv_consume_integer_or_decimal(Str s, Str *consumed, Str *rest);
bool burrow__httpsfv_consume_string(Str s, Str *consumed, Str *rest);
bool burrow__httpsfv_consume_token(Str s, Str *consumed, Str *rest);
bool burrow__httpsfv_consume_byte_sequence(Str s, Str *consumed, Str *rest);
bool burrow__httpsfv_consume_boolean(Str s, Str *consumed, Str *rest);
bool burrow__httpsfv_consume_date(Str s, Str *consumed, Str *rest);
bool burrow__httpsfv_consume_display_string(Str s, Str *consumed, Str *rest);
bool burrow__httpsfv_consume_bare_item(Str s, Str *consumed, Str *rest);

#endif
