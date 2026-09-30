/* encoding/xml: an XML 1.0 parser and writer that understands namespaces.
 *
 * This is the token layer of Go's encoding/xml. A decoder reads a document
 * one token at a time, start tags, end tags, text, comments, processing
 * instructions and directives, and checks as it goes that the tags nest. An
 * encoder writes tokens back out, escaping text and checking the same thing.
 *
 * Go's Token is an interface holding one of six types. Here it is XmlToken, a
 * tagged union with one member for each, and a zeroed XmlToken is Go's nil.
 *
 * A token from a decoder points into the decoder, its names, its attributes
 * and its text alike, and is good until the next call that reads from it.
 * xml_copy_token and the _copy functions make one that is not.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/xml */

/* Reading every start tag in a document:
 *
 *     XmlDecoder *d = xml_new_decoder(a, r);
 *     for (;;) {
 *         Error err = BURROW_NO_ERROR;
 *         XmlToken tok = xml_decoder_token(d, &err);
 *         if (errors_is(err, io_eof))
 *             break;
 *         if (BURROW_FAILED(err))
 *             ...;
 *         if (tok.kind == XML_START_ELEMENT)
 *             ... tok.start.name.local ...;
 *     }
 *     xml_decoder_free(d);
 *
 * and writing some:
 *
 *     XmlEncoder *e = xml_new_encoder(a, w);
 *     XmlStartElement start = {{BURROW_S(""), BURROW_S("greeting")}, {0}};
 *     xml_encoder_encode_token(e, (XmlToken){XML_START_ELEMENT, .start = start});
 *     xml_encoder_encode_token(e, (XmlToken){XML_CHAR_DATA, .char_data = text});
 *     xml_encoder_encode_token(e, (XmlToken){XML_END_ELEMENT, .end = {start.name}});
 *     xml_encoder_close(e);
 *     xml_encoder_free(e); */

#ifndef BURROW_ENCODING_XML_H
#define BURROW_ENCODING_XML_H

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* xml.Header, the declaration to put at the top of a document, newline and
 * all. */
#define XML_HEADER "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"

/* ------------------------------------------------------------------ tokens */

/* xml.Name. space is the namespace's URL once a decoder has translated it,
 * and the prefix before that, as RawToken leaves it. */
typedef struct XmlName {
    Str space;
    Str local;
} XmlName;

extern const Type burrow_type_XmlName;

/* xml.Attr. */
typedef struct XmlAttr {
    XmlName name;
    Str value;
} XmlAttr;

extern const Type burrow_type_XmlAttr;

/* xml.StartElement. attr is a Slice of XmlAttr. */
typedef struct XmlStartElement {
    XmlName name;
    Slice attr;
} XmlStartElement;

extern const Type burrow_type_XmlStartElement;

/* xml.EndElement. */
typedef struct XmlEndElement {
    XmlName name;
} XmlEndElement;

extern const Type burrow_type_XmlEndElement;

/* xml.CharData, text with its entities replaced, as a Slice of Byte. */
typedef Slice XmlCharData;

/* xml.Comment, what is between <!-- and -->. */
typedef Slice XmlComment;

/* xml.ProcInst, <?target inst?>. */
typedef struct XmlProcInst {
    Str target;
    Slice inst;
} XmlProcInst;

extern const Type burrow_type_XmlProcInst;

/* xml.Directive, what is between <! and >, such as a DOCTYPE. A directive
 * that holds comments has each one replaced by a space. */
typedef Slice XmlDirective;

/* Which member of an XmlToken is in use. XML_TOKEN_NONE is Go's nil. */
typedef enum XmlTokenKind {
    XML_TOKEN_NONE = 0,
    XML_START_ELEMENT,
    XML_END_ELEMENT,
    XML_CHAR_DATA,
    XML_COMMENT,
    XML_PROC_INST,
    XML_DIRECTIVE,
} XmlTokenKind;

/* xml.Token. */
typedef struct XmlToken {
    XmlTokenKind kind;
    union {
        XmlStartElement start;
        XmlEndElement end;
        XmlCharData char_data;
        XmlComment comment;
        XmlProcInst proc_inst;
        XmlDirective directive;
    };
} XmlToken;

