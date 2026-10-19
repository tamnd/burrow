/* go/types: printing types, and the type hashes contexts key instances by.
 *
 * Derived from Go's src/go/types/typestring.go. Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* The types being written, innermost first, which is Go's seen map: a type
 * is only ever in it while it is being written, so a stack on the C stack is
 * enough. */
struct GtSeen {
    TypesType t;
    struct GtSeen *next;
};

/* ------------------------------------------------------------- buffers */

void burrow__types_buf_str(BytesBuffer *buf, Str s) {
    if (s.len > 0 && bytes_buffer_write_string(buf, s, NULL) != s.len)
        burrow__types_oom();
}

void burrow__types_buf_byte(BytesBuffer *buf, Byte c) {
    Str s = {&c, 1};
    burrow__types_buf_str(buf, s);
}

/* The buffer's bytes, which came from a, handed over as they are. */
Str burrow__types_buf_done(BytesBuffer *buf, Alloc *a) {
    (void)a;
    Slice b = bytes_buffer_bytes(buf);
    Str s = {(const Byte *)b.p, b.len};
    return s;
}

/* ---------------------------------------------------------- qualifiers */

static Str gt_relative_to(void *env, TypesPackage *other) {
    if (env == other)
        return BURROW_STR_EMPTY; /* same package; unqualified */
    return other->path;
}

TypesQualifier types_relative_to(TypesPackage *pkg) {
    if (pkg == NULL)
        return TYPES_NO_QUALIFIER;
    return BURROW_FN(TypesQualifier, gt_relative_to, pkg);
}

Str types_type_string(Alloc *a, TypesType t, TypesQualifier qf) {
    BytesBuffer buf = BYTES_BUFFER(a);
    types_write_type(&buf, t, qf);
    return burrow__types_buf_done(&buf, a);
}

void types_write_type(BytesBuffer *buf, TypesType t, TypesQualifier qf) {
    GtTypeWriter w = burrow__types_new_type_writer(buf, qf);
    burrow__types_tw_typ(&w, t);
}

void types_write_signature(BytesBuffer *buf, TypesSignature *sig, TypesQualifier qf) {
    GtTypeWriter w = burrow__types_new_type_writer(buf, qf);
    burrow__types_tw_signature(&w, sig);
}

/* -------------------------------------------------------- type writers */

GtTypeWriter burrow__types_new_type_writer(BytesBuffer *buf, TypesQualifier qf) {
    GtTypeWriter w = {buf, NULL, qf, NULL, NULL, true, false, false};
    return w;
}

GtTypeWriter burrow__types_new_type_hasher(BytesBuffer *buf, TypesContext *ctxt) {
    burrow__types_assert(ctxt != NULL);
    GtTypeWriter w = {buf, NULL, TYPES_NO_QUALIFIER, ctxt, NULL, false, false, false};
    return w;
}

static void gt_tw_byte(GtTypeWriter *w, Byte b) {
    if (w->ctxt != NULL) {
        if (b == ' ')
            b = '#';
        burrow__types_buf_byte(w->buf, b);
        return;
    }
    burrow__types_buf_byte(w->buf, b);
    if (b == ',' || b == ';')
        burrow__types_buf_byte(w->buf, ' ');
}

void burrow__types_tw_string(GtTypeWriter *w, Str s) {
    burrow__types_buf_str(w->buf, s);
}

static void gt_tw_int(GtTypeWriter *w, int64_t v) {
    Byte b[24];
    Int i = (Int)sizeof b;
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do {
        b[--i] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0)
        b[--i] = '-';
    Str s = {b + i, (Int)sizeof b - i};
    burrow__types_tw_string(w, s);
}

static void gt_tw_error(GtTypeWriter *w, Str msg) {
    if (w->ctxt != NULL)
        panic_str(msg);
    burrow__types_buf_byte(w->buf, '<');
    burrow__types_buf_str(w->buf, msg);
    burrow__types_buf_byte(w->buf, '>');
}

/* "cycle to " and Go's goTypeName, the dynamic type of typ as %T prints it
 * without the package. */
