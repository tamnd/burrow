/* Which instruction set extensions the processor has, for the crypto code that
 * has a faster path when they are there.
 *
 * On x86 the answer is cpuid, which any program can ask. On arm64 the
 * instructions that say are privileged, so the answer comes from the system:
 * the auxiliary vector on Linux and FreeBSD, sysctl on Apple systems and
 * IsProcessorFeaturePresent on Windows. Wherever the system will not say, a
 * feature the compiler was told to assume, with -march or by default as on
 * Apple silicon, still counts, since the program would not run on a machine
 * without it anyway.
 *
 * The answer and GODEBUG are both read once and kept, the way Go does it at
 * startup. The cache is a relaxed atomic for the same reason pal_page_size's
 * is: two threads that get here together both work out the same number.
 *
 * Derived from Go's src/internal/cpu/cpu.go, cpu_x86.go, cpu_arm64_hwcap.go and
 * cpu_arm64_darwin.go.
 * Go source: go1.27.1.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include <stdint.h>
#include <string.h>

#if defined(BURROW_ARCH_AMD64) || defined(BURROW_ARCH_386)
#define CPU_X86 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#if defined(BURROW_ARCH_ARM64)
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_FREEBSD)
#define CPU_AUXV 1
#include <sys/auxv.h>
#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
#define CPU_SYSCTL 1
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(BURROW_OS_WINDOWS)
#define CPU_WINDOWS 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#endif

#if defined(CPU_X86)
/* cpuid with a subleaf, which leaf 7 needs. */
static void cpu_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
#if defined(_MSC_VER) && !defined(__clang__)
    int v[4];
    __cpuidex(v, (int)leaf, (int)sub);
    for (int i = 0; i < 4; i++)
        r[i] = (uint32_t)v[i];
#else
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;
    __cpuid_count(leaf, sub, a, b, c, d);
    r[0] = a;
    r[1] = b;
    r[2] = c;
    r[3] = d;
#endif
}

/* The bits doinit reads in cpu_x86.go: SSSE3 and SSE4.1 from leaf 1 and SHA
 * from leaf 7. None of them need the operating system to save any extra
 * state, so unlike AVX there is no xgetbv to check. */
static uint32_t cpu_detect(void) {
    uint32_t r[4];
    cpu_cpuid(0, 0, r);
    uint32_t max = r[0];
    if (max < 1)
        return 0;
    uint32_t f = 0;
    cpu_cpuid(1, 0, r);
    if ((r[2] & (UINT32_C(1) << 9)) != 0)
        f |= PAL_CPU_X86_SSSE3;
    if ((r[2] & (UINT32_C(1) << 19)) != 0)
        f |= PAL_CPU_X86_SSE41;
    if (max >= 7) {
        cpu_cpuid(7, 0, r);
        if ((r[1] & (UINT32_C(1) << 29)) != 0)
            f |= PAL_CPU_X86_SHA;
    }
    return f;
}
#elif defined(BURROW_ARCH_ARM64)

#if defined(CPU_SYSCTL)
static bool cpu_sysctl(const char *name) {
    int v = 0;
    size_t n = sizeof v;
    return sysctlbyname(name, &v, &n, NULL, 0) == 0 && v != 0;
}
#endif

