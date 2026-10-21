/* Derived from Go's src/crypto/internal/fips140test/check_test.go.
 * Go source: go1.27.1.
 *
 * Go's tests run the program again with GODEBUG=fips140=on, which runs the
 * check when the program starts. burrow cannot turn FIPS mode on yet, so the
 * program here runs again with BURROW_FIPS140_CHECK=1 in its environment
 * instead, and TestIntegrityCheck runs the check and panics when it fails, the
 * way Go's init does.
 *
 * Go's linker writes the checksum into every program. Here that is burrow-gen
 * fips, which needs Python, and building the tests does not. So when the
 * program has no checksum, TestIntegrityCheck writes the right one into a copy
 * of itself and runs that, the way TestIntegrityCheckFailure writes a wrong one.
 * make fips-check runs burrow-gen fips on this program first, so that there
 * TestIntegrityCheck checks the checksum burrow-gen wrote.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/sha256.h"
#include "burrow/os/exec.h"
#include "burrow/path/filepath.h"
#include "burrow/unicode.h"

#include "../src/crypto/fips140_module.h"

#include <stdint.h>
#include <string.h>

/* cryptotest.MustSupportFIPS140. */
#define MUST_SUPPORT_FIPS140(t)                                                        \
    do {                                                                               \
        const char *fips_why = burrow__fips140_supported();                            \
        if (fips_why != NULL)                                                          \
            testing_t_skipf_v((t), "test requires FIPS 140 mode: %s", fips_why);       \
    } while (0)

/* Not in Go: the magic at the start of the info that burrow-gen fips fills in.
 * The 0xff comes from a volatile so the compiler cannot put the whole magic in
 * this program as a constant, which would be a second copy for find_info. Go
 * leaves the 0xff out of its constant for the same reason. */
static void fips_magic(Byte m[16]) {
    volatile Byte ff = 0xff;
    m[0] = ff;
    memcpy(m + 1, " burrow fips ", 13);
    m[14] = ff;
    m[15] = 0;
}

/* Not in Go: where the info is in bin, the bytes of this program, or -1 after
 * failing the test when it is not there exactly once. */
static Int find_info(TestingT *t, Slice bin) {
    Byte m[16];
    fips_magic(m);
    Slice ms = slice_from(m, 16, 16, TYPE_BYTE);
    Int i = bytes_index(bin, ms);
    if (i < 0) {
        testing_t_errorf_v(t, "no FIPS info in the program");
        return -1;
    }
    if (bytes_index(slice_sub(bin, i + 1, bin.len), ms) >= 0) {
        testing_t_errorf_v(t, "the FIPS info is in the program more than once");
        return -1;
    }
    return i;
}

/* Writes a copy of this program with sum in place of its checksum, runs
 * TestIntegrityCheck in it with BURROW_FIPS140_CHECK=1, and returns what it
 * printed. False when that could not be done, after failing the test. */
static bool run_with_sum(TestingT *t, Alloc *a, const Byte sum[32], Slice *out,
                         Error *runerr) {
    Error err = BURROW_NO_ERROR;
    Str exe = os_executable(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Executable: %s", error_text(err));
        return false;
    }
    Slice bin = os_read_file(a, exe, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "ReadFile: %s", error_text(err));
        return false;
    }
    Int i = find_info(t, bin);
    if (i < 0)
        return false;
    memcpy((Byte *)bin.p + i + 16, sum, 32);

    Str dir = os_mkdir_temp(a, BURROW_S(""), BURROW_S("fips140test"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "MkdirTemp: %s", error_text(err));
        return false;
    }
    Str bin_path = filepath_join_v(a, 2, dir, BURROW_S("fips140test.exe"));
    err = os_write_file(bin_path, bin, 0755);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "WriteFile: %s", error_text(err));
        os_remove_all(dir);
        return false;
    }

#if defined(BURROW_OS_DARWIN)
    /* Regenerate the macOS ad-hoc code signature. */
    ExecCmd *sign = exec_command_v(a, BURROW_S("codesign"), 4, BURROW_S("-s"),
                                   BURROW_S("-"), BURROW_S("-f"), bin_path);
    Slice sout = exec_cmd_combined_output(sign, a, &err);
    if (BURROW_FAILED(err)) {
        Str so = {(const Byte *)sout.p, sout.len};
        testing_t_errorf_v(t, "codesign failed: %s\n%s", error_text(err), so);
        os_remove_all(dir);
        return false;
    }
#endif

    testing_t_logf_v(t, "running modified binary...");
    ExecCmd *cmd = exec_command_v(a, bin_path, 2, BURROW_S("-test.v"),
                                  BURROW_S("-test.run=^TestIntegrityCheck$"));
    Slice env = os_environ(a);
    cmd->env = BURROW_APPEND(Str, a, env, BURROW_S("BURROW_FIPS140_CHECK=1"));
    *out = exec_cmd_combined_output(cmd, a, runerr);
    Str printed = {(const Byte *)out->p, out->len};
    testing_t_logf_v(t, "running with BURROW_FIPS140_CHECK=1:\n%s", printed);
    os_remove_all(dir);
    return true;
}

static bool out_contains(Slice out, const char *s) {
    Int n = (Int)strlen(s);
    return bytes_contains(out, slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE));
}

static void TestIntegrityCheck(TestingT *t) {
    MUST_SUPPORT_FIPS140(t);
    const char *why = burrow__fips140_verify();
    if (why == NULL) {
        testing_t_logf_v(t, "verified");
        return;
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    bool found = false;
    os_lookup_env(a, BURROW_S("BURROW_FIPS140_CHECK"), &found);
    if (found)
        panic_str(fmt_sprintf_v(a, "fips140: %s", why));
    if (strcmp(why, "no verification checksum found") != 0) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "fips140: %s", why);
    }

    /* Not in Go: no checksum, so run a copy of this program with the right one
     * written in, which is what Go's linker would have done. */
    Byte sum[32];
    if (!burrow__fips140_module_sum(sum)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "computing the checksum failed");
    }
    Slice out = {0};
    Error err = BURROW_NO_ERROR;
    if (run_with_sum(t, a, sum, &out, &err)) {
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "binary with the checksum written in failed: %s",
                               error_text(err));
        if (!out_contains(out, "verified"))
            testing_t_errorf_v(t, "binary with the checksum written in did not verify");
    }
    arena_free(&ar);
}

static void TestIntegrityCheckFailure(TestingT *t) {
    MUST_SUPPORT_FIPS140(t);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Replace the expected module checksum with a different value. */
    Byte sum[32];
    memset(sum, 'X', sizeof sum);
    Slice out = {0};
    Error err = BURROW_NO_ERROR;
    if (run_with_sum(t, a, sum, &out, &err)) {
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "modified binary did not fail as expected");
        if (!out_contains(out, "fips140: verification mismatch"))
            testing_t_errorf_v(t, "modified binary did not fail with expected message");
        if (out_contains(out, "verified"))
            testing_t_errorf_v(t, "modified binary did not exit");
    }
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
