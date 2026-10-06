/* golang.org/x/net/dns/dnsmessage, the copy Go vendors.
 *
 * DNS messages as they go over the wire: a Parser that reads one a piece at a
 * time without allocating, a Builder that writes one a piece at a time, and a
 * Message that holds a whole one for Pack and Unpack. Go's pure resolver asks
 * its questions and reads its answers with this, and burrow's will too. It is
 * an internal of burrow's because it is an internal of Go's.
 *
 * Go's ResourceBody is an interface with a type for each kind of record. Here
 * a DnsmsgResourceBody is a tagged union of the same structs, and the kind
 * DNSMSG_BODY_NONE is Go's nil body. A Go slice is a pointer, a length and a
 * capacity, and so are the arrays here. Packing reads p and len and never
 * looks at cap.
 *
 * What a parse makes, the arrays and the bytes behind TXT strings, options,
 * SVCB values and unknown records, is in memory from the Alloc it was given,
 * as Go's parse makes copies. The _free functions give back what a parse made
 * and nothing else, so do not hand them something you put together yourself.
 * With an arena there is nothing to free.
 *
 * Errors are Go's. The sentinels below have Go's texts, and an error that says
 * where it happened is Go's nestedError, whose text is "where: what" and which
 * does not unwrap. Running out of memory, which Go cannot do, is
 * burrow_err_out_of_memory, and Go's Pack, which has a map to fill, is the
 * only place outside a parse that can.
 *
 * Message.Pack writes each record's type and length into its header, as Go's
 * does, so it takes the message by a pointer that is not const.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package xnet/dnsmessage */

#ifndef BURROW_SRC_XNET_DNSMESSAGE_H
#define BURROW_SRC_XNET_DNSMESSAGE_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------- codes */

/* Type, Class, OpCode, RCode and SVCParamKey. */
typedef uint16_t DnsmsgType;
typedef uint16_t DnsmsgClass;
typedef uint16_t DnsmsgOpCode;
typedef uint16_t DnsmsgRCode;
typedef uint16_t DnsmsgSVCParamKey;

enum {
    DNSMSG_TYPE_A = 1,
    DNSMSG_TYPE_NS = 2,
    DNSMSG_TYPE_CNAME = 5,
    DNSMSG_TYPE_SOA = 6,
    DNSMSG_TYPE_PTR = 12,
    DNSMSG_TYPE_MX = 15,
    DNSMSG_TYPE_TXT = 16,
    DNSMSG_TYPE_AAAA = 28,
    DNSMSG_TYPE_SRV = 33,
    DNSMSG_TYPE_OPT = 41,
    DNSMSG_TYPE_SVCB = 64,
    DNSMSG_TYPE_HTTPS = 65,
    /* Questions only. */
    DNSMSG_TYPE_WKS = 11,
    DNSMSG_TYPE_HINFO = 13,
    DNSMSG_TYPE_MINFO = 14,
    DNSMSG_TYPE_AXFR = 252,
    DNSMSG_TYPE_ALL = 255,
};

enum {
    DNSMSG_CLASS_INET = 1,
    DNSMSG_CLASS_CSNET = 2,
    DNSMSG_CLASS_CHAOS = 3,
    DNSMSG_CLASS_HESIOD = 4,
    /* Questions only. */
    DNSMSG_CLASS_ANY = 255,
};

enum {
    DNSMSG_RCODE_SUCCESS = 0,
    DNSMSG_RCODE_FORMAT_ERROR = 1,
    DNSMSG_RCODE_SERVER_FAILURE = 2,
    DNSMSG_RCODE_NAME_ERROR = 3,
    DNSMSG_RCODE_NOT_IMPLEMENTED = 4,
    DNSMSG_RCODE_REFUSED = 5,
};