static Str gt_cycle_msg(TypesType typ) {
    switch ((TypesTypeKind)typ->kind) {
    case TYPES_KIND_BASIC:
        return BURROW_S("cycle to *Basic");
    case TYPES_KIND_POINTER:
        return BURROW_S("cycle to *Pointer");
    case TYPES_KIND_ARRAY:
        return BURROW_S("cycle to *Array");
    case TYPES_KIND_SLICE:
        return BURROW_S("cycle to *Slice");
    case TYPES_KIND_STRUCT:
        return BURROW_S("cycle to *Struct");
    case TYPES_KIND_TUPLE:
        return BURROW_S("cycle to *Tuple");
    case TYPES_KIND_SIGNATURE:
        return BURROW_S("cycle to *Signature");
    case TYPES_KIND_INTERFACE:
        return BURROW_S("cycle to *Interface");
    case TYPES_KIND_MAP:
        return BURROW_S("cycle to *Map");
    case TYPES_KIND_CHAN:
        return BURROW_S("cycle to *Chan");
    case TYPES_KIND_NAMED:
        return BURROW_S("cycle to *Named");
    case TYPES_KIND_TYPE_PARAM:
        return BURROW_S("cycle to *TypeParam");
    case TYPES_KIND_UNION:
        return BURROW_S("cycle to *Union");
    case TYPES_KIND_ALIAS:
        return BURROW_S("cycle to *Alias");
    case TYPES_KIND_INVALID:
    default:
        break;
    }
    return BURROW_S("cycle to <nil>");
}

static void gt_tw_type_name(GtTypeWriter *w, TypesTypeName *obj) {
    burrow__types_write_package_prefix(w->buf, GT_OBJ(obj)->pkg, w->qf);
    burrow__types_tw_string(w, GT_OBJ(obj)->name);
}

static void gt_tw_tuple(GtTypeWriter *w, TypesTuple *tup, bool variadic) {
    gt_tw_byte(w, '(');
    if (tup != NULL) {
        Int n = tup->vars.len;
        for (Int i = 0; i < n; i++) {
            TypesVar *v = GT_AT(tup->vars, i, TypesVar *);
            if (i > 0)
                gt_tw_byte(w, ',');
            /* parameter names are ignored for type identity and thus type
             * hashes */
            if (w->ctxt == NULL && GT_OBJ(v)->name.len != 0 && w->param_names) {
                burrow__types_tw_string(w, GT_OBJ(v)->name);
                gt_tw_byte(w, ' ');
            }
            TypesType typ = GT_OBJ(v)->typ;
            if (variadic && i == n - 1) {
                TypesSlice *slice = types_as_slice(typ);
                if (slice != NULL) {
                    burrow__types_tw_string(w, BURROW_S("..."));
                    burrow__types_tw_typ(w, slice->elem);
                } else {
                    /* append(slice, str...) and its generic relatives, which
                     * are written func([]byte, T...), dots after the type. */
                    burrow__types_tw_typ(w, typ);
                    burrow__types_tw_string(w, BURROW_S("..."));
                }
            } else {
                burrow__types_tw_typ(w, typ);
            }
        }
    }
    gt_tw_byte(w, ')');
}

void burrow__types_tw_signature(GtTypeWriter *w, TypesSignature *sig) {
    Slice tps = burrow__types_tparam_list(sig->tparams);
    bool reset = false;
    if (tps.len != 0) {
        if (w->ctxt != NULL) {
            burrow__types_assert(w->tparams == NULL);
            w->tparams = sig->tparams;
            reset = true;
        }
        burrow__types_tw_tparam_list(w, tps);
    }

    gt_tw_tuple(w, sig->params, sig->variadic);

    Int n = types_tuple_len(sig->results);
    if (n != 0) {
        gt_tw_byte(w, ' ');
        TypesVar *r0 = GT_AT(sig->results->vars, 0, TypesVar *);
        if (n == 1 && (w->ctxt != NULL || GT_OBJ(r0)->name.len == 0))
            /* single unnamed result (if type hashing, name must be ignored) */
            burrow__types_tw_typ(w, GT_OBJ(r0)->typ);
        else
            /* multiple or named result(s) */
            gt_tw_tuple(w, sig->results, false);
    }
    if (reset)
        w->tparams = NULL;
}

