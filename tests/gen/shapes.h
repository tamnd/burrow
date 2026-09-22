/* Plain structs for the generator to read, written the way somebody who had
 * never heard of burrow would write them.
 *
 * That is the point of the fixture. Nothing here is a macro call, the fields
 * are declared as fields, and the only burrow in sight is the marker comment
 * and the tags, both of which expand to nothing in an ordinary build. What
 * tests/gen_test.c then checks is that the descriptor generated from this is
 * the same descriptor the DSL would have produced from the same fields.
 *
 * tests/gen/shapes_gen.c is generated from this file and is checked in, so
 * building burrow needs no libclang. tools/check-gen.sh regenerates it and
 * diffs, on any machine that has one.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_GEN_SHAPES_H
#define BURROW_TESTS_GEN_SHAPES_H

#include "burrow/core.h"
#include "burrow/declare.h"

/* burrow:reflect */
typedef struct {
    Int X BURROW_TAG("json:\"x\"");
    Int Y BURROW_TAG("json:\"y\"");
    Str Label BURROW_TAG("json:\"label,omitempty\"");
} GenPoint;

/* The other marker, on a type that also has a struct tag of its own so that
 * both spellings are exercised rather than only the one the docs show. */
typedef struct GenSpan {
    Byte Small BURROW_TAG("db:\"small\"");
    int32_t Big BURROW_TAG("db:\"big\"");
    bool Flag;
} GenSpan BURROW_REFLECT;

/* Not marked, and the generator has to leave it alone. If it did not, this
 * would fail to compile rather than fail a check, because there is no
 * descriptor for a char and the paste would ask for one. */
typedef struct {
    char whatever;
} GenIgnored;

#endif /* BURROW_TESTS_GEN_SHAPES_H */
