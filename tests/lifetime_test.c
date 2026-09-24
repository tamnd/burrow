/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"
#include "burrow/own.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"
#include "burrow/version.h"

#include <stdint.h>
#include <string.h>

/* The annotations are claims, and this is where the claims get tested.
 *
 * tools/check-annotations.sh already refuses a declaration that returns a
 * pointer and says nothing, and refuses an annotation that names a parameter
 * that is not there. Neither of those says whether the annotation is true.
 * Being true needs the memory to exist, so it happens here instead.
 *
 * Two questions answer almost all of it. Did the call allocate, and does the
 * result point into the argument? The tracking allocator answers the first
 * exactly, because counting live blocks is what it is for, and pointer
 * arithmetic answers the second. So:
 *
 *   BURROW_OWNS(ret)        the live count went up
 *   BURROW_BORROWS(ret, s)  the live count did not, and where the borrow is
 *                           into a buffer rather than into a static string the
 *                           result is inside that buffer
 *   BURROW_STATIC(ret)      the live count did not, the same call twice gives
 *                           the same pointer, and the result outlives every
 *                           allocator in the program
 *
 * What none of this can prove is that a borrow stays valid for exactly as long
 * as the thing it borrowed from. That is what the AddressSanitizer runs in CI
 * are for, and between them and the live count there is not much room left for
 * an annotation to be wrong and unnoticed.
 *
 * One rule for reading a failure here. A test failing does not mean the
 * function is broken. It means the function and the line above it disagree, and
 * the first thing to work out is which of the two is wrong. */

/* ------------------------------------------------------------------ helpers */

/* An allocator that counts, so that a test can say "this call allocated" as a
 * fact rather than as a guess about what the implementation does. */
typedef struct Counted {
    Arena ar;
    Track tr;
    Alloc *a;
    size_t mark;
} Counted;

static void counted_start(Counted *c) {
    /* An arena underneath, not the heap. Nothing in this file frees what it
     * allocates, because the point of every test is to look at the result after
     * the call, and a per block free would be a second thing that could be
     * getting the lifetime wrong. An arena gives all of it back in one call at
     * the end, which is both the answer to LeakSanitizer and the pattern the
     * guide tells everybody else to use.
     *
     * The quarantine is off because nothing here frees, so there would never be
     * anything in it, and track_check never runs because live blocks at the end
     * are the normal state here rather than a fault. */
    arena_init(&c->ar, NULL, 0);
    track_init(&c->tr, arena_allocator(&c->ar));
    track_set_quarantine(&c->tr, 0);
    c->a = track_allocator(&c->tr);
    c->mark = 0;
}

static void counted_mark(Counted *c) {
    c->mark = track_live(&c->tr);
}

static bool counted_grew(Counted *c) {
    return track_live(&c->tr) > c->mark;
}

static void counted_end(Counted *c) {
    track_free(&c->tr);
    arena_free(&c->ar);
}

/* Comparing pointers into different objects is not something C defines, so the
 * arithmetic happens on integers where it is defined everywhere. */
static bool inside(const void *p, const void *base, size_t n) {
    uintptr_t x = (uintptr_t)p;
    uintptr_t lo = (uintptr_t)base;
    return x >= lo && x < lo + n;
}

static bool str_inside(Str s, const void *base, size_t n) {
    return inside(s.p, base, n);
}

/* --------------------------------------------------------------- OWNS */

static void TestOwnsStrCloneAllocatesAndDoesNotAlias(TestingT *t) {
    Counted c;
    counted_start(&c);
    Str src = BURROW_S("the quick brown fox");

    counted_mark(&c);
    Str got = str_clone(c.a, src);

    CHECK(counted_grew(&c));
    CHECK(got.p != src.p);
    CHECK(!str_inside(got, src.p, (size_t)src.len));
    CHECK(str_eq(got, src));

    counted_end(&c);
}

static void TestOwnsStrToCstrAllocates(TestingT *t) {
    Counted c;
    counted_start(&c);
    Str src = BURROW_S("hello");

    counted_mark(&c);
    char *got = str_to_cstr(c.a, src);

    CHECK(counted_grew(&c));
    CHECK(got != NULL);
    if (got != NULL) {
        CHECK((const Byte *)got != src.p);
        CHECK(strcmp(got, "hello") == 0);
    }

    counted_end(&c);
}

