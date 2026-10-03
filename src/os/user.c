/* os/user, from Go's user.go, lookup.go, lookup_unix.go, listgroups_unix.go,
 * cgo_lookup_unix.go, cgo_listgroups_unix.go and lookup_stubs.go.
 *
 * Go picks one of two implementations at build time: libc through cgo, or
 * parsing /etc/passwd and /etc/group. Both are here, with libc the default on
 * Unix and BURROW_OSUSERGO choosing the parser, which is Go's osusergo tag.
 * The parsers are built everywhere so their tests run everywhere.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os/user.h"

#include "burrow/bufio.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/syscall.h"

#include "user_internal.h"

#include <string.h>

#define USER_LIT(s) ((Str){(const Byte *)(s), (Int)sizeof(s) - 1})

#if defined(BURROW_OS_WINDOWS)
#define USER_IMPL_WINDOWS 1
#elif defined(BURROW_OSUSERGO)
#define USER_IMPL_FILES 1
#else
#define USER_IMPL_LIBC 1
#endif

/* -------------------------------------------------------------- results */

static Byte *user_put(Byte *p, Str s, Str *out) {
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    *out = str_from_bytes(p, s.len);
    return p + s.len;
}

/* One allocation for the User and its strings, so user_free is one call. */
static User *user_make(Alloc *a, Str uid, Str gid, Str username, Str name, Str home) {
    size_t size = sizeof(User) + (size_t)uid.len + (size_t)gid.len +
                  (size_t)username.len + (size_t)name.len + (size_t)home.len;
    User *u = (User *)mem_alloc_nozero(a, size, _Alignof(User));
    if (u == NULL)
        return NULL;
    Byte *p = (Byte *)(u + 1);
    p = user_put(p, uid, &u->uid);
    p = user_put(p, gid, &u->gid);
    p = user_put(p, username, &u->username);
    p = user_put(p, name, &u->name);
    (void)user_put(p, home, &u->home_dir);
    u->size = size;
    return u;
}

static UserGroup *user_group_make(Alloc *a, Str gid, Str name) {
    size_t size = sizeof(UserGroup) + (size_t)gid.len + (size_t)name.len;
    UserGroup *g = (UserGroup *)mem_alloc_nozero(a, size, _Alignof(UserGroup));
    if (g == NULL)
        return NULL;
    Byte *p = (Byte *)(g + 1);
    p = user_put(p, gid, &g->gid);
    (void)user_put(p, name, &g->name);
    g->size = size;
    return g;
}

/* A Str over a C string from the C library, empty for NULL. */
static Str user_cstr_str(const char *s) {
    return s == NULL ? BURROW_STR_EMPTY
                     : str_from_bytes((const Byte *)s, (Int)strlen(s));
}

static User *user_copy(Alloc *a, const User *u) {
    return user_make(a, u->uid, u->gid, u->username, u->name, u->home_dir);
}

void user_free(Alloc *a, User *u) {
    if (u != NULL)
        mem_free(a, u, u->size, _Alignof(User));
}

void user_group_free(Alloc *a, UserGroup *g) {
    if (g != NULL)
        mem_free(a, g, g->size, _Alignof(UserGroup));
}

/* --------------------------------------------------------------- errors */

/* The payload comes first in each box, so errors_as gives a pointer to it. */
typedef struct UserIdErrorBox {
    Int id;
    Str message;
} UserIdErrorBox;

typedef struct UserStrErrorBox {
    Str s;
    Str message;
} UserStrErrorBox;

#define USER_ERROR_TYPE(var, name, kind, T, tag)                                       \
    static const Type var = {                                                          \
        {(const Byte *)name, (Int)sizeof(name) - 1},                                   \
        {(const Byte *)"os/user", 7},                                                  \
        kind,                                                                          \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        tag,                                                                           \
        NULL,                                                                          \
    }

USER_ERROR_TYPE(user_id_error_desc, "UnknownUserIdError", KIND_INT, Int, 0x75737269U);
USER_ERROR_TYPE(user_name_error_desc, "UnknownUserError", KIND_STRING, Str,
                0x7573726eU);
USER_ERROR_TYPE(group_id_error_desc, "UnknownGroupIdError", KIND_STRING, Str,
                0x67727069U);
USER_ERROR_TYPE(group_name_error_desc, "UnknownGroupError", KIND_STRING, Str,
                0x6772706eU);

const Type *const TYPE_USER_UNKNOWN_USER_ID_ERROR = &user_id_error_desc;
const Type *const TYPE_USER_UNKNOWN_USER_ERROR = &user_name_error_desc;
const Type *const TYPE_USER_UNKNOWN_GROUP_ID_ERROR = &group_id_error_desc;
const Type *const TYPE_USER_UNKNOWN_GROUP_ERROR = &group_name_error_desc;

static Str user_id_error_message(const void *self) {
    return ((const UserIdErrorBox *)self)->message;
}

