/* What xml.c shares with xml_marshal.c and lets its tests see.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_ENCODING_XML_INTERNAL_H
#define BURROW_SRC_ENCODING_XML_INTERNAL_H

#include "burrow/encoding/xml.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"

/* Go's isInCharacterRange. */
bool burrow__xml_is_in_character_range(Rune r);

/* Go's procInst: the value of param in the text of an xml declaration, such
 * as 1.0 for version, or empty. It points into s. */
Str burrow__xml_proc_inst(Str param, Str s);
bool burrow__xml_is_valid_directive(Slice dir);

/* Go's isName. */
bool burrow__xml_is_name(Str s);

/* pushEOF and popEOF, which fence off the element an UnmarshalXML method
 * is given. */
bool burrow__xml_dec_push_eof(XmlDecoder *d);
bool burrow__xml_dec_pop_eof(XmlDecoder *d);

/* ---------------------------------------------------------------- encoding */

/* Where escaped text goes: an IoWriter for EscapeText, the encoder's buffer
 * for the encoder. */
typedef Error (*XmlPut)(void *ctx, const Byte *p, Int n);

/* A prefix createAttrPrefix made, and the URL it stands for, in one block.
 * An entry with no block is markPrefix's mark. */
typedef struct XmlPrefix {
    Byte *block;
    Str prefix;
    Str url;
} XmlPrefix;

typedef struct XmlTag {
    Int off;
    Int space_len;
    Int local_len;
} XmlTag;

struct XmlEncoder {
    Alloc *a;
    BufioWriter *w;
    Int seq;
    Str indent;
    Str prefix;
    Int depth;
    bool indented_in;
    bool put_newline;
    Map *attr_ns;     /* prefix to URL */
    Map *attr_prefix; /* URL to prefix */
    XmlPrefix *prefixes;
    Int nprefixes, capprefixes;
    /* The open tags, a stack of offsets into tag_bytes, where each one's
     * space and then its local name are kept. */
    XmlTag *tags;
    Int ntags, captags;
    Byte *tag_bytes;
    Int ntag_bytes, captag_bytes;
    bool closed;
    Error err;
    /* Scratch space for Encode and EncodeElement: attribute lists, the text
     * of numbers and what MarshalText and MarshalXMLAttr return. It is reset
     * when the outermost call returns. */
    Arena scratch;
    Int marshal_depth;
};

/* escapeText, with escape_newline true for EscapeText and EscapeString. */
Error burrow__xml_escape_to(XmlPut put, void *ctx, const Byte *s, Int len,
                            bool escape_newline);

/* An XmlPut into an encoder's buffer, with ctx the encoder. */
Error burrow__xml_put_encoder(void *ctx, const Byte *p, Int n);

/* The printer's Write, WriteString and WriteByte, which keep the first write
 * error in e->err and do nothing after it. */
void burrow__xml_enc_write(XmlEncoder *e, const Byte *p, Int n);
void burrow__xml_enc_write_str(XmlEncoder *e, Str s);
void burrow__xml_enc_write_byte(XmlEncoder *e, Byte c);
Error burrow__xml_enc_cached_write_error(XmlEncoder *e);

/* EscapeString, writeIndent, writeStart and writeEnd. */
void burrow__xml_enc_escape_string(XmlEncoder *e, Str s);
void burrow__xml_enc_write_indent(XmlEncoder *e, int depth_delta);
Error burrow__xml_enc_write_start(XmlEncoder *e, const XmlStartElement *start);
Error burrow__xml_enc_write_end(XmlEncoder *e, XmlName name);

/* Open tag i, pointing into tag_bytes, so good until the next push. An
 * empty local name is the mark marshalInterface leaves. */
XmlName burrow__xml_enc_tag(const XmlEncoder *e, Int i);

/* Pushes the empty tag marshalInterface puts down before calling MarshalXML,
 * which writeEnd will not close. */
Error burrow__xml_enc_push_mark(XmlEncoder *e);

/* ---------------------------------------------------------------- typeinfo */

/* Go's fieldFlags. */
enum {
    XF_ELEMENT = 1,
    XF_ATTR = 2,
    XF_CDATA = 4,
    XF_CHARDATA = 8,
    XF_INNERXML = 16,
    XF_COMMENT = 32,
    XF_ANY = 64,
    XF_OMITEMPTY = 128,
    XF_MODE = XF_ELEMENT | XF_ATTR | XF_CDATA | XF_CHARDATA | XF_INNERXML | XF_COMMENT |
              XF_ANY
};

/* Go's fieldInfo. idx is the path of field numbers from the struct down,
 * more than one long for a field of an embedded struct. parents are the
 * names before the last > in a tag like a>b>c. */
typedef struct XmlFieldInfo {
    const Int *idx;
    Int nidx;
    Str name;
    Str xmlns;
    int flags;
    const Str *parents;
    Int nparents;
} XmlFieldInfo;

/* Go's typeInfo, built once for each type and kept. */
typedef struct XmlTypeInfo {
    const XmlFieldInfo *xmlname;
    XmlFieldInfo *fields;
    Int nfields;
} XmlTypeInfo;

/* getTypeInfo. A type whose tags are wrong gets an error, built again each
 * time as Go builds it, and nothing is kept for it. */
const XmlTypeInfo *burrow__xml_type_info(const Type *t, Error *err);

/* finfo.value(v, dontInitNilPointers): where the field is inside the struct
 * of type t at p, and its type in *ft. NULL when a nil pointer to an embedded
 * struct is on the way. */
void *burrow__xml_field_value(const XmlFieldInfo *f, const Type *t, void *p,
                              const Type **ft);

/* ---------------------------------------------------------------- unmarshal */

/* Go's errUnmarshalDepth: exceeded max depth. */
extern const Error burrow__xml_err_unmarshal_depth;

/* What xml_decoder_raw_token returns inside an UnmarshalXML method. */
extern const Error burrow__xml_err_raw_token;

#endif /* BURROW_SRC_ENCODING_XML_INTERNAL_H */
