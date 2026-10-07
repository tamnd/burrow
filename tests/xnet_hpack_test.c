/* Derived from golang.org/x/net/http2/hpack's tests, hpack_test.go,
 * encode_test.go and tables_test.go, the copy Go 1.27.1 vendors.
 *
 * Go's tests reach into the package, and these do the same through the
 * burrow__hpack_ functions. TestHeaderFieldTable swaps a table in for the
 * static one, which here is a table with is_static set. The two stress tests
 * seed from the clock and log the seed, as Go's do. TestSlowIncrementalDecode
 * is skipped, as it is in Go.
 *
 * TestHuffmanTrie, TestHeaderFieldString, TestDecodingErrors and
 * TestOutOfMemory are burrow's own. The first builds the decoding trie from
 * the codes the way huffman.go does and checks it is the one
 * tools/gen-xnet-hpack.sh wrote.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/xnet/hpack.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

static HpackHeaderField pair(const char *name, const char *value) {
    return (HpackHeaderField){str_from_cstr(name), str_from_cstr(value), false};
}

static HpackHeaderField field(const char *name, const char *value, bool sensitive) {
    return (HpackHeaderField){str_from_cstr(name), str_from_cstr(value), sensitive};
}

static bool field_eq(HpackHeaderField a, HpackHeaderField b) {
    return str_eq(a.name, b.name) && str_eq(a.value, b.value) &&
           a.sensitive == b.sensitive;
}

static Str field_string(Alloc *a, HpackHeaderField f) {
    return burrow__hpack_header_field_string(a, f);
}

static bool fields_eq(const HpackHeaderField *got, Int ngot,
                      const HpackHeaderField *want, Int nwant) {
    if (ngot != nwant)
        return false;
    for (Int i = 0; i < ngot; i++)
        if (!field_eq(got[i], want[i]))
            return false;
    return true;
}

/* Go's removeSpace and dehex, which also drops newlines. */
static Str remove_space(Alloc *a, const char *s) {
    Int n = (Int)strlen(s);
    Byte *p = mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Int w = 0;
    for (Int i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\n')
            p[w++] = (Byte)s[i];
    return str_from_bytes(p, w);
}

static Slice dehex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, remove_space(a, s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("dehex: bad hex"));
    return b;
}

static Str hexs(Alloc *a, Slice b) {
    return hex_encode_to_string(a, b);
}

static Slice bytes_of(const void *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

/* Collects what a decoder emits, copied into a. */
typedef struct Got {
    Alloc *a;
    HpackHeaderField f[32];
    Int n;
} Got;

static void got_emit(void *ctx, HpackHeaderField f) {
    Got *g = ctx;
    if (g->n == 32)
        return;
    g->f[g->n++] = (HpackHeaderField){str_clone(g->a, f.name), str_clone(g->a, f.value),
                                      f.sensitive};
}

static void no_emit(void *ctx, HpackHeaderField f) {
    (void)ctx;
    (void)f;
}

static HpackHeaderField must_at(TestingT *t, HpackDecoder *d, Int idx) {
    HpackHeaderField hf = {BURROW_STR_EMPTY, BURROW_STR_EMPTY, false};
    if (!burrow__hpack_decoder_at(d, (uint64_t)idx, &hf))
        testing_t_errorf_v(t, "bogus index %d", idx);
    return hf;
}

/* The dynamic table newest first, which is Go's reverseCopy. */
static Int reverse_copy(const HpackDynamicTable *dt, HpackHeaderField *out, Int cap) {
    Int n = dt->table.len;
    for (Int i = 0; i < n && i < cap; i++)
        out[i] = *burrow__hpack_table_at(&dt->table, n - 1 - i);
    return n;
}

static bool same_err(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* ------------------------------------------------------------ tables_test */

static void TestHeaderFieldTable(TestingT *t) {
    HpackTable table;
    if (!burrow__hpack_table_init(&table, heap_allocator())) {
        testing_t_fatalf_v(t, "table init failed");
        return;
    }
    burrow__hpack_table_add(&table, pair("key1", "value1-1"));
    burrow__hpack_table_add(&table, pair("key2", "value2-1"));
    burrow__hpack_table_add(&table, pair("key1", "value1-2"));
    burrow__hpack_table_add(&table, pair("key3", "value3-1"));
    burrow__hpack_table_add(&table, pair("key4", "value4-1"));
    burrow__hpack_table_add(&table, pair("key2", "value2-2"));

    /* Tests will be run twice: once before evicting anything, and again after
     * evicting the three oldest entries. */
    static const struct {
        const char *name, *value;
        uint64_t before_want_static_i;
        uint64_t after_want_static_i;
        bool sensitive;
        bool before_want_match;
        bool after_want_match;
    } tests[] = {
        {"key1", "value1-1", 1, 0, false, true, false},
        {"key1", "value1-2", 3, 0, false, true, false},
        {"key1", "value1-3", 3, 0, false, false, false},
        {"key2", "value2-1", 2, 3, false, true, false},
        {"key2", "value2-2", 6, 3, false, true, true},
        {"key2", "value2-3", 6, 3, false, false, false},
        {"key4", "value4-1", 5, 2, false, true, true},
        /* Name match only, because sensitive. */
        {"key4", "value4-1", 5, 2, true, false, false},
        /* Key not found. */
        {"key5", "value5-x", 0, 0, false, false, false},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int pass = 0; pass < 2; pass++) {
        const char *when = pass == 0 ? "before evictions" : "after evictions";
        for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
            HpackHeaderField f =
                field(tests[i].name, tests[i].value, tests[i].sensitive);
            uint64_t want_i = pass == 0 ? tests[i].before_want_static_i
                                        : tests[i].after_want_static_i;
            bool want_match =
                pass == 0 ? tests[i].before_want_match : tests[i].after_want_match;
            bool got_match = false;
            table.is_static = true;
            uint64_t got_i = burrow__hpack_table_search(&table, f, &got_match);
            if (got_i != want_i || got_match != want_match)
                testing_t_errorf_v(t, "%s: searchStatic(%s)=%d,%t want %d,%t", when,
                                   field_string(a, f), (Int)got_i, got_match,
                                   (Int)want_i, want_match);
            /* The dynamic table is the reversed table. */
            uint64_t want_dynamic_i =
                want_i == 0 ? 0 : (uint64_t)table.len - want_i + 1;
            table.is_static = false;
            got_i = burrow__hpack_table_search(&table, f, &got_match);
            if (got_i != want_dynamic_i || got_match != want_match)
                testing_t_errorf_v(t, "%s: searchDynamic(%s)=%d,%t want %d,%t", when,
                                   field_string(a, f), (Int)got_i, got_match,
                                   (Int)want_dynamic_i, want_match);
        }
        if (pass == 0)
            burrow__hpack_table_evict_oldest(&table, 3);
    }
    burrow__hpack_table_free(&table);
    arena_free(&ar);
}

static void TestHeaderFieldTable_LookupMapEviction(TestingT *t) {
    HpackTable table;
    if (!burrow__hpack_table_init(&table, heap_allocator())) {
        testing_t_fatalf_v(t, "table init failed");
        return;
    }
    burrow__hpack_table_add(&table, pair("key1", "value1-1"));
    burrow__hpack_table_add(&table, pair("key2", "value2-1"));
    burrow__hpack_table_add(&table, pair("key1", "value1-2"));
    burrow__hpack_table_add(&table, pair("key3", "value3-1"));
    burrow__hpack_table_add(&table, pair("key4", "value4-1"));
    burrow__hpack_table_add(&table, pair("key2", "value2-2"));

    /* evict all pairs */
    burrow__hpack_table_evict_oldest(&table, table.len);

    if (table.len > 0)
        testing_t_errorf_v(t, "table.len() = %d, want 0", table.len);
    if (map_len(table.by_name) > 0)
        testing_t_errorf_v(t, "len(table.byName) = %d, want 0", map_len(table.by_name));
    if (map_len(table.by_name_value) > 0)
        testing_t_errorf_v(t, "len(table.byNameValue) = %d, want 0",
                           map_len(table.by_name_value));
    burrow__hpack_table_free(&table);
}

static const char *const static_from_spec[] = {
    "          +-------+-----------------------------+---------------+",
    "          | 1     | :authority                  |               |",
    "          | 2     | :method                     | GET           |",
    "          | 3     | :method                     | POST          |",
    "          | 4     | :path                       | /             |",
    "          | 5     | :path                       | /index.html   |",
    "          | 6     | :scheme                     | http          |",
    "          | 7     | :scheme                     | https         |",
    "          | 8     | :status                     | 200           |",
    "          | 9     | :status                     | 204           |",
    "          | 10    | :status                     | 206           |",
    "          | 11    | :status                     | 304           |",
    "          | 12    | :status                     | 400           |",
    "          | 13    | :status                     | 404           |",
    "          | 14    | :status                     | 500           |",
    "          | 15    | accept-charset              |               |",
    "          | 16    | accept-encoding             | gzip, deflate |",
    "          | 17    | accept-language             |               |",
    "          | 18    | accept-ranges               |               |",
    "          | 19    | accept                      |               |",
    "          | 20    | access-control-allow-origin |               |",
    "          | 21    | age                         |               |",
    "          | 22    | allow                       |               |",
    "          | 23    | authorization               |               |",
    "          | 24    | cache-control               |               |",
    "          | 25    | content-disposition         |               |",
    "          | 26    | content-encoding            |               |",
    "          | 27    | content-language            |               |",
    "          | 28    | content-length              |               |",
    "          | 29    | content-location            |               |",
    "          | 30    | content-range               |               |",
    "          | 31    | content-type                |               |",
    "          | 32    | cookie                      |               |",
    "          | 33    | date                        |               |",
    "          | 34    | etag                        |               |",
    "          | 35    | expect                      |               |",
    "          | 36    | expires                     |               |",
    "          | 37    | from                        |               |",
    "          | 38    | host                        |               |",
    "          | 39    | if-match                    |               |",
    "          | 40    | if-modified-since           |               |",
    "          | 41    | if-none-match               |               |",
    "          | 42    | if-range                    |               |",
    "          | 43    | if-unmodified-since         |               |",
    "          | 44    | last-modified               |               |",
    "          | 45    | link                        |               |",
    "          | 46    | location                    |               |",
    "          | 47    | max-forwards                |               |",
    "          | 48    | proxy-authenticate          |               |",
    "          | 49    | proxy-authorization         |               |",
    "          | 50    | range                       |               |",
    "          | 51    | referer                     |               |",
    "          | 52    | refresh                     |               |",
    "          | 53    | retry-after                 |               |",
    "          | 54    | server                      |               |",
    "          | 55    | set-cookie                  |               |",
    "          | 56    | strict-transport-security   |               |",
    "          | 57    | transfer-encoding           |               |",
    "          | 58    | user-agent                  |               |",
    "          | 59    | vary                        |               |",
    "          | 60    | via                         |               |",
    "          | 61    | www-authenticate            |               |",
    "          +-------+-----------------------------+---------------+",
};

/* The cells of a row of the table above, trimmed. Go matches each row with a
 * regexp, and for these rows it comes to the same thing. */
static int spec_cells(Str line, Str cells[3]) {
    int n = 0;
    Int start = -1;
    for (Int i = 0; i < line.len; i++) {
        if (line.p[i] != '|')
            continue;
        if (start >= 0 && n < 3)
            cells[n++] = strings_trim_space(str_from_bytes(line.p + start, i - start));
        start = i + 1;
    }
    return n;
}

static void TestStaticTable(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    int rows = 0;
    for (size_t k = 0; k < sizeof static_from_spec / sizeof static_from_spec[0]; k++) {
        Str l = str_from_cstr(static_from_spec[k]);
        if (!strings_contains(l, BURROW_S("|")))
            continue;
        Str m[3];
        if (spec_cells(l, m) != 3)
            continue;
        Error err = BURROW_NO_ERROR;
        Int i = strconv_atoi(m[0], &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Bogus integer on line %q", l);
            continue;
        }
        if (i < 1 || i > BURROW__HPACK_STATIC_LEN) {
            testing_t_errorf_v(t, "Bogus index %d on line %q", i, l);
            continue;
        }
        rows++;
        HpackHeaderField e = burrow__hpack_static[i - 1];
        if (!str_eq(e.name, m[1]))
            testing_t_errorf_v(t, "header index %d name = %q; want %q", i, e.name,
                               m[1]);
        if (!str_eq(e.value, m[2]))
            testing_t_errorf_v(t, "header index %d value = %q; want %q", i, e.value,
                               m[2]);
        if (e.sensitive)
            testing_t_errorf_v(t, "header index %d sensitive = %t; want %t", i,
                               e.sensitive, false);
        bool match = false;
        uint64_t got =
            burrow__hpack_static_search((HpackHeaderField){m[1], m[2], false}, &match);
        if (!match || got != (uint64_t)i)
            testing_t_errorf_v(t, "header by name %s value %s index = %s; want %s",
                               m[1], m[2], strconv_itoa(a, match ? (Int)got : 0), m[0]);
    }
    if (rows != BURROW__HPACK_STATIC_LEN)
        testing_t_errorf_v(t, "read %d rows from the spec, want %d", (Int)rows,
                           (Int)BURROW__HPACK_STATIC_LEN);
    arena_free(&ar);
}

/* ------------------------------------------------------------- hpack_test */

static void TestDynamicTableAt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HpackDecoder *d = burrow__hpack_new_decoder(heap_allocator(), 4096, NULL, NULL);
    if (d == NULL) {
        testing_t_fatalf_v(t, "NewDecoder failed");
        return;
    }
    HpackHeaderField got = must_at(t, d, 2), want = pair(":method", "GET");
    if (!field_eq(got, want))
        testing_t_errorf_v(t, "at(2) = %s; want %s", field_string(a, got),
                           field_string(a, want));
    burrow__hpack_dynamic_table_add(&d->dyn_tab, pair("foo", "bar"));
    burrow__hpack_dynamic_table_add(&d->dyn_tab, pair("blake", "miz"));
    got = must_at(t, d, BURROW__HPACK_STATIC_LEN + 1);
    want = pair("blake", "miz");
    if (!field_eq(got, want))
        testing_t_errorf_v(t, "at(dyn 1) = %s; want %s", field_string(a, got),
                           field_string(a, want));
    got = must_at(t, d, BURROW__HPACK_STATIC_LEN + 2);
    want = pair("foo", "bar");
    if (!field_eq(got, want))
        testing_t_errorf_v(t, "at(dyn 2) = %s; want %s", field_string(a, got),
                           field_string(a, want));
    got = must_at(t, d, 3);
    want = pair(":method", "POST");
    if (!field_eq(got, want))
        testing_t_errorf_v(t, "at(3) = %s; want %s", field_string(a, got),
                           field_string(a, want));
    burrow__hpack_decoder_free(d);
    arena_free(&ar);
}

