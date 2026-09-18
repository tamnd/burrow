/* What platform is this, decided entirely by the preprocessor.
 *
 * There is no configure step in burrow and there never will be. Everything a
 * build system would normally probe for is either something the compiler
 * already told us, which is this file, or something that has to be asked of the
 * running kernel rather than of the build machine, which is a runtime check
 * elsewhere. The second kind is why one binary can run on an old kernel and a
 * new one, and it is how Go does it too.
 *
 * The names match Go's GOOS and GOARCH wherever Go has one, so burrow_os_name()
 * returns the same string a Go program would print, and a bug report that says
 * linux/arm64 means the same thing in both projects.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_PLATFORM_H
#define BURROW_PLATFORM_H

#include "burrow/own.h"

/* ---------------------------------------------------------------- compiler */

#if defined(_MSC_VER) && !defined(__clang__)
#define BURROW_CC_MSVC 1
#elif defined(__clang__)
#define BURROW_CC_CLANG 1
#elif defined(__GNUC__)
#define BURROW_CC_GCC 1
#else
#define BURROW_CC_UNKNOWN 1
#endif

#ifndef BURROW_CC_MSVC
#define BURROW_CC_MSVC 0
#endif
#ifndef BURROW_CC_CLANG
#define BURROW_CC_CLANG 0
#endif
#ifndef BURROW_CC_GCC
#define BURROW_CC_GCC 0
#endif
#ifndef BURROW_CC_UNKNOWN
#define BURROW_CC_UNKNOWN 0
#endif

/* C23 is not required anywhere and never will be, since MSVC decides the floor
 * and the floor is C11. It is worth detecting because a few things get faster
 * or safer when it is available, never different. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define BURROW_C23 1
#else
#define BURROW_C23 0
#endif

/* -------------------------------------------------------------- the system */

#if defined(__COSMOPOLITAN__)
/* Checked before everything else on purpose. A cosmopolitan build decides what
 * it is at startup rather than at compile time, so anything below would be
 * answering a question that does not have a compile time answer. */
#define BURROW_OS_COSMO 1
#define BURROW_OS_NAME "cosmo"
#elif defined(__wasi__)
#define BURROW_OS_WASI 1
#define BURROW_OS_NAME "wasip1"
#elif defined(_WIN32)
#define BURROW_OS_WINDOWS 1
#define BURROW_OS_NAME "windows"
#elif defined(__ANDROID__)
#define BURROW_OS_ANDROID 1
#define BURROW_OS_LINUX 1 /* Android is Linux, with a different libc */
#define BURROW_OS_NAME "android"
#elif defined(__linux__)
#define BURROW_OS_LINUX 1
#define BURROW_OS_NAME "linux"
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#define BURROW_OS_IOS 1
#define BURROW_OS_NAME "ios"
#else
#define BURROW_OS_DARWIN 1
#define BURROW_OS_NAME "darwin"
#endif
#elif defined(__FreeBSD__)
#define BURROW_OS_FREEBSD 1
#define BURROW_OS_NAME "freebsd"
#elif defined(__OpenBSD__)
#define BURROW_OS_OPENBSD 1
#define BURROW_OS_NAME "openbsd"
#elif defined(__NetBSD__)
#define BURROW_OS_NETBSD 1
#define BURROW_OS_NAME "netbsd"
#elif defined(__DragonFly__)
#define BURROW_OS_DRAGONFLY 1
#define BURROW_OS_NAME "dragonfly"
#elif defined(__sun) && defined(__SVR4)
#define BURROW_OS_SOLARIS 1
#define BURROW_OS_NAME "illumos"
#elif defined(_AIX)
#define BURROW_OS_AIX 1
#define BURROW_OS_NAME "aix"
#elif defined(__EMSCRIPTEN__)
#define BURROW_OS_JS 1
#define BURROW_OS_NAME "js"
#else
#error                                                                                 \
    "burrow does not know this operating system. Add it to platform.h, which is the only place that has to change."
#endif

/* Everything that is close enough to Unix that the platform layer can share one
 * implementation. Windows and the two web targets are the ones that are not. */
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_OPENBSD) ||                        \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_DRAGONFLY) ||                       \
    defined(BURROW_OS_SOLARIS) || defined(BURROW_OS_AIX) || defined(BURROW_OS_COSMO)
#define BURROW_UNIX 1
#else
#define BURROW_UNIX 0
#endif

#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_OPENBSD) ||                        \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_DRAGONFLY)
#define BURROW_BSD 1
#else
#define BURROW_BSD 0
#endif

/* --------------------------------------------------------------- the chip */

