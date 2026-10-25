#include <stdio.h>

#include "burrow/burrow.h"

typedef void (*GreetFunc)(const char *who);

int main(void) {
    // doc: open
    Error err = BURROW_NO_ERROR;
    Plugin *p = plugin_open(BURROW_S("./greeter.so"), &err);
    if (BURROW_FAILED(err)) {
        /* plugin.Open("./greeter.so"): realpath failed */
        printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
        return 0;
    }
    PluginSymbol sym = plugin_lookup(p, BURROW_S("greet"), &err);
    if (BURROW_FAILED(err)) {
        printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
        return 0;
    }
    union {
        PluginSymbol object;
        GreetFunc fn;
    } u = {sym};
    u.fn("gopher");
    // doc: end
    return 0;
}

/* Output:
plugin.Open("./greeter.so"): realpath failed
*/
