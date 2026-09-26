/* Derived from Go's src/sort/sort_test.go and src/sort/search_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/sort/internal.h"

#include "burrow/burrow.h"
#include "burrow/math.h"
#include "burrow/math/bits.h"
#include "burrow/sort.h"

#include "burrow/mem.h"
#include <math.h>
#include <string.h>

/* A splitmix64 generator standing in for math/rand until that is ported. */
static uint64_t rng_state = 0x9e3779b97f4a7c15u;

static uint64_t rng_next(void) {
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15u);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
    return z ^ (z >> 31);
}

static Int rng_intn(Int n) {
    return (Int)(rng_next() % (uint64_t)n);
}

static Int ints[] = {74, 59, 238, -784, 9845, 959, 905, 0, 0, 42, 7586, -5467984, 7586};
static double float64s[14];
static Str strings_data[] = {BURROW_S_INIT(""),         BURROW_S_INIT("Hello"),
                             BURROW_S_INIT("foo"),      BURROW_S_INIT("bar"),
                             BURROW_S_INIT("foo"),      BURROW_S_INIT("f00"),
                             BURROW_S_INIT("%*&^*&^&"), BURROW_S_INIT("***")};

#define NELEM(a) ((Int)(sizeof(a) / sizeof((a)[0])))

static void init_float64s(void) {
    double v[] = {74.3,      59.0,       math_inf(1), 238.2,        -784.0,
                  2.3,       math_nan(), math_nan(),  math_inf(-1), 9845.768,
                  -959.7485, 905,        7.8,         7.8};
    memcpy(float64s, v, sizeof v);
}

static Slice int_slice(Int *p, Int n) {
    return slice_from(p, n, n, TYPE_INT);
}

static Slice float64_slice(double *p, Int n) {
    return slice_from(p, n, n, TYPE_FLOAT64);
}

static Slice string_slice(Str *p, Int n) {
    return slice_from(p, n, n, TYPE_STRING);
}

static void TestSortIntSlice(TestingT *t) {
    Int data[NELEM(ints)];
    memcpy(data, ints, sizeof ints);
    SortIntSlice a = int_slice(data, NELEM(data));
    sort_sort(sort_int_slice_as_sort_interface(&a));
    if (!sort_is_sorted(sort_int_slice_as_sort_interface(&a)))
        testing_t_errorf_v(t, "sorted ints: got a slice that is not sorted");
}

static void TestSortFloat64Slice(TestingT *t) {
    init_float64s();
    double data[NELEM(float64s)];
    memcpy(data, float64s, sizeof float64s);
    SortFloat64Slice a = float64_slice(data, NELEM(data));
    sort_sort(sort_float64_slice_as_sort_interface(&a));
    if (!sort_is_sorted(sort_float64_slice_as_sort_interface(&a)))
        testing_t_errorf_v(t, "sorted float64s: got a slice that is not sorted");
}

/* Go compares Sort with slices.Sort. Here the second is sort_float64s, which is
 * the same algorithm as slices.Sort. NaNs compare equal for this. */
static void TestSortFloat64sCompareSlicesSort(TestingT *t) {
    init_float64s();
    double slice1[NELEM(float64s)], slice2[NELEM(float64s)];
    memcpy(slice1, float64s, sizeof float64s);
    memcpy(slice2, float64s, sizeof float64s);

    SortFloat64Slice a = float64_slice(slice1, NELEM(slice1));
    sort_sort(sort_float64_slice_as_sort_interface(&a));
    sort_float64s(float64_slice(slice2, NELEM(slice2)));

    for (Int i = 0; i < NELEM(slice1); i++) {
        bool both_nan = math_is_nan(slice1[i]) && math_is_nan(slice2[i]);
        if (!both_nan && slice1[i] != slice2[i])
            testing_t_errorf_v(t,
                               "mismatch between Sort and slices.Sort at %d: %g and %g",
                               i, slice1[i], slice2[i]);
    }
}

static void TestSortStringSlice(TestingT *t) {
    Str data[NELEM(strings_data)];
    memcpy(data, strings_data, sizeof strings_data);
    SortStringSlice a = string_slice(data, NELEM(data));
    sort_sort(sort_string_slice_as_sort_interface(&a));
    if (!sort_is_sorted(sort_string_slice_as_sort_interface(&a)))
        testing_t_errorf_v(t, "sorted strings: got a slice that is not sorted");
}

static void TestInts(TestingT *t) {
    Int data[NELEM(ints)];
    memcpy(data, ints, sizeof ints);
    sort_ints(int_slice(data, NELEM(data)));
    if (!sort_ints_are_sorted(int_slice(data, NELEM(data))))
        testing_t_errorf_v(t, "Ints did not sort");
}

static void TestFloat64s(TestingT *t) {
    init_float64s();
    double data[NELEM(float64s)];
    memcpy(data, float64s, sizeof float64s);
    sort_float64s(float64_slice(data, NELEM(data)));
    if (!sort_float64s_are_sorted(float64_slice(data, NELEM(data))))
        testing_t_errorf_v(t, "Float64s did not sort");
}

