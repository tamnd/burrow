/* net/http/internal/http2, the HTTP/2 that net/http is built on.
 *
 * This much is the frame layer: frame.go, errors.go and the part of http2.go
 * that the frames need. A Framer reads frames from an IoReader and writes them
 * to an IoWriter, one Write per frame, and knows nothing about streams or
 * connections beyond the order HEADERS and CONTINUATION frames have to come
 * in. With read_meta_headers set it puts a HEADERS frame and the
 * CONTINUATION frames after it back together and decodes the header block
 * with HPACK.
 *
 * Go's Frame is an interface with a type for each kind of frame. Here an
 * Http2Frame is a tagged union of the same structs, with the FrameHeader they
 * all embed at the front and kind saying which one it is. Go's frames are the
 * collector's, so a frame read without SetReuseFrames is still there after
 * the next read, and only the bytes it points into are gone. That is how it
 * works here too: a frame from read_frame is the caller's, to give back with
 * burrow__http2_frame_free, and its byte fields are only good until the next
 * read, as Go documents. The accessors check, and panic as Go's do. A DATA
 * frame read with SetReuseFrames on is the Framer's and is the same frame
 * every time, and freeing it does nothing. Give back every frame before the
 * Framer it came from.
 *
 * A MetaHeadersFrame owns copies of its fields, in memory from the Framer's
 * Alloc, and frame_free gives them back. A PriorityUpdateFrame owns its
 * Priority, which Go copies too.
 *
 * Errors are Go's. ConnectionError and StreamError are values in Go and
 * errors here, and burrow__http2_error_connection and
 * burrow__http2_error_stream are the type assertions. errors_is matches a
 * ConnectionError against another with the same code. The errors a read
 * makes are in the error arena, as burrow's are, and ErrorDetail is the
 * Framer's and lasts until the next read. Running out of memory, which Go
 * cannot do, is burrow_err_out_of_memory.
 *
 * GODEBUG=http2debug=1 and http2debug=2 turn on the logging Go's does, to
 * standard error in the format of Go's log package.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/internal/http2 */

#ifndef BURROW_SRC_NET_HTTP2_H
#define BURROW_SRC_NET_HTTP2_H

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include "../xnet/hpack.h"

#include <stdbool.h>
#include <stdint.h>

/* How much room the _string functions below that take a buffer need. */
enum { HTTP2_STRING_MAX = 32 };

/* ---------------------------------------------------------------- errors */

/* ErrCode, an HTTP/2 error code from RFC 7540 section 7. */
typedef uint32_t Http2ErrCode;

enum {
    HTTP2_ERR_CODE_NO = 0x0,
    HTTP2_ERR_CODE_PROTOCOL = 0x1,
    HTTP2_ERR_CODE_INTERNAL = 0x2,
    HTTP2_ERR_CODE_FLOW_CONTROL = 0x3,
    HTTP2_ERR_CODE_SETTINGS_TIMEOUT = 0x4,
    HTTP2_ERR_CODE_STREAM_CLOSED = 0x5,
    HTTP2_ERR_CODE_FRAME_SIZE = 0x6,
    HTTP2_ERR_CODE_REFUSED_STREAM = 0x7,
    HTTP2_ERR_CODE_CANCEL = 0x8,
    HTTP2_ERR_CODE_COMPRESSION = 0x9,
    HTTP2_ERR_CODE_CONNECT = 0xa,
    HTTP2_ERR_CODE_ENHANCE_YOUR_CALM = 0xb,
    HTTP2_ERR_CODE_INADEQUATE_SECURITY = 0xc,
    HTTP2_ERR_CODE_HTTP_1_1_REQUIRED = 0xd,
};

/* String, such as "PROTOCOL_ERROR", or "unknown error code 0xf" for a code
 * with no name. The result is static or in buf. */
BURROW_BORROWS(ret, buf) Str burrow__http2_err_code_string(Http2ErrCode e,
                                                           Byte buf[HTTP2_STRING_MAX]);

/* stringToken, which is String but "ERR_UNKNOWN_15" for a code with no
 * name. */
BURROW_BORROWS(ret, buf) Str
burrow__http2_err_code_string_token(Http2ErrCode e, Byte buf[HTTP2_STRING_MAX]);

