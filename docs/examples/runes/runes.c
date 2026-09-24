#include <stdio.h>

#include "burrow/burrow.h"

BURROW_SENTINEL_ERROR(err_bad_request, "bad request");

static void loop(Str s) {
    // doc: loop
    Int i;
    Rune r;
    for (StrIter it = str_runes(s); str_next_rune(&it, &i, &r);)
        printf("%lld: %lx\n", (long long)i, (unsigned long)r);
    // doc: end
}

static void count(Str s) {
    // doc: count
    Int count = 0;
    for (StrIter it = str_runes(s); str_next_rune(&it, NULL, NULL);)
        count++;
    // doc: end
    printf("%lld runes, and utf8_rune_count_in_string agrees: %s\n", (long long)count,
           count == utf8_rune_count_in_string(s) ? "yes" : "no");
}

static void lines_in(Str s) {
    Int lines = 0;
    // doc: bytes
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] == '\n')
            lines++;
    // doc: end
    printf("%lld lines\n", (long long)lines);
}

static void decode(Str s) {
    // doc: decode
    Int size;
    Rune r = utf8_decode_rune_in_string(s, &size); /* r, size := ... */
    r = utf8_decode_rune_in_string(s, NULL);       /* r, _ = ... */
    // doc: end
    printf("%lx is %lld bytes\n", (unsigned long)r, (long long)size);
}

// doc: validate
static Error accept(Str body) {
    if (!utf8_valid_string(body))
        return err_bad_request;
    return BURROW_NO_ERROR;
}
// doc: end

static void encode(Rune r) {
    // doc: encode
    Byte buf[UTF8_UTF_MAX];
    Slice out = slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE);
    Int n = utf8_encode_rune(out, r);
    // doc: end
    printf("%lx encodes as", (unsigned long)r);
    for (Int i = 0; i < n; i++)
        printf(" %02x", buf[i]);
    printf("\n");
}

static void append(Alloc *a, const Rune *runes, Int n) {
    // doc: append
    Slice out = slice_nil(TYPE_BYTE);
    for (Int i = 0; i < n; i++)
        out = utf8_append_rune(a, out, runes[i]);
    // doc: end
    printf("appended %lld runes into %lld bytes: %.*s\n", (long long)n,
           (long long)out.len, (int)out.len, (const char *)out.p);
}

static void back_up(const Byte *buf, Int i) {
    Int from = i;
    // doc: start
    while (i > 0 && !utf8_rune_start(buf[i]))
        i--;
    // doc: end
    printf("from byte %lld back to %lld\n", (long long)from, (long long)i);
}

static void wide(Alloc *a) {
    // doc: utf16
    Rune in[] = {'h', 'i', 0x1F600};
    Slice units = utf16_encode(a, slice_from(in, 3, 3, TYPE_RUNE));
    Slice back = utf16_decode(a, units);
    // doc: end
    const uint16_t *u = units.p;
    printf("%lld runes are %lld units:", (long long)3, (long long)units.len);
    for (Int i = 0; i < units.len; i++)
        printf(" %04x", (unsigned)u[i]);
    printf(", and decode gives back %lld runes\n", (long long)back.len);

    // doc: pair
    Rune lo;
    Rune hi = utf16_encode_rune(0x1F600, &lo);
    Rune r = utf16_decode_rune(hi, lo); /* 0x1F600 again */
    // doc: end
    printf("%lx is the pair %lx %lx, and back is %lx\n", (unsigned long)0x1F600,
           (unsigned long)hi, (unsigned long)lo, (unsigned long)r);
}

int main(void) {
    Str s = BURROW_S("héllo, 世界");
    loop(BURROW_S("aé世"));
    count(s);
    lines_in(BURROW_S("one\ntwo\nthree\n"));
    decode(BURROW_S("世界"));

    Error err = accept(BURROW_S("\xc0\xaf"));
    printf("an overlong slash is %s\n", BURROW_FAILED(err) ? "refused" : "accepted");
    err = accept(s);
    printf("the greeting is %s\n", BURROW_FAILED(err) ? "refused" : "accepted");

    encode(0x4e16);
    encode(0xd800);

    Arena arena;
    arena_init(&arena, heap_allocator(), 0);
    Rune runes[] = {'h', 0xe9, 0x4e16};
    append(arena_allocator(&arena), runes, 3);
    wide(arena_allocator(&arena));
    arena_free(&arena);

    back_up(s.p, 10);
    return 0;
}

/* Output:
0: 61
1: e9
3: 4e16
9 runes, and utf8_rune_count_in_string agrees: yes
3 lines
4e16 is 3 bytes
an overlong slash is refused
the greeting is accepted
4e16 encodes as e4 b8 96
d800 encodes as ef bf bd
appended 3 runes into 6 bytes: hé世
3 runes are 4 units: 0068 0069 d83d de00, and decode gives back 3 runes
1f600 is the pair d83d de00, and back is 1f600
from byte 10 back to 8
*/