enum {
    DNSMSG_SVC_PARAM_MANDATORY = 0,
    DNSMSG_SVC_PARAM_ALPN = 1,
    DNSMSG_SVC_PARAM_NO_DEFAULT_ALPN = 2,
    DNSMSG_SVC_PARAM_PORT = 3,
    DNSMSG_SVC_PARAM_IPV4_HINT = 4,
    DNSMSG_SVC_PARAM_ECH = 5,
    DNSMSG_SVC_PARAM_IPV6_HINT = 6,
    DNSMSG_SVC_PARAM_DOH_PATH = 7,
    DNSMSG_SVC_PARAM_OHTTP = 8,
    DNSMSG_SVC_PARAM_TLS_SUPPORTED_GROUPS = 9,
};

/* String for Type, Class, RCode and SVCParamKey: the name Go gives a known
 * code, which is static, or the code in decimal written into buf. */
BURROW_BORROWS(ret, buf) Str burrow__dnsmsg_type_string(DnsmsgType t, Byte buf[5]);
BURROW_BORROWS(ret, buf) Str burrow__dnsmsg_class_string(DnsmsgClass c, Byte buf[5]);
BURROW_BORROWS(ret, buf) Str burrow__dnsmsg_rcode_string(DnsmsgRCode r, Byte buf[5]);
BURROW_BORROWS(ret, buf) Str burrow__dnsmsg_svc_param_key_string(DnsmsgSVCParamKey k,
                                                                 Byte buf[5]);

/* Go's printPaddedUint8, printUint8Bytes and printUint32, for the tests. Each
 * writes into buf and returns how much. */
Int burrow__dnsmsg_print_padded_uint8(Byte buf[3], uint8_t i);
Int burrow__dnsmsg_print_uint8_bytes(Byte buf[3], uint8_t i);
Int burrow__dnsmsg_print_uint32(Byte buf[10], uint32_t i);

/* --------------------------------------------------------------- errors */

/* ErrNotStarted and ErrSectionDone, the two Go exports. */
extern const Error burrow__dnsmsg_err_not_started;
extern const Error burrow__dnsmsg_err_section_done;

/* The rest of Go's errors, which it keeps to itself and its tests check. */
extern const Error burrow__dnsmsg_err_base_len;
extern const Error burrow__dnsmsg_err_calc_len;
extern const Error burrow__dnsmsg_err_reserved;
extern const Error burrow__dnsmsg_err_too_many_ptr;
extern const Error burrow__dnsmsg_err_invalid_ptr;
extern const Error burrow__dnsmsg_err_invalid_name;
extern const Error burrow__dnsmsg_err_nil_resource_body;
extern const Error burrow__dnsmsg_err_resource_len;
extern const Error burrow__dnsmsg_err_seg_too_long;
extern const Error burrow__dnsmsg_err_name_too_long;
extern const Error burrow__dnsmsg_err_zero_seg_len;
extern const Error burrow__dnsmsg_err_res_too_long;
extern const Error burrow__dnsmsg_err_too_many_questions;
extern const Error burrow__dnsmsg_err_too_many_answers;
extern const Error burrow__dnsmsg_err_too_many_authorities;
extern const Error burrow__dnsmsg_err_too_many_additionals;
extern const Error burrow__dnsmsg_err_non_canonical_name;
extern const Error burrow__dnsmsg_err_string_too_long;
extern const Error burrow__dnsmsg_err_param_out_of_order;
extern const Error burrow__dnsmsg_err_too_long_svcb_value;

/* Whether err is a nestedError, and if it is, its s and the error it holds.
 * prefix and inner may be NULL. */
bool burrow__dnsmsg_error_nested(Error err, Str *prefix, Error *inner);

/* ---------------------------------------------------------------- names */

/* Name: up to 255 bytes of text, with a dot after every label. */
typedef struct DnsmsgName {
    Byte data[255];
    uint8_t length;
} DnsmsgName;

/* NewName and MustNewName. The second panics where the first fails, which is
 * when s is longer than 255 bytes. */
Error burrow__dnsmsg_new_name(Str s, DnsmsgName *out);
DnsmsgName burrow__dnsmsg_must_new_name(Str s);

/* Name.String. */
BURROW_BORROWS(ret, n) Str burrow__dnsmsg_name_string(const DnsmsgName *n);

/* ------------------------------------------------------------- the parts */