/* ConnectionError(code), which reads "connection error: " and the code's
 * String. One with a named code is a constant, and the others are in the
 * error arena. */
Error burrow__http2_connection_error(Http2ErrCode code);

/* Whether err is a ConnectionError, and its code. */
bool burrow__http2_error_connection(Error err, Http2ErrCode *code);

/* StreamError. cause may be BURROW_NO_ERROR. */
typedef struct Http2StreamError {
    uint32_t stream_id;
    Http2ErrCode code;
    Error cause;
} Http2StreamError;

/* StreamError{id, code, cause} as an error, in the error arena. It reads
 * "stream error: stream ID 1; PROTOCOL_ERROR", with "; " and the cause after
 * it when there is one, and it does not unwrap, as in Go. */
Error burrow__http2_stream_error(uint32_t id, Http2ErrCode code, Error cause);

/* Whether err is a StreamError, and what is in it. */
bool burrow__http2_error_stream(Error err, Http2StreamError *se);

/* errFromPeer and goAwayFlowError. */
extern const Error burrow__http2_err_from_peer;
extern const Error burrow__http2_err_go_away_flow;

/* connError, which a frame parser gives back and read_frame turns into a
 * ConnectionError with the reason in ErrorDetail. It reads "http2: connection
 * error: " and then the code and the reason. In the error arena. */
Error burrow__http2_conn_error(Http2ErrCode code, Str reason);
bool burrow__http2_error_conn(Error err, Http2ErrCode *code, Str *reason);

/* pseudoHeaderError, duplicatePseudoHeaderError, headerFieldNameError and
 * headerFieldValueError, each holding a header name. In the error arena. */
Error burrow__http2_pseudo_header_error(Str name);
Error burrow__http2_duplicate_pseudo_header_error(Str name);
Error burrow__http2_header_field_name_error(Str name);
Error burrow__http2_header_field_value_error(Str name);

/* errMixPseudoHeaderTypes and errPseudoAfterRegular. */
extern const Error burrow__http2_err_mix_pseudo_header_types;
extern const Error burrow__http2_err_pseudo_after_regular;

/* ErrFrameTooLarge. */
extern const Error burrow__http2_err_frame_too_large;

/* errStreamID, errDepStreamID, errPadLength and errPadBytes, which the write
 * functions give back. */
extern const Error burrow__http2_err_stream_id;
extern const Error burrow__http2_err_dep_stream_id;
extern const Error burrow__http2_err_pad_length;
extern const Error burrow__http2_err_pad_bytes;

/* -------------------------------------------------------------- settings */

/* SettingID, a SETTINGS parameter from RFC 7540 section 6.5.2 and later. */
typedef uint16_t Http2SettingID;

enum {
    HTTP2_SETTING_HEADER_TABLE_SIZE = 0x1,
    HTTP2_SETTING_ENABLE_PUSH = 0x2,
    HTTP2_SETTING_MAX_CONCURRENT_STREAMS = 0x3,
    HTTP2_SETTING_INITIAL_WINDOW_SIZE = 0x4,
    HTTP2_SETTING_MAX_FRAME_SIZE = 0x5,
    HTTP2_SETTING_MAX_HEADER_LIST_SIZE = 0x6,
    HTTP2_SETTING_ENABLE_CONNECT_PROTOCOL = 0x8,
    HTTP2_SETTING_NO_RFC7540_PRIORITIES = 0x9,
};

/* String, such as "MAX_FRAME_SIZE", or "UNKNOWN_SETTING_7". */
BURROW_BORROWS(ret, buf) Str
burrow__http2_setting_id_string(Http2SettingID s, Byte buf[HTTP2_STRING_MAX]);

/* Setting, one parameter and its value. */
typedef struct Http2Setting {
    Http2SettingID id;
    uint32_t val;
} Http2Setting;

/* String, which is "[MAX_FRAME_SIZE = 16384]". */
BURROW_OWNS(ret) Str burrow__http2_setting_string(Alloc *a, Http2Setting s);

/* Valid: no error, or the ConnectionError a peer that sent s has made. */
Error burrow__http2_setting_valid(Http2Setting s);

/* validWireHeaderFieldName: a token with no upper case letters in it. */
bool burrow__http2_valid_wire_header_field_name(Str v);

/* ClientPreface, what a client sends before its first frame. */
#define HTTP2_CLIENT_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"

