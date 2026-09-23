#include <stdio.h>
#include <stdlib.h>

#include "burrow/burrow.h"

// Stand-ins for the one place an embedded target has to leave a last message,
// and for the way it starts again.
static void my_log_write(const Byte *p, Int n) {
    printf("log: %.*s\n", (int)n, (const char *)p);
}

static void my_reboot(void) {
    fflush(stdout);
    exit(0);
}

// doc: handler
static void to_the_log(Str msg) {
    my_log_write(msg.p, msg.len);
    my_reboot();
}

int main(void) {
    runtime_set_fatal_handler(to_the_log);
    runtime_throw(BURROW_S("the queue holds more than its capacity"));
}
// doc: end

/* Output:
log: the queue holds more than its capacity
*/
