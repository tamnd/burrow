/* log/syslog. See include/burrow/log/syslog.h.
 *
 * Derived from Go's src/log/syslog/syslog.go and syslog_unix.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/log/syslog.h"

#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdint.h>

#define SEVERITY_MASK 0x07
#define FACILITY_MASK 0xf8

/* This interface and the separate syslog_unix.go file exist for
 * Solaris support as implemented by gccgo. On Solaris you cannot
 * simply open a TCP connection to the syslog daemon. The gccgo
 * sources have a syslog_solaris.go file that implements unixSyslog to
 * return a type that satisfies this interface and simply calls the C
 * library syslog function.
 *
 * Not in Go: there is only netConn here, so the Writer holds its two fields,
 * conn and local, in place of a serverConn. */
struct SyslogWriter {
    SyslogPriority priority;
    Str tag;
    Str hostname;
    Str network;
    Str raddr;

    SyncMutex mu; /* guards conn */
    NetConn conn; /* the nil NetConn for none */
    bool local;

    Alloc *a;
    Arena keep; /* tag, network, raddr and hostname */
    Arena tmp;  /* what one write formats, under mu */
};

static const Type syslog_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"log/syslog", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(SyslogWriter),
    (uint16_t)_Alignof(SyslogWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73796c67U, /* "sylg" */
    NULL,
};

const Type *const TYPE_SYSLOG_WRITER = &syslog_writer_desc;

static void set_err(Error *err, Error e) {
    if (err != NULL)
        *err = e;
}

static bool has_conn(const SyslogWriter *w) {
    return w->conn.vt != NULL;
}

/* Closes and gives back w->conn, with the error from the close. */
static Error drop_conn(SyslogWriter *w) {
    Error e = w->conn.vt->closer.close(w->conn.data);
    net_conn_free(w->conn);
    w->conn.vt = NULL;
    w->conn.data = NULL;
    return e;
}

/* unixSyslog opens a connection to the syslog daemon running on the
 * local machine using a Unix domain socket. */
static Error unix_syslog(SyslogWriter *w) {
    static const Str log_types[] = {BURROW_S_INIT("unixgram"), BURROW_S_INIT("unix")};
    static const Str log_paths[] = {BURROW_S_INIT("/dev/log"),
                                    BURROW_S_INIT("/var/run/syslog"),
                                    BURROW_S_INIT("/var/run/log")};
    for (size_t i = 0; i < sizeof log_types / sizeof log_types[0]; i++) {
        for (size_t j = 0; j < sizeof log_paths / sizeof log_paths[0]; j++) {
            Error e = BURROW_NO_ERROR;
            NetConn conn = net_dial(w->a, log_types[i], log_paths[j], &e);
            if (BURROW_OK(e)) {
                w->conn = conn;
                w->local = true;
                return BURROW_NO_ERROR;
            }
        }
    }
    return errors_new(error_allocator(), BURROW_S("Unix syslog delivery error"));
}

/* connect makes a connection to the syslog server.
 * It must be called with w.mu held. */
static Error syslog_connect(SyslogWriter *w) {
    if (has_conn(w)) {
        /* ignore err from close, it makes sense to continue anyway */
        (void)drop_conn(w);
    }

    Error err = BURROW_NO_ERROR;
    if (str_is_empty(w->network)) {
        err = unix_syslog(w);
        if (str_is_empty(w->hostname))
            w->hostname = BURROW_S("localhost");
    } else {
        NetConn c = net_dial(w->a, w->network, w->raddr, &err);
        if (BURROW_OK(err)) {
            w->conn = c;
            w->local = str_eq(w->network, BURROW_S("unixgram")) ||
                       str_eq(w->network, BURROW_S("unix"));
            if (str_is_empty(w->hostname)) {
                NetAddr la = c.vt->local_addr(c.data);
                if (la.vt != NULL)
                    w->hostname = la.vt->string(la.data, arena_allocator(&w->keep));
            }
        }
    }
    return err;
}

static void writer_free(SyslogWriter *w) {
    arena_free(&w->tmp);
    arena_free(&w->keep);
    mem_free(w->a, w, sizeof *w, _Alignof(SyslogWriter));
}

SyslogWriter *syslog_new(Alloc *a, SyslogPriority priority, Str tag, Error *err) {
    return syslog_dial(a, BURROW_S(""), BURROW_S(""), priority, tag, err);
}