enum {
    /* initialHeaderTableSize. */
    HTTP2_INITIAL_HEADER_TABLE_SIZE = 4096,
    /* frameHeaderLen. */
    HTTP2_FRAME_HEADER_LEN = 9,
    /* minMaxFrameSize and maxFrameSize. */
    HTTP2_MIN_MAX_FRAME_SIZE = 1 << 14,
    HTTP2_MAX_FRAME_SIZE = (1 << 24) - 1,
};

/* ----------------------------------------------------------- frame types */

/* FrameType. */
typedef uint8_t Http2FrameType;

enum {
    HTTP2_FRAME_DATA = 0x0,
    HTTP2_FRAME_HEADERS = 0x1,
    HTTP2_FRAME_PRIORITY = 0x2,
    HTTP2_FRAME_RST_STREAM = 0x3,
    HTTP2_FRAME_SETTINGS = 0x4,
    HTTP2_FRAME_PUSH_PROMISE = 0x5,
    HTTP2_FRAME_PING = 0x6,
    HTTP2_FRAME_GO_AWAY = 0x7,
    HTTP2_FRAME_WINDOW_UPDATE = 0x8,
    HTTP2_FRAME_CONTINUATION = 0x9,
    HTTP2_FRAME_PRIORITY_UPDATE = 0x10,
};

/* String, such as "DATA", or "UNKNOWN_FRAME_TYPE_32". As in Go, a type in the
 * gap between CONTINUATION and PRIORITY_UPDATE is the empty string. */
BURROW_BORROWS(ret, buf) Str
burrow__http2_frame_type_string(Http2FrameType t, Byte buf[HTTP2_STRING_MAX]);

/* Flags, whose meaning depends on the frame type. */
typedef uint8_t Http2Flags;

enum {
    HTTP2_FLAG_DATA_END_STREAM = 0x1,
    HTTP2_FLAG_DATA_PADDED = 0x8,

    HTTP2_FLAG_HEADERS_END_STREAM = 0x1,
    HTTP2_FLAG_HEADERS_END_HEADERS = 0x4,
    HTTP2_FLAG_HEADERS_PADDED = 0x8,
    HTTP2_FLAG_HEADERS_PRIORITY = 0x20,

    HTTP2_FLAG_SETTINGS_ACK = 0x1,

    HTTP2_FLAG_PING_ACK = 0x1,

    HTTP2_FLAG_CONTINUATION_END_HEADERS = 0x4,

    HTTP2_FLAG_PUSH_PROMISE_END_HEADERS = 0x4,
    HTTP2_FLAG_PUSH_PROMISE_PADDED = 0x8,
};

/* Has: whether f has every flag in v. */
bool burrow__http2_flags_has(Http2Flags f, Http2Flags v);

/* FrameHeader, the nine bytes in front of every frame. valid is whether the
 * frame's bytes may still be looked at. */
typedef struct Http2FrameHeader {
    bool valid;
    Http2FrameType type;
    Http2Flags flags;
    uint32_t length;
    uint32_t stream_id;
} Http2FrameHeader;

/* String, which is "[FrameHeader DATA flags=END_STREAM stream=1 len=3]". */
BURROW_OWNS(ret) Str burrow__http2_frame_header_string(Alloc *a, Http2FrameHeader h);

/* ReadFrameHeader: the nine bytes from r. Most callers want read_frame. */
Http2FrameHeader burrow__http2_read_frame_header(IoReader r, Error *err);

/* ---------------------------------------------------------------- frames */

/* PriorityParam. The first three are RFC 7540's and the last two RFC 9218's,
 * which Go does not export. incremental is 0 or 1. */
typedef struct Http2PriorityParam {
    uint32_t stream_dep;
    bool exclusive;
    uint8_t weight;
    uint8_t urgency;
    uint8_t incremental;
} Http2PriorityParam;

bool burrow__http2_priority_param_is_zero(Http2PriorityParam p);
bool burrow__http2_priority_param_equal(Http2PriorityParam a, Http2PriorityParam b);

/* defaultRFC9218Priority: urgency 3, and incremental unless can_use_default,
 * which is what Go does so that streams keep being written round robin. */
Http2PriorityParam burrow__http2_default_rfc9218_priority(bool can_use_default);

