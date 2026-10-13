/* The system's resolver on everything but Windows: getaddrinfo, getnameinfo
 * and res_nsearch, which are the three calls Go's cgo resolver makes.
 *
 * libc asks NSS, mDNS, a VPN's resolver or whatever else the machine is set up
 * with, which is the whole reason to come here rather than read resolv.conf.
 * res_nsearch is the awkward one. Go calls res_search in its place on Linux
 * and OpenBSD, and glibc only has that in libc from 2.34, in libresolv before,
 * and macOS only has res_nsearch in libresolv. This layer links nothing beyond
 * libc, so it reaches them as weak symbols, and through dlopen on macOS, and
 * answers PAL_ENOSYS where it cannot.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(BURROW_OS_WASI)

/* wasip1 has no resolver to ask. net uses its own there, as Go does. */

static void pgai_nosys(PalLookupError *err) {
    if (err == NULL)
        return;
    memset(err, 0, sizeof *err);
    err->code = PAL_EAI_SYSTEM;
    err->err = PAL_ENOSYS;
}

int64_t pal_getaddrinfo(const char *host, const char *service, int32_t family,
                        int32_t socktype, int32_t protocol, int32_t flags,
                        PalAddrInfo *out, int64_t cap, PalLookupError *err) {
    (void)host;
    (void)service;
    (void)family;
    (void)socktype;
    (void)protocol;
    (void)flags;
    (void)out;
    (void)cap;
    pgai_nosys(err);
    return -1;
}

bool pal_getnameinfo(const PalSockAddr *addr, char *host, int64_t cap,
                     PalLookupError *err) {
    (void)addr;
    (void)host;
    (void)cap;
    pgai_nosys(err);
    return false;
}

int64_t pal_res_search(const char *name, int32_t rclass, int32_t rtype, uint8_t *ans,
                       int64_t cap, PalErrno *err) {
    (void)name;
    (void)rclass;
    (void)rtype;
    (void)ans;
    (void)cap;
    BURROW_OUT(err, PAL_ENOSYS);
    return -1;
}

#else

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
#include <dlfcn.h>
#endif

/* ------------------------------------------------------------------ errors */

static int32_t pgai_code(int rc) {
    if (rc == EAI_AGAIN)
        return PAL_EAI_AGAIN;
    if (rc == EAI_NONAME)
        return PAL_EAI_NONAME;
    if (rc == EAI_SERVICE)
        return PAL_EAI_SERVICE;
    if (rc == EAI_SYSTEM)
        return PAL_EAI_SYSTEM;
#if defined(EAI_OVERFLOW)
    if (rc == EAI_OVERFLOW)
        return PAL_EAI_OVERFLOW;
#endif
#if defined(EAI_NODATA)
    /* The same number as EAI_NONAME on some systems. */
    if (rc == EAI_NODATA && EAI_NODATA != EAI_NONAME)
        return PAL_EAI_NODATA;
#endif
#if defined(EAI_ADDRFAMILY)
    if (rc == EAI_ADDRFAMILY)
        return PAL_EAI_ADDRFAMILY;
#endif
    return PAL_EAI_OTHER;
}

/* rc and errno as ours. errno is only worth anything for EAI_SYSTEM, and Go
 * takes a zero there to mean the process is out of descriptors, which is
 * net's to decide and not this layer's. */
static void pgai_fail(int rc, int saved, PalLookupError *err) {
    if (err == NULL)
        return;
    memset(err, 0, sizeof *err);
    err->code = pgai_code(rc);
    if (rc == EAI_SYSTEM && saved != 0)
        err->err = burrow__pal_errno(saved);
    const char *text = gai_strerror(rc);
    if (text != NULL) {
        size_t n = strlen(text);
        if (n >= sizeof err->text)
            n = sizeof err->text - 1;
        memcpy(err->text, text, n);
    }
}

/* ------------------------------------------------------------- getaddrinfo */

static void pgai_from_native(const struct sockaddr *sa, socklen_t len,
                             PalSockAddr *out) {
    memset(out, 0, sizeof *out);
    if (sa == NULL)
        return;
    if (sa->sa_family == AF_INET && len >= (socklen_t)sizeof(struct sockaddr_in)) {
        struct sockaddr_in in;
        memcpy(&in, sa, sizeof in);
        out->family = PAL_AF_INET;
        out->port = ntohs(in.sin_port);
        memcpy(out->addr, &in.sin_addr, 4);
    } else if (sa->sa_family == AF_INET6 &&
               len >= (socklen_t)sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6 in6;
        memcpy(&in6, sa, sizeof in6);
        out->family = PAL_AF_INET6;
        out->port = ntohs(in6.sin6_port);
        out->scope_id = in6.sin6_scope_id;
        memcpy(out->addr, &in6.sin6_addr, 16);
    }
}

