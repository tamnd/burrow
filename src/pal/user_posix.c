/* The user and group databases on everything but Windows, through libc, which
 * is the path Go takes when it has cgo and the one it always takes on macOS.
 * libc asks NSS, so this finds users from LDAP and the like as well as the
 * ones in /etc/passwd.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

static PalErrno user_cap_ok(char *buf, int64_t cap, bool *found) {
    *found = false;
    if (cap <= 0 || buf == NULL)
        return PAL_EINVAL;
    return PAL_OK;
}

#if defined(BURROW_OS_WASI)

/* wasip1 has no user database in its libc. os/user reads /etc/passwd and
 * /etc/group there instead, as Go does, so these only say there is nothing. */

PalErrno pal_getpwnam(const char *name, PalPasswd *pw, char *buf, int64_t cap,
                      bool *found) {
    (void)name;
    (void)pw;
    PalErrno e = user_cap_ok(buf, cap, found);
    return e != PAL_OK ? e : PAL_ENOSYS;
}

PalErrno pal_getpwuid(uint32_t uid, PalPasswd *pw, char *buf, int64_t cap,
                      bool *found) {
    (void)uid;
    (void)pw;
    PalErrno e = user_cap_ok(buf, cap, found);
    return e != PAL_OK ? e : PAL_ENOSYS;
}

PalErrno pal_getgrnam(const char *name, PalGroup *gr, char *buf, int64_t cap,
                      bool *found) {
    (void)name;
    (void)gr;
    PalErrno e = user_cap_ok(buf, cap, found);
    return e != PAL_OK ? e : PAL_ENOSYS;
}

PalErrno pal_getgrgid(uint32_t gid, PalGroup *gr, char *buf, int64_t cap, bool *found) {
    (void)gid;
    (void)gr;
    PalErrno e = user_cap_ok(buf, cap, found);
    return e != PAL_OK ? e : PAL_ENOSYS;
}

int64_t pal_user_buf_size(bool group) {
    (void)group;
    return -1;
}

int pal_getgrouplist(const char *name, uint32_t gid, uint32_t *gids, int *n) {
    (void)name;
    (void)gid;
    (void)gids;
    *n = 0;
    errno = ENOSYS;
    return -1;
}

#else

#include <grp.h>
#include <pwd.h>

_Static_assert(sizeof(uid_t) == sizeof(uint32_t), "uid_t is 32 bits");
_Static_assert(sizeof(gid_t) == sizeof(uint32_t), "gid_t is 32 bits");

static PalErrno user_passwd(int rc, struct passwd *res, PalPasswd *pw, bool *found) {
    if (rc != 0)
        return burrow__pal_errno(rc);
    if (res == NULL)
        return PAL_OK;
    pw->name = res->pw_name;
    pw->gecos = res->pw_gecos;
    pw->dir = res->pw_dir;
    pw->uid = (uint32_t)res->pw_uid;
    pw->gid = (uint32_t)res->pw_gid;
    *found = true;
    return PAL_OK;
}

static PalErrno user_group(int rc, struct group *res, PalGroup *gr, bool *found) {
    if (rc != 0)
        return burrow__pal_errno(rc);
    if (res == NULL)
        return PAL_OK;
    gr->name = res->gr_name;
    gr->gid = (uint32_t)res->gr_gid;
    *found = true;
    return PAL_OK;
}

PalErrno pal_getpwnam(const char *name, PalPasswd *pw, char *buf, int64_t cap,
                      bool *found) {
    PalErrno e = user_cap_ok(buf, cap, found);
    if (e != PAL_OK)
        return e;
    struct passwd p, *res = NULL;
    int rc = getpwnam_r(name, &p, buf, (size_t)cap, &res);
    return user_passwd(rc, res, pw, found);
}

PalErrno pal_getpwuid(uint32_t uid, PalPasswd *pw, char *buf, int64_t cap,
                      bool *found) {
    PalErrno e = user_cap_ok(buf, cap, found);
    if (e != PAL_OK)
        return e;
    struct passwd p, *res = NULL;
    int rc = getpwuid_r((uid_t)uid, &p, buf, (size_t)cap, &res);
    return user_passwd(rc, res, pw, found);
}