/* parseRFC9218Priority: the u and i of a Priority header value. A value that
 * is not a dictionary gives the default and false. */
bool burrow__http2_parse_rfc9218_priority(Str s, bool can_use_default,
                                          Http2PriorityParam *p);

/* Which struct in Http2Frame's union is in use. */
typedef enum Http2FrameKind {
    HTTP2_DATA_FRAME,
    HTTP2_HEADERS_FRAME,
    HTTP2_PRIORITY_FRAME,
    HTTP2_RST_STREAM_FRAME,
    HTTP2_SETTINGS_FRAME,
    HTTP2_PUSH_PROMISE_FRAME,
    HTTP2_PING_FRAME,
    HTTP2_GO_AWAY_FRAME,
    HTTP2_WINDOW_UPDATE_FRAME,
    HTTP2_CONTINUATION_FRAME,
    HTTP2_PRIORITY_UPDATE_FRAME,
    HTTP2_UNKNOWN_FRAME,
    HTTP2_META_HEADERS_FRAME,
} Http2FrameKind;

/* The fields of each of Go's frame types, without the FrameHeader. The Slices
 * point into the Framer's read buffer. */
typedef struct Http2DataFrame {
    Slice data;
} Http2DataFrame;

typedef struct Http2HeadersFrame {
    /* Set when the frame has HTTP2_FLAG_HEADERS_PRIORITY. */
    Http2PriorityParam priority;
    Slice header_frag_buf;
} Http2HeadersFrame;

typedef struct Http2PriorityFrame {
    Http2PriorityParam priority;
} Http2PriorityFrame;

typedef struct Http2RSTStreamFrame {
    Http2ErrCode err_code;
} Http2RSTStreamFrame;

typedef struct Http2SettingsFrame {
    Slice p;
} Http2SettingsFrame;

typedef struct Http2PushPromiseFrame {
    uint32_t promise_id;
    Slice header_frag_buf;
} Http2PushPromiseFrame;

typedef struct Http2PingFrame {
    Byte data[8];
} Http2PingFrame;

typedef struct Http2GoAwayFrame {
    uint32_t last_stream_id;
    Http2ErrCode err_code;
    Slice debug_data;
} Http2GoAwayFrame;

typedef struct Http2WindowUpdateFrame {
    /* Never read with the high bit set. */
    uint32_t increment;
} Http2WindowUpdateFrame;

typedef struct Http2ContinuationFrame {
    Slice header_frag_buf;
} Http2ContinuationFrame;

typedef struct Http2PriorityUpdateFrame {
    /* The frame's own copy. */
    Str priority;
    uint32_t prioritized_stream_id;
} Http2PriorityUpdateFrame;

typedef struct Http2UnknownFrame {
    Slice p;
} Http2UnknownFrame;

/* MetaHeadersFrame: a HEADERS frame and the CONTINUATION frames after it,
 * decoded. The HEADERS frame's header is the frame's header, and is not valid,
 * since the fragments are gone. The fields are in HTTP/2's order and every
 * name and value in them has been checked, but a required pseudo header may
 * be missing. truncated is whether MaxHeaderListSize or MaxHeaderValueCount
 * cut the list short. */
typedef struct Http2MetaHeadersFrame {
    Http2HeadersFrame headers;
    HpackHeaderFields fields;
    bool truncated;
} Http2MetaHeadersFrame;

typedef struct Http2Framer Http2Framer;

typedef struct Http2Frame {
    Http2FrameHeader header;
    Http2FrameKind kind;
    /* The Framer that read the frame, which frame_free tells, and its Alloc.
     * Both NULL for a frame a parser filled in for someone else. */
    Http2Framer *framer;
    Alloc *a;
    /* The Framer's reused DATA frame, which frame_free leaves alone. */
    bool cached;
    union {
        Http2DataFrame data;
        Http2HeadersFrame headers;
        Http2PriorityFrame priority;
        Http2RSTStreamFrame rst_stream;
        Http2SettingsFrame settings;
        Http2PushPromiseFrame push_promise;
        Http2PingFrame ping;
        Http2GoAwayFrame go_away;
        Http2WindowUpdateFrame window_update;
        Http2ContinuationFrame continuation;
        Http2PriorityUpdateFrame priority_update;
        Http2UnknownFrame unknown;
        Http2MetaHeadersFrame meta_headers;
    } u;
} Http2Frame;

