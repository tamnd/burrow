/* The generator and the DSL have to agree, field for field.
 *
 * There are two ways to get a descriptor in burrow and neither is deprecated,
 * which is only tenable if they produce the same thing. So this file declares
 * the same struct twice, once as a plain annotated C struct that tools/burrow-
 * gen read (tests/gen/shapes.h) and once as a field list the DSL expanded, and
 * then compares the two descriptors member by member.
 *
 * Anything the generator gets wrong about a field shows up here as a mismatch
 * rather than as a wrong answer inside fmt or encoding/json a year later.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/type.h"

#include "gen/shapes_gen.h"
#include "harness.h"

#include <stdint.h>

/* The same three fields as GenPoint in tests/gen/shapes.h, in the same order,
 * with the same tags. Keep them in step by hand; that is what the test is for. */
#define DSL_POINT_FIELDS(F, T)                                                         \
    F(T, Int, X, "json:\"x\"")                                                         \
    F(T, Int, Y, "json:\"y\"")                                                         \
    F(T, Str, Label, "json:\"label,omitempty\"")

BURROW_STRUCT(DslPoint, DSL_POINT_FIELDS);

/* The same three as GenSpan, which is the interesting one for layout because a
 * Byte followed by an int32_t has padding between them on every target that
 * aligns an int32_t, and a generator that guessed offsets would get it wrong on
 * one of them. */
#define DSL_SPAN_FIELDS(F, T)                                                          \
    F(T, Byte, Small, "db:\"small\"")                                                  \
    F(T, int32_t, Big, "db:\"big\"")                                                   \
    F(T, bool, Flag, "")

BURROW_STRUCT(DslSpan, DSL_SPAN_FIELDS);

/* Everything about a descriptor except its name, which is the one thing the two
 * are entitled to disagree about since they describe differently named types. */
static bool same_shape(const Type *gen, const Type *dsl) {
    if (gen->kind != dsl->kind || gen->size != dsl->size || gen->align != dsl->align)
        return false;

    if (gen->nfield != dsl->nfield || gen->nmethod != dsl->nmethod)
        return false;

    for (uint16_t i = 0; i < gen->nfield; i++) {
        const Field *a = &gen->fields[i];
        const Field *b = &dsl->fields[i];

        if (!str_eq(a->name, b->name) || !str_eq(a->tag, b->tag))
            return false;
        if (a->type != b->type || a->offset != b->offset)
            return false;
    }

    return true;
}

TEST(a_generated_descriptor_matches_the_dsl) {
    CHECK(same_shape(TYPE_OF(GenPoint), TYPE_OF(DslPoint)));
    CHECK(same_shape(TYPE_OF(GenSpan), TYPE_OF(DslSpan)));
}

/* The name is the one thing that does differ, and it is the type's own name
 * rather than anything the generator invented. */
TEST(a_generated_descriptor_is_named_after_its_type) {
    CHECK(str_eq(TYPE_OF(GenPoint)->name, BURROW_S("GenPoint")));
    CHECK(str_eq(TYPE_OF(GenSpan)->name, BURROW_S("GenSpan")));
    CHECK_INT_EQ(TYPE_OF(GenPoint)->pkg_path.len, 0);
}

TEST(the_fields_are_the_ones_that_were_declared) {
    const Type *t = TYPE_OF(GenPoint);

    CHECK_INT_EQ(t->nfield, 3);
    CHECK(str_eq(t->fields[0].name, BURROW_S("X")));
    CHECK(str_eq(t->fields[1].name, BURROW_S("Y")));
    CHECK(str_eq(t->fields[2].name, BURROW_S("Label")));
}

/* A field with no BURROW_TAG gets an empty tag rather than a NULL one, so that
 * tag_lookup can be handed it without a NULL check first. */
TEST(a_field_with_no_tag_has_an_empty_tag) {
    const Field *f = type_field_by_name(TYPE_OF(GenSpan), BURROW_S("Flag"));

    CHECK(f != NULL);
    CHECK_INT_EQ(f->tag.len, 0);
    CHECK(f->tag.p != NULL);
}

/* The tags survive the trip through the annotation intact, quotes and commas
 * and all, which is the thing most likely to be mangled by a generator that
 * re-escapes a string one time too many or one too few. */
TEST(the_tags_survive_the_round_trip) {
    const Type *t = TYPE_OF(GenPoint);

    CHECK(str_eq(t->fields[0].tag, BURROW_S("json:\"x\"")));
    CHECK(str_eq(t->fields[2].tag, BURROW_S("json:\"label,omitempty\"")));
}

/* Offsets come from offsetof in the generated file, so they are the compiler's
 * answer for this target and not the generator's guess. Checking them against
 * offsetof here is checking that the right field got the right one. */
TEST(the_offsets_are_this_targets_offsets) {
    const Type *t = TYPE_OF(GenSpan);

    CHECK_INT_EQ(t->fields[0].offset, (uint32_t)offsetof(GenSpan, Small));
    CHECK_INT_EQ(t->fields[1].offset, (uint32_t)offsetof(GenSpan, Big));
    CHECK_INT_EQ(t->fields[2].offset, (uint32_t)offsetof(GenSpan, Flag));

    /* And the padding is real, so this is not a test that passes because every
     * offset happens to be the sum of the sizes before it. */
    CHECK(t->fields[1].offset > t->fields[0].offset + 1);
}

TEST(a_generated_type_is_a_struct_of_the_right_size) {
    CHECK_INT_EQ(TYPE_OF(GenSpan)->kind, KIND_STRUCT);
    CHECK_INT_EQ(TYPE_OF(GenSpan)->size, (uint32_t)sizeof(GenSpan));
    CHECK_INT_EQ(TYPE_OF(GenSpan)->align, (uint16_t)_Alignof(GenSpan));
}

/* Nothing was generated for GenIgnored, which is not marked. There is no way to
 * assert the absence of a symbol from inside C, so what this asserts is the
 * consequence: the type still exists and is still usable, it just has no
 * descriptor, and the build proves the second half by linking at all. */
TEST(an_unmarked_struct_is_left_alone) {
    GenIgnored g = {'x'};

    CHECK_INT_EQ(g.whatever, 'x');
}

int main(void) {
    RUN(a_generated_descriptor_matches_the_dsl);
    RUN(a_generated_descriptor_is_named_after_its_type);
    RUN(the_fields_are_the_ones_that_were_declared);
    RUN(a_field_with_no_tag_has_an_empty_tag);
    RUN(the_tags_survive_the_round_trip);
    RUN(the_offsets_are_this_targets_offsets);
    RUN(a_generated_type_is_a_struct_of_the_right_size);
    RUN(an_unmarked_struct_is_left_alone);

    return harness_report("gen");
}
