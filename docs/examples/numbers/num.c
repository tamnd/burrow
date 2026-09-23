#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"

static void operators(void) {
    uint32_t h = 2166136261u;
    Byte b = 'x';
    double total = 7;
    Int n = 2;
    int64_t wide = 300;
    // doc: operators
    h = (h * 16777619u) ^ b;        /* fine, wraps the same in both */
    double avg = total / (double)n; /* fine */
    int8_t low = (int8_t)wide;      /* fine, truncates */
    // doc: end
    printf("h %" PRIu32 ", avg %.1f, low %d\n", h, avg, low);
}

static void wrapping(Int a, Int b) {
    // doc: wrap
    Int n = int_add(a, b); /* wraps, like Go */
    Int m = a + b;         /* undefined behaviour if it overflows */
    // doc: end
    printf("%lld %lld\n", (long long)n, (long long)m);
    printf("INT64_MAX + 1 is %lld\n", (long long)int_add(INT64_MAX, 1));
}

static void dividing(Int a, Int b) {
    // doc: divide
    Int q = int_div(a, b);
    Int r = int_mod(a, b);
    // doc: end
    printf("%lld / %lld is %lld remainder %lld\n", (long long)a, (long long)b,
           (long long)q, (long long)r);
}

static void shifting(Uint hash, Int value, Int n) {
    // doc: shift
    Uint h = uint_shl(hash, 13);
    Int s = int_shr(value, n);
    // doc: end
    printf("%llu %lld\n", (unsigned long long)h, (long long)s);
}

static void from_float(double f) {
    // doc: float
    Int n = int_from_float64(f);
    // doc: end
    printf("%g becomes %lld\n", f, (long long)n);
}

static void generic(Int subtotal, Int tax, uint32_t hash) {
    // doc: generic
    Int total = BURROW_ADD(subtotal, tax);
    uint32_t h = BURROW_SHL(hash, 5);
    // doc: end
    printf("%lld %" PRIu32 "\n", (long long)total, h);
}

static void bit_twiddling(uint64_t x, uint64_t y) {
    // doc: bits
    Int width = bits_len64(x);               /* 0 for 0, like Go */
    Int set = bits_ones_count64(x);          /* how many bits are 1 */
    uint64_t lo, hi = bits_mul64(x, y, &lo); /* the full 128 bit product */
    // doc: end
    printf("len %lld, ones %lld, product %016" PRIx64 "%016" PRIx64 "\n",
           (long long)width, (long long)set, hi, lo);
}

int main(void) {
    operators();
    wrapping(40, 2);
    dividing(-7, 2);
    dividing(INT64_MIN, -1);
    shifting(1, -8, 100);
    from_float(1e300);
    from_float(-2.9);
    generic(40, 2, 1);
    bit_twiddling(0xff00, UINT64_MAX);
    return 0;
}

/* Output:
h 84696423, avg 3.5, low 44
42 42
INT64_MAX + 1 is -9223372036854775808
-7 / 2 is -3 remainder -1
-9223372036854775808 / -1 is -9223372036854775808 remainder 0
8192 -1
1e+300 becomes 9223372036854775807
-2.9 becomes -2
42 32
len 16, ones 8, product 000000000000feffffffffffffff0100
*/
