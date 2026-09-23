/* What a differential fuzz target is made of.
 *
 * A target is two functions that are handed the same bytes and write a report
 * about them: one written in C against burrow, in fuzz/<name>.c, and one
 * written in Go against Go's standard library, in fuzz/oracle/<name>.go. The
 * driver runs both and fails on the first byte where the reports differ. See
 * docs/design/14-testing.md section 3.
 *
 * This is test code, not library code, so it uses malloc and abort freely.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_FUZZ_H
#define BURROW_FUZZ_H

#include <stddef.h>
#include <stdint.h>

/* A growing byte buffer that the burrow side writes its report into. */
typedef struct Report {
    char *p;
    size_t len;
    size_t cap;
} Report;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void report_printf(Report *r, const char *format, ...);

/* The bytes as lowercase hex with no separators, which is what Go's %x prints
 * for a []byte. */
void report_hex(Report *r, const uint8_t *p, size_t n);

/* The burrow side of a target. */
typedef void (*BurrowSide)(const uint8_t *data, size_t n, Report *out);

/* The Go side, exported from the archive that fuzz/oracle builds. The report
 * is malloc'd and the caller frees it. */
typedef char *(*OracleSide)(unsigned char *data, size_t n, size_t *outlen);

typedef struct FuzzTarget {
    const char *name;
    BurrowSide burrow;
    OracleSide oracle;
} FuzzTarget;

/* Each target file defines exactly one of these, and the driver is linked
 * against one target file at a time. */
extern const FuzzTarget fuzz_target;

#endif
