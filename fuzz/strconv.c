/* strconv asked everything it can be asked about one byte string.
 *
 * fuzz/oracle/strconv.go writes the same report from Go, line for line. A
 * change here is a change there.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include <burrow/error.h>
#include <burrow/mem.h>
#include <burrow/mem/arena.h>
#include <burrow/mem/heap.h>
#include <burrow/slice.h>
#include <burrow/strconv.h>

#include <string.h>

#include "fuzz.h"

char *oracle_strconv(unsigned char *data, size_t n, size_t *outlen);

static const char float_fmts[] = "beEfgGxX";

static void report_str(Report *out, Str s) {
    report_printf(out, "%.*s", (int)s.len, (const char *)s.p);
}

static void report_err(Report *out, Error err) {
    if (BURROW_OK(err))
        report_printf(out, " nil\n");
    else {
        report_printf(out, " ");
        report_str(out, error_text(err));
        report_printf(out, "\n");
    }
}

static uint64_t le(const uint8_t *p, int n) {
    uint64_t u = 0;
    for (int i = n - 1; i >= 0; i--)
        u = u << 8 | p[i];
    return u;
}

static double f64(uint64_t u) {
    double f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static uint64_t bits64(double f) {
    uint64_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static double f32(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return (double)f;
}

/* Every format, at the shortest precision and at prec, the way both sides see
 * f. */
static void formats(Alloc *a, Report *out, double f, Int prec, Int bit_size) {
    for (const char *c = float_fmts; *c != '\0'; c++) {
        report_printf(out, "format %c %d ", *c, (int)bit_size);
        report_str(out, strconv_format_float(a, f, (Byte)*c, -1, bit_size));
        report_printf(out, " ");
        report_str(out, strconv_format_float(a, f, (Byte)*c, prec, bit_size));
        report_printf(out, "\n");
    }
}

static void burrow_strconv(const uint8_t *data, size_t n, Report *out) {
    Str s = {data, (Int)n};
    Error err;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    ArenaMark em = error_mark();
    Int prec = n > 0 ? data[0] % 24 : 3;

    static const Int bases[] = {0, 2, 8, 10, 16, 36};
    static const Int sizes[] = {8, 32, 64};
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 3; j++) {
            int64_t v = strconv_parse_int(s, bases[i], sizes[j], &err);
            report_printf(out, "parse_int %d %d %lld", (int)bases[i], (int)sizes[j],
                          (long long)v);
            report_err(out, err);
            uint64_t u = strconv_parse_uint(s, bases[i], sizes[j], &err);
            report_printf(out, "parse_uint %d %d %llu", (int)bases[i], (int)sizes[j],
                          (unsigned long long)u);
            report_err(out, err);
        }
    }
    Int ai = strconv_atoi(s, &err);
    report_printf(out, "atoi %lld", (long long)ai);
    report_err(out, err);
    bool b = strconv_parse_bool(s, &err);
    report_printf(out, "parse_bool %d", b);
    report_err(out, err);

    double f = strconv_parse_float(s, 64, &err);
    report_printf(out, "parse_float 64 %016llx", (unsigned long long)bits64(f));
    report_err(out, err);
    formats(a, out, f, prec, 64);
    f = strconv_parse_float(s, 32, &err);
    report_printf(out, "parse_float 32 %016llx", (unsigned long long)bits64(f));
    report_err(out, err);
    formats(a, out, f, prec, 32);

    static const Int csizes[] = {64, 128};
    for (int j = 0; j < 2; j++) {
        Complex128 c = strconv_parse_complex(s, csizes[j], &err);
        report_printf(out, "parse_complex %d %016llx %016llx", (int)csizes[j],
                      (unsigned long long)bits64(c.re),
                      (unsigned long long)bits64(c.im));
        report_err(out, err);
        report_printf(out, "format_complex ");
        report_str(out, strconv_format_complex(a, c, 'g', -1, csizes[j]));
        report_printf(out, " ");
        report_str(out, strconv_format_complex(a, c, 'e', prec, csizes[j]));
        report_printf(out, "\n");
    }

    report_printf(out, "quote ");
    report_str(out, strconv_quote(a, s));
    report_printf(out, "\nquote_to_ascii ");
    report_str(out, strconv_quote_to_ascii(a, s));
    report_printf(out, "\nquote_to_graphic ");
    report_str(out, strconv_quote_to_graphic(a, s));
    report_printf(out, "\ncan_backquote %d\n", strconv_can_backquote(s));

    Str u = strconv_unquote(a, s, &err);
    report_printf(out, "unquote ");
    report_hex(out, u.p, (size_t)u.len);
    report_err(out, err);
    Str qp = strconv_quoted_prefix(s, &err);
    report_printf(out, "quoted_prefix ");
    report_hex(out, qp.p, (size_t)qp.len);
    report_err(out, err);

    static const Byte quotes[] = {'"', '\'', 0};
    for (int j = 0; j < 3; j++) {
        for (Str rest = s; rest.len > 0;) {
            bool mb;
            Str tail;
            Rune r = strconv_unquote_char(rest, quotes[j], &mb, &tail, &err);
            report_printf(out, "unquote_char %d %ld %d %lld", quotes[j], (long)r, mb,
                          (long long)(BURROW_OK(err) ? rest.len - tail.len : 0));
            report_err(out, err);
            if (!BURROW_OK(err))
                break;
            rest = tail;
        }
    }

    for (size_t i = 0; i + 4 <= n; i += 4) {
        Rune r = (Rune)(uint32_t)le(data + i, 4);
        report_printf(out, "rune %ld %d %d ", (long)r, strconv_is_print(r),
                      strconv_is_graphic(r));
        report_str(out, strconv_quote_rune(a, r));
        report_printf(out, " ");
        report_str(out, strconv_quote_rune_to_ascii(a, r));
        report_printf(out, " ");
        report_str(out, strconv_quote_rune_to_graphic(a, r));
        report_printf(out, "\n");
        formats(a, out, f32((uint32_t)le(data + i, 4)), prec, 32);
    }
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t v = le(data + i, 8);
        Int base = 2 + data[i] % 35;
        report_printf(out, "format_int %d ", (int)base);
        report_str(out, strconv_format_int(a, (int64_t)v, base));
        report_printf(out, " ");
        report_str(out, strconv_format_uint(a, v, base));
        report_printf(out, "\n");
        formats(a, out, f64(v), prec, 64);
    }

    error_release(em);
    arena_free(&ar);
}

const FuzzTarget fuzz_target = {"strconv", burrow_strconv, oracle_strconv};