void burrow__types_tw_type_list(GtTypeWriter *w, Slice list) {
    gt_tw_byte(w, '[');
    for (Int i = 0; i < list.len; i++) {
        if (i > 0)
            gt_tw_byte(w, ',');
        burrow__types_tw_typ(w, GT_AT(list, i, TypesType));
    }
    gt_tw_byte(w, ']');
}

void burrow__types_tw_tparam_list(GtTypeWriter *w, Slice list) {
    gt_tw_byte(w, '[');
    TypesType prev = NULL;
    for (Int i = 0; i < list.len; i++) {
        TypesTypeParam *tpar = GT_AT(list, i, TypesTypeParam *);
        /* list is expected to hold type parameter names, but don't crash if
         * that's not the case. */
        if (tpar == NULL) {
            gt_tw_error(w, BURROW_S("nil type parameter"));
            continue;
        }
        if (i > 0) {
            if (tpar->bound != prev) {
                /* bound changed - write previous one before advancing */
                gt_tw_byte(w, ' ');
                burrow__types_tw_typ(w, prev);
            }
            gt_tw_byte(w, ',');
        }
        prev = tpar->bound;
        burrow__types_tw_typ(w, types_type(tpar));
    }
    if (prev != NULL) {
        gt_tw_byte(w, ' ');
        burrow__types_tw_typ(w, prev);
    }
    gt_tw_byte(w, ']');
}

/* Go's subscript: x in subscript digits, which are all three bytes of UTF-8
 * from U+2080 up. */
static void gt_tw_subscript(GtTypeWriter *w, uint64_t x) {
    Byte b[32 * 3];
    Int i = (Int)sizeof b;
    for (;;) {
        i -= 3;
        b[i] = 0xE2;
        b[i + 1] = 0x82;
        b[i + 2] = (Byte)(0x80 + x % 10);
        x /= 10;
        if (x == 0)
            break;
    }
    Str s = {b + i, (Int)sizeof b - i};
    burrow__types_tw_string(w, s);
}

