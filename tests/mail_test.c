/* Derived from Go's src/net/mail/message_test.go. Go source: go1.27.1.
 *
 * Go compares addresses with reflect.DeepEqual, and here each name and address
 * is compared. BenchmarkConsumePhrase and BenchmarkConsumeComment call the
 * parser's internals, which are static here, and are left out.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/net/mail.h"

#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mime.h"
#include "burrow/net/textproto.h"
#include "burrow/strings.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

/* A Str from a literal that may have a NUL in it. */
#define L(s) {(const Byte *)(s), (Int)sizeof(s) - 1}

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

/* ------------------------------------------------------------------ parsing */

typedef struct HeaderField {
    Str key;
    Str values[2];
    Int n;
} HeaderField;

typedef struct ParseCase {
    Str in;
    HeaderField header[5];
    Int nheader;
    Str body;
} ParseCase;

static const ParseCase parse_tests[] = {
    {
        /* RFC 5322, Appendix A.1.1 */
        L("From: John Doe <jdoe@machine.example>\n"
          "To: Mary Smith <mary@example.net>\n"
          "Subject: Saying Hello\n"
          "Date: Fri, 21 Nov 1997 09:55:06 -0600\n"
          "Message-ID: <1234@local.machine.example>\n"
          "\n"
          "This is a message just to say hello.\n"
          "So, \"Hello\".\n"),
        {
            {L("From"), {L("John Doe <jdoe@machine.example>")}, 1},
            {L("To"), {L("Mary Smith <mary@example.net>")}, 1},
            {L("Subject"), {L("Saying Hello")}, 1},
            {L("Date"), {L("Fri, 21 Nov 1997 09:55:06 -0600")}, 1},
            {L("Message-Id"), {L("<1234@local.machine.example>")}, 1},
        },
        5,
        L("This is a message just to say hello.\nSo, \"Hello\".\n"),
    },
    {
        /* RFC 5965, Appendix B.1, a part of the multipart message (a header-only
         * sub message) */
        L("Feedback-Type: abuse\n"
          "User-Agent: SomeGenerator/1.0\n"
          "Version: 1\n"),
        {
            {L("Feedback-Type"), {L("abuse")}, 1},
            {L("User-Agent"), {L("SomeGenerator/1.0")}, 1},
            {L("Version"), {L("1")}, 1},
        },
        3,
        L(""),
    },
    {
        /* RFC 5322 permits any printable ASCII character, except colon, in a
         * header key. Issue #58862. */
        L("From: iant@golang.org\n"
          "Custom/Header: v\n"
          "\n"
          "Body\n"),
        {
            {L("From"), {L("iant@golang.org")}, 1},
            {L("Custom/Header"), {L("v")}, 1},
        },
        2,
        L("Body\n"),
    },
    {
        /* RFC 4155 mbox format. We've historically permitted this, so we
         * continue to permit it. Issue #60332. */
        L("From iant@golang.org Mon Jun 19 00:00:00 2023\n"
          "From: iant@golang.org\n"
          "\n"
          "Hello, gophers!\n"),
        {
            {L("From"), {L("iant@golang.org")}, 1},
            {L("From iant@golang.org Mon Jun 19 00"), {L("00:00 2023")}, 1},
        },
        2,
        L("Hello, gophers!\n"),
    },
};

static bool header_eq(MailHeader h, const ParseCase *tc) {
    if (map_len(h) != tc->nheader)
        return false;
    for (Int i = 0; i < tc->nheader; i++) {
        const HeaderField *f = &tc->header[i];
        Slice vs = textproto_mime_header_values(h, f->key);
        if (vs.len != f->n)
            return false;
        for (Int j = 0; j < f->n; j++)
            if (!str_eq(((const Str *)vs.p)[j], f->values[j]))
                return false;
    }
    return true;
}

static void TestParsing(TestingT *t) {
    ARENA_BEGIN;
    for (Int i = 0; i < (Int)(sizeof parse_tests / sizeof parse_tests[0]); i++) {
        const ParseCase *tc = &parse_tests[i];
        StringsReader *sr = strings_new_reader(a, tc->in);
        Error err;
        MailMessage *msg = mail_read_message(a, strings_reader_as_io_reader(sr), &err);
        if (msg == NULL) {
            testing_t_errorf_v(t, "test #%d: Failed parsing message: %v", i, err);
            continue;
        }
        if (!header_eq(msg->header, tc))
            testing_t_errorf_v(t, "test #%d: Incorrectly parsed message header.", i);
        Slice body = io_read_all(a, msg->body, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test #%d: Failed reading body: %v", i, err);
            mail_message_free(msg);
            continue;
        }
        Str got = {(const Byte *)body.p, body.len};
        if (!str_eq(got, tc->body))
            testing_t_errorf_v(t,
                               "test #%d: Incorrectly parsed message body.\nGot:\n%s\n"
                               "Want:\n%s",
                               i, got, tc->body);
        mail_message_free(msg);
    }
    ARENA_END;
}

/* -------------------------------------------------------------------- dates */

enum { ZONE_NONE, ZONE_MINUS6, ZONE_UTC, ZONE_GMT, ZONE_PLUS13 };

typedef struct DateCase {
    Str in;
    int year, month, day, hour, min, sec;
    int zone; /* ZONE_NONE for Go's zero Time */
    bool valid;
} DateCase;

static Time date_want(Alloc *a, const DateCase *tc) {
    TimeLocation *loc = time_utc_loc;
    switch (tc->zone) {
    case ZONE_NONE:
        return (Time){0};
    case ZONE_MINUS6:
        loc = time_fixed_zone(a, S(""), (Int)-6 * 60 * 60);
        break;
    case ZONE_GMT:
        loc = time_fixed_zone(a, S("GMT"), 0);
        break;
    case ZONE_PLUS13:
        loc = time_fixed_zone(a, S(""), (Int)13 * 60 * 60);
        break;
    default:
        break;
    }
    return time_date(tc->year, (TimeMonth)tc->month, tc->day, tc->hour, tc->min,
                     tc->sec, 0, loc);
}