static void TestDynamicTableSizeEvict(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HpackDecoder *d = burrow__hpack_new_decoder(heap_allocator(), 4096, NULL, NULL);
    if (d == NULL) {
        testing_t_fatalf_v(t, "NewDecoder failed");
        return;
    }
    HpackDynamicTable *dt = &d->dyn_tab;
    if (dt->size != 0) {
        testing_t_errorf_v(t, "size = %d; want %d", (Int)dt->size, (Int)0);
        goto out;
    }
    burrow__hpack_dynamic_table_add(dt, pair("blake", "eats pizza"));
    if (dt->size != 15 + 32) {
        testing_t_errorf_v(t, "after pizza, size = %d; want %d", (Int)dt->size,
                           (Int)(15 + 32));
        goto out;
    }
    burrow__hpack_dynamic_table_add(dt, pair("foo", "bar"));
    if (dt->size != 15 + 32 + 6 + 32) {
        testing_t_errorf_v(t, "after foo bar, size = %d; want %d", (Int)dt->size,
                           (Int)(15 + 32 + 6 + 32));
        goto out;
    }
    burrow__hpack_dynamic_table_set_max_size(dt, 15 + 32 + 1 /* slop */);
    if (dt->size != 6 + 32) {
        testing_t_errorf_v(t, "after setMaxSize, size = %d; want %d", (Int)dt->size,
                           (Int)(6 + 32));
        goto out;
    }
    HpackHeaderField got = must_at(t, d, BURROW__HPACK_STATIC_LEN + 1);
    if (!field_eq(got, pair("foo", "bar")))
        testing_t_errorf_v(t, "at(dyn 1) = %s; want %s", field_string(a, got),
                           field_string(a, pair("foo", "bar")));
    burrow__hpack_dynamic_table_add(
        dt, (HpackHeaderField){BURROW_S("long"), strings_repeat(a, BURROW_S("x"), 500),
                               false});
    if (dt->size != 0)
        testing_t_errorf_v(t, "after big one, size = %d; want %d", (Int)dt->size,
                           (Int)0);
out:
    burrow__hpack_decoder_free(d);
    arena_free(&ar);
}

static Str fields_string(Alloc *a, const HpackHeaderField *f, Int n) {
    Str s = BURROW_S("[");
    for (Int i = 0; i < n; i++)
        s = fmt_sprintf_v(a, "%s%s%s", s, i > 0 ? " " : "", field_string(a, f[i]));
    return fmt_sprintf_v(a, "%s]", s);
}

static void TestDecoderDecode(TestingT *t) {
    static const HpackHeaderField c21[] = {
        {BURROW_S_INIT("custom-key"), BURROW_S_INIT("custom-header"), false}};
    static const HpackHeaderField c22[] = {
        {BURROW_S_INIT(":path"), BURROW_S_INIT("/sample/path"), false}};
    static const HpackHeaderField c23[] = {
        {BURROW_S_INIT("password"), BURROW_S_INIT("secret"), true}};
    static const HpackHeaderField c24[] = {
        {BURROW_S_INIT(":method"), BURROW_S_INIT("GET"), false}};
    static const struct {
        const char *name;
        const char *in;
        const HpackHeaderField *want;
        Int nwant;
        const HpackHeaderField *want_dyn_tab; /* newest entry first */
        Int nwant_dyn_tab;
    } tests[] = {
        /* C.2.1 Literal Header Field with Indexing
         * https://httpwg.org/specs/rfc7541.html#rfc.section.C.2.1 */
        {"C.2.1", "400a 6375 7374 6f6d 2d6b 6579 0d63 7573 746f 6d2d 6865 6164 6572",
         c21, 1, c21, 1},
        /* C.2.2 Literal Header Field without Indexing
         * https://httpwg.org/specs/rfc7541.html#rfc.section.C.2.2 */
        {"C.2.2", "040c 2f73 616d 706c 652f 7061 7468", c22, 1, NULL, 0},
        /* C.2.3 Literal Header Field never Indexed
         * https://httpwg.org/specs/rfc7541.html#rfc.section.C.2.3 */
        {"C.2.3", "1008 7061 7373 776f 7264 0673 6563 7265 74", c23, 1, NULL, 0},
        /* C.2.4 Indexed Header Field
         * https://httpwg.org/specs/rfc7541.html#rfc.section.C.2.4 */
        {"C.2.4", "82", c24, 1, NULL, 0},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HpackDecoder *d = burrow__hpack_new_decoder(heap_allocator(), 4096, NULL, NULL);
        if (d == NULL) {
            testing_t_fatalf_v(t, "NewDecoder failed");
            return;
        }
        Error err = BURROW_NO_ERROR;
        HpackHeaderFields hf =
            burrow__hpack_decoder_decode_full(d, a, dehex(a, tests[i].in), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %s", tests[i].name, error_text(err));
            burrow__hpack_decoder_free(d);
            continue;
        }
        if (!fields_eq(hf.p, hf.len, tests[i].want, tests[i].nwant))
            testing_t_errorf_v(t, "%s: Got %s; want %s", tests[i].name,
                               fields_string(a, hf.p, hf.len),
                               fields_string(a, tests[i].want, tests[i].nwant));
        HpackHeaderField dyn[8];
        Int ndyn = reverse_copy(&d->dyn_tab, dyn, 8);
        if (!fields_eq(dyn, ndyn, tests[i].want_dyn_tab, tests[i].nwant_dyn_tab))
            testing_t_errorf_v(
                t, "%s: dynamic table after = %s; want %s", tests[i].name,
                fields_string(a, dyn, ndyn),
                fields_string(a, tests[i].want_dyn_tab, tests[i].nwant_dyn_tab));
        burrow__hpack_decoder_free(d);
    }
    arena_free(&ar);
}

