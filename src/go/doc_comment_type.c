/* go/doc/comment: the type descriptors.
 *
 * One struct descriptor and one pointer descriptor for each node, with Go's
 * names for the types and the fields. A Block or a Text, which Go declares as
 * an interface, is a pointer to the header each node starts with. Plain and
 * Italic are strings in Go and nodes holding one here, so their descriptors
 * are structs with a Text field.
 *
 * Derived from Go's src/go/doc/comment/parse.go.
 * Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/doc/comment.h"

#include "burrow/core.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>

#define DCT_STR(s) {(const Byte *)(s), (Int)sizeof(s) - 1}
#define DCT_FIELD(T, go, c, ft)                                                        \
    {DCT_STR(go), {NULL, 0}, &(ft), (uint32_t)offsetof(T, c)}
#define DCT_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

#define DCT_STRUCT(T, go, fields)                                                      \
    const Type burrow_type_##T = {                                                     \
        DCT_STR(go),                                                                   \
        DCT_STR("go/doc/comment"),                                                     \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        DCT_COUNT(fields),                                                             \
        0,                                                                             \
        fields,                                                                        \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

#define DCT_PTR(T)                                                                     \
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

#define DCT_SLICE(name, elem)                                                          \
    static const Type name = {                                                         \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        KIND_SLICE,                                                                    \
        (uint32_t)sizeof(Slice),                                                       \
        (uint16_t)_Alignof(Slice),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        elem,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* The header every node starts with, which a Block or a Text points at. */
static const Field dct_fields_base[] = {
    {DCT_STR("Kind"),
     {NULL, 0},
     &burrow_type_Int,
     (uint32_t)offsetof(CommentBase, kind)},
};

static const Type dct_base_type = {
    DCT_STR("node"),
    DCT_STR("go/doc/comment"),
    KIND_STRUCT,
    (uint32_t)sizeof(CommentBase),
    (uint16_t)_Alignof(CommentBase),
    DCT_COUNT(dct_fields_base),
    0,
    dct_fields_base,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

#define DCT_IFACE(T, go)                                                               \
    const Type burrow_type_##T = {                                                     \
        DCT_STR(go),                                                                   \
        DCT_STR("go/doc/comment"),                                                     \
        KIND_POINTER,                                                                  \
        (uint32_t)sizeof(CommentBase *),                                               \
        (uint16_t)_Alignof(CommentBase *),                                             \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        &dct_base_type,                                                                \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

DCT_IFACE(CommentBlock, "Block");
DCT_IFACE(CommentText, "Text");

DCT_SLICE(dct_slice_block, &burrow_type_CommentBlock);
DCT_SLICE(dct_slice_text, &burrow_type_CommentText);
DCT_SLICE(dct_slice_link_def, &burrow_type_CommentLinkDefPtr);
DCT_SLICE(dct_slice_list_item, &burrow_type_CommentListItemPtr);

static const Field dct_fields_doc[] = {
    DCT_FIELD(CommentDoc, "Content", content, dct_slice_block),
    DCT_FIELD(CommentDoc, "Links", links, dct_slice_link_def),
};
DCT_STRUCT(CommentDoc, "Doc", dct_fields_doc);
DCT_PTR(CommentDoc);

static const Field dct_fields_link_def[] = {
    DCT_FIELD(CommentLinkDef, "Text", text, burrow_type_Str),
    DCT_FIELD(CommentLinkDef, "URL", url, burrow_type_Str),
    DCT_FIELD(CommentLinkDef, "Used", used, burrow_type_bool),
};
DCT_STRUCT(CommentLinkDef, "LinkDef", dct_fields_link_def);
DCT_PTR(CommentLinkDef);

static const Field dct_fields_heading[] = {
    DCT_FIELD(CommentHeading, "Text", text, dct_slice_text),
};
DCT_STRUCT(CommentHeading, "Heading", dct_fields_heading);
DCT_PTR(CommentHeading);

static const Field dct_fields_list[] = {
    DCT_FIELD(CommentList, "Items", items, dct_slice_list_item),
    DCT_FIELD(CommentList, "ForceBlankBefore", force_blank_before, burrow_type_bool),
    DCT_FIELD(CommentList, "ForceBlankBetween", force_blank_between, burrow_type_bool),
};
DCT_STRUCT(CommentList, "List", dct_fields_list);
DCT_PTR(CommentList);

static const Field dct_fields_list_item[] = {
    DCT_FIELD(CommentListItem, "Number", number, burrow_type_Str),
    DCT_FIELD(CommentListItem, "Content", content, dct_slice_block),
};
DCT_STRUCT(CommentListItem, "ListItem", dct_fields_list_item);
DCT_PTR(CommentListItem);

static const Field dct_fields_paragraph[] = {
    DCT_FIELD(CommentParagraph, "Text", text, dct_slice_text),
};
DCT_STRUCT(CommentParagraph, "Paragraph", dct_fields_paragraph);
DCT_PTR(CommentParagraph);

static const Field dct_fields_code[] = {
    DCT_FIELD(CommentCode, "Text", text, burrow_type_Str),
};
DCT_STRUCT(CommentCode, "Code", dct_fields_code);
DCT_PTR(CommentCode);

static const Field dct_fields_plain[] = {
    DCT_FIELD(CommentPlain, "Text", text, burrow_type_Str),
};
DCT_STRUCT(CommentPlain, "Plain", dct_fields_plain);
DCT_PTR(CommentPlain);

static const Field dct_fields_italic[] = {
    DCT_FIELD(CommentItalic, "Text", text, burrow_type_Str),
};
DCT_STRUCT(CommentItalic, "Italic", dct_fields_italic);
DCT_PTR(CommentItalic);

static const Field dct_fields_link[] = {
    DCT_FIELD(CommentLink, "Auto", auto_, burrow_type_bool),
    DCT_FIELD(CommentLink, "Text", text, dct_slice_text),
    DCT_FIELD(CommentLink, "URL", url, burrow_type_Str),
};
DCT_STRUCT(CommentLink, "Link", dct_fields_link);
DCT_PTR(CommentLink);

static const Field dct_fields_doc_link[] = {
    DCT_FIELD(CommentDocLink, "Text", text, dct_slice_text),
    DCT_FIELD(CommentDocLink, "ImportPath", import_path, burrow_type_Str),
    DCT_FIELD(CommentDocLink, "Recv", recv, burrow_type_Str),
    DCT_FIELD(CommentDocLink, "Name", name, burrow_type_Str),
};
DCT_STRUCT(CommentDocLink, "DocLink", dct_fields_doc_link);
DCT_PTR(CommentDocLink);
