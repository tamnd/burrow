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
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Guarded to match the backend choice in burrow/mcontext.h exactly. On mingw the
 * processor is right and the ABI is not, and on a build that asked for the
 * portable path this would be a second definition of the same symbol. Either
 * way what is wanted here is an empty object file. */
#if defined(__x86_64__) && !defined(_WIN32) && !defined(BURROW_PORTABLE_CONTEXT)

/* Mach-O puts an underscore in front of every C symbol and ELF does not. */
#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

/* The frame this builds and pops, measured from the saved stack pointer. Both
 * halves are in this file so the two can only disagree by somebody editing one
 * of them and not the other, which is the whole reason burrow__mcontext_make_asm
 * is not written in C.
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

	.text

/* void burrow__mcontext_switch_raw(burrow__MContext *from, burrow__MContext *to)
 *
 * rdi is from, rsi is to. The unwinding directives describe the pushes and are
 * accurate all the way through, including after the stack pointer has been
 * swapped, because the frame on the other stack has the same shape by
 * construction. */
	.globl SYM(burrow__mcontext_switch_raw)
#if !defined(__APPLE__)
	.type burrow__mcontext_switch_raw,@function
#endif
	.p2align 4
SYM(burrow__mcontext_switch_raw):
	.cfi_startproc
	pushq	%rbp
	.cfi_adjust_cfa_offset 8
	.cfi_offset %rbp, -16
	pushq	%rbx
	.cfi_adjust_cfa_offset 8
	.cfi_offset %rbx, -24
	pushq	%r12
	.cfi_adjust_cfa_offset 8
	.cfi_offset %r12, -32
	pushq	%r13
	.cfi_adjust_cfa_offset 8
	.cfi_offset %r13, -40
	pushq	%r14
	.cfi_adjust_cfa_offset 8
	.cfi_offset %r14, -48
	pushq	%r15
	.cfi_adjust_cfa_offset 8
	.cfi_offset %r15, -56
	subq	$8, %rsp
	.cfi_adjust_cfa_offset 8
	stmxcsr	(%rsp)
	fnstcw	4(%rsp)

	/* The swap itself, and the only two instructions that touch the
	 * contexts. Everything before this belongs to the old stack and
	 * everything after it belongs to the new one. */
	movq	%rsp, (%rdi)
	movq	(%rsi), %rsp

	ldmxcsr	(%rsp)
	fldcw	4(%rsp)
	addq	$8, %rsp
	.cfi_adjust_cfa_offset -8
	popq	%r15
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r15
	popq	%r14
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r14
	popq	%r13
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r13
	popq	%r12
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r12
	popq	%rbx
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rbx
	popq	%rbp
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rbp
	ret
	.cfi_endproc
#if !defined(__APPLE__)
	.size burrow__mcontext_switch_raw, . - burrow__mcontext_switch_raw
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
	.globl SYM(burrow__mcontext_make_asm)
#if !defined(__APPLE__)
	.type burrow__mcontext_make_asm,@function
#endif
	.p2align 4
SYM(burrow__mcontext_make_asm):
	.cfi_startproc
	movq	%rdi, %rax
	andq	$-16, %rax
	subq	$64, %rax

	stmxcsr	(%rax)
	fnstcw	4(%rax)
	movq	$0, 8(%rax)	/* r15 */
	movq	$0, 16(%rax)	/* r14 */
	movq	$0, 24(%rax)	/* r13 */
	movq	%rsi, 32(%rax)	/* r12, which the trampoline reads */
	movq	$0, 40(%rax)	/* rbx */
	movq	$0, 48(%rax)	/* rbp, zero so a frame pointer walk stops here */

	leaq	SYM(burrow__mcontext_entry)(%rip), %rcx
	movq	%rcx, 56(%rax)
	ret
	.cfi_endproc
#if !defined(__APPLE__)
	.size burrow__mcontext_make_asm, . - burrow__mcontext_make_asm
#endif

/* Where a fresh context begins. Reached by the ret above rather than by a call,
 * so there is no return address underneath and nothing to unwind into.
 *
 * .cfi_undefined on the return address is what says that: a debugger or a
 * profiler walking this stack stops here cleanly instead of reading whatever
 * the buffer happened to contain before. */
	.globl SYM(burrow__mcontext_entry)
#if !defined(__APPLE__)
	.type burrow__mcontext_entry,@function
#endif
	.p2align 4
SYM(burrow__mcontext_entry):
	.cfi_startproc
	.cfi_undefined %rip
	movq	%r12, %rdi
	call	SYM(burrow__mcontext_start)
	/* burrow__mcontext_start is declared _Noreturn and ends in a fatal error,
	 * so this is only reached if that stops being true. */
	ud2
	.cfi_endproc
#if !defined(__APPLE__)
	.size burrow__mcontext_entry, . - burrow__mcontext_entry
#endif

/* Says the stack does not need to be executable. Without it the linker assumes
 * the worst for the whole program, because one object file with no opinion is
 * enough to mark every stack in the binary. */
#if !defined(__APPLE__)
	.section .note.GNU-stack,"",@progbits
#endif

#endif /* __x86_64__ && !_WIN32 && !BURROW_PORTABLE_CONTEXT */
