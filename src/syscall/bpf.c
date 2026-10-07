/* The BSD packet filter on macOS and FreeBSD, one ioctl(2) each.
 *
 * Go hands the kernel a pointer to an int where it reads and writes a u_int,
 * which works because the int is zeroed first and the machines are little
 * endian. This does the same with an Int, so the values match Go's.
 *
 * Derived from Go's src/syscall/bpf_bsd.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_FREEBSD)

#include "internal.h"

#include <string.h>

SyscallBpfInsn syscall_bpf_stmt(Int code, Int k) {
    return (SyscallBpfInsn){.code = (uint16_t)code, .k = (uint32_t)k};
}

SyscallBpfInsn syscall_bpf_jump(Int code, Int k, Int jt, Int jf) {
    return (SyscallBpfInsn){
        .code = (uint16_t)code, .jt = (uint8_t)jt, .jf = (uint8_t)jf, .k = (uint32_t)k};
}

/* The ioctl req on fd with an Int that starts as v, and what it ends as. */
static Int bpf_int(Int fd, Uint req, Int v, Error *err) {
    Error e = burrow__syscall_ioctl_ptr(fd, req, &v);
    BURROW_OUT(err, e);
    return BURROW_FAILED(e) ? 0 : v;
}

Int syscall_bpf_buflen(Int fd, Error *err) {
    return bpf_int(fd, SYSCALL_BIOCGBLEN, 0, err);
}

Int syscall_set_bpf_buflen(Int fd, Int l, Error *err) {
    return bpf_int(fd, SYSCALL_BIOCSBLEN, l, err);
}

Int syscall_bpf_datalink(Int fd, Error *err) {
    return bpf_int(fd, SYSCALL_BIOCGDLT, 0, err);
}

Int syscall_set_bpf_datalink(Int fd, Int t, Error *err) {
    return bpf_int(fd, SYSCALL_BIOCSDLT, t, err);
}

Error syscall_set_bpf_promisc(Int fd, Int m) {
    return burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCPROMISC, &m);
}

Error syscall_flush_bpf(Int fd) {
    return burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCFLUSH, NULL);
}

/* Go's ivalue, the struct ifreq the kernel reads the interface name from. */
typedef struct BpfIvalue {
    uint8_t name[SYSCALL_IFNAMSIZ];
    int16_t value;
} BpfIvalue;

Str syscall_bpf_interface(Int fd, Str name, Error *err) {
    BpfIvalue iv;
    memset(&iv, 0, sizeof iv);
    Error e = burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCGETIF, &iv);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e))
        return (Str){NULL, 0};
    return name;
}

Error syscall_set_bpf_interface(Int fd, Str name) {
    BpfIvalue iv;
    memset(&iv, 0, sizeof iv);
    if (name.len > 0)
        memcpy(
            iv.name, name.p,
            (size_t)(name.len < (Int)sizeof iv.name ? name.len : (Int)sizeof iv.name));
    return burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCSETIF, &iv);
}

SyscallTimeval syscall_bpf_timeout(Int fd, Error *err) {
    SyscallTimeval tv;
    memset(&tv, 0, sizeof tv);
    Error e = burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCGRTIMEOUT, &tv);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e))
        memset(&tv, 0, sizeof tv);
    return tv;
}

Error syscall_set_bpf_timeout(Int fd, SyscallTimeval *tv) {
    return burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCSRTIMEOUT, tv);
}

SyscallBpfStat syscall_bpf_stats(Int fd, Error *err) {
    SyscallBpfStat s;
    memset(&s, 0, sizeof s);
    Error e = burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCGSTATS, &s);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e))
        memset(&s, 0, sizeof s);
    return s;
}

Error syscall_set_bpf_immediate(Int fd, Int m) {
    return burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCIMMEDIATE, &m);
}

/* Go takes &i[0] and panics on an empty program. This passes it on with no
 * instructions and lets the kernel say what it thinks. */
Error syscall_set_bpf(Int fd, Slice i) {
    SyscallBpfProgram p;
    memset(&p, 0, sizeof p);
    p.len = (uint32_t)i.len;
    p.insns = i.len > 0 ? (SyscallBpfInsn *)i.p : NULL;
    return burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCSETF, &p);
}

Error syscall_check_bpf_version(Int fd) {
    SyscallBpfVersion v;
    memset(&v, 0, sizeof v);
    Error e = burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCVERSION, &v);
    if (BURROW_FAILED(e))
        return e;
    if (v.major_ != SYSCALL_BPF_MAJOR_VERSION || v.minor_ != SYSCALL_BPF_MINOR_VERSION)
        return burrow__syscall_errno_err(SYSCALL_EINVAL);
    return BURROW_NO_ERROR;
}

Int syscall_bpf_headercmpl(Int fd, Error *err) {
    return bpf_int(fd, SYSCALL_BIOCGHDRCMPLT, 0, err);
}

Error syscall_set_bpf_headercmpl(Int fd, Int f) {
    return burrow__syscall_ioctl_ptr(fd, SYSCALL_BIOCSHDRCMPLT, &f);
}

#endif
