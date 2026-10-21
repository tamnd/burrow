/* Derived from Go's src/crypto/internal/fips140/check/check.go and the
 * Supported function in src/crypto/internal/fips140/fips140.go.
 * Go source: go1.27.1.
 *
 * Go's linker writes the checksum and the bounds of the four sections it
 * hashes into go:fipsinfo. Here the bounds come from the linker's symbols for
 * the two sections in fips140_module.h, or on Windows from the section table
 * in the program's headers, and the checksum from burrow-gen fips, which finds
 * burrow's info object by its section and writes the checksum into the file
 * after linking.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/fips140.h"

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

/* The bounds of the module's code and data, and the section the info object
 * is in. The ELF and Mach-O linkers give the bounds of a section by name. PE
 * has nothing like that, so they come from the section table, which the loader
 * maps at __ImageBase with the rest of the headers. That is the whole of each
 * section, as burrow-gen fips sums it, padding and all. */
#if defined(CRYPTO_FIPS_SECTIONS) && defined(_WIN32)

extern const Byte __ImageBase[];
#if defined(_MSC_VER) && !defined(__clang__)
#pragma section(".bfipi", read)
#define FIPS_INFO_STORAGE __declspec(allocate(".bfipi"))
#else
#define FIPS_INFO_STORAGE __attribute__((used, section(".bfipi"))) static
#endif

#elif defined(CRYPTO_FIPS_SECTIONS) && defined(__APPLE__)

__extension__ extern const Byte
    fips_text_start[] __asm("section$start$__TEXT$__bfips_text");
__extension__ extern const Byte
    fips_text_end[] __asm("section$end$__TEXT$__bfips_text");
__extension__ extern const Byte
    fips_rodata_start[] __asm("section$start$__TEXT$__bfips_rodata");
__extension__ extern const Byte
    fips_rodata_end[] __asm("section$end$__TEXT$__bfips_rodata");
#define FIPS_INFO_STORAGE __attribute__((used, section("__TEXT,__bfips_info"))) static
#define FIPS_TEXT_START fips_text_start
#define FIPS_TEXT_END fips_text_end
#define FIPS_RODATA_START fips_rodata_start
#define FIPS_RODATA_END fips_rodata_end

#elif defined(CRYPTO_FIPS_SECTIONS)

extern const Byte __start_burrow_fips_text[];
extern const Byte __stop_burrow_fips_text[];
extern const Byte __start_burrow_fips_rodata[];
extern const Byte __stop_burrow_fips_rodata[];
#define FIPS_INFO_STORAGE __attribute__((used, section("burrow_fips_info"))) static
#define FIPS_TEXT_START __start_burrow_fips_text
#define FIPS_TEXT_END __stop_burrow_fips_text
#define FIPS_RODATA_START __start_burrow_fips_rodata
#define FIPS_RODATA_END __stop_burrow_fips_rodata

#endif

#if defined(CRYPTO_FIPS_SECTIONS)
FIPS_INFO_STORAGE const volatile FipsInfo burrow__fips_info = FIPS_INFO_INIT;
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

#if defined(_WIN32)

CRYPTO_FIPS_TEXT static uint32_t fips_le(const Byte *p, int n) {
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/* The start and end of the section called name in the section table: past the
 * signature at the offset at 0x3c, the 20 bytes of the file header, with the
 * number of sections at 2 and the size of the optional header at 16, and that
 * header, come 40 bytes for each section, with the name first, the size at 8
 * and the address at 12. */
CRYPTO_FIPS_TEXT static void fips_pe_section(const char name[8], const Byte **start,
                                             const Byte **end) {
    const Byte *base = FIPS_BOUND(__ImageBase);
    const Byte *fh = base + fips_le(base + 0x3c, 4) + 4;
    uint32_t n = fips_le(fh + 2, 2);
    const Byte *s = fh + 20 + fips_le(fh + 16, 2);
    *start = NULL;
    *end = NULL;
    for (uint32_t i = 0; i < n; i++, s += 40) {
        if (memcmp(s, name, 8) == 0) {
            *start = base + fips_le(s + 12, 4);
            *end = *start + fips_le(s + 8, 4);
            return;
        }
    }
}

CRYPTO_FIPS_TEXT void burrow__fips140_sections(const Byte *bounds[4]) {
    fips_pe_section(".bfipt\0\0", &bounds[0], &bounds[1]);
    fips_pe_section(".bfipr\0\0", &bounds[2], &bounds[3]);
}

#else

CRYPTO_FIPS_TEXT void burrow__fips140_sections(const Byte *bounds[4]) {
    bounds[0] = FIPS_BOUND(FIPS_TEXT_START);
    bounds[1] = FIPS_BOUND(FIPS_TEXT_END);
    bounds[2] = FIPS_BOUND(FIPS_RODATA_START);
    bounds[3] = FIPS_BOUND(FIPS_RODATA_END);
}

#endif

/* Zeroes the n bytes at p through a volatile pointer, so that the compiler
 * cannot leave it out for being the last store to a local, as it can a memset. */
CRYPTO_FIPS_TEXT static void fips_clear(Byte *p, size_t n) {
    volatile Byte *v = p;
    for (size_t i = 0; i < n; i++)
        v[i] = 0;
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
    fips_clear(nbuf, sizeof nbuf);
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
    fips_clear(sum, sizeof sum);
    fips_clear(want, sizeof want);
    return match ? NULL : "verification mismatch";
}

#else

CRYPTO_FIPS_TEXT void burrow__fips140_sections(const Byte *bounds[4]) {
    for (int i = 0; i < 4; i++)
        bounds[i] = NULL;
}

CRYPTO_FIPS_TEXT bool burrow__fips140_module_sum(Byte sum[32]) {
    memset(sum, 0, 32);
    return false;
}

CRYPTO_FIPS_TEXT const char *burrow__fips140_verify(void) {
    return "no verification checksum found";
}

#endif
