/* Derived from Go's src/os/user/user_test.go, lookup_unix_test.go,
 * listgroups_unix_test.go and cgo_lookup_unix_test.go. Go source: go1.27.1.
 *
 * Go runs lookup_unix_test.go and listgroups_unix_test.go only when it uses
 * the /etc parsers, but here the parsers are in every build, so their tests
 * run everywhere. TestNegativeUid needs the libc path, so it skips under
 * BURROW_OSUSERGO and on Windows. Go's userBuffer = 1 and groupBuffer = 1,
 * which make the first buffer too small so the retry runs, are the buf
 * arguments of burrow__user_current and burrow__user_lookup_group_id.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/os/user_internal.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/os/user.h"
#include "burrow/sort.h"
#include "burrow/strings.h"

#include <stdio.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

#if defined(BURROW_OS_WINDOWS)
#define USER_IMPLEMENTED false
#define HAS_CGO false
#elif defined(BURROW_OSUSERGO)
#define USER_IMPLEMENTED true
#define HAS_CGO false
#else
#define USER_IMPLEMENTED true
#define HAS_CGO true
#endif

static Arena ar;
static Alloc *a;
static bool has_user;
static bool has_home;

static void check_user(TestingT *t) {
    if (!USER_IMPLEMENTED)
        testing_t_skip_v(t, "user: not implemented; skipping tests");
}

/* Current, or a skip when it cannot work here, as Go's tests do. */
static User *current_or_skip(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    User *u = user_current(a, &e);
    if (BURROW_FAILED(e)) {
        if (HAS_CGO || (has_user && has_home))
            testing_t_fatalf_v(t, "Current: %v", e);
        testing_t_skipf_v(t, "skipping: %v", e);
    }
    return u;
}

static void TestCurrent(TestingT *t) {
    check_user(t);
    Error e = BURROW_NO_ERROR;
    User *u = burrow__user_current(a, 1, &e); /* force use of retry code */
    if (BURROW_FAILED(e)) {
        if (HAS_CGO || (has_user && has_home))
            testing_t_fatalf_v(t, "Current: %v", e);
        testing_t_skipf_v(t, "skipping: %v", e);
    }
    if (u->home_dir.len == 0)
        testing_t_errorf_v(t, "didn't get a HomeDir");
    if (u->username.len == 0)
        testing_t_errorf_v(t, "didn't get a username");
}

static void compare(TestingT *t, const User *want, const User *got) {
    if (!str_eq(want->uid, got->uid))
        testing_t_errorf_v(t, "got Uid=%q; want %q", got->uid, want->uid);
    if (!str_eq(want->username, got->username))
        testing_t_errorf_v(t, "got Username=%q; want %q", got->username,
                           want->username);
    if (!str_eq(want->name, got->name))
        testing_t_errorf_v(t, "got Name=%q; want %q", got->name, want->name);
    if (!str_eq(want->home_dir, got->home_dir))
        testing_t_errorf_v(t, "got HomeDir=%q; want %q", got->home_dir, want->home_dir);
    if (!str_eq(want->gid, got->gid))
        testing_t_errorf_v(t, "got Gid=%q; want %q", got->gid, want->gid);
}

