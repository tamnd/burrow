/* log/slog, structured logging.
 *
 * Go's log/slog. A record is a time, a level, a message and a list of key and
 * value pairs called attributes, and a handler turns records into output. Two
 * handlers come with the package: the text handler writes key=value pairs and
 * the JSON handler writes one JSON object a line.
 *
 *     SlogTextHandler *th = slog_new_text_handler(a, os_file_as_io_writer(os_stderr),
 *                                                 NULL);
 *     SlogLogger *log = slog_new(a, slog_text_handler_as_handler(th));
 *     slog_logger_info_v(log, "listening", "addr", addr,
 *                        slog_bool(BURROW_S("tls"), true));
 *     // time=2026-10-13T09:15:02.418+07:00 level=INFO msg=listening addr=:8080 tls=true
 *
 * The _v macros take the attributes the way Go's ...any does: a key followed by
 * its value, or an SlogAttr, or an SlogValue. A key with nothing after it is
 * logged under "!BADKEY", and so is a value where a key should be. C's true and
 * false are ints, so a bare one logs as 1 or 0, and slog_bool is the way to log
 * a bool. The macros also pass the file, line and function they are written
 * in, which is what the AddSource option prints. The plain functions under
 * them take a Slice of Any.
 *
 * Values are borrowed and never copied. A string attribute points at the
 * caller's bytes and an Any points at the caller's value, so everything has to
 * live until the call that logs it returns. For arguments to a _v macro that is
 * automatic. Handlers made by this package format what WithAttrs gives them
 * into bytes straight away, so the attributes passed to slog_logger_with need
 * not outlive the call either. A handler of your own that keeps a record or an
 * attribute after Handle returns has to copy what it keeps.
 *
 * Memory comes from the allocator a logger or handler was made with. A logger
 * or handler made from another one, by With or WithGroup, shares state with
 * it, so free the derived ones first. With an arena none of this needs
 * thinking about. Each call to Handle builds its line in an arena of its own
 * and writes it with one Write under the handler's lock.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package log/slog */

#ifndef BURROW_LOG_SLOG_H
#define BURROW_LOG_SLOG_H

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- kind */

/* slog.Kind, what an SlogValue holds. */
typedef enum SlogKind {
    SLOG_KIND_ANY,
    SLOG_KIND_BOOL,
    SLOG_KIND_DURATION,
    SLOG_KIND_FLOAT64,
    SLOG_KIND_INT64,
    SLOG_KIND_STRING,
    SLOG_KIND_TIME,
    SLOG_KIND_UINT64,
    SLOG_KIND_GROUP,
    SLOG_KIND_LOG_VALUER,
} SlogKind;

/* "Any", "Bool" and so on, and "<unknown slog.Kind>" for anything else. */
BURROW_STATIC(ret) Str slog_kind_string(SlogKind k);

extern const Type burrow_type_SlogKind;
#define TYPE_SLOG_KIND TYPE_OF(SlogKind)

/* ------------------------------------------------------------------- value */

/* slog.Value. A tagged union: kind says which member of u is live. Go packs
 * the same thing into two words and a type switch. Build one with the
 * constructors below and read it with the accessors, not through u.
 *
 * The zero SlogValue is Go's zero Value, which is SLOG_KIND_ANY holding nil. */
typedef struct SlogValue {
    SlogKind kind;
    union {
        int64_t i;   /* SLOG_KIND_INT64 */
        uint64_t u;  /* SLOG_KIND_UINT64 */
        double f;    /* SLOG_KIND_FLOAT64 */
        bool b;      /* SLOG_KIND_BOOL */
        Duration d;  /* SLOG_KIND_DURATION */
        Str s;       /* SLOG_KIND_STRING */
        Slice group; /* SLOG_KIND_GROUP, a Slice of SlogAttr */
        Time t;      /* SLOG_KIND_TIME */
        Any any;     /* SLOG_KIND_ANY and SLOG_KIND_LOG_VALUER */
    } u;
} SlogValue;

extern const Type burrow_type_SlogValue;
#define TYPE_SLOG_VALUE TYPE_OF(SlogValue)

/* slog.Attr, a key and a value. An attribute with an empty key and a zero value
 * is ignored by the handlers here, which is how a ReplaceAttr function drops
 * one. */
