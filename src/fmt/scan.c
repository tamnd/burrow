/* Derived from Go's src/fmt/scan.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file.
 *
 * The scanner is Go's, down to the way it reports a failure: every helper
 * panics with a scanError, and the two entry points that run a scan catch it
 * and return what it carried. Here that is a panic with a private type and a
 * BURROW_TRY, which costs one setjmp per scan and keeps every helper as short
 * as Go's.
 *
 * Operands differ because C does. Go is handed a pointer and asks reflect what
 * it points at. Here an operand is an Any holding the type stored and where to
 * store it, so there is no operand that is not a pointer and no error for one.
 *
 * Strings are copies made in the allocator the caller passes, since there is
 * no garbage collector to hand the token buffer to. */

#include "burrow/fmt.h"

#include "format.h"

#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdio.h>
#include <string.h>

#define SCAN_LIT(lit) BURROW_S(lit)

/* The rune getRune gives at the end of the input. */
#define SCAN_EOF (-1)

/* The width of an operand with no width, which is as good as none. */
#define SCAN_HUGE_WID (1 << 30)

/* Where the token buffer starts, before it moves to the heap. */
#define SCAN_STACK 128

static bool scan_err_eq(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* ------------------------------------------------------------ the rune source
 *
 * Go's readRune, which turns bytes into runes one byte at a time so that it
 * never reads past the rune it returns, and keeps the last rune for
 * UnreadRune. A string is read the same way, since Go reads one through a
 * reader too. A nested scan reads through the ScanState it was given. */

typedef enum ScanSrcKind {
    SCAN_SRC_STRING,
    SCAN_SRC_READER,
    SCAN_SRC_STATE
} ScanSrcKind;

typedef struct ScanRunes {
    ScanSrcKind kind;
    Str str;
    Int pos;
    IoReader r;
    FmtScanState outer;
    Byte buf[UTF8_UTF_MAX];
    Int pending;             /* bytes in pend; only more than zero for bad UTF-8 */
    Byte pend[UTF8_UTF_MAX]; /* bytes left over */
    Rune peek;               /* the next rune when >= 0, ~(previous rune) when < 0 */
} ScanRunes;

static Error scan_read_byte(ScanRunes *r, Byte *b) {
    if (r->pending > 0) {
        *b = r->pend[0];
        memmove(r->pend, r->pend + 1, UTF8_UTF_MAX - 1);
        r->pending--;
        return BURROW_NO_ERROR;
    }
    if (r->kind == SCAN_SRC_STRING) {
        if (r->pos >= r->str.len)
            return io_eof;
        *b = r->str.p[r->pos++];
        return BURROW_NO_ERROR;
    }
    /* io.ReadFull of one byte. */
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int n = r->r.vt->read(r->r.data, slice_from(r->pend, 1, 1, TYPE_BYTE), &err);
        if (n >= 1) {
            *b = r->pend[0];
            return BURROW_NO_ERROR;
        }
        if (BURROW_FAILED(err))
            return err;
    }
}

static Rune scan_runes_read(ScanRunes *r, Int *size, Error *err) {
    *size = 0;
    *err = BURROW_NO_ERROR;
    if (r->kind == SCAN_SRC_STATE)
        return r->outer.vt->read_rune(r->outer.data, size, err);
    if (r->peek >= 0) {
        Rune rr = r->peek;
        r->peek = ~rr;
        *size = utf8_rune_len(rr);
        return rr;
    }
    Error e = scan_read_byte(r, &r->buf[0]);
    if (BURROW_FAILED(e)) {
        *err = e;
        return 0;
    }
    if (r->buf[0] < UTF8_RUNE_SELF) { /* fast check for common ASCII case */
        Rune rr = r->buf[0];
        *size = 1;
        r->peek = ~rr;
        return rr;
    }
    Int n;
    for (n = 1; !utf8_full_rune(slice_from(r->buf, n, n, TYPE_BYTE)); n++) {
        e = scan_read_byte(r, &r->buf[n]);
        if (BURROW_FAILED(e)) {
            if (scan_err_eq(e, io_eof))
                break;
            *err = e;
            return 0;
        }
    }
    Int sz;
    Rune rr = utf8_decode_rune(slice_from(r->buf, n, n, TYPE_BYTE), &sz);
    if (sz < n) { /* an error, save the bytes for the next read */
        memcpy(r->pend + r->pending, r->buf + sz, (size_t)(n - sz));
        r->pending += n - sz;
    }
    r->peek = ~rr;
    *size = sz;
    return rr;
}

