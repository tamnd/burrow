# Logging

`burrow/log.h` is Go's `log` package. A `LogLogger` writes each message as one line to an `IoWriter`, after a header that can hold a prefix, the date and time, and the file and line the call was made from. There is a standard logger, which writes to standard error with the date and time, and the functions without `logger` in their names use it.

## A logger of your own

`log_new` takes the writer, a prefix and the flags that say what goes in the header. Here the lines go to a buffer, and `LOG_LSHORTFILE` puts the file name and line number of each call in front of the message:

<!-- example: ../examples/log/log.c#logger -->
```c
BytesBuffer buf = BYTES_BUFFER(a);
LogLogger *logger = log_new(a, bytes_buffer_as_io_writer(&buf),
                            BURROW_S("logger: "), LOG_LSHORTFILE);
log_logger_print_v(logger, "Hello, log file!");
log_logger_println_v(logger, "opened", 3, "tables");

Slice out = bytes_buffer_bytes(&buf);
printf("%.*s", P(out));
log_logger_free(a, logger);
bytes_buffer_free(&buf);
```

That prints:

```text
logger: log.c:29: Hello, log file!
logger: log.c:30: opened 3 tables
```

The `_v` macros take their operands the way `fmt_printf_v` does. `log_logger_print_v` formats them as `fmt_sprint` would, `log_logger_printf_v` takes a format first, and `log_logger_println_v` puts a space between every two operands. Every call writes one line. A message that doesn't end in a newline gets one, and one that does isn't given a second.

The header is built from these flags, in this order:

| Flag | Adds |
|---|---|
| `LOG_LDATE` | the date in the local time zone, `2009/01/23` |
| `LOG_LTIME` | the time in the local time zone, `01:23:23` |
| `LOG_LMICROSECONDS` | microseconds after the time, `01:23:23.123123`, and implies `LOG_LTIME` |
| `LOG_LLONGFILE` | the file name as the compiler saw it, and the line |
| `LOG_LSHORTFILE` | the last element of the file name, and the line. It wins over `LOG_LLONGFILE` |
| `LOG_LUTC` | the date and time in UTC rather than the local time zone |
| `LOG_LMSGPREFIX` | the prefix just before the message, not at the start of the line |

`LOG_LSTD_FLAGS` is `LOG_LDATE | LOG_LTIME`, which is what the standard logger starts with.

The file and line come from the `_v` macros, which pass `__FILE__` and `__LINE__` along. The functions under them, such as `log_logger_printf`, take a `Slice` of `Any` the way Go's take `...any`, and called directly they print `???` for the file, because `runtime_caller` doesn't know file names yet. Go prints the same thing when it can't find the caller. `log_logger_output` is in the same position.

A logger is safe to use from many threads. Each line goes out in one write under the logger's lock, so lines from different threads don't mix. The prefix isn't copied, so it has to outlive the logger's use of it. A string literal is the usual thing.

A `LogLogger` can also be a variable of your own, as in Go. A zeroed one writes to standard error until `log_logger_set_output` gives it something else.

## The standard logger

`log_set_output`, `log_set_flags` and `log_set_prefix` change the standard logger, and `log_printf_v` and the rest write to it. `log_default` hands back a pointer to it, for code that takes a `LogLogger`:

<!-- example: ../examples/log/log.c#std -->
```c
log_set_output(to_stdout);
log_set_flags(LOG_LMSGPREFIX);
log_set_prefix(BURROW_S("serve: "));
log_printf_v("listening on %s", "localhost:8080");
log_print_v("no newline at the end, ", "so one is added");
log_print_v("one is here already\n");
```

`to_stdout` is a small writer over standard output, so that the lines come out in order with the example's `printf` calls:

<!-- example: ../examples/log/log.c#stdout -->
```c
/* Standard output as an IoWriter, so that the lines land in order with
 * printf's. */
static Int stdout_write(void *self, Slice p, Error *err) {
    (void)self;
    if (err)
        *err = BURROW_NO_ERROR;
    return (Int)fwrite(p.p, 1, (size_t)p.len, stdout);
}

static const IoWriterVT stdout_vt = {NULL, stdout_write};
static const IoWriter to_stdout = {&stdout_vt, NULL};
```

That prints:

```text
serve: listening on localhost:8080
serve: no newline at the end, so one is added
serve: one is here already
```

## Fatal and panic

`log_fatal_v`, `log_fatalf_v` and `log_fatalln_v` write the line and then end the program with status 1, the way `os_exit` does, without running deferred calls or flushing stdio buffers.

`log_panic_v`, `log_panicf_v` and `log_panicln_v` write the line and then panic with the message as a string, which a `BURROW_TRY` block can catch:

<!-- example: ../examples/log/log.c#panic -->
```c
log_set_flags(0);
log_set_prefix(BURROW_S(""));
BURROW_TRY {
    log_panicf_v("bad config: %d servers", 0);
}
BURROW_CATCH(p) {
    Str msg = panic_text(p);
    printf("recovered: %.*s\n", P(msg));
}
BURROW_TRY_END;
```

That prints the log line and then the recovered message:

```text
bad config: 0 servers
recovered: bad config: 0 servers
```

The message comes from `error_allocator`, which is where the library's other formatted panics get theirs.

## Structured logging

`burrow/log/slog.h` is Go's `log/slog`. A record is a time, a level, a message and a list of attributes, each a key and a value, and a handler turns records into output. The text handler writes `key=value` pairs and the JSON handler writes one JSON object a line. A `SlogLogger` sits in front of a handler and builds the records:

<!-- example: ../examples/log/slog.c#text -->
```c
SlogHandlerOptions opts = {0};
opts.replace_attr = no_time;
SlogTextHandler *h = slog_new_text_handler(a, to_stdout, &opts);
SlogLogger *logger = slog_new(a, slog_text_handler_as_handler(h));

slog_logger_info_v(logger, "server started", "port", 8080,
                   slog_bool(BURROW_S("tls"), false));
slog_logger_debug_v(logger, "not shown, debug is below the default level");
slog_logger_warn_v(logger, "slow request",
                   slog_string(BURROW_S("path"), BURROW_S("/a b")),
                   slog_duration(BURROW_S("took"), 1500 * TIME_MILLISECOND));

SlogLogger *req = slog_logger_with_v(logger, "request_id", "f00d");
SlogLogger *db = slog_logger_with_group(req, BURROW_S("db"));
slog_logger_error_v(db, "query failed", "table", "users", "err", io_eof);

slog_logger_free(db);
slog_logger_free(req);
slog_logger_free(logger);
slog_handler_free(slog_text_handler_as_handler(h));
```

That prints:

```text
level=INFO msg="server started" port=8080 tls=false
level=WARN msg="slow request" path="/a b" took=1.5s
level=ERROR msg="query failed" request_id=f00d db.table=users db.err=EOF
```

The `_v` macros take attributes the way Go's `...any` does. A key followed by its value is one attribute, and an `SlogAttr` made by `slog_string`, `slog_int`, `slog_duration` and the rest is another. A key with no value after it, or a value where a key should be, is logged under `!BADKEY`. C's `true` and `false` are ints, so a bare one logs as `1` or `0`; `slog_bool` is how to log a bool.

`slog_logger_with_v` makes a logger whose records all carry some attributes, and `slog_logger_with_group` one whose attributes go under a group name, which the text handler writes as `db.table`. Both make a new logger to free, and it shares its handler's state with the one it came from, so free the derived loggers first. The debug line isn't printed, because a handler with no level set drops anything below `SLOG_LEVEL_INFO`.

The lines have no time in them because of `replace_attr` in the options, which sees every attribute before it is written and can change it or drop it. Returning the zero `SlogAttr` drops it. The time, level, message and source come through it too, under `SLOG_TIME_KEY` and the other key names, with no groups:

<!-- example: ../examples/log/slog.c#replace -->
```c
/* Drops the time from every line, so that the output stays the same from one
 * run to the next. */
static SlogAttr no_time(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    (void)a;
    if (groups.len == 0 && str_eq(attr.key, SLOG_TIME_KEY))
        return (SlogAttr){0};
    return attr;
}
```

## Levels and JSON

The JSON handler takes the same options. Setting `level` to a `SlogLevelVar` lets the minimum level change while the program runs, from any thread:

<!-- example: ../examples/log/slog.c#json -->
```c
SlogLevelVar level = {0};
slog_level_var_set(&level, SLOG_LEVEL_DEBUG);
SlogHandlerOptions opts = {0};
opts.replace_attr = no_time;
opts.level = slog_level_var_as_leveler(&level);
SlogJSONHandler *h = slog_new_json_handler(a, to_stdout, &opts);
SlogLogger *logger = slog_new(a, slog_json_handler_as_handler(h));

slog_logger_debug_v(logger, "cache miss", "key", "user:42");
slog_logger_info_v(logger, "login",
                   slog_group_v(BURROW_S("user"), "id", 42, "name", "ada"));

slog_level_var_set(&level, SLOG_LEVEL_INFO);
slog_logger_debug_v(logger, "not shown any more");

slog_logger_free(logger);
slog_handler_free(slog_json_handler_as_handler(h));
```

That prints:

```text
{"level":"DEBUG","msg":"cache miss","key":"user:42"}
{"level":"INFO","msg":"login","user":{"id":42,"name":"ada"}}
```

`slog_group_v` makes a group attribute on the stack, which the JSON handler writes as a nested object. The levels are ints, with `SLOG_LEVEL_DEBUG`, `SLOG_LEVEL_INFO`, `SLOG_LEVEL_WARN` and `SLOG_LEVEL_ERROR` at -4, 0, 4 and 8, and anything in between is fine. `slog_level_string` names one as Go does, such as `WARN+2`.

Values are borrowed and never copied. A string attribute points at the caller's bytes and an `Any` points at the caller's value, so they have to last until the call that logs them returns, which arguments to a `_v` macro always do. The handlers in the package format what `slog_logger_with` gives them straight away, so those attributes needn't outlive the call either.

## slog and log together

The default slog logger, which `slog_info_v` and the other functions without `logger` in their names use, writes through the standard logger in `log`, so its lines get log's prefix and flags. `slog_set_default` turns that around: from then on the standard logger's lines go to the new slog handler as records, as in Go. Their level is `SLOG_LEVEL_INFO` unless `slog_set_log_logger_level` has changed it. Calling `log_set_output` afterwards breaks the connection again.

`slog_new_log_logger` goes the other way for a single logger. It makes a `LogLogger` whose lines become records at a level you choose, for code that only knows how to write to a `LogLogger`.

With `add_source` set, a handler writes the file and line of the call, which the `_v` macros pass along from `__FILE__` and `__LINE__`. The plain functions under them, which take a `Slice` of `Any`, have only what `runtime_caller` can find, and that doesn't include file names yet.

## The system log

`burrow/log/syslog.h` is Go's `log/syslog`. `syslog_dial` connects to a syslog server over the network you name, and `syslog_new` finds the local daemon on a Unix socket. The priority combines a facility and a severity, and each message goes out with it, a timestamp, the host name, the tag and the process ID. Here the server is a UDP socket in the same program:

<!-- example: ../examples/log/syslog.c#dial -->
```c
SyslogWriter *w = syslog_dial(a, BURROW_S("udp"), addr,
                              SYSLOG_LOG_WARNING | SYSLOG_LOG_DAEMON,
                              BURROW_S("demotag"), &err);
if (BURROW_FAILED(err))
    log_fatal_v(err);
IoWriter out = syslog_writer_as_io_writer(w);
fmt_fprintf_v(out, "This is a daemon warning with demotag.");
err = syslog_writer_emerg(
    w, BURROW_S("And this is a daemon emergency with demotag."));
if (BURROW_FAILED(err))
    log_fatal_v(err);
syslog_writer_free(w);
```

The server gets this, less the timestamp, host name, tag and pid in the middle of each line:

```text
<28> This is a daemon warning with demotag.
<24> And this is a daemon emergency with demotag.
```

`syslog_writer_emerg` and the other severity functions keep the facility from the dial and use their own severity, which is why the second line is 24, `SYSLOG_LOG_DAEMON` with `SYSLOG_LOG_EMERG`. When a write fails, the writer connects again and tries once more. `syslog_new_logger` gives a `LogLogger` that writes to the local daemon, and `syslog_logger_free` frees it along with its writer.

The package builds everywhere, unlike Go's, which leaves it out on Windows and Plan 9. Where nothing listens on the usual Unix sockets, which includes Windows and recent macOS, `syslog_new` fails with "Unix syslog delivery error".