static Str user_str_error_message(const void *self) {
    return ((const UserStrErrorBox *)self)->message;
}

static bool user_id_error_is(const void *self, Error target);
static bool user_str_error_is(const void *self, Error target);
static Error user_id_error_clone(const void *self, Alloc *a);
static Error user_name_error_clone(const void *self, Alloc *a);
static Error group_id_error_clone(const void *self, Alloc *a);
static Error group_name_error_clone(const void *self, Alloc *a);

static const ErrorVT user_id_error_vt = {
    &user_id_error_desc, user_id_error_message, NULL, NULL, user_id_error_is, NULL,
    user_id_error_clone,
};
static const ErrorVT user_name_error_vt = {
    &user_name_error_desc, user_str_error_message, NULL, NULL, user_str_error_is, NULL,
    user_name_error_clone,
};
static const ErrorVT group_id_error_vt = {
    &group_id_error_desc, user_str_error_message, NULL, NULL, user_str_error_is, NULL,
    group_id_error_clone,
};
static const ErrorVT group_name_error_vt = {
    &group_name_error_desc, user_str_error_message, NULL, NULL, user_str_error_is, NULL,
    group_name_error_clone,
};

/* Go compares these with ==, which is the type and the value. */
static bool user_id_error_is(const void *self, Error target) {
    if (target.vt != &user_id_error_vt || target.data == NULL)
        return false;
    return ((const UserIdErrorBox *)self)->id ==
           ((const UserIdErrorBox *)target.data)->id;
}

static bool user_str_error_is(const void *self, Error target) {
    if (target.data == NULL ||
        (target.vt != &user_name_error_vt && target.vt != &group_id_error_vt &&
         target.vt != &group_name_error_vt))
        return false;
    /* The message says which type it is, and has the value in it. */
    const UserStrErrorBox *b = (const UserStrErrorBox *)self;
    const UserStrErrorBox *t = (const UserStrErrorBox *)target.data;
    return str_eq(b->message, t->message) && str_eq(b->s, t->s);
}

static Str user_id_text(Alloc *a, Int id) {
    Byte num[24];
    Int n = 0;
    uint64_t v = id < 0 ? (uint64_t)0 - (uint64_t)id : (uint64_t)id;
    do {
        num[sizeof num - 1 - (size_t)n++] = (Byte)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    if (id < 0)
        num[sizeof num - 1 - (size_t)n++] = '-';
    Str prefix = USER_LIT("user: unknown userid ");
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(prefix.len + n), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, prefix.p, (size_t)prefix.len);
    memcpy(p + prefix.len, num + sizeof num - (size_t)n, (size_t)n);
    return str_from_bytes(p, prefix.len + n);
}

