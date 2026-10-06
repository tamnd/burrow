/* The arm64 DIT bit, set and cleared on the thread that asks. See
 * burrow/dit.h for who asks and why.
 *
 * DIT is PSTATE bit 24, and it is read and written as the system register
 * S3_3_C4_C2_5, which is the spelling every assembler takes, including the ones
 * that do not know the name dit. Arm says no barrier is needed after writing a
 * PSTATE field, and Apple says one is, so on Apple systems the write is
 * followed by the DSB and ISB Go uses there (go.dev/issue/77776).
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/dit.h"

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include <stdint.h>
#include <string.h>

#if defined(BURROW_DIT)

enum { DIT_BIT = 24 };

static uint64_t dit_read(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, s3_3_c4_c2_5" : "=r"(v));
    return v;
}

static void dit_write(uint64_t v) {
    __asm__ volatile("msr s3_3_c4_c2_5, %0" : : "r"(v) : "memory");
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
    __asm__ volatile("dsb nsh\n\tisb" : : : "memory");
#endif
}

bool burrow__dit_supported(void) {
    return (pal_cpu_features() & PAL_CPU_ARM64_DIT) != 0;
}

bool burrow__dit_enabled(void) {
    return ((dit_read() >> DIT_BIT) & 1) != 0;
}

bool burrow__dit_enable(void) {
    if (burrow__dit_enabled())
        return true;
    dit_write((uint64_t)1 << DIT_BIT);
    return false;
}

void burrow__dit_disable(void) {
    dit_write(0);
}

#else

bool burrow__dit_supported(void) {
    return false;
}

bool burrow__dit_enabled(void) {
    return false;
}

bool burrow__dit_enable(void) {
    return false;
}

void burrow__dit_disable(void) {}

#endif

/* 0 not read yet, 1 off, 2 on. */
static uint32_t dit_everywhere;

/* Whether the last dataindependenttiming in a GODEBUG value is 1, which is how
 * Go reads one setting given twice. */
static bool dit_godebug(const char *v) {
    static const char key[] = "dataindependenttiming=";
    size_t kl = sizeof key - 1;
    bool on = false;
    while (*v != '\0') {
        const char *end = strchr(v, ',');
        if (end == NULL)
            end = v + strlen(v);
        if ((size_t)(end - v) >= kl && memcmp(v, key, kl) == 0)
            on = (size_t)(end - v) == kl + 1 && v[kl] == '1';
        v = *end == ',' ? end + 1 : end;
    }
    return on;
}

bool burrow__dit_everywhere(void) {
    uint32_t got = burrow__atomic_load_relaxed_u32(&dit_everywhere);
    if (got != 0)
        return got == 2;
    bool on = false;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            on = dit_godebug(*env + 8);
            break;
        }
    }
    burrow__atomic_store_relaxed_u32(&dit_everywhere, on ? 2U : 1U);
    return on;
}
