/* Derived from Go's src/encoding/csv/reader_test.go, writer_test.go and
 * fuzz_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/csv.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

#include "check.h"

#include <string.h>

#define S BURROW_S

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

#define CHECK_STR(got, want)                                                           \
    do {                                                                               \
        Str got_ = (got), want_ = (want);                                              \
        if (!str_eq(got_, want_))                                                      \
            testing_t_errorf_v(t, "%s = %q, want %q", #got, got_, want_);              \
    } while (0)

static Str cstr(const char *s) {
    return str_from_cstr(s);
}

static IoReader strings_io(Str s) {
    StringsReader *r = strings_new_reader(a, s);
    return strings_reader_as_io_reader(r);
}

/* A record written out the way %q prints a []string, for messages. */
static Str show(Slice rec) {
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_byte(&b, '[');
    for (Int i = 0; i < rec.len; i++) {
        if (i > 0)
            strings_builder_write_byte(&b, ' ');
        strings_builder_write_string(&b, strconv_quote(a, BURROW_AT(Str, rec, i)),
                                     NULL);
    }
    strings_builder_write_byte(&b, ']');
    return strings_builder_string(&b);
}

/* ------------------------------------------------------------------ reading */

/* The fields of one expected record, ended by END. */
static const char END[] = "\x01end of record\x01";
#define MAX_FIELDS 8
typedef struct Rec {
    const char *f[MAX_FIELDS];
} Rec;
#define R(...)                                                                         \
    {                                                                                  \
        {                                                                              \
            __VA_ARGS__, END                                                           \
        }                                                                              \
    }

enum { E_NONE, E_BARE_QUOTE, E_QUOTE, E_FIELD_COUNT, E_INVALID_DELIM };

typedef struct ReadTest {
    const char *name;
    const char *input;
    const Rec *output; /* ended by a Rec whose first field is NULL */
    Int fields_per_record;
    Rune comma;
    Rune comment;
    int errors[4];
    bool use_fields_per_record; /* false means fields_per_record is -1 */
    bool lazy_quotes;
    bool trim_leading_space;
    bool reuse_record;
} ReadTest;

#define OUT(...) ((const Rec[]){__VA_ARGS__, {{NULL}}})

/* In these tests, the §, ¶ and ∑ characters in input are used to denote the
 * start of a field, a record boundary and the position of an error
 * respectively. They are removed before parsing and are used to verify the
 * position information reported by csv_reader_field_pos. */
