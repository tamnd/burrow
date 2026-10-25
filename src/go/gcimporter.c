/* go/internal/gcimporter: reading the unified export data of the gc compiler
 * into go/types packages.
 *
 * Derived from Go's src/go/internal/gcimporter/gcimporter.go, support.go and
 * ureader.go. Go source: go1.27.3.
 *
 * As in Go, export data that is not well formed panics. Not in Go: everything
 * the importer makes comes from the allocator it is given.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "gcimporter.h"
#include "pkgbits.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define GCI_S(lit) BURROW_S(lit)

/* ---------------------------------------------------------------- support */

BURROW_NORETURN static void gci_assert_failed(void) {
    panic_str(GCI_S("assertion failed"));
}

#define gci_assert(b) ((b) ? (void)0 : gci_assert_failed())

static void *gci_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size == 0 ? 1 : size, align);
    if (p == NULL)
        panic_str(GCI_S("gcimporter: out of memory"));
    return p;
}

static Slice gci_append(Alloc *a, Slice s, const void *p) {
    Slice t = slice_append(a, s, p, 1);
    if (t.p == NULL)
        panic_str(GCI_S("gcimporter: out of memory"));
    return t;
}

/* A slice of n NULL pointers. */
static Slice gci_ptrs(Alloc *a, Int n) {
    if (n == 0)
        return slice_nil(TYPE_UNSAFE_POINTER);
    Slice s = slice_make(a, TYPE_UNSAFE_POINTER, n, n);
    if (s.p == NULL)
        panic_str(GCI_S("gcimporter: out of memory"));
    return s;
}

/* Synthesize a token.Pos */
typedef struct GciFileInfo {
    TokenFile *file;
    Int lastline;
} GciFileInfo;

typedef struct GciFakeFileSet {
    TokenFileSet *fset;
    Map *files; /* Str -> GciFileInfo * */
    Slice list; /* of GciFileInfo *, in the order they were made */
} GciFakeFileSet;

enum { gci_maxlines = 64 * 1024 };

static TokenPos gci_fake_pos(Alloc *a, GciFakeFileSet *s, Str file, Int line,
                             Int column) {
    (void)column;
    /* TODO(mdempsky): Make use of column. */

    /* Since we don't know the set of needed file positions, we reserve maxlines
     * positions per file. We delay calling token.File.SetLines until all
     * positions have been calculated (by way of fakeFileSet.setLines), so that
     * we can avoid setting unnecessary lines. See also golang/go#46586. */
    GciFileInfo **found = map_get(s->files, &file);
    GciFileInfo *f = found != NULL ? *found : NULL;
    if (f == NULL) {
        f = gci_alloc(a, sizeof *f, _Alignof(GciFileInfo));
        f->file = token_file_set_add_file(s->fset, file, -1, gci_maxlines);
        if (f->file == NULL)
            panic_str(GCI_S("gcimporter: out of memory"));
        f->lastline = 0;
        Str key = str_clone(a, file);
        if (!map_set(s->files, &key, &f))
            panic_str(GCI_S("gcimporter: out of memory"));
        s->list = gci_append(a, s->list, &f);
    }

    if (line > gci_maxlines)
        line = 1;
    if (line > f->lastline)
        f->lastline = line;

    /* Return a fake position assuming that f.file consists only of newlines. */
    return (TokenPos)(token_file_base(f->file) + line - 1);
}

/* Not in Go: the lines are one table as long as the longest file needs,
 * rather than a package variable of maxlines. */
static void gci_fake_set_lines(Alloc *a, GciFakeFileSet *s) {
    Int longest = 0;
    for (Int i = 0; i < s->list.len; i++) {
        GciFileInfo *f = BURROW_AT(GciFileInfo *, s->list, i);
        if (f->lastline > longest)
            longest = f->lastline;
    }
    if (longest == 0)
        return;
    Slice fake_lines = slice_make(a, TYPE_INT, longest, longest);
    if (fake_lines.p == NULL)
        panic_str(GCI_S("gcimporter: out of memory"));
    for (Int i = 0; i < longest; i++)
        BURROW_AT(Int, fake_lines, i) = i;
    for (Int i = 0; i < s->list.len; i++) {
        GciFileInfo *f = BURROW_AT(GciFileInfo *, s->list, i);
        Slice lines = fake_lines;
        lines.len = f->lastline;
        (void)token_file_set_lines(f->file, lines);
    }
}

/* See cmd/compile/internal/noder.typeInfo. */
typedef struct GciTypeInfo {
    PbIndex idx;
    bool derived;
} GciTypeInfo;

/* See cmd/compile/internal/types.SplitVargenSuffix. */
static Str gci_split_vargen_suffix(Str name, Str *suffix) {
    Int i = name.len;
    while (i > 0 && name.p[i - 1] >= '0' && name.p[i - 1] <= '9')
        i--;
    const Str dot = GCI_S("\xc2\xb7"); /* · */
    if (i >= dot.len && memcmp(name.p + i - dot.len, dot.p, (size_t)dot.len) == 0) {
        i -= dot.len;
        *suffix = str_from_bytes(name.p + i, name.len - i);
        return str_from_bytes(name.p, i);
    }
    *suffix = GCI_S("");
    return name;
}

/* ---------------------------------------------------------------- ureader */

/* What typeParamNames leaves for later: the constraints to set. */
typedef struct GciLater {
    Slice tparams; /* of TypesTypeParam * */
    Slice types;   /* of TypesType */
} GciLater;

/* A pkgReader holds the shared state for reading a unified IR package
 * description. */
