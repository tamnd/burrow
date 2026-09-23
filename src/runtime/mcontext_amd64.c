/* Context switching for the SysV AMD64 ABI, which is Linux, macOS, the BSDs and
 * Solaris. Windows on the same processor is a different ABI and is not here.
 *
 * The ABI says a function may assume rbx, rbp, r12, r13, r14 and r15 survive a
 * call, so those are what has to be saved, along with the two pieces of
 * floating point mode that are also callee saved: the SSE control word in mxcsr
 * and the x87 control word. Everything else the compiler already treats as
 * destroyed by any call, which is why this is thirty instructions and not a
 * hundred and thirty.
 *
 * None of it goes in the context struct. It goes on the stack being switched
 * away from, and the struct holds the one stack pointer that finds it again.
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

#if defined(BURROW_MCONTEXT_ASM) && defined(BURROW_ARCH_AMD64)

/* The name a C function has in the assembler, which is its C name with the
 * target's prefix in front: an underscore on Mach-O and nothing on ELF. The
 * compiler says which in __USER_LABEL_PREFIX__, and pasting that in means no
 * list of platforms to keep up to date here. */
#define MCONTEXT_AMD64_STR2(x) #x
#define MCONTEXT_AMD64_STR(x) MCONTEXT_AMD64_STR2(x)
#define SYM(name) MCONTEXT_AMD64_STR(__USER_LABEL_PREFIX__) #name

/* The frame this builds and pops, measured from the saved stack pointer. Both
 * halves are in this file so the two can only disagree by somebody editing one
 * of them and not the other, which is the whole reason burrow__mcontext_make_asm
 * is assembly and not C.
 *
 *     0   mxcsr, then the x87 control word at 4
 *     8   r15
 *     16  r14
 *     24  r13
 *     32  r12
 *     40  rbx
 *     48  rbp
 *     56  return address
 *     64  where the stack pointer lands once the return has been taken
 *
 * 64 bytes, and the saved stack pointer is 16 byte aligned, which puts the
 * stack pointer back on a 16 byte boundary at the far end. That is what the ABI
 * wants at a call instruction, and the trampoline's first act is a call. */

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
 * rdi is from, rsi is to. The unwinding directives describe the pushes and are
 * accurate all the way through, including after the stack pointer has been
 * swapped, because the frame on the other stack has the same shape by
 * construction. */
    "\t.globl " SYM(burrow__mcontext_switch_raw) "\n"
#if !defined(__APPLE__)
    "\t.type burrow__mcontext_switch_raw,@function\n"
#endif
    "\t.p2align 4\n"
    SYM(burrow__mcontext_switch_raw) ":\n"
    "\t.cfi_startproc\n"
    "\tpushq %rbp\n"
    "\t.cfi_adjust_cfa_offset 8\n"
    "\t.cfi_offset %rbp, -16\n"
    "\tpushq %rbx\n"
    "\t.cfi_adjust_cfa_offset 8\n"
    "\t.cfi_offset %rbx, -24\n"
    "\tpushq %r12\n"
    "\t.cfi_adjust_cfa_offset 8\n"
    "\t.cfi_offset %r12, -32\n"
    "\tpushq %r13\n"
    "\t.cfi_adjust_cfa_offset 8\n"
    "\t.cfi_offset %r13, -40\n"
    "\tpushq %r14\n"
    "\t.cfi_adjust_cfa_offset 8\n"
    "\t.cfi_offset %r14, -48\n"
    "\tpushq %r15\n"
    "\t.cfi_adjust_cfa_offset 8\n"
    "\t.cfi_offset %r15, -56\n"
    "\tsubq $8, %rsp\n"
    "\t.cfi_adjust_cfa_offset 8\n"
    "\tstmxcsr (%rsp)\n"
    "\tfnstcw 4(%rsp)\n"

    /* The swap itself, and the only two instructions that touch the
     * contexts. Everything before this belongs to the old stack and
     * everything after it belongs to the new one. */
    "\tmovq %rsp, (%rdi)\n"
    "\tmovq (%rsi), %rsp\n"

    "\tldmxcsr (%rsp)\n"
    "\tfldcw 4(%rsp)\n"
    "\taddq $8, %rsp\n"
    "\t.cfi_adjust_cfa_offset -8\n"
    "\tpopq %r15\n"
    "\t.cfi_adjust_cfa_offset -8\n"
    "\t.cfi_restore %r15\n"
    "\tpopq %r14\n"
    "\t.cfi_adjust_cfa_offset -8\n"
    "\t.cfi_restore %r14\n"
    "\tpopq %r13\n"
    "\t.cfi_adjust_cfa_offset -8\n"
    "\t.cfi_restore %r13\n"
    "\tpopq %r12\n"
    "\t.cfi_adjust_cfa_offset -8\n"
    "\t.cfi_restore %r12\n"
    "\tpopq %rbx\n"
    "\t.cfi_adjust_cfa_offset -8\n"
    "\t.cfi_restore %rbx\n"
    "\tpopq %rbp\n"
    "\t.cfi_adjust_cfa_offset -8\n"
    "\t.cfi_restore %rbp\n"
    "\tret\n"
    "\t.cfi_endproc\n"
