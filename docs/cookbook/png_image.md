# Draw a picture and save it as a PNG

`image_new_rgba` makes a picture in memory with one 4 byte pixel for each point, and `png_encode` writes it out. `png_decode_config` reads only the header, which is enough for the size, and `png_decode` reads the whole thing back. A PNG is lossless, so the pixels come back exactly as they were drawn.

<!-- example: ../examples/cookbook/png_image.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/image/png.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A red to blue fade from left to right, with a white bar across the
     * middle. Pixels are addressed from the top left corner. */
    enum { width = 64, height = 32 };
    ImageRGBA *m = image_new_rgba(a, image_rect(0, 0, width, height));
    for (Int y = 0; y < height; y++) {
        for (Int x = 0; x < width; x++) {
            uint8_t t = (uint8_t)(x * 255 / (width - 1));
            ColorRGBA c = {(uint8_t)(255 - t), 0, t, 255};
            if (y >= 14 && y < 18)
                c = (ColorRGBA){255, 255, 255, 255};
            image_rgba_set_rgba(m, x, y, c);
        }
    }

    Error err = NO_ERROR;
    OsFile *f = os_create(a, S("fade.png"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    err = png_encode(a, os_file_as_io_writer(f), image_rgba_as_image(m));
    Error cerr = os_file_close(f);
    if (FAILED(err) || FAILED(cerr)) {
        fmt_println_v(FAILED(err) ? err : cerr);
        return 1;
    }

    /* Reading the header alone gives the size without decoding the pixels. */
    f = os_open(a, S("fade.png"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    ImageConfig cfg = png_decode_config(a, os_file_as_io_reader(f), &err);
    os_file_close(f);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    fmt_printf_v("fade.png is %dx%d\n", cfg.width, cfg.height);

    f = os_open(a, S("fade.png"), &err);
    Image back = png_decode(a, os_file_as_io_reader(f), &err);
    os_file_close(f);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    ImageRGBA *rgba = back.data;
    ColorRGBA left = image_rgba_rgba_at(rgba, 0, 0);
    ColorRGBA right = image_rgba_rgba_at(rgba, width - 1, 0);
    ColorRGBA bar = image_rgba_rgba_at(rgba, 20, 15);
    fmt_printf_v("left %d,%d,%d  right %d,%d,%d  bar %d,%d,%d\n", left.r, left.g,
                 left.b, right.r, right.g, right.b, bar.r, bar.g, bar.b);
    image_decoded_free(back, a);
    os_remove(S("fade.png"));
    arena_free(&ar);
}
```

It prints:

```text
fade.png is 64x32
left 255,0,0  right 0,0,255  bar 255,255,255
```

The program is [docs/examples/cookbook/png_image.c](../examples/cookbook/png_image.c). CI builds and runs it and checks that it prints this.