PalErrno pal_getgrnam(const char *name, PalGroup *gr, char *buf, int64_t cap,
                      bool *found) {
    PalErrno e = user_cap_ok(buf, cap, found);
    if (e != PAL_OK)
        return e;
    struct group g, *res = NULL;
    int rc = getgrnam_r(name, &g, buf, (size_t)cap, &res);
    return user_group(rc, res, gr, found);
}

PalErrno pal_getgrgid(uint32_t gid, PalGroup *gr, char *buf, int64_t cap, bool *found) {
    PalErrno e = user_cap_ok(buf, cap, found);
    if (e != PAL_OK)
        return e;
    struct group g, *res = NULL;
    int rc = getgrgid_r((gid_t)gid, &g, buf, (size_t)cap, &res);
    return user_group(rc, res, gr, found);
}

int64_t pal_user_buf_size(bool group) {
#if defined(_SC_GETPW_R_SIZE_MAX) && defined(_SC_GETGR_R_SIZE_MAX)
    long n = sysconf(group ? _SC_GETGR_R_SIZE_MAX : _SC_GETPW_R_SIZE_MAX);
    return n < 0 ? -1 : (int64_t)n;
#else
    (void)group;
    return -1;
#endif
}

int pal_getgrouplist(const char *name, uint32_t gid, uint32_t *gids, int *n) {
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
    /* Apple's takes ints, which are the same size. */
    return getgrouplist(name, (int)gid, (int *)(void *)gids, n);
#else
    return getgrouplist(name, (gid_t)gid, (gid_t *)(void *)gids, n);
#endif
}

#endif /* !BURROW_OS_WASI */

/* The Windows account calls, which have nothing to ask here. */

PalErrno pal_win_current_user(char *buf, int64_t cap, int *stage) {
    (void)buf, (void)cap;
    *stage = 0;
    return PAL_ENOTSUP;
}

PalErrno pal_win_current_groups(char *buf, int64_t cap, int *n, int *stage) {
    (void)buf, (void)cap;
    *n = 0;
    *stage = 0;
    return PAL_ENOTSUP;
}

PalErrno pal_win_lookup_name(const char *name, char *buf, int64_t cap, uint32_t *type,
                             bool *service) {
    (void)name, (void)buf, (void)cap;
    *type = 0;
    *service = false;
    return PAL_ENOTSUP;
}

PalErrno pal_win_lookup_sid(const char *sid, char *buf, int64_t cap, uint32_t *type,
                            bool *service) {
    (void)sid, (void)buf, (void)cap;
    *type = 0;
    *service = false;
    return PAL_ENOTSUP;
}

PalErrno pal_win_domain_joined(bool *joined) {
    *joined = false;
    return PAL_ENOTSUP;
}

PalErrno pal_win_display_name(const char *account, char *buf, int64_t cap) {
    (void)account, (void)buf, (void)cap;
    return PAL_ENOTSUP;
}

PalErrno pal_win_user_full_name(const char *server, const char *user, char *buf,
                                int64_t cap) {
    (void)server, (void)user, (void)buf, (void)cap;
    return PAL_ENOTSUP;
}

PalErrno pal_win_user_primary_group(const char *server, const char *user,
                                    uint32_t *rid) {
    (void)server, (void)user;
    *rid = 0;
    return PAL_ENOTSUP;
}

PalErrno pal_win_user_local_groups(const char *user, char *buf, int64_t cap, int *n) {
    (void)user, (void)buf, (void)cap;
    *n = 0;
    return PAL_ENOTSUP;
}

PalErrno pal_win_profile_path(const char *sid, char *buf, int64_t cap) {
    (void)sid, (void)buf, (void)cap;
    return PAL_ENOTSUP;
}

PalErrno pal_win_profiles_dir(char *buf, int64_t cap) {
    (void)buf, (void)cap;
    return PAL_ENOTSUP;
}

#endif /* !BURROW_OS_WINDOWS */