typedef struct GciPkgReader {
    PbPkgDecoder dec;

    GciFakeFileSet fake;

    TypesContext *ctxt;
    Map *imports; /* previously imported packages, indexed by path */

    /* lazily initialized arrays corresponding to the unified IR PosBase, Pkg,
     * and Type sections, respectively. */
    Str *pos_bases; /* position bases (i.e., file names) */
    TypesPackage **pkgs;
    TypesType *typs;

    /* laterFns holds functions that need to be invoked at the end of import
     * reading.
     *
     * TODO(mdempsky): Is it safe to have a single "later" slice or do we need
     * to have multiple passes? See comments on CL 386002 and go.dev/issue/52104.
     *
     * Not in Go: what is to be done later is always setting the constraints of
     * a list of type parameters, so it is those lists that are kept. */
    Slice later_fns; /* of GciLater * */

    /* ifaces holds a list of constructed Interfaces, which need to have
     * Complete called after importing is done. */
    Slice ifaces; /* of TypesInterface * */

    Alloc *a;
} GciPkgReader;

/* A readerDict holds the state for type parameters that parameterize the
 * current unified IR element. */
typedef struct GciReaderDict {
    GciTypeInfo
        *rtbounds; /* contains constraint types for each parameter in rtparams */
    Int nrtbounds;
    Slice rtparams; /* contains receiver type parameters for an element */

    GciTypeInfo *tbounds; /* contains constraint types for each parameter in tparams */
    Int ntbounds;
    Slice tparams; /* contains type parameters for an element */

    /* derived is a slice of types derived from tparams, which may be
     * instantiated while reading the current element. */
    PbIndex *derived;
    Int nderived;
    TypesType *derived_types; /* lazily instantiated from derived */
} GciReaderDict;

/* A reader holds the state for reading a single unified IR element within a
 * package. */
typedef struct GciReader {
    PbDecoder d;

    GciPkgReader *p;

    GciReaderDict *dict;
} GciReader;

static GciReader gci_new_reader(GciPkgReader *pr, PbSectionKind k, PbIndex idx,
                                PbSyncMarker marker) {
    return (GciReader){burrow__pb_new_decoder(&pr->dec, k, idx, marker), pr, NULL};
}

static GciReader gci_temp_reader(GciPkgReader *pr, PbSectionKind k, PbIndex idx,
                                 PbSyncMarker marker) {
    return (GciReader){burrow__pb_temp_decoder(&pr->dec, k, idx, marker), pr, NULL};
}

static void gci_retire_reader(GciPkgReader *pr, GciReader *r) {
    burrow__pb_retire_decoder(&pr->dec, &r->d);
}

static TypesPackage *gci_pkg(GciReader *r);
static TypesType gci_typ(GciReader *r);
static TypesSignature *gci_signature(GciReader *r, TypesVar *recv, Slice rtparams,
                                     Slice tparams);
static TypesVar *gci_param(GciReader *r, TypesVarKind kind);
static Slice gci_type_param_names(GciReader *r, bool is_gen_meth);
static TypesPackage *gci_obj_idx(GciPkgReader *pr, PbIndex idx, Str *name);
static GciReaderDict *gci_obj_dict_idx(GciPkgReader *pr, PbIndex idx);
static TypesType gci_typ_idx(GciPkgReader *pr, GciTypeInfo info, GciReaderDict *dict);

static Str gci_ident(GciReader *r, PbSyncMarker marker, TypesPackage **pkg) {
    burrow__pb_sync(&r->d, marker);
    *pkg = gci_pkg(r);
    return burrow__pb_string(&r->d);
}

static Str gci_qualified_ident(GciReader *r, TypesPackage **pkg) {
    return gci_ident(r, PB_SYNC_SYM, pkg);
}

static Str gci_local_ident(GciReader *r, TypesPackage **pkg) {
    return gci_ident(r, PB_SYNC_LOCAL_IDENT, pkg);
}

static Str gci_selector(GciReader *r, TypesPackage **pkg) {
    return gci_ident(r, PB_SYNC_SELECTOR, pkg);
}

/* pkgScope returns pkg.Scope(). If pkg is nil, it returns types.Universe
 * instead.
 *
 * TODO(mdempsky): Remove after x/tools can depend on Go 1.19. */
static TypesScope *gci_pkg_scope(TypesPackage *pkg) {
    if (pkg != NULL)
        return types_package_scope(pkg);
    return types_universe();
}

/* newAliasTypeName returns a new TypeName, with a materialized *types.Alias if
 * supported. */
static TypesTypeName *gci_new_alias_type_name(Alloc *a, TokenPos pos, TypesPackage *pkg,
                                              Str name, TypesType rhs, Slice tparams) {
    TypesTypeName *tname = types_new_type_name(a, pos, pkg, name, NULL);
    TypesAlias *alias =
        types_new_alias(a, tname, rhs); /* form TypeName -> Alias cycle */
    types_alias_set_type_params(alias, tparams);
    return tname;
}

/* @@@ Positions */

static Str gci_pos_base_idx(GciPkgReader *pr, PbIndex idx);

static TokenPos gci_pos(GciReader *r) {
    burrow__pb_sync(&r->d, PB_SYNC_POS);
    if (!burrow__pb_bool(&r->d))
        return TOKEN_NO_POS;

    /* TODO(mdempsky): Delta encoding. */
    Str pos_base = gci_pos_base_idx(r->p, burrow__pb_reloc(&r->d, PB_SECTION_POS_BASE));
    Uint line = burrow__pb_uint(&r->d);
    Uint col = burrow__pb_uint(&r->d);
    return gci_fake_pos(r->p->a, &r->p->fake, pos_base, (Int)line, (Int)col);
}

static Str gci_pos_base_idx(GciPkgReader *pr, PbIndex idx) {
    gci_assert(idx >= 0 &&
               (Int)idx < burrow__pb_num_elems(&pr->dec, PB_SECTION_POS_BASE));
    Str b = pr->pos_bases[idx];
    if (b.len != 0)
        return b;

    Str filename;
    {
        GciReader r = gci_temp_reader(pr, PB_SECTION_POS_BASE, idx, PB_SYNC_POS_BASE);

        /* Within types2, position bases have a lot more details (e.g.,
         * keeping track of where //line directives appeared exactly).
         *
         * For go/types, we just track the file name. */

        filename = burrow__pb_string(&r.d);

        if (burrow__pb_bool(&r.d)) { /* file base */
            /* Was: "b = token.NewTrimmedFileBase(filename, true)" */
        } else { /* line base */
            TokenPos pos = gci_pos(&r);
            Uint line = burrow__pb_uint(&r.d);
            Uint col = burrow__pb_uint(&r.d);

            /* Was: "b = token.NewLineBase(pos, filename, true, line, col)" */
            (void)pos;
            (void)line;
            (void)col;
        }
        gci_retire_reader(pr, &r);
    }
    b = filename;
    pr->pos_bases[idx] = b;
    return b;
}

