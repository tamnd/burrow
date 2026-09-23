/* Processes on Windows, and mapping a file.
 *
 * A process id here is the process handle, not the number Task Manager shows,
 * because waiting on a process and ending one both take the handle and a
 * number would have to be turned back into one through a table or through
 * OpenProcess, which can open some other process once the number is reused.
 * pal_wait closes it, as waitpid reaps a child.
 *
 * Windows has no descriptor table to hand a child. Slots 0 to 2 become its
 * standard handles. Anything past them is inherited under its own handle
 * value, and the child has to be told those values some other way, which is
 * what Go's fuzzing does with an environment variable. Only the handles named
 * in fds are inherited, through PROC_THREAD_ATTRIBUTE_HANDLE_LIST, so a spawn
 * on one thread cannot leak a handle another thread is using.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(_WIN32) && !defined(_WIN32_WINNT)
/* Vista, for the handle list. */
#define _WIN32_WINNT 0x0601
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>
#include <string.h>

#include <windows.h>

static bool proc_fail(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
    return false;
}

static void *proc_alloc(size_t n) {
    return HeapAlloc(GetProcessHeap(), 0, n);
}

static void proc_free(void *p) {
    if (p != NULL)
        HeapFree(GetProcessHeap(), 0, p);
}

/* ------------------------------------------------------------ command line */

/* Go's appendEscapeArg, into out if it is not NULL. Answers the length. */
static size_t proc_escape_arg(const char *s, char *out) {
    size_t n = 0;
#define PUT(c)                                                                         \
    do {                                                                               \
        if (out != NULL)                                                               \
            out[n] = (c);                                                              \
        n++;                                                                           \
    } while (0)
    if (s[0] == 0) {
        PUT('"');
        PUT('"');
        return n;
    }
    bool backslash = false;
    bool space = false;
    for (const char *c = s; *c != 0; c++) {
        backslash = backslash || *c == '"' || *c == '\\';
        space = space || *c == ' ' || *c == '\t';
    }
    if (!backslash && !space) {
        for (const char *c = s; *c != 0; c++)
            PUT(*c);
        return n;
    }
    if (space)
        PUT('"');
    size_t slashes = 0;
    for (const char *c = s; *c != 0; c++) {
        if (*c == '\\') {
            slashes++;
        } else if (*c == '"') {
            for (; slashes > 0; slashes--)
                PUT('\\');
            PUT('\\');
        } else {
            slashes = 0;
        }
        PUT(*c);
    }
    if (space) {
        for (; slashes > 0; slashes--)
            PUT('\\');
        PUT('"');
    }
#undef PUT
    return n;
}

/* Go's makeCmdLine, widened, on the process heap. */
static wchar_t *proc_command_line(const char *const *argv, PalErrno *err) {
    size_t n = 0;
    for (size_t i = 0; argv[i] != NULL; i++)
        n += (i > 0 ? 1 : 0) + proc_escape_arg(argv[i], NULL);
    char *line = proc_alloc(n + 1);
    wchar_t *w = proc_alloc((n + 1) * sizeof *w);
    if (line == NULL || w == NULL) {
        proc_free(line);
        proc_free(w);
        BURROW_OUT(err, PAL_ENOMEM);
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; argv[i] != NULL; i++) {
        if (i > 0)
            line[o++] = ' ';
        o += proc_escape_arg(argv[i], line + o);
    }
    line[o] = 0;
    bool ok = burrow__pal_widen(line, w, n + 1, err);
    proc_free(line);
    if (!ok) {
        proc_free(w);
        return NULL;
    }
    return w;
}

/* The environment block CreateProcessW takes: each entry and its NUL, then one
 * more NUL. UTF-16 never needs more units than UTF-8 has bytes. */
static wchar_t *proc_environment_block(const char *const *envp, PalErrno *err) {
    size_t n = 1;
    for (size_t i = 0; envp[i] != NULL; i++)
        n += strlen(envp[i]) + 1;
    wchar_t *w = proc_alloc((n + 1) * sizeof *w);
    if (w == NULL) {
        BURROW_OUT(err, PAL_ENOMEM);
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; envp[i] != NULL; i++) {
        if (!burrow__pal_widen(envp[i], w + o, n + 1 - o, err)) {
            proc_free(w);
            return NULL;
        }
        o += wcslen(w + o) + 1;
    }
    if (o == 0)
        w[o++] = 0; /* an empty block is two NULs */
    w[o] = 0;
    return w;
}