/* typeSet writes a canonical hash for an interface type set. */
static void gt_tw_type_set(GtTypeWriter *w, GtTypeSet *s) {
    burrow__types_assert(w->ctxt != NULL);
    bool first = true;
    for (Int i = 0; i < s->methods.len; i++) {
        TypesFunc *m = GT_AT(s->methods, i, TypesFunc *);
        if (!first)
            gt_tw_byte(w, ';');
        first = false;
        burrow__types_tw_string(w, GT_OBJ(m)->name);
        burrow__types_tw_signature(w, types_as_signature(GT_OBJ(m)->typ));
    }
    if (burrow__types_termlist_is_all(s->terms))
        return; /* nothing to do */
    if (burrow__types_termlist_is_empty(s->terms)) {
        burrow__types_write_termlist(w->buf, s->terms);
        return;
    }
    /* terms are not canonically sorted, so we sort their hashes instead. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n = s->terms.len;
    Str *hashes = burrow__types_alloc(a, (size_t)n * sizeof(Str), _Alignof(Str));
    for (Int i = 0; i < n; i++) {
        TypesTerm *term = GT_AT(s->terms, i, TypesTerm *);
        BytesBuffer b = BYTES_BUFFER(a);
        if (term->tilde)
            burrow__types_buf_byte(&b, '~');
        GtTypeWriter h = burrow__types_new_type_hasher(&b, w->ctxt);
        burrow__types_tw_typ(&h, term->typ);
        hashes[i] = burrow__types_buf_done(&b, a);
    }
    sort_strings(slice_from(hashes, n, n, TYPE_STRING));
    if (!first)
        gt_tw_byte(w, ';');
    for (Int i = 0; i < n; i++) {
        if (i > 0)
            burrow__types_tw_string(w, BURROW_S("|"));
        burrow__types_tw_string(w, hashes[i]);
    }
    arena_free(&ar);
}

static bool gt_tw_seen(GtTypeWriter *w, TypesType typ) {
    for (struct GtSeen *s = w->seen; s != NULL; s = s->next)
        if (s->t == typ)
            return true;
    return false;
}

static void gt_tw_struct(GtTypeWriter *w, TypesStruct *t) {
    burrow__types_tw_string(w, BURROW_S("struct{"));
    for (Int i = 0; i < t->fields.len; i++) {
        TypesVar *f = GT_AT(t->fields, i, TypesVar *);
        if (i > 0)
            gt_tw_byte(w, ';');

        /* If disambiguating one struct for another, look for the first
         * unexported field. Do this first in case of nested structs; tag the
         * first-outermost field. */
        bool pkg_annotate = false;
        if (BURROW_FUNC_IS_NIL(w->qf) && w->pkg_info &&
            !burrow__types_is_exported(GT_OBJ(f)->name)) {
            /* note for embedded types, type name is field name, and "string"
             * etc are lower case hence unexported. */
            pkg_annotate = true;
            w->pkg_info = false; /* only tag once */
        }

        /* This doesn't do the right thing for embedded type aliases where we
         * should print the alias name, not the aliased type (see
         * go.dev/issue/44410). */
        if (!f->embedded) {
            burrow__types_tw_string(w, GT_OBJ(f)->name);
            gt_tw_byte(w, ' ');
        }
        burrow__types_tw_typ(w, GT_OBJ(f)->typ);
        if (pkg_annotate) {
            burrow__types_tw_string(w, BURROW_S(" /* package "));
            burrow__types_tw_string(w, types_package_path(GT_OBJ(f)->pkg));
            burrow__types_tw_string(w, BURROW_S(" */ "));
        }
        Str tag = types_struct_tag(t, i);
        if (tag.len != 0) {
            gt_tw_byte(w, ' ');
            Arena ar;
            arena_init(&ar, NULL, 0);
            burrow__types_tw_string(w, strconv_quote(arena_allocator(&ar), tag));
            arena_free(&ar);
        }
    }
    gt_tw_byte(w, '}');
}

static void gt_tw_interface(GtTypeWriter *w, TypesInterface *t) {
    if (w->ctxt == NULL) {
        TypesNamed *cmp = burrow__types_as_named(
            types_object_type(burrow__types_universe_comparable()));
        burrow__types_assert(cmp != NULL);
        if (types_type(t) == cmp->underlying) {
            burrow__types_tw_string(w, BURROW_S("interface{comparable}"));
            return;
        }
    }
    if (t->implicit) {
        if (t->methods.len == 0 && t->embeddeds.len == 1) {
            burrow__types_tw_typ(w, GT_AT(t->embeddeds, 0, TypesType));
            return;
        }
        /* Something's wrong with the implicit interface. Print it as such and
         * continue. */
        burrow__types_tw_string(w, BURROW_S("/* implicit */ "));
    }
    burrow__types_tw_string(w, BURROW_S("interface{"));
    bool first = true;
    if (w->ctxt != NULL) {
        gt_tw_type_set(w, burrow__types_iface_type_set(t));
    } else {
        for (Int i = 0; i < t->methods.len; i++) {
            TypesFunc *m = GT_AT(t->methods, i, TypesFunc *);
            if (!first)
                gt_tw_byte(w, ';');
            first = false;
            burrow__types_tw_string(w, GT_OBJ(m)->name);
            burrow__types_tw_signature(w, types_as_signature(GT_OBJ(m)->typ));
        }
        for (Int i = 0; i < t->embeddeds.len; i++) {
            if (!first)
                gt_tw_byte(w, ';');
            first = false;
            burrow__types_tw_typ(w, GT_AT(t->embeddeds, i, TypesType));
        }
    }
    gt_tw_byte(w, '}');
}

