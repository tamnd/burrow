# Read a file of JSON lines

A file with one JSON value on each line can be read with a single `JsonDecoder`, one value per call, so the whole file never has to be in memory. A field missing from a line is left alone, so each event starts from zero. The decoder returns `io_eof` at a clean end and another error when the input stops partway through a value.

<!-- example: ../examples/cookbook/json_stream.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

#define EVENT_FIELDS(F, T)                                                             \
    F(T, Str, User, "json:\"user\"")                                                   \
    F(T, Str, Action, "json:\"action\"")                                               \
    F(T, Int, Bytes, "json:\"bytes\"")
BURROW_STRUCT(Event, EVENT_FIELDS);

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* One JSON value to a line, and the last line was cut off when whatever
     * wrote the file stopped partway. */
    Error err = os_write_file(
        S("events.jsonl"),
        BURROW_B("{\"user\":\"ada\",\"action\":\"upload\",\"bytes\":1200}\n"
                 "{\"user\":\"bob\",\"action\":\"upload\",\"bytes\":300}\n"
                 "{\"user\":\"ada\",\"action\":\"delete\"}\n"
                 "{\"user\":\"ada\",\"action\":\"upload\",\"bytes\":800}\n"
                 "{\"user\":\"cy\",\"act"),
        0644);
    OsFile *f = os_open(a, S("events.jsonl"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }

    /* The decoder reads one value at a time, so the file never has to fit in
     * memory. A field missing from a line is left as it was, so start each
     * event from zero. */
    JsonDecoder *dec = json_new_decoder(a, os_file_as_io_reader(f));
    Map *bytes = map_make(a, TYPE_STRING, TYPE_INT, 0);
    Int n = 0;
    for (;;) {
        Event e = {0};
        err = json_decoder_decode(dec, ANY(TYPE_OF(Event), &e));
        if (FAILED(err))
            break;
        n++;
        Int *total = MAP_GET(Str, Int, bytes, e.User);
        MAP_SET(Str, Int, bytes, e.User, (total != NULL ? *total : 0) + e.Bytes);
    }

    /* io_eof is a clean end. Anything else means the input was bad. */
    if (!errors_is(err, io_eof))
        fmt_printf_v("stopped after %d events: %v\n", n, err);
    fmt_println_v(bytes);
    json_decoder_free(dec);
    os_file_close(f);
    os_remove(S("events.jsonl"));
    arena_free(&ar);
}
```

It prints:

```text
stopped after 4 events: unexpected EOF
map[ada:2000 bob:300]
```

The program is [docs/examples/cookbook/json_stream.c](../examples/cookbook/json_stream.c). CI builds and runs it and checks that it prints this.
