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
#include "burrow/proc.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include "harness.h"

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

TEST(a_registered_type_is_found_by_name) {
    const Type *t = type_by_name(BURROW_S("RegPoint"));

    CHECK(t == TYPE_OF(RegPoint));
    if (t != NULL)
        CHECK(str_eq(t->name, BURROW_S("RegPoint")));
}

TEST(a_type_that_was_not_registered_is_not_found) {
    /* It still has a descriptor and reflection on a value of it still works.
     * Registration is only about finding it from a name. */
    CHECK(type_by_name(BURROW_S("QuietPoint")) == NULL);
    CHECK(TYPE_OF(QuietPoint) != NULL);
}

TEST(a_name_nothing_registered_gives_null) {
    CHECK(type_by_name(BURROW_S("NoSuchType")) == NULL);
    CHECK(type_by_name(BURROW_S("a.b.c.NoSuchType")) == NULL);
    CHECK(type_by_name(BURROW_STR_EMPTY) == NULL);
}

TEST(the_package_path_is_part_of_the_name) {
    const Type *t = type_by_name(BURROW_S("github.com/tamnd/shapes.v2.Circle"));
    CHECK(t == &burrow_type_Circle);

    /* The bare name is not the qualified name, and a type in a package is not
     * reachable without it. Two packages are allowed to have a Circle. */
    CHECK(type_by_name(BURROW_S("Circle")) == NULL);

    /* Nor is a prefix of the package path. */
    CHECK(type_by_name(BURROW_S("shapes.v2.Circle")) == NULL);
}

TEST(a_qualified_name_can_be_written_out) {
    Byte buf[64];

    Str q = type_qualified_name(&burrow_type_Circle, buf, (Int)sizeof buf);
    CHECK(str_eq(q, BURROW_S("github.com/tamnd/shapes.v2.Circle")));

    /* No package path means no dot, not a leading one. */
    Str p = type_qualified_name(TYPE_OF(RegPoint), buf, (Int)sizeof buf);
    CHECK(str_eq(p, BURROW_S("RegPoint")));

    /* And it round trips, which is the property that matters: the name this
     * writes is the name the lookup takes. */
    CHECK(type_by_name(p) == TYPE_OF(RegPoint));
}

