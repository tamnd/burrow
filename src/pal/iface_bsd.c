/* The network interfaces on macOS and the BSDs, from getifaddrs.
 *
 * Go reads the NET_RT_IFLIST sysctl itself, through internal/routebsd, and
 * getifaddrs reads the same sysctl and hands back the same records, in the
 * same order, with the work of walking each system's message layout done by
 * the system's own libc. What is left is what Go does with the addresses on
 * top of the layout, which is here: the link record of each interface gives
 * its index, its hardware address and its MTU, a kernel's link local IPv6
 * address loses the zone it carries inside it, and a netmask in the kernel's
 * short form, with no family, is read the way routebsd reads it.
 *
 * The multicast groups come from getifmaddrs, which reads NET_RT_IFLIST2 on
 * macOS and NET_RT_IFMALIST on FreeBSD, which are the sysctls Go reads there.
 * Go hands back none on NetBSD, OpenBSD and DragonFly, and so does this.
 *
 * Go gives a failure here as the bare errno, so call is NULL.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_OPENBSD) || defined(BURROW_OS_DRAGONFLY)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

/* What a sockaddr in a routing message is padded to, which is routebsd's
 * kernelAlign. */
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
#define IFB_ALIGN 4
#elif defined(BURROW_OS_NETBSD)
#define IFB_ALIGN 8
#else
#define IFB_ALIGN ((int)sizeof(long))
#endif

static int ifb_roundup(int l) {
    if (l == 0)
        return IFB_ALIGN;
    return (l + IFB_ALIGN - 1) & ~(IFB_ALIGN - 1);
}

static void ifb_fail(PalIfError *err, int native) {
    if (err != NULL) {
        err->call = NULL;
        err->err = burrow__pal_errno(native);
    }
}

static uint32_t ifb_flags(unsigned int raw) {
    uint32_t f = 0;
    if ((raw & IFF_UP) != 0)
        f |= PAL_IFF_UP;
    if ((raw & IFF_RUNNING) != 0)
        f |= PAL_IFF_RUNNING;
    if ((raw & IFF_BROADCAST) != 0)
        f |= PAL_IFF_BROADCAST;
    if ((raw & IFF_LOOPBACK) != 0)
        f |= PAL_IFF_LOOPBACK;
    if ((raw & IFF_POINTOPOINT) != 0)
        f |= PAL_IFF_POINTTOPOINT;
    if ((raw & IFF_MULTICAST) != 0)
        f |= PAL_IFF_MULTICAST;
    return f;
}

/* The index of the interface called name, from its link record in list. */
static int32_t ifb_index(const struct ifaddrs *list, const char *name) {
    for (const struct ifaddrs *p = list; p != NULL; p = p->ifa_next) {
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_LINK)
            continue;
        if (strcmp(p->ifa_name, name) == 0)
            return (int32_t)((const struct sockaddr_dl *)(const void *)p->ifa_addr)
                ->sdl_index;
    }
    return 0;
}

/* routebsd's parseInetAddr: the address, as many bytes of it as sa_len says
 * there are, and the zone a KAME stack keeps in the second word of a link or
 * interface local IPv6 address taken out. */
static bool ifb_inet(const struct sockaddr *sa, int32_t *family, uint8_t *ip) {
    const uint8_t *b = (const uint8_t *)sa;
    int l = (int)sa->sa_len;
    if (sa->sa_family == AF_INET) {
        memset(ip, 0, 4);
        if (l != 0) {
            int n = l < 8 ? l : 8;
            if (n > 4)
                memcpy(ip, b + 4, (size_t)(n - 4));
        }
        *family = PAL_AF_INET;
        return true;
    }
    if (sa->sa_family == AF_INET6) {
        memset(ip, 0, 16);
        if (l != 0) {
            int n = l < 24 ? l : 24;
            if (n > 8)
                memcpy(ip, b + 8, (size_t)(n - 8));
            if ((ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80) ||
                (ip[0] == 0xff && ((ip[1] & 0x0f) == 0x01 || (ip[1] & 0x0f) == 0x02))) {
                ip[2] = 0;
                ip[3] = 0;
            }
        }
        *family = PAL_AF_INET6;
        return true;
    }
    return false;
}

/* routebsd's parseKernelInetAddr, for a netmask with no family, which is the
 * kernel's short form: a length in bytes and the leading bytes of the mask
 * after it. The family it is read for is the one of the last address before
 * it in the message, and nothing comes before the netmask in an address
 * message, so it is always AF_UNSPEC there, which reads as IPv4 unless the
 * length is a whole IPv6 sockaddr's. */
