/* The user and group databases on Windows. There is no passwd or group file
 * and no getpwnam_r, so those calls say so, and os/user asks the security
 * APIs through the pal_win_ calls below instead.
 *
 * They are Go's lookup_windows.go and the syscall wrappers it uses, cut where
 * the system is called. The functions live in advapi32, netapi32, secur32 and
 * userenv, and burrow links nothing but kernel32, so each call loads what it
 * needs and lets it go again, as src/pal/registry_windows.c does.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <windows.h>

#include <lm.h>
#include <sddl.h>
#define SECURITY_WIN32 1
#include <security.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

PalErrno pal_getpwnam(const char *name, PalPasswd *pw, char *buf, int64_t cap,
                      bool *found) {
    (void)name, (void)pw, (void)buf, (void)cap;
    *found = false;
    return PAL_ENOTSUP;
}

PalErrno pal_getpwuid(uint32_t uid, PalPasswd *pw, char *buf, int64_t cap,
                      bool *found) {
    (void)uid, (void)pw, (void)buf, (void)cap;
    *found = false;
    return PAL_ENOTSUP;
}

PalErrno pal_getgrnam(const char *name, PalGroup *gr, char *buf, int64_t cap,
                      bool *found) {
    (void)name, (void)gr, (void)buf, (void)cap;
    *found = false;
    return PAL_ENOTSUP;
}

PalErrno pal_getgrgid(uint32_t gid, PalGroup *gr, char *buf, int64_t cap, bool *found) {
    (void)gid, (void)gr, (void)buf, (void)cap;
    *found = false;
    return PAL_ENOTSUP;
}

int64_t pal_user_buf_size(bool group) {
    (void)group;
    return -1;
}

int pal_getgrouplist(const char *name, uint32_t gid, uint32_t *gids, int *n) {
    (void)name, (void)gid, (void)gids;
    *n = 0;
    return -1;
}

/* ------------------------------------------------------- the libraries */

typedef BOOL(WINAPI *UwOpenProcessTokenFn)(HANDLE, DWORD, PHANDLE);
typedef BOOL(WINAPI *UwOpenThreadTokenFn)(HANDLE, DWORD, BOOL, PHANDLE);
typedef BOOL(WINAPI *UwRevertToSelfFn)(void);
typedef BOOL(WINAPI *UwImpersonateFn)(HANDLE);
typedef BOOL(WINAPI *UwGetTokenInformationFn)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID,
                                              DWORD, PDWORD);
typedef BOOL(WINAPI *UwSidToStringFn)(PSID, LPWSTR *);
typedef BOOL(WINAPI *UwStringToSidFn)(LPCWSTR, PSID *);
typedef BOOL(WINAPI *UwLookupAccountSidFn)(LPCWSTR, PSID, LPWSTR, LPDWORD, LPWSTR,
                                           LPDWORD, PSID_NAME_USE);
typedef BOOL(WINAPI *UwLookupAccountNameFn)(LPCWSTR, LPCWSTR, PSID, LPDWORD, LPWSTR,
                                            LPDWORD, PSID_NAME_USE);
typedef BOOL(WINAPI *UwIsValidSidFn)(PSID);
typedef PUCHAR(WINAPI *UwSubAuthorityCountFn)(PSID);
typedef PSID_IDENTIFIER_AUTHORITY(WINAPI *UwIdentifierAuthorityFn)(PSID);
typedef PDWORD(WINAPI *UwSubAuthorityFn)(PSID, DWORD);
typedef LSTATUS(WINAPI *UwRegOpenFn)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS(WINAPI *UwRegQueryFn)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LSTATUS(WINAPI *UwRegCloseFn)(HKEY);
typedef NET_API_STATUS(WINAPI *UwNetGetJoinFn)(LPCWSTR, LPWSTR *,
                                               PNETSETUP_JOIN_STATUS);
typedef NET_API_STATUS(WINAPI *UwNetUserGetInfoFn)(LPCWSTR, LPCWSTR, DWORD, LPBYTE *);
typedef NET_API_STATUS(WINAPI *UwNetLocalGroupsFn)(LPCWSTR, LPCWSTR, DWORD, DWORD,
                                                   LPBYTE *, DWORD, LPDWORD, LPDWORD);
