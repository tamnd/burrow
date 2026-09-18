/* burrow's own version and provenance surface.
 *
 * Nothing in here is a port of anything. These are burrow's own symbols, so
 * they carry the burrow_ prefix that ported symbols deliberately do not: a
 * ported symbol's first segment is Go's package name, and there is no Go
 * package called version.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_VERSION_H
#define BURROW_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

#define BURROW_VERSION_MAJOR 0
#define BURROW_VERSION_MINOR 0
#define BURROW_VERSION_PATCH 1
#define BURROW_VERSION_STRING "0.0.1"

/* An ordered integer, so a preprocessor conditional can ask for a version the
 * way sqlite3's SQLITE_VERSION_NUMBER lets you. Major, minor and patch get two
 * digits each, which caps a component at 99 and is fine for a project that has
 * no intention of shipping a hundred minor releases. */
#define BURROW_VERSION_NUMBER                                                          \
    (BURROW_VERSION_MAJOR * 10000 + BURROW_VERSION_MINOR * 100 + BURROW_VERSION_PATCH)

/* The version of the library you linked against, which is not necessarily the
 * version of the header you compiled against. Worth checking if you ship
 * burrow as a shared object, pointless if you amalgamate, and the amalgamation
 * is the supported path. */
const char *burrow_version(void);
int burrow_version_number(void);

/* The git commit the amalgamation was generated from, or "unknown" for a build
 * out of a working tree that was not clean. Same idea as sqlite3_sourceid. */
const char *burrow_sourceid(void);

/* The Go release this port was read against, as "go1.27.1 5f0b5b1b". Every
 * ported file names its upstream source in the same terms, so this is the
 * single number that tells you what to diff against. */
const char *burrow_go_version(void);

/* The full text of LICENSE, NOTICE and PATENTS, concatenated. Embedding it
 * means a binary that ships burrow can satisfy the attribution clause by
 * printing something, which is the only mechanism some deployments have. */
const char *burrow_license(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_VERSION_H */