typedef struct DnsmsgHeader {
    uint16_t id;
    bool response;
    DnsmsgOpCode op_code;
    bool authoritative;
    bool truncated;
    bool recursion_desired;
    bool recursion_available;
    bool authentic_data;
    bool checking_disabled;
    DnsmsgRCode rcode;
} DnsmsgHeader;

typedef struct DnsmsgQuestion {
    DnsmsgName name;
    DnsmsgType type;
    DnsmsgClass class_;
} DnsmsgQuestion;

typedef struct DnsmsgResourceHeader {
    DnsmsgName name;
    DnsmsgType type;
    DnsmsgClass class_;
    uint32_t ttl;
    uint16_t length;
} DnsmsgResourceHeader;

/* ResourceHeader.SetEDNS0, which never fails, DNSSECAllowed and
 * ExtendedRCode. */
Error burrow__dnsmsg_resource_header_set_edns0(DnsmsgResourceHeader *h,
                                               int udp_payload_len,
                                               DnsmsgRCode ext_rcode, bool dnssec_ok);
bool burrow__dnsmsg_resource_header_dnssec_allowed(const DnsmsgResourceHeader *h);
DnsmsgRCode burrow__dnsmsg_resource_header_extended_rcode(const DnsmsgResourceHeader *h,
                                                          DnsmsgRCode rcode);

typedef struct DnsmsgAResource {
    Byte a[4];
} DnsmsgAResource;

typedef struct DnsmsgAAAAResource {
    Byte aaaa[16];
} DnsmsgAAAAResource;

typedef struct DnsmsgNSResource {
    DnsmsgName ns;
} DnsmsgNSResource;

typedef struct DnsmsgCNAMEResource {
    DnsmsgName cname;
} DnsmsgCNAMEResource;

typedef struct DnsmsgPTRResource {
    DnsmsgName ptr;
} DnsmsgPTRResource;

typedef struct DnsmsgMXResource {
    uint16_t pref;
    DnsmsgName mx;
} DnsmsgMXResource;

typedef struct DnsmsgSOAResource {
    DnsmsgName ns;
    DnsmsgName mbox;
    uint32_t serial;
    uint32_t refresh;
    uint32_t retry;
    uint32_t expire;
    uint32_t min_ttl;
} DnsmsgSOAResource;

typedef struct DnsmsgStrs {
    Str *p;
    Int len, cap;
} DnsmsgStrs;

typedef struct DnsmsgTXTResource {
    DnsmsgStrs txt;
} DnsmsgTXTResource;

typedef struct DnsmsgSRVResource {
    uint16_t priority;
    uint16_t weight;
    uint16_t port;
    DnsmsgName target; /* never compressed, as RFC 2782 says */
} DnsmsgSRVResource;

/* An EDNS(0) option. data is bytes. */
typedef struct DnsmsgOption {
    uint16_t code;
    Slice data;
} DnsmsgOption;

typedef struct DnsmsgOptions {
    DnsmsgOption *p;
    Int len, cap;
} DnsmsgOptions;

typedef struct DnsmsgOPTResource {
    DnsmsgOptions options;
} DnsmsgOPTResource;

/* value is bytes. */
typedef struct DnsmsgSVCParam {
    DnsmsgSVCParamKey key;
    Slice value;
} DnsmsgSVCParam;

typedef struct DnsmsgSVCParams {
    DnsmsgSVCParam *p;
    Int len, cap;
} DnsmsgSVCParams;

/* params is in strictly increasing order of key. */
typedef struct DnsmsgSVCBResource {
    uint16_t priority;
    DnsmsgName target;
    DnsmsgSVCParams params;
} DnsmsgSVCBResource;

typedef struct DnsmsgHTTPSResource {
    DnsmsgSVCBResource svcb;
} DnsmsgHTTPSResource;

/* data is bytes. */
typedef struct DnsmsgUnknownResource {
    DnsmsgType type;
    Slice data;
} DnsmsgUnknownResource;

/* SVCBResource.GetParam, SetParam and DeleteParam. GetParam's value borrows
 * from r. SetParam keeps value, not a copy, as Go keeps the slice, and like
 * Go's slices.Insert it makes a bigger array from a when the one there is full
 * and leaves the old one where it was. It returns false only when a has run
 * out. */
