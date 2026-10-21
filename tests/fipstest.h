/* What the tests of the FIPS 140-3 module share, which Go's are in one package
 * for, crypto/internal/fips140test.
 *
 * Go's tests run the program again with GODEBUG=fips140=on, and Go's linker
 * has written the module's checksum into every program, so the check passes
 * there. Here that is burrow-gen fips, which needs Python, and building the
 * tests does not. So fipstest_copy writes a copy of the running program with a
 * checksum written in, for the tests to run instead. With the right checksum,
 * the copy is what burrow-gen fips would have made.
 *
 * Not in Go.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_FIPSTEST_H
#define BURROW_TESTS_FIPSTEST_H

#include "burrow/burrow.h"
#include "burrow/os/exec.h"
#include "burrow/path/filepath.h"

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

/* The magic at the start of the info that burrow-gen fips fills in. The 0xff
 * comes from a volatile so the compiler cannot put the whole magic in the
 * program as a constant, which would be a second copy for fipstest_find_info.
 * Go leaves the 0xff out of its constant for the same reason. */
static inline void fipstest_magic(Byte m[16]) {
    volatile Byte ff = 0xff;
    m[0] = ff;
    memcpy(m + 1, " burrow fips ", 13);
    m[14] = ff;
    m[15] = 0;
}

/* Where the info is in bin, the bytes of the program, or -1 after failing the
 * test when it is not there exactly once. */
static inline Int fipstest_find_info(TestingT *t, Slice bin) {
    Byte m[16];
    fipstest_magic(m);
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

/* Writes a copy of the running program with sum in place of its checksum, in a
 * new directory, which it puts in *dir for the caller to remove. Returns the
 * path of the copy, or an empty Str after failing the test. */
static inline Str fipstest_copy(TestingT *t, Alloc *a, const Byte sum[32], Str *dir) {
    *dir = BURROW_STR_EMPTY;
    Error err = BURROW_NO_ERROR;
    Str exe = os_executable(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Executable: %s", error_text(err));
        return BURROW_STR_EMPTY;
    }
    Slice bin = os_read_file(a, exe, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "ReadFile: %s", error_text(err));
        return BURROW_STR_EMPTY;
    }
    Int i = fipstest_find_info(t, bin);
    if (i < 0)
        return BURROW_STR_EMPTY;
    memcpy((Byte *)bin.p + i + 16, sum, 32);

    Str d = os_mkdir_temp(a, BURROW_S(""), BURROW_S("fips140test"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "MkdirTemp: %s", error_text(err));
        return BURROW_STR_EMPTY;
    }
    *dir = d;
    Str bin_path = filepath_join_v(a, 2, d, BURROW_S("fips140test.exe"));
    err = os_write_file(bin_path, bin, 0755);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "WriteFile: %s", error_text(err));
        return BURROW_STR_EMPTY;
    }

#if defined(BURROW_OS_DARWIN)
    /* Regenerate the macOS ad-hoc code signature. */
    ExecCmd *sign = exec_command_v(a, BURROW_S("codesign"), 4, BURROW_S("-s"),
                                   BURROW_S("-"), BURROW_S("-f"), bin_path);
    Slice sout = exec_cmd_combined_output(sign, a, &err);
    if (BURROW_FAILED(err)) {
        Str so = {(const Byte *)sout.p, sout.len};
        testing_t_errorf_v(t, "codesign failed: %s\n%s", error_text(err), so);
        return BURROW_STR_EMPTY;
    }
#endif
    return bin_path;
}

static inline bool fipstest_contains(Slice out, const char *s) {
    Int n = (Int)strlen(s);
    return bytes_contains(out, slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE));
}

#endif /* BURROW_TESTS_FIPSTEST_H */