static void TestStrings(TestingT *t) {
    Str data[NELEM(strings_data)];
    memcpy(data, strings_data, sizeof strings_data);
    sort_strings(string_slice(data, NELEM(data)));
    if (!sort_strings_are_sorted(string_slice(data, NELEM(data))))
        testing_t_errorf_v(t, "Strings did not sort");
}

static bool str_slice_less(void *env, Int i, Int j) {
    const Str *data = env;
    return str_cmp(data[i], data[j]) < 0;
}

static void TestSlice(TestingT *t) {
    Str data[NELEM(strings_data)];
    memcpy(data, strings_data, sizeof strings_data);
    SortLessFunc less = BURROW_FN(SortLessFunc, str_slice_less, data);
    sort_slice(string_slice(data, NELEM(data)), less);
    if (!sort_slice_is_sorted(string_slice(data, NELEM(data)), less))
        testing_t_errorf_v(t, "Slice did not sort");
}

static void TestSortLarge_Random(TestingT *t) {
    Int n = 1000000;
    if (testing_short())
        n /= 100;
    Int *data = BURROW_NEW_N(heap_allocator(), Int, (size_t)n);
    CHECK(data != NULL);
    for (Int i = 0; i < n; i++)
        data[i] = rng_intn(100);
    Slice s = int_slice(data, n);
    if (sort_ints_are_sorted(s))
        testing_t_fatalf_v(t, "terrible rand.rand");
    sort_ints(s);
    if (!sort_ints_are_sorted(s))
        testing_t_errorf_v(t, "sort didn't sort - 1M ints");
    mem_free(heap_allocator(), data, (size_t)n * sizeof *data, _Alignof(Int));
}

static void TestReverseSortIntSlice(TestingT *t) {
    Int data[NELEM(ints)], data1[NELEM(ints)];
    memcpy(data, ints, sizeof ints);
    memcpy(data1, ints, sizeof ints);
    SortIntSlice a = int_slice(data, NELEM(data));
    sort_sort(sort_int_slice_as_sort_interface(&a));
    SortIntSlice r = int_slice(data1, NELEM(data1));
    SortReverse rev = sort_reverse(sort_int_slice_as_sort_interface(&r));
    sort_sort(sort_reverse_as_sort_interface(&rev));
    Int n = NELEM(data);
    for (Int i = 0; i < n; i++) {
        if (data[i] != data1[n - 1 - i])
            testing_t_errorf_v(t, "reverse sort didn't sort");
        if (i > n / 2)
            break;
    }
}

static void TestBreakPatterns(TestingT *t) {
    /* Special slice used to trigger breakPatterns. */
    Int data[30];
    for (Int i = 0; i < 30; i++)
        data[i] = 10;
    data[(30 / 4) * 1] = 0;
    data[(30 / 4) * 2] = 1;
    data[(30 / 4) * 3] = 2;
    SortIntSlice s = int_slice(data, 30);
    sort_sort(sort_int_slice_as_sort_interface(&s));
    CHECK(sort_ints_are_sorted(s));
}

static void TestReverseRange(TestingT *t) {
    Int data[] = {1, 2, 3, 4, 5, 6, 7};
    SortIntSlice s = int_slice(data, 7);
    burrow__sort_reverse_range(sort_int_slice_as_sort_interface(&s), 0, 7);
    for (Int i = 6; i > 0; i--) {
        if (data[i] > data[i - 1])
            testing_t_fatalf_v(t, "reverseRange didn't work");
    }

    Int data1[] = {1, 2, 3, 4, 5, 6, 7};
    Int data2[] = {1, 2, 5, 4, 3, 6, 7};
    SortIntSlice s1 = int_slice(data1, 7);
    burrow__sort_reverse_range(sort_int_slice_as_sort_interface(&s1), 2, 5);
    for (Int i = 0; i < 7; i++) {
        if (data1[i] != data2[i])
            testing_t_fatalf_v(t, "reverseRange didn't work");
    }
}

/* nonDeterministicTestingData: a less that answers at random. */
typedef struct NonDeterministic {
    TestingT *t;
} NonDeterministic;

static Int nd_len(void *self) {
    (void)self;
    return 500;
}

static bool nd_less(void *self, Int i, Int j) {
    NonDeterministic *d = self;
    if (i < 0 || j < 0 || i >= 500 || j >= 500)
        testing_t_fatalf_v(d->t, "nondeterministic comparison out of bounds");
    return (rng_next() >> 40) < (1u << 23);
}

static void nd_swap(void *self, Int i, Int j) {
    NonDeterministic *d = self;
    if (i < 0 || j < 0 || i >= 500 || j >= 500)
        testing_t_fatalf_v(d->t, "nondeterministic comparison out of bounds");
}

static const SortInterfaceVT nd_vt = {NULL, nd_len, nd_less, nd_swap};

/* sort_sort must not go out of bounds when less answers inconsistently.
 * See https://golang.org/issue/14377. */
