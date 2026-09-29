#include "burrow/image/color.h"
#include "burrow/burrow.h"
#include "burrow/image/color/palette.h"

/* A model of our own: every color comes out as the gray of its green. */
static Color green_gray(void *env, Color c) {
    (void)env;
    return color_gray16_as_color((ColorGray16){(uint16_t)color_rgba(c).g});
}

int main(void) {
    // doc: colors
    Color c = color_nrgba_as_color((ColorNRGBA){0xff, 0x80, 0x00, 0x80});
    ColorRGBAValue v = color_rgba(c);
    fmt_printf_v("%v is %#x %#x %#x %#x premultiplied\n",
                 BURROW_ANY(TYPE_OF(Color), &c), v.r, v.g, v.b, v.a);
    // doc: end

    // doc: models
    Color g = color_model_convert(color_gray_model, c);
    Color y = color_model_convert(color_y_cb_cr_model, c);
    fmt_printf_v("gray %v, ycbcr %+v\n", BURROW_ANY(TYPE_OF(Color), &g),
                 BURROW_ANY(TYPE_OF(Color), &y));
    if (g.vt->self_type == TYPE_OF(ColorGray))
        fmt_printf_v("the gray is %d\n", g.data.gray.y);
    // doc: end

    // doc: func
    ColorModelFunc f = {green_gray, NULL};
    ColorModel m = color_model_func(&f);
    Color out = color_model_convert(m, c);
    fmt_printf_v("%#v\n", BURROW_ANY(TYPE_OF(Color), &out));
    // doc: end

    // doc: palette
    Color sky = color_rgba_as_color((ColorRGBA){0x40, 0x90, 0xe0, 0xff});
    Int i = color_palette_index(palette_web_safe, sky);
    Color near = color_palette_convert(palette_plan9, sky);
    Color safe = ((const Color *)palette_web_safe.p)[i];
    fmt_printf_v("web-safe %d is %v, closest in Plan 9 is %v\n", i,
                 BURROW_ANY(TYPE_OF(Color), &safe), BURROW_ANY(TYPE_OF(Color), &near));
    // doc: end
    return 0;
}

/* Output:
{255 128 0 128} is 0x8080 0x4080 0x0 0x8080 premultiplied
gray {76}, ycbcr {Y:76 Cb:85 Cr:165}
the gray is 76
color.Gray16{Y:0x4080}
web-safe 58 is {51 153 204 255}, closest in Plan 9 is {73 147 221 255}
*/
