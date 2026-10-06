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
