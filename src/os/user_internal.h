/* The parts of os/user its tests reach into, as Go's tests do from inside the
 * package. The parsers work on any reader and are built on every system, so
 * Go's tables for them run everywhere, not only in an osusergo build.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_OS_USER_INTERNAL_H
#define BURROW_SRC_OS_USER_INTERNAL_H

#include "burrow/os/user.h"

#include "burrow/io.h"
#include "burrow/pal.h"

/* findGroupName, findGroupId, findUsername and findUserId: a lookup in r,
 * which reads like /etc/group or /etc/passwd. */
BURROW_OWNS(ret) UserGroup *burrow__user_find_group_name(Alloc *a, Str name, IoReader r,
                                                         Error *err);
BURROW_OWNS(ret) UserGroup *burrow__user_find_group_id(Alloc *a, Str id, IoReader r,
                                                       Error *err);
BURROW_OWNS(ret) User *burrow__user_find_username(Alloc *a, Str name, IoReader r,
                                                  Error *err);
BURROW_OWNS(ret) User *burrow__user_find_user_id(Alloc *a, Str uid, IoReader r,
                                                 Error *err);

/* listGroupsFromReader: u's groups from r, which reads like /etc/group. Like
 * Go's it can return the groups found so far along with an error. */
BURROW_OWNS(ret) Slice burrow__user_list_groups_from_reader(const User *u, Alloc *a,
                                                            IoReader r, Error *err);

/* buildUser, for Go's TestNegativeUid. */
BURROW_OWNS(ret) User *burrow__user_build(Alloc *a, const PalPasswd *pw);

/* current and lookupGroupId without the cache, with the first buffer for the
 * libc calls buf bytes long. Go's tests set userBuffer and groupBuffer to 1 to
 * get the retry path, and this is how these tests do it. 0 is the usual size.
 * In an osusergo build buf is ignored, as it is in Go. */
BURROW_OWNS(ret) User *burrow__user_current(Alloc *a, int64_t buf, Error *err);
BURROW_OWNS(ret) UserGroup *burrow__user_lookup_group_id(Alloc *a, Str gid, int64_t buf,
                                                         Error *err);

#endif /* BURROW_SRC_OS_USER_INTERNAL_H */