static void gt_tw_chan(GtTypeWriter *w, TypesChan *t) {
    Str s = BURROW_STR_EMPTY;
    bool parens = false;
    switch (t->dir) {
    case TYPES_SEND_RECV: {
        s = BURROW_S("chan ");
        /* chan (<-chan T) requires parentheses */
        TypesChan *c = types_as_chan(t->elem);
        if (c != NULL && c->dir == TYPES_RECV_ONLY)
            parens = true;
        break;
    }
    case TYPES_SEND_ONLY:
        s = BURROW_S("chan<- ");
        break;
    case TYPES_RECV_ONLY:
        s = BURROW_S("<-chan ");
        break;
    default:
        gt_tw_error(w, BURROW_S("unknown channel direction"));
    }
    burrow__types_tw_string(w, s);
    if (parens)
        gt_tw_byte(w, '(');
    burrow__types_tw_typ(w, t->elem);
    if (parens)
        gt_tw_byte(w, ')');
}

static void gt_tw_type_param(GtTypeWriter *w, TypesTypeParam *t) {
    if (t->obj == NULL) {
        gt_tw_error(w, BURROW_S("unnamed type parameter"));
        return;
    }
    Slice list = burrow__types_tparam_list(w->tparams);
    for (Int i = 0; i < list.len; i++) {
        if (GT_AT(list, i, TypesTypeParam *) == t) {
            /* The names of type parameters that are declared by the type
             * being hashed are not part of the type identity. Replace them
             * with a placeholder indicating their index. */
            gt_tw_byte(w, '$');
            gt_tw_int(w, i);
            return;
        }
    }
    burrow__types_tw_string(w, GT_OBJ(t->obj)->name);
    if (w->tp_subscripts || w->ctxt != NULL)
        gt_tw_subscript(w, t->id);
    /* If the type parameter name is the same as a predeclared object (say
     * int), point out where it is declared to avoid confusing error messages.
     * go/types has no file set here to print the position with. */
    if (w->ctxt == NULL &&
        types_scope_lookup(types_universe(), GT_OBJ(t->obj)->name) != NULL)
        burrow__types_tw_string(w, BURROW_S("/* type parameter */"));
}

static void gt_tw_named(GtTypeWriter *w, TypesNamed *t) {
    /* If hashing, write a unique prefix for t to represent its identity,
     * since named type identity is pointer identity. */
    if (w->ctxt != NULL)
        gt_tw_int(w, burrow__types_context_get_id(w->ctxt, types_type(t)));
    gt_tw_type_name(w, t->obj); /* when hashing written for readability only */
    if (t->inst != NULL) {
        /* instantiated type */
        burrow__types_tw_type_list(w, burrow__types_type_list(t->inst->targs));
    } else if (w->ctxt == NULL) {
        /* For type hashing, don't need to format the TypeParams */
        Slice tps = burrow__types_tparam_list(types_named_type_params(t));
        if (tps.len != 0) /* parameterized type */
            burrow__types_tw_tparam_list(w, tps);
    }
}

static void gt_tw_alias(GtTypeWriter *w, TypesAlias *t) {
    gt_tw_type_name(w, t->obj);
    Slice list = burrow__types_type_list(t->targs);
    if (list.len != 0) {
        /* instantiated type */
        burrow__types_tw_type_list(w, list);
    } else if (w->ctxt == NULL) {
        Slice tps = burrow__types_tparam_list(types_alias_type_params(t));
        if (tps.len != 0) /* parameterized type */
            burrow__types_tw_tparam_list(w, tps);
    }
    if (w->ctxt != NULL) {
        TypesType typ = types_unalias(GT_OBJ(t->obj)->typ);
        if (typ == NULL)
            panic_str(BURROW_S("known implementation limitation: encountered an "
                               "incomplete alias (see go.dev/issue/78296)"));
        burrow__types_tw_typ(w, typ);
    }
}

static void gt_tw_basic(GtTypeWriter *w, TypesBasic *t) {
    /* exported basic types go into package unsafe (currently this is just
     * unsafe.Pointer) */
    if (burrow__types_is_exported(t->name)) {
        TypesObject o = types_scope_lookup(types_unsafe()->scope, t->name);
        TypesTypeName *obj = o != NULL ? types_as_type_name(o) : NULL;
        if (obj != NULL) {
            gt_tw_type_name(w, obj);
            return;
        }
    }
    burrow__types_tw_string(w, t->name);
}