/* StartElement.End, the end tag that goes with e. */
XmlEndElement xml_start_element_end(XmlStartElement e);

/* The Copy methods. Go's copy only the bytes, since its strings can't change
 * under it. These copy the strings too, so the copy owes nothing to where the
 * original came from. Each is one block from a, which xml_token_free gives
 * back, or which goes with an arena. */
BURROW_OWNS(ret) XmlStartElement xml_start_element_copy(XmlStartElement e, Alloc *a);
BURROW_OWNS(ret) XmlCharData xml_char_data_copy(XmlCharData c, Alloc *a);
BURROW_OWNS(ret) XmlComment xml_comment_copy(XmlComment c, Alloc *a);
BURROW_OWNS(ret) XmlProcInst xml_proc_inst_copy(XmlProcInst p, Alloc *a);
BURROW_OWNS(ret) XmlDirective xml_directive_copy(XmlDirective d, Alloc *a);

/* xml.CopyToken, the right one of the above for t's kind. An end element's
 * name is copied as well. */
BURROW_OWNS(ret) XmlToken xml_copy_token(Alloc *a, XmlToken t);

/* Gives back a token from xml_copy_token, or anything from the _copy
 * functions put into a token of the matching kind, to the a it came from. */
void xml_token_free(Alloc *a, XmlToken t);

/* ------------------------------------------------------------------ errors */

/* xml.SyntaxError. line is where the problem was found. */
typedef struct XmlSyntaxError {
    Str msg;
    Int line;
} XmlSyntaxError;

extern const Type *const TYPE_XML_SYNTAX_ERROR;

/* Go's message, built in a: XML syntax error on line 3: unexpected EOF */
BURROW_OWNS(ret) Str xml_syntax_error_error(const XmlSyntaxError *e, Alloc *a);

/* e as an Error, with its msg copied, built in a. errors_as with
 * TYPE_XML_SYNTAX_ERROR hands back the XmlSyntaxError. */
BURROW_OWNS(ret) Error xml_syntax_error_as_error(Alloc *a, const XmlSyntaxError *e);

/* xml.TagPathError: two fields of a struct whose tags put them at the same
 * path. */
typedef struct XmlTagPathError {
    const Type *struct_type;
    Str field1, tag1;
    Str field2, tag2;
} XmlTagPathError;

extern const Type *const TYPE_XML_TAG_PATH_ERROR;

/* Go's message, built in a: xml.S field "A" with tag "x>y" conflicts with
 * field "B" with tag "x" */
BURROW_OWNS(ret) Str xml_tag_path_error_error(const XmlTagPathError *e, Alloc *a);

/* xml.UnsupportedTypeError: a value marshal has no way to write, such as a
 * map or a channel. */
typedef struct XmlUnsupportedTypeError {
    const Type *type;
} XmlUnsupportedTypeError;

extern const Type *const TYPE_XML_UNSUPPORTED_TYPE_ERROR;

/* Go's message, built in a: xml: unsupported type: map[string]string */
BURROW_OWNS(ret) Str xml_unsupported_type_error_error(const XmlUnsupportedTypeError *e,
                                                      Alloc *a);

/* --------------------------------------------------------------- decoding */

/* xml.TokenReader, anything that hands out tokens one at a time. A decoder
 * can read from one of these instead of from bytes, and checks and
 * translates what it is given the same way. */
typedef struct XmlTokenReaderVT {
    const Type *self_type;
    XmlToken (*token)(void *self, Error *err);
} XmlTokenReaderVT;

typedef struct XmlTokenReader {
    const XmlTokenReaderVT *vt;
    void *data;
} XmlTokenReader;

/* TokenReader.Token. */
XmlToken xml_token_reader_token(XmlTokenReader r, Error *err);

/* Decoder.CharsetReader. Given the charset a document declares and the rest
 * of the document, it returns a reader of the same document in UTF-8. */
BURROW_FUNC(XmlCharsetReader, IoReader, Str charset, IoReader input, Error *err);

typedef struct XmlStack XmlStack;

