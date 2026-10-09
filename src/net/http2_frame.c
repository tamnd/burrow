/* net/http/internal/http2: the frame layer, from frame.go, errors.go and
 * http2.go.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http2.h"
#include "httpsfv.h"

#include "../xnet/httpguts.h"

#include "burrow/atomic.h"
#include "burrow/bytes.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/strconv.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

static Slice h2_bytes(const Byte *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

/* prefix and then v in base 10 or 16, in buf. */
static Str h2_numbered(Byte buf[HTTP2_STRING_MAX], const char *prefix, uint32_t v,
                       uint32_t base) {
    size_t pl = strlen(prefix);
    memcpy(buf, prefix, pl);
    Byte digits[10];
    Int nd = 0;
    do {
        uint32_t d = v % base;
        digits[nd++] = (Byte)(d < 10U ? (uint32_t)'0' + d : (uint32_t)'a' + d - 10U);
        v /= base;
    } while (v != 0);
    Int n = (Int)pl;
    while (nd > 0)
        buf[n++] = digits[--nd];
    return str_from_bytes(buf, n);
}

/* --------------------------------------------------------------- GODEBUG */

enum {
    H2_DEBUG_KNOWN = 1 << 0,
    H2_DEBUG_VERBOSE = 1 << 1,
    H2_DEBUG_FRAMES = 1 << 2,
    H2_DEBUG_XCONNECT = 1 << 3,
};

static uint32_t h2_debug_flags;

static uint32_t h2_debug_parse(const char *v) {
    uint32_t f = H2_DEBUG_KNOWN;
    if (v != NULL) {
        if (strstr(v, "http2debug=1") != NULL)
            f |= H2_DEBUG_VERBOSE;
        if (strstr(v, "http2debug=2") != NULL)
            f |= H2_DEBUG_VERBOSE | H2_DEBUG_FRAMES;
        if (strstr(v, "http2xconnect=1") != NULL)
            f |= H2_DEBUG_XCONNECT;
    }
    burrow__atomic_store_relaxed_u32(&h2_debug_flags, f);
    return f;
}

static uint32_t h2_debug(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&h2_debug_flags);
    if ((f & H2_DEBUG_KNOWN) != 0)
        return f;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    return h2_debug_parse(v);
}

static bool h2_verbose(void) {
    return (h2_debug() & H2_DEBUG_VERBOSE) != 0;
}

void burrow__http2_godebug_set(const char *value) {
    if (value == NULL)
        burrow__atomic_store_relaxed_u32(&h2_debug_flags, 0);
    else
        (void)h2_debug_parse(value);
}

/* log.Printf: the date and time, msg and a newline, in one write to standard
 * error. */
static void h2_std_log(void *env, Str msg) {
    (void)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str ts = time_format(time_now(), a, BURROW_S("2006/01/02 15:04:05"));
    Str line = fmt_sprintf_v(a, "%s %s\n", ts, msg);
    Error err = BURROW_NO_ERROR;
    (void)os_file_write(os_stderr_file(), h2_bytes(line.p, line.len), &err);
    arena_free(&ar);
}

static void h2_std_log_str(Str msg) {
    h2_std_log(NULL, msg);
}

bool burrow__http2_verbose_logs(void) {
    return h2_verbose();
}

bool burrow__http2_extended_connect_disabled(void) {
    return (h2_debug() & H2_DEBUG_XCONNECT) == 0;
}

void burrow__http2_log(Str msg) {
    h2_std_log(NULL, msg);
}

/* ---------------------------------------------------------------- errors */

static const Str h2_err_code_names[] = {
    BURROW_S_INIT("NO_ERROR"),
    BURROW_S_INIT("PROTOCOL_ERROR"),
    BURROW_S_INIT("INTERNAL_ERROR"),
    BURROW_S_INIT("FLOW_CONTROL_ERROR"),
    BURROW_S_INIT("SETTINGS_TIMEOUT"),
    BURROW_S_INIT("STREAM_CLOSED"),
    BURROW_S_INIT("FRAME_SIZE_ERROR"),
    BURROW_S_INIT("REFUSED_STREAM"),
    BURROW_S_INIT("CANCEL"),
    BURROW_S_INIT("COMPRESSION_ERROR"),
    BURROW_S_INIT("CONNECT_ERROR"),
    BURROW_S_INIT("ENHANCE_YOUR_CALM"),
    BURROW_S_INIT("INADEQUATE_SECURITY"),
    BURROW_S_INIT("HTTP_1_1_REQUIRED"),
};

enum {
    H2_NUM_ERR_CODES = (int)(sizeof h2_err_code_names / sizeof h2_err_code_names[0])
};

Str burrow__http2_err_code_string(Http2ErrCode e, Byte buf[HTTP2_STRING_MAX]) {
    if (e < (uint32_t)H2_NUM_ERR_CODES)
        return h2_err_code_names[e];
    return h2_numbered(buf, "unknown error code 0x", e, 16U);
}

Str burrow__http2_err_code_string_token(Http2ErrCode e, Byte buf[HTTP2_STRING_MAX]) {
    if (e < (uint32_t)H2_NUM_ERR_CODES)
        return h2_err_code_names[e];
    return h2_numbered(buf, "ERR_UNKNOWN_", e, 10U);
}

/* ConnectionError. */
typedef struct H2ConnectionError {
    Http2ErrCode code;
    Str message;
} H2ConnectionError;

static Str h2_connection_message(const void *self) {
    return ((const H2ConnectionError *)self)->message;
}

static bool h2_connection_is(const void *self, Error target);
static Error h2_connection_clone(const void *self, Alloc *a);

static const ErrorVT h2_connection_vt = {
    .message = h2_connection_message,
    .is = h2_connection_is,
    .clone = h2_connection_clone,
};

#define H2_CE(c, name) {c, BURROW_S_INIT("connection error: " name)}

static const H2ConnectionError h2_connection_errors[H2_NUM_ERR_CODES] = {
    H2_CE(0x0, "NO_ERROR"),
    H2_CE(0x1, "PROTOCOL_ERROR"),
    H2_CE(0x2, "INTERNAL_ERROR"),
    H2_CE(0x3, "FLOW_CONTROL_ERROR"),
    H2_CE(0x4, "SETTINGS_TIMEOUT"),
    H2_CE(0x5, "STREAM_CLOSED"),
    H2_CE(0x6, "FRAME_SIZE_ERROR"),
    H2_CE(0x7, "REFUSED_STREAM"),
    H2_CE(0x8, "CANCEL"),
    H2_CE(0x9, "COMPRESSION_ERROR"),
    H2_CE(0xa, "CONNECT_ERROR"),
    H2_CE(0xb, "ENHANCE_YOUR_CALM"),
    H2_CE(0xc, "INADEQUATE_SECURITY"),
    H2_CE(0xd, "HTTP_1_1_REQUIRED"),
};