static void TestNonDeterministicComparison(TestingT *t) {
    NonDeterministic td = {t};
    SortInterface data = {&nd_vt, &td};
    for (Int i = 0; i < 10; i++)
        sort_sort(data);
}

/* testingData, which counts compares and swaps and fails after too many. */
typedef struct TestingData {
    char desc[96];
    TestingT *t;
    Int *data;
    Int n;
    Int maxswap; /* number of swaps allowed */
    Int ncmp, nswap;
} TestingData;

static Int td_len(void *self) {
    return ((TestingData *)self)->n;
}

static bool td_less(void *self, Int i, Int j) {
    TestingData *d = self;
    d->ncmp++;
    return d->data[i] < d->data[j];
}

static void td_swap(void *self, Int i, Int j) {
    TestingData *d = self;
    if (d->nswap >= d->maxswap)
        testing_t_fatalf_v(d->t, "%s: used %d swaps sorting slice of %d", d->desc,
                           d->nswap, d->n);
    d->nswap++;
    Int tmp = d->data[i];
    d->data[i] = d->data[j];
    d->data[j] = tmp;
}

static const SortInterfaceVT td_vt = {NULL, td_len, td_less, td_swap};

static Int lg(Int n) {
    Int i = 0;
    while (((Int)1 << i) < n)
        i++;
    return i;
}

enum { SAWTOOTH, RAND, STAGGER, PLATEAU, SHUFFLE, NDIST };
enum { COPY, REVERSE, REVERSE_FIRST_HALF, REVERSE_SECOND_HALF, SORTED, DITHER, NMODE };

typedef enum { ALGO_SORT, ALGO_HEAPSORT, ALGO_STABLE } Algo;

static void run_algo(Algo algo, SortInterface data) {
    switch (algo) {
    case ALGO_SORT:
        sort_sort(data);
        break;
    case ALGO_HEAPSORT:
        burrow__sort_heapsort(data);
        break;
    case ALGO_STABLE:
        sort_stable(data);
        break;
    default:
        break;
    }
}

static void test_bentley_mcilroy(TestingT *t, Algo algo) {
    Int sizes_long[] = {100, 1023, 1024, 1025};
    Int sizes_short[] = {100, 127, 128, 129};
    const Int *sizes = testing_short() ? sizes_short : sizes_long;
    static const char *const dists[] = {"sawtooth", "rand", "stagger", "plateau",
                                        "shuffle"};
    static const char *const modes[] = {"copy",     "reverse", "reverse1",
                                        "reverse2", "sort",    "dither"};
    static Int tmp1[1025], tmp2[1025];
    for (Int si = 0; si < 4; si++) {
        Int n = sizes[si];
        for (Int m = 1; m < 2 * n; m *= 2) {
            for (int dist = 0; dist < NDIST; dist++) {
                Int j = 0;
                Int k = 1;
                Int *data = tmp1;
                for (Int i = 0; i < n; i++) {
                    switch (dist) {
                    case SAWTOOTH:
                        data[i] = i % m;
                        break;
                    case RAND:
                        data[i] = rng_intn(m);
                        break;
                    case STAGGER:
                        data[i] = (i * m + i) % n;
                        break;
                    case PLATEAU:
                        data[i] = i < m ? i : m;
                        break;
                    case SHUFFLE:
                        if (rng_intn(m) != 0) {
                            j += 2;
                            data[i] = j;
                        } else {
                            k += 2;
                            data[i] = k;
                        }
                        break;
                    default:
                        break;
                    }
                }

                Int *mdata = tmp2;
                for (int mode = 0; mode < NMODE; mode++) {
                    switch (mode) {
                    case COPY:
                        for (Int i = 0; i < n; i++)
                            mdata[i] = data[i];
                        break;
                    case REVERSE:
                        for (Int i = 0; i < n; i++)
                            mdata[i] = data[n - i - 1];
                        break;
                    case REVERSE_FIRST_HALF:
                        for (Int i = 0; i < n / 2; i++)
                            mdata[i] = data[n / 2 - i - 1];
                        for (Int i = n / 2; i < n; i++)
                            mdata[i] = data[i];
                        break;
                    case REVERSE_SECOND_HALF:
                        for (Int i = 0; i < n / 2; i++)
                            mdata[i] = data[i];
                        for (Int i = n / 2; i < n; i++)
                            mdata[i] = data[n - (i - n / 2) - 1];
                        break;
                    case SORTED:
                        for (Int i = 0; i < n; i++)
                            mdata[i] = data[i];
                        /* Ints is known to be correct because mode Sort runs
                         * after mode _Copy. */
                        sort_ints(int_slice(mdata, n));
                        break;
                    case DITHER:
                        for (Int i = 0; i < n; i++)
                            mdata[i] = data[i] + i % 5;
                        break;
                    default:
                        break;
                    }

                    TestingData d = {{0}, t, mdata, n, 0, 0, 0};
                    snprintf(d.desc, sizeof d.desc, "n=%lld m=%lld dist=%s mode=%s",
                             (long long)n, (long long)m, dists[dist], modes[mode]);
                    d.maxswap = algo == ALGO_STABLE ? n * lg(n) * lg(n) / 3
                                                    : n * lg(n) * 12 / 10;
                    run_algo(algo, (SortInterface){&td_vt, &d});

                    if (!sort_ints_are_sorted(int_slice(mdata, n)))
                        testing_t_fatalf_v(t, "%s: ints not sorted", d.desc);
                }
            }
        }
    }
}

