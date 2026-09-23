/* The processor's name, for the cpu: line a benchmark run starts with.
 *
 * Go asks the processor first and the system second, and so does this. On x86
 * the processor answers through cpuid's brand string. Everywhere else it has
 * no name to give, and the answer comes from sysctl on the BSDs and macOS and
 * from /proc/cpuinfo on Linux. Windows on ARM has neither and gets an empty
 * name, which is what Go prints there too: no cpu: line at all.
 *
 * _DEFAULT_SOURCE is for sysctlbyname on the BSDs, which hide it under a strict
 * C11 build, and it goes before the first include for the usual reason. It is
 * harmless everywhere else, and the amalgamation already has it.
 *
 * Derived from Go's src/internal/cpu/cpu_x86.go and src/internal/sysinfo.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#endif

#include "burrow/platform.h"

#include "burrow/pal.h"

#include <stdint.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define CPUNAME_X86 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_OPENBSD) || defined(BURROW_OS_DRAGONFLY)
#define CPUNAME_SYSCTL 1
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#if defined(BURROW_OS_LINUX)
#define CPUNAME_PROC 1
#include <stdio.h>
#endif

#if defined(CPUNAME_X86) || defined(CPUNAME_SYSCTL) || defined(CPUNAME_PROC)
/* Copies n bytes of s into buf as a NUL terminated string, cut to fit, and
 * returns how many it kept. */
static int64_t cpuname_put(char *buf, int64_t cap, const char *s, size_t n) {
    if (cap <= 0)
        return 0;
    if ((uint64_t)n > (uint64_t)(cap - 1))
        n = (size_t)(cap - 1);
    memcpy(buf, s, n);
    buf[n] = '\0';
    return (int64_t)n;
}
#endif

#if defined(CPUNAME_X86)
static void cpuname_cpuid(uint32_t leaf, uint32_t r[4]) {
#if defined(_MSC_VER) && !defined(__clang__)
    int v[4];
    __cpuid(v, (int)leaf);
    for (int i = 0; i < 4; i++)
        r[i] = (uint32_t)v[i];
#else
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;
    __cpuid(leaf, a, b, c, d);
    r[0] = a;
    r[1] = b;
    r[2] = c;
    r[3] = d;
#endif
}

/* cpu.Name: the brand string, leading spaces trimmed and cut at the first NUL. */
static int64_t cpuname_brand(char *buf, int64_t cap) {
    uint32_t r[4];
    cpuname_cpuid(0x80000000U, r);
    if (r[0] < 0x80000004U)
        return 0;
    char data[48];
    for (uint32_t i = 0; i < 3; i++) {
        cpuname_cpuid(0x80000002U + i, r);
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                data[i * 16 + (uint32_t)j * 4 + (uint32_t)k] = (char)(r[j] >> (8 * k));
    }
    size_t start = 0;
    while (start < sizeof data && data[start] == ' ')
        start++;
    size_t end = start;
    while (end < sizeof data && data[end] != '\0')
        end++;
    return cpuname_put(buf, cap, data + start, end - start);
}
#endif

#if defined(CPUNAME_PROC)
static bool cpuname_has(const char *s, size_t n, const char *sub) {
    size_t m = strlen(sub);
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(s + i, sub, m) == 0)
            return true;
    return false;
}

/* sysinfo's osCPUInfoName on Linux: the model name from the first 512 bytes of
 * /proc/cpuinfo, with the clock speed after it when the name does not already
 * carry one. */
static int64_t cpuname_proc(char *buf, int64_t cap) {
    FILE *f = fopen("/proc/cpuinfo", "rb");
    if (f == NULL)
        return 0;
    char data[512];
    size_t got = fread(data, 1, sizeof data, f);
    fclose(f);

    const char *model = NULL;
    size_t nmodel = 0;
    const char *mhz = NULL;
    size_t nmhz = 0;
    size_t i = 0;
    while (i < got) {
        size_t eol = i;
        while (eol < got && data[eol] != '\n')
            eol++;
        const char *line = data + i;
        size_t n = eol - i;
        for (size_t c = 0; c + 1 < n; c++) {
            if (line[c] != ':' || line[c + 1] != ' ')
                continue;
            size_t ks = 0;
            size_t ke = c;
            while (ks < ke && (line[ks] == ' ' || line[ks] == '\t'))
                ks++;
            while (ke > ks && (line[ke - 1] == ' ' || line[ke - 1] == '\t'))
                ke--;
            const char *key = line + ks;
            size_t nk = ke - ks;
            if ((nk == 10 && memcmp(key, "model name", 10) == 0) ||
                (nk == 10 && memcmp(key, "Model Name", 10) == 0)) {
                model = line + c + 2;
                nmodel = n - c - 2;
            } else if ((nk == 7 && memcmp(key, "cpu MHz", 7) == 0) ||
                       (nk == 7 && memcmp(key, "CPU MHz", 7) == 0)) {
                mhz = line + c + 2;
                nmhz = n - c - 2;
            }
            break;
        }
        i = eol + 1;
    }
    if (model == NULL || nmodel == 0)
        return 0;
    if (mhz == NULL || nmhz == 0 || cpuname_has(model, nmodel, "GHz") ||
        cpuname_has(model, nmodel, "MHz"))
        return cpuname_put(buf, cap, model, nmodel);

    /* The two are separate lines of data, so together they fit in it. */
    char joined[sizeof data + 8];
    size_t n = 0;
    static const char at[] = " @ ";
    static const char unit[] = "MHz";
    memcpy(joined, model, nmodel);
    n += nmodel;
    for (size_t k = 0; at[k] != '\0'; k++)
        joined[n++] = at[k];
    memcpy(joined + n, mhz, nmhz);
    n += nmhz;
    for (size_t k = 0; unit[k] != '\0'; k++)
        joined[n++] = unit[k];
    return cpuname_put(buf, cap, joined, n);
}
#endif

int64_t pal_cpu_name(char *buf, int64_t cap) {
    if (cap > 0)
        buf[0] = '\0';
    int64_t n = 0;
#if defined(CPUNAME_X86)
    n = cpuname_brand(buf, cap);
#endif
#if defined(CPUNAME_SYSCTL)
    if (n == 0) {
        char data[256];
        size_t len = sizeof data;
        if (sysctlbyname("machdep.cpu.brand_string", data, &len, NULL, 0) == 0 &&
            len > 0) {
            if (data[len - 1] == '\0')
                len--;
            n = cpuname_put(buf, cap, data, len);
        }
    }
#endif
#if defined(CPUNAME_PROC)
    if (n == 0)
        n = cpuname_proc(buf, cap);
#endif
    return n;
}
