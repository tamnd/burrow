/* StartProcess on Windows and what it needs: EscapeArg and the command line,
 * the environment block, FullPath and finding argv0 from the new working
 * directory, and the attribute list that says which handles the child gets.
 *
 * Derived from Go's src/syscall/exec_windows.go, and newProcThreadAttributeList
 * from syscall_windows.go. Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/unicode/utf16.h"
#include "burrow/utf8.h"

#include "internal.h"

#include <string.h>

SyncRWMutex syscall_fork_lock;

/* ------------------------------------------------------------- EscapeArg */

/* Go's appendEscapeArg, writing to out when it is not NULL, and how many
 * bytes it takes either way. */
static Int exw_escape_arg(Str s, Byte *out) {
    if (s.len == 0) {
        if (out != NULL)
            memcpy(out, "\"\"", 2);
        return 2;
    }
    bool needs_backslash = false;
    bool has_space = false;
    for (Int i = 0; i < s.len; i++) {
        switch (s.p[i]) {
        case '"':
        case '\\':
            needs_backslash = true;
            break;
        case ' ':
        case '\t':
            has_space = true;
            break;
        default:
            break;
        }
    }
    Int n = 0;
    if (!needs_backslash && !has_space) {
        /* No special handling required; normal case. */
        if (out != NULL)
            memcpy(out, s.p, (size_t)s.len);
        return s.len;
    }
    if (!needs_backslash) {
        /* has_space is true, so the string needs quoting. */
        if (out != NULL) {
            out[0] = '"';
            memcpy(out + 1, s.p, (size_t)s.len);
            out[s.len + 1] = '"';
        }
        return s.len + 2;
    }
    if (has_space) {
        if (out != NULL)
            out[n] = '"';
        n++;
    }
    Int slashes = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        switch (c) {
        case '\\':
            slashes++;
            break;
        case '"':
            for (; slashes > 0; slashes--) {
                if (out != NULL)
                    out[n] = '\\';
                n++;
            }
            if (out != NULL)
                out[n] = '\\';
            n++;
            break;
        default:
            slashes = 0;
            break;
        }
        if (out != NULL)
            out[n] = c;
        n++;
    }
    if (has_space) {
        for (; slashes > 0; slashes--) {
            if (out != NULL)
                out[n] = '\\';
            n++;
        }
        if (out != NULL)
            out[n] = '"';
        n++;
    }
    return n;
}

Str syscall_escape_arg(Alloc *a, Str s) {
    Int n = exw_escape_arg(s, NULL);
    Byte *b = mem_alloc_nozero(a, (size_t)n, 1);
    if (b == NULL)
        return BURROW_STR_EMPTY;
    exw_escape_arg(s, b);
    return str_from_bytes(b, n);
}

/* Go's makeCmdLine: the arguments, a slice of Str, escaped and joined with
 * spaces, from a. */
static Str exw_make_cmd_line(Alloc *a, Slice args, Error *err) {
    const Str *v = (const Str *)args.p;
    Int n = 0;
    for (Int i = 0; i < args.len; i++)
        n += (n > 0 ? 1 : 0) + exw_escape_arg(v[i], NULL);
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *b = mem_alloc_nozero(a, (size_t)n, 1);
    if (b == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return BURROW_STR_EMPTY;
    }
    Int w = 0;
    for (Int i = 0; i < args.len; i++) {
        if (w > 0)
            b[w++] = ' ';
        w += exw_escape_arg(v[i], b + w);
    }
    return str_from_bytes(b, w);
}

/* ------------------------------------------------------ the environment */

/* The key of kv, up to its first '=', or none when it has no '='. */
static bool exw_env_key(Str kv, Str *key) {
    for (Int i = 0; i < kv.len; i++) {
        if (kv.p[i] == '=') {
            *key = (Str){kv.p, i};
            return true;
        }
    }
    return false;
}

static Byte exw_upper(Byte c) {
    return c >= 'a' && c <= 'z' ? (Byte)(c - ('a' - 'A')) : c;
}