typedef struct SlogAttr {
    Str key;
    SlogValue value;
} SlogAttr;

extern const Type burrow_type_SlogAttr;
#define TYPE_SLOG_ATTR TYPE_OF(SlogAttr)

/* The constructors, Go's StringValue, IntValue and the rest. None of them
 * allocates and none copies: the string's bytes stay where they are. */
BURROW_BORROWS(ret, value) SlogValue slog_string_value(Str value);
SlogValue slog_int_value(Int v);
SlogValue slog_int64_value(int64_t v);
SlogValue slog_uint64_value(uint64_t v);
SlogValue slog_float64_value(double v);
SlogValue slog_bool_value(bool v);
/* The monotonic clock reading is dropped, as Go drops it. */
SlogValue slog_time_value(Time v);
SlogValue slog_duration_value(Duration v);

/* slog.GroupValue. The group keeps as, a Slice of SlogAttr, and does not copy
 * it. Go copies when it has to leave out attributes that are themselves empty
 * groups. Here they are left out by moving the later ones down inside as, so
 * as has to be writable and is changed. */
BURROW_BORROWS(ret, as) SlogValue slog_group_value(Slice as);

/* slog.AnyValue. An Any holding one of the kinds above gives that kind:
 * Str is a string, every integer type of TYPE_INT64 or narrower is an int64,
 * every unsigned one a uint64, float and double a float64, TYPE_DURATION a
 * duration, TYPE_TIME a time, a Slice of SlogAttr a group, and an SlogValue
 * itself. A type with a LogValue method, or an Any holding an SlogLogValuer,
 * is SLOG_KIND_LOG_VALUER. Anything else is SLOG_KIND_ANY and keeps the Any,
 * which still points where it pointed. */
BURROW_BORROWS(ret, v) SlogValue slog_any_value(Any v);

SlogKind slog_value_kind(SlogValue v);

/* Value.Any: the value as an Any, of TYPE_INT64 for an int64 and so on, and
 * the Any itself for SLOG_KIND_ANY. The Any points into v, so v has to outlive
 * it. */
BURROW_BORROWS(ret, v) Any slog_value_any(const SlogValue *v);

/* Value.String. The string itself for SLOG_KIND_STRING, borrowed, and what
 * fmt's %v would print for the rest, built in a. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, v) Str slog_value_string(SlogValue v, Alloc *a);

/* The typed accessors. Each panics with Go's message when v holds something
 * else: "Value kind is String, not Int64". */
int64_t slog_value_int64(SlogValue v);
uint64_t slog_value_uint64(SlogValue v);
double slog_value_float64(SlogValue v);
bool slog_value_bool(SlogValue v);
Duration slog_value_duration(SlogValue v);
Time slog_value_time(SlogValue v);
/* A Slice of SlogAttr. Panics with "Group: bad kind". */
BURROW_BORROWS(ret, v) Slice slog_value_group(SlogValue v);
/* What a SLOG_KIND_LOG_VALUER holds. */
BURROW_BORROWS(ret, v) Any slog_value_log_valuer(SlogValue v);

/* Value.Equal. Values of SLOG_KIND_ANY compare with any_equal, which panics on
 * types Go cannot compare either. */
bool slog_value_equal(SlogValue v, SlogValue w);

/* Value.Resolve. Calls LogValue until the result is not a LogValuer, at most
 * 100 times, after which the result is an error saying so. A LogValue that
 * panics gives an error value starting "LogValue panicked". */
SlogValue slog_value_resolve(SlogValue v);

/* -------------------------------------------------------------- log valuer */

/* slog.LogValuer, for a value that wants to be logged as something else, a
 * secret as "REDACTED" or a struct as a group:
 *
 *     static SlogValue token_log_value(void *self) {
 *         (void)self;
 *         return slog_string_value(BURROW_S("REDACTED"));
 *     }
 *
 * A type can do the same with a LogValue method in its descriptor, taking no
 * arguments and returning an SlogValue. */
typedef struct SlogLogValuerVT {
    const Type *self_type;
    SlogValue (*log_value)(void *self);
} SlogLogValuerVT;

typedef struct SlogLogValuer {
    const SlogLogValuerVT *vt;
    void *data;
} SlogLogValuer;

