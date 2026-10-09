/* Derived from Go's src/net/http/internal/ascii/print.go.
 * Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_ascii.h"

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/strings.h"

#include <stdbool.h>

static Byte ha_lower(Byte b) {
    if ('A' <= b && b <= 'Z')
        return (Byte)(b + ('a' - 'A'));
    return b;
}

bool burrow__http_ascii_equal_fold(Str s, Str t) {
    if (s.len != t.len)
        return false;
    for (Int i = 0; i < s.len; i++) {
        if (ha_lower(s.p[i]) != ha_lower(t.p[i]))
            return false;
    }
    return true;
}

bool burrow__http_ascii_is_print(Str s) {
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] < ' ' || s.p[i] > '~')
            return false;
    }
    return true;
}

bool burrow__http_ascii_is(Str s) {
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] > 0x7F)
            return false;
    }
    return true;
}

Str burrow__http_ascii_to_lower(Alloc *a, Str s, bool *ok) {
    if (!burrow__http_ascii_is_print(s)) {
        *ok = false;
        return BURROW_STR_EMPTY;
    }
    Str out = strings_to_lower(a, s);
    /* strings_to_lower gives s back when there is nothing to change, so an
     * empty result for a string that was not empty means a said no. */
    *ok = out.len == s.len;
    return out;
}