static const ReadTest read_tests[] = {
    {.name = "Simple", .input = "§a,§b,§c\n", .output = OUT(R("a", "b", "c"))},
    {.name = "CRLF",
     .input = "§a,§b\r\n¶§c,§d\r\n",
     .output = OUT(R("a", "b"), R("c", "d"))},
    {.name = "BareCR", .input = "§a,§b\rc,§d\r\n", .output = OUT(R("a", "b\rc", "d"))},
    {.name = "RFC4180test",
     .input = "§#field1,§field2,§field3\n"
              "¶§\"aaa\",§\"bb\n"
              "b\",§\"ccc\"\n"
              "¶§\"a,a\",§\"b\"\"bb\",§\"ccc\"\n"
              "¶§zzz,§yyy,§xxx\n",
     .output = OUT(R("#field1", "field2", "field3"), R("aaa", "bb\nb", "ccc"),
                   R("a,a", "b\"bb", "ccc"), R("zzz", "yyy", "xxx")),
     .use_fields_per_record = true,
     .fields_per_record = 0},
    {.name = "NoEOLTest", .input = "§a,§b,§c", .output = OUT(R("a", "b", "c"))},
    {.name = "Semicolon",
     .input = "§a;§b;§c\n",
     .output = OUT(R("a", "b", "c")),
     .comma = ';'},
    {.name = "MultiLine",
     .input = "§\"two\n"
              "line\",§\"one line\",§\"three\n"
              "line\n"
              "field\"",
     .output = OUT(R("two\nline", "one line", "three\nline\nfield"))},
    {.name = "BlankLine",
     .input = "§a,§b,§c\n\n¶§d,§e,§f\n\n",
     .output = OUT(R("a", "b", "c"), R("d", "e", "f"))},
    {.name = "BlankLineFieldCount",
     .input = "§a,§b,§c\n\n¶§d,§e,§f\n\n",
     .output = OUT(R("a", "b", "c"), R("d", "e", "f")),
     .use_fields_per_record = true,
     .fields_per_record = 0},
    {.name = "TrimSpace",
     .input = " §a,  §b,   §c\n",
     .output = OUT(R("a", "b", "c")),
     .trim_leading_space = true},
    {.name = "LeadingSpace",
     .input = "§ a,§  b,§   c\n",
     .output = OUT(R(" a", "  b", "   c"))},
    {.name = "Comment",
     .input = "#1,2,3\n§a,§b,§c\n#comment",
     .output = OUT(R("a", "b", "c")),
     .comment = '#'},
    {.name = "NoComment",
     .input = "§#1,§2,§3\n¶§a,§b,§c",
     .output = OUT(R("#1", "2", "3"), R("a", "b", "c"))},
    {.name = "LazyQuotes",
     .input = "§a \"word\",§\"1\"2\",§a\",§\"b",
     .output = OUT(R("a \"word\"", "1\"2", "a\"", "b")),
     .lazy_quotes = true},
    {.name = "BareQuotes",
     .input = "§a \"word\",§\"1\"2\",§a\"",
     .output = OUT(R("a \"word\"", "1\"2", "a\"")),
     .lazy_quotes = true},
    {.name = "BareDoubleQuotes",
     .input = "§a\"\"b,§c",
     .output = OUT(R("a\"\"b", "c")),
     .lazy_quotes = true},
    {.name = "BadDoubleQuotes", .input = "§a∑\"\"b,c", .errors = {E_BARE_QUOTE}},
    {.name = "TrimQuote",
     .input = " §\"a\",§\" b\",§c",
     .output = OUT(R("a", " b", "c")),
     .trim_leading_space = true},
    {.name = "BadBareQuote", .input = "§a ∑\"word\",\"b\"", .errors = {E_BARE_QUOTE}},
    {.name = "BadTrailingQuote", .input = "§\"a word\",b∑\"", .errors = {E_BARE_QUOTE}},
    {.name = "ExtraneousQuote", .input = "§\"a ∑\"word\",\"b\"", .errors = {E_QUOTE}},
    {.name = "BadFieldCount",
     .input = "§a,§b,§c\n¶∑§d,§e",
     .errors = {E_NONE, E_FIELD_COUNT},
     .output = OUT(R("a", "b", "c"), R("d", "e")),
     .use_fields_per_record = true,
     .fields_per_record = 0},
    {.name = "BadFieldCountMultiple",
     .input = "§a,§b,§c\n¶∑§d,§e\n¶∑§f",
     .errors = {E_NONE, E_FIELD_COUNT, E_FIELD_COUNT},
     .output = OUT(R("a", "b", "c"), R("d", "e"), R("f")),
     .use_fields_per_record = true,
     .fields_per_record = 0},
    {.name = "BadFieldCount1",
     .input = "§∑a,§b,§c",
     .errors = {E_FIELD_COUNT},
     .output = OUT(R("a", "b", "c")),
     .use_fields_per_record = true,
     .fields_per_record = 2},
    {.name = "FieldCount",
     .input = "§a,§b,§c\n¶§d,§e",
     .output = OUT(R("a", "b", "c"), R("d", "e"))},
    {.name = "TrailingCommaEOF",
     .input = "§a,§b,§c,§",
     .output = OUT(R("a", "b", "c", ""))},
    {.name = "TrailingCommaEOL",
     .input = "§a,§b,§c,§\n",
     .output = OUT(R("a", "b", "c", ""))},
    {.name = "TrailingCommaSpaceEOF",
     .input = "§a,§b,§c, §",
     .output = OUT(R("a", "b", "c", "")),
     .trim_leading_space = true},
    {.name = "TrailingCommaSpaceEOL",
     .input = "§a,§b,§c, §\n",
     .output = OUT(R("a", "b", "c", "")),
     .trim_leading_space = true},
    {.name = "TrailingCommaLine3",
     .input = "§a,§b,§c\n¶§d,§e,§f\n¶§g,§hi,§",
     .output = OUT(R("a", "b", "c"), R("d", "e", "f"), R("g", "hi", "")),
     .trim_leading_space = true},
    {.name = "NotTrailingComma3",
     .input = "§a,§b,§c,§ \n",
     .output = OUT(R("a", "b", "c", " "))},
    {.name = "CommaFieldTest",
     .input = "§x,§y,§z,§w\n"
              "¶§x,§y,§z,§\n"
              "¶§x,§y,§,§\n"
              "¶§x,§,§,§\n"
              "¶§,§,§,§\n"
              "¶§\"x\",§\"y\",§\"z\",§\"w\"\n"
              "¶§\"x\",§\"y\",§\"z\",§\"\"\n"
              "¶§\"x\",§\"y\",§\"\",§\"\"\n"
              "¶§\"x\",§\"\",§\"\",§\"\"\n"
              "¶§\"\",§\"\",§\"\",§\"\"\n",
     .output = OUT(R("x", "y", "z", "w"), R("x", "y", "z", ""), R("x", "y", "", ""),
                   R("x", "", "", ""), R("", "", "", ""), R("x", "y", "z", "w"),
                   R("x", "y", "z", ""), R("x", "y", "", ""), R("x", "", "", ""),
                   R("", "", "", ""))},
    {.name = "TrailingCommaIneffective1",
     .input = "§a,§b,§\n¶§c,§d,§e",
     .output = OUT(R("a", "b", ""), R("c", "d", "e")),
     .trim_leading_space = true},
    {.name = "ReadAllReuseRecord",
     .input = "§a,§b\n¶§c,§d",
     .output = OUT(R("a", "b"), R("c", "d")),
     .reuse_record = true},
    {.name = "StartLine1", .input = "§a,\"b\nc∑\"d,e", .errors = {E_QUOTE}},
    {.name = "StartLine2",
     .input = "§a,§b\n¶§\"d\n\n,e∑",
     .errors = {E_NONE, E_QUOTE},
     .output = OUT(R("a", "b"))},
    {.name = "CRLFInQuotedField",
     .input = "§A,§\"Hello\r\nHi\",§B\r\n",
     .output = OUT(R("A", "Hello\nHi", "B"))},
    {.name = "BinaryBlobField",
     .input = "§x09\x41\xb4\x1c,§aktau",
     .output = OUT(R("x09A\xb4\x1c", "aktau"))},
    {.name = "TrailingCR",
     .input = "§field1,§field2\r",
     .output = OUT(R("field1", "field2"))},
    {.name = "QuotedTrailingCR", .input = "§\"field\"\r", .output = OUT(R("field"))},
    {.name = "QuotedTrailingCRCR", .input = "§\"field∑\"\r\r", .errors = {E_QUOTE}},
    {.name = "FieldCR", .input = "§field\rfield\r", .output = OUT(R("field\rfield"))},
    {.name = "FieldCRCR",
     .input = "§field\r\rfield\r\r",
     .output = OUT(R("field\r\rfield\r"))},
    {.name = "FieldCRCRLF",
     .input = "§field\r\r\n¶§field\r\r\n",
     .output = OUT(R("field\r"), R("field\r"))},
    {.name = "FieldCRCRLFCR",
     .input = "§field\r\r\n¶§\rfield\r\r\n\r",
     .output = OUT(R("field\r"), R("\rfield\r"))},
    {.name = "FieldCRCRLFCRCR",
     .input = "§field\r\r\n¶§\r\rfield\r\r\n¶§\r\r",
     .output = OUT(R("field\r"), R("\r\rfield\r"), R("\r"))},
    {.name = "MultiFieldCRCRLFCRCR",
     .input = "§field1,§field2\r\r\n¶§\r\rfield1,§field2\r\r\n¶§\r\r,§",
     .output =
         OUT(R("field1", "field2\r"), R("\r\rfield1", "field2\r"), R("\r\r", ""))},
    {.name = "NonASCIICommaAndComment",
     .input = "§a£§b,c£ \t§d,e\n€ comment\n",
     .output = OUT(R("a", "b,c", "d,e")),
     .trim_leading_space = true,
     .comma = 0xa3,      /* £ */
     .comment = 0x20ac}, /* € */
    {.name = "NonASCIICommaAndCommentWithQuotes",
     .input = "§a€§\"  b,\"€§ c\nλ comment\n",
     .output = OUT(R("a", "  b,", " c")),
     .comma = 0x20ac,   /* € */
     .comment = 0x3bb}, /* λ */
    /* λ and θ start with the same byte. This tests that the parser doesn't
     * confuse such characters. */
    {.name = "NonASCIICommaConfusion",
     .input = "§\"abθcd\"λ§efθgh",
     .output = OUT(R("abθcd", "efθgh")),
     .comma = 0x3bb,     /* λ */
     .comment = 0x20ac}, /* € */
    {.name = "NonASCIICommentConfusion",
     .input = "§λ\n¶§λ\nθ\n¶§λ\n",
     .output = OUT(R("λ"), R("λ"), R("λ")),
     .comment = 0x3b8}, /* θ */
    {.name = "QuotedFieldMultipleLF",
     .input = "§\"\n\n\n\n\"",
     .output = OUT(R("\n\n\n\n"))},
    {.name = "MultipleCRLF", .input = "\r\n\r\n\r\n\r\n"},
    /* The implementation may read each line in several chunks if it doesn't
     * fit entirely in the read buffer, so we should test the code to handle
     * that condition. The input and output are built in TestRead. */
    {.name = "HugeLines", .comment = '#'},
    {.name = "QuoteWithTrailingCRLF",
     .input = "§\"foo∑\"bar\"\r\n",
     .errors = {E_QUOTE}},
    {.name = "LazyQuoteWithTrailingCRLF",
     .input = "§\"foo\"bar\"\r\n",
     .output = OUT(R("foo\"bar")),
     .lazy_quotes = true},
    {.name = "DoubleQuoteWithTrailingCRLF",
     .input = "§\"foo\"\"bar\"\r\n",
     .output = OUT(R("foo\"bar"))},
    {.name = "EvenQuotes", .input = "§\"\"\"\"\"\"\"\"", .output = OUT(R("\"\"\""))},
    {.name = "OddQuotes", .input = "§\"\"\"\"\"\"\"∑", .errors = {E_QUOTE}},
    {.name = "LazyOddQuotes",
     .input = "§\"\"\"\"\"\"\"",
     .output = OUT(R("\"\"\"")),
     .lazy_quotes = true},
    {.name = "BadComma1", .comma = '\n', .errors = {E_INVALID_DELIM}},
    {.name = "BadComma2", .comma = '\r', .errors = {E_INVALID_DELIM}},
    {.name = "BadComma3", .comma = '"', .errors = {E_INVALID_DELIM}},
    {.name = "BadComma4", .comma = UTF8_RUNE_ERROR, .errors = {E_INVALID_DELIM}},
    {.name = "BadComment1", .comment = '\n', .errors = {E_INVALID_DELIM}},
    {.name = "BadComment2", .comment = '\r', .errors = {E_INVALID_DELIM}},
    {.name = "BadComment3", .comment = UTF8_RUNE_ERROR, .errors = {E_INVALID_DELIM}},
    {.name = "BadCommaComment",
     .comma = 'X',
     .comment = 'X',
     .errors = {E_INVALID_DELIM}},
};