/* xml.Decoder. The first group of fields are Go's, set them after making the
 * decoder and before reading from it. The rest are the decoder's own.
 *
 * strict, true to begin with, holds a document to the XML specification.
 * Turned off, the decoder takes an attribute with no value or no quotes, an
 * entity it doesn't know, which it leaves as it is, and a start tag with no
 * end tag, which it closes when its parent closes.
 *
 * auto_close, a Slice of Str, names elements that close themselves straight
 * after they open when strict is off, as xml_html_auto_close does for HTML.
 *
 * entity maps entity names to their text, Str to Str, beyond the five XML
 * has. xml_html_entity is one for HTML. The decoder only reads it.
 *
 * charset_reader, when set, is called for a document that says it is in
 * something other than UTF-8. Without it such a document is an error.
 *
 * default_space is the namespace of names that have none. */
typedef struct XmlDecoder {
    bool strict;
    Slice auto_close;
    Map *entity;
    XmlCharsetReader charset_reader;
    Str default_space;

    Alloc *a;
    IoReader src;                  /* what charset_reader is handed */
    BufioReader *direct;           /* bytes come from here, */
    struct BytesReader *bytes_src; /* or here, */
    struct StringsReader *str_src; /* or here, */
    const Method *read_byte;       /* or from src's ReadByte */
    BufioReader **owned;           /* the readers made for sources without one */
    Int nowned, capowned;
    XmlTokenReader t;
    Byte *buf; /* Go's d.buf */
    Int blen, bcap;
    XmlAttr *attrs; /* the attributes of the last start tag */
    Int nattrs, capattrs;
    Arena tok;      /* the strings of the last token */
    Arena saved[2]; /* next_token lives in saved[cur] */
    int cur;
    Arena names; /* every namespace prefix seen, once each */
    Map *prefixes;
    XmlStack *stk;
    Int stk_depth;
    XmlStack *free;
    bool need_close;
    XmlName to_close;
    Byte *to_close_buf;
    Int to_close_cap;
    XmlToken next_token;
    int next_byte;
    Map *ns; /* prefix to URL, the URLs from a */
    Error err;
    Int line;
    int64_t linestart;
    int64_t offset;
    bool oom; /* an allocation failed during the current call */
    /* What Unmarshal keeps. inner is Go's d.saved, every byte read while
     * keep_inner is on, for innerxml fields. scratch holds the start tags
     * and text it needs across tokens and is emptied when the outermost
     * Decode returns. */
    bool in_unmarshal_xml;
    bool keep_inner;
    Byte *inner;
    Int ninner, capinner;
    Arena scratch;
    Int unmarshal_depth;
} XmlDecoder;

extern const Type *const TYPE_XML_DECODER;

/* xml.NewDecoder. A decoder of r, which it reads ahead of what it has
 * parsed. NULL when a refuses. */
BURROW_OWNS(ret) XmlDecoder *xml_new_decoder(Alloc *a, IoReader r);

/* xml.NewTokenDecoder. A decoder of the tokens t hands out. When t is a
 * decoder's own, from xml_decoder_as_token_reader, that decoder comes back
 * and nothing new is made, so free it once. */
BURROW_OWNS(ret) XmlDecoder *xml_new_token_decoder(Alloc *a, XmlTokenReader t);

/* Frees a decoder and everything it holds. The last token it handed out goes
 * with it. */
void xml_decoder_free(XmlDecoder *d);

/* The decoder as a TokenReader, whose Token is xml_decoder_token. */
XmlTokenReader xml_decoder_as_token_reader(XmlDecoder *d);

/* Decoder.Token. The next token, with namespace prefixes replaced by their
 * URLs and every end tag checked against its start tag. A tag written <a/>
 * comes back as a start and then an end. At the end of the input it is a
 * zeroed token and io_eof, or a syntax error if an element is still open. */
XmlToken xml_decoder_token(XmlDecoder *d, Error *err);

/* Decoder.RawToken. Token without the checks or the translation, so names
 * keep their prefixes and end tags need not match. */
XmlToken xml_decoder_raw_token(XmlDecoder *d, Error *err);