typedef NET_API_STATUS(WINAPI *UwNetFreeFn)(LPVOID);
typedef BOOLEAN(WINAPI *UwGetUserNameExFn)(EXTENDED_NAME_FORMAT, LPWSTR, PULONG);
typedef BOOLEAN(WINAPI *UwTranslateNameFn)(LPCWSTR, EXTENDED_NAME_FORMAT,
                                           EXTENDED_NAME_FORMAT, LPWSTR, PULONG);
typedef BOOL(WINAPI *UwProfileDirFn)(HANDLE, LPWSTR, LPDWORD);
typedef BOOL(WINAPI *UwProfilesDirFn)(LPWSTR, LPDWORD);

/* Every function this file calls outside kernel32, and the libraries they
 * came from. */
typedef struct UwApi {
    HMODULE advapi, netapi, secur, userenv;
    UwOpenProcessTokenFn open_process_token;
    UwOpenThreadTokenFn open_thread_token;
    UwRevertToSelfFn revert_to_self;
    UwImpersonateFn impersonate;
    UwGetTokenInformationFn token_info;
    UwSidToStringFn sid_to_string;
    UwStringToSidFn string_to_sid;
    UwLookupAccountSidFn lookup_sid;
    UwLookupAccountNameFn lookup_name;
    UwIsValidSidFn is_valid_sid;
    UwSubAuthorityCountFn sub_authority_count;
    UwIdentifierAuthorityFn identifier_authority;
    UwSubAuthorityFn sub_authority;
    UwRegOpenFn reg_open;
    UwRegQueryFn reg_query;
    UwRegCloseFn reg_close;
    UwNetGetJoinFn net_get_join;
    UwNetUserGetInfoFn net_user_get_info;
    UwNetLocalGroupsFn net_local_groups;
    UwNetFreeFn net_free;
    UwGetUserNameExFn get_user_name_ex;
    UwTranslateNameFn translate_name;
    UwProfileDirFn profile_dir;
    UwProfilesDirFn profiles_dir;
} UwApi;

#define UW_GET(T, lib, name) ((T)(void (*)(void))GetProcAddress(lib, name))

/* Frees what a uw_open that failed part way had loaded. */
static void uw_close(UwApi *api) {
    if (api->advapi != NULL)
        FreeLibrary(api->advapi);
    if (api->netapi != NULL)
        FreeLibrary(api->netapi);
    if (api->secur != NULL)
        FreeLibrary(api->secur);
    if (api->userenv != NULL)
        FreeLibrary(api->userenv);
}

enum { UW_ADVAPI = 1, UW_NETAPI = 2, UW_SECUR = 4, UW_USERENV = 8 };

/* Loads the libraries in libs and finds what this file uses from them. Go's
 * Current never loads netapi32, and a test checks that, so each call asks for
 * only the libraries it needs. They stay loaded after, as Go's lazy DLLs do.
 * Loading one means opening its file, and a thread impersonating at the
 * identification level can't, so a library freed after one call could not be
 * loaded again by the next. */