static void TestSortBM(TestingT *t) {
    test_bentley_mcilroy(t, ALGO_SORT);
}

static void TestHeapsortBM(TestingT *t) {
    test_bentley_mcilroy(t, ALGO_HEAPSORT);
}

static void TestStableBM(TestingT *t) {
    test_bentley_mcilroy(t, ALGO_STABLE);
}

/* This is based on the "antiquicksort" implementation by M. Douglas McIlroy.
 * See https://www.cs.dartmouth.edu/~doug/mdmspe.pdf for more info. */
typedef struct Adversary {
    TestingT *t;
    Int *data; /* item values, initialized to special gas value and changed by Less */
    Int n;
    Int maxcmp;    /* number of comparisons allowed */
    Int ncmp;      /* number of comparisons (calls to Less) */
    Int nsolid;    /* number of elements that have been set to non-gas values */
    Int candidate; /* guess at current pivot */
    Int gas;       /* special value for unset elements, higher than everything else */
} Adversary;

static Int adv_len(void *self) {
    return ((Adversary *)self)->n;
}

static bool adv_less(void *self, Int i, Int j) {
    Adversary *d = self;
    if (d->ncmp >= d->maxcmp)
        testing_t_fatalf_v(d->t,
                           "used %d comparisons sorting adversary data with size %d",
                           d->ncmp, d->n);
    d->ncmp++;

    if (d->data[i] == d->gas && d->data[j] == d->gas) {
        if (i == d->candidate) {
            /* freeze i */
            d->data[i] = d->nsolid;
            d->nsolid++;
        } else {
            /* freeze j */
            d->data[j] = d->nsolid;
            d->nsolid++;
        }
    }

    if (d->data[i] == d->gas)
        d->candidate = i;
    else if (d->data[j] == d->gas)
        d->candidate = j;

    return d->data[i] < d->data[j];
}

static void adv_swap(void *self, Int i, Int j) {
    Adversary *d = self;
    Int tmp = d->data[i];
    d->data[i] = d->data[j];
    d->data[j] = tmp;
}

static const SortInterfaceVT adv_vt = {NULL, adv_len, adv_less, adv_swap};

static void TestAdversary(TestingT *t) {
    enum {
        size = 10000
    }; /* large enough to distinguish between O(n^2) and O(n*log(n)) */
    static Int data[size];
    Int gas = size - 1;
    for (Int i = 0; i < size; i++)
        data[i] = gas;
    /* the factor 4 was found by trial and error */
    Adversary d = {t, data, size, size * lg(size) * 4, 0, 0, 0, gas};
    sort_sort((SortInterface){&adv_vt, &d}); /* This should degenerate to heapsort. */
    /* Check data is fully populated and sorted. */
    for (Int i = 0; i < size; i++) {
        if (data[i] != i)
            testing_t_fatalf_v(t, "adversary data not fully sorted");
    }
}

static void TestStableInts(TestingT *t) {
    Int data[NELEM(ints)];
    memcpy(data, ints, sizeof ints);
    SortIntSlice s = int_slice(data, NELEM(data));
    sort_stable(sort_int_slice_as_sort_interface(&s));
    if (!sort_ints_are_sorted(s))
        testing_t_errorf_v(t, "Stable did not sort ints");
}

/* intPairs compare on a only. */
typedef struct IntPair {
    Int a, b;
} IntPair;

typedef struct IntPairs {
    IntPair *p;
    Int n;
} IntPairs;

static Int pairs_len(void *self) {
    return ((IntPairs *)self)->n;
}

static bool pairs_less(void *self, Int i, Int j) {
    IntPairs *d = self;
    return d->p[i].a < d->p[j].a;
}

static void pairs_swap(void *self, Int i, Int j) {
    IntPairs *d = self;
    IntPair tmp = d->p[i];
    d->p[i] = d->p[j];
    d->p[j] = tmp;
}

static const SortInterfaceVT pairs_vt = {NULL, pairs_len, pairs_less, pairs_swap};

/* Record initial order in B. */
static void pairs_init_b(IntPairs *d) {
    for (Int i = 0; i < d->n; i++)
        d->p[i].b = i;
}

/* InOrder checks if a-equal elements were not reordered. */
static bool pairs_in_order(const IntPairs *d) {
    Int last_a = -1, last_b = 0;
    for (Int i = 0; i < d->n; i++) {
        if (last_a != d->p[i].a) {
            last_a = d->p[i].a;
            last_b = d->p[i].b;
            continue;
        }
        if (d->p[i].b <= last_b)
            return false;
        last_b = d->p[i].b;
    }
    return true;
}