static Error user_id_error_build(Alloc *a, Int id) {
    UserIdErrorBox *b =
        (UserIdErrorBox *)mem_alloc_nozero(a, sizeof *b, _Alignof(UserIdErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->id = id;
    b->message = user_id_text(a, id);
    if (b->message.len == 0) {
        mem_free(a, b, sizeof *b, _Alignof(UserIdErrorBox));
        return burrow_err_out_of_memory;
    }
    return (Error){&user_id_error_vt, b};
}

static Str user_str_prefix(const ErrorVT *vt) {
    if (vt == &user_name_error_vt)
        return USER_LIT("user: unknown user ");
    if (vt == &group_id_error_vt)
        return USER_LIT("group: unknown groupid ");
    return USER_LIT("group: unknown group ");
}

/* The box, the value and the message in one allocation. */
static Error user_str_error_build(Alloc *a, const ErrorVT *vt, Str s) {
    Str prefix = user_str_prefix(vt);
    size_t size =
        sizeof(UserStrErrorBox) + (size_t)s.len + (size_t)(prefix.len + s.len);
    UserStrErrorBox *b =
        (UserStrErrorBox *)mem_alloc_nozero(a, size, _Alignof(UserStrErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    p = user_put(p, s, &b->s);
    Byte *m = p;
    if (prefix.len > 0)
        memcpy(p, prefix.p, (size_t)prefix.len);
    if (s.len > 0)
        memcpy(p + prefix.len, s.p, (size_t)s.len);
    b->message = str_from_bytes(m, prefix.len + s.len);
    return (Error){vt, b};
}

static Str user_str_text(Alloc *a, const ErrorVT *vt, Str s) {
    Str prefix = user_str_prefix(vt);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(prefix.len + s.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, prefix.p, (size_t)prefix.len);
    if (s.len > 0)
        memcpy(p + prefix.len, s.p, (size_t)s.len);
    return str_from_bytes(p, prefix.len + s.len);
}

static Error user_id_error_clone(const void *self, Alloc *a) {
    return user_id_error_build(a, ((const UserIdErrorBox *)self)->id);
}

static Error user_name_error_clone(const void *self, Alloc *a) {
    return user_str_error_build(a, &user_name_error_vt,
                                ((const UserStrErrorBox *)self)->s);
}

static Error group_id_error_clone(const void *self, Alloc *a) {
    return user_str_error_build(a, &group_id_error_vt,
                                ((const UserStrErrorBox *)self)->s);
}

static Error group_name_error_clone(const void *self, Alloc *a) {
    return user_str_error_build(a, &group_name_error_vt,
                                ((const UserStrErrorBox *)self)->s);
}

Str user_unknown_user_id_error_error(UserUnknownUserIdError e, Alloc *a) {
    return user_id_text(a, e);
}

Error user_unknown_user_id_error_as_error(UserUnknownUserIdError e, Alloc *a) {
    return user_id_error_build(a, e);
}

Str user_unknown_user_error_error(UserUnknownUserError e, Alloc *a) {
    return user_str_text(a, &user_name_error_vt, e);
}

Error user_unknown_user_error_as_error(UserUnknownUserError e, Alloc *a) {
    return user_str_error_build(a, &user_name_error_vt, e);
}

Str user_unknown_group_id_error_error(UserUnknownGroupIdError e, Alloc *a) {
    return user_str_text(a, &group_id_error_vt, e);
}

Error user_unknown_group_id_error_as_error(UserUnknownGroupIdError e, Alloc *a) {
    return user_str_error_build(a, &group_id_error_vt, e);
}

Str user_unknown_group_error_error(UserUnknownGroupError e, Alloc *a) {
    return user_str_text(a, &group_name_error_vt, e);
}

Error user_unknown_group_error_as_error(UserUnknownGroupError e, Alloc *a) {
    return user_str_error_build(a, &group_name_error_vt, e);
}

/* ------------------------------------------------------------- parsing */

/* What a row has to match: value in field idx, which is 0 or 2. */
typedef struct UserMatch {
    Alloc *a;
    Str value;
    Str substr; /* ":" + value + ":", or value + ":" when idx is 0 */
    int idx;
    bool group;
} UserMatch;

/* strings.SplitN(line, ":", n) into parts, returning how many. */
static int user_split(Str line, Str *parts, int n) {
    int k = 0;
    while (k < n - 1) {
        Int i = strings_index_byte(line, ':');
        if (i < 0)
            break;
        parts[k++] = (Str){line.p, i};
        line = (Str){line.p + i + 1, line.len - i - 1};
    }
    parts[k++] = line;
    return k;
}

static bool user_is_int(Str s) {
    Error e = BURROW_NO_ERROR;
    (void)strconv_atoi(s, &e);
    return BURROW_OK(e);
}

/* matchGroupIndexValue and matchUserIndexValue. NULL to go on to the next
 * row. */
static void *user_match_line(UserMatch *m, Str line, Error *err) {
    Str colon = USER_LIT(":");
    if (!strings_contains(line, m->substr) ||
        strings_count(line, colon) < (m->group ? 3 : 6))
        return NULL;
    Str parts[7];
    int n = user_split(line, parts, m->group ? 4 : 7);
    if (n < (m->group ? 4 : 6) || parts[0].len == 0 ||
        !str_eq(parts[m->idx], m->value) || parts[0].p[0] == '+' ||
        parts[0].p[0] == '-')
        return NULL;
    if (!user_is_int(parts[2]))
        return NULL;
    if (m->group) {
        UserGroup *g = user_group_make(m->a, parts[2], parts[0]);
        if (g == NULL)
            *err = burrow_err_out_of_memory;
        return g;
    }
    if (!user_is_int(parts[3]))
        return NULL;
    /* The GECOS field is not quite standard, but the full name is meant to
     * come first in a list split by commas. */
    Str name = strings_cut(parts[4], USER_LIT(","), NULL, NULL);
    User *u = user_make(m->a, parts[2], parts[3], parts[0], name, parts[5]);
    if (u == NULL)
        *err = burrow_err_out_of_memory;
    return u;
}

/* readColonFile. A match, an error, or NULL with no error at the end of r. */
static void *user_read_colon_file(IoReader r, UserMatch *m, Int read_cols, Error *err) {
    *err = BURROW_NO_ERROR;
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);
    BufioReader *rd = bufio_new_reader(sa, r);
    void *v = NULL;
    if (rd == NULL) {
        *err = burrow_err_out_of_memory;
        goto done;
    }
    for (;;) {
        bool is_prefix = false;
        Slice whole = slice_nil(TYPE_BYTE);
        Error e = BURROW_NO_ERROR;

        /* A line comes in pieces as long as the buffer, and the pieces are
         * joined until there are enough columns. */
        for (;;) {
            Slice line = bufio_reader_read_line(rd, &is_prefix, &e);
            if (BURROW_FAILED(e)) {
                if (!errors_is(e, io_eof))
                    *err = e;
                goto done;
            }
            if (!is_prefix && whole.len == 0) {
                whole = line;
                break;
            }
            whole = slice_append(sa, whole, line.p, line.len);
            if (whole.p == NULL && line.len > 0) {
                *err = burrow_err_out_of_memory;
                goto done;
            }
            if (!is_prefix || strings_count(str_from_bytes(whole.p, whole.len),
                                            USER_LIT(":")) >= read_cols)
                break;
        }

        /* There is no spec for these files, so this follows glibc's parser,
         * which allows comments and space at the start of a line. */
        Str t = strings_trim_space(str_from_bytes(whole.p, whole.len));
        if (t.len == 0 || t.p[0] == '#')
            continue;
        v = user_match_line(m, t, err);
        if (v != NULL || BURROW_FAILED(*err))
            goto done;

        /* Skip the rest of a long line. */
        while (is_prefix) {
            (void)bufio_reader_read_line(rd, &is_prefix, &e);
            if (BURROW_FAILED(e)) {
                if (!errors_is(e, io_eof))
                    *err = e;
                goto done;
            }
        }
    }
done:
    bufio_reader_free(rd);
    arena_free(&scratch);
    return v;
}

static Str user_substr(Alloc *a, Str value, int idx) {
    Int lead = idx > 0 ? 1 : 0;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(lead + value.len + 1), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    if (lead)
        p[0] = ':';
    if (value.len > 0)
        memcpy(p + lead, value.p, (size_t)value.len);
    p[lead + value.len] = ':';
    return str_from_bytes(p, lead + value.len + 1);
}

static void *user_find(Alloc *a, Str value, int idx, bool group, IoReader r,
                       Error *err) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    UserMatch m = {a, value, user_substr(arena_allocator(&scratch), value, idx), idx,
                   group};
    void *v = NULL;
    if (m.substr.len == 0)
        *err = burrow_err_out_of_memory;
    else
        v = user_read_colon_file(r, &m, group ? 3 : 6, err);
    arena_free(&scratch);
    return v;
}

UserGroup *burrow__user_find_group_id(Alloc *a, Str id, IoReader r, Error *err) {
    UserGroup *g = (UserGroup *)user_find(a, id, 2, true, r, err);
    if (g == NULL && BURROW_OK(*err))
        *err = user_unknown_group_id_error_as_error(id, error_allocator());
    return g;
}

UserGroup *burrow__user_find_group_name(Alloc *a, Str name, IoReader r, Error *err) {
    UserGroup *g = (UserGroup *)user_find(a, name, 0, true, r, err);
    if (g == NULL && BURROW_OK(*err))
        *err = user_unknown_group_error_as_error(name, error_allocator());
    return g;
}

User *burrow__user_find_user_id(Alloc *a, Str uid, IoReader r, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int i = strconv_atoi(uid, &e);
    if (BURROW_FAILED(e)) {
        *err = fmt_errorf_v("user: invalid userid %s", uid);
        return NULL;
    }
    User *u = (User *)user_find(a, uid, 2, false, r, err);
    if (u == NULL && BURROW_OK(*err))
        *err = user_unknown_user_id_error_as_error(i, error_allocator());
    return u;
}

User *burrow__user_find_username(Alloc *a, Str name, IoReader r, Error *err) {
    User *u = (User *)user_find(a, name, 0, false, r, err);
    if (u == NULL && BURROW_OK(*err))
        *err = user_unknown_user_error_as_error(name, error_allocator());
    return u;
}

static Slice user_append_str(Alloc *a, Slice s, Str v, Error *err) {
    Str c = str_clone(a, v);
    if (c.len != v.len) {
        *err = burrow_err_out_of_memory;
        return s;
    }
    Slice t = slice_append(a, s, &c, 1);
    if (t.p == NULL)
        *err = burrow_err_out_of_memory;
    return t;
}

Slice burrow__user_list_groups_from_reader(const User *u, Alloc *a, IoReader r,
                                           Error *err) {
    *err = BURROW_NO_ERROR;
    Slice groups = slice_nil(TYPE_STRING);
    if (u->username.len == 0) {
        *err = errors_new(error_allocator(),
                          USER_LIT("user: list groups: empty username"));
        return groups;
    }
    Error e = BURROW_NO_ERROR;
    Int primary = strconv_atoi(u->gid, &e);
    if (BURROW_FAILED(e)) {
        *err = fmt_errorf_v("user: list groups for %s: invalid gid %q", u->username,
                            u->gid);
        return groups;
    }

    /* ",john," and the three ways it can sit in a list. */
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);
    Int n = u->username.len + 2;
    Byte *commas = (Byte *)mem_alloc_nozero(sa, (size_t)n, 1);
    BufioReader *rd = commas != NULL ? bufio_new_reader(sa, r) : NULL;
    if (rd == NULL) {
        *err = burrow_err_out_of_memory;
        goto done;
    }
    commas[0] = ',';
    memcpy(commas + 1, u->username.p, (size_t)u->username.len);
    commas[n - 1] = ',';
    Str user_commas = str_from_bytes(commas, n);
    Str user_first = str_from_bytes(commas + 1, n - 1);
    Str user_last = str_from_bytes(commas, n - 1);
    Str user_only = str_from_bytes(commas + 1, n - 2);

    /* The primary group first. */
    groups = user_append_str(a, groups, u->gid, err);
    if (BURROW_FAILED(*err))
        goto done;

    bool finished = false;
    while (!finished) {
        Error re = BURROW_NO_ERROR;
        Slice raw = bufio_reader_read_bytes(rd, sa, '\n', &re);
        if (BURROW_FAILED(re)) {
            if (errors_is(re, io_eof)) {
                finished = true;
            } else {
                *err = re;
                goto done;
            }
        }

        /* As in readColonFile, and a group whose name starts with "+" or "-"
         * is one glibc does not find. */
        Str line = strings_trim_space(str_from_bytes(raw.p, raw.len));
        if (line.len == 0 || line.p[0] == '#' || line.p[0] == '+' || line.p[0] == '-')
            continue;

        /* groupname:password:GID:user_list, such as wheel:x:10:john,paul */
        Int list_idx = strings_last_index_byte(line, ':');
        if (list_idx == -1 || list_idx == line.len - 1)
            continue;
        Str head = {line.p, list_idx};
        if (strings_count(head, USER_LIT(":")) != 2)
            continue;
        Str list = {line.p + list_idx + 1, line.len - list_idx - 1};
        if (!(str_eq(list, user_only) || strings_has_prefix(list, user_first) ||
              strings_has_suffix(list, user_last) ||
              strings_contains(list, user_commas)))
            continue;

        Str parts[3];
        if (user_split(head, parts, 3) != 3 || parts[0].len == 0)
            continue;
        Error ge = BURROW_NO_ERROR;
        Int gid = strconv_atoi(parts[2], &ge);
        if (BURROW_FAILED(ge) || gid == primary)
            continue;
        groups = user_append_str(a, groups, parts[2], err);
        if (BURROW_FAILED(*err))
            goto done;
    }
done:
    bufio_reader_free(rd);
    arena_free(&scratch);
    return groups;
}

/* ------------------------------------------------------- the two files */

#if defined(USER_IMPL_FILES)

static const Str user_passwd_file = {(const Byte *)"/etc/passwd", 11};
static const Str user_group_file = {(const Byte *)"/etc/group", 10};

static void *user_find_in(Alloc *a, Str file, Str value, Error *err,
                          void *(*find)(Alloc *, Str, IoReader, Error *)) {
    *err = BURROW_NO_ERROR;
    OsFile *f = os_open(a, file, err);
    if (BURROW_FAILED(*err))
        return NULL;
    void *v = find(a, value, os_file_as_io_reader(f), err);
    os_file_free(f);
    return v;
}

static void *user_find_group_name_in(Alloc *a, Str v, IoReader r, Error *err) {
    return burrow__user_find_group_name(a, v, r, err);
}
static void *user_find_group_id_in(Alloc *a, Str v, IoReader r, Error *err) {
    return burrow__user_find_group_id(a, v, r, err);
}
static void *user_find_username_in(Alloc *a, Str v, IoReader r, Error *err) {
    return burrow__user_find_username(a, v, r, err);
}
static void *user_find_user_id_in(Alloc *a, Str v, IoReader r, Error *err) {
    return burrow__user_find_user_id(a, v, r, err);
}

static UserGroup *user_impl_lookup_group(Alloc *a, Str name, Error *err) {
    return (UserGroup *)user_find_in(a, user_group_file, name, err,
                                     user_find_group_name_in);
}

static UserGroup *user_impl_lookup_group_id(Alloc *a, Str gid, int64_t buf,
                                            Error *err) {
    (void)buf;
    return (UserGroup *)user_find_in(a, user_group_file, gid, err,
                                     user_find_group_id_in);
}

static User *user_impl_lookup(Alloc *a, Str username, Error *err) {
    return (User *)user_find_in(a, user_passwd_file, username, err,
                                user_find_username_in);
}

static User *user_impl_lookup_id(Alloc *a, Str uid, Error *err) {
    return (User *)user_find_in(a, user_passwd_file, uid, err, user_find_user_id_in);
}

static Slice user_impl_list_groups(const User *u, Alloc *a, Error *err) {
    *err = BURROW_NO_ERROR;
    OsFile *f = os_open(a, user_group_file, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_STRING);
    Slice s = burrow__user_list_groups_from_reader(u, a, os_file_as_io_reader(f), err);
    os_file_free(f);
    return s;
}

/* lookup_stubs.go's current: /etc/passwd when it has the user, and $USER and
 * $HOME when it does not. */
static User *user_impl_current(Alloc *a, int64_t buf, Error *err) {
    (void)buf;
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);
    Int id = os_getuid();
    Str uid = id >= 0 ? strconv_itoa(sa, id) : BURROW_STR_EMPTY;
    /* $USER and /etc/passwd may disagree, and the file wins. Go issue
     * 27524. */
    User *u = user_impl_lookup_id(a, uid, err);
    if (BURROW_OK(*err)) {
        arena_free(&scratch);
        return u;
    }
    Int gid_n = os_getgid();
    Str gid = gid_n >= 0 ? strconv_itoa(sa, gid_n) : BURROW_STR_EMPTY;
    Error he = BURROW_NO_ERROR;
    Str home = os_user_home_dir(sa, &he);
    Str username = os_getenv(sa, USER_LIT("USER"));
    *err = BURROW_NO_ERROR;
    /* Without cgo, but with enough to go on, that will do. */
    if (uid.len > 0 && username.len > 0 && home.len > 0) {
        u = user_make(a, uid, gid, username, BURROW_STR_EMPTY, home);
        if (u == NULL)
            *err = burrow_err_out_of_memory;
        arena_free(&scratch);
        return u;
    }
    Str missing = username.len == 0 && home.len == 0 ? USER_LIT("$USER, $HOME")
                  : username.len == 0                ? USER_LIT("$USER")
                                                     : USER_LIT("$HOME");
    *err = fmt_errorf_v("user: Current requires cgo or %s set in environment", missing);
    arena_free(&scratch);
    return NULL;
}