/* @@@ Packages */

static TypesPackage *gci_do_pkg(GciReader *r) {
    GciPkgReader *pr = r->p;
    Str path = burrow__pb_string(&r->d);
    if (path.len == 0)
        path = burrow__pb_pkg_decoder_pkg_path(&pr->dec);
    else if (str_eq(path, GCI_S("builtin")))
        return NULL; /* universe */
    else if (str_eq(path, GCI_S("unsafe")))
        return types_unsafe();

    TypesPackage **found = map_get(pr->imports, &path);
    if (found != NULL && *found != NULL)
        return *found;

    Str name = burrow__pb_string(&r->d);

    TypesPackage *pkg = types_new_package(pr->a, path, name);
    if (pkg == NULL)
        panic_str(GCI_S("gcimporter: out of memory"));
    Str key = types_package_path(pkg);
    if (!map_set(pr->imports, &key, &pkg))
        panic_str(GCI_S("gcimporter: out of memory"));

    return pkg;
}

static TypesPackage *gci_pkg_idx(GciPkgReader *pr, PbIndex idx) {
    gci_assert(idx >= 0 && (Int)idx < burrow__pb_num_elems(&pr->dec, PB_SECTION_PKG));
    /* TODO(mdempsky): Consider using some non-nil pointer to indicate the
     * universe scope, so we don't need to keep re-reading it. */
    TypesPackage *pkg = pr->pkgs[idx];
    if (pkg != NULL)
        return pkg;

    GciReader r = gci_new_reader(pr, PB_SECTION_PKG, idx, PB_SYNC_PKG_DEF);
    pkg = gci_do_pkg(&r);
    pr->pkgs[idx] = pkg;
    return pkg;
}

static TypesPackage *gci_pkg(GciReader *r) {
    burrow__pb_sync(&r->d, PB_SYNC_PKG);
    return gci_pkg_idx(r->p, burrow__pb_reloc(&r->d, PB_SECTION_PKG));
}

/* @@@ Types */

static GciTypeInfo gci_typ_info(GciReader *r) {
    burrow__pb_sync(&r->d, PB_SYNC_TYPE);
    if (burrow__pb_bool(&r->d))
        return (GciTypeInfo){(PbIndex)burrow__pb_len(&r->d), true};
    return (GciTypeInfo){burrow__pb_reloc(&r->d, PB_SECTION_TYPE), false};
}

static TypesType gci_typ(GciReader *r) {
    return gci_typ_idx(r->p, gci_typ_info(r), r->dict);
}

static TypesType gci_do_typ(GciReader *r);

static TypesType gci_typ_idx(GciPkgReader *pr, GciTypeInfo info, GciReaderDict *dict) {
    PbIndex idx = info.idx;
    TypesType *where;
    if (info.derived) {
        gci_assert(dict != NULL && idx >= 0 && (Int)idx < dict->nderived);
        where = &dict->derived_types[idx];
        idx = dict->derived[idx];
    } else {
        gci_assert(idx >= 0 &&
                   (Int)idx < burrow__pb_num_elems(&pr->dec, PB_SECTION_TYPE));
        where = &pr->typs[idx];
    }

    TypesType typ = *where;
    if (typ != NULL)
        return typ;

    {
        GciReader r = gci_temp_reader(pr, PB_SECTION_TYPE, idx, PB_SYNC_TYPE_IDX);
        r.dict = dict;

        typ = gci_do_typ(&r);
        gci_assert(typ != NULL);
        gci_retire_reader(pr, &r);
    }
    /* See comment in pkgReader.typIdx explaining how this happens. */
    TypesType prev = *where;
    if (prev != NULL)
        return prev;

    *where = typ;
    return typ;
}

static void gci_obj(GciReader *r, Slice *targs, TypesObject *obj);

static TypesStruct *gci_struct_type(GciReader *r) {
    Alloc *a = r->p->a;
    Int n = burrow__pb_len(&r->d);
    gci_assert(n <= r->d.data.len); /* Not in Go: a bound before allocating */
    Slice fields = gci_ptrs(a, n);
    Slice tags = slice_nil(TYPE_STRING);
    for (Int i = 0; i < n; i++) {
        TokenPos pos = gci_pos(r);
        TypesPackage *pkg;
        Str name = gci_selector(r, &pkg);
        TypesType ftyp = gci_typ(r);
        Str tag = burrow__pb_string(&r->d);
        bool embedded = burrow__pb_bool(&r->d);

        BURROW_AT(TypesVar *, fields, i) =
            types_new_field(a, pos, pkg, name, ftyp, embedded);
        if (tag.len != 0) {
            while (tags.len < i) {
                Str empty = GCI_S("");
                tags = gci_append(a, tags, &empty);
            }
            tags = gci_append(a, tags, &tag);
        }
    }
    return types_new_struct(a, fields, tags);
}

static TypesUnion *gci_union_type(GciReader *r) {
    Alloc *a = r->p->a;
    Int n = burrow__pb_len(&r->d);
    gci_assert(n <= r->d.data.len); /* Not in Go: a bound before allocating */
    Slice terms = gci_ptrs(a, n);
    for (Int i = 0; i < n; i++) {
        bool tilde = burrow__pb_bool(&r->d);
        BURROW_AT(TypesTerm *, terms, i) = types_new_term(a, tilde, gci_typ(r));
    }
    return types_new_union(a, terms);
}