static PalErrno uw_open(UwApi *api, int libs) {
    memset(api, 0, sizeof *api);
    if ((libs & UW_ADVAPI) != 0 &&
        (api->advapi = LoadLibraryW(L"advapi32.dll")) == NULL)
        goto fail;
    if ((libs & UW_NETAPI) != 0 &&
        (api->netapi = LoadLibraryW(L"netapi32.dll")) == NULL)
        goto fail;
    if ((libs & UW_SECUR) != 0 && (api->secur = LoadLibraryW(L"secur32.dll")) == NULL)
        goto fail;
    if ((libs & UW_USERENV) != 0 &&
        (api->userenv = LoadLibraryW(L"userenv.dll")) == NULL)
        goto fail;
    bool ok = true;
    if (api->advapi != NULL) {
        HMODULE a = api->advapi;
        api->open_process_token = UW_GET(UwOpenProcessTokenFn, a, "OpenProcessToken");
        api->open_thread_token = UW_GET(UwOpenThreadTokenFn, a, "OpenThreadToken");
        api->revert_to_self = UW_GET(UwRevertToSelfFn, a, "RevertToSelf");
        api->impersonate = UW_GET(UwImpersonateFn, a, "ImpersonateLoggedOnUser");
        api->token_info = UW_GET(UwGetTokenInformationFn, a, "GetTokenInformation");
        api->sid_to_string = UW_GET(UwSidToStringFn, a, "ConvertSidToStringSidW");
        api->string_to_sid = UW_GET(UwStringToSidFn, a, "ConvertStringSidToSidW");
        api->lookup_sid = UW_GET(UwLookupAccountSidFn, a, "LookupAccountSidW");
        api->lookup_name = UW_GET(UwLookupAccountNameFn, a, "LookupAccountNameW");
        api->is_valid_sid = UW_GET(UwIsValidSidFn, a, "IsValidSid");
        api->sub_authority_count =
            UW_GET(UwSubAuthorityCountFn, a, "GetSidSubAuthorityCount");
        api->identifier_authority =
            UW_GET(UwIdentifierAuthorityFn, a, "GetSidIdentifierAuthority");
        api->sub_authority = UW_GET(UwSubAuthorityFn, a, "GetSidSubAuthority");
        api->reg_open = UW_GET(UwRegOpenFn, a, "RegOpenKeyExW");
        api->reg_query = UW_GET(UwRegQueryFn, a, "RegQueryValueExW");
        api->reg_close = UW_GET(UwRegCloseFn, a, "RegCloseKey");
        ok = ok && api->open_process_token != NULL && api->open_thread_token != NULL &&
             api->revert_to_self != NULL && api->impersonate != NULL &&
             api->token_info != NULL && api->sid_to_string != NULL &&
             api->string_to_sid != NULL && api->lookup_sid != NULL &&
             api->lookup_name != NULL && api->is_valid_sid != NULL &&
             api->sub_authority_count != NULL && api->identifier_authority != NULL &&
             api->sub_authority != NULL && api->reg_open != NULL &&
             api->reg_query != NULL && api->reg_close != NULL;
    }
    if (api->netapi != NULL) {
        HMODULE n = api->netapi;
        api->net_get_join = UW_GET(UwNetGetJoinFn, n, "NetGetJoinInformation");
        api->net_user_get_info = UW_GET(UwNetUserGetInfoFn, n, "NetUserGetInfo");
        api->net_local_groups = UW_GET(UwNetLocalGroupsFn, n, "NetUserGetLocalGroups");
        api->net_free = UW_GET(UwNetFreeFn, n, "NetApiBufferFree");
        ok = ok && api->net_get_join != NULL && api->net_user_get_info != NULL &&
             api->net_local_groups != NULL && api->net_free != NULL;
    }
    if (api->secur != NULL) {
        api->get_user_name_ex = UW_GET(UwGetUserNameExFn, api->secur, "GetUserNameExW");
        api->translate_name = UW_GET(UwTranslateNameFn, api->secur, "TranslateNameW");
        ok = ok && api->get_user_name_ex != NULL && api->translate_name != NULL;
    }
    if (api->userenv != NULL) {
        api->profile_dir =
            UW_GET(UwProfileDirFn, api->userenv, "GetUserProfileDirectoryW");
        api->profiles_dir =
            UW_GET(UwProfilesDirFn, api->userenv, "GetProfilesDirectoryW");
        ok = ok && api->profile_dir != NULL && api->profiles_dir != NULL;
    }
    if (ok)
        return PAL_OK;
fail:;
    PalErrno e = burrow__pal_errno_win(GetLastError());
    uw_close(api);
    return e;
}

/* ------------------------------------------------------------- strings */

static void *uw_alloc(size_t n) {
    return HeapAlloc(GetProcessHeap(), 0, n);
}

static void uw_free(void *p) {
    if (p != NULL)
        HeapFree(GetProcessHeap(), 0, p);
}

/* s as UTF-16 from the process heap. Every byte of UTF-8 is at most one unit
 * of UTF-16, so strlen + 1 units is always room enough. */
static wchar_t *uw_widen(const char *s, PalErrno *err) {
    size_t n = strlen(s) + 1;
    wchar_t *w = (wchar_t *)uw_alloc(n * sizeof(wchar_t));
    if (w == NULL) {
        *err = PAL_ENOMEM;
        return NULL;
    }
    if (!burrow__pal_widen(s, w, n, err)) {
        uw_free(w);
        return NULL;
    }
    return w;
}

/* Where the strings a call gives go, one after another, each with a NUL. */
typedef struct UwOut {
    char *p;
    int64_t left;
    bool full;
} UwOut;

/* Go's UTF16ToString: up to the first NUL, with an unpaired surrogate turned
 * into U+FFFD. burrow__pal_narrow keeps those, so they are replaced first. */
