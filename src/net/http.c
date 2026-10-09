/* Derived from Go's src/net/http/status.go and http.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/net/http.h"

#include "../xnet/httpguts.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------- Status */

Str http_status_text(Int code) {
    switch (code) {
    case HTTP_STATUS_CONTINUE:
        return BURROW_S("Continue");
    case HTTP_STATUS_SWITCHING_PROTOCOLS:
        return BURROW_S("Switching Protocols");
    case HTTP_STATUS_PROCESSING:
        return BURROW_S("Processing");
    case HTTP_STATUS_EARLY_HINTS:
        return BURROW_S("Early Hints");
    case HTTP_STATUS_OK:
        return BURROW_S("OK");
    case HTTP_STATUS_CREATED:
        return BURROW_S("Created");
    case HTTP_STATUS_ACCEPTED:
        return BURROW_S("Accepted");
    case HTTP_STATUS_NON_AUTHORITATIVE_INFO:
        return BURROW_S("Non-Authoritative Information");
    case HTTP_STATUS_NO_CONTENT:
        return BURROW_S("No Content");
    case HTTP_STATUS_RESET_CONTENT:
        return BURROW_S("Reset Content");
    case HTTP_STATUS_PARTIAL_CONTENT:
        return BURROW_S("Partial Content");
    case HTTP_STATUS_MULTI_STATUS:
        return BURROW_S("Multi-Status");
    case HTTP_STATUS_ALREADY_REPORTED:
        return BURROW_S("Already Reported");
    case HTTP_STATUS_IM_USED:
        return BURROW_S("IM Used");
    case HTTP_STATUS_MULTIPLE_CHOICES:
        return BURROW_S("Multiple Choices");
    case HTTP_STATUS_MOVED_PERMANENTLY:
        return BURROW_S("Moved Permanently");
    case HTTP_STATUS_FOUND:
        return BURROW_S("Found");
    case HTTP_STATUS_SEE_OTHER:
        return BURROW_S("See Other");
    case HTTP_STATUS_NOT_MODIFIED:
        return BURROW_S("Not Modified");
    case HTTP_STATUS_USE_PROXY:
        return BURROW_S("Use Proxy");
    case HTTP_STATUS_TEMPORARY_REDIRECT:
        return BURROW_S("Temporary Redirect");
    case HTTP_STATUS_PERMANENT_REDIRECT:
        return BURROW_S("Permanent Redirect");
    case HTTP_STATUS_BAD_REQUEST:
        return BURROW_S("Bad Request");
    case HTTP_STATUS_UNAUTHORIZED:
        return BURROW_S("Unauthorized");
    case HTTP_STATUS_PAYMENT_REQUIRED:
        return BURROW_S("Payment Required");
    case HTTP_STATUS_FORBIDDEN:
        return BURROW_S("Forbidden");
    case HTTP_STATUS_NOT_FOUND:
        return BURROW_S("Not Found");
    case HTTP_STATUS_METHOD_NOT_ALLOWED:
        return BURROW_S("Method Not Allowed");
    case HTTP_STATUS_NOT_ACCEPTABLE:
        return BURROW_S("Not Acceptable");
    case HTTP_STATUS_PROXY_AUTH_REQUIRED:
        return BURROW_S("Proxy Authentication Required");
    case HTTP_STATUS_REQUEST_TIMEOUT:
        return BURROW_S("Request Timeout");
    case HTTP_STATUS_CONFLICT:
        return BURROW_S("Conflict");
    case HTTP_STATUS_GONE:
        return BURROW_S("Gone");
    case HTTP_STATUS_LENGTH_REQUIRED:
        return BURROW_S("Length Required");
    case HTTP_STATUS_PRECONDITION_FAILED:
        return BURROW_S("Precondition Failed");
    case HTTP_STATUS_REQUEST_ENTITY_TOO_LARGE:
        return BURROW_S("Request Entity Too Large");
    case HTTP_STATUS_REQUEST_URI_TOO_LONG:
        return BURROW_S("Request URI Too Long");
    case HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE:
        return BURROW_S("Unsupported Media Type");
    case HTTP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE:
        return BURROW_S("Requested Range Not Satisfiable");
    case HTTP_STATUS_EXPECTATION_FAILED:
        return BURROW_S("Expectation Failed");
    case HTTP_STATUS_TEAPOT:
        return BURROW_S("I'm a teapot");
    case HTTP_STATUS_MISDIRECTED_REQUEST:
        return BURROW_S("Misdirected Request");
    case HTTP_STATUS_UNPROCESSABLE_ENTITY:
        return BURROW_S("Unprocessable Entity");
    case HTTP_STATUS_LOCKED:
        return BURROW_S("Locked");
    case HTTP_STATUS_FAILED_DEPENDENCY:
        return BURROW_S("Failed Dependency");
    case HTTP_STATUS_TOO_EARLY:
        return BURROW_S("Too Early");
    case HTTP_STATUS_UPGRADE_REQUIRED:
        return BURROW_S("Upgrade Required");
    case HTTP_STATUS_PRECONDITION_REQUIRED:
        return BURROW_S("Precondition Required");
    case HTTP_STATUS_TOO_MANY_REQUESTS:
        return BURROW_S("Too Many Requests");
    case HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE:
        return BURROW_S("Request Header Fields Too Large");
    case HTTP_STATUS_UNAVAILABLE_FOR_LEGAL_REASONS:
        return BURROW_S("Unavailable For Legal Reasons");
    case HTTP_STATUS_INTERNAL_SERVER_ERROR:
        return BURROW_S("Internal Server Error");
    case HTTP_STATUS_NOT_IMPLEMENTED:
        return BURROW_S("Not Implemented");
    case HTTP_STATUS_BAD_GATEWAY:
        return BURROW_S("Bad Gateway");
    case HTTP_STATUS_SERVICE_UNAVAILABLE:
        return BURROW_S("Service Unavailable");
    case HTTP_STATUS_GATEWAY_TIMEOUT:
        return BURROW_S("Gateway Timeout");
    case HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED:
        return BURROW_S("HTTP Version Not Supported");
    case HTTP_STATUS_VARIANT_ALSO_NEGOTIATES:
        return BURROW_S("Variant Also Negotiates");
    case HTTP_STATUS_INSUFFICIENT_STORAGE:
        return BURROW_S("Insufficient Storage");
    case HTTP_STATUS_LOOP_DETECTED:
        return BURROW_S("Loop Detected");
    case HTTP_STATUS_NOT_EXTENDED:
        return BURROW_S("Not Extended");
    case HTTP_STATUS_NETWORK_AUTHENTICATION_REQUIRED:
        return BURROW_S("Network Authentication Required");
    default:
        return BURROW_STR_EMPTY;
    }
}