/* Ours into the platform's. An if and not a switch, for the same reason as in
 * net_posix.c: Cosmopolitan's constants are not known until the program
 * runs. */
static int pgai_int(int32_t v, int32_t a, int na, int32_t b, int nb, int32_t c,
                    int nc) {
    if (v == a)
        return na;
    if (v == b)
        return nb;
    if (v == c)
        return nc;
    return 0;
}

int64_t pal_getaddrinfo(const char *host, const char *service, int32_t family,
                        int32_t socktype, int32_t protocol, int32_t flags,
                        PalAddrInfo *out, int64_t cap, PalLookupError *err) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = pgai_int(family, PAL_AF_INET, AF_INET, PAL_AF_INET6, AF_INET6,
                               PAL_AF_UNSPEC, AF_UNSPEC);
    hints.ai_socktype = pgai_int(socktype, PAL_SOCK_STREAM, SOCK_STREAM, PAL_SOCK_DGRAM,
                                 SOCK_DGRAM, 0, 0);
    hints.ai_protocol = pgai_int(protocol, PAL_IPPROTO_TCP, IPPROTO_TCP,
                                 PAL_IPPROTO_UDP, IPPROTO_UDP, 0, 0);
    int f = 0;
    if ((flags & PAL_AI_CANONNAME) != 0)
        f |= AI_CANONNAME;
#if defined(AI_V4MAPPED)
    if ((flags & PAL_AI_V4MAPPED) != 0)
        f |= AI_V4MAPPED;
#endif
#if defined(AI_ALL)
    if ((flags & PAL_AI_ALL) != 0)
        f |= AI_ALL;
#endif
#if defined(AI_MASK)
    /* Go's darwin and BSD flags are masked the same way, since getaddrinfo
     * there refuses a flag outside it. */
    f &= AI_MASK;
#endif
    hints.ai_flags = f;

    struct addrinfo *res = NULL;
    errno = 0;
    int rc = getaddrinfo(host, service, &hints, &res);
    if (rc != 0) {
        pgai_fail(rc, errno, err);
        return -1;
    }
    int64_t n = 0;
    for (const struct addrinfo *r = res; r != NULL; r = r->ai_next, n++) {
        if (out == NULL || n >= cap)
            continue;
        PalAddrInfo *o = &out[n];
        pgai_from_native(r->ai_addr, r->ai_addrlen, &o->addr);
        o->socktype = r->ai_socktype == SOCK_STREAM  ? PAL_SOCK_STREAM
                      : r->ai_socktype == SOCK_DGRAM ? PAL_SOCK_DGRAM
                      : r->ai_socktype == SOCK_RAW   ? PAL_SOCK_RAW
                                                     : 0;
        o->protocol = r->ai_protocol;
    }
    freeaddrinfo(res);
    return n;
}

/* ------------------------------------------------------------- getnameinfo */

