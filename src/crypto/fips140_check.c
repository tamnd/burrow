/* Derived from Go's src/crypto/internal/fips140/check/check.go and the
 * Supported function in src/crypto/internal/fips140/fips140.go.
 * Go source: go1.27.1.
 *
 * Go's linker writes the checksum and the bounds of the four sections it
 * hashes into go:fipsinfo. Here the bounds come from the linker's symbols for
 * the two sections in fips140_module.h, and the checksum from burrow-gen fips,
 * which finds burrow's info object by its section and writes the checksum
 * into the file after linking.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "fips140_module.h"

#include "burrow/core.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/sha256.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* Not in Go: what burrow-gen fips looks for and fills in. The magic is "\xff"
 * " burrow fips " "\xff\x00", and the sum is all zeros until burrow-gen fips
 * has been run on the program. volatile so the compiler reads the sum from
 * memory instead of folding in the zeros it was given here. Go's Linkinfo
 * also has the bounds of the sections, which the linker writes as relocations,
 * and that is something only a linker can do. */
typedef struct FipsInfo {
    Byte magic[16];
    Byte sum[32];
} FipsInfo;

#define FIPS_INFO_INIT                                                                 \
    {{0xff, ' ', 'b', 'u', 'r', 'r', 'o', 'w', ' ', 'f', 'i', 'p', 's', ' ', 0xff, 0}, \
     {0}}

/* The sections either side of the module's code and data, and the one the info
 * object is in. The ELF and Mach-O linkers give the bounds of a section by
 * name. PE has nothing like that, so the bounds are bytes of our own in
 * sections that sort before and after the ones the module goes in, as
 * registry.c does for the types, and the end is one past the last of them,
 * which is where the joined section ends. */
#if defined(CRYPTO_FIPS_SECTIONS) && defined(_MSC_VER) && !defined(__clang__)

#pragma section(".bfipt$a", read, execute)
#pragma section(".bfipt$z", read, execute)
#pragma section(".bfipr$a", read)
#pragma section(".bfipr$m", read)
#pragma section(".bfipr$z", read)
#pragma section(".bfipi", read)
__declspec(allocate(".bfipt$a")) const Byte burrow__fips_text_first = 0;
__declspec(allocate(".bfipt$z")) const Byte burrow__fips_text_last = 0;
__declspec(allocate(".bfipr$a")) const Byte burrow__fips_rodata_first = 0;
__declspec(allocate(".bfipr$z")) const Byte burrow__fips_rodata_last = 0;
__declspec(allocate(".bfipi")) const volatile FipsInfo burrow__fips_info =
    FIPS_INFO_INIT;
#define FIPS_TEXT_START (&burrow__fips_text_first)
#define FIPS_TEXT_END (&burrow__fips_text_last + 1)
#define FIPS_RODATA_START (&burrow__fips_rodata_first)
#define FIPS_RODATA_END (&burrow__fips_rodata_last + 1)

#elif defined(CRYPTO_FIPS_SECTIONS) && defined(_WIN32)

__attribute__((used, section(".bfipt$a"))) static const Byte fips_text_first = 0;
__attribute__((used, section(".bfipt$z"))) static const Byte fips_text_last = 0;
__attribute__((used, section(".bfipr$a"))) static const Byte fips_rodata_first = 0;
__attribute__((used, section(".bfipr$z"))) static const Byte fips_rodata_last = 0;
__attribute__((used, section(".bfipi"))) static const volatile FipsInfo fips_info =
    FIPS_INFO_INIT;
#define burrow__fips_info fips_info
#define FIPS_TEXT_START (&fips_text_first)
#define FIPS_TEXT_END (&fips_text_last + 1)
#define FIPS_RODATA_START (&fips_rodata_first)
#define FIPS_RODATA_END (&fips_rodata_last + 1)

#elif defined(CRYPTO_FIPS_SECTIONS) && defined(__APPLE__)

__extension__ extern const Byte
    fips_text_start[] __asm("section$start$__TEXT$__bfips_text");
__extension__ extern const Byte
    fips_text_end[] __asm("section$end$__TEXT$__bfips_text");
__extension__ extern const Byte
    fips_rodata_start[] __asm("section$start$__TEXT$__bfips_rodata");
__extension__ extern const Byte
    fips_rodata_end[] __asm("section$end$__TEXT$__bfips_rodata");
__attribute__((
    used, section("__TEXT,__bfips_info"))) static const volatile FipsInfo fips_info =
    FIPS_INFO_INIT;
#define burrow__fips_info fips_info
#define FIPS_TEXT_START fips_text_start
#define FIPS_TEXT_END fips_text_end
#define FIPS_RODATA_START fips_rodata_start
#define FIPS_RODATA_END fips_rodata_end

#elif defined(CRYPTO_FIPS_SECTIONS)

extern const Byte __start_burrow_fips_text[];
extern const Byte __stop_burrow_fips_text[];
extern const Byte __start_burrow_fips_rodata[];
extern const Byte __stop_burrow_fips_rodata[];
__attribute__((used,
               section("burrow_fips_info"))) static const volatile FipsInfo fips_info =
    FIPS_INFO_INIT;
#define burrow__fips_info fips_info
#define FIPS_TEXT_START __start_burrow_fips_text
#define FIPS_TEXT_END __stop_burrow_fips_text
#define FIPS_RODATA_START __start_burrow_fips_rodata
#define FIPS_RODATA_END __stop_burrow_fips_rodata

#endif

/* One byte of data so the rodata section is there even when nothing else is
 * in it, since asking the Mach-O linker for the bounds of a section that does
 * not exist does not link, and the linker is free to drop a constant nothing
 * uses. The code section always has the functions below. */
