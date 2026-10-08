/* net/http/internal/http2, the HTTP/2 that net/http is built on.
 *
 * This much is the frame layer: frame.go, errors.go and the part of http2.go
 * that the frames need. A Framer reads frames from an IoReader and writes them
 * to an IoWriter, one Write per frame, and knows nothing about streams or
 * connections beyond the order HEADERS and CONTINUATION frames have to come
 * in. With read_meta_headers set it puts a HEADERS frame and the
 * CONTINUATION frames after it back together and decodes the header block
 * with HPACK. Under it are the pieces the server and client share: the flow
 * control windows from flow.go, the chunked dataBuffer and the pipe a stream's
 * body is read through.
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
#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/slice.h"
#include "burrow/sync.h"

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

/* ------------------------------------------------------------ flow control
 *
 * flow.go. An Http2Inflow is the window this end gave the peer, and an
 * Http2Outflow is the window the peer gave this end. Both are zero values
 * ready to use, as Go's are. */

/* inflowMinRefresh, the fewest bytes a WINDOW_UPDATE is sent for. */
enum { HTTP2_INFLOW_MIN_REFRESH = 4 << 10 };

typedef struct Http2Inflow {
    int32_t avail;
    int32_t unsent;
} Http2Inflow;

void burrow__http2_inflow_init(Http2Inflow *f, int32_t n);

/* Gives n bytes back to the peer's window and returns how many to send in a
 * WINDOW_UPDATE, which is zero while the update is too small to be worth a
 * frame. Panics, as Go's does, when n is negative or the window would pass
 * 2^31-1. */
int32_t burrow__http2_inflow_add(Http2Inflow *f, Int n);

/* Takes n bytes from the window and reports whether there were that many. */
bool burrow__http2_inflow_take(Http2Inflow *f, uint32_t n);

/* takeInflows: takes n bytes from both windows, or from neither when either
 * is short. */
bool burrow__http2_take_inflows(Http2Inflow *f1, Http2Inflow *f2, uint32_t n);

typedef struct Http2Outflow {
    /* The DATA bytes this end may send. */
    int32_t n;
    /* The connection's window, shared by its streams. NULL on the
     * connection's own. */
    struct Http2Outflow *conn;
} Http2Outflow;

void burrow__http2_outflow_set_conn_flow(Http2Outflow *f, Http2Outflow *cf);
int32_t burrow__http2_outflow_available(const Http2Outflow *f);

/* Takes n bytes from the window and the connection's. Panics when n is more
 * than available gives. */
void burrow__http2_outflow_take(Http2Outflow *f, int32_t n);

/* Adds n, which may be negative, and reports false, changing nothing, when
 * the sum would pass 2^31-1. */
bool burrow__http2_outflow_add(Http2Outflow *f, int32_t n);

/* ------------------------------------------------------------- dataBuffer
 *
 * databuffer.go. The bytes of DATA frames for one stream, kept in chunks of
 * 1, 2, 4, 8 or 16 KiB, so a connection can limit the memory it holds
 * without limiting how big one body can be. Go takes the chunks from a
 * sync.Pool for each size. Here they come from a, or the heap when a is
 * NULL, and go back as soon as they are read, since burrow's allocators do
 * the job the pools do in Go. A zeroed one is ready to use. */
typedef struct Http2DataBuffer {
    Alloc *a;
    Slice *chunks; /* each one's len is its size class */
    Int nchunks;
    Int chunks_cap;
    Int r;            /* the next byte to read is chunks[0][r] */
    Int w;            /* the next byte to write is chunks[nchunks-1][w] */
    Int size;         /* the bytes buffered */
    int64_t expected; /* at least this many bytes are still to come, if > 0 */
} Http2DataBuffer;

/* errReadEmpty. */
extern const Error burrow__http2_err_read_empty;

/* Read copies into p, and gives errReadEmpty when there is nothing. */
Int burrow__http2_data_buffer_read(Http2DataBuffer *b, Slice p, Error *err);
Int burrow__http2_data_buffer_len(const Http2DataBuffer *b);

/* Write appends p. It fails only when an allocation does, and then gives
 * burrow_err_out_of_memory and the count of what it kept. */
Int burrow__http2_data_buffer_write(Http2DataBuffer *b, Slice p, Error *err);

/* Gives back the chunks still held. The buffer is empty and usable after. */
void burrow__http2_data_buffer_free(Http2DataBuffer *b);

/* ------------------------------------------------------------------- pipe
 *
 * pipe.go. A goroutine-safe reader and writer over one buffer, like io.Pipe
 * with a buffer in the middle and no halves. A zeroed Http2Pipe is ready to
 * use once it has a buffer, as Go's is. Errors given to it are copied into
 * its own arena, and the ones it gives back are copies in the caller's error
 * arena, so errors_is matches them against what was given.
 * burrow__http2_pipe_free gives back the arena and the done channel and
 * leaves the buffer alone, which is the caller's. */