static Error h2_connection_in(Alloc *a, Http2ErrCode code) {
    if (code < (uint32_t)H2_NUM_ERR_CODES)
        return (Error){&h2_connection_vt, &h2_connection_errors[code]};
    static const char prefix[] = "connection error: ";
    Byte buf[HTTP2_STRING_MAX];
    Str cs = burrow__http2_err_code_string(code, buf);
    Int n = (Int)sizeof prefix - 1 + cs.len;
    H2ConnectionError *e = (H2ConnectionError *)mem_alloc_nozero(
        a, sizeof(H2ConnectionError) + (size_t)n, _Alignof(H2ConnectionError));
    if (e == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(e + 1);
    memcpy(p, prefix, sizeof prefix - 1);
    memcpy(p + sizeof prefix - 1, cs.p, (size_t)cs.len);
    e->code = code;
    e->message = str_from_bytes(p, n);
    return (Error){&h2_connection_vt, e};
}

static bool h2_connection_is(const void *self, Error target) {
    return target.vt == &h2_connection_vt &&
           ((const H2ConnectionError *)target.data)->code ==
               ((const H2ConnectionError *)self)->code;
}

static Error h2_connection_clone(const void *self, Alloc *a) {
    return h2_connection_in(a, ((const H2ConnectionError *)self)->code);
}

Error burrow__http2_connection_error(Http2ErrCode code) {
    return h2_connection_in(error_allocator(), code);
}

bool burrow__http2_error_connection(Error err, Http2ErrCode *code) {
    if (err.vt != &h2_connection_vt)
        return false;
    if (code != NULL)
        *code = ((const H2ConnectionError *)err.data)->code;
    return true;
}

/* StreamError. */
typedef struct H2StreamError {
    Http2StreamError se;
    Str message;
} H2StreamError;

static Str h2_stream_message(const void *self) {
    return ((const H2StreamError *)self)->message;
}

static bool h2_stream_is(const void *self, Error target);
static Error h2_stream_clone(const void *self, Alloc *a);

static const ErrorVT h2_stream_vt = {
    .message = h2_stream_message,
    .is = h2_stream_is,
    .clone = h2_stream_clone,
};

static Error h2_stream_in(Alloc *a, uint32_t id, Http2ErrCode code, Error cause) {
    static const char prefix[] = "stream error: stream ID ";
    Byte digits[HTTP2_STRING_MAX];
    Str ds = h2_numbered(digits, "", id, 10U);
    Byte buf[HTTP2_STRING_MAX];
    Str cs = burrow__http2_err_code_string(code, buf);
    Str ct = error_text(cause);
    bool has_cause = BURROW_FAILED(cause);
    Int n = (Int)sizeof prefix - 1 + ds.len + 2 + cs.len;
    if (has_cause)
        n += 2 + ct.len;
    H2StreamError *e = (H2StreamError *)mem_alloc_nozero(
        a, sizeof(H2StreamError) + (size_t)n, _Alignof(H2StreamError));
    if (e == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(e + 1);
    Int at = 0;
    memcpy(p, prefix, sizeof prefix - 1);
    at += (Int)sizeof prefix - 1;
    memcpy(p + at, ds.p, (size_t)ds.len);
    at += ds.len;
    memcpy(p + at, "; ", 2);
    at += 2;
    memcpy(p + at, cs.p, (size_t)cs.len);
    at += cs.len;
    if (has_cause) {
        memcpy(p + at, "; ", 2);
        at += 2;
        memcpy(p + at, ct.p, (size_t)ct.len);
    }
    e->se.stream_id = id;
    e->se.code = code;
    e->se.cause = cause;
    e->message = str_from_bytes(p, n);
    return (Error){&h2_stream_vt, e};
}

static bool h2_stream_is(const void *self, Error target) {
    if (target.vt != &h2_stream_vt)
        return false;
    const Http2StreamError *a = &((const H2StreamError *)self)->se;
    const Http2StreamError *b = &((const H2StreamError *)target.data)->se;
    if (a->stream_id != b->stream_id || a->code != b->code)
        return false;
    if (BURROW_OK(b->cause))
        return BURROW_OK(a->cause);
    return errors_is(a->cause, b->cause);
}

static Error h2_stream_clone(const void *self, Alloc *a) {
    const Http2StreamError *se = &((const H2StreamError *)self)->se;
    Error cause = BURROW_NO_ERROR;
    if (BURROW_FAILED(se->cause)) {
        cause = error_retain(a, se->cause);
        if (cause.vt == burrow_err_out_of_memory.vt &&
            cause.data == burrow_err_out_of_memory.data &&
            se->cause.data != burrow_err_out_of_memory.data)
            return cause;
    }
    return h2_stream_in(a, se->stream_id, se->code, cause);
}

Error burrow__http2_stream_error(uint32_t id, Http2ErrCode code, Error cause) {
    return h2_stream_in(error_allocator(), id, code, cause);
}

bool burrow__http2_error_stream(Error err, Http2StreamError *se) {
    if (err.vt != &h2_stream_vt)
        return false;
    if (se != NULL)
        *se = ((const H2StreamError *)err.data)->se;
    return true;
}

BURROW_SENTINEL_ERROR(burrow__http2_err_from_peer, "received from peer");
BURROW_SENTINEL_ERROR(burrow__http2_err_go_away_flow,
                      "connection exceeded flow control window size");

/* connError. */
typedef struct H2ConnError {
    Http2ErrCode code;
    Str reason;
    Str message;
} H2ConnError;

static Str h2_conn_message(const void *self) {
    return ((const H2ConnError *)self)->message;
}

static Error h2_conn_clone(const void *self, Alloc *a);

static const ErrorVT h2_conn_vt = {
    .message = h2_conn_message,
    .clone = h2_conn_clone,
};

static Error h2_conn_in(Alloc *a, Http2ErrCode code, Str reason) {
    static const char prefix[] = "http2: connection error: ";
    Byte buf[HTTP2_STRING_MAX];
    Str cs = burrow__http2_err_code_string(code, buf);
    Int n = (Int)sizeof prefix - 1 + cs.len + 2 + reason.len;
    H2ConnError *e = (H2ConnError *)mem_alloc_nozero(
        a, sizeof(H2ConnError) + (size_t)(reason.len + n), _Alignof(H2ConnError));
    if (e == NULL)
        return burrow_err_out_of_memory;
    Byte *r = (Byte *)(e + 1);
    if (reason.len > 0)
        memcpy(r, reason.p, (size_t)reason.len);
    Byte *p = r + reason.len;
    Int at = 0;
    memcpy(p, prefix, sizeof prefix - 1);
    at += (Int)sizeof prefix - 1;
    memcpy(p + at, cs.p, (size_t)cs.len);
    at += cs.len;
    memcpy(p + at, ": ", 2);
    at += 2;
    if (reason.len > 0)
        memcpy(p + at, reason.p, (size_t)reason.len);
    e->code = code;
    e->reason = str_from_bytes(r, reason.len);
    e->message = str_from_bytes(p, n);
    return (Error){&h2_conn_vt, e};
}

static Error h2_conn_clone(const void *self, Alloc *a) {
    const H2ConnError *e = (const H2ConnError *)self;
    return h2_conn_in(a, e->code, e->reason);
}

Error burrow__http2_conn_error(Http2ErrCode code, Str reason) {
    return h2_conn_in(error_allocator(), code, reason);
}

bool burrow__http2_error_conn(Error err, Http2ErrCode *code, Str *reason) {
    if (err.vt != &h2_conn_vt)
        return false;
    const H2ConnError *e = (const H2ConnError *)err.data;
    if (code != NULL)
        *code = e->code;
    if (reason != NULL)
        *reason = e->reason;
    return true;
}

/* pseudoHeaderError, duplicatePseudoHeaderError, headerFieldNameError and
 * headerFieldValueError, which are strings in Go and compare by kind and
 * name. */
enum {
    H2_HDR_PSEUDO,
    H2_HDR_DUPLICATE_PSEUDO,
    H2_HDR_FIELD_NAME,
    H2_HDR_FIELD_VALUE,
};

typedef struct H2HeaderError {
    int kind;
    Str name;
    Str message;
} H2HeaderError;

static Str h2_header_message(const void *self) {
    return ((const H2HeaderError *)self)->message;
}

static bool h2_header_is(const void *self, Error target);
static Error h2_header_clone(const void *self, Alloc *a);

static const ErrorVT h2_header_vt = {
    .message = h2_header_message,
    .is = h2_header_is,
    .clone = h2_header_clone,
};

static Error h2_header_in(Alloc *a, int kind, Str name) {
    const char *prefix = "invalid header field value for ";
    if (kind == H2_HDR_PSEUDO)
        prefix = "invalid pseudo-header ";
    else if (kind == H2_HDR_DUPLICATE_PSEUDO)
        prefix = "duplicate pseudo-header ";
    else if (kind == H2_HDR_FIELD_NAME)
        prefix = "invalid header field name ";
    Int pl = (Int)strlen(prefix);
    Int ql = burrow__strconv_quote_into(NULL, name);
    H2HeaderError *e = (H2HeaderError *)mem_alloc_nozero(
        a, sizeof(H2HeaderError) + (size_t)(name.len + pl + ql),
        _Alignof(H2HeaderError));
    if (e == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(e + 1);
    if (name.len > 0)
        memcpy(p, name.p, (size_t)name.len);
    Byte *m = p + name.len;
    memcpy(m, prefix, (size_t)pl);
    (void)burrow__strconv_quote_into(m + pl, name);
    e->kind = kind;
    e->name = str_from_bytes(p, name.len);
    e->message = str_from_bytes(m, pl + ql);
    return (Error){&h2_header_vt, e};
}

static bool h2_header_is(const void *self, Error target) {
    if (target.vt != &h2_header_vt)
        return false;
    const H2HeaderError *a = (const H2HeaderError *)self;
    const H2HeaderError *b = (const H2HeaderError *)target.data;
    return a->kind == b->kind && str_eq(a->name, b->name);
}

static Error h2_header_clone(const void *self, Alloc *a) {
    const H2HeaderError *e = (const H2HeaderError *)self;
    return h2_header_in(a, e->kind, e->name);
}

Error burrow__http2_pseudo_header_error(Str name) {
    return h2_header_in(error_allocator(), H2_HDR_PSEUDO, name);
}

Error burrow__http2_duplicate_pseudo_header_error(Str name) {
    return h2_header_in(error_allocator(), H2_HDR_DUPLICATE_PSEUDO, name);
}

Error burrow__http2_header_field_name_error(Str name) {
    return h2_header_in(error_allocator(), H2_HDR_FIELD_NAME, name);
}

Error burrow__http2_header_field_value_error(Str name) {
    return h2_header_in(error_allocator(), H2_HDR_FIELD_VALUE, name);
}

BURROW_SENTINEL_ERROR(burrow__http2_err_mix_pseudo_header_types,
                      "mix of request and response pseudo headers");
BURROW_SENTINEL_ERROR(burrow__http2_err_pseudo_after_regular,
                      "pseudo header field after regular");
BURROW_SENTINEL_ERROR(burrow__http2_err_frame_too_large, "http2: frame too large");
BURROW_SENTINEL_ERROR(burrow__http2_err_stream_id, "invalid stream ID");
BURROW_SENTINEL_ERROR(burrow__http2_err_dep_stream_id, "invalid dependent stream ID");
BURROW_SENTINEL_ERROR(burrow__http2_err_pad_length, "pad length too large");
BURROW_SENTINEL_ERROR(
    burrow__http2_err_pad_bytes,
    "padding bytes must all be zeros unless AllowIllegalWrites is enabled");

static const Str h2_window_incr_text = BURROW_S_INIT("illegal window increment value");
static const Error h2_err_window_incr = {&burrow_sentinel_error_vt,
                                         &h2_window_incr_text};

static const Str h2_illegal_meta_text =
    BURROW_S_INIT("illegal use of AllowIllegalReads with ReadMetaHeaders");
static const Error h2_err_illegal_meta = {&burrow_sentinel_error_vt,
                                          &h2_illegal_meta_text};

/* -------------------------------------------------------------- settings */

static const Str h2_setting_names[] = {
    BURROW_S_INIT(""),
    BURROW_S_INIT("HEADER_TABLE_SIZE"),
    BURROW_S_INIT("ENABLE_PUSH"),
    BURROW_S_INIT("MAX_CONCURRENT_STREAMS"),
    BURROW_S_INIT("INITIAL_WINDOW_SIZE"),
    BURROW_S_INIT("MAX_FRAME_SIZE"),
    BURROW_S_INIT("MAX_HEADER_LIST_SIZE"),
    BURROW_S_INIT(""),
    BURROW_S_INIT("ENABLE_CONNECT_PROTOCOL"),
    BURROW_S_INIT("NO_RFC7540_PRIORITIES"),
};

Str burrow__http2_setting_id_string(Http2SettingID s, Byte buf[HTTP2_STRING_MAX]) {
    if (s < sizeof h2_setting_names / sizeof h2_setting_names[0] &&
        h2_setting_names[s].len > 0)
        return h2_setting_names[s];
    return h2_numbered(buf, "UNKNOWN_SETTING_", s, 10U);
}

Str burrow__http2_setting_string(Alloc *a, Http2Setting s) {
    Byte buf[HTTP2_STRING_MAX];
    return fmt_sprintf_v(a, "[%s = %d]", burrow__http2_setting_id_string(s.id, buf),
                         s.val);
}

Error burrow__http2_setting_valid(Http2Setting s) {
    if (s.id == HTTP2_SETTING_ENABLE_PUSH ||
        s.id == HTTP2_SETTING_ENABLE_CONNECT_PROTOCOL) {
        if (s.val != 1 && s.val != 0)
            return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    } else if (s.id == HTTP2_SETTING_INITIAL_WINDOW_SIZE) {
        if (s.val > 0x7fffffffU)
            return burrow__http2_connection_error(HTTP2_ERR_CODE_FLOW_CONTROL);
    } else if (s.id == HTTP2_SETTING_MAX_FRAME_SIZE) {
        if (s.val < 16384U || s.val > 0xffffffU)
            return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    return BURROW_NO_ERROR;
}

bool burrow__http2_valid_wire_header_field_name(Str v) {
    if (v.len == 0)
        return false;
    /* Go ranges over runes, and nothing outside ASCII is a token rune, so
     * looking at bytes gives the same answer. */
    for (Int i = 0; i < v.len; i++) {
        Byte c = v.p[i];
        if (!burrow__httpguts_is_token_rune((int32_t)c))
            return false;
        if ('A' <= c && c <= 'Z')
            return false;
    }
    return true;
}

/* ----------------------------------------------------------- frame types */

static const Str h2_frame_names[0x11] = {
    BURROW_S_INIT("DATA"),
    BURROW_S_INIT("HEADERS"),
    BURROW_S_INIT("PRIORITY"),
    BURROW_S_INIT("RST_STREAM"),
    BURROW_S_INIT("SETTINGS"),
    BURROW_S_INIT("PUSH_PROMISE"),
    BURROW_S_INIT("PING"),
    BURROW_S_INIT("GOAWAY"),
    BURROW_S_INIT("WINDOW_UPDATE"),
    BURROW_S_INIT("CONTINUATION"),
    BURROW_S_INIT(""),
    BURROW_S_INIT(""),
    BURROW_S_INIT(""),
    BURROW_S_INIT(""),
    BURROW_S_INIT(""),
    BURROW_S_INIT(""),
    BURROW_S_INIT("PRIORITY_UPDATE"),
};

Str burrow__http2_frame_type_string(Http2FrameType t, Byte buf[HTTP2_STRING_MAX]) {
    if (t < 0x11)
        return h2_frame_names[t];
    return h2_numbered(buf, "UNKNOWN_FRAME_TYPE_", t, 10U);
}

bool burrow__http2_flags_has(Http2Flags f, Http2Flags v) {
    return (f & v) == v;
}

/* flagName[t][bit]. */
static Str h2_flag_name(Http2FrameType t, Http2Flags bit) {
    if (t == HTTP2_FRAME_DATA) {
        if (bit == HTTP2_FLAG_DATA_END_STREAM)
            return BURROW_S("END_STREAM");
        if (bit == HTTP2_FLAG_DATA_PADDED)
            return BURROW_S("PADDED");
    } else if (t == HTTP2_FRAME_HEADERS) {
        if (bit == HTTP2_FLAG_HEADERS_END_STREAM)
            return BURROW_S("END_STREAM");
        if (bit == HTTP2_FLAG_HEADERS_END_HEADERS)
            return BURROW_S("END_HEADERS");
        if (bit == HTTP2_FLAG_HEADERS_PADDED)
            return BURROW_S("PADDED");
        if (bit == HTTP2_FLAG_HEADERS_PRIORITY)
            return BURROW_S("PRIORITY");
    } else if (t == HTTP2_FRAME_SETTINGS || t == HTTP2_FRAME_PING) {
        if (bit == HTTP2_FLAG_SETTINGS_ACK)
            return BURROW_S("ACK");
    } else if (t == HTTP2_FRAME_CONTINUATION) {
        if (bit == HTTP2_FLAG_CONTINUATION_END_HEADERS)
            return BURROW_S("END_HEADERS");
    } else if (t == HTTP2_FRAME_PUSH_PROMISE) {
        if (bit == HTTP2_FLAG_PUSH_PROMISE_END_HEADERS)
            return BURROW_S("END_HEADERS");
        if (bit == HTTP2_FLAG_PUSH_PROMISE_PADDED)
            return BURROW_S("PADDED");
    }
    return BURROW_STR_EMPTY;
}

/* writeDebug. */
static void h2_write_debug(BytesBuffer *b, Http2FrameHeader h) {
    Byte buf[HTTP2_STRING_MAX];
    (void)bytes_buffer_write_string(b, burrow__http2_frame_type_string(h.type, buf),
                                    NULL);
    if (h.flags != 0) {
        (void)bytes_buffer_write_string(b, BURROW_S(" flags="), NULL);
        int set = 0;
        for (unsigned i = 0; i < 8U; i++) {
            Http2Flags bit = (Http2Flags)(1U << i);
            if ((h.flags & bit) == 0)
                continue;
            set++;
            if (set > 1)
                (void)bytes_buffer_write_string(b, BURROW_S("|"), NULL);
            Str name = h2_flag_name(h.type, bit);
            if (name.len > 0)
                (void)bytes_buffer_write_string(b, name, NULL);
            else
                (void)fmt_fprintf_v(bytes_buffer_as_io_writer(b), "0x%x",
                                    (unsigned)bit);
        }
    }
    if (h.stream_id != 0)
        (void)fmt_fprintf_v(bytes_buffer_as_io_writer(b), " stream=%d", h.stream_id);
    (void)fmt_fprintf_v(bytes_buffer_as_io_writer(b), " len=%d", h.length);
}

Str burrow__http2_frame_header_string(Alloc *a, Http2FrameHeader h) {
    BytesBuffer b = BYTES_BUFFER(a);
    (void)bytes_buffer_write_string(&b, BURROW_S("[FrameHeader "), NULL);
    h2_write_debug(&b, h);
    (void)bytes_buffer_write_string(&b, BURROW_S("]"), NULL);
    Str s = bytes_buffer_string(&b, a);
    bytes_buffer_free(&b);
    return s;
}

static Http2FrameHeader h2_read_frame_header(Byte *buf, IoReader r, Error *err) {
    Http2FrameHeader fh = {0};
    Error e = BURROW_NO_ERROR;
    (void)io_read_full(r, h2_bytes(buf, HTTP2_FRAME_HEADER_LEN), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return fh;
    }
    fh.length = (uint32_t)buf[0] << 16 | (uint32_t)buf[1] << 8 | (uint32_t)buf[2];
    fh.type = buf[3];
    fh.flags = buf[4];
    fh.stream_id = ((uint32_t)buf[5] << 24 | (uint32_t)buf[6] << 16 |
                    (uint32_t)buf[7] << 8 | (uint32_t)buf[8]) &
                   0x7fffffffU;
    fh.valid = true;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return fh;
}

Http2FrameHeader burrow__http2_read_frame_header(IoReader r, Error *err) {
    Byte buf[HTTP2_FRAME_HEADER_LEN];
    return h2_read_frame_header(buf, r, err);
}

/* invalidHTTP1LookingFrameHeader, which is what "HTTP/1.1 " reads as. */
static bool h2_http1_looking(Http2FrameHeader fh) {
    return fh.valid && fh.length == 0x485454U && fh.type == 'P' && fh.flags == '/' &&
           fh.stream_id == 0x312e3120U;
}

/* ---------------------------------------------------------------- frames */

bool burrow__http2_priority_param_is_zero(Http2PriorityParam p) {
    return p.stream_dep == 0 && !p.exclusive && p.weight == 0 && p.urgency == 0 &&
           p.incremental == 0;
}

bool burrow__http2_priority_param_equal(Http2PriorityParam a, Http2PriorityParam b) {
    return a.stream_dep == b.stream_dep && a.exclusive == b.exclusive &&
           a.weight == b.weight && a.urgency == b.urgency &&
           a.incremental == b.incremental;
}

Http2PriorityParam burrow__http2_default_rfc9218_priority(bool can_use_default) {
    Http2PriorityParam p = {0};
    p.urgency = 3;
    p.incremental = can_use_default ? 0 : 1;
    return p;
}

static void h2_priority_dict(void *env, Str key, Str val, Str param) {
    (void)param;
    Http2PriorityParam *p = (Http2PriorityParam *)env;
    if (str_eq(key, BURROW_S("u"))) {
        int64_t u = 0;
        if (burrow__httpsfv_parse_integer(val, &u) && u >= 0 && u <= 7)
            p->urgency = (uint8_t)u;
    } else if (str_eq(key, BURROW_S("i"))) {
        bool i = false;
        if (burrow__httpsfv_parse_boolean(val, &i))
            p->incremental = i ? 1 : 0;
    }
}

bool burrow__http2_parse_rfc9218_priority(Str s, bool can_use_default,
                                          Http2PriorityParam *p) {
    Http2PriorityParam v = burrow__http2_default_rfc9218_priority(can_use_default);
    bool ok = burrow__httpsfv_parse_dictionary(
        s, BURROW_FN(HttpsfvDictFunc, h2_priority_dict, &v));
    if (!ok)
        v = burrow__http2_default_rfc9218_priority(can_use_default);
    *p = v;
    return ok;
}

void burrow__http2_frame_free(Http2Frame *f) {
    if (f == NULL || f->cached)
        return;
    if (f->framer != NULL && f->framer->last_frame == f)
        f->framer->last_frame = NULL;
    Alloc *a = f->a;
    if (a == NULL)
        return;
    if (f->kind == HTTP2_PRIORITY_UPDATE_FRAME) {
        Str s = f->u.priority_update.priority;
        if (s.len > 0)
            mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    } else if (f->kind == HTTP2_META_HEADERS_FRAME) {
        burrow__hpack_header_fields_free(a, &f->u.meta_headers.fields);
    }
    mem_free(a, f, sizeof(Http2Frame), _Alignof(Http2Frame));
}

/* checkValid. */
static void h2_check_valid(const Http2Frame *f) {
    if (!f->header.valid)
        panic_str(BURROW_S("Frame accessor called on non-owned Frame"));
}

/* What Go's type system does for it. */
static void h2_want_kind(const Http2Frame *f, Http2FrameKind k) {
    if (f->kind != k)
        panic_str(BURROW_S("http2: frame method called on the wrong kind of frame"));
}

static const Http2HeadersFrame *h2_headers_of(const Http2Frame *f) {
    if (f->kind == HTTP2_META_HEADERS_FRAME)
        return &f->u.meta_headers.headers;
    h2_want_kind(f, HTTP2_HEADERS_FRAME);
    return &f->u.headers;
}

Slice burrow__http2_data_frame_data(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_DATA_FRAME);
    h2_check_valid(f);
    return f->u.data.data;
}

bool burrow__http2_data_frame_stream_ended(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_DATA_FRAME);
    return burrow__http2_flags_has(f->header.flags, HTTP2_FLAG_DATA_END_STREAM);
}

Slice burrow__http2_headers_frame_header_block_fragment(const Http2Frame *f) {
    const Http2HeadersFrame *h = h2_headers_of(f);
    h2_check_valid(f);
    return h->header_frag_buf;
}

bool burrow__http2_headers_frame_headers_ended(const Http2Frame *f) {
    (void)h2_headers_of(f);
    return burrow__http2_flags_has(f->header.flags, HTTP2_FLAG_HEADERS_END_HEADERS);
}

bool burrow__http2_headers_frame_stream_ended(const Http2Frame *f) {
    (void)h2_headers_of(f);
    return burrow__http2_flags_has(f->header.flags, HTTP2_FLAG_HEADERS_END_STREAM);
}

bool burrow__http2_headers_frame_has_priority(const Http2Frame *f) {
    (void)h2_headers_of(f);
    return burrow__http2_flags_has(f->header.flags, HTTP2_FLAG_HEADERS_PRIORITY);
}

bool burrow__http2_settings_frame_is_ack(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_SETTINGS_FRAME);
    return burrow__http2_flags_has(f->header.flags, HTTP2_FLAG_SETTINGS_ACK);
}

Int burrow__http2_settings_frame_num_settings(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_SETTINGS_FRAME);
    return f->u.settings.p.len / 6;
}

