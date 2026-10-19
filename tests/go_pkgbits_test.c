/* Derived from Go's src/internal/pkgbits/pkgbits_test.go. Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/go/pkgbits.h"
#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static void TestRoundTrip(TestingT *t) {
    static const PbVersion versions[] = {PB_V0, PB_V1, PB_V2, PB_V3};
    for (Int i = 0; i < NELEM(versions); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);

        PbPkgEncoder pw = burrow__pb_new_pkg_encoder(a, versions[i], -1);
        PbEncoder *w = burrow__pb_new_encoder(&pw, PB_SECTION_META, PB_SYNC_PUBLIC);
        burrow__pb_flush(w);

        StringsBuilder b = STRINGS_BUILDER(a);
        Byte fingerprint[8];
        burrow__pb_dump_to(&pw, strings_builder_as_io_writer(&b), fingerprint);
        Str input = strings_builder_string(&b);

        PbPkgDecoder pr = burrow__pb_new_pkg_decoder(a, S("package_id"), input);
        PbDecoder r = burrow__pb_new_decoder(&pr, PB_SECTION_META, PB_PUBLIC_ROOT_IDX,
                                             PB_SYNC_PUBLIC);

        if (burrow__pb_decoder_version(&r) != burrow__pb_encoder_version(w))
            testing_t_errorf_v(t,
                               "Expected reader version %d to be the writer version %d",
                               (int64_t)burrow__pb_decoder_version(&r),
                               (int64_t)burrow__pb_encoder_version(w));
        arena_free(&ar);
    }
}

/* Type checker to enforce that known V* have the constant values they must
 * have. */
_Static_assert(PB_V0 == 0, "V0");
_Static_assert(PB_V1 == 1, "V1");
_Static_assert(PB_V2 == 2, "V2");
_Static_assert(PB_V3 == 3, "V3");

typedef struct VfPair {
    PbVersion v;
    PbField f;
} VfPair;

static void TestVersions(TestingT *t) {
    /* has field tests */
    static const VfPair has[] = {
        {PB_V1, PB_FIELD_FLAGS},
        {PB_V2, PB_FIELD_FLAGS},
        {PB_V0, PB_FIELD_HAS_INIT},
        {PB_V1, PB_FIELD_HAS_INIT},
        {PB_V0, PB_FIELD_DERIVED_FUNC_INSTANCE},
        {PB_V1, PB_FIELD_DERIVED_FUNC_INSTANCE},
        {PB_V0, PB_FIELD_DERIVED_INFO_NEEDED},
        {PB_V1, PB_FIELD_DERIVED_INFO_NEEDED},
        {PB_V2, PB_FIELD_ALIAS_TYPE_PARAM_NAMES},
        {PB_V3, PB_FIELD_COMPACT_COMP_LITERALS},
    };
    for (Int i = 0; i < NELEM(has); i++) {
        if (!burrow__pb_version_has(has[i].v, has[i].f))
            testing_t_errorf_v(t, "Expected version %d to have field %d",
                               (int64_t)has[i].v, (int64_t)has[i].f);
    }

    /* does not have field tests */
    static const VfPair has_not[] = {
        {PB_V0, PB_FIELD_FLAGS},
        {PB_V2, PB_FIELD_HAS_INIT},
        {PB_V2, PB_FIELD_DERIVED_FUNC_INSTANCE},
        {PB_V2, PB_FIELD_DERIVED_INFO_NEEDED},
        {PB_V0, PB_FIELD_ALIAS_TYPE_PARAM_NAMES},
        {PB_V1, PB_FIELD_ALIAS_TYPE_PARAM_NAMES},
        {PB_V0, PB_FIELD_COMPACT_COMP_LITERALS},
        {PB_V1, PB_FIELD_COMPACT_COMP_LITERALS},
        {PB_V2, PB_FIELD_COMPACT_COMP_LITERALS},
    };
    for (Int i = 0; i < NELEM(has_not); i++) {
        if (burrow__pb_version_has(has_not[i].v, has_not[i].f))
            testing_t_errorf_v(t, "Expected version %d to not have field %d",
                               (int64_t)has_not[i].v, (int64_t)has_not[i].f);
    }
}

#define TESTS(X)                                                                       \
    X(TestRoundTrip)                                                                   \
    X(TestVersions)

TESTING_MAIN(TESTS)