static TypesInterface *gci_interface_type(GciReader *r) {
    Alloc *a = r->p->a;
    Int nmethods = burrow__pb_len(&r->d);
    gci_assert(nmethods <= r->d.data.len); /* Not in Go: a bound before allocating */
    Slice methods = gci_ptrs(a, nmethods);
    Int nembeddeds = burrow__pb_len(&r->d);
    gci_assert(nembeddeds <= r->d.data.len);
    Slice embeddeds = gci_ptrs(a, nembeddeds);
    bool implicit = nmethods == 0 && nembeddeds == 1 && burrow__pb_bool(&r->d);

    for (Int i = 0; i < nmethods; i++) {
        TokenPos pos = gci_pos(r);
        TypesPackage *pkg;
        Str name = gci_selector(r, &pkg);
        TypesSignature *mtyp = gci_signature(r, NULL, slice_nil(TYPE_UNSAFE_POINTER),
                                             slice_nil(TYPE_UNSAFE_POINTER));
        BURROW_AT(TypesFunc *, methods, i) = types_new_func(a, pos, pkg, name, mtyp);
    }

    for (Int i = 0; i < nembeddeds; i++)
        BURROW_AT(TypesType, embeddeds, i) = gci_typ(r);

    TypesInterface *iface = types_new_interface_type(a, methods, embeddeds);
    if (implicit)
        types_interface_mark_implicit(iface);

    /* We need to call iface.Complete(), but if there are any embedded defined
     * types, then we may not have set their underlying interface type yet. So
     * we need to defer calling Complete until after we've called SetUnderlying
     * everywhere.
     *
     * TODO(mdempsky): After CL 424876 lands, it should be safe to call
     * iface.Complete() immediately. */
    r->p->ifaces = gci_append(a, r->p->ifaces, &iface);

    return iface;
}

static TypesTuple *gci_params(GciReader *r, TypesVarKind kind) {
    burrow__pb_sync(&r->d, PB_SYNC_PARAMS);

    Int n = burrow__pb_len(&r->d);
    gci_assert(n <= r->d.data.len); /* Not in Go: a bound before allocating */
    Slice params = gci_ptrs(r->p->a, n);
    for (Int i = 0; i < n; i++)
        BURROW_AT(TypesVar *, params, i) = gci_param(r, kind);

    return types_new_tuple(r->p->a, params);
}

static TypesVar *gci_param(GciReader *r, TypesVarKind kind) {
    burrow__pb_sync(&r->d, PB_SYNC_PARAM);

    TokenPos pos = gci_pos(r);
    TypesPackage *pkg;
    Str name = gci_local_ident(r, &pkg);
    TypesType typ = gci_typ(r);

    TypesVar *param = types_new_param(r->p->a, pos, pkg, name, typ);
    types_var_set_kind(param, kind); /* in {Recv,Param,Result}Var */
    return param;
}

static TypesSignature *gci_signature(GciReader *r, TypesVar *recv, Slice rtparams,
                                     Slice tparams) {
    burrow__pb_sync(&r->d, PB_SYNC_SIGNATURE);

    TypesTuple *params = gci_params(r, TYPES_PARAM_VAR);
    TypesTuple *results = gci_params(r, TYPES_RESULT_VAR);
    bool variadic = burrow__pb_bool(&r->d);

    return types_new_signature_type(r->p->a, recv, rtparams, tparams, params, results,
                                    variadic);
}

static TypesType gci_do_typ(GciReader *r) {
    Alloc *a = r->p->a;
    Int tag = burrow__pb_code(&r->d, PB_SYNC_TYPE);
    switch (tag) {
    case PB_TYPE_BASIC: {
        Int kind = burrow__pb_len(&r->d);
        gci_assert(kind < 26); /* Not in Go: the index is checked */
        return types_type(types_typ[kind]);
    }

    case PB_TYPE_NAMED: {
        Slice targs;
        TypesObject obj;
        gci_obj(r, &targs, &obj);
        TypesTypeName *name = types_as_type_name(obj);
        gci_assert(name != NULL); /* Not in Go: the type assertion */
        if (targs.len != 0) {
            Error err = BURROW_NO_ERROR;
            TypesType t =
                types_instantiate(a, r->p->ctxt, types_object_type(types_object(name)),
                                  targs, false, &err);
            return t;
        }
        return types_object_type(types_object(name));
    }

    case PB_TYPE_TYPE_PARAM: {
        Int n = burrow__pb_len(&r->d);
        gci_assert(r->dict != NULL); /* Not in Go: the indexes are checked */
        if (n < r->dict->nrtbounds) {
            gci_assert(n < r->dict->rtparams.len);
            return types_type(BURROW_AT(TypesTypeParam *, r->dict->rtparams, n));
        }
        gci_assert(n - r->dict->nrtbounds < r->dict->tparams.len);
        return types_type(
            BURROW_AT(TypesTypeParam *, r->dict->tparams, n - r->dict->nrtbounds));
    }

    case PB_TYPE_ARRAY: {
        int64_t len = (int64_t)burrow__pb_uint64(&r->d);
        return types_type(types_new_array(a, gci_typ(r), len));
    }
    case PB_TYPE_CHAN: {
        TypesChanDir dir = (TypesChanDir)burrow__pb_len(&r->d);
        return types_type(types_new_chan(a, dir, gci_typ(r)));
    }
    case PB_TYPE_MAP: {
        TypesType key = gci_typ(r);
        return types_type(types_new_map(a, key, gci_typ(r)));
    }
    case PB_TYPE_POINTER:
        return types_type(types_new_pointer(a, gci_typ(r)));
    case PB_TYPE_SIGNATURE:
        return types_type(gci_signature(r, NULL, slice_nil(TYPE_UNSAFE_POINTER),
                                        slice_nil(TYPE_UNSAFE_POINTER)));
    case PB_TYPE_SLICE:
        return types_type(types_new_slice(a, gci_typ(r)));
    case PB_TYPE_STRUCT:
        return types_type(gci_struct_type(r));
    case PB_TYPE_INTERFACE:
        return types_type(gci_interface_type(r));
    case PB_TYPE_UNION:
        return types_type(gci_union_type(r));

    default:
        panic_str(fmt_sprintf_v(a, "unhandled type tag: %d", tag));
    }
}

