/* go/types: a hash function for types.
 *
 * Derived from Go's src/go/types/hash.go. Go source: go1.27.1.
 *
 * Copyright 2026 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/hash/maphash.h"
#include "burrow/panic.h"
#include "burrow/type.h"

#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

static void hr_byte(MaphashHash *h, Byte c) {
    Error err = maphash_hash_write_byte(h, c);
    (void)err;
}

static void hr_bool(MaphashHash *h, bool b) {
    maphash_write_comparable(h, TYPE_BOOL, &b);
}

static void hr_int(MaphashHash *h, Int n) {
    maphash_write_comparable(h, TYPE_INT, &n);
}

/* hasher holds the state of a single hash traversal, namely, whether we are
 * inside the signature of a generic function. This is used to optimize
 * hr_type_param. */
typedef struct GtHasher {
    bool in_generic_sig;
} GtHasher;

static void hr_hash(GtHasher hr, MaphashHash *h, TypesType t);
static void hr_shallow_hash(GtHasher hr, MaphashHash *h, TypesType t);
static void hr_shallow_tuple(GtHasher hr, MaphashHash *h, TypesTuple *t);
static void hr_tuple(GtHasher hr, MaphashHash *h, TypesTuple *t);
static void hr_type_param(GtHasher hr, MaphashHash *h, TypesTypeParam *t);
static void hr_type_name(MaphashHash *h, TypesTypeName *tname);

void types_hasher_hash(TypesHasher hr, MaphashHash *h, TypesType t) {
    (void)hr;
    /* The two hashers use essentially the same hash function, which ignores
     * tags; only the Equal methods vary. But for future-proofing we
     * gratuitously force them to differ by one byte. */
    hr_byte(h, 0);
    hr_hash((GtHasher){false}, h, t);
}

void types_hasher_ignore_tags_hash(TypesHasherIgnoreTags hr, MaphashHash *h,
                                   TypesType t) {
    (void)hr;
    hr_byte(h, 1);
    hr_hash((GtHasher){false}, h, t);
}

bool types_hasher_equal(TypesHasher hr, TypesType x, TypesType y) {
    (void)hr;
    return types_identical(x, y);
}

bool types_hasher_ignore_tags_equal(TypesHasherIgnoreTags hr, TypesType x,
                                    TypesType y) {
    (void)hr;
    return types_identical_ignore_tags(x, y);
}

static void hr_vt_hash(void *self, MaphashHash *h, const void *v) {
    (void)self;
    types_hasher_hash((TypesHasher){0}, h, *(const TypesType *)v);
}

static bool hr_vt_equal(void *self, const void *x, const void *y) {
    (void)self;
    return types_identical(*(const TypesType *)x, *(const TypesType *)y);
}

static void hr_it_vt_hash(void *self, MaphashHash *h, const void *v) {
    (void)self;
    types_hasher_ignore_tags_hash((TypesHasherIgnoreTags){0}, h, *(const TypesType *)v);
}

static bool hr_it_vt_equal(void *self, const void *x, const void *y) {
    (void)self;
    return types_identical_ignore_tags(*(const TypesType *)x, *(const TypesType *)y);
}

static const MaphashHasherVT hr_vt = {hr_vt_hash, hr_vt_equal};
static const MaphashHasherVT hr_it_vt = {hr_it_vt_hash, hr_it_vt_equal};

MaphashHasher types_hasher_as_hasher(TypesHasher hr) {
    (void)hr;
    return (MaphashHasher){&hr_vt, NULL};
}

MaphashHasher types_hasher_ignore_tags_as_hasher(TypesHasherIgnoreTags hr) {
    (void)hr;
    return (MaphashHasher){&hr_it_vt, NULL};
}

