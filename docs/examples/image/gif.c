#include "burrow/image/gif.h"
#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

int main(void) {
    Alloc *a = heap_allocator();

    // doc: encode-all
    Color colors[3] = {
        color_rgba_as_color((ColorRGBA){0, 0, 0, 255}),
        color_rgba_as_color((ColorRGBA){255, 0, 0, 255}),
        color_rgba_as_color((ColorRGBA){0, 0, 255, 255}),
    };
    ColorPalette pal = {colors, 3, 3, TYPE_OF(Color)};
    ImagePalettedPtr frames[3];
    Int delays[3];
    for (Int f = 0; f < 3; f++) {
        frames[f] = image_new_paletted(a, image_rect(0, 0, 8, 8), pal);
        for (Int y = 0; y < 8; y++)
            image_paletted_set_color_index(frames[f], (y + f) % 8, y,
                                           1 + (uint8_t)(f % 2));
        delays[f] = 50;
    }
    Gif g = {
        .image = slice_from(frames, 3, 3, TYPE_OF(ImagePalettedPtr)),
        .delay = slice_from(delays, 3, 3, TYPE_INT),
        .loop_count = 0,
        .disposal = slice_nil(TYPE_BYTE),
        .config = {color_palette_as_model(&pal), 8, 8},
    };
    BytesBuffer out = BYTES_BUFFER(a);
    Error err = gif_encode_all(a, bytes_buffer_as_io_writer(&out), &g);
    if (BURROW_FAILED(err))
        return 1;
    Slice data = bytes_buffer_bytes(&out);
    fmt_printf_v("%d bytes, starting %q\n", data.len, str_from_bytes(data.p, 6));
    for (Int f = 0; f < 3; f++)
        image_paletted_free(frames[f], a);
    // doc: end

    // doc: decode-all
    BytesReader r;
    bytes_reader_reset(&r, data);
    Gif *back = gif_decode_all(a, bytes_reader_as_io_reader(&r), &err);
    if (BURROW_FAILED(err))
        return 1;
    fmt_printf_v("%d frames of %dx%d, loop count %d\n", back->image.len,
                 back->config.width, back->config.height, back->loop_count);
    ImagePaletted *second = ((ImagePaletted **)back->image.p)[1];
    fmt_printf_v("frame 1 waits %d/100 s, pixel (1, 0) is index %d\n",
                 ((Int *)back->delay.p)[1],
                 image_paletted_color_index_at(second, 1, 0));
    gif_free(back, a);
    // doc: end

    // doc: encode
    ImageRGBA *m = image_new_rgba(a, image_rect(0, 0, 32, 32));
    for (Int y = 0; y < 32; y++)
        for (Int x = 0; x < 32; x++)
            image_rgba_set_rgba(
                m, x, y, (ColorRGBA){(uint8_t)(x * 8), (uint8_t)(y * 8), 128, 255});
    GifOptions o = {.num_colors = 16};
    bytes_buffer_reset(&out);
    err = gif_encode(a, bytes_buffer_as_io_writer(&out), image_rgba_as_image(m), &o);
    if (BURROW_FAILED(err))
        return 1;
    image_rgba_free(m, a);

    gif_register();
    bytes_reader_reset(&r, bytes_buffer_bytes(&out));
    Str format;
    Image img = image_decode(a, bytes_reader_as_io_reader(&r), &format, &err);
    if (BURROW_FAILED(err))
        return 1;
    ImagePaletted *p = (ImagePaletted *)img.data;
    fmt_printf_v("%s, %d colors\n", format, p->palette.len);
    image_decoded_free(img, a);
    // doc: end

    // doc: errors
    bytes_reader_reset(&r, slice_sub(bytes_buffer_bytes(&out), 0, 40));
    img = gif_decode(a, bytes_reader_as_io_reader(&r), &err);
    fmt_printf_v("%v\n", err);
    // doc: end

    bytes_buffer_free(&out);
    return 0;
}

/* Output:
144 bytes, starting "GIF89a"
3 frames of 8x8, loop count 0
frame 1 waits 50/100 s, pixel (1, 0) is index 2
gif, 16 colors
gif: reading color table: unexpected EOF
*/
