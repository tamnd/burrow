/* Context switching for AArch64, which is the AAPCS on Linux and the BSDs and
 * Apple's near enough identical version of it on macOS.
 *
 * The registers a function may assume survive a call are x19 to x28, the frame
 * pointer x29, the link register x30, and the low half of v8 to v15, which are
 * spelled d8 to d15 when that is all you want. The floating point control
 * register is callee preserved too, and it is saved here for the same reason
 * mxcsr is saved on amd64: a goroutine that changes the rounding mode and then
 * parks should not hand that mode to whoever runs next.
 *
 * Under Cosmopolitan x28 is the exception. It holds the thread pointer there,
 * so the switch below leaves it alone rather than restoring it.
 *
 * Pointer authentication is not used. macOS builds for arm64 rather than
 * arm64e for everything that is not a system binary, and the signed return
 * address there would have to be re-signed against the new stack pointer.
 * Branch target identification is not used either, which means a linker that
 * cares will mark the binary as not having it. Both belong with the hardening
 * pass rather than here.
 *
 * It is assembly written as one top level __asm__ statement in a C file, rather
 * than a .S file beside it, so that the build is C files and nothing else. That
 * is what lets the amalgamation be one burrow.c that any C compiler builds with
 * no assembler step and no second file to carry around, and it costs nothing
 * here: the compiler hands the text to the same assembler the .S went to.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Guarded on the macros burrow/mcontext.h picks the backend with, so the two
 * cannot disagree. On Windows, on the other processor, and in a build that
 * asked for the portable path, this file is the two headers and nothing else,
 * and the functions below come from mcontext.c instead. */
#include "burrow/mcontext.h"
#include "burrow/platform.h"

#if defined(BURROW_MCONTEXT_ASM) && defined(BURROW_ARCH_ARM64)

/* The name a C function has in the assembler, which is its C name with the
 * target's prefix in front: an underscore on Mach-O and nothing on ELF. The
 * compiler says which in __USER_LABEL_PREFIX__, and pasting that in means no
 * list of platforms to keep up to date here. */
#define MCONTEXT_ARM64_STR2(x) #x
#define MCONTEXT_ARM64_STR(x) MCONTEXT_ARM64_STR2(x)
#define SYM(name) MCONTEXT_ARM64_STR(__USER_LABEL_PREFIX__) #name

/* The frame this builds and pops, measured from the saved stack pointer.
 *
 *     0    x19, x20
 *     16   x21, x22
 *     32   x23, x24
 *     48   x25, x26
 *     64   x27, x28
 *     80   x29, x30
 *     96   d8, d9
 *     112  d10, d11
 *     128  d12, d13
 *     144  d14, d15
 *     160  fpcr, and eight bytes of padding after it
 *
 * 176 bytes, which is a multiple of 16, which is what the stack pointer has to
 * be at all times on this architecture and not merely at a call. */

/* One instruction a line, the way an assembler file would have it, which is
 * the layout clang-format would undo. */
