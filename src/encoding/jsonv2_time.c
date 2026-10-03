/* Derived from Go's src/encoding/json/v2/arshal_time.go.
 * Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/json/v2.h"

#include "burrow/fmt.h"
#include "burrow/math.h"
#include "burrow/math/bits.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strconv.h"
#include "burrow/time.h"

#include "jsonv2_internal.h"

#include <string.h>

/* Go gives Time and Duration their own arshalers rather than methods, so that
 * package time need not import json. They take over from whatever methods the
 * two types have, and a WithMarshalers function still comes before them. */

#define JVT_SECOND ((uint64_t)1000000000)
#define JVT_HOUR ((uint64_t)3600 * JVT_SECOND)

BURROW_SENTINEL_ERROR(burrow__jsonv2_err_inaccurate_date_units,
                      "inaccurate year, month, week, or day units");

static Str jvt_sub(Str s, Int from, Int to) {
    return str_from_bytes(s.p + from, to - from);
}

static void jvt_put_uint(JsonBuf *b, uint64_t n) {
    Byte buf[20];
    Int i = (Int)sizeof buf;
    do {
        buf[--i] = (Byte)('0' + n % 10);
        n /= 10;
    } while (n > 0);
    jsonbuf_put(b, buf + i, (Int)sizeof buf - i);
}

/* appendPaddedBase10: n zero padded to the width of max10 - 1. */
static void jvt_put_padded(JsonBuf *b, uint64_t n, uint64_t max10) {
    if (n < max10 / 10) {
        Int i = b->len;
        jvt_put_uint(b, n + max10 / 10);
        if (!b->failed)
            b->p[i]--;
        return;
    }
    jvt_put_uint(b, n);
}

/* appendFracBase10: the fraction n/max10, with no trailing zeros. */
static void jvt_put_frac(JsonBuf *b, uint64_t n, uint64_t max10) {
    if (n == 0)
        return;
    jsonbuf_byte(b, '.');
    jvt_put_padded(b, n, max10);
    while (!b->failed && b->len > 0 && b->p[b->len - 1] == '0')
        b->len--;
}

static bool jvt_is_digit(Byte c) {
    return c >= '0' && c <= '9';
}

/* parsePaddedBase10. A short b is read as if it had zeros on the end, and a
 * long one is cut off, but what is cut off must still be digits. */
static bool jvt_parse_padded(Str b, uint64_t max10, uint64_t *out) {
    uint64_t n = 0;
    Int i = 0;
    for (uint64_t pow10 = 1; pow10 < max10; pow10 *= 10) {
        n *= 10;
        if (i < b.len) {
            if (!jvt_is_digit(b.p[i])) {
                *out = n;
                return false;
            }
            n += (uint64_t)(b.p[i] - '0');
            i++;
        }
    }
    *out = n;
    for (; i < b.len; i++)
        if (!jvt_is_digit(b.p[i]))
            return false;
    return true;
}

/* parseFracBase10. */
static bool jvt_parse_frac(Str b, uint64_t max10, uint64_t *out) {
    *out = 0;
    if (b.len == 0)
        return true;
    if (b.len < 2 || b.p[0] != '.')
        return false;
    return jvt_parse_padded(jvt_sub(b, 1, b.len), max10, out);
}

/* consumeSign. */
static Str jvt_consume_sign(Str b, bool allow_plus, bool *neg) {
    *neg = false;
    if (b.len > 0) {
        if (b.p[0] == '-') {
            *neg = true;
            return jvt_sub(b, 1, b.len);
        }
        if (b.p[0] == '+' && allow_plus)
            return jvt_sub(b, 1, b.len);
    }
    return b;
}

/* bytesCutByte with include set: the suffix keeps c. */
static Str jvt_cut_byte(Str b, Byte c, Str *suffix) {
    for (Int i = 0; i < b.len; i++) {
        if (b.p[i] == c) {
            *suffix = jvt_sub(b, i, b.len);
            return jvt_sub(b, 0, i);
        }
    }
    *suffix = BURROW_STR_EMPTY;
    return b;
}

/* cutBytes from parseDurationISO8601: bytes.Cut with either of two bytes. */
static bool jvt_cut2(Str b, Byte c0, Byte c1, Str *prefix, Str *suffix) {
    for (Int i = 0; i < b.len; i++) {
        if (b.p[i] == c0 || b.p[i] == c1) {
            *prefix = jvt_sub(b, 0, i);
            *suffix = jvt_sub(b, i + 1, b.len);
            return true;
        }
    }
    *prefix = b;
    *suffix = BURROW_STR_EMPTY;
    return false;
}

