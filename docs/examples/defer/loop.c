#include <stdio.h>

#include "burrow/burrow.h"

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

int main(void) {
    const char *paths[] = {"a.txt", "missing.txt", "b.txt", "c.txt"};
    Int n = 4;
    for (Int i = 0; i < n; i++) {
        if (i != 1) {
            FILE *f = fopen(paths[i], "w");
            fputs(paths[i], f);
            fclose(f);
        }
    }

    char line[64];
    // doc: loop
    for (Int i = 0; i < n; i++) {
        BURROW_SCOPE {
            FILE *f = open_file(paths[i]);
            if (f == NULL)
                continue;
            BURROW_DEFER(close_file, f);

            if (fgets(line, sizeof line, f) != NULL)
                printf("%s says %s\n", paths[i], line);
        }
        BURROW_SCOPE_END;
    }
    // doc: end

    printf("most files open at once: %d, still open: %d\n", most_open, open_files);
    return 0;
}

/* Output:
a.txt says a.txt
b.txt says b.txt
c.txt says c.txt
most files open at once: 1, still open: 0
*/
