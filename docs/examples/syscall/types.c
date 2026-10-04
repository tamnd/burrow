#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"
#include "burrow/syscall.h"

int main(void) {
    // doc: sockaddr
    SyscallRawSockaddrInet4 sa;
    memset(&sa, 0, sizeof sa);
    sa.family = SYSCALL_AF_INET;
    uint8_t *port = (uint8_t *)&sa.port; /* big endian, as the system wants it */
    port[0] = 8080 >> 8;
    port[1] = 8080 & 0xff;
    sa.addr[0] = 127;
    sa.addr[3] = 1;
    // doc: end
#if !defined(BURROW_OS_WINDOWS)
    // doc: stat
    Int size = (Int)sizeof(SyscallStat_t); /* 144 on Linux on amd64, 128 on arm64 */
    // doc: end
    printf("%d\n", (int)size);
#endif
    printf("%d.%d.%d.%d:%d\n", sa.addr[0], sa.addr[1], sa.addr[2], sa.addr[3],
           port[0] << 8 | port[1]);
    return sizeof sa == 16 ? 0 : 1;
}