static MailHeader date_header(Alloc *a, Str date) {
    MailHeader h = textproto_mime_header_make(a);
    textproto_mime_header_add(h, S("Date"), date);
    return h;
}

static void TestDateParsing(TestingT *t) {
    static const DateCase tests[] = {
        /* RFC 5322, Appendix A.1.1 */
        {L("Fri, 21 Nov 1997 09:55:06 -0600"), 1997, 11, 21, 9, 55, 6, ZONE_MINUS6,
         true},
        /* RFC 5322, Appendix A.6.2. Obsolete date. */
        {L("21 Nov 97 09:55:06 GMT"), 1997, 11, 21, 9, 55, 6, ZONE_GMT, true},
        /* Commonly found format not specified by RFC 5322. */
        {L("Fri, 21 Nov 1997 09:55:06 -0600 (MDT)"), 1997, 11, 21, 9, 55, 6,
         ZONE_MINUS6, true},
        {L("Thu, 20 Nov 1997 09:55:06 -0600 (MDT)"), 1997, 11, 20, 9, 55, 6,
         ZONE_MINUS6, true},
        {L("Thu, 20 Nov 1997 09:55:06 GMT (GMT)"), 1997, 11, 20, 9, 55, 6, ZONE_UTC,
         true},
        {L("Fri, 21 Nov 1997 09:55:06 +1300 (TOT)"), 1997, 11, 21, 9, 55, 6,
         ZONE_PLUS13, true},
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const DateCase *tc = &tests[i];
        Time want = date_want(a, tc);
        Error err;
        Time date = mail_header_date(date_header(a, tc->in), a, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Header(Date: %s).Date(): %v", tc->in, err);
        else if (!time_equal(date, want))
            testing_t_errorf_v(t, "Header(Date: %s).Date() = %s, want %s", tc->in,
                               time_string(date, a), time_string(want, a));

        date = mail_parse_date(a, tc->in, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "ParseDate(%s): %v", tc->in, err);
        else if (!time_equal(date, want))
            testing_t_errorf_v(t, "ParseDate(%s) = %s, want %s", tc->in,
                               time_string(date, a), time_string(want, a));
    }
    ARENA_END;
}

static void TestDateParsingCFWS(TestingT *t) {
    static const DateCase tests[] = {
        /* FWS-only. No date. */
        {L("   "), 1997, 11, 21, 9, 55, 6, ZONE_MINUS6, false},
        /* FWS is allowed before optional day of week. */
        {L("   Fri, 21 Nov 1997 09:55:06 -0600"), 1997, 11, 21, 9, 55, 6, ZONE_MINUS6,
         true},
        {L("21 Nov 1997 09:55:06 -0600"), 1997, 11, 21, 9, 55, 6, ZONE_MINUS6, true},
        /* missing , */
        {L("Fri 21 Nov 1997 09:55:06 -0600"), 1997, 11, 21, 9, 55, 6, ZONE_MINUS6,
         false},
        /* FWS is allowed before day of month but HTAB fails. */
        {L("Fri,        21 Nov 1997 09:55:06 -0600"), 1997, 11, 21, 9, 55, 6,
         ZONE_MINUS6, true},
        /* FWS is allowed before and after year but HTAB fails. */
        {L("Fri, 21 Nov       1997     09:55:06 -0600"), 1997, 11, 21, 9, 55, 6,
         ZONE_MINUS6, true},
        /* FWS is allowed before zone but HTAB is not handled. Obsolete timezone is
         * handled. */
        {L("Fri, 21 Nov 1997 09:55:06           CST"), 0, 0, 0, 0, 0, 0, ZONE_NONE,
         true},
        /* FWS is allowed after date and a CRLF is already replaced. */
        {L("Fri, 21 Nov 1997 09:55:06           CST (no leading FWS and a trailing "
           "CRLF) \r\n"),
         0, 0, 0, 0, 0, 0, ZONE_NONE, true},
        /* CFWS is a reduced set of US-ASCII where space and accentuated are
         * obsolete. No error. */
        {L("Fri, 21    Nov 1997    09:55:06 -0600 (MDT and non-US-ASCII signs éèç )"),
         1997, 11, 21, 9, 55, 6, ZONE_MINUS6, true},
        /* CFWS is allowed after zone including a nested comment. Trailing FWS is
         * allowed. */
        {L("Fri, 21 Nov 1997 09:55:06 -0600    \r\n (thisisa(valid)cfws)   \t "), 1997,
         11, 21, 9, 55, 6, ZONE_MINUS6, true},
        /* CRLF is incomplete and misplaced. */
        {L("Fri, 21 Nov 1997 \r 09:55:06 -0600    \r\n (thisisa(valid)cfws)   \t "),
         1997, 11, 21, 9, 55, 6, ZONE_MINUS6, false},
        /* CRLF is complete but misplaced. No error is returned. Should be false in
         * the strict interpretation of RFC 5322. */
        {L("Fri, 21 Nov 199\r\n7  09:55:06 -0600    \r\n (thisisa(valid)cfws)   \t "),
         1997, 11, 21, 9, 55, 6, ZONE_MINUS6, true},
        /* Invalid ASCII in date. */
        {L("Fri, 21 Nov 1997 ù 09:55:06 -0600    \r\n (thisisa(valid)cfws)   \t "),
         1997, 11, 21, 9, 55, 6, ZONE_MINUS6, false},
        /* CFWS chars () in date. */
        {L("Fri, 21 Nov () 1997 09:55:06 -0600    \r\n (thisisa(valid)cfws)   \t "),
         1997, 11, 21, 9, 55, 6, ZONE_MINUS6, false},
        /* Timezone is invalid but T is found in comment. */
        {L("Fri, 21 Nov 1997 09:55:06 -060    \r\n (Thisisa(valid)cfws)   \t "), 1997,
         11, 21, 9, 55, 6, ZONE_MINUS6, false},
        /* Date has no month. */
        {L("Fri, 21  1997 09:55:06 -0600"), 1997, 11, 21, 9, 55, 6, ZONE_MINUS6, false},
        /* Invalid month : OCT iso Oct */
        {L("Fri, 21 OCT 1997 09:55:06 CST"), 0, 0, 0, 0, 0, 0, ZONE_NONE, false},
        /* A too short time zone. */
        {L("Fri, 21 Nov 1997 09:55:06 -060"), 1997, 11, 21, 9, 55, 6, ZONE_MINUS6,
         false},
        /* A too short obsolete time zone. */
        {L("Fri, 21  1997 09:55:06 GT"), 1997, 11, 21, 9, 55, 6, ZONE_MINUS6, false},
        /* Ensure that the presence of "T" in the date doesn't trip out ParseDate,
         * as per issue 39260. */
        {L("Tue, 26 May 2020 14:04:40 GMT"), 2020, 5, 26, 14, 4, 40, ZONE_UTC, true},
        {L("Tue, 26 May 2020 14:04:40 UT"), 2020, 5, 26, 14, 4, 40, ZONE_UTC, true},
        {L("Thu, 21 May 2020 14:04:40 UT"), 2020, 5, 21, 14, 4, 40, ZONE_UTC, true},
        {L("Tue, 26 May 2020 14:04:40 XT"), 2020, 5, 26, 14, 4, 40, ZONE_UTC, false},
        {L("Thu, 21 May 2020 14:04:40 XT"), 2020, 5, 21, 14, 4, 40, ZONE_UTC, false},
        {L("Thu, 21 May 2020 14:04:40 UTC"), 2020, 5, 21, 14, 4, 40, ZONE_UTC, true},
        {L("Fri, 21 Nov 1997 09:55:06 GMT (GMT)"), 1997, 11, 21, 9, 55, 6, ZONE_UTC,
         true},
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const DateCase *tc = &tests[i];
        Time want = date_want(a, tc);
        Error err;
        Time date = mail_header_date(date_header(a, tc->in), a, &err);
        if (BURROW_FAILED(err) && tc->valid) {
            testing_t_errorf_v(t, "Header(Date: %s).Date(): %v", tc->in, err);
        } else if (BURROW_OK(err) && time_is_zero(want)) {
            /* OK. Used when exact result depends on the system's local
             * zoneinfo. */
        } else if (BURROW_OK(err) && !time_equal(date, want) && tc->valid) {
            testing_t_errorf_v(t, "Header(Date: %s).Date() = %s, want %s", tc->in,
                               time_string(date, a), time_string(want, a));
        } else if (BURROW_OK(err) && !tc->valid) {
            testing_t_errorf_v(t,
                               "Header(Date: %s).Date() did not return an error but %s",
                               tc->in, time_string(date, a));
        }

        date = mail_parse_date(a, tc->in, &err);
        if (BURROW_FAILED(err) && tc->valid) {
            testing_t_errorf_v(t, "ParseDate(%s): %v", tc->in, err);
        } else if (BURROW_OK(err) && time_is_zero(want)) {
            /* OK, as above. */
        } else if (BURROW_OK(err) && !tc->valid) {
            testing_t_errorf_v(t, "ParseDate(%s) did not return an error but %s",
                               tc->in, time_string(date, a));
        } else if (BURROW_OK(err) && tc->valid && !time_equal(date, want)) {
            testing_t_errorf_v(t, "ParseDate(%s) = %s, want %s", tc->in,
                               time_string(date, a), time_string(want, a));
        }
    }
    ARENA_END;
}

/* ---------------------------------------------------------- address errors */

typedef struct ErrCase {
    Str text;
    Str want;
} ErrCase;

static const ErrCase must_err_test_cases[] = {
    {L("=?iso-8859-2?Q?Bogl=E1rka_Tak=E1cs?= <unknown@gmail.com>"),
     L("charset not supported")},
    {L("a@gmail.com b@gmail.com"), L("expected single address")},
    {L("\xed\xa0\x80 <micro@example.net>"), L("invalid utf-8 in address")},
    {L("\"\xed\xa0\x80\" <half-surrogate@example.com>"),
     L("invalid utf-8 in quoted-string")},
    {L("\"\\\x80\" <escaped-invalid-unicode@example.net>"),
     L("invalid utf-8 in quoted-string")},
    {L("\"\x00\" <null@example.net>"), L("bad character in quoted-string")},
    {L("\"\\\x00\" <escaped-null@example.net>"), L("bad character in quoted-string")},
    {L("John Doe"), L("no angle-addr")},
    {L("<jdoe#machine.example>"), L("missing @ in addr-spec")},
    {L("John <middle> Doe <jdoe@machine.example>"), L("missing @ in addr-spec")},
    {L("cfws@example.com ("), L("misformatted parenthetical comment")},
    {L("empty group: ;"), L("empty group")},
    {L("root group: embed group: null@example.com;"), L("no angle-addr")},
    {L("group not closed: null@example.com"), L("expected comma")},
    {L("group: first@example.com, second@example.com;"),
     L("group with multiple addresses")},
    {L("john.doe"), L("missing '@' or angle-addr")},
    {L("john.doe@"), L("missing '@' or angle-addr")},
    {L("John Doe@foo.bar"), L("no angle-addr")},
    {L(" group: null@example.com; (asd"), L("misformatted parenthetical comment")},
    {L(" group: ; (asd"), L("misformatted parenthetical comment")},
    {L("(John) Doe <jdoe@machine.example>"), L("missing word in phrase:")},
    {L("<jdoe@[\xed\xa0\x80"
       "192.168.0.1]>"),
     L("invalid utf-8 in domain-literal")},
    {L("<jdoe@[[192.168.0.1]>"), L("bad character in domain-literal")},
    {L("<jdoe@[192.168.0.1>"), L("unclosed domain-literal")},
    {L("<jdoe@[256.0.0.1]>"), L("invalid IP address in domain-literal")},
    {L("<jdoe@[fd42::de:ad:be:ef]>"), L("invalid IP address in domain-literal")},
};

#define N_MUST_ERR ((Int)(sizeof must_err_test_cases / sizeof must_err_test_cases[0]))

static void custom_word_decoder(void *env, TestingT *t) {
    (void)env;
    ARENA_BEGIN;
    MimeWordDecoder dec = {0};
    MailAddressParser p = {&dec};
    for (Int i = 0; i < N_MUST_ERR; i++) {
        const ErrCase *tc = &must_err_test_cases[i];
        Error err;
        MailAddress *addr = mail_address_parser_parse(&p, a, tc->text, &err);
        if (addr != NULL || !strings_contains(error_text(err), tc->want))
            testing_t_errorf_v(t, "p.Parse(%q) #%d want %q, got %v", tc->text, i,
                               tc->want, err);
        mail_address_free(a, addr);
    }
    ARENA_END;
}

static void TestAddressParsingError(TestingT *t) {
    ARENA_BEGIN;
    for (Int i = 0; i < N_MUST_ERR; i++) {
        const ErrCase *tc = &must_err_test_cases[i];
        Error err;
        MailAddress *addr = mail_parse_address(a, tc->text, &err);
        if (addr != NULL || !strings_contains(error_text(err), tc->want))
            testing_t_errorf_v(t, "mail.ParseAddress(%q) #%d want %q, got %v", tc->text,
                               i, tc->want, err);
        mail_address_free(a, addr);
    }
    ARENA_END;
    testing_t_run(t, S("CustomWordDecoder"),
                  BURROW_FN(TestingTFunc, custom_word_decoder, NULL));
}

/* -------------------------------------------------------- address parsing */

typedef struct WantAddr {
    Str name;
    Str address;
} WantAddr;

typedef struct AddrCase {
    Str in;
    WantAddr exp[3];
    Int n;
} AddrCase;

static const AddrCase address_tests[] = {
    /* Bare address */
    {L("jdoe@machine.example"), {{L(""), L("jdoe@machine.example")}}, 1},
    /* RFC 5322, Appendix A.1.1 */
    {L("John Doe <jdoe@machine.example>"),
     {{L("John Doe"), L("jdoe@machine.example")}},
     1},
    /* RFC 5322, Appendix A.1.2 */
    {L("\"Joe Q. Public\" <john.q.public@example.com>"),
     {{L("Joe Q. Public"), L("john.q.public@example.com")}},
     1},
    /* Comment in display name */
    {L("John (middle) Doe <jdoe@machine.example>"),
     {{L("John Doe"), L("jdoe@machine.example")}},
     1},
    /* Display name is quoted string, so comment is not a comment */
    {L("\"John (middle) Doe\" <jdoe@machine.example>"),
     {{L("John (middle) Doe"), L("jdoe@machine.example")}},
     1},
    {L("\"John <middle> Doe\" <jdoe@machine.example>"),
     {{L("John <middle> Doe"), L("jdoe@machine.example")}},
     1},
    {L("Mary Smith <mary@x.test>, jdoe@example.org, Who? <one@y.test>"),
     {{L("Mary Smith"), L("mary@x.test")},
      {L(""), L("jdoe@example.org")},
      {L("Who?"), L("one@y.test")}},
     3},
    {L("<boss@nil.test>, \"Giant; \\\"Big\\\" Box\" <sysservices@example.net>"),
     {{L(""), L("boss@nil.test")},
      {L("Giant; \"Big\" Box"), L("sysservices@example.net")}},
     2},
    /* RFC 5322, Appendix A.6.1 */
    {L("Joe Q. Public <john.q.public@example.com>"),
     {{L("Joe Q. Public"), L("john.q.public@example.com")}},
     1},
    /* RFC 5322, Appendix A.1.3 */
    {L("group1: groupaddr1@example.com;"), {{L(""), L("groupaddr1@example.com")}}, 1},
    {L("empty group: ;"), {{L(""), L("")}}, 0},
    {L("A Group:Ed Jones <c@a.test>,joe@where.test,John <jdoe@one.test>;"),
     {{L("Ed Jones"), L("c@a.test")},
      {L(""), L("joe@where.test")},
      {L("John"), L("jdoe@one.test")}},
     3},
    /* RFC5322 4.4 obs-addr-list */
    {L(" , joe@where.test,,John <jdoe@one.test>,"),
     {{L(""), L("joe@where.test")}, {L("John"), L("jdoe@one.test")}},
     2},
    {L(" , joe@where.test,,John <jdoe@one.test>,,"),
     {{L(""), L("joe@where.test")}, {L("John"), L("jdoe@one.test")}},
     2},
    {L("Group1: <addr1@example.com>;, Group 2: addr2@example.com;, John "
       "<addr3@example.com>"),
     {{L(""), L("addr1@example.com")},
      {L(""), L("addr2@example.com")},
      {L("John"), L("addr3@example.com")}},
     3},
    /* RFC 2047 "Q"-encoded ISO-8859-1 address. */
    {L("=?iso-8859-1?q?J=F6rg_Doe?= <joerg@example.com>"),
     {{L("Jörg Doe"), L("joerg@example.com")}},
     1},
    /* RFC 2047 "Q"-encoded US-ASCII address. Dumb but legal. */
    {L("=?us-ascii?q?J=6Frg_Doe?= <joerg@example.com>"),
     {{L("Jorg Doe"), L("joerg@example.com")}},
     1},
    /* RFC 2047 "Q"-encoded UTF-8 address. */
    {L("=?utf-8?q?J=C3=B6rg_Doe?= <joerg@example.com>"),
     {{L("Jörg Doe"), L("joerg@example.com")}},
     1},
    /* RFC 2047 "Q"-encoded UTF-8 address with multiple encoded-words. */
    {L("=?utf-8?q?J=C3=B6rg?=  =?utf-8?q?Doe?= <joerg@example.com>"),
     {{L("JörgDoe"), L("joerg@example.com")}},
     1},
    /* RFC 2047, Section 8. */
    {L("=?ISO-8859-1?Q?Andr=E9?= Pirard <PIRARD@vm1.ulg.ac.be>"),
     {{L("André Pirard"), L("PIRARD@vm1.ulg.ac.be")}},
     1},
    /* Custom example of RFC 2047 "B"-encoded ISO-8859-1 address. */
    {L("=?ISO-8859-1?B?SvZyZw==?= <joerg@example.com>"),
     {{L("Jörg"), L("joerg@example.com")}},
     1},
    /* Custom example of RFC 2047 "B"-encoded UTF-8 address. */
    {L("=?UTF-8?B?SsO2cmc=?= <joerg@example.com>"),
     {{L("Jörg"), L("joerg@example.com")}},
     1},
    /* Custom example with "." in name. For issue 4938 */
    {L("Asem H. <noreply@example.com>"), {{L("Asem H."), L("noreply@example.com")}}, 1},
    /* RFC 6532 3.2.3, qtext /= UTF8-non-ascii */
    {L("\"Gø Pher\" <gopher@example.com>"),
     {{L("Gø Pher"), L("gopher@example.com")}},
     1},
    /* RFC 6532 3.2, atext /= UTF8-non-ascii */
    {L("µ <micro@example.com>"), {{L("µ"), L("micro@example.com")}}, 1},
    /* RFC 6532 3.2.2, local address parts allow UTF-8 */
    {L("Micro <µ@example.com>"), {{L("Micro"), L("µ@example.com")}}, 1},
    /* RFC 6532 3.2.4, domains parts allow UTF-8 */
    {L("Micro <micro@µ.example.com>"), {{L("Micro"), L("micro@µ.example.com")}}, 1},
    /* Issue 14866 */
    {L("\"\" <emptystring@example.com>"), {{L(""), L("emptystring@example.com")}}, 1},
    /* CFWS */
    {L("<cfws@example.com> (CFWS (cfws))  (another comment)"),
     {{L(""), L("cfws@example.com")}},
     1},
    {L("<cfws@example.com> ()  (another comment), <cfws2@example.com> (another)"),
     {{L(""), L("cfws@example.com")}, {L(""), L("cfws2@example.com")}},
     2},
    /* Comment as display name */
    {L("john@example.com (John Doe)"), {{L("John Doe"), L("john@example.com")}}, 1},
    /* Comment and display name */
    {L("John Doe <john@example.com> (Joey)"),
     {{L("John Doe"), L("john@example.com")}},
     1},
    /* Comment as display name, no space */
    {L("john@example.com(John Doe)"), {{L("John Doe"), L("john@example.com")}}, 1},
    /* Comment as display name, Q-encoded */
    {L("asjo@example.com (Adam =?utf-8?Q?Sj=C3=B8gren?=)"),
     {{L("Adam Sjøgren"), L("asjo@example.com")}},
     1},
    /* Comment as display name, Q-encoded and tab-separated */
    {L("asjo@example.com (Adam\t=?utf-8?Q?Sj=C3=B8gren?=)"),
     {{L("Adam Sjøgren"), L("asjo@example.com")}},
     1},
    /* Nested comment as display name, Q-encoded */
    {L("asjo@example.com (Adam =?utf-8?Q?Sj=C3=B8gren?= (Debian))"),
     {{L("Adam Sjøgren (Debian)"), L("asjo@example.com")}},
     1},
    /* Comment in group display name */
    {L("group (comment:): a@example.com, b@example.com;"),
     {{L(""), L("a@example.com")}, {L(""), L("b@example.com")}},
     2},
    {L("x(:\"):\"@a.example;(\"@b.example;"),
     {{L(""), L("@a.example;(@b.example")}},
     1},
    /* Domain-literal */
    {L("jdoe@[192.168.0.1]"), {{L(""), L("jdoe@[192.168.0.1]")}}, 1},
    {L("John Doe <jdoe@[192.168.0.1]>"), {{L("John Doe"), L("jdoe@[192.168.0.1]")}}, 1},
    /* IPv6 Domain-literal */
    {L("jdoe@[IPv6:fd42::dead:beef:1234]"),
     {{L(""), L("jdoe@[IPv6:fd42::dead:beef:1234]")}},
     1},
    {L("John Doe <jdoe@[IPv6:fd42::dead:beef:1234]>"),
     {{L("John Doe"), L("jdoe@[IPv6:fd42::dead:beef:1234]")}},
     1},
};

static bool addr_is(const MailAddress *got, const WantAddr *want) {
    return got != NULL && str_eq(got->name, want->name) &&
           str_eq(got->address, want->address);
}

/* Each case through p's Parse when it wants one address, and through
 * ParseList. */
static void check_addresses(TestingT *t, Alloc *a, const MailAddressParser *p,
                            const AddrCase *tests, Int n) {
    for (Int i = 0; i < n; i++) {
        const AddrCase *tc = &tests[i];
        Error err;
        if (tc->n == 1) {
            MailAddress *addr = mail_address_parser_parse(p, a, tc->in, &err);
            if (addr == NULL) {
                testing_t_errorf_v(t, "Failed parsing (single) %q: %v", tc->in, err);
                continue;
            }
            if (!addr_is(addr, &tc->exp[0]))
                testing_t_errorf_v(t, "Parse (single) of %q: got %q <%s>, want %q <%s>",
                                   tc->in, addr->name, addr->address, tc->exp[0].name,
                                   tc->exp[0].address);
            mail_address_free(a, addr);
        }

        Slice addrs = mail_address_parser_parse_list(p, a, tc->in, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Failed parsing (list) %q: %v", tc->in, err);
            continue;
        }
        if (addrs.len != tc->n) {
            testing_t_errorf_v(t, "Parse (list) of %q: got %d addresses, want %d",
                               tc->in, addrs.len, tc->n);
        } else {
            MailAddress **v = (MailAddress **)addrs.p;
            for (Int j = 0; j < addrs.len; j++)
                if (!addr_is(v[j], &tc->exp[j]))
                    testing_t_errorf_v(
                        t, "Parse (list) of %q: #%d got %q <%s>, want %q <%s>", tc->in,
                        j, v[j]->name, v[j]->address, tc->exp[j].name,
                        tc->exp[j].address);
        }
        mail_address_list_free(a, addrs);
    }
}

static void TestAddressParsing(TestingT *t) {
    ARENA_BEGIN;
    check_addresses(t, a, NULL, address_tests,
                    (Int)(sizeof address_tests / sizeof address_tests[0]));
    ARENA_END;
}

static const AddrCase parser_tests[] = {
    /* Bare address */
    {L("jdoe@machine.example"), {{L(""), L("jdoe@machine.example")}}, 1},
    /* RFC 5322, Appendix A.1.1 */
    {L("John Doe <jdoe@machine.example>"),
     {{L("John Doe"), L("jdoe@machine.example")}},
     1},
    /* RFC 5322, Appendix A.1.2 */
    {L("\"Joe Q. Public\" <john.q.public@example.com>"),
     {{L("Joe Q. Public"), L("john.q.public@example.com")}},
     1},
    {L("Mary Smith <mary@x.test>, jdoe@example.org, Who? <one@y.test>"),
     {{L("Mary Smith"), L("mary@x.test")},
      {L(""), L("jdoe@example.org")},
      {L("Who?"), L("one@y.test")}},
     3},
    {L("<boss@nil.test>, \"Giant; \\\"Big\\\" Box\" <sysservices@example.net>"),
     {{L(""), L("boss@nil.test")},
      {L("Giant; \"Big\" Box"), L("sysservices@example.net")}},
     2},
    /* RFC 2047 "Q"-encoded ISO-8859-1 address. */
    {L("=?iso-8859-1?q?J=F6rg_Doe?= <joerg@example.com>"),
     {{L("Jörg Doe"), L("joerg@example.com")}},
     1},
    /* RFC 2047 "Q"-encoded US-ASCII address. Dumb but legal. */
    {L("=?us-ascii?q?J=6Frg_Doe?= <joerg@example.com>"),
     {{L("Jorg Doe"), L("joerg@example.com")}},
     1},
    /* RFC 2047 "Q"-encoded ISO-8859-15 address. */
    {L("=?ISO-8859-15?Q?J=F6rg_Doe?= <joerg@example.com>"),
     {{L("Jörg Doe"), L("joerg@example.com")}},
     1},
    /* RFC 2047 "B"-encoded windows-1252 address. */
    {L("=?windows-1252?q?Andr=E9?= Pirard <PIRARD@vm1.ulg.ac.be>"),
     {{L("André Pirard"), L("PIRARD@vm1.ulg.ac.be")}},
     1},
    /* Custom example of RFC 2047 "B"-encoded ISO-8859-15 address. */
    {L("=?ISO-8859-15?B?SvZyZw==?= <joerg@example.com>"),
     {{L("Jörg"), L("joerg@example.com")}},
     1},
    /* Custom example of RFC 2047 "B"-encoded UTF-8 address. */
    {L("=?UTF-8?B?SsO2cmc=?= <joerg@example.com>"),
     {{L("Jörg"), L("joerg@example.com")}},
     1},
    /* Custom example with "." in name. For issue 4938 */
    {L("Asem H. <noreply@example.com>"), {{L("Asem H."), L("noreply@example.com")}}, 1},
    /* Domain-literal */
    {L("jdoe@[192.168.0.1]"), {{L(""), L("jdoe@[192.168.0.1]")}}, 1},
    {L("John Doe <jdoe@[192.168.0.1]>"), {{L("John Doe"), L("jdoe@[192.168.0.1]")}}, 1},
    /* IPv6 Domain-literal */
    {L("jdoe@[IPv6:fd42::dead:beef:1234]"),
     {{L(""), L("jdoe@[IPv6:fd42::dead:beef:1234]")}},
     1},
    {L("John Doe <jdoe@[IPv6:fd42::dead:beef:1234]>"),
     {{L("John Doe"), L("jdoe@[IPv6:fd42::dead:beef:1234]")}},
     1},
};

/* The test's CharsetReader: it reads everything and swaps the one byte each
 * charset needs for its UTF-8. */
typedef struct Latin {
    Alloc *a;
    StringsReader out;
} Latin;

static IoReader latin_reader(void *env, Str charset, IoReader input, Error *err) {
    Latin *l = (Latin *)env;
    Slice in = io_read_all(l->a, input, err);
    if (BURROW_FAILED(*err))
        return (IoReader){0};
    Str s = {(const Byte *)in.p, in.len};
    if (str_eq(charset, S("iso-8859-15")))
        s = strings_replace_all(l->a, s, S("\xf6"), S("ö"));
    else if (str_eq(charset, S("windows-1252")))
        s = strings_replace_all(l->a, s, S("\xe9"), S("é"));
    strings_reader_reset(&l->out, s);
    return strings_reader_as_io_reader(&l->out);
}

static void TestAddressParser(TestingT *t) {
    ARENA_BEGIN;
    Latin l = {0};
    l.a = a;
    MimeWordDecoder dec = {BURROW_FN(MimeCharsetReader, latin_reader, &l)};
    MailAddressParser ap = {&dec};
    check_addresses(t, a, &ap, parser_tests,
                    (Int)(sizeof parser_tests / sizeof parser_tests[0]));
    ARENA_END;
}

/* ----------------------------------------------------------- formatting */

static void TestAddressString(TestingT *t) {
    static const struct {
        WantAddr addr;
        Str exp;
    } tests[] = {
        {{L(""), L("bob@example.com")}, L("<bob@example.com>")},
        /* quoted local parts: RFC 5322, 3.4.1. and 3.2.4. */
        {{L(""), L("my@idiot@address@example.com")},
         L("<\"my@idiot@address\"@example.com>")},
        /* quoted local parts */
        {{L(""), L(" @example.com")}, L("<\" \"@example.com>")},
        {{L("Bob"), L("bob@example.com")}, L("\"Bob\" <bob@example.com>")},
        /* note the ö (o with an umlaut) */
        {{L("Böb"), L("bob@example.com")}, L("=?utf-8?q?B=C3=B6b?= <bob@example.com>")},
        {{L("Bob Jane"), L("bob@example.com")}, L("\"Bob Jane\" <bob@example.com>")},
        {{L("Böb Jacöb"), L("bob@example.com")},
         L("=?utf-8?q?B=C3=B6b_Jac=C3=B6b?= <bob@example.com>")},
        /* https://golang.org/issue/12098 */
        {{L("Rob"), L("")}, L("\"Rob\" <@>")},
        /* https://golang.org/issue/12098 */
        {{L("Rob"), L("@")}, L("\"Rob\" <@>")},
        {{L("Böb, Jacöb"), L("bob@example.com")},
         L("=?utf-8?b?QsO2YiwgSmFjw7Zi?= <bob@example.com>")},
        {{L("=??Q?x?="), L("hello@world.com")}, L("\"=??Q?x?=\" <hello@world.com>")},
        {{L("=?hello"), L("hello@world.com")}, L("\"=?hello\" <hello@world.com>")},
        {{L("world?="), L("hello@world.com")}, L("\"world?=\" <hello@world.com>")},
        /* should q-encode even for invalid utf-8. */
        {{L("\xed\xa0\x80"), L("invalid-utf8@example.net")},
         L("=?utf-8?q?=ED=A0=80?= <invalid-utf8@example.net>")},
        /* Domain-literal */
        {{L(""), L("bob@[192.168.0.1]")}, L("<bob@[192.168.0.1]>")},
        {{L("Bob"), L("bob@[192.168.0.1]")}, L("\"Bob\" <bob@[192.168.0.1]>")},
        /* IPv6 Domain-literal */
        {{L(""), L("bob@[IPv6:fd42::dead:beef:1234]")},
         L("<bob@[IPv6:fd42::dead:beef:1234]>")},
        {{L("Bob"), L("bob@[IPv6:fd42::dead:beef:1234]")},
         L("\"Bob\" <bob@[IPv6:fd42::dead:beef:1234]>")},
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        MailAddress addr = {tests[i].addr.name, tests[i].addr.address, NULL, 0};
        Str s = mail_address_string(&addr, a);
        if (!str_eq(s, tests[i].exp)) {
            testing_t_errorf_v(t, "Address{%q, %q}.String() = %s, want %s", addr.name,
                               addr.address, s, tests[i].exp);
            continue;
        }

        /* Check round-trip. */
        if (addr.address.len > 0 && !str_eq(addr.address, S("@"))) {
            Error err;
            MailAddress *got = mail_parse_address(a, tests[i].exp, &err);
            if (got == NULL) {
                testing_t_errorf_v(t, "ParseAddress(%s): %v", tests[i].exp, err);
                continue;
            }
            if (!addr_is(got, &tests[i].addr))
                testing_t_errorf_v(t, "ParseAddress(%s) = %q <%s>, want %q <%s>",
                                   tests[i].exp, got->name, got->address, addr.name,
                                   addr.address);
            mail_address_free(a, got);
        }
    }
    ARENA_END;
}

/* Check if all valid addresses can be parsed, formatted and parsed again */
static void TestAddressParsingAndFormatting(TestingT *t) {
    /* Should pass */
    static const Str tests[] = {
        L("<Bob@example.com>"),
        L("<bob.bob@example.com>"),
        L("<\".bob\"@example.com>"),
        L("<\" \"@example.com>"),
        L("<some.mail-with-dash@example.com>"),
        L("<\"dot.and space\"@example.com>"),
        L("<\"very.unusual.@.unusual.com\"@example.com>"),
        L("<admin@mailserver1>"),
        L("<postmaster@localhost>"),
        L("<#!$%&'*+-/=?^_`{}|~@example.org>"),
        /* escaped quotes */
        L("<\"very.(),:;<>[]\\\".VERY.\\\"very@\\\\ "
          "\\\"very\\\".unusual\"@strange.example."
          "com>"),
        /* escaped backslashes */
        L("<\"()<>[]:,;@\\\\\\\"!#$%&'*+-/=?^_{}| ~.a\"@example.org>"),
        L("<\"Abc\\\\@def\"@example.com>"),
        L("<\"Joe\\\\Blow\"@example.com>"),
        L("<test1/test2=test3@example.com>"),
        L("<def!xyz%abc@example.com>"),
        L("<_somename@example.com>"),
        L("<joe@uk>"),
        L("<~@example.com>"),
        L("<\"...\"@test.com>"),
        L("<\"john..doe\"@example.com>"),
        L("<\"john.doe.\"@example.com>"),
        L("<\".john.doe\"@example.com>"),
        L("<\".\"@example.com>"),
        L("<\"..\"@example.com>"),
        L("<\"0:\"@0>"),
        L("<Bob@[192.168.0.1]>"),
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        MailAddress *addr = mail_parse_address(a, tests[i], &err);
        if (addr == NULL) {
            testing_t_errorf_v(t, "Couldn't parse address %s: %v", tests[i], err);
            continue;
        }
        Str str = mail_address_string(addr, a);
        mail_address_free(a, addr);
        addr = mail_parse_address(a, str, &err);
        if (addr == NULL) {
            testing_t_errorf_v(t, "ParseAddr(%q) error: %v", tests[i], err);
            continue;
        }
        Str again = mail_address_string(addr, a);
        if (!str_eq(again, tests[i]))
            testing_t_errorf_v(t, "String() round-trip = %q; want %q", again, tests[i]);
        mail_address_free(a, addr);
    }

    /* Should fail */
    static const Str bad_tests[] = {
        L("<Abc.example.com>"),
        L("<A@b@c@example.com>"),
        L("<a\"b(c)d,e:f;g<h>i[j\\k]l@example.com>"),
        L("<just\"not\"right@example.com>"),
        L("<this is\"not\\allowed@example.com>"),
        L("<this\\ still\\\"not\\\\allowed@example.com>"),
        L("<john..doe@example.com>"),
        L("<john.doe@example..com>"),
        L("<john.doe@example..com>"),
        L("<john.doe.@example.com>"),
        L("<john.doe.@.example.com>"),
        L("<.john.doe@example.com>"),
        L("<@example.com>"),
        L("<.@example.com>"),
        L("<test@.>"),
        L("< @example.com>"),
        L("<\"\"test\"\"blah\"\"@example.com>"),
        L("<\"\"@0>"),
    };
    for (size_t i = 0; i < sizeof bad_tests / sizeof bad_tests[0]; i++) {
        Error err;
        MailAddress *addr = mail_parse_address(a, bad_tests[i], &err);
        if (addr != NULL || BURROW_OK(err))
            testing_t_errorf_v(t, "Should have failed to parse address: %s",
                               bad_tests[i]);
        mail_address_free(a, addr);
    }
    ARENA_END;
}

static void TestAddressFormattingAndParsing(TestingT *t) {
    static const WantAddr tests[] = {
        {L("@lïce"), L("alice@example.com")},
        {L("Böb O'Connor"), L("bob@example.com")},
        {L("???"), L("bob@example.com")},
        {L("Böb ???"), L("bob@example.com")},
        {L("Böb (Jacöb)"), L("bob@example.com")},
        {L("à#$%&'(),.:;<>@[]^`{|}~'"), L("bob@example.com")},
        /* https://golang.org/issue/11292 */
        {L("\"\\\x1f,\""), L("0@0")},
        /* https://golang.org/issue/12782 */
        {L("naé, mée"), L("test.mail@gmail.com")},
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        MailAddress addr = {tests[i].name, tests[i].address, NULL, 0};
        Str s = mail_address_string(&addr, a);
        Error err;
        MailAddress *parsed = mail_parse_address(a, s, &err);
        if (parsed == NULL) {
            testing_t_errorf_v(t, "test #%d: ParseAddr(%q) error: %v", (Int)i, s, err);
            continue;
        }
        if (!str_eq(parsed->name, tests[i].name))
            testing_t_errorf_v(t, "test #%d: Parsed name = %q; want %q", (Int)i,
                               parsed->name, tests[i].name);
        if (!str_eq(parsed->address, tests[i].address))
            testing_t_errorf_v(t, "test #%d: Parsed address = %q; want %q", (Int)i,
                               parsed->address, tests[i].address);
        mail_address_free(a, parsed);
    }
    ARENA_END;
}

static void TestEmptyAddress(TestingT *t) {
    ARENA_BEGIN;
    Error err;
    MailAddress *parsed = mail_parse_address(a, S(""), &err);
    if (parsed != NULL || BURROW_OK(err))
        testing_t_errorf_v(t, "ParseAddress(\"\") = %v, want nil, error", err);
    mail_address_free(a, parsed);
    static const Str lists[] = {L(""), L(","), L("a@b c@d")};
    for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++) {
        Slice list = mail_parse_address_list(a, lists[i], &err);
        if (list.len > 0 || BURROW_OK(err))
            testing_t_errorf_v(
                t, "ParseAddressList(%q) = %d addresses, %v, want nil, error", lists[i],
                list.len, err);
        mail_address_list_free(a, list);
    }
    ARENA_END;
}