/* What makePositions gives: where each field starts, where each record's
 * error is, and the input with the markers taken out. */
#define MAX_RECORDS 16
typedef struct Positions {
    Int nrec;
    Int nfield[MAX_RECORDS];
    Int pos[MAX_RECORDS][MAX_FIELDS][2];
    bool has_err[MAX_RECORDS];
    Int err_pos[MAX_RECORDS][2];
    Str input;
} Positions;

static void make_positions(Positions *p, Str text) {
    memset(p, 0, sizeof *p);
    Byte *buf = (Byte *)mem_alloc(a, (size_t)text.len + 1, 1);
    Int n = 0;
    Int line = 1, col = 1;
    Int rec = 0;
    Int i = 0;
    while (i < text.len) {
        Int size;
        Rune r = utf8_decode_rune(slice_from((void *)(uintptr_t)(text.p + i),
                                             text.len - i, text.len - i, TYPE_BYTE),
                                  &size);
        switch (r) {
        case '\n':
            line++;
            col = 1;
            buf[n++] = '\n';
            break;
        case 0xa7: /* § */
            if (p->nrec == 0)
                p->nrec = 1;
            p->pos[p->nrec - 1][p->nfield[p->nrec - 1]][0] = line;
            p->pos[p->nrec - 1][p->nfield[p->nrec - 1]][1] = col;
            p->nfield[p->nrec - 1]++;
            break;
        case 0xb6: /* ¶ */
            p->nrec++;
            rec++;
            break;
        case 0x2211: /* ∑ */
            p->has_err[rec] = true;
            p->err_pos[rec][0] = line;
            p->err_pos[rec][1] = col;
            break;
        default:
            memcpy(buf + n, text.p + i, (size_t)size);
            n += size;
            col += size;
            break;
        }
        i += size;
    }
    p->input = str_from_bytes(buf, n);
}

