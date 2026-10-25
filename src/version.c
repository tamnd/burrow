/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/version.h"

/* Set by the build when it knows, which is release builds and any build out of
 * a clean checkout. A dirty tree gets "unknown" on purpose, because a source id
 * that is almost right is worse than one that admits it does not know. */
#ifndef BURROW_SOURCE_ID
#define BURROW_SOURCE_ID "unknown"
#endif

/* Pinned rather than detected. The port is read against one Go release at a
 * time and moving to the next one is a deliberate act with a changelog entry,
 * not something that happens because a developer upgraded their toolchain. */
#define BURROW_GO_VERSION "go1.27.3"

const char *burrow_version(void) {
    return BURROW_VERSION_STRING;
}

int burrow_version_number(void) {
    return BURROW_VERSION_NUMBER;
}

const char *burrow_sourceid(void) {
    return BURROW_SOURCE_ID;
}

const char *burrow_go_version(void) {
    return BURROW_GO_VERSION;
}

const char *burrow_license(void) {
    return "burrow\n"
           "Copyright 2026 The burrow Authors\n"
           "Copyright 2009 The Go Authors\n"
           "\n"
           "burrow is a C port of the Go standard library. Substantial portions of\n"
           "this software are derived from the Go programming language's standard\n"
           "library, Copyright 2009 The Go Authors, licensed under the BSD 3-Clause\n"
           "licence below, with an additional patent grant.\n"
           "\n"
           "burrow is not affiliated with, endorsed by, or sponsored by Google or\n"
           "the Go project. Go is a trademark of Google LLC.\n"
           "\n"
           "Redistribution and use in source and binary forms, with or without\n"
           "modification, are permitted provided that the following conditions are\n"
           "met:\n"
           "\n"
           "   * Redistributions of source code must retain the above copyright\n"
           "notice, this list of conditions and the following disclaimer.\n"
           "   * Redistributions in binary form must reproduce the above\n"
           "copyright notice, this list of conditions and the following disclaimer\n"
           "in the documentation and/or other materials provided with the\n"
           "distribution.\n"
           "   * Neither the name of Google LLC nor the names of its\n"
           "contributors may be used to endorse or promote products derived from\n"
           "this software without specific prior written permission.\n"
           "\n"
           "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
           "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
           "LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\n"
           "A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
           "OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\n"
           "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n"
           "LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
           "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
           "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
           "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
           "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
           "\n"
           "Additional IP Rights Grant (Patents)\n"
           "\n"
           "\"This implementation\" means the copyrightable works distributed by\n"
           "Google as part of the Go project.\n"
           "\n"
           "Google hereby grants to You a perpetual, worldwide, non-exclusive,\n"
           "no-charge, royalty-free, irrevocable (except as stated in this section)\n"
           "patent license to make, have made, use, offer to sell, sell, import,\n"
           "transfer and otherwise run, modify and propagate the contents of this\n"
           "implementation of Go, where such license applies only to those patent\n"
           "claims, both currently owned or controlled by Google and acquired in\n"
           "the future, licensable by Google that are necessarily infringed by this\n"
           "implementation of Go.  This grant does not include claims that would be\n"
           "infringed only as a consequence of further modification of this\n"
           "implementation.  If you or your agent or exclusive licensee institute or\n"
           "order or agree to the institution of patent litigation against any\n"
           "entity (including a cross-claim or counterclaim in a lawsuit) alleging\n"
           "that this implementation of Go or any code incorporated within this\n"
           "implementation of Go constitutes direct or contributory patent\n"
           "infringement, or inducement of patent infringement, then any patent\n"
           "rights granted to you under this License for this implementation of Go\n"
           "shall terminate as of the date such litigation is filed.\n";
}