/* ------------------------------------------------------------------ spawn */

int64_t pal_spawn(const PalSpawn *req, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (req == NULL || req->path == NULL || req->argv == NULL || req->nfds < 0 ||
        req->nfds > 256 || (req->nfds > 0 && req->fds == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }

    int64_t result = -1;
    wchar_t *app = proc_alloc(PAL_WPATH_MAX * sizeof(wchar_t));
    wchar_t *dir =
        req->dir != NULL ? proc_alloc(PAL_WPATH_MAX * sizeof(wchar_t)) : NULL;
    wchar_t *cmd = NULL;
    wchar_t *env = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    HANDLE std[3] = {NULL, NULL, NULL};
    HANDLE list[256];
    DWORD nlist = 0;
    int32_t marked[256];
    int32_t nmarked = 0;

    if (app == NULL || (req->dir != NULL && dir == NULL)) {
        BURROW_OUT(err, PAL_ENOMEM);
        goto out;
    }
    if (!burrow__pal_widen(req->path, app, PAL_WPATH_MAX, err))
        goto out;
    if (dir != NULL && !burrow__pal_widen(req->dir, dir, PAL_WPATH_MAX, err))
        goto out;
    cmd = proc_command_line(req->argv, err);
    if (cmd == NULL)
        goto out;
    if (req->envp != NULL) {
        env = proc_environment_block(req->envp, err);
        if (env == NULL)
            goto out;
    }

    /* The standard handles are inheritable copies, as Go makes them, so that
     * the originals stay as they were. */
    HANDLE self = GetCurrentProcess();
    for (int32_t i = 0; i < req->nfds && i < 3; i++) {
        if (req->fds[i] == PAL_INVALID_HANDLE)
            continue;
        if (!DuplicateHandle(self, (HANDLE)(intptr_t)req->fds[i], self, &std[i], 0,
                             TRUE, DUPLICATE_SAME_ACCESS)) {
            std[i] = NULL;
            proc_fail(err);
            goto out;
        }
        list[nlist++] = std[i];
    }
    for (int32_t i = 3; i < req->nfds; i++) {
        if (req->fds[i] == PAL_INVALID_HANDLE)
            continue;
        HANDLE h = (HANDLE)(intptr_t)req->fds[i];
        bool seen = false;
        for (DWORD j = 0; j < nlist; j++)
            seen = seen || list[j] == h;
        if (seen)
            continue;
        DWORD info = 0;
        if (!GetHandleInformation(h, &info)) {
            proc_fail(err);
            goto out;
        }
        if ((info & HANDLE_FLAG_INHERIT) == 0) {
            if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
                proc_fail(err);
                goto out;
            }
            marked[nmarked++] = i;
        }
        list[nlist++] = h;
    }

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = std[0];
    si.StartupInfo.hStdOutput = std[1];
    si.StartupInfo.hStdError = std[2];
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (nlist > 0) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &size);
        attrs = proc_alloc(size);
        if (attrs == NULL) {
            BURROW_OUT(err, PAL_ENOMEM);
            goto out;
        }
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
            proc_free(attrs);
            attrs = NULL;
            proc_fail(err);
            goto out;
        }
        if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       list, nlist * sizeof list[0], NULL, NULL)) {
            proc_fail(err);
            goto out;
        }
        si.lpAttributeList = attrs;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    if ((req->flags & (PAL_SPAWN_SETPGID | PAL_SPAWN_SETSID)) != 0)
        flags |= CREATE_NEW_PROCESS_GROUP;
    if ((req->flags & PAL_SPAWN_SETSID) != 0)
        flags |= DETACHED_PROCESS;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(app, cmd, NULL, NULL, nlist > 0, flags, env, dir,
                        &si.StartupInfo, &pi)) {
        /* A directory that is not there is ERROR_DIRECTORY, the same as one
         * that is a file, and POSIX tells those two apart. */
        DWORD code = GetLastError();
        if (code == ERROR_DIRECTORY && dir != NULL &&
            GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) {
            BURROW_OUT(err, PAL_ENOENT);
            goto out;
        }
        SetLastError(code);
        proc_fail(err);
        goto out;
    }
    CloseHandle(pi.hThread);
    result = (int64_t)(intptr_t)pi.hProcess;

