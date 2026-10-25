# Write structured logs as JSON

log/slog writes a message along with key and value pairs, and the JSON handler puts each line out as one JSON object. A logger made with `slog_logger_with_v` adds its attributes to every line, which is handy for a request id. The level comes from a `SlogLevelVar`, so it can change while the program runs.

<!-- example: ../examples/cookbook/slog_logs.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/log/slog.h"

/* Leaves the time out of every line so the output is the same on each run. A
 * real program would keep it. */
static SlogAttr no_time(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    (void)a;
    if (groups.len == 0 && str_eq(attr.key, SLOG_TIME_KEY))
        return (SlogAttr){0};
    return attr;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* One JSON object to a line is easy for log tools to read. The level var
     * can be changed while the program runs. */
    SlogLevelVar level = {0};
    SlogHandlerOptions opts = {0};
    opts.replace_attr = no_time;
    opts.level = slog_level_var_as_leveler(&level);
    SlogJSONHandler *h =
        slog_new_json_handler(a, os_file_as_io_writer(os_stdout), &opts);
    SlogLogger *logger = slog_new(a, slog_json_handler_as_handler(h));

    slog_logger_info_v(logger, "starting", "version", "1.4.0", "workers", 4);
    slog_logger_debug_v(logger, "not shown, info is the default level");

    /* A logger made with some attributes adds them to every line it writes,
     * which saves passing a request id to every call. */
    SlogLogger *req = slog_logger_with_v(logger, "request", "r-81");
    slog_logger_warn_v(req, "slow query", "table", "orders",
                       slog_duration(S("took"), 1200 * TIME_MILLISECOND));
    slog_logger_error_v(req, "query failed", "err", io_err_unexpected_eof);

    slog_level_var_set(&level, SLOG_LEVEL_DEBUG);
    slog_logger_debug_v(logger, "now debug lines show", "cache", "cold");

    slog_logger_free(req);
    slog_logger_free(logger);
    slog_handler_free(slog_json_handler_as_handler(h));
    arena_free(&ar);
}
```

It prints:

```text
{"level":"INFO","msg":"starting","version":"1.4.0","workers":4}
{"level":"WARN","msg":"slow query","request":"r-81","table":"orders","took":1200000000}
{"level":"ERROR","msg":"query failed","request":"r-81","err":"unexpected EOF"}
{"level":"DEBUG","msg":"now debug lines show","cache":"cold"}
```

The program is [docs/examples/cookbook/slog_logs.c](../examples/cookbook/slog_logs.c). CI builds and runs it and checks that it prints this.
