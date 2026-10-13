/* go/doc: the type descriptors.
 *
 * One struct descriptor and one pointer descriptor for each of the package's
 * types, with Go's names for the types and the fields. Value.order is in Go's
 * struct too, unexported, and so is the pair of maps at the end of Package.
 *
 * Derived from Go's src/go/doc/doc.go and example.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/doc.h"

#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>

#define GDT_STR(s) {(const Byte *)(s), (Int)sizeof(s) - 1}
#define GDT_FIELD(T, go, c, ft)                                                        \
    {GDT_STR(go), {NULL, 0}, &(ft), (uint32_t)offsetof(T, c)}
#define GDT_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

#define GDT_STRUCT(T, go, fields)                                                      \
    const Type burrow_type_##T = {                                                     \
        GDT_STR(go),                                                                   \
        GDT_STR("go/doc"),                                                             \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        GDT_COUNT(fields),                                                             \
        0,                                                                             \
        fields,                                                                        \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

#define GDT_PTR(T)                                                                     \
    const Type burrow_type_##T##Ptr = {                                                \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        KIND_POINTER,                                                                  \
        (uint32_t)sizeof(void *),                                                      \
        (uint16_t)_Alignof(void *),                                                    \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        TYPE_OF(T),                                                                    \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

#define GDT_UNNAMED(name, kind, size, align, elem, key)                                \
    static const Type name = {                                                         \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        kind,                                                                          \
        (uint32_t)(size),                                                              \
        (uint16_t)(align),                                                             \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        elem,                                                                          \
        key,                                                                           \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

#define GDT_SLICE(name, elem)                                                          \
    GDT_UNNAMED(name, KIND_SLICE, sizeof(Slice), _Alignof(Slice), elem, NULL)

const Type burrow_type_DocMode = {
    GDT_STR("Mode"),
    GDT_STR("go/doc"),
    KIND_INT,
    (uint32_t)sizeof(DocMode),
    (uint16_t)_Alignof(DocMode),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

GDT_SLICE(gdt_slice_str, &burrow_type_Str);
GDT_SLICE(gdt_slice_comment_group, &burrow_type_AstCommentGroupPtr);
GDT_SLICE(gdt_slice_example, &burrow_type_DocExamplePtr);
GDT_SLICE(gdt_slice_value, &burrow_type_DocValuePtr);
GDT_SLICE(gdt_slice_func, &burrow_type_DocFuncPtr);
GDT_SLICE(gdt_slice_type, &burrow_type_DocTypePtr);
GDT_UNNAMED(gdt_map_notes, KIND_MAP, sizeof(Map *), _Alignof(Map *),
            &burrow_type_DocNoteSlice, &burrow_type_Str);
GDT_UNNAMED(gdt_map_str, KIND_MAP, sizeof(Map *), _Alignof(Map *), &burrow_type_Str,
            &burrow_type_Str);
GDT_UNNAMED(gdt_map_bool, KIND_MAP, sizeof(Map *), _Alignof(Map *), &burrow_type_bool,
            &burrow_type_Str);

const Type burrow_type_DocNoteSlice = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_DocNotePtr,
    NULL,
    0,
    0,
    NULL,
};

static const Field gdt_fields_example[] = {
    GDT_FIELD(DocExample, "Name", name, burrow_type_Str),
    GDT_FIELD(DocExample, "Suffix", suffix, burrow_type_Str),
    GDT_FIELD(DocExample, "Doc", doc, burrow_type_Str),
    GDT_FIELD(DocExample, "Code", code, burrow_type_AstNode),
    GDT_FIELD(DocExample, "Play", play, burrow_type_AstFilePtr),
    GDT_FIELD(DocExample, "Comments", comments, gdt_slice_comment_group),
    GDT_FIELD(DocExample, "Output", output, burrow_type_Str),
    GDT_FIELD(DocExample, "Unordered", unordered, burrow_type_bool),
    GDT_FIELD(DocExample, "EmptyOutput", empty_output, burrow_type_bool),
    GDT_FIELD(DocExample, "Order", order, burrow_type_Int),
};
GDT_STRUCT(DocExample, "Example", gdt_fields_example);
GDT_PTR(DocExample);

static const Field gdt_fields_value[] = {
    GDT_FIELD(DocValue, "Doc", doc, burrow_type_Str),
    GDT_FIELD(DocValue, "Names", names, gdt_slice_str),
    GDT_FIELD(DocValue, "Decl", decl, burrow_type_AstGenDeclPtr),
    GDT_FIELD(DocValue, "order", order, burrow_type_Int),
};
GDT_STRUCT(DocValue, "Value", gdt_fields_value);
GDT_PTR(DocValue);

static const Field gdt_fields_func[] = {
    GDT_FIELD(DocFunc, "Doc", doc, burrow_type_Str),
    GDT_FIELD(DocFunc, "Name", name, burrow_type_Str),
    GDT_FIELD(DocFunc, "Decl", decl, burrow_type_AstFuncDeclPtr),
    GDT_FIELD(DocFunc, "Recv", recv, burrow_type_Str),
    GDT_FIELD(DocFunc, "Orig", orig, burrow_type_Str),
    GDT_FIELD(DocFunc, "Level", level, burrow_type_Int),
    GDT_FIELD(DocFunc, "Examples", examples, gdt_slice_example),
};
GDT_STRUCT(DocFunc, "Func", gdt_fields_func);
GDT_PTR(DocFunc);

static const Field gdt_fields_type[] = {
    GDT_FIELD(DocType, "Doc", doc, burrow_type_Str),
    GDT_FIELD(DocType, "Name", name, burrow_type_Str),
    GDT_FIELD(DocType, "Decl", decl, burrow_type_AstGenDeclPtr),
    GDT_FIELD(DocType, "Consts", consts, gdt_slice_value),
    GDT_FIELD(DocType, "Vars", vars, gdt_slice_value),
    GDT_FIELD(DocType, "Funcs", funcs, gdt_slice_func),
    GDT_FIELD(DocType, "Methods", methods, gdt_slice_func),
    GDT_FIELD(DocType, "Examples", examples, gdt_slice_example),
};
GDT_STRUCT(DocType, "Type", gdt_fields_type);
GDT_PTR(DocType);

static const Field gdt_fields_note[] = {
    GDT_FIELD(DocNote, "Pos", pos, burrow_type_TokenPos),
    GDT_FIELD(DocNote, "End", end, burrow_type_TokenPos),
    GDT_FIELD(DocNote, "UID", uid, burrow_type_Str),
    GDT_FIELD(DocNote, "Body", body, burrow_type_Str),
};
GDT_STRUCT(DocNote, "Note", gdt_fields_note);
GDT_PTR(DocNote);

static const Field gdt_fields_package[] = {
    GDT_FIELD(DocPackage, "Doc", doc, burrow_type_Str),
    GDT_FIELD(DocPackage, "Name", name, burrow_type_Str),
    GDT_FIELD(DocPackage, "ImportPath", import_path, burrow_type_Str),
    GDT_FIELD(DocPackage, "Imports", imports, gdt_slice_str),
    GDT_FIELD(DocPackage, "Filenames", filenames, gdt_slice_str),
    GDT_FIELD(DocPackage, "Notes", notes, gdt_map_notes),
    GDT_FIELD(DocPackage, "Bugs", bugs, gdt_slice_str),
    GDT_FIELD(DocPackage, "Consts", consts, gdt_slice_value),
    GDT_FIELD(DocPackage, "Types", types, gdt_slice_type),
    GDT_FIELD(DocPackage, "Vars", vars, gdt_slice_value),
    GDT_FIELD(DocPackage, "Funcs", funcs, gdt_slice_func),
    GDT_FIELD(DocPackage, "Examples", examples, gdt_slice_example),
    GDT_FIELD(DocPackage, "importByName", import_by_name, gdt_map_str),
    GDT_FIELD(DocPackage, "syms", syms, gdt_map_bool),
};
GDT_STRUCT(DocPackage, "Package", gdt_fields_package);
GDT_PTR(DocPackage);