/* @@@ Objects */

static void gci_obj(GciReader *r, Slice *targs, TypesObject *obj) {
    burrow__pb_sync(&r->d, PB_SYNC_OBJECT);

    if (burrow__pb_version_has(burrow__pb_decoder_version(&r->d),
                               PB_FIELD_DERIVED_FUNC_INSTANCE))
        gci_assert(!burrow__pb_bool(&r->d));

    Str name;
    TypesPackage *pkg =
        gci_obj_idx(r->p, burrow__pb_reloc(&r->d, PB_SECTION_OBJ), &name);
    *obj = types_scope_lookup(gci_pkg_scope(pkg), name);

    Int n = burrow__pb_len(&r->d);
    gci_assert(n <= r->d.data.len); /* Not in Go: a bound before allocating */
    *targs = gci_ptrs(r->p->a, n);
    for (Int i = 0; i < n; i++)
        BURROW_AT(TypesType, *targs, i) = gci_typ(r);
}

typedef struct GciIndexedMethod {
    Int index; /* (or -1 in V4) */
    TypesFunc *fn;
} GciIndexedMethod;

static int gci_compare_method_index(void *env, const void *x, const void *y) {
    (void)env;
    const GciIndexedMethod *a = *(const GciIndexedMethod *const *)x;
    const GciIndexedMethod *b = *(const GciIndexedMethod *const *)y;
    return (a->index > b->index) - (a->index < b->index);
}

/* The index comes back through idx, -1 when the version has no method order. */
static TypesFunc *gci_method(GciReader *r, Int *idx) {
    Alloc *a = r->p->a;
    burrow__pb_sync(&r->d, PB_SYNC_METHOD);
    *idx = -1;
    if (burrow__pb_version_has(burrow__pb_decoder_version(&r->d),
                               PB_FIELD_PRESERVE_METHOD_ORDER))
        *idx = burrow__pb_len(&r->d);
    TokenPos pos = gci_pos(r);
    TypesPackage *pkg;
    Str name = gci_selector(r, &pkg);

    Slice rparams = gci_type_param_names(r, false);
    TypesVar *recv = gci_param(r, TYPES_RECV_VAR);
    TypesSignature *sig =
        gci_signature(r, recv, rparams, slice_nil(TYPE_UNSAFE_POINTER));

    (void)gci_pos(r); /* TODO(mdempsky): Remove; this is a hacker for linker.go. */
    return types_new_func(a, pos, pkg, name, sig);
}

static void gci_declare(TypesPackage *obj_pkg, TypesObject obj) {
    types_scope_insert(gci_pkg_scope(obj_pkg), obj);
}

