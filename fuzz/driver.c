/* The differential fuzzer's driver.
 *
 * Built with -fsanitize=fuzzer it is a libFuzzer target: every input goes to
 * both sides of one FuzzTarget, and a report that differs is a crash, which
 * libFuzzer then minimises and saves. Built with -DFUZZ_STANDALONE it is a
 * program that replays files and directories of inputs through the same
 * comparison, for compilers that have no libFuzzer and for checking the
 * committed corpus in CI without fuzzing.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz.h"

void report_printf(Report *r, const char *format, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, format);
        int n = vsnprintf(r->p ? r->p + r->len : NULL, r->cap - r->len, format, ap);
        va_end(ap);
        if (n < 0) {
            abort();
        }
        if ((size_t)n < r->cap - r->len) {
            r->len += (size_t)n;
            return;
        }
        size_t cap = r->cap ? r->cap * 2 : 256;
        while (cap - r->len <= (size_t)n) {
            cap *= 2;
        }
        r->p = realloc(r->p, cap);
        if (!r->p) {
            abort();
        }
        r->cap = cap;
    }
}

void report_hex(Report *r, const uint8_t *p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        report_printf(r, "%c%c", digits[p[i] >> 4], digits[p[i] & 15]);
    }
}

/* How long the line starting at p is, not counting its newline. */
static int line_len(const char *p, size_t n) {
    const char *nl = memchr(p, '\n', n);
    return (int)(nl ? (size_t)(nl - p) : n);
}

/* Prints the first line where the two reports part ways, then both reports
 * whole, then stops the process so that libFuzzer records the input. */
static void diverged(const char *want, size_t wlen, const char *got, size_t glen) {
    size_t i = 0, line = 1, start = 0;
    while (i < wlen && i < glen && want[i] == got[i]) {
        if (want[i] == '\n') {
            line++;
            start = i + 1;
        }
        i++;
    }
    fprintf(stderr, "\n%s: burrow and Go disagree at line %zu\n", fuzz_target.name,
            line);
    fprintf(stderr, "  go:     %.*s\n", line_len(want + start, wlen - start),
            want + start);
    fprintf(stderr, "  burrow: %.*s\n", line_len(got + start, glen - start),
            got + start);
    fprintf(stderr, "\n--- go\n%.*s--- burrow\n%.*s---\n", (int)wlen, want, (int)glen,
            got);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    size_t wlen = 0;
    char *want = fuzz_target.oracle((unsigned char *)data, n, &wlen);
    Report got = {0};
    fuzz_target.burrow(data, n, &got);
    if (wlen != got.len || (wlen && memcmp(want, got.p, wlen) != 0)) {
        diverged(want, wlen, got.p ? got.p : "", got.len);
    }
    free(want);
    free(got.p);
    return 0;
}

#if defined(FUZZ_STANDALONE)

#include <dirent.h>
#include <sys/stat.h>

static int replayed;

static void replay_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 4096;
            buf = realloc(buf, cap);
            if (!buf) {
                abort();
            }
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        if (got == 0) {
            break;
        }
        len += got;
    }
    fclose(f);
    LLVMFuzzerTestOneInput(buf, len);
    free(buf);
    replayed++;
}

static void replay(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        perror(path);
        exit(2);
    }
    if (!S_ISDIR(st.st_mode)) {
        replay_file(path);
        return;
    }
    DIR *d = opendir(path);
    if (!d) {
        perror(path);
        exit(2);
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char sub[4096];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        replay(sub);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE_OR_DIRECTORY...\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        replay(argv[i]);
    }
    printf("ok\tfuzz %s\t%d inputs agree\n", fuzz_target.name, replayed);
    return 0;
}

#endif
