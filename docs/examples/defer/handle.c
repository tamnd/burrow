#include <stdio.h>

#include "burrow/burrow.h"

BURROW_SENTINEL_ERROR(err_cannot_open, "cannot open the file");

static int open_files;
static int most_open;

// fopen and fclose, counting, so main can say whether the defer closed it.
static FILE *open_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (f != NULL && ++open_files > most_open)
        most_open = open_files;
    return f;
}

static void close_file(void *f) {
    fclose(f);
    open_files--;
}

// doc: handle
static Error count_lines(const char *path, Int *lines) {
    BURROW_SCOPE {
        FILE *f = open_file(path);
        if (f == NULL)
            return err_cannot_open;
        BURROW_DEFER(close_file, f);

        for (int c; (c = fgetc(f)) != EOF;)
            if (c == '\n')
                ++*lines;
    }
    BURROW_SCOPE_END;

    return BURROW_NO_ERROR;
}
// doc: end

int main(void) {
    FILE *f = fopen("notes.txt", "w");
    fputs("one\ntwo\nthree\n", f);
    fclose(f);

    Int lines = 0;
    Error err = count_lines("notes.txt", &lines);
    printf("%lld lines, failed: %s, files open: %d\n", (long long)lines,
           BURROW_FAILED(err) ? "yes" : "no", open_files);

    err = count_lines("missing.txt", &lines);
    Str msg = error_message(err);
    printf("%.*s, files open: %d\n", (int)msg.len, (const char *)msg.p, open_files);
    return 0;
}

/* Output:
3 lines, failed: no, files open: 0
cannot open the file, files open: 0
*/