/* Decoder.Skip. Reads tokens until the end of the element whose start tag
 * was the last one read, nested elements and all. */
BURROW_BORROWS(ret) Error xml_decoder_skip(XmlDecoder *d);

/* Decoder.InputOffset, how many bytes of the input the tokens so far used. */
int64_t xml_decoder_input_offset(const XmlDecoder *d);

/* Decoder.InputPos. Returns the line, from 1, where the next token starts,
 * and stores the column, also from 1 and counted in bytes, in column when it
 * is not NULL. */
Int xml_decoder_input_pos(const XmlDecoder *d, Int *column);

/* xml.HTMLEntity, the entities HTML has, for a decoder's entity. It is made
 * the first time it is asked for and shared from then on, so don't change or
 * free it. */
BURROW_STATIC(ret) Map *xml_html_entity(void);

/* xml.HTMLAutoClose, the HTML elements that have no end tag, as a Slice of
 * Str, for a decoder's auto_close. */
extern const Slice xml_html_auto_close;

/* --------------------------------------------------------------- encoding */

/* xml.Encoder. Opaque. */
typedef struct XmlEncoder XmlEncoder;

extern const Type *const TYPE_XML_ENCODER;

/* xml.NewEncoder. An encoder that writes to w through a buffer, which
 * xml_encoder_flush and xml_encoder_close empty. NULL when a refuses. */
BURROW_OWNS(ret) XmlEncoder *xml_new_encoder(Alloc *a, IoWriter w);

/* Frees an encoder without flushing it. */
void xml_encoder_free(XmlEncoder *e);

/* Encoder.Indent. Each element goes on its own line, after prefix and one
 * indent for each element it is inside. Both are copied. */
void xml_encoder_indent(XmlEncoder *e, Str prefix, Str indent);

/* Encoder.EncodeToken. Writes t, escaping text and attribute values. A start
 * tag's namespace becomes an xmlns attribute, and an attribute's a prefix
 * declared as it is first used. An end tag has to match the start tag it
 * closes, a comment can't hold -->, and the rest is checked as Go checks it.
 * The output is buffered, so call xml_encoder_flush when done. */
BURROW_BORROWS(ret) Error xml_encoder_encode_token(XmlEncoder *e, XmlToken t);

/* Encoder.Flush, which writes what is buffered to w. */
BURROW_BORROWS(ret) Error xml_encoder_flush(XmlEncoder *e);

/* Encoder.Close. Flushes, and reports an element left open. Anything written
 * after it is an error. */
BURROW_BORROWS(ret) Error xml_encoder_close(XmlEncoder *e);

/* -------------------------------------------------------------- marshaling
 *
 * Marshal writes a value as XML by its type's descriptor, the way Go's
 * reflection does it. A struct is an element named by its XMLName field, or
 * by the name it is marshaled under, or else by its type's name, and each
 * exported field goes inside it, as an attribute, as text, as a comment or as
 * an element of its own, as the field's xml tag says:
 *
 *     BURROW_SLICE_TYPE(Strs, Str);
 *
 *     #define PERSON_FIELDS(F, T)                              \
 *         F(T, XmlName, XMLName, "xml:\"person\"")             \
 *         F(T, Int, Id, "xml:\"id,attr\"")                     \
 *         F(T, Str, FirstName, "xml:\"name>first\"")           \
 *         F(T, Str, LastName, "xml:\"name>last\"")             \
 *         F(T, Strs, Email, "xml:\"email\"")
 *     BURROW_STRUCT(Person, PERSON_FIELDS);
 *
 * gives <person id="13"><name><first>John</first><last>Doe</last></name>
 * <email>a@example.com</email></person>, in one line. The tag rules, the
 * omitempty flag, the handling of pointers and interfaces and the errors are
 * all Go's.
 *
 * A type can write itself with a MarshalXML method, declared with
 * XML_SIG_MARSHAL_XML, an attribute with a MarshalXMLAttr method declared
 * with XML_SIG_MARSHAL_XML_ATTR, and text with encoding's MarshalText. Every
 * method here takes a pointer receiver, so a method on T is found for a T and
 * for a *T alike, where Go would only find one with a pointer receiver on a
 * value it could take the address of. */

