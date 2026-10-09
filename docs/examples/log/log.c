#include <stdio.h>

#include "burrow/burrow.h"

#define P(s) (int)(s).len, (const char *)(s).p

// doc: stdout
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
// doc: end

int main(void) {
    Alloc *a = heap_allocator();

    {
        // doc: logger
        BytesBuffer buf = BYTES_BUFFER(a);
        LogLogger *logger = log_new(a, bytes_buffer_as_io_writer(&buf),
                                    BURROW_S("logger: "), LOG_LSHORTFILE);
        log_logger_print_v(logger, "Hello, log file!");
        log_logger_println_v(logger, "opened", 3, "tables");

        Slice out = bytes_buffer_bytes(&buf);
        printf("%.*s", P(out));
        log_logger_free(a, logger);
        bytes_buffer_free(&buf);
        // doc: end
    }

    {
        // doc: std
        log_set_output(to_stdout);
        log_set_flags(LOG_LMSGPREFIX);
        log_set_prefix(BURROW_S("serve: "));
        log_printf_v("listening on %s", "localhost:8080");
        log_print_v("no newline at the end, ", "so one is added");
        log_print_v("one is here already\n");
        // doc: end
    }

    {
        // doc: panic
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
        // doc: end
    }
    return 0;
}

/* Output:
logger: log.c:29: Hello, log file!
logger: log.c:30: opened 3 tables
serve: listening on localhost:8080
serve: no newline at the end, so one is added
serve: one is here already
bad config: 0 servers
recovered: bad config: 0 servers
*/