static void TestStability(TestingT *t) {
    Int n = 100000, m = 1000;
    if (testing_short()) {
        n = 1000;
        m = 100;
    }
    IntPairs data = {BURROW_NEW_N(heap_allocator(), IntPair, (size_t)n), n};
    CHECK(data.p != NULL);
    SortInterface si = {&pairs_vt, &data};

    /* random distribution */
    for (Int i = 0; i < n; i++)
        data.p[i].a = rng_intn(m);
    if (sort_is_sorted(si))
        testing_t_fatalf_v(t, "terrible rand.rand");
    pairs_init_b(&data);
    sort_stable(si);
    if (!sort_is_sorted(si))
        testing_t_errorf_v(t, "Stable didn't sort %d ints", n);
    if (!pairs_in_order(&data))
        testing_t_errorf_v(t, "Stable wasn't stable on %d ints", n);

    /* already sorted */
    pairs_init_b(&data);
    sort_stable(si);
    if (!sort_is_sorted(si))
        testing_t_errorf_v(t, "Stable shuffled sorted %d ints (order)", n);
    if (!pairs_in_order(&data))
        testing_t_errorf_v(t, "Stable shuffled sorted %d ints (stability)", n);

    /* sorted reversed */
    for (Int i = 0; i < n; i++)
        data.p[i].a = n - i;
    pairs_init_b(&data);
    sort_stable(si);
    if (!sort_is_sorted(si))
        testing_t_errorf_v(t, "Stable didn't sort %d ints", n);
    if (!pairs_in_order(&data))
        testing_t_errorf_v(t, "Stable wasn't stable on %d ints", n);
    mem_free(heap_allocator(), data.p, (size_t)n * sizeof(IntPair), _Alignof(IntPair));
}

/* ------------------------------------------------------------ search_test.go */

typedef struct SearchData {
    const Int *a;
    Int x;
} SearchData;

static bool search_ge(void *env, Int i) {
    const SearchData *d = env;
    return d->a[i] >= d->x;
}

static Int data[] = {-10, -5, 0, 1, 2, 3, 5, 7, 11, 100, 100, 100, 1000, 10000};

static bool at_least(void *env, Int i) {
    return i >= *(const Int *)env;
}

static bool always(void *env, Int i) {
    (void)env;
    (void)i;
    return true;
}

static bool never(void *env, Int i) {
    (void)env;
    (void)i;
    return false;
}

static bool descending_a(void *env, Int i) {
    static const Int d[] = {99, 99, 59, 42, 7, 0, -1, -1};
    (void)env;
    return d[i] <= 7;
}

static bool descending_7(void *env, Int i) {
    (void)env;
    return 1000000000 - i <= 7;
}

static void TestSearch(TestingT *t) {
    static Int one = 1, n991 = 991;
    SearchData sd[] = {{data, -20}, {data, -10}, {data, -9},    {data, -6},
                       {data, -5},  {data, 3},   {data, 11},    {data, 99},
                       {data, 100}, {data, 101}, {data, 10000}, {data, 10001}};
    const Int nd = NELEM(data);
    struct {
        const char *name;
        Int n;
        SortSearchFunc f;
        Int i;
    } tests[] = {
        {"empty", 0, {NULL, NULL}, 0},
        {"1 1", 1, BURROW_FN(SortSearchFunc, at_least, &one), 1},
        {"1 true", 1, BURROW_FN(SortSearchFunc, always, NULL), 0},
        {"1 false", 1, BURROW_FN(SortSearchFunc, never, NULL), 1},
        {"1e9 991", 1000000000, BURROW_FN(SortSearchFunc, at_least, &n991), 991},
        {"1e9 true", 1000000000, BURROW_FN(SortSearchFunc, always, NULL), 0},
        {"1e9 false", 1000000000, BURROW_FN(SortSearchFunc, never, NULL), 1000000000},
        {"data -20", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[0]), 0},
        {"data -10", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[1]), 0},
        {"data -9", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[2]), 1},
        {"data -6", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[3]), 1},
        {"data -5", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[4]), 1},
        {"data 3", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[5]), 5},
        {"data 11", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[6]), 8},
        {"data 99", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[7]), 9},
        {"data 100", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[8]), 9},
        {"data 101", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[9]), 12},
        {"data 10000", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[10]), 13},
        {"data 10001", nd, BURROW_FN(SortSearchFunc, search_ge, &sd[11]), 14},
        {"descending a", 7, BURROW_FN(SortSearchFunc, descending_a, NULL), 4},
        {"descending 7", 1000000000, BURROW_FN(SortSearchFunc, descending_7, NULL),
         1000000000 - 7},
        {"overflow", 2000000000, BURROW_FN(SortSearchFunc, never, NULL), 2000000000},
    };
    for (size_t k = 0; k < sizeof tests / sizeof tests[0]; k++) {
        Int i = sort_search(tests[k].n, tests[k].f);
        if (i != tests[k].i)
            testing_t_errorf_v(t, "%s: expected index %d; got %d", tests[k].name,
                               tests[k].i, i);
    }
}

typedef struct FindCase {
    const Str *data;
    Int n;
    Str target;
} FindCase;