#endif /* USER_IMPL_FILES */

/* ----------------------------------------------------------------- libc */

#if defined(USER_IMPL_LIBC)

enum { USER_MAX_BUFFER = 1 << 20, USER_MAX_GROUPS = 2048 };

/* bufferKind.initialSize. */
static int64_t user_initial_size(bool group, int64_t buf) {
    if (buf > 0)
        return buf;
    int64_t sz = pal_user_buf_size(group);
    if (sz == -1)
        return 1024; /* FreeBSD, DragonFly and musl have no suggestion */
    if (sz <= 0 || sz > USER_MAX_BUFFER)
        return USER_MAX_BUFFER;
    return sz;
}

/* What one call looks up, so retryWithBuffer can make it again. */
typedef struct UserCall {
    int kind; /* 0 getpwnam, 1 getpwuid, 2 getgrnam, 3 getgrgid */
    const char *name;
    uint32_t id;
    PalPasswd pw;
    PalGroup gr;
    bool found;
} UserCall;

static PalErrno user_call(UserCall *c, char *buf, int64_t cap) {
    switch (c->kind) {
    case 0:
        return pal_getpwnam(c->name, &c->pw, buf, cap, &c->found);
    case 1:
        return pal_getpwuid(c->id, &c->pw, buf, cap, &c->found);
    case 2:
        return pal_getgrnam(c->name, &c->gr, buf, cap, &c->found);
    default:
        return pal_getgrgid(c->id, &c->gr, buf, cap, &c->found);
    }
}

