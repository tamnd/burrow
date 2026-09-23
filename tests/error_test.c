/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "harness.h"

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

static bool msg_is(Error err, const char *want) {
    return str_eq(error_text(err), str_from_cstr(want));
}

/* Two sentinels of our own, because the library has none yet to point at and
 * because that is how every package in Go declares its own. */
BURROW_SENTINEL_ERROR(test_err_closed, "file already closed");
BURROW_SENTINEL_ERROR(test_err_not_exist, "file does not exist");

/* ------------------------------------------------ a wrapping error by hand
 *
 * There is no public constructor for a wrapping error, because in Go there is
 * not one either: fmt.Errorf with %w is the only way, and fmt is not written.
 * What a caller does have is the vtable, so this is a wrapping error built the
 * way a ported package will build one, which makes it a better test of the
 * vtable than a helper in the library would be. */
typedef struct Wrapped {
    Str text;
    Error inner;
} Wrapped;

static Str wrapped_message(const void *self) {
    return ((const Wrapped *)self)->text;
}

static Error wrapped_unwrap(const void *self) {
    return ((const Wrapped *)self)->inner;
}

static const Type wrapped_type = {
    {(const Byte *)"Wrapped", 7},
    {(const Byte *)"errortest", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(Wrapped),
    (uint16_t)_Alignof(Wrapped),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x77726170u,
    NULL,
};

static const ErrorVT wrapped_vt = {
    &wrapped_type, wrapped_message, wrapped_unwrap, NULL, NULL, NULL,
};

static Error wrap(Str text, Error inner) {
    Wrapped *w = BURROW_NEW(a, Wrapped);
    w->text = text;
    w->inner = inner;
    return (Error){&wrapped_vt, w};
}

/* An error with a custom Is, which is what os.ErrNotExist's wrappers do and
 * what syscall.Errno does: it matches something it is not identical to. */
typedef struct Errno {
    int code;
} Errno;

static Str errno_message(const void *self) {
    (void)self;
    return BURROW_S("errno");
}

static bool errno_is(const void *self, Error target) {
    const Errno *e = (const Errno *)self;
    return e->code == 2 && target.vt == test_err_not_exist.vt &&
           target.data == test_err_not_exist.data;
}

static const ErrorVT errno_vt = {
    NULL, errno_message, NULL, NULL, errno_is, NULL,
};

static Error make_errno(int code) {
    Errno *e = BURROW_NEW(a, Errno);
    e->code = code;
    return (Error){&errno_vt, e};
}

/* An error with a custom As, which is the rarer half of the pair. */
typedef struct Disguised {
    Wrapped payload;
} Disguised;

static Str disguised_message(const void *self) {
    (void)self;
    return BURROW_S("disguised");
}

static const void *disguised_as(const void *self, const Type *target) {
    if (target == &wrapped_type)
        return &((const Disguised *)self)->payload;
    return NULL;
}

static const ErrorVT disguised_vt = {
    NULL, disguised_message, NULL, NULL, NULL, disguised_as,
};

/* ------------------------------------------------------------- the basics */

TEST(the_zero_error_is_success) {
    Error none = BURROW_NO_ERROR;

    CHECK(BURROW_OK(none));
    CHECK(!BURROW_FAILED(none));
    CHECK(none.vt == NULL);
    CHECK(none.data == NULL);

    /* A struct with an Error field in it starts out successful without anybody
     * saying so, which is the reason the zero value has to mean this. */
    struct Holder {
        Int n;
        Error err;
    } h = {0, {NULL, NULL}};
    CHECK(BURROW_OK(h.err));
}

TEST(the_message_of_no_error_is_empty_rather_than_a_crash) {
    Error none = BURROW_NO_ERROR;
    CHECK(str_is_empty(error_text(none)));
    CHECK_INT_EQ(error_text(none).len, 0);
}

TEST(errors_new_carries_its_text) {
    Error err = errors_new(a, BURROW_S("no such host"));

    CHECK(BURROW_FAILED(err));
    CHECK(msg_is(err, "no such host"));
}

TEST(errors_new_copies_the_text) {
    char buf[] = "temporary";
    Error err = errors_new(a, str_from_bytes(buf, 9));

    /* Scribble over the original. Go's errors.New copies the string because a
     * Go string is immutable; ours has to copy because a C buffer is not. */
    memset(buf, 'x', 9);

    CHECK(msg_is(err, "temporary"));
}

TEST(errors_new_of_an_empty_string_is_still_an_error) {
    Error err = errors_new(a, BURROW_STR_EMPTY);

    CHECK(BURROW_FAILED(err));
    CHECK_INT_EQ(error_text(err).len, 0);
}

/* This is the reason sentinels are declared once at file scope and not built
 * where they are used. Go's errors.New does the same thing and the same trap is
 * in Go's own documentation. */
TEST(two_errors_with_the_same_text_are_not_the_same_error) {
    Error one = errors_new(a, BURROW_S("EOF"));
    Error two = errors_new(a, BURROW_S("EOF"));

    CHECK(msg_is(one, "EOF"));
    CHECK(msg_is(two, "EOF"));
    CHECK(!errors_is(one, two));
    CHECK(!errors_is(two, one));
    CHECK(errors_is(one, one));
}

/* ------------------------------------------------------------- sentinels */

TEST(a_sentinel_needs_no_allocator) {
    /* Nothing in this test touches a. That is the whole point: a comparison
     * against io.EOF on a hot read path cannot be allowed to allocate. */
    CHECK(BURROW_FAILED(test_err_closed));
    CHECK(msg_is(test_err_closed, "file already closed"));
    CHECK(errors_is(test_err_closed, test_err_closed));
}

TEST(two_sentinels_are_distinct_without_anybody_numbering_them) {
    CHECK(!errors_is(test_err_closed, test_err_not_exist));
    CHECK(!errors_is(test_err_not_exist, test_err_closed));

    /* They share a vtable, so the data pointer is what tells them apart, and
     * the data pointer is the address of each one's own static Str. */
    CHECK(test_err_closed.vt == test_err_not_exist.vt);
    CHECK(test_err_closed.data != test_err_not_exist.data);
}

TEST(the_out_of_memory_error_is_available_without_memory) {
    Fixed tiny;
    Byte buf[8];
    Error err;

    /* Eight bytes, which is not enough for the struct let alone the text. The
     * point is that errors_new still returns an error rather than a success, a
     * crash or an error with no message in it. */
    fixed_init(&tiny, buf, sizeof(buf));
    err = errors_new(fixed_allocator(&tiny), BURROW_S("this will not fit"));

    CHECK(BURROW_FAILED(err));
    CHECK(errors_is(err, burrow_err_out_of_memory));
    CHECK(msg_is(err, "out of memory"));
}

TEST(join_reports_out_of_memory_rather_than_losing_the_errors) {
    Fixed tiny;
    Byte buf[16];
    Error err;

    fixed_init(&tiny, buf, sizeof(buf));
    err = errors_join_v(fixed_allocator(&tiny), 2, test_err_closed, test_err_not_exist);

    CHECK(BURROW_FAILED(err));
    CHECK(errors_is(err, burrow_err_out_of_memory));
}

/* ---------------------------------------------------------------- unwrap */

TEST(an_error_that_wraps_nothing_unwraps_to_nothing) {
    Error err = errors_new(a, BURROW_S("plain"));

    CHECK(BURROW_OK(errors_unwrap(err)));
    CHECK(BURROW_OK(errors_unwrap(BURROW_NO_ERROR)));
    CHECK(BURROW_OK(errors_unwrap(test_err_closed)));
}

TEST(unwrap_gives_back_exactly_what_was_wrapped) {
    Error outer = wrap(BURROW_S("open /etc/passwd"), test_err_not_exist);
    Error got = errors_unwrap(outer);

    CHECK(msg_is(outer, "open /etc/passwd"));
    CHECK(got.vt == test_err_not_exist.vt);
    CHECK(got.data == test_err_not_exist.data);
    CHECK(BURROW_OK(errors_unwrap(got)));
}

/* ------------------------------------------------------------------- is */

TEST(is_finds_a_sentinel_through_a_chain) {
    Error inner = wrap(BURROW_S("read"), test_err_closed);
    Error outer = wrap(BURROW_S("copy"), inner);

    CHECK(errors_is(outer, test_err_closed));
    CHECK(errors_is(outer, inner));
    CHECK(errors_is(outer, outer));
    CHECK(!errors_is(outer, test_err_not_exist));

    /* And not the other way round, because wrapping is directional. */
    CHECK(!errors_is(test_err_closed, outer));
    CHECK(!errors_is(inner, outer));
}

TEST(is_of_nothing_is_true_only_for_nothing) {
    CHECK(errors_is(BURROW_NO_ERROR, BURROW_NO_ERROR));
    CHECK(!errors_is(test_err_closed, BURROW_NO_ERROR));
    CHECK(!errors_is(BURROW_NO_ERROR, test_err_closed));
}

TEST(is_asks_a_custom_is_before_giving_up) {
    Error enoent = make_errno(2);
    Error eperm = make_errno(1);

    CHECK(errors_is(enoent, test_err_not_exist));
    CHECK(!errors_is(eperm, test_err_not_exist));
    CHECK(!errors_is(enoent, test_err_closed));

    /* Through a wrapper too, since that is how os returns it. */
    CHECK(errors_is(wrap(BURROW_S("open"), enoent), test_err_not_exist));
}

/* Filling in both unwrap slots is a mistake with no Go equivalent, because a Go
 * type has one method of that name and cannot do it. So the chain form wins,
 * which is the first case in Go's type switch, and the multi slot is never
 * consulted. This one returns nothing, so if it were consulted the sentinel
 * underneath would be lost and the checks below would fail. */
static Slice confused_unwrap_multi(const void *self) {
    (void)self;
    return slice_nil(TYPE_ERROR);
}

TEST(an_error_that_wraps_both_ways_uses_the_chain) {
    static const ErrorVT confused_vt = {
        NULL, wrapped_message, wrapped_unwrap, confused_unwrap_multi, NULL, NULL,
    };
    Wrapped *w = BURROW_NEW(a, Wrapped);
    Error err;

    w->text = BURROW_S("confused");
    w->inner = test_err_closed;
    err = (Error){&confused_vt, w};

    CHECK(errors_is(err, err));
    CHECK(errors_is(err, test_err_closed));
    CHECK(errors_is(errors_unwrap(err), test_err_closed));
}

/* ------------------------------------------------------------------- as */

TEST(as_finds_a_concrete_type_through_a_chain) {
    Error inner = wrap(BURROW_S("inner"), test_err_closed);
    Error outer = wrap(BURROW_S("outer"), inner);
    const Wrapped *got = errors_as(outer, &wrapped_type);

    /* The outermost match wins, which is what Go's As does: it stops at the
     * first error in the chain that fits. */
    CHECK(got != NULL);
    CHECK(got == (const Wrapped *)outer.data);
    CHECK(str_eq(got->text, BURROW_S("outer")));
}

TEST(as_gives_null_rather_than_a_guess) {
    Error err = errors_new(a, BURROW_S("plain"));

    CHECK(errors_as(err, &wrapped_type) == NULL);
    CHECK(errors_as(BURROW_NO_ERROR, &wrapped_type) == NULL);
    CHECK(errors_as(test_err_closed, &wrapped_type) == NULL);
    CHECK(errors_as(err, NULL) == NULL);
}

TEST(as_asks_a_custom_as_before_giving_up) {
    Disguised *d = BURROW_NEW(a, Disguised);
    Error err;
    const Wrapped *got;

    d->payload.text = BURROW_S("payload");
    d->payload.inner = BURROW_NO_ERROR;
    err = (Error){&disguised_vt, d};

    got = errors_as(err, &wrapped_type);
    CHECK(got == &d->payload);
    CHECK(str_eq(got->text, BURROW_S("payload")));

    /* And it declines anything else rather than handing back its own bytes. */
    CHECK(errors_as(err, TYPE_ERROR) == NULL);
}

/* ----------------------------------------------------------------- join */

TEST(join_of_nothing_is_nothing) {
    Slice empty = slice_nil(TYPE_ERROR);

    CHECK(BURROW_OK(errors_join(a, empty)));
    CHECK(BURROW_OK(errors_join_v(a, 0)));
    CHECK(BURROW_OK(errors_join_v(a, 2, BURROW_NO_ERROR, BURROW_NO_ERROR)));
}

TEST(join_drops_the_successes) {
    Error err = errors_join_v(a, 4, BURROW_NO_ERROR, test_err_closed, BURROW_NO_ERROR,
                              test_err_not_exist);

    CHECK(BURROW_FAILED(err));
    CHECK(msg_is(err, "file already closed\nfile does not exist"));
    CHECK(errors_is(err, test_err_closed));
    CHECK(errors_is(err, test_err_not_exist));
}

TEST(join_of_one_still_wraps_it) {
    Error err = errors_join_v(a, 2, BURROW_NO_ERROR, test_err_closed);

    /* Go wraps a single survivor rather than returning it unchanged, and that
     * has to hold here or errors_is would answer differently depending on
     * whether the caller filtered first. */
    CHECK(BURROW_FAILED(err));
    CHECK(!errors_is(test_err_closed, err));
    CHECK(errors_is(err, test_err_closed));
    CHECK(msg_is(err, "file already closed"));
}

TEST(join_takes_a_slice_as_well_as_arguments) {
    Slice errs = slice_make(a, TYPE_ERROR, 0, 3);
    Error err;

    errs = BURROW_APPEND(Error, a, errs, test_err_closed);
    errs = BURROW_APPEND(Error, a, errs, BURROW_NO_ERROR);
    errs = BURROW_APPEND(Error, a, errs, test_err_not_exist);

    err = errors_join(a, errs);
    CHECK(msg_is(err, "file already closed\nfile does not exist"));
    CHECK(errors_is(err, test_err_closed));
    CHECK(errors_is(err, test_err_not_exist));
}

TEST(is_searches_the_whole_tree_and_not_just_the_first_branch) {
    Error left = wrap(BURROW_S("left"), test_err_closed);
    Error right = wrap(BURROW_S("right"), test_err_not_exist);
    Error both = errors_join_v(a, 2, left, right);
    Error nested = errors_join_v(a, 2, errors_new(a, BURROW_S("first")), both);

    CHECK(errors_is(both, test_err_closed));
    CHECK(errors_is(both, test_err_not_exist));

    /* Two levels of tree with the answer in the second branch of the second
     * level, which is the case a chain walk would miss. */
    CHECK(errors_is(nested, test_err_not_exist));
    CHECK(errors_is(nested, left));
    CHECK(!errors_is(nested, errors_new(a, BURROW_S("first"))));
}

TEST(as_searches_the_whole_tree_too) {
    Error plain = errors_new(a, BURROW_S("plain"));
    Error wrapped = wrap(BURROW_S("second branch"), test_err_closed);
    Error both = errors_join_v(a, 2, plain, wrapped);
    const Wrapped *got = errors_as(both, &wrapped_type);

    CHECK(got != NULL);
    CHECK(got == (const Wrapped *)wrapped.data);
    CHECK(str_eq(got->text, BURROW_S("second branch")));
}

TEST(unwrap_of_a_join_gives_nothing) {
    Error err = errors_join_v(a, 2, test_err_closed, test_err_not_exist);

    /* errors.Unwrap is defined over Unwrap() error and says nothing about the
     * tree form, so a joined error is not unwrappable by it. Go behaves the
     * same way and it surprises people, so it is pinned here. */
    CHECK(BURROW_OK(errors_unwrap(err)));
    CHECK(errors_is(err, test_err_closed));
}

TEST(a_joined_error_holds_its_children_rather_than_copying_their_text) {
    Error inner = errors_new(a, BURROW_S("inner"));
    Error err = errors_join_v(a, 1, inner);

    CHECK(errors_is(err, inner));
    CHECK(msg_is(err, "inner"));
}

TEST(joining_errors_with_empty_messages_still_puts_the_newlines_in) {
    Error blank = errors_new(a, BURROW_STR_EMPTY);
    Error err = errors_join_v(a, 3, blank, test_err_closed, blank);

    CHECK(msg_is(err, "\nfile already closed\n"));
    CHECK(errors_is(err, test_err_closed));
    CHECK(errors_is(err, blank));
}

/* ----------------------------------------------------------- descriptor */

TEST(the_error_descriptor_describes_an_interface) {
    CHECK_INT_EQ(TYPE_ERROR->kind, KIND_INTERFACE);
    CHECK_INT_EQ(TYPE_ERROR->size, sizeof(Error));
    CHECK_INT_EQ(TYPE_ERROR->align, _Alignof(Error));
    CHECK(str_eq(TYPE_ERROR->name, BURROW_S("error")));

    /* No package path, because error is a language builtin and not a library
     * type, the same as int and string. */
    CHECK(str_is_empty(TYPE_ERROR->pkg_path));
}

TEST(the_error_descriptor_compares_by_both_words) {
    Error x = test_err_closed;
    Error y = test_err_closed;
    Error z = test_err_not_exist;

    CHECK(type_equal(TYPE_ERROR, &x, &y));
    CHECK(!type_equal(TYPE_ERROR, &x, &z));
    CHECK(type_hash(TYPE_ERROR, &x, 0) == type_hash(TYPE_ERROR, &y, 0));
    CHECK(type_hash(TYPE_ERROR, &x, 0) != type_hash(TYPE_ERROR, &z, 0));
}

TEST(a_slice_of_errors_works_like_any_other_slice) {
    Slice errs = slice_make(a, TYPE_ERROR, 2, 2);

    CHECK(BURROW_OK(BURROW_AT(Error, errs, 0)));
    CHECK(BURROW_OK(BURROW_AT(Error, errs, 1)));

    BURROW_AT(Error, errs, 0) = test_err_closed;
    CHECK(errors_is(BURROW_AT(Error, errs, 0), test_err_closed));
    CHECK(BURROW_OK(BURROW_AT(Error, errs, 1)));
}

int main(void) {
    setup();
    RUN(the_zero_error_is_success);
    RUN(the_message_of_no_error_is_empty_rather_than_a_crash);
    RUN(errors_new_carries_its_text);
    RUN(errors_new_copies_the_text);
    RUN(errors_new_of_an_empty_string_is_still_an_error);
    RUN(two_errors_with_the_same_text_are_not_the_same_error);
    RUN(a_sentinel_needs_no_allocator);
    RUN(two_sentinels_are_distinct_without_anybody_numbering_them);
    RUN(the_out_of_memory_error_is_available_without_memory);
    RUN(join_reports_out_of_memory_rather_than_losing_the_errors);
    RUN(an_error_that_wraps_nothing_unwraps_to_nothing);
    RUN(unwrap_gives_back_exactly_what_was_wrapped);
    RUN(is_finds_a_sentinel_through_a_chain);
    RUN(is_of_nothing_is_true_only_for_nothing);
    RUN(is_asks_a_custom_is_before_giving_up);
    RUN(an_error_that_wraps_both_ways_uses_the_chain);
    RUN(as_finds_a_concrete_type_through_a_chain);
    RUN(as_gives_null_rather_than_a_guess);
    RUN(as_asks_a_custom_as_before_giving_up);
    RUN(join_of_nothing_is_nothing);
    RUN(join_drops_the_successes);
    RUN(join_of_one_still_wraps_it);
    RUN(join_takes_a_slice_as_well_as_arguments);
    RUN(is_searches_the_whole_tree_and_not_just_the_first_branch);
    RUN(as_searches_the_whole_tree_too);
    RUN(unwrap_of_a_join_gives_nothing);
    RUN(a_joined_error_holds_its_children_rather_than_copying_their_text);
    RUN(joining_errors_with_empty_messages_still_puts_the_newlines_in);
    RUN(the_error_descriptor_describes_an_interface);
    RUN(the_error_descriptor_compares_by_both_words);
    RUN(a_slice_of_errors_works_like_any_other_slice);
    teardown();
    return harness_report("error");
}
