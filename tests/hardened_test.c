/* Not in Go: the hardened build, make HARDENED=1, which
 * docs/guides/building.md describes. Go has nothing like it.
 *
 * TestHardenedHeaderLimits checks the HTTP server's default header limits
 * in any build, and that the library was built with the same answer as this
 * file. TestHardenedBoundsTrap runs only in a hardened build, and checks that
 * an index past the end of an array stops the program.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"
#include "burrow/burrow.h"
#include "burrow/net/http.h"
#include "burrow/os/exec.h"

#if defined(BURROW_HARDENED) && BURROW_HARDENED
#define HARDENED_BUILD 1
#else
#define HARDENED_BUILD 0
#endif

static void TestHardenedHeaderLimits(TestingT *t) {
#if HARDENED_BUILD
    Int want_bytes = (Int)64 << 10, want_count = 100;
#else
    Int want_bytes = (Int)1 << 20, want_count = 500;
#endif
    if (HTTP_DEFAULT_MAX_HEADER_BYTES != want_bytes)
        testing_t_errorf_v(t, "HTTP_DEFAULT_MAX_HEADER_BYTES = %d, want %d",
                           HTTP_DEFAULT_MAX_HEADER_BYTES, want_bytes);
    if (HTTP_DEFAULT_MAX_HEADER_VALUE_COUNT != want_count)
        testing_t_errorf_v(t, "HTTP_DEFAULT_MAX_HEADER_VALUE_COUNT = %d, want %d",
                           HTTP_DEFAULT_MAX_HEADER_VALUE_COUNT, want_count);

    /* The library's own idea of the limit, for a server that sets none. When
     * it differs, the library and this file were built with different
     * settings of BURROW_HARDENED. */
    HttpServer srv;
    memset(&srv, 0, sizeof srv);
    Int got = burrow__http_server_max_header_value_count(&srv);
    if (got != want_count)
        testing_t_errorf_v(t, "the server's default header value count is %d, want %d",
                           got, want_count);
}

/* The array the child indexes past the end of, and the index, volatile so
 * that the compiler can't see the index is out of range and refuse to build
 * the program, or leave the read out. */
static int hardened_array[4];
static volatile Int hardened_index = 4;

static void TestHardenedBoundsTrap(TestingT *t) {
    Alloc *h = heap_allocator();
    if (os_getenv(h, BURROW_S("BURROW_TEST_HARDENED_TRAP")).len > 0) {
        int v = hardened_array[hardened_index];
        fmt_printf_v("no trap, read %d\n", v);
        os_exit(0);
    }
    if (!HARDENED_BUILD)
        testing_t_skip_v(t, "not a hardened build");
#if defined(_WIN32)
    /* MSVC has no trapping bounds checks, and with mingw the trap is an
     * unhandled exception, which goes to the system's crash handler. Under wine
     * that starts winedbg, which waits, and on a desktop it can open a dialog. */
    testing_t_skip_v(t, "the trap goes to the crash handler on windows");
#endif
    SKIP_WITHOUT_EXEC(t);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str exe = os_executable(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Executable failed: %s", err);
        arena_free(&ar);
        return;
    }
    /* This program again, running this test with the variable set, which
     * makes it read past the end of the array. */
    ExecCmd *cmd =
        exec_command_v(a, exe, 2, BURROW_S("-test.run=^TestHardenedBoundsTrap$"),
                       BURROW_S("-test.count=1"));
    Slice env = os_environ(a);
    env = BURROW_APPEND(Str, a, env, BURROW_S("BURROW_TEST_HARDENED_TRAP=1"));
    cmd->env = env;
    Slice out = exec_cmd_combined_output(cmd, a, &err);
    exec_cmd_free(cmd);
    Str outs = str_from_bytes(out.p, out.len);
    if (errors_as(err, TYPE_EXEC_EXIT_ERROR) == NULL)
        testing_t_errorf_v(t, "the read past the end didn't stop the program: %v\n%s",
                           err, outs);
    else
        testing_t_logf_v(t, "the child stopped with %v", err);
    arena_free(&ar);
}

#define TESTS(X) X(TestHardenedHeaderLimits) X(TestHardenedBoundsTrap)
TESTING_MAIN(TESTS)
