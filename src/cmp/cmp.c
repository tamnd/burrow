/* cmp, derived from Go's src/cmp/cmp.go (go1.27.1).
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/cmp.h"

#include "burrow/core.h"

/* The integers have no NaN, so Compare is two comparisons and Less is one. */
#define CMP_INT(T, suffix)                                                             \
    int cmp_compare_##suffix(T x, T y) {                                               \
        return (x > y) - (x < y);                                                      \
    }                                                                                  \
    bool cmp_less_##suffix(T x, T y) {                                                 \
        return x < y;                                                                  \
    }                                                                                  \
    T cmp_or_##suffix(T x, T y) {                                                      \
        return x != 0 ? x : y;                                                         \
    }

CMP_INT(int8_t, int8)
CMP_INT(int16_t, int16)
CMP_INT(int32_t, int32)
CMP_INT(int64_t, int64)
CMP_INT(uint8_t, uint8)
CMP_INT(uint16_t, uint16)
CMP_INT(uint32_t, uint32)
CMP_INT(uint64_t, uint64)

/* isNaN is x != x, as in Go, which needs no math library. */
#define CMP_FLOAT(T, suffix)                                                           \
    int cmp_compare_##suffix(T x, T y) {                                               \
        bool x_nan = x != x;                                                           \
        bool y_nan = y != y;                                                           \
        if (x_nan)                                                                     \
            return y_nan ? 0 : -1;                                                     \
        if (y_nan)                                                                     \
            return +1;                                                                 \
        return (x > y) - (x < y);                                                      \
    }                                                                                  \
    bool cmp_less_##suffix(T x, T y) {                                                 \
        return (x != x && y == y) || x < y;                                            \
    }                                                                                  \
    T cmp_or_##suffix(T x, T y) {                                                      \
        return x != 0 ? x : y;                                                         \
    }

/* NOLINTBEGIN(misc-redundant-expression) */
CMP_FLOAT(float, float32)
CMP_FLOAT(double, float64)
/* NOLINTEND(misc-redundant-expression) */

int cmp_compare_str(Str x, Str y) {
    int c = str_cmp(x, y);
    return (c > 0) - (c < 0);
}

bool cmp_less_str(Str x, Str y) {
    return str_cmp(x, y) < 0;
}

Str cmp_or_str(Str x, Str y) {
    return x.len != 0 ? x : y;
}

bool cmp_or_bool(bool x, bool y) {
    return x || y;
}
