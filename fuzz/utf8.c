/* unicode/utf8 asked everything it can be asked about one byte string.
 *
 * fuzz/oracle/utf8.go writes the same report from Go, line for line. A change
 * here is a change there.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include <burrow/mem.h>
#include <burrow/mem/heap.h>
#include <burrow/slice.h>
#include <burrow/utf8.h>

#include "fuzz.h"

char *oracle_utf8(unsigned char *data, size_t n, size_t *outlen);

static void burrow_utf8(const uint8_t *data, size_t n, Report *out) {
    Slice b = slice_from((void *)data, (Int)n, (Int)n, TYPE_BYTE);
    Str s = {data, (Int)n};

    report_printf(out, "valid %d\n", utf8_valid(b));
    report_printf(out, "valid_string %d\n", utf8_valid_string(s));
    report_printf(out, "rune_count %lld\n", (long long)utf8_rune_count(b));
    report_printf(out, "rune_count_in_string %lld\n",
                  (long long)utf8_rune_count_in_string(s));
    report_printf(out, "full_rune %d\n", utf8_full_rune(b));
    report_printf(out, "full_rune_in_string %d\n", utf8_full_rune_in_string(s));
    if (n > 0) {
        report_printf(out, "rune_start %d\n", utf8_rune_start(data[0]));
    }

    for (Slice p = b; p.len > 0;) {
        Int size;
        Rune r = utf8_decode_rune(p, &size);
        report_printf(out, "decode %ld %lld\n", (long)r, (long long)size);
        p.p = (Byte *)p.p + size;
        p.len -= size;
        p.cap -= size;
    }
    for (Str p = s; p.len > 0;) {
        Int size;
        Rune r = utf8_decode_rune_in_string(p, &size);
        report_printf(out, "decode_in_string %ld %lld\n", (long)r, (long long)size);
        p.p += size;
        p.len -= size;
    }
    for (Slice p = b; p.len > 0;) {
        Int size;
        Rune r = utf8_decode_last_rune(p, &size);
        report_printf(out, "decode_last %ld %lld\n", (long)r, (long long)size);
        p.len -= size;
    }
    for (Str p = s; p.len > 0;) {
        Int size;
        Rune r = utf8_decode_last_rune_in_string(p, &size);
        report_printf(out, "decode_last_in_string %ld %lld\n", (long)r,
                      (long long)size);
        p.len -= size;
    }

    Alloc *a = heap_allocator();
    for (size_t i = 0; i + 4 <= n; i += 4) {
        const uint8_t *q = data + i;
        Rune r = (Rune)((uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 |
                        (uint32_t)q[3] << 24);
        Byte buf[UTF8_UTF_MAX];
        Int w =
            utf8_encode_rune(slice_from(buf, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE), r);
        report_printf(out, "rune %ld len %lld valid %d encode ", (long)r,
                      (long long)utf8_rune_len(r), utf8_valid_rune(r));
        report_hex(out, buf, (size_t)w);
        report_printf(out, "\n");

        /* Three bytes with no room after them, so the append has to grow. It
		 * copies rather than writing into the fuzzer's input, which is read
		 * only. */
        Slice grown = utf8_append_rune(a, slice_from((void *)q, 3, 3, TYPE_BYTE), r);
        report_printf(out, "append ");
        report_hex(out, grown.p, (size_t)grown.len);
        report_printf(out, "\n");
        mem_free(a, grown.p, (size_t)grown.cap, 1);
    }
}

const FuzzTarget fuzz_target = {"utf8", burrow_utf8, oracle_utf8};