static Error want_sentinel(int e) {
    switch (e) {
    case E_BARE_QUOTE:
        return csv_err_bare_quote;
    case E_QUOTE:
        return csv_err_quote;
    case E_FIELD_COUNT:
        return csv_err_field_count;
    default:
        return BURROW_NO_ERROR;
    }
}

/* Go's errorWithPosition followed by reflect.DeepEqual against got. The
 * invalid delimiter error is not exported, so it is recognised by its text. */
static bool error_matches(TestingT *t, Error got, int want, Int rec,
                          const Positions *p) {
    if (want == E_INVALID_DELIM)
        return BURROW_FAILED(got) &&
               str_eq(error_text(got), S("csv: invalid field or comment delimiter"));
    const CsvParseError *pe =
        (const CsvParseError *)errors_as(got, TYPE_CSV_PARSE_ERROR);
    if (pe == NULL)
        return false;
    if (rec >= p->nrec || !p->has_err[rec]) {
        testing_t_fatalf_v(t, "no positions found for error at record %d", rec);
        return false;
    }
    Error w = want_sentinel(want);
    return pe->err.vt == w.vt && pe->err.data == w.data &&
           pe->start_line == p->pos[rec][0][0] && pe->line == p->err_pos[rec][0] &&
           pe->column == p->err_pos[rec][1];
}

static bool record_equal(Slice got, const Rec *want) {
    Int n = 0;
    while (want->f[n] != END)
        n++;
    if (got.len != n)
        return false;
    for (Int i = 0; i < n; i++)
        if (!str_eq(BURROW_AT(Str, got, i), cstr(want->f[i])))
            return false;
    return true;
}

static Int count_records(const Rec *out) {
    Int n = 0;
    while (out != NULL && out[n].f[0] != NULL)
        n++;
    return n;
}

static CsvReader *new_test_reader(const ReadTest *tt, Str input) {
    CsvReader *r = csv_new_reader(a, strings_io(input));
    if (tt->comma != 0)
        r->comma = tt->comma;
    r->comment = tt->comment;
    r->fields_per_record = tt->use_fields_per_record ? tt->fields_per_record : -1;
    r->lazy_quotes = tt->lazy_quotes;
    r->trim_leading_space = tt->trim_leading_space;
    r->reuse_record = tt->reuse_record;
    return r;
}

