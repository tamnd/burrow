#include "burrow/image/draw.h"
#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

int main(void) {
    Alloc *a = heap_allocator();

    // doc: draw
    ImageRGBA *dst = image_new_rgba(a, image_rect(0, 0, 4, 1));
    draw_draw(image_rgba_as_image(dst), image_rgba_bounds(dst),
              image_uniform_as_image(image_white), image_zp, DRAW_SRC);
    ImageUniform *red =
        image_new_uniform(a, color_nrgba_as_color((ColorNRGBA){0xff, 0, 0, 0x80}));
    draw_draw(image_rgba_as_image(dst), image_rect(1, 0, 3, 1),
              image_uniform_as_image(red), image_zp, DRAW_OVER);
    for (Int x = 0; x < 4; x++) {
        ColorRGBA c = image_rgba_rgba_at(dst, x, 0);
        fmt_printf_v(x < 3 ? "%v " : "%v\n", BURROW_ANY(TYPE_OF(ColorRGBA), &c));
    }
    // doc: end

    // doc: mask
    ImageAlpha *mask = image_new_alpha(a, image_rect(0, 0, 4, 1));
    for (Int x = 0; x < 4; x++)
        image_alpha_set_alpha(mask, x, 0, (ColorAlpha){(uint8_t)(x * 0x55)});
    draw_draw_mask(image_rgba_as_image(dst), image_rgba_bounds(dst),
                   image_uniform_as_image(image_black), image_zp,
                   image_alpha_as_image(mask), image_zp, DRAW_OVER);
    for (Int x = 0; x < 4; x++) {
        ColorRGBA c = image_rgba_rgba_at(dst, x, 0);
        fmt_printf_v(x < 3 ? "%v " : "%v\n", BURROW_ANY(TYPE_OF(ColorRGBA), &c));
    }
    image_alpha_free(mask, a);
    // doc: end

    // doc: dither
    ImageGray *ramp = image_new_gray(a, image_rect(0, 0, 8, 1));
    for (Int x = 0; x < 8; x++)
        image_gray_set_gray(ramp, x, 0, (ColorGray){(uint8_t)(x * 32)});
    Color bw[2] = {color_gray16_as_color(color_black),
                   color_gray16_as_color(color_white)};
    ImagePaletted *p = image_new_paletted(a, image_rect(0, 0, 8, 1),
                                          slice_from(bw, 2, 2, TYPE_OF(Color)));
    draw_drawer_draw(draw_floyd_steinberg, image_paletted_as_image(p),
                     image_paletted_bounds(p), image_gray_as_image(ramp), image_zp);
    for (Int x = 0; x < 8; x++)
        fmt_printf_v("%d", image_paletted_color_index_at(p, x, 0));
    fmt_printf_v("\n");
    image_paletted_free(p, a);
    image_gray_free(ramp, a);
    // doc: end

    image_uniform_free(red, a);
    image_rgba_free(dst, a);
    return 0;
}

/* Output:
{255 255 255 255} {255 127 127 255} {255 127 127 255} {255 255 255 255}
{255 255 255 255} {170 84 84 255} {85 42 42 255} {0 0 0 255}
00010111
*/