bool burrow__dnsmsg_svcb_get_param(const DnsmsgSVCBResource *r, DnsmsgSVCParamKey key,
                                   Slice *value);
bool burrow__dnsmsg_svcb_set_param(DnsmsgSVCBResource *r, Alloc *a,
                                   DnsmsgSVCParamKey key, Slice value);
bool burrow__dnsmsg_svcb_delete_param(DnsmsgSVCBResource *r, DnsmsgSVCParamKey key);

typedef enum DnsmsgBodyKind {
    DNSMSG_BODY_NONE,
    DNSMSG_BODY_A,
    DNSMSG_BODY_NS,
    DNSMSG_BODY_CNAME,
    DNSMSG_BODY_SOA,
    DNSMSG_BODY_PTR,
    DNSMSG_BODY_MX,
    DNSMSG_BODY_TXT,
    DNSMSG_BODY_AAAA,
    DNSMSG_BODY_SRV,
    DNSMSG_BODY_SVCB,
    DNSMSG_BODY_HTTPS,
    DNSMSG_BODY_OPT,
    DNSMSG_BODY_UNKNOWN,
} DnsmsgBodyKind;

/* ResourceBody. */
typedef struct DnsmsgResourceBody {
    DnsmsgBodyKind kind;
    union {
        DnsmsgAResource a;
        DnsmsgNSResource ns;
        DnsmsgCNAMEResource cname;
        DnsmsgSOAResource soa;
        DnsmsgPTRResource ptr;
        DnsmsgMXResource mx;
        DnsmsgTXTResource txt;
        DnsmsgAAAAResource aaaa;
        DnsmsgSRVResource srv;
        DnsmsgSVCBResource svcb;
        DnsmsgHTTPSResource https;
        DnsmsgOPTResource opt;
        DnsmsgUnknownResource unknown;
    } u;
} DnsmsgResourceBody;

typedef struct DnsmsgResource {
    DnsmsgResourceHeader header;
    DnsmsgResourceBody body;
} DnsmsgResource;

typedef struct DnsmsgQuestions {
    DnsmsgQuestion *p;
    Int len, cap;
} DnsmsgQuestions;

typedef struct DnsmsgResources {
    DnsmsgResource *p;
    Int len, cap;
} DnsmsgResources;

typedef struct DnsmsgMessage {
    DnsmsgHeader header;
    DnsmsgQuestions questions;
    DnsmsgResources answers;
    DnsmsgResources authorities;
    DnsmsgResources additionals;
} DnsmsgMessage;

/* Give back what a parse made. Each leaves its argument empty. */
void burrow__dnsmsg_txt_free(Alloc *a, DnsmsgTXTResource *r);
void burrow__dnsmsg_opt_free(Alloc *a, DnsmsgOPTResource *r);
void burrow__dnsmsg_svcb_free(Alloc *a, DnsmsgSVCBResource *r);
void burrow__dnsmsg_unknown_free(Alloc *a, DnsmsgUnknownResource *r);
void burrow__dnsmsg_body_free(Alloc *a, DnsmsgResourceBody *b);
void burrow__dnsmsg_questions_free(Alloc *a, DnsmsgQuestions *qs);
void burrow__dnsmsg_resources_free(Alloc *a, DnsmsgResources *rs);
void burrow__dnsmsg_message_free(Alloc *a, DnsmsgMessage *m);

/* GoString, which writes out the Go that would make the value, as Go's does.
 * The result is in memory from a, and empty when a runs out. */
BURROW_OWNS(ret) Str burrow__dnsmsg_message_go_string(Alloc *a, const DnsmsgMessage *m);
BURROW_OWNS(ret) Str burrow__dnsmsg_header_go_string(Alloc *a, const DnsmsgHeader *h);
BURROW_OWNS(ret) Str burrow__dnsmsg_question_go_string(Alloc *a,
                                                       const DnsmsgQuestion *q);
BURROW_OWNS(ret) Str burrow__dnsmsg_resource_go_string(Alloc *a,
                                                       const DnsmsgResource *r);