extern const Type burrow_type_SlogLogValuer;
#define TYPE_SLOG_LOG_VALUER TYPE_OF(SlogLogValuer)

/* -------------------------------------------------------------------- attr */

/* The attribute constructors, Go's String, Int and the rest. Like the value
 * constructors they do not copy. */
BURROW_BORROWS(ret, key, value) SlogAttr slog_string(Str key, Str value);
BURROW_BORROWS(ret, key) SlogAttr slog_int64(Str key, int64_t value);
BURROW_BORROWS(ret, key) SlogAttr slog_int(Str key, Int value);
BURROW_BORROWS(ret, key) SlogAttr slog_uint64(Str key, uint64_t v);
BURROW_BORROWS(ret, key) SlogAttr slog_float64(Str key, double v);
BURROW_BORROWS(ret, key) SlogAttr slog_bool(Str key, bool v);
BURROW_BORROWS(ret, key) SlogAttr slog_time(Str key, Time v);
BURROW_BORROWS(ret, key) SlogAttr slog_duration(Str key, Duration v);
BURROW_BORROWS(ret, key, value) SlogAttr slog_any(Str key, Any value);

/* slog.GroupAttrs. attrs is a Slice of SlogAttr, kept as slog_group_value
 * keeps it. */
BURROW_BORROWS(ret, key, attrs) SlogAttr slog_group_attrs(Str key, Slice attrs);

/* slog.Group, from keys and values the way the _v macros take them. The
 * attributes are made in a. slog_group_v makes them on the stack instead. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, key, args) SlogAttr slog_group(Alloc *a, Str key,
                                                                    Slice args);

bool slog_attr_equal(SlogAttr a, SlogAttr b);

/* Attr.String, "key=value", built in al. */
BURROW_OWNS(ret) Str slog_attr_string(SlogAttr a, Alloc *al);

/* ------------------------------------------------------------------- level */

/* slog.Level. Any integer is a level, and the four names are spaced four apart
 * so that there is room between them. */
typedef Int SlogLevel;

enum {
    SLOG_LEVEL_DEBUG = -4,
    SLOG_LEVEL_INFO = 0,
    SLOG_LEVEL_WARN = 4,
    SLOG_LEVEL_ERROR = 8,
};

extern const Type burrow_type_SlogLevel;
#define TYPE_SLOG_LEVEL TYPE_OF(SlogLevel)

/* Level.String: "INFO", or the nearest name below with the distance from it,
 * "WARN+2" or "DEBUG-1". The four names are literals and the others are built
 * in a. */
BURROW_OWNS(ret) Str slog_level_string(SlogLevel l, Alloc *a);

/* The marshalling methods. Text is the String form and JSON is that form
 * quoted. Unmarshalling takes a name, in any case, with an optional offset,
 * "warn-3", and fails with Go's errors: slog: level string "x": unknown name. */
BURROW_OWNS(ret) Slice slog_level_marshal_json(SlogLevel l, Alloc *a, Error *err);
BURROW_STATIC(ret) Error slog_level_unmarshal_json(SlogLevel *l, Alloc *a, Slice data);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice slog_level_append_text(SlogLevel l,
                                                                     Alloc *a, Slice b,
                                                                     Error *err);
BURROW_OWNS(ret) Slice slog_level_marshal_text(SlogLevel l, Alloc *a, Error *err);
BURROW_STATIC(ret) Error slog_level_unmarshal_text(SlogLevel *l, Alloc *a, Slice data);

/* Level.Level, which makes a Level a Leveler in Go. */
SlogLevel slog_level_level(SlogLevel l);

/* slog.LevelVar, a level that can be changed while handlers are using it. The
 * zero value is SLOG_LEVEL_INFO and is ready to use. */
typedef struct SlogLevelVar {
    SyncAtomicInt64 val;
} SlogLevelVar;

extern const Type burrow_type_SlogLevelVar;
#define TYPE_SLOG_LEVEL_VAR TYPE_OF(SlogLevelVar)