typedef struct EncAndWant {
    const char *enc;
    HpackHeaderField want[8];
    Int nwant;
    HpackHeaderField want_dyn_tab[8];
    Int nwant_dyn_tab;
    uint32_t want_dyn_size;
} EncAndWant;

#define P(n, v) {BURROW_S_INIT(n), BURROW_S_INIT(v), false}

static void test_decode_series(TestingT *t, uint32_t size, const EncAndWant *steps,
                               Int nsteps) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HpackDecoder *d = burrow__hpack_new_decoder(heap_allocator(), size, NULL, NULL);
    if (d == NULL) {
        testing_t_fatalf_v(t, "NewDecoder failed");
        return;
    }
    for (Int i = 0; i < nsteps; i++) {
        const EncAndWant *step = &steps[i];
        Error err = BURROW_NO_ERROR;
        HpackHeaderFields hf =
            burrow__hpack_decoder_decode_full(d, a, dehex(a, step->enc), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Error at step index %d: %s", i, error_text(err));
            break;
        }
        if (!fields_eq(hf.p, hf.len, step->want, step->nwant)) {
            testing_t_errorf_v(t, "At step index %d: Got headers %s; want %s", i,
                               fields_string(a, hf.p, hf.len),
                               fields_string(a, step->want, step->nwant));
            break;
        }
        HpackHeaderField dyn[8];
        Int ndyn = reverse_copy(&d->dyn_tab, dyn, 8);
        if (!fields_eq(dyn, ndyn, step->want_dyn_tab, step->nwant_dyn_tab))
            testing_t_errorf_v(
                t, "After step index %d, dynamic table = %s; want %s", i,
                fields_string(a, dyn, ndyn),
                fields_string(a, step->want_dyn_tab, step->nwant_dyn_tab));
        if (d->dyn_tab.size != step->want_dyn_size)
            testing_t_errorf_v(t,
                               "After step index %d, dynamic table size = %d; want %d",
                               i, (Int)d->dyn_tab.size, (Int)step->want_dyn_size);
    }
    burrow__hpack_decoder_free(d);
    arena_free(&ar);
}

/* C.3 Request Examples without Huffman Coding
 * https://httpwg.org/specs/rfc7541.html#rfc.section.C.3 */
static void TestDecodeC3_NoHuffman(TestingT *t) {
    static const EncAndWant steps[] = {
        {"8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d",
         {P(":method", "GET"), P(":scheme", "http"), P(":path", "/"),
          P(":authority", "www.example.com")},
         4,
         {P(":authority", "www.example.com")},
         1,
         57},
        {"8286 84be 5808 6e6f 2d63 6163 6865",
         {P(":method", "GET"), P(":scheme", "http"), P(":path", "/"),
          P(":authority", "www.example.com"), P("cache-control", "no-cache")},
         5,
         {P("cache-control", "no-cache"), P(":authority", "www.example.com")},
         2,
         110},
        {"8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65",
         {P(":method", "GET"), P(":scheme", "https"), P(":path", "/index.html"),
          P(":authority", "www.example.com"), P("custom-key", "custom-value")},
         5,
         {P("custom-key", "custom-value"), P("cache-control", "no-cache"),
          P(":authority", "www.example.com")},
         3,
         164},
    };
    test_decode_series(t, 4096, steps, 3);
}

/* C.4 Request Examples with Huffman Coding
 * https://httpwg.org/specs/rfc7541.html#rfc.section.C.4 */
static void TestDecodeC4_Huffman(TestingT *t) {
    static const EncAndWant steps[] = {
        {"8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff",
         {P(":method", "GET"), P(":scheme", "http"), P(":path", "/"),
          P(":authority", "www.example.com")},
         4,
         {P(":authority", "www.example.com")},
         1,
         57},
        {"8286 84be 5886 a8eb 1064 9cbf",
         {P(":method", "GET"), P(":scheme", "http"), P(":path", "/"),
          P(":authority", "www.example.com"), P("cache-control", "no-cache")},
         5,
         {P("cache-control", "no-cache"), P(":authority", "www.example.com")},
         2,
         110},
        {"8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf",
         {P(":method", "GET"), P(":scheme", "https"), P(":path", "/index.html"),
          P(":authority", "www.example.com"), P("custom-key", "custom-value")},
         5,
         {P("custom-key", "custom-value"), P("cache-control", "no-cache"),
          P(":authority", "www.example.com")},
         3,
         164},
    };
    test_decode_series(t, 4096, steps, 3);
}

/* https://httpwg.org/specs/rfc7541.html#rfc.section.C.5
 * "This section shows several consecutive header lists, corresponding to HTTP
 * responses, on the same connection. The HTTP/2 setting parameter
 * SETTINGS_HEADER_TABLE_SIZE is set to the value of 256 octets, causing some
 * evictions to occur." */
static void TestDecodeC5_ResponsesNoHuff(TestingT *t) {
    static const EncAndWant steps[] = {
        {"4803 3330 3258 0770 7269 7661 7465 611d\n"
         "4d6f 6e2c 2032 3120 4f63 7420 3230 3133\n"
         "2032 303a 3133 3a32 3120 474d 546e 1768\n"
         "7474 7073 3a2f 2f77 7777 2e65 7861 6d70\n"
         "6c65 2e63 6f6d\n",
         {P(":status", "302"), P("cache-control", "private"),
          P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
          P("location", "https://www.example.com")},
         4,
         {P("location", "https://www.example.com"),
          P("date", "Mon, 21 Oct 2013 20:13:21 GMT"), P("cache-control", "private"),
          P(":status", "302")},
         4,
         222},
        {"4803 3330 37c1 c0bf",
         {P(":status", "307"), P("cache-control", "private"),
          P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
          P("location", "https://www.example.com")},
         4,
         {P(":status", "307"), P("location", "https://www.example.com"),
          P("date", "Mon, 21 Oct 2013 20:13:21 GMT"), P("cache-control", "private")},
         4,
         222},
        {"88c1 611d 4d6f 6e2c 2032 3120 4f63 7420\n"
         "3230 3133 2032 303a 3133 3a32 3220 474d\n"
         "54c0 5a04 677a 6970 7738 666f 6f3d 4153\n"
         "444a 4b48 514b 425a 584f 5157 454f 5049\n"
         "5541 5851 5745 4f49 553b 206d 6178 2d61\n"
         "6765 3d33 3630 303b 2076 6572 7369 6f6e\n"
         "3d31\n",
         {P(":status", "200"), P("cache-control", "private"),
          P("date", "Mon, 21 Oct 2013 20:13:22 GMT"),
          P("location", "https://www.example.com"), P("content-encoding", "gzip"),
          P("set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1")},
         6,
         {P("set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"),
          P("content-encoding", "gzip"), P("date", "Mon, 21 Oct 2013 20:13:22 GMT")},
         3,
         215},
    };
    test_decode_series(t, 256, steps, 3);
}

/* https://httpwg.org/specs/rfc7541.html#rfc.section.C.6
 * "This section shows the same examples as the previous section, but using
 * Huffman encoding for the literal values. The HTTP/2 setting parameter
 * SETTINGS_HEADER_TABLE_SIZE is set to the value of 256 octets, causing some
 * evictions to occur. The eviction mechanism uses the length of the decoded
 * literal values, so the same evictions occurs as in the previous section." */
