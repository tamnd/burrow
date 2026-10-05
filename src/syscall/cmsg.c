/* Socket control messages: CmsgLen and CmsgSpace, parsing the messages
 * Recvmsg gives back, and SCM_RIGHTS and, on Linux, SCM_CREDENTIALS.
 *
 * Derived from Go's src/syscall/sockcmsg_unix.go, sockcmsg_unix_other.go and
 * sockcmsg_linux.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/mem.h"
#include "burrow/slice.h"

#include "internal.h"

#include <string.h>

static const Type cmsg_desc = {
    {(const Byte *)"SocketControlMessage", 20},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallSocketControlMessage),
    (uint16_t)_Alignof(SyscallSocketControlMessage),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73636d67U, /* "scmg" */
    NULL,
};

const Type *const TYPE_SYSCALL_SOCKET_CONTROL_MESSAGE = &cmsg_desc;

/* Rounds n up to the alignment the system puts control messages at: a
 * pointer's, except on 64-bit macOS, where it is 4. */
static Int cmsg_align(Int n) {
#if (defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)) && UINTPTR_MAX > 0xFFFFFFFFU
    const Int salign = 4;
#else
    const Int salign = (Int)sizeof(void *);
#endif
    return (n + salign - 1) & ~(salign - 1);
}

Int syscall_cmsg_len(Int datalen) {
    return cmsg_align(SYSCALL_SIZEOF_CMSGHDR) + datalen;
}

Int syscall_cmsg_space(Int datalen) {
    return cmsg_align(SYSCALL_SIZEOF_CMSGHDR) + cmsg_align(datalen);
}

/* The header at the start of b, copied out since b need not be aligned for
 * it, and the data after it, which the header's length says how much of. */
static Error cmsg_header_and_data(const Byte *b, Int n, SyscallCmsghdr *h,
                                  Slice *data) {
    memcpy(h, b, sizeof *h);
    /* len is 32 bits on some systems and 64 on others. */
    uint64_t hlen = h->len;
    if (hlen < SYSCALL_SIZEOF_CMSGHDR || hlen > (uint64_t)n)
        return burrow__syscall_errno_err(SYSCALL_EINVAL);
    Int off = cmsg_align(SYSCALL_SIZEOF_CMSGHDR);
    Int len = (Int)h->len - off;
    if (len < 0)
        len = 0;
    *data = (Slice){(void *)(uintptr_t)(b + off), len, len, TYPE_BYTE};
    return BURROW_NO_ERROR;
}

Slice syscall_parse_socket_control_message(Alloc *a, Slice b, Error *err) {
    const Byte *p = (const Byte *)b.p;
    Slice nil = slice_nil(TYPE_SYSCALL_SOCKET_CONTROL_MESSAGE);
    /* Once to check them and count them, and again to fill them in. */
    Int count = 0;
    for (Int i = 0; i + syscall_cmsg_len(0) <= b.len; count++) {
        SyscallCmsghdr h;
        Slice data;
        Error e = cmsg_header_and_data(p + i, b.len - i, &h, &data);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return nil;
        }
        i += cmsg_align((Int)h.len);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (count == 0)
        return nil;
    SyscallSocketControlMessage *msgs = (SyscallSocketControlMessage *)mem_alloc_array(
        a, (size_t)count, sizeof *msgs, _Alignof(SyscallSocketControlMessage));
    if (msgs == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    Int i = 0;
    for (Int k = 0; k < count; k++) {
        (void)cmsg_header_and_data(p + i, b.len - i, &msgs[k].header, &msgs[k].data);
        i += cmsg_align((Int)msgs[k].header.len);
    }
    return (Slice){msgs, count, count, TYPE_SYSCALL_SOCKET_CONTROL_MESSAGE};
}

/* A zeroed buffer from a for one message of datalen bytes of data, with its
 * header filled in, or a nil slice. */
static Slice cmsg_new(Alloc *a, Int level, Int typ, Int datalen) {
    Int space = syscall_cmsg_space(datalen);
    Byte *b = (Byte *)mem_alloc(a, (size_t)space, _Alignof(SyscallCmsghdr));
    if (b == NULL)
        return slice_nil(TYPE_BYTE);
    SyscallCmsghdr h;
    memset(&h, 0, sizeof h);
    h.level = (int32_t)level;
    h.type = (int32_t)typ;
    syscall_cmsghdr_set_len(&h, syscall_cmsg_len(datalen));
    memcpy(b, &h, sizeof h);
    return (Slice){b, space, space, TYPE_BYTE};
}

Slice syscall_unix_rights(Alloc *a, Slice fds) {
    const Int *fd = (const Int *)fds.p;
    Slice b = cmsg_new(a, SYSCALL_SOL_SOCKET, SYSCALL_SCM_RIGHTS, fds.len * 4);
    if (b.p == NULL)
        return b;
    Byte *data = (Byte *)b.p + cmsg_align(SYSCALL_SIZEOF_CMSGHDR);
    for (Int i = 0; i < fds.len; i++) {
        int32_t v = (int32_t)fd[i];
        memcpy(data + 4 * i, &v, 4);
    }
    return b;
}

/* Go makes a descriptor of each four bytes and would read past the end of a
 * message whose length is not a multiple of four. Here the bytes left over
 * are left out. */
Slice syscall_parse_unix_rights(Alloc *a, SyscallSocketControlMessage *m, Error *err) {
    Slice nil = slice_nil(TYPE_INT);
    if (m->header.level != SYSCALL_SOL_SOCKET || m->header.type != SYSCALL_SCM_RIGHTS) {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
        return nil;
    }
    Int n = m->data.len >> 2;
    Int *fds =
        (Int *)mem_alloc_array(a, (size_t)(n > 0 ? n : 1), sizeof *fds, _Alignof(Int));
    if (fds == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    const Byte *p = (const Byte *)m->data.p;
    for (Int i = 0; i < n; i++) {
        int32_t v;
        memcpy(&v, p + 4 * i, 4);
        fds[i] = v;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (Slice){fds, n, n > 0 ? n : 1, TYPE_INT};
}

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)
Slice syscall_unix_credentials(Alloc *a, SyscallUcred *ucred) {
    Slice b =
        cmsg_new(a, SYSCALL_SOL_SOCKET, SYSCALL_SCM_CREDENTIALS, SYSCALL_SIZEOF_UCRED);
    if (b.p != NULL)
        memcpy((Byte *)b.p + cmsg_align(SYSCALL_SIZEOF_CMSGHDR), ucred, sizeof *ucred);
    return b;
}

SyscallUcred syscall_parse_unix_credentials(SyscallSocketControlMessage *m,
                                            Error *err) {
    SyscallUcred ucred;
    memset(&ucred, 0, sizeof ucred);
    if (m->header.level != SYSCALL_SOL_SOCKET ||
        m->header.type != SYSCALL_SCM_CREDENTIALS ||
        (size_t)m->data.len < sizeof ucred) {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
        return ucred;
    }
    memcpy(&ucred, m->data.p, sizeof ucred);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ucred;
}
#endif

#endif
