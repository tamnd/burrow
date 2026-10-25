/* log/syslog, a simple interface to the system log service.
 *
 * Go's log/syslog. It can send messages to the syslog daemon using UNIX domain
 * sockets, UDP or TCP.
 *
 *     Error err = BURROW_NO_ERROR;
 *     SyslogWriter *w = syslog_dial(a, BURROW_S("tcp"), BURROW_S("localhost:1234"),
 *                                   SYSLOG_LOG_WARNING | SYSLOG_LOG_DAEMON,
 *                                   BURROW_S("demotag"), &err);
 *     if (BURROW_FAILED(err))
 *         log_fatal_v(err);
 *     (void)syslog_writer_emerg(w, BURROW_S("And this is a daemon emergency."));
 *     syslog_writer_free(w);
 *
 * Only one call to Dial is necessary. On write failures, the syslog client
 * will attempt to reconnect to the server and write again.
 *
 * The syslog package is frozen and is not accepting new features. Some
 * external packages provide more functionality. See:
 *
 *     https://godoc.org/?q=syslog
 *
 * Not in Go: each message goes to the server in one write, so a datagram holds
 * one message and a stream connection gets them whole, one after another.
 *
 * Not in Go: Go does not build this package on Windows or Plan 9. Here it is
 * in every build. On a system with no syslog daemon listening on a Unix socket,
 * which includes Windows, syslog_new fails with "Unix syslog delivery error",
 * and syslog_dial reaches what net_dial reaches there.
 *
 * BUG(akumar): This package is not implemented on Plan 9.
 *
 * BUG(odeke-em): As of Darwin's macOS 12.X (Monterey), Apple's syslog daemon
 * no longer listens on a UNIX domain socket and instead now uses their
 * proprietary Apple Unified Logging (AUL) system, hence this package will no
 * longer work. Please see https://developer.apple.com/documentation/os/logging.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package log/syslog */

#ifndef BURROW_LOG_SYSLOG_H
#define BURROW_LOG_SYSLOG_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* syslog.Priority. The Priority is a combination of the syslog facility and
 * severity. For example, SYSLOG_LOG_ALERT | SYSLOG_LOG_FTP sends an alert
 * severity message from the FTP facility. The default severity is
 * SYSLOG_LOG_EMERG; the default facility is SYSLOG_LOG_KERN. */
typedef Int SyslogPriority;

/* Not in Go: the severities and the facilities are one enum here, where Go has
 * two const blocks, because MSVC warns when values of two different enums are
 * combined with |, and that is how a priority is made. */
enum {
    /* Severity.
     *
     * From /usr/include/sys/syslog.h.
     * These are the same on Linux, BSD, and OS X. */
    SYSLOG_LOG_EMERG = 0,
    SYSLOG_LOG_ALERT = 1,
    SYSLOG_LOG_CRIT = 2,
    SYSLOG_LOG_ERR = 3,
    SYSLOG_LOG_WARNING = 4,
    SYSLOG_LOG_NOTICE = 5,
    SYSLOG_LOG_INFO = 6,
    SYSLOG_LOG_DEBUG = 7,

    /* Facility.
     *
     * From /usr/include/sys/syslog.h.
     * These are the same up to LOG_FTP on Linux, BSD, and OS X. */
    SYSLOG_LOG_KERN = 0 << 3,
    SYSLOG_LOG_USER = 1 << 3,
    SYSLOG_LOG_MAIL = 2 << 3,
    SYSLOG_LOG_DAEMON = 3 << 3,
    SYSLOG_LOG_AUTH = 4 << 3,
    SYSLOG_LOG_SYSLOG = 5 << 3,
    SYSLOG_LOG_LPR = 6 << 3,
    SYSLOG_LOG_NEWS = 7 << 3,
    SYSLOG_LOG_UUCP = 8 << 3,
    SYSLOG_LOG_CRON = 9 << 3,
    SYSLOG_LOG_AUTHPRIV = 10 << 3,
    SYSLOG_LOG_FTP = 11 << 3,
    /* 12 to 15 are unused. */
    SYSLOG_LOG_LOCAL0 = 16 << 3,
    SYSLOG_LOG_LOCAL1 = 17 << 3,
    SYSLOG_LOG_LOCAL2 = 18 << 3,
    SYSLOG_LOG_LOCAL3 = 19 << 3,
    SYSLOG_LOG_LOCAL4 = 20 << 3,
    SYSLOG_LOG_LOCAL5 = 21 << 3,
    SYSLOG_LOG_LOCAL6 = 22 << 3,
    SYSLOG_LOG_LOCAL7 = 23 << 3,
};

