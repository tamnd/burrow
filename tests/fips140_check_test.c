/* Derived from Go's src/crypto/internal/fips140test/check_test.go.
 * Go source: go1.27.1.
 *
 * Go's linker writes the checksum into every program. Here that is burrow-gen
 * fips, which needs Python, and building the tests does not. So when the
 * program has no checksum, TestIntegrityCheck writes the right one into a copy
 * of itself and runs that with GODEBUG=fips140=on, the way
 * TestIntegrityCheckFailure writes a wrong one. make fips-check runs burrow-gen
 * fips on this program first, so that there TestIntegrityCheck checks the
 * checksum burrow-gen wrote.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fipstest.h"

#include "burrow/burrow.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/sha256.h"
#include "burrow/unicode.h"

#include <stdint.h>
#include <string.h>

static void TestIntegrityCheck(TestingT *t) {
    if (burrow__fips140_verified()) {
        testing_t_logf_v(t, "verified");
        return;
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    if (fipstest_setting(a) == FIPS140_ON) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "GODEBUG=fips140=on but verification did not run");
    }

    Slice out = {0};
    fipstest_rerun(t, a, "on", &out);
    /* Not in Go: check that the program that ran again got as far as the
     * check. */
    if (out.len > 0 && !fipstest_contains(out, "verified"))
        testing_t_errorf_v(t, "fips140=on subprocess did not verify");
    arena_free(&ar);
}

static void TestIntegrityCheckFailure(TestingT *t) {
    module_status(t);
    MUST_SUPPORT_FIPS140(t);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Replace the expected module checksum with a different value. */
    Byte sum[32];
    memset(sum, 'X', sizeof sum);
    Str dir;
    Str bin_path = fipstest_copy(t, a, sum, &dir);
    if (bin_path.len > 0) {
        testing_t_logf_v(t, "running modified binary...");
        ExecCmd *cmd = exec_command_v(a, bin_path, 2, BURROW_S("-test.v"),
                                      BURROW_S("-test.run=^TestIntegrityCheck$"));
        cmd->env = BURROW_APPEND(Str, a, os_environ(a), BURROW_S("GODEBUG=fips140=on"));
        Error err = BURROW_NO_ERROR;
        Slice out = exec_cmd_combined_output(cmd, a, &err);
        Str printed = {(const Byte *)out.p, out.len};
        testing_t_logf_v(t, "running with GODEBUG=fips140=on:\n%s", printed);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "modified binary did not fail as expected");
        if (!fipstest_contains(out, "fips140: verification mismatch"))
            testing_t_errorf_v(t, "modified binary did not fail with expected message");
        if (fipstest_contains(out, "verified"))
            testing_t_errorf_v(t, "modified binary did not exit");
    }
    if (dir.len > 0)
        os_remove_all(dir);
    arena_free(&ar);
}

static bool in_section(const Byte *const b[4], int i, const void *p) {
    uintptr_t u = (uintptr_t)p;
    return (uintptr_t)b[2 * i] <= u && u < (uintptr_t)b[2 * i + 1];
}

/* Not in Go: Go's test is in a package whose other tests use the whole module,
 * so all of it is in the test program. Here the test is a program of its own,
 * and a linker that leaves out what nothing refers to, as MSVC's does, would
 * leave out most of the module and fail the size check below. These are a few
 * of the module's algorithms for the test to refer to. */
static void (*volatile const fips_keep[])(void) = {
    (void (*)(void))sha256_sum256,
    (void (*)(void))aes_new_cipher,
    (void (*)(void))ed25519_sign,
    (void (*)(void))mlkem_generate_key768,
};

static void TestIntegrityCheckInfo(TestingT *t) {
    MUST_SUPPORT_FIPS140(t);

    /* Not in Go: see fips_keep. */
    for (size_t i = 0; i < sizeof fips_keep / sizeof fips_keep[0]; i++)
        if (fips_keep[i] == NULL)
            testing_t_errorf_v(t, "fips_keep[%d] is nil", (Int)i);

    /* Check that the checktest symbols are initialized properly. */
    if (burrow__fips140_checktest_rodata != 2)
        testing_t_errorf_v(t, "checktest.RODATA = %d, want 2",
                           burrow__fips140_checktest_rodata);

    /* Check that the checktest symbols are in the right sections. */
    const Byte *b[4];
    burrow__fips140_sections(b);
    const void *text = (const void *)(uintptr_t)burrow__fips140_checktest_text;
    const void *rodata = &burrow__fips140_checktest_rodata;
    if (!in_section(b, 0, text))
        testing_t_errorf_v(t, "checktest.TEXT (%p) not in section #0 (%p..%p)", text,
                           (const void *)b[0], (const void *)b[1]);
    if (!in_section(b, 1, rodata))
        testing_t_errorf_v(t, "checktest.RODATA (%p) not in section #1 (%p..%p)",
                           rodata, (const void *)b[2], (const void *)b[3]);

    /* Check that the symbols are not in unexpected sections (that is, no
     * overlaps). */
    if (in_section(b, 1, text))
        testing_t_errorf_v(t, "checktest.TEXT (%p) unexpectedly in section #1", text);
    if (in_section(b, 0, rodata))
        testing_t_errorf_v(t, "checktest.RODATA (%p) unexpectedly in section #0",
                           rodata);

    /* Check that non-FIPS symbols are not in any of the sections. */
    const void *other = (const void *)(uintptr_t)os_getenv;
    const void *hex = unicode_ascii_hex_digit;
    for (int i = 0; i < 2; i++) {
        if (in_section(b, i, other))
            testing_t_errorf_v(t, "os_getenv (%p) unexpectedly in section #%d", other,
                               i);
        if (in_section(b, i, hex))
            testing_t_errorf_v(
                t, "unicode_ascii_hex_digit (%p) unexpectedly in section #%d", hex, i);
    }

    /* Check that we have enough data in total. */
    size_t n = (size_t)(b[1] - b[0]) + (size_t)(b[3] - b[2]);
    if (n < 16 * 1024)
        testing_t_fatalf_v(t, "fips sections not big enough: %d, want at least 16 kB",
                           (Int)n);
}

#define TESTS(X)                                                                       \
    X(TestIntegrityCheck)                                                              \
    X(TestIntegrityCheckFailure)                                                       \
    X(TestIntegrityCheckInfo)

TESTING_MAIN(TESTS)