BURROW_OWNS(ret) Str
burrow__dnsmsg_resource_header_go_string(Alloc *a, const DnsmsgResourceHeader *h);
BURROW_OWNS(ret) Str burrow__dnsmsg_body_go_string(Alloc *a,
                                                   const DnsmsgResourceBody *b);
BURROW_OWNS(ret) Str burrow__dnsmsg_name_go_string(Alloc *a, const DnsmsgName *n);

/* -------------------------------------------------------------- packing */

/* What Go's append does to a []byte: the bytes, how many there are and how
 * many fit. When they stop fitting the bytes move to memory from a, which the
 * buffer then owns. */
typedef struct DnsmsgBuf {
    Alloc *a;
    Byte *p;
    Int len, cap;
    bool owned;
} DnsmsgBuf;

/* A buffer that starts as b, which it does not own. */
DnsmsgBuf burrow__dnsmsg_buf(Alloc *a, Slice b);

/* The bytes as a slice, which borrows from the buffer. */
BURROW_BORROWS(ret, b) Slice burrow__dnsmsg_buf_slice(const DnsmsgBuf *b);

/* Give back what the buffer owns. */
void burrow__dnsmsg_buf_free(DnsmsgBuf *b);

/* The map from a name's text to where it was written, which Go's
 * compression is. Every key is a copy, in memory of the map's own. */
typedef struct DnsmsgCompression DnsmsgCompression;

BURROW_OWNS(ret) DnsmsgCompression *burrow__dnsmsg_compression_new(Alloc *a);
void burrow__dnsmsg_compression_free(DnsmsgCompression *c);

/* Message.Pack and Message.AppendPack. out is in memory from a, or b's when
 * it has room. */
Error burrow__dnsmsg_message_pack(DnsmsgMessage *m, Alloc *a, Slice *out);
Error burrow__dnsmsg_message_append_pack(DnsmsgMessage *m, Alloc *a, Slice b,
                                         Slice *out);

/* Message.Unpack. m is filled in as far as it got, so free it either way. */
Error burrow__dnsmsg_message_unpack(DnsmsgMessage *m, Alloc *a, Slice msg);

/* The unexported pieces of packing and unpacking that Go's tests reach. A
 * pack appends to msg, and on an error msg is as it was, though the bytes
 * past its end may not be. c may be NULL, which is Go's nil map. */
Error burrow__dnsmsg_name_pack(const DnsmsgName *n, DnsmsgBuf *msg,
                               DnsmsgCompression *c, Int compression_off);
Error burrow__dnsmsg_name_unpack(DnsmsgName *n, Slice msg, Int off, Int *new_off);
Error burrow__dnsmsg_question_pack(const DnsmsgQuestion *q, DnsmsgBuf *msg,
                                   DnsmsgCompression *c, Int compression_off);
Error burrow__dnsmsg_resource_pack(DnsmsgResource *r, DnsmsgBuf *msg,
                                   DnsmsgCompression *c, Int compression_off);
Error burrow__dnsmsg_resource_header_pack(const DnsmsgResourceHeader *h, DnsmsgBuf *msg,
                                          DnsmsgCompression *c, Int compression_off,
                                          Int *len_off);
Error burrow__dnsmsg_resource_header_unpack(DnsmsgResourceHeader *h, Slice msg, Int off,
                                            Int *new_off);
Error burrow__dnsmsg_body_pack(const DnsmsgResourceBody *b, DnsmsgBuf *msg,
                               DnsmsgCompression *c, Int compression_off);
Error burrow__dnsmsg_unpack_resource_body(Alloc *a, Slice msg, Int off,
                                          DnsmsgResourceHeader hdr,
                                          DnsmsgResourceBody *out, Int *new_off);

/* -------------------------------------------------------------- parsing */

typedef enum DnsmsgSection {
    DNSMSG_SECTION_NOT_STARTED,
    DNSMSG_SECTION_HEADER,
    DNSMSG_SECTION_QUESTIONS,
    DNSMSG_SECTION_ANSWERS,
    DNSMSG_SECTION_AUTHORITIES,
    DNSMSG_SECTION_ADDITIONALS,
    DNSMSG_SECTION_DONE,
} DnsmsgSection;