static void TestOwnsSliceMakeAllocates(TestingT *t) {
    Counted c;
    counted_start(&c);

    counted_mark(&c);
    Slice s = slice_make(c.a, TYPE_INT, 4, 8);

    CHECK(counted_grew(&c));
    CHECK(s.p != NULL);

    counted_end(&c);
}

/* A zero length slice with a real capacity still allocates, because Go's
 * make([]T, 0, 8) does and code ported from Go relies on the capacity being
 * there. A zero capacity one is allowed not to. */
static void TestOwnsSliceMakeAllocatesForCapacityAlone(TestingT *t) {
    Counted c;
    counted_start(&c);

    counted_mark(&c);
    Slice s = slice_make(c.a, TYPE_INT, 0, 8);

    CHECK(counted_grew(&c));
    CHECK(s.cap == 8);

    counted_end(&c);
}

static void TestOwnsStrAndSliceConversionsCopy(TestingT *t) {
    Counted c;
    counted_start(&c);
    Str src = BURROW_S("abcdef");

    counted_mark(&c);
    Slice bytes = slice_from_str(c.a, src);
    CHECK(counted_grew(&c));
    CHECK((const Byte *)bytes.p != src.p);
    CHECK(!inside(bytes.p, src.p, (size_t)src.len));

    counted_mark(&c);
    Str back = str_from_slice(c.a, bytes);
    CHECK(counted_grew(&c));
    CHECK(back.p != (const Byte *)bytes.p);
    CHECK(str_eq(back, src));

    counted_end(&c);
}

static void TestOwnsErrorsNewAllocatesAndCopiesTheText(TestingT *t) {
    Counted c;
    counted_start(&c);
    Str text = BURROW_S("something went wrong");

    counted_mark(&c);
    Error err = errors_new(c.a, text);

    CHECK(counted_grew(&c));
    Str msg = error_text(err);
    CHECK(str_eq(msg, text));
    /* Copied, not kept. Without this the error would be a BURROW_RETAINS on
     * text and every caller would have to keep the text alive. */
    CHECK(msg.p != text.p);

    counted_end(&c);
}

static void TestOwnsMapMakeAllocates(TestingT *t) {
    Counted c;
    counted_start(&c);

    counted_mark(&c);
    Map *m = map_make(c.a, TYPE_INT, TYPE_INT, 0);

    CHECK(counted_grew(&c));
    CHECK(m != NULL);

    map_free(m);
    counted_end(&c);
}

static void TestOwnsAnyBoxCopiesTheValue(TestingT *t) {
    Counted c;
    counted_start(&c);
    Int v = 42;
    Any a = BURROW_ANY(TYPE_INT, &v);

    counted_mark(&c);
    Any boxed = any_box(c.a, a);

    CHECK(counted_grew(&c));
    CHECK(boxed.data != NULL);
    CHECK(boxed.t == TYPE_INT);
    if (boxed.data != NULL) {
        CHECK(boxed.data != (void *)&v);
        CHECK(*(Int *)boxed.data == 42);

        /* A box that aliased the original would still read 42 here, which is
         * why the pointer is checked above and the value is changed below. */
        v = 7;
        CHECK(*(Int *)boxed.data == 42);
    }

    counted_end(&c);
}

static void TestOwnsMemAllocFamilyAllocates(TestingT *t) {
    Counted c;
    counted_start(&c);

    counted_mark(&c);
    void *p = mem_alloc(c.a, 32, 8);
    CHECK(counted_grew(&c));
    CHECK(p != NULL);

    counted_mark(&c);
    void *q = mem_alloc_array(c.a, 8, 4, 4);
    CHECK(counted_grew(&c));
    CHECK(q != NULL);

    counted_end(&c);
}

/* --------------------------------------------------------------- BORROWS */

static void TestBorrowsStrFromCstrPointsAtTheInput(TestingT *t) {
    Counted c;
    counted_start(&c);
    const char *src = "borrowed";

    counted_mark(&c);
    Str got = str_from_cstr(src);

    CHECK(!counted_grew(&c));
    CHECK((const void *)got.p == (const void *)src);
    CHECK(got.len == 8);

    counted_end(&c);
}