/* The argument types in the signatures below. */
typedef XmlEncoder *XmlEncoderArg;
extern const Type burrow_type_XmlEncoderArg;

/* Error marshal_xml(T *self, XmlEncoder *e, XmlStartElement start). It writes
 * itself with xml_encoder_encode_token and xml_encoder_encode_element, and
 * has to close every element it opens. */
#define XML_SIG_MARSHAL_XML(IN, OUT)                                                   \
    IN(0, XmlEncoderArg) IN(1, XmlStartElement) OUT(Error)

/* XmlAttr marshal_xml_attr(T *self, Alloc *a, XmlName name, Error *err). An
 * attribute with an empty local name is left out. */
#define XML_SIG_MARSHAL_XML_ATTR(IN, OUT)                                              \
    IN(0, EncodingAllocArg) IN(1, XmlName) IN(2, EncodingErrorArg) OUT(XmlAttr)

/* xml.Marshaler and xml.MarshalerAttr, for code that wants to hold one.
 * Marshal goes by the methods the value's type lists, so the vtables are for
 * your code and not for it. */
typedef struct XmlMarshalerVT {
    const Type *self_type;
    Error (*marshal_xml)(void *self, XmlEncoder *e, XmlStartElement start);
} XmlMarshalerVT;

typedef struct XmlMarshaler {
    const XmlMarshalerVT *vt;
    void *data;
} XmlMarshaler;

typedef struct XmlMarshalerAttrVT {
    const Type *self_type;
    XmlAttr (*marshal_xml_attr)(void *self, Alloc *a, XmlName name, Error *err);
} XmlMarshalerAttrVT;

typedef struct XmlMarshalerAttr {
    const XmlMarshalerAttrVT *vt;
    void *data;
} XmlMarshalerAttr;

extern const Type burrow_type_XmlMarshaler;
extern const Type burrow_type_XmlMarshalerAttr;

/* xml.Marshal. The XML for v, in a, or the nil slice and an error. */
BURROW_OWNS(ret) Slice xml_marshal(Alloc *a, Any v, Error *err);

/* xml.MarshalIndent: xml_marshal with each element on its own line, after
 * prefix and one indent for each element it is inside. */
BURROW_OWNS(ret) Slice xml_marshal_indent(Alloc *a, Any v, Str prefix, Str indent,
                                          Error *err);

/* Encoder.Encode. Writes v as xml_marshal does, then flushes. */
BURROW_BORROWS(ret) Error xml_encoder_encode(XmlEncoder *e, Any v);

/* Encoder.EncodeElement. Writes v with start as its outermost element, which
 * is how a MarshalXML method writes a value inside the element it was
 * given. */
BURROW_BORROWS(ret) Error xml_encoder_encode_element(XmlEncoder *e, Any v,
                                                     XmlStartElement start);

/* ------------------------------------------------------------ unmarshaling
 *
 * Unmarshal is Marshal run backwards. It reads one element into the value v
 * points at, with the same tag rules: a struct field gets the attribute, the
 * text, the comments, the inner XML or the child elements its tag names, a
 * slice field gets one more element for each match, a nil pointer is
 * allocated, and an XMLName field is checked against the element's name and
 * set to it. Anything the value has no place for is skipped. With the Person
 * above,
 *
 *     Person p = {0};
 *     Error err = xml_unmarshal(a, data, BURROW_ANY(TYPE_OF(Person), &p));
 *
 * fills in p from data. Every string, slice and pointer Unmarshal stores is
 * allocated from a, so an arena is the natural thing to give it. For
 * xml_decoder_decode it is the decoder's allocator.
 *
 * A type can read itself with an UnmarshalXML method, declared with
 * XML_SIG_UNMARSHAL_XML, an attribute with UnmarshalXMLAttr, declared with
 * XML_SIG_UNMARSHAL_XML_ATTR, and text with encoding's UnmarshalText. As with
 * marshaling, a method is found on T for a T and a *T alike.
 *
 * Unmarshal recurses once for each element it reads into a value, and stops
 * with "exceeded max depth" past 10,000, which is Go's limit. Go's stack grows
 * to fit and a goroutine's here does not, so a program that reads documents
 * nested thousands deep should read them on a goroutine started with
 * go_stack and a few megabytes. Elements that are skipped cost no stack. */

