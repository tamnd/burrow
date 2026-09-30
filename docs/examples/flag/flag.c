#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

// doc: stdout
/* Standard output as an IoWriter, for a set whose messages should go there
 * rather than to standard error. */
static Int stdout_write(void *self, Slice p, Error *err) {
    (void)self;
    if (err)
        *err = BURROW_NO_ERROR;
    return (Int)fwrite(p.p, 1, (size_t)p.len, stdout);
}

static const IoWriterVT stdout_vt = {NULL, stdout_write};
static const IoWriter to_stdout = {&stdout_vt, NULL};
// doc: end

// doc: value
/* A list of durations, set from a comma separated list, which is the
 * user-defined flag type in Go's own example. */
typedef struct Interval {
    Duration d[8];
    Int n;
} Interval;

static Str interval_string(void *self, Alloc *a) {
    Interval *iv = self;
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < iv->n; i++) {
        if (i > 0)
            strings_builder_write_byte(&b, ',');
        Byte buf[DURATION_STRING_MAX];
        Int n = duration_format(iv->d[i], buf);
        strings_builder_write(&b, slice_from(buf, n, n, TYPE_BYTE), NULL);
    }
    return strings_builder_string(&b);
}

static Error interval_set(void *self, Str value) {
    Interval *iv = self;
    if (iv->n > 0)
        return errors_new(error_allocator(), BURROW_S("interval flag already set"));
    while (value.len > 0) {
        Str rest;
        bool found;
        Str dt = strings_cut(value, BURROW_S(","), &rest, &found);
        Error err;
        Duration d = time_parse_duration(dt, &err);
        if (!BURROW_OK(err))
            return err;
        if (iv->n == 8)
            return errors_new(error_allocator(), BURROW_S("too many intervals"));
        iv->d[iv->n++] = d;
        value = rest;
    }
    return BURROW_NO_ERROR;
}

static const FlagValueVT interval_vt = {NULL, interval_string, interval_set, NULL,
                                        NULL};
// doc: end

// doc: func
static Error add_include(void *env, Str dir) {
    Int *count = env;
    printf("include %.*s\n", (int)dir.len, dir.p);
    (*count)++;
    return BURROW_NO_ERROR;
}
// doc: end

int main(void) {
    {
        // doc: set
        FlagFlagSet *fs =
            flag_new_flag_set(NULL, BURROW_S("serve"), FLAG_CONTINUE_ON_ERROR);
        Int *port = flag_flag_set_int(fs, BURROW_S("port"), 8080,
                                      BURROW_S("port to listen on"));
        bool verbose;
        flag_flag_set_bool_var(fs, &verbose, BURROW_S("v"), false,
                               BURROW_S("log every request"));
        Duration *timeout =
            flag_flag_set_duration(fs, BURROW_S("timeout"), 30 * TIME_SECOND,
                                   BURROW_S("how long a request may take"));

        Str argv[] = {BURROW_S("-port=9000"), BURROW_S("-v"),
                      BURROW_S("--timeout"),  BURROW_S("1m30s"),
                      BURROW_S("site"),       BURROW_S("-not-a-flag")};
        Error err = flag_flag_set_parse(fs, slice_from(argv, 6, 6, TYPE_STRING));
        if (!BURROW_OK(err))
            return 1;

        printf("port=%lld v=%d timeout=%lld\n", (long long)*port, verbose,
               (long long)(*timeout / TIME_SECOND)); /* port=9000 v=1 timeout=90 */
        for (Int i = 0; i < flag_flag_set_n_arg(fs); i++) {
            Str arg = flag_flag_set_arg(fs, i);
            printf("arg %.*s\n", (int)arg.len, arg.p);
        }
        flag_flag_set_free(fs);
        // doc: end
    }

    {
        // doc: use-value
        FlagFlagSet *fs =
            flag_new_flag_set(NULL, BURROW_S("events"), FLAG_CONTINUE_ON_ERROR);
        Interval iv = {{0}, 0};
        flag_flag_set_var(
            fs, (FlagValue){&interval_vt, &iv}, BURROW_S("deltaT"),
            BURROW_S("comma-separated list of intervals to use between events"));

        Str argv[] = {BURROW_S("-deltaT"), BURROW_S("10s,1m,250ms")};
        Error err = flag_flag_set_parse(fs, slice_from(argv, 2, 2, TYPE_STRING));
        if (!BURROW_OK(err))
            return 1;
        printf(
            "%d intervals, the last %lldms\n", (int)iv.n,
            (long long)(iv.d[2] / TIME_MILLISECOND)); /* 3 intervals, the last 250ms */
        flag_flag_set_free(fs);
        // doc: end
    }

    {
        // doc: use-func
        FlagFlagSet *fs =
            flag_new_flag_set(NULL, BURROW_S("cc"), FLAG_CONTINUE_ON_ERROR);
        Int count = 0;
        flag_flag_set_func(fs, BURROW_S("I"), BURROW_S("add a `directory` to search"),
                           BURROW_FN(FlagFunc, add_include, &count));

        Str argv[] = {BURROW_S("-I"), BURROW_S("include"), BURROW_S("-I=vendor")};
        (void)flag_flag_set_parse(fs, slice_from(argv, 3, 3, TYPE_STRING));
        printf("%d directories\n", (int)count);
        flag_flag_set_free(fs);
        // doc: end
    }

    {
        // doc: errors
        FlagFlagSet *fs =
            flag_new_flag_set(NULL, BURROW_S("serve"), FLAG_CONTINUE_ON_ERROR);
        flag_flag_set_set_output(fs, to_stdout);
        flag_flag_set_int(fs, BURROW_S("port"), 8080, BURROW_S("port to listen on"));
        flag_flag_set_string(fs, BURROW_S("root"), BURROW_S("."),
                             BURROW_S("serve files from `dir`"));

        Str argv[] = {BURROW_S("-port=http")};
        Error err = flag_flag_set_parse(fs, slice_from(argv, 1, 1, TYPE_STRING));
        Str msg = error_text(err);
        printf("err: %.*s\n", (int)msg.len, msg.p);
        flag_flag_set_free(fs);
        // doc: end
    }

    {
        // doc: command-line
        Int *n = flag_int(BURROW_S("n"), 3, BURROW_S("how many times"));
        flag_parse();
        printf("n=%lld with %lld arguments\n", (long long)*n, (long long)flag_n_arg());
        // doc: end
    }
    return 0;
}

/* Output:
port=9000 v=1 timeout=90
arg site
arg -not-a-flag
3 intervals, the last 250ms
include include
include vendor
2 directories
invalid value "http" for flag -port: parse error
Usage of serve:
  -port int
    	port to listen on (default 8080)
  -root dir
    	serve files from dir (default ".")
err: invalid value "http" for flag -port: parse error
n=3 with 0 arguments
*/