static void TestDecodeC6_ResponsesHuffman(TestingT *t) {
    static const EncAndWant steps[] = {
        {"4882 6402 5885 aec3 771a 4b61 96d0 7abe\n"
         "9410 54d4 44a8 2005 9504 0b81 66e0 82a6\n"
         "2d1b ff6e 919d 29ad 1718 63c7 8f0b 97c8\n"
         "e9ae 82ae 43d3\n",
         {P(":status", "302"), P("cache-control", "private"),
          P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
          P("location", "https://www.example.com")},
         4,
         {P("location", "https://www.example.com"),
          P("date", "Mon, 21 Oct 2013 20:13:21 GMT"), P("cache-control", "private"),
          P(":status", "302")},
         4,
         222},
        {"4883 640e ffc1 c0bf",
         {P(":status", "307"), P("cache-control", "private"),
          P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
          P("location", "https://www.example.com")},
         4,
         {P(":status", "307"), P("location", "https://www.example.com"),
          P("date", "Mon, 21 Oct 2013 20:13:21 GMT"), P("cache-control", "private")},
         4,
         222},
        {"88c1 6196 d07a be94 1054 d444 a820 0595\n"
         "040b 8166 e084 a62d 1bff c05a 839b d9ab\n"
         "77ad 94e7 821d d7f2 e6c7 b335 dfdf cd5b\n"
         "3960 d5af 2708 7f36 72c1 ab27 0fb5 291f\n"
         "9587 3160 65c0 03ed 4ee5 b106 3d50 07\n",
         {P(":status", "200"), P("cache-control", "private"),
          P("date", "Mon, 21 Oct 2013 20:13:22 GMT"),
          P("location", "https://www.example.com"), P("content-encoding", "gzip"),
          P("set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1")},
         6,
         {P("set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"),
          P("content-encoding", "gzip"), P("date", "Mon, 21 Oct 2013 20:13:22 GMT")},
         3,
         215},
    };
    test_decode_series(t, 256, steps, 3);
}

/* Go's HuffmanDecode into a bytes.Buffer, and the buffer's contents. */
static Error huffman_decode_buf(Alloc *a, Slice in, Str *out) {
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = BURROW_NO_ERROR;
    (void)burrow__hpack_huffman_decode(bytes_buffer_as_io_writer(&buf), in, &err);
    *out = bytes_buffer_string(&buf, a);
    return err;
}

/* Go's huffmanDecode(&buf, maxLen, v). */
static Error huffman_decode_max(Alloc *a, Int max_len, Slice v, Str *out) {
    *out = BURROW_STR_EMPTY;
    Int cap = v.len * 8 / 5 + 1;
    Byte *p = mem_alloc_nozero(a, (size_t)cap, 1);
    if (p == NULL)
        return burrow_err_out_of_memory;
    Int n = 0;
    Error err = burrow__hpack_huffman_decode_into(p, cap, &n, max_len, v.p, v.len);
    *out = str_from_bytes(p, n);
    return err;
}

static void TestHuffmanDecodeExcessPadding(TestingT *t) {
    static const struct {
        Byte b[8];
        Int n;
    } tests[] = {
        {{0xff}, 1},                         /* Padding Exceeds 7 bits */
        {{0x1f, 0xff}, 2},                   /* {"a", 1 byte excess padding} */
        {{0x1f, 0xff, 0xff}, 3},             /* {"a", 2 byte excess padding} */
        {{0x1f, 0xff, 0xff, 0xff}, 4},       /* {"a", 3 byte excess padding} */
        {{0xff, 0x9f, 0xff, 0xff, 0xff}, 5}, /* {"a", 29 bit excess padding} */
        {{'R', 0xbc, '0', 0xff, 0xff, 0xff, 0xff},
         7}, /* Padding ends on partial symbol. */
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str out;
        Slice in = bytes_of(tests[i].b, tests[i].n);
        Error err = huffman_decode_buf(a, in, &out);
        if (!same_err(err, burrow__hpack_err_invalid_huffman))
            testing_t_errorf_v(t, "test-%d: decode(%q) = %s; want ErrInvalidHuffman",
                               (Int)i, str_from_bytes(tests[i].b, tests[i].n),
                               BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err));
    }
    arena_free(&ar);
}

static void TestHuffmanDecodeEOS(TestingT *t) {
    static const Byte in[] = {0xff, 0xff, 0xff, 0xff, 0xfc}; /* {EOS, "?"} */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str out;
    Error err = huffman_decode_buf(arena_allocator(&ar), bytes_of(in, 5), &out);
    if (!same_err(err, burrow__hpack_err_invalid_huffman))
        testing_t_errorf_v(t, "error = %s; want ErrInvalidHuffman",
                           BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err));
    arena_free(&ar);
}

static void TestHuffmanDecodeMaxLengthOnTrailingByte(TestingT *t) {
    static const Byte in[] = {0x00, 0x01}; /* {"0", "0", "0"} */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str out;
    Error err = huffman_decode_max(arena_allocator(&ar), 2, bytes_of(in, 2), &out);
    if (!same_err(err, burrow__hpack_err_string_length))
        testing_t_errorf_v(t, "error = %s; want ErrStringLength",
                           BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err));
    arena_free(&ar);
}

static void TestHuffmanDecodeCorruptPadding(TestingT *t) {
    static const Byte in[] = {0x00};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str out;
    Error err = huffman_decode_buf(arena_allocator(&ar), bytes_of(in, 1), &out);
    if (!same_err(err, burrow__hpack_err_invalid_huffman))
        testing_t_errorf_v(t, "error = %s; want ErrInvalidHuffman",
                           BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err));
    arena_free(&ar);
}

static const struct {
    const char *hex;
    const char *plain;
} huffman_cases[] = {
    {"f1e3 c2e5 f23a 6ba0 ab90 f4ff", "www.example.com"},
    {"a8eb 1064 9cbf", "no-cache"},
    {"25a8 49e9 5ba9 7d7f", "custom-key"},
    {"25a8 49e9 5bb8 e8b4 bf", "custom-value"},
    {"6402", "302"},
    {"aec3 771a 4b", "private"},
    {"d07a be94 1054 d444 a820 0595 040b 8166 e082 a62d 1bff",
     "Mon, 21 Oct 2013 20:13:21 GMT"},
    {"9d29 ad17 1863 c78f 0b97 c8e9 ae82 ae43 d3", "https://www.example.com"},
    {"9bd9 ab", "gzip"},
    {"94e7 821d d7f2 e6c7 b335 dfdf cd5b 3960 d5af 2708 7f36 72c1 ab27 0fb5 291f 9587 "
     "3160 65c0 03ed 4ee5 b106 3d50 07",
     "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"},
};

static void TestHuffmanDecode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof huffman_cases / sizeof huffman_cases[0]; i++) {
        Error herr = BURROW_NO_ERROR;
        Slice in = hex_decode_string(a, remove_space(a, huffman_cases[i].hex), &herr);
        if (BURROW_FAILED(herr)) {
            testing_t_errorf_v(t, "%d. hex input error: %s", (Int)i, error_text(herr));
            continue;
        }
        Str got;
        Error err = huffman_decode_buf(a, in, &got);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%d. decode error: %s", (Int)i, error_text(err));
            continue;
        }
        Str want = str_from_cstr(huffman_cases[i].plain);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "%d. decode = %q; want %q", (Int)i, got, want);
    }
    arena_free(&ar);
}

static void TestAppendHuffmanString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof huffman_cases / sizeof huffman_cases[0]; i++) {
        Str want = remove_space(a, huffman_cases[i].hex);
        Slice buf = burrow__hpack_append_huffman_string(
            a, slice_nil(TYPE_BYTE), str_from_cstr(huffman_cases[i].plain));
        Str got = hexs(a, buf);
        if (!str_eq(want, got))
            testing_t_errorf_v(t, "%d. encode = %q; want %q", (Int)i, got, want);
    }
    arena_free(&ar);
}

static void TestHuffmanMaxStrLen(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str msg = BURROW_S("Some string");
    Slice huff = burrow__hpack_append_huffman_string(a, slice_nil(TYPE_BYTE), msg);

    const Int good[] = {0, msg.len, msg.len + 1};
    for (int i = 0; i < 3; i++) {
        Str out;
        Error err = huffman_decode_max(a, good[i], huff, &out);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "For maxLen=%d, unexpected error: %s", good[i],
                               error_text(err));
        if (!str_eq(out, msg))
            testing_t_errorf_v(t, "For maxLen=%d, out = %q; want %q", good[i], out,
                               msg);
    }
    Str out;
    Error err = huffman_decode_max(a, msg.len - 1, huff, &out);
    if (!same_err(err, burrow__hpack_err_string_length))
        testing_t_errorf_v(t, "err = %s; want ErrStringLength",
                           BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err));
    arena_free(&ar);
}

static void TestHuffmanRoundtripStress(TestingT *t) {
    enum { LEN = 50 }; /* of uncompressed string */
    Byte input[LEN];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Int n = testing_short() ? 100 : 5000;
    int64_t seed = time_unix_nano(time_now());
    testing_t_logf_v(t, "Seed = %s", strconv_format_int(a, seed, 10));
    MathRandRand *src = math_rand_new(a, math_rand_new_source(a, seed));
    if (src == NULL) {
        testing_t_fatalf_v(t, "rand.New failed");
        return;
    }
    int64_t enc_size = 0;
    for (Int i = 0; i < n; i++) {
        ArenaMark m = arena_mark(&ar);
        for (int l = 0; l < LEN; l++)
            input[l] = (Byte)math_rand_rand_intn(src, 256);
        Str in = str_from_bytes(input, LEN);
        Slice huff = burrow__hpack_append_huffman_string(a, slice_nil(TYPE_BYTE), in);
        enc_size += huff.len;
        Str out;
        Error err = huffman_decode_max(a, 0, huff, &out);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Failed to decode %q -> %q -> error %s", in,
                               str_from_bytes(huff.p, huff.len), error_text(err));
        else if (!str_eq(out, in))
            testing_t_errorf_v(t, "Roundtrip failure on %q -> %q -> %q", in,
                               str_from_bytes(huff.p, huff.len), out);
        arena_release(&ar, m);
    }
    testing_t_logf_v(t, "Compressed size of original: %.2f%% (%d -> %d)",
                     100 * ((double)enc_size / ((double)LEN * (double)n)), (Int)LEN * n,
                     (Int)enc_size);
    arena_free(&ar);
}