#if defined(__x86_64__) || defined(_M_X64)
#define BURROW_ARCH_AMD64 1
#define BURROW_ARCH_NAME "amd64"
#define BURROW_PTR_BITS 64
#elif defined(__aarch64__) || defined(_M_ARM64)
#define BURROW_ARCH_ARM64 1
#define BURROW_ARCH_NAME "arm64"
#define BURROW_PTR_BITS 64
#elif defined(__i386__) || defined(_M_IX86)
#define BURROW_ARCH_386 1
#define BURROW_ARCH_NAME "386"
#define BURROW_PTR_BITS 32
#elif defined(__arm__) || defined(_M_ARM)
#define BURROW_ARCH_ARM 1
#define BURROW_ARCH_NAME "arm"
#define BURROW_PTR_BITS 32
#elif defined(__riscv) && __riscv_xlen == 64
#define BURROW_ARCH_RISCV64 1
#define BURROW_ARCH_NAME "riscv64"
#define BURROW_PTR_BITS 64
#elif defined(__loongarch64)
#define BURROW_ARCH_LOONG64 1
#define BURROW_ARCH_NAME "loong64"
#define BURROW_PTR_BITS 64
#elif defined(__powerpc64__) || defined(__ppc64__)
#define BURROW_ARCH_PPC64 1
#define BURROW_PTR_BITS 64
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&                     \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BURROW_ARCH_NAME "ppc64le"
#else
#define BURROW_ARCH_NAME "ppc64"
#endif
#elif defined(__s390x__)
#define BURROW_ARCH_S390X 1
#define BURROW_ARCH_NAME "s390x"
#define BURROW_PTR_BITS 64
#elif defined(__mips64)
#define BURROW_ARCH_MIPS64 1
#define BURROW_ARCH_NAME "mips64"
#define BURROW_PTR_BITS 64
#elif defined(__wasm32__) || defined(__wasm__)
#define BURROW_ARCH_WASM 1
#define BURROW_ARCH_NAME "wasm"
#define BURROW_PTR_BITS 32
#else
#error                                                                                 \
    "burrow does not know this architecture. Add it to platform.h, which is the only place that has to change."
#endif

/* ------------------------------------------------------------- byte order */

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#define BURROW_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#elif defined(BURROW_ARCH_S390X)
#define BURROW_BIG_ENDIAN 1
#else
/* Everything else burrow runs on is little endian, and MSVC only targets
 * little endian machines at all. */
#define BURROW_BIG_ENDIAN 0
#endif

#define BURROW_LITTLE_ENDIAN (!BURROW_BIG_ENDIAN)

/* ------------------------------------------------------- the sanitizers
 *
 * Whether this translation unit is being compiled under the address sanitizer,
 * which matters to the handful of places that have to tell it what they are
 * about to do. Switching stacks is the one that exists today: a sanitizer that
 * is not told believes the thread is still on the stack it was on before.
 *
 * Two spellings because the compilers disagree. clang answers __has_feature and
 * gcc predefines a macro. A compiler that does neither reads as off and the
 * code goes unannotated, which is the same as building with no sanitizer at
 * all, and is the right way for this to fail. */

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BURROW_ASAN 1
#endif
#endif

#if !defined(BURROW_ASAN)
#if defined(__SANITIZE_ADDRESS__)
#define BURROW_ASAN 1
#else
#define BURROW_ASAN 0
#endif
#endif

/* --------------------------------------------------- attributes and hints
 *
 * Spelled once here so that no other file in the tree has to know which
 * compiler understands which syntax. Every one of these is allowed to expand to
 * nothing, because all of them are advice. */

#if BURROW_CC_MSVC
#define BURROW_INLINE __forceinline
#define BURROW_NOINLINE __declspec(noinline)
#define BURROW_NORETURN __declspec(noreturn)
#define BURROW_THREAD_LOCAL __declspec(thread)
#define BURROW_UNUSED
#define BURROW_PRINTF(fmt_index, first_arg)
#define BURROW_LIKELY(x) (x)
#define BURROW_UNLIKELY(x) (x)
#else
#define BURROW_INLINE inline __attribute__((always_inline))
#define BURROW_NOINLINE __attribute__((noinline))
#define BURROW_NORETURN _Noreturn
#define BURROW_THREAD_LOCAL _Thread_local
#define BURROW_UNUSED __attribute__((unused))
#define BURROW_PRINTF(fmt_index, first_arg)                                            \
    __attribute__((format(printf, fmt_index, first_arg)))
#define BURROW_LIKELY(x) __builtin_expect(!!(x), 1)
#define BURROW_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/* ------------------------------------------------------------- at runtime */

#ifdef __cplusplus
extern "C" {
#endif

/* The same strings the macros carry, for callers that cannot read macros. That
 * is anything reaching burrow through a foreign function interface, and it is
 * also the amalgamation user who wants a bug report to say which build they
 * have without recompiling anything. */
BURROW_STATIC(ret) const char *burrow_os_name(void);
BURROW_STATIC(ret) const char *burrow_arch_name(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_PLATFORM_H */