static int find_cmp(void *env, Int i) {
    const FindCase *c = env;
    return str_cmp(c->target, c->data[i]);
}

static void TestFind(TestingT *t) {
    static const Str str1[] = {BURROW_S_INIT("foo")};
    static const Str str2[] = {BURROW_S_INIT("ab"), BURROW_S_INIT("ca")};
    static const Str str3[] = {BURROW_S_INIT("mo"), BURROW_S_INIT("qo"),
                               BURROW_S_INIT("vo")};
    static const Str str4[] = {BURROW_S_INIT("ab"), BURROW_S_INIT("ad"),
                               BURROW_S_INIT("ca"), BURROW_S_INIT("xy")};

    /* slice with repeating elements */
    static const Str str_repeats[] = {
        BURROW_S_INIT("ba"), BURROW_S_INIT("ca"), BURROW_S_INIT("da"),
        BURROW_S_INIT("da"), BURROW_S_INIT("da"), BURROW_S_INIT("ka"),
        BURROW_S_INIT("ma"), BURROW_S_INIT("ma"), BURROW_S_INIT("ta")};

    /* slice with all element equal */
    static const Str str_same[] = {BURROW_S_INIT("xx"), BURROW_S_INIT("xx"),
                                   BURROW_S_INIT("xx")};

#define D(a) a, NELEM(a)
    struct {
        const Str *data;
        Int n;
        const char *target;
        Int want_pos;
        bool want_found;
    } tests[] = {
        {NULL, 0, "foo", 0, false},      {NULL, 0, "", 0, false},

        {D(str1), "foo", 0, true},       {D(str1), "bar", 0, false},
        {D(str1), "zx", 1, false},

        {D(str2), "aa", 0, false},       {D(str2), "ab", 0, true},
        {D(str2), "ad", 1, false},       {D(str2), "ca", 1, true},
        {D(str2), "ra", 2, false},

        {D(str3), "bb", 0, false},       {D(str3), "mo", 0, true},
        {D(str3), "nb", 1, false},       {D(str3), "qo", 1, true},
        {D(str3), "tr", 2, false},       {D(str3), "vo", 2, true},
        {D(str3), "xr", 3, false},

        {D(str4), "aa", 0, false},       {D(str4), "ab", 0, true},
        {D(str4), "ac", 1, false},       {D(str4), "ad", 1, true},
        {D(str4), "ax", 2, false},       {D(str4), "ca", 2, true},
        {D(str4), "cc", 3, false},       {D(str4), "dd", 3, false},
        {D(str4), "xy", 3, true},        {D(str4), "zz", 4, false},

        {D(str_repeats), "da", 2, true}, {D(str_repeats), "db", 5, false},
        {D(str_repeats), "ma", 6, true}, {D(str_repeats), "mb", 8, false},

        {D(str_same), "xx", 0, true},    {D(str_same), "ab", 0, false},
        {D(str_same), "zz", 3, false},
    };
#undef D

    for (size_t k = 0; k < sizeof tests / sizeof tests[0]; k++) {
        FindCase c = {tests[k].data, tests[k].n, str_from_cstr(tests[k].target)};
        bool found;
        Int pos = sort_find(c.n, BURROW_FN(SortFindFunc, find_cmp, &c), &found);
        if (pos != tests[k].want_pos || found != tests[k].want_found)
            testing_t_errorf_v(t, "%s: Find got (%d, %v), want (%d, %v)",
                               tests[k].target, pos, found, tests[k].want_pos,
                               tests[k].want_found);
    }
}

/* log2 computes the binary logarithm of x, rounded up to the next integer.
 * (log2(0) == 0, log2(1) == 0, log2(2) == 1, log2(3) == 2, etc.) */
static Int log2_up(Int x) {
    if (x < 1)
        return 0;
    return bits_len((Uint)(x - 1));
}

typedef struct Counted {
    Int x;
    Int count;
} Counted;

static bool counted_at_least(void *env, Int i) {
    Counted *c = env;
    c->count++;
    return i >= c->x;
}

static void TestSearchEfficiency(TestingT *t) {
    Int n = 100;
    Int step = 1;
    for (Int exp = 2; exp < 10; exp++) {
        /* n == 10**exp, step == 10**(exp-2) */
        Int max = log2_up(n);
        for (Int x = 0; x < n; x += step) {
            Counted c = {x, 0};
            Int i = sort_search(n, BURROW_FN(SortSearchFunc, counted_at_least, &c));
            if (i != x)
                testing_t_errorf_v(t, "n = %d: expected index %d; got %d", n, x, i);
            if (c.count > max)
                testing_t_errorf_v(t, "n = %d, x = %d: expected <= %d calls; got %d", n,
                                   x, max, c.count);
        }
        n *= 10;
        step *= 10;
    }
}

