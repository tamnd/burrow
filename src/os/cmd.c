/* exec.Cmd: starting a program, feeding it and collecting what it writes.
 *
 * Derived from Go's src/os/exec/exec.go, exec_unix.go and exec_windows.go.
 * Go source: go1.27.1.
 *
 * Go copies to and from the child's streams on goroutines. Here each copy runs
 * on a thread of its own, and so does the watch on a Cmd's context, which
 * keeps a Cmd usable from a plain main and keeps a P free while a pipe blocks.
 * A blocked read of a pipe cannot be ended by closing it from another thread,
 * as Go's poller can, so the copying threads wait for the pipe with
 * pal_fd_wait on Unix and are woken with pal_thread_cancel_io on Windows when
 * WaitDelay runs out. The errors the threads make are copied into the Cmd's
 * own arena under its lock, and Wait copies them again into the caller's
 * error_allocator.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os/exec.h"

#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/clock.h"
#include "burrow/io/fs.h"
#include "burrow/lock.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/note.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/path/filepath.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync/atomic.h"
#include "burrow/thread.h"
#include "burrow/type.h"

#include "exec_internal.h"
#include "internal.h"

#include <stdarg.h>
#include <string.h>

#define CMD_LIT(s) ((Str){(const Byte *)(s), (Int)(sizeof(s) - 1)})
#define CMD_COPY_BUF ((Int)32 * 1024)
#define CMD_MAX_FILES 6

BURROW_SENTINEL_ERROR(exec_err_dot,
                      "cannot run executable found relative to current directory");
BURROW_SENTINEL_ERROR(exec_err_wait_delay,
                      "exec: WaitDelay expired before I/O complete");

/* io.Copy's errInvalidWrite, which Go does not export either. */
static const Str cmd_err_invalid_write__text = {
    (const Byte *)"invalid write result", (Int)(sizeof("invalid write result") - 1)};
static const Error cmd_err_invalid_write = {&burrow_sentinel_error_vt,
                                            &cmd_err_invalid_write__text};

static Byte *cmd_put(Byte *p, Str s) {
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return p + s.len;
}

static Error cmd_errorf(const char *text) {
    return errors_new(error_allocator(), str_from_cstr(text));
}

/* ---------------------------------------------------------------- exec.Error */

typedef struct ExecErrorBox {
    ExecError e;
    Str message;
} ExecErrorBox;

static Str exec_error_message(const void *self) {
    return ((const ExecErrorBox *)self)->message;
}

static Error exec_error_unwrap_slot(const void *self) {
    return ((const ExecError *)self)->err;
}

