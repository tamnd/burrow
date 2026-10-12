/* log, after Go's src/log/log.go.
 *
 * One line per call, built in an arena that lives for the call and written with
 * one Write under the logger's out lock. Go keeps a pool of buffers for the
 * same job. The arena asks the heap for one chunk the first time it is used and
 * gives it back at the end, which is the cost a pool would save, and a logger
 * that writes to io_discard never gets that far.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/log.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>

/* Go's std, what log.Default returns. Zero apart from the flags, so it needs no
 * constructor, and an output that was never set means standard error. */
static LogLogger log_std = {.flag = LOG_LSTD_FLAGS};

static LogLogger *log_or_std(LogLogger *l) {
    return l != NULL ? l : &log_std;
}

LogLogger *log_new(Alloc *a, IoWriter out, Str prefix, Int flag) {
    LogLogger *l = mem_alloc(a, sizeof *l, alignof(LogLogger));
    if (l == NULL)
        return NULL;
    *l = (LogLogger){0};
    log_logger_set_output(l, out);
    log_logger_set_prefix(l, prefix);
    log_logger_set_flags(l, flag);
    return l;
}

void log_logger_free(Alloc *a, LogLogger *l) {
    if (l != NULL)
        mem_free(a, l, sizeof *l, alignof(LogLogger));
}

LogLogger *log_default(void) {
    return &log_std;
}

void log_logger_set_output(LogLogger *l, IoWriter w) {
    sync_mutex_lock(&l->out_mu);
    l->out = w;
    burrow__atomic_store_u32(&l->is_discard, w.vt != NULL && w.vt == io_discard.vt);
    sync_mutex_unlock(&l->out_mu);
}

/* The writer to use, standard error for a logger that never had one. Called
 * with out_mu held. */
static IoWriter log_out(const LogLogger *l) {
    if (l->out.vt == NULL)
        return os_file_as_io_writer(os_stderr);
    return l->out;
}

IoWriter log_logger_writer(LogLogger *l) {
    sync_mutex_lock(&l->out_mu);
    IoWriter w = log_out(l);
    sync_mutex_unlock(&l->out_mu);
    return w;
}

Int log_logger_flags(LogLogger *l) {
    return (Int)burrow__atomic_load_u32(&l->flag);
}

void log_logger_set_flags(LogLogger *l, Int flag) {
    burrow__atomic_store_u32(&l->flag, (uint32_t)flag);
}

Str log_logger_prefix(LogLogger *l) {
    sync_mutex_lock(&l->prefix_mu);
    Str p = l->prefix;
    sync_mutex_unlock(&l->prefix_mu);
    return p;
}

void log_logger_set_prefix(LogLogger *l, Str prefix) {
    sync_mutex_lock(&l->prefix_mu);
    l->prefix = prefix;
    sync_mutex_unlock(&l->prefix_mu);
}

/* ------------------------------------------------------------------ output */

static Slice log_put(Alloc *a, Slice buf, Str s) {
    return slice_append(a, buf, s.p, s.len);
}

static Slice log_putc(Alloc *a, Slice buf, char c) {
    return slice_append(a, buf, &c, 1);
}

/* Go's itoa: i in decimal, padded with zeros to wid digits. A negative width
 * means no padding. */
static Slice log_itoa(Alloc *a, Slice buf, Int i, int wid) {
    Byte b[20];
    int bp = (int)sizeof b - 1;
    while (i >= 10 || wid > 1) {
        wid--;
        Int q = i / 10;
        b[bp--] = (Byte)('0' + (i - q * 10));
        i = q;
    }
    b[bp] = (Byte)('0' + i);
    return slice_append(a, buf, &b[bp], (Int)sizeof b - bp);
}

/* Go's formatHeader. */
static Slice log_header(Alloc *a, Slice buf, Time t, Str prefix, Int flag, Str file,
                        Int line) {
    if ((flag & LOG_LMSGPREFIX) == 0)
        buf = log_put(a, buf, prefix);
    if ((flag & (LOG_LDATE | LOG_LTIME | LOG_LMICROSECONDS)) != 0) {
        if ((flag & LOG_LUTC) != 0)
            t = time_utc(t);
        if ((flag & LOG_LDATE) != 0) {
            TimeDateRet d = time_date_of(t);
            buf = log_itoa(a, buf, d.year, 4);
            buf = log_putc(a, buf, '/');
            buf = log_itoa(a, buf, (Int)d.month, 2);
            buf = log_putc(a, buf, '/');
            buf = log_itoa(a, buf, d.day, 2);
            buf = log_putc(a, buf, ' ');
        }
        if ((flag & (LOG_LTIME | LOG_LMICROSECONDS)) != 0) {
            TimeClockRet c = time_clock(t);
            buf = log_itoa(a, buf, c.hour, 2);
            buf = log_putc(a, buf, ':');
            buf = log_itoa(a, buf, c.min, 2);
            buf = log_putc(a, buf, ':');
            buf = log_itoa(a, buf, c.sec, 2);
            if ((flag & LOG_LMICROSECONDS) != 0) {
                buf = log_putc(a, buf, '.');
                buf = log_itoa(a, buf, time_nanosecond(t) / 1000, 6);
            }
            buf = log_putc(a, buf, ' ');
        }
    }
    if ((flag & (LOG_LSHORTFILE | LOG_LLONGFILE)) != 0) {
        if ((flag & LOG_LSHORTFILE) != 0) {
            /* Go looks for '/' only. __FILE__ on Windows can have either. */
            for (Int i = file.len - 1; i > 0; i--) {
                Byte ch = file.p[i];
#ifdef _WIN32
                bool sep = ch == '/' || ch == '\\';
#else
                bool sep = ch == '/';
#endif
                if (sep) {
                    file = (Str){file.p + i + 1, file.len - i - 1};
                    break;
                }
            }
        }
        buf = log_put(a, buf, file);
        buf = log_putc(a, buf, ':');
        buf = log_itoa(a, buf, line, -1);
        buf = log_put(a, buf, BURROW_S(": "));
    }
    if ((flag & LOG_LMSGPREFIX) != 0)
        buf = log_put(a, buf, prefix);
    return buf;
}

