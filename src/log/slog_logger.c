/* log/slog: Logger, the default logger, and the bridge to the log package.
 *
 * The default logger is static and starts out with the static default handler,
 * so nothing has to run before the first call. slog_set_default swaps a
 * pointer, as Go's atomic.Pointer does.
 *
 * Go captures the caller's program counter for every record. Here the _v
 * macros pass __FILE__, __LINE__ and __func__ instead, which needs no symbol
 * table to turn back into a place. The plain functions still capture a pc, for
 * a handler that asks for the source of a record that was not made by a
 * macro.
 *
 * Derived from Go's src/log/slog/logger.go. Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/log/slog.h"

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"

#include "slog_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <string.h>

struct SlogLogger {
    Alloc *a; /* NULL for the default logger, which is static */
    SlogHandler handler;
    bool owns; /* the handler came from With or WithGroup */
};

/* logLoggerLevel. The zero LevelVar is INFO. */
SlogLevelVar burrow__slog_log_logger_level;

/* The logger slog_default returns until slog_set_default is called. Its
 * handler is filled in where it is read, because it lives in another file. */
static SlogLogger slogl_std;

/* defaultLogger, NULL until slog_set_default. */
static SyncAtomicPointer slogl_default;

/* The logger whose handler log's output goes to, once slog_set_default has
 * taken it over. */
static SyncAtomicPointer slogl_log_target;

/* capturePC in the handlerWriter slog_set_default makes: whether log's flags
 * asked for a file when it was called. */
static SyncAtomicBool slogl_capture_site;

static SlogHandler slogl_handler(const SlogLogger *l) {
    return l == &slogl_std ? burrow__slog_std_handler() : l->handler;
}

SlogLogger *slog_new(Alloc *a, SlogHandler h) {
    if (h.vt == NULL)
        panic_str(BURROW_S("nil Handler"));
    SlogLogger *l = BURROW_NEW(a, SlogLogger);
    if (l == NULL)
        return NULL;
    l->a = a;
    l->handler = h;
    l->owns = false;
    return l;
}

void slog_logger_free(SlogLogger *l) {
    if (l == NULL || l == &slogl_std)
        return;
    if (l->owns)
        slog_handler_free(l->handler);
    mem_free(l->a, l, sizeof *l, alignof(SlogLogger));
}

SlogHandler slog_logger_handler(SlogLogger *l) {
    return slogl_handler(l != NULL ? l : slog_default());
}

/* A logger in l's allocator, or the heap's for the default logger, with h. */
static SlogLogger *slogl_derive(const SlogLogger *l, SlogHandler h, bool owns) {
    if (h.vt == NULL)
        return NULL;
    Alloc *a = l->a != NULL ? l->a : heap_allocator();
    SlogLogger *c = BURROW_NEW(a, SlogLogger);
    if (c == NULL) {
        if (owns)
            slog_handler_free(h);
        return NULL;
    }
    c->a = a;
    c->handler = h;
    c->owns = owns;
    return c;
}