out:
    for (int32_t i = 0; i < nmarked; i++)
        SetHandleInformation((HANDLE)(intptr_t)req->fds[marked[i]], HANDLE_FLAG_INHERIT,
                             0);
    for (int i = 0; i < 3; i++) {
        if (std[i] != NULL)
            CloseHandle(std[i]);
    }
    if (attrs != NULL) {
        DeleteProcThreadAttributeList(attrs);
        proc_free(attrs);
    }
    proc_free(env);
    proc_free(cmd);
    proc_free(dir);
    proc_free(app);
    return result;
}

int64_t pal_wait(int64_t pid, int32_t *status, uint32_t flags, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (pid <= 0 || (flags & ~(uint32_t)PAL_WAIT_NOHANG) != 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    HANDLE h = (HANDLE)(intptr_t)pid;
    DWORD w = WaitForSingleObject(h, (flags & PAL_WAIT_NOHANG) != 0 ? 0 : INFINITE);
    if (w == WAIT_TIMEOUT)
        return 0;
    if (w != WAIT_OBJECT_0) {
        proc_fail(err);
        return -1;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) {
        proc_fail(err);
        return -1;
    }
    CloseHandle(h);
    BURROW_OUT(status, (int32_t)code);
    return pid;
}

/* Go's Process.Kill is TerminateProcess with an exit code of 1, and nothing
 * else a POSIX signal does has a Windows equivalent that works on an arbitrary
 * process. Zero asks whether it is still running. */
bool pal_kill(int64_t pid, int32_t sig, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (pid <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    HANDLE h = (HANDLE)(intptr_t)pid;
    if (sig == 0) {
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            BURROW_OUT(err, PAL_ESRCH);
            return false;
        }
        return true;
    }
    if (sig != PAL_SIGKILL) {
        BURROW_OUT(err, PAL_ENOTSUP);
        return false;
    }
    return TerminateProcess(h, 1) || proc_fail(err);
}

int64_t pal_getpid(void) {
    return (int64_t)GetCurrentProcessId();
}

/* ExitProcess tells every DLL the process is detaching, and the C runtime
 * answers by flushing stdio, which is running something on the way out.
 * TerminateProcess on ourselves does not, and it does not return once the
 * process is on its way down. ExitProcess is behind it only so that the
 * compiler can see nothing comes back. */
void pal_exit(int32_t code) {
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    ExitProcess((UINT)code);
}

/* -------------------------------------------------------------- environment */

/* The environment as the process had it the first time anyone asked, in
 * UTF-8. Windows keeps the real one as UTF-16 and the os package reads that
 * through its own calls, so this copy is what the runtime and the testing
 * package start from and is never rebuilt. */
static INIT_ONCE env_once = INIT_ONCE_STATIC_INIT;
static const char *const *env_list;

static BOOL CALLBACK env_build(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    static const char *const empty[] = {NULL};
    env_list = empty;
    wchar_t *block = GetEnvironmentStringsW();
    if (block == NULL)
        return TRUE;
    size_t count = 0;
    size_t units = 0;
    for (const wchar_t *p = block; *p != 0; p += wcslen(p) + 1) {
        count++;
        units += wcslen(p) + 1;
    }
    /* Three bytes of UTF-8 per unit is the most a unit can need, since a pair
     * of surrogates gives four bytes for two units. */
    size_t bytes = (count + 1) * sizeof(char *) + units * 3;
    char **list = proc_alloc(bytes);
    if (list == NULL) {
        FreeEnvironmentStringsW(block);
        return TRUE;
    }
    char *text = (char *)(list + count + 1);
    char *end = (char *)list + bytes;
    size_t i = 0;
    for (const wchar_t *p = block; *p != 0; p += wcslen(p) + 1) {
        size_t n = wcslen(p);
        int64_t got = burrow__pal_narrow(p, n, text, (size_t)(end - text) - 1);
        if (got < 0)
            break;
        text[got] = 0;
        list[i++] = text;
        text += got + 1;
    }
    list[i] = NULL;
    FreeEnvironmentStringsW(block);
    env_list = (const char *const *)list;
    return TRUE;
}

const char *const *pal_environ(void) {
    InitOnceExecuteOnce(&env_once, env_build, NULL, NULL);
    return env_list;
}

/* ------------------------------------------------------------ exec lookup */

/* A variable from the process's own environment, in UTF-8 on the process
 * heap, or NULL when it is not set. */
static char *proc_getenv_utf8(const wchar_t *name) {
    DWORD n = GetEnvironmentVariableW(name, NULL, 0);
    if (n == 0)
        return NULL;
    wchar_t *w = proc_alloc(n * sizeof *w);
    char *s = proc_alloc((size_t)n * 3 + 1);
    if (w == NULL || s == NULL) {
        proc_free(w);
        proc_free(s);
        return NULL;
    }
    DWORD got = GetEnvironmentVariableW(name, w, n);
    int64_t len = got < n ? burrow__pal_narrow(w, got, s, (size_t)n * 3) : -1;
    proc_free(w);
    if (len < 0) {
        proc_free(s);
        return NULL;
    }
    s[len] = 0;
    return s;
}

/* Go's chkStat: there, and not a directory. */
static bool proc_chk_stat(const char *path) {
    wchar_t *w = proc_alloc(PAL_WPATH_MAX * sizeof *w);
    if (w == NULL)
        return false;
    bool ok = false;
    if (burrow__pal_widen(path, w, PAL_WPATH_MAX, NULL)) {
        DWORD a = GetFileAttributesW(w);
        ok = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    proc_free(w);
    return ok;
}

static bool proc_is_sep(char c) {
    return c == ':' || c == '\\' || c == '/';
}

/* Go's hasExt: a dot after the last separator. */
static bool proc_has_ext(const char *file, size_t n) {
    size_t dot = n;
    size_t sep = n;
    for (size_t i = 0; i < n; i++) {
        if (file[i] == '.')
            dot = i;
        else if (proc_is_sep(file[i]))
            sep = i;
    }
    return dot < n && (sep == n || sep < dot);
}

static char proc_ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* Go's findExecutable, with the candidate built in buf, which already holds
 * the file in its first n bytes. Answers the length of what was found, or -1,
 * with *range set when a candidate would not fit. */
static int64_t proc_find_executable(char *buf, size_t n, size_t cap, const char *exts,
                                    bool *range) {
    if (proc_has_ext(buf, n) && proc_chk_stat(buf))
        return (int64_t)n;
    const char *e = exts;
    while (*e != 0) {
        const char *end = e;
        while (*end != 0 && *end != ';')
            end++;
        size_t elen = (size_t)(end - e);
        if (elen > 0) {
            bool dot = e[0] == '.';
            size_t need = n + elen + (dot ? 0 : 1);
            if (need + 1 > cap) {
                *range = true;
            } else {
                size_t o = n;
                if (!dot)
                    buf[o++] = '.';
                for (size_t i = 0; i < elen; i++)
                    buf[o++] = proc_ascii_lower(e[i]);
                buf[o] = 0;
                if (proc_chk_stat(buf))
                    return (int64_t)o;
                buf[n] = 0;
            }
        }
        e = *end == 0 ? end : end + 1;
    }
    return -1;
}

/* dir joined to name in buf, as filepath.Join would put them, with an empty
 * dir meaning name alone. */
static size_t proc_join(char *buf, size_t cap, const char *dir, size_t dlen,
                        const char *name, bool *range) {
    size_t nlen = strlen(name);
    bool sep = dlen > 0 && dir[dlen - 1] != '\\' && dir[dlen - 1] != '/';
    size_t need = dlen + (sep ? 1 : 0) + nlen;
    if (need + 1 > cap) {
        *range = true;
        return 0;
    }
    size_t o = 0;
    memcpy(buf, dir, dlen);
    o = dlen;
    if (sep)
        buf[o++] = '\\';
    memcpy(buf + o, name, nlen);
    o += nlen;
    buf[o] = 0;
    return o;
}

int64_t pal_exec_lookup(const char *name, char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (name == NULL || buf == NULL || cap <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    if (name[0] == 0) {
        BURROW_OUT(err, PAL_ENOENT);
        return -1;
    }
    char *pathext = proc_getenv_utf8(L"PATHEXT");
    const char *exts =
        pathext != NULL && pathext[0] != 0 ? pathext : ".com;.exe;.bat;.cmd";
    /* Candidates are built in a buffer of our own, so that a caller whose
     * buffer is too small hears ERANGE about the program that is there rather
     * than ENOENT because it was never looked at. A candidate too long even
     * for this one cannot be a file Windows would run. */
    char cand[PAL_WPATH_MAX * 3];
    bool range = false;
    int64_t found = -1;
    bool sep = false;
    for (const char *c = name; *c != 0; c++)
        sep = sep || proc_is_sep(*c);

    if (sep) {
        size_t n = proc_join(cand, sizeof cand, NULL, 0, name, &range);
        if (!range)
            found = proc_find_executable(cand, n, sizeof cand, exts, &range);
    } else {
        /* The current directory first, as Windows itself searches, unless the
         * variable that turns that off is set. A name found there is relative,
         * which is how os/exec knows to answer ErrDot. */
        DWORD nodot =
            GetEnvironmentVariableW(L"NoDefaultCurrentDirectoryInExePath", NULL, 0);
        if (nodot == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            size_t n = proc_join(cand, sizeof cand, NULL, 0, name, &range);
            if (!range)
                found = proc_find_executable(cand, n, sizeof cand, exts, &range);
        }
        char *path = proc_getenv_utf8(L"PATH");
        /* filepath.SplitList: semicolons, except inside quotes, which go. */
        for (const char *p = path; found < 0 && p != NULL && *p != 0;) {
            char dir[PAL_WPATH_MAX];
            size_t dlen = 0;
            bool quoted = false;
            while (*p != 0 && (quoted || *p != ';')) {
                if (*p == '"')
                    quoted = !quoted;
                else if (dlen + 1 < sizeof dir)
                    dir[dlen++] = *p;
                p++;
            }
            if (*p == ';')
                p++;
            if (dlen == 0)
                continue;
            size_t n = proc_join(cand, sizeof cand, dir, dlen, name, &range);
            if (n > 0)
                found = proc_find_executable(cand, n, sizeof cand, exts, &range);
        }
        proc_free(path);
    }
    proc_free(pathext);
    if (found < 0) {
        BURROW_OUT(err, PAL_ENOENT);
        return -1;
    }
    if (found >= cap) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    memcpy(buf, cand, (size_t)found + 1);
    return found;
}

/* -------------------------------------------------------------------- mmap */

void *pal_mmap(int64_t fd, int64_t off, int64_t len, uint32_t prot, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (fd == PAL_INVALID_HANDLE || off < 0 || len <= 0 ||
        (prot & ~(uint32_t)(PAL_PROT_READ | PAL_PROT_WRITE | PAL_PROT_EXEC |
                            PAL_PROT_PRIVATE)) != 0 ||
        (prot & (PAL_PROT_WRITE | PAL_PROT_EXEC)) == (PAL_PROT_WRITE | PAL_PROT_EXEC)) {
        BURROW_OUT(err, PAL_EINVAL);
        return NULL;
    }
    bool write = (prot & PAL_PROT_WRITE) != 0;
    bool exec = (prot & PAL_PROT_EXEC) != 0;
    bool priv = (prot & PAL_PROT_PRIVATE) != 0;
    DWORD page;
    DWORD access;
    if (priv) {
        page = exec ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
        access = FILE_MAP_COPY;
    } else if (write) {
        page = exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        access = FILE_MAP_WRITE;
    } else {
        page = exec ? PAGE_EXECUTE_READ : PAGE_READONLY;
        access = FILE_MAP_READ;
    }
    if (exec)
        access |= FILE_MAP_EXECUTE;
    uint64_t end = (uint64_t)off + (uint64_t)len;
    HANDLE m = CreateFileMappingW((HANDLE)(intptr_t)fd, NULL, page, (DWORD)(end >> 32),
                                  (DWORD)end, NULL);
    if (m == NULL) {
        proc_fail(err);
        return NULL;
    }
    void *addr =
        MapViewOfFile(m, access, (DWORD)((uint64_t)off >> 32), (DWORD)off, (SIZE_T)len);
    if (addr == NULL)
        proc_fail(err);
    /* The view holds the mapping open, so the handle can go now. */
    CloseHandle(m);
    return addr;
}

bool pal_munmap(void *addr, int64_t len, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (addr == NULL || len <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    return UnmapViewOfFile(addr) || proc_fail(err);
}

#endif /* BURROW_OS_WINDOWS */