SyslogWriter *syslog_dial(Alloc *a, Str network, Str raddr, SyslogPriority priority,
                          Str tag, Error *err) {
    if (priority < 0 || priority > (SYSLOG_LOG_LOCAL7 | SYSLOG_LOG_DEBUG)) {
        set_err(err, errors_new(error_allocator(),
                                BURROW_S("log/syslog: invalid priority")));
        return NULL;
    }

    if (str_is_empty(tag)) {
        Slice args = os_args();
        if (args.len > 0)
            tag = ((const Str *)args.p)[0];
    }

    SyslogWriter *w = mem_alloc(a, sizeof *w, _Alignof(SyslogWriter));
    if (w == NULL) {
        set_err(err, burrow_err_out_of_memory);
        return NULL;
    }
    w->a = a;
    arena_init(&w->keep, a, 0);
    arena_init(&w->tmp, a, 0);
    Alloc *ka = arena_allocator(&w->keep);
    w->priority = priority;
    w->tag = str_clone(ka, tag);
    w->network = str_clone(ka, network);
    w->raddr = str_clone(ka, raddr);
    if (w->tag.len != tag.len || w->network.len != network.len ||
        w->raddr.len != raddr.len) {
        writer_free(w);
        set_err(err, burrow_err_out_of_memory);
        return NULL;
    }
    w->hostname = os_hostname(ka, NULL);

    sync_mutex_lock(&w->mu);
    Error e = syslog_connect(w);
    sync_mutex_unlock(&w->mu);
    if (BURROW_FAILED(e)) {
        writer_free(w);
        set_err(err, e);
        return NULL;
    }
    set_err(err, BURROW_NO_ERROR);
    return w;
}

/* write generates and writes a syslog formatted string. The
 * format is as follows: <PRI>TIMESTAMP HOSTNAME TAG[PID]: MSG */
static Int syslog_write(SyslogWriter *w, SyslogPriority p, Str msg, Error *err) {
    /* ensure it ends in a \n */
    Str nl = BURROW_S("");
    if (!strings_has_suffix(msg, BURROW_S("\n")))
        nl = BURROW_S("\n");

    ArenaMark m = arena_mark(&w->tmp);
    Alloc *ta = arena_allocator(&w->tmp);
    IoWriter out = net_conn_as_io_writer(w->conn);
    Int pr = p;
    Int pid = os_getpid();
    Error e = BURROW_NO_ERROR;
    if (w->local) {
        /* Compared to the network form below, the changes are:
         *	1. Use time.Stamp instead of time.RFC3339.
         *	2. Drop the hostname field from the Fprintf. */
        Str timestamp = time_format(time_now(), ta, TIME_STAMP);
        Any args[] = {
            BURROW_ANY(TYPE_INT, &pr),        BURROW_ANY(TYPE_STRING, &timestamp),
            BURROW_ANY(TYPE_STRING, &w->tag), BURROW_ANY(TYPE_INT, &pid),
            BURROW_ANY(TYPE_STRING, &msg),    BURROW_ANY(TYPE_STRING, &nl)};
        (void)fmt_fprintf(out, BURROW_S("<%d>%s %s[%d]: %s%s"),
                          slice_from(args, 6, 6, TYPE_ANY), &e);
    } else {
        Str timestamp = time_format(time_now(), ta, TIME_RFC3339);
        Any args[] = {BURROW_ANY(TYPE_INT, &pr),
                      BURROW_ANY(TYPE_STRING, &timestamp),
                      BURROW_ANY(TYPE_STRING, &w->hostname),
                      BURROW_ANY(TYPE_STRING, &w->tag),
                      BURROW_ANY(TYPE_INT, &pid),
                      BURROW_ANY(TYPE_STRING, &msg),
                      BURROW_ANY(TYPE_STRING, &nl)};
        (void)fmt_fprintf(out, BURROW_S("<%d>%s %s %s[%d]: %s%s"),
                          slice_from(args, 7, 7, TYPE_ANY), &e);
    }
    arena_release(&w->tmp, m);
    if (BURROW_FAILED(e)) {
        set_err(err, e);
        return 0;
    }
    /* Note: return the length of the input, not the number of
     * bytes printed by Fprintf, because this must behave like
     * an io.Writer. */
    set_err(err, BURROW_NO_ERROR);
    return msg.len;
}

