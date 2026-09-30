/* Derived from Go's src/encoding/xml/typeinfo.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/xml.h"

#include "xml_internal.h"

#include "burrow/fmt.h"
#include "burrow/lock.h"
#include "burrow/mem/heap.h"

#include <string.h>

static const Str xi_xml_name = BURROW_S_INIT("XMLName");
static const Str xi_xml_key = BURROW_S_INIT("xml");

/* A value for fmt's %T, which reads the type and never the data for the
 * kinds that get here. */
static Any xi_type_arg(const Type *t) {
    static const uint64_t dummy[2];
    return (Any){t, (void *)(uintptr_t)dummy};
}

static Int xi_index_byte(Str s, Byte c) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] == c)
            return i;
    return -1;
}

/* Go's reflect.StructField.Anonymous, which covers an embedded *T too. */
static bool xi_field_anonymous(const Field *f) {
    if (field_is_embedded(f))
        return true;
    const Type *t = f->type;
    return t != NULL && t->kind == KIND_POINTER && t->name.len == 0 &&
           t->elem != NULL && str_eq(t->elem->name, f->name);
}

/* ---------------------------------------------------------- TagPathError */

typedef struct XmlTagPathErrorBox {
    XmlTagPathError e;
    Str message;
} XmlTagPathErrorBox;

static Str xml_tag_path_error_message(const void *self) {
    return ((const XmlTagPathErrorBox *)self)->message;
}