bool pal_getnameinfo(const PalSockAddr *addr, char *host, int64_t cap,
                     PalLookupError *err) {
    if (addr == NULL || host == NULL || cap <= 0 ||
        (addr->family != PAL_AF_INET && addr->family != PAL_AF_INET6)) {
        pgai_fail(EAI_FAMILY, 0, err);
        return false;
    }
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    socklen_t len;
    if (addr->family == PAL_AF_INET) {
        struct sockaddr_in in;
        memset(&in, 0, sizeof in);
        in.sin_family = (sa_family_t)AF_INET;
        in.sin_port = htons(addr->port);
        memcpy(&in.sin_addr, addr->addr, 4);
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || BURROW_BSD
        in.sin_len = (uint8_t)sizeof in;
#endif
        memcpy(&ss, &in, sizeof in);
        len = (socklen_t)sizeof in;
    } else {
        struct sockaddr_in6 in6;
        memset(&in6, 0, sizeof in6);
        in6.sin6_family = (sa_family_t)AF_INET6;
        in6.sin6_port = htons(addr->port);
        in6.sin6_scope_id = addr->scope_id;
        memcpy(&in6.sin6_addr, addr->addr, 16);
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || BURROW_BSD
        in6.sin6_len = (uint8_t)sizeof in6;
#endif
        memcpy(&ss, &in6, sizeof in6);
        len = (socklen_t)sizeof in6;
    }
    socklen_t hlen = cap > 0x10000 ? (socklen_t)0x10000 : (socklen_t)cap;
    errno = 0;
    int rc = getnameinfo((const struct sockaddr *)&ss, len, host, hlen, NULL, 0,
                         NI_NAMEREQD);
    if (rc != 0) {
        pgai_fail(rc, errno, err);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- res_nsearch */

/* Bigger than struct __res_state on any of the systems below, so that this
 * file needs no resolv.h, which musl and Cosmopolitan do not agree on. */
#define PRES_STATE 4096

typedef int (*PresInit)(void *);
typedef int (*PresNSearch)(void *, const char *, int, int, unsigned char *, int);
typedef void (*PresClose)(void *);
typedef int (*PresSearch)(const char *, int, int, unsigned char *, int);

/* The calls, or NULL for each one this system does not have. Linux and
 * OpenBSD get res_search, which keeps its state per thread there, and the
 * rest get res_ninit, res_nsearch and res_nclose, which is the split Go's
 * cgo_unix_cgo_res.go and cgo_unix_cgo_resn.go make. */
typedef struct PresFuncs {
    PresInit init;
    PresNSearch nsearch;
    PresClose close_;
    PresSearch search;
} PresFuncs;

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_OPENBSD)

/* Weak, so that a glibc older than 2.34, which keeps it in libresolv, or a
 * static link that did not pull it in, leaves it NULL rather than failing to
 * link. glibc 2.34 keeps its old __res_search only for programs already linked
 * against it, and res_search is the name a new program links to. musl has it
 * in libc. */
extern int res_search(const char *, int, int, unsigned char *, int)
    __attribute__((weak));

static void pres_funcs(PresFuncs *f) {
    memset(f, 0, sizeof *f);
    f->search = res_search;
}

#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)

/* libresolv, which is in the shared cache on every macOS and which Go links
 * for the same three calls. The library stays open, since the addresses are
 * only good while it is, and opening one already open costs a reference
 * count. */
static void pres_funcs(PresFuncs *f) {
    memset(f, 0, sizeof *f);
    void *lib = dlopen("/usr/lib/libresolv.9.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (lib == NULL)
        return;
    union {
        void *object;
        PresInit init;
        PresNSearch nsearch;
        PresClose close_;
    } u;
    u.object = dlsym(lib, "res_9_ninit");
    f->init = u.init;
    u.object = dlsym(lib, "res_9_nsearch");
    f->nsearch = u.nsearch;
    u.object = dlsym(lib, "res_9_nclose");
    f->close_ = u.close_;
}

#elif BURROW_BSD

/* resolv.h makes the three of them macros for these names. Weak for the
 * same reason as above. */
extern int __res_ninit(void *) __attribute__((weak));
extern int __res_nsearch(void *, const char *, int, int, unsigned char *, int)
    __attribute__((weak));
extern void __res_nclose(void *) __attribute__((weak));

static void pres_funcs(PresFuncs *f) {
    memset(f, 0, sizeof *f);
    f->init = __res_ninit;
    f->nsearch = __res_nsearch;
    f->close_ = __res_nclose;
}

#else

static void pres_funcs(PresFuncs *f) {
    memset(f, 0, sizeof *f);
}

#endif

int64_t pal_res_search(const char *name, int32_t rclass, int32_t rtype, uint8_t *ans,
                       int64_t cap, PalErrno *err) {
    PresFuncs f;
    pres_funcs(&f);
    if (f.search == NULL && (f.init == NULL || f.nsearch == NULL || f.close_ == NULL)) {
        BURROW_OUT(err, PAL_ENOSYS);
        return -1;
    }
    if (name == NULL || ans == NULL || cap <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    int len = cap > 0xffff ? 0xffff : (int)cap;
    int n;
    if (f.search != NULL) {
        n = f.search(name, (int)rclass, (int)rtype, ans, len);
    } else {
        /* Eight byte aligned, which is all the state's fields ask. */
        uint64_t state[PRES_STATE / sizeof(uint64_t)];
        memset(state, 0, sizeof state);
        errno = 0;
        if (f.init(state) != 0) {
            int saved = errno;
            BURROW_OUT(err, saved != 0 ? burrow__pal_errno(saved) : PAL_EOTHER);
            return -2;
        }
        n = f.nsearch(state, name, (int)rclass, (int)rtype, ans, len);
        f.close_(state);
    }
    if (n < 0) {
        BURROW_OUT(err, PAL_EOTHER);
        return -1;
    }
    BURROW_OUT(err, PAL_OK);
    return n;
}

#endif /* BURROW_OS_WASI */

#endif /* !BURROW_OS_WINDOWS */