/* ------------------------------------------------------------- burrow only */

/* Header.AddressList on a field that is there and on one that is not. */
static void TestHeaderAddressList(TestingT *t) {
    ARENA_BEGIN;
    MailHeader h = textproto_mime_header_make(a);
    textproto_mime_header_add(h, S("to"), S("Ann <ann@example.com>, bob@example.com"));
    Error err;
    Slice list = mail_header_address_list(h, a, S("To"), &err);
    MailAddress **v = (MailAddress **)list.p;
    if (BURROW_FAILED(err) || list.len != 2 || !str_eq(v[0]->name, S("Ann")) ||
        !str_eq(v[1]->address, S("bob@example.com")))
        testing_t_errorf_v(t, "AddressList(To) = %d addresses, %v", list.len, err);
    mail_address_list_free(a, list);

    list = mail_header_address_list(h, a, S("Cc"), &err);
    if (list.len != 0 || !errors_is(err, mail_err_header_not_present))
        testing_t_errorf_v(t, "AddressList(Cc) = %d addresses, %v, want %v", list.len,
                           err, mail_err_header_not_present);
    Time date = mail_header_date(h, a, &err);
    if (!time_is_zero(date) || !errors_is(err, mail_err_header_not_present))
        testing_t_errorf_v(t, "Date() = %s, %v, want %v", time_string(date, a), err,
                           mail_err_header_not_present);
    if (!str_eq(mail_header_get(h, S("TO")),
                S("Ann <ann@example.com>, bob@example.com")))
        testing_t_errorf_v(t, "Get(TO) = %q", mail_header_get(h, S("TO")));
    ARENA_END;
}