/* retryWithBuffer: the call again with twice the buffer each time it says
 * ERANGE, up to a megabyte. *notfound is set when the answer is that there is
 * no such row, which is ENOENT or nothing found. The buffer is from sa and
 * the strings in c point into it. */
static Error user_retry(Alloc *sa, UserCall *c, int64_t size, bool *notfound) {
    *notfound = false;
    for (;;) {
        char *buf = (char *)mem_alloc_nozero(sa, (size_t)size, 1);
        if (buf == NULL)
            return burrow_err_out_of_memory;
        PalErrno pe = user_call(c, buf, size);
        if (pe == PAL_OK) {
            *notfound = !c->found;
            return BURROW_NO_ERROR;
        }
        if (pe == PAL_ENOENT) {
            *notfound = true;
            return BURROW_NO_ERROR;
        }
        if (pe != PAL_ERANGE)
            return syscall_errno_as_error(syscall_errno_from_pal(pe),
                                          error_allocator());
        int64_t next = size * 2;
        if (next <= 0 || next > USER_MAX_BUFFER)
            return fmt_errorf_v("internal buffer exceeds %d bytes",
                                (Int)USER_MAX_BUFFER);
        size = next;
    }
}

static const char *user_cstr(Alloc *sa, Str s) {
    char *p = (char *)mem_alloc_nozero(sa, (size_t)s.len + 1, 1);
    if (p == NULL)
        return NULL;
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    p[s.len] = 0;
    return p;
}