#if !defined(__APPLE__)
    "\t.size burrow__mcontext_switch_raw, . - burrow__mcontext_switch_raw\n"
#endif

/* void *burrow__mcontext_make_asm(void *stack_top, burrow__MContext *self)
 *
 * rdi is one past the last byte of the caller's stack buffer and rsi is the
 * context. Returns the stack pointer the first switch should load.
 *
 * The frame is filled in so that the pops above hand r12 the context and then
 * return into the trampoline. mxcsr and the x87 word are copied from whatever
 * the calling thread has set right now, so a context inherits the floating
 * point mode of whoever created it, which is what a goroutine would expect. */
    "\t.globl " SYM(burrow__mcontext_make_asm) "\n"
#if !defined(__APPLE__)
    "\t.type burrow__mcontext_make_asm,@function\n"
#endif
    "\t.p2align 4\n"
    SYM(burrow__mcontext_make_asm) ":\n"
    "\t.cfi_startproc\n"
    "\tmovq %rdi, %rax\n"
    "\tandq $-16, %rax\n"
    "\tsubq $64, %rax\n"

    "\tstmxcsr (%rax)\n"
    "\tfnstcw 4(%rax)\n"
    "\tmovq $0, 8(%rax)\n" /* r15 */
    "\tmovq $0, 16(%rax)\n" /* r14 */
    "\tmovq $0, 24(%rax)\n" /* r13 */
    "\tmovq %rsi, 32(%rax)\n" /* r12, which the trampoline reads */
    "\tmovq $0, 40(%rax)\n" /* rbx */
    "\tmovq $0, 48(%rax)\n" /* rbp, zero so a frame pointer walk stops here */

    "\tleaq " SYM(burrow__mcontext_entry) "(%rip), %rcx\n"
    "\tmovq %rcx, 56(%rax)\n"
    "\tret\n"
    "\t.cfi_endproc\n"
#if !defined(__APPLE__)
    "\t.size burrow__mcontext_make_asm, . - burrow__mcontext_make_asm\n"
#endif

/* Where a fresh context begins. Reached by the ret above rather than by a call,
 * so there is no return address underneath and nothing to unwind into.
 *
 * .cfi_undefined on the return address is what says that: a debugger or a
 * profiler walking this stack stops here cleanly instead of reading whatever
 * the buffer happened to contain before. */
    "\t.globl " SYM(burrow__mcontext_entry) "\n"
#if !defined(__APPLE__)
    "\t.type burrow__mcontext_entry,@function\n"
#endif
    "\t.p2align 4\n"
    SYM(burrow__mcontext_entry) ":\n"
    "\t.cfi_startproc\n"
    "\t.cfi_undefined %rip\n"
    "\tmovq %r12, %rdi\n"
    "\tcall " SYM(burrow__mcontext_start) "\n"
    /* burrow__mcontext_start is declared _Noreturn and ends in a fatal error,
     * so this is only reached if that stops being true. */
    "\tud2\n"
    "\t.cfi_endproc\n"
#if !defined(__APPLE__)
    "\t.size burrow__mcontext_entry, . - burrow__mcontext_entry\n"
#endif

#if !defined(__APPLE__)
    "\t.popsection\n"
#endif
);
/* clang-format on */

#endif /* BURROW_MCONTEXT_ASM && BURROW_ARCH_AMD64 */
