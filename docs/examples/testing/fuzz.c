#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

// doc: fuzz
/* Reverses s byte by byte, which is wrong for any rune longer than a byte. */
static Str reverse(Alloc *a, Str s) {
    Byte *p = (Byte *)mem_alloc(a, (size_t)s.len + 1, 1);
    for (Int i = 0; i < s.len; i++)
        p[i] = s.p[s.len - 1 - i];
    return str_from_bytes(p, s.len);
}

static void fuzz_reverse(void *env, TestingT *t, Slice args) {
    (void)env;
    Str s = testing_fuzz_arg(args, 0, Str);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str rev = reverse(arena_allocator(&ar), s);
    if (utf8_valid_string(s) && !utf8_valid_string(rev))
        testing_t_errorf_v(t, "reverse(%q) = %q, not valid UTF-8", s, rev);
    arena_free(&ar);
}

/* Takes a []byte and an int, and skips the inputs it has no use for. */
static void fuzz_count(void *env, TestingT *t, Slice args) {
    (void)env;
    Bytes b = testing_fuzz_arg(args, 0, Bytes);
    Int max = testing_fuzz_arg(args, 1, Int);
    if (b.len > max)
        testing_t_skip_v(t, "longer than", max);
    Int n = utf8_rune_count(b);
    if (n > b.len)
        testing_t_errorf_v(t, "%d runes in %d bytes", n, b.len);
}

static void FuzzReverse(TestingF *f) {
    testing_f_add_v(f, "gopher");
    testing_f_add_v(f, "h\xc3\xa9llo");
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, fuzz_reverse, NULL), TYPE_STRING);
}

static void FuzzCount(TestingF *f) {
    Byte data[] = {'a', 0xE2, 0x98, 0xBA, 'b'};
    testing_f_add_v(f, slice_from(data, 5, 5, TYPE_BYTE), 16);
    testing_f_add_v(f, slice_from(data, 5, 5, TYPE_BYTE), 2);
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, fuzz_count, NULL), TYPE_BYTES,
                     TYPE_INT);
}

#define TESTS(X) X(FuzzReverse) X(FuzzCount)
// doc: end

/* Prints the status and exits 0, so that the example itself counts as
 * passing. */
static int TestMain(TestingM *m) {
    int code = testing_m_run(m);
    printf("exit status %d\n", code);
    return 0;
}

TESTING_MAIN_WITH(TestMain, TESTS)

/* Output:
--- FAIL: FuzzReverse (0.00s)
    --- FAIL: FuzzReverse/seed#1 (0.00s)
        fuzz.c:22: reverse("héllo") = "oll\xa9\xc3h", not valid UTF-8
FAIL
exit status 1
*/