/* parseDec2. Undefined, as in Go, when b is not digits. */
static unsigned jvt_dec2(const Byte *b, Int n) {
    if (n < 2)
        return 0;
    return (unsigned)(Byte)(10 * (b[0] - '0') + (b[1] - '0'));
}

static uint64_t jvt_abs(int64_t d, bool *neg) {
    *neg = d < 0;
    return *neg ? 0 - (uint64_t)d : (uint64_t)d;
}

/* mayApplyDurationSign, which wraps the way Go's multiply does. */
static Duration jvt_apply_sign(uint64_t n, bool neg) {
    return (Duration)(neg ? 0 - n : n);
}

static bool jvt_parse_uint(Str b, uint64_t *v) {
    *v = 0;
    return b.len > 0 && burrow__jsonwire_parse_uint(b.p, b.len, v);
}

/* ------------------------------------------------------------------ duration */

void burrow__jsonv2_append_duration_base10(JsonBuf *b, Duration d, uint64_t pow10) {
    bool neg;
    uint64_t n = jvt_abs(d, &neg);
    if (neg)
        jsonbuf_byte(b, '-');
    jvt_put_uint(b, n / pow10);
    jvt_put_frac(b, n % pow10, pow10);
}

static Error jvt_duration_error(const char *what, Str b, Error cause) {
    if (what[0] == 'I')
        return fmt_errorf_v("invalid ISO 8601 duration %q: %w", b, cause);
    return fmt_errorf_v("invalid duration %q: %w", b, cause);
}

Duration burrow__jsonv2_parse_duration_base10(Str b, uint64_t pow10, Error *err) {
    bool neg;
    Str rest = jvt_consume_sign(b, false, &neg);
    Str frac_bytes;
    Str whole_bytes = jvt_cut_byte(rest, '.', &frac_bytes);
    uint64_t whole, frac, lo, co;
    bool ok_whole = jvt_parse_uint(whole_bytes, &whole);
    bool ok_frac = jvt_parse_frac(frac_bytes, pow10, &frac);
    uint64_t hi = bits_mul64(whole, pow10, &lo);
    uint64_t sum = bits_add64(lo, frac, 0, &co);
    Duration d = jvt_apply_sign(sum, neg);
    *err = BURROW_NO_ERROR;
    if ((!ok_whole && whole != UINT64_MAX) || !ok_frac) {
        *err = jvt_duration_error("", b, strconv_err_syntax);
        return 0;
    }
    if (!ok_whole || hi > 0 || co > 0 || neg != (d < 0)) {
        *err = jvt_duration_error("", b, strconv_err_range);
        return 0;
    }
    return d;
}

/* appendDurationISO8601: only hours, minutes and seconds, which ISO 8601 calls
 * accurate, with the parts that are zero left out. */
void burrow__jsonv2_append_duration_iso8601(JsonBuf *b, Duration d) {
    if (d == 0) {
        jsonbuf_str(b, JV_LIT("PT0S"));
        return;
    }
    bool neg;
    uint64_t n = jvt_abs(d, &neg);
    if (neg)
        jsonbuf_byte(b, '-');
    jsonbuf_str(b, JV_LIT("PT"));
    uint64_t nsec = n % JVT_SECOND;
    n /= JVT_SECOND;
    uint64_t sec = n % 60;
    n /= 60;
    uint64_t min = n % 60, hour = n / 60;
    if (hour > 0) {
        jvt_put_uint(b, hour);
        jsonbuf_byte(b, 'H');
    }
    if (min > 0) {
        jvt_put_uint(b, min);
        jsonbuf_byte(b, 'M');
    }
    if (sec > 0 || nsec > 0) {
        jvt_put_uint(b, sec);
        jvt_put_frac(b, nsec, JVT_SECOND);
        jsonbuf_byte(b, 'S');
    }
}

/* What parseDurationISO8601's closure shares with it. */
typedef struct JvtIso {
    bool invalid, overflow, inaccurate, saw_frac;
    uint64_t sum;
} JvtIso;