SlogLevel slog_level_var_level(SlogLevelVar *v);
void slog_level_var_set(SlogLevelVar *v, SlogLevel l);
/* "LevelVar(INFO)". */
BURROW_OWNS(ret) Str slog_level_var_string(SlogLevelVar *v, Alloc *a);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice
slog_level_var_append_text(SlogLevelVar *v, Alloc *a, Slice b, Error *err);
BURROW_OWNS(ret) Slice slog_level_var_marshal_text(SlogLevelVar *v, Alloc *a,
                                                   Error *err);
BURROW_STATIC(ret) Error slog_level_var_unmarshal_text(SlogLevelVar *v, Alloc *a,
                                                       Slice data);

/* slog.Leveler, where a handler gets its minimum level from. */
typedef struct SlogLevelerVT {
    const Type *self_type;
    SlogLevel (*level)(void *self);
} SlogLevelerVT;

typedef struct SlogLeveler {
    const SlogLevelerVT *vt;
    void *data;
} SlogLeveler;

/* A fixed level as a Leveler. The level is kept in the interface value itself,
 * so there is nothing to keep alive. */
SlogLeveler slog_level_as_leveler(SlogLevel l);
BURROW_BORROWS(ret, v) SlogLeveler slog_level_var_as_leveler(SlogLevelVar *v);

/* ------------------------------------------------------------------ source */

/* slog.Source, where a record was logged from. The JSON handler writes it as a
 * group with these three keys and the text handler as file:line. */
typedef struct SlogSource {
    Str function;
    Str file;
    Int line;
} SlogSource;

extern const Type burrow_type_SlogSource;
#define TYPE_SLOG_SOURCE TYPE_OF(SlogSource)

/* ------------------------------------------------------------------ record */

enum { BURROW__SLOG_NATTRS_INLINE = 5 };

/* slog.Record, one log event. time, message, level and pc are Go's public
 * fields. source is where the call was written, which the _v macros fill in
 * and the plain functions leave empty. The rest is the attribute storage: the
 * first five inline and the others in back. Use the functions to get at them.
 *
 * Records are passed by value, as in Go, and a copy shares back with the
 * original. slog_record_clone makes a copy that can be added to without the
 * two getting in each other's way. */
typedef struct SlogRecord {
    Time time;
    Str message;
    SlogLevel level;
    Uintptr pc;
    SlogSource source;
    SlogAttr front[BURROW__SLOG_NATTRS_INLINE];
    Int nfront;
    Slice back;
} SlogRecord;

/* slog.NewRecord. pc is a program counter from runtime_callers, or 0. */
BURROW_BORROWS(ret, msg) SlogRecord slog_new_record(Time t, SlogLevel level, Str msg,
                                                    Uintptr pc);

/* Record.Clone. Neither copies the attributes: the clone is cut off from back's
 * spare room, so its next add has to allocate. */
SlogRecord slog_record_clone(SlogRecord r);

Int slog_record_num_attrs(const SlogRecord *r);

/* Record.Attrs: f on each attribute in order, until it returns false. */
void slog_record_attrs(const SlogRecord *r, bool (*f)(void *ctx, SlogAttr a),
                       void *ctx);

/* Record.AddAttrs and Record.Add. Past the first five, the attributes go in
 * back, which grows in a. Empty groups are left out. */
void slog_record_add_attrs(SlogRecord *r, Alloc *a, Slice attrs);
void slog_record_add(SlogRecord *r, Alloc *a, Slice args);

/* Record.Source, into *out. False when the record does not know where it came
 * from, which is what Go's nil is. The source the record was made with when it
 * has one, and otherwise what the program counter says, which today is the
 * function name and no file. A value rather than Go's pointer, because there
 * is nowhere for a pointer to point when the answer comes from pc. */
bool slog_record_source(const SlogRecord *r, SlogSource *out);

/* ----------------------------------------------------------------- handler */

typedef struct SlogHandlerVT SlogHandlerVT;

/* slog.Handler. A zeroed one is Go's nil Handler. */
typedef struct SlogHandler {
    const SlogHandlerVT *vt;
    void *data;
} SlogHandler;

/* Go's four methods and a free.
 *
 * enabled is asked before a record is built, so it can be cheap about saying
 * no. handle gets the record by value and may add to it. with_attrs and
 * with_group return a new handler, allocated however this one likes, or a nil
 * handler when that fails. Neither may keep the Slice or the name it is given
 * past the call without copying.
 *
 * free gives the handler back. It may be NULL for a handler that has nothing to
 * give back. slog_handler_free is the way to call it. */