static Error scan_runes_unread(ScanRunes *r) {
    if (r->kind == SCAN_SRC_STATE)
        return r->outer.vt->unread_rune(r->outer.data);
    if (r->peek >= 0)
        return errors_new(
            error_allocator(),
            SCAN_LIT("fmt: scanning called UnreadRune with no rune available"));
    r->peek = ~r->peek;
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------ the state */

typedef struct Ss {
    ScanRunes rs;
    FmtBuf buf;       /* token accumulator */
    Int count;        /* runes consumed so far */
    bool at_eof;      /* already read EOF */
    bool nl_is_end;   /* whether newline terminates scan */
    bool nl_is_space; /* whether newline counts as white space */
    Int arg_limit;    /* max value of count for this arg; arg_limit <= limit */
    Int limit;        /* max value of count */
    Int max_wid;      /* width of this arg */
    Alloc *a;
} Ss;

/* Go's scanError, the panic a scan's helpers raise and its entry points
 * catch. The type is private, so no other panic can look like one. */
typedef struct ScanError {
    Error err;
} ScanError;

static const Type scan_error_desc = {
    BURROW_S_INIT("scanError"),
    BURROW_S_INIT("fmt"),
    KIND_STRUCT,
    (uint32_t)sizeof(ScanError),
    (uint16_t)_Alignof(ScanError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

BURROW_NORETURN static void ss_error(Error err) {
    ScanError se = {err};
    panic((Any){&scan_error_desc, &se});
}

BURROW_NORETURN static void ss_error_str(Str msg) {
    ss_error(errors_new(error_allocator(), msg));
}

/* ss_error_str for a literal. The Str is built here and not at the call site,
 * because a compound literal inside BURROW_TRY is a temporary gcc warns might
 * be clobbered by the longjmp, once enough gets inlined into the function with
 * the TRY in it, as it does in the amalgamation. */
BURROW_NORETURN static void ss_error_bytes(const char *p, Int n) {
    ss_error_str((Str){(const Byte *)p, n});
}

#define ss_error_lit(lit) ss_error_bytes("" lit, (Int)(sizeof(lit) - 1))

static Rune ss_read_rune(Ss *s, Int *size, Error *err) {
    *size = 0;
    *err = BURROW_NO_ERROR;
    if (s->at_eof || s->count >= s->arg_limit) {
        *err = io_eof;
        return 0;
    }
    Rune r = scan_runes_read(&s->rs, size, err);
    if (BURROW_OK(*err)) {
        s->count++;
        if (s->nl_is_end && r == '\n')
            s->at_eof = true;
    } else if (scan_err_eq(*err, io_eof)) {
        s->at_eof = true;
    }
    return r;
}

/* The next rune, or SCAN_EOF at the end of the input. */
static Rune ss_get_rune(Ss *s) {
    Int size;
    Error err;
    Rune r = ss_read_rune(s, &size, &err);
    if (BURROW_FAILED(err)) {
        if (scan_err_eq(err, io_eof))
            return SCAN_EOF;
        ss_error(err);
    }
    return r;
}

/* The next rune, where the end of the input is an error. */
static Rune ss_must_read_rune(Ss *s) {
    Rune r = ss_get_rune(s);
    if (r == SCAN_EOF)
        ss_error(io_err_unexpected_eof);
    return r;
}

static void ss_unread_rune(Ss *s) {
    (void)scan_runes_unread(&s->rs);
    s->at_eof = false;
    s->count--;
}

/* Go's list of white space, which is not unicode.IsSpace: it leaves out
 * U+180E and has U+0085 and U+00A0 in it. */
static const uint16_t scan_space[][2] = {
    {0x0009, 0x000d}, {0x0020, 0x0020}, {0x0085, 0x0085}, {0x00a0, 0x00a0},
    {0x1680, 0x1680}, {0x2000, 0x200a}, {0x2028, 0x2029}, {0x202f, 0x202f},
    {0x205f, 0x205f}, {0x3000, 0x3000},
};

static bool scan_is_space(Rune r) {
    if (r < 0 || r >= 1 << 16)
        return false;
    uint16_t rx = (uint16_t)r;
    for (size_t i = 0; i < sizeof scan_space / sizeof scan_space[0]; i++) {
        if (rx < scan_space[i][0])
            return false;
        if (rx <= scan_space[i][1])
            return true;
    }
    return false;
}

static void ss_skip_space(Ss *s) {
    for (;;) {
        Rune r = ss_get_rune(s);
        if (r == SCAN_EOF)
            return;
        if (r == '\r') {
            /* peek("\n"), inline, since peek is further down. */
            Rune n = ss_get_rune(s);
            if (n != SCAN_EOF)
                ss_unread_rune(s);
            if (n == '\n')
                continue;
        }
        if (r == '\n') {
            if (s->nl_is_space)
                continue;
            ss_error_lit("unexpected newline");
        }
        if (!scan_is_space(r)) {
            ss_unread_rune(s);
            break;
        }
    }
}

static bool ss_call(RuneFunc f, Rune r) {
    if (f.f == NULL)
        return !scan_is_space(r);
    return f.f(f.env, r);
}

static Str ss_buf_str(Ss *s) {
    if (s->buf.failed)
        ss_error(burrow_err_out_of_memory);
    return (Str){s->buf.p, s->buf.len};
}

/* The run of runes that satisfy f, after the space if skip is set. */
static Str ss_token(Ss *s, bool skip, RuneFunc f) {
    if (skip)
        ss_skip_space(s);
    for (;;) {
        Rune r = ss_get_rune(s);
        if (r == SCAN_EOF)
            break;
        if (!ss_call(f, r)) {
            ss_unread_rune(s);
            break;
        }
        burrow__fmt_buf_write_rune(&s->buf, r);
    }
    return ss_buf_str(s);
}

static bool scan_index_rune(const char *ok, Rune r) {
    for (; *ok != '\0'; ok++)
        if ((Rune)(unsigned char)*ok == r)
            return true;
    return false;
}

/* Reads the next rune and keeps it in the buffer when it is in ok and accept
 * is set. A rune not in ok is put back. */
static bool ss_consume(Ss *s, const char *ok, bool accept) {
    Rune r = ss_get_rune(s);
    if (r == SCAN_EOF)
        return false;
    if (scan_index_rune(ok, r)) {
        if (accept)
            burrow__fmt_buf_write_rune(&s->buf, r);
        return true;
    }
    if (accept)
        ss_unread_rune(s);
    return false;
}

/* Whether the next rune is in ok, without consuming it. */
static bool ss_peek(Ss *s, const char *ok) {
    Rune r = ss_get_rune(s);
    if (r != SCAN_EOF)
        ss_unread_rune(s);
    return scan_index_rune(ok, r);
}

/* Panics with io_eof at the end of the input, which the entry points report
 * as it is rather than as a scan error. */
static void ss_not_eof(Ss *s) {
    Rune r = ss_get_rune(s);
    if (r == SCAN_EOF) {
        Error e = io_eof;
        panic((Any){TYPE_ERROR, &e});
    }
    ss_unread_rune(s);
}

static bool ss_accept(Ss *s, const char *ok) {
    return ss_consume(s, ok, true);
}

static bool ss_ok_verb(Rune verb, const char *ok_verbs, const char *typ) {
    if (scan_index_rune(ok_verbs, verb))
        return true;
    ss_error(fmt_errorf_v("bad verb '%%%c' for %s", verb, typ));
}

/* ------------------------------------------------------------------ values */

static bool ss_scan_bool(Ss *s, Rune verb) {
    ss_skip_space(s);
    ss_not_eof(s);
    ss_ok_verb(verb, "tv", "boolean");
    switch (ss_get_rune(s)) {
    case '0':
        return false;
    case '1':
        return true;
    case 't':
    case 'T':
        if (ss_accept(s, "rR") && (!ss_accept(s, "uU") || !ss_accept(s, "eE")))
            ss_error_lit("syntax error scanning boolean");
        return true;
    case 'f':
    case 'F':
        if (ss_accept(s, "aA") &&
            (!ss_accept(s, "lL") || !ss_accept(s, "sS") || !ss_accept(s, "eE")))
            ss_error_lit("syntax error scanning boolean");
        return false;
    default:
        return false;
    }
}

#define SCAN_BINARY "01"
#define SCAN_OCTAL "01234567"
#define SCAN_DECIMAL "0123456789"
#define SCAN_HEX "0123456789aAbBcCdDeEfF"
#define SCAN_SIGN "+-"
#define SCAN_FLOAT_VERBS "beEfFgGv"

static Int ss_get_base(Rune verb, const char **digits) {
    ss_ok_verb(verb, "bdoUxXv", "integer");
    *digits = SCAN_DECIMAL;
    switch (verb) {
    case 'b':
        *digits = SCAN_BINARY;
        return 2;
    case 'o':
        *digits = SCAN_OCTAL;
        return 8;
    case 'x':
    case 'X':
    case 'U':
        *digits = SCAN_HEX;
        return 16;
    default:
        return 10;
    }
}

static Str ss_scan_number(Ss *s, const char *digits, bool have_digits) {
    if (!have_digits) {
        ss_not_eof(s);
        if (!ss_accept(s, digits))
            ss_error_lit("expected integer");
    }
    while (ss_accept(s, digits)) {
    }
    return ss_buf_str(s);
}

static int64_t ss_scan_rune(Ss *s, Int bit_size) {
    ss_not_eof(s);
    Rune r = ss_get_rune(s);
    unsigned n = (unsigned)bit_size;
    int64_t x = (int64_t)((uint64_t)(int64_t)r << (64 - n)) >> (64 - n);
    if (x != (int64_t)r)
        ss_error(fmt_errorf_v("overflow on character value %c", r));
    return (int64_t)r;
}

/* The digits a %v integer may use, from its prefix, with the prefix and a
 * leading zero left in the buffer. Underscores are allowed after a prefix
 * because strconv checks them with base 0. */
static const char *ss_scan_base_prefix(Ss *s, bool *zero_found) {
    *zero_found = false;
    if (!ss_peek(s, "0"))
        return SCAN_DECIMAL "_";
    ss_accept(s, "0");
    *zero_found = true;
    if (ss_peek(s, "bB")) {
        ss_consume(s, "bB", true);
        return SCAN_BINARY "_";
    }
    if (ss_peek(s, "oO")) {
        ss_consume(s, "oO", true);
        return SCAN_OCTAL "_";
    }
    if (ss_peek(s, "xX")) {
        ss_consume(s, "xX", true);
        return SCAN_HEX "_";
    }
    return SCAN_OCTAL "_";
}

static int64_t ss_scan_int(Ss *s, Rune verb, Int bit_size) {
    if (verb == 'c')
        return ss_scan_rune(s, bit_size);
    ss_skip_space(s);
    ss_not_eof(s);
    const char *digits;
    Int base = ss_get_base(verb, &digits);
    bool have_digits = false;
    if (verb == 'U') {
        if (!ss_consume(s, "U", false) || !ss_consume(s, "+", false))
            ss_error_lit("bad unicode format ");
    } else {
        ss_accept(
            s, SCAN_SIGN); /* If there's a sign, it will be left in the token buffer. */
        if (verb == 'v') {
            digits = ss_scan_base_prefix(s, &have_digits);
            base = 0;
        }
    }
    Str tok = ss_scan_number(s, digits, have_digits);
    Error err = BURROW_NO_ERROR;
    int64_t i = strconv_parse_int(tok, base, 64, &err);
    if (BURROW_FAILED(err))
        ss_error(err);
    unsigned n = (unsigned)bit_size;
    int64_t x = (int64_t)((uint64_t)i << (64 - n)) >> (64 - n);
    if (x != i)
        ss_error(fmt_errorf_v("integer overflow on token %s", tok));
    return i;
}

static uint64_t ss_scan_uint(Ss *s, Rune verb, Int bit_size) {
    if (verb == 'c')
        return (uint64_t)ss_scan_rune(s, bit_size);
    ss_skip_space(s);
    ss_not_eof(s);
    const char *digits;
    Int base = ss_get_base(verb, &digits);
    bool have_digits = false;
    if (verb == 'U') {
        if (!ss_consume(s, "U", false) || !ss_consume(s, "+", false))
            ss_error_lit("bad unicode format ");
    } else if (verb == 'v') {
        digits = ss_scan_base_prefix(s, &have_digits);
        base = 0;
    }
    Str tok = ss_scan_number(s, digits, have_digits);
    Error err = BURROW_NO_ERROR;
    uint64_t i = strconv_parse_uint(tok, base, 64, &err);
    if (BURROW_FAILED(err))
        ss_error(err);
    unsigned n = (unsigned)bit_size;
    uint64_t x = (i << (64 - n)) >> (64 - n);
    if (x != i)
        ss_error(fmt_errorf_v("unsigned integer overflow on token %s", tok));
    return i;
}

/* A float's characters, appended to the buffer from start, which is where the
 * buffer is cut back to first. Returns where the token ends. The buffer is
 * appended to rather than reset so that a complex number can keep its real
 * part while it reads the imaginary one. */
static Int ss_float_token(Ss *s, Int start) {
    s->buf.len = start;
    /* NaN? */
    if (ss_accept(s, "nN") && ss_accept(s, "aA") && ss_accept(s, "nN"))
        return s->buf.len;
    /* leading sign? */
    ss_accept(s, SCAN_SIGN);
    /* Inf? */
    if (ss_accept(s, "iI") && ss_accept(s, "nN") && ss_accept(s, "fF"))
        return s->buf.len;
    const char *digits = SCAN_DECIMAL "_";
    const char *exp = "eEpP";
    if (ss_accept(s, "0") && ss_accept(s, "xX")) {
        digits = SCAN_HEX "_";
        exp = "pP";
    }
    /* digits? */
    while (ss_accept(s, digits)) {
    }
    /* decimal point? */
    if (ss_accept(s, ".")) {
        /* fraction? */
        while (ss_accept(s, digits)) {
        }
    }
    /* exponent? */
    if (ss_accept(s, exp)) {
        /* leading sign? */
        ss_accept(s, SCAN_SIGN);
        /* digits? */
        while (ss_accept(s, SCAN_DECIMAL "_")) {
        }
    }
    return s->buf.len;
}

/* Go's math.Ldexp, which libm's ldexp agrees with, but burrow does not link
 * libm. */
static double scan_ldexp(double frac, Int exp) {
    uint64_t x;
    memcpy(&x, &frac, sizeof x);
    if (frac == 0 || ((x >> 52) & 0x7ff) == 0x7ff) /* zero, NaN or an infinity */
        return frac;
    if ((frac < 0 ? -frac : frac) < 2.2250738585072014e-308) { /* SmallestNormal */
        frac *= (double)(UINT64_C(1) << 52);
        exp -= 52;
    }
    memcpy(&x, &frac, sizeof x);
    exp += (Int)((x >> 52) & 0x7ff) - 1023;
    if (exp < -1075) {
        double zero = 0;
        return frac < 0 ? -zero : zero; /* underflow */
    }
    if (exp > 1023) { /* overflow */
        uint64_t inf =
            frac < 0 ? UINT64_C(0xfff0000000000000) : UINT64_C(0x7ff0000000000000);
        double r;
        memcpy(&r, &inf, sizeof r);
        return r;
    }
    double m = 1;
    if (exp < -1022) { /* denormal */
        exp += 53;
        m = 1.0 / (double)(UINT64_C(1) << 53);
    }
    x &= ~(UINT64_C(0x7ff) << 52);
    x |= (uint64_t)(exp + 1023) << 52;
    double r;
    memcpy(&r, &x, sizeof r);
    return m * r;
}

/* An error from strconv with the whole token as the number, the way Go
 * rewrites NumError.Num when it parsed only part of it. */
BURROW_NORETURN static void ss_num_error(Error err, Str str) {
    const StrconvNumError *ne =
        (const StrconvNumError *)errors_as(err, TYPE_STRCONV_NUM_ERROR);
    if (ne != NULL) {
        StrconvNumError e = *ne;
        e.num = str;
        err = strconv_num_error_as_error(error_allocator(), &e);
    }
    ss_error(err);
}

static double ss_convert_float(Str str, Int n) {
    Int p = -1;
    bool has_x = false;
    for (Int i = 0; i < str.len; i++) {
        if (str.p[i] == 'p' && p < 0)
            p = i;
        if (str.p[i] == 'x' || str.p[i] == 'X')
            has_x = true;
    }
    Error err = BURROW_NO_ERROR;
    if (p >= 0 && !has_x) {
        /* Atof doesn't handle power-of-2 exponents, but they're easy to
         * evaluate. */
        double f = strconv_parse_float((Str){str.p, p}, n, &err);
        if (BURROW_FAILED(err))
            ss_num_error(err, str);
        Int m = strconv_atoi((Str){str.p + p + 1, str.len - p - 1}, &err);
        if (BURROW_FAILED(err))
            ss_num_error(err, str);
        return scan_ldexp(f, m);
    }
    double f = strconv_parse_float(str, n, &err);
    if (BURROW_FAILED(err))
        ss_error(err);
    return f;
}

static double ss_scan_float(Ss *s, Int n) {
    ss_skip_space(s);
    ss_not_eof(s);
    ss_float_token(s, 0);
    return ss_convert_float(ss_buf_str(s), n);
}

static Complex128 ss_scan_complex(Ss *s, Rune verb, Int n) {
    Complex128 c = {0, 0};
    ss_ok_verb(verb, SCAN_FLOAT_VERBS, "complex");
    ss_skip_space(s);
    ss_not_eof(s);
    /* complexTokens: the real part, then the sign, then the imaginary part,
     * kept next to each other in the buffer. */
    s->buf.len = 0;
    bool parens = ss_accept(s, "(");
    Int real_end = ss_float_token(s, 0);
    s->buf.len = real_end;
    if (!ss_accept(s, "+-"))
        ss_error_lit("syntax error scanning complex number");
    Int imag_end = ss_float_token(s, real_end + 1);
    if (!ss_accept(s, "i"))
        ss_error_lit("syntax error scanning complex number");
    if (parens && !ss_accept(s, ")"))
        ss_error_lit("syntax error scanning complex number");
    Str all = ss_buf_str(s);
    c.re = ss_convert_float((Str){all.p, real_end}, n / 2);
    c.im = ss_convert_float((Str){all.p + real_end, imag_end - real_end}, n / 2);
    return c;
}

static void ss_scan_percent(Ss *s) {
    ss_skip_space(s);
    ss_not_eof(s);
    if (!ss_accept(s, "%"))
        ss_error_lit("missing literal %");
}

static int scan_hex_digit(Rune d) {
    if (d >= '0' && d <= '9')
        return (int)(d - '0');
    if (d >= 'a' && d <= 'f')
        return 10 + (int)(d - 'a');
    if (d >= 'A' && d <= 'F')
        return 10 + (int)(d - 'A');
    return -1;
}

static bool ss_hex_byte(Ss *s, Byte *b) {
    Rune rune1 = ss_get_rune(s);
    if (rune1 == SCAN_EOF)
        return false;
    int value1 = scan_hex_digit(rune1);
    if (value1 < 0) {
        ss_unread_rune(s);
        return false;
    }
    int value2 = scan_hex_digit(ss_must_read_rune(s));
    if (value2 < 0)
        ss_error_lit("illegal hex digit");
    *b = (Byte)(value1 << 4 | value2);
    return true;
}

static Str ss_hex_string(Ss *s) {
    ss_not_eof(s);
    Byte b;
    while (ss_hex_byte(s, &b))
        fmt_buf_write_byte(&s->buf, b);
    if (s->buf.len == 0)
        ss_error_lit("no hex data for %x string");
    return ss_buf_str(s);
}

/* A quoted string, which comes back allocated in a when it had escapes and as
 * a view of the buffer when it did not. */
static Str ss_quoted_string(Ss *s) {
    ss_not_eof(s);
    Rune quote = ss_get_rune(s);
    switch (quote) {
    case '`':
        /* Back-quoted: Anything goes until EOF or back quote. */
        for (;;) {
            Rune r = ss_must_read_rune(s);
            if (r == quote)
                break;
            burrow__fmt_buf_write_rune(&s->buf, r);
        }
        return ss_buf_str(s);
    case '"': {
        /* Double-quoted: Include the quotes and let strconv.Unquote do the
         * backslash escapes. */
        fmt_buf_write_byte(&s->buf, '"');
        for (;;) {
            Rune r = ss_must_read_rune(s);
            burrow__fmt_buf_write_rune(&s->buf, r);
            if (r == '\\') {
                /* In a legal backslash escape, no matter how long, only the
                 * character immediately after the escape can itself be a
                 * backslash or quote. Thus we only need to protect the first
                 * character after the backslash. */
                burrow__fmt_buf_write_rune(&s->buf, ss_must_read_rune(s));
            } else if (r == '"') {
                break;
            }
        }
        Error err = BURROW_NO_ERROR;
        Str result = strconv_unquote(s->a, ss_buf_str(s), &err);
        if (BURROW_FAILED(err))
            ss_error(err);
        return result;
    }
    default:
        ss_error_lit("expected quoted string");
    }
}

static Str ss_convert_string(Ss *s, Rune verb) {
    ss_ok_verb(verb, "svqxX", "string");
    ss_skip_space(s);
    ss_not_eof(s);
    switch (verb) {
    case 'q':
        return ss_quoted_string(s);
    case 'x':
    case 'X':
        return ss_hex_string(s);
    default:
        /* %s and %v just return the next word */
        return ss_token(s, true, (RuneFunc){NULL, NULL});
    }
}

/* str's bytes in a, unless they are there already, which they are when they
 * are not in the token buffer. */
static Byte *ss_own(Ss *s, Str str) {
    Byte *base = s->buf.p;
    if (str.len == 0)
        return NULL;
    if (base != NULL && str.p >= base && str.p < base + s->buf.len) {
        Byte *p = (Byte *)mem_alloc_nozero(s->a, (size_t)str.len, 1);
        if (p == NULL)
            ss_error(burrow_err_out_of_memory);
        memcpy(p, str.p, (size_t)str.len);
        return p;
    }
    return (Byte *)(uintptr_t)str.p;
}

static void scan_store_int(void *p, uint32_t size, int64_t v) {
    switch (size) {
    case 1:
        *(int8_t *)p = (int8_t)v;
        break;
    case 2:
        *(int16_t *)p = (int16_t)v;
        break;
    case 4:
        *(int32_t *)p = (int32_t)v;
        break;
    default:
        *(int64_t *)p = v;
    }
}

static void scan_store_uint(void *p, uint32_t size, uint64_t v) {
    switch (size) {
    case 1:
        *(uint8_t *)p = (uint8_t)v;
        break;
    case 2:
        *(uint16_t *)p = (uint16_t)v;
        break;
    case 4:
        *(uint32_t *)p = (uint32_t)v;
        break;
    default:
        *(uint64_t *)p = v;
    }
}

/* A Scan method with Go's signature. */
static bool scan_is_scan_method(const Method *m) {
    return m != NULL && m->ftype != NULL && type_num_in(m->ftype) == 2 &&
           type_num_out(m->ftype) == 1 && type_in(m->ftype, 0) == TYPE_FMT_SCAN_STATE &&
           type_in(m->ftype, 1) != NULL && type_in(m->ftype, 1)->kind == KIND_INT32 &&
           type_out(m->ftype, 0) == TYPE_ERROR;
}

/* Defined with the rest of the ScanState further down. A forward declaration
 * of the table itself would be a tentative definition of a const object, which
 * MSVC rejects. */
static const FmtScanStateVT *ss_state_vtable(void);

BURROW_NORETURN static void ss_cant_scan(Any arg) {
    ss_error(fmt_errorf_v("can't scan type: *%T", arg));
}

/* Scans one value and stores it through arg. */
static void ss_scan_one(Ss *s, Rune verb, Any arg) {
    s->buf.len = 0;
    const Type *t = arg.t;
    void *p = arg.data;
    if (t == NULL || p == NULL)
        ss_error_lit("can't scan type: <nil>");

    /* If the operand has its own Scan method, use that. */
    if (t->nmethod > 0) {
        const Method *m = type_method_by_name(t, SCAN_LIT("Scan"));
        if (scan_is_scan_method(m)) {
            FmtScanState st = {ss_state_vtable(), s};
            Error err = BURROW_NO_ERROR;
            void *args[2] = {&st, &verb};
            void *rets[1] = {&err};
            method_call(m, p, args, rets);
            if (BURROW_FAILED(err)) {
                if (scan_err_eq(err, io_eof))
                    err = io_err_unexpected_eof;
                ss_error(err);
            }
            return;
        }
    }

    switch (t->kind) {
    case KIND_BOOL:
        *(bool *)p = ss_scan_bool(s, verb);
        break;
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        scan_store_int(p, t->size, ss_scan_int(s, verb, (Int)t->size * 8));
        break;
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        scan_store_uint(p, t->size, ss_scan_uint(s, verb, (Int)t->size * 8));
        break;
    case KIND_FLOAT32:
    case KIND_FLOAT64: {
        /* Go checks the verb for float32 and float64 themselves and not for
         * a type declared on top of one, so the same goes here. */
        if (t == TYPE_FLOAT32 && !ss_ok_verb(verb, SCAN_FLOAT_VERBS, "float32"))
            break;
        if (t == TYPE_FLOAT64 && !ss_ok_verb(verb, SCAN_FLOAT_VERBS, "float64"))
            break;
        if (t->kind == KIND_FLOAT32)
            *(float *)p = (float)ss_scan_float(s, 32);
        else
            *(double *)p = ss_scan_float(s, 64);
        break;
    }
    case KIND_COMPLEX64: {
        Complex128 c = ss_scan_complex(s, verb, 64);
        *(Complex64 *)p = (Complex64){(float)c.re, (float)c.im};
        break;
    }
    case KIND_COMPLEX128:
        *(Complex128 *)p = ss_scan_complex(s, verb, 128);
        break;
    case KIND_STRING: {
        Str str = ss_convert_string(s, verb);
        *(Str *)p = (Str){ss_own(s, str), str.len};
        break;
    }
    case KIND_SLICE: {
        const Type *elem = t->elem;
        if (elem == NULL || elem->kind != KIND_UINT8)
            ss_cant_scan(arg);
        Str str = ss_convert_string(s, verb);
        /* Go's []byte("") is empty and not nil, which slice_make gives
         * without allocating. */
        if (str.len == 0)
            *(Slice *)p = slice_make(s->a, elem, 0, 0);
        else
            *(Slice *)p = (Slice){ss_own(s, str), str.len, str.len, elem};
        break;
    }
    case KIND_INVALID:
    case KIND_ARRAY:
    case KIND_CHAN:
    case KIND_FUNC:
    case KIND_INTERFACE:
    case KIND_MAP:
    case KIND_POINTER:
    case KIND_STRUCT:
    case KIND_UNSAFE_POINTER:
    case KIND_MAX:
    default:
        ss_cant_scan(arg);
    }
}

/* What errorHandler does with a panic: a scan error or io_eof becomes the
 * error returned, and anything else carries on unwinding. */
static void ss_recover(Ss *s, Any e, Error *err) {
    if (e.t == &scan_error_desc) {
        *err = ((const ScanError *)e.data)->err;
        return;
    }
    if (e.t == TYPE_ERROR && scan_err_eq(*(const Error *)e.data, io_eof)) {
        *err = io_eof;
        return;
    }
    burrow__fmt_buf_free(&s->buf);
    panic(e);
}

static Int ss_do_scan(Ss *s, Slice args, Error *err) {
    volatile Int num_processed = 0;
    Error e = BURROW_NO_ERROR;
    BURROW_TRY {
        const Any *a = (const Any *)args.p;
        for (Int i = 0; i < args.len; i++) {
            ss_scan_one(s, 'v', a[i]);
            num_processed++;
        }
        /* Check for newline (or EOF) if required (Scanln etc.). */
        if (s->nl_is_end) {
            for (;;) {
                Rune r = ss_get_rune(s);
                if (r == '\n' || r == SCAN_EOF)
                    break;
                if (!scan_is_space(r)) {
                    ss_error_lit("expected newline");
                }
            }
        }
    }
    BURROW_CATCH(p) {
        ss_recover(s, p, &e);
    }
    BURROW_TRY_END;
    if (err != NULL)
        *err = e;
    return num_processed;
}

/* The number at s[start:end], if there is one, and where it stopped. */
static Int scan_parsenum(Str s, Int start, Int end, bool *isnum, Int *newi) {
    Int num = 0;
    *isnum = false;
    if (start >= end) {
        *newi = end;
        return 0;
    }
    Int i;
    for (i = start; i < end && '0' <= s.p[i] && s.p[i] <= '9'; i++) {
        if (num > 1000000 || num < -1000000) {
            *isnum = false;
            *newi = end;
            return 0; /* Overflow; crazy long number most likely. */
        }
        num = num * 10 + (Int)(s.p[i] - '0');
        *isnum = true;
    }
    *newi = i;
    return num;
}

static Rune scan_decode(Str format, Int i, Int *w) {
    return utf8_decode_rune_in_string((Str){format.p + i, format.len - i}, w);
}

/* Matches the input against the format up to the next verb, and returns how
 * much of the format it used, or -1 when the input does not match. Space in
 * the format matches any space in the input, except that newlines have to
 * match newlines. */
static Int ss_advance(Ss *s, Str format) {
    Int i = 0;
    while (i < format.len) {
        Int w;
        Rune fmtc = scan_decode(format, i, &w);

        /* Space processing. */
        if (scan_is_space(fmtc)) {
            Int newlines = 0;
            bool trailing_space = false;
            while (scan_is_space(fmtc) && i < format.len) {
                if (fmtc == '\n') {
                    newlines++;
                    trailing_space = false;
                } else {
                    trailing_space = true;
                }
                i += w;
                fmtc = scan_decode(format, i, &w);
            }
            for (Int j = 0; j < newlines; j++) {
                Rune inputc = ss_get_rune(s);
                while (scan_is_space(inputc) && inputc != '\n')
                    inputc = ss_get_rune(s);
                if (inputc != '\n' && inputc != SCAN_EOF)
                    ss_error_lit("newline in format does not match input");
            }
            if (trailing_space) {
                Rune inputc = ss_get_rune(s);
                if (newlines == 0) {
                    /* If the trailing space stood alone (did not follow a
                     * newline), need to find at least one space to consume. */
                    if (!scan_is_space(inputc) && inputc != SCAN_EOF)
                        ss_error_str(
                            SCAN_LIT("expected space in input to match format"));
                    if (inputc == '\n')
                        ss_error_str(
                            SCAN_LIT("newline in input does not match format"));
                }
                while (scan_is_space(inputc) && inputc != '\n')
                    inputc = ss_get_rune(s);
                if (inputc != SCAN_EOF)
                    ss_unread_rune(s);
            }
            continue;
        }

        /* Verbs. */
        if (fmtc == '%') {
            /* % at end of string is an error. */
            if (i + w == format.len)
                ss_error_lit("missing verb: % at end of format string");
            /* %% acts like a real percent */
            Int w2;
            Rune nextc = scan_decode(format, i + w, &w2);
            if (nextc != '%')
                return i;
            i += w; /* skip the first % */
        }

        /* Literals. */
        Rune inputc = ss_must_read_rune(s);
        if (fmtc != inputc) {
            ss_unread_rune(s);
            return -1;
        }
        i += w;
    }
    return i;
}

static Int ss_do_scanf(Ss *s, Str format, Slice args, Error *err) {
    volatile Int num_processed = 0;
    Error e = BURROW_NO_ERROR;
    BURROW_TRY {
        const Any *a = (const Any *)args.p;
        Int end = format.len - 1;
        /* We process one item per non-trivial format */
        for (Int i = 0; i <= end;) {
            /* A named local and not a compound literal, which gcc would warn
             * might be clobbered by the longjmp. */
            Str tail = {format.p + i, format.len - i};
            Int w = ss_advance(s, tail);
            if (w > 0) {
                i += w;
                continue;
            }
            /* Either we failed to advance, we have a percent character, or
             * we ran out of input. */
            if (format.p[i] != '%') {
                /* Can't advance format. Why not? */
                if (w < 0)
                    ss_error_lit("input does not match format");
                /* Otherwise at EOF; "too many operands" error handled below */
                break;
            }
            i++; /* % is one byte */

            /* do we have 20 (width)? */
            bool wid_present;
            s->max_wid = scan_parsenum(format, i, end, &wid_present, &i);
            if (!wid_present)
                s->max_wid = SCAN_HUGE_WID;

            Int cw;
            Rune c = scan_decode(format, i, &cw);
            i += cw;

            if (c != 'c')
                ss_skip_space(s);
            if (c == '%') {
                ss_scan_percent(s);
                continue; /* Do not consume an argument. */
            }
            s->arg_limit = s->limit;
            if (s->count + s->max_wid < s->arg_limit)
                s->arg_limit = s->count + s->max_wid;

            if (num_processed >= args.len) { /* out of operands */
                Str rest = {format.p + i - cw, format.len - i + cw};
                ss_error(fmt_errorf_v("too few operands for format '%%%s'", rest));
            }
            ss_scan_one(s, c, a[num_processed]);
            num_processed++;
            s->arg_limit = s->limit;
        }
        if (num_processed < args.len)
            ss_error_lit("too many operands");
    }
    BURROW_CATCH(p) {
        ss_recover(s, p, &e);
    }
    BURROW_TRY_END;
    if (err != NULL)
        *err = e;
    return num_processed;
}

/* ------------------------------------------------------------- the ScanState */

static Rune ss_state_read_rune(void *self, Int *size, Error *err) {
    return ss_read_rune((Ss *)self, size, err);
}

static Error ss_state_unread_rune(void *self) {
    ss_unread_rune((Ss *)self);
    return BURROW_NO_ERROR;
}

static void ss_state_skip_space(void *self) {
    ss_skip_space((Ss *)self);
}

/* Token recovers from a scan error and returns it, as Go's does, and lets
 * every other panic through. */
static Slice ss_state_token(void *self, bool skip_space, RuneFunc f, Error *err) {
    Ss *s = (Ss *)self;
    volatile Int n = 0;
    Error e = BURROW_NO_ERROR;
    BURROW_TRY {
        s->buf.len = 0;
        n = ss_token(s, skip_space, f).len;
    }
    BURROW_CATCH(p) {
        if (p.t != &scan_error_desc)
            panic(p);
        e = ((const ScanError *)p.data)->err;
    }
    BURROW_TRY_END;
    if (err != NULL)
        *err = e;
    if (BURROW_FAILED(e))
        return slice_nil(TYPE_BYTE);
    return slice_from(s->buf.p, n, n, TYPE_BYTE);
}

static Int ss_state_width(void *self, bool *ok) {
    Ss *s = (Ss *)self;
    if (s->max_wid == SCAN_HUGE_WID) {
        *ok = false;
        return 0;
    }
    *ok = true;
    return s->max_wid;
}

/* The Read method is only in ScanState so that ScanState satisfies
 * io.Reader. It will never be called when used as intended, so there is no
 * need to make it actually work. */
static Int ss_state_read(void *self, Slice buf, Error *err) {
    (void)self;
    (void)buf;
    if (err != NULL)
        *err =
            errors_new(error_allocator(),
                       SCAN_LIT("ScanState's Read should not be called. Use ReadRune"));
    return 0;
}

static const FmtScanStateVT ss_state_vt = {
    NULL,           ss_state_read_rune, ss_state_unread_rune, ss_state_skip_space,
    ss_state_token, ss_state_width,     ss_state_read,
};

static const FmtScanStateVT *ss_state_vtable(void) {
    return &ss_state_vt;
}

const Type burrow_type_FmtScanState = {
    BURROW_S_INIT("ScanState"),
    BURROW_S_INIT("fmt"),
    KIND_INTERFACE,
    (uint32_t)sizeof(FmtScanState),
    (uint16_t)_Alignof(FmtScanState),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Int scan_state_reader_read(void *self, Slice p, Error *err) {
    FmtScanState *st = (FmtScanState *)self;
    return st->vt->read(st->data, p, err);
}

static const IoReaderVT scan_state_reader_vt = {NULL, scan_state_reader_read};

IoReader fmt_scan_state_reader(FmtScanState *st) {
    return (IoReader){&scan_state_reader_vt, st};
}

/* ---------------------------------------------------------------- the entries */

static void ss_init(Ss *s, Alloc *a, Byte *stack, Int n, bool nl_is_space,
                    bool nl_is_end) {
    memset(s, 0, sizeof *s);
    s->rs.peek = -1;
    s->buf = (FmtBuf){stack, 0, n, false, false};
    s->nl_is_space = nl_is_space;
    s->nl_is_end = nl_is_end;
    s->limit = SCAN_HUGE_WID;
    s->arg_limit = SCAN_HUGE_WID;
    s->max_wid = SCAN_HUGE_WID;
    s->a = a;
}

static void ss_from_reader(Ss *s, IoReader r) {
    if (r.vt == &scan_state_reader_vt) {
        /* A Scan method scanning through its state: read runes from the
         * state, which is what Go does with any io.RuneScanner. */
        s->rs.kind = SCAN_SRC_STATE;
        s->rs.outer = *(const FmtScanState *)r.data;
        return;
    }
    s->rs.kind = SCAN_SRC_READER;
    s->rs.r = r;
}

typedef enum ScanMode { SCAN_PLAIN, SCAN_LN, SCAN_F } ScanMode;

static Int ss_run(Ss *s, ScanMode mode, Str format, Slice args, Error *err) {
    Int n =
        mode == SCAN_F ? ss_do_scanf(s, format, args, err) : ss_do_scan(s, args, err);
    burrow__fmt_buf_free(&s->buf);
    return n;
}

static Int scan_string(Alloc *a, Str str, ScanMode mode, Str format, Slice args,
                       Error *err) {
    Byte stack[SCAN_STACK];
    Ss s;
    ss_init(&s, a, stack, (Int)sizeof stack, mode == SCAN_PLAIN, mode == SCAN_LN);
    s.rs.kind = SCAN_SRC_STRING;
    s.rs.str = str;
    return ss_run(&s, mode, format, args, err);
}

static Int scan_reader(Alloc *a, IoReader r, ScanMode mode, Str format, Slice args,
                       Error *err) {
    Byte stack[SCAN_STACK];
    Ss s;
    ss_init(&s, a, stack, (Int)sizeof stack, mode == SCAN_PLAIN, mode == SCAN_LN);
    ss_from_reader(&s, r);
    return ss_run(&s, mode, format, args, err);
}

/* Standard input, a byte at a time through C's stdin. */
static Int scan_stdin_read(void *self, Slice p, Error *err) {
    (void)self;
    if (p.len == 0)
        return 0;
    int c = getc(stdin);
    if (c == EOF) {
        if (ferror(stdin))
            *err =
                errors_new(error_allocator(), SCAN_LIT("read /dev/stdin: read error"));
        else
            *err = io_eof;
        return 0;
    }
    ((Byte *)p.p)[0] = (Byte)c;
    return 1;
}

static const IoReaderVT scan_stdin_vt = {NULL, scan_stdin_read};

static IoReader scan_stdin(void) {
    return (IoReader){&scan_stdin_vt, NULL};
}

Int fmt_scan(Alloc *a, Slice args, Error *err) {
    return scan_reader(a, scan_stdin(), SCAN_PLAIN, BURROW_STR_EMPTY, args, err);
}

Int fmt_scanln(Alloc *a, Slice args, Error *err) {
    return scan_reader(a, scan_stdin(), SCAN_LN, BURROW_STR_EMPTY, args, err);
}

Int fmt_scanf(Alloc *a, Str format, Slice args, Error *err) {
    return scan_reader(a, scan_stdin(), SCAN_F, format, args, err);
}

Int fmt_sscan(Alloc *a, Str str, Slice args, Error *err) {
    return scan_string(a, str, SCAN_PLAIN, BURROW_STR_EMPTY, args, err);
}

Int fmt_sscanln(Alloc *a, Str str, Slice args, Error *err) {
    return scan_string(a, str, SCAN_LN, BURROW_STR_EMPTY, args, err);
}

Int fmt_sscanf(Alloc *a, Str str, Str format, Slice args, Error *err) {
    return scan_string(a, str, SCAN_F, format, args, err);
}

Int fmt_fscan(Alloc *a, IoReader r, Slice args, Error *err) {
    return scan_reader(a, r, SCAN_PLAIN, BURROW_STR_EMPTY, args, err);
}

Int fmt_fscanln(Alloc *a, IoReader r, Slice args, Error *err) {
    return scan_reader(a, r, SCAN_LN, BURROW_STR_EMPTY, args, err);
}

Int fmt_fscanf(Alloc *a, IoReader r, Str format, Slice args, Error *err) {
    return scan_reader(a, r, SCAN_F, format, args, err);
}

/* ---------------------------------------------------------------- operands */

Any burrow__scan_of_any(Any v, burrow__ScanBox *box) {
    (void)box;
    return v;
}

/* A Slice is a byte slice unless it says it holds something else, in which
 * case the scan says it cannot store into it. */
Any burrow__scan_of_slice(Slice *p, burrow__ScanBox *box) {
    box->t.kind = KIND_SLICE;
    box->t.size = (uint32_t)sizeof(Slice);
    box->t.align = (uint16_t)_Alignof(Slice);
    box->t.elem = p->elem != NULL ? p->elem : TYPE_BYTE;
    return (Any){&box->t, p};
}

/* T is a type and cannot be parenthesised. */
#define SCAN_OF(name, T, type)                                                         \
    /* NOLINTNEXTLINE(bugprone-macro-parentheses) */                                   \
    Any burrow__scan_of_##name(T *p, burrow__ScanBox *box) {                           \
        (void)box;                                                                     \
        return (Any){(type), p};                                                       \
    }

#define SCAN_SIGNED(T)                                                                 \
    (sizeof(T) == 8 ? TYPE_INT64 : sizeof(T) == 4 ? TYPE_INT32 : TYPE_INT16)
#define SCAN_UNSIGNED(T)                                                               \
    (sizeof(T) == 8 ? TYPE_UINT64 : sizeof(T) == 4 ? TYPE_UINT32 : TYPE_UINT16)

SCAN_OF(str, Str, TYPE_STRING)
SCAN_OF(bool, bool, TYPE_BOOL)
SCAN_OF(f32, float, TYPE_FLOAT32)
SCAN_OF(f64, double, TYPE_FLOAT64)
SCAN_OF(c64, Complex64, TYPE_COMPLEX64)
SCAN_OF(c128, Complex128, TYPE_COMPLEX128)
SCAN_OF(int, Int, TYPE_INT)
SCAN_OF(uint, Uint, TYPE_UINT)
SCAN_OF(cint, int, SCAN_SIGNED(int))
SCAN_OF(cuint, unsigned, SCAN_UNSIGNED(unsigned))
SCAN_OF(i8, int8_t, TYPE_INT8)
SCAN_OF(i16, int16_t, TYPE_INT16)
SCAN_OF(i32, int32_t, TYPE_INT32)
SCAN_OF(i64, int64_t, TYPE_INT64)
SCAN_OF(u8, uint8_t, TYPE_UINT8)
SCAN_OF(u16, uint16_t, TYPE_UINT16)
SCAN_OF(u32, uint32_t, TYPE_UINT32)
SCAN_OF(u64, uint64_t, TYPE_UINT64)
SCAN_OF(long, long, SCAN_SIGNED(long))
SCAN_OF(ulong, unsigned long, SCAN_UNSIGNED(unsigned long))
SCAN_OF(llong, long long, TYPE_INT64)
SCAN_OF(ullong, unsigned long long, TYPE_UINT64)