/* syslog.Writer. A Writer is a connection to a syslog server. Any number of
 * goroutines may write to one at once. */
typedef struct SyslogWriter SyslogWriter;

/* syslog.New. New establishes a new connection to the system log daemon.
 * Each write to the returned writer sends a log message with the given
 * priority (a combination of the syslog facility and severity) and prefix
 * tag. If tag is empty, os_args()[0] is used.
 *
 * Not in Go: the writer is made in a, and NULL comes back with the error when
 * there is no daemon to reach. Give it back with syslog_writer_free. */
BURROW_OWNS(ret) SyslogWriter *syslog_new(Alloc *a, SyslogPriority priority, Str tag,
                                          Error *err);

/* syslog.Dial. Dial establishes a connection to a log daemon by connecting to
 * address raddr on the specified network. Each write to the returned writer
 * sends a log message with the facility and severity (from priority) and tag.
 * If tag is empty, os_args()[0] is used. If network is empty, Dial will
 * connect to the local syslog server. Otherwise, see the documentation for
 * net_dial for valid values of network and raddr.
 *
 * Not in Go: a priority outside SYSLOG_LOG_EMERG to
 * SYSLOG_LOG_LOCAL7 | SYSLOG_LOG_DEBUG is the error
 * "log/syslog: invalid priority". The writer keeps its own copies of network,
 * raddr and tag. */
BURROW_OWNS(ret) SyslogWriter *syslog_dial(Alloc *a, Str network, Str raddr,
                                           SyslogPriority priority, Str tag,
                                           Error *err);

/* Writer.Write. Write sends a log message to the syslog daemon. Not in Go:
 * the message gets a newline unless it ends in one, and the count is len(b),
 * not what went to the server, as an io.Writer's has to be. */
Int syslog_writer_write(SyslogWriter *w, Slice b, Error *err);

/* Writer.Close. Close closes a connection to the syslog daemon. Not in Go: a
 * write after it connects again. */
BURROW_STATIC(ret) Error syslog_writer_close(SyslogWriter *w);

/* Writer.Emerg and the rest. Each logs m with its own severity, ignoring the
 * severity passed to New, and keeps the facility. */
BURROW_STATIC(ret) Error syslog_writer_emerg(SyslogWriter *w, Str m);
BURROW_STATIC(ret) Error syslog_writer_alert(SyslogWriter *w, Str m);
BURROW_STATIC(ret) Error syslog_writer_crit(SyslogWriter *w, Str m);
BURROW_STATIC(ret) Error syslog_writer_err(SyslogWriter *w, Str m);
BURROW_STATIC(ret) Error syslog_writer_warning(SyslogWriter *w, Str m);
BURROW_STATIC(ret) Error syslog_writer_notice(SyslogWriter *w, Str m);
BURROW_STATIC(ret) Error syslog_writer_info(SyslogWriter *w, Str m);
BURROW_STATIC(ret) Error syslog_writer_debug(SyslogWriter *w, Str m);

/* Not in Go: the Writer as an io.Writer, which Go converts to without being
 * asked. */
IoWriter syslog_writer_as_io_writer(SyslogWriter *w);

/* Not in Go, which has a collector: closes w if it is open and gives back
 * everything it holds. NULL is fine. Nothing may be writing to w. */
void syslog_writer_free(SyslogWriter *w);

/* syslog.NewLogger. NewLogger creates a log.Logger whose output is written to
 * the system log service with the specified priority, a combination of the
 * syslog facility and severity. The log_flag argument is the flag set passed
 * through to log_new to create the Logger.
 *
 * Not in Go: the logger and the writer under it are made in a. Give both back
 * with syslog_logger_free. */
BURROW_OWNS(ret) LogLogger *syslog_new_logger(Alloc *a, SyslogPriority p, Int log_flag,
                                              Error *err);

/* Not in Go: frees a logger from syslog_new_logger, and the writer it writes
 * to, from the same a. NULL is fine. */
void syslog_logger_free(Alloc *a, LogLogger *l);

/* The descriptor of SyslogWriter. */
extern const Type *const TYPE_SYSLOG_WRITER;

#ifdef __cplusplus
}
#endif

#endif /* BURROW_LOG_SYSLOG_H */
