/* The network interfaces on Windows, from GetAdaptersAddresses.
 *
 * This is Go's interface_windows.go. Every call asks for the whole list,
 * starting from the 15000 bytes Microsoft suggests and growing the buffer for
 * as long as the answer is that it was too small, and then picks out what it
 * was asked for. An interface's index is IfIndex, or Ipv6IfIndex where an
 * adapter has no IPv4 side, and its flags are guessed from its media type,
 * which is what Go does too.
 *
 * GetAdaptersAddresses is in iphlpapi.dll, which is loaded when it is first
 * needed, as advapi32.dll is for the registry, so that nothing that does not
 * ask for interfaces links against it.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0601
#endif

#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>
#include <string.h>

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on

typedef ULONG(WINAPI *GetAdaptersAddressesFn)(ULONG, ULONG, PVOID,
                                              PIP_ADAPTER_ADDRESSES, PULONG);

/* The list and the library it came from, freed together. */
typedef struct IwList {
    HMODULE lib;
    IP_ADAPTER_ADDRESSES *head;
} IwList;

static void iw_fail(PalIfError *err, const char *call, PalErrno e) {
    if (err != NULL) {
        err->call = call;
        err->err = e;
    }
}

static void iw_free(IwList *l) {
    if (l->head != NULL)
        HeapFree(GetProcessHeap(), 0, l->head);
    if (l->lib != NULL)
        FreeLibrary(l->lib);
}

/* adapterAddresses. head is NULL when there are no adapters at all. */
static bool iw_list(IwList *l, PalIfError *err) {
    memset(l, 0, sizeof *l);
    l->lib = LoadLibraryW(L"iphlpapi.dll");
    if (l->lib == NULL) {
        iw_fail(err, "getadaptersaddresses", burrow__pal_errno_win(GetLastError()));
        return false;
    }
    GetAdaptersAddressesFn get = (GetAdaptersAddressesFn)(void (*)(void))GetProcAddress(
        l->lib, "GetAdaptersAddresses");
    if (get == NULL) {
        iw_fail(err, "getadaptersaddresses", burrow__pal_errno_win(GetLastError()));
        iw_free(l);
        return false;
    }
    ULONG n = 15000;
    for (;;) {
        ULONG have = n;
        IP_ADAPTER_ADDRESSES *b =
            (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, have);
        if (b == NULL) {
            iw_fail(err, "getadaptersaddresses", PAL_ENOMEM);
            iw_free(l);
            return false;
        }
        ULONG r = get(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS,
                      NULL, b, &n);
        if (r == ERROR_SUCCESS) {
            if (n == 0)
                HeapFree(GetProcessHeap(), 0, b);
            else
                l->head = b;
            return true;
        }
        HeapFree(GetProcessHeap(), 0, b);
        if (r != ERROR_BUFFER_OVERFLOW || n <= have) {
            iw_fail(err, "getadaptersaddresses", burrow__pal_errno_win(r));
            iw_free(l);
            return false;
        }
    }
}

static int32_t iw_index(const IP_ADAPTER_ADDRESSES *aa) {
    return aa->IfIndex != 0 ? (int32_t)aa->IfIndex : (int32_t)aa->Ipv6IfIndex;
}

/* FriendlyName, with an unpaired surrogate as U+FFFD, which is what Go's
 * UTF16PtrToString makes of one. */
static void iw_name(PWCHAR w, char *out) {
    size_t n = 0;
    while (w != NULL && w[n] != 0)
        n++;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)w[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && (uint32_t)w[i + 1] >= 0xdc00 &&
            (uint32_t)w[i + 1] < 0xe000) {
            i++;
            continue;
        }
        if (c >= 0xd800 && c < 0xe000)
            w[i] = (wchar_t)0xfffd;
    }
    int64_t m = n == 0 ? 0 : burrow__pal_narrow(w, n, out, PAL_NAME_MAX);
    out[m < 0 ? 0 : m] = 0;
}

int64_t pal_if_enumerate(int32_t index, PalInterface *out, int64_t cap,
                         PalIfError *err) {
    IwList l;
    if (!iw_list(&l, err))
        return -1;
    int64_t count = 0;
    for (IP_ADAPTER_ADDRESSES *aa = l.head; aa != NULL; aa = aa->Next) {
        int32_t ix = iw_index(aa);
        if (index != 0 && index != ix)
            continue;
        if (count < cap) {
            PalInterface *ifi = &out[count];
            memset(ifi, 0, sizeof *ifi);
            ifi->index = ix;
            iw_name(aa->FriendlyName, ifi->name);
            if (aa->OperStatus == IfOperStatusUp)
                ifi->flags |= PAL_IFF_UP | PAL_IFF_RUNNING;
            switch (aa->IfType) {
            case IF_TYPE_ETHERNET_CSMACD:
            case IF_TYPE_ISO88025_TOKENRING:
            case IF_TYPE_IEEE80211:
            case IF_TYPE_IEEE1394:
                ifi->flags |= PAL_IFF_BROADCAST | PAL_IFF_MULTICAST;
                break;
            case IF_TYPE_PPP:
            case IF_TYPE_TUNNEL:
                ifi->flags |= PAL_IFF_POINTTOPOINT | PAL_IFF_MULTICAST;
                break;
            case IF_TYPE_SOFTWARE_LOOPBACK:
                ifi->flags |= PAL_IFF_LOOPBACK | PAL_IFF_MULTICAST;
                break;
            case IF_TYPE_ATM:
                ifi->flags |=
                    PAL_IFF_BROADCAST | PAL_IFF_POINTTOPOINT | PAL_IFF_MULTICAST;
                break;
            default:
                break;
            }
            ifi->mtu = aa->Mtu == 0xffffffffu ? -1 : (int32_t)aa->Mtu;
            size_t alen = aa->PhysicalAddressLength;
            if (alen > sizeof aa->PhysicalAddress)
                alen = sizeof aa->PhysicalAddress;
            memcpy(ifi->hwaddr, aa->PhysicalAddress, alen);
            ifi->hwaddr_len = (int32_t)alen;
        }
        count++;
        if (index == ix)
            break;
    }
    iw_free(&l);
    return count;
}