/* pipeBuffer, the interface the buffer is behind. */
typedef struct Http2PipeBufferVT {
    Int (*len)(void *self);
    Int (*read)(void *self, Slice p, Error *err);
    Int (*write)(void *self, Slice p, Error *err);
} Http2PipeBufferVT;

typedef struct Http2PipeBuffer {
    const Http2PipeBufferVT *vt; /* NULL when there is none */
    void *self;
} Http2PipeBuffer;

/* A dataBuffer and a bytes.Buffer as a pipeBuffer. */
Http2PipeBuffer burrow__http2_data_buffer_as_pipe_buffer(Http2DataBuffer *b);
Http2PipeBuffer burrow__http2_bytes_buffer_as_pipe_buffer(BytesBuffer *b);

typedef struct Http2Pipe {
    SyncMutex mu;
    SyncCond c;        /* c.l is set to mu on first use */
    Http2PipeBuffer b; /* none once reading is done */
    Int unread;        /* the bytes left unread when done */
    Error err;         /* the read error once empty, and set means closed */
    Error break_err;   /* the read error straight away */
    Chan *donec;       /* closed on error */
    Func read_fn;      /* run in a Read before the error, if f is set */
    Arena err_arena;   /* where err and break_err are */
} Http2Pipe;

/* errClosedPipeWrite and errUninitializedPipeWrite. */
extern const Error burrow__http2_err_closed_pipe_write;
extern const Error burrow__http2_err_uninitialized_pipe_write;

/* setBuffer. Does nothing to a closed pipe. */
void burrow__http2_pipe_set_buffer(Http2Pipe *p, Http2PipeBuffer b);
Int burrow__http2_pipe_len(Http2Pipe *p);

/* Read waits for data and copies it into d. */
Int burrow__http2_pipe_read(Http2Pipe *p, Slice d, Error *err);

/* Write copies d into the buffer and wakes a reader. */
Int burrow__http2_pipe_write(Http2Pipe *p, Slice d, Error *err);

/* CloseWithError: the next Read gives err once the data is read. err must be
 * an error. */
void burrow__http2_pipe_close_with_error(Http2Pipe *p, Error err);

/* BreakWithError: the next Read gives err straight away, and the data left
 * is counted as unread. */
void burrow__http2_pipe_break_with_error(Http2Pipe *p, Error err);

/* closeWithErrorAndCode: CloseWithError that runs fn in the reader before it
 * gives the error, once. */
void burrow__http2_pipe_close_with_error_and_code(Http2Pipe *p, Error err, Func fn);

/* Err: the error BreakWithError or CloseWithError set first, if any. */
Error burrow__http2_pipe_err(Http2Pipe *p);

/* Done: a channel that is closed when the pipe is. It is the pipe's, and
 * pipe_free frees it. */
BURROW_BORROWS(ret, p) Chan *burrow__http2_pipe_done(Http2Pipe *p);

IoReader burrow__http2_pipe_as_io_reader(Http2Pipe *p);
IoWriter burrow__http2_pipe_as_io_writer(Http2Pipe *p);

void burrow__http2_pipe_free(Http2Pipe *p);

/* --------------------------------------------------------------- logging */

/* Whether GODEBUG has http2debug=1 or 2, which is Go's VerboseLogs. */
bool burrow__http2_verbose_logs(void);

/* log.Printf: the date and time, msg and a newline, to standard error. */
void burrow__http2_log(Str msg);

/* ---------------------------------------------------------- frame writers
 *
 * write.go. Go's writeFramer is an interface with a type for each frame the
 * server writes. Here an Http2WriteFramer is a tagged union of them, small
 * enough to copy, made with the burrow__http2_write_ functions. The two big
 * ones, writeResHeaders and writePushPromise, are pointers to structs that
 * stay the server's, and so are the settings a SETTINGS writer points at. */

typedef struct Http2ServerConn Http2ServerConn;
typedef struct Http2Stream Http2Stream;

/* writeContext, which the server is. header_encoder gives the HPACK encoder
 * and the buffer it writes into. */
typedef struct Http2WriteContextVT {
    Http2Framer *(*framer)(void *self);
    Error (*flush)(void *self);
    Error (*close_conn)(void *self);
    HpackEncoder *(*header_encoder)(void *self, BytesBuffer **buf);
} Http2WriteContextVT;

typedef struct Http2WriteContext {
    const Http2WriteContextVT *vt;
    void *self;
} Http2WriteContext;

/* writeResHeaders: a HEADERS frame and any CONTINUATION frames for a response
 * header or trailer. */
