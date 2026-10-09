/* log, simple logging.
 *
 * Go's log package. A LogLogger writes each message as one line to an
 * IoWriter, after a header that can hold a prefix, the date and time, and the
 * file and line of the call. The standard logger, log_default, writes to
 * standard error with the date and time, and the log_ functions without
 * "logger" in their names use it.
 *
 *     log_printf_v("listening on %s", addr);
 *
 *     BytesBuffer b = BYTES_BUFFER(a);
 *     LogLogger *l = log_new(a, bytes_buffer_as_io_writer(&b), BURROW_S("db: "),
 *                            LOG_LSHORTFILE);
 *     log_logger_println_v(l, "opened", 3, "tables");
 *     // b holds "db: main.c:12: opened 3 tables\n"
 *     log_logger_free(a, l);
 *
 * The _v macros take their operands the way fmt_printf_v does, and they also
 * pass the file and line they are written on, which is what LOG_LSHORTFILE and
 * LOG_LLONGFILE print. The functions under them take a Slice of Any, as Go's
 * take ...any. Called directly they print "???" for the file, because
 * runtime_caller does not know file names yet, and Go prints the same when
 * runtime.Caller fails.
 *
 * Every call makes one Write to the writer, under the logger's lock, so lines
 * from different threads do not mix. A message that does not end in a newline
 * gets one.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package log */

#ifndef BURROW_LOG_H
#define BURROW_LOG_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/sync.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What goes in the header of each line. The pieces always come in this order,
 * and only LOG_LMSGPREFIX moves anything: it puts the prefix just before the
 * message instead of at the start of the line. The prefix is followed by a
 * colon only when it ends in one. LOG_LDATE | LOG_LTIME gives
 *
 *     2009/01/23 01:23:23 message
 *
 * and LOG_LDATE | LOG_LTIME | LOG_LMICROSECONDS | LOG_LLONGFILE gives
 *
 *     2009/01/23 01:23:23.123123 /a/b/c/d.c:23: message */
enum {
    LOG_LDATE = 1 << 0,         /* the date in the local time zone: 2009/01/23 */
    LOG_LTIME = 1 << 1,         /* the time in the local time zone: 01:23:23 */
    LOG_LMICROSECONDS = 1 << 2, /* 01:23:23.123123, and implies LOG_LTIME */
    LOG_LLONGFILE = 1 << 3,     /* the full file name and line: /a/b/c/d.c:23 */
    LOG_LSHORTFILE = 1 << 4, /* the last element and line: d.c:23. Wins over LONGFILE */
    LOG_LUTC = 1 << 5,       /* the date and time in UTC, not the local zone */
    LOG_LMSGPREFIX = 1 << 6, /* the prefix just before the message */
    LOG_LSTD_FLAGS = LOG_LDATE | LOG_LTIME, /* what the standard logger starts with */
};

/* log.Logger. The fields are visible so that a LogLogger can be a variable of
 * its own, as in Go, where a zero Logger is ready once it has an output. Do not
 * touch them. A zero LogLogger with no output writes to standard error.
 *
 * The prefix is kept as it is given and not copied, so its bytes have to last
 * as long as the logger uses them. A literal is the usual thing. */
typedef struct LogLogger {
    IoWriter out;
    Str prefix;
    SyncMutex out_mu;    /* over out */
    SyncMutex prefix_mu; /* over prefix */
    uint32_t flag;       /* read and written atomically */
    uint32_t is_discard; /* out is io_discard, read and written atomically */
} LogLogger;

/* log.New. A logger in one allocation from a, or NULL when a says no. */
BURROW_OWNS(ret) LogLogger *log_new(Alloc *a, IoWriter out, Str prefix, Int flag);

/* Gives back a logger from log_new. NULL is fine. */
void log_logger_free(Alloc *a, LogLogger *l);

/* log.Default. The standard logger, which the log_ functions use. */
BURROW_STATIC(ret) LogLogger *log_default(void);

void log_logger_set_output(LogLogger *l, IoWriter w);
IoWriter log_logger_writer(LogLogger *l);
Int log_logger_flags(LogLogger *l);
void log_logger_set_flags(LogLogger *l, Int flag);
BURROW_BORROWS(ret, l) Str log_logger_prefix(LogLogger *l);
void log_logger_set_prefix(LogLogger *l, Str prefix);

/* Logger.Output. Writes s as one line. calldepth is how many frames up the
 * file and line come from, counting this call as one, as in Go. */
BURROW_STATIC(ret) Error log_logger_output(LogLogger *l, Int calldepth, Str s);

/* Logger.Print, Printf and Println, which format as fmt_sprint, fmt_sprintf and
 * fmt_sprintln do. The output error is dropped, as Go drops it. */
void log_logger_print(LogLogger *l, Slice v);
void log_logger_printf(LogLogger *l, Str format, Slice v);
void log_logger_println(LogLogger *l, Slice v);