static void TestHuffmanDecodeFuzz(TestingT *t) {
    enum { LEN = 50 }; /* of compressed */
    Byte zbuf[LEN];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Int n = testing_short() ? 100 : 5000;
    int64_t seed = time_unix_nano(time_now());
    testing_t_logf_v(t, "Seed = %s", strconv_format_int(a, seed, 10));
    MathRandRand *src = math_rand_new(a, math_rand_new_source(a, seed));
    if (src == NULL) {
        testing_t_fatalf_v(t, "rand.New failed");
        return;
    }
    Int num_fail = 0;
    for (Int i = 0; i < n; i++) {
        Int zlen = LEN;
        if (i == 0) {
            /* Start with at least one invalid one. */
            memcpy(zbuf, "00\x91\xff\xff\xff\xff\xc8", 8);
            zlen = 8;
        } else {
            for (int l = 0; l < LEN; l++)
                zbuf[l] = (Byte)math_rand_rand_intn(src, 256);
        }
        ArenaMark m = arena_mark(&ar);
        Str out;
        Error err = huffman_decode_max(a, 0, bytes_of(zbuf, zlen), &out);
        if (BURROW_FAILED(err)) {
            if (same_err(err, burrow__hpack_err_invalid_huffman))
                num_fail++;
            else
                testing_t_errorf_v(t, "Failed to decode %q: %s",
                                   str_from_bytes(zbuf, zlen), error_text(err));
        }
        arena_release(&ar, m);
    }
    testing_t_logf_v(t, "%.2f%% are invalid (%d / %d)",
                     100 * (double)num_fail / (double)n, num_fail, n);
    if (num_fail < 1)
        testing_t_errorf_v(
            t, "expected at least one invalid huffman encoding (test starts "
               "with one)");
    arena_free(&ar);
}

static void TestReadVarInt(TestingT *t) {
    enum { NIL, NEED_MORE, OVERFLOW };
    static const struct {
        Int n;
        Byte p[16];
        Int plen;
        uint64_t i;
        Int consumed;
        Int err;
    } tests[] = {
        /* Fits in a byte: */
        {1, {0}, 1, 0, 1, NIL},
        {2, {2}, 1, 2, 1, NIL},
        {3, {6}, 1, 6, 1, NIL},
        {4, {14}, 1, 14, 1, NIL},
        {5, {30}, 1, 30, 1, NIL},
        {6, {62}, 1, 62, 1, NIL},
        {7, {126}, 1, 126, 1, NIL},
        {8, {254}, 1, 254, 1, NIL},

        /* Doesn't fit in a byte: */
        {1, {1}, 1, 0, 0, NEED_MORE},
        {2, {3}, 1, 0, 0, NEED_MORE},
        {3, {7}, 1, 0, 0, NEED_MORE},
        {4, {15}, 1, 0, 0, NEED_MORE},
        {5, {31}, 1, 0, 0, NEED_MORE},
        {6, {63}, 1, 0, 0, NEED_MORE},
        {7, {127}, 1, 0, 0, NEED_MORE},
        {8, {255}, 1, 0, 0, NEED_MORE},

        /* Ignoring top bits: */
        {5, {255, 154, 10}, 3, 1337, 3, NIL}, /* high dummy three bits: 111 */
        {5, {159, 154, 10}, 3, 1337, 3, NIL}, /* high dummy three bits: 100 */
        {5, {191, 154, 10}, 3, 1337, 3, NIL}, /* high dummy three bits: 101 */

        /* Extra byte: */
        {5, {191, 154, 10, 2}, 4, 1337, 3, NIL}, /* extra byte */

        /* Short a byte: */
        {5, {191, 154}, 2, 0, 0, NEED_MORE},

        /* integer overflow: */
        {1,
         {255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
          128},
         16,
         0,
         0,
         OVERFLOW},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t k = 0; k < sizeof tests / sizeof tests[0]; k++) {
        Int used = -1;
        Error err = BURROW_NO_ERROR;
        uint64_t i = burrow__hpack_read_var_int((Byte)tests[k].n, tests[k].p,
                                                tests[k].plen, &used, &err);
        Error want_err = tests[k].err == NIL ? BURROW_NO_ERROR
                         : tests[k].err == NEED_MORE
                             ? burrow__hpack_err_need_more
                             : burrow__hpack_err_varint_overflow;
        bool err_ok = tests[k].err == NIL ? BURROW_OK(err) : same_err(err, want_err);
        if (i != tests[k].i || used != tests[k].consumed || !err_ok)
            testing_t_errorf_v(
                t, "readVarInt(%d, %s) = {%d %d %s}; want {%d %d %s}", (Int)tests[k].n,
                hexs(a, bytes_of(tests[k].p, tests[k].plen)), (Int)i, used,
                BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err), (Int)tests[k].i,
                tests[k].consumed,
                BURROW_OK(want_err) ? BURROW_S("<nil>") : error_text(want_err));
    }
    arena_free(&ar);
}

/* Fuzz crash, originally reported at https://github.com/bradfitz/http2/issues/56 */
static void TestHuffmanFuzzCrash(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Str got = burrow__hpack_huffman_decode_to_string(
        arena_allocator(&ar), bytes_of("00\x91\xff\xff\xff\xff\xc8", 8), &err);
    if (got.len != 0)
        testing_t_errorf_v(t, "Got %q; want empty string", got);
    if (!same_err(err, burrow__hpack_err_invalid_huffman))
        testing_t_errorf_v(t, "Err = %s; want ErrInvalidHuffman",
                           BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err));
    arena_free(&ar);
}

typedef struct EmitCount {
    HpackDecoder *dec;
    int num_callback;
} EmitCount;

static void emit_count_disable(void *ctx, HpackHeaderField f) {
    (void)f;
    EmitCount *c = ctx;
    c->num_callback++;
    burrow__hpack_decoder_set_emit_enabled(c->dec, false);
}

static void TestEmitEnabled(TestingT *t) {
    Alloc *h = heap_allocator();
    BytesBuffer buf = BYTES_BUFFER(h);
    HpackEncoder *enc = burrow__hpack_new_encoder(h, bytes_buffer_as_io_writer(&buf));
    if (enc == NULL) {
        testing_t_fatalf_v(t, "NewEncoder failed");
        return;
    }
    burrow__hpack_encoder_write_field(enc, pair("foo", "bar"));
    burrow__hpack_encoder_write_field(enc, pair("foo", "bar"));

    EmitCount c = {NULL, 0};
    HpackDecoder *dec = burrow__hpack_new_decoder(h, 8 << 20, emit_count_disable, &c);
    if (dec == NULL) {
        testing_t_fatalf_v(t, "NewDecoder failed");
        return;
    }
    c.dec = dec;
    if (!burrow__hpack_decoder_emit_enabled(dec))
        testing_t_errorf_v(t, "initial emit enabled = false; want true");
    Error err = BURROW_NO_ERROR;
    (void)burrow__hpack_decoder_write(dec, bytes_buffer_bytes(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s", error_text(err));
    if (c.num_callback != 1)
        testing_t_errorf_v(t, "num callbacks = %d; want 1", (Int)c.num_callback);
    if (burrow__hpack_decoder_emit_enabled(dec))
        testing_t_errorf_v(t, "emit enabled = true; want false");
    burrow__hpack_decoder_free(dec);
    burrow__hpack_encoder_free(enc);
    bytes_buffer_free(&buf);
}

static void TestSlowIncrementalDecode(TestingT *t) {
    /* TODO(dneil): Fix for -race mode. */
    testing_t_skip_v(t, "too slow in -race mode");
}

static void TestSaveBufLimit(TestingT *t) {
    enum { MAX_STR = 1 << 10 };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Got got = {0};
    got.a = a;
    HpackDecoder *dec =
        burrow__hpack_new_decoder(heap_allocator(), 4096, got_emit, &got);
    if (dec == NULL) {
        testing_t_fatalf_v(t, "NewDecoder failed");
        return;
    }
    burrow__hpack_decoder_set_max_string_length(dec, MAX_STR);
    Byte tb = burrow__hpack_encode_type_byte(false, false);
    Slice frag = slice_append(a, slice_nil(TYPE_BYTE), &tb, 1);
    frag = burrow__hpack_append_var_int(a, frag, 7, 3);
    frag = slice_append(a, frag, "foo", 3);
    frag = burrow__hpack_append_var_int(a, frag, 7, 3);
    frag = slice_append(a, frag, "bar", 3);

    Error err = BURROW_NO_ERROR;
    (void)burrow__hpack_decoder_write(dec, frag, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s", error_text(err));
        goto out;
    }
    HpackHeaderField want[] = {pair("foo", "bar")};
    if (!fields_eq(got.f, got.n, want, 1))
        testing_t_errorf_v(t, "After small writes, got %s; want %s",
                           fields_string(a, got.f, got.n), fields_string(a, want, 1));

    frag = slice_append(a, slice_nil(TYPE_BYTE), &tb, 1);
    frag = burrow__hpack_append_var_int(a, frag, 7, (Int)MAX_STR * 3);
    Byte *zeros = mem_alloc(a, (Int)MAX_STR * 3, 1);
    if (zeros == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    frag = slice_append(a, frag, zeros, (Int)MAX_STR * 3);

    (void)burrow__hpack_decoder_write(dec, frag, &err);
    if (!same_err(err, burrow__hpack_err_string_length))
        testing_t_errorf_v(t, "Write error = %s; want ErrStringLength",
                           BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err));
out:
    burrow__hpack_decoder_free(dec);
    arena_free(&ar);
}

static void TestDynamicSizeUpdate(TestingT *t) {
    Alloc *h = heap_allocator();
    BytesBuffer buf = BYTES_BUFFER(h);
    HpackEncoder *enc = burrow__hpack_new_encoder(h, bytes_buffer_as_io_writer(&buf));
    if (enc == NULL) {
        testing_t_fatalf_v(t, "NewEncoder failed");
        return;
    }
    burrow__hpack_encoder_set_max_dynamic_table_size(enc, 255);
    burrow__hpack_encoder_write_field(enc, pair("foo", "bar"));

    HpackDecoder *d = burrow__hpack_new_decoder(h, 4096, no_emit, NULL);
    if (d == NULL) {
        testing_t_fatalf_v(t, "NewDecoder failed");
        return;
    }
    Error err = BURROW_NO_ERROR;
    (void)burrow__hpack_decoder_write(d, bytes_buffer_bytes(&buf), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "unexpected error: got = %s", error_text(err));
        goto out;
    }

    (void)burrow__hpack_decoder_close(d);

    /* Start a new header */
    (void)burrow__hpack_decoder_write(d, bytes_buffer_bytes(&buf), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "unexpected error: got = %s", error_text(err));
        goto out;
    }

    /* must fail since the dynamic table update must be at the beginning */
    (void)burrow__hpack_decoder_write(d, bytes_buffer_bytes(&buf), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t,
                           "dynamic table size update not at the beginning of a header "
                           "block");
out:
    burrow__hpack_decoder_free(d);
    burrow__hpack_encoder_free(enc);
    bytes_buffer_free(&buf);
}