/* clang-format off */
__asm__(
#if defined(__APPLE__)
    "\t.text\n"
#else
    "\t.pushsection .text\n"
#endif

/* void burrow__mcontext_switch_raw(burrow__MContext *from, burrow__MContext *to)
 *
 * x0 is from, x1 is to. */
    "\t.globl " SYM(burrow__mcontext_switch_raw) "\n"
#if !defined(__APPLE__)
    "\t.type burrow__mcontext_switch_raw,%function\n"
#endif
    "\t.p2align 4\n"
    SYM(burrow__mcontext_switch_raw) ":\n"
    "\t.cfi_startproc\n"
    "\tsub sp, sp, #176\n"
    "\t.cfi_def_cfa_offset 176\n"
    "\tstp x19, x20, [sp, #0]\n"
    "\t.cfi_offset x19, -176\n"
    "\t.cfi_offset x20, -168\n"
    "\tstp x21, x22, [sp, #16]\n"
    "\t.cfi_offset x21, -160\n"
    "\t.cfi_offset x22, -152\n"
    "\tstp x23, x24, [sp, #32]\n"
    "\t.cfi_offset x23, -144\n"
    "\t.cfi_offset x24, -136\n"
    "\tstp x25, x26, [sp, #48]\n"
    "\t.cfi_offset x25, -128\n"
    "\t.cfi_offset x26, -120\n"
    "\tstp x27, x28, [sp, #64]\n"
    "\t.cfi_offset x27, -112\n"
    "\t.cfi_offset x28, -104\n"
    "\tstp x29, x30, [sp, #80]\n"
    "\t.cfi_offset x29, -96\n"
    "\t.cfi_offset x30, -88\n"
    "\tstp d8, d9, [sp, #96]\n"
    "\tstp d10, d11, [sp, #112]\n"
    "\tstp d12, d13, [sp, #128]\n"
    "\tstp d14, d15, [sp, #144]\n"
    "\tmrs x2, fpcr\n"
    "\tstr x2, [sp, #160]\n"

    /* The swap itself. The stack pointer cannot be stored directly, so it
     * goes through x2, which is caller saved and therefore ours. */
    "\tmov x2, sp\n"
    "\tstr x2, [x0]\n"
    "\tldr x2, [x1]\n"
    "\tmov sp, x2\n"

    "\tldr x2, [sp, #160]\n"
    "\tmsr fpcr, x2\n"
    "\tldp d14, d15, [sp, #144]\n"
    "\tldp d12, d13, [sp, #128]\n"
    "\tldp d10, d11, [sp, #112]\n"
    "\tldp d8, d9, [sp, #96]\n"
    "\tldp x29, x30, [sp, #80]\n"
    "\t.cfi_restore x29\n"
    "\t.cfi_restore x30\n"
    /* Cosmopolitan keeps its thread pointer in x28 and builds everything with
     * it reserved, so x28 belongs to the thread and not to the goroutine. The
     * goroutine coming in may have last run on another thread, and loading its
     * x28 would hand this thread somebody else's thread local storage, or none
     * at all for a goroutine that has never run. So there it is saved, for the
     * frame layout's sake, and never loaded. */
#if defined(__COSMOPOLITAN__)
    "\tldr x27, [sp, #64]\n"
    "\t.cfi_restore x27\n"
#else
    "\tldp x27, x28, [sp, #64]\n"
    "\t.cfi_restore x27\n"
    "\t.cfi_restore x28\n"
#endif
    "\tldp x25, x26, [sp, #48]\n"
    "\t.cfi_restore x25\n"
    "\t.cfi_restore x26\n"
    "\tldp x23, x24, [sp, #32]\n"
    "\t.cfi_restore x23\n"
    "\t.cfi_restore x24\n"
    "\tldp x21, x22, [sp, #16]\n"
    "\t.cfi_restore x21\n"
    "\t.cfi_restore x22\n"
    "\tldp x19, x20, [sp, #0]\n"
    "\t.cfi_restore x19\n"
    "\t.cfi_restore x20\n"
    "\tadd sp, sp, #176\n"
    "\t.cfi_def_cfa_offset 0\n"
    "\tret\n"
    "\t.cfi_endproc\n"
#if !defined(__APPLE__)
    "\t.size burrow__mcontext_switch_raw, . - burrow__mcontext_switch_raw\n"
#endif

/* void *burrow__mcontext_make_asm(void *stack_top, burrow__MContext *self)
 *
 * x0 is one past the last byte of the caller's stack buffer and x1 is the
 * context. Returns the stack pointer the first switch should load.
 *
 * x19 gets the context and x30 gets the entry stub, so that the loads above
 * hand the trampoline what it needs and then return straight into it. x29 is
 * zeroed, which is how a frame pointer walk knows it has reached the bottom. */
    "\t.globl " SYM(burrow__mcontext_make_asm) "\n"
#if !defined(__APPLE__)
    "\t.type burrow__mcontext_make_asm,%function\n"
#endif
    "\t.p2align 4\n"
    SYM(burrow__mcontext_make_asm) ":\n"
    "\t.cfi_startproc\n"
    "\tand x0, x0, #-16\n"
    "\tsub x0, x0, #176\n"

    "\tstr x1, [x0, #0]\n" /* x19, which the trampoline reads */
    "\tstr xzr, [x0, #8]\n" /* x20 */
    "\tstp xzr, xzr, [x0, #16]\n"
    "\tstp xzr, xzr, [x0, #32]\n"
    "\tstp xzr, xzr, [x0, #48]\n"
    "\tstp xzr, xzr, [x0, #64]\n"
    "\tstr xzr, [x0, #80]\n" /* x29 */

#if defined(__APPLE__)
    "\tadrp x2, " SYM(burrow__mcontext_entry) "@PAGE\n"
    "\tadd x2, x2, " SYM(burrow__mcontext_entry) "@PAGEOFF\n"
#else
    "\tadrp x2, burrow__mcontext_entry\n"
    "\tadd x2, x2, :lo12:burrow__mcontext_entry\n"
#endif
    "\tstr x2, [x0, #88]\n" /* x30 */

    "\tstp xzr, xzr, [x0, #96]\n" /* d8, d9 */
    "\tstp xzr, xzr, [x0, #112]\n" /* d10, d11 */
    "\tstp xzr, xzr, [x0, #128]\n" /* d12, d13 */
    "\tstp xzr, xzr, [x0, #144]\n" /* d14, d15 */

    /* The floating point mode is inherited from whoever is creating the
     * context, which is what a goroutine would expect of the goroutine that
     * started it. */
    "\tmrs x2, fpcr\n"
    "\tstr x2, [x0, #160]\n"
    "\tstr xzr, [x0, #168]\n"

    "\tret\n"
    "\t.cfi_endproc\n"
#if !defined(__APPLE__)
    "\t.size burrow__mcontext_make_asm, . - burrow__mcontext_make_asm\n"
#endif

/* Where a fresh context begins. Reached by the ret above, so x30 holds this
 * address rather than a real caller's, and there is nothing underneath to
 * unwind into. .cfi_undefined on x30 is what tells a debugger to stop here. */
    "\t.globl " SYM(burrow__mcontext_entry) "\n"
#if !defined(__APPLE__)
    "\t.type burrow__mcontext_entry,%function\n"
#endif
    "\t.p2align 4\n"
    SYM(burrow__mcontext_entry) ":\n"
    "\t.cfi_startproc\n"
    "\t.cfi_undefined x30\n"
    "\tmov x0, x19\n"
    "\tbl " SYM(burrow__mcontext_start) "\n"
    /* burrow__mcontext_start is declared _Noreturn and ends in a fatal error,
     * so this is only reached if that stops being true. */
    "\tbrk #1\n"
    "\t.cfi_endproc\n"
#if !defined(__APPLE__)
    "\t.size burrow__mcontext_entry, . - burrow__mcontext_entry\n"
#endif

#if !defined(__APPLE__)
    "\t.popsection\n"
#endif
);
/* clang-format on */

#endif /* BURROW_MCONTEXT_ASM && BURROW_ARCH_ARM64 */