/* The header as it is on the wire, Go's header. */
typedef struct DnsmsgWireHeader {
    uint16_t id;
    uint16_t bits;
    uint16_t questions;
    uint16_t answers;
    uint16_t authorities;
    uint16_t additionals;
} DnsmsgWireHeader;

/* header.unpack. */
Error burrow__dnsmsg_wire_header_unpack(DnsmsgWireHeader *h, Slice msg, Int off,
                                        Int *new_off);

/* Parser. The zero value is one that has not started. It borrows the message
 * it is given, which has to stay put until the parse is done. */
typedef struct DnsmsgParser {
    Slice msg;
    DnsmsgWireHeader header;
    DnsmsgSection section;
    Int off;
    Int index;
    bool res_header_valid;
    Int res_header_offset;
    DnsmsgType res_header_type;
    uint16_t res_header_length;
} DnsmsgParser;

Error burrow__dnsmsg_parser_start(DnsmsgParser *p, Slice msg, DnsmsgHeader *h);

Error burrow__dnsmsg_parser_question(DnsmsgParser *p, DnsmsgQuestion *q);
Error burrow__dnsmsg_parser_all_questions(DnsmsgParser *p, Alloc *a,
                                          DnsmsgQuestions *qs);
Error burrow__dnsmsg_parser_skip_question(DnsmsgParser *p);
Error burrow__dnsmsg_parser_skip_all_questions(DnsmsgParser *p);

Error burrow__dnsmsg_parser_answer_header(DnsmsgParser *p, DnsmsgResourceHeader *h);
Error burrow__dnsmsg_parser_answer(DnsmsgParser *p, Alloc *a, DnsmsgResource *r);
Error burrow__dnsmsg_parser_all_answers(DnsmsgParser *p, Alloc *a, DnsmsgResources *rs);
Error burrow__dnsmsg_parser_skip_answer(DnsmsgParser *p);
Error burrow__dnsmsg_parser_skip_all_answers(DnsmsgParser *p);

Error burrow__dnsmsg_parser_authority_header(DnsmsgParser *p, DnsmsgResourceHeader *h);
Error burrow__dnsmsg_parser_authority(DnsmsgParser *p, Alloc *a, DnsmsgResource *r);
Error burrow__dnsmsg_parser_all_authorities(DnsmsgParser *p, Alloc *a,
                                            DnsmsgResources *rs);
Error burrow__dnsmsg_parser_skip_authority(DnsmsgParser *p);
Error burrow__dnsmsg_parser_skip_all_authorities(DnsmsgParser *p);

Error burrow__dnsmsg_parser_additional_header(DnsmsgParser *p, DnsmsgResourceHeader *h);
Error burrow__dnsmsg_parser_additional(DnsmsgParser *p, Alloc *a, DnsmsgResource *r);
Error burrow__dnsmsg_parser_all_additionals(DnsmsgParser *p, Alloc *a,
                                            DnsmsgResources *rs);
Error burrow__dnsmsg_parser_skip_additional(DnsmsgParser *p);
Error burrow__dnsmsg_parser_skip_all_additionals(DnsmsgParser *p);

/* The body of the record whose header was the last thing read. Each fails
 * with ErrNotStarted when that record is of another type. */
Error burrow__dnsmsg_parser_cname_resource(DnsmsgParser *p, DnsmsgCNAMEResource *r);
Error burrow__dnsmsg_parser_mx_resource(DnsmsgParser *p, DnsmsgMXResource *r);
Error burrow__dnsmsg_parser_ns_resource(DnsmsgParser *p, DnsmsgNSResource *r);
Error burrow__dnsmsg_parser_ptr_resource(DnsmsgParser *p, DnsmsgPTRResource *r);
Error burrow__dnsmsg_parser_soa_resource(DnsmsgParser *p, DnsmsgSOAResource *r);
Error burrow__dnsmsg_parser_txt_resource(DnsmsgParser *p, Alloc *a,
                                         DnsmsgTXTResource *r);