typedef struct Http2WriteResHeaders {
    uint32_t stream_id;
    Int http_res_code; /* 0 means no :status */
    HttpHeader h;      /* may be NULL */
    /* Which keys of h to write, or NULL for all of them. */
    const Str *trailers;
    Int ntrailers;
    bool end_stream;
    Str date;
    Str content_type;
    Str content_length;
} Http2WriteResHeaders;

/* allocatePromisedID, which the server runs just before it writes the
 * PUSH_PROMISE. */
BURROW_FUNC(Http2AllocatePromisedIDFunc, uint32_t, Error *err);

/* writePushPromise: a PUSH_PROMISE frame and any CONTINUATION frames. */
typedef struct Http2WritePushPromise {
    uint32_t stream_id; /* the stream doing the pushing */
    Str method;
    const Url *url; /* for :scheme, :authority and :path */
    HttpHeader h;
    Http2AllocatePromisedIDFunc allocate_promised_id;
    uint32_t promised_id;
} Http2WritePushPromise;

/* Which of Go's writer types an Http2WriteFramer is. NIL is Go's nil, which
 * is what reply_to_writer leaves behind. */
typedef enum Http2WriteKind {
    HTTP2_WRITE_NIL,
    HTTP2_WRITE_FLUSH,
    HTTP2_WRITE_SETTINGS,
    HTTP2_WRITE_GO_AWAY,
    HTTP2_WRITE_DATA,
    HTTP2_WRITE_HANDLER_PANIC_RST,
    HTTP2_WRITE_STREAM_ERROR,
    HTTP2_WRITE_PING,
    HTTP2_WRITE_PING_ACK,
    HTTP2_WRITE_SETTINGS_ACK,
    HTTP2_WRITE_RES_HEADERS,
    HTTP2_WRITE_PUSH_PROMISE,
    HTTP2_WRITE_100_CONTINUE,
    HTTP2_WRITE_WINDOW_UPDATE,
} Http2WriteKind;

/* writeData. p is the handler's, which waits until it is written. */
typedef struct Http2WriteData {
    uint32_t stream_id;
    Slice p;
    bool end_stream;
} Http2WriteData;

typedef struct Http2WriteFramer {
    Http2WriteKind kind;
    union {
        struct {
            const Http2Setting *p;
            Int n;
        } settings;
        struct {
            uint32_t max_stream_id;
            Http2ErrCode code;
        } go_away;
        Http2WriteData data;
        /* handlerPanicRST's and write100ContinueHeadersFrame's. */
        uint32_t stream_id;
        Http2StreamError stream_error;
        Byte ping[8]; /* writePing's and writePingAck's */
        Http2WriteResHeaders *res_headers;
        Http2WritePushPromise *push_promise;
        struct {
            uint32_t stream_id; /* 0 for the connection */
            uint32_t n;
        } window_update;
    } u;
} Http2WriteFramer;

Http2WriteFramer burrow__http2_write_flush(void);
Http2WriteFramer burrow__http2_write_settings(const Http2Setting *settings, Int n);
Http2WriteFramer burrow__http2_write_go_away(uint32_t max_stream_id, Http2ErrCode code);
Http2WriteFramer burrow__http2_write_data(uint32_t stream_id, Slice p, bool end_stream);
Http2WriteFramer burrow__http2_write_handler_panic_rst(uint32_t stream_id);
Http2WriteFramer burrow__http2_write_stream_error(Http2StreamError se);
Http2WriteFramer burrow__http2_write_ping(const Byte data[8]);
Http2WriteFramer burrow__http2_write_ping_ack(const Byte data[8]);
Http2WriteFramer burrow__http2_write_settings_ack(void);
Http2WriteFramer burrow__http2_write_res_headers(Http2WriteResHeaders *rh);
Http2WriteFramer burrow__http2_write_push_promise(Http2WritePushPromise *pp);
Http2WriteFramer burrow__http2_write_100_continue(uint32_t stream_id);
Http2WriteFramer burrow__http2_write_window_update(uint32_t stream_id, uint32_t n);

/* writeFrame. Panics for a NIL writer. */
Error burrow__http2_write_frame(const Http2WriteFramer *w, Http2WriteContext ctx);

/* staysWithinBuffer: whether the writer writes no more than max bytes and
 * does not flush. */
bool burrow__http2_write_stays_within_buffer(const Http2WriteFramer *w, Int max);

/* writeEndsStream: whether the frame leaves the stream half closed (local).
 * Panics for a NIL writer, as Go's does for nil. */
bool burrow__http2_write_ends_stream(const Http2WriteFramer *w);

/* ------------------------------------------------------- write scheduling
 *
 * writesched.go, writesched_roundrobin.go and
 * writesched_priority_rfc9218.go. Go 1.27 has only these two schedulers.
 *
 * A scheduler copies the requests pushed to it into queues from the
 * Alloc it was made with. Go's cannot run out of memory, and here open_stream
 * and push say false when they do, with nothing changed. */

