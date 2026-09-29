#include "burrow/image.h"
#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

/* A made-up format: "TINY", a width and a height byte, then the gray pixels. */
static ImageConfig tiny_config(void *env, Alloc *a, IoReader r, Error *err) {
    (void)env;
    (void)a;
    ImageConfig c = {0};
    Byte head[6];
    io_read_full(r, slice_from(head, 6, 6, TYPE_BYTE), err);
    if (BURROW_FAILED(*err))
        return c;
    c.color_model = color_gray_model;
    c.width = head[4];
    c.height = head[5];
    return c;
}

static Image tiny_decode(void *env, Alloc *a, IoReader r, Error *err) {
    ImageConfig c = tiny_config(env, a, r, err);
    if (BURROW_FAILED(*err))
        return (Image){0};
    ImageGray *m = image_new_gray(a, image_rect(0, 0, c.width, c.height));
    io_read_full(r, m->pix, err);
    if (BURROW_FAILED(*err)) {
        image_gray_free(m, a);
        return (Image){0};
    }
    return image_gray_as_image(m);
}

int main(void) {
    Alloc *a = heap_allocator();

    // doc: geometry
    ImageRectangle r = image_rect(0, 0, 4, 3);
    ImageRectangle i = image_rectangle_intersect(r, image_rect(2, 1, 8, 8));
    fmt_printf_v("%v %d %d %v\n", BURROW_ANY(TYPE_OF(ImageRectangle), &r),
                 image_rectangle_dx(r), image_rectangle_dy(r),
                 BURROW_ANY(TYPE_OF(ImageRectangle), &i));
    ImagePoint p = image_pt(3, 2);
    fmt_printf_v("%t %t\n", image_point_in(p, r),
                 image_point_in(image_point_add(p, image_pt(1, 1)), r));
    // doc: end

    // doc: pixels
    ImageRGBA *m = image_new_rgba(a, r);
    image_rgba_set_rgba(m, 1, 1, (ColorRGBA){0xff, 0, 0, 0xff});
    ImageRGBA sub = image_rgba_sub_image(m, image_rect(1, 1, 3, 3));
    image_rgba_set(&sub, 2, 2, color_gray_as_color((ColorGray){0x80}));
    ImageRectangle b = image_rgba_bounds(&sub);
    ColorRGBA px = image_rgba_rgba_at(m, 2, 2);
    Color at = image_rgba_at(m, 1, 1);
    fmt_printf_v("%v %d %v %v %t\n", BURROW_ANY(TYPE_OF(ImageRectangle), &b),
                 sub.stride, BURROW_ANY(TYPE_OF(ColorRGBA), &px),
                 BURROW_ANY(TYPE_OF(Color), &at), image_rgba_opaque(m));
    image_rgba_free(m, a);
    // doc: end

    // doc: paletted
    Color bw[2] = {color_gray16_as_color(color_black),
                   color_gray16_as_color(color_white)};
    ImagePaletted *q = image_new_paletted(a, image_rect(0, 0, 2, 2),
                                          slice_from(bw, 2, 2, TYPE_OF(Color)));
    image_paletted_set(q, 1, 0, color_gray_as_color((ColorGray){200}));
    Color qc = image_paletted_at(q, 1, 0);
    fmt_printf_v("%d %v %d\n", image_paletted_color_index_at(q, 1, 0),
                 BURROW_ANY(TYPE_OF(Color), &qc),
                 image_paletted_color_index_at(q, 0, 0));
    image_paletted_free(q, a);
    // doc: end

    // doc: formats
    image_register_format(BURROW_S("tiny"), BURROW_S("TINY"),
                          (ImageDecodeFunc){tiny_decode, NULL},
                          (ImageDecodeConfigFunc){tiny_config, NULL});
    static const Byte data[] = "TINY\x03\x02\x00\x10\x20\x30\x40\x50";
    Slice s = slice_from((Byte *)data, 12, 12, TYPE_BYTE);
    BytesReader br;
    Str name;
    Error err = BURROW_NO_ERROR;

    bytes_reader_reset(&br, s);
    ImageConfig c = image_decode_config(a, bytes_reader_as_io_reader(&br), &name, &err);
    fmt_printf_v("%s %d %d %t\n", name, c.width, c.height, (bool)BURROW_OK(err));

    bytes_reader_reset(&br, s);
    Image img = image_decode(a, bytes_reader_as_io_reader(&br), &name, &err);
    ImageRectangle ib = image_bounds(img);
    Color ic = image_at(img, 2, 1);
    fmt_printf_v("%s %v %v %t\n", name, BURROW_ANY(TYPE_OF(ImageRectangle), &ib),
                 BURROW_ANY(TYPE_OF(Color), &ic), (bool)BURROW_OK(err));
    image_gray_free(img.data, a);

    bytes_reader_reset(&br, slice_from((Byte *)"GIF89a", 6, 6, TYPE_BYTE));
    image_decode(a, bytes_reader_as_io_reader(&br), &name, &err);
    fmt_printf_v("%s %t\n", error_text(err), errors_is(err, image_err_format));
    // doc: end
    return 0;
}

/* Output:
(0,0)-(4,3) 4 3 (2,1)-(4,3)
true false
(1,1)-(3,3) 16 {128 128 128 255} {255 0 0 255} false
1 {65535} 0
tiny 3 2 true
tiny (0,0)-(3,2) {80} true
image: unknown format true
*/