struct SlogHandlerVT {
    const Type *self_type;
    bool (*enabled)(void *self, Context ctx, SlogLevel level);
    Error (*handle)(void *self, Context ctx, SlogRecord r);
    SlogHandler (*with_attrs)(void *self, Slice attrs);
    SlogHandler (*with_group)(void *self, Str name);
    void (*free)(void *self);
};

/* Frees a handler through its vtable. Free one made by with_attrs or
 * with_group before the one it was made from. A nil handler is fine. */
void slog_handler_free(SlogHandler h);

/* slog.DiscardHandler: never enabled, and every method does nothing. */
extern const SlogHandler slog_discard_handler;

/* The keys the built in handlers use for the four built in attributes, and
 * that a ReplaceAttr function sees. */
#define SLOG_TIME_KEY BURROW_S("time")
#define SLOG_LEVEL_KEY BURROW_S("level")
#define SLOG_MESSAGE_KEY BURROW_S("msg")
#define SLOG_SOURCE_KEY BURROW_S("source")

/* Go's ReplaceAttr. Called for each attribute that is not a group, with the
 * names of the groups it is in, a Slice of Str. What it returns is logged in
 * place of attr, and an attribute with an empty key and a zero value is not
 * logged at all.
 *
 * a is an arena that lasts until the record has been written, for building a
 * replacement in. The time, level, source and message come through here too,
 * with no groups, under the keys above. The level arrives as an Any of
 * TYPE_SLOG_LEVEL and the source as an Any of TYPE_SLOG_SOURCE. */
typedef SlogAttr (*SlogReplaceAttrFunc)(void *ctx, Alloc *a, Slice groups,
                                        SlogAttr attr);

/* slog.HandlerOptions. The zero value is the default: no source, level INFO
 * and no replacement. */
typedef struct SlogHandlerOptions {
    bool add_source;
    SlogLeveler level; /* a nil Leveler means SLOG_LEVEL_INFO */
    SlogReplaceAttrFunc replace_attr;
    void *replace_attr_ctx;
} SlogHandlerOptions;

/* slog.TextHandler, which writes
 *
 *     time=2026-10-13T09:15:02.418+07:00 level=INFO msg=hello count=3
 *
 * quoting a key or a string value with strconv_quote when it is empty or has a
 * space, an equals sign, a quote or something unprintable in it. A group's name
 * goes before each of its keys with a dot, g.a=1. Times have millisecond
 * precision, and an Any is written with its MarshalText or AppendText method
 * when it has one, quoted when it is a byte slice, and with fmt's %+v
 * otherwise.
 *
 * Made in a, which with_attrs and with_group allocate from too. opts may be
 * NULL and is copied. NULL when a says no. */
typedef struct SlogTextHandler SlogTextHandler;

BURROW_OWNS(ret) SlogTextHandler *slog_new_text_handler(Alloc *a, IoWriter w,
                                                        const SlogHandlerOptions *opts);
BURROW_BORROWS(ret, h) SlogHandler slog_text_handler_as_handler(SlogTextHandler *h);
bool slog_text_handler_enabled(SlogTextHandler *h, Context ctx, SlogLevel level);
BURROW_STATIC(ret) Error slog_text_handler_handle(SlogTextHandler *h, Context ctx,
                                                  SlogRecord r);
BURROW_OWNS(ret) SlogHandler slog_text_handler_with_attrs(SlogTextHandler *h,
                                                          Slice attrs);
BURROW_OWNS(ret) SlogHandler slog_text_handler_with_group(SlogTextHandler *h, Str name);

/* slog.JSONHandler, which writes
 *
 *     {"time":"2026-10-13T09:15:02.418391+07:00","level":"INFO","msg":"hello","count":3}
 *
 * with full precision times, durations in nanoseconds and groups as nested
 * objects. An error is written as its message and any other Any through
 * encoding/json with HTML escaping off, so a type with MarshalJSON gets to say
 * how. Made the same way as the text handler. */
typedef struct SlogJSONHandler SlogJSONHandler;

BURROW_OWNS(ret) SlogJSONHandler *slog_new_json_handler(Alloc *a, IoWriter w,
                                                        const SlogHandlerOptions *opts);