/* Gives back a frame from read_frame. NULL does nothing. */
void burrow__http2_frame_free(Http2Frame *f);

/* The methods. The ones that hand out bytes panic when the frame is no
 * longer valid, as Go's do. */
BURROW_BORROWS(ret, f) Slice burrow__http2_data_frame_data(const Http2Frame *f);
bool burrow__http2_data_frame_stream_ended(const Http2Frame *f);

BURROW_BORROWS(ret, f) Slice
burrow__http2_headers_frame_header_block_fragment(const Http2Frame *f);
bool burrow__http2_headers_frame_headers_ended(const Http2Frame *f);
bool burrow__http2_headers_frame_stream_ended(const Http2Frame *f);
bool burrow__http2_headers_frame_has_priority(const Http2Frame *f);

bool burrow__http2_settings_frame_is_ack(const Http2Frame *f);
bool burrow__http2_settings_frame_value(const Http2Frame *f, Http2SettingID id,
                                        uint32_t *v);
/* Setting: the setting at index i, from 0 to num_settings less one. */
Http2Setting burrow__http2_settings_frame_setting(const Http2Frame *f, Int i);
Int burrow__http2_settings_frame_num_settings(const Http2Frame *f);
bool burrow__http2_settings_frame_has_duplicates(const Http2Frame *f);

/* ForeachSetting: fn for each setting, stopping at the first error, which it
 * gives back. */
BURROW_FUNC(Http2SettingFunc, Error, Http2Setting s);
Error burrow__http2_settings_frame_foreach_setting(const Http2Frame *f,
                                                   Http2SettingFunc fn);

bool burrow__http2_ping_frame_is_ack(const Http2Frame *f);

BURROW_BORROWS(ret, f) Slice
burrow__http2_go_away_frame_debug_data(const Http2Frame *f);

BURROW_BORROWS(ret, f) Slice burrow__http2_unknown_frame_payload(const Http2Frame *f);

BURROW_BORROWS(ret, f) Slice
burrow__http2_continuation_frame_header_block_fragment(const Http2Frame *f);
bool burrow__http2_continuation_frame_headers_ended(const Http2Frame *f);

BURROW_BORROWS(ret, f) Slice
burrow__http2_push_promise_frame_header_block_fragment(const Http2Frame *f);
bool burrow__http2_push_promise_frame_headers_ended(const Http2Frame *f);

/* PseudoValue: the value of the pseudo header named pseudo, without its
 * colon, or the empty string. */
BURROW_BORROWS(ret, f) Str
burrow__http2_meta_headers_frame_pseudo_value(const Http2Frame *f, Str pseudo);

/* RegularFields and PseudoFields: the fields after the pseudo headers, and
 * the pseudo headers. Both point into the frame's own array. */
BURROW_BORROWS(ret, f) HpackHeaderFields
burrow__http2_meta_headers_frame_regular_fields(const Http2Frame *f);
BURROW_BORROWS(ret, f) HpackHeaderFields
burrow__http2_meta_headers_frame_pseudo_fields(const Http2Frame *f);

/* rfc9218Priority: the priority the frame's Priority header asks for, or the
 * default when it has none or one that does not parse. A request with a
 * Priority header makes the peer priority aware from then on. One with Via,
 * Forwarded or X-Forwarded-For came through an intermediary, and gets the
 * round robin default. */
Http2PriorityParam burrow__http2_meta_headers_frame_rfc9218_priority(
    const Http2Frame *f, bool priority_aware, bool *priority_aware_after,
    bool *has_intermediary);

/* checkPseudos: whether the pseudo headers are all known, none twice, and
 * not a mix of a request's and a response's. */
Error burrow__http2_meta_headers_frame_check_pseudos(const Http2Frame *f);

/* summarizeFrame, the text the debug logging prints for a frame. */
BURROW_OWNS(ret) Str burrow__http2_summarize_frame(Alloc *a, const Http2Frame *f);

/* frameParser and typeFrameParser. A parser fills in f from the header and
 * payload, with f's bytes pointing into payload. f comes zeroed apart from
 * framer and a, and a PRIORITY_UPDATE frame's Priority is copied with f->a, or
 * points into payload too when f->a is NULL. count_error may have a NULL f. An
 * unknown type gets the parser for UnknownFrame. */