static void hr_hash(GtHasher hr, MaphashHash *h, TypesType t) {
    /* See types_identical for rationale. */
    switch (t == NULL ? (Int)TYPES_KIND_INVALID : t->kind) {
    case TYPES_KIND_ALIAS:
        hr_hash(hr, h, types_unalias(t));
        break;

    case TYPES_KIND_ARRAY: {
        TypesArray *a = GT_KNOWN(TypesArray, t);
        hr_byte(h, 'A');
        int64_t n = types_array_len(a);
        maphash_write_comparable(h, TYPE_INT64, &n);
        hr_hash(hr, h, types_array_elem(a));
        break;
    }

    case TYPES_KIND_BASIC:
        hr_byte(h, 'B');
        hr_byte(h, (Byte)types_basic_kind(GT_KNOWN(TypesBasic, t)));
        break;

    case TYPES_KIND_CHAN: {
        TypesChan *c = GT_KNOWN(TypesChan, t);
        hr_byte(h, 'C');
        hr_byte(h, (Byte)types_chan_dir(c));
        hr_hash(hr, h, types_chan_elem(c));
        break;
    }

    case TYPES_KIND_INTERFACE: {
        TypesInterface *it = GT_KNOWN(TypesInterface, t);
        Int n = types_interface_num_methods(it);
        hr_byte(h, 'I');
        hr_byte(h, (Byte)n);

        /* Interfaces are identical if they have the same set of methods, with
         * identical names and types, and they have the same set of type
         * restrictions. See types_identical for more details.
         *
         * Hash the methods.
         *
         * Because types_identical treats Methods as an unordered set, we must
         * either:
         * (a) sort the methods into some canonical order; or
         * (b) hash them each in parallel, combine them with a commutative
         *     operation such as + or ^, and then write this value into the
         *     primary hasher.
         * Since (a) requires allocation, we choose (b). */
        uint64_t hash = 0;
        for (Int i = 0; i < n; i++) {
            TypesFunc *m = types_interface_method(it, i);
            MaphashHash subh = {0};
            maphash_hash_set_seed(&subh, maphash_hash_seed(h));
            /* Ignore m's package. Use shallow hash on method signature to
             * avoid anonymous interface cycles. */
            maphash_hash_write_string(&subh, types_object_name(types_object(m)), NULL);
            hr_shallow_hash(hr, &subh, types_object_type(types_object(m)));
            hash ^= maphash_hash_sum64(&subh);
        }
        maphash_write_comparable(h, TYPE_UINT64, &hash);

        /* Hash type restrictions.
         * TODO(adonovan): call (fork of) InterfaceTermSet from
         * golang.org/x/tools/internal/typeparams/normalize.go. */
        break;
    }

    case TYPES_KIND_MAP: {
        TypesMap *m = GT_KNOWN(TypesMap, t);
        hr_byte(h, 'M');
        hr_hash(hr, h, types_map_key(m));
        hr_hash(hr, h, types_map_elem(m));
        break;
    }

    case TYPES_KIND_NAMED: {
        TypesNamed *n = GT_KNOWN(TypesNamed, t);
        hr_byte(h, 'N');
        hr_type_name(h, types_named_obj(n));
        TypesTypeList *targs = types_named_type_args(n);
        for (Int i = 0; i < types_type_list_len(targs); i++)
            hr_hash(hr, h, types_type_list_at(targs, i));
        break;
    }

    case TYPES_KIND_POINTER:
        hr_byte(h, 'P');
        hr_hash(hr, h, types_pointer_elem(GT_KNOWN(TypesPointer, t)));
        break;

    case TYPES_KIND_SIGNATURE: {
        TypesSignature *sig = GT_KNOWN(TypesSignature, t);
        hr_byte(h, 'F');
        hr_bool(h, types_signature_variadic(sig));
        TypesTypeParamList *tparams = types_signature_type_params(sig);
        Int n = types_type_param_list_len(tparams);
        if (n > 0) {
            hr.in_generic_sig = true; /* affects constraints, params, and results */

            hr_int(h, n);
            for (Int i = 0; i < n; i++)
                hr_hash(
                    hr, h,
                    types_type_param_constraint(types_type_param_list_at(tparams, i)));
        }
        hr_tuple(hr, h, types_signature_params(sig));
        hr_tuple(hr, h, types_signature_results(sig));
        break;
    }

    case TYPES_KIND_SLICE:
        hr_byte(h, 'S');
        hr_hash(hr, h, types_slice_elem(GT_KNOWN(TypesSlice, t)));
        break;

    case TYPES_KIND_STRUCT: {
        TypesStruct *s = GT_KNOWN(TypesStruct, t);
        hr_byte(h, 'R'); /* mnemonic: a struct is a record type */
        Int n = types_struct_num_fields(s);
        hr_byte(h, (Byte)n);
        for (Int i = 0; i < n; i++) {
            TypesVar *f = types_struct_field(s, i);
            hr_bool(h, types_var_anonymous(f));
            /* Ignore the tag, so that a single hash function can be used with
             * both types_identical and types_identical_ignore_tags. */
            maphash_hash_write_string(h, types_object_name(types_object(f)),
                                      NULL); /* (ignore f's package) */
            hr_hash(hr, h, types_object_type(types_object(f)));
        }
        break;
    }

    case TYPES_KIND_TUPLE:
        hr_tuple(hr, h, GT_KNOWN(TypesTuple, t));
        break;

    case TYPES_KIND_TYPE_PARAM:
        hr_type_param(hr, h, GT_KNOWN(TypesTypeParam, t));
        break;

    case TYPES_KIND_UNION:
        hr_byte(h, 'U');
        /* TODO(adonovan): opt: call (fork of) UnionTermSet from
         * golang.org/x/tools/internal/typeparams/normalize.go. */
        break;

    default:
        burrow__types_assert(t == NULL);
        panic_str(BURROW_S("<nil>: <nil>"));
    }
}

static void hr_tuple(GtHasher hr, MaphashHash *h, TypesTuple *t) {
    Int n = types_tuple_len(t);
    hr_byte(h, 'T');
    hr_byte(h, (Byte)n);
    for (Int i = 0; i < n; i++)
        hr_hash(hr, h, types_object_type(types_object(types_tuple_at(t, i))));
}

