#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/syscall.h"

int main(void) {
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_FREEBSD)
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: sysctl
    Error err = BURROW_NO_ERROR;
    Str os = syscall_sysctl(a, BURROW_S("kern.ostype"), &err);
    uint32_t maxproc = syscall_sysctl_uint32(BURROW_S("kern.maxproc"), &err);
    if (BURROW_OK(err))
        printf("%.*s, up to %u processes\n", (int)os.len, (const char *)os.p,
               (unsigned)maxproc);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;

    // doc: kevent
    Int kq = syscall_kqueue(&err);
    Int p[2];
    err = syscall_pipe((Slice){p, 2, 2, TYPE_INT});
    SyscallKevent_t change = {0}, ev = {0};
    syscall_set_kevent(&change, p[0], SYSCALL_EVFILT_READ, SYSCALL_EV_ADD);
    (void)syscall_kevent(kq, (Slice){&change, 1, 1, NULL}, slice_nil(NULL), NULL, &err);
    Byte hi[2] = {'h', 'i'};
    (void)syscall_write(p[1], (Slice){hi, 2, 2, TYPE_BYTE}, &err);
    SyscallTimespec second = syscall_nsec_to_timespec(1000000000);
    Int n =
        syscall_kevent(kq, slice_nil(NULL), (Slice){&ev, 1, 1, NULL}, &second, &err);
    if (n == 1)
        printf("%d bytes to read\n", (int)ev.data);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;

    // doc: rib
    Slice tab = syscall_route_rib(a, SYSCALL_NET_RT_IFLIST, 0, &err);
    Slice msgs = syscall_parse_routing_message(a, tab, &err);
    SyscallRoutingMessage *ms = (SyscallRoutingMessage *)msgs.p;
    for (Int i = 0; i < msgs.len; i++) {
        if (ms[i].vt->self_type != TYPE_SYSCALL_INTERFACE_MESSAGE)
            continue;
        Slice sas = syscall_parse_routing_sockaddr(a, ms[i], &err);
        if (sas.len == 0)
            continue;
        SyscallSockaddr ifp = ((SyscallSockaddr *)sas.p)[SYSCALL_RTAX_IFP];
        SyscallSockaddrDatalink *dl = (SyscallSockaddrDatalink *)ifp.data;
        printf("interface %d is %.*s\n", (int)dl->index, (int)dl->nlen,
               (const char *)dl->data);
    }
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    arena_free(&ar);
#endif
    return 0;
}