static int32_t ifb_kernel_mask(const struct sockaddr *sa, uint8_t *mask) {
    const uint8_t *b = (const uint8_t *)sa;
    int l = ifb_roundup((int)b[0]);
    if (b[0] == sizeof(struct sockaddr_in6)) {
        memcpy(mask, b + 8, 16);
        return 16;
    }
    if (b[0] == sizeof(struct sockaddr_in)) {
        memcpy(mask, b + 4, 4);
        return 4;
    }
    memset(mask, 0, 4);
    if (l - 1 < 4)
        memcpy(mask, b + 1, (size_t)(l - 1));
    else
        memcpy(mask, b + l - 4, 4);
    return 4;
}

static int32_t ifb_mask(const struct sockaddr *sa, uint8_t *mask) {
    if (sa == NULL)
        return 0;
    if (sa->sa_family == AF_INET || sa->sa_family == AF_INET6) {
        int32_t fam = 0;
        (void)ifb_inet(sa, &fam, mask);
        return fam == PAL_AF_INET ? 4 : 16;
    }
    if (sa->sa_family == AF_LINK)
        return 0;
    return ifb_kernel_mask(sa, mask);
}

int64_t pal_if_enumerate(int32_t index, PalInterface *out, int64_t cap,
                         PalIfError *err) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) {
        ifb_fail(err, errno);
        return -1;
    }
    int64_t count = 0;
    for (const struct ifaddrs *p = list; p != NULL; p = p->ifa_next) {
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_LINK)
            continue;
        const struct sockaddr_dl *dl =
            (const struct sockaddr_dl *)(const void *)p->ifa_addr;
        if (index != 0 && index != (int32_t)dl->sdl_index)
            continue;
        if (count < cap) {
            PalInterface *ifi = &out[count];
            memset(ifi, 0, sizeof *ifi);
            ifi->index = (int32_t)dl->sdl_index;
            size_t n = strlen(p->ifa_name);
            if (n > PAL_NAME_MAX)
                n = PAL_NAME_MAX;
            memcpy(ifi->name, p->ifa_name, n);
            ifi->flags = ifb_flags(p->ifa_flags);
            size_t alen = dl->sdl_alen;
            if (alen > sizeof ifi->hwaddr)
                alen = sizeof ifi->hwaddr;
            memcpy(ifi->hwaddr, dl->sdl_data + dl->sdl_nlen, alen);
            ifi->hwaddr_len = (int32_t)alen;
            if (p->ifa_data != NULL)
                ifi->mtu = (int32_t)((const struct if_data *)p->ifa_data)->ifi_mtu;
        }
        count++;
        if (index != 0)
            break;
    }
    freeifaddrs(list);
    return count;
}

int64_t pal_if_addrs(int32_t index, PalIfAddr *out, int64_t cap, PalIfError *err) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) {
        ifb_fail(err, errno);
        return -1;
    }
    int64_t count = 0;
    for (const struct ifaddrs *p = list; p != NULL; p = p->ifa_next) {
        if (p->ifa_addr == NULL)
            continue;
        PalIfAddr ifa;
        memset(&ifa, 0, sizeof ifa);
        if (!ifb_inet(p->ifa_addr, &ifa.family, ifa.addr))
            continue;
        /* Go keeps an address only when it has a netmask to go with it. */
        ifa.mask_len = ifb_mask(p->ifa_netmask, ifa.mask);
        if (ifa.mask_len == 0)
            continue;
        ifa.index = ifb_index(list, p->ifa_name);
        if (index != 0 && index != ifa.index)
            continue;
        ifa.kind = PAL_IFA_NET;
        if (count < cap)
            out[count] = ifa;
        count++;
    }
    freeifaddrs(list);
    return count;
}

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_FREEBSD)

int64_t pal_if_multicast_addrs(int32_t index, PalIfAddr *out, int64_t cap,
                               PalIfError *err) {
    struct ifmaddrs *list = NULL;
    if (getifmaddrs(&list) != 0) {
        ifb_fail(err, errno);
        return -1;
    }
    int64_t count = 0;
    for (const struct ifmaddrs *p = list; p != NULL; p = p->ifma_next) {
        if (p->ifma_name == NULL || p->ifma_name->sa_family != AF_LINK ||
            p->ifma_addr == NULL)
            continue;
        const struct sockaddr_dl *dl =
            (const struct sockaddr_dl *)(const void *)p->ifma_name;
        if (index != (int32_t)dl->sdl_index)
            continue;
        PalIfAddr ifa;
        memset(&ifa, 0, sizeof ifa);
        if (!ifb_inet(p->ifma_addr, &ifa.family, ifa.addr))
            continue;
        ifa.index = index;
        ifa.kind = PAL_IFA_ADDR;
        if (count < cap)
            out[count] = ifa;
        count++;
    }
    freeifmaddrs(list);
    return count;
}

#else

int64_t pal_if_multicast_addrs(int32_t index, PalIfAddr *out, int64_t cap,
                               PalIfError *err) {
    (void)index;
    (void)out;
    (void)cap;
    (void)err;
    return 0;
}

#endif

#endif /* the BSDs */