BURROW_FUNC(Http2CountErrorFunc, void, Str token);
typedef Error (*Http2FrameParser)(Http2Frame *f, Http2FrameHeader fh,
                                  Http2CountErrorFunc count_error, Slice payload);
Http2FrameParser burrow__http2_type_frame_parser(Http2FrameType t);

/* ---------------------------------------------------------------- framer */

/* A log function, for Go's debugReadLoggerf and debugWriteLoggerf. msg has no
 * newline at the end. */
BURROW_FUNC(Http2LogFunc, void, Str msg);

/* Framer. Make one with new_framer. The fields from allow_illegal_writes to
 * count_error are Go's exported ones, and Transport's and Server's, and are
 * the caller's to set. Leave the rest alone. */
struct Http2Framer {
    /* Write frames that break the spec, for testing other implementations.
     * Otherwise the write functions prefer an error. */
    bool allow_illegal_writes;
    /* Read frames, and frames in an order, that break the spec. It cannot be
     * used with read_meta_headers. */
    bool allow_illegal_reads;
    /* When set, read_frame puts HEADERS and CONTINUATION frames together and
     * gives back a MetaHeadersFrame. The decoder is the caller's. */
    HpackDecoder *read_meta_headers;
    /* MAX_HEADER_LIST_SIZE, for read_meta_headers. 0 is 16 MiB. */
    uint32_t max_header_list_size;
    /* The most header values read_meta_headers takes. 0 is no limit. */
    Int max_header_value_count;
    /* Told a token naming the problem whenever a frame does not parse. */
    Http2CountErrorFunc count_error;

    Alloc *a;
    IoReader r;
    IoWriter w;
    Http2Frame *last_frame;
    Error err_detail;
    /* Non-zero after a HEADERS or CONTINUATION frame with more to come. */
    uint32_t last_header_stream;
    Http2FrameType last_frame_type;
    uint32_t max_read_size;
    Byte header_buf[HTTP2_FRAME_HEADER_LEN];
    Byte *read_buf;
    Int read_buf_cap;
    Byte *wbuf;
    Int wbuf_len;
    Int wbuf_cap;
    bool wbuf_failed;
    bool log_reads;
    bool log_writes;
    /* The Framer that reads back what this one writes, for log_writes. */
    Http2Framer *debug_framer;
    BytesBuffer *debug_framer_buf;
    Http2LogFunc debug_read_logger;
    Http2LogFunc debug_write_logger;
    /* The DATA frame SetReuseFrames reuses, or NULL. */
    Http2Frame *frame_cache;
    bool reuse_frames;
    /* What err_detail reads, when it is not a constant. */
    Str detail_text;
    Byte *detail_buf;
    Int detail_cap;
};

/* NewFramer: frames written to w and read from r. Either may be the zero
 * value when it will not be used. NULL when a cannot give the memory. */
BURROW_OWNS(ret) Http2Framer *burrow__http2_new_framer(Alloc *a, IoWriter w,
                                                       IoReader r);
void burrow__http2_framer_free(Http2Framer *fr);

/* SetReuseFrames: from now on read_frame gives back the same DATA frame each
 * time. */
void burrow__http2_framer_set_reuse_frames(Http2Framer *fr);

/* SetMaxReadFrameSize: the largest frame read_frame takes, at most
 * HTTP2_MAX_FRAME_SIZE. Telling the peer is the caller's job. */
void burrow__http2_framer_set_max_read_frame_size(Http2Framer *fr, uint32_t v);

/* ErrorDetail: more about the last error read_frame gave back, or no error.
 * It is the Framer's, and is gone at the next read. */
Error burrow__http2_framer_error_detail(const Http2Framer *fr);

/* ReadFrameHeader: the header of the next frame, without its payload, which
 * is then read_frame_for_header's or the caller's to read. A frame larger
 * than SetMaxReadFrameSize gives back its header and ErrFrameTooLarge. */
Http2FrameHeader burrow__http2_framer_read_frame_header(Http2Framer *fr, Error *err);

/* ReadFrameForHeader: the payload for fh, read and parsed. */
BURROW_OWNS(ret) Http2Frame *
burrow__http2_framer_read_frame_for_header(Http2Framer *fr, Http2FrameHeader fh,
                                           Error *err);