static void run_read_test(void *env, TestingT *t) {
    const ReadTest *tt = (const ReadTest *)env;
    Rec huge_out[2];
    const Rec *output = tt->output;
    Str text = cstr(tt->input != NULL ? tt->input : "");
    if (strcmp(tt->name, "HugeLines") == 0) {
        Str at = strings_repeat(a, S("@"), 5000);
        Str star = strings_repeat(a, S("*"), 5000);
        StringsBuilder sb = STRINGS_BUILDER(a);
        strings_builder_write_string(&sb, strings_repeat(a, S("#ignore\n"), 10000),
                                     NULL);
        strings_builder_write_string(&sb, S("§"), NULL);
        strings_builder_write_string(&sb, at, NULL);
        strings_builder_write_string(&sb, S(",§"), NULL);
        strings_builder_write_string(&sb, star, NULL);
        text = strings_builder_string(&sb);
        char *at_c = (char *)mem_alloc(a, 5001, 1);
        char *star_c = (char *)mem_alloc(a, 5001, 1);
        memcpy(at_c, at.p, 5000);
        memcpy(star_c, star.p, 5000);
        huge_out[0] = (Rec){{at_c, star_c, END}};
        huge_out[1] = (Rec){{NULL}};
        output = huge_out;
    }
    Int nout = count_records(output);

    Positions p;
    make_positions(&p, text);

    CsvReader *r = new_test_reader(tt, p.input);
    Error err = BURROW_NO_ERROR;
    Slice out = csv_reader_read_all(r, a, &err);
    Int first = -1;
    for (Int i = 0; i < 4; i++) {
        if (tt->errors[i] != E_NONE) {
            first = i;
            break;
        }
    }
    if (first >= 0) {
        if (!error_matches(t, err, tt->errors[first], first, &p))
            testing_t_fatalf_v(t, "ReadAll() error mismatch:\ngot  %v",
                               error_text(err));
        if (out.p != NULL)
            testing_t_fatalf_v(t, "ReadAll() output: got %d records, want nil",
                               out.len);
    } else {
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "unexpected Readall() error: %v", error_text(err));
        bool same = out.len == nout;
        for (Int i = 0; same && i < nout; i++)
            same = record_equal(BURROW_AT(Slice, out, i), &output[i]);
        if (!same)
            testing_t_fatalf_v(t, "ReadAll() output mismatch: got %d records, want %d",
                               out.len, nout);
    }

    /* Check input offset after call ReadAll() */
    if (BURROW_OK(err) && (int64_t)p.input.len != csv_reader_input_offset(r))
        testing_t_errorf_v(
            t, "wrong input offset after call ReadAll():\ngot:  %d\nwant: %d",
            csv_reader_input_offset(r), p.input.len);
    csv_reader_free(r);

    /* Check field and error positions. */
    r = new_test_reader(tt, p.input);
    for (Int rec_num = 0;; rec_num++) {
        err = BURROW_NO_ERROR;
        Slice rec = csv_reader_read(r, a, &err);
        int want = rec_num < 4 ? tt->errors[rec_num] : E_NONE;
        bool ok;
        if (want != E_NONE)
            ok = error_matches(t, err, want, rec_num, &p);
        else if (rec_num >= nout)
            ok = errors_is(err, io_eof) && err.data == io_eof.data;
        else
            ok = BURROW_OK(err);
        if (!ok)
            testing_t_fatalf_v(t, "Read() error at record %d:\ngot %v", rec_num,
                               BURROW_FAILED(err) ? error_text(err) : S("<nil>"));
        /* ErrFieldCount is explicitly non-fatal. */
        if (BURROW_FAILED(err) && !errors_is(err, csv_err_field_count)) {
            if (rec_num < nout)
                testing_t_fatalf_v(t, "need more records; got %d want %d", rec_num,
                                   nout);
            break;
        }
        if (!record_equal(rec, &output[rec_num]))
            testing_t_errorf_v(t, "Read vs ReadAll mismatch;\ngot %v", show(rec));
        if (rec_num >= p.nrec || p.nfield[rec_num] != rec.len)
            testing_t_fatalf_v(t, "mismatched position length at record %d", rec_num);
        for (Int i = 0; i < rec.len; i++) {
            Int col;
            Int line = csv_reader_field_pos(r, i, &col);
            if (line != p.pos[rec_num][i][0] || col != p.pos[rec_num][i][1])
                testing_t_errorf_v(
                    t,
                    "position mismatch at record %d, field %d;\ngot [%d %d]\n"
                    "want [%d %d]",
                    rec_num, i, line, col, p.pos[rec_num][i][0], p.pos[rec_num][i][1]);
        }
    }
    csv_reader_free(r);
}

static void TestRead(TestingT *t) {
    for (size_t i = 0; i < sizeof read_tests / sizeof read_tests[0]; i++) {
        const ReadTest *tt = &read_tests[i];
        testing_t_run(t, cstr(tt->name),
                      BURROW_FN(TestingTFunc, run_read_test, (void *)(uintptr_t)tt));
    }
}

/* Not in Go's tests: the parts of Read that TestRead does not reach. */

static void TestParseErrorText(TestingT *t) {
    CsvParseError e = {3, 3, 5, csv_err_quote};
    CHECK_STR(csv_parse_error_error(a, &e),
              S("parse error on line 3, column 5: extraneous or missing \" in "
                "quoted-field"));
    e.start_line = 2;
    CHECK_STR(csv_parse_error_error(a, &e),
              S("record on line 2; parse error on line 3, column 5: extraneous or "
                "missing \" in quoted-field"));
    e.err = csv_err_field_count;
    CHECK_STR(csv_parse_error_error(a, &e),
              S("record on line 3: wrong number of fields"));
    Error err = csv_parse_error_as_error(a, &e);
    CHECK_STR(error_text(err), S("record on line 3: wrong number of fields"));
    CHECK(errors_is(err, csv_err_field_count));
    const CsvParseError *pe =
        (const CsvParseError *)errors_as(err, TYPE_CSV_PARSE_ERROR);
    CHECK(pe != NULL && pe->start_line == 2 && pe->column == 5);
    Error inner = csv_parse_error_unwrap(&e);
    CHECK(inner.data == csv_err_field_count.data);
}