/* The errors ReadMessage gives for a header it cannot read, which Go's tests
 * do not reach. */
static void TestReadMessageErrors(TestingT *t) {
    static const struct {
        Str in;
        Str want;
    } tests[] = {
        {L(" From: a@b\n\nbody\n"), L("malformed initial line: \" From: a@b\"")},
        {L("From a@b\nno colon here\n\n"),
         L("malformed header line: \"no colon here\"")},
        {L(""), L("EOF")},
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        StringsReader *sr = strings_new_reader(a, tests[i].in);
        Error err;
        MailMessage *msg = mail_read_message(a, strings_reader_as_io_reader(sr), &err);
        if (msg != NULL || !str_eq(error_text(err), tests[i].want))
            testing_t_errorf_v(t, "ReadMessage(%q) = %v, want %s", tests[i].in, err,
                               tests[i].want);
        mail_message_free(msg);
    }
    ARENA_END;
}

#define TESTS(X)                                                                       \
    X(TestParsing)                                                                     \
    X(TestDateParsing)                                                                 \
    X(TestDateParsingCFWS)                                                             \
    X(TestAddressParsingError)                                                         \
    X(TestAddressParsing)                                                              \
    X(TestAddressParser)                                                               \
    X(TestAddressString)                                                               \
    X(TestAddressParsingAndFormatting)                                                 \
    X(TestAddressFormattingAndParsing)                                                 \
    X(TestEmptyAddress)                                                                \
    X(TestHeaderAddressList)                                                           \
    X(TestReadMessageErrors)
TESTING_MAIN(TESTS)
