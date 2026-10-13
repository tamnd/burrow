#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/log/slog.h"

static Int stdout_write(void *self, Slice p, Error *err) {
    (void)self;
    if (err)
        *err = BURROW_NO_ERROR;
    return (Int)fwrite(p.p, 1, (size_t)p.len, stdout);
}

static const IoWriterVT stdout_vt = {NULL, stdout_write};
static const IoWriter to_stdout = {&stdout_vt, NULL};

// doc: replace
/* Drops the time from every line, so that the output stays the same from one
 * run to the next. */
static SlogAttr no_time(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    (void)a;
    if (groups.len == 0 && str_eq(attr.key, SLOG_TIME_KEY))
        return (SlogAttr){0};
    return attr;
}
// doc: end

int main(void) {
    Alloc *a = heap_allocator();

    {
        // doc: text
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
        // doc: end
    }

    {
        // doc: json
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
        // doc: end
    }
    return 0;
}

/* Output:
level=INFO msg="server started" port=8080 tls=false
level=WARN msg="slow request" path="/a b" took=1.5s
level=ERROR msg="query failed" request_id=f00d db.table=users db.err=EOF
{"level":"DEBUG","msg":"cache miss","key":"user:42"}
{"level":"INFO","msg":"login","user":{"id":42,"name":"ada"}}
*/