static void TestReadPartialRecord(TestingT *t) {
    CsvReader *r = csv_new_reader(a, strings_io(S("a,b,\"c\"d\n")));
    Error err = BURROW_NO_ERROR;
    Slice rec = csv_reader_read(r, a, &err);
    CHECK(errors_is(err, csv_err_quote));
    CHECK_STR(error_text(err), S("parse error on line 1, column 7: extraneous or "
                                 "missing \" in quoted-field"));
    CHECK_INT_EQ(rec.len, 2);
    if (rec.len == 2) {
        CHECK_STR(BURROW_AT(Str, rec, 0), S("a"));
        CHECK_STR(BURROW_AT(Str, rec, 1), S("b"));
    }
    csv_reader_free(r);
}

static void TestFieldPosPanics(TestingT *t) {
    CsvReader *r = csv_new_reader(a, strings_io(S("a,b\n")));
    Error err = BURROW_NO_ERROR;
    (void)csv_reader_read(r, a, &err);
    Str text = BURROW_STR_EMPTY;
    BURROW_TRY {
        (void)csv_reader_field_pos(r, 2, NULL);
    }
    BURROW_CATCH(v) {
        if (v.t == TYPE_STRING)
            text = *(const Str *)v.data;
    }
    BURROW_TRY_END;
    CHECK_STR(text, S("out of range index passed to FieldPos"));
    csv_reader_free(r);
}

static void TestReuseRecord(TestingT *t) {
    CsvReader *r = csv_new_reader(a, strings_io(S("a,b\nlonger,fields,here\nc,d\n")));
    r->reuse_record = true;
    r->fields_per_record = -1;
    Error err = BURROW_NO_ERROR;
    Slice r1 = csv_reader_read(r, a, &err);
    CHECK(BURROW_OK(err) && r1.len == 2);
    Slice r2 = csv_reader_read(r, a, &err);
    CHECK(BURROW_OK(err) && r2.len == 3);
    CHECK_STR(BURROW_AT(Str, r2, 1), S("fields"));
    Slice r3 = csv_reader_read(r, a, &err);
    CHECK(BURROW_OK(err) && r3.len == 2);
    CHECK(r3.p == r2.p);
    CHECK_STR(BURROW_AT(Str, r3, 1), S("d"));
    csv_reader_free(r);
}

/* A heap run of the reader with records freed one by one, so the sizes
 * csv_record_free and csv_records_free compute are the ones allocated. */
static void TestRecordFree(TestingT *t) {
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *h = track_allocator(&tr);
    StringsReader sr;
    strings_reader_reset(&sr, S("a,bb,\"c\"\"c\"\nd,,f\nlonger,line,than,before\n"));
    CsvReader *r = csv_new_reader(h, strings_reader_as_io_reader(&sr));
    r->fields_per_record = -1;
    Error err = BURROW_NO_ERROR;
    Slice rec = csv_reader_read(r, h, &err);
    CHECK(BURROW_OK(err) && rec.len == 3);
    csv_record_free(h, rec);
    Slice all = csv_reader_read_all(r, h, &err);
    CHECK(BURROW_OK(err) && all.len == 2);
    csv_records_free(h, all);
    csv_reader_free(r);

    /* A parse error partway through a field leaves its text in the reader's
     * buffer. The record handed back must not count it, or freeing it by its
     * fields gives back the wrong size. */
    const char *partial[] = {"a,b,\"c\"d\n", "\"ab\"x\n"};
    for (size_t i = 0; i < sizeof partial / sizeof partial[0]; i++) {
        strings_reader_reset(&sr, str_from_cstr(partial[i]));
        r = csv_new_reader(h, strings_reader_as_io_reader(&sr));
        rec = csv_reader_read(r, h, &err);
        CHECK(errors_is(err, csv_err_quote));
        csv_record_free(h, rec);
        csv_reader_free(r);
    }
    CHECK_INT_EQ((Int)track_check(&tr), 0);
    track_free(&tr);
}

/* ------------------------------------------------------------------ writing */

#define W_MAX_RECS 3
typedef struct WriteTest {
    Rec input[W_MAX_RECS]; /* ended by a Rec whose first field is NULL */
    const char *output;
    bool invalid_delim;
    bool use_crlf;
    Rune comma;
} WriteTest;