/* Logger.Fatal and the rest. As Print and the rest, then os_exit(1). */
BURROW_NORETURN void log_logger_fatal(LogLogger *l, Slice v);
BURROW_NORETURN void log_logger_fatalf(LogLogger *l, Str format, Slice v);
BURROW_NORETURN void log_logger_fatalln(LogLogger *l, Slice v);

/* Logger.Panic and the rest. As Print and the rest, then a panic with the
 * message as a Str, which panic_text gives back in a catch block. The message
 * comes from error_allocator, as the library's other formatted panics do. */
BURROW_NORETURN void log_logger_panic(LogLogger *l, Slice v);
BURROW_NORETURN void log_logger_panicf(LogLogger *l, Str format, Slice v);
BURROW_NORETURN void log_logger_panicln(LogLogger *l, Slice v);

/* The same on the standard logger. */
void log_set_output(IoWriter w);
IoWriter log_writer(void);
Int log_flags(void);
void log_set_flags(Int flag);
BURROW_STATIC(ret) Str log_prefix(void);
void log_set_prefix(Str prefix);
BURROW_STATIC(ret) Error log_output(Int calldepth, Str s);
void log_print(Slice v);
void log_printf(Str format, Slice v);
void log_println(Slice v);
BURROW_NORETURN void log_fatal(Slice v);
BURROW_NORETURN void log_fatalf(Str format, Slice v);
BURROW_NORETURN void log_fatalln(Slice v);
BURROW_NORETURN void log_panic(Slice v);
BURROW_NORETURN void log_panicf(Str format, Slice v);
BURROW_NORETURN void log_panicln(Slice v);

/* ------------------------------------------------------------------ macros */

/* What the _v macros call. how is one of these, and l NULL means the standard
 * logger. Not for calling directly. */
enum {
    BURROW__LOG_PRINT = 0,
    BURROW__LOG_PRINTF = 1,
    BURROW__LOG_PRINTLN = 2,
};
void burrow__log_at(LogLogger *l, const char *file, int line, int how, Str format,
                    Slice v);
BURROW_NORETURN void burrow__log_fatal_at(LogLogger *l, const char *file, int line,
                                          int how, Str format, Slice v);
BURROW_NORETURN void burrow__log_panic_at(LogLogger *l, const char *file, int line,
                                          int how, Str format, Slice v);

#define BURROW__LOG_FMT(f, how, l, ...)                                                \
    f((l), __FILE__, __LINE__, (how), BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define BURROW__LOG_ARGS(f, how, l, ...)                                               \
    f((l), __FILE__, __LINE__, (how), BURROW_STR_EMPTY,                                \
      BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))

#define log_logger_print_v(l, ...)                                                     \
    BURROW__LOG_ARGS(burrow__log_at, BURROW__LOG_PRINT, l, __VA_ARGS__)
#define log_logger_printf_v(l, ...)                                                    \
    BURROW__LOG_FMT(burrow__log_at, BURROW__LOG_PRINTF, l, __VA_ARGS__)
#define log_logger_println_v(l, ...)                                                   \
    BURROW__LOG_ARGS(burrow__log_at, BURROW__LOG_PRINTLN, l, __VA_ARGS__)
#define log_logger_fatal_v(l, ...)                                                     \
    BURROW__LOG_ARGS(burrow__log_fatal_at, BURROW__LOG_PRINT, l, __VA_ARGS__)
#define log_logger_fatalf_v(l, ...)                                                    \
    BURROW__LOG_FMT(burrow__log_fatal_at, BURROW__LOG_PRINTF, l, __VA_ARGS__)
#define log_logger_fatalln_v(l, ...)                                                   \
    BURROW__LOG_ARGS(burrow__log_fatal_at, BURROW__LOG_PRINTLN, l, __VA_ARGS__)
#define log_logger_panic_v(l, ...)                                                     \
    BURROW__LOG_ARGS(burrow__log_panic_at, BURROW__LOG_PRINT, l, __VA_ARGS__)
#define log_logger_panicf_v(l, ...)                                                    \
    BURROW__LOG_FMT(burrow__log_panic_at, BURROW__LOG_PRINTF, l, __VA_ARGS__)
#define log_logger_panicln_v(l, ...)                                                   \
    BURROW__LOG_ARGS(burrow__log_panic_at, BURROW__LOG_PRINTLN, l, __VA_ARGS__)

#define log_print_v(...) log_logger_print_v(NULL, __VA_ARGS__)
#define log_printf_v(...) log_logger_printf_v(NULL, __VA_ARGS__)
#define log_println_v(...) log_logger_println_v(NULL, __VA_ARGS__)
#define log_fatal_v(...) log_logger_fatal_v(NULL, __VA_ARGS__)
#define log_fatalf_v(...) log_logger_fatalf_v(NULL, __VA_ARGS__)
#define log_fatalln_v(...) log_logger_fatalln_v(NULL, __VA_ARGS__)
#define log_panic_v(...) log_logger_panic_v(NULL, __VA_ARGS__)
#define log_panicf_v(...) log_logger_panicf_v(NULL, __VA_ARGS__)
#define log_panicln_v(...) log_logger_panicln_v(NULL, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