static UserGroup *user_build_group(Alloc *a, Alloc *sa, const PalGroup *gr) {
    /* strconv.Itoa(int(gid)), and int is 64 bits. */
    Str gid = strconv_format_int(sa, (int64_t)gr->gid, 10);
    return user_group_make(a, gid, user_cstr_str(gr->name));
}

static User *user_impl_lookup_uid(Alloc *a, Int uid, int64_t buf, Error *err) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    UserCall c = {1, NULL, (uint32_t)uid, {0}, {0}, false};
    bool notfound;
    User *u = NULL;
    Error e = user_retry(arena_allocator(&scratch), &c, user_initial_size(false, buf),
                         &notfound);
    if (notfound)
        *err = user_unknown_user_id_error_as_error(uid, error_allocator());
    else if (BURROW_FAILED(e))
        *err = fmt_errorf_v("user: lookup userid %d: %v", uid, e);
    else if ((u = burrow__user_build(a, &c.pw)) == NULL)
        *err = burrow_err_out_of_memory;
    arena_free(&scratch);
    return u;
}

static User *user_impl_current(Alloc *a, int64_t buf, Error *err) {
    return user_impl_lookup_uid(a, os_getuid(), buf, err);
}

static User *user_impl_lookup(Alloc *a, Str username, Error *err) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);
    UserCall c = {0, user_cstr(sa, username), 0, {0}, {0}, false};
    bool notfound = false;
    User *u = NULL;
    Error e = c.name == NULL
                  ? burrow_err_out_of_memory
                  : user_retry(sa, &c, user_initial_size(false, 0), &notfound);
    if (notfound)
        *err = user_unknown_user_error_as_error(username, error_allocator());
    else if (BURROW_FAILED(e))
        *err = fmt_errorf_v("user: lookup username %s: %v", username, e);
    else if ((u = burrow__user_build(a, &c.pw)) == NULL)
        *err = burrow_err_out_of_memory;
    arena_free(&scratch);
    return u;
}