static const WriteTest write_tests[] = {
    {.input = {R("abc")}, .output = "abc\n"},
    {.input = {R("abc")}, .output = "abc\r\n", .use_crlf = true},
    {.input = {R("\"abc\"")}, .output = "\"\"\"abc\"\"\"\n"},
    {.input = {R("a\"b")}, .output = "\"a\"\"b\"\n"},
    {.input = {R("\"a\"b\"")}, .output = "\"\"\"a\"\"b\"\"\"\n"},
    {.input = {R(" abc")}, .output = "\" abc\"\n"},
    {.input = {R("abc,def")}, .output = "\"abc,def\"\n"},
    {.input = {R("abc", "def")}, .output = "abc,def\n"},
    {.input = {R("abc"), R("def")}, .output = "abc\ndef\n"},
    {.input = {R("abc\ndef")}, .output = "\"abc\ndef\"\n"},
    {.input = {R("abc\ndef")}, .output = "\"abc\r\ndef\"\r\n", .use_crlf = true},
    {.input = {R("abc\rdef")}, .output = "\"abcdef\"\r\n", .use_crlf = true},
    {.input = {R("abc\rdef")}, .output = "\"abc\rdef\"\n", .use_crlf = false},
    {.input = {R("")}, .output = "\n"},
    {.input = {R("", "")}, .output = ",\n"},
    {.input = {R("", "", "")}, .output = ",,\n"},
    {.input = {R("", "", "a")}, .output = ",,a\n"},
    {.input = {R("", "a", "")}, .output = ",a,\n"},
    {.input = {R("", "a", "a")}, .output = ",a,a\n"},
    {.input = {R("a", "", "")}, .output = "a,,\n"},
    {.input = {R("a", "", "a")}, .output = "a,,a\n"},
    {.input = {R("a", "a", "")}, .output = "a,a,\n"},
    {.input = {R("a", "a", "a")}, .output = "a,a,a\n"},
    {.input = {R("\\.")}, .output = "\"\\.\"\n"},
    {.input = {R("x09\x41\xb4\x1c", "aktau")}, .output = "x09\x41\xb4\x1c,aktau\n"},
    {.input = {R(",x09\x41\xb4\x1c", "aktau")},
     .output = "\",x09\x41\xb4\x1c\",aktau\n"},
    {.input = {R("a", "a", "")}, .output = "a|a|\n", .comma = '|'},
    {.input = {R(",", ",", "")}, .output = ",|,|\n", .comma = '|'},
    {.input = {R("foo")}, .output = "", .comma = '"', .invalid_delim = true},
};

/* A Rec as the Slice of Str the writer takes. */
static Slice rec_slice(const Rec *rec) {
    Int n = 0;
    while (rec->f[n] != END)
        n++;
    Slice s = slice_make(a, TYPE_STRING, n, n);
    for (Int i = 0; i < n; i++)
        BURROW_AT(Str, s, i) = cstr(rec->f[i]);
    return s;
}

static Slice recs_slice(const Rec *recs, Int n) {
    Slice s = slice_make(a, TYPE_BYTES, n, n);
    for (Int i = 0; i < n; i++)
        BURROW_AT(Slice, s, i) = rec_slice(&recs[i]);
    return s;
}

static void TestWrite(TestingT *t) {
    for (size_t n = 0; n < sizeof write_tests / sizeof write_tests[0]; n++) {
        const WriteTest *tt = &write_tests[n];
        BytesBuffer b = BYTES_BUFFER(a);
        CsvWriter *f = csv_new_writer(a, bytes_buffer_as_io_writer(&b));
        f->use_crlf = tt->use_crlf;
        if (tt->comma != 0)
            f->comma = tt->comma;
        Int nrec = 0;
        while (nrec < W_MAX_RECS && tt->input[nrec].f[0] != NULL)
            nrec++;
        Error err = csv_writer_write_all(f, recs_slice(tt->input, nrec));
        bool err_ok =
            tt->invalid_delim
                ? str_eq(error_text(err), S("csv: invalid field or comment delimiter"))
                : BURROW_OK(err);
        if (!err_ok)
            testing_t_errorf_v(t, "Unexpected error:\ngot  %v",
                               BURROW_FAILED(err) ? error_text(err) : S("<nil>"));
        Str out = bytes_buffer_string(&b, a);
        if (!str_eq(out, cstr(tt->output)))
            testing_t_errorf_v(t, "#%d: out=%q want %q", (Int)n, out, cstr(tt->output));
        csv_writer_free(f);
    }
}

BURROW_SENTINEL_ERROR(csv_test_err, "Test");

static Int error_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    BURROW_OUT(err, csv_test_err);
    return 0;
}

static const IoWriterVT error_writer_vt = {NULL, error_write};

static void TestError(TestingT *t) {
    BytesBuffer b = BYTES_BUFFER(a);
    CsvWriter *f = csv_new_writer(a, bytes_buffer_as_io_writer(&b));
    Rec abc = R("abc");
    (void)csv_writer_write(f, rec_slice(&abc));
    csv_writer_flush(f);
    Error err = csv_writer_error(f);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Unexpected error: %s\n", error_text(err));
    csv_writer_free(f);

    f = csv_new_writer(a, (IoWriter){&error_writer_vt, NULL});
    (void)csv_writer_write(f, rec_slice(&abc));
    csv_writer_flush(f);
    err = csv_writer_error(f);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "Error should not be nil");
    csv_writer_free(f);
}

/* ------------------------------------------------------------------ fuzzing */

typedef struct FuzzOpts {
    Rune comma;
    Rune comment;
    bool lazy_quotes;
    bool trim_leading_space;
} FuzzOpts;