Http2Setting burrow__http2_settings_frame_setting(const Http2Frame *f, Int i) {
    h2_want_kind(f, HTTP2_SETTINGS_FRAME);
    if (i < 0 || i >= f->u.settings.p.len / 6)
        panic_str(BURROW_S("runtime error: index out of range"));
    const Byte *b = (const Byte *)f->u.settings.p.p + i * 6;
    Http2Setting s;
    s.id = (Http2SettingID)((unsigned)b[0] << 8 | (unsigned)b[1]);
    s.val = (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 |
            (uint32_t)b[5];
    return s;
}

bool burrow__http2_settings_frame_value(const Http2Frame *f, Http2SettingID id,
                                        uint32_t *v) {
    h2_want_kind(f, HTTP2_SETTINGS_FRAME);
    h2_check_valid(f);
    Int n = burrow__http2_settings_frame_num_settings(f);
    for (Int i = 0; i < n; i++) {
        Http2Setting s = burrow__http2_settings_frame_setting(f, i);
        if (s.id == id) {
            BURROW_OUT(v, s.val);
            return true;
        }
    }
    BURROW_OUT(v, 0U);
    return false;
}

bool burrow__http2_settings_frame_has_duplicates(const Http2Frame *f) {
    Int num = burrow__http2_settings_frame_num_settings(f);
    if (num == 0)
        return false;
    /* Go uses a map from 10 settings up. A bitmap of every id is the same
     * thing, and without the memory for one the pairs are still there. */
    Byte *seen = NULL;
    if (num >= 10 && f->a != NULL)
        seen = (Byte *)mem_alloc(f->a, 65536U / 8U, 1);
    if (seen == NULL) {
        for (Int i = 0; i < num; i++) {
            Http2SettingID idi = burrow__http2_settings_frame_setting(f, i).id;
            for (Int j = i + 1; j < num; j++) {
                if (idi == burrow__http2_settings_frame_setting(f, j).id)
                    return true;
            }
        }
        return false;
    }
    bool dup = false;
    for (Int i = 0; i < num && !dup; i++) {
        Http2SettingID id = burrow__http2_settings_frame_setting(f, i).id;
        Byte bit = (Byte)(1U << (id & 7U));
        if ((seen[id >> 3] & bit) != 0)
            dup = true;
        seen[id >> 3] = (Byte)(seen[id >> 3] | bit);
    }
    mem_free(f->a, seen, 65536U / 8U, 1);
    return dup;
}

Error burrow__http2_settings_frame_foreach_setting(const Http2Frame *f,
                                                   Http2SettingFunc fn) {
    h2_want_kind(f, HTTP2_SETTINGS_FRAME);
    h2_check_valid(f);
    Int n = burrow__http2_settings_frame_num_settings(f);
    for (Int i = 0; i < n; i++) {
        Error err = BURROW_CALLF(fn, burrow__http2_settings_frame_setting(f, i));
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

bool burrow__http2_ping_frame_is_ack(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_PING_FRAME);
    return burrow__http2_flags_has(f->header.flags, HTTP2_FLAG_PING_ACK);
}

Slice burrow__http2_go_away_frame_debug_data(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_GO_AWAY_FRAME);
    h2_check_valid(f);
    return f->u.go_away.debug_data;
}

Slice burrow__http2_unknown_frame_payload(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_UNKNOWN_FRAME);
    h2_check_valid(f);
    return f->u.unknown.p;
}