static User *user_impl_lookup_id(Alloc *a, Str uid, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int i = strconv_atoi(uid, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    return user_impl_lookup_uid(a, i, 0, err);
}

static UserGroup *user_impl_lookup_group(Alloc *a, Str name, Error *err) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);
    UserCall c = {2, user_cstr(sa, name), 0, {0}, {0}, false};
    bool notfound = false;
    UserGroup *g = NULL;
    Error e = c.name == NULL
                  ? burrow_err_out_of_memory
                  : user_retry(sa, &c, user_initial_size(true, 0), &notfound);
    if (notfound)
        *err = user_unknown_group_error_as_error(name, error_allocator());
    else if (BURROW_FAILED(e))
        *err = fmt_errorf_v("user: lookup groupname %s: %v", name, e);
    else if ((g = user_build_group(a, sa, &c.gr)) == NULL)
        *err = burrow_err_out_of_memory;
    arena_free(&scratch);
    return g;
}

static UserGroup *user_impl_lookup_group_id(Alloc *a, Str gid, int64_t buf,
                                            Error *err) {
    Error e = BURROW_NO_ERROR;
    Int i = strconv_atoi(gid, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);
    UserCall c = {3, NULL, (uint32_t)i, {0}, {0}, false};
    bool notfound;
    UserGroup *g = NULL;
    e = user_retry(sa, &c, user_initial_size(true, buf), &notfound);
    if (notfound)
        *err = user_unknown_group_id_error_as_error(strconv_itoa(sa, i),
                                                    error_allocator());
    else if (BURROW_FAILED(e))
        *err = fmt_errorf_v("user: lookup groupid %d: %v", i, e);
    else if ((g = user_build_group(a, sa, &c.gr)) == NULL)
        *err = burrow_err_out_of_memory;
    arena_free(&scratch);
    return g;
}

/* cgo_listgroups_unix.go's listGroups. */
static Slice user_impl_list_groups(const User *u, Alloc *a, Error *err) {
    Slice out = slice_nil(TYPE_STRING);
    Error e = BURROW_NO_ERROR;
    Int ug = strconv_atoi(u->gid, &e);
    if (BURROW_FAILED(e)) {
        *err = fmt_errorf_v("user: list groups for %s: invalid gid %q", u->username,
                            u->gid);
        return out;
    }
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);
    const char *name = user_cstr(sa, u->username);
    int n = 256;
    uint32_t *gids =
        (uint32_t *)mem_alloc(sa, sizeof(uint32_t) * 256, _Alignof(uint32_t));
    if (name == NULL || gids == NULL) {
        *err = burrow_err_out_of_memory;
        goto done;
    }
    if (pal_getgrouplist(name, (uint32_t)ug, gids, &n) == -1) {
        /* groupRetry. n is the size it needs now, except on macOS, which
         * does not set it, as Go's comment says. */
        if (n > USER_MAX_GROUPS) {
            *err = fmt_errorf_v("user: %q is a member of more than %d groups",
                                u->username, (Int)USER_MAX_GROUPS);
            goto done;
        }
        gids = (uint32_t *)mem_alloc(sa, sizeof(uint32_t) * (size_t)(n > 0 ? n : 1),
                                     _Alignof(uint32_t));
        if (gids == NULL) {
            *err = burrow_err_out_of_memory;
            goto done;
        }
        if (pal_getgrouplist(name, (uint32_t)ug, gids, &n) == -1) {
            *err = fmt_errorf_v("user: list groups for %s failed", u->username);
            goto done;
        }
    }
    for (int i = 0; i < n; i++) {
        /* strconv.Itoa(int(g)), with g a gid_t. */
        out =
            user_append_str(a, out, strconv_format_int(sa, (int64_t)gids[i], 10), err);
        if (BURROW_FAILED(*err))
            goto done;
    }