Error burrow__dnsmsg_parser_srv_resource(DnsmsgParser *p, DnsmsgSRVResource *r);
Error burrow__dnsmsg_parser_a_resource(DnsmsgParser *p, DnsmsgAResource *r);
Error burrow__dnsmsg_parser_aaaa_resource(DnsmsgParser *p, DnsmsgAAAAResource *r);
Error burrow__dnsmsg_parser_opt_resource(DnsmsgParser *p, Alloc *a,
                                         DnsmsgOPTResource *r);
Error burrow__dnsmsg_parser_svcb_resource(DnsmsgParser *p, Alloc *a,
                                          DnsmsgSVCBResource *r);
Error burrow__dnsmsg_parser_https_resource(DnsmsgParser *p, Alloc *a,
                                           DnsmsgHTTPSResource *r);
/* Any type at all. */
Error burrow__dnsmsg_parser_unknown_resource(DnsmsgParser *p, Alloc *a,
                                             DnsmsgUnknownResource *r);

/* ------------------------------------------------------------- building */

/* Builder. buf is where the message starts, after whatever buf already has
 * in it, and the bytes move to memory from a when they outgrow it. The zero
 * value is one that has not started. */
typedef struct DnsmsgBuilder {
    DnsmsgBuf msg;
    DnsmsgSection section;
    DnsmsgWireHeader header;
    Int start;
    DnsmsgCompression *compression;
} DnsmsgBuilder;

/* NewBuilder. buf may be nil, and then the builder starts with room for 512
 * bytes, as Go's does. Fails only when a runs out. */
Error burrow__dnsmsg_new_builder(DnsmsgBuilder *b, Alloc *a, Slice buf, DnsmsgHeader h);

/* EnableCompression. Fails only when a runs out. */
Error burrow__dnsmsg_builder_enable_compression(DnsmsgBuilder *b);

Error burrow__dnsmsg_builder_start_questions(DnsmsgBuilder *b);
Error burrow__dnsmsg_builder_start_answers(DnsmsgBuilder *b);
Error burrow__dnsmsg_builder_start_authorities(DnsmsgBuilder *b);
Error burrow__dnsmsg_builder_start_additionals(DnsmsgBuilder *b);

Error burrow__dnsmsg_builder_question(DnsmsgBuilder *b, const DnsmsgQuestion *q);

Error burrow__dnsmsg_builder_cname_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                            const DnsmsgCNAMEResource *r);
Error burrow__dnsmsg_builder_mx_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                         const DnsmsgMXResource *r);
Error burrow__dnsmsg_builder_ns_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                         const DnsmsgNSResource *r);
Error burrow__dnsmsg_builder_ptr_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                          const DnsmsgPTRResource *r);
Error burrow__dnsmsg_builder_soa_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                          const DnsmsgSOAResource *r);
Error burrow__dnsmsg_builder_txt_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                          const DnsmsgTXTResource *r);
Error burrow__dnsmsg_builder_srv_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                          const DnsmsgSRVResource *r);
Error burrow__dnsmsg_builder_a_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                        const DnsmsgAResource *r);
Error burrow__dnsmsg_builder_aaaa_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                           const DnsmsgAAAAResource *r);
Error burrow__dnsmsg_builder_opt_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                          const DnsmsgOPTResource *r);
Error burrow__dnsmsg_builder_svcb_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                           const DnsmsgSVCBResource *r);
Error burrow__dnsmsg_builder_https_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                            const DnsmsgHTTPSResource *r);
Error burrow__dnsmsg_builder_unknown_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                              const DnsmsgUnknownResource *r);

/* Finish. The message is everything from where buf started, header and all,
 * and out is buf with it on the end. The bytes are the caller's from here on:
 * in buf's memory when they fit and in memory from a when they did not. */
Error burrow__dnsmsg_builder_finish(DnsmsgBuilder *b, Slice *out);

/* Give back what the builder holds, which after Finish is only the map. */
void burrow__dnsmsg_builder_free(DnsmsgBuilder *b);

#endif /* BURROW_SRC_XNET_DNSMESSAGE_H */
