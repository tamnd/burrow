/* os/user: looking up user accounts and groups by name or id.
 *
 *     Error err;
 *     User *me = user_current(a, &err);
 *     if (BURROW_OK(err))
 *         printf("%.*s\n", (int)me->home_dir.len, (const char *)me->home_dir.p);
 *     user_free(a, me);
 *
 * On Unix there are two ways to answer, as in Go. By default the answers come
 * from libc, getpwnam_r and the rest, which is what Go does when it has cgo
 * and what it always does on macOS. libc goes through NSS, so it finds users
 * that only exist in LDAP or systemd-homed. Build with BURROW_OSUSERGO defined,
 * which is Go's osusergo build tag, and the answers come from parsing
 * /etc/passwd and /etc/group instead, which is what Go does without cgo.
 *
 * On Windows a uid or gid is a SID such as "S-1-5-21-...-1001", a username is
 * "DOMAIN\user", and the answers come from the process token, the account
 * database and the registry, as in Go. Looking up an account that does not
 * exist gives the system's error there rather than the Unknown errors below,
 * which is also what Go does.
 *
 * A User or UserGroup is one allocation from a that holds every string in it,
 * and user_free or user_group_free gives it back. The Slice that
 * user_group_ids returns is a Slice of Str, each its own allocation, which an
 * arena is the easy way to hold.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package os/user */

#ifndef BURROW_OS_USER_H
#define BURROW_OS_USER_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* user.User, a user account. */
typedef struct User {
    /* The user id. A decimal number on Unix, and a SID such as "S-1-5-18" on
     * Windows. */
    Str uid;
    /* The primary group id, in the same form. */
    Str gid;
    /* The login name. */
    Str username;
    /* The user's real or display name, which may be empty. On Unix it is the
     * first field of the GECOS list. */
    Str name;
    /* The home directory, if the user has one. */
    Str home_dir;

    size_t size; /* of the allocation, for user_free */
} User;

/* user.Group, a group of users. */
typedef struct UserGroup {
    Str gid; /* a decimal number on Unix */
    Str name;

    size_t size; /* of the allocation, for user_group_free */
} UserGroup;

/* user.Current. The first call that works is remembered for the life of the
 * process, so later calls do not see changes to the account, as in Go. Each
 * call returns a fresh copy in a. NULL with err set when it fails. */
BURROW_OWNS(ret) User *user_current(Alloc *a, Error *err);

/* user.Lookup, by login name. A user that does not exist gives a
 * UserUnknownUserError. */
BURROW_OWNS(ret) User *user_lookup(Alloc *a, Str username, Error *err);

/* user.LookupId, by user id. A user that does not exist gives a
 * UserUnknownUserIdError. */
BURROW_OWNS(ret) User *user_lookup_id(Alloc *a, Str uid, Error *err);

/* user.LookupGroup, by group name. A group that does not exist gives a
 * UserUnknownGroupError. */
BURROW_OWNS(ret) UserGroup *user_lookup_group(Alloc *a, Str name, Error *err);

/* user.LookupGroupId, by group id. A group that does not exist gives a
 * UserUnknownGroupIdError. */
BURROW_OWNS(ret) UserGroup *user_lookup_group_id(Alloc *a, Str gid, Error *err);

/* user.User.GroupIds: the ids of the groups u is in, a Slice of Str from a,
 * with u's primary group among them. */
BURROW_OWNS(ret) Slice user_group_ids(const User *u, Alloc *a, Error *err);

/* Give back a User or UserGroup from this package. NULL is fine. */
void user_free(Alloc *a, User *u);
void user_group_free(Alloc *a, UserGroup *g);

/* user.UnknownUserIdError, the id that was not found. Its text is "user:
 * unknown userid " and the id. */
typedef Int UserUnknownUserIdError;
extern const Type *const TYPE_USER_UNKNOWN_USER_ID_ERROR;
BURROW_OWNS(ret) Str user_unknown_user_id_error_error(UserUnknownUserIdError e,
                                                      Alloc *a);
BURROW_OWNS(ret) Error user_unknown_user_id_error_as_error(UserUnknownUserIdError e,
                                                           Alloc *a);

/* user.UnknownUserError, the name that was not found. Its text is "user:
 * unknown user " and the name. */
typedef Str UserUnknownUserError;
extern const Type *const TYPE_USER_UNKNOWN_USER_ERROR;
BURROW_OWNS(ret) Str user_unknown_user_error_error(UserUnknownUserError e, Alloc *a);
BURROW_OWNS(ret) Error user_unknown_user_error_as_error(UserUnknownUserError e,
                                                        Alloc *a);

/* user.UnknownGroupIdError, the id that was not found. Its text is "group:
 * unknown groupid " and the id. */
typedef Str UserUnknownGroupIdError;
extern const Type *const TYPE_USER_UNKNOWN_GROUP_ID_ERROR;
BURROW_OWNS(ret) Str user_unknown_group_id_error_error(UserUnknownGroupIdError e,
                                                       Alloc *a);
BURROW_OWNS(ret) Error user_unknown_group_id_error_as_error(UserUnknownGroupIdError e,
                                                            Alloc *a);

/* user.UnknownGroupError, the name that was not found. Its text is "group:
 * unknown group " and the name. */
typedef Str UserUnknownGroupError;
extern const Type *const TYPE_USER_UNKNOWN_GROUP_ERROR;
BURROW_OWNS(ret) Str user_unknown_group_error_error(UserUnknownGroupError e, Alloc *a);
BURROW_OWNS(ret) Error user_unknown_group_error_as_error(UserUnknownGroupError e,
                                                         Alloc *a);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_OS_USER_H */
