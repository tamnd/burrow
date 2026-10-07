/* MAC addresses, from Go's mac.go: HardwareAddr, its String and ParseMAC.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/net.h"

#include "internal.h"

#include <string.h>

#define MA_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)

Str net_hardware_addr_string(NetHardwareAddr hw, Alloc *a) {
    static const char hex[] = "0123456789abcdef";
    if (hw.len == 0)
        return BURROW_STR_EMPTY;
    Int n = hw.len * 3 - 1;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    const Byte *b = (const Byte *)hw.p;
    for (Int i = 0; i < hw.len; i++) {
        if (i > 0)
            p[i * 3 - 1] = ':';
        p[i * 3] = (Byte)hex[b[i] >> 4];
        p[i * 3 + 1] = (Byte)hex[b[i] & 0xF];
    }
    return str_from_bytes(p, n);
}

static Str ma_from(Str s, Int i) {
    return str_from_bytes(s.p + i, s.len - i);
}

static Str ma_span(Str s, Int i, Int j) {
    return str_from_bytes(s.p + i, j - i);
}

/* The bytes of s in hw, which has room for n of them, in the form whose
 * separator is at s[2] or s[4], or none. */
static bool ma_parse(Str s, Byte *hw, Int n) {
    if (s.p[2] == ':' || s.p[2] == '-') {
        for (Int x = 0, i = 0; i < n; i++, x += 3)
            if (!burrow__net_xtoi2(ma_from(s, x), s.p[2], &hw[i]))
                return false;
    } else if (s.p[4] == '.') {
        for (Int x = 0, i = 0; i < n; i += 2, x += 5) {
            if (!burrow__net_xtoi2(ma_span(s, x, x + 2), 0, &hw[i]))
                return false;
            if (!burrow__net_xtoi2(ma_from(s, x + 2), s.p[4], &hw[i + 1]))
                return false;
        }
    } else {
        for (Int x = 0, i = 0; i < n; i++, x += 2)
            if (!burrow__net_xtoi2(ma_span(s, x, x + 2), 0, &hw[i]))
                return false;
    }
    return true;
}

NetHardwareAddr net_parse_mac(Alloc *a, Str s, Error *err) {
    Int n = 0;
    if (s.len >= 12) {
        if (s.p[2] == ':' || s.p[2] == '-')
            n = (s.len + 1) % 3 == 0 ? (s.len + 1) / 3 : 0;
        else if (s.p[4] == '.')
            n = (s.len + 1) % 5 == 0 ? 2 * (s.len + 1) / 5 : 0;
        else
            n = s.len % 2 == 0 ? s.len / 2 : 0;
    }
    if (n == 6 || n == 8 || n == 20) {
        Byte hw[20];
        if (ma_parse(s, hw, n)) {
            Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
            if (p == NULL) {
                BURROW_OUT(err, burrow_err_out_of_memory);
                return slice_nil(TYPE_BYTE);
            }
            memcpy(p, hw, (size_t)n);
            return slice_from(p, n, n, TYPE_BYTE);
        }
    }
    NetAddrError e = {MA_LIT("invalid MAC address"), s};
    BURROW_OUT(err, net_addr_error_as_error(&e, error_allocator()));
    return slice_nil(TYPE_BYTE);
}

/* ------------------------------------------------------------- descriptor */

static Str ma_m_string(NetHardwareAddr *self) {
    return net_hardware_addr_string(*self, error_allocator());
}

#define MA_SIG_STRING(IN, OUT) OUT(Str)

#define MA_METHODS(M, T) M(T, String, ma_m_string, MA_SIG_STRING)

BURROW_METHODS_DEFINE(NetHardwareAddr, MA_METHODS);

const Type burrow_type_NetHardwareAddr = {
    BURROW_S_INIT("HardwareAddr"),
    BURROW_S_INIT("net"),
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    (uint16_t)(sizeof burrow__methods_NetHardwareAddr /
               sizeof burrow__methods_NetHardwareAddr[0]),
    NULL,
    burrow__methods_NetHardwareAddr,
    TYPE_BYTE,
    NULL,
    0,
    0,
    NULL,
};