BURROW_BORROWS(ret, h) SlogHandler slog_json_handler_as_handler(SlogJSONHandler *h);
bool slog_json_handler_enabled(SlogJSONHandler *h, Context ctx, SlogLevel level);
BURROW_STATIC(ret) Error slog_json_handler_handle(SlogJSONHandler *h, Context ctx,
                                                  SlogRecord r);
BURROW_OWNS(ret) SlogHandler slog_json_handler_with_attrs(SlogJSONHandler *h,
                                                          Slice attrs);
BURROW_OWNS(ret) SlogHandler slog_json_handler_with_group(SlogJSONHandler *h, Str name);

/* slog.MultiHandler, which passes each record to every one of the n handlers
 * that is enabled for it, a clone each, and joins their errors. The array is
 * copied into a and the handlers in it are not freed with it, the way
 * io_multi_writer takes its writers. */
typedef struct SlogMultiHandler SlogMultiHandler;

BURROW_OWNS(ret) SlogMultiHandler *
slog_new_multi_handler(Alloc *a, const SlogHandler *handlers, Int n);
BURROW_BORROWS(ret, h) SlogHandler slog_multi_handler_as_handler(SlogMultiHandler *h);
bool slog_multi_handler_enabled(SlogMultiHandler *h, Context ctx, SlogLevel level);
BURROW_STATIC(ret) Error slog_multi_handler_handle(SlogMultiHandler *h, Context ctx,
                                                   SlogRecord r);
BURROW_OWNS(ret) SlogHandler slog_multi_handler_with_attrs(SlogMultiHandler *h,
                                                           Slice attrs);
BURROW_OWNS(ret) SlogHandler slog_multi_handler_with_group(SlogMultiHandler *h,
                                                           Str name);

/* ------------------------------------------------------------------ logger */

/* slog.Logger, a handler and the allocator to make derived loggers in. */
typedef struct SlogLogger SlogLogger;

/* slog.New. Panics with "nil Handler" on a nil handler, and returns NULL when
 * a says no. The logger does not own h. */
BURROW_OWNS(ret) BURROW_RETAINS(h) SlogLogger *slog_new(Alloc *a, SlogHandler h);

/* Gives the logger back, and the handler too when the logger made it, which is
 * what With and WithGroup do. NULL and the default logger are fine. */
void slog_logger_free(SlogLogger *l);

BURROW_BORROWS(ret, l) SlogHandler slog_logger_handler(SlogLogger *l);

/* Logger.With and Logger.WithGroup. A new logger in l's allocator, whose
 * handler is l's with the attributes or group added. Go hands back l itself
 * when there is nothing to add. These always make a new logger, so that every
 * result can be freed the same way. NULL when allocating fails. */
BURROW_OWNS(ret) SlogLogger *slog_logger_with(SlogLogger *l, Slice args);
BURROW_OWNS(ret) SlogLogger *slog_logger_with_group(SlogLogger *l, Str name);

/* Logger.Enabled. A nil ctx means context_background, here and below. */
bool slog_logger_enabled(SlogLogger *l, Context ctx, SlogLevel level);

/* Logger.Log and Logger.LogAttrs. */
void slog_logger_log(SlogLogger *l, Context ctx, SlogLevel level, Str msg, Slice args);
void slog_logger_log_attrs(SlogLogger *l, Context ctx, SlogLevel level, Str msg,
                           Slice attrs);

void slog_logger_debug(SlogLogger *l, Str msg, Slice args);
void slog_logger_debug_context(SlogLogger *l, Context ctx, Str msg, Slice args);
void slog_logger_info(SlogLogger *l, Str msg, Slice args);
void slog_logger_info_context(SlogLogger *l, Context ctx, Str msg, Slice args);
void slog_logger_warn(SlogLogger *l, Str msg, Slice args);
void slog_logger_warn_context(SlogLogger *l, Context ctx, Str msg, Slice args);
void slog_logger_error(SlogLogger *l, Str msg, Slice args);
void slog_logger_error_context(SlogLogger *l, Context ctx, Str msg, Slice args);

/* slog.Default. To begin with it writes through the log package's standard
 * logger, as "INFO message key=value", with log's flags in front. */