/* The parts of server.go's stream and serverConn the scheduler looks at. The
 * server's other fields come with the server. */
struct Http2ServerConn {
    int32_t max_frame_size;
};

struct Http2Stream {
    Http2ServerConn *sc;
    uint32_t id;
    Http2Outflow flow;
};

/* FrameWriteRequest. */
typedef struct Http2FrameWriteRequest {
    Http2WriteFramer write;
    /* The stream the frame is on, or NULL for frames that are on none, such
     * as PING and SETTINGS, and for RST_STREAM, whose StreamError says the
     * stream. */
    Http2Stream *stream;
    /* NULL, or a Chan of Error with room for one, which is sent the write's
     * result. */
    Chan *done;
} Http2FrameWriteRequest;

/* StreamID: 0 for a frame on no stream. */
uint32_t burrow__http2_frame_write_request_stream_id(const Http2FrameWriteRequest *wr);

/* isControl: on no stream, or RST_STREAM, for MaxQueuedControlFrames. */
bool burrow__http2_frame_write_request_is_control(const Http2FrameWriteRequest *wr);

/* DataSize: the flow control bytes the frame needs, which is 0 but for DATA. */
Int burrow__http2_frame_write_request_data_size(const Http2FrameWriteRequest *wr);

/* Consume takes min(n, available) bytes, where available is what the
 * stream's flow control and the connection's frame size allow. It gives back
 * 0 when it can take none, 1 with the whole frame in consumed, and 2 with the
 * bytes taken in consumed and the rest in rest, and the bytes taken come off
 * the stream's window. */
Int burrow__http2_frame_write_request_consume(const Http2FrameWriteRequest *wr,
                                              int32_t n,
                                              Http2FrameWriteRequest *consumed,
                                              Http2FrameWriteRequest *rest);

/* replyToWriter sends err to done, if there is one, and makes the writer
 * NIL. It panics if done has no room. */
void burrow__http2_frame_write_request_reply_to_writer(Http2FrameWriteRequest *wr,
                                                       Error err);

/* OpenStreamOptions. pusher_id is 0 for a stream the client opened. */
typedef struct Http2OpenStreamOptions {
    uint32_t pusher_id;
    Http2PriorityParam priority;
} Http2OpenStreamOptions;

/* WriteScheduler. Its methods are never called at the same time. */
typedef struct Http2WriteSchedulerVT {
    bool (*open_stream)(void *self, uint32_t stream_id, Http2OpenStreamOptions opts);
    void (*close_stream)(void *self, uint32_t stream_id);
    void (*adjust_stream)(void *self, uint32_t stream_id, Http2PriorityParam p);
    bool (*push)(void *self, Http2FrameWriteRequest wr);
    bool (*pop)(void *self, Http2FrameWriteRequest *wr);
    void (*free)(void *self);
} Http2WriteSchedulerVT;

typedef struct Http2WriteScheduler {
    const Http2WriteSchedulerVT *vt; /* NULL when making it ran out of memory */
    void *self;
} Http2WriteScheduler;

/* newRoundRobinWriteScheduler: control frames first, and then the streams
 * that have something to write, in turn. */
Http2WriteScheduler burrow__http2_new_round_robin_write_scheduler(Alloc *a);

/* newPriorityWriteSchedulerRFC9218: control frames first, and then by RFC
 * 9218 urgency, in turn among incremental streams and one at a time among the
 * rest. */
Http2WriteScheduler burrow__http2_new_priority_write_scheduler_rfc9218(Alloc *a);

/* Whether ws is the RFC 9218 one, which Go asks with a type assertion. */
bool burrow__http2_write_scheduler_is_rfc9218(Http2WriteScheduler ws);

/* OpenStream panics for a stream that is open already. CloseStream drops
 * whatever the stream has queued. AdjustStream may name a stream that is not
 * open. Push queues a frame, and Pop takes the next one that can be written,
 * or says false. */
bool burrow__http2_write_scheduler_open_stream(Http2WriteScheduler ws, uint32_t stream_id,
                                               Http2OpenStreamOptions opts);
void burrow__http2_write_scheduler_close_stream(Http2WriteScheduler ws,
                                                uint32_t stream_id);
void burrow__http2_write_scheduler_adjust_stream(Http2WriteScheduler ws,
                                                 uint32_t stream_id,
                                                 Http2PriorityParam p);
bool burrow__http2_write_scheduler_push(Http2WriteScheduler ws, Http2FrameWriteRequest wr);
bool burrow__http2_write_scheduler_pop(Http2WriteScheduler ws, Http2FrameWriteRequest *wr);
void burrow__http2_write_scheduler_free(Http2WriteScheduler ws);

#endif
