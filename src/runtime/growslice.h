/* How Go's append grows a slice, size classes and all, for the ports that have
 * to know the capacity a slice ends up with because Go's code can read up to
 * it. slice_append keeps to nextslicecap alone, as the size classes are about
 * Go's allocator rather than about the language.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_RUNTIME_GROWSLICE_H
#define BURROW_SRC_RUNTIME_GROWSLICE_H

#include "burrow/core.h"

#include <stddef.h>
#include <stdint.h>

/* Go's size classes, from internal/runtime/gc/sizeclasses.go. */
static const uint16_t growslice_size_classes[] = {
    0,     8,     16,    24,    32,    48,    64,    80,    96,    112,   128,   144,
    160,   176,   192,   208,   224,   240,   256,   288,   320,   352,   384,   416,
    448,   480,   512,   576,   640,   704,   768,   896,   1024,  1152,  1280,  1408,
    1536,  1792,  2048,  2304,  2688,  3072,  3200,  3456,  4096,  4864,  5376,  6144,
    6528,  6784,  6912,  8192,  9472,  9728,  10240, 10880, 12288, 13568, 14336, 16384,
    18432, 19072, 20480, 21760, 24576, 27264, 28672, 32768,
};

/* runtime.roundupsize for memory with no pointers in it. */
static inline Int growslice_roundupsize(Int size) {
    if (size <= 32768 - 8) {
        size_t lo = 0,
               hi =
                   sizeof growslice_size_classes / sizeof growslice_size_classes[0] - 1;
        while (lo < hi) {
            size_t m = lo + (hi - lo) / 2;
            if ((Int)growslice_size_classes[m] < size)
                lo = m + 1;
            else
                hi = m;
        }
        return (Int)growslice_size_classes[lo];
    }
    Int r = size + 8192 - 1;
    if (r < size)
        return size;
    return r & ~(Int)(8192 - 1);
}

/* runtime.nextslicecap. */
static inline Int growslice_nextslicecap(Int new_len, Int old_cap) {
    Int newcap = old_cap;
    Int doublecap = newcap + newcap;
    if (new_len > doublecap)
        return new_len;
    if (old_cap < 256)
        return doublecap;
    for (;;) {
        newcap += (newcap + 3 * (Int)256) >> 2;
        if ((uint64_t)newcap >= (uint64_t)new_len)
            break;
    }
    if (newcap <= 0)
        return new_len;
    return newcap;
}

#endif /* BURROW_SRC_RUNTIME_GROWSLICE_H */