static void iw_mask(uint8_t *mask, int bits) {
    for (int i = 0; i < bits / 8; i++)
        mask[i] = 0xff;
    if (bits % 8 != 0)
        mask[bits / 8] = (uint8_t)(0xff << (8 - bits % 8));
}

/* One of an adapter's addresses into *ifa, in Sockaddr's terms: false with
 * *bad set for a family Go cannot turn into a Sockaddr, and false alone for an
 * AF_UNIX one, which it can but which the callers skip. */
static bool iw_addr(const SOCKET_ADDRESS *sa, PalIfAddr *ifa, bool *bad) {
    const struct sockaddr *p = sa->lpSockaddr;
    if (p == NULL) {
        *bad = true;
        return false;
    }
    if (p->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)p;
        ifa->family = PAL_AF_INET;
        memcpy(ifa->addr, &in->sin_addr, 4);
        return true;
    }
    if (p->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)(const void *)p;
        ifa->family = PAL_AF_INET6;
        memcpy(ifa->addr, &in6->sin6_addr, 16);
        return true;
    }
    if (p->sa_family != AF_UNIX)
        *bad = true;
    return false;
}

/* The unicast, anycast or multicast addresses of the adapters with this
 * index, or of all of them for 0. */
typedef enum { IW_UNICAST, IW_ANYCAST, IW_MULTICAST } IwWhich;

static bool iw_collect(const IP_ADAPTER_ADDRESSES *aa, IwWhich which, int32_t ix,
                       PalIfAddr *out, int64_t cap, int64_t *count) {
    const void *p;
    switch (which) {
    case IW_UNICAST:
        p = aa->FirstUnicastAddress;
        break;
    case IW_ANYCAST:
        p = aa->FirstAnycastAddress;
        break;
    case IW_MULTICAST:
    default:
        p = aa->FirstMulticastAddress;
        break;
    }
    while (p != NULL) {
        const SOCKET_ADDRESS *sa;
        const void *next;
        int bits = 0;
        switch (which) {
        case IW_UNICAST: {
            const IP_ADAPTER_UNICAST_ADDRESS *u = (const IP_ADAPTER_UNICAST_ADDRESS *)p;
            sa = &u->Address;
            next = u->Next;
            bits = (int)u->OnLinkPrefixLength;
            break;
        }
        case IW_ANYCAST: {
            const IP_ADAPTER_ANYCAST_ADDRESS *a = (const IP_ADAPTER_ANYCAST_ADDRESS *)p;
            sa = &a->Address;
            next = a->Next;
            break;
        }
        case IW_MULTICAST:
        default: {
            const IP_ADAPTER_MULTICAST_ADDRESS *m =
                (const IP_ADAPTER_MULTICAST_ADDRESS *)p;
            sa = &m->Address;
            next = m->Next;
            break;
        }
        }
        PalIfAddr ifa;
        memset(&ifa, 0, sizeof ifa);
        bool bad = false;
        if (iw_addr(sa, &ifa, &bad)) {
            ifa.index = ix;
            if (which == IW_UNICAST) {
                ifa.kind = PAL_IFA_NET;
                int max = ifa.family == PAL_AF_INET ? 32 : 128;
                /* CIDRMask is nil past the end of the address. */
                if (bits <= max) {
                    ifa.mask_len = max / 8;
                    iw_mask(ifa.mask, bits);
                }
            } else {
                ifa.kind = PAL_IFA_ADDR;
            }
            if (*count < cap)
                out[*count] = ifa;
            (*count)++;
        } else if (bad) {
            return false;
        }
        p = next;
    }
    return true;
}

static int64_t iw_addrs(int32_t index, bool multicast, PalIfAddr *out, int64_t cap,
                        PalIfError *err) {
    IwList l;
    if (!iw_list(&l, err))
        return -1;
    int64_t count = 0;
    for (IP_ADAPTER_ADDRESSES *aa = l.head; aa != NULL; aa = aa->Next) {
        int32_t ix = iw_index(aa);
        if (index != 0 && index != ix)
            continue;
        bool ok;
        if (multicast)
            ok = iw_collect(aa, IW_MULTICAST, ix, out, cap, &count);
        else
            ok = iw_collect(aa, IW_UNICAST, ix, out, cap, &count) &&
                 iw_collect(aa, IW_ANYCAST, ix, out, cap, &count);
        if (!ok) {
            iw_free(&l);
            iw_fail(err, "sockaddr", PAL_EAFNOSUPPORT);
            return -1;
        }
    }
    iw_free(&l);
    return count;
}

int64_t pal_if_addrs(int32_t index, PalIfAddr *out, int64_t cap, PalIfError *err) {
    return iw_addrs(index, false, out, cap, err);
}

int64_t pal_if_multicast_addrs(int32_t index, PalIfAddr *out, int64_t cap,
                               PalIfError *err) {
    return iw_addrs(index, true, out, cap, err);
}

#endif /* BURROW_OS_WINDOWS */