/* mayParseUnit: one number and its designator, when b has the designator. */
static Str jvt_iso_unit(JvtIso *s, Str b, Byte des_hi, Byte des_lo, uint64_t unit) {
    Str number, suffix;
    if (!jvt_cut2(b, des_hi, des_lo, &number, &suffix) || s->saw_frac)
        return b;
    uint64_t n = 0, co = 0;
    Str whole, frac;
    if (jvt_cut2(number, '.', ',', &whole, &frac)) {
        s->saw_frac = true;
        s->invalid = s->invalid || frac.len == 0 || unit > JVT_HOUR;
        if (unit == JVT_SECOND) {
            bool ok = jvt_parse_padded(frac, JVT_SECOND, &n);
            s->invalid = s->invalid || !ok;
        } else if (!s->invalid) {
            /* Go builds "0." + frac for ParseFloat and then insists that what
             * comes after the first byte of frac is all digits. */
            Byte stack[64];
            Byte *buf = stack;
            Int need = frac.len + 2;
            if (need > (Int)sizeof stack)
                buf = mem_alloc(heap_allocator(), (size_t)need, 1);
            double f = 0;
            Error perr = burrow_err_out_of_memory;
            if (buf != NULL) {
                buf[0] = '0';
                buf[1] = '.';
                memcpy(buf + 2, frac.p, (size_t)frac.len);
                perr = BURROW_NO_ERROR;
                f = strconv_parse_float(str_from_bytes(buf, need), 64, &perr);
                if (buf != stack)
                    mem_free(heap_allocator(), buf, (size_t)need, 1);
            }
            bool digits = true;
            for (Int i = 1; i < frac.len; i++)
                if (!jvt_is_digit(frac.p[i]))
                    digits = false;
            s->invalid = s->invalid || BURROW_FAILED(perr) || !digits;
            /* Never overflows, since f is within [0, 1]. */
            n = (uint64_t)math_round(f * (double)unit);
        }
        s->sum = bits_add64(s->sum, n, 0, &co);
        s->overflow = s->overflow || co > 0;
    }
    while (whole.len > 1 && whole.p[0] == '0')
        whole = jvt_sub(whole, 1, whole.len);
    bool ok = jvt_parse_uint(whole, &n);
    uint64_t lo;
    uint64_t hi = bits_mul64(n, unit, &lo);
    s->sum = bits_add64(s->sum, lo, 0, &co);
    s->invalid = s->invalid || (!ok && n != UINT64_MAX);
    s->overflow = s->overflow || (!ok && n == UINT64_MAX) || hi > 0 || co > 0;
    s->inaccurate = s->inaccurate || unit > JVT_HOUR;
    return suffix;
}

/* parseDurationISO8601: ISO 8601-1:2019's durations with a leading sign,
 * either decimal separator, leading zeros and lower case designators allowed,
 * and a fraction only in the last of the hours, minutes and seconds. Years,
 * months, weeks and days give a best guess and errInaccurateDateUnits. This is
 * the grammar of JavaScript's Temporal.Duration. */
Duration burrow__jsonv2_parse_duration_iso8601(Str b, Error *err) {
    JvtIso s = {false, false, false, false, 0};
    bool neg;
    Str rest = jvt_consume_sign(b, true, &neg);
    Str prefix, after_p, dur_date, dur_time;
    bool ok_p = jvt_cut2(rest, 'P', 'p', &prefix, &after_p);
    bool ok_t = jvt_cut2(after_p, 'T', 't', &dur_date, &dur_time);
    s.invalid = prefix.len > 0 || !ok_p || (ok_t && dur_time.len == 0) ||
                dur_date.len + dur_time.len == 0;
    if (dur_date.len > 0) {
        /* 365.2425 days a year, and a twelfth of that a month. */
        dur_date =
            jvt_iso_unit(&s, dur_date, 'Y', 'y', (uint64_t)31556952 * JVT_SECOND);
        dur_date = jvt_iso_unit(&s, dur_date, 'M', 'm', (uint64_t)2629746 * JVT_SECOND);
        dur_date = jvt_iso_unit(&s, dur_date, 'W', 'w', (uint64_t)604800 * JVT_SECOND);
        dur_date = jvt_iso_unit(&s, dur_date, 'D', 'd', (uint64_t)86400 * JVT_SECOND);
        s.invalid = s.invalid || dur_date.len > 0;
    }
    if (dur_time.len > 0) {
        dur_time = jvt_iso_unit(&s, dur_time, 'H', 'h', JVT_HOUR);
        dur_time = jvt_iso_unit(&s, dur_time, 'M', 'm', (uint64_t)60 * JVT_SECOND);
        dur_time = jvt_iso_unit(&s, dur_time, 'S', 's', JVT_SECOND);
        s.invalid = s.invalid || dur_time.len > 0;
    }
    Duration d = jvt_apply_sign(s.sum, neg);
    s.overflow = s.overflow || (neg != (d < 0) && d != 0);
    *err = BURROW_NO_ERROR;
    if (s.invalid) {
        *err = jvt_duration_error("I", b, strconv_err_syntax);
        return 0;
    }
    if (s.overflow) {
        *err = jvt_duration_error("I", b, strconv_err_range);
        return 0;
    }
    if (s.inaccurate)
        *err = jvt_duration_error("I", b, burrow__jsonv2_err_inaccurate_date_units);
    return d;
}