/* hr_type_param encodes a type parameter into hasher h. */
static void hr_type_param(GtHasher hr, MaphashHash *h, TypesTypeParam *t) {
    hr_byte(h, 'P');
    /* Within the signature of a generic function, TypeParams are identical if
     * they have the same index and constraint, so we hash them based on index.
     *
     * When we are outside a generic function, free TypeParams are identical
     * iff they are the same object, so we can use a more discriminating hash
     * consistent with object identity. This optimization saves typeutil.Map about 4%
     * when hashing all the Info.Types in the forward closure of net/http. */
    if (!hr.in_generic_sig) {
        /* Optimization: outside a generic function signature, use a more
         * discrimating hash consistent with object identity. */
        hr_type_name(h, types_type_param_obj(t));
    } else {
        hr_byte(h, (Byte)types_type_param_index(t));
    }
}

/* hr_type_name hashes the pointer of tname. */
static void hr_type_name(MaphashHash *h, TypesTypeName *tname) {
    hr_byte(h, 'N');
    /* Since types_identical uses == to compare TypeNames, the hash function
     * hashes the pointer. */
    UnsafePointer p = tname;
    maphash_write_comparable(h, TYPE_UNSAFE_POINTER, &p);
}

/* A NULL tuple is the empty one, as a nil *Tuple is in Go. */
static void hr_shallow_tuple(GtHasher hr, MaphashHash *h, TypesTuple *t) {
    Int n = types_tuple_len(t);
    hr_byte(h, 'T');
    hr_byte(h, (Byte)n);
    for (Int i = 0; i < n; i++)
        hr_shallow_hash(hr, h, types_object_type(types_object(types_tuple_at(t, i))));
}

/* hr_shallow_hash computes a hash of t without looking at any of its element
 * Types, to avoid potential anonymous cycles in the types of interface
 * methods.
 *
 * When an unnamed non-empty interface type appears anywhere among the
 * arguments or results of an interface method, there is a potential for
 * endless recursion. Consider:
 *
 *     type X interface { m() []*interface { X } }
 *
 * The problem is that the Methods of the interface in m's result type include
 * m itself; there is no mention of the named type X that might help us break
 * the cycle. (See comment in types_identical, case *Interface, for more.) */
static void hr_shallow_hash(GtHasher hr, MaphashHash *h, TypesType t) {
    /* t is the type of an interface method (Signature), its params or
     * results (Tuples), or their immediate elements (mostly Slice, Pointer,
     * Basic, Named), so there's no need to optimize anything else. */
    switch (t == NULL ? (Int)TYPES_KIND_INVALID : t->kind) {
    case TYPES_KIND_ALIAS:
        hr_shallow_hash(hr, h, types_unalias(t));
        break;

    case TYPES_KIND_ARRAY: {
        hr_byte(h, 'A');
        int64_t n = types_array_len(GT_KNOWN(TypesArray, t));
        maphash_write_comparable(h, TYPE_INT64, &n);
        /* ignore the element type */
        break;
    }

    case TYPES_KIND_BASIC:
        hr_byte(h, 'B');
        hr_byte(h, (Byte)types_basic_kind(GT_KNOWN(TypesBasic, t)));
        break;

    case TYPES_KIND_CHAN:
        hr_byte(h, 'C');
        /* ignore the direction and element type */
        break;

    case TYPES_KIND_INTERFACE:
        hr_byte(h, 'I');
        /* no recursion here */
        break;

    case TYPES_KIND_MAP:
        hr_byte(h, 'M');
        /* ignore the key and element types */
        break;

    case TYPES_KIND_NAMED:
        hr_type_name(h, types_named_obj(GT_KNOWN(TypesNamed, t)));
        break;

    case TYPES_KIND_POINTER:
        hr_byte(h, 'P');
        /* ignore the element type */
        break;

    case TYPES_KIND_SIGNATURE: {
        TypesSignature *sig = GT_KNOWN(TypesSignature, t);
        hr_byte(h, types_signature_variadic(sig) ? 1 : 0);
        /* The Signature/Tuple recursion is always finite and invariably
         * shallow. */
        hr_shallow_tuple(hr, h, types_signature_params(sig));
        hr_shallow_tuple(hr, h, types_signature_results(sig));
        break;
    }

    case TYPES_KIND_SLICE:
        hr_byte(h, 'S');
        /* ignore the element type */
        break;

    case TYPES_KIND_STRUCT:
        hr_byte(h, 'R'); /* mnemonic: a struct is a record type */
        hr_byte(h, (Byte)types_struct_num_fields(GT_KNOWN(TypesStruct, t)));
        /* ignore the fields */
        break;

    case TYPES_KIND_TUPLE:
        hr_shallow_tuple(hr, h, GT_KNOWN(TypesTuple, t));
        break;

    case TYPES_KIND_TYPE_PARAM:
        hr_type_param(hr, h, GT_KNOWN(TypesTypeParam, t));
        break;

    case TYPES_KIND_UNION:
        hr_byte(h, 'U');
        /* ignore the term set */
        break;

    default:
        burrow__types_assert(t == NULL);
        panic_str(BURROW_S("shallowHash: <nil>: <nil>"));
    }
}