static void TestLookup(TestingT *t) {
    check_user(t);
    User *want = current_or_skip(t);
    Error e = BURROW_NO_ERROR;
    User *got = user_lookup(a, want->username, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Lookup: %v", e);
    compare(t, want, got);
}

static void TestLookupId(TestingT *t) {
    check_user(t);
    User *want = current_or_skip(t);
    Error e = BURROW_NO_ERROR;
    User *got = user_lookup_id(a, want->uid, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "LookupId: %v", e);
    compare(t, want, got);
}

static void TestLookupGroup(TestingT *t) {
    check_user(t);
    User *u = current_or_skip(t);
    Error e = BURROW_NO_ERROR;
    UserGroup *g1 = burrow__user_lookup_group_id(a, u->gid, 1, &e); /* force retry */
    if (BURROW_FAILED(e)) {
        /* Maybe the group is not defined. That is fine. On rsc's OS X laptop
         * he logs in with group 5000 even though there is no name for group
         * 5000. Such is Unix. */
        testing_t_logf_v(t, "LookupGroupId(%q): %v", u->gid, e);
        return;
    }
    if (!str_eq(g1->gid, u->gid))
        testing_t_errorf_v(t, "LookupGroupId(%q).Gid = %s; want %s", u->gid, g1->gid,
                           u->gid);

    UserGroup *g2 = user_lookup_group(a, g1->name, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "LookupGroup(%q): %v", g1->name, e);
    if (!str_eq(g1->gid, g2->gid) || !str_eq(g1->name, g2->name))
        testing_t_errorf_v(t, "LookupGroup(%q) = {%s %s}; want {%s %s}", g1->name,
                           g2->gid, g2->name, g1->gid, g1->name);

    /* The uncached path gives the same answer. */
    UserGroup *g3 = user_lookup_group_id(a, u->gid, &e);
    if (BURROW_FAILED(e) || !str_eq(g3->name, g1->name))
        testing_t_errorf_v(t, "LookupGroupId(%q) = %v; want %s", u->gid, e, g1->name);
}

static void TestGroupIds(TestingT *t) {
    check_user(t);
    User *u = current_or_skip(t);
    Error e = BURROW_NO_ERROR;
    Slice gids = user_group_ids(u, a, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s.GroupIds(): %v", u->username, e);
    const Str *g = (const Str *)gids.p;
    for (Int i = 0; i < gids.len; i++)
        if (str_eq(g[i], u->gid))
            return;
    testing_t_errorf_v(t, "%s.GroupIds() has %d entries; does not contain user GID %s",
                       u->username, gids.len, u->gid);
}

/* Not in Go: Lookup and LookupId of the current user come from the cache,
 * and a copy from it is the caller's to free. */
static void TestCurrentCopies(TestingT *t) {
    check_user(t);
    (void)current_or_skip(t);
    Alloc *h = heap_allocator();
    Error e = BURROW_NO_ERROR;
    User *u1 = user_current(h, &e);
    User *u2 = user_current(h, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Current: %v", e);
    if (u1 == u2)
        testing_t_errorf_v(t, "Current returned the same pointer twice");
    compare(t, u1, u2);
    user_free(h, u1);
    user_free(h, u2);
}

/* ------------------------------------------------------- lookup_unix_test */

/* prefix + s, for the messages the tests expect. */
static Str concat(Str prefix, Str s) {
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(prefix.len + s.len) + 1, 1);
    memcpy(p, prefix.p, (size_t)prefix.len);
    if (s.len > 0)
        memcpy(p + prefix.len, s.p, (size_t)s.len);
    return str_from_bytes(p, prefix.len + s.len);
}

static Str test_group_file;

static void make_test_group_file(void) {
    static const char head[] = "# See the opendirectoryd(8) man page for additional\n"
                               "# information about Open Directory.\n"
                               "##\n"
                               "nobody:*:-2:\n"
                               "nogroup:*:-1:\n"
                               "wheel:*:0:root\n"
                               "emptyid:*::root\n"
                               "invalidgid:*:notanumber:root\n"
                               "+plussign:*:20:root\n"
                               "-minussign:*:21:root\n"
                               "# Next line is invalid (empty group name)\n"
                               ":*:22:root\n"
                               "\n"
                               "daemon:*:1:root\n"
                               "    indented:*:7:root\n"
                               "# comment:*:4:found\n"
                               "     # comment:*:4:found\n"
                               "kmem:*:2:root\n"
                               "manymembers:x:777:jill,jody,john,jack,jov,user777\n"
                               "largegroup:x:1000:user1";
    size_t cap = sizeof head + 7500 * 10;
    char *buf = (char *)mem_alloc_nozero(a, cap, 1);
    size_t n = sizeof head - 1;
    memcpy(buf, head, n);
    for (int i = 2; i <= 7500; i++)
        n += (size_t)snprintf(buf + n, cap - n, ",user%d", i);
    test_group_file = str_from_bytes((const Byte *)buf, (Int)n);
}

typedef struct GroupTest {
    bool in; /* testGroupFile or "" */
    Str name;
    Str gid;
} GroupTest;

static IoReader reader(bool in, StringsReader *r) {
    strings_reader_reset(r, in ? test_group_file : S(""));
    return strings_reader_as_io_reader(r);
}

static void TestFindGroupName(TestingT *t) {
    const GroupTest tests[] = {
        {true, S("nobody"), S("-2")},       {true, S("kmem"), S("2")},
        {true, S("notinthefile"), S("")},   {true, S("comment"), S("")},
        {true, S("plussign"), S("")},       {true, S("+plussign"), S("")},
        {true, S("-minussign"), S("")},     {true, S("minussign"), S("")},
        {true, S("emptyid"), S("")},        {true, S("invalidgid"), S("")},
        {true, S("indented"), S("7")},      {true, S("# comment"), S("")},
        {true, S("largegroup"), S("1000")}, {true, S("manymembers"), S("777")},
        {false, S("emptyfile"), S("")},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const GroupTest *tt = &tests[i];
        StringsReader r;
        Error e = BURROW_NO_ERROR;
        UserGroup *got =
            burrow__user_find_group_name(a, tt->name, reader(tt->in, &r), &e);
        if (tt->gid.len == 0) {
            if (BURROW_OK(e)) {
                testing_t_errorf_v(t, "findGroupName(%s): got nil error, expected err",
                                   tt->name);
                continue;
            }
            const Str *terr = errors_as(e, TYPE_USER_UNKNOWN_GROUP_ERROR);
            if (terr == NULL) {
                testing_t_errorf_v(t, "findGroupName(%s): got unexpected error %v",
                                   tt->name, e);
                continue;
            }
            Str want = concat(S("group: unknown group "), tt->name);
            if (!str_eq(error_text(e), want))
                testing_t_errorf_v(t, "findGroupName(%s): got %v, want %v", tt->name, e,
                                   tt->name);
        } else {
            if (BURROW_FAILED(e))
                testing_t_fatalf_v(t, "findGroupName(%s): got unexpected error %v",
                                   tt->name, e);
            if (!str_eq(got->gid, tt->gid))
                testing_t_errorf_v(t, "findGroupName(%s): got gid %v, want %s",
                                   tt->name, got->gid, tt->gid);
            if (!str_eq(got->name, tt->name))
                testing_t_errorf_v(t, "findGroupName(%s): got name %s, want %s",
                                   tt->name, got->name, tt->name);
        }
    }
}

static void TestFindGroupId(TestingT *t) {
    /* name and gid swap roles: gid is the input, name the answer. */
    const GroupTest tests[] = {
        {true, S("nobody"), S("-2")},     {true, S("kmem"), S("2")},
        {true, S(""), S("notinthefile")}, {true, S(""), S("comment")},
        {true, S("indented"), S("7")},    {true, S(""), S("4")},
        {true, S(""), S("20")}, /* row starts with a plus */
        {true, S(""), S("21")}, /* row starts with a minus */
        {false, S(""), S("emptyfile")},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const GroupTest *tt = &tests[i];
        StringsReader r;
        Error e = BURROW_NO_ERROR;
        UserGroup *got = burrow__user_find_group_id(a, tt->gid, reader(tt->in, &r), &e);
        if (tt->name.len == 0) {
            if (BURROW_OK(e)) {
                testing_t_errorf_v(t, "findGroupId(%s): got nil error, expected err",
                                   tt->gid);
                continue;
            }
            if (errors_as(e, TYPE_USER_UNKNOWN_GROUP_ID_ERROR) == NULL) {
                testing_t_errorf_v(t, "findGroupId(%s): got unexpected error %v",
                                   tt->name, e);
                continue;
            }
            Str want = concat(S("group: unknown groupid "), tt->gid);
            if (!str_eq(error_text(e), want))
                testing_t_errorf_v(t, "findGroupId(%s): got %v, want %v", tt->name, e,
                                   tt->name);
        } else {
            if (BURROW_FAILED(e))
                testing_t_fatalf_v(t, "findGroupId(%s): got unexpected error %v",
                                   tt->name, e);
            if (!str_eq(got->gid, tt->gid))
                testing_t_errorf_v(t, "findGroupId(%s): got gid %v, want %s", tt->name,
                                   got->gid, tt->gid);
            if (!str_eq(got->name, tt->name))
                testing_t_errorf_v(t, "findGroupId(%s): got name %s, want %s", tt->name,
                                   got->name, tt->name);
        }
    }
}

static const Str test_user_file = BURROW_S_INIT(
    "   # Example user file\n"
    "root:x:0:0:root:/root:/bin/bash\n"
    "daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin\n"
    "bin:x:2:3:bin:/bin:/usr/sbin/nologin\n"
    "     indented:x:3:3:indented:/dev:/usr/sbin/nologin\n"
    "sync:x:4:65534:sync:/bin:/bin/sync\n"
    "negative:x:-5:60:games:/usr/games:/usr/sbin/nologin\n"
    "man:x:6:12:man:/var/cache/man:/usr/sbin/nologin\n"
    "allfields:x:6:12:mansplit,man2,man3,man4:/home/allfields:/usr/sbin/nologin\n"
    "+plussign:x:8:10:man:/var/cache/man:/usr/sbin/nologin\n"
    "-minussign:x:9:10:man:/var/cache/man:/usr/sbin/nologin\n"
    "\n"
    "malformed:x:27:12 # more:colons:after:comment\n"
    "\n"
    "struid:x:notanumber:12 # more:colons:after:comment\n"
    "\n"
    "# commented:x:28:12:commented:/var/cache/man:/usr/sbin/nologin\n"
    "      # commentindented:x:29:12:commentindented:/var/cache/man:/usr/sbin/"
    "nologin\n"
    "\n"
    "struid2:x:30:badgid:struid2name:/home/struid:/usr/sbin/nologin\n");

typedef struct UserTest {
    bool in; /* testUserFile or "" */
    Str name;
    Str uid;
} UserTest;

static IoReader user_reader(bool in, StringsReader *r) {
    strings_reader_reset(r, in ? test_user_file : S(""));
    return strings_reader_as_io_reader(r);
}

static void TestInvalidUserId(TestingT *t) {
    StringsReader r;
    Error e = BURROW_NO_ERROR;
    (void)burrow__user_find_user_id(a, S("notanumber"), user_reader(false, &r), &e);
    if (BURROW_OK(e))
        testing_t_fatalf_v(t, "findUserId('notanumber'): got nil error");
    Str want = S("user: invalid userid notanumber");
    if (!str_eq(error_text(e), want))
        testing_t_errorf_v(t, "findUserId('notanumber'): got %v, want %s", e, want);
}

static void TestLookupUserId(TestingT *t) {
    const UserTest tests[] = {
        {true, S("negative"), S("-5")},
        {true, S("bin"), S("2")},
        {true, S(""), S("100")}, /* not in the file */
        {true, S(""), S("8")},   /* plus sign, glibc doesn't find it */
        {true, S(""), S("9")},   /* minus sign, glibc doesn't find it */
        {true, S(""), S("27")},  /* malformed */
        {true, S(""), S("28")},  /* commented out */
        {true, S(""), S("29")},  /* commented out, indented */
        {true, S("indented"), S("3")},
        {true, S(""), S("30")}, /* the Gid is not valid, shouldn't match */
        {false, S(""), S("1")},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const UserTest *tt = &tests[i];
        StringsReader r;
        Error e = BURROW_NO_ERROR;
        User *got = burrow__user_find_user_id(a, tt->uid, user_reader(tt->in, &r), &e);
        if (tt->name.len == 0) {
            if (BURROW_OK(e)) {
                testing_t_errorf_v(t, "findUserId(%s): got nil error, expected err",
                                   tt->uid);
                continue;
            }
            if (errors_as(e, TYPE_USER_UNKNOWN_USER_ID_ERROR) == NULL) {
                testing_t_errorf_v(t, "findUserId(%s): got unexpected error %v",
                                   tt->name, e);
                continue;
            }
            Str want = concat(S("user: unknown userid "), tt->uid);
            if (!str_eq(error_text(e), want))
                testing_t_errorf_v(t, "findUserId(%s): got %v, want %v", tt->name, e,
                                   want);
        } else {
            if (BURROW_FAILED(e))
                testing_t_fatalf_v(t, "findUserId(%s): got unexpected error %v",
                                   tt->name, e);
            if (!str_eq(got->uid, tt->uid))
                testing_t_errorf_v(t, "findUserId(%s): got uid %v, want %s", tt->name,
                                   got->uid, tt->uid);
            if (!str_eq(got->username, tt->name))
                testing_t_errorf_v(t, "findUserId(%s): got name %s, want %s", tt->name,
                                   got->username, tt->name);
        }
    }
}

static void TestLookupUserPopulatesAllFields(TestingT *t) {
    StringsReader r;
    Error e = BURROW_NO_ERROR;
    User *u = burrow__user_find_username(a, S("allfields"), user_reader(true, &r), &e);
    if (BURROW_FAILED(e))
        testing_t_fatal_v(t, e);
    User want = {S("6"), S("12"), S("allfields"), S("mansplit"), S("/home/allfields"),
                 0};
    if (!str_eq(u->username, want.username) || !str_eq(u->uid, want.uid) ||
        !str_eq(u->gid, want.gid) || !str_eq(u->name, want.name) ||
        !str_eq(u->home_dir, want.home_dir))
        testing_t_errorf_v(t,
                           "findUsername: got {%q %q %q %q %q}, want {%q %q %q %q %q}",
                           u->uid, u->gid, u->username, u->name, u->home_dir, want.uid,
                           want.gid, want.username, want.name, want.home_dir);
}

static void TestLookupUser(TestingT *t) {
    const UserTest tests[] = {
        {true, S("negative"), S("-5")},      {true, S("bin"), S("2")},
        {true, S("notinthefile"), S("")},    {true, S("indented"), S("3")},
        {true, S("plussign"), S("")},        {true, S("+plussign"), S("")},
        {true, S("minussign"), S("")},       {true, S("-minussign"), S("")},
        {true, S("   indented"), S("")},     {true, S("commented"), S("")},
        {true, S("commentindented"), S("")}, {true, S("malformed"), S("")},
        {true, S("# commented"), S("")},     {false, S("emptyfile"), S("")},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const UserTest *tt = &tests[i];
        StringsReader r;
        Error e = BURROW_NO_ERROR;
        User *got =
            burrow__user_find_username(a, tt->name, user_reader(tt->in, &r), &e);
        if (tt->uid.len == 0) {
            if (BURROW_OK(e)) {
                testing_t_errorf_v(t, "lookupUser(%s): got nil error, expected err",
                                   tt->uid);
                continue;
            }
            if (errors_as(e, TYPE_USER_UNKNOWN_USER_ERROR) == NULL) {
                testing_t_errorf_v(t, "lookupUser(%s): got unexpected error %v",
                                   tt->name, e);
                continue;
            }
            Str want = concat(S("user: unknown user "), tt->name);
            if (!str_eq(error_text(e), want))
                testing_t_errorf_v(t, "lookupUser(%s): got %v, want %v", tt->name, e,
                                   want);
        } else {
            if (BURROW_FAILED(e))
                testing_t_fatalf_v(t, "lookupUser(%s): got unexpected error %v",
                                   tt->name, e);
            if (!str_eq(got->uid, tt->uid))
                testing_t_errorf_v(t, "lookupUser(%s): got uid %v, want %s", tt->name,
                                   got->uid, tt->uid);
            if (!str_eq(got->username, tt->name))
                testing_t_errorf_v(t, "lookupUser(%s): got name %s, want %s", tt->name,
                                   got->username, tt->name);
        }
    }
}

/* ---------------------------------------------------- listgroups_unix_test */

typedef struct ListGroupsTest {
    bool in;
    Str user;
    Str gid;
    const char *gids; /* space separated */
    bool err;
} ListGroupsTest;

static void check_same_ids(TestingT *t, Slice got, const char *want_list) {
    Slice want = strings_fields(a, str_from_cstr(want_list));
    Str *g = (Str *)got.p;
    Str *w = (Str *)want.p;
    if (got.len != want.len) {
        testing_t_errorf_v(t, "ID list mismatch: got %d IDs; want %s", got.len,
                           want_list);
        return;
    }
    sort_strings(got);
    sort_strings(want);
    for (Int i = 0; i < want.len; i++) {
        if (!str_eq(g[i], w[i])) {
            testing_t_errorf_v(t, "ID list mismatch (at index %d): got %s; want %s", i,
                               g[i], w[i]);
            return;
        }
    }
}

static void TestListGroups(TestingT *t) {
    const ListGroupsTest tests[] = {
        {true, S("root"), S("0"), "0 1 2 7", false},
        {true, S("jill"), S("33"), "33 777", false},
        {true, S("jody"), S("34"), "34 777", false},
        {true, S("john"), S("35"), "35 777", false},
        {true, S("jov"), S("37"), "37 777", false},
        {true, S("user777"), S("7"), "7 777 1000", false},
        {true, S("user1111"), S("1111"), "1111 1000", false},
        {true, S("user1000"), S("1000"), "1000", false},
        {true, S("user7500"), S("7500"), "1000 7500", false},
        {true, S("no-such-user"), S("2345"), "2345", false},
        {false, S("no-such-user"), S("2345"), "2345", false},
        /* Error cases. */
        {false, S(""), S("2345"), "", true},
        {false, S("joanna"), S("bad"), "", true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const ListGroupsTest *tc = &tests[i];
        User u = {BURROW_STR_EMPTY, tc->gid,          tc->user,
                  BURROW_STR_EMPTY, BURROW_STR_EMPTY, 0};
        StringsReader r;
        Error e = BURROW_NO_ERROR;
        Slice got = burrow__user_list_groups_from_reader(&u, a, reader(tc->in, &r), &e);
        if (tc->err) {
            if (BURROW_OK(e))
                testing_t_errorf_v(t, "listGroups(%q): got nil; want error", tc->user);
            continue; /* no more checks */
        }
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "listGroups(%q): got %v error, want nil", tc->user,
                               e);
            continue; /* no more checks */
        }
        check_same_ids(t, got, tc->gids);
    }
}

/* ------------------------------------------------- cgo_lookup_unix_test */

/* Issue 22739. */
static void TestNegativeUid(TestingT *t) {
    if (!HAS_CGO)
        testing_t_skip_v(t, "needs the libc lookups");
    /* Go's structPasswdForNegativeTest: uid and gid of 1<<32 - 2 and 1<<32 - 3. */
    PalPasswd sp = {"", "", "", 0, 0};
    sp.uid = (uint32_t)(((uint64_t)1 << 32) - 2);
    sp.gid = (uint32_t)(((uint64_t)1 << 32) - 3);
    User *u = burrow__user_build(a, &sp);
    if (!str_eq(u->uid, S("4294967294")))
        testing_t_errorf_v(t, "Uid = %q; want %q", u->uid, S("4294967294"));
    if (!str_eq(u->gid, S("4294967293")))
        testing_t_errorf_v(t, "Gid = %q; want %q", u->gid, S("4294967293"));
}

/* Not in Go: the error types compare by value, the way Go's == does. */
static void TestErrorValues(TestingT *t) {
    Alloc *ea = error_allocator();
    Error e1 = user_unknown_user_id_error_as_error(42, ea);
    if (!errors_is(e1, user_unknown_user_id_error_as_error(42, ea)))
        testing_t_errorf_v(t, "UnknownUserIdError(42) is not itself");
    if (errors_is(e1, user_unknown_user_id_error_as_error(43, ea)))
        testing_t_errorf_v(t, "UnknownUserIdError(42) is UnknownUserIdError(43)");
    const Int *id = errors_as(e1, TYPE_USER_UNKNOWN_USER_ID_ERROR);
    if (id == NULL || *id != 42)
        testing_t_errorf_v(t, "errors.As gave the wrong id");
    Error e2 = user_unknown_group_error_as_error(S("wheel"), ea);
    if (errors_is(e2, user_unknown_group_id_error_as_error(S("wheel"), ea)))
        testing_t_errorf_v(t, "UnknownGroupError is UnknownGroupIdError");
    if (!errors_is(e2, user_unknown_group_error_as_error(S("wheel"), ea)))
        testing_t_errorf_v(t, "UnknownGroupError(wheel) is not itself");
    Str m = user_unknown_user_id_error_error(-7, a);
    if (!str_eq(m, S("user: unknown userid -7")))
        testing_t_errorf_v(t, "got %q", m);
    m = user_unknown_user_error_error(S("bob"), a);
    if (!str_eq(m, S("user: unknown user bob")))
        testing_t_errorf_v(t, "got %q", m);
}

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    has_user = os_getenv(a, S("USER")).len > 0;
    has_home = os_getenv(a, S("HOME")).len > 0;
    make_test_group_file();
}

#define TESTS(X)                                                                       \
    X(TestCurrent)                                                                     \
    X(TestLookup)                                                                      \
    X(TestLookupId)                                                                    \
    X(TestLookupGroup)                                                                 \
    X(TestGroupIds)                                                                    \
    X(TestCurrentCopies)                                                               \
    X(TestFindGroupName)                                                               \
    X(TestFindGroupId)                                                                 \
    X(TestInvalidUserId)                                                               \
    X(TestLookupUserId)                                                                \
    X(TestLookupUserPopulatesAllFields)                                                \
    X(TestLookupUser)                                                                  \
    X(TestListGroups)                                                                  \
    X(TestNegativeUid)                                                                 \
    X(TestErrorValues)

static int os_user_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(os_user_main, TESTS)