static const Type exec_error_desc = {
    {(const Byte *)"Error", 5},
    {(const Byte *)"os/exec", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(ExecError),
    (uint16_t)_Alignof(ExecError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x65786572U, /* "exer" */
    NULL,
};

const Type *const TYPE_EXEC_ERROR = &exec_error_desc;

static Error exec_error_clone(const void *self, Alloc *a);

static const ErrorVT exec_error_vt = {
    &exec_error_desc, exec_error_message, exec_error_unwrap_slot, NULL, NULL, NULL,
    exec_error_clone,
};

/* One allocation: the box, the name, and "exec: " + the quoted name + ": " +
 * the text of err. */
static Error exec_error_new(Alloc *a, Str name, Error err) {
    Str q = strconv_quote(heap_allocator(), name);
    if (q.len == 0) { /* even "" quotes to two bytes */
        return burrow_err_out_of_memory;
    }
    Str text = error_text(err);
    Int mlen = 6 + q.len + 2 + text.len;
    size_t size = sizeof(ExecErrorBox) + (size_t)name.len + (size_t)mlen;
    ExecErrorBox *b = (ExecErrorBox *)mem_alloc_nozero(a, size, _Alignof(ExecErrorBox));
    if (b == NULL) {
        mem_free(heap_allocator(), (void *)(uintptr_t)q.p, (size_t)q.len, 1);
        return burrow_err_out_of_memory;
    }
    Byte *p = (Byte *)(b + 1);
    b->e.name = str_from_bytes(p, name.len);
    p = cmd_put(p, name);
    b->e.err = err;
    b->message = str_from_bytes(p, mlen);
    p = cmd_put(p, CMD_LIT("exec: "));
    p = cmd_put(p, q);
    p = cmd_put(p, CMD_LIT(": "));
    cmd_put(p, text);
    mem_free(heap_allocator(), (void *)(uintptr_t)q.p, (size_t)q.len, 1);
    return (Error){&exec_error_vt, b};
}

static Error exec_error_clone(const void *self, Alloc *a) {
    const ExecError *e = (const ExecError *)self;
    return exec_error_new(a, e->name, error_retain(a, e->err));
}

Error burrow__exec_error(Str name, Error err) {
    return exec_error_new(error_allocator(), name, err);
}

Str exec_error_error(const ExecError *e, Alloc *a) {
    Error err = exec_error_new(a, e->name, e->err);
    if (err.vt != &exec_error_vt)
        return BURROW_STR_EMPTY;
    return ((const ExecErrorBox *)err.data)->message;
}

Error exec_error_as_error(const ExecError *e, Alloc *a) {
    return exec_error_new(a, e->name, e->err);
}

/* -------------------------------------------------------- exec.wrappedError */

typedef struct CmdWrapped {
    Error err;
    Str message;
} CmdWrapped;

static Str cmd_wrapped_message(const void *self) {
    return ((const CmdWrapped *)self)->message;
}

static Error cmd_wrapped_unwrap(const void *self) {
    return ((const CmdWrapped *)self)->err;
}

static const Type cmd_wrapped_desc = {
    {(const Byte *)"wrappedError", 12},
    {(const Byte *)"os/exec", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(CmdWrapped),
    (uint16_t)_Alignof(CmdWrapped),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x65787772U, /* "exwr" */
    NULL,
};

static const ErrorVT cmd_wrapped_vt = {
    &cmd_wrapped_desc, cmd_wrapped_message, cmd_wrapped_unwrap, NULL, NULL, NULL, NULL,
};

/* prefix + ": " + the text of err, wrapping err. */
static Error cmd_wrap(Alloc *a, Str prefix, Error err) {
    Str text = error_text(err);
    Int mlen = prefix.len + 2 + text.len;
    CmdWrapped *w = (CmdWrapped *)mem_alloc_nozero(a, sizeof(CmdWrapped) + (size_t)mlen,
                                                   _Alignof(CmdWrapped));
    if (w == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(w + 1);
    w->err = err;
    w->message = str_from_bytes(p, mlen);
    p = cmd_put(p, prefix);
    p = cmd_put(p, CMD_LIT(": "));
    cmd_put(p, text);
    return (Error){&cmd_wrapped_vt, w};
}

/* ------------------------------------------------------------ exec.ExitError */

/* The ExecExitError first, so errors_as hands back a pointer to it, then the
 * ProcessState it points at, then the message and the stderr bytes. */
typedef struct ExecExitErrorBox {
    ExecExitError e;
    OsProcessState ps;
    Str message;
} ExecExitErrorBox;

static Str exec_exit_error_message(const void *self) {
    return ((const ExecExitErrorBox *)self)->message;
}

static const Type exec_exit_error_desc = {
    {(const Byte *)"ExitError", 9},
    {(const Byte *)"os/exec", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(ExecExitError),
    (uint16_t)_Alignof(ExecExitError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x65786565U, /* "exee" */
    NULL,
};

const Type *const TYPE_EXEC_EXIT_ERROR = &exec_exit_error_desc;

static Error exec_exit_error_clone(const void *self, Alloc *a);

static const ErrorVT exec_exit_error_vt = {
    &exec_exit_error_desc, exec_exit_error_message, NULL, NULL, NULL, NULL,
    exec_exit_error_clone,
};

static Error exec_exit_error_new(Alloc *a, const OsProcessState *ps, Slice stderr_) {
    Str text = os_process_state_string(ps, heap_allocator());
    size_t size = sizeof(ExecExitErrorBox) + (size_t)text.len + (size_t)stderr_.len;
    ExecExitErrorBox *b =
        (ExecExitErrorBox *)mem_alloc_nozero(a, size, _Alignof(ExecExitErrorBox));
    if (b == NULL) {
        mem_free(heap_allocator(), (void *)(uintptr_t)text.p, (size_t)text.len, 1);
        return burrow_err_out_of_memory;
    }
    Byte *p = (Byte *)(b + 1);
    b->ps = *ps;
    b->ps.alloc = NULL;
    b->e.process_state = &b->ps;
    b->message = str_from_bytes(p, text.len);
    p = cmd_put(p, text);
    mem_free(heap_allocator(), (void *)(uintptr_t)text.p, (size_t)text.len, 1);
    if (slice_is_nil(stderr_)) {
        b->e.stderr_ = slice_nil(TYPE_BYTE);
    } else {
        if (stderr_.len > 0)
            memcpy(p, stderr_.p, (size_t)stderr_.len);
        b->e.stderr_ = slice_from(p, stderr_.len, stderr_.len, TYPE_BYTE);
    }
    return (Error){&exec_exit_error_vt, b};
}

static Error exec_exit_error_clone(const void *self, Alloc *a) {
    const ExecExitError *e = (const ExecExitError *)self;
    return exec_exit_error_new(a, e->process_state, e->stderr_);
}

Str exec_exit_error_error(const ExecExitError *e, Alloc *a) {
    return os_process_state_string(e->process_state, a);
}

/* ----------------------------------------------------- the state of one Cmd */

typedef struct CmdCopier {
    burrow__Thread t;
    burrow__ExecState *st;
    bool in;      /* stdin: from r into pipe. Otherwise from pipe into w. */
    bool running; /* the thread started and has not been joined */
    IoReader r;
    IoWriter w;
    OsFile *pipe;
    SyncAtomicUint32 in_io; /* on Windows, inside a ReadFile or WriteFile of pipe */
} CmdCopier;

struct burrow__ExecState {
    burrow__Lock mu;
    Arena errs; /* the errors the threads hand over, under mu */
    Slice own_args;
    bool own_cmd; /* by exec_command, which allocated the Cmd too */

    /* The child's ends, closed once Start is done with them, and ours. */
    OsFile *child_io[CMD_MAX_FILES];
    int n_child_io;
    OsFile *parent_io[CMD_MAX_FILES];
    bool parent_copier[CMD_MAX_FILES]; /* a copier owns it and closes it */
    bool parent_closed[CMD_MAX_FILES];
    int n_parent_io;
    OsFile *made[CMD_MAX_FILES * 2]; /* every OsFile this Cmd made, to free */
    int n_made;

    CmdCopier cp[3];
    int n_cp;
    int running;     /* copiers not finished, under mu */
    Error first_err; /* the first error a copier finished with, under mu */
    bool consumed;   /* Go's goroutineErr = nil */
    burrow__Note copiers_done;
    SyncAtomicUint32 cancelled;
    int64_t cancel_r, cancel_w; /* on Unix, the pipe that wakes pal_fd_wait */

    /* The watch on the context. */
    bool watching;
    burrow__Thread watch;
    Chan *stop;
    burrow__Note arrive, result_ready;
    Error res_err;
    bool res_timer;
    int64_t res_deadline;
    Error late_err; /* the watch could not be started */
};

static void cmd_add_made(burrow__ExecState *st, OsFile *f) {
    if (st->n_made < CMD_MAX_FILES * 2)
        st->made[st->n_made++] = f;
}

static void cmd_add_child(burrow__ExecState *st, OsFile *f) {
    if (st->n_child_io < CMD_MAX_FILES)
        st->child_io[st->n_child_io++] = f;
}

static void cmd_add_parent(burrow__ExecState *st, OsFile *f, bool copier) {
    if (st->n_parent_io < CMD_MAX_FILES) {
        st->parent_copier[st->n_parent_io] = copier;
        st->parent_closed[st->n_parent_io] = false;
        st->parent_io[st->n_parent_io++] = f;
    }
}

/* err as it can live in the Cmd, under mu. */
static Error cmd_keep(burrow__ExecState *st, Error err) {
    if (BURROW_OK(err))
        return err;
    return error_retain(arena_allocator(&st->errs), err);
}

/* ------------------------------------------------------------------- Command */

static Error cmd_default_cancel(void *env) {
    ExecCmd *c = (ExecCmd *)env;
    return os_process_kill(c->process);
}

static burrow__ExecState *cmd_state_new(Alloc *a) {
    burrow__ExecState *st = (burrow__ExecState *)mem_alloc(a, sizeof(burrow__ExecState),
                                                           _Alignof(burrow__ExecState));
    if (st == NULL)
        return NULL;
    arena_init(&st->errs, heap_allocator(), 0);
    burrow__note_init_transient(&st->copiers_done);
    burrow__note_init_transient(&st->arrive);
    burrow__note_init_transient(&st->result_ready);
    st->cancel_r = PAL_INVALID_HANDLE;
    st->cancel_w = PAL_INVALID_HANDLE;
    return st;
}

/* The state of a Cmd somebody filled in by hand, as Go lets you, which gets
 * it the first time it needs it, from the heap when alloc was left NULL. */
static burrow__ExecState *cmd_state(ExecCmd *c) {
    if (c->st == NULL) {
        if (c->alloc == NULL)
            c->alloc = heap_allocator();
        c->st = cmd_state_new(c->alloc);
    }
    return c->st;
}

static ExecCmd *cmd_new(Alloc *a, Str name, Slice args) {
    ExecCmd *c = (ExecCmd *)mem_alloc(a, sizeof(ExecCmd), _Alignof(ExecCmd));
    if (c == NULL)
        return NULL;
    burrow__ExecState *st = cmd_state_new(a);
    if (st == NULL) {
        mem_free(a, c, sizeof(ExecCmd), _Alignof(ExecCmd));
        return NULL;
    }
    st->own_cmd = true;
    c->alloc = a;
    c->st = st;

    Slice argv = slice_make(a, TYPE_STRING, 0, args.len + 1);
    argv = BURROW_APPEND(Str, a, argv, name);
    for (Int i = 0; i < args.len; i++)
        argv = BURROW_APPEND(Str, a, argv, BURROW_AT(Str, args, i));
    st->own_args = argv;
    c->args = argv;
    c->path = name;

    if (str_eq(filepath_base(name), name)) {
        Error err = BURROW_NO_ERROR;
        Str lp = exec_look_path(a, name, &err);
        if (lp.len > 0)
            c->path = lp;
        if (lp.len > 0 && lp.p != name.p)
            c->look_out = lp; /* ours to free */
        if (BURROW_FAILED(err))
            c->err = err;
    } else {
#if defined(BURROW_OS_WINDOWS)
        if (filepath_is_abs(name)) {
            Error err = BURROW_NO_ERROR;
            Str lp = burrow__exec_look_extensions(a, name, BURROW_STR_EMPTY, &err);
            if (BURROW_OK(err)) {
                c->look_in = name;
                c->look_out = lp;
            } else {
                c->err = err;
            }
        }
#endif
    }
    return c;
}

ExecCmd *exec_command(Alloc *a, Str name, Slice args) {
    return cmd_new(a, name, args);
}

static Slice cmd_va_args(int n, va_list ap, Str *store) {
    for (int i = 0; i < n; i++)
        store[i] = va_arg(ap, Str);
    return slice_from(store, n, n, TYPE_STRING);
}

ExecCmd *exec_command_v(Alloc *a, Str name, int n, ...) {
    Str small[16];
    Str *store = small;
    if (n < 0)
        n = 0;
    if (n > 16) {
        store =
            (Str *)mem_alloc(heap_allocator(), sizeof(Str) * (size_t)n, _Alignof(Str));
        if (store == NULL)
            return NULL;
    }
    va_list ap;
    va_start(ap, n);
    Slice args = cmd_va_args(n, ap, store);
    va_end(ap);
    ExecCmd *c = cmd_new(a, name, args);
    if (store != small)
        mem_free(heap_allocator(), store, sizeof(Str) * (size_t)n, _Alignof(Str));
    return c;
}

static ExecCmd *cmd_with_context(ExecCmd *c, Context ctx) {
    if (c == NULL)
        return NULL;
    c->ctx = ctx;
    c->cancel = (ExecCancelFunc){cmd_default_cancel, c};
    return c;
}

ExecCmd *exec_command_context(Alloc *a, Context ctx, Str name, Slice args) {
    if (ctx.vt == NULL)
        panic_str(CMD_LIT("nil Context"));
    return cmd_with_context(cmd_new(a, name, args), ctx);
}

ExecCmd *exec_command_context_v(Alloc *a, Context ctx, Str name, int n, ...) {
    if (ctx.vt == NULL)
        panic_str(CMD_LIT("nil Context"));
    Str small[16];
    Str *store = small;
    if (n < 0)
        n = 0;
    if (n > 16) {
        store =
            (Str *)mem_alloc(heap_allocator(), sizeof(Str) * (size_t)n, _Alignof(Str));
        if (store == NULL)
            return NULL;
    }
    va_list ap;
    va_start(ap, n);
    Slice args = cmd_va_args(n, ap, store);
    va_end(ap);
    ExecCmd *c = cmd_with_context(cmd_new(a, name, args), ctx);
    if (store != small)
        mem_free(heap_allocator(), store, sizeof(Str) * (size_t)n, _Alignof(Str));
    return c;
}

/* ------------------------------------------------------------------- String */

static Slice cmd_argv(const ExecCmd *c, Str *one) {
    if (c->args.len > 0)
        return c->args;
    *one = c->path;
    return slice_from(one, 1, 1, TYPE_STRING);
}

Str exec_cmd_string(const ExecCmd *c, Alloc *a) {
    if (BURROW_FAILED(c->err))
        return strings_join(a, c->args, CMD_LIT(" "));
    Str one;
    Slice argv = cmd_argv(c, &one);
    Int n = c->path.len;
    for (Int i = 1; i < argv.len; i++)
        n += 1 + BURROW_AT(Str, argv, i).len;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Byte *q = cmd_put(p, c->path);
    for (Int i = 1; i < argv.len; i++) {
        *q++ = ' ';
        q = cmd_put(q, BURROW_AT(Str, argv, i));
    }
    return str_from_bytes(p, n);
}

/* ------------------------------------------------------------- environment */

/* dedupEnvCase: the last of each key wins, in the order the keys first came. */
static Slice cmd_dedup_env(Alloc *a, bool case_insensitive, Slice env, Error *err) {
    Slice out = slice_make(a, TYPE_STRING, 0, env.len);
    Slice keys = slice_make(a, TYPE_STRING, 0, env.len);
    for (Int n = env.len; n > 0; n--) {
        Str kv = BURROW_AT(Str, env, n - 1);
        if (strings_index_byte(kv, 0) != -1) {
            *err = cmd_errorf("exec: environment variable contains NUL");
            continue;
        }
        Int i = strings_index(kv, CMD_LIT("="));
        if (i == 0)
            i = strings_index(str_from_bytes(kv.p + 1, kv.len - 1), CMD_LIT("=")) + 1;
        if (i < 0) {
            if (kv.len > 0)
                out = BURROW_APPEND(Str, a, out, kv);
            continue;
        }
        Str k = str_from_bytes(kv.p, i);
        if (case_insensitive)
            k = strings_to_lower(a, k);
        bool saw = false;
        for (Int j = 0; j < keys.len && !saw; j++)
            saw = str_eq(BURROW_AT(Str, keys, j), k);
        if (saw)
            continue;
        keys = BURROW_APPEND(Str, a, keys, k);
        out = BURROW_APPEND(Str, a, out, kv);
    }
    for (Int i = 0; i < out.len / 2; i++) {
        Int j = out.len - i - 1;
        Str t = BURROW_AT(Str, out, i);
        BURROW_AT(Str, out, i) = BURROW_AT(Str, out, j);
        BURROW_AT(Str, out, j) = t;
    }
    return out;
}

static Slice cmd_environ(const ExecCmd *c, Alloc *a, Error *err) {
    *err = BURROW_NO_ERROR;
    Slice env = c->env;
    if (slice_is_nil(env)) {
        env = os_environ(a);
#if !defined(BURROW_OS_WINDOWS)
        if (c->dir.len > 0) {
            Error ae = BURROW_NO_ERROR;
            Str pwd = filepath_abs(a, c->dir, &ae);
            if (BURROW_OK(ae)) {
                Str kv = burrow__os_cat3(a, CMD_LIT("PWD="), pwd, BURROW_STR_EMPTY);
                env = BURROW_APPEND(Str, a, env, kv);
            } else {
                *err = ae;
            }
        }
#endif
    }
#if defined(BURROW_OS_WINDOWS)
    bool ci = true;
#else
    bool ci = false;
#endif
    Error de = BURROW_NO_ERROR;
    env = cmd_dedup_env(a, ci, env, &de);
    if (BURROW_OK(*err))
        *err = de;
#if defined(BURROW_OS_WINDOWS)
    /* addCriticalEnv: Windows programs fail in odd ways without SYSTEMROOT. */
    for (Int i = 0; i < env.len; i++) {
        Str v;
        bool ok = false;
        Str k = strings_cut(BURROW_AT(Str, env, i), CMD_LIT("="), &v, &ok);
        if (ok && strings_equal_fold(k, CMD_LIT("SYSTEMROOT")))
            return env;
    }
    Str kv = burrow__os_cat3(a, CMD_LIT("SYSTEMROOT="),
                             os_getenv(a, CMD_LIT("SYSTEMROOT")), BURROW_STR_EMPTY);
    env = BURROW_APPEND(Str, a, env, kv);
#endif
    return env;
}

Slice exec_cmd_environ(const ExecCmd *c, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    return cmd_environ(c, a, &err);
}

/* ---------------------------------------------------------------- copying */

static bool cmd_cancelled(burrow__ExecState *st) {
    return sync_atomic_uint32_load(&st->cancelled) != 0;
}

/* Waits until the copier can use its pipe. False when it has been told to
 * stop. On Windows the I/O itself is what gets cancelled, so this marks the
 * copier as inside it, and cmd_io_end takes the mark off. */
static bool cmd_io_begin(CmdCopier *cp, bool write) {
    burrow__ExecState *st = cp->st;
#if defined(BURROW_OS_WINDOWS)
    (void)write;
    sync_atomic_uint32_store(&cp->in_io, 1);
    if (cmd_cancelled(st)) {
        sync_atomic_uint32_store(&cp->in_io, 0);
        return false;
    }
    return true;
#else
    if (cmd_cancelled(st))
        return false;
    PalErrno pe = PAL_OK;
    return pal_fd_wait(cp->pipe->fd, write, st->cancel_r, &pe) != 0;
#endif
}

static void cmd_io_end(CmdCopier *cp) {
#if defined(BURROW_OS_WINDOWS)
    sync_atomic_uint32_store(&cp->in_io, 0);
#else
    (void)cp;
#endif
}

static bool cmd_is_eof(Error e) {
    return e.vt == io_eof.vt && e.data == io_eof.data;
}

/* The copy from the child's stdout or stderr into w, io.Copy(w, pr). */
static Error cmd_copy_out(CmdCopier *cp, Byte *buf) {
    Slice b = slice_from(buf, CMD_COPY_BUF, CMD_COPY_BUF, TYPE_BYTE);
    for (;;) {
        if (!cmd_io_begin(cp, false))
            return fs_path_error_new(error_allocator(), CMD_LIT("read"), cp->pipe->name,
                                     fs_err_closed);
        Error re = BURROW_NO_ERROR;
        Int n = os_file_read(cp->pipe, b, &re);
        cmd_io_end(cp);
        if (n > 0) {
            Error we = BURROW_NO_ERROR;
            Int nw = cp->w.vt->write(cp->w.data, slice_from(buf, n, n, TYPE_BYTE), &we);
            if (nw < 0 || n < nw) {
                nw = 0;
                if (BURROW_OK(we))
                    we = cmd_err_invalid_write;
            }
            if (BURROW_FAILED(we))
                return we;
            if (n != nw)
                return io_err_short_write;
        }
        if (BURROW_FAILED(re))
            return cmd_is_eof(re) ? BURROW_NO_ERROR : re;
    }
}

/* All of p into the stdin pipe, a piece at a time on Unix so that a write
 * never blocks where the copier cannot be woken. */
static Error cmd_write_pipe(CmdCopier *cp, const Byte *p, Int n) {
    Int off = 0;
    while (off < n) {
        if (!cmd_io_begin(cp, true))
            return fs_path_error_new(error_allocator(), CMD_LIT("write"),
                                     cp->pipe->name, fs_err_closed);
#if defined(BURROW_OS_WINDOWS)
        Int m = n - off;
#else
        Int m = n - off < PAL_PIPE_BUF ? n - off : PAL_PIPE_BUF;
#endif
        Error we = BURROW_NO_ERROR;
        Int w = os_file_write(
            cp->pipe, slice_from((void *)(uintptr_t)(p + off), m, m, TYPE_BYTE), &we);
        cmd_io_end(cp);
        if (w > 0)
            off += w;
        if (BURROW_FAILED(we))
            return we;
        if (w != m)
            return io_err_short_write;
    }
    return BURROW_NO_ERROR;
}

/* skipStdinCopyError: a child that exits without reading all of its input is
 * not an error, as long as it otherwise succeeded. */
static bool cmd_skip_stdin_copy_error(Error err) {
    if (BURROW_OK(err) || err.vt->self_type != TYPE_FS_PATH_ERROR)
        return false;
    const FsPathError *pe = (const FsPathError *)err.data;
    if (!str_eq(pe->op, CMD_LIT("write")) || !str_eq(pe->path, CMD_LIT("|1")))
        return false;
    if (BURROW_OK(pe->err) || pe->err.vt->self_type != TYPE_SYSCALL_ERRNO)
        return false;
    SyscallErrno e = *(const SyscallErrno *)pe->err.data;
#if defined(BURROW_OS_WINDOWS)
    /* ERROR_NO_DATA is 0xe8, which Go spells out because syscall does not. */
    return e == SYSCALL_ERROR_BROKEN_PIPE || e == (SyscallErrno)0xe8;
#else
    return e == SYSCALL_EPIPE;
#endif
}

/* The copy from the Cmd's stdin into the child's, io.Copy(pw, c.Stdin). */
static Error cmd_copy_in(CmdCopier *cp, Byte *buf) {
    Slice b = slice_from(buf, CMD_COPY_BUF, CMD_COPY_BUF, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    for (;;) {
        Error re = BURROW_NO_ERROR;
        Int n = cp->r.vt->read(cp->r.data, b, &re);
        if (n < 0 || n > CMD_COPY_BUF) {
            err = cmd_err_invalid_write;
            break;
        }
        if (n > 0) {
            Error we = cmd_write_pipe(cp, buf, n);
            if (BURROW_FAILED(we)) {
                err = we;
                break;
            }
        }
        if (BURROW_FAILED(re)) {
            if (!cmd_is_eof(re))
                err = re;
            break;
        }
    }
    return err;
}

static void cmd_copier_main(void *arg) {
    CmdCopier *cp = (CmdCopier *)arg;
    burrow__ExecState *st = cp->st;
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    Byte *buf = (Byte *)mem_alloc_nozero(heap_allocator(), (size_t)CMD_COPY_BUF, 1);
    if (cp->in) {
        /* A child that has gone is an EPIPE to report, not a reason for the
         * whole program to go. */
        pal_signal_mask(PAL_SIGPIPE, true, NULL);
        if (buf == NULL)
            err = burrow_err_out_of_memory;
        else
            err = cmd_copy_in(cp, buf);
        if (cmd_skip_stdin_copy_error(err))
            err = BURROW_NO_ERROR;
        Error ce = os_file_close(cp->pipe);
        if (BURROW_OK(err))
            err = ce;
    } else {
        if (buf == NULL)
            err = burrow_err_out_of_memory;
        else
            err = cmd_copy_out(cp, buf);
        (void)os_file_close(cp->pipe); /* in case the copy stopped on a write error */
    }
    if (buf != NULL)
        mem_free(heap_allocator(), buf, (size_t)CMD_COPY_BUF, 1);

    burrow__lock(&st->mu);
    if (BURROW_OK(st->first_err))
        st->first_err = cmd_keep(st, err);
    bool last = --st->running == 0;
    burrow__unlock(&st->mu);
    if (last)
        burrow__note_wake(&st->copiers_done);
    error_release(m);
    burrow__error_thread_exit();
}

/* Tells the copiers to stop and closes the pipes the caller was given, which
 * is Go's closeDescriptors(c.parentIOPipes) when WaitDelay runs out. On
 * Windows it keeps cancelling until the copiers are done, since a cancel only
 * reaches a ReadFile or WriteFile that is already running. */
static void cmd_stop_io(burrow__ExecState *st) {
    sync_atomic_uint32_store(&st->cancelled, 1);
    burrow__lock(&st->mu);
    for (int i = 0; i < st->n_parent_io; i++) {
        if (!st->parent_copier[i] && !st->parent_closed[i]) {
            st->parent_closed[i] = true;
            (void)burrow__os_file_close_nowait(st->parent_io[i]);
        }
    }
    burrow__unlock(&st->mu);
#if defined(BURROW_OS_WINDOWS)
    for (;;) {
        for (int i = 0; i < st->n_cp; i++) {
            CmdCopier *cp = &st->cp[i];
            if (cp->running && sync_atomic_uint32_load(&cp->in_io) != 0)
                (void)pal_thread_cancel_io(cp->t.handle, NULL);
        }
        if (burrow__note_sleep_timeout(&st->copiers_done, 5 * 1000 * 1000))
            break;
    }
#else
    if (st->cancel_w != PAL_INVALID_HANDLE) {
        /* Closing the write end makes the read end readable for good. */
        burrow__lock(&st->mu);
        int64_t w = st->cancel_w;
        st->cancel_w = PAL_INVALID_HANDLE;
        burrow__unlock(&st->mu);
        if (w != PAL_INVALID_HANDLE)
            (void)pal_close(w, NULL);
    }
#endif
}

static void cmd_join_copiers(burrow__ExecState *st) {
    for (int i = 0; i < st->n_cp; i++) {
        if (st->cp[i].running) {
            burrow__thread_join(&st->cp[i].t);
            st->cp[i].running = false;
        }
    }
}

/* ------------------------------------------------------------- the watch */

static void cmd_watch_main(void *arg) {
    ExecCmd *c = (ExecCmd *)arg;
    burrow__ExecState *st = c->st;
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    bool timer = false;
    int64_t deadline = 0;

    SelectCase cases[2] = {BURROW_RECV(st->stop, NULL),
                           BURROW_RECV(context_done(c->ctx), NULL)};
    if (chan_select(cases, 2) == 0)
        goto deliver;

    if (c->cancel.f != NULL) {
        Error ie = c->cancel.f(c->cancel.env);
        if (BURROW_OK(ie)) {
            err = context_err(c->ctx);
        } else if (errors_is(ie, os_err_process_done)) {
            /* The process already finished: nothing to do. */
        } else {
            err = cmd_wrap(error_allocator(), CMD_LIT("exec: canceling Cmd"), ie);
        }
    }
    if (c->wait_delay == 0)
        goto deliver;

    deadline = burrow__nanotime() + c->wait_delay;
    if (burrow__note_sleep_timeout(&st->arrive, c->wait_delay)) {
        timer = true;
        goto deliver;
    }

    /* The process has had WaitDelay to exit since the cancel. */
    bool killed = false;
    Error ke = os_process_kill(c->process);
    if (BURROW_OK(ke)) {
        killed = true;
    } else if (!errors_is(ke, os_err_process_done)) {
        err = cmd_wrap(error_allocator(), CMD_LIT("exec: killing Cmd"), ke);
    }

    if (st->n_cp > 0 && !st->consumed) {
        if (burrow__note_is_open(&st->copiers_done)) {
            burrow__lock(&st->mu);
            Error ge = st->first_err;
            burrow__unlock(&st->mu);
            if (BURROW_OK(err) && !killed)
                err = ge;
        } else {
            cmd_stop_io(st);
            burrow__note_sleep(&st->copiers_done);
            if (BURROW_OK(err))
                err = exec_err_wait_delay;
        }
        st->consumed = true;
    }

deliver:
    burrow__lock(&st->mu);
    st->res_err = cmd_keep(st, err);
    st->res_timer = timer;
    st->res_deadline = deadline;
    burrow__unlock(&st->mu);
    burrow__note_wake(&st->result_ready);
    error_release(m);
    burrow__error_thread_exit();
}

/* ------------------------------------------------------------------- Start */

static bool cmd_same(IoWriter x, IoWriter y) {
    if (x.data != y.data)
        return false;
    if (x.vt == y.vt)
        return true;
    return x.vt->self_type != NULL && x.vt->self_type == y.vt->self_type;
}

static OsFile *cmd_as_file(const Type *self_type, void *data) {
    return self_type == TYPE_OS_FILE ? (OsFile *)data : NULL;
}

static CmdCopier *cmd_add_copier(burrow__ExecState *st) {
    CmdCopier *cp = &st->cp[st->n_cp++];
    memset(cp, 0, sizeof(*cp));
    cp->st = st;
    return cp;
}

static OsFile *cmd_child_stdin(ExecCmd *c, Error *err) {
    burrow__ExecState *st = c->st;
    if (c->stdin_.vt == NULL) {
        OsFile *f = os_open(c->alloc, CMD_LIT(OS_DEV_NULL), err);
        if (f == NULL)
            return NULL;
        cmd_add_made(st, f);
        cmd_add_child(st, f);
        return f;
    }
    OsFile *f = cmd_as_file(c->stdin_.vt->self_type, c->stdin_.data);
    if (f != NULL)
        return f;
    OsFile *pw = NULL;
    OsFile *pr = os_pipe(c->alloc, &pw, err);
    if (pr == NULL)
        return NULL;
    cmd_add_made(st, pr);
    cmd_add_made(st, pw);
    cmd_add_child(st, pr);
    cmd_add_parent(st, pw, true);
    CmdCopier *cp = cmd_add_copier(st);
    cp->in = true;
    cp->r = c->stdin_;
    cp->pipe = pw;
    return pr;
}

static OsFile *cmd_writer_descriptor(ExecCmd *c, IoWriter w, Error *err) {
    burrow__ExecState *st = c->st;
    if (w.vt == NULL) {
        OsFile *f = os_open_file(c->alloc, CMD_LIT(OS_DEV_NULL), OS_O_WRONLY, 0, err);
        if (f == NULL)
            return NULL;
        cmd_add_made(st, f);
        cmd_add_child(st, f);
        return f;
    }
    OsFile *f = cmd_as_file(w.vt->self_type, w.data);
    if (f != NULL)
        return f;
    OsFile *pw = NULL;
    OsFile *pr = os_pipe(c->alloc, &pw, err);
    if (pr == NULL)
        return NULL;
    cmd_add_made(st, pr);
    cmd_add_made(st, pw);
    cmd_add_child(st, pw);
    cmd_add_parent(st, pr, true);
    CmdCopier *cp = cmd_add_copier(st);
    cp->w = w;
    cp->pipe = pr;
    return pw;
}

static void cmd_close_children(burrow__ExecState *st) {
    for (int i = 0; i < st->n_child_io; i++)
        (void)os_file_close(st->child_io[i]);
    st->n_child_io = 0;
}

static void cmd_close_parents(burrow__ExecState *st) {
    burrow__lock(&st->mu);
    for (int i = 0; i < st->n_parent_io; i++) {
        if (!st->parent_closed[i]) {
            st->parent_closed[i] = true;
            (void)burrow__os_file_close_nowait(st->parent_io[i]);
        }
    }
    st->n_parent_io = 0;
    burrow__unlock(&st->mu);
}

static Error cmd_start(ExecCmd *c, Arena *scratch, bool *started) {
    burrow__ExecState *st = c->st;
    Alloc *s = arena_allocator(scratch);
    if (c->path.len == 0 && BURROW_OK(c->err))
        c->err = cmd_errorf("exec: no command");
    if (BURROW_FAILED(c->err))
        return c->err;
    Str lp = c->path;
#if defined(BURROW_OS_WINDOWS)
    if (c->look_in.p != NULL && str_eq(c->path, c->look_in)) {
        lp = c->look_out;
    } else {
        Error le = BURROW_NO_ERROR;
        lp = burrow__exec_look_extensions(s, c->path, c->dir, &le);
        if (BURROW_FAILED(le))
            return le;
    }
#endif
    if (c->cancel.f != NULL && c->ctx.vt == NULL)
        return cmd_errorf(
            "exec: command with a non-nil Cancel was not created with CommandContext");
    if (c->ctx.vt != NULL) {
        Chan *done = context_done(c->ctx);
        if (done != NULL) {
            SelectCase cases[2] = {BURROW_RECV(done, NULL), BURROW_DEFAULT};
            if (chan_select(cases, 2) == 0)
                return context_err(c->ctx);
        }
    }

    Error err = BURROW_NO_ERROR;
    Slice files = slice_make(s, TYPE_UINTPTR, 0, 3 + c->extra_files.len);
    OsFile *in = cmd_child_stdin(c, &err);
    if (in == NULL)
        return err;
    files = BURROW_APPEND(OsFile *, s, files, in);
    OsFile *out = cmd_writer_descriptor(c, c->stdout_, &err);
    if (out == NULL)
        return err;
    files = BURROW_APPEND(OsFile *, s, files, out);
    OsFile *errf = out;
    if (!(c->stderr_.vt != NULL && c->stdout_.vt != NULL &&
          cmd_same(c->stderr_, c->stdout_))) {
        errf = cmd_writer_descriptor(c, c->stderr_, &err);
        if (errf == NULL)
            return err;
    }
    files = BURROW_APPEND(OsFile *, s, files, errf);
    for (Int i = 0; i < c->extra_files.len; i++)
        files =
            BURROW_APPEND(OsFile *, s, files, BURROW_AT(OsFile *, c->extra_files, i));

    Slice env = cmd_environ(c, s, &err);
    if (BURROW_FAILED(err))
        return err;

    Str one;
    OsProcAttr attr = {c->dir, env, files, c->sys_proc_attr};
    /* From the heap rather than c->alloc: Wait makes the ProcessState while the
     * copiers may be allocating from an arena the caller handed them. */
    c->process = os_start_process(heap_allocator(), lp, cmd_argv(c, &one), &attr, &err);
    if (c->process == NULL)
        return BURROW_FAILED(err) ? err : burrow_err_out_of_memory;
    *started = true;

    if (st->n_cp > 0) {
#if !defined(BURROW_OS_WINDOWS)
        int64_t p[2];
        if (pal_pipe(p, 0, NULL)) {
            st->cancel_r = p[0];
            st->cancel_w = p[1];
        }
#endif
        st->running = st->n_cp;
        for (int i = 0; i < st->n_cp; i++) {
            CmdCopier *cp = &st->cp[i];
            if (burrow__thread_start(&cp->t, cmd_copier_main, cp, 0)) {
                cp->running = true;
                continue;
            }
            /* No thread to copy with. The pipe is closed so the child sees the
             * end of its input or a broken pipe, and Wait reports this. */
            (void)os_file_close(cp->pipe);
            burrow__lock(&st->mu);
            if (BURROW_OK(st->first_err))
                st->first_err = cmd_keep(
                    st, cmd_errorf("exec: could not start a thread to copy with"));
            bool last = --st->running == 0;
            burrow__unlock(&st->mu);
            if (last)
                burrow__note_wake(&st->copiers_done);
        }
    }

    if ((c->cancel.f != NULL || c->wait_delay != 0) && c->ctx.vt != NULL &&
        context_done(c->ctx) != NULL) {
        st->stop = chan_make(heap_allocator(), TYPE_UINT8, 0);
        if (st->stop != NULL &&
            burrow__thread_start(&st->watch, cmd_watch_main, c, 0)) {
            st->watching = true;
        } else {
            /* Nothing would cancel the process when the context is done, so
             * it is not left to run unsupervised. */
            (void)os_process_kill(c->process);
            st->late_err =
                cmd_errorf("exec: could not start a thread to watch the context");
        }
    }
    return BURROW_NO_ERROR;
}

Error exec_cmd_start(ExecCmd *c) {
    if (c->start_called)
        return cmd_errorf("exec: already started");
    c->start_called = true;
    burrow__ExecState *st = cmd_state(c);
    if (st == NULL)
        return burrow_err_out_of_memory;
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    bool started = false;
    Error err = cmd_start(c, &scratch, &started);
    cmd_close_children(st);
    if (!started) {
        cmd_close_parents(st);
        st->n_cp = 0;
    }
    arena_free(&scratch);
    return err;
}

/* -------------------------------------------------------------------- Wait */

static Error cmd_await_copiers(ExecCmd *c, bool has_timer, int64_t deadline) {
    burrow__ExecState *st = c->st;
    if (st->n_cp == 0 || st->consumed)
        return BURROW_NO_ERROR;
    st->consumed = true;
    if (!has_timer) {
        if (c->wait_delay == 0) {
            burrow__note_sleep(&st->copiers_done);
            return st->first_err;
        }
        if (burrow__note_is_open(&st->copiers_done))
            return st->first_err;
        deadline = burrow__nanotime() + c->wait_delay;
    }
    int64_t left = deadline - burrow__nanotime();
    if (burrow__note_sleep_timeout(&st->copiers_done, left))
        return st->first_err;
    cmd_stop_io(st);
    burrow__note_sleep(&st->copiers_done);
    return exec_err_wait_delay;
}

Error exec_cmd_wait(ExecCmd *c) {
    if (c->process == NULL)
        return cmd_errorf("exec: not started");
    if (c->process_state != NULL)
        return cmd_errorf("exec: Wait was already called");
    burrow__ExecState *st = c->st;

    Error err = BURROW_NO_ERROR;
    OsProcessState *state = os_process_wait(c->process, &err);
    if (BURROW_OK(err) && state != NULL && !os_process_state_success(state))
        err = exec_exit_error_new(error_allocator(), state, slice_nil(TYPE_BYTE));
    c->process_state = state;

    bool has_timer = false;
    int64_t deadline = 0;
    if (st->watching) {
        chan_close(st->stop);
        burrow__note_wake(&st->arrive);
        burrow__note_sleep(&st->result_ready);
        burrow__thread_join(&st->watch);
        st->watching = false;
        has_timer = st->res_timer;
        deadline = st->res_deadline;
        if (BURROW_OK(err) && BURROW_FAILED(st->res_err))
            err = error_retain(error_allocator(), st->res_err);
    }
    if (BURROW_OK(err) && BURROW_FAILED(st->late_err))
        err = st->late_err;

    Error ge = cmd_await_copiers(c, has_timer, deadline);
    cmd_join_copiers(st);
    if (BURROW_OK(err) && BURROW_FAILED(ge))
        err = error_retain(error_allocator(), ge);
    cmd_close_parents(st);
    return err;
}

Error exec_cmd_run(ExecCmd *c) {
    Error err = exec_cmd_start(c);
    if (BURROW_FAILED(err))
        return err;
    return exec_cmd_wait(c);
}

/* ------------------------------------------------------- prefixSuffixSaver */

/* The start and the end of what a program wrote to its standard error, at
 * most n bytes of each. */
typedef struct CmdSaver {
    Int n;
    Byte *prefix;
    Int prefix_len;
    Byte *suffix; /* a ring once it is full */
    Int suffix_len;
    Int suffix_off;
    int64_t skipped;
} CmdSaver;

static Int cmd_saver_fill(CmdSaver *w, Byte *dst, Int *len, const Byte **p, Int *plen) {
    Int remain = w->n - *len;
    if (remain > 0) {
        Int add = *plen < remain ? *plen : remain;
        memcpy(dst + *len, *p, (size_t)add);
        *len += add;
        *p += add;
        *plen -= add;
    }
    return *plen;
}

static Int cmd_saver_write(void *self, Slice s, Error *err) {
    CmdSaver *w = (CmdSaver *)self;
    *err = BURROW_NO_ERROR;
    Int lenp = s.len;
    const Byte *p = (const Byte *)s.p;
    Int plen = s.len;
    if (plen == 0)
        return 0;
    if (w->prefix == NULL) {
        w->prefix = (Byte *)mem_alloc_nozero(heap_allocator(), (size_t)w->n, 1);
        w->suffix = (Byte *)mem_alloc_nozero(heap_allocator(), (size_t)w->n, 1);
        if (w->prefix == NULL || w->suffix == NULL) {
            *err = burrow_err_out_of_memory;
            return 0;
        }
    }
    cmd_saver_fill(w, w->prefix, &w->prefix_len, &p, &plen);
    Int overage = plen - w->n;
    if (overage > 0) {
        p += overage;
        plen -= overage;
        w->skipped += overage;
    }
    cmd_saver_fill(w, w->suffix, &w->suffix_len, &p, &plen);
    while (plen > 0) {
        Int room = w->n - w->suffix_off;
        Int n = plen < room ? plen : room;
        memcpy(w->suffix + w->suffix_off, p, (size_t)n);
        p += n;
        plen -= n;
        w->skipped += n;
        w->suffix_off += n;
        if (w->suffix_off == w->n)
            w->suffix_off = 0;
    }
    return lenp;
}

static const IoWriterVT cmd_saver_vt = {NULL, cmd_saver_write};

/* Bytes: prefix, then a note of how much was left out, then the suffix in
 * order. From the heap, or nil when nothing was written. */
static Slice cmd_saver_bytes(CmdSaver *w) {
    if (w->prefix == NULL)
        return slice_nil(TYPE_BYTE);
    Byte note[64];
    Int nlen = 0;
    if (w->skipped > 0) {
        Str k = strconv_format_int(heap_allocator(), w->skipped, 10);
        Str parts[3] = {CMD_LIT("\n... omitting "), k, CMD_LIT(" bytes ...\n")};
        for (int i = 0; i < 3; i++) {
            memcpy(note + nlen, parts[i].p, (size_t)parts[i].len);
            nlen += parts[i].len;
        }
        mem_free(heap_allocator(), (void *)(uintptr_t)k.p, (size_t)k.len, 1);
    }
    Int total = w->prefix_len + nlen + w->suffix_len;
    Slice out = slice_make(heap_allocator(), TYPE_BYTE, total, total);
    Byte *q = (Byte *)out.p;
    if (q == NULL)
        return slice_nil(TYPE_BYTE);
    memcpy(q, w->prefix, (size_t)w->prefix_len);
    q += w->prefix_len;
    if (w->skipped == 0) {
        memcpy(q, w->suffix, (size_t)w->suffix_len);
        return out;
    }
    memcpy(q, note, (size_t)nlen);
    q += nlen;
    memcpy(q, w->suffix + w->suffix_off, (size_t)(w->suffix_len - w->suffix_off));
    q += w->suffix_len - w->suffix_off;
    memcpy(q, w->suffix, (size_t)w->suffix_off);
    return out;
}

static void cmd_saver_free(CmdSaver *w) {
    if (w->prefix != NULL)
        mem_free(heap_allocator(), w->prefix, (size_t)w->n, 1);
    if (w->suffix != NULL)
        mem_free(heap_allocator(), w->suffix, (size_t)w->n, 1);
}

/* ----------------------------------------------------------------- Output */

/* What b holds, copied into a, and b freed. Nil when nothing was written, as
 * bytes.Buffer.Bytes is. */
static Slice cmd_take(Alloc *a, BytesBuffer *b) {
    Slice in = bytes_buffer_bytes(b);
    Slice out = slice_nil(TYPE_BYTE);
    if (in.p != NULL) {
        out = slice_make(a, TYPE_BYTE, in.len, in.len);
        if (in.len > 0 && out.p != NULL)
            memcpy(out.p, in.p, (size_t)in.len);
    }
    bytes_buffer_free(b);
    return out;
}

Slice exec_cmd_output(ExecCmd *c, Alloc *a, Error *err) {
    if (c->stdout_.vt != NULL) {
        *err = cmd_errorf("exec: Stdout already set");
        return slice_nil(TYPE_BYTE);
    }
    /* The copier fills the buffer on its own thread, so it is on the heap and
     * goes into a once Wait is done. */
    BytesBuffer out = BYTES_BUFFER(heap_allocator());
    c->stdout_ = bytes_buffer_as_io_writer(&out);

    CmdSaver saver = {32 << 10, NULL, 0, NULL, 0, 0, 0};
    bool capture = c->stderr_.vt == NULL;
    if (capture)
        c->stderr_ = (IoWriter){&cmd_saver_vt, &saver};

    *err = exec_cmd_run(c);
    if (BURROW_FAILED(*err) && capture && err->vt == &exec_exit_error_vt) {
        const ExecExitError *ee = (const ExecExitError *)err->data;
        Slice b = cmd_saver_bytes(&saver);
        *err = exec_exit_error_new(error_allocator(), ee->process_state, b);
        if (!slice_is_nil(b))
            mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
    }
    if (capture) {
        cmd_saver_free(&saver);
        c->stderr_ = (IoWriter){NULL, NULL};
    }
    c->stdout_ = (IoWriter){NULL, NULL};
    return cmd_take(a, &out);
}

Slice exec_cmd_combined_output(ExecCmd *c, Alloc *a, Error *err) {
    if (c->stdout_.vt != NULL) {
        *err = cmd_errorf("exec: Stdout already set");
        return slice_nil(TYPE_BYTE);
    }
    if (c->stderr_.vt != NULL) {
        *err = cmd_errorf("exec: Stderr already set");
        return slice_nil(TYPE_BYTE);
    }
    BytesBuffer b = BYTES_BUFFER(heap_allocator());
    c->stdout_ = bytes_buffer_as_io_writer(&b);
    c->stderr_ = c->stdout_;
    *err = exec_cmd_run(c);
    c->stdout_ = (IoWriter){NULL, NULL};
    c->stderr_ = (IoWriter){NULL, NULL};
    return cmd_take(a, &b);
}

/* ------------------------------------------------------------------ pipes */

IoWriteCloser exec_cmd_stdin_pipe(ExecCmd *c, Error *err) {
    *err = BURROW_NO_ERROR;
    if (c->stdin_.vt != NULL) {
        *err = cmd_errorf("exec: Stdin already set");
        return (IoWriteCloser){NULL, NULL};
    }
    if (c->process != NULL) {
        *err = cmd_errorf("exec: StdinPipe after process started");
        return (IoWriteCloser){NULL, NULL};
    }
    burrow__ExecState *st = cmd_state(c);
    if (st == NULL) {
        *err = burrow_err_out_of_memory;
        return (IoWriteCloser){NULL, NULL};
    }
    OsFile *pw = NULL;
    OsFile *pr = os_pipe(c->alloc, &pw, err);
    if (pr == NULL)
        return (IoWriteCloser){NULL, NULL};
    cmd_add_made(st, pr);
    cmd_add_made(st, pw);
    c->stdin_ = os_file_as_io_reader(pr);
    cmd_add_child(st, pr);
    cmd_add_parent(st, pw, false);
    return os_file_as_io_write_closer(pw);
}

static IoReadCloser cmd_output_pipe(ExecCmd *c, IoWriter *slot, const char *set,
                                    const char *after, Error *err) {
    *err = BURROW_NO_ERROR;
    if (slot->vt != NULL) {
        *err = cmd_errorf(set);
        return (IoReadCloser){NULL, NULL};
    }
    if (c->process != NULL) {
        *err = cmd_errorf(after);
        return (IoReadCloser){NULL, NULL};
    }
    burrow__ExecState *st = cmd_state(c);
    if (st == NULL) {
        *err = burrow_err_out_of_memory;
        return (IoReadCloser){NULL, NULL};
    }
    OsFile *pw = NULL;
    OsFile *pr = os_pipe(c->alloc, &pw, err);
    if (pr == NULL)
        return (IoReadCloser){NULL, NULL};
    cmd_add_made(st, pr);
    cmd_add_made(st, pw);
    *slot = os_file_as_io_writer(pw);
    cmd_add_child(st, pw);
    cmd_add_parent(st, pr, false);
    return os_file_as_io_read_closer(pr);
}

IoReadCloser exec_cmd_stdout_pipe(ExecCmd *c, Error *err) {
    return cmd_output_pipe(c, &c->stdout_, "exec: Stdout already set",
                           "exec: StdoutPipe after process started", err);
}

IoReadCloser exec_cmd_stderr_pipe(ExecCmd *c, Error *err) {
    return cmd_output_pipe(c, &c->stderr_, "exec: Stderr already set",
                           "exec: StderrPipe after process started", err);
}

/* -------------------------------------------------------------------- free */

void exec_cmd_free(ExecCmd *c) {
    if (c == NULL)
        return;
    burrow__ExecState *st = c->st;
    Alloc *a = c->alloc;
    if (st == NULL)
        return; /* filled in by hand and never started */
    if (c->process != NULL && c->process_state == NULL) {
        /* Started and never waited for, which Go would call a leak. Rather
         * than leave threads blocked on it, the program is killed and
         * reaped. */
        (void)os_process_kill(c->process);
        ArenaMark m = error_mark();
        (void)exec_cmd_wait(c);
        error_release(m);
    }
    if (st->watching) {
        chan_close(st->stop);
        burrow__note_wake(&st->arrive);
        burrow__thread_join(&st->watch);
        st->watching = false;
    }
    cmd_join_copiers(st);
    if (st->stop != NULL)
        chan_free(st->stop);
    if (st->cancel_r != PAL_INVALID_HANDLE)
        (void)pal_close(st->cancel_r, NULL);
    if (st->cancel_w != PAL_INVALID_HANDLE)
        (void)pal_close(st->cancel_w, NULL);
    for (int i = 0; i < st->n_made; i++)
        os_file_free(st->made[i]);
    burrow__note_free(&st->copiers_done);
    burrow__note_free(&st->arrive);
    burrow__note_free(&st->result_ready);
    arena_free(&st->errs);
    if (c->process_state != NULL)
        os_process_state_free(c->process_state);
    if (c->process != NULL)
        os_process_free(c->process);
    if (st->own_args.p != NULL)
        mem_free(a, st->own_args.p, (size_t)st->own_args.cap * sizeof(Str),
                 _Alignof(Str));
    if (c->look_out.p != NULL)
        mem_free(a, (void *)(uintptr_t)c->look_out.p, (size_t)c->look_out.len, 1);
    bool made = st->own_cmd;
    mem_free(a, st, sizeof(burrow__ExecState), _Alignof(burrow__ExecState));
    c->st = NULL;
    c->process = NULL;
    c->process_state = NULL;
    if (made)
        mem_free(a, c, sizeof(ExecCmd), _Alignof(ExecCmd));
}