TEST(a_name_too_long_for_the_buffer_is_cut_short) {
    Byte buf[8];

    Str q = type_qualified_name(&burrow_type_Circle, buf, (Int)sizeof buf);
    CHECK_INT_EQ((int)q.len, 8);
    CHECK(str_eq(q, BURROW_S("github.c")));

    /* Not allocated for, not an error, and not silently longer than the buffer,
     * which is the one of those three that would matter. */
    CHECK(type_qualified_name(&burrow_type_Circle, NULL, 64).len == 0);
    CHECK(type_qualified_name(NULL, buf, (Int)sizeof buf).len == 0);
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

TEST(a_type_can_be_registered_while_the_program_runs) {
    CHECK(type_by_name(BURROW_S("plugin.Late")) == NULL);

    Int before = type_registry_len();
    CHECK(type_register(&late_type));
    CHECK_INT_EQ((int)(type_registry_len() - before), 1);

    CHECK(type_by_name(BURROW_S("plugin.Late")) == &late_type);
}

TEST(registering_the_same_type_twice_changes_nothing) {
    CHECK(type_register(&late_type));

    Int before = type_registry_len();
    CHECK(type_register(&late_type));
    CHECK_INT_EQ((int)(type_registry_len() - before), 0);

    CHECK(type_register(NULL) == false);
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

TEST(a_second_copy_of_one_type_is_accepted) {
    CHECK(type_register(&late_type));

    Int before = type_registry_len();
    CHECK(type_register(&late_again));
    CHECK_INT_EQ((int)(type_registry_len() - before), 0);

    /* The first one stays. Which of two identical descriptors answers is not
     * something a caller can tell apart, but it should not change. */
    CHECK(type_by_name(BURROW_S("plugin.Late")) == &late_type);
}

TEST(a_different_type_under_a_taken_name_is_refused) {
    CHECK(type_register(&late_type));

    CHECK(type_register(&late_impostor) == false);

    /* Refused means the first one is still there, not that the name is now
     * broken. Whoever is already holding this name keeps it. */
    CHECK(type_by_name(BURROW_S("plugin.Late")) == &late_type);
}

TEST(identity_is_the_address_when_there_is_one) {
    CHECK(type_same(TYPE_OF(RegPoint), TYPE_OF(RegPoint)));
    CHECK(type_same(TYPE_INT, TYPE_OF(Int)));
    CHECK(!type_same(TYPE_OF(RegPoint), TYPE_OF(QuietPoint)));
    CHECK(!type_same(NULL, TYPE_INT));
    CHECK(type_same(NULL, NULL));
}

TEST(identity_falls_back_to_the_name) {
    /* Two descriptors, one type. This is the shared library case and the only
     * reason type_same is a function rather than a comparison. */
    CHECK(&late_type != &late_again);
    CHECK(type_same(&late_type, &late_again));

    /* One name, two types. The kind is enough to tell these apart, and when it
     * is not the size usually is, and when neither is there is nothing left to
     * ask. That boundary is in the doc comment rather than pretended away. */
    CHECK(!type_same(&late_type, &late_impostor));
}

TEST(an_unnamed_type_has_no_identity_beyond_its_address) {
    /* []int has no name, so two descriptors for it cannot be shown to be the
     * same type from here. Saying no is the honest answer: saying yes would
     * make every unnamed slice descriptor equal to every other. */
    const Type *a = TYPE_OF(IntsA);
    const Type *b = TYPE_OF(IntsB);

    CHECK(a != b);
    CHECK(!type_same(a, b));
    CHECK(type_same(a, a));
}

TEST(the_registry_holds_what_was_put_in_it) {
    /* Both of the file scope registrations, and nothing that was not
     * registered. The count is not checked against a number because the section
     * holds whatever the whole program registered, and this file is linked with
     * the library. */
    CHECK(type_registry_len() >= 2);
    CHECK(type_by_name(BURROW_S("RegPoint")) != NULL);
    CHECK(type_by_name(BURROW_S("github.com/tamnd/shapes.v2.Circle")) != NULL);
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
        if (type_by_name(BURROW_S("RegPoint")) != TYPE_OF(RegPoint))
            sync_atomic_int64_add(&race_misses, 1);
        if (type_by_name(BURROW_S("github.com/tamnd/shapes.v2.Circle")) !=
            &burrow_type_Circle)
            sync_atomic_int64_add(&race_misses, 1);
        if (type_by_name(BURROW_S("NotThere")) != NULL)
            sync_atomic_int64_add(&race_misses, 1);
        sync_atomic_int64_add(&race_lookups, 1);
        runtime_gosched();
    }

    sync_atomic_int64_add(&race_running, -1);
}

static void race_writer(void *arg) {
    (void)arg;

    for (Int i = 0; i < LATECOMERS; i++) {
        if (!type_register(&latecomers[i]))
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

TEST(lookups_keep_working_while_the_table_grows_underneath_them) {
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
        CHECK(type_by_name(name) == &latecomers[i]);
    }
}

int main(void) {
    RUN(a_registered_type_is_found_by_name);
    RUN(a_type_that_was_not_registered_is_not_found);
    RUN(a_name_nothing_registered_gives_null);
    RUN(the_package_path_is_part_of_the_name);
    RUN(a_qualified_name_can_be_written_out);
    RUN(a_name_too_long_for_the_buffer_is_cut_short);
    RUN(a_type_can_be_registered_while_the_program_runs);
    RUN(registering_the_same_type_twice_changes_nothing);
    RUN(a_second_copy_of_one_type_is_accepted);
    RUN(a_different_type_under_a_taken_name_is_refused);
    RUN(identity_is_the_address_when_there_is_one);
    RUN(identity_falls_back_to_the_name);
    RUN(an_unnamed_type_has_no_identity_beyond_its_address);
    RUN(the_registry_holds_what_was_put_in_it);
    RUN(lookups_keep_working_while_the_table_grows_underneath_them);

    return harness_report("registry");
}