static void TestBorrowsStrFromBytesPointsAtTheInput(TestingT *t) {
    Counted c;
    counted_start(&c);
    Byte buf[6] = {'a', 'b', 'c', 'd', 'e', 'f'};

    counted_mark(&c);
    Str got = str_from_bytes(buf, 6);

    CHECK(!counted_grew(&c));
    CHECK(str_inside(got, buf, sizeof buf));

    counted_end(&c);
}

static void TestBorrowsSliceFromPointsAtTheInput(TestingT *t) {
    Counted c;
    counted_start(&c);
    Int buf[4] = {1, 2, 3, 4};

    counted_mark(&c);
    Slice s = slice_from(buf, 4, 4, TYPE_INT);

    CHECK(!counted_grew(&c));
    CHECK(s.p == (void *)buf);

    counted_end(&c);
}

static void TestBorrowsSliceAtPointsIntoTheSlice(TestingT *t) {
    Counted c;
    counted_start(&c);
    Int buf[4] = {10, 20, 30, 40};
    Slice s = slice_from(buf, 4, 4, TYPE_INT);

    counted_mark(&c);
    void *p = slice_at(s, 2);

    CHECK(!counted_grew(&c));
    CHECK(inside(p, buf, sizeof buf));
    CHECK(*(Int *)p == 30);

    /* The inline fast path has the same annotation and has to keep it. It is a
     * separate function as far as a caller is concerned, and a fast path that
     * quietly copied would be a fast path that broke the contract. */
    counted_mark(&c);
    void *q = slice_at_fast(s, 2, sizeof(Int));
    CHECK(!counted_grew(&c));
    CHECK(q == p);

    counted_end(&c);
}