SlogLogger *slog_logger_with(SlogLogger *l, Slice args) {
    if (l == NULL)
        l = slog_default();
    SlogHandler h = slogl_handler(l);
    if (args.len == 0)
        return slogl_derive(l, h, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice attrs = burrow__slog_args_to_attrs(arena_allocator(&ar), args);
    SlogHandler c = h.vt->with_attrs(h.data, attrs);
    arena_free(&ar);
    return slogl_derive(l, c, true);
}

SlogLogger *slog_logger_with_group(SlogLogger *l, Str name) {
    if (l == NULL)
        l = slog_default();
    SlogHandler h = slogl_handler(l);
    if (name.len == 0)
        return slogl_derive(l, h, false);
    return slogl_derive(l, h.vt->with_group(h.data, name), true);
}

SlogLogger *slog_with(Slice args) {
    return slog_logger_with(slog_default(), args);
}

static Context slogl_ctx(Context ctx) {
    return ctx.vt != NULL ? ctx : context_background();
}

bool slog_logger_enabled(SlogLogger *l, Context ctx, SlogLevel level) {
    SlogHandler h = slog_logger_handler(l);
    return h.vt->enabled(h.data, slogl_ctx(ctx), level);
}

/* Logger.log and Logger.logAttrs. src is where the _v macros say the call was,
 * or NULL, and then the pc of whoever called the function that called this
 * goes in the record, as Go's runtime.Callers(3) does. */
BURROW_NOINLINE static void slogl_log(SlogLogger *l, Context ctx, SlogLevel level,
                                      const SlogSource *src, Str msg, Slice args,
                                      bool attrs) {
    if (l == NULL)
        l = slog_default();
    ctx = slogl_ctx(ctx);
    SlogHandler h = slogl_handler(l);
    if (!h.vt->enabled(h.data, ctx, level))
        return;
    Uintptr pc = 0;
    if (src == NULL) {
        Uintptr pcs[1] = {0};
        if (runtime_callers(3, slice_from(pcs, 1, 1, TYPE_UINTPTR)) == 1)
            pc = pcs[0];
    }
    Arena ar;
    arena_init(&ar, NULL, 0);
    SlogRecord r = slog_new_record(time_now(), level, msg, pc);
    if (src != NULL)
        r.source = *src;
    if (attrs)
        slog_record_add_attrs(&r, arena_allocator(&ar), args);
    else
        slog_record_add(&r, arena_allocator(&ar), args);
    (void)h.vt->handle(h.data, ctx, r);
    arena_free(&ar);
}

static SlogSource slogl_site(const char *file, int line, const char *func) {
    SlogSource s;
    memset(&s, 0, sizeof s);
    if (func != NULL)
        s.function = str_from_cstr(func);
    if (file != NULL) {
        s.file = str_from_cstr(file);
        s.line = line;
    }
    return s;
}

void burrow__slog_log_at(SlogLogger *l, Context ctx, SlogLevel level, const char *file,
                         int line, const char *func, Str msg, Slice args) {
    SlogSource src = slogl_site(file, line, func);
    slogl_log(l, ctx, level, &src, msg, args, false);
}

void burrow__slog_log_attrs_at(SlogLogger *l, Context ctx, SlogLevel level,
                               const char *file, int line, const char *func, Str msg,
                               Slice attrs) {
    SlogSource src = slogl_site(file, line, func);
    slogl_log(l, ctx, level, &src, msg, attrs, true);
}

void slog_logger_log(SlogLogger *l, Context ctx, SlogLevel level, Str msg, Slice args) {
    slogl_log(l, ctx, level, NULL, msg, args, false);
}

void slog_logger_log_attrs(SlogLogger *l, Context ctx, SlogLevel level, Str msg,
                           Slice attrs) {
    slogl_log(l, ctx, level, NULL, msg, attrs, true);
}

void slog_logger_debug(SlogLogger *l, Str msg, Slice args) {
    slogl_log(l, context_background(), SLOG_LEVEL_DEBUG, NULL, msg, args, false);
}

void slog_logger_debug_context(SlogLogger *l, Context ctx, Str msg, Slice args) {
    slogl_log(l, ctx, SLOG_LEVEL_DEBUG, NULL, msg, args, false);
}

void slog_logger_info(SlogLogger *l, Str msg, Slice args) {
    slogl_log(l, context_background(), SLOG_LEVEL_INFO, NULL, msg, args, false);
}

void slog_logger_info_context(SlogLogger *l, Context ctx, Str msg, Slice args) {
    slogl_log(l, ctx, SLOG_LEVEL_INFO, NULL, msg, args, false);
}

void slog_logger_warn(SlogLogger *l, Str msg, Slice args) {
    slogl_log(l, context_background(), SLOG_LEVEL_WARN, NULL, msg, args, false);
}

void slog_logger_warn_context(SlogLogger *l, Context ctx, Str msg, Slice args) {
    slogl_log(l, ctx, SLOG_LEVEL_WARN, NULL, msg, args, false);
}

void slog_logger_error(SlogLogger *l, Str msg, Slice args) {
    slogl_log(l, context_background(), SLOG_LEVEL_ERROR, NULL, msg, args, false);
}

void slog_logger_error_context(SlogLogger *l, Context ctx, Str msg, Slice args) {
    slogl_log(l, ctx, SLOG_LEVEL_ERROR, NULL, msg, args, false);
}

void slog_debug(Str msg, Slice args) {
    slogl_log(NULL, context_background(), SLOG_LEVEL_DEBUG, NULL, msg, args, false);
}

void slog_debug_context(Context ctx, Str msg, Slice args) {
    slogl_log(NULL, ctx, SLOG_LEVEL_DEBUG, NULL, msg, args, false);
}

void slog_info(Str msg, Slice args) {
    slogl_log(NULL, context_background(), SLOG_LEVEL_INFO, NULL, msg, args, false);
}

void slog_info_context(Context ctx, Str msg, Slice args) {
    slogl_log(NULL, ctx, SLOG_LEVEL_INFO, NULL, msg, args, false);
}

void slog_warn(Str msg, Slice args) {
    slogl_log(NULL, context_background(), SLOG_LEVEL_WARN, NULL, msg, args, false);
}

void slog_warn_context(Context ctx, Str msg, Slice args) {
    slogl_log(NULL, ctx, SLOG_LEVEL_WARN, NULL, msg, args, false);
}

void slog_error(Str msg, Slice args) {
    slogl_log(NULL, context_background(), SLOG_LEVEL_ERROR, NULL, msg, args, false);
}

void slog_error_context(Context ctx, Str msg, Slice args) {
    slogl_log(NULL, ctx, SLOG_LEVEL_ERROR, NULL, msg, args, false);
}

void slog_log(Context ctx, SlogLevel level, Str msg, Slice args) {
    slogl_log(NULL, ctx, level, NULL, msg, args, false);
}

void slog_log_attrs(Context ctx, SlogLevel level, Str msg, Slice attrs) {
    slogl_log(NULL, ctx, level, NULL, msg, attrs, true);
}

/* ---------------------------------------------------------------- default */

SlogLogger *slog_default(void) {
    SlogLogger *l = sync_atomic_pointer_load(&slogl_default);
    return l != NULL ? l : &slogl_std;
}

SlogLevel slog_set_log_logger_level(SlogLevel level) {
    SlogLevel old = slog_level_var_level(&burrow__slog_log_logger_level);
    slog_level_var_set(&burrow__slog_log_logger_level, level);
    return old;
}

/* handlerWriter.Write: one log line as one record, without its newline, if h
 * is enabled at level. Go captures the pc of the log call when log's flags
 * asked for a file, and here the log package says where the call was. */
static Int slogl_write_record(SlogHandler h, SlogLevel level, bool capture_site,
                              Slice buf, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Context ctx = context_background();
    if (!h.vt->enabled(h.data, ctx, level))
        return 0;
    Int n = buf.len;
    if (n > 0 && ((const Byte *)buf.p)[n - 1] == '\n')
        n--;
    SlogRecord r = slog_new_record(time_now(), level, str_from_bytes(buf.p, n), 0);
    if (capture_site && !burrow__log_writing_site(&r.source.file, &r.source.line))
        memset(&r.source, 0, sizeof r.source);
    Error e = h.vt->handle(h.data, ctx, r);
    if (BURROW_FAILED(e) && err != NULL)
        *err = e;
    return buf.len;
}

static Int slogl_log_write(void *self, Slice buf, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    (void)self;
    SlogLogger *l = sync_atomic_pointer_load(&slogl_log_target);
    if (l == NULL)
        return buf.len;
    SlogLevel level = slog_level_var_level(&burrow__slog_log_logger_level);
    bool capture = sync_atomic_bool_load(&slogl_capture_site);
    return slogl_write_record(slogl_handler(l), level, capture, buf, err);
}

static const IoWriterVT slogl_log_writer_vt = {NULL, slogl_log_write};

void slog_set_default(SlogLogger *l) {
    sync_atomic_pointer_store(&slogl_default, l);
    if (!burrow__slog_is_default_handler(slogl_handler(l))) {
        sync_atomic_pointer_store(&slogl_log_target, l);
        Int flags = log_flags();
        sync_atomic_bool_store(&slogl_capture_site,
                               (flags & (LOG_LSHORTFILE | LOG_LLONGFILE)) != 0);
        log_set_output((IoWriter){&slogl_log_writer_vt, NULL});
        log_set_flags(0);
    }
}

/* ------------------------------------------------------------ log logger */

/* NewLogLogger's logger and its handlerWriter in one allocation. The logger
 * comes first, so the pointer handed out is the allocation. */
typedef struct SlogLogLogger {
    LogLogger log;
    SlogHandler h;
    SlogLevel level;
} SlogLogLogger;

static Int slogl_log_logger_write(void *self, Slice buf, Error *err) {
    const SlogLogLogger *x = self;
    return slogl_write_record(x->h, x->level, false, buf, err);
}

static const IoWriterVT slogl_log_logger_vt = {NULL, slogl_log_logger_write};

LogLogger *slog_new_log_logger(Alloc *a, SlogHandler h, SlogLevel level) {
    SlogLogLogger *x = BURROW_NEW(a, SlogLogLogger);
    if (x == NULL)
        return NULL;
    memset(x, 0, sizeof *x);
    x->h = h;
    x->level = level;
    log_logger_set_output(&x->log, (IoWriter){&slogl_log_logger_vt, x});
    log_logger_set_flags(&x->log, 0);
    return &x->log;
}

void slog_log_logger_free(Alloc *a, LogLogger *l) {
    if (l != NULL)
        mem_free(a, l, sizeof(SlogLogLogger), alignof(SlogLogLogger));
}
