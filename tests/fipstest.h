/* What the tests of the FIPS 140-3 module share, which Go's are in one package
 * for, crypto/internal/fips140test, or get from crypto/internal/cryptotest.
 *
 * Go's tests run the program again with GODEBUG=fips140=on, and Go's linker
 * has written the module's checksum into every program, so the check passes
 * there. Here that is burrow-gen fips, which needs Python, and building the
 * tests does not. So fipstest_copy writes a copy of the running program with a
 * checksum written in, for the tests to run instead. With the right checksum,
 * the copy is what burrow-gen fips would have made.
 *
 * Not in Go, apart from module_status and fipstest_rerun.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_FIPSTEST_H
#define BURROW_TESTS_FIPSTEST_H

#include "burrow/burrow.h"
#include "burrow/os/exec.h"
#include "burrow/path/filepath.h"
#include "burrow/regexp.h"

#include "burrow/crypto/fips140.h"

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

/* moduleStatus. Not in Go: burrow's module has no name and no PAA/PAI setting,
 * so those lines are left out. */
static inline void module_status(TestingT *t) {
    if (burrow__fips140_enabled())
        testing_t_logf_v(t, "FIPS 140-3 mode enabled");
    else
        testing_t_logf_v(t, "FIPS 140-3 mode not enabled");

    testing_t_logf_v(t, "Module version: %s", fips140_version());

    if (burrow__fips140_verified())
        testing_t_logf_v(t, "FIPS 140-3 integrity self-check succeeded");
    else
        testing_t_logf_v(t, "FIPS 140-3 integrity self-check not succeeded");
}

/* The fips140 setting in GODEBUG, as burrow__fips140_setting reads it. */
static inline Fips140Setting fipstest_setting(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fips140Setting s =
        burrow__fips140_setting(str_to_cstr(a, os_getenv(a, BURROW_S("GODEBUG"))));
    arena_free(&ar);
    return s;
}

/* A program like this one with the module's checksum in it. That is this one
 * when burrow-gen fips has been run on it, and otherwise a copy with the
 * checksum written in, in *dir for the caller to remove. Empty after failing
 * the test when there is neither. */
static inline Str fipstest_program(TestingT *t, Alloc *a, Str *dir) {
    *dir = BURROW_STR_EMPTY;
    const char *why = burrow__fips140_verify();
    if (why == NULL) {
        Error err = BURROW_NO_ERROR;
        Str exe = os_executable(a, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Executable: %s", error_text(err));
            return BURROW_STR_EMPTY;
        }
        return exe;
    }
    if (strcmp(why, "no verification checksum found") != 0) {
        testing_t_errorf_v(t, "fips140: %s", why);
        return BURROW_STR_EMPTY;
    }
    Byte sum[32];
    if (!burrow__fips140_module_sum(sum)) {
        testing_t_errorf_v(t, "computing the checksum failed");
        return BURROW_STR_EMPTY;
    }
    return fipstest_copy(t, a, sum, dir);
}

/* cryptotest.RerunWithFIPS140Enabled, with mode "on", and
 * RerunWithFIPS140Enforced, with mode "only". Not in Go: the program that runs
 * again is the one from fipstest_program, and what it printed goes in *out
 * when out is not NULL. */
static inline void fipstest_rerun(TestingT *t, Alloc *a, const char *mode, Slice *out) {
    MUST_SUPPORT_FIPS140(t);
    Str dir;
    Str exe = fipstest_program(t, a, &dir);
    if (exe.len > 0) {
        Str name = testing_t_name(t);
        Str name_regex = fmt_sprintf_v(a, "^%s$", regexp_quote_meta(a, name));
        Str run = fmt_sprintf_v(a, "-test.run=%s", name_regex);
        ExecCmd *cmd = exec_command_v(a, exe, 2, run, BURROW_S("-test.v"));
        Str setting = fmt_sprintf_v(a, "GODEBUG=fips140=%s", str_from_cstr(mode));
        cmd->env = BURROW_APPEND(Str, a, os_environ(a), setting);
        Error err = BURROW_NO_ERROR;
        Slice o = exec_cmd_combined_output(cmd, a, &err);
        Str printed = {(const Byte *)o.p, o.len};
        testing_t_logf_v(t, "running with GODEBUG=fips140=%s:\n%s", str_from_cstr(mode),
                         printed);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "fips140=%s subprocess failed: %s",
                               str_from_cstr(mode), error_text(err));
        if (out != NULL)
            *out = o;
    }
    if (dir.len > 0)
        os_remove_all(dir);
}

#endif /* BURROW_TESTS_FIPSTEST_H */
