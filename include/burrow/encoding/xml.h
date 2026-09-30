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
    IoReader src;            /* what charset_reader is handed */
    BufioReader *direct;     /* bytes come from here, */
    const Method *read_byte; /* or from src's ReadByte */
    BufioReader **owned;     /* the readers made for sources without one */
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
