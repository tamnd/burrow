/* go/types: support for more detailed error messages.
 *
 * Derived from Go's src/go/types/errsupport.go. Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/panic.h"
#include "burrow/utf8.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* tail returns the string s without its first (UTF-8) character. If s has at
 * most one character, the result is s. */
static Str tes_tail(Str s) {
    if (s.len == 0)
        return s;
    Int size = 0;
    (void)utf8_decode_rune_in_string(s, &size);
    if (size < s.len)
        return str_from_bytes(s.p + size, s.len - size);
    return s;
}

typedef enum TesCase {
    TES_MISSING,     /* no object found */
    TES_MISSPELLED,  /* found object with different spelling */
    TES_UNEXPORTED,  /* found object with name differing only in first letter */
    TES_INACCESSIBLE /* found object with matching name but inaccessible from the
                        current package */
} TesCase;

/* lookupError returns a case-specific error when a lookup of selector sel in
 * the given type fails but an object with alternative spelling (case folding)
 * is found. If struct_lit is set, the error message is specifically for struct
 * literal fields.
 *
 * Provide more detail if there is an unexported object, or one with different
 * capitalization. If selector and object are in the same package (==), export
 * doesn't matter, otherwise (!=) it does. Messages depend on whether it's a
 * general lookup or a field lookup in a struct literal.
 *
 *   case           sel     pkg   have   message (examples for general lookup)
 *   ------------------------------------------------------------------------
 *   ok             x.Foo   ==    Foo
 *   misspelled     x.Foo   ==    FoO    type X has no field or method Foo, but
 *                                       does have field FoO
 *   misspelled     x.Foo   ==    foo    ... but does have field foo
 *   misspelled     x.Foo   ==    foO    ... but does have field foO
 *
 *   misspelled     x.foo   ==    Foo    ... but does have field Foo
 *   misspelled     x.foo   ==    FoO    ... but does have field FoO
 *   ok             x.foo   ==    foo
 *   misspelled     x.foo   ==    foO    ... but does have field foO
 *
 *   ok             x.Foo   !=    Foo
 *   misspelled     x.Foo   !=    FoO    ... but does have field FoO
 *   unexported     x.Foo   !=    foo    ... but does have unexported field foo
 *   missing        x.Foo   !=    foO    type X has no field or method Foo
 *
 *   misspelled     x.foo   !=    Foo    ... but does have field Foo
 *   missing        x.foo   !=    FoO    type X has no field or method foo
 *   inaccessible   x.foo   !=    foo    cannot refer to unexported field foo
 *   missing        x.foo   !=    foO    type X has no field or method foo
 *
 * obj may be NULL. */
Str burrow__tc_lookup_error(TypesChecker *check, TypesType typ, Str sel,
                            TypesObject obj, bool struct_lit) {
    /* determine case */
    TesCase e = TES_MISSING;
    Str alt = {0}; /* alternative spelling of selector; if any */
    if (obj != NULL) {
        alt = types_object_name(obj);
        if (types_object_pkg(obj) == check->pkg) {
            burrow__types_assert(!str_eq(alt, sel)); /* otherwise there is no lookup
                                                         error */
            e = TES_MISSPELLED;
        } else if (burrow__types_is_exported(sel)) {
            if (burrow__types_is_exported(alt))
                e = TES_MISSPELLED;
            else if (str_eq(tes_tail(sel), tes_tail(alt)))
                e = TES_UNEXPORTED;
        } else if (burrow__types_is_exported(alt)) {
            if (str_eq(tes_tail(sel), tes_tail(alt)))
                e = TES_MISSPELLED;
        } else if (str_eq(sel, alt)) {
            e = TES_INACCESSIBLE;
        }
    }

    Str ts = burrow__tc_ts(check, typ);
    if (struct_lit) {
        switch (e) {
        case TES_MISSING:
            return GT_SPRINTF(check, "unknown field %s in struct literal of type %s",
                              sel, ts);
        case TES_MISSPELLED:
            return GT_SPRINTF(check,
                              "unknown field %s in struct literal of type %s, but does "
                              "have %s",
                              sel, ts, alt);
        case TES_UNEXPORTED:
            return GT_SPRINTF(check,
                              "unknown field %s in struct literal of type %s, but does "
                              "have unexported %s",
                              sel, ts, alt);
        case TES_INACCESSIBLE:
            return GT_SPRINTF(
                check,
                "cannot refer to unexported field %s in struct literal of "
                "type %s",
                alt, ts);
        default:
            break;
        }
    } else {
        Str what = BURROW_S("object");
        if (types_as_var(obj) != NULL)
            what = BURROW_S("field");
        else if (types_as_func(obj) != NULL)
            what = BURROW_S("method");
        switch (e) {
        case TES_MISSING:
            return GT_SPRINTF(check, "type %s has no field or method %s", ts, sel);
        case TES_MISSPELLED:
            return GT_SPRINTF(check,
                              "type %s has no field or method %s, but does have %s %s",
                              ts, sel, what, alt);
        case TES_UNEXPORTED:
            return GT_SPRINTF(check,
                              "type %s has no field or method %s, but does have "
                              "unexported %s %s",
                              ts, sel, what, alt);
        case TES_INACCESSIBLE:
            return GT_SPRINTF(check, "cannot refer to unexported %s %s", what, alt);
        default:
            break;
        }
    }

    panic_str(BURROW_S("unreachable"));
}