static Int write_and_retry(SyslogWriter *w, SyslogPriority p, Str s, Error *err) {
    SyslogPriority pr = (w->priority & FACILITY_MASK) | (p & SEVERITY_MASK);

    sync_mutex_lock(&w->mu);

    if (has_conn(w)) {
        Error e = BURROW_NO_ERROR;
        Int n = syslog_write(w, pr, s, &e);
        if (BURROW_OK(e)) {
            sync_mutex_unlock(&w->mu);
            set_err(err, BURROW_NO_ERROR);
            return n;
        }
    }
    Error e = syslog_connect(w);
    if (BURROW_FAILED(e)) {
        sync_mutex_unlock(&w->mu);
        set_err(err, e);
        return 0;
    }
    Int n = syslog_write(w, pr, s, err);
    sync_mutex_unlock(&w->mu);
    return n;
}

Int syslog_writer_write(SyslogWriter *w, Slice b, Error *err) {
    Str s = {(const Byte *)b.p, b.len};
    return write_and_retry(w, w->priority, s, err);
}

Error syslog_writer_close(SyslogWriter *w) {
    sync_mutex_lock(&w->mu);
    Error e = BURROW_NO_ERROR;
    if (has_conn(w))
        e = drop_conn(w);
    sync_mutex_unlock(&w->mu);
    return e;
}

static Error log_at(SyslogWriter *w, SyslogPriority p, Str m) {
    Error err = BURROW_NO_ERROR;
    (void)write_and_retry(w, p, m, &err);
    return err;
}

Error syslog_writer_emerg(SyslogWriter *w, Str m) {
    return log_at(w, SYSLOG_LOG_EMERG, m);
}

Error syslog_writer_alert(SyslogWriter *w, Str m) {
    return log_at(w, SYSLOG_LOG_ALERT, m);
}

Error syslog_writer_crit(SyslogWriter *w, Str m) {
    return log_at(w, SYSLOG_LOG_CRIT, m);
}

Error syslog_writer_err(SyslogWriter *w, Str m) {
    return log_at(w, SYSLOG_LOG_ERR, m);
}

Error syslog_writer_warning(SyslogWriter *w, Str m) {
    return log_at(w, SYSLOG_LOG_WARNING, m);
}

Error syslog_writer_notice(SyslogWriter *w, Str m) {
    return log_at(w, SYSLOG_LOG_NOTICE, m);
}

Error syslog_writer_info(SyslogWriter *w, Str m) {
    return log_at(w, SYSLOG_LOG_INFO, m);
}

Error syslog_writer_debug(SyslogWriter *w, Str m) {
    return log_at(w, SYSLOG_LOG_DEBUG, m);
}

static Int syslog_io_write(void *self, Slice p, Error *err) {
    return syslog_writer_write((SyslogWriter *)self, p, err);
}

static const IoWriterVT syslog_writer_vt = {&syslog_writer_desc, syslog_io_write};

IoWriter syslog_writer_as_io_writer(SyslogWriter *w) {
    IoWriter out = {&syslog_writer_vt, w};
    return out;
}

void syslog_writer_free(SyslogWriter *w) {
    if (w == NULL)
        return;
    if (has_conn(w))
        (void)drop_conn(w);
    writer_free(w);
}

LogLogger *syslog_new_logger(Alloc *a, SyslogPriority p, Int log_flag, Error *err) {
    Error e = BURROW_NO_ERROR;
    SyslogWriter *s = syslog_new(a, p, BURROW_S(""), &e);
    if (BURROW_FAILED(e)) {
        set_err(err, e);
        return NULL;
    }
    LogLogger *l = log_new(a, syslog_writer_as_io_writer(s), BURROW_S(""), log_flag);
    if (l == NULL) {
        syslog_writer_free(s);
        set_err(err, burrow_err_out_of_memory);
        return NULL;
    }
    set_err(err, BURROW_NO_ERROR);
    return l;
}

void syslog_logger_free(Alloc *a, LogLogger *l) {
    if (l == NULL)
        return;
    IoWriter out = log_logger_writer(l);
    if (out.vt == &syslog_writer_vt)
        syslog_writer_free((SyslogWriter *)out.data);
    log_logger_free(a, l);
}