static void fuzz_roundtrip(void *env, TestingT *t, Slice args) {
    (void)env;
    Bytes in = testing_fuzz_arg(args, 0, Bytes);
    static const FuzzOpts opts[] = {
        {',', 0, false, false},   {';', 0, false, false}, {'\t', 0, false, false},
        {',', 0, true, false},    {',', 0, false, true},  {',', '#', false, false},
        {',', ';', false, false},
    };
    Arena far;
    arena_init(&far, NULL, 0);
    Alloc *fa = arena_allocator(&far);
    Str input = str_from_bytes((const Byte *)in.p, in.len);

    for (size_t k = 0; k < sizeof opts / sizeof opts[0]; k++) {
        const FuzzOpts *tt = &opts[k];
        StringsReader sr;
        strings_reader_reset(&sr, input);
        CsvReader *r = csv_new_reader(fa, strings_reader_as_io_reader(&sr));
        r->comma = tt->comma;
        r->comment = tt->comment;
        r->lazy_quotes = tt->lazy_quotes;
        r->trim_leading_space = tt->trim_leading_space;

        Error err = BURROW_NO_ERROR;
        Slice records = csv_reader_read_all(r, fa, &err);
        if (BURROW_FAILED(err))
            continue;

        BytesBuffer buf = BYTES_BUFFER(fa);
        CsvWriter *w = csv_new_writer(fa, bytes_buffer_as_io_writer(&buf));
        w->comma = tt->comma;
        err = csv_writer_write_all(w, records);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "input %q: %v", input, error_text(err));
            break;
        }
        /* The writer doesn't support comments, so it can turn the quoted
         * record "#" into a non-quoted comment line, failing the roundtrip
         * check below. */
        if (tt->comment != 0)
            continue;

        r = csv_new_reader(fa, bytes_buffer_as_io_reader(&buf));
        r->comma = tt->comma;
        r->comment = tt->comment;
        r->lazy_quotes = tt->lazy_quotes;
        r->trim_leading_space = tt->trim_leading_space;
        Slice result = csv_reader_read_all(r, fa, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "input %q: second read: %v", input, error_text(err));
            break;
        }

        /* The reader turns \r\n into \n. The reader also parses the quoted
         * record "" as an empty string, and the writer turns that into an
         * empty line, which the reader skips over, so those are left out. */
        Int j = 0;
        bool same = true;
        for (Int i = 0; same && i < records.len; i++) {
            Slice rec = BURROW_AT(Slice, records, i);
            if (rec.len == 1 && BURROW_AT(Str, rec, 0).len == 0)
                continue;
            if (j >= result.len) {
                same = false;
                break;
            }
            Slice got = BURROW_AT(Slice, result, j++);
            same = got.len == rec.len;
            for (Int f = 0; same && f < rec.len; f++) {
                Str want =
                    strings_replace_all(fa, BURROW_AT(Str, rec, f), S("\r\n"), S("\n"));
                same = str_eq(BURROW_AT(Str, got, f), want);
            }
        }
        if (same && j != result.len)
            same = false;
        if (!same) {
            testing_t_fatalf_v(t,
                               "input %q, comma %q: first read %d records, second %d",
                               input, tt->comma, records.len, result.len);
            break;
        }
    }
    arena_free(&far);
}

static void FuzzRoundtrip(TestingF *f) {
    /* Go's target has no seeds of its own. These are inputs from the read
     * tests, so the roundtrip is checked on something without -test.fuzz. */
    static const char *const seeds[] = {
        "a,b,c\n",
        "a,b\r\nc,d\r\n",
        "\"two\nline\",\"one line\",\"three\nline\nfield\"",
        "#1,2,3\na,b,c\n#comment",
        "a \"word\",\"1\"2\",a\",\"b",
        "\"\"\"\"\"\"\"\"",
        "field\r\r\n\r\rfield\r\r\n\r\r",
        "a;b;c\n\"x;y\";z\n",
        " a,\t b,\"\"\n\"\"\n",
        "x09\x41\xb4\x1c,aktau",
        "\\.\n",
        "\"a\r\nb\",c\r\n",
    };
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        Bytes b = slice_from((void *)(uintptr_t)seeds[i], (Int)strlen(seeds[i]),
                             (Int)strlen(seeds[i]), TYPE_BYTE);
        testing_f_add_v(f, BURROW_ANY(TYPE_BYTES, &b));
    }
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, fuzz_roundtrip, NULL), TYPE_BYTES);
}

static int TestMain(TestingM *m) {
    setup();
    int rc = testing_m_run(m);
    teardown();
    return rc;
}

#define TESTS(X)                                                                       \
    X(TestRead)                                                                        \
    X(TestParseErrorText)                                                              \
    X(TestReadPartialRecord)                                                           \
    X(TestFieldPosPanics)                                                              \
    X(TestReuseRecord)                                                                 \
    X(TestRecordFree)                                                                  \
    X(TestWrite)                                                                       \
    X(TestError)                                                                       \
    X(FuzzRoundtrip)
TESTING_MAIN_WITH(TestMain, TESTS)