static void uw_put_n(UwOut *o, wchar_t *w, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)w[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && (uint32_t)w[i + 1] >= 0xdc00 &&
            (uint32_t)w[i + 1] < 0xe000) {
            i++;
            continue;
        }
        if (c >= 0xd800 && c < 0xe000)
            w[i] = (wchar_t)0xfffd;
    }
    if (o->full || o->left < 1) {
        o->full = true;
        return;
    }
    int64_t k = burrow__pal_narrow(w, n, o->p, (size_t)(o->left - 1));
    if (k < 0) {
        o->full = true;
        return;
    }
    o->p[k] = 0;
    o->p += k + 1;
    o->left -= k + 1;
}

static void uw_put(UwOut *o, wchar_t *w) {
    uw_put_n(o, w, w == NULL ? 0 : wcslen(w));
}

static PalErrno uw_done(const UwOut *o) {
    return o->full ? PAL_ERANGE : PAL_OK;
}

/* A SID in string form, "S-1-5-...", into o. */
static PalErrno uw_put_sid(const UwApi *api, UwOut *o, PSID sid) {
    LPWSTR s = NULL;
    if (!api->sid_to_string(sid, &s))
        return burrow__pal_errno_win(GetLastError());
    uw_put(o, s);
    LocalFree(s);
    return PAL_OK;
}

/* ----------------------------------------------------------- the token */

/* GetTokenInformation into a buffer from the process heap, asking for the
 * size first, as Go's getInfo does. */
static void *uw_token_info(const UwApi *api, HANDLE t, TOKEN_INFORMATION_CLASS c,
                           PalErrno *err) {
    DWORD n = 50;
    for (;;) {
        void *b = uw_alloc(n);
        if (b == NULL) {
            *err = PAL_ENOMEM;
            return NULL;
        }
        if (api->token_info(t, c, b, n, &n))
            return b;
        DWORD e = GetLastError();
        uw_free(b);
        if (e != ERROR_INSUFFICIENT_BUFFER) {
            *err = burrow__pal_errno_win(e);
            return NULL;
        }
    }
}

/* Go's runAsProcessOwner: f with any impersonation on this thread dropped,
 * and taken up again after. *stage is 1 or 2 when getting the token or
 * dropping the impersonation failed. */
static PalErrno uw_as_owner(const UwApi *api, PalErrno (*f)(const UwApi *, void *),
                            void *env, int *stage) {
    *stage = 0;
    HANDLE prev = NULL;
    bool is_process = false;
    if (!api->open_thread_token(GetCurrentThread(),
                                TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, TRUE,
                                &prev)) {
        DWORD e = GetLastError();
        if (e != ERROR_NO_TOKEN) {
            *stage = 1;
            return burrow__pal_errno_win(e);
        }
        /* Not impersonating, so the process token is the current one. */
        is_process = true;
        if (!api->open_process_token(GetCurrentProcess(), TOKEN_QUERY, &prev)) {
            *stage = 1;
            return burrow__pal_errno_win(GetLastError());
        }
    }
    if (!is_process && !api->revert_to_self()) {
        PalErrno e = burrow__pal_errno_win(GetLastError());
        CloseHandle(prev);
        *stage = 2;
        return e;
    }
    PalErrno r = f(api, env);
    if (!is_process && !api->impersonate(prev)) {
        /* Go ends the goroutine and its thread here rather than let it run on
         * as the process owner. A C thread cannot be ended safely from under
         * its caller, so this ends the process. */
        DWORD code = GetLastError();
        char msg[512];
        int n =
            snprintf(msg, sizeof msg, "os/user: failed to revert to previous token: ");
        DWORD m = FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
            msg + n, (DWORD)(sizeof msg - (size_t)n - 2), NULL);
        while (m > 0 && (msg[n + (int)m - 1] == '\n' || msg[n + (int)m - 1] == '\r' ||
                         msg[n + (int)m - 1] == '.'))
            m--;
        msg[n + (int)m] = '\n';
        DWORD wrote = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, (DWORD)n + m + 1, &wrote, NULL);
        pal_exit(2);
    }
    CloseHandle(prev);
    return r;
}