/* Smoke tests for convenience wrappers - not comprehensive. */
static void TestSearchWrappers(TestingT *t) {
    static double fdata[] = {-3.14, 0, 1, 2, 1000.7};
    static Str sdata[] = {BURROW_S_INIT("f"), BURROW_S_INIT("foo"),
                          BURROW_S_INIT("foobar"), BURROW_S_INIT("x")};
    Slice d = int_slice(data, NELEM(data));
    Slice f = float64_slice(fdata, NELEM(fdata));
    Slice s = string_slice(sdata, NELEM(sdata));
    struct {
        const char *name;
        Int result;
        Int i;
    } tests[] = {
        {"SearchInts", sort_search_ints(d, 11), 8},
        {"SearchFloat64s", sort_search_float64s(f, 2.1), 4},
        {"SearchStrings", sort_search_strings(s, BURROW_S("")), 0},
        {"IntSlice.Search", sort_int_slice_search(d, 0), 2},
        {"Float64Slice.Search", sort_float64_slice_search(f, 2.0), 3},
        {"StringSlice.Search", sort_string_slice_search(s, BURROW_S("x")), 3},
    };
    for (size_t k = 0; k < sizeof tests / sizeof tests[0]; k++) {
        if (tests[k].result != tests[k].i)
            testing_t_errorf_v(t, "%s: expected index %d; got %d", tests[k].name,
                               tests[k].i, tests[k].result);
    }
}

/* Abstract exhaustive test: all sizes up to 100, all possible return values.
 * If there are any small corner cases, this test exercises them. */
static void TestSearchExhaustive(TestingT *t) {
    for (Int size = 0; size <= 100; size++) {
        for (Int targ = 0; targ <= size; targ++) {
            Int i = sort_search(size, BURROW_FN(SortSearchFunc, at_least, &targ));
            if (i != targ)
                testing_t_errorf_v(t, "Search(%d, %d) = %d", size, targ, i);
        }
    }
}