/* Where a line came from. have is false when the caller did not say, and then
 * runtime_caller is asked, but only when the flags print it. */
typedef struct LogSite {
    Str file;
    Int line;
    Int depth; /* for runtime_caller, counted from log_emit's caller */
    bool have;
} LogSite;

/* What goes after the header: s as it is, or v formatted as how says. */
typedef struct LogMsg {
    Str format;
    Slice v;
    Str s;
    int how; /* BURROW__LOG_PRINT and the rest, or -1 for s */
} LogMsg;

/* Go's Logger.output. The site lookup is done here and not in a helper so that
 * the depth counts the frames it says it does. */
BURROW_NOINLINE static Error log_emit(LogLogger *l, LogSite site, LogMsg m) {
    if (burrow__atomic_load_u32(&l->is_discard) != 0)
        return BURROW_NO_ERROR;

    Time now = time_now(); /* as early as Go takes it */
    Str prefix = log_logger_prefix(l);
    Int flag = log_logger_flags(l);

    if ((flag & (LOG_LSHORTFILE | LOG_LLONGFILE)) != 0 && !site.have) {
        Uintptr pc = 0;
        if (!runtime_caller(site.depth + 1, &pc, &site.file, &site.line) ||
            site.file.len == 0) {
            site.file = BURROW_S("???");
            site.line = 0;
        }
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Slice buf = slice_nil(TYPE_BYTE);
    buf = log_header(a, buf, now, prefix, flag, site.file, site.line);
    switch (m.how) {
    case BURROW__LOG_PRINT:
        buf = fmt_append(a, buf, m.v);
        break;
    case BURROW__LOG_PRINTF:
        buf = fmt_appendf(a, buf, m.format, m.v);
        break;
    case BURROW__LOG_PRINTLN:
        buf = fmt_appendln(a, buf, m.v);
        break;
    default:
        buf = log_put(a, buf, m.s);
        break;
    }
    if (buf.len == 0 || ((const Byte *)buf.p)[buf.len - 1] != '\n')
        buf = log_putc(a, buf, '\n');

    Error err = BURROW_NO_ERROR;
    sync_mutex_lock(&l->out_mu);
    IoWriter w = log_out(l);
    w.vt->write(w.data, buf, &err);
    sync_mutex_unlock(&l->out_mu);

    arena_free(&ar);
    return err;
}

static LogSite log_site_at(const char *file, int line) {
    LogSite s = {0};
    s.have = true;
    s.file = file != NULL ? str_from_cstr(file) : BURROW_S("???");
    s.line = file != NULL ? line : 0;
    return s;
}

static LogSite log_site_up(Int depth) {
    LogSite s = {0};
    s.depth = depth;
    return s;
}

static LogMsg log_msg(int how, Str format, Slice v) {
    LogMsg m = {0};
    m.how = how;
    m.format = format;
    m.v = v;
    return m;
}

static LogMsg log_msg_str(Str s) {
    LogMsg m = {0};
    m.how = -1;
    m.s = s;
    return m;
}

/* The message for Panic, from the goroutine's error arena, since it outlives
 * the call. That is where the library's other formatted panics come from. */
static Str log_sprint(int how, Str format, Slice v) {
    Alloc *a = error_allocator();
    switch (how) {
    case BURROW__LOG_PRINTF:
        return fmt_sprintf(a, format, v);
    case BURROW__LOG_PRINTLN:
        return fmt_sprintln(a, v);
    default:
        return fmt_sprint(a, v);
    }
}

/* Go's Fatal and Panic hand Output a string from fmt.Sprint and friends, which
 * is the same bytes Print appends, so these go through log_emit the same way
 * and only differ after it. */
BURROW_NOINLINE BURROW_NORETURN static void log_die(LogLogger *l, LogSite site, int how,
                                                    Str format, Slice v) {
    (void)log_emit(l, site, log_msg(how, format, v));
    pal_exit(1);
}

BURROW_NOINLINE BURROW_NORETURN static void log_raise(LogLogger *l, LogSite site,
                                                      int how, Str format, Slice v) {
    Str s = log_sprint(how, format, v);
    (void)log_emit(l, site, log_msg_str(s));
    panic_str(s);
}

Error log_logger_output(LogLogger *l, Int calldepth, Str s) {
    return log_emit(l, log_site_up(calldepth), log_msg_str(s));
}

void log_logger_print(LogLogger *l, Slice v) {
    (void)log_emit(l, log_site_up(1), log_msg(BURROW__LOG_PRINT, BURROW_STR_EMPTY, v));
}

void log_logger_printf(LogLogger *l, Str format, Slice v) {
    (void)log_emit(l, log_site_up(1), log_msg(BURROW__LOG_PRINTF, format, v));
}

void log_logger_println(LogLogger *l, Slice v) {
    (void)log_emit(l, log_site_up(1),
                   log_msg(BURROW__LOG_PRINTLN, BURROW_STR_EMPTY, v));
}

void log_logger_fatal(LogLogger *l, Slice v) {
    log_die(l, log_site_up(2), BURROW__LOG_PRINT, BURROW_STR_EMPTY, v);
}

void log_logger_fatalf(LogLogger *l, Str format, Slice v) {
    log_die(l, log_site_up(2), BURROW__LOG_PRINTF, format, v);
}

void log_logger_fatalln(LogLogger *l, Slice v) {
    log_die(l, log_site_up(2), BURROW__LOG_PRINTLN, BURROW_STR_EMPTY, v);
}

void log_logger_panic(LogLogger *l, Slice v) {
    log_raise(l, log_site_up(2), BURROW__LOG_PRINT, BURROW_STR_EMPTY, v);
}

void log_logger_panicf(LogLogger *l, Str format, Slice v) {
    log_raise(l, log_site_up(2), BURROW__LOG_PRINTF, format, v);
}

void log_logger_panicln(LogLogger *l, Slice v) {
    log_raise(l, log_site_up(2), BURROW__LOG_PRINTLN, BURROW_STR_EMPTY, v);
}

/* ------------------------------------------------------ the standard logger */

void log_set_output(IoWriter w) {
    log_logger_set_output(&log_std, w);
}

IoWriter log_writer(void) {
    return log_logger_writer(&log_std);
}

Int log_flags(void) {
    return log_logger_flags(&log_std);
}

void log_set_flags(Int flag) {
    log_logger_set_flags(&log_std, flag);
}

Str log_prefix(void) {
    return log_logger_prefix(&log_std);
}

void log_set_prefix(Str prefix) {
    log_logger_set_prefix(&log_std, prefix);
}

Error log_output(Int calldepth, Str s) {
    return log_emit(&log_std, log_site_up(calldepth), log_msg_str(s));
}

void log_print(Slice v) {
    (void)log_emit(&log_std, log_site_up(1),
                   log_msg(BURROW__LOG_PRINT, BURROW_STR_EMPTY, v));
}

void log_printf(Str format, Slice v) {
    (void)log_emit(&log_std, log_site_up(1), log_msg(BURROW__LOG_PRINTF, format, v));
}

void log_println(Slice v) {
    (void)log_emit(&log_std, log_site_up(1),
                   log_msg(BURROW__LOG_PRINTLN, BURROW_STR_EMPTY, v));
}

void log_fatal(Slice v) {
    log_die(&log_std, log_site_up(2), BURROW__LOG_PRINT, BURROW_STR_EMPTY, v);
}

void log_fatalf(Str format, Slice v) {
    log_die(&log_std, log_site_up(2), BURROW__LOG_PRINTF, format, v);
}

void log_fatalln(Slice v) {
    log_die(&log_std, log_site_up(2), BURROW__LOG_PRINTLN, BURROW_STR_EMPTY, v);
}

void log_panic(Slice v) {
    log_raise(&log_std, log_site_up(2), BURROW__LOG_PRINT, BURROW_STR_EMPTY, v);
}

void log_panicf(Str format, Slice v) {
    log_raise(&log_std, log_site_up(2), BURROW__LOG_PRINTF, format, v);
}

void log_panicln(Slice v) {
    log_raise(&log_std, log_site_up(2), BURROW__LOG_PRINTLN, BURROW_STR_EMPTY, v);
}

/* ------------------------------------------------------------- the _v macros */

void burrow__log_at(LogLogger *l, const char *file, int line, int how, Str format,
                    Slice v) {
    (void)log_emit(log_or_std(l), log_site_at(file, line), log_msg(how, format, v));
}

void burrow__log_fatal_at(LogLogger *l, const char *file, int line, int how, Str format,
                          Slice v) {
    log_die(log_or_std(l), log_site_at(file, line), how, format, v);
}

void burrow__log_panic_at(LogLogger *l, const char *file, int line, int how, Str format,
                          Slice v) {
    log_raise(log_or_std(l), log_site_at(file, line), how, format, v);
}