static TypesPackage *gci_obj_idx(GciPkgReader *pr, PbIndex idx, Str *name) {
    Alloc *a = pr->a;
    TypesPackage *obj_pkg;
    Str obj_name;
    Int tag;
    {
        GciReader rname = gci_temp_reader(pr, PB_SECTION_NAME, idx, PB_SYNC_OBJECT1);

        obj_name = gci_qualified_ident(&rname, &obj_pkg);
        gci_assert(obj_name.len != 0);

        tag = burrow__pb_code(&rname.d, PB_SYNC_CODE_OBJ);
        gci_retire_reader(pr, &rname);
    }
    *name = obj_name;

    if (tag == PB_OBJ_STUB) {
        gci_assert(obj_pkg == NULL || obj_pkg == types_unsafe());
        return obj_pkg;
    }

    /* Ignore local types promoted to global scope (#55110). */
    Str suffix;
    (void)gci_split_vargen_suffix(obj_name, &suffix);
    if (suffix.len != 0)
        return obj_pkg;

    /* TODO(mark): This, like the above splitVargenSuffix, is not ideal.
     * Ignore generic methods promoted to global scope. */
    if (strings_contains(obj_name, GCI_S(".")))
        return obj_pkg;

    if (types_scope_lookup(gci_pkg_scope(obj_pkg), obj_name) == NULL) {
        GciReaderDict *dict = gci_obj_dict_idx(pr, idx);

        GciReader r = gci_new_reader(pr, PB_SECTION_OBJ, idx, PB_SYNC_OBJECT1);
        r.dict = dict;
        PbVersion v = burrow__pb_decoder_version(&r.d);

        switch (tag) {
        case PB_OBJ_ALIAS: {
            TokenPos pos = gci_pos(&r);
            Slice tparams = slice_nil(TYPE_UNSAFE_POINTER);
            if (burrow__pb_version_has(v, PB_FIELD_ALIAS_TYPE_PARAM_NAMES))
                tparams = gci_type_param_names(&r, false);
            TypesType typ = gci_typ(&r);
            gci_declare(obj_pkg, types_object(gci_new_alias_type_name(
                                     a, pos, obj_pkg, obj_name, typ, tparams)));
            break;
        }

        case PB_OBJ_CONST: {
            TokenPos pos = gci_pos(&r);
            TypesType typ = gci_typ(&r);
            ConstantValue val = burrow__pb_value(&r.d);
            gci_declare(obj_pkg, types_object(types_new_const(a, pos, obj_pkg, obj_name,
                                                              typ, val)));
            break;
        }

        case PB_OBJ_FUNC: {
            TokenPos pos = gci_pos(&r);
            if (burrow__pb_version_has(v, PB_FIELD_GENERIC_METHODS))
                gci_assert(!burrow__pb_bool(&r.d)); /* generic methods are read in their
                                                       defining type */
            Slice tparams = gci_type_param_names(&r, false);
            TypesSignature *sig =
                gci_signature(&r, NULL, slice_nil(TYPE_UNSAFE_POINTER), tparams);
            gci_declare(obj_pkg,
                        types_object(types_new_func(a, pos, obj_pkg, obj_name, sig)));
            break;
        }

        case PB_OBJ_TYPE: {
            TokenPos pos = gci_pos(&r);

            TypesTypeName *obj = types_new_type_name(a, pos, obj_pkg, obj_name, NULL);
            TypesNamed *named =
                types_new_named(a, obj, NULL, slice_nil(TYPE_UNSAFE_POINTER));
            gci_declare(obj_pkg, types_object(obj));

            types_named_set_type_params(named, gci_type_param_names(&r, false));

            TypesType underlying = types_type_underlying(gci_typ(&r));

            /* If the underlying type is an interface, we need to duplicate its
             * methods so we can replace the receiver parameter's type (#49906). */
            TypesInterface *iface = types_as_interface(underlying);
            if (iface != NULL && types_interface_num_explicit_methods(iface) != 0) {
                Int nm = types_interface_num_explicit_methods(iface);
                Slice methods = gci_ptrs(a, nm);
                for (Int i = 0; i < nm; i++) {
                    TypesFunc *fn = types_interface_explicit_method(iface, i);
                    TypesSignature *sig = types_func_signature(fn);
                    TypesObject fo = types_object(fn);

                    TypesVar *recv =
                        types_new_var(a, types_object_pos(fo), types_object_pkg(fo),
                                      GCI_S(""), types_type(named));
                    types_var_set_kind(recv, TYPES_RECV_VAR);
                    BURROW_AT(TypesFunc *, methods, i) = types_new_func(
                        a, types_object_pos(fo), types_object_pkg(fo),
                        types_object_name(fo),
                        types_new_signature(a, recv, types_signature_params(sig),
                                            types_signature_results(sig),
                                            types_signature_variadic(sig)));
                }

                Int ne = types_interface_num_embeddeds(iface);
                Slice embeds = gci_ptrs(a, ne);
                for (Int i = 0; i < ne; i++)
                    BURROW_AT(TypesType, embeds, i) =
                        types_interface_embedded_type(iface, i);

                TypesInterface *new_iface =
                    types_new_interface_type(a, methods, embeds);
                pr->ifaces = gci_append(a, pr->ifaces, &new_iface);
                underlying = types_type(new_iface);
            }

            types_named_set_underlying(named, underlying);

            if (burrow__pb_version_has(v, PB_FIELD_GENERIC_METHODS)) {
                /* V4 (go1.27.0) emitted all non-generic methods before all
                 * generic ones, discarding source order: a bug
                 * (go.dev/issue/81188). V5 (go1.27.x) fixes it by emitting an
                 * explicit index along with each method. */

                /* ordinary methods */
                Int nord = burrow__pb_len(&r.d);
                gci_assert(nord <= r.d.data.len); /* Not in Go: bounds first */
                GciIndexedMethod *ord = gci_alloc(a, (size_t)nord * sizeof *ord,
                                                  _Alignof(GciIndexedMethod));
                for (Int i = 0; i < nord; i++)
                    ord[i].fn = gci_method(&r, &ord[i].index);

                /* generic methods */
                Int ngen = burrow__pb_len(&r.d);
                gci_assert(ngen <= r.d.data.len); /* Not in Go: bounds first */
                Int nmethods = nord + ngen;
                GciIndexedMethod *methods = gci_alloc(
                    a, (size_t)nmethods * sizeof *methods, _Alignof(GciIndexedMethod));
                for (Int i = 0; i < nord; i++)
                    methods[i] = ord[i];
                for (Int i = nord; i < nmethods; i++) {
                    /* Careful: objIdx is used to read in package-scoped
                     * declarations, which methods are not. Instead, decode it
                     * here. This makes it easier to associate it with the type
                     * and avoids the main objIdx loop. */
                    PbIndex midx = burrow__pb_reloc(&r.d, PB_SECTION_OBJ);

                    GciReader mr =
                        gci_temp_reader(pr, PB_SECTION_OBJ, midx, PB_SYNC_OBJECT1);
                    mr.dict = gci_obj_dict_idx(pr, midx);

                    TokenPos mpos = gci_pos(&mr);
                    gci_assert(burrow__pb_bool(&mr.d)); /* generic method */
                    TypesPackage *pkg;
                    Str mname = gci_selector(&mr, &pkg);
                    Slice rtparams = gci_type_param_names(&mr, true);
                    TypesVar *recv = gci_param(&mr, TYPES_RECV_VAR);
                    Int method_idx = -1;
                    if (burrow__pb_version_has(burrow__pb_decoder_version(&mr.d),
                                               PB_FIELD_PRESERVE_METHOD_ORDER))
                        method_idx = burrow__pb_len(&mr.d);
                    Slice tparams = gci_type_param_names(&mr, false);
                    TypesSignature *sig = gci_signature(&mr, recv, rtparams, tparams);

                    gci_retire_reader(pr, &mr);
                    methods[i].index = method_idx;
                    methods[i].fn = types_new_func(a, mpos, pkg, mname, sig);
                }

                Slice order = gci_ptrs(a, nmethods);
                for (Int i = 0; i < nmethods; i++)
                    BURROW_AT(GciIndexedMethod *, order, i) = &methods[i];
                if (burrow__pb_version_has(v, PB_FIELD_PRESERVE_METHOD_ORDER))
                    slices_sort_func(order,
                                     (SlicesCmpFunc){gci_compare_method_index, NULL});

                for (Int i = 0; i < nmethods; i++)
                    types_named_add_method(named,
                                           BURROW_AT(GciIndexedMethod *, order, i)->fn);

            } else {
                for (Int i = 0, n = burrow__pb_len(&r.d); i < n; i++) {
                    Int unused;
                    types_named_add_method(named, gci_method(&r, &unused));
                }
            }
            break;
        }

        case PB_OBJ_VAR: {
            TokenPos pos = gci_pos(&r);
            TypesType typ = gci_typ(&r);
            gci_declare(obj_pkg,
                        types_object(types_new_var(a, pos, obj_pkg, obj_name, typ)));
            break;
        }

        default:
            panic_str(GCI_S("weird"));
        }
    }

    return obj_pkg;
}

