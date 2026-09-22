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
 * Pointer authentication is not used. macOS builds for arm64 rather than
 * arm64e for everything that is not a system binary, and the signed return
 * address there would have to be re-signed against the new stack pointer.
 * Branch target identification is not used either, which means a linker that
 * cares will mark the binary as not having it. Both belong with the hardening
 * pass rather than here.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Guarded to match the backend choice in burrow/mcontext.h exactly, so that a
 * Windows on arm build and a build that asked for the portable path both get an
 * empty object file rather than a duplicate symbol. */
#if defined(__aarch64__) && !defined(_WIN32) && !defined(BURROW_PORTABLE_CONTEXT)

#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

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

	.text

/* void burrow__mcontext_switch_raw(burrow__MContext *from, burrow__MContext *to)
 *
 * x0 is from, x1 is to. */
	.globl SYM(burrow__mcontext_switch_raw)
#if !defined(__APPLE__)
	.type burrow__mcontext_switch_raw,%function
#endif
	.p2align 4
SYM(burrow__mcontext_switch_raw):
	.cfi_startproc
	sub	sp, sp, #176
	.cfi_def_cfa_offset 176
	stp	x19, x20, [sp, #0]
	.cfi_offset x19, -176
	.cfi_offset x20, -168
	stp	x21, x22, [sp, #16]
	.cfi_offset x21, -160
	.cfi_offset x22, -152
	stp	x23, x24, [sp, #32]
	.cfi_offset x23, -144
	.cfi_offset x24, -136
	stp	x25, x26, [sp, #48]
	.cfi_offset x25, -128
	.cfi_offset x26, -120
	stp	x27, x28, [sp, #64]
	.cfi_offset x27, -112
	.cfi_offset x28, -104
	stp	x29, x30, [sp, #80]
	.cfi_offset x29, -96
	.cfi_offset x30, -88
	stp	d8, d9, [sp, #96]
	stp	d10, d11, [sp, #112]
	stp	d12, d13, [sp, #128]
	stp	d14, d15, [sp, #144]
	mrs	x2, fpcr
	str	x2, [sp, #160]

	/* The swap itself. The stack pointer cannot be stored directly, so it
	 * goes through x2, which is caller saved and therefore ours. */
	mov	x2, sp
	str	x2, [x0]
	ldr	x2, [x1]
	mov	sp, x2

	ldr	x2, [sp, #160]
	msr	fpcr, x2
	ldp	d14, d15, [sp, #144]
	ldp	d12, d13, [sp, #128]
	ldp	d10, d11, [sp, #112]
	ldp	d8, d9, [sp, #96]
	ldp	x29, x30, [sp, #80]
	.cfi_restore x29
	.cfi_restore x30
	ldp	x27, x28, [sp, #64]
	.cfi_restore x27
	.cfi_restore x28
	ldp	x25, x26, [sp, #48]
	.cfi_restore x25
	.cfi_restore x26
	ldp	x23, x24, [sp, #32]
	.cfi_restore x23
	.cfi_restore x24
	ldp	x21, x22, [sp, #16]
	.cfi_restore x21
	.cfi_restore x22
	ldp	x19, x20, [sp, #0]
	.cfi_restore x19
	.cfi_restore x20
	add	sp, sp, #176
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
#if !defined(__APPLE__)
	.size burrow__mcontext_switch_raw, . - burrow__mcontext_switch_raw
#endif

/* void *burrow__mcontext_make_asm(void *stack_top, burrow__MContext *self)
 *
 * x0 is one past the last byte of the caller's stack buffer and x1 is the
 * context. Returns the stack pointer the first switch should load.
 *
 * x19 gets the context and x30 gets the entry stub, so that the loads above
 * hand the trampoline what it needs and then return straight into it. x29 is
 * zeroed, which is how a frame pointer walk knows it has reached the bottom. */
	.globl SYM(burrow__mcontext_make_asm)
#if !defined(__APPLE__)
	.type burrow__mcontext_make_asm,%function
#endif
	.p2align 4
SYM(burrow__mcontext_make_asm):
	.cfi_startproc
	and	x0, x0, #-16
	sub	x0, x0, #176

	str	x1, [x0, #0]		/* x19, which the trampoline reads */
	str	xzr, [x0, #8]		/* x20 */
	stp	xzr, xzr, [x0, #16]
	stp	xzr, xzr, [x0, #32]
	stp	xzr, xzr, [x0, #48]
	stp	xzr, xzr, [x0, #64]
	str	xzr, [x0, #80]		/* x29 */

#if defined(__APPLE__)
	adrp	x2, SYM(burrow__mcontext_entry)@PAGE
	add	x2, x2, SYM(burrow__mcontext_entry)@PAGEOFF
#else
	adrp	x2, burrow__mcontext_entry
	add	x2, x2, :lo12:burrow__mcontext_entry
#endif
	str	x2, [x0, #88]		/* x30 */

	stp	xzr, xzr, [x0, #96]	/* d8, d9 */
	stp	xzr, xzr, [x0, #112]	/* d10, d11 */
	stp	xzr, xzr, [x0, #128]	/* d12, d13 */
	stp	xzr, xzr, [x0, #144]	/* d14, d15 */

	/* The floating point mode is inherited from whoever is creating the
	 * context, which is what a goroutine would expect of the goroutine that
	 * started it. */
	mrs	x2, fpcr
	str	x2, [x0, #160]
	str	xzr, [x0, #168]

	ret
	.cfi_endproc
#if !defined(__APPLE__)
	.size burrow__mcontext_make_asm, . - burrow__mcontext_make_asm
#endif

/* Where a fresh context begins. Reached by the ret above, so x30 holds this
 * address rather than a real caller's, and there is nothing underneath to
 * unwind into. .cfi_undefined on x30 is what tells a debugger to stop here. */
	.globl SYM(burrow__mcontext_entry)
#if !defined(__APPLE__)
	.type burrow__mcontext_entry,%function
#endif
	.p2align 4
SYM(burrow__mcontext_entry):
	.cfi_startproc
	.cfi_undefined x30
	mov	x0, x19
	bl	SYM(burrow__mcontext_start)
	/* burrow__mcontext_start is declared _Noreturn and ends in a fatal error,
	 * so this is only reached if that stops being true. */
	brk	#1
	.cfi_endproc
#if !defined(__APPLE__)
	.size burrow__mcontext_entry, . - burrow__mcontext_entry
#endif

/* Says the stack does not need to be executable. One object file with no
 * opinion is enough for the linker to assume the worst for the whole program. */
#if !defined(__APPLE__)
	.section .note.GNU-stack,"",%progbits
#endif

#endif /* __aarch64__ && !_WIN32 && !BURROW_PORTABLE_CONTEXT */
