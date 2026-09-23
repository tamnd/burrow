#include <stdio.h>

#include "burrow/burrow.h"

int main(void) {
    // doc: walk
    Uintptr pcs[32];
    Int n = runtime_callers(0, slice_from(pcs, 32, 32, TYPE_UINTPTR));
    // doc: end

    // Frame 0 is runtime_callers itself and frame 1 is main, so a walk that
    // worked has at least those two.
    printf("found main's frame: %s\n", n >= 2 ? "yes" : "no");
    return 0;
}

/* Output:
found main's frame: yes
*/