static void gt_tw_union(GtTypeWriter *w, TypesUnion *t) {
    /* Unions only appear as (syntactic) embedded elements in interfaces and
     * syntactically cannot be empty. */
    if (t->terms.len == 0) {
        gt_tw_error(w, BURROW_S("empty union"));
        return;
    }
    for (Int i = 0; i < t->terms.len; i++) {
        TypesTerm *term = GT_AT(t->terms, i, TypesTerm *);
        if (i > 0)
            burrow__types_tw_string(w, BURROW_S(" | "));
        if (term->tilde)
            gt_tw_byte(w, '~');
        burrow__types_tw_typ(w, term->typ);
    }
}

void burrow__types_tw_typ(GtTypeWriter *w, TypesType typ) {
    if (typ == NULL) {
        gt_tw_error(w, BURROW_S("nil"));
        return;
    }
    if (gt_tw_seen(w, typ)) {
        gt_tw_error(w, gt_cycle_msg(typ));
        return;
    }
    struct GtSeen seen = {typ, w->seen};
    w->seen = &seen;

    switch ((TypesTypeKind)typ->kind) {
    case TYPES_KIND_BASIC:
        gt_tw_basic(w, GT_KNOWN(TypesBasic, typ));
        break;
    case TYPES_KIND_ARRAY: {
        TypesArray *t = GT_KNOWN(TypesArray, typ);
        gt_tw_byte(w, '[');
        gt_tw_int(w, t->len);
        gt_tw_byte(w, ']');
        burrow__types_tw_typ(w, t->elem);
        break;
    }
    case TYPES_KIND_SLICE:
        burrow__types_tw_string(w, BURROW_S("[]"));
        burrow__types_tw_typ(w, GT_KNOWN(TypesSlice, typ)->elem);
        break;
    case TYPES_KIND_STRUCT:
        gt_tw_struct(w, GT_KNOWN(TypesStruct, typ));
        break;
    case TYPES_KIND_POINTER:
        gt_tw_byte(w, '*');
        burrow__types_tw_typ(w, GT_KNOWN(TypesPointer, typ)->base);
        break;
    case TYPES_KIND_TUPLE:
        gt_tw_tuple(w, GT_KNOWN(TypesTuple, typ), false);
        break;
    case TYPES_KIND_SIGNATURE:
        burrow__types_tw_string(w, BURROW_S("func"));
        burrow__types_tw_signature(w, GT_KNOWN(TypesSignature, typ));
        break;
    case TYPES_KIND_UNION:
        gt_tw_union(w, GT_KNOWN(TypesUnion, typ));
        break;
    case TYPES_KIND_INTERFACE:
        gt_tw_interface(w, GT_KNOWN(TypesInterface, typ));
        break;
    case TYPES_KIND_MAP: {
        TypesMap *t = GT_KNOWN(TypesMap, typ);
        burrow__types_tw_string(w, BURROW_S("map["));
        burrow__types_tw_typ(w, t->key);
        gt_tw_byte(w, ']');
        burrow__types_tw_typ(w, t->elem);
        break;
    }
    case TYPES_KIND_CHAN:
        gt_tw_chan(w, GT_KNOWN(TypesChan, typ));
        break;
    case TYPES_KIND_NAMED:
        gt_tw_named(w, GT_KNOWN(TypesNamed, typ));
        break;
    case TYPES_KIND_TYPE_PARAM:
        gt_tw_type_param(w, GT_KNOWN(TypesTypeParam, typ));
        break;
    case TYPES_KIND_ALIAS:
        gt_tw_alias(w, GT_KNOWN(TypesAlias, typ));
        break;
    case TYPES_KIND_INVALID:
    default:
        gt_tw_error(w, BURROW_S("nil"));
        break;
    }

    w->seen = seen.next;
}