Slice burrow__http2_continuation_frame_header_block_fragment(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_CONTINUATION_FRAME);
    h2_check_valid(f);
    return f->u.continuation.header_frag_buf;
}

bool burrow__http2_continuation_frame_headers_ended(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_CONTINUATION_FRAME);
    return burrow__http2_flags_has(f->header.flags,
                                   HTTP2_FLAG_CONTINUATION_END_HEADERS);
}

Slice burrow__http2_push_promise_frame_header_block_fragment(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_PUSH_PROMISE_FRAME);
    h2_check_valid(f);
    return f->u.push_promise.header_frag_buf;
}

bool burrow__http2_push_promise_frame_headers_ended(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_PUSH_PROMISE_FRAME);
    return burrow__http2_flags_has(f->header.flags,
                                   HTTP2_FLAG_PUSH_PROMISE_END_HEADERS);
}

static const HpackHeaderFields *h2_meta_fields(const Http2Frame *f) {
    h2_want_kind(f, HTTP2_META_HEADERS_FRAME);
    return &f->u.meta_headers.fields;
}

Str burrow__http2_meta_headers_frame_pseudo_value(const Http2Frame *f, Str pseudo) {
    const HpackHeaderFields *fs = h2_meta_fields(f);
    for (Int i = 0; i < fs->len; i++) {
        HpackHeaderField hf = fs->p[i];
        if (!burrow__hpack_header_field_is_pseudo(hf))
            return BURROW_STR_EMPTY;
        if (str_eq(str_from_bytes(hf.name.p + 1, hf.name.len - 1), pseudo))
            return hf.value;
    }
    return BURROW_STR_EMPTY;
}

HpackHeaderFields burrow__http2_meta_headers_frame_regular_fields(const Http2Frame *f) {
    const HpackHeaderFields *fs = h2_meta_fields(f);
    HpackHeaderFields r = {0};
    for (Int i = 0; i < fs->len; i++) {
        if (!burrow__hpack_header_field_is_pseudo(fs->p[i])) {
            r.p = fs->p + i;
            r.len = fs->len - i;
            r.cap = fs->cap - i;
            return r;
        }
    }
    return r;
}

HpackHeaderFields burrow__http2_meta_headers_frame_pseudo_fields(const Http2Frame *f) {
    const HpackHeaderFields *fs = h2_meta_fields(f);
    for (Int i = 0; i < fs->len; i++) {
        if (!burrow__hpack_header_field_is_pseudo(fs->p[i])) {
            HpackHeaderFields r = {fs->p, i, fs->cap};
            return r;
        }
    }
    return *fs;
}

Http2PriorityParam burrow__http2_meta_headers_frame_rfc9218_priority(
    const Http2Frame *f, bool priority_aware, bool *priority_aware_after,
    bool *has_intermediary) {
    const HpackHeaderFields *fs = h2_meta_fields(f);
    Str s = BURROW_STR_EMPTY;
    bool inter = false;
    for (Int i = 0; i < fs->len; i++) {
        Str name = fs->p[i].name;
        if (str_eq(name, BURROW_S("priority"))) {
            s = fs->p[i].value;
            priority_aware = true;
        }
        if (str_eq(name, BURROW_S("via")) || str_eq(name, BURROW_S("forwarded")) ||
            str_eq(name, BURROW_S("x-forwarded-for")))
            inter = true;
    }
    Http2PriorityParam p;
    (void)burrow__http2_parse_rfc9218_priority(s, priority_aware && !inter, &p);
    BURROW_OUT(priority_aware_after, priority_aware);
    BURROW_OUT(has_intermediary, inter);
    return p;
}

Error burrow__http2_meta_headers_frame_check_pseudos(const Http2Frame *f) {
    bool is_request = false;
    bool is_response = false;
    HpackHeaderFields pf = burrow__http2_meta_headers_frame_pseudo_fields(f);
    for (Int i = 0; i < pf.len; i++) {
        Str name = pf.p[i].name;
        if (str_eq(name, BURROW_S(":method")) || str_eq(name, BURROW_S(":path")) ||
            str_eq(name, BURROW_S(":scheme")) || str_eq(name, BURROW_S(":authority")) ||
            str_eq(name, BURROW_S(":protocol"))) {
            is_request = true;
        } else if (str_eq(name, BURROW_S(":status"))) {
            is_response = true;
        } else {
            return burrow__http2_pseudo_header_error(name);
        }
        for (Int j = 0; j < i; j++) {
            if (str_eq(name, pf.p[j].name))
                return burrow__http2_duplicate_pseudo_header_error(name);
        }
    }
    if (is_request && is_response)
        return burrow__http2_err_mix_pseudo_header_types;
    return BURROW_NO_ERROR;
}

static Error h2_summarize_setting(void *env, Http2Setting s) {
    BytesBuffer *b = (BytesBuffer *)env;
    Byte buf[HTTP2_STRING_MAX];
    (void)fmt_fprintf_v(bytes_buffer_as_io_writer(b), " %s=%d,",
                        burrow__http2_setting_id_string(s.id, buf), s.val);
    return BURROW_NO_ERROR;
}

Str burrow__http2_summarize_frame(Alloc *a, const Http2Frame *f) {
    BytesBuffer b = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&b);
    Byte buf[HTTP2_STRING_MAX];
    h2_write_debug(&b, f->header);
    if (f->kind == HTTP2_SETTINGS_FRAME) {
        if (burrow__http2_settings_frame_num_settings(f) > 0) {
            (void)bytes_buffer_write_string(&b, BURROW_S(", settings:"), NULL);
            Int before = bytes_buffer_len(&b);
            (void)burrow__http2_settings_frame_foreach_setting(
                f, BURROW_FN(Http2SettingFunc, h2_summarize_setting, &b));
            if (bytes_buffer_len(&b) > before)
                bytes_buffer_truncate(&b, bytes_buffer_len(&b) - 1);
        } else {
            h2_check_valid(f);
        }
    } else if (f->kind == HTTP2_DATA_FRAME) {
        Slice data = burrow__http2_data_frame_data(f);
        enum { MAX = 256 };
        Int full = data.len;
        if (data.len > MAX)
            data.len = MAX;
        (void)fmt_fprintf_v(w, " data=%q", data);
        if (full > MAX)
            (void)fmt_fprintf_v(w, " (%d bytes omitted)", full - MAX);
    } else if (f->kind == HTTP2_WINDOW_UPDATE_FRAME) {
        if (f->header.stream_id == 0)
            (void)bytes_buffer_write_string(&b, BURROW_S(" (conn)"), NULL);
        (void)fmt_fprintf_v(w, " incr=%d", f->u.window_update.increment);
    } else if (f->kind == HTTP2_PING_FRAME) {
        (void)fmt_fprintf_v(w, " ping=%q", h2_bytes(f->u.ping.data, 8));
    } else if (f->kind == HTTP2_GO_AWAY_FRAME) {
        (void)fmt_fprintf_v(w, " LastStreamID=%d ErrCode=%s Debug=%q",
                            f->u.go_away.last_stream_id,
                            burrow__http2_err_code_string(f->u.go_away.err_code, buf),
                            f->u.go_away.debug_data);
    } else if (f->kind == HTTP2_RST_STREAM_FRAME) {
        (void)fmt_fprintf_v(
            w, " ErrCode=%s",
            burrow__http2_err_code_string(f->u.rst_stream.err_code, buf));
    }
    Str s = bytes_buffer_string(&b, a);
    bytes_buffer_free(&b);
    return s;
}

/* --------------------------------------------------------------- parsers */

static void h2_count(Http2CountErrorFunc fn, Str token) {
    if (!BURROW_FUNC_IS_NIL(fn))
        BURROW_CALLF(fn, token);
}

static uint32_t h2_u32(const Byte *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           (uint32_t)p[3];
}

static Slice h2_sub(Slice s, Int from, Int to) {
    return h2_bytes((const Byte *)s.p + from, to - from);
}

static Error h2_parse_data(Http2Frame *f, Http2FrameHeader fh,
                           Http2CountErrorFunc count_error, Slice payload) {
    if (fh.stream_id == 0) {
        h2_count(count_error, BURROW_S("frame_data_stream_0"));
        return burrow__http2_conn_error(HTTP2_ERR_CODE_PROTOCOL,
                                        BURROW_S("DATA frame with stream ID 0"));
    }
    f->kind = HTTP2_DATA_FRAME;
    f->header = fh;
    Int pad = 0;
    if (burrow__http2_flags_has(fh.flags, HTTP2_FLAG_DATA_PADDED)) {
        if (payload.len == 0) {
            h2_count(count_error, BURROW_S("frame_data_pad_byte_short"));
            return io_err_unexpected_eof;
        }
        pad = ((const Byte *)payload.p)[0];
        payload = h2_sub(payload, 1, payload.len);
    }
    if (pad > payload.len) {
        h2_count(count_error, BURROW_S("frame_data_pad_too_big"));
        return burrow__http2_conn_error(HTTP2_ERR_CODE_PROTOCOL,
                                        BURROW_S("pad size larger than data payload"));
    }
    f->u.data.data =
        payload.p == NULL ? payload : h2_sub(payload, 0, payload.len - pad);
    return BURROW_NO_ERROR;
}

