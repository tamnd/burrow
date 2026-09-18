/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

/* Two functions, and the whole point of them is that platform.h did the work at
 * compile time. If this file ever grows a probe, something has gone wrong with
 * the no configure step rule.
 *
 * The one case that will eventually need more than this is a cosmopolitan
 * build, which is genuinely a different operating system depending on where you
 * ran it. Answering that properly needs the platform layer, so it is a lie this
 * file will stop telling in P2 rather than one it can fix today. */

const char *burrow_os_name(void) {
    return BURROW_OS_NAME;
}

const char *burrow_arch_name(void) {
    return BURROW_ARCH_NAME;
}
