/* Tests for the type registry.
 *
 * The registry is one table for the whole program, which is the point of it, so
 * these tests share state and a few of them read counts relative to what was
 * there before rather than against a fixed number. A test that asserted the
 * registry holds exactly four types would break the moment another test in this
 * file registered a fifth, and would be testing the file rather than the code.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/declare.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/proc.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include "check.h"

#include <stdint.h>

/* Two types declared the ordinary way, one registered and one not, because the
 * difference between them is most of what there is to check. */

#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")

BURROW_STRUCT(RegPoint, POINT_FIELDS);
BURROW_REGISTER_TYPE(RegPoint);

BURROW_STRUCT(QuietPoint, POINT_FIELDS);

/* Two distinct descriptors for []int, for the identity tests below. */
BURROW_SLICE_TYPE(IntsA, Int);
BURROW_SLICE_TYPE(IntsB, Int);

/* A hand written descriptor with a package path, since BURROW_STRUCT does not
 * set one and the interesting half of a qualified name is the package. The
 * path has dots in it on purpose: that is what a real Go import path looks
 * like, and it is the case the split at the last dot exists for. */
static const Field circle_fields[] = {
    {BURROW_S_INIT("R"), BURROW_S_INIT(""), TYPE_INT, 0},
};

const Type burrow_type_Circle = {
    BURROW_S_INIT("Circle"),
    BURROW_S_INIT("github.com/tamnd/shapes.v2"),
    KIND_STRUCT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
    1,
    0,
    circle_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

BURROW_REGISTER_TYPE(Circle);

static void TestARegisteredTypeIsFoundByName(TestingT *t) {
    const Type *ty = type_by_name(BURROW_S("RegPoint"), NULL);

    CHECK(ty == TYPE_OF(RegPoint));
    if (ty != NULL)
        CHECK(str_eq(ty->name, BURROW_S("RegPoint")));
}

static void TestATypeThatWasNotRegisteredIsNotFound(TestingT *t) {
    /* It still has a descriptor and reflection on a value of it still works.
     * Registration is only about finding it from a name. */
    CHECK(type_by_name(BURROW_S("QuietPoint"), NULL) == NULL);
    CHECK(TYPE_OF(QuietPoint) != NULL);
}

static void TestANameNothingRegisteredGivesNull(TestingT *t) {
    CHECK(type_by_name(BURROW_S("NoSuchType"), NULL) == NULL);
    CHECK(type_by_name(BURROW_S("a.b.c.NoSuchType"), NULL) == NULL);
    CHECK(type_by_name(BURROW_STR_EMPTY, NULL) == NULL);
}

static void TestThePackagePathIsPartOfTheName(TestingT *t) {
    const Type *ty = type_by_name(BURROW_S("github.com/tamnd/shapes.v2.Circle"), NULL);
    CHECK(ty == &burrow_type_Circle);

    /* The bare name is not the qualified name, and a type in a package is not
     * reachable without it. Two packages are allowed to have a Circle. */
    CHECK(type_by_name(BURROW_S("Circle"), NULL) == NULL);

    /* Nor is a prefix of the package path. */
    CHECK(type_by_name(BURROW_S("shapes.v2.Circle"), NULL) == NULL);
}

static void TestAQualifiedNameCanBeWrittenOut(TestingT *t) {
    Byte buf[64];

    Str q = type_qualified_name(&burrow_type_Circle, buf, (Int)sizeof buf);
    CHECK(str_eq(q, BURROW_S("github.com/tamnd/shapes.v2.Circle")));

    /* No package path means no dot, not a leading one. */
    Str p = type_qualified_name(TYPE_OF(RegPoint), buf, (Int)sizeof buf);
    CHECK(str_eq(p, BURROW_S("RegPoint")));

    /* And it round trips, which is the property that matters: the name this
     * writes is the name the lookup takes. */
    CHECK(type_by_name(p, NULL) == TYPE_OF(RegPoint));
}

static void TestANameTooLongForTheBufferIsNotWrittenAtAll(TestingT *t) {
    Byte buf[8];

    /* "github.c" would be a perfectly well formed qualified name and a lookup
     * would take it, which is exactly the problem: the caller would get an
     * answer about a type nobody asked about. Nothing is the answer that can be
     * acted on. */
    CHECK(type_qualified_name(&burrow_type_Circle, buf, (Int)sizeof buf).len == 0);

    /* Exactly enough room still works, so this is a bounds check and not a
     * margin somebody guessed at. */
    Byte exact[8];
    Str p = type_qualified_name(TYPE_OF(RegPoint), exact, 8);
    CHECK(str_eq(p, BURROW_S("RegPoint")));
    CHECK(type_qualified_name(TYPE_OF(RegPoint), exact, 7).len == 0);

    CHECK(type_qualified_name(&burrow_type_Circle, NULL, 64).len == 0);
    CHECK(type_qualified_name(NULL, buf, (Int)sizeof buf).len == 0);
}

/* ------------------------------------------------------------- the boundary
 *
 * The registry is the one part of reflect that can be handed a name from
 * outside the program, so it is the one part that has to be specific about why
 * it could not answer. A caller holding nothing but a NULL cannot tell a
 * corrupt stream from a peer built against a newer version of the schema, and
 * those two get handled by different people. */

static void TestAnUnregisteredNameSaysItIsUnregistered(TestingT *t) {
    Error err = BURROW_NO_ERROR;

    CHECK(type_by_name(BURROW_S("NoSuchType"), &err) == NULL);
    CHECK(BURROW_FAILED(err));
    CHECK(errors_is(err, type_err_not_registered));

    /* A type that exists in this program but was never registered is the same
     * answer, because from in here the two are the same situation. */
    CHECK(type_by_name(BURROW_S("QuietPoint"), &err) == NULL);
    CHECK(errors_is(err, type_err_not_registered));
}

static void TestANameThatIsNotANameSaysSoInstead(TestingT *t) {
    Error err = BURROW_NO_ERROR;

    CHECK(type_by_name(BURROW_STR_EMPTY, &err) == NULL);
    CHECK(errors_is(err, type_err_name_invalid));

    /* A trailing dot names a package and then no type. */
    CHECK(type_by_name(BURROW_S("image."), &err) == NULL);
    CHECK(errors_is(err, type_err_name_invalid));

    /* A leading one claims a package and does not name it. */
    CHECK(type_by_name(BURROW_S(".Point"), &err) == NULL);
    CHECK(errors_is(err, type_err_name_invalid));

    CHECK(type_by_name(BURROW_S("."), &err) == NULL);
    CHECK(errors_is(err, type_err_name_invalid));

    /* Dots in the middle of a package path are ordinary, so this one is merely
     * unknown rather than malformed. The difference is the point of the test. */
    CHECK(type_by_name(BURROW_S("a.b.c.NoSuchType"), &err) == NULL);
    CHECK(errors_is(err, type_err_not_registered));
}

static void TestANameThatIsFoundLeavesNoErrorBehind(TestingT *t) {
    /* Set to something first, because a caller reuses one err across a loop and
     * a function that only ever writes on failure leaves the last failure
     * sitting there to be read as this call's answer. */
    Error err = type_err_conflict;

    CHECK(type_by_name(BURROW_S("RegPoint"), &err) == TYPE_OF(RegPoint));
    CHECK(BURROW_OK(err));
}

static void TestTheErrorIsOptionalLikeEveryOtherOutParameter(TestingT *t) {
    /* Already how the rest of this file calls it, so this is the test that says
     * it is deliberate rather than tolerated. */
    CHECK(type_by_name(BURROW_S("NoSuchType"), NULL) == NULL);
    CHECK(type_register(NULL, NULL) == false);
}

/* A descriptor that is not in the section, for the runtime half. */
static const Type late_type = {
    BURROW_S_INIT("Late"),
    BURROW_S_INIT("plugin"),
    KIND_INT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
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

static void TestATypeCanBeRegisteredWhileTheProgramRuns(TestingT *t) {
    CHECK(type_by_name(BURROW_S("plugin.Late"), NULL) == NULL);

    Int before = type_registry_len();
    CHECK(type_register(&late_type, NULL));
    CHECK_INT_EQ((int)(type_registry_len() - before), 1);

    CHECK(type_by_name(BURROW_S("plugin.Late"), NULL) == &late_type);
}

static void TestRegisteringTheSameTypeTwiceChangesNothing(TestingT *t) {
    CHECK(type_register(&late_type, NULL));

    Int before = type_registry_len();
    CHECK(type_register(&late_type, NULL));
    CHECK_INT_EQ((int)(type_registry_len() - before), 0);

    CHECK(type_register(NULL, NULL) == false);
}

/* The same type reaching the program twice, which is what two shared libraries
 * each carrying a copy looks like from here: two descriptors, two addresses,
 * one type. */
static const Type late_again = {
    BURROW_S_INIT("Late"),
    BURROW_S_INIT("plugin"),
    KIND_INT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
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

/* And a different type wearing the same name. */
static const Type late_impostor = {
    BURROW_S_INIT("Late"),
    BURROW_S_INIT("plugin"),
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
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

static void TestASecondCopyOfOneTypeIsAccepted(TestingT *t) {
    CHECK(type_register(&late_type, NULL));

    Int before = type_registry_len();
    CHECK(type_register(&late_again, NULL));
    CHECK_INT_EQ((int)(type_registry_len() - before), 0);

    /* The first one stays. Which of two identical descriptors answers is not
     * something a caller can tell apart, but it should not change. */
    CHECK(type_by_name(BURROW_S("plugin.Late"), NULL) == &late_type);
}

static void TestADifferentTypeUnderATakenNameIsRefused(TestingT *t) {
    CHECK(type_register(&late_type, NULL));

    Error err = BURROW_NO_ERROR;
    CHECK(type_register(&late_impostor, &err) == false);
    CHECK(errors_is(err, type_err_conflict));

    /* Refused means the first one is still there, not that the name is now
     * broken. Whoever is already holding this name keeps it. */
    CHECK(type_by_name(BURROW_S("plugin.Late"), NULL) == &late_type);
}

/* A descriptor with no name, which is what an unnamed type such as []int has.
 * Registering one would put a row in the table that no lookup can ever reach,
 * so it is refused rather than accepted and forgotten. */
static const Type nameless = {
    BURROW_S_INIT(""),
    BURROW_S_INIT(""),
    KIND_INT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
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

static void TestADescriptorWithNothingToRegisterItUnderIsRefused(TestingT *t) {
    Error err = BURROW_NO_ERROR;

    CHECK(type_register(&nameless, &err) == false);
    CHECK(errors_is(err, type_err_name_invalid));

    CHECK(type_register(NULL, &err) == false);
    CHECK(errors_is(err, type_err_name_invalid));

    /* And an unnamed type from the DSL is the same case, which is the one a
     * program actually runs into. */
    CHECK(type_register(TYPE_OF(IntsA), &err) == false);
    CHECK(errors_is(err, type_err_name_invalid));
}

static void TestARegistrationThatWorkedLeavesNoErrorBehind(TestingT *t) {
    Error err = type_err_conflict;

    CHECK(type_register(&late_type, &err));
    CHECK(BURROW_OK(err));
}

/* The three of them are distinct, which is the only property the whole thing
 * rests on: a caller that cannot tell them apart is back to holding a bool. */
static void TestTheThreeAnswersAreThreeDifferentAnswers(TestingT *t) {
    CHECK(!errors_is(type_err_not_registered, type_err_name_invalid));
    CHECK(!errors_is(type_err_not_registered, type_err_conflict));
    CHECK(!errors_is(type_err_name_invalid, type_err_conflict));

    CHECK(errors_is(type_err_not_registered, type_err_not_registered));
    CHECK(!errors_is(type_err_not_registered, BURROW_NO_ERROR));

    /* Each says what it is, because these get logged and a log line reading
     * "reflect: error" helps nobody. */
    CHECK(error_text(type_err_not_registered).len > 0);
    CHECK(error_text(type_err_name_invalid).len > 0);
    CHECK(error_text(type_err_conflict).len > 0);
}

static void TestIdentityIsTheAddressWhenThereIsOne(TestingT *t) {
    CHECK(type_same(TYPE_OF(RegPoint), TYPE_OF(RegPoint)));
    CHECK(type_same(TYPE_INT, TYPE_OF(Int)));
    CHECK(!type_same(TYPE_OF(RegPoint), TYPE_OF(QuietPoint)));
    CHECK(!type_same(NULL, TYPE_INT));
    CHECK(type_same(NULL, NULL));
}

static void TestIdentityFallsBackToTheName(TestingT *t) {
    /* Two descriptors, one type. This is the shared library case and the only
     * reason type_same is a function rather than a comparison. */
    CHECK(&late_type != &late_again);
    CHECK(type_same(&late_type, &late_again));

    /* One name, two types. The kind is enough to tell these apart, and when it
     * is not the size usually is, and when neither is there is nothing left to
     * ask. That boundary is in the doc comment rather than pretended away. */
    CHECK(!type_same(&late_type, &late_impostor));
}

static void TestAnUnnamedTypeHasNoIdentityBeyondItsAddress(TestingT *t) {
    /* []int has no name, so two descriptors for it cannot be shown to be the
     * same type from here. Saying no is the honest answer: saying yes would
     * make every unnamed slice descriptor equal to every other. */
    const Type *a = TYPE_OF(IntsA);
    const Type *b = TYPE_OF(IntsB);

    CHECK(a != b);
    CHECK(!type_same(a, b));
    CHECK(type_same(a, a));
}

static void TestTheRegistryHoldsWhatWasPutInIt(TestingT *t) {
    /* Both of the file scope registrations, and nothing that was not
     * registered. The count is not checked against a number because the section
     * holds whatever the whole program registered, and this file is linked with
     * the library. */
    CHECK(type_registry_len() >= 2);
    CHECK(type_by_name(BURROW_S("RegPoint"), NULL) != NULL);
    CHECK(type_by_name(BURROW_S("github.com/tamnd/shapes.v2.Circle"), NULL) != NULL);
}

/* --------------------------------------------------------- under contention
 *
 * Lookups are a read lock and registration is a write lock, which is the whole
 * of the synchronisation, and the case it exists for is a module loaded while
 * the program is already running and already resolving names. So: readers going
 * flat out while a writer registers underneath them, including the registration
 * that makes the table grow and moves every entry.
 *
 * The goroutines report through atomics, because only a test function may
 * check. */

#define READERS 4
#define LATECOMERS 64

static Type latecomers[LATECOMERS];
static Byte latecomer_names[LATECOMERS][8];

static SyncAtomicUint32 race_stop;
static SyncAtomicInt64 race_running;
static SyncAtomicInt64 race_misses;
static SyncAtomicInt64 race_lookups;

static void race_reader(void *arg) {
    (void)arg;

    while (sync_atomic_uint32_load(&race_stop) == 0) {
        /* A name that was there before the writer started and must stay
         * findable through every growth of the table. */
        if (type_by_name(BURROW_S("RegPoint"), NULL) != TYPE_OF(RegPoint))
            sync_atomic_int64_add(&race_misses, 1);
        if (type_by_name(BURROW_S("github.com/tamnd/shapes.v2.Circle"), NULL) !=
            &burrow_type_Circle)
            sync_atomic_int64_add(&race_misses, 1);
        if (type_by_name(BURROW_S("NotThere"), NULL) != NULL)
            sync_atomic_int64_add(&race_misses, 1);
        sync_atomic_int64_add(&race_lookups, 1);
        runtime_gosched();
    }

    sync_atomic_int64_add(&race_running, -1);
}

static void race_writer(void *arg) {
    (void)arg;

    for (Int i = 0; i < LATECOMERS; i++) {
        if (!type_register(&latecomers[i], NULL))
            sync_atomic_int64_add(&race_misses, 1);
        runtime_gosched();
    }

    sync_atomic_uint32_store(&race_stop, 1);
    sync_atomic_int64_add(&race_running, -1);
}

static void race_main(void *arg) {
    (void)arg;

    runtime_gomaxprocs(READERS + 1);
    sync_atomic_int64_store(&race_running, READERS + 1);

    for (int i = 0; i < READERS; i++) {
        if (!go(BURROW_FN(Func, race_reader, NULL)))
            sync_atomic_int64_add(&race_running, -1);
    }
    if (!go(BURROW_FN(Func, race_writer, NULL)))
        sync_atomic_int64_add(&race_running, -1);

    while (sync_atomic_int64_load(&race_running) > 0)
        runtime_gosched();
}

static void TestLookupsKeepWorkingWhileTheTableGrowsUnderneathThem(TestingT *t) {
    /* Built here rather than at file scope because each needs a name of its
     * own and C has no way to generate sixty four of those in a static
     * initialiser. Written once, before any of the goroutines exist, and only
     * read after that. */
    for (Int i = 0; i < LATECOMERS; i++) {
        latecomer_names[i][0] = (Byte)'L';
        latecomer_names[i][1] = (Byte)('a' + (i / 26));
        latecomer_names[i][2] = (Byte)('a' + (i % 26));
        latecomers[i].name = (Str){latecomer_names[i], 3};
        latecomers[i].pkg_path = BURROW_S("late");
        latecomers[i].kind = KIND_INT;
        latecomers[i].size = (uint32_t)sizeof(Int);
        latecomers[i].align = (uint16_t)_Alignof(Int);
    }

    sync_atomic_uint32_store(&race_stop, 0);
    sync_atomic_int64_store(&race_misses, 0);
    sync_atomic_int64_store(&race_lookups, 0);

    runtime_main(BURROW_FN(Func, race_main, NULL));

    CHECK_INT_EQ((int)sync_atomic_int64_load(&race_misses), 0);
    CHECK(sync_atomic_int64_load(&race_lookups) > 0);

    /* And every latecomer arrived, which is the writer's half. Looked up by
     * name rather than counted, so a table that grew and lost an entry on the
     * way is caught rather than averaged out. */
    Byte q[16];
    for (Int i = 0; i < LATECOMERS; i++) {
        Str name = type_qualified_name(&latecomers[i], q, (Int)sizeof q);
        CHECK(type_by_name(name, NULL) == &latecomers[i]);
    }
}

#define TESTS(X)                                                                       \
    X(TestARegisteredTypeIsFoundByName)                                                \
    X(TestATypeThatWasNotRegisteredIsNotFound)                                         \
    X(TestANameNothingRegisteredGivesNull)                                             \
    X(TestThePackagePathIsPartOfTheName)                                               \
    X(TestAQualifiedNameCanBeWrittenOut)                                               \
    X(TestANameTooLongForTheBufferIsNotWrittenAtAll)                                   \
    X(TestAnUnregisteredNameSaysItIsUnregistered)                                      \
    X(TestANameThatIsNotANameSaysSoInstead)                                            \
    X(TestANameThatIsFoundLeavesNoErrorBehind)                                         \
    X(TestTheErrorIsOptionalLikeEveryOtherOutParameter)                                \
    X(TestATypeCanBeRegisteredWhileTheProgramRuns)                                     \
    X(TestRegisteringTheSameTypeTwiceChangesNothing)                                   \
    X(TestASecondCopyOfOneTypeIsAccepted)                                              \
    X(TestADifferentTypeUnderATakenNameIsRefused)                                      \
    X(TestADescriptorWithNothingToRegisterItUnderIsRefused)                            \
    X(TestARegistrationThatWorkedLeavesNoErrorBehind)                                  \
    X(TestTheThreeAnswersAreThreeDifferentAnswers)                                     \
    X(TestIdentityIsTheAddressWhenThereIsOne)                                          \
    X(TestIdentityFallsBackToTheName)                                                  \
    X(TestAnUnnamedTypeHasNoIdentityBeyondItsAddress)                                  \
    X(TestTheRegistryHoldsWhatWasPutInIt)                                              \
    X(TestLookupsKeepWorkingWhileTheTableGrowsUnderneathThem)

TESTING_MAIN_BARE(TESTS)
