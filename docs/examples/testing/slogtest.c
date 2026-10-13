#include "burrow/testing/slogtest.h"
#include "burrow/burrow.h"
#include "burrow/encoding/json/v2.h"
#include "burrow/mem/arena.h"

// doc: results
/* The results function: each line the handler wrote, decoded as a JSON
 * object into a map[string]any. */
typedef struct Lines {
    Alloc *a;
    BytesBuffer *buf;
} Lines;

static Slice parse_lines(void *env) {
    Lines *l = env;
    Slice ms = slice_nil(TYPE_JSONV2_MAP_STRING_ANY);
    Slice lines = bytes_split(l->a, bytes_buffer_bytes(l->buf), BURROW_B("\n"));
    const Slice *ls = lines.p;
    for (Int i = 0; i < lines.len; i++) {
        if (ls[i].len == 0)
            continue;
        Map *m = NULL;
        Error err =
            json_unmarshal(l->a, ls[i], BURROW_ANY(TYPE_JSONV2_MAP_STRING_ANY, &m));
        if (BURROW_FAILED(err))
            return slice_nil(TYPE_JSONV2_MAP_STRING_ANY); /* in a real test, fatal */
        ms = slice_append(l->a, ms, &m, 1);
    }
    return ms;
}

static void TestJSONHandler(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogJSONHandler *h =
        slog_new_json_handler(a, bytes_buffer_as_io_writer(&buf), NULL);

    Lines l = {a, &buf};
    Error err = slogtest_test_handler(slog_json_handler_as_handler(h),
                                      BURROW_FN(SlogtestResultsFunc, parse_lines, &l));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    fmt_println_v("the JSON handler passes");
    arena_free(&ar);
}
// doc: end

// doc: broken
/* A handler with a bug: it leaves the time out of every record. */
static SlogAttr drop_time(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    (void)a;
    if (groups.len == 0 && str_eq(attr.key, SLOG_TIME_KEY))
        return (SlogAttr){0};
    return attr;
}

static void TestNoTime(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogHandlerOptions opts = {0};
    opts.replace_attr = drop_time;
    SlogJSONHandler *h =
        slog_new_json_handler(a, bytes_buffer_as_io_writer(&buf), &opts);

    Lines l = {a, &buf};
    Error err = slogtest_test_handler(slog_json_handler_as_handler(h),
                                      BURROW_FN(SlogtestResultsFunc, parse_lines, &l));

    /* One line for each check that failed, ending with the file and line of
     * the case, which are left off here. */
    Str rest = error_text(err);
    while (rest.len > 0) {
        Str source = BURROW_STR_EMPTY;
        bool found = false;
        Str line = strings_cut(rest, BURROW_S("\n"), &rest, &found);
        fmt_println_v(strings_cut(line, BURROW_S(" ("), &source, &found));
    }
    arena_free(&ar);
}
// doc: end

#define TESTS(X) X(TestJSONHandler) X(TestNoTime)
TESTING_MAIN(TESTS)

/* Output:
the JSON handler passes
missing key "time": this test expects slog.TimeKey, slog.LevelKey and slog.MessageKey
missing key "time": a Handler should handle the WithGroup method
missing key "time": a Handler should handle multiple WithGroup and WithAttr calls
missing key "time": a Handler should not output groups if there are no attributes
missing key "time": a Handler should not output nested groups if there are no attributes
PASS
*/