done:
    arena_free(&scratch);
    return out;
}

#endif /* USER_IMPL_LIBC */

/* -------------------------------------------------------------- Windows */

#if defined(USER_IMPL_WINDOWS)

static Error user_windows_error(void) {
    return errors_new(error_allocator(),
                      USER_LIT("user: lookups on windows are not implemented yet"));
}

static User *user_impl_current(Alloc *a, int64_t buf, Error *err) {
    (void)a, (void)buf;
    *err = user_windows_error();
    return NULL;
}

static User *user_impl_lookup(Alloc *a, Str username, Error *err) {
    (void)a, (void)username;
    *err = user_windows_error();
    return NULL;
}

static User *user_impl_lookup_id(Alloc *a, Str uid, Error *err) {
    (void)a, (void)uid;
    *err = user_windows_error();
    return NULL;
}

static UserGroup *user_impl_lookup_group(Alloc *a, Str name, Error *err) {
    (void)a, (void)name;
    *err = user_windows_error();
    return NULL;
}

static UserGroup *user_impl_lookup_group_id(Alloc *a, Str gid, int64_t buf,
                                            Error *err) {
    (void)a, (void)gid, (void)buf;
    *err = user_windows_error();
    return NULL;
}

static Slice user_impl_list_groups(const User *u, Alloc *a, Error *err) {
    (void)u, (void)a;
    *err = user_windows_error();
    return slice_nil(TYPE_STRING);
}

#endif /* USER_IMPL_WINDOWS */

/* buildUser. The GECOS field is not quite standard, but the full name is
 * meant to come first in a list split by commas. */
User *burrow__user_build(Alloc *a, const PalPasswd *pw) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);
    Str name = strings_cut(user_cstr_str(pw->gecos), USER_LIT(","), NULL, NULL);
    User *u = user_make(a, strconv_format_uint(sa, pw->uid, 10),
                        strconv_format_uint(sa, pw->gid, 10), user_cstr_str(pw->name),
                        name, user_cstr_str(pw->dir));
    arena_free(&scratch);
    return u;
}

/* ---------------------------------------------------------- the package */

User *burrow__user_current(Alloc *a, int64_t buf, Error *err) {
    *err = BURROW_NO_ERROR;
    return user_impl_current(a, buf, err);
}

UserGroup *burrow__user_lookup_group_id(Alloc *a, Str gid, int64_t buf, Error *err) {
    *err = BURROW_NO_ERROR;
    return user_impl_lookup_group_id(a, gid, buf, err);
}

/* Current's cache, filled once and only read after that. */
static SyncOnce user_cache_once;
static User *user_cache_u;
static Error user_cache_err;

static void user_cache_fill(void *env) {
    (void)env;
    Error e = BURROW_NO_ERROR;
    user_cache_u = user_impl_current(heap_allocator(), 0, &e);
    if (BURROW_FAILED(e))
        user_cache_err = error_retain(heap_allocator(), e);
}

User *user_current(Alloc *a, Error *err) {
    sync_once_do(&user_cache_once, BURROW_FN(Func, user_cache_fill, NULL));
    *err = BURROW_NO_ERROR;
    if (BURROW_FAILED(user_cache_err)) {
        *err = user_cache_err;
        return NULL;
    }
    User *u = user_copy(a, user_cache_u);
    if (u == NULL)
        *err = burrow_err_out_of_memory;
    return u;
}

/* Lookup and LookupId answer from Current when it is the same user. */
static bool user_current_is(bool by_id, Str v) {
    sync_once_do(&user_cache_once, BURROW_FN(Func, user_cache_fill, NULL));
    if (BURROW_FAILED(user_cache_err) || user_cache_u == NULL)
        return false;
    return str_eq(by_id ? user_cache_u->uid : user_cache_u->username, v);
}

User *user_lookup(Alloc *a, Str username, Error *err) {
    *err = BURROW_NO_ERROR;
    if (user_current_is(false, username))
        return user_current(a, err);
    return user_impl_lookup(a, username, err);
}

User *user_lookup_id(Alloc *a, Str uid, Error *err) {
    *err = BURROW_NO_ERROR;
    if (user_current_is(true, uid))
        return user_current(a, err);
    return user_impl_lookup_id(a, uid, err);
}

UserGroup *user_lookup_group(Alloc *a, Str name, Error *err) {
    *err = BURROW_NO_ERROR;
    return user_impl_lookup_group(a, name, err);
}

UserGroup *user_lookup_group_id(Alloc *a, Str gid, Error *err) {
    *err = BURROW_NO_ERROR;
    return user_impl_lookup_group_id(a, gid, 0, err);
}

Slice user_group_ids(const User *u, Alloc *a, Error *err) {
    *err = BURROW_NO_ERROR;
    return user_impl_list_groups(u, a, err);
}