/* ------------------------------------------------------------ encode_test */

static void TestEncoderTableSizeUpdate(TestingT *t) {
    static const struct {
        uint32_t size1, size2;
        const char *want_hex;
    } tests[] = {
        /* Should emit 2 table size updates (2048 and 4096) */
        {2048, 4096, "3fe10f 3fe11f 82"},

        /* Should emit 1 table size update (2048) */
        {16384, 2048, "3fe10f 82"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        BytesBuffer buf = BYTES_BUFFER(a);
        HpackEncoder *e = burrow__hpack_new_encoder(heap_allocator(),
                                                    bytes_buffer_as_io_writer(&buf));
        if (e == NULL) {
            testing_t_fatalf_v(t, "NewEncoder failed");
            return;
        }
        burrow__hpack_encoder_set_max_dynamic_table_size(e, tests[i].size1);
        burrow__hpack_encoder_set_max_dynamic_table_size(e, tests[i].size2);
        Error err = burrow__hpack_encoder_write_field(e, pair(":method", "GET"));
        if (BURROW_FAILED(err)) {
            burrow__hpack_encoder_free(e);
            testing_t_fatalf_v(t, "%s", error_text(err));
            return;
        }
        Str want = remove_space(a, tests[i].want_hex);
        Str got = hexs(a, bytes_buffer_bytes(&buf));
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "e.SetDynamicTableSize %d, %d = %q; want %q",
                               (Int)tests[i].size1, (Int)tests[i].size2, got, want);
        burrow__hpack_encoder_free(e);
    }
    arena_free(&ar);
}

static void TestEncoderWriteField(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Alloc *h = heap_allocator();
    BytesBuffer buf = BYTES_BUFFER(h);
    HpackEncoder *e = burrow__hpack_new_encoder(h, bytes_buffer_as_io_writer(&buf));
    Got got = {0};
    got.a = a;
    HpackDecoder *d = burrow__hpack_new_decoder(h, 4 << 10, got_emit, &got);
    if (e == NULL || d == NULL) {
        testing_t_fatalf_v(t, "NewEncoder or NewDecoder failed");
        return;
    }

    static const struct {
        HpackHeaderField hdrs[5];
        Int n;
    } tests[] = {
        {{P(":method", "GET"), P(":scheme", "http"), P(":path", "/"),
          P(":authority", "www.example.com")},
         4},
        {{P(":method", "GET"), P(":scheme", "http"), P(":path", "/"),
          P(":authority", "www.example.com"), P("cache-control", "no-cache")},
         5},
        {{P(":method", "GET"), P(":scheme", "https"), P(":path", "/index.html"),
          P(":authority", "www.example.com"), P("custom-key", "custom-value")},
         5},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bytes_buffer_reset(&buf);
        got.n = 0;
        for (Int k = 0; k < tests[i].n; k++) {
            Error err = burrow__hpack_encoder_write_field(e, tests[i].hdrs[k]);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s", error_text(err));
                goto out;
            }
        }
        Error err = BURROW_NO_ERROR;
        (void)burrow__hpack_decoder_write(d, bytes_buffer_bytes(&buf), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d. Decoder Write = %s", (Int)i, error_text(err));
        if (!fields_eq(got.f, got.n, tests[i].hdrs, tests[i].n))
            testing_t_errorf_v(t, "%d. Decoded %s; want %s", (Int)i,
                               fields_string(a, got.f, got.n),
                               fields_string(a, tests[i].hdrs, tests[i].n));
    }
out:
    burrow__hpack_decoder_free(d);
    burrow__hpack_encoder_free(e);
    bytes_buffer_free(&buf);
    arena_free(&ar);
}

