// Go's Example_bloomFilter from hash/maphash, a Bloom filter over any element
// type, with the element type's hashing and equality given as a MaphashHasher.
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

// doc: filter
typedef struct BloomFilter {
    MaphashHasher hasher;
    MaphashSeed *seeds; // each seed picks a hash function
    Int nseeds;
    Byte *bytes; // the bit vector
    Int nbytes;
} BloomFilter;

// reduce maps hash into [0, n), the way Lemire suggests instead of a modulo.
static uint64_t reduce(uint64_t hash, uint64_t n) {
    return (hash >> 32) * n >> 32;
}

static void locate(const BloomFilter *f, MaphashSeed seed, const void *v,
                   uint64_t *index, Byte *mask) {
    MaphashHash h = {0};
    maphash_hash_set_seed(&h, seed);
    maphash_hasher_hash(f->hasher, &h, v);
    uint64_t hash = maphash_hash_sum64(&h);
    *index = reduce(hash, (uint64_t)f->nbytes);
    *mask = (Byte)(1U << (hash % 8));
}

static void bloom_insert(BloomFilter *f, const void *v) {
    for (Int i = 0; i < f->nseeds; i++) {
        uint64_t index;
        Byte bit;
        locate(f, f->seeds[i], v, &index, &bit);
        f->bytes[index] |= bit;
    }
}

static bool bloom_contains(const BloomFilter *f, const void *v) {
    for (Int i = 0; i < f->nseeds; i++) {
        uint64_t index;
        Byte bit;
        locate(f, f->seeds[i], v, &index, &bit);
        if ((f->bytes[index] & bit) == 0)
            return false;
    }
    return true;
}
// doc: end

// The sizes from https://en.wikipedia.org/wiki/Bloom_filter, for n elements
// and a false positive rate of p.
static BloomFilter bloom_new(Alloc *a, MaphashHasher hasher, Int n, double p) {
    BloomFilter f = {hasher, NULL, 1, NULL, 1};
    const double ln2 = 0.69314718055994530942;
    if (n > 0) {
        double m = -((double)n * math_log(p)) / (ln2 * ln2);
        f.nbytes = (Int)m / 8;
        if ((double)(f.nbytes * 8) < m)
            f.nbytes++;
        f.nseeds = (Int)math_round(-math_log(p) / ln2);
        if (f.nseeds < 1)
            f.nseeds = 1;
    }
    f.seeds = BURROW_NEW_N(a, MaphashSeed, f.nseeds);
    for (Int i = 0; i < f.nseeds; i++)
        f.seeds[i] = maphash_make_seed();
    f.bytes = BURROW_NEW_N(a, Byte, f.nbytes);
    memset(f.bytes, 0, (size_t)f.nbytes);
    return f;
}

int main(void) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    // doc: use
    // A filter for 2 strings with a one in a billion false positive rate.
    MaphashComparableHasher strs = {TYPE_STRING};
    BloomFilter f = bloom_new(a, maphash_comparable_hasher_as_hasher(&strs), 2, 1e-9);

    Str apple = BURROW_S("apple"), banana = BURROW_S("banana");
    bloom_insert(&f, &apple);
    bloom_insert(&f, &banana);

    Str fruits[] = {BURROW_S("apple"), BURROW_S("banana"), BURROW_S("cherry")};
    for (int i = 0; i < 3; i++)
        printf("Contains(\"%.*s\") = %s\n", (int)fruits[i].len,
               (const char *)fruits[i].p,
               bloom_contains(&f, &fruits[i]) ? "true" : "false");
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
Contains("apple") = true
Contains("banana") = true
Contains("cherry") = false
*/
