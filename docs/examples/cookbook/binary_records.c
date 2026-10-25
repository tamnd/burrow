// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/encoding/binary.h"
#include "burrow/encoding/hex.h"

/* One reading from a sensor: when, in Unix seconds, the temperature in tenths
 * of a degree, and the humidity in percent. Every field has a fixed size, so
 * a record is always 8 bytes. */
#define READING_FIELDS(F, T)                                                           \
    F(T, uint32_t, Time, "")                                                           \
    F(T, int16_t, Temp, "")                                                            \
    F(T, uint16_t, Humidity, "")
BURROW_STRUCT(Reading, READING_FIELDS);

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Pick a byte order and use the same one to read. Big endian is the usual
     * choice for files and for the network. */
    Reading in[] = {{1700000000, 215, 40}, {1700000060, -32, 55}, {1700000120, 7, 61}};
    Error err = NO_ERROR;
    OsFile *f = os_create(a, S("readings.bin"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    for (Int i = 0; i < 3 && !FAILED(err); i++)
        err = binary_write(a, os_file_as_io_writer(f), binary_big_endian,
                           ANY(TYPE_OF(Reading), &in[i]));
    os_file_close(f);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }

    Slice data = os_read_file(a, S("readings.bin"), &err);
    Int size = binary_size(ANY(TYPE_OF(Reading), &in[0]));
    fmt_printf_v("%d bytes, %d a record\n", data.len, size);
    fmt_print_v(hex_dump(a, data));

    /* Read records until the file runs out. A file that stops partway through
     * a record gives io_err_unexpected_eof instead of io_eof, so the two can
     * be told apart. */
    f = os_open(a, S("readings.bin"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    for (;;) {
        Reading r;
        err = binary_read(a, os_file_as_io_reader(f), binary_big_endian,
                          ANY(TYPE_OF(Reading), &r));
        if (FAILED(err))
            break;
        Int t = r.Time, temp = r.Temp, humidity = r.Humidity;
        fmt_printf_v("%d: %.1f C, %d%%\n", t, (double)temp / 10, humidity);
    }
    bool at_end = errors_is(err, io_eof);
    fmt_printf_v("stopped at the end of the file: %t\n", at_end);
    os_file_close(f);
    os_remove(S("readings.bin"));
    arena_free(&ar);
}
// doc: end

/* Output:
24 bytes, 8 a record
00000000  65 53 f1 00 00 d7 00 28  65 53 f1 3c ff e0 00 37  |eS.....(eS.<...7|
00000010  65 53 f1 78 00 07 00 3d                           |eS.x...=|
1700000000: 21.5 C, 40%
1700000060: -3.2 C, 55%
1700000120: 0.7 C, 61%
stopped at the end of the file: true
*/