/* GetUserNameEx into o, with Go's GetUserName retry. */
static PalErrno uw_user_name(const UwApi *api, EXTENDED_NAME_FORMAT f, UwOut *o) {
    ULONG n = 50;
    for (;;) {
        wchar_t *b = (wchar_t *)uw_alloc((size_t)n * sizeof(wchar_t));
        if (b == NULL)
            return PAL_ENOMEM;
        ULONG got = n;
        if (api->get_user_name_ex(f, b, &got)) {
            uw_put_n(o, b, got);
            uw_free(b);
            return PAL_OK;
        }
        DWORD e = GetLastError();
        uw_free(b);
        if (e != ERROR_MORE_DATA || got <= n)
            return burrow__pal_errno_win(e);
        n = got;
    }
}

/* GetUserProfileDirectory into o, starting at 100 units as Go does. */
static PalErrno uw_profile_dir(const UwApi *api, HANDLE t, UwOut *o) {
    DWORD n = 100;
    for (;;) {
        wchar_t *b = (wchar_t *)uw_alloc((size_t)n * sizeof(wchar_t));
        if (b == NULL)
            return PAL_ENOMEM;
        DWORD got = n;
        if (api->profile_dir(t, b, &got)) {
            uw_put(o, b);
            uw_free(b);
            return PAL_OK;
        }
        DWORD e = GetLastError();
        uw_free(b);
        if (e != ERROR_INSUFFICIENT_BUFFER || got <= n)
            return burrow__pal_errno_win(e);
        n = got;
    }
}

typedef struct UwCurrent {
    UwOut out;
    int count;
} UwCurrent;

static PalErrno uw_current_user(const UwApi *api, void *env) {
    UwOut *o = &((UwCurrent *)env)->out;
    HANDLE t = NULL;
    if (!api->open_process_token(GetCurrentProcess(), TOKEN_QUERY, &t))
        return burrow__pal_errno_win(GetLastError());
    PalErrno e = PAL_OK;
    TOKEN_USER *u = (TOKEN_USER *)uw_token_info(api, t, TokenUser, &e);
    TOKEN_PRIMARY_GROUP *pg =
        u == NULL ? NULL
                  : (TOKEN_PRIMARY_GROUP *)uw_token_info(api, t, TokenPrimaryGroup, &e);
    if (pg != NULL)
        e = uw_put_sid(api, o, u->User.Sid);
    if (pg != NULL && e == PAL_OK)
        e = uw_put_sid(api, o, pg->PrimaryGroup);
    if (pg != NULL && e == PAL_OK)
        e = uw_profile_dir(api, t, o);
    char *sam = o->p;
    if (pg != NULL && e == PAL_OK)
        e = uw_user_name(api, NameSamCompatible, o);
    if (pg != NULL && e == PAL_OK && !o->full) {
        /* No display name is no error, as in Go, and the account name goes
         * in its place. */
        UwOut keep = *o;
        if (uw_user_name(api, NameDisplay, o) != PAL_OK) {
            *o = keep;
            size_t n = strlen(sam) + 1;
            if ((int64_t)n > o->left) {
                o->full = true;
            } else {
                memcpy(o->p, sam, n);
                o->p += n;
                o->left -= (int64_t)n;
            }
        }
    }
    uw_free(pg);
    uw_free(u);
    CloseHandle(t);
    return e != PAL_OK ? e : uw_done(o);
}

static PalErrno uw_current_groups(const UwApi *api, void *env) {
    UwCurrent *c = (UwCurrent *)env;
    HANDLE t = NULL;
    if (!api->open_process_token(GetCurrentProcess(), TOKEN_QUERY, &t))
        return burrow__pal_errno_win(GetLastError());
    PalErrno e = PAL_OK;
    TOKEN_GROUPS *g = (TOKEN_GROUPS *)uw_token_info(api, t, TokenGroups, &e);
    for (DWORD i = 0; g != NULL && e == PAL_OK && i < g->GroupCount; i++) {
        e = uw_put_sid(api, &c->out, g->Groups[i].Sid);
        c->count++;
    }
    uw_free(g);
    CloseHandle(t);
    return e != PAL_OK ? e : uw_done(&c->out);
}

PalErrno pal_win_current_user(char *buf, int64_t cap, int *stage) {
    *stage = 0;
    UwApi api;
    PalErrno e = uw_open(&api, UW_ADVAPI | UW_SECUR | UW_USERENV);
    if (e != PAL_OK)
        return e;
    UwCurrent c = {{buf, cap, false}, 0};
    e = uw_as_owner(&api, uw_current_user, &c, stage);
    return e;
}