static GciReaderDict *gci_obj_dict_idx(GciPkgReader *pr, PbIndex idx) {
    Alloc *a = pr->a;
    GciReaderDict *dict = gci_alloc(a, sizeof *dict, _Alignof(GciReaderDict));
    *dict = (GciReaderDict){0};
    dict->rtparams = slice_nil(TYPE_UNSAFE_POINTER);
    dict->tparams = slice_nil(TYPE_UNSAFE_POINTER);

    {
        GciReader r = gci_temp_reader(pr, PB_SECTION_OBJ_DICT, idx, PB_SYNC_OBJECT1);
        PbVersion v = burrow__pb_decoder_version(&r.d);
        Int implicits = burrow__pb_len(&r.d);
        if (implicits != 0)
            panic_str(fmt_sprintf_v(
                a, "unexpected object with %d implicit type parameter(s)", implicits));

        Int nreceivers = 0;
        if (burrow__pb_version_has(v, PB_FIELD_GENERIC_METHODS))
            nreceivers = burrow__pb_len(&r.d);
        Int nexplicits = burrow__pb_len(&r.d);
        /* Not in Go: bounds before allocating */
        gci_assert(nreceivers <= r.d.data.len && nexplicits <= r.d.data.len);

        dict->rtbounds = gci_alloc(a, (size_t)nreceivers * sizeof(GciTypeInfo),
                                   _Alignof(GciTypeInfo));
        dict->nrtbounds = nreceivers;
        for (Int i = 0; i < nreceivers; i++)
            dict->rtbounds[i] = gci_typ_info(&r);

        dict->tbounds = gci_alloc(a, (size_t)nexplicits * sizeof(GciTypeInfo),
                                  _Alignof(GciTypeInfo));
        dict->ntbounds = nexplicits;
        for (Int i = 0; i < nexplicits; i++)
            dict->tbounds[i] = gci_typ_info(&r);

        Int nderived = burrow__pb_len(&r.d);
        gci_assert(nderived <= r.d.data.len);
        dict->derived =
            gci_alloc(a, (size_t)nderived * sizeof(PbIndex), _Alignof(PbIndex));
        dict->derived_types =
            gci_alloc(a, (size_t)nderived * sizeof(TypesType), _Alignof(TypesType));
        dict->nderived = nderived;
        for (Int i = 0; i < nderived; i++) {
            dict->derived[i] = burrow__pb_reloc(&r.d, PB_SECTION_TYPE);
            dict->derived_types[i] = NULL;
            if (burrow__pb_version_has(v, PB_FIELD_DERIVED_INFO_NEEDED))
                gci_assert(!burrow__pb_bool(&r.d));
        }

        gci_retire_reader(pr, &r);
    }
    /* function references follow, but reader doesn't need those */

    return dict;
}

static Slice gci_type_param_names(GciReader *r, bool is_gen_meth) {
    Alloc *a = r->p->a;
    burrow__pb_sync(&r->d, PB_SYNC_TYPE_PARAM_NAMES);

    /* Note: This code assumes there are no implicit type parameters. This is
     * fine since it only reads exported declarations, which never have
     * implicits. */

    GciTypeInfo *in;
    Int nin;
    Slice *out;
    if (is_gen_meth) {
        in = r->dict->rtbounds;
        nin = r->dict->nrtbounds;
        out = &r->dict->rtparams;
    } else {
        in = r->dict->tbounds;
        nin = r->dict->ntbounds;
        out = &r->dict->tparams;
    }

    if (nin == 0)
        return slice_nil(TYPE_UNSAFE_POINTER);

    /* Careful: Type parameter lists may have cycles. To allow for this, we
     * construct the type parameter list in two passes: first we create all the
     * TypeNames and TypeParams, then we construct and set the bound type. */

    /* We have to save tparams outside of the closure, because typeParamNames
     * can be called multiple times with the same dictionary instance. */
    Slice tparams = gci_ptrs(a, nin);
    *out = tparams;

    for (Int i = 0; i < nin; i++) {
        TokenPos pos = gci_pos(r);
        TypesPackage *pkg;
        Str name = gci_local_ident(r, &pkg);

        TypesTypeName *tname = types_new_type_name(a, pos, pkg, name, NULL);
        BURROW_AT(TypesTypeParam *, tparams, i) = types_new_type_param(a, tname, NULL);
    }

    /* The reader dictionary will continue mutating before we have time to call
     * delayed functions; make a local copy of the constraints. */
    Slice types = gci_ptrs(a, nin);
    for (Int i = 0; i < nin; i++)
        BURROW_AT(TypesType, types, i) = gci_typ_idx(r->p, in[i], r->dict);

    /* This needs to happen later to make sure SetUnderlying has been called. */
    GciLater *later = gci_alloc(a, sizeof *later, _Alignof(GciLater));
    *later = (GciLater){tparams, types};
    r->p->later_fns = gci_append(a, r->p->later_fns, &later);

    return tparams;
}

static int gci_compare_path(void *env, const void *x, const void *y) {
    (void)env;
    TypesPackage *a = *(TypesPackage *const *)x;
    TypesPackage *b = *(TypesPackage *const *)y;
    return (int)strings_compare(types_package_path(a), types_package_path(b));
}

/* readUnifiedPackage reads a package description from the given unified IR
 * export data decoder. */
