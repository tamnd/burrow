/* Data independent timing, the processor mode crypto/subtle's
 * WithDataIndependentTiming asks for.
 *
 * On arm64 it is the DIT bit of PSTATE, which makes the instructions Arm lists
 * take the same time whatever their operands are. It belongs to the thread,
 * and a goroutine moves between threads, so the runtime keeps the goroutine's
 * wish next to it and sets the bit on whichever thread runs it, as Go does
 * with g.ditWanted and m.ditEnabled. A goroutine started by one that wants it
 * wants it too.
 *
 * Nothing else has such a mode that a program can turn on, so everywhere else
 * burrow__dit_supported is false and the rest does nothing.
 *
 * GODEBUG=dataindependenttiming=1 turns it on for every thread the runtime
 * starts and leaves it on, which is Go's setting of the same name.
 *
 * Derived from Go's src/runtime/dit.go and src/internal/runtime/sys/dit_arm64.s.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_DIT_H
#define BURROW_DIT_H

#include "burrow/core.h"
#include "burrow/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(BURROW_ARCH_ARM64) && (defined(__GNUC__) || defined(__clang__))
#define BURROW_DIT 1
#endif

/* Whether this processor has the mode, which is Go's sys.DITSupported. */
bool burrow__dit_supported(void);

/* Whether GODEBUG=dataindependenttiming=1 asked for it everywhere. Read once. */
bool burrow__dit_everywhere(void);

/* The bit on this thread: set it, answering whether it was set already, clear
 * it, and read it. Only to be called where burrow__dit_supported is true. */
bool burrow__dit_enable(void);
void burrow__dit_disable(void);
bool burrow__dit_enabled(void);

/* Go's dit_setEnabled and dit_setDisabled: the bit on this thread and the wish
 * on the goroutine running here, if there is one. In sched.c, since the
 * goroutine and the thread are its business. */
bool burrow__dit_set_enabled(void);
void burrow__dit_set_disabled(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_DIT_H */
