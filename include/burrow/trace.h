/* Walking the stack, which is what turns a panic into a bug report.
 *
 * A panic that prints "index out of range" and nothing else tells you what
 * happened and not where, and where is the part nobody can guess. This is the
 * machinery that answers it: given a frame to start from, follow the saved
 * frame pointers and write down the return address in each one.
 *
 * The addresses are the whole of it for now. Turning one into a file and a line
 * is symbolisation, which needs a table of function addresses that does not
 * exist yet, and the design in docs/design/06-runtime.md section 11 says where
 * that table comes from: the amalgamation generator emits it. Until then an
 * address is still worth printing, because addr2line and atos both take one.
 *
 * How the walk works is the same everywhere it works at all. A frame pointer
 * points at a pair of words, the saved frame pointer of the caller and the
 * address the call will return to, so the chain is a linked list and the walk
 * is a loop over it. What differs is whether the pair is there. It is on amd64,
 * arm64 and 386 when the code was built with frame pointers, which burrow's own
 * Makefile asks for with -fno-omit-frame-pointer and which macOS and an
 * increasing number of Linux distributions now do by default. It is not there
 * on MSVC, which describes its frames with unwind tables instead and gets the
 * platform call for this rather than a walk, and it is not there on the
 * architectures burrow has not been run on, where a walk answers no frames
 * rather than guessing at a layout nobody has tested.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TRACE_H
#define BURROW_TRACE_H

#include "burrow/core.h"
#include "burrow/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where this function was called from, which is a value rather than a place.
 *
 * That difference is the reason it is here. A frame address stops meaning
 * anything the moment its frame goes away, and the panic path is made of
 * functions that never return, which a compiler is free to enter by jumping
 * into rather than calling, reusing the frame it came from. A return address
 * copied out of that frame keeps its meaning afterwards, so it is what the
 * runtime hands along when it wants to say which frame a trace should begin at:
 * the trace is searched for it rather than counted down to.
 *
 * Spelled at the call site, because asking a helper for this gives the helper's
 * caller and not yours. */
#if defined(_MSC_VER)
#include <intrin.h>
#define BURROW_RETURN_ADDRESS _ReturnAddress()
#else
#define BURROW_RETURN_ADDRESS __builtin_return_address(0)
#endif

/* Where the walk starts, which is the frame of whoever writes this.
 *
 * It has to be spelled at the call site rather than worked out inside the
 * walker, because anything the walker can see about itself is a fact about the
 * walker, and a compiler is free to inline, merge or tail call its way out of
 * any fixed relationship between the two frames. Taking it here removes the
 * question: the walk begins at the frame of the function that asked, whatever
 * the optimiser did with everything around it.
 *
 * What the value is differs by platform because what the two implementations
 * need differs. The frame pointer walk needs a frame to start following from.
 * The Windows one asks the operating system for the whole stack and then has to
 * find the asking function in the answer, and the return address is what it
 * looks for, which is exact however many frames the optimiser removed in
 * between. Either way the promise below is the same sentence. */
#if defined(BURROW_OS_WINDOWS)
#define BURROW_WALK_FROM BURROW_RETURN_ADDRESS
#else
#define BURROW_WALK_FROM __builtin_frame_address(0)
#endif

/* The callers of the function that passed from, innermost first.
 *
 * pcs[0] is the address the asking function will return to, pcs[1] is where its
 * caller will return to, and so on outwards until the stack runs out or max is
 * reached. skip drops that many from the front, which is how a runtime function
 * keeps its own frames out of a trace the user is going to read. Answers how
 * many it wrote.
 *
 * Call it as burrow__callers(BURROW_WALK_FROM, ...) and nothing else. The macro
 * is what makes the first sentence true.
 *
 * Zero is a normal answer. It is what an architecture with no walk implemented
 * gives, what a thread whose stack bounds the system will not report gives, and
 * what a frame pointer that has been overwritten gives, which is a thing that
 * happens in exactly the situation where somebody wanted a stack trace. The
 * walk never faults on a broken chain: every frame is checked against the
 * bounds of the stack it should be on, and every step has to move up it. */
Int burrow__callers(void *from, Int skip, Uintptr *pcs, Int max);

/* How many frames burrow collects when it prints a trace of its own.
 *
 * Go stops at a hundred and says how many it dropped. Sixty four is the same
 * idea with a smaller number, because this buffer sits on the stack of a
 * program that is in the middle of failing, and the frames worth reading are at
 * the top. */
#define BURROW_TRACEBACK_MAX 64

/* Prints frames collected by the walk above to standard error, one address a
 * line, indented the way Go indents the lines under a goroutine.
 *
 * Separate from the walk because the two happen in different frames: the panic
 * printer collects where the program went wrong and prints it several calls
 * later, once it has worked out what to say above it. It allocates nothing and
 * formats nothing but hexadecimal, for the reason the whole of that path is
 * written the way it is: running out of memory is one of the ways a program
 * arrives here. The names arrive with symbolisation, and they arrive here. */
void burrow__traceback(const Uintptr *pcs, Int n);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TRACE_H */