/* durationArshaler. base is 0 for Duration's String, 1e0, 1e3, 1e6 or 1e9 for
 * a decimal count of nanoseconds, microseconds, milliseconds or seconds, and
 * 8601 for ISO 8601. */
typedef struct JvtDuration {
    Duration td;
    uint64_t base;
} JvtDuration;

static bool jvt_duration_init(JvtDuration *a, Str format) {
    static const struct {
        const char *name;
        uint64_t base;
    } formats[] = {
        {"units", 0},    {"sec", 1000000000}, {"milli", 1000000},
        {"micro", 1000}, {"nano", 1},         {"iso8601", 8601},
    };
    for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++) {
        if (str_eq(format, str_from_cstr(formats[i].name))) {
            a->base = formats[i].base;
            return true;
        }
    }
    return false;
}

static bool jvt_duration_numeric(const JvtDuration *a) {
    return a->base != 0 && a->base != 8601;
}

Error burrow__jsonv2_append_duration(JsonBuf *b, Duration td, uint64_t base) {
    switch (base) {
    case 0: {
        Byte buf[DURATION_STRING_MAX];
        jsonbuf_put(b, buf, duration_format(td, buf));
        break;
    }
    case 8601:
        burrow__jsonv2_append_duration_iso8601(b, td);
        break;
    default:
        burrow__jsonv2_append_duration_base10(b, td, base);
        break;
    }
    return BURROW_NO_ERROR;
}

Duration burrow__jsonv2_parse_duration(Str b, uint64_t base, Error *err) {
    switch (base) {
    case 0:
        return time_parse_duration(b, err);
    case 8601:
        return burrow__jsonv2_parse_duration_iso8601(b, err);
    default:
        return burrow__jsonv2_parse_duration_base10(b, base, err);
    }
}

static Error jvt_duration_into(JsonBuf *b, void *ctx) {
    const JvtDuration *a = (const JvtDuration *)ctx;
    return burrow__jsonv2_append_duration(b, a->td, a->base);
}

/* TODO in Go (go.dev/issue/71631): there is no default form yet. */
static Error jvt_no_default(const JsontextOptions *o) {
    if (JV_GET(o, JSONFLAG_FORMAT_TAG_SUPPORTED))
        return burrow__jsonv2_errorf(
            JV_LIT("no default representation; specify an explicit format"));
    return burrow__jsonv2_errorf(JV_LIT("no default representation"));
}