static void TestBorrowsSliceSubSharesTheBackingArray(TestingT *t) {
    Counted c;
    counted_start(&c);
    Int buf[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    Slice s = slice_from(buf, 8, 8, TYPE_INT);

    counted_mark(&c);
    Slice sub = slice_sub(s, 2, 5);

    CHECK(!counted_grew(&c));
    CHECK(inside(sub.p, buf, sizeof buf));
    CHECK(sub.len == 3);

    /* Sharing means a write through one is visible through the other, which is
     * the whole reason the annotation is there. */
    *(Int *)slice_at(sub, 0) = 99;
    CHECK(buf[2] == 99);

    counted_mark(&c);
    Slice sub3 = slice_sub3(s, 1, 4, 6);
    CHECK(!counted_grew(&c));
    CHECK(inside(sub3.p, buf, sizeof buf));
    CHECK(sub3.cap == 5);

    counted_end(&c);
}

static void TestBorrowsMapGetPointsIntoTheTable(TestingT *t) {
    Counted c;
    counted_start(&c);
    Map *m = map_make(c.a, TYPE_INT, TYPE_INT, 0);
    Int key = 3;
    Int val = 300;
    CHECK(map_set(m, &key, &val));

    counted_mark(&c);
    void *got = map_get(m, &key);

    CHECK(!counted_grew(&c));
    CHECK(got != NULL);
    if (got != NULL) {
        CHECK(got != (void *)&val);
        CHECK(*(Int *)got == 300);

        /* Into the table, not a copy of it. Writing through the pointer changes
         * what the next lookup finds. */
        *(Int *)got = 301;
        void *again = map_get(m, &key);
        CHECK(again != NULL && *(Int *)again == 301);
    }

    map_free(m);
    counted_end(&c);
}

static void TestBorrowsErrorMessagePointsIntoTheError(TestingT *t) {
    Counted c;
    counted_start(&c);
    Error err = errors_new(c.a, BURROW_S("disk on fire"));

    counted_mark(&c);
    Str msg = error_text(err);

    CHECK(!counted_grew(&c));
    CHECK(str_eq(msg, BURROW_S("disk on fire")));

    counted_end(&c);
}

static void TestBorrowsAnyAssertHandsBackTheStoredPointer(TestingT *t) {
    Counted c;
    counted_start(&c);
    Int v = 5;
    Any a = BURROW_ANY(TYPE_INT, &v);

    counted_mark(&c);
    void *p = any_assert(a, TYPE_INT);

    CHECK(!counted_grew(&c));
    CHECK(p == (void *)&v);
    CHECK(any_assert(a, TYPE_STRING) == NULL);

    counted_end(&c);
}

static void TestBorrowsTypeNameDoesNotAllocate(TestingT *t) {
    Counted c;
    counted_start(&c);

    counted_mark(&c);
    Str name = type_name(TYPE_INT);

    CHECK(!counted_grew(&c));
    CHECK(str_eq(name, BURROW_S("int")));
    /* The same descriptor gives the same bytes back, which is what makes it a
     * borrow from the descriptor and not a rendering. */
    CHECK(type_name(TYPE_INT).p == name.p);

    counted_end(&c);
}

/* The allocator accessors all return a pointer into the struct they were given,
 * which is why the documentation says not to copy the struct afterwards. If one
 * of these ever returned a pointer to something else, copying an Arena would
 * start working and then stop working, which is the worst way for a rule to be
 * enforced. */
static void TestBorrowsAllocatorAccessorsPointIntoTheirStruct(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    CHECK(inside(arena_allocator(&ar), &ar, sizeof ar));
    arena_free(&ar);

    unsigned char buf[256];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    CHECK(inside(fixed_allocator(&fx), &fx, sizeof fx));

    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *a = track_allocator(&tr);
    CHECK(inside(a, &tr, sizeof tr));
    /* TRACK_HERE hands back what it was given, so it borrows from that and not
     * from the Track it looked inside. */
    CHECK(track_note(a, __FILE__, __LINE__) == a);
    track_free(&tr);
}

/* --------------------------------------------------------------- STATIC */

/* Two calls, one pointer, and it is still readable after every allocator in the
 * test has gone away. That is the whole claim BURROW_STATIC makes. */
static void TestStaticVersionStringsAreNotAllocated(TestingT *t) {
    Counted c;
    counted_start(&c);

    counted_mark(&c);
    const char *v = burrow_version();
    const char *sid = burrow_sourceid();
    const char *gov = burrow_go_version();
    const char *lic = burrow_license();

    CHECK(!counted_grew(&c));
    CHECK(v == burrow_version());
    CHECK(sid == burrow_sourceid());
    CHECK(gov == burrow_go_version());
    CHECK(lic == burrow_license());

    counted_end(&c);

    /* After the allocator is gone. A string that came from c.a would be freed
     * memory by now and this would be a use after free that AddressSanitizer
     * reports in CI. */
    CHECK(strcmp(v, BURROW_VERSION_STRING) == 0);
    CHECK(sid[0] != '\0');
    CHECK(strncmp(gov, "go1.", 4) == 0);
    CHECK(lic[0] != '\0');
}

static void TestStaticPlatformNamesAreNotAllocated(TestingT *t) {
    Counted c;
    counted_start(&c);

    counted_mark(&c);
    const char *os = burrow_os_name();
    const char *arch = burrow_arch_name();

    CHECK(!counted_grew(&c));
    CHECK(os == burrow_os_name());
    CHECK(arch == burrow_arch_name());

    counted_end(&c);

    CHECK(strcmp(os, BURROW_OS_NAME) == 0);
    CHECK(strcmp(arch, BURROW_ARCH_NAME) == 0);
}

static void TestStaticHeapAllocatorIsOneObject(TestingT *t) {
    Counted c;
    counted_start(&c);

    counted_mark(&c);
    Alloc *a = heap_allocator();

    CHECK(!counted_grew(&c));
    CHECK(a != NULL);
    CHECK(a == heap_allocator());

    counted_end(&c);

    /* Still usable with no initialisation and nothing alive, which is the
     * reason it is the one allocator with no init function. */
    void *p = mem_alloc(heap_allocator(), 16, 8);
    CHECK(p != NULL);
    mem_free(heap_allocator(), p, 16, 8);
}

static void TestStaticKindNameReturnsTheSameBytesEveryTime(TestingT *t) {
    Counted c;
    counted_start(&c);

    counted_mark(&c);
    Str n = kind_name(KIND_INT);

    CHECK(!counted_grew(&c));
    CHECK(str_eq(n, BURROW_S("int")));
    CHECK(kind_name(KIND_INT).p == n.p);

    counted_end(&c);

    CHECK(str_eq(n, BURROW_S("int")));
}

static void TestStaticTypeAccessorsOutliveTheMap(TestingT *t) {
    Counted c;
    counted_start(&c);
    Map *m = map_make(c.a, TYPE_STRING, TYPE_INT, 0);

    counted_mark(&c);
    const Type *k = map_key_type(m);
    const Type *v = map_val_type(m);

    CHECK(!counted_grew(&c));
    CHECK(k == TYPE_STRING);
    CHECK(v == TYPE_INT);

    map_free(m);
    counted_end(&c);

    /* The descriptors are not the map's to own, so freeing the map leaves them
     * alone. This is why they are BURROW_STATIC and not a borrow from m. */
    CHECK(str_eq(type_name(k), BURROW_S("string")));
    CHECK(str_eq(type_name(v), BURROW_S("int")));
}

static void TestStaticSliceNilOwnsNothing(TestingT *t) {
    Counted c;
    counted_start(&c);

    counted_mark(&c);
    Slice s = slice_nil(TYPE_INT);

    CHECK(!counted_grew(&c));
    CHECK(s.p == NULL);
    CHECK(s.len == 0);
    CHECK(s.cap == 0);
    CHECK(slice_is_nil(s));

    counted_end(&c);

    /* Nothing to free means nothing that could have been freed. */
    CHECK(slice_is_nil(s));
}

/* ------------------------------------------------- OWNS and BORROWS together */

/* Append is the case the pair of annotations exists for. With room it writes
 * into the array it was given and hands back a header over the same memory.
 * Without room it allocates. A caller cannot tell which happened, so both are
 * true of the declaration and both have to be true of the function. */

static void TestAppendBorrowsWhenThereIsCapacity(TestingT *t) {
    Counted c;
    counted_start(&c);
    Slice s = slice_make(c.a, TYPE_INT, 1, 8);
    void *before = s.p;
    Int v = 7;

    counted_mark(&c);
    Slice got = slice_append(c.a, s, &v, 1);

    CHECK(!counted_grew(&c));
    CHECK(got.p == before);
    CHECK(got.len == 2);
    CHECK(got.cap == 8);
    CHECK(*(Int *)slice_at(got, 1) == 7);

    counted_end(&c);
}

static void TestAppendOwnsWhenItHasToGrow(TestingT *t) {
    Counted c;
    counted_start(&c);
    Slice s = slice_make(c.a, TYPE_INT, 2, 2);
    *(Int *)slice_at(s, 0) = 11;
    *(Int *)slice_at(s, 1) = 22;
    void *before = s.p;
    Int v = 7;

    counted_mark(&c);
    Slice got = slice_append(c.a, s, &v, 1);

    CHECK(counted_grew(&c));
    CHECK(got.p != before);
    CHECK(got.len == 3);
    CHECK(got.cap >= 3);
    CHECK(*(Int *)slice_at(got, 0) == 11);
    CHECK(*(Int *)slice_at(got, 2) == 7);

    /* The old array is still there and still readable, because append does not
     * free what it was given. That is what makes it safe for a caller to hold
     * both headers, and it is why s is not a dangling pointer afterwards. */
    CHECK(((Int *)before)[0] == 11);
    CHECK(((Int *)before)[1] == 22);

    /* And the two are separate now, so writing through one does not show up in
     * the other. A grow that aliased would make this fail. */
    *(Int *)slice_at(got, 0) = 33;
    CHECK(((Int *)before)[0] == 11);

    counted_end(&c);
}

static void TestAppendSliceBorrowsThenOwns(TestingT *t) {
    Counted c;
    counted_start(&c);
    Slice src = slice_make(c.a, TYPE_INT, 2, 2);
    Slice dst = slice_make(c.a, TYPE_INT, 0, 8);
    void *before = dst.p;

    counted_mark(&c);
    Slice got = slice_append_slice(c.a, dst, src);
    CHECK(!counted_grew(&c));
    CHECK(got.p == before);
    CHECK(got.len == 2);

    /* Now past the capacity, in one call, which is the branch that allocates. */
    Slice big = slice_make(c.a, TYPE_INT, 32, 32);
    counted_mark(&c);
    Slice grown = slice_append_slice(c.a, got, big);
    CHECK(counted_grew(&c));
    CHECK(grown.p != before);
    CHECK(grown.len == 34);

    counted_end(&c);
}

static void TestAppendFastKeepsTheSamePromise(TestingT *t) {
    Counted c;
    counted_start(&c);
    Slice s = slice_make(c.a, TYPE_INT, 0, 4);
    void *before = s.p;
    Int v = 1;

    counted_mark(&c);
    s = slice_append_fast(c.a, s, &v, sizeof(Int));
    CHECK(!counted_grew(&c));
    CHECK(s.p == before);

    /* Fill it, then go one past, and the inline path has to fall through to the
     * one that can allocate. */
    for (Int i = 0; i < 4; i++)
        s = slice_append_fast(c.a, s, &v, sizeof(Int));

    CHECK(s.len == 5);
    CHECK(s.p != before);

    counted_end(&c);
}

static void TestUtf8AppendRuneBorrowsThenOwns(TestingT *t) {
    Counted c;
    counted_start(&c);
    Slice p = slice_make(c.a, TYPE_BYTE, 0, 8);
    void *before = p.p;

    counted_mark(&c);
    p = utf8_append_rune(c.a, p, 'a');
    CHECK(!counted_grew(&c));
    CHECK(p.p == before);
    CHECK(p.len == 1);

    /* Four bytes of runes into eight bytes of capacity, twice over, which has
     * to leave the original array behind somewhere in the middle. */
    for (int i = 0; i < 4; i++)
        p = utf8_append_rune(c.a, p, 0x1F600);

    CHECK(p.len == 17);
    CHECK(p.p != before);
    CHECK(((Byte *)p.p)[0] == 'a');

    counted_end(&c);
}

#define TESTS(X)                                                                       \
    X(TestOwnsStrCloneAllocatesAndDoesNotAlias)                                        \
    X(TestOwnsStrToCstrAllocates)                                                      \
    X(TestOwnsSliceMakeAllocates)                                                      \
    X(TestOwnsSliceMakeAllocatesForCapacityAlone)                                      \
    X(TestOwnsStrAndSliceConversionsCopy)                                              \
    X(TestOwnsErrorsNewAllocatesAndCopiesTheText)                                      \
    X(TestOwnsMapMakeAllocates)                                                        \
    X(TestOwnsAnyBoxCopiesTheValue)                                                    \
    X(TestOwnsMemAllocFamilyAllocates)                                                 \
    X(TestBorrowsStrFromCstrPointsAtTheInput)                                          \
    X(TestBorrowsStrFromBytesPointsAtTheInput)                                         \
    X(TestBorrowsSliceFromPointsAtTheInput)                                            \
    X(TestBorrowsSliceAtPointsIntoTheSlice)                                            \
    X(TestBorrowsSliceSubSharesTheBackingArray)                                        \
    X(TestBorrowsMapGetPointsIntoTheTable)                                             \
    X(TestBorrowsErrorMessagePointsIntoTheError)                                       \
    X(TestBorrowsAnyAssertHandsBackTheStoredPointer)                                   \
    X(TestBorrowsTypeNameDoesNotAllocate)                                              \
    X(TestBorrowsAllocatorAccessorsPointIntoTheirStruct)                               \
    X(TestStaticVersionStringsAreNotAllocated)                                         \
    X(TestStaticPlatformNamesAreNotAllocated)                                          \
    X(TestStaticHeapAllocatorIsOneObject)                                              \
    X(TestStaticKindNameReturnsTheSameBytesEveryTime)                                  \
    X(TestStaticTypeAccessorsOutliveTheMap)                                            \
    X(TestStaticSliceNilOwnsNothing)                                                   \
    X(TestAppendBorrowsWhenThereIsCapacity)                                            \
    X(TestAppendOwnsWhenItHasToGrow)                                                   \
    X(TestAppendSliceBorrowsThenOwns)                                                  \
    X(TestAppendFastKeepsTheSamePromise)                                               \
    X(TestUtf8AppendRuneBorrowsThenOwns)

TESTING_MAIN(TESTS)