/* Go's cmpEnv: the keys compared with ASCII upper cased, where an entry with
 * no '=' has a nil key, which is less than every other. */
static int exw_cmp_env(Str a, Str b) {
    Str ka, kb;
    bool ha = exw_env_key(a, &ka);
    bool hb = exw_env_key(b, &kb);
    if (!ha)
        ka = BURROW_STR_EMPTY;
    if (!hb)
        kb = BURROW_STR_EMPTY;
    Int n = ka.len < kb.len ? ka.len : kb.len;
    for (Int i = 0; i < n; i++) {
        Byte ca = exw_upper(ka.p[i]), cb = exw_upper(kb.p[i]);
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    return ka.len < kb.len ? -1 : ka.len > kb.len ? 1 : 0;
}

/* Go's createEnvBlock: the strings in envv, sorted by name as CreateProcess
 * wants them, each as UTF-16 with a 0 after it and another 0 after them all,
 * from a. A string with a NUL in it is EINVAL. */
static uint16_t *exw_env_block(Alloc *a, Slice envv, Error *err) {
    if (envv.len == 0) {
        uint16_t *b = BURROW_NEW_N(a, uint16_t, 2);
        if (b == NULL)
            BURROW_OUT(err, burrow_err_out_of_memory);
        else
            b[0] = b[1] = 0;
        return b;
    }
    const Str *in = (const Str *)envv.p;
    Int units = 1;
    for (Int i = 0; i < envv.len; i++) {
        if (in[i].len > 0 && memchr(in[i].p, 0, (size_t)in[i].len) != NULL) {
            BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
            return NULL;
        }
        /* A byte is never more than one unit, nor a 4-byte sequence more
         * than two. */
        units += in[i].len + 1;
    }
    /* Sorted with a stable insertion sort, which gives Go's order for every
     * environment whose names differ. */
    Str *v = BURROW_NEW_N(a, Str, (size_t)envv.len);
    uint16_t *b = BURROW_NEW_N(a, uint16_t, (size_t)units);
    if (v == NULL || b == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    for (Int i = 0; i < envv.len; i++) {
        Str x = in[i];
        Int j = i;
        while (j > 0 && exw_cmp_env(v[j - 1], x) > 0) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
    Int w = 0;
    for (Int i = 0; i < envv.len; i++) {
        Str s = v[i];
        while (s.len > 0) {
            Int size = 0;
            Rune r = utf8_decode_rune_in_string(s, &size);
            Rune r2 = 0;
            Rune r1 = utf16_encode_rune(r, &r2);
            if (r1 == 0xFFFD && r2 == 0xFFFD) {
                b[w++] = (uint16_t)r;
            } else {
                b[w++] = (uint16_t)r1;
                b[w++] = (uint16_t)r2;
            }
            s.p += size;
            s.len -= size;
        }
        b[w++] = 0;
    }
    b[w] = 0;
    return b;
}

/* -------------------------------------------------------------- FullPath */

Str syscall_full_path(Alloc *a, Str name, Error *err) {
    burrow__SyscallWString w;
    uint16_t *p = burrow__syscall_wstring(&w, name, err);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    uint32_t n = 100;
    for (;;) {
        uint32_t size = n;
        uint16_t *buf = BURROW_NEW_N(heap_allocator(), uint16_t, size);
        if (buf == NULL) {
            burrow__syscall_wstring_free(&w);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return BURROW_STR_EMPTY;
        }
        Error e = BURROW_NO_ERROR;
        n = syscall_get_full_path_name(p, size, buf, NULL, &e);
        if (BURROW_FAILED(e)) {
            mem_free(heap_allocator(), buf, (size_t)size * sizeof(uint16_t),
                     _Alignof(uint16_t));
            burrow__syscall_wstring_free(&w);
            BURROW_OUT(err, e);
            return BURROW_STR_EMPTY;
        }
        if (n <= size) {
            Str s = syscall_utf16_to_string(
                a, (Slice){buf, (Int)n, (Int)size, TYPE_UINT16});
            mem_free(heap_allocator(), buf, (size_t)size * sizeof(uint16_t),
                     _Alignof(uint16_t));
            burrow__syscall_wstring_free(&w);
            BURROW_OUT(err, BURROW_NO_ERROR);
            return s;
        }
        mem_free(heap_allocator(), buf, (size_t)size * sizeof(uint16_t),
                 _Alignof(uint16_t));
    }
}

static bool exw_is_slash(Byte c) {
    return c == '\\' || c == '/';
}

/* a and b joined, from al. */
static Str exw_concat(Alloc *al, Str a, Str b, Error *err) {
    Byte *p = mem_alloc_nozero(al, (size_t)(a.len + b.len) + 1, 1);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return BURROW_STR_EMPTY;
    }
    if (a.len > 0)
        memcpy(p, a.p, (size_t)a.len);
    if (b.len > 0)
        memcpy(p + a.len, b.p, (size_t)b.len);
    return str_from_bytes(p, a.len + b.len);
}

/* FullPath of a and b joined, from al. */
static Str exw_full_path_of(Alloc *al, Str a, Str b, Error *err) {
    Error e = BURROW_NO_ERROR;
    Str s = exw_concat(al, a, b, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return BURROW_STR_EMPTY;
    }
    return syscall_full_path(al, s, err);
}

/* Go's normalizeDir. */
static Str exw_normalize_dir(Alloc *al, Str dir, Error *err) {
    Error e = BURROW_NO_ERROR;
    Str ndir = syscall_full_path(al, dir, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return BURROW_STR_EMPTY;
    }
    if (ndir.len > 2 && exw_is_slash(ndir.p[0]) && exw_is_slash(ndir.p[1])) {
        /* dir cannot have \\server\share\path form */
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
        return BURROW_STR_EMPTY;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ndir;
}

/* Go's joinExeDirAndFName, with what it makes from al. */
static Str exw_join_exe_dir_and_fname(Alloc *al, Str dir, Str p, Error *err) {
    if (p.len == 0) {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
        return BURROW_STR_EMPTY;
    }
    if (p.len > 2 && exw_is_slash(p.p[0]) && exw_is_slash(p.p[1])) {
        /* \\server\share\path form */
        BURROW_OUT(err, BURROW_NO_ERROR);
        return p;
    }
    Error e = BURROW_NO_ERROR;
    if (p.len > 1 && p.p[1] == ':') {
        /* has drive letter */
        if (p.len == 2) {
            BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
            return BURROW_STR_EMPTY;
        }
        if (exw_is_slash(p.p[2])) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return p;
        }
        Str d = exw_normalize_dir(al, dir, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return BURROW_STR_EMPTY;
        }
        if (exw_upper(p.p[0]) == exw_upper(d.p[0])) {
            Str dd = exw_concat(al, d, BURROW_S("\\"), &e);
            if (BURROW_FAILED(e)) {
                BURROW_OUT(err, e);
                return BURROW_STR_EMPTY;
            }
            return exw_full_path_of(al, dd, (Str){p.p + 2, p.len - 2}, err);
        }
        return syscall_full_path(al, p, err);
    }
    /* no drive letter */
    Str d = exw_normalize_dir(al, dir, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return BURROW_STR_EMPTY;
    }
    if (exw_is_slash(p.p[0]))
        return exw_full_path_of(al, (Str){d.p, d.len < 2 ? d.len : 2}, p, err);
    Str dd = exw_concat(al, d, BURROW_S("\\"), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return BURROW_STR_EMPTY;
    }
    return exw_full_path_of(al, dd, p, err);
}

/* ------------------------------------------- the attribute list and the rest */

#define EXW_PROC_THREAD_ATTRIBUTE_PARENT_PROCESS 0x00020000U
#define EXW_PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002U
#define EXW_EXTENDED_STARTUPINFO_PRESENT 0x00080000U

/* Go's _STARTUPINFOEXW. */
typedef struct ExwStartupInfoEx {
    SyscallStartupInfo startup_info;
    void *proc_thread_attribute_list;
} ExwStartupInfoEx;

static Error exw_initialize_list(void *list, uint32_t count, Uintptr *size) {
    SyscallErrno e1 = 0;
    Uintptr r1 = burrow__syscall_n(
        syscall_lazy_proc_addr(
            &burrow__syscall_procs
                [BURROW__SYSCALL_PROC_INITIALIZE_PROC_THREAD_ATTRIBUTE_LIST]),
        (const Uintptr[]){(Uintptr)list, (Uintptr)count, 0, (Uintptr)(void *)size}, 4,
        NULL, &e1);
    return r1 == 0 ? burrow__syscall_errno_err(e1) : BURROW_NO_ERROR;
}

static void exw_delete_list(void *list) {
    SyscallErrno e1 = 0;
    (void)burrow__syscall_n(
        syscall_lazy_proc_addr(
            &burrow__syscall_procs
                [BURROW__SYSCALL_PROC_DELETE_PROC_THREAD_ATTRIBUTE_LIST]),
        (const Uintptr[]){(Uintptr)list}, 1, NULL, &e1);
}

static Error exw_update_list(void *list, Uintptr attr, void *value, Uintptr size) {
    SyscallErrno e1 = 0;
    Uintptr r1 = burrow__syscall_n(
        syscall_lazy_proc_addr(
            &burrow__syscall_procs[BURROW__SYSCALL_PROC_UPDATE_PROC_THREAD_ATTRIBUTE]),
        (const Uintptr[]){(Uintptr)list, 0, attr, (Uintptr)value, size, 0, 0}, 7, NULL,
        &e1);
    return r1 == 0 ? burrow__syscall_errno_err(e1) : BURROW_NO_ERROR;
}

/* Go's newProcThreadAttributeList: a list with room for count attributes, in
 * memory from LocalAlloc, which exw_free_list gives back. */
static void *exw_new_list(uint32_t count, Error *err) {
    Uintptr size = 0;
    Error e = exw_initialize_list(NULL, count, &size);
    const SyscallErrno *n = errors_as(e, TYPE_SYSCALL_ERRNO);
    if (n == NULL || *n != SYSCALL_ERROR_INSUFFICIENT_BUFFER) {
        if (BURROW_OK(e))
            e = errors_new(error_allocator(),
                           BURROW_S("unable to query buffer size from "
                                    "InitializeProcThreadAttributeList"));
        BURROW_OUT(err, e);
        return NULL;
    }
    /* LMEM_FIXED, 0. */
    Uintptr list = burrow__syscall_local_alloc(0, (uint32_t)size, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    e = exw_initialize_list((void *)list, count, &size);
    if (BURROW_FAILED(e)) {
        (void)syscall_local_free((SyscallHandle)list, NULL);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (void *)list;
}

static void exw_free_list(void *list) {
    exw_delete_list(list);
    (void)syscall_local_free((SyscallHandle)(Uintptr)list, NULL);
}

void syscall_close_on_exec(SyscallHandle fd) {
    (void)syscall_set_handle_information(fd, (uint32_t)SYSCALL_HANDLE_FLAG_INHERIT, 0);
}

Error syscall_set_nonblock(SyscallHandle fd, bool nonblocking) {
    (void)fd;
    (void)nonblocking;
    return BURROW_NO_ERROR;
}

Error syscall_exec(Str argv0, Slice argv, Slice envv) {
    (void)argv0;
    (void)argv;
    (void)envv;
    return burrow__syscall_errno_err(SYSCALL_EWINDOWS);
}

/* --------------------------------------------------------- StartProcess */

/* What syscall_start_process has to give back on the way out, in the
 * reverse of the order Go's defers run in, which is the same thing. */
typedef struct ExwCleanup {
    SyscallHandle process;
    SyscallHandle parent;
    SyscallHandle dup[3];
    void *list;
    burrow__SyscallWString argv0;
    burrow__SyscallWString cmd;
    burrow__SyscallWString dir;
    bool have_argv0, have_cmd, have_dir;
    Arena ar;
} ExwCleanup;

static void exw_cleanup(ExwCleanup *c) {
    if (c->list != NULL)
        exw_free_list(c->list);
    for (int i = 2; i >= 0; i--) {
        if (c->dup[i] != 0)
            (void)syscall_duplicate_handle(c->parent, c->dup[i], 0, NULL, 0, false,
                                           (uint32_t)SYSCALL_DUPLICATE_CLOSE_SOURCE);
    }
    if (c->have_dir)
        burrow__syscall_wstring_free(&c->dir);
    if (c->have_cmd)
        burrow__syscall_wstring_free(&c->cmd);
    if (c->have_argv0)
        burrow__syscall_wstring_free(&c->argv0);
    arena_free(&c->ar);
}

static Int exw_fail(ExwCleanup *c, Uintptr *handle, Error *err, Error e) {
    exw_cleanup(c);
    if (handle != NULL)
        *handle = 0;
    BURROW_OUT(err, e);
    return 0;
}

Int syscall_start_process(Str argv0, Slice argv, const SyscallProcAttr *attr,
                          Uintptr *handle, Error *err) {
    static const SyscallProcAttr zero_proc_attr = {0};
    static const SyscallSysProcAttr zero_sys_proc_attr = {0};
    if (handle != NULL)
        *handle = 0;
    if (argv0.len == 0) {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EWINDOWS));
        return 0;
    }
    if (attr == NULL)
        attr = &zero_proc_attr;
    const SyscallSysProcAttr *sys = attr->sys;
    if (sys == NULL)
        sys = &zero_sys_proc_attr;
    if (attr->files.len > 3) {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EWINDOWS));
        return 0;
    }
    if (attr->files.len < 3) {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
        return 0;
    }

    ExwCleanup c;
    memset(&c, 0, sizeof c);
    arena_init(&c.ar, NULL, 0);
    Alloc *al = arena_allocator(&c.ar);
    Error e = BURROW_NO_ERROR;

    if (attr->dir.len != 0) {
        /* StartProcess takes argv0 as relative to attr->dir, since it is as if
         * the child changes to attr->dir before it runs argv0. CreateProcess
         * looks for argv0 relative to the current directory, and only once
         * the child has started does it change to attr->dir. Making argv0
         * absolute here makes up the difference. */
        argv0 = exw_join_exe_dir_and_fname(al, attr->dir, argv0, &e);
        if (BURROW_FAILED(e))
            return exw_fail(&c, handle, err, e);
    }
    uint16_t *argv0p = burrow__syscall_wstring(&c.argv0, argv0, &e);
    if (argv0p == NULL)
        return exw_fail(&c, handle, err, e);
    c.have_argv0 = true;

    /* CreateProcess takes the command line as one string: sys->cmd_line when
     * it is set, and otherwise the arguments escaped and joined with
     * spaces. */
    Str cmdline = sys->cmd_line;
    if (cmdline.len == 0) {
        cmdline = exw_make_cmd_line(al, argv, &e);
        if (BURROW_FAILED(e))
            return exw_fail(&c, handle, err, e);
    }
    uint16_t *argvp = NULL;
    if (cmdline.len != 0) {
        argvp = burrow__syscall_wstring(&c.cmd, cmdline, &e);
        if (argvp == NULL)
            return exw_fail(&c, handle, err, e);
        c.have_cmd = true;
    }
    uint16_t *dirp = NULL;
    if (attr->dir.len != 0) {
        dirp = burrow__syscall_wstring(&c.dir, attr->dir, &e);
        if (dirp == NULL)
            return exw_fail(&c, handle, err, e);
        c.have_dir = true;
    }

    SyscallHandle p = syscall_get_current_process(NULL);
    c.parent = p;
    if (sys->parent_process != 0)
        c.parent = sys->parent_process;
    const Uintptr *files = (const Uintptr *)attr->files.p;
    for (int i = 0; i < 3; i++) {
        if (files[i] > 0) {
            e = syscall_duplicate_handle(p, (SyscallHandle)files[i], c.parent,
                                         &c.dup[i], 0, true,
                                         (uint32_t)SYSCALL_DUPLICATE_SAME_ACCESS);
            if (BURROW_FAILED(e))
                return exw_fail(&c, handle, err, e);
        }
    }
    c.list = exw_new_list(2, &e);
    if (c.list == NULL)
        return exw_fail(&c, handle, err, e);
    ExwStartupInfoEx si;
    memset(&si, 0, sizeof si);
    si.startup_info.cb = (uint32_t)sizeof si;
    si.startup_info.flags = (uint32_t)SYSCALL_STARTF_USESTDHANDLES;
    if (sys->hide_window) {
        si.startup_info.flags |= (uint32_t)SYSCALL_STARTF_USESHOWWINDOW;
        si.startup_info.show_window = (uint16_t)SYSCALL_SW_HIDE;
    }
    SyscallHandle parent_process = sys->parent_process;
    if (parent_process != 0) {
        e = exw_update_list(c.list, EXW_PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
                            &parent_process, (Uintptr)sizeof parent_process);
        if (BURROW_FAILED(e))
            return exw_fail(&c, handle, err, e);
    }
    si.startup_info.std_input = c.dup[0];
    si.startup_info.std_output = c.dup[1];
    si.startup_info.std_err = c.dup[2];

    /* A NULL handle anywhere in PROC_THREAD_ATTRIBUTE_HANDLE_LIST makes it
     * take the whole list as empty, so those go. */
    Int extra = sys->additional_inherited_handles.len;
    SyscallHandle *fd = BURROW_NEW_N(al, SyscallHandle, (size_t)(3 + extra));
    if (fd == NULL)
        return exw_fail(&c, handle, err, burrow_err_out_of_memory);
    Int nfd = 0;
    for (int i = 0; i < 3; i++) {
        if (c.dup[i] != 0)
            fd[nfd++] = c.dup[i];
    }
    const SyscallHandle *more =
        (const SyscallHandle *)sys->additional_inherited_handles.p;
    for (Int i = 0; i < extra; i++) {
        if (more[i] != 0)
            fd[nfd++] = more[i];
    }
    bool will_inherit_handles = nfd > 0 && !sys->no_inherit_handles;

    /* Do not accidentally inherit more than these handles. */
    if (will_inherit_handles) {
        e = exw_update_list(c.list, EXW_PROC_THREAD_ATTRIBUTE_HANDLE_LIST, fd,
                            (Uintptr)nfd * (Uintptr)sizeof fd[0]);
        if (BURROW_FAILED(e))
            return exw_fail(&c, handle, err, e);
    }

    uint16_t *env_block = exw_env_block(al, attr->env, &e);
    if (env_block == NULL)
        return exw_fail(&c, handle, err, e);

    si.proc_thread_attribute_list = c.list;
    SyscallProcessInformation pi;
    memset(&pi, 0, sizeof pi);
    uint32_t flags = sys->creation_flags |
                     (uint32_t)SYSCALL_CREATE_UNICODE_ENVIRONMENT |
                     EXW_EXTENDED_STARTUPINFO_PRESENT;
    if (sys->token != 0)
        e = syscall_create_process_as_user(
            sys->token, argv0p, argvp, sys->process_attributes, sys->thread_attributes,
            will_inherit_handles, flags, env_block, dirp, &si.startup_info, &pi);
    else
        e = syscall_create_process(argv0p, argvp, sys->process_attributes,
                                   sys->thread_attributes, will_inherit_handles, flags,
                                   env_block, dirp, &si.startup_info, &pi);
    if (BURROW_FAILED(e))
        return exw_fail(&c, handle, err, e);
    (void)syscall_close_handle(pi.thread);
    exw_cleanup(&c);
    if (handle != NULL)
        *handle = (Uintptr)pi.process;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (Int)pi.process_id;
}

#endif /* BURROW_OS_WINDOWS */