BURROW_STATIC(ret) SlogLogger *slog_default(void);

/* slog.SetDefault. l has to outlive its time as the default. Unless l's
 * handler is the one slog_default started with, this also sends the log
 * package's output through l's handler at the level slog_set_log_logger_level
 * set, and sets log's flags to 0. */
void slog_set_default(SlogLogger *l);

/* slog.SetLogLoggerLevel. Before slog_set_default, the lowest level the default
 * handler writes. After, the level log package output is logged at. Returns
 * the old one. */
SlogLevel slog_set_log_logger_level(SlogLevel level);

/* slog.With, on the default logger. */
BURROW_OWNS(ret) SlogLogger *slog_with(Slice args);

void slog_debug(Str msg, Slice args);
void slog_debug_context(Context ctx, Str msg, Slice args);
void slog_info(Str msg, Slice args);
void slog_info_context(Context ctx, Str msg, Slice args);
void slog_warn(Str msg, Slice args);
void slog_warn_context(Context ctx, Str msg, Slice args);
void slog_error(Str msg, Slice args);
void slog_error_context(Context ctx, Str msg, Slice args);
void slog_log(Context ctx, SlogLevel level, Str msg, Slice args);
void slog_log_attrs(Context ctx, SlogLevel level, Str msg, Slice attrs);

/* slog.NewLogLogger: a log package logger whose every line becomes a record at
 * level, handled by h. Both live in one allocation from a, which
 * slog_log_logger_free gives back. */
BURROW_OWNS(ret) BURROW_RETAINS(h) LogLogger *
slog_new_log_logger(Alloc *a, SlogHandler h, SlogLevel level);
void slog_log_logger_free(Alloc *a, LogLogger *l);

/* ------------------------------------------------------------------ macros */

/* What the _v macros expand to. Not for calling directly. */
void burrow__slog_log_at(SlogLogger *l, Context ctx, SlogLevel level, const char *file,
                         int line, const char *func, Str msg, Slice args);
void burrow__slog_log_attrs_at(SlogLogger *l, Context ctx, SlogLevel level,
                               const char *file, int line, const char *func, Str msg,
                               Slice attrs);
BURROW_BORROWS(ret, key, args, scratch) SlogAttr burrow__slog_group(Str key, Slice args,
                                                                    SlogAttr *scratch);
BURROW_BORROWS(ret, box) Any burrow__slog_any_of_attr(SlogAttr v, burrow__AnyBox *box);
BURROW_BORROWS(ret, box) Any burrow__slog_any_of_value(SlogValue v,
                                                       burrow__AnyBox *box);

/* The room one argument gets. An SlogAttr is bigger than anything fmt boxes, so
 * the box is a union with one, and the functions above find it through the
 * pointer to its first member. */
typedef union burrow__SlogBox {
    burrow__AnyBox any;
    SlogAttr attr;
    SlogValue value;
} burrow__SlogBox;

/* One argument as an Any: an SlogAttr or an SlogValue as itself, and anything
 * else the way fmt's _v macros take it. */
#define BURROW__SLOG_ARG(x)                                                            \
    _Generic((x),                                                                      \
        SlogAttr: burrow__slog_any_of_attr,                                            \
        SlogValue: burrow__slog_any_of_value,                                          \
        default: BURROW__ANY_OF_FN(x))((x), &(burrow__SlogBox){.any = {.v = {0}}}.any)

#define BURROW__SLOG_FARGS(...) BURROW__FMT_FARGS(BURROW__SLOG_ARG, __VA_ARGS__)

#define BURROW__SLOG_AT(l, ctx, level, ...)                                            \
    burrow__slog_log_at((l), (ctx), (level), __FILE__, __LINE__, __func__,             \
                        BURROW__SLOG_FARGS(__VA_ARGS__))

/* slog_logger_info_v(l, msg, key, value, ...) and the rest. */
#define slog_logger_log_v(l, ctx, level, ...)                                          \
    BURROW__SLOG_AT(l, ctx, level, __VA_ARGS__)
#define slog_logger_debug_v(l, ...)                                                    \
    BURROW__SLOG_AT(l, context_background(), SLOG_LEVEL_DEBUG, __VA_ARGS__)