/* ---------------------------------------------------------------- Protocols */

enum {
    HTTP_PROTO_HTTP1 = 1 << 0,
    HTTP_PROTO_HTTP2 = 1 << 1,
    HTTP_PROTO_UNENCRYPTED_HTTP2 = 1 << 2,
    HTTP_PROTO_HTTP3 = 1 << 3,
};

static void http_proto_set_bit(HttpProtocols *p, uint8_t bit, bool ok) {
    if (ok)
        p->bits = (uint8_t)(p->bits | bit);
    else
        p->bits = (uint8_t)(p->bits & ~bit);
}

bool http_protocols_http1(HttpProtocols p) {
    return (p.bits & HTTP_PROTO_HTTP1) != 0;
}

void http_protocols_set_http1(HttpProtocols *p, bool ok) {
    http_proto_set_bit(p, HTTP_PROTO_HTTP1, ok);
}

bool http_protocols_http2(HttpProtocols p) {
    return (p.bits & HTTP_PROTO_HTTP2) != 0;
}

void http_protocols_set_http2(HttpProtocols *p, bool ok) {
    http_proto_set_bit(p, HTTP_PROTO_HTTP2, ok);
}

bool http_protocols_unencrypted_http2(HttpProtocols p) {
    return (p.bits & HTTP_PROTO_UNENCRYPTED_HTTP2) != 0;
}

void http_protocols_set_unencrypted_http2(HttpProtocols *p, bool ok) {
    http_proto_set_bit(p, HTTP_PROTO_UNENCRYPTED_HTTP2, ok);
}

bool burrow__http_protocols_http3(HttpProtocols p) {
    return (p.bits & HTTP_PROTO_HTTP3) != 0;
}

void burrow__http_protocols_set_http3(HttpProtocols *p, bool ok) {
    http_proto_set_bit(p, HTTP_PROTO_HTTP3, ok);
}

bool burrow__http_protocols_empty(HttpProtocols p) {
    return p.bits == 0;
}

/* Every set of the four, by its bits, so String has nothing to build. */
static const Str http_proto_names[16] = {
    BURROW_S_INIT("{}"),
    BURROW_S_INIT("{HTTP1}"),
    BURROW_S_INIT("{HTTP2}"),
    BURROW_S_INIT("{HTTP1,HTTP2}"),
    BURROW_S_INIT("{UnencryptedHTTP2}"),
    BURROW_S_INIT("{HTTP1,UnencryptedHTTP2}"),
    BURROW_S_INIT("{HTTP2,UnencryptedHTTP2}"),
    BURROW_S_INIT("{HTTP1,HTTP2,UnencryptedHTTP2}"),
    BURROW_S_INIT("{HTTP3}"),
    BURROW_S_INIT("{HTTP1,HTTP3}"),
    BURROW_S_INIT("{HTTP2,HTTP3}"),
    BURROW_S_INIT("{HTTP1,HTTP2,HTTP3}"),
    BURROW_S_INIT("{UnencryptedHTTP2,HTTP3}"),
    BURROW_S_INIT("{HTTP1,UnencryptedHTTP2,HTTP3}"),
    BURROW_S_INIT("{HTTP2,UnencryptedHTTP2,HTTP3}"),
    BURROW_S_INIT("{HTTP1,HTTP2,UnencryptedHTTP2,HTTP3}"),
};

Str http_protocols_string(HttpProtocols p) {
    return http_proto_names[p.bits & 0x0F];
}

/* ------------------------------------------------------------------ helpers */

Str burrow__http_remove_port(Str host) {
    for (Int i = host.len - 1; i >= 0; i--) {
        switch (host.p[i]) {
        case ':':
            return str_from_bytes(host.p, i);
        case ']':
            return host;
        default:
            break;
        }
    }
    return host;
}

bool burrow__http_is_token(Str v) {
    return burrow__httpguts_valid_header_field_name(v);
}

bool burrow__http_string_contains_ctl_byte(Str s) {
    for (Int i = 0; i < s.len; i++) {
        Byte b = s.p[i];
        if (b < ' ' || b == 0x7F)
            return true;
    }
    return false;
}

Str burrow__http_hex_escape_non_ascii(Alloc *a, Str s) {
    Int n = 0;
    for (Int i = 0; i < s.len; i++)
        n += s.p[i] >= 0x80 ? 3 : 1;
    if (n == s.len)
        return s;
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (b == NULL)
        return BURROW_STR_EMPTY;
    static const char hex[] = "0123456789abcdef";
    Int j = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c >= 0x80) {
            b[j++] = '%';
            b[j++] = (Byte)hex[c >> 4];
            b[j++] = (Byte)hex[c & 0x0F];
        } else {
            b[j++] = c;
        }
    }
    b[n] = 0;
    return str_from_bytes(b, n);
}