PalErrno pal_win_current_groups(char *buf, int64_t cap, int *n, int *stage) {
    *n = 0;
    *stage = 0;
    UwApi api;
    PalErrno e = uw_open(&api, UW_ADVAPI);
    if (e != PAL_OK)
        return e;
    UwCurrent c = {{buf, cap, false}, 0};
    e = uw_as_owner(&api, uw_current_groups, &c, stage);
    *n = c.count;
    return e;
}

/* ------------------------------------------------------------ accounts */

/* Go's isServiceAccount: S-1-5-18, S-1-5-19 and S-1-5-20. */
static bool uw_is_service(const UwApi *api, PSID sid) {
    if (!api->is_valid_sid(sid))
        return false;
    static const BYTE nt[6] = {0, 0, 0, 0, 0, 5};
    if (*api->sub_authority_count(sid) != SID_REVISION ||
        memcmp(api->identifier_authority(sid)->Value, nt, sizeof nt) != 0)
        return false;
    DWORD rid = *api->sub_authority(sid, 0);
    return rid == SECURITY_LOCAL_SYSTEM_RID || rid == SECURITY_LOCAL_SERVICE_RID ||
           rid == SECURITY_NETWORK_SERVICE_RID;
}

PalErrno pal_win_lookup_name(const char *name, char *buf, int64_t cap, uint32_t *type,
                             bool *service) {
    *type = 0;
    *service = false;
    UwApi api;
    PalErrno e = uw_open(&api, UW_ADVAPI);
    if (e != PAL_OK)
        return e;
    wchar_t *w = uw_widen(name, &e);
    UwOut o = {buf, cap, false};
    /* Go's LookupSID, which starts at 100 bytes and 50 units. */
    DWORD n = 100, dn = 50;
    while (w != NULL) {
        PSID sid = (PSID)uw_alloc(n);
        wchar_t *dom = (wchar_t *)uw_alloc((size_t)dn * sizeof(wchar_t));
        if (sid == NULL || dom == NULL) {
            uw_free(sid);
            uw_free(dom);
            e = PAL_ENOMEM;
            break;
        }
        SID_NAME_USE use = 0;
        DWORD gn = n, gdn = dn;
        if (api.lookup_name(NULL, w, sid, &gn, dom, &gdn, &use)) {
            *type = (uint32_t)use;
            *service = uw_is_service(&api, sid);
            e = uw_put_sid(&api, &o, sid);
            if (e == PAL_OK)
                e = uw_done(&o);
            uw_free(sid);
            uw_free(dom);
            break;
        }
        DWORD le = GetLastError();
        uw_free(sid);
        uw_free(dom);
        if (le != ERROR_INSUFFICIENT_BUFFER || (gn <= n && gdn <= dn)) {
            e = burrow__pal_errno_win(le);
            break;
        }
        if (gn > n)
            n = gn;
        if (gdn > dn)
            dn = gdn;
    }
    uw_free(w);
    return e;
}

PalErrno pal_win_lookup_sid(const char *sid, char *buf, int64_t cap, uint32_t *type,
                            bool *service) {
    *type = 0;
    *service = false;
    UwApi api;
    PalErrno e = uw_open(&api, UW_ADVAPI);
    if (e != PAL_OK)
        return e;
    wchar_t *w = uw_widen(sid, &e);
    PSID psid = NULL;
    if (w != NULL && !api.string_to_sid(w, &psid))
        e = burrow__pal_errno_win(GetLastError());
    UwOut o = {buf, cap, false};
    /* Go's LookupAccount, which starts at 50 units for both. */
    DWORD n = 50, dn = 50;
    while (psid != NULL) {
        wchar_t *nb = (wchar_t *)uw_alloc((size_t)n * sizeof(wchar_t));
        wchar_t *db = (wchar_t *)uw_alloc((size_t)dn * sizeof(wchar_t));
        if (nb == NULL || db == NULL) {
            uw_free(nb);
            uw_free(db);
            e = PAL_ENOMEM;
            break;
        }
        SID_NAME_USE use = 0;
        DWORD gn = n, gdn = dn;
        if (api.lookup_sid(NULL, psid, nb, &gn, db, &gdn, &use)) {
            *type = (uint32_t)use;
            *service = uw_is_service(&api, psid);
            uw_put(&o, nb);
            uw_put(&o, db);
            e = uw_put_sid(&api, &o, psid);
            if (e == PAL_OK)
                e = uw_done(&o);
            uw_free(nb);
            uw_free(db);
            break;
        }
        DWORD le = GetLastError();
        uw_free(nb);
        uw_free(db);
        if (le != ERROR_INSUFFICIENT_BUFFER || (gn <= n && gdn <= dn)) {
            e = burrow__pal_errno_win(le);
            break;
        }
        if (gn > n)
            n = gn;
        if (gdn > dn)
            dn = gdn;
    }
    if (psid != NULL)
        LocalFree(psid);
    uw_free(w);
    return e;
}