static const Type xml_tag_path_error_desc = {
    {(const Byte *)"TagPathError", 12},
    {(const Byte *)"encoding/xml", 12},
    KIND_STRUCT,
    (uint32_t)sizeof(XmlTagPathError),
    (uint16_t)_Alignof(XmlTagPathError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x78747065U, /* "xtpe" */
    NULL,
};

const Type *const TYPE_XML_TAG_PATH_ERROR = &xml_tag_path_error_desc;

Str xml_tag_path_error_error(const XmlTagPathError *e, Alloc *a) {
    return fmt_sprintf_v(
        a, "%T field %q with tag %q conflicts with field %q with tag %q",
        xi_type_arg(e->struct_type), e->field1, e->tag1, e->field2, e->tag2);
}

static Error xml_tag_path_error_clone(const void *self, Alloc *a);

static const ErrorVT xml_tag_path_error_vt = {
    &xml_tag_path_error_desc, xml_tag_path_error_message, NULL, NULL, NULL, NULL,
    xml_tag_path_error_clone,
};

/* The box, then the four strings, in one block, and the message in another. */
static Error xml_tag_path_error_new(Alloc *a, const XmlTagPathError *e) {
    Str msg = xml_tag_path_error_error(e, a);
    if (msg.p == NULL)
        return burrow_err_out_of_memory;
    Int n = e->field1.len + e->tag1.len + e->field2.len + e->tag2.len;
    XmlTagPathErrorBox *b = (XmlTagPathErrorBox *)mem_alloc_nozero(
        a, sizeof(XmlTagPathErrorBox) + (size_t)n, _Alignof(XmlTagPathErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    const Str *in[4] = {&e->field1, &e->tag1, &e->field2, &e->tag2};
    Str *out[4] = {&b->e.field1, &b->e.tag1, &b->e.field2, &b->e.tag2};
    for (int i = 0; i < 4; i++) {
        if (in[i]->len > 0)
            memcpy(p, in[i]->p, (size_t)in[i]->len);
        *out[i] = str_from_bytes(p, in[i]->len);
        p += in[i]->len;
    }
    b->e.struct_type = e->struct_type;
    b->message = msg;
    return (Error){&xml_tag_path_error_vt, b};
}

static Error xml_tag_path_error_clone(const void *self, Alloc *a) {
    return xml_tag_path_error_new(a, (const XmlTagPathError *)self);
}

/* ------------------------------------------------------------ the builder */

typedef struct XiBuild {
    Alloc *a;
    XmlTypeInfo *ti;
    Int cap;
} XiBuild;

static const XmlTypeInfo *xi_get_locked(const Type *t, Error *err);

/* structFieldInfo, with the field at path idx. */
static XmlFieldInfo *xi_struct_field_info(Alloc *a, const Type *typ, const Field *f,
                                          const Int *idx, Int nidx, Error *err);

/* lookupXMLName. */
static XmlFieldInfo *xi_lookup_xml_name(Alloc *a, const Type *typ) {
    while (typ->kind == KIND_POINTER)
        typ = typ->elem;
    if (typ->kind != KIND_STRUCT)
        return NULL;
    for (uint16_t i = 0; i < typ->nfield; i++) {
        const Field *f = &typ->fields[i];
        if (!str_eq(f->name, xi_xml_name))
            continue;
        Error err = BURROW_NO_ERROR;
        Int *idx = (Int *)mem_alloc(a, sizeof(Int), _Alignof(Int));
        if (idx == NULL)
            return NULL;
        idx[0] = i;
        XmlFieldInfo *finfo = xi_struct_field_info(a, typ, f, idx, 1, &err);
        if (BURROW_OK(err) && finfo != NULL && finfo->name.len > 0)
            return finfo;
        break;
    }
    return NULL;
}

static bool xi_oom(Error *err) {
    *err = burrow_err_out_of_memory;
    return false;
}

static XmlFieldInfo *xi_struct_field_info(Alloc *a, const Type *typ, const Field *f,
                                          const Int *idx, Int nidx, Error *err) {
    XmlFieldInfo *finfo =
        (XmlFieldInfo *)mem_alloc(a, sizeof(XmlFieldInfo), _Alignof(XmlFieldInfo));
    if (finfo == NULL) {
        xi_oom(err);
        return NULL;
    }
    finfo->idx = idx;
    finfo->nidx = nidx;

    /* Split the tag into namespace, name and flags. */
    Str full = tag_get(a, f->tag, xi_xml_key);
    Str tag = full;
    Int sp = xi_index_byte(tag, ' ');
    if (sp >= 0) {
        finfo->xmlns = str_from_bytes(tag.p, sp);
        tag = str_from_bytes(tag.p + sp + 1, tag.len - sp - 1);
    }

    Int comma = xi_index_byte(tag, ',');
    Str rest = {NULL, 0};
    if (comma < 0) {
        finfo->flags = XF_ELEMENT;
    } else {
        rest = str_from_bytes(tag.p + comma + 1, tag.len - comma - 1);
        tag = str_from_bytes(tag.p, comma);
        Str r = rest;
        for (;;) {
            Int c = xi_index_byte(r, ',');
            Str flag = c < 0 ? r : str_from_bytes(r.p, c);
            if (str_eq(flag, BURROW_S("attr")))
                finfo->flags |= XF_ATTR;
            else if (str_eq(flag, BURROW_S("cdata")))
                finfo->flags |= XF_CDATA;
            else if (str_eq(flag, BURROW_S("chardata")))
                finfo->flags |= XF_CHARDATA;
            else if (str_eq(flag, BURROW_S("innerxml")))
                finfo->flags |= XF_INNERXML;
            else if (str_eq(flag, BURROW_S("comment")))
                finfo->flags |= XF_COMMENT;
            else if (str_eq(flag, BURROW_S("any")))
                finfo->flags |= XF_ANY;
            else if (str_eq(flag, BURROW_S("omitempty")))
                finfo->flags |= XF_OMITEMPTY;
            if (c < 0)
                break;
            r = str_from_bytes(r.p + c + 1, r.len - c - 1);
        }

        /* Validate the flags used. */
        bool valid = true;
        int mode = finfo->flags & XF_MODE;
        switch (mode) {
        case 0:
            finfo->flags |= XF_ELEMENT;
            break;
        case XF_ATTR:
        case XF_CDATA:
        case XF_CHARDATA:
        case XF_INNERXML:
        case XF_COMMENT:
        case XF_ANY:
        case XF_ANY | XF_ATTR:
            if (str_eq(f->name, xi_xml_name) || (tag.len > 0 && mode != XF_ATTR))
                valid = false;
            break;
        default:
            /* This will also catch multiple modes in a single field. */
            valid = false;
        }
        if ((finfo->flags & XF_MODE) == XF_ANY)
            finfo->flags |= XF_ELEMENT;
        if ((finfo->flags & XF_OMITEMPTY) != 0 &&
            (finfo->flags & (XF_ELEMENT | XF_ATTR)) == 0)
            valid = false;
        if (!valid) {
            *err = fmt_errorf_v("xml: invalid tag in field %s of type %T: %q", f->name,
                                xi_type_arg(typ), full);
            return NULL;
        }
    }

    /* Use of xmlns without a name is not allowed. */
    if (finfo->xmlns.len > 0 && tag.len == 0) {
        *err = fmt_errorf_v("xml: namespace without name in field %s of type %T: %q",
                            f->name, xi_type_arg(typ), full);
        return NULL;
    }

    if (str_eq(f->name, xi_xml_name)) {
        /* The XMLName field records the XML element name. Don't modify
         * finfo->name. */
        finfo->name = tag;
        return finfo;
    }

    if (tag.len == 0) {
        /* If the name part of the tag is completely empty, get the default
         * from the XMLName of the underlying struct if feasible, or field
         * name otherwise. */
        XmlFieldInfo *xmlname = xi_lookup_xml_name(a, f->type);
        if (xmlname != NULL) {
            finfo->xmlns = xmlname->xmlns;
            finfo->name = xmlname->name;
        } else {
            finfo->name = f->name;
        }
        return finfo;
    }

    /* Prepare field name and parents. */
    Int np = 1;
    for (Int i = 0; i < tag.len; i++)
        if (tag.p[i] == '>')
            np++;
    Str *parents = (Str *)mem_alloc(a, (size_t)np * sizeof(Str), _Alignof(Str));
    if (parents == NULL) {
        xi_oom(err);
        return NULL;
    }
    Str r = tag;
    for (Int i = 0; i < np; i++) {
        Int gt = xi_index_byte(r, '>');
        parents[i] = gt < 0 ? r : str_from_bytes(r.p, gt);
        if (gt >= 0)
            r = str_from_bytes(r.p + gt + 1, r.len - gt - 1);
    }
    if (parents[0].len == 0)
        parents[0] = f->name;
    if (parents[np - 1].len == 0) {
        *err = fmt_errorf_v("xml: trailing '>' in field %s of type %T", f->name,
                            xi_type_arg(typ));
        return NULL;
    }
    finfo->name = parents[np - 1];
    if (np > 1) {
        if ((finfo->flags & XF_ELEMENT) == 0) {
            *err = fmt_errorf_v("xml: %s chain not valid with %s flag", tag, rest);
            return NULL;
        }
        finfo->parents = parents;
        finfo->nparents = np - 1;
    }

    /* If the field type has an XMLName field, the names must match also if
     * the tag says so. */
    if ((finfo->flags & XF_ELEMENT) != 0) {
        const Type *ftyp = f->type;
        XmlFieldInfo *xmlname = xi_lookup_xml_name(a, ftyp);
        if (xmlname != NULL && !str_eq(xmlname->name, finfo->name)) {
            *err = fmt_errorf_v(
                "xml: name %q in tag of %T.%s conflicts with name %q in %T.XMLName",
                finfo->name, xi_type_arg(typ), f->name, xmlname->name,
                xi_type_arg(ftyp));
            return NULL;
        }
    }
    return finfo;
}

/* typ.FieldByIndex(idx), which follows pointers to embedded structs. */
static const Field *xi_field_by_index(const Type *typ, const Int *idx, Int nidx) {
    const Field *f = NULL;
    for (Int i = 0; i < nidx; i++) {
        if (i > 0 && typ->kind == KIND_POINTER && typ->elem->kind == KIND_STRUCT)
            typ = typ->elem;
        f = &typ->fields[idx[i]];
        typ = f->type;
    }
    return f;
}

static bool xi_append(XiBuild *b, const XmlFieldInfo *f, Error *err) {
    XmlTypeInfo *ti = b->ti;
    if (ti->nfields == b->cap) {
        Int cap = b->cap == 0 ? 8 : b->cap * 2;
        XmlFieldInfo *nf = (XmlFieldInfo *)mem_alloc(
            b->a, (size_t)cap * sizeof(XmlFieldInfo), _Alignof(XmlFieldInfo));
        if (nf == NULL)
            return xi_oom(err);
        if (ti->nfields > 0)
            memcpy(nf, ti->fields, (size_t)ti->nfields * sizeof(XmlFieldInfo));
        ti->fields = nf;
        b->cap = cap;
    }
    ti->fields[ti->nfields++] = *f;
    return true;
}

static bool xi_parents_eq(const XmlFieldInfo *a, const XmlFieldInfo *b, Int n) {
    for (Int p = 0; p < n; p++)
        if (!str_eq(a->parents[p], b->parents[p]))
            return false;
    return true;
}

/* addFieldInfo: adds newf to the fields, or not, or replaces the ones it
 * conflicts with, or fails, by Go's rules for embedding. */
static bool xi_add_field_info(XiBuild *b, const Type *typ, const XmlFieldInfo *newf,
                              Error *err) {
    XmlTypeInfo *ti = b->ti;
    Int stack_conflicts[8];
    Int *conflicts = stack_conflicts;
    Int nconflicts = 0, capconflicts = 8;

    /* First, figure all conflicts. Most working code will have none. */
    for (Int i = 0; i < ti->nfields; i++) {
        const XmlFieldInfo *oldf = &ti->fields[i];
        if ((oldf->flags & XF_MODE) != (newf->flags & XF_MODE))
            continue;
        if (oldf->xmlns.len > 0 && newf->xmlns.len > 0 &&
            !str_eq(oldf->xmlns, newf->xmlns))
            continue;
        Int minl = newf->nparents < oldf->nparents ? newf->nparents : oldf->nparents;
        if (!xi_parents_eq(oldf, newf, minl))
            continue;
        bool conflict;
        if (oldf->nparents > newf->nparents)
            conflict = str_eq(oldf->parents[newf->nparents], newf->name);
        else if (oldf->nparents < newf->nparents)
            conflict = str_eq(newf->parents[oldf->nparents], oldf->name);
        else
            conflict =
                str_eq(newf->name, oldf->name) && str_eq(newf->xmlns, oldf->xmlns);
        if (!conflict)
            continue;
        if (nconflicts == capconflicts) {
            Int *nc = (Int *)mem_alloc(b->a, (size_t)capconflicts * 2 * sizeof(Int),
                                       _Alignof(Int));
            if (nc == NULL)
                return xi_oom(err);
            memcpy(nc, conflicts, (size_t)nconflicts * sizeof(Int));
            conflicts = nc;
            capconflicts *= 2;
        }
        conflicts[nconflicts++] = i;
    }

    /* Without conflicts, add the new field and return. */
    if (nconflicts == 0)
        return xi_append(b, newf, err);

    /* If any conflict is shallower, ignore the new field. This matches the
     * Go field resolution on embedding. */
    for (Int c = 0; c < nconflicts; c++)
        if (ti->fields[conflicts[c]].nidx < newf->nidx)
            return true;

    /* Otherwise, if any of them is at the same depth level, it's an error. */
    for (Int c = 0; c < nconflicts; c++) {
        const XmlFieldInfo *oldf = &ti->fields[conflicts[c]];
        if (oldf->nidx == newf->nidx) {
            const Field *f1 = xi_field_by_index(typ, oldf->idx, oldf->nidx);
            const Field *f2 = xi_field_by_index(typ, newf->idx, newf->nidx);
            XmlTagPathError e = {
                typ,
                f1->name,
                tag_get(b->a, f1->tag, xi_xml_key),
                f2->name,
                tag_get(b->a, f2->tag, xi_xml_key),
            };
            *err = xml_tag_path_error_new(error_allocator(), &e);
            return false;
        }
    }

    /* Otherwise, the new field is shallower, and thus takes precedence, so
     * drop the conflicting fields from tinfo and append the new one. */
    for (Int c = nconflicts - 1; c >= 0; c--) {
        Int i = conflicts[c];
        memmove(&ti->fields[i], &ti->fields[i + 1],
                (size_t)(ti->nfields - i - 1) * sizeof(XmlFieldInfo));
        ti->nfields--;
    }
    return xi_append(b, newf, err);
}

/* getTypeInfo's body, building into b. */
static bool xi_build(XiBuild *b, const Type *typ, Error *err) {
    XmlTypeInfo *ti = b->ti;
    if (typ->kind != KIND_STRUCT || typ == &burrow_type_XmlName)
        return true;
    for (uint16_t i = 0; i < typ->nfield; i++) {
        const Field *f = &typ->fields[i];
        bool anonymous = xi_field_anonymous(f);
        if (!field_is_exported(f) && !anonymous)
            continue; /* Private field */
        Str x = tag_get(b->a, f->tag, xi_xml_key);
        if (x.len == 1 && x.p[0] == '-')
            continue;

        /* For embedded structs, embed its fields. */
        if (anonymous) {
            const Type *t = f->type;
            if (t->kind == KIND_POINTER)
                t = t->elem;
            if (t->kind == KIND_STRUCT) {
                const XmlTypeInfo *inner = xi_get_locked(t, err);
                if (inner == NULL)
                    return false;
                if (ti->xmlname == NULL)
                    ti->xmlname = inner->xmlname;
                for (Int k = 0; k < inner->nfields; k++) {
                    XmlFieldInfo finfo = inner->fields[k];
                    Int *idx = (Int *)mem_alloc(
                        b->a, (size_t)(finfo.nidx + 1) * sizeof(Int), _Alignof(Int));
                    if (idx == NULL)
                        return xi_oom(err);
                    idx[0] = i;
                    memcpy(idx + 1, finfo.idx, (size_t)finfo.nidx * sizeof(Int));
                    finfo.idx = idx;
                    finfo.nidx++;
                    if (!xi_add_field_info(b, typ, &finfo, err))
                        return false;
                }
                continue;
            }
        }

        Int *idx = (Int *)mem_alloc(b->a, sizeof(Int), _Alignof(Int));
        if (idx == NULL)
            return xi_oom(err);
        idx[0] = i;
        XmlFieldInfo *finfo = xi_struct_field_info(b->a, typ, f, idx, 1, err);
        if (finfo == NULL)
            return false;

        if (str_eq(f->name, xi_xml_name)) {
            ti->xmlname = finfo;
            continue;
        }

        /* Add the field if it doesn't conflict with other fields. */
        if (!xi_add_field_info(b, typ, finfo, err))
            return false;
    }
    return true;
}

/* ---------------------------------------------------------------- the cache */

/* Every type's info, built once and kept, as Go keeps it in a sync.Map. Each
 * one has an arena of its own. */
typedef struct XiEntry {
    const Type *t;
    const XmlTypeInfo *ti;
} XiEntry;

static burrow__Lock xi_lock;
static XiEntry *xi_cache;
static Int xi_cache_cap;
static Int xi_cache_len;

static uint64_t xi_ptr_hash(const void *p) {
    uint64_t h = (uint64_t)(uintptr_t)p;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

static const XmlTypeInfo *xi_cache_find(const Type *t) {
    if (xi_cache_cap == 0)
        return NULL;
    uint64_t m = (uint64_t)(xi_cache_cap - 1);
    for (uint64_t h = xi_ptr_hash(t) & m;; h = (h + 1) & m) {
        if (xi_cache[h].t == t)
            return xi_cache[h].ti;
        if (xi_cache[h].t == NULL)
            return NULL;
    }
}

static bool xi_cache_put(const Type *t, const XmlTypeInfo *ti) {
    if ((xi_cache_len + 1) * 2 > xi_cache_cap) {
        Int nc = xi_cache_cap == 0 ? 64 : xi_cache_cap * 2;
        XiEntry *nv = (XiEntry *)mem_alloc(
            heap_allocator(), (size_t)nc * sizeof(XiEntry), _Alignof(XiEntry));
        if (nv == NULL)
            return false;
        for (Int i = 0; i < xi_cache_cap; i++) {
            if (xi_cache[i].t == NULL)
                continue;
            uint64_t h = xi_ptr_hash(xi_cache[i].t) & (uint64_t)(nc - 1);
            while (nv[h].t != NULL)
                h = (h + 1) & (uint64_t)(nc - 1);
            nv[h] = xi_cache[i];
        }
        mem_free(heap_allocator(), xi_cache, (size_t)xi_cache_cap * sizeof(XiEntry),
                 _Alignof(XiEntry));
        xi_cache = nv;
        xi_cache_cap = nc;
    }
    uint64_t m = (uint64_t)(xi_cache_cap - 1);
    uint64_t h = xi_ptr_hash(t) & m;
    while (xi_cache[h].t != NULL)
        h = (h + 1) & m;
    xi_cache[h].t = t;
    xi_cache[h].ti = ti;
    xi_cache_len++;
    return true;
}

/* Called with xi_lock held, and again for each embedded struct on the way. */
static const XmlTypeInfo *xi_get_locked(const Type *t, Error *err) {
    const XmlTypeInfo *found = xi_cache_find(t);
    if (found != NULL)
        return found;
    Arena *ar = BURROW_NEW(heap_allocator(), Arena);
    if (ar == NULL) {
        xi_oom(err);
        return NULL;
    }
    arena_init(ar, NULL, 0);
    XiBuild b = {arena_allocator(ar), NULL, 0};
    b.ti = (XmlTypeInfo *)mem_alloc(b.a, sizeof(XmlTypeInfo), _Alignof(XmlTypeInfo));
    if (b.ti != NULL && xi_build(&b, t, err) && xi_cache_put(t, b.ti))
        return b.ti;
    /* Out of memory unless xi_build said what went wrong. */
    if (b.ti == NULL || BURROW_OK(*err))
        xi_oom(err);
    arena_free(ar);
    mem_free(heap_allocator(), ar, sizeof(Arena), _Alignof(Arena));
    return NULL;
}

const XmlTypeInfo *burrow__xml_type_info(const Type *t, Error *err) {
    burrow__lock(&xi_lock);
    const XmlTypeInfo *ti = xi_get_locked(t, err);
    burrow__unlock(&xi_lock);
    return ti;
}

void *burrow__xml_field_value(const XmlFieldInfo *f, const Type *t, void *p,
                              const Type **ft) {
    for (Int i = 0; i < f->nidx; i++) {
        if (i > 0 && t->kind == KIND_POINTER && t->elem->kind == KIND_STRUCT) {
            p = *(void **)p;
            if (p == NULL)
                return NULL;
            t = t->elem;
        }
        const Field *fd = &t->fields[f->idx[i]];
        p = (Byte *)p + fd->offset;
        t = fd->type;
    }
    *ft = t;
    return p;
}
