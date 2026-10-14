/* package reflect, interfaces: Implements, putting a value into an interface
 * with methods, and TypeAssert. Go's implements from reflect/type.go and
 * TypeAssert from reflect/value.go.
 *
 * Go answers Implements from method sets and makes the table an interface value
 * needs at run time when it has to. Here an interface with methods is a vtable
 * of C function pointers, and nothing can make one of those at run time. So
 * the answer comes from two places. A vtable recorded with BURROW_IMPLEMENTS
 * says yes, and is what a value goes into the interface with. An interface
 * whose descriptor lists its methods, which BURROW_IFACE_TYPE_DEFINE makes,
 * also says yes for a type with all of them, as Go would, but a value of that
 * type still needs a recorded vtable to go in, and reflect panics saying so
 * when there is none.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "burrow/panic.h"

/* Whether t is an unnamed pointer to a type that has the methods, which is
 * the one place a pointer gets them, as burrow__reflect_method_set says. */
static bool rfi_borrows(const Type *t) {
    return t->kind == KIND_POINTER && t->name.len == 0 && t->nmethod == 0 &&
           t->elem != NULL && t->elem->kind != KIND_POINTER &&
           t->elem->kind != KIND_INTERFACE;
}

static void *rfi_word(const void *p) {
    void *w;
    memcpy(&w, p, sizeof w);
    return w;
}

/* The vtable recorded for t as u, and the data word that goes with it in *data:
 * p itself, or the pointer at p when the vtable is the one for what t points
 * at. NULL when there is none. */
static const void *rfi_vtable(const Type *u, const Type *t, void *p, void **data) {
    const void *vt = type_vtable(t, u);
    if (vt != NULL) {
        *data = p;
        return vt;
    }
    if (rfi_borrows(t)) {
        vt = type_vtable(t->elem, u);
        if (vt != NULL) {
            *data = p == NULL ? NULL : rfi_word(p);
            return vt;
        }
    }
    return NULL;
}

/* Whether t has every method the interface u lists, each with the same type.
 * An unexported one only counts when t is from u's package, since the same
 * lower case name in two packages is two different names. */
static bool rfi_has_methods(const Type *u, const Type *t) {
    const Type *owner = rfi_borrows(t) ? t->elem : t;
    uint16_t n;
    const Method *ms = burrow__reflect_method_set(t, &n);
    for (uint16_t i = 0; i < u->nmethod; i++) {
        const Method *want = &u->methods[i];
        if (!burrow__reflect_exported(want->name) &&
            !str_eq(owner->pkg_path, u->pkg_path))
            return false;
        bool found = false;
        for (uint16_t j = 0; j < n && !found; j++)
            found = str_eq(ms[j].name, want->name) &&
                    burrow__reflect_identical(ms[j].ftype, want->ftype);
        if (!found)
            return false;
    }
    return true;
}

bool burrow__reflect_implements(const Type *u, const Type *t) {
    if (u == NULL || t == NULL || u->kind != KIND_INTERFACE)
        return false;
    if (u == TYPE_ANY || burrow__reflect_identical(u, t))
        return true;
    void *data;
    if (rfi_vtable(u, t, NULL, &data) != NULL)
        return true;
    if (u->nmethod > 0 && u->methods != NULL)
        return rfi_has_methods(u, t);
    return false;
}

void burrow__reflect_to_iface(const Type *u, const Type *t, void *p, Iface *out,
                              const char *context) {
    if (u->size != sizeof(Iface)) {
        Str a = burrow__reflect_concat(str_from_cstr(context), BURROW_S(": "),
                                       reflect_type_string(u));
        burrow__reflect_panic(a, BURROW_S(" holds its value inline, which reflect "),
                              BURROW_S("cannot put one into"));
    }
    if (t->kind == KIND_INTERFACE) {
        if (!burrow__reflect_unwrap(&t, &p)) {
            *out = (Iface){NULL, NULL};
            return;
        }
        /* The vtable did not say what it holds, so there is nothing to look
         * up, but a value already of type u can go in as it is. */
        if (t->kind == KIND_INTERFACE && burrow__reflect_identical(t, u)) {
            memcpy(out, p, sizeof *out);
            return;
        }
    }
    void *data;
    const void *vt = rfi_vtable(u, t, p, &data);
    if (vt == NULL) {
        Str a =
            burrow__reflect_concat(str_from_cstr(context), BURROW_S(": no vtable for "),
                                   reflect_type_string(t));
        a = burrow__reflect_concat(a, BURROW_S(" as "), reflect_type_string(u));
        burrow__reflect_panic(a, BURROW_S(", which BURROW_IMPLEMENTS records"),
                              BURROW_STR_EMPTY);
    }
    *out = (Iface){(const IfaceVT *)vt, data};
}

/* -------------------------------------------------------------- TypeAssert */

bool reflect_type_assert(ReflectValue v, ReflectType t, void *out) {
    if (v.typ == NULL)
        burrow__reflect_panic_kind(BURROW_S("reflect.TypeAssert"), KIND_INVALID);
    if (v.flag & BURROW__RF_RO)
        panic_str(BURROW_S("reflect.TypeAssert: cannot return value obtained from "
                           "unexported field or method"));
    if (t == NULL)
        panic_str(BURROW_S("reflect.TypeAssert: nil type"));
    memset(out, 0, t->size);

    /* An interface asserts what it holds, as v.Interface().(T) would. */
    const Type *vt = v.typ;
    void *p = v.ptr;
    if (vt->kind == KIND_INTERFACE) {
        if (!burrow__reflect_unwrap(&vt, &p))
            return false;
        /* Holding something it does not name, which only asserts to its own
         * type. */
        if (vt->kind == KIND_INTERFACE) {
            if (!burrow__reflect_identical(t, vt))
                return false;
            type_copy(t, out, v.ptr);
            return true;
        }
    }

    if (t->kind == KIND_INTERFACE) {
        if (!burrow__reflect_implements(t, vt))
            return false;
        if (t == TYPE_ANY) {
            Any a = BURROW_ANY(vt, p);
            memcpy(out, &a, sizeof a);
            return true;
        }
        Iface i;
        burrow__reflect_to_iface(t, vt, p, &i, "reflect.TypeAssert");
        memcpy(out, &i, sizeof i);
        return true;
    }

    /* Two concrete types, which match only when they are the same type. */
    if (!burrow__reflect_identical(t, vt))
        return false;
    type_copy(t, out, p);
    return true;
}
