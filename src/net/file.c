/* FileConn, FileListener and FilePacketConn, from Go's src/net/file.go and
 * file_posix.go: a connection or listener from a copy of the socket in an
 * OsFile, of whichever kind the socket's family and type make it.
 *
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/syscall.h"
#include "burrow/type.h"

#include "../os/internal.h"

#include <stdint.h>
#include <string.h>

#define FA_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)

/* ---------------------------------------------------------------- fileAddr */

/* fileAddr, the file's name as an address, which is what the OpError of a
 * FileConn that fails names. */
static const Type fa_desc = {
    BURROW_S_INIT("fileAddr"),
    BURROW_S_INIT("net"),
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e666164U, /* "nfad" */
    NULL,
};

static Str fa_network(void *self) {
    (void)self;
    return FA_LIT("file+net");
}

static Str fa_string(void *self, Alloc *a) {
    Str s = *(const Str *)self;
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len);
}

static const NetAddrVT fa_vt = {&fa_desc, fa_network, fa_string};

static bool fa_is_oom(Error e) {
    return e.vt == burrow_err_out_of_memory.vt &&
           e.data == burrow_err_out_of_memory.data;
}

/* The OpError FileConn and the others put err in, with the name kept in
 * error_allocator as long as the error is. */
static Error fa_error(OsFile *f, Error err) {
    if (BURROW_OK(err) || fa_is_oom(err))
        return err;
    Str name = os_file_name(f);
    Alloc *ea = error_allocator();
    Str *box = (Str *)mem_alloc(ea, sizeof(Str) + (size_t)name.len, _Alignof(Str));
    if (box == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(box + 1);
    if (name.len > 0)
        memcpy(p, name.p, (size_t)name.len);
    *box = str_from_bytes(p, name.len);
    NetAddr addr = {&fa_vt, box};
    return burrow__net_op_error(FA_LIT("file"), FA_LIT("file+net"),
                                (NetAddr){NULL, NULL}, addr, err);
}

/* ------------------------------------------------------------------- kinds */

/* The address type Go's addrFunc gives a socket, by the network it got. */
typedef enum { FA_NONE, FA_TCP, FA_UDP, FA_IP, FA_UNIX } FaKind;

static FaKind fa_kind(const burrow__NetFileSock *fs) {
    if (fs->net.len == 0)
        return FA_NONE;
    if (fs->family == PAL_AF_UNIX)
        return FA_UNIX;
    if (fs->sotype == PAL_SOCK_STREAM)
        return FA_TCP;
    if (fs->sotype == PAL_SOCK_DGRAM)
        return FA_UDP;
    return FA_IP;
}

/* What a socket of a kind the caller has no use for ends with. */
static Error fa_einval(const burrow__NetFileSock *fs) {
    (void)pal_socket_close(fs->s, NULL);
    return burrow__net_einval();
}

/* ------------------------------------------------------------- the three */

NetConn net_file_conn(Alloc *a, OsFile *f, Error *err) {
    NetConn c = {NULL, NULL};
    burrow__NetFileSock fs;
    Error e = burrow__netfd_file_sock(f, &fs);
    if (BURROW_OK(e)) {
        switch (fa_kind(&fs)) {
        case FA_TCP:
            c = net_tcp_conn_as_conn(burrow__net_tcp_conn_from_file(a, &fs, &e));
            break;
        case FA_UDP:
            c = net_udp_conn_as_conn(burrow__net_udp_conn_from_file(a, &fs, &e));
            break;
        case FA_IP:
            c = net_ip_conn_as_conn(burrow__net_ip_conn_from_file(a, &fs, &e));
            break;
        case FA_UNIX:
            c = net_unix_conn_as_conn(burrow__net_unix_conn_from_file(a, &fs, &e));
            break;
        case FA_NONE:
        default:
            e = fa_einval(&fs);
            break;
        }
    }
    BURROW_OUT(err, fa_error(f, e));
    return c;
}

NetListener net_file_listener(Alloc *a, OsFile *f, Error *err) {
    NetListener l = {NULL, NULL};
    burrow__NetFileSock fs;
    Error e = burrow__netfd_file_sock(f, &fs);
    if (BURROW_OK(e)) {
        switch (fa_kind(&fs)) {
        case FA_TCP:
            l = net_tcp_listener_as_listener(
                burrow__net_tcp_listener_from_file(a, &fs, &e));
            break;
        case FA_UNIX:
            l = net_unix_listener_as_listener(
                burrow__net_unix_listener_from_file(a, &fs, &e));
            break;
        case FA_UDP:
        case FA_IP:
        case FA_NONE:
        default:
            e = fa_einval(&fs);
            break;
        }
    }
    BURROW_OUT(err, fa_error(f, e));
    return l;
}

NetPacketConn net_file_packet_conn(Alloc *a, OsFile *f, Error *err) {
    NetPacketConn c = {NULL, NULL};
    burrow__NetFileSock fs;
    Error e = burrow__netfd_file_sock(f, &fs);
    if (BURROW_OK(e)) {
        switch (fa_kind(&fs)) {
        case FA_UDP:
            c = net_udp_conn_as_packet_conn(burrow__net_udp_conn_from_file(a, &fs, &e));
            break;
        case FA_IP:
            c = net_ip_conn_as_packet_conn(burrow__net_ip_conn_from_file(a, &fs, &e));
            break;
        case FA_UNIX:
            c = net_unix_conn_as_packet_conn(
                burrow__net_unix_conn_from_file(a, &fs, &e));
            break;
        case FA_TCP:
        case FA_NONE:
        default:
            e = fa_einval(&fs);
            break;
        }
    }
    BURROW_OUT(err, fa_error(f, e));
    return c;
}