PalErrno pal_win_domain_joined(bool *joined) {
    *joined = false;
    UwApi api;
    PalErrno e = uw_open(&api, UW_NETAPI);
    if (e != PAL_OK)
        return e;
    LPWSTR domain = NULL;
    NETSETUP_JOIN_STATUS status = NetSetupUnknownStatus;
    NET_API_STATUS st = api.net_get_join(NULL, &domain, &status);
    if (st != NERR_Success) {
        e = burrow__pal_errno_win(st);
    } else {
        api.net_free(domain);
        *joined = status == NetSetupDomainName;
    }
    return e;
}

PalErrno pal_win_display_name(const char *account, char *buf, int64_t cap) {
    UwApi api;
    PalErrno e = uw_open(&api, UW_SECUR);
    if (e != PAL_OK)
        return e;
    wchar_t *w = uw_widen(account, &e);
    UwOut o = {buf, cap, false};
    /* Go's TranslateAccountName with an initial size of 50. */
    ULONG n = 50;
    while (w != NULL) {
        wchar_t *b = (wchar_t *)uw_alloc((size_t)n * sizeof(wchar_t));
        if (b == NULL) {
            e = PAL_ENOMEM;
            break;
        }
        ULONG got = n;
        if (api.translate_name(w, NameSamCompatible, NameDisplay, b, &got)) {
            uw_put(&o, b);
            uw_free(b);
            e = uw_done(&o);
            break;
        }
        DWORD le = GetLastError();
        uw_free(b);
        if (le != ERROR_INSUFFICIENT_BUFFER || got <= n) {
            e = burrow__pal_errno_win(le);
            break;
        }
        n = got;
    }
    uw_free(w);
    return e;
}

/* NetUserGetInfo at level, with the buffer it gives in *p. */
static PalErrno uw_user_info(const UwApi *api, const char *server, const char *user,
                             DWORD level, LPBYTE *p) {
    PalErrno e = PAL_OK;
    wchar_t *s = uw_widen(server, &e);
    wchar_t *u = s == NULL ? NULL : uw_widen(user, &e);
    if (u != NULL) {
        NET_API_STATUS st = api->net_user_get_info(s, u, level, p);
        if (st != NERR_Success)
            e = burrow__pal_errno_win(st);
    }
    uw_free(u);
    uw_free(s);
    return e;
}

PalErrno pal_win_user_full_name(const char *server, const char *user, char *buf,
                                int64_t cap) {
    UwApi api;
    PalErrno e = uw_open(&api, UW_NETAPI);
    if (e != PAL_OK)
        return e;
    LPBYTE p = NULL;
    e = uw_user_info(&api, server, user, 10, &p);
    if (e == PAL_OK) {
        UwOut o = {buf, cap, false};
        uw_put(&o, ((USER_INFO_10 *)(void *)p)->usri10_full_name);
        e = uw_done(&o);
        api.net_free(p);
    }
    return e;
}

PalErrno pal_win_user_primary_group(const char *server, const char *user,
                                    uint32_t *rid) {
    *rid = 0;
    UwApi api;
    PalErrno e = uw_open(&api, UW_NETAPI);
    if (e != PAL_OK)
        return e;
    LPBYTE p = NULL;
    e = uw_user_info(&api, server, user, 4, &p);
    if (e == PAL_OK) {
        *rid = (uint32_t)((USER_INFO_4 *)(void *)p)->usri4_primary_group_id;
        api.net_free(p);
    }
    return e;
}

