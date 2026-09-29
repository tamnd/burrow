#include "burrow/image/png.h"
#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

int main(void) {
    Alloc *a = heap_allocator();

    // doc: encode
    ImageGray *g = image_new_gray(a, image_rect(0, 0, 16, 16));
    for (Int y = 0; y < 16; y++)
        for (Int x = 0; x < 16; x++)
            image_gray_set_gray(g, x, y, (ColorGray){(uint8_t)(x * 16 + y)});
    BytesBuffer out = BYTES_BUFFER(a);
    Error err = png_encode(a, bytes_buffer_as_io_writer(&out), image_gray_as_image(g));
    if (BURROW_FAILED(err))
        return 1;
    Slice png = bytes_buffer_bytes(&out);
    fmt_printf_v("%d bytes, starting % x\n", png.len, slice_sub(png, 0, 8));
    image_gray_free(g, a);
    // doc: end

    // doc: decode
    png_register();
    BytesReader r;
    bytes_reader_reset(&r, png);
    Str format;
    Image m = image_decode(a, bytes_reader_as_io_reader(&r), &format, &err);
    if (BURROW_FAILED(err))
        return 1;
    ImageGray *back = (ImageGray *)m.data;
    ImageRectangle b = image_gray_bounds(back);
    fmt_printf_v("%s %v, pixel (3, 5) is %d\n", format,
                 BURROW_ANY(TYPE_OF(ImageRectangle), &b),
                 image_gray_gray_at(back, 3, 5).y);
    image_decoded_free(m, a);
    // doc: end

    // doc: errors
    ((Byte *)png.p)[20] ^= 1;
    bytes_reader_reset(&r, png);
    m = png_decode(a, bytes_reader_as_io_reader(&r), &err);
    fmt_printf_v("%v\n", err);
    if (errors_as(err, TYPE_PNG_FORMAT_ERROR) != NULL)
        fmt_printf_v("the file is broken\n");
    // doc: end

    bytes_buffer_free(&out);
    return 0;
}

/* Output:
77 bytes, starting 89 50 4e 47 0d 0a 1a 0a
png (0,0)-(16,16), pixel (3, 5) is 53
png: invalid format: invalid checksum
the file is broken
*/