static Error h2_parse_settings(Http2Frame *f, Http2FrameHeader fh,
                               Http2CountErrorFunc count_error, Slice p) {
    if (burrow__http2_flags_has(fh.flags, HTTP2_FLAG_SETTINGS_ACK) && fh.length > 0) {
        h2_count(count_error, BURROW_S("frame_settings_ack_with_length"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_FRAME_SIZE);
    }
    if (fh.stream_id != 0) {
        h2_count(count_error, BURROW_S("frame_settings_has_stream"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    if (p.len % 6 != 0) {
        h2_count(count_error, BURROW_S("frame_settings_mod_6"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_FRAME_SIZE);
    }
    f->kind = HTTP2_SETTINGS_FRAME;
    f->header = fh;
    f->u.settings.p = p;
    uint32_t v = 0;
    if (burrow__http2_settings_frame_value(f, HTTP2_SETTING_INITIAL_WINDOW_SIZE, &v) &&
        v > 0x7fffffffU) {
        h2_count(count_error, BURROW_S("frame_settings_window_size_too_big"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_FLOW_CONTROL);
    }
    return BURROW_NO_ERROR;
}

static Error h2_parse_ping(Http2Frame *f, Http2FrameHeader fh,
                           Http2CountErrorFunc count_error, Slice payload) {
    if (payload.len != 8) {
        h2_count(count_error, BURROW_S("frame_ping_length"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_FRAME_SIZE);
    }
    if (fh.stream_id != 0) {
        h2_count(count_error, BURROW_S("frame_ping_has_stream"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    f->kind = HTTP2_PING_FRAME;
    f->header = fh;
    memcpy(f->u.ping.data, payload.p, 8);
    return BURROW_NO_ERROR;
}

static Error h2_parse_go_away(Http2Frame *f, Http2FrameHeader fh,
                              Http2CountErrorFunc count_error, Slice p) {
    if (fh.stream_id != 0) {
        h2_count(count_error, BURROW_S("frame_goaway_has_stream"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    if (p.len < 8) {
        h2_count(count_error, BURROW_S("frame_goaway_short"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_FRAME_SIZE);
    }
    const Byte *b = (const Byte *)p.p;
    f->kind = HTTP2_GO_AWAY_FRAME;
    f->header = fh;
    f->u.go_away.last_stream_id = h2_u32(b) & 0x7fffffffU;
    f->u.go_away.err_code = h2_u32(b + 4);
    f->u.go_away.debug_data = h2_sub(p, 8, p.len);
    return BURROW_NO_ERROR;
}

static Error h2_parse_unknown(Http2Frame *f, Http2FrameHeader fh,
                              Http2CountErrorFunc count_error, Slice p) {
    (void)count_error;
    f->kind = HTTP2_UNKNOWN_FRAME;
    f->header = fh;
    f->u.unknown.p = p;
    return BURROW_NO_ERROR;
}

static Error h2_parse_window_update(Http2Frame *f, Http2FrameHeader fh,
                                    Http2CountErrorFunc count_error, Slice p) {
    if (p.len != 4) {
        h2_count(count_error, BURROW_S("frame_windowupdate_bad_len"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_FRAME_SIZE);
    }
    uint32_t inc = h2_u32((const Byte *)p.p) & 0x7fffffffU;
    if (inc == 0) {
        if (fh.stream_id == 0) {
            h2_count(count_error, BURROW_S("frame_windowupdate_zero_inc_conn"));
            return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
        }
        h2_count(count_error, BURROW_S("frame_windowupdate_zero_inc_stream"));
        return burrow__http2_stream_error(fh.stream_id, HTTP2_ERR_CODE_PROTOCOL,
                                          BURROW_NO_ERROR);
    }
    f->kind = HTTP2_WINDOW_UPDATE_FRAME;
    f->header = fh;
    f->u.window_update.increment = inc;
    return BURROW_NO_ERROR;
}

static Error h2_parse_headers(Http2Frame *f, Http2FrameHeader fh,
                              Http2CountErrorFunc count_error, Slice p) {
    if (fh.stream_id == 0) {
        h2_count(count_error, BURROW_S("frame_headers_zero_stream"));
        return burrow__http2_conn_error(HTTP2_ERR_CODE_PROTOCOL,
                                        BURROW_S("HEADERS frame with stream ID 0"));
    }
    Http2HeadersFrame hf = {0};
    Int pad = 0;
    if (burrow__http2_flags_has(fh.flags, HTTP2_FLAG_HEADERS_PADDED)) {
        if (p.len == 0) {
            h2_count(count_error, BURROW_S("frame_headers_pad_short"));
            return io_err_unexpected_eof;
        }
        pad = ((const Byte *)p.p)[0];
        p = h2_sub(p, 1, p.len);
    }
    if (burrow__http2_flags_has(fh.flags, HTTP2_FLAG_HEADERS_PRIORITY)) {
        if (p.len < 4) {
            h2_count(count_error, BURROW_S("frame_headers_prio_short"));
            return io_err_unexpected_eof;
        }
        uint32_t v = h2_u32((const Byte *)p.p);
        p = h2_sub(p, 4, p.len);
        hf.priority.stream_dep = v & 0x7fffffffU;
        hf.priority.exclusive = v != hf.priority.stream_dep;
        if (p.len == 0) {
            h2_count(count_error, BURROW_S("frame_headers_prio_weight_short"));
            return io_err_unexpected_eof;
        }
        hf.priority.weight = ((const Byte *)p.p)[0];
        p = h2_sub(p, 1, p.len);
    }
    if (p.len - pad < 0) {
        h2_count(count_error, BURROW_S("frame_headers_pad_too_big"));
        return burrow__http2_stream_error(fh.stream_id, HTTP2_ERR_CODE_PROTOCOL,
                                          BURROW_NO_ERROR);
    }
    hf.header_frag_buf = p.p == NULL ? p : h2_sub(p, 0, p.len - pad);
    f->kind = HTTP2_HEADERS_FRAME;
    f->header = fh;
    f->u.headers = hf;
    return BURROW_NO_ERROR;
}

static Error h2_parse_priority(Http2Frame *f, Http2FrameHeader fh,
                               Http2CountErrorFunc count_error, Slice payload) {
    if (fh.stream_id == 0) {
        h2_count(count_error, BURROW_S("frame_priority_zero_stream"));
        return burrow__http2_conn_error(HTTP2_ERR_CODE_PROTOCOL,
                                        BURROW_S("PRIORITY frame with stream ID 0"));
    }
    if (payload.len != 5) {
        h2_count(count_error, BURROW_S("frame_priority_bad_length"));
        Byte buf[HTTP2_STRING_MAX + 48];
        static const char pre[] = "PRIORITY frame payload size was ";
        memcpy(buf, pre, sizeof pre - 1);
        Str n = h2_numbered(buf + sizeof pre - 1, "", (uint32_t)payload.len, 10U);
        Int at = (Int)sizeof pre - 1 + n.len;
        memcpy(buf + at, "; want 5", 8);
        return burrow__http2_conn_error(HTTP2_ERR_CODE_FRAME_SIZE,
                                        str_from_bytes(buf, at + 8));
    }
    const Byte *b = (const Byte *)payload.p;
    uint32_t v = h2_u32(b);
    uint32_t id = v & 0x7fffffffU;
    f->kind = HTTP2_PRIORITY_FRAME;
    f->header = fh;
    f->u.priority.priority.weight = b[4];
    f->u.priority.priority.stream_dep = id;
    f->u.priority.priority.exclusive = id != v;
    return BURROW_NO_ERROR;
}

static Error h2_parse_priority_update(Http2Frame *f, Http2FrameHeader fh,
                                      Http2CountErrorFunc count_error, Slice payload) {
    if (fh.stream_id != 0) {
        h2_count(count_error, BURROW_S("frame_priority_update_non_zero_stream"));
        return burrow__http2_conn_error(
            HTTP2_ERR_CODE_PROTOCOL,
            BURROW_S("PRIORITY_UPDATE frame with non-zero stream ID"));
    }
    if (payload.len < 4) {
        h2_count(count_error, BURROW_S("frame_priority_update_bad_length"));
        Byte buf[HTTP2_STRING_MAX + 64];
        static const char pre[] = "PRIORITY_UPDATE frame payload size was ";
        memcpy(buf, pre, sizeof pre - 1);
        Str n = h2_numbered(buf + sizeof pre - 1, "", (uint32_t)payload.len, 10U);
        Int at = (Int)sizeof pre - 1 + n.len;
        memcpy(buf + at, "; want at least 4", 17);
        return burrow__http2_conn_error(HTTP2_ERR_CODE_FRAME_SIZE,
                                        str_from_bytes(buf, at + 17));
    }
    uint32_t id = h2_u32((const Byte *)payload.p) & 0x7fffffffU;
    if (id == 0) {
        h2_count(count_error,
                 BURROW_S("frame_priority_update_prioritizing_zero_stream"));
        return burrow__http2_conn_error(
            HTTP2_ERR_CODE_PROTOCOL,
            BURROW_S("PRIORITY_UPDATE frame with prioritized stream ID of zero"));
    }
    Slice rest = h2_sub(payload, 4, payload.len);
    Str prio = BURROW_STR_EMPTY;
    if (rest.len > 0 && f->a == NULL) {
        prio = str_from_bytes((const Byte *)rest.p, rest.len);
    } else if (rest.len > 0) {
        Byte *c = (Byte *)mem_alloc_nozero(f->a, (size_t)rest.len, 1);
        if (c == NULL)
            return burrow_err_out_of_memory;
        memcpy(c, rest.p, (size_t)rest.len);
        prio = str_from_bytes(c, rest.len);
    }
    f->kind = HTTP2_PRIORITY_UPDATE_FRAME;
    f->header = fh;
    f->u.priority_update.prioritized_stream_id = id;
    f->u.priority_update.priority = prio;
    return BURROW_NO_ERROR;
}

static Error h2_parse_rst_stream(Http2Frame *f, Http2FrameHeader fh,
                                 Http2CountErrorFunc count_error, Slice p) {
    if (p.len != 4) {
        h2_count(count_error, BURROW_S("frame_rststream_bad_len"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_FRAME_SIZE);
    }
    if (fh.stream_id == 0) {
        h2_count(count_error, BURROW_S("frame_rststream_zero_stream"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    f->kind = HTTP2_RST_STREAM_FRAME;
    f->header = fh;
    f->u.rst_stream.err_code = h2_u32((const Byte *)p.p);
    return BURROW_NO_ERROR;
}

static Error h2_parse_continuation(Http2Frame *f, Http2FrameHeader fh,
                                   Http2CountErrorFunc count_error, Slice p) {
    if (fh.stream_id == 0) {
        h2_count(count_error, BURROW_S("frame_continuation_zero_stream"));
        return burrow__http2_conn_error(
            HTTP2_ERR_CODE_PROTOCOL, BURROW_S("CONTINUATION frame with stream ID 0"));
    }
    f->kind = HTTP2_CONTINUATION_FRAME;
    f->header = fh;
    f->u.continuation.header_frag_buf = p;
    return BURROW_NO_ERROR;
}

static Error h2_parse_push_promise(Http2Frame *f, Http2FrameHeader fh,
                                   Http2CountErrorFunc count_error, Slice p) {
    if (fh.stream_id == 0) {
        h2_count(count_error, BURROW_S("frame_pushpromise_zero_stream"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    Int pad = 0;
    if (burrow__http2_flags_has(fh.flags, HTTP2_FLAG_PUSH_PROMISE_PADDED)) {
        if (p.len == 0) {
            h2_count(count_error, BURROW_S("frame_pushpromise_pad_short"));
            return io_err_unexpected_eof;
        }
        pad = ((const Byte *)p.p)[0];
        p = h2_sub(p, 1, p.len);
    }
    if (p.len < 4) {
        h2_count(count_error, BURROW_S("frame_pushpromise_promiseid_short"));
        return io_err_unexpected_eof;
    }
    uint32_t id = h2_u32((const Byte *)p.p) & 0x7fffffffU;
    p = h2_sub(p, 4, p.len);
    if (pad > p.len) {
        h2_count(count_error, BURROW_S("frame_pushpromise_pad_too_big"));
        return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    f->kind = HTTP2_PUSH_PROMISE_FRAME;
    f->header = fh;
    f->u.push_promise.promise_id = id;
    f->u.push_promise.header_frag_buf = h2_sub(p, 0, p.len - pad);
    return BURROW_NO_ERROR;
}

static const Http2FrameParser h2_parsers[0x11] = {
    h2_parse_data,
    h2_parse_headers,
    h2_parse_priority,
    h2_parse_rst_stream,
    h2_parse_settings,
    h2_parse_push_promise,
    h2_parse_ping,
    h2_parse_go_away,
    h2_parse_window_update,
    h2_parse_continuation,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    h2_parse_priority_update,
};

Http2FrameParser burrow__http2_type_frame_parser(Http2FrameType t) {
    if (t < 0x11 && h2_parsers[t] != NULL)
        return h2_parsers[t];
    return h2_parse_unknown;
}

/* ---------------------------------------------------------------- framer */

void burrow__http2_framer_set_max_read_frame_size(Http2Framer *fr, uint32_t v) {
    if (v > (uint32_t)HTTP2_MAX_FRAME_SIZE)
        v = HTTP2_MAX_FRAME_SIZE;
    fr->max_read_size = v;
}

Http2Framer *burrow__http2_new_framer(Alloc *a, IoWriter w, IoReader r) {
    Http2Framer *fr =
        (Http2Framer *)mem_alloc(a, sizeof(Http2Framer), _Alignof(Http2Framer));
    if (fr == NULL)
        return NULL;
    fr->a = a;
    fr->w = w;
    fr->r = r;
    bool frames = (h2_debug() & H2_DEBUG_FRAMES) != 0;
    fr->log_reads = frames;
    fr->log_writes = frames;
    fr->debug_read_logger = BURROW_FN(Http2LogFunc, h2_std_log, NULL);
    fr->debug_write_logger = BURROW_FN(Http2LogFunc, h2_std_log, NULL);
    burrow__http2_framer_set_max_read_frame_size(fr, HTTP2_MAX_FRAME_SIZE);
    return fr;
}

void burrow__http2_framer_free(Http2Framer *fr) {
    if (fr == NULL)
        return;
    Alloc *a = fr->a;
    if (fr->read_buf != NULL)
        mem_free(a, fr->read_buf, (size_t)fr->read_buf_cap, 1);
    if (fr->wbuf != NULL)
        mem_free(a, fr->wbuf, (size_t)fr->wbuf_cap, 1);
    if (fr->detail_buf != NULL)
        mem_free(a, fr->detail_buf, (size_t)fr->detail_cap, 1);
    if (fr->frame_cache != NULL)
        mem_free(a, fr->frame_cache, sizeof(Http2Frame), _Alignof(Http2Frame));
    burrow__http2_framer_free(fr->debug_framer);
    if (fr->debug_framer_buf != NULL)
        bytes_buffer_free(fr->debug_framer_buf);
    mem_free(a, fr, sizeof(Http2Framer), _Alignof(Http2Framer));
}

void burrow__http2_framer_set_reuse_frames(Http2Framer *fr) {
    fr->reuse_frames = true;
}

Error burrow__http2_framer_error_detail(const Http2Framer *fr) {
    return fr->err_detail;
}

/* errors.New(reason) in ErrorDetail, which the Framer keeps. */
static void h2_set_detail(Http2Framer *fr, Str reason) {
    if (reason.len > fr->detail_cap) {
        Byte *p = (Byte *)mem_alloc_nozero(fr->a, (size_t)reason.len, 1);
        if (p == NULL) {
            fr->err_detail = burrow_err_out_of_memory;
            return;
        }
        if (fr->detail_buf != NULL)
            mem_free(fr->a, fr->detail_buf, (size_t)fr->detail_cap, 1);
        fr->detail_buf = p;
        fr->detail_cap = reason.len;
    }
    if (reason.len > 0)
        memcpy(fr->detail_buf, reason.p, (size_t)reason.len);
    fr->detail_text = str_from_bytes(fr->detail_buf, reason.len);
    fr->err_detail = (Error){&burrow_sentinel_error_vt, &fr->detail_text};
}

/* connError on the Framer. */
static Error h2_framer_conn_error(Http2Framer *fr, Http2ErrCode code, Str reason) {
    h2_set_detail(fr, reason);
    return burrow__http2_connection_error(code);
}

static Error h2_check_frame_order(Http2Framer *fr, Http2FrameHeader fh) {
    Http2FrameType last_type = fr->last_frame_type;
    fr->last_frame_type = fh.type;
    if (fr->allow_illegal_reads)
        return BURROW_NO_ERROR;

    Str msg = BURROW_STR_EMPTY;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *ta = arena_allocator(&ar);
    Byte b1[HTTP2_STRING_MAX];
    Byte b2[HTTP2_STRING_MAX];
    if (fr->last_header_stream != 0) {
        if (fh.type != HTTP2_FRAME_CONTINUATION)
            msg = fmt_sprintf_v(
                ta,
                "got %s for stream %d; expected CONTINUATION following %s for stream "
                "%d",
                burrow__http2_frame_type_string(fh.type, b1), fh.stream_id,
                burrow__http2_frame_type_string(last_type, b2), fr->last_header_stream);
        else if (fh.stream_id != fr->last_header_stream)
            msg =
                fmt_sprintf_v(ta, "got CONTINUATION for stream %d; expected stream %d",
                              fh.stream_id, fr->last_header_stream);
    } else if (fh.type == HTTP2_FRAME_CONTINUATION) {
        msg = fmt_sprintf_v(ta, "unexpected CONTINUATION for stream %d", fh.stream_id);
    }
    if (msg.len > 0) {
        Error err = h2_framer_conn_error(fr, HTTP2_ERR_CODE_PROTOCOL, msg);
        arena_free(&ar);
        return err;
    }
    arena_free(&ar);

    if (fh.type == HTTP2_FRAME_HEADERS || fh.type == HTTP2_FRAME_CONTINUATION) {
        if (burrow__http2_flags_has(fh.flags, HTTP2_FLAG_HEADERS_END_HEADERS))
            fr->last_header_stream = 0;
        else
            fr->last_header_stream = fh.stream_id;
    }
    return BURROW_NO_ERROR;
}

static Error h2_http1_wrap(Error err) {
    return fmt_errorf_v(
        "http2: failed reading the frame payload: %w, note that the frame "
        "header looked like an HTTP/1.1 header",
        err);
}

Http2FrameHeader burrow__http2_framer_read_frame_header(Http2Framer *fr, Error *err) {
    fr->err_detail = BURROW_NO_ERROR;
    Error e = BURROW_NO_ERROR;
    Http2FrameHeader fh = h2_read_frame_header(fr->header_buf, fr->r, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return fh;
    }
    if (fh.length > fr->max_read_size) {
        if (h2_http1_looking(fh))
            e = h2_http1_wrap(burrow__http2_err_frame_too_large);
        else
            e = burrow__http2_err_frame_too_large;
        BURROW_OUT(err, e);
        return fh;
    }
    BURROW_OUT(err, h2_check_frame_order(fr, fh));
    return fh;
}

/* "http2: Framer %p: <verb> <summary>" to a logger. */
static void h2_log_frame(Http2Framer *fr, Http2LogFunc logger, Str verb,
                         const Http2Frame *f) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *ta = arena_allocator(&ar);
    Str msg = fmt_sprintf_v(ta, "http2: Framer %p: %s %s", (const void *)fr, verb,
                            burrow__http2_summarize_frame(ta, f));
    BURROW_CALLF(logger, msg);
    arena_free(&ar);
}

static Http2Frame *h2_read_meta_frame(Http2Framer *fr, Http2Frame *hf, Error *err);

Http2Frame *burrow__http2_framer_read_frame_for_header(Http2Framer *fr,
                                                       Http2FrameHeader fh,
                                                       Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (fr->last_frame != NULL)
        fr->last_frame->header.valid = false;
    if ((Int)fh.length > fr->read_buf_cap) {
        Byte *p = (Byte *)mem_alloc_nozero(fr->a, (size_t)fh.length, 1);
        if (p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        if (fr->read_buf != NULL)
            mem_free(fr->a, fr->read_buf, (size_t)fr->read_buf_cap, 1);
        fr->read_buf = p;
        fr->read_buf_cap = (Int)fh.length;
    }
    Slice payload = h2_bytes(fr->read_buf, (Int)fh.length);
    Error e = BURROW_NO_ERROR;
    (void)io_read_full(fr->r, payload, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, h2_http1_looking(fh) ? h2_http1_wrap(e) : e);
        return NULL;
    }
    Http2Frame *f = NULL;
    if (fh.type == HTTP2_FRAME_DATA && fr->reuse_frames) {
        if (fr->frame_cache == NULL)
            fr->frame_cache = (Http2Frame *)mem_alloc(fr->a, sizeof(Http2Frame),
                                                      _Alignof(Http2Frame));
        f = fr->frame_cache;
        if (f != NULL) {
            memset(f, 0, sizeof *f);
            f->cached = true;
        }
    } else {
        f = (Http2Frame *)mem_alloc(fr->a, sizeof(Http2Frame), _Alignof(Http2Frame));
    }
    if (f == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    f->framer = fr;
    f->a = fr->a;
    e = burrow__http2_type_frame_parser(fh.type)(f, fh, fr->count_error, payload);
    if (BURROW_FAILED(e)) {
        burrow__http2_frame_free(f);
        Http2ErrCode code = 0;
        Str reason = BURROW_STR_EMPTY;
        if (burrow__http2_error_conn(e, &code, &reason))
            e = h2_framer_conn_error(fr, code, reason);
        BURROW_OUT(err, e);
        return NULL;
    }
    fr->last_frame = f;
    if (fr->log_reads)
        h2_log_frame(fr, fr->debug_read_logger, BURROW_S("read"), f);
    if (fh.type == HTTP2_FRAME_HEADERS && fr->read_meta_headers != NULL)
        return h2_read_meta_frame(fr, f, err);
    return f;
}

Http2Frame *burrow__http2_framer_read_frame(Http2Framer *fr, Error *err) {
    Error e = BURROW_NO_ERROR;
    Http2FrameHeader fh = burrow__http2_framer_read_frame_header(fr, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    return burrow__http2_framer_read_frame_for_header(fr, fh, err);
}

/* ------------------------------------------------------- meta headers */

typedef struct H2Meta {
    Http2Framer *fr;
    Http2Frame *mh;
    HpackDecoder *hdec;
    uint32_t remain_size;
    bool saw_regular;
    bool failed;
    Int header_count;
    Error invalid;
} H2Meta;

static Str h2_clone_str(Alloc *a, Str s, bool *failed) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL) {
        *failed = true;
        return BURROW_STR_EMPTY;
    }
    memcpy(p, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len);
}

static void h2_meta_append(H2Meta *st, HpackHeaderField hf) {
    Alloc *a = st->fr->a;
    HpackHeaderFields *o = &st->mh->u.meta_headers.fields;
    if (st->failed)
        return;
    if (o->len == o->cap) {
        Int cap = o->cap == 0 ? 8 : o->cap * 2;
        HpackHeaderField *p = (HpackHeaderField *)mem_alloc_nozero(
            a, (size_t)cap * sizeof(HpackHeaderField), _Alignof(HpackHeaderField));
        if (p == NULL) {
            st->failed = true;
            return;
        }
        if (o->len > 0)
            memcpy(p, o->p, (size_t)o->len * sizeof(HpackHeaderField));
        if (o->p != NULL)
            mem_free(a, o->p, (size_t)o->cap * sizeof(HpackHeaderField),
                     _Alignof(HpackHeaderField));
        o->p = p;
        o->cap = cap;
    }
    HpackHeaderField g = {h2_clone_str(a, hf.name, &st->failed),
                          h2_clone_str(a, hf.value, &st->failed), hf.sensitive};
    o->p[o->len++] = g;
}

static void h2_meta_emit(void *ctx, HpackHeaderField hf) {
    H2Meta *st = (H2Meta *)ctx;
    Http2Framer *fr = st->fr;
    if (h2_verbose() && fr->log_reads) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str msg =
            fmt_sprintf_v(arena_allocator(&ar),
                          "http2: decoded hpack field {Name:%s Value:%s Sensitive:%t}",
                          hf.name, hf.value, hf.sensitive);
        BURROW_CALLF(fr->debug_read_logger, msg);
        arena_free(&ar);
    }
    st->header_count++;
    if (fr->max_header_value_count > 0 &&
        st->header_count > fr->max_header_value_count) {
        burrow__hpack_decoder_set_emit_enabled(st->hdec, false);
        st->mh->u.meta_headers.truncated = true;
        st->remain_size = 0;
        return;
    }
    int bad = -1;
    if (!burrow__httpguts_valid_header_field_value(hf.value))
        bad = H2_HDR_FIELD_VALUE;
    bool is_pseudo = hf.name.len > 0 && hf.name.p[0] == ':';
    if (is_pseudo) {
        if (st->saw_regular)
            bad = H2_HDR_PSEUDO;
    } else {
        st->saw_regular = true;
        if (!burrow__http2_valid_wire_header_field_name(hf.name))
            bad = H2_HDR_FIELD_NAME;
    }
    if (bad >= 0) {
        /* H2_HDR_PSEUDO stands in for errPseudoAfterRegular here. */
        if (bad == H2_HDR_PSEUDO)
            st->invalid = burrow__http2_err_pseudo_after_regular;
        else
            st->invalid = h2_header_in(error_allocator(), bad, hf.name);
        burrow__hpack_decoder_set_emit_enabled(st->hdec, false);
        return;
    }
    uint32_t size = burrow__hpack_header_field_size(hf);
    if (size > st->remain_size) {
        burrow__hpack_decoder_set_emit_enabled(st->hdec, false);
        st->mh->u.meta_headers.truncated = true;
        st->remain_size = 0;
        return;
    }
    st->remain_size -= size;
    h2_meta_append(st, hf);
}

static Http2Frame *h2_meta_fail(H2Meta *st, Error e, Error *err) {
    burrow__hpack_decoder_set_emit_func(st->hdec, NULL, NULL);
    BURROW_OUT(err, e);
    return st->mh;
}

static Http2Frame *h2_read_meta_frame(Http2Framer *fr, Http2Frame *hf, Error *err) {
    if (fr->allow_illegal_reads) {
        burrow__http2_frame_free(hf);
        BURROW_OUT(err, h2_err_illegal_meta);
        return NULL;
    }
    Http2HeadersFrame h = hf->u.headers;
    memset(&hf->u, 0, sizeof hf->u);
    hf->u.meta_headers.headers = h;
    hf->kind = HTTP2_META_HEADERS_FRAME;

    H2Meta st = {0};
    st.fr = fr;
    st.mh = hf;
    st.hdec = fr->read_meta_headers;
    st.remain_size =
        fr->max_header_list_size == 0 ? 16U << 20 : fr->max_header_list_size;
    HpackDecoder *hdec = st.hdec;
    burrow__hpack_decoder_set_emit_enabled(hdec, true);
    /* maxHeaderStringLen: int(maxHeaderListSize), or 0 where that is
     * negative, which is only where int is 32 bits. */
    uint64_t max_len = st.remain_size;
    burrow__hpack_decoder_set_max_string_length(
        hdec, max_len > (uint64_t)BURROW_INT_MAX ? 0 : (Int)max_len);
    burrow__hpack_decoder_set_emit_func(hdec, h2_meta_emit, &st);

    Slice frag = h.header_frag_buf;
    bool ended =
        burrow__http2_flags_has(hf->header.flags, HTTP2_FLAG_HEADERS_END_HEADERS);
    Http2Frame *cont = NULL;
    for (;;) {
        /* 2*remainSize is a uint32 in Go, and wraps the same way. */
        if ((int64_t)frag.len > (int64_t)(uint32_t)(2U * st.remain_size)) {
            burrow__http2_frame_free(cont);
            if (h2_verbose())
                h2_std_log_str(BURROW_S("http2: header list too large"));
            return h2_meta_fail(
                &st, burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL), err);
        }
        if (BURROW_FAILED(st.invalid)) {
            burrow__http2_frame_free(cont);
            if (h2_verbose()) {
                Arena ar;
                arena_init(&ar, heap_allocator(), 0);
                h2_std_log_str(fmt_sprintf_v(arena_allocator(&ar),
                                             "http2: invalid header: %v", st.invalid));
                arena_free(&ar);
            }
            return h2_meta_fail(
                &st, burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL), err);
        }
        Error we = BURROW_NO_ERROR;
        (void)burrow__hpack_decoder_write(hdec, frag, &we);
        if (BURROW_FAILED(we)) {
            burrow__http2_frame_free(cont);
            return h2_meta_fail(
                &st, burrow__http2_connection_error(HTTP2_ERR_CODE_COMPRESSION), err);
        }
        if (ended)
            break;
        Error re = BURROW_NO_ERROR;
        Http2Frame *next = burrow__http2_framer_read_frame(fr, &re);
        burrow__http2_frame_free(cont);
        cont = NULL;
        if (BURROW_FAILED(re)) {
            burrow__http2_frame_free(next);
            burrow__hpack_decoder_set_emit_func(hdec, NULL, NULL);
            burrow__http2_frame_free(hf);
            BURROW_OUT(err, re);
            return NULL;
        }
        if (next == NULL || next->kind != HTTP2_CONTINUATION_FRAME)
            panic_str(BURROW_S("http2: expected a CONTINUATION frame"));
        cont = next;
        frag = cont->u.continuation.header_frag_buf;
        ended = burrow__http2_flags_has(cont->header.flags,
                                        HTTP2_FLAG_CONTINUATION_END_HEADERS);
    }
    burrow__http2_frame_free(cont);

    hf->u.meta_headers.headers.header_frag_buf = (Slice){0};
    hf->header.valid = false;

    Error ce = burrow__hpack_decoder_close(hdec);
    burrow__hpack_decoder_set_emit_func(hdec, NULL, NULL);
    if (BURROW_FAILED(ce)) {
        BURROW_OUT(err, burrow__http2_connection_error(HTTP2_ERR_CODE_COMPRESSION));
        return hf;
    }
    if (st.failed) {
        burrow__http2_frame_free(hf);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    bool pseudos = false;
    Error bad = st.invalid;
    if (BURROW_OK(bad)) {
        bad = burrow__http2_meta_headers_frame_check_pseudos(hf);
        pseudos = true;
    }
    if (BURROW_FAILED(bad)) {
        fr->err_detail = bad;
        if (h2_verbose()) {
            Arena ar;
            arena_init(&ar, heap_allocator(), 0);
            Alloc *ta = arena_allocator(&ar);
            if (pseudos)
                h2_std_log_str(
                    fmt_sprintf_v(ta, "http2: invalid pseudo headers: %v", bad));
            else
                h2_std_log_str(fmt_sprintf_v(ta, "http2: invalid header: %v", bad));
            arena_free(&ar);
        }
        uint32_t id = hf->header.stream_id;
        burrow__http2_frame_free(hf);
        BURROW_OUT(err, burrow__http2_stream_error(id, HTTP2_ERR_CODE_PROTOCOL, bad));
        return NULL;
    }
    return hf;
}

/* --------------------------------------------------------------- writing */

static const Byte h2_pad_zeros[255] = {0};

static void h2_wput(Http2Framer *fr, const void *p, Int n) {
    if (fr->wbuf_failed || n <= 0)
        return;
    if (n > BURROW_INT_MAX - fr->wbuf_len) {
        fr->wbuf_failed = true;
        return;
    }
    Int need = fr->wbuf_len + n;
    if (need > fr->wbuf_cap) {
        Int cap = fr->wbuf_cap < 64 ? 64 : fr->wbuf_cap;
        while (cap < need)
            cap = cap > BURROW_INT_MAX / 2 ? need : cap * 2;
        Byte *nb = (Byte *)mem_alloc_nozero(fr->a, (size_t)cap, 1);
        if (nb == NULL) {
            fr->wbuf_failed = true;
            return;
        }
        if (fr->wbuf_len > 0)
            memcpy(nb, fr->wbuf, (size_t)fr->wbuf_len);
        if (fr->wbuf != NULL)
            mem_free(fr->a, fr->wbuf, (size_t)fr->wbuf_cap, 1);
        fr->wbuf = nb;
        fr->wbuf_cap = cap;
    }
    memcpy(fr->wbuf + fr->wbuf_len, p, (size_t)n);
    fr->wbuf_len += n;
}

static void h2_wbyte(Http2Framer *fr, Byte v) {
    h2_wput(fr, &v, 1);
}

static void h2_wu32(Http2Framer *fr, uint32_t v) {
    Byte b[4] = {(Byte)(v >> 24), (Byte)(v >> 16), (Byte)(v >> 8), (Byte)v};
    h2_wput(fr, b, 4);
}

void burrow__http2_framer_start_write(Http2Framer *fr, Http2FrameType t,
                                      Http2Flags flags, uint32_t stream_id) {
    fr->wbuf_len = 0;
    fr->wbuf_failed = false;
    Byte h[HTTP2_FRAME_HEADER_LEN] = {
        0,
        0,
        0,
        t,
        flags,
        (Byte)(stream_id >> 24),
        (Byte)(stream_id >> 16),
        (Byte)(stream_id >> 8),
        (Byte)stream_id,
    };
    h2_wput(fr, h, HTTP2_FRAME_HEADER_LEN);
}

void burrow__http2_framer_write_bytes(Http2Framer *fr, Slice v) {
    h2_wput(fr, v.p, v.len);
}

/* logWrite: read back what is about to be written, and log it. */
static void h2_log_write(Http2Framer *fr) {
    if (fr->debug_framer == NULL) {
        BytesBuffer *b = bytes_new_buffer(fr->a, (Slice){0});
        if (b == NULL)
            return;
        Http2Framer *d = burrow__http2_new_framer(fr->a, (IoWriter){0},
                                                  bytes_buffer_as_io_reader(b));
        if (d == NULL) {
            bytes_buffer_free(b);
            return;
        }
        d->log_reads = false;
        d->allow_illegal_reads = true;
        fr->debug_framer_buf = b;
        fr->debug_framer = d;
    }
    (void)bytes_buffer_write(fr->debug_framer_buf, h2_bytes(fr->wbuf, fr->wbuf_len),
                             NULL);
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = burrow__http2_framer_read_frame(fr->debug_framer, &err);
    if (BURROW_FAILED(err)) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        BURROW_CALLF(
            fr->debug_write_logger,
            fmt_sprintf_v(arena_allocator(&ar),
                          "http2: Framer %p: failed to decode just-written frame",
                          (const void *)fr));
        arena_free(&ar);
        return;
    }
    h2_log_frame(fr, fr->debug_write_logger, BURROW_S("wrote"), f);
    burrow__http2_frame_free(f);
}

Error burrow__http2_framer_end_write(Http2Framer *fr) {
    if (fr->wbuf_failed)
        return burrow_err_out_of_memory;
    Int length = fr->wbuf_len - HTTP2_FRAME_HEADER_LEN;
    if (length >= (Int)1 << 24)
        return burrow__http2_err_frame_too_large;
    fr->wbuf[0] = (Byte)(length >> 16);
    fr->wbuf[1] = (Byte)(length >> 8);
    fr->wbuf[2] = (Byte)length;
    if (fr->log_writes)
        h2_log_write(fr);
    if (fr->w.vt == NULL)
        panic_str(BURROW_S("http2: Framer has no writer"));
    Error err = BURROW_NO_ERROR;
    Int n = fr->w.vt->write(fr->w.data, h2_bytes(fr->wbuf, fr->wbuf_len), &err);
    if (BURROW_OK(err) && n != fr->wbuf_len)
        err = io_err_short_write;
    return err;
}

static bool h2_valid_stream_id_or_zero(uint32_t id) {
    return (id & 0x80000000U) == 0;
}

static bool h2_valid_stream_id(uint32_t id) {
    return id != 0 && (id & 0x80000000U) == 0;
}

Error burrow__http2_framer_write_data(Http2Framer *fr, uint32_t stream_id,
                                      bool end_stream, Slice data) {
    return burrow__http2_framer_write_data_padded(fr, stream_id, end_stream, data,
                                                  (Slice){0});
}

Error burrow__http2_framer_write_data_padded(Http2Framer *fr, uint32_t stream_id,
                                             bool end_stream, Slice data, Slice pad) {
    Error err =
        burrow__http2_framer_start_write_data_padded(fr, stream_id, end_stream, data, pad);
    if (BURROW_FAILED(err))
        return err;
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_start_write_data_padded(Http2Framer *fr, uint32_t stream_id,
                                                   bool end_stream, Slice data,
                                                   Slice pad) {
    if (!h2_valid_stream_id(stream_id) && !fr->allow_illegal_writes)
        return burrow__http2_err_stream_id;
    if (pad.len > 0) {
        if (pad.len > 255)
            return burrow__http2_err_pad_length;
        if (!fr->allow_illegal_writes) {
            for (Int i = 0; i < pad.len; i++) {
                if (((const Byte *)pad.p)[i] != 0)
                    return burrow__http2_err_pad_bytes;
            }
        }
    }
    Http2Flags flags = 0;
    if (end_stream)
        flags |= HTTP2_FLAG_DATA_END_STREAM;
    if (pad.p != NULL)
        flags |= HTTP2_FLAG_DATA_PADDED;
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_DATA, flags, stream_id);
    if (pad.p != NULL)
        h2_wbyte(fr, (Byte)pad.len);
    h2_wput(fr, data.p, data.len);
    h2_wput(fr, pad.p, pad.len);
    return BURROW_NO_ERROR;
}

Error burrow__http2_framer_write_settings(Http2Framer *fr, const Http2Setting *settings,
                                          Int n) {
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_SETTINGS, 0, 0);
    for (Int i = 0; i < n; i++) {
        h2_wbyte(fr, (Byte)(settings[i].id >> 8));
        h2_wbyte(fr, (Byte)settings[i].id);
        h2_wu32(fr, settings[i].val);
    }
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_settings_ack(Http2Framer *fr) {
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_SETTINGS, HTTP2_FLAG_SETTINGS_ACK,
                                     0);
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_ping(Http2Framer *fr, bool ack, const Byte data[8]) {
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_PING,
                                     ack ? HTTP2_FLAG_PING_ACK : 0, 0);
    h2_wput(fr, data, 8);
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_go_away(Http2Framer *fr, uint32_t max_stream_id,
                                         Http2ErrCode code, Slice debug_data) {
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_GO_AWAY, 0, 0);
    h2_wu32(fr, max_stream_id & 0x7fffffffU);
    h2_wu32(fr, code);
    h2_wput(fr, debug_data.p, debug_data.len);
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_window_update(Http2Framer *fr, uint32_t stream_id,
                                               uint32_t incr) {
    if ((incr < 1 || incr > 2147483647U) && !fr->allow_illegal_writes)
        return h2_err_window_incr;
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_WINDOW_UPDATE, 0, stream_id);
    h2_wu32(fr, incr);
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_headers(Http2Framer *fr, Http2HeadersFrameParam p) {
    if (!h2_valid_stream_id(p.stream_id) && !fr->allow_illegal_writes)
        return burrow__http2_err_stream_id;
    bool prio = !burrow__http2_priority_param_is_zero(p.priority);
    Http2Flags flags = 0;
    if (p.pad_length != 0)
        flags |= HTTP2_FLAG_HEADERS_PADDED;
    if (p.end_stream)
        flags |= HTTP2_FLAG_HEADERS_END_STREAM;
    if (p.end_headers)
        flags |= HTTP2_FLAG_HEADERS_END_HEADERS;
    if (prio)
        flags |= HTTP2_FLAG_HEADERS_PRIORITY;
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_HEADERS, flags, p.stream_id);
    if (p.pad_length != 0)
        h2_wbyte(fr, p.pad_length);
    if (prio) {
        uint32_t v = p.priority.stream_dep;
        if (!h2_valid_stream_id_or_zero(v) && !fr->allow_illegal_writes)
            return burrow__http2_err_dep_stream_id;
        if (p.priority.exclusive)
            v |= 0x80000000U;
        h2_wu32(fr, v);
        h2_wbyte(fr, p.priority.weight);
    }
    h2_wput(fr, p.block_fragment.p, p.block_fragment.len);
    h2_wput(fr, h2_pad_zeros, p.pad_length);
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_priority(Http2Framer *fr, uint32_t stream_id,
                                          Http2PriorityParam p) {
    if (!h2_valid_stream_id(stream_id) && !fr->allow_illegal_writes)
        return burrow__http2_err_stream_id;
    if (!h2_valid_stream_id_or_zero(p.stream_dep))
        return burrow__http2_err_dep_stream_id;
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_PRIORITY, 0, stream_id);
    uint32_t v = p.stream_dep;
    if (p.exclusive)
        v |= 0x80000000U;
    h2_wu32(fr, v);
    h2_wbyte(fr, p.weight);
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_priority_update(Http2Framer *fr, uint32_t stream_id,
                                                 Str priority) {
    if (!h2_valid_stream_id(stream_id) && !fr->allow_illegal_writes)
        return burrow__http2_err_stream_id;
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_PRIORITY_UPDATE, 0, 0);
    h2_wu32(fr, stream_id);
    h2_wput(fr, priority.p, priority.len);
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_rst_stream(Http2Framer *fr, uint32_t stream_id,
                                            Http2ErrCode code) {
    if (!h2_valid_stream_id(stream_id) && !fr->allow_illegal_writes)
        return burrow__http2_err_stream_id;
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_RST_STREAM, 0, stream_id);
    h2_wu32(fr, code);
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_continuation(Http2Framer *fr, uint32_t stream_id,
                                              bool end_headers,
                                              Slice header_block_fragment) {
    if (!h2_valid_stream_id(stream_id) && !fr->allow_illegal_writes)
        return burrow__http2_err_stream_id;
    Http2Flags flags = end_headers ? HTTP2_FLAG_CONTINUATION_END_HEADERS : 0;
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_CONTINUATION, flags, stream_id);
    h2_wput(fr, header_block_fragment.p, header_block_fragment.len);
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_push_promise(Http2Framer *fr,
                                              Http2PushPromiseParam p) {
    if (!h2_valid_stream_id(p.stream_id) && !fr->allow_illegal_writes)
        return burrow__http2_err_stream_id;
    Http2Flags flags = 0;
    if (p.pad_length != 0)
        flags |= HTTP2_FLAG_PUSH_PROMISE_PADDED;
    if (p.end_headers)
        flags |= HTTP2_FLAG_PUSH_PROMISE_END_HEADERS;
    burrow__http2_framer_start_write(fr, HTTP2_FRAME_PUSH_PROMISE, flags, p.stream_id);
    if (p.pad_length != 0)
        h2_wbyte(fr, p.pad_length);
    if (!h2_valid_stream_id(p.promise_id) && !fr->allow_illegal_writes)
        return burrow__http2_err_stream_id;
    h2_wu32(fr, p.promise_id);
    h2_wput(fr, p.block_fragment.p, p.block_fragment.len);
    h2_wput(fr, h2_pad_zeros, p.pad_length);
    return burrow__http2_framer_end_write(fr);
}

Error burrow__http2_framer_write_raw_frame(Http2Framer *fr, Http2FrameType t,
                                           Http2Flags flags, uint32_t stream_id,
                                           Slice payload) {
    burrow__http2_framer_start_write(fr, t, flags, stream_id);
    h2_wput(fr, payload.p, payload.len);
    return burrow__http2_framer_end_write(fr);
}