static uint32_t cpu_detect(void) {
    uint32_t f = 0;
#if defined(__ARM_FEATURE_SHA2) || defined(__ARM_FEATURE_CRYPTO)
    f |= PAL_CPU_ARM64_SHA1 | PAL_CPU_ARM64_SHA2;
#endif
#if defined(__ARM_FEATURE_SHA512)
    f |= PAL_CPU_ARM64_SHA512;
#endif
#if defined(__ARM_FEATURE_SHA3)
    f |= PAL_CPU_ARM64_SHA3;
#endif
#if defined(CPU_AUXV)
    /* The HWCAP bits from the kernel's uapi hwcap.h, which Go's
     * cpu_arm64_hwcap.go reads too. */
    unsigned long hw = 0;
#if defined(BURROW_OS_FREEBSD)
    if (elf_aux_info(AT_HWCAP, &hw, (int)sizeof hw) != 0)
        hw = 0;
#else
    hw = getauxval(AT_HWCAP);
#endif
    if ((hw & (1UL << 5)) != 0)
        f |= PAL_CPU_ARM64_SHA1;
    if ((hw & (1UL << 6)) != 0)
        f |= PAL_CPU_ARM64_SHA2;
    if ((hw & (1UL << 17)) != 0)
        f |= PAL_CPU_ARM64_SHA3;
    if ((hw & (1UL << 21)) != 0)
        f |= PAL_CPU_ARM64_SHA512;
#elif defined(CPU_SYSCTL)
    /* Every Apple arm64 processor has SHA-1 and SHA-256, which is why Go's
     * cpu_arm64_darwin.go sets them without asking. The FEAT_ names arrived in
     * macOS 12 and the older ones are what came before. */
    f |= PAL_CPU_ARM64_SHA1 | PAL_CPU_ARM64_SHA2;
    if (cpu_sysctl("hw.optional.arm.FEAT_SHA512") ||
        cpu_sysctl("hw.optional.armv8_2_sha512"))
        f |= PAL_CPU_ARM64_SHA512;
    if (cpu_sysctl("hw.optional.arm.FEAT_SHA3") ||
        cpu_sysctl("hw.optional.armv8_2_sha3"))
        f |= PAL_CPU_ARM64_SHA3;
#elif defined(CPU_WINDOWS)
    /* PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE is 30, and SHA3 and SHA512 are
     * 64 and 65, which older SDKs do not name. A Windows too old to know one
     * answers no. */
    if (IsProcessorFeaturePresent(30))
        f |= PAL_CPU_ARM64_SHA1 | PAL_CPU_ARM64_SHA2;
    if (IsProcessorFeaturePresent(64))
        f |= PAL_CPU_ARM64_SHA3;
    if (IsProcessorFeaturePresent(65))
        f |= PAL_CPU_ARM64_SHA512;
#endif
    return f;
}
#else
static uint32_t cpu_detect(void) {
    return 0;
}
#endif

typedef struct CpuOption {
    const char *name;
    uint32_t bit;
} CpuOption;

static const CpuOption cpu_options[] = {
    {"ssse3", PAL_CPU_X86_SSSE3}, {"sse41", PAL_CPU_X86_SSE41},
    {"sha", PAL_CPU_X86_SHA},     {"sha1", PAL_CPU_ARM64_SHA1},
    {"sha2", PAL_CPU_ARM64_SHA2}, {"sha512", PAL_CPU_ARM64_SHA512},
    {"sha3", PAL_CPU_ARM64_SHA3},
};

/* processOptions from internal/cpu: the cpu.name=on and cpu.name=off fields of
 * GODEBUG, in order, so a later one wins, with cpu.all for every feature. The
 * result is the bits to clear. "on" cannot add a feature the processor lacks,
 * so all it does is undo an earlier "off". Go prints a warning for a field it
 * does not understand; this skips it quietly, since a library has no business
 * writing to a program's stderr. */
static uint32_t cpu_godebug_off(const char *env, size_t n) {
    uint32_t off = 0;
    size_t i = 0;
    while (i < n) {
        size_t end = i;
        while (end < n && env[end] != ',')
            end++;
        const char *field = env + i;
        size_t len = end - i;
        i = end + 1;
        if (len < 4 || memcmp(field, "cpu.", 4) != 0)
            continue;
        size_t eq = 4;
        while (eq < len && field[eq] != '=')
            eq++;
        if (eq == len)
            continue;
        const char *key = field + 4;
        size_t nkey = eq - 4;
        const char *value = field + eq + 1;
        size_t nvalue = len - eq - 1;
        bool enable;
        if (nvalue == 2 && memcmp(value, "on", 2) == 0)
            enable = true;
        else if (nvalue == 3 && memcmp(value, "off", 3) == 0)
            enable = false;
        else
            continue;
        uint32_t bits = 0;
        if (nkey == 3 && memcmp(key, "all", 3) == 0) {
            for (size_t k = 0; k < sizeof cpu_options / sizeof cpu_options[0]; k++)
                bits |= cpu_options[k].bit;
        } else {
            for (size_t k = 0; k < sizeof cpu_options / sizeof cpu_options[0]; k++)
                if (strlen(cpu_options[k].name) == nkey &&
                    memcmp(cpu_options[k].name, key, nkey) == 0)
                    bits = cpu_options[k].bit;
        }
        if (enable)
            off &= ~bits;
        else
            off |= bits;
    }
    return off;
}

/* The top bit says the rest has been worked out, since no features at all is
 * a real answer. */
#define CPU_KNOWN (UINT32_C(1) << 31)

static uint32_t cached_features;

uint32_t pal_cpu_features(void) {
    uint32_t got = burrow__atomic_load_relaxed_u32(&cached_features);
    if ((got & CPU_KNOWN) != 0)
        return got & ~CPU_KNOWN;

    uint32_t f = cpu_detect();
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            const char *v = *env + 8;
            f &= ~cpu_godebug_off(v, strlen(v));
            break;
        }
    }

    burrow__atomic_store_relaxed_u32(&cached_features, f | CPU_KNOWN);
    return f;
}
