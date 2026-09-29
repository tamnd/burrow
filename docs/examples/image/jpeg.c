#include "burrow/image/jpeg.h"
#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

int main(void) {
    Alloc *a = heap_allocator();

    // doc: encode
    ImageRGBA *m = image_new_rgba(a, image_rect(0, 0, 64, 48));
    for (Int y = 0; y < 48; y++)
        for (Int x = 0; x < 64; x++)
            image_rgba_set_rgba(
                m, x, y, (ColorRGBA){(uint8_t)(x * 4), (uint8_t)(y * 5), 200, 255});
    BytesBuffer out = BYTES_BUFFER(a);
    for (Int q = 25; q <= 100; q += 25) {
        JpegOptions o = {.quality = q};
        bytes_buffer_reset(&out);
        Error err =
            jpeg_encode(a, bytes_buffer_as_io_writer(&out), image_rgba_as_image(m), &o);
        if (BURROW_FAILED(err))
            return 1;
        fmt_printf_v("quality %d: %d bytes\n", q, bytes_buffer_len(&out));
    }
    // doc: end

    // doc: decode
    BytesReader r;
    bytes_reader_reset(&r, bytes_buffer_bytes(&out));
    Error err = BURROW_NO_ERROR;
    Image back = jpeg_decode(a, bytes_reader_as_io_reader(&r), &err);
    if (BURROW_FAILED(err))
        return 1;
    ImageYCbCr *ycc = (ImageYCbCr *)back.data;
    ColorRGBAValue c = color_rgba(image_at(back, 40, 20));
    ImageRectangle b = image_bounds(back);
    fmt_printf_v("(%d,%d)-(%d,%d), subsampling %s\n", b.min.x, b.min.y, b.max.x,
                 b.max.y, image_y_cb_cr_subsample_ratio_string(ycc->subsample_ratio));
    fmt_printf_v("pixel (40, 20) is %d %d %d, was 160 100 200\n", c.r >> 8, c.g >> 8,
                 c.b >> 8);
    image_decoded_free(back, a);
    image_rgba_free(m, a);
    // doc: end

    // doc: config
    jpeg_register();
    bytes_reader_reset(&r, bytes_buffer_bytes(&out));
    Str format;
    ImageConfig cfg =
        image_decode_config(a, bytes_reader_as_io_reader(&r), &format, &err);
    if (BURROW_FAILED(err))
        return 1;
    fmt_printf_v("%s, %dx%d\n", format, cfg.width, cfg.height);
    // doc: end

    // doc: errors
    bytes_reader_reset(&r, slice_sub(bytes_buffer_bytes(&out), 2, 40));
    back = jpeg_decode(a, bytes_reader_as_io_reader(&r), &err);
    fmt_printf_v("%v\n", err);
    const JpegFormatError *fe = errors_as(err, TYPE_JPEG_FORMAT_ERROR);
    fmt_printf_v("format error: %s\n", *fe);
    bytes_reader_reset(&r, slice_sub(bytes_buffer_bytes(&out), 0, 300));
    back = jpeg_decode(a, bytes_reader_as_io_reader(&r), &err);
    fmt_printf_v("%v\n", err);
    // doc: end

    bytes_buffer_free(&out);
    return 0;
}

/* Output:
quality 25: 741 bytes
quality 50: 774 bytes
quality 75: 870 bytes
quality 100: 1662 bytes
(0,0)-(64,48), subsampling YCbCrSubsampleRatio420
pixel (40, 20) is 160 100 198, was 160 100 200
jpeg, 64x48
invalid JPEG format: missing SOI marker
format error: missing SOI marker
unexpected EOF
*/