/* ReadFrame: the next frame. The error can be ErrFrameTooLarge, a
 * ConnectionError, a StreamError or whatever the reader said. A frame and an
 * error can come back together, and then the frame's stream is the one at
 * fault. Free whatever frame comes back. */
BURROW_OWNS(ret) Http2Frame *burrow__http2_framer_read_frame(Http2Framer *fr,
                                                             Error *err);

/* The write functions. Each one makes one Write to w. Keeping to the peer's
 * frame size, and not writing from two threads at once, are the caller's
 * jobs. */

/* WriteData and WriteDataPadded. A pad with a NULL p is no padding and no
 * PADDED flag, and an empty one with a p is the flag and no padding. The pad
 * has to be zeros, unless allow_illegal_writes, and no longer than 255. */
Error burrow__http2_framer_write_data(Http2Framer *fr, uint32_t stream_id,
                                      bool end_stream, Slice data);
Error burrow__http2_framer_write_data_padded(Http2Framer *fr, uint32_t stream_id,
                                             bool end_stream, Slice data, Slice pad);

/* WriteSettings, with the n settings at settings and no ACK, and
 * WriteSettingsAck. */
Error burrow__http2_framer_write_settings(Http2Framer *fr, const Http2Setting *settings,
                                          Int n);
Error burrow__http2_framer_write_settings_ack(Http2Framer *fr);

Error burrow__http2_framer_write_ping(Http2Framer *fr, bool ack, const Byte data[8]);

Error burrow__http2_framer_write_go_away(Http2Framer *fr, uint32_t max_stream_id,
                                         Http2ErrCode code, Slice debug_data);

/* WriteWindowUpdate. incr has to be from 1 to 2^31-1, unless
 * allow_illegal_writes. A stream of 0 is the whole connection. */
Error burrow__http2_framer_write_window_update(Http2Framer *fr, uint32_t stream_id,
                                               uint32_t incr);

/* HeadersFrameParam. A zero priority leaves the PRIORITY flag off. */
typedef struct Http2HeadersFrameParam {
    uint32_t stream_id;
    Slice block_fragment;
    bool end_stream;
    bool end_headers;
    uint8_t pad_length;
    Http2PriorityParam priority;
} Http2HeadersFrameParam;

/* WriteHeaders, one HEADERS frame. Encoding the headers and splitting them
 * into CONTINUATION frames happen elsewhere. */
Error burrow__http2_framer_write_headers(Http2Framer *fr, Http2HeadersFrameParam p);

Error burrow__http2_framer_write_priority(Http2Framer *fr, uint32_t stream_id,
                                          Http2PriorityParam p);

Error burrow__http2_framer_write_priority_update(Http2Framer *fr, uint32_t stream_id,
                                                 Str priority);

Error burrow__http2_framer_write_rst_stream(Http2Framer *fr, uint32_t stream_id,
                                            Http2ErrCode code);

Error burrow__http2_framer_write_continuation(Http2Framer *fr, uint32_t stream_id,
                                              bool end_headers,
                                              Slice header_block_fragment);

/* PushPromiseParam. */
typedef struct Http2PushPromiseParam {
    uint32_t stream_id;
    uint32_t promise_id;
    Slice block_fragment;
    bool end_headers;
    uint8_t pad_length;
} Http2PushPromiseParam;

Error burrow__http2_framer_write_push_promise(Http2Framer *fr, Http2PushPromiseParam p);

/* WriteRawFrame, for frame types this does not know. */
Error burrow__http2_framer_write_raw_frame(Http2Framer *fr, Http2FrameType t,
                                           Http2Flags flags, uint32_t stream_id,
                                           Slice payload);

/* startWrite, writeBytes and endWrite, which the write functions are made of
 * and Go's tests call. end_write gives back ErrFrameTooLarge for a payload of
 * 16 MiB or more. */
void burrow__http2_framer_start_write(Http2Framer *fr, Http2FrameType t,
                                      Http2Flags flags, uint32_t stream_id);
void burrow__http2_framer_write_bytes(Http2Framer *fr, Slice v);
Error burrow__http2_framer_end_write(Http2Framer *fr);

/* Sets what GODEBUG says from value, or reads GODEBUG again when value is
 * NULL. For tests. */
void burrow__http2_godebug_set(const char *value);

#endif