#if defined(CRYPTO_FIPS_SECTIONS) && !defined(_WIN32)
CRYPTO_FIPS_RODATA __attribute__((used)) static const Byte fips_rodata_anchor = 0;
#endif

/* Hides where an address came from, for the reason BURROW__SEC_BOUND gives in
 * registry.c: the linker puts the module between the two bounds after the
 * compiler has stopped looking, and a compiler that thinks it knows the
 * objects would say the walk runs off the end of them. */
#if defined(__GNUC__)
#define FIPS_BOUND(p)                                                                  \
    (__extension__({                                                                   \
        const Byte *fips_b = (p);                                                      \
        __asm__("" : "+r"(fips_b));                                                    \
        fips_b;                                                                        \
    }))
#else
#define FIPS_BOUND(p) ((const Byte *)(p))
#endif

CRYPTO_FIPS_TEXT const char *burrow__fips140_supported(void) {
    /* ASAN disapproves of reading swaths of global memory in fips140/check. */
#if BURROW_ASAN
    return "FIPS 140-3 mode is incompatible with ASAN";
#elif !defined(CRYPTO_FIPS_SECTIONS) || defined(__wasm__) ||                           \
    (defined(BURROW_OS_WINDOWS) && defined(BURROW_ARCH_386)) ||                        \
    defined(BURROW_OS_OPENBSD) || defined(BURROW_OS_AIX)
    /* Not in Go: a build with no FIPS sections is not supported either. */
    return "FIPS 140-3 mode is not supported on " BURROW_OS_NAME "-" BURROW_ARCH_NAME;
#else
    return NULL;
#endif
}

CRYPTO_FIPS_TEXT void burrow__fips140_checktest_text(void) {}

CRYPTO_FIPS_RODATA const int32_t burrow__fips140_checktest_rodata = 2;

#if defined(CRYPTO_FIPS_SECTIONS)

CRYPTO_FIPS_TEXT void burrow__fips140_sections(const Byte *bounds[4]) {
    bounds[0] = FIPS_BOUND(FIPS_TEXT_START);
    bounds[1] = FIPS_BOUND(FIPS_TEXT_END);
    bounds[2] = FIPS_BOUND(FIPS_RODATA_START);
    bounds[3] = FIPS_BOUND(FIPS_RODATA_END);
}

/* Writes the length of the n bytes at p, as eight big endian bytes, and then
 * the bytes. */
CRYPTO_FIPS_TEXT static void fips_write_section(Hash h, const Byte *p, const Byte *end,
                                                Byte nbuf[8]) {
    size_t n = (size_t)(end - p);
    uint64_t v = (uint64_t)n;
    for (int i = 7; i >= 0; i--) {
        nbuf[i] = (Byte)v;
        v >>= 8;
    }
    hash_write(h, slice_from(nbuf, 8, 8, TYPE_BYTE), NULL);
    hash_write(h, slice_from((void *)(uintptr_t)p, (Int)n, (Int)n, TYPE_BYTE), NULL);
}

CRYPTO_FIPS_TEXT bool burrow__fips140_module_sum(Byte sum[32]) {
    Byte key[32] = {0};
    Byte nbuf[8];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Hash h = hmac_new(arena_allocator(&ar), sha256_new,
                      slice_from(key, sizeof key, sizeof key, TYPE_BYTE));
    if (h.vt == NULL) {
        arena_free(&ar);
        return false;
    }
    /* Not in Go, which writes "go fips object v1\n". */
    static const char head[] = "burrow fips object v1\n";
    Int hl = (Int)sizeof head - 1;
    hash_write(h, slice_from((void *)(uintptr_t)head, hl, hl, TYPE_BYTE), NULL);
    const Byte *b[4];
    burrow__fips140_sections(b);
    fips_write_section(h, b[0], b[1], nbuf);
    fips_write_section(h, b[2], b[3], nbuf);
    Slice got = hash_sum(arena_allocator(&ar), h, slice_from(sum, 0, 32, TYPE_BYTE));
    bool ok = got.len == 32 && got.p == sum;
    memset(nbuf, 0, sizeof nbuf);
    hash_reset(h);
    arena_free(&ar);
    return ok;
}

CRYPTO_FIPS_TEXT const char *burrow__fips140_verify(void) {
    static const char magic[] = " burrow fips ";
    Byte want[32];
    bool zero = true;
    for (size_t i = 0; i < sizeof want; i++) {
        want[i] = burrow__fips_info.sum[i];
        zero = zero && want[i] == 0;
    }
    bool ok = burrow__fips_info.magic[0] == 0xff &&
              burrow__fips_info.magic[14] == 0xff && burrow__fips_info.magic[15] == 0;
    for (size_t i = 0; i < sizeof magic - 1; i++)
        ok = ok && burrow__fips_info.magic[i + 1] == (Byte)magic[i];
    if (!ok || zero)
        return "no verification checksum found";

    Byte sum[32];
    if (!burrow__fips140_module_sum(sum))
        return "out of memory";
    bool match = memcmp(sum, want, sizeof sum) == 0;

    /* "The temporary value(s) generated during the integrity test of the
     * module's software or firmware shall [05.10] be zeroised from the module
     * upon completion of the integrity test" */
    memset(sum, 0, sizeof sum);
    memset(want, 0, sizeof want);
    return match ? NULL : "verification mismatch";
}

#else

void burrow__fips140_sections(const Byte *bounds[4]) {
    for (int i = 0; i < 4; i++)
        bounds[i] = NULL;
}

bool burrow__fips140_module_sum(Byte sum[32]) {
    memset(sum, 0, 32);
    return false;
}

const char *burrow__fips140_verify(void) {
    return "no verification checksum found";
}

#endif