typedef XmlDecoder *XmlDecoderArg;
extern const Type burrow_type_XmlDecoderArg;

/* Error unmarshal_xml(T *self, XmlDecoder *d, XmlStartElement start). It
 * reads exactly one element, the one start opened, with xml_decoder_token or
 * xml_decoder_decode_element, and must not use xml_decoder_raw_token. Anything
 * it keeps has to be allocated, from d->a or elsewhere, since start and the
 * tokens are only good until it returns. */
#define XML_SIG_UNMARSHAL_XML(IN, OUT)                                                 \
    IN(0, XmlDecoderArg) IN(1, XmlStartElement) OUT(Error)

/* Error unmarshal_xml_attr(T *self, Alloc *a, XmlAttr attr). attr is only
 * good until it returns, so it copies what it keeps, from a. */
#define XML_SIG_UNMARSHAL_XML_ATTR(IN, OUT)                                            \
    IN(0, EncodingAllocArg) IN(1, XmlAttr) OUT(Error)

/* xml.Unmarshaler and xml.UnmarshalerAttr, for code that wants to hold one.
 * Like the marshaling ones, Unmarshal goes by the methods a type lists. */
typedef struct XmlUnmarshalerVT {
    const Type *self_type;
    Error (*unmarshal_xml)(void *self, XmlDecoder *d, XmlStartElement start);
} XmlUnmarshalerVT;

typedef struct XmlUnmarshaler {
    const XmlUnmarshalerVT *vt;
    void *data;
} XmlUnmarshaler;

typedef struct XmlUnmarshalerAttrVT {
    const Type *self_type;
    Error (*unmarshal_xml_attr)(void *self, Alloc *a, XmlAttr attr);
} XmlUnmarshalerAttrVT;

typedef struct XmlUnmarshalerAttr {
    const XmlUnmarshalerAttrVT *vt;
    void *data;
} XmlUnmarshalerAttr;

extern const Type burrow_type_XmlUnmarshaler;
extern const Type burrow_type_XmlUnmarshalerAttr;

/* xml.UnmarshalError, which is a string in Go: an element that did not
 * match the XMLName field of the struct it was read into. */
typedef Str XmlUnmarshalError;

extern const Type *const TYPE_XML_UNMARSHAL_ERROR;

/* The message, which is e itself, copied into a. */
BURROW_OWNS(ret) Str xml_unmarshal_error_error(XmlUnmarshalError e, Alloc *a);

/* xml.Unmarshal. Reads the first element in data into the value v points
 * at. An Any with no type is Go's non-pointer case and one with a type and
 * no data its nil pointer; both are errors. */
BURROW_OWNS(v) BURROW_STATIC(ret) Error xml_unmarshal(Alloc *a, Slice data, Any v);

/* Decoder.Decode. Reads tokens up to the next start element and unmarshals
 * that element into v, allocating from d->a. */
BURROW_OWNS(v) BURROW_STATIC(ret) Error xml_decoder_decode(XmlDecoder *d, Any v);

/* Decoder.DecodeElement. Unmarshals the element start opened, which the
 * caller has already read, into v. A NULL start is xml_decoder_decode. */
BURROW_OWNS(v) BURROW_STATIC(ret) Error
xml_decoder_decode_element(XmlDecoder *d, Any v, const XmlStartElement *start);

/* ---------------------------------------------------------------- escaping */

/* xml.EscapeText. Writes s to w with <, >, &, ', ", tab, newline and
 * carriage return written as character references, and anything that isn't
 * a valid XML character written as U+FFFD. */
BURROW_BORROWS(ret) Error xml_escape_text(IoWriter w, Slice s);

/* xml.Escape, EscapeText without the error, which Go keeps for old code. */
void xml_escape(IoWriter w, Slice s);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_XML_H */