static TypesPackage *gci_read_unified_package(Alloc *a, TokenFileSet *fset,
                                              TypesContext *ctxt, Map *imports,
                                              PbPkgDecoder input) {
    GciPkgReader *pr = gci_alloc(a, sizeof *pr, _Alignof(GciPkgReader));
    *pr = (GciPkgReader){0};
    pr->dec = input;
    pr->fake.fset = fset;
    pr->fake.files = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    if (pr->fake.files == NULL)
        panic_str(GCI_S("gcimporter: out of memory"));
    pr->fake.list = slice_nil(TYPE_UNSAFE_POINTER);
    pr->ctxt = ctxt;
    pr->imports = imports;
    pr->a = a;

    Int nposbases = burrow__pb_num_elems(&input, PB_SECTION_POS_BASE);
    Int npkgs = burrow__pb_num_elems(&input, PB_SECTION_PKG);
    Int ntyps = burrow__pb_num_elems(&input, PB_SECTION_TYPE);
    pr->pos_bases = gci_alloc(a, (size_t)nposbases * sizeof(Str), _Alignof(Str));
    for (Int i = 0; i < nposbases; i++)
        pr->pos_bases[i] = GCI_S("");
    pr->pkgs =
        gci_alloc(a, (size_t)npkgs * sizeof(TypesPackage *), _Alignof(TypesPackage *));
    memset(pr->pkgs, 0, (size_t)npkgs * sizeof(TypesPackage *));
    pr->typs = gci_alloc(a, (size_t)ntyps * sizeof(TypesType), _Alignof(TypesType));
    memset(pr->typs, 0, (size_t)ntyps * sizeof(TypesType));
    pr->later_fns = slice_nil(TYPE_UNSAFE_POINTER);
    pr->ifaces = slice_nil(TYPE_UNSAFE_POINTER);

    GciReader r =
        gci_new_reader(pr, PB_SECTION_META, PB_PUBLIC_ROOT_IDX, PB_SYNC_PUBLIC);
    PbVersion v = burrow__pb_decoder_version(&r.d);
    TypesPackage *pkg = gci_pkg(&r);
    if (burrow__pb_version_has(v, PB_FIELD_HAS_INIT))
        (void)burrow__pb_bool(&r.d);

    for (Int i = 0, n = burrow__pb_len(&r.d); i < n; i++) {
        /* As if r.obj(), but avoiding the Scope.Lookup call, to avoid eager
         * loading of imports. */
        burrow__pb_sync(&r.d, PB_SYNC_OBJECT);
        if (burrow__pb_version_has(v, PB_FIELD_DERIVED_FUNC_INSTANCE))
            gci_assert(!burrow__pb_bool(&r.d));
        Str name;
        (void)gci_obj_idx(pr, burrow__pb_reloc(&r.d, PB_SECTION_OBJ), &name);
        gci_assert(burrow__pb_len(&r.d) == 0);
    }

    burrow__pb_sync(&r.d, PB_SYNC_EOF);

    for (Int i = 0; i < pr->later_fns.len; i++) {
        GciLater *fn = BURROW_AT(GciLater *, pr->later_fns, i);
        for (Int j = 0; j < fn->types.len; j++)
            types_type_param_set_constraint(BURROW_AT(TypesTypeParam *, fn->tparams, j),
                                            BURROW_AT(TypesType, fn->types, j));
    }

    for (Int i = 0; i < pr->ifaces.len; i++)
        (void)types_interface_complete(BURROW_AT(TypesInterface *, pr->ifaces, i));

    /* Imports() of pkg are all of the transitive packages that were loaded. */
    Slice imps = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < npkgs; i++) {
        TypesPackage *imp = pr->pkgs[i];
        if (imp != NULL && imp != pkg)
            imps = gci_append(a, imps, &imp);
    }
    slices_sort_func(imps, (SlicesCmpFunc){gci_compare_path, NULL});
    types_package_set_imports(pkg, imps);

    /* package was imported completely and without errors */
    types_package_mark_complete(pkg);

    gci_fake_set_lines(a, &pr->fake);
    return pkg;
}

/* ----------------------------------------------------------------- Import */

TypesPackage *burrow__gcimporter_import(Alloc *a, TokenFileSet *fset, Map *packages,
                                        Str path, Str src_dir, ImporterLookup lookup,
                                        Map *exports, Error *err) {
    *err = BURROW_NO_ERROR;
    IoReadCloser rc;
    Str id;
    Str filename = GCI_S("");
    if (!BURROW_FUNC_IS_NIL(lookup)) {
        /* With custom lookup specified, assume that caller has converted path
         * to a canonical import path for use in the map. */
        if (str_eq(path, GCI_S("unsafe")))
            return types_unsafe();
        id = path;

        /* No need to re-import if the package was imported completely before. */
        TypesPackage **found = map_get(packages, &id);
        if (found != NULL && *found != NULL && types_package_complete(*found))
            return *found;
        Error lerr = BURROW_NO_ERROR;
        IoReadCloser f = BURROW_CALLF(lookup, a, path, &lerr);
        if (BURROW_FAILED(lerr)) {
            *err = lerr;
            return NULL;
        }
        rc = f;
    } else {
        filename = burrow__exportdata_find_pkg(a, path, src_dir, exports, &id, err);
        if (filename.len == 0) {
            if (str_eq(path, GCI_S("unsafe"))) {
                *err = BURROW_NO_ERROR;
                return types_unsafe();
            }
            return NULL;
        }
        *err = BURROW_NO_ERROR;

        /* no need to re-import if the package was imported completely before */
        TypesPackage **found = map_get(packages, &id);
        if (found != NULL && *found != NULL && types_package_complete(*found))
            return *found;

        /* open file */
        OsFile *f = os_open(a, filename, err);
        if (BURROW_FAILED(*err))
            return NULL;
        rc = os_file_as_io_read_closer(f);
    }

    BufioReader *buf = bufio_new_reader(a, (IoReader){&rc.vt->reader, rc.data});
    if (buf == NULL) {
        (void)rc.vt->closer.close(rc.data);
        *err = errors_new(a, GCI_S("out of memory"));
        return NULL;
    }

    TypesPackage *pkg = NULL;
    Slice data = burrow__exportdata_read_unified(a, buf, err);
    bufio_reader_free(buf);
    (void)rc.vt->closer.close(rc.data);
    if (BURROW_FAILED(*err)) {
        *err = fmt_errorf_v("import %q: %v", path, *err);
        if (filename.len != 0) /* add file name to error */
            *err = fmt_errorf_v("%s: %v", filename, *err);
        return NULL;
    }
    Str s = str_from_bytes(data.p, data.len);

    PbPkgDecoder input = burrow__pb_new_pkg_decoder(a, id, s);
    pkg = gci_read_unified_package(a, fset, NULL, packages, input);
    return pkg;
}