#define slog_logger_debug_context_v(l, ctx, ...)                                       \
    BURROW__SLOG_AT(l, ctx, SLOG_LEVEL_DEBUG, __VA_ARGS__)
#define slog_logger_info_v(l, ...)                                                     \
    BURROW__SLOG_AT(l, context_background(), SLOG_LEVEL_INFO, __VA_ARGS__)
#define slog_logger_info_context_v(l, ctx, ...)                                        \
    BURROW__SLOG_AT(l, ctx, SLOG_LEVEL_INFO, __VA_ARGS__)
#define slog_logger_warn_v(l, ...)                                                     \
    BURROW__SLOG_AT(l, context_background(), SLOG_LEVEL_WARN, __VA_ARGS__)
#define slog_logger_warn_context_v(l, ctx, ...)                                        \
    BURROW__SLOG_AT(l, ctx, SLOG_LEVEL_WARN, __VA_ARGS__)
#define slog_logger_error_v(l, ...)                                                    \
    BURROW__SLOG_AT(l, context_background(), SLOG_LEVEL_ERROR, __VA_ARGS__)
#define slog_logger_error_context_v(l, ctx, ...)                                       \
    BURROW__SLOG_AT(l, ctx, SLOG_LEVEL_ERROR, __VA_ARGS__)

/* The same on the default logger. */
#define slog_log_v(ctx, level, ...) slog_logger_log_v(NULL, ctx, level, __VA_ARGS__)
#define slog_debug_v(...) slog_logger_debug_v(NULL, __VA_ARGS__)
#define slog_debug_context_v(ctx, ...)                                                 \
    slog_logger_debug_context_v(NULL, ctx, __VA_ARGS__)
#define slog_info_v(...) slog_logger_info_v(NULL, __VA_ARGS__)
#define slog_info_context_v(ctx, ...) slog_logger_info_context_v(NULL, ctx, __VA_ARGS__)
#define slog_warn_v(...) slog_logger_warn_v(NULL, __VA_ARGS__)
#define slog_warn_context_v(ctx, ...) slog_logger_warn_context_v(NULL, ctx, __VA_ARGS__)
#define slog_error_v(...) slog_logger_error_v(NULL, __VA_ARGS__)
#define slog_error_context_v(ctx, ...)                                                 \
    slog_logger_error_context_v(NULL, ctx, __VA_ARGS__)

/* slog_logger_log_attrs_v(l, ctx, level, msg, attr, ...), every argument after
 * the message an SlogAttr. */
#define slog_logger_log_attrs_v(l, ctx, level, msg, ...)                               \
    burrow__slog_log_attrs_at(                                                         \
        (l), (ctx), (level), __FILE__, __LINE__, __func__, BURROW__FMT_STR(msg),       \
        (Slice){(SlogAttr[]){__VA_ARGS__}, BURROW__FMT_NARGS(__VA_ARGS__),             \
                BURROW__FMT_NARGS(__VA_ARGS__), TYPE_SLOG_ATTR})
#define slog_log_attrs_v(ctx, level, msg, ...)                                         \
    slog_logger_log_attrs_v(NULL, ctx, level, msg, __VA_ARGS__)

/* The arguments as a Slice of Any, for the functions that take one. */
#define slog_args_v(...) BURROW__FMT_ARGS(BURROW__SLOG_ARG, __VA_ARGS__)

/* slog_logger_with_v(l, key, value, ...) and slog_with_v(key, value, ...). */
#define slog_logger_with_v(l, ...) slog_logger_with((l), slog_args_v(__VA_ARGS__))
#define slog_with_v(...) slog_with(slog_args_v(__VA_ARGS__))

/* slog_record_add_v(r, a, key, value, ...). */
#define slog_record_add_v(r, a, ...) slog_record_add((r), (a), slog_args_v(__VA_ARGS__))

/* slog_group_v(key, key, value, ...): slog.Group with its attributes on the
 * stack, so the result lasts until the end of the enclosing block. */
#define slog_group_v(...)                                                              \
    burrow__slog_group(BURROW__SLOG_FARGS(__VA_ARGS__),                                \
                       (SlogAttr[BURROW__FMT_NARGS(__VA_ARGS__)]){0})

#ifdef __cplusplus
}
#endif

#endif /* BURROW_LOG_SLOG_H */