static void TestEncoderSearchTable(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HpackEncoder *e = burrow__hpack_new_encoder(heap_allocator(), (IoWriter){0});
    if (e == NULL) {
        testing_t_fatalf_v(t, "NewEncoder failed");
        return;
    }

    burrow__hpack_dynamic_table_add(&e->dyn_tab, pair("foo", "bar"));
    burrow__hpack_dynamic_table_add(&e->dyn_tab, pair("blake", "miz"));
    burrow__hpack_dynamic_table_add(&e->dyn_tab, pair(":method", "GET"));

    const uint64_t s = BURROW__HPACK_STATIC_LEN;
    const struct {
        HpackHeaderField hf;
        uint64_t want_i;
        bool want_match;
    } tests[] = {
        /* Name and Value match */
        {pair("foo", "bar"), s + 3, true},
        {pair("blake", "miz"), s + 2, true},
        {pair(":method", "GET"), 2, true},

        /* Only name match because Sensitive == true. This is allowed to match
         * any ":method" entry. The current implementation uses the last entry
         * added in newStaticTable. */
        {field(":method", "GET", true), 3, false},

        /* Only Name matches */
        {pair("foo", "..."), s + 3, false},
        {pair("blake", "..."), s + 2, false},
        /* As before, this is allowed to match any ":method" entry. */
        {pair(":method", "..."), 3, false},

        /* None match */
        {pair("foo-", "bar"), 0, false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool got_match = false;
        uint64_t got_i = burrow__hpack_encoder_search_table(e, tests[i].hf, &got_match);
        if (got_i != tests[i].want_i || got_match != tests[i].want_match)
            testing_t_errorf_v(t, "d.search(%s) = %d, %t; want %d, %t",
                               field_string(a, tests[i].hf), (Int)got_i, got_match,
                               (Int)tests[i].want_i, tests[i].want_match);
    }
    burrow__hpack_encoder_free(e);
    arena_free(&ar);
}

static void TestAppendVarInt(TestingT *t) {
    static const struct {
        Int n;
        uint64_t i;
        Byte want[8];
        Int nwant;
    } tests[] = {
        /* Fits in a byte: */
        {1, 0, {0}, 1},
        {2, 2, {2}, 1},
        {3, 6, {6}, 1},
        {4, 14, {14}, 1},
        {5, 30, {30}, 1},
        {6, 62, {62}, 1},
        {7, 126, {126}, 1},
        {8, 254, {254}, 1},

        /* Multiple bytes: */
        {5, 1337, {31, 154, 10}, 3},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Slice got = burrow__hpack_append_var_int(a, slice_nil(TYPE_BYTE),
                                                 (Byte)tests[i].n, tests[i].i);
        Slice want = bytes_of(tests[i].want, tests[i].nwant);
        if (!bytes_equal(got, want))
            testing_t_errorf_v(t, "appendVarInt(nil, %d, %d) = %s; want %s",
                               (Int)tests[i].n, (Int)tests[i].i, hexs(a, got),
                               hexs(a, want));
    }
    arena_free(&ar);
}

static void TestAppendHpackString(TestingT *t) {
    static const struct {
        const char *s, *want_hex;
    } tests[] = {
        /* Huffman encoded */
        {"www.example.com", "8c f1e3 c2e5 f23a 6ba0 ab90 f4ff"},

        /* Not Huffman encoded */
        {"a", "01 61"},

        /* zero length */
        {"", "00"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str want = remove_space(a, tests[i].want_hex);
        Slice buf = burrow__hpack_append_hpack_string(a, slice_nil(TYPE_BYTE),
                                                      str_from_cstr(tests[i].s));
        Str got = hexs(a, buf);
        if (!str_eq(want, got))
            testing_t_errorf_v(t, "appendHpackString(nil, %q) = %q; want %q",
                               str_from_cstr(tests[i].s), got, want);
    }
    arena_free(&ar);
}

static void TestAppendIndexed(TestingT *t) {
    static const struct {
        uint64_t i;
        const char *want_hex;
    } tests[] = {
        /* 1 byte */
        {1, "81"},
        {126, "fe"},

        /* 2 bytes */
        {127, "ff00"},
        {128, "ff01"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str want = remove_space(a, tests[i].want_hex);
        Slice buf = burrow__hpack_append_indexed(a, slice_nil(TYPE_BYTE), tests[i].i);
        Str got = hexs(a, buf);
        if (!str_eq(want, got))
            testing_t_errorf_v(t, "appendIndex(nil, %d) = %q; want %q", (Int)tests[i].i,
                               got, want);
    }
    arena_free(&ar);
}

static void TestAppendNewName(TestingT *t) {
    static const struct {
        HpackHeaderField f;
        bool indexing;
        const char *want_hex;
    } tests[] = {
        /* Incremental indexing */
        {{BURROW_S_INIT("custom-key"), BURROW_S_INIT("custom-value"), false},
         true,
         "40 88 25a8 49e9 5ba9 7d7f 89 25a8 49e9 5bb8 e8b4 bf"},

        /* Without indexing */
        {{BURROW_S_INIT("custom-key"), BURROW_S_INIT("custom-value"), false},
         false,
         "00 88 25a8 49e9 5ba9 7d7f 89 25a8 49e9 5bb8 e8b4 bf"},

        /* Never indexed */
        {{BURROW_S_INIT("custom-key"), BURROW_S_INIT("custom-value"), true},
         true,
         "10 88 25a8 49e9 5ba9 7d7f 89 25a8 49e9 5bb8 e8b4 bf"},
        {{BURROW_S_INIT("custom-key"), BURROW_S_INIT("custom-value"), true},
         false,
         "10 88 25a8 49e9 5ba9 7d7f 89 25a8 49e9 5bb8 e8b4 bf"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str want = remove_space(a, tests[i].want_hex);
        Slice buf = burrow__hpack_append_new_name(a, slice_nil(TYPE_BYTE), tests[i].f,
                                                  tests[i].indexing);
        Str got = hexs(a, buf);
        if (!str_eq(want, got))
            testing_t_errorf_v(t, "appendNewName(nil, %s, %t) = %q; want %q",
                               field_string(a, tests[i].f), tests[i].indexing, got,
                               want);
    }
    arena_free(&ar);
}

static void TestAppendIndexedName(TestingT *t) {
    static const struct {
        HpackHeaderField f;
        uint64_t i;
        bool indexing;
        const char *want_hex;
    } tests[] = {
        /* Incremental indexing */
        {{BURROW_S_INIT(":status"), BURROW_S_INIT("302"), false},
         8,
         true,
         "48 82 6402"},

        /* Without indexing */
        {{BURROW_S_INIT(":status"), BURROW_S_INIT("302"), false},
         8,
         false,
         "08 82 6402"},

        /* Never indexed */
        {{BURROW_S_INIT(":status"), BURROW_S_INIT("302"), true}, 8, true, "18 82 6402"},
        {{BURROW_S_INIT(":status"), BURROW_S_INIT("302"), true},
         8,
         false,
         "18 82 6402"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str want = remove_space(a, tests[i].want_hex);
        Slice buf = burrow__hpack_append_indexed_name(
            a, slice_nil(TYPE_BYTE), tests[i].f, tests[i].i, tests[i].indexing);
        Str got = hexs(a, buf);
        if (!str_eq(want, got))
            testing_t_errorf_v(t, "appendIndexedName(nil, %s, %t) = %q; want %q",
                               field_string(a, tests[i].f), tests[i].indexing, got,
                               want);
    }
    arena_free(&ar);
}

static void TestAppendTableSize(TestingT *t) {
    static const struct {
        uint32_t i;
        const char *want_hex;
    } tests[] = {
        /* Fits into 1 byte */
        {30, "3e"},

        /* Extra byte */
        {31, "3f00"},
        {32, "3f01"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str want = remove_space(a, tests[i].want_hex);
        Slice buf =
            burrow__hpack_append_table_size(a, slice_nil(TYPE_BYTE), tests[i].i);
        Str got = hexs(a, buf);
        if (!str_eq(want, got))
            testing_t_errorf_v(t, "appendTableSize(nil, %d) = %q; want %q",
                               (Int)tests[i].i, got, want);
    }
    arena_free(&ar);
}

static void TestEncoderSetMaxDynamicTableSize(TestingT *t) {
    Alloc *h = heap_allocator();
    BytesBuffer buf = BYTES_BUFFER(h);
    HpackEncoder *e = burrow__hpack_new_encoder(h, bytes_buffer_as_io_writer(&buf));
    if (e == NULL) {
        testing_t_fatalf_v(t, "NewEncoder failed");
        return;
    }
    static const struct {
        uint32_t v;
        bool want_update;
        uint32_t want_min_size;
        uint32_t want_max_size;
    } tests[] = {
        /* Set new table size to 2048 */
        {2048, true, 2048, 2048},

        /* Set new table size to 16384, but still limited to 4096 */
        {16384, true, 2048, 4096},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        burrow__hpack_encoder_set_max_dynamic_table_size(e, tests[i].v);
        if (e->table_size_update != tests[i].want_update)
            testing_t_errorf_v(t, "e.tableSizeUpdate = %t; want %t",
                               e->table_size_update, tests[i].want_update);
        if (e->min_size != tests[i].want_min_size)
            testing_t_errorf_v(t, "e.minSize = %d; want %d", (Int)e->min_size,
                               (Int)tests[i].want_min_size);
        if (e->dyn_tab.max_size != tests[i].want_max_size)
            testing_t_errorf_v(t, "e.maxSize = %d; want %d", (Int)e->dyn_tab.max_size,
                               (Int)tests[i].want_max_size);
    }
    burrow__hpack_encoder_free(e);
    bytes_buffer_free(&buf);
}

static void TestEncoderSetMaxDynamicTableSizeLimit(TestingT *t) {
    HpackEncoder *e = burrow__hpack_new_encoder(heap_allocator(), (IoWriter){0});
    if (e == NULL) {
        testing_t_fatalf_v(t, "NewEncoder failed");
        return;
    }
    /* 4095 < initialHeaderTableSize means maxSize is truncated to 4095. */
    burrow__hpack_encoder_set_max_dynamic_table_size_limit(e, 4095);
    if (e->dyn_tab.max_size != 4095)
        testing_t_errorf_v(t, "e.dynTab.maxSize = %d; want %d",
                           (Int)e->dyn_tab.max_size, (Int)4095);
    if (e->max_size_limit != 4095)
        testing_t_errorf_v(t, "e.maxSizeLimit = %d; want %d", (Int)e->max_size_limit,
                           (Int)4095);
    if (!e->table_size_update)
        testing_t_errorf_v(t, "e.tableSizeUpdate = %t; want %t", e->table_size_update,
                           true);
    /* maxSize will be truncated to maxSizeLimit */
    burrow__hpack_encoder_set_max_dynamic_table_size(e, 16384);
    if (e->dyn_tab.max_size != 4095)
        testing_t_errorf_v(t, "e.dynTab.maxSize = %d; want %d",
                           (Int)e->dyn_tab.max_size, (Int)4095);
    /* 8192 > current maxSizeLimit, so maxSize does not change. */
    burrow__hpack_encoder_set_max_dynamic_table_size_limit(e, 8192);
    if (e->dyn_tab.max_size != 4095)
        testing_t_errorf_v(t, "e.dynTab.maxSize = %d; want %d",
                           (Int)e->dyn_tab.max_size, (Int)4095);
    if (e->max_size_limit != 8192)
        testing_t_errorf_v(t, "e.maxSizeLimit = %d; want %d", (Int)e->max_size_limit,
                           (Int)8192);
    burrow__hpack_encoder_free(e);
}

/* ----------------------------------------------------------- burrow's own */

/* huffman.go's buildRootHuffmanNode, numbering the nodes as they are made,
 * against the trie the generator wrote. */
static void TestHuffmanTrie(TestingT *t) {
    static uint16_t nodes[BURROW__HPACK_TRIE_NODES + 1][256];
    memset(nodes, 0, sizeof nodes);
    int nnodes = 1;
    for (int sym = 0; sym < 256; sym++) {
        uint32_t code = burrow__hpack_huffman_codes[sym];
        unsigned code_len = burrow__hpack_huffman_code_len[sym];
        int cur = 0;
        while (code_len > 8) {
            code_len -= 8;
            Byte i = (Byte)(code >> code_len);
            if (nodes[cur][i] == 0) {
                if (nnodes > BURROW__HPACK_TRIE_NODES) {
                    testing_t_fatalf_v(t, "more than %d nodes",
                                       (Int)BURROW__HPACK_TRIE_NODES);
                    return;
                }
                nodes[cur][i] = (uint16_t)(BURROW__HPACK_TRIE_NODE | nnodes++);
            }
            cur = nodes[cur][i] & 0xFF;
        }
        unsigned shift = 8 - code_len;
        unsigned start = (Byte)(code << shift);
        for (unsigned i = start; i < start + (1U << shift); i++)
            nodes[cur][i] = (uint16_t)(code_len << 8 | (unsigned)sym);
    }
    if (nnodes != BURROW__HPACK_TRIE_NODES)
        testing_t_errorf_v(t, "built %d nodes, want %d", (Int)nnodes,
                           (Int)BURROW__HPACK_TRIE_NODES);
    for (int n = 0; n < BURROW__HPACK_TRIE_NODES; n++)
        for (int i = 0; i < 256; i++)
            if (nodes[n][i] != burrow__hpack_trie[n][i]) {
                testing_t_errorf_v(t, "trie[%d][%d] = %d, built %d", (Int)n, (Int)i,
                                   (Int)burrow__hpack_trie[n][i], (Int)nodes[n][i]);
                return;
            }
}

static void TestHeaderFieldString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str got = field_string(a, pair("foo", "bar"));
    if (!str_eq(got, BURROW_S("header field \"foo\" = \"bar\"")))
        testing_t_errorf_v(t, "String() = %q", got);
    got = field_string(a, field("password", "se\"cret", true));
    if (!str_eq(got,
                BURROW_S("header field \"password\" = \"se\\\"cret\" (sensitive)")))
        testing_t_errorf_v(t, "String() = %q", got);
    if (!burrow__hpack_header_field_is_pseudo(pair(":path", "/")))
        testing_t_errorf_v(t, "IsPseudo(:path) = false");
    if (burrow__hpack_header_field_is_pseudo(pair("path", "/")) ||
        burrow__hpack_header_field_is_pseudo(pair("", "/")))
        testing_t_errorf_v(t, "IsPseudo(path) = true");
    if (burrow__hpack_header_field_size(pair("foo", "bar")) != 38)
        testing_t_errorf_v(t, "Size() = %d, want 38",
                           (Int)burrow__hpack_header_field_size(pair("foo", "bar")));
    arena_free(&ar);
}

/* The messages Go's DecodingError and InvalidIndexError give, and that a
 * retained copy keeps what it is. */
static void TestDecodingErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HpackDecoder *d = burrow__hpack_new_decoder(heap_allocator(), 4095, no_emit, NULL);
    if (d == NULL) {
        testing_t_fatalf_v(t, "NewDecoder failed");
        return;
    }

    Error err = BURROW_NO_ERROR;
    (void)burrow__hpack_decoder_decode_full(d, a, dehex(a, "ff00"), &err);
    Str want = BURROW_S("decoding error: invalid indexed representation index 127");
    if (BURROW_OK(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "index 127: %q, want %q",
                           BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err), want);
    Error inner = BURROW_NO_ERROR;
    Int idx = 0;
    if (!burrow__hpack_error_decoding(err, &inner) ||
        !burrow__hpack_error_invalid_index(inner, &idx) || idx != 127)
        testing_t_errorf_v(t, "index 127: not a DecodingError{InvalidIndexError(127)}");
    else if (!str_eq(error_text(inner),
                     BURROW_S("invalid indexed representation index 127")))
        testing_t_errorf_v(t, "inner = %q", error_text(inner));
    Error kept = error_retain(a, err);
    if (!str_eq(error_text(kept), want) ||
        !burrow__hpack_error_decoding(kept, &inner) ||
        !burrow__hpack_error_invalid_index(inner, &idx) || idx != 127)
        testing_t_errorf_v(t, "retained: %q", error_text(kept));

    (void)burrow__hpack_decoder_decode_full(d, a, dehex(a, "400366"), &err);
    want = BURROW_S("decoding error: truncated headers");
    if (BURROW_OK(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "truncated: %q, want %q",
                           BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err), want);
    kept = error_retain(a, err);
    if (!same_err(kept, err))
        testing_t_errorf_v(t,
                           "a retained truncated headers error is a different error");

    (void)burrow__hpack_decoder_decode_full(d, a, dehex(a, "3fe11f"), &err);
    want = BURROW_S("decoding error: dynamic table size update too large");
    if (BURROW_OK(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "too large: %q, want %q",
                           BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err), want);

    want = BURROW_S("decoding error: varint integer overflow");
    if (!str_eq(error_text(burrow__hpack_err_varint_overflow), want) ||
        !burrow__hpack_error_decoding(burrow__hpack_err_varint_overflow, NULL))
        testing_t_errorf_v(t, "errVarintOverflow = %q, want %q",
                           error_text(burrow__hpack_err_varint_overflow), want);
    burrow__hpack_decoder_free(d);
    arena_free(&ar);
}

/* An allocator that gives out a fixed number of allocations, and counts what
 * is still out. */
typedef struct Budget {
    Alloc *under;
    Int left;
    Int live;
} Budget;

static void *budget_alloc(void *self, size_t size, size_t align) {
    Budget *b = self;
    if (b->left == 0)
        return NULL;
    b->left--;
    void *p = mem_alloc(b->under, size, align);
    if (p != NULL)
        b->live++;
    return p;
}

static void *budget_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    Budget *b = self;
    if (b->left == 0)
        return NULL;
    b->left--;
    return mem_realloc(b->under, p, old, nsz, align);
}

static void budget_free(void *self, void *p, size_t size, size_t align) {
    Budget *b = self;
    b->live--;
    mem_free(b->under, p, size, align);
}

static const AllocVT budget_vt = {
    .alloc = budget_alloc,
    .realloc = budget_realloc,
    .free = budget_free,
};

/* Encodes with every allocation failing in turn. A field that fails is not
 * sent and must leave the encoder in step with a decoder of what was sent,
 * and nothing may be left allocated once the encoder and decoder are freed. */
static void TestOutOfMemory(TestingT *t) {
    static const HpackHeaderField fields[] = {
        P(":method", "GET"),
        P(":authority", "www.example.com"),
        P("custom-key", "custom-value"),
        P("custom-key", "custom-value"),
        P("cache-control", "no-cache"),
        P("custom-key", "other-value"),
        P("x-long", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        P("custom-key", "custom-value"),
    };
    enum { NF = sizeof fields / sizeof fields[0] };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int n = 0; n < 200; n++) {
        Budget b = {heap_allocator(), n, 0};
        Alloc ba = {.vt = &budget_vt, .self = &b};
        BytesBuffer buf = BYTES_BUFFER(heap_allocator());
        HpackEncoder *e =
            burrow__hpack_new_encoder(&ba, bytes_buffer_as_io_writer(&buf));
        if (e == NULL) {
            bytes_buffer_free(&buf);
            continue;
        }
        burrow__hpack_encoder_set_max_dynamic_table_size(e, 200);
        HpackHeaderField sent[NF];
        Int nsent = 0;
        for (int i = 0; i < NF; i++) {
            Error err = burrow__hpack_encoder_write_field(e, fields[i]);
            if (BURROW_OK(err))
                sent[nsent++] = fields[i];
            else if (!same_err(err, burrow_err_out_of_memory))
                testing_t_errorf_v(t, "budget %d: WriteField: %s", n, error_text(err));
        }
        burrow__hpack_encoder_free(e);

        Budget db = {heap_allocator(), n, 0};
        Alloc dba = {.vt = &budget_vt, .self = &db};
        Got got = {0};
        got.a = a;
        HpackDecoder *d = burrow__hpack_new_decoder(&dba, 4096, got_emit, &got);
        if (d != NULL) {
            Error err = BURROW_NO_ERROR;
            (void)burrow__hpack_decoder_write(d, bytes_buffer_bytes(&buf), &err);
            burrow__hpack_decoder_free(d);
        }
        /* And again with all the memory it wants, to check what was sent. */
        HpackDecoder *full =
            burrow__hpack_new_decoder(heap_allocator(), 4096, got_emit, &got);
        if (full == NULL) {
            testing_t_fatalf_v(t, "NewDecoder failed");
            return;
        }
        got.n = 0;
        Error err = BURROW_NO_ERROR;
        (void)burrow__hpack_decoder_write(full, bytes_buffer_bytes(&buf), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "budget %d: decoding what was sent: %s", n,
                               error_text(err));
        else if (!fields_eq(got.f, got.n, sent, nsent))
            testing_t_errorf_v(t, "budget %d: decoded %s; sent %s", n,
                               fields_string(a, got.f, got.n),
                               fields_string(a, sent, nsent));
        burrow__hpack_decoder_free(full);
        bytes_buffer_free(&buf);
        if (b.live != 0 || db.live != 0)
            testing_t_errorf_v(t, "budget %d: %d and %d allocations left", n, b.live,
                               db.live);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestHeaderFieldTable)                                                            \
    X(TestHeaderFieldTable_LookupMapEviction)                                          \
    X(TestStaticTable)                                                                 \
    X(TestDynamicTableAt)                                                              \
    X(TestDynamicTableSizeEvict)                                                       \
    X(TestDecoderDecode)                                                               \
    X(TestDecodeC3_NoHuffman)                                                          \
    X(TestDecodeC4_Huffman)                                                            \
    X(TestDecodeC5_ResponsesNoHuff)                                                    \
    X(TestDecodeC6_ResponsesHuffman)                                                   \
    X(TestHuffmanDecodeExcessPadding)                                                  \
    X(TestHuffmanDecodeEOS)                                                            \
    X(TestHuffmanDecodeMaxLengthOnTrailingByte)                                        \
    X(TestHuffmanDecodeCorruptPadding)                                                 \
    X(TestHuffmanDecode)                                                               \
    X(TestAppendHuffmanString)                                                         \
    X(TestHuffmanMaxStrLen)                                                            \
    X(TestHuffmanRoundtripStress)                                                      \
    X(TestHuffmanDecodeFuzz)                                                           \
    X(TestReadVarInt)                                                                  \
    X(TestHuffmanFuzzCrash)                                                            \
    X(TestEmitEnabled)                                                                 \
    X(TestSlowIncrementalDecode)                                                       \
    X(TestSaveBufLimit)                                                                \
    X(TestDynamicSizeUpdate)                                                           \
    X(TestEncoderTableSizeUpdate)                                                      \
    X(TestEncoderWriteField)                                                           \
    X(TestEncoderSearchTable)                                                          \
    X(TestAppendVarInt)                                                                \
    X(TestAppendHpackString)                                                           \
    X(TestAppendIndexed)                                                               \
    X(TestAppendNewName)                                                               \
    X(TestAppendIndexedName)                                                           \
    X(TestAppendTableSize)                                                             \
    X(TestEncoderSetMaxDynamicTableSize)                                               \
    X(TestEncoderSetMaxDynamicTableSizeLimit)                                          \
    X(TestHuffmanTrie)                                                                 \
    X(TestHeaderFieldString)                                                           \
    X(TestDecodingErrors)                                                              \
    X(TestOutOfMemory)

TESTING_MAIN(TESTS)