static int seq_cmp(void *env, Int i) {
    /* Encodes the unmaterialized sequence with elem[i] == (i+1)*2 */
    Int x = *(const Int *)env;
    Int d = x - (i + 1) * 2;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

/* Abstract exhaustive test for Find. */
static void TestFindExhaustive(TestingT *t) {
    /* Test Find for different sequence sizes and search targets. For each
     * size, we have a (unmaterialized) sequence of integers: 2,4...size*2 And
     * we're looking for every possible integer between 1 and size*2 + 1. */
    for (Int size = 0; size <= 100; size++) {
        for (Int x = 1; x <= size * 2 + 1; x++) {
            bool want_found;
            Int want_pos;

            bool found;
            Int pos = sort_find(size, BURROW_FN(SortFindFunc, seq_cmp, &x), &found);

            if (x % 2 == 0) {
                want_pos = x / 2 - 1;
                want_found = true;
            } else {
                want_pos = x / 2;
                want_found = false;
            }
            if (found != want_found || pos != want_pos)
                testing_t_errorf_v(t, "Find(%d, %d): got (%d, %v), want (%d, %v)", size,
                                   x, pos, found, want_pos, want_found);
        }
    }
}

/* ------------------------------------------------------------ burrow's own */

/* sort_slice and sort_sort must leave equal elements in exactly the order Go
 * leaves them. The expected order is what Go 1.27.1 prints for the same input
 * with sort.Slice on the key only. */
static const Int go_slice_order[200] = {
    0,   130, 143, 91,  39,  195, 156, 65,  52,  26,  104, 169, 182, 13,  78,  117, 111,
    176, 124, 150, 20,  46,  59,  189, 163, 137, 7,   98,  72,  33,  85,  170, 53,  157,
    105, 79,  144, 1,   183, 27,  40,  14,  66,  118, 131, 196, 92,  8,   86,  21,  164,
    151, 177, 125, 190, 47,  112, 73,  99,  138, 60,  34,  132, 184, 106, 15,  54,  67,
    119, 93,  171, 197, 2,   158, 41,  28,  80,  145, 100, 191, 152, 35,  139, 165, 22,
    126, 74,  87,  48,  178, 113, 61,  9,   55,  133, 16,  120, 29,  68,  159, 146, 172,
    81,  198, 3,   42,  185, 107, 94,  10,  140, 23,  166, 62,  114, 49,  179, 36,  101,
    127, 192, 75,  153, 88,  108, 82,  121, 95,  69,  173, 160, 30,  43,  147, 134, 186,
    56,  4,   199, 17,  63,  193, 180, 37,  167, 102, 141, 24,  11,  89,  115, 128, 76,
    50,  154, 109, 18,  31,  83,  5,   70,  161, 96,  148, 187, 57,  122, 135, 174, 44,
    51,  103, 25,  129, 168, 90,  194, 64,  155, 116, 38,  181, 12,  142, 77,  58,  162,
    110, 188, 136, 6,   97,  19,  149, 123, 32,  71,  175, 84,  45,
};

static bool pair_slice_less(void *env, Int i, Int j) {
    const IntPair *p = env;
    return p[i].a < p[j].a;
}

static void TestUnstableOrderMatchesGo(TestingT *t) {
    IntPair p[200], q[200];
    for (Int i = 0; i < 200; i++) {
        p[i] = (IntPair){(i * 7919) % 13, i};
        q[i] = p[i];
    }
    static const Type pair_type = {{(const Byte *)"pair", 4},
                                   {(const Byte *)"", 0},
                                   KIND_STRUCT,
                                   sizeof(IntPair),
                                   _Alignof(IntPair),
                                   0,
                                   0,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   0,
                                   0,
                                   NULL};
    Slice s = slice_from(p, 200, 200, &pair_type);
    sort_slice(s, BURROW_FN(SortLessFunc, pair_slice_less, p));
    IntPairs qs = {q, 200};
    sort_sort((SortInterface){&pairs_vt, &qs});
    for (Int i = 0; i < 200; i++) {
        if (p[i].b != go_slice_order[i])
            testing_t_fatalf_v(t, "sort_slice: element %d is %d, Go has %d", i, p[i].b,
                               go_slice_order[i]);
        if (q[i].b != go_slice_order[i])
            testing_t_fatalf_v(t, "sort_sort: element %d is %d, Go has %d", i, q[i].b,
                               go_slice_order[i]);
    }
}

/* The sign bits Go's sort.Float64s leaves in a slice of zeros, negative zeros
 * and small values, where a zero and a negative zero compare equal. */
static const char go_float64s_signs[] =
    "11111111111111001101001101001001101001101101001001"
    "00100100110110100100100110110100100100000000000000";

static void TestFloat64sZeroOrderMatchesGo(TestingT *t) {
    double f[100], g[100];
    for (Int i = 0; i < 100; i++) {
        switch (i % 3) {
        case 0:
            f[i] = 0;
            break;
        case 1:
            f[i] = -0.0;
            break;
        default:
            f[i] = (double)((i * 31) % 7) - 3;
            break;
        }
        g[i] = f[i];
    }
    sort_float64s(float64_slice(f, 100));
    SortFloat64Slice gs = float64_slice(g, 100);
    sort_sort(sort_float64_slice_as_sort_interface(&gs));
    for (Int i = 0; i < 100; i++) {
        char want = go_float64s_signs[i];
        if ((math_signbit(f[i]) ? '1' : '0') != want)
            testing_t_fatalf_v(t, "sort_float64s: sign of element %d differs from Go",
                               i);
        if ((math_signbit(g[i]) ? '1' : '0') != want)
            testing_t_fatalf_v(t, "sort_sort: sign of element %d differs from Go", i);
    }
}

/* sort_slice swaps whole elements whatever their size. */
typedef struct Wide {
    Int key;
    unsigned char pad[93];
} Wide;

static bool wide_less(void *env, Int i, Int j) {
    const Wide *w = env;
    return w[i].key < w[j].key;
}

static void TestSliceSwapsAnySize(TestingT *t) {
    Wide w[50];
    for (Int i = 0; i < 50; i++) {
        w[i].key = (i * 17) % 50;
        memset(w[i].pad, (int)w[i].key, sizeof w[i].pad);
    }
    static const Type wide_type = {{(const Byte *)"Wide", 4},
                                   {(const Byte *)"", 0},
                                   KIND_STRUCT,
                                   sizeof(Wide),
                                   _Alignof(Wide),
                                   0,
                                   0,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   0,
                                   0,
                                   NULL};
    Slice s = slice_from(w, 50, 50, &wide_type);
    sort_slice_stable(s, BURROW_FN(SortLessFunc, wide_less, w));
    for (Int i = 0; i < 50; i++) {
        CHECK_INT_EQ(w[i].key, i);
        for (size_t k = 0; k < sizeof w[i].pad; k++)
            CHECK_INT_EQ(w[i].pad[k], i);
    }
}

#define TESTS(X)                                                                       \
    X(TestSortIntSlice)                                                                \
    X(TestSortFloat64Slice)                                                            \
    X(TestSortFloat64sCompareSlicesSort)                                               \
    X(TestSortStringSlice)                                                             \
    X(TestInts)                                                                        \
    X(TestFloat64s)                                                                    \
    X(TestStrings)                                                                     \
    X(TestSlice)                                                                       \
    X(TestSortLarge_Random)                                                            \
    X(TestReverseSortIntSlice)                                                         \
    X(TestBreakPatterns)                                                               \
    X(TestReverseRange)                                                                \
    X(TestNonDeterministicComparison)                                                  \
    X(TestSortBM)                                                                      \
    X(TestHeapsortBM)                                                                  \
    X(TestStableBM)                                                                    \
    X(TestAdversary)                                                                   \
    X(TestStableInts)                                                                  \
    X(TestStability)                                                                   \
    X(TestSearch)                                                                      \
    X(TestFind)                                                                        \
    X(TestSearchEfficiency)                                                            \
    X(TestSearchWrappers)                                                              \
    X(TestSearchExhaustive)                                                            \
    X(TestFindExhaustive)                                                              \
    X(TestUnstableOrderMatchesGo)                                                      \
    X(TestFloat64sZeroOrderMatchesGo)                                                  \
    X(TestSliceSwapsAnySize)

TESTING_MAIN(TESTS)