static Error jvt_marshal_duration(JsontextEncoder *e, const Type *t, void *p,
                                  JsontextOptions *mo) {
    JvtDuration m = {0, 0};
    if (JV_HAS(mo, JSONFLAG_FORMAT_TAG)) {
        if (!jvt_duration_init(&m, mo->format))
            return burrow__jsonv2_invalid_format_enc(e, t, mo);
    } else if (JV_GET(mo, JSONFLAG_FORMAT_DURATION_AS_NANO)) {
        return burrow__jsonv2_marshal_default(e, t, p, mo);
    } else {
        return burrow__jsonv2_marshal_error_before(e, t, jvt_no_default(mo));
    }
    if (JV_GET(mo, JSONFLAG_STRING_TAG) && !jvt_duration_numeric(&m) &&
        !JV_GET(mo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
        return burrow__jsonv2_marshal_error_before(
            e, t, burrow__jsonv2_err_invalid_string_tag);
    m.td = *(const Duration *)p;
    bool str = !jvt_duration_numeric(&m) || jt_e_need_name(e->st.last) ||
               JV_GET(mo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_STRING_TAG);
    Error err =
        burrow__jsontext_append_raw(e, str ? '"' : '0', true, jvt_duration_into, &m);
    if (BURROW_FAILED(err) && !burrow__jsonv2_is_syntactic(err) &&
        !burrow__jsontext_is_io_error(err))
        err = burrow__jsonv2_marshal_error_before(e, t, err);
    return err;
}

static Error jvt_unmarshal_duration(JsontextDecoder *d, const Type *t, void *p,
                                    JsontextOptions *uo) {
    JvtDuration u = {0, 0};
    if (JV_HAS(uo, JSONFLAG_FORMAT_TAG)) {
        if (!jvt_duration_init(&u, uo->format))
            return burrow__jsonv2_invalid_format_dec(d, t, uo);
    } else if (JV_GET(uo, JSONFLAG_FORMAT_DURATION_AS_NANO)) {
        return burrow__jsonv2_unmarshal_default(d, t, p, uo);
    } else {
        return burrow__jsonv2_unmarshal_error_before_skipping(d, t, jvt_no_default(uo));
    }
    if (JV_GET(uo, JSONFLAG_STRING_TAG) && !jvt_duration_numeric(&u) &&
        !JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
        return burrow__jsonv2_unmarshal_error_before_skipping(
            d, t, burrow__jsonv2_err_invalid_string_tag);
    bool str = !jvt_duration_numeric(&u) || jt_e_need_name(d->st.last) ||
               JV_GET(uo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_STRING_TAG);
    Duration *td = (Duration *)p;
    unsigned flags = 0;
    Error err = BURROW_NO_ERROR;
    Slice val = burrow__jsontext_read_value(d, &flags, &err);
    if (BURROW_FAILED(err))
        return err;
    JsonBuf scratch = {NULL, 0, 0, heap_allocator(), true, false};
    Str s = str_from_bytes(val.p, val.len);
    switch (jsontext_value_kind(val)) {
    case 'n':
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            *td = 0;
        return BURROW_NO_ERROR;
    case '"':
        if (!str)
            goto mismatch;
        s = burrow__jsonwire_unquote_may_copy(
            (const Byte *)val.p, val.len, (flags & JSONWIRE_STRING_NON_VERBATIM) == 0,
            &scratch);
        break;
    case '0':
        if (str)
            goto mismatch;
        break;
    default:
        goto mismatch;
    }
    Duration v = burrow__jsonv2_parse_duration(s, u.base, &err);
    burrow__jsonbuf_free(&scratch);
    if (BURROW_FAILED(err))
        return burrow__jsonv2_unmarshal_error_after(d, t, err);
    *td = v;
    return BURROW_NO_ERROR;
mismatch:
    return burrow__jsonv2_unmarshal_error_after(d, t, BURROW_NO_ERROR);
}

/* ---------------------------------------------------------------------- time */

/* timeArshaler. base is 0 for RFC 3339, 1e0, 1e3, 1e6 or 1e9 for a decimal
 * count of seconds, milliseconds, microseconds or nanoseconds since the Unix
 * epoch, and UINT64_MAX for a layout of the caller's own. */
typedef struct JvtTime {
    Time tt;
    uint64_t base;
    Str format;
    bool loose_rfc3339;
} JvtTime;

static bool jvt_time_init(JvtTime *a, Str format) {
    const struct {
        const char *name;
        Str layout;
    } layouts[] = {
        {"ANSIC", TIME_ANSIC},
        {"UnixDate", TIME_UNIX_DATE},
        {"RubyDate", TIME_RUBY_DATE},
        {"RFC822", TIME_RFC822},
        {"RFC822Z", TIME_RFC822_Z},
        {"RFC850", TIME_RFC850},
        {"RFC1123", TIME_RFC1123},
        {"RFC1123Z", TIME_RFC1123_Z},
        {"Kitchen", TIME_KITCHEN},
        {"Stamp", TIME_STAMP},
        {"StampMilli", TIME_STAMP_MILLI},
        {"StampMicro", TIME_STAMP_MICRO},
        {"StampNano", TIME_STAMP_NANO},
        {"DateTime", TIME_DATE_TIME},
        {"DateOnly", TIME_DATE_ONLY},
        {"TimeOnly", TIME_TIME_ONLY},
    };
    static const struct {
        const char *name;
        uint64_t base;
    } unix_bases[] = {
        {"unix", 1},
        {"unixmilli", 1000},
        {"unixmicro", 1000000},
        {"unixnano", 1000000000},
    };
    /* An exported constant of package time starts with an upper case letter. */
    if (format.len == 0)
        return false;
    a->base = UINT64_MAX;
    Byte c = format.p[0];
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')) {
        a->format = format;
        return true;
    }
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        if (str_eq(format, str_from_cstr(layouts[i].name))) {
            a->format = layouts[i].layout;
            return true;
        }
    }
    if (str_eq(format, JV_LIT("RFC3339"))) {
        a->base = 0;
        a->format = TIME_RFC3339;
        return true;
    }
    if (str_eq(format, JV_LIT("RFC3339Nano"))) {
        a->base = 0;
        a->format = TIME_RFC3339_NANO;
        return true;
    }
    for (size_t i = 0; i < sizeof unix_bases / sizeof unix_bases[0]; i++) {
        if (str_eq(format, str_from_cstr(unix_bases[i].name))) {
            a->base = unix_bases[i].base;
            return true;
        }
    }
    /* Any other Go identifier is turned down, in case time adds a constant
     * by that name later. */
    if (burrow__jsonv2_all_letters(format))
        return false;
    a->format = format;
    return true;
}

static bool jvt_time_numeric(const JvtTime *a) {
    return (int64_t)a->base > 0;
}

/* negateSecNano, for nsec in [0, 1e9). */
static void jvt_negate(int64_t *sec, int64_t *nsec) {
    *sec = ~*sec;
    *nsec = -*nsec + 1000000000;
    *sec = (int64_t)((uint64_t)*sec + (uint64_t)(*nsec / 1000000000));
    *nsec %= 1000000000;
}

void burrow__jsonv2_append_time_unix(JsonBuf *b, Time t, uint64_t pow10) {
    int64_t sec = time_unix(t), nsec = (int64_t)time_nanosecond(t);
    if (sec < 0) {
        jsonbuf_byte(b, '-');
        jvt_negate(&sec, &nsec);
    }
    uint64_t usec = (uint64_t)sec, unsec = (uint64_t)nsec;
    if (pow10 == 1) {
        jvt_put_uint(b, usec);
        jvt_put_frac(b, unsec, JVT_SECOND);
    } else if (usec < JVT_SECOND) {
        jvt_put_uint(b, usec * pow10 + unsec / (JVT_SECOND / pow10));
        jvt_put_frac(b, (unsec * pow10) % JVT_SECOND, JVT_SECOND);
    } else {
        jvt_put_uint(b, usec);
        jvt_put_padded(b, unsec / (JVT_SECOND / pow10), pow10);
        jvt_put_frac(b, (unsec * pow10) % JVT_SECOND, JVT_SECOND);
    }
}

static Error jvt_time_error(Str b, Error cause) {
    return fmt_errorf_v("invalid time %q: %w", b, cause);
}

Time burrow__jsonv2_parse_time_unix(Str b, uint64_t pow10, Error *err) {
    bool neg;
    Str rest = jvt_consume_sign(b, false, &neg);
    Str frac_bytes;
    Str whole_bytes = jvt_cut_byte(rest, '.', &frac_bytes);
    uint64_t whole, frac;
    bool ok_whole = jvt_parse_uint(whole_bytes, &whole);
    bool ok_frac = jvt_parse_frac(frac_bytes, JVT_SECOND / pow10, &frac);
    int64_t sec = 0, nsec = 0;
    if (pow10 == 1) {
        sec = (int64_t)whole;
        nsec = (int64_t)frac;
    } else if (ok_whole) {
        sec = (int64_t)(whole / pow10);
        nsec = (int64_t)((whole % pow10) * (JVT_SECOND / pow10) + frac);
    } else if (whole == UINT64_MAX) {
        /* Too big for a uint64 in pow10 units, so the last digits are split
         * off as the fraction of a second. */
        Int width = pow10 == 1000 ? 3 : pow10 == 1000000 ? 6 : 9;
        ok_whole =
            jvt_parse_uint(jvt_sub(whole_bytes, 0, whole_bytes.len - width), &whole);
        uint64_t mid;
        jvt_parse_padded(jvt_sub(whole_bytes, whole_bytes.len - width, whole_bytes.len),
                         pow10, &mid);
        sec = (int64_t)whole;
        nsec = (int64_t)(mid * (JVT_SECOND / pow10) + frac);
    }
    if (neg)
        jvt_negate(&sec, &nsec);
    Time t = time_utc(time_from_unix(sec, nsec));
    *err = BURROW_NO_ERROR;
    if ((!ok_whole && whole != UINT64_MAX) || !ok_frac) {
        *err = jvt_time_error(b, strconv_err_syntax);
        return (Time){0};
    }
    if (!ok_whole || neg != (time_unix(t) < 0)) {
        *err = jvt_time_error(b, strconv_err_range);
        return (Time){0};
    }
    return t;
}

static Error jvt_time_into(JsonBuf *b, void *ctx) {
    const JvtTime *a = (const JvtTime *)ctx;
    if (a->base != 0 && a->base != UINT64_MAX) {
        burrow__jsonv2_append_time_unix(b, a->tt, a->base);
        return BURROW_NO_ERROR;
    }
    Str layout = a->format.len > 0 ? a->format : TIME_RFC3339_NANO;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice s = time_append_format(a->tt, arena_allocator(&ar),
                                 (Slice){NULL, 0, 0, TYPE_BYTE}, layout);
    Int n0 = b->len;
    jsonbuf_put(b, s.p, s.len);
    arena_free(&ar);
    if (a->base == UINT64_MAX || b->failed)
        return BURROW_NO_ERROR;
    /* Not every Time can be written as RFC 3339 (go.dev/issue/4556 and
     * go.dev/issue/54580), so the edges are checked here. */
    const Byte *p = b->p + n0;
    Int n = b->len - n0;
    if (n <= 4 || p[4] != '-')
        return burrow__jsonv2_errorf(JV_LIT("year outside of range [0,9999]"));
    if (p[n - 1] != 'Z') {
        Byte c = p[n - 6];
        if (jvt_is_digit(c) || jvt_dec2(p + n - 5, 5) >= 24)
            return burrow__jsonv2_errorf(
                JV_LIT("timezone hour outside of range [0,23]"));
    }
    return BURROW_NO_ERROR;
}

static Error jvt_marshal_time(JsontextEncoder *e, const Type *t, void *p,
                              JsontextOptions *mo) {
    JvtTime m = {{0}, 0, BURROW_STR_EMPTY, false};
    if (JV_HAS(mo, JSONFLAG_FORMAT_TAG) && !jvt_time_init(&m, mo->format))
        return burrow__jsonv2_invalid_format_enc(e, t, mo);
    if (JV_GET(mo, JSONFLAG_STRING_TAG) && !jvt_time_numeric(&m) &&
        !JV_GET(mo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
        return burrow__jsonv2_marshal_error_before(
            e, t, burrow__jsonv2_err_invalid_string_tag);
    m.tt = *(const Time *)p;
    bool str = !jvt_time_numeric(&m) || jt_e_need_name(e->st.last) ||
               JV_GET(mo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_STRING_TAG);
    Error err = burrow__jsontext_append_raw(e, str ? '"' : '0', m.base != UINT64_MAX,
                                            jvt_time_into, &m);
    if (BURROW_OK(err))
        return err;
    /* Always wrapped here, where unmarshal never is. */
    if (JV_GET(mo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
        return burrow__json_new_marshaler_error(t, err, "MarshalJSON");
    if (!burrow__jsonv2_is_syntactic(err) && !burrow__jsontext_is_io_error(err))
        err = burrow__jsonv2_marshal_error_before(e, t, err);
    return err;
}

static Error jvt_parse_error(Str b, const char *layout_elem, Str value_elem,
                             const char *message) {
    TimeParseError pe = {TIME_RFC3339, b, str_from_cstr(layout_elem), value_elem,
                         str_from_cstr(message)};
    return time_parse_error_as_error(error_allocator(), &pe);
}

/* time_unmarshal_text accepts things RFC 3339 does not (go.dev/issue/57912),
 * which Go turns down here until package time does it itself. */
static Error jvt_strict_rfc3339(Str b) {
    if (b.p[12] == ':')
        return jvt_parse_error(b, "15", jvt_sub(b, 11, 12), "");
    if (b.p[19] == ',')
        return jvt_parse_error(b, ".", JV_LIT(","), "");
    if (b.p[b.len - 1] != 'Z') {
        Str zone = jvt_sub(b, b.len - 6, b.len);
        if (jvt_dec2(b.p + b.len - 5, 5) >= 24)
            return jvt_parse_error(b, "Z07:00", zone, ": timezone hour out of range");
        if (jvt_dec2(b.p + b.len - 2, 2) >= 60)
            return jvt_parse_error(b, "Z07:00", zone, ": timezone minute out of range");
    }
    return BURROW_NO_ERROR;
}

static Error jvt_time_parse(JvtTime *u, Alloc *a, Str b) {
    Error err = BURROW_NO_ERROR;
    if (u->base == 0) {
        err = time_unmarshal_text(
            &u->tt, a, (Slice){(void *)(uintptr_t)b.p, b.len, b.len, TYPE_BYTE});
        if (BURROW_FAILED(err) || u->loose_rfc3339)
            return err;
        return jvt_strict_rfc3339(b);
    }
    if (u->base == UINT64_MAX)
        u->tt = time_parse(a, u->format, b, &err);
    else
        u->tt = burrow__jsonv2_parse_time_unix(b, u->base, &err);
    return err;
}

static Error jvt_unmarshal_time(JsontextDecoder *d, const Type *t, void *p,
                                JsontextOptions *uo) {
    JvtTime u = {{0}, 0, BURROW_STR_EMPTY, false};
    if (JV_HAS(uo, JSONFLAG_FORMAT_TAG)) {
        if (!jvt_time_init(&u, uo->format))
            return burrow__jsonv2_invalid_format_dec(d, t, uo);
    } else if (JV_GET(uo, JSONFLAG_PARSE_TIME_WITH_LOOSE_RFC3339)) {
        u.loose_rfc3339 = true;
    }
    if (JV_GET(uo, JSONFLAG_STRING_TAG) && !jvt_time_numeric(&u) &&
        !JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
        return burrow__jsonv2_unmarshal_error_before_skipping(
            d, t, burrow__jsonv2_err_invalid_string_tag);
    bool str = !jvt_time_numeric(&u) || jt_e_need_name(d->st.last) ||
               JV_GET(uo, JSONFLAG_STRINGIFY_NUMBERS | JSONFLAG_STRING_TAG);
    Time *tt = (Time *)p;
    unsigned flags = 0;
    Error err = BURROW_NO_ERROR;
    Slice val = burrow__jsontext_read_value(d, &flags, &err);
    if (BURROW_FAILED(err))
        return err;
    JsonBuf scratch = {NULL, 0, 0, heap_allocator(), true, false};
    Str s = str_from_bytes(val.p, val.len);
    switch (jsontext_value_kind(val)) {
    case 'n':
        if (!JV_GET(uo, JSONFLAG_MERGE_WITH_LEGACY_SEMANTICS))
            *tt = (Time){0};
        return BURROW_NO_ERROR;
    case '"':
        if (!str)
            goto mismatch;
        s = burrow__jsonwire_unquote_may_copy(
            (const Byte *)val.p, val.len, (flags & JSONWIRE_STRING_NON_VERBATIM) == 0,
            &scratch);
        break;
    case '0':
        if (str)
            goto mismatch;
        break;
    default:
        goto mismatch;
    }
    err = jvt_time_parse(&u, d->out_alloc, s);
    burrow__jsonbuf_free(&scratch);
    if (BURROW_FAILED(err)) {
        /* Never wrapped here, where marshal always is. */
        if (JV_GET(uo, JSONFLAG_REPORT_ERRORS_WITH_LEGACY_SEMANTICS))
            return err;
        return burrow__jsonv2_unmarshal_error_after(d, t, err);
    }
    *tt = u.tt;
    return BURROW_NO_ERROR;
mismatch:
    return burrow__jsonv2_unmarshal_error_after(d, t, BURROW_NO_ERROR);
}

/* ------------------------------------------------------------------ dispatch */

bool burrow__jsonv2_is_time_type(const Type *t) {
    return t == TYPE_TIME || t == TYPE_DURATION;
}

Error burrow__jsonv2_marshal_time(JsontextEncoder *e, const Type *t, void *p,
                                  JsontextOptions *mo) {
    if (t == TYPE_DURATION)
        return jvt_marshal_duration(e, t, p, mo);
    return jvt_marshal_time(e, t, p, mo);
}

Error burrow__jsonv2_unmarshal_time(JsontextDecoder *d, const Type *t, void *p,
                                    JsontextOptions *uo) {
    if (t == TYPE_DURATION)
        return jvt_unmarshal_duration(d, t, p, uo);
    return jvt_unmarshal_time(d, t, p, uo);
}