PalErrno pal_win_user_local_groups(const char *user, char *buf, int64_t cap, int *n) {
    *n = 0;
    UwApi api;
    PalErrno e = uw_open(&api, UW_NETAPI);
    if (e != PAL_OK)
        return e;
    wchar_t *u = uw_widen(user, &e);
    if (u != NULL) {
        LPBYTE p = NULL;
        DWORD read = 0, total = 0;
        NET_API_STATUS st = api.net_local_groups(NULL, u, 0, LG_INCLUDE_INDIRECT, &p,
                                                 MAX_PREFERRED_LENGTH, &read, &total);
        if (st != NERR_Success) {
            e = burrow__pal_errno_win(st);
        } else {
            UwOut o = {buf, cap, false};
            LOCALGROUP_USERS_INFO_0 *g = (LOCALGROUP_USERS_INFO_0 *)(void *)p;
            for (DWORD i = 0; i < read; i++) {
                if (g[i].lgrui0_name == NULL)
                    continue;
                uw_put(&o, g[i].lgrui0_name);
                (*n)++;
            }
            e = uw_done(&o);
            if (p != NULL)
                api.net_free(p);
        }
    }
    uw_free(u);
    return e;
}

PalErrno pal_win_profile_path(const char *sid, char *buf, int64_t cap) {
    UwApi api;
    PalErrno e = uw_open(&api, UW_ADVAPI);
    if (e != PAL_OK)
        return e;
    static const char prefix[] =
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\";
    size_t pn = sizeof prefix - 1, sn = strlen(sid);
    char *path = (char *)uw_alloc(pn + sn + 1);
    wchar_t *w = NULL;
    if (path == NULL) {
        e = PAL_ENOMEM;
    } else {
        memcpy(path, prefix, pn);
        memcpy(path + pn, sid, sn + 1);
        w = uw_widen(path, &e);
    }
    HKEY k = NULL;
    if (w != NULL) {
        LSTATUS st = api.reg_open(HKEY_LOCAL_MACHINE, w, 0, KEY_QUERY_VALUE, &k);
        if (st != ERROR_SUCCESS) {
            e = burrow__pal_errno_win((unsigned long)st);
            k = NULL;
        }
    }
    if (k != NULL) {
        /* Go's GetStringValue: a string type or ErrUnexpectedType, up to
         * the first NUL. */
        DWORD type = 0, size = 0;
        LSTATUS st = api.reg_query(k, L"ProfileImagePath", NULL, &type, NULL, &size);
        wchar_t *v = NULL;
        while (st == ERROR_SUCCESS || st == ERROR_MORE_DATA) {
            uw_free(v);
            v = (wchar_t *)uw_alloc((size_t)size + sizeof(wchar_t));
            if (v == NULL) {
                st = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }
            DWORD got = size;
            st = api.reg_query(k, L"ProfileImagePath", NULL, &type, (LPBYTE)v, &got);
            if (st == ERROR_SUCCESS) {
                v[got / sizeof(wchar_t)] = 0;
                break;
            }
            size = got;
        }
        if (st != ERROR_SUCCESS) {
            e = burrow__pal_errno_win((unsigned long)st);
        } else if (type != REG_SZ && type != REG_EXPAND_SZ) {
            e = burrow__pal_errno_win(ERROR_UNSUPPORTED_TYPE);
        } else {
            UwOut o = {buf, cap, false};
            uw_put(&o, v);
            e = uw_done(&o);
        }
        uw_free(v);
        api.reg_close(k);
    }
    uw_free(w);
    uw_free(path);
    return e;
}

PalErrno pal_win_profiles_dir(char *buf, int64_t cap) {
    UwApi api;
    PalErrno e = uw_open(&api, UW_USERENV);
    if (e != PAL_OK)
        return e;
    /* Go's getProfilesDirectory, from 100 units. */
    DWORD n = 100;
    for (;;) {
        wchar_t *b = (wchar_t *)uw_alloc((size_t)n * sizeof(wchar_t));
        if (b == NULL) {
            e = PAL_ENOMEM;
            break;
        }
        DWORD got = n;
        if (api.profiles_dir(b, &got)) {
            UwOut o = {buf, cap, false};
            uw_put(&o, b);
            uw_free(b);
            e = uw_done(&o);
            break;
        }
        DWORD le = GetLastError();
        uw_free(b);
        if (le != ERROR_INSUFFICIENT_BUFFER || got <= n) {
            e = burrow__pal_errno_win(le);
            break;
        }
        n = got;
    }
    return e;
}

#endif /* BURROW_OS_WINDOWS */
