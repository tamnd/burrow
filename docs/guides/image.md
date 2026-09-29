# Images

Go's image packages are coming over one at a time. `burrow/image.h`, `burrow/image/color.h`, `burrow/image/color/palette.h` and `burrow/image/draw.h` are done. The PNG, GIF and JPEG codecs come next.

## image/color

`burrow/image/color.h` is Go's `image/color`: the color types, the models that convert between them, the Y'CbCr and CMYK conversions, and palettes.

### Colors

Each of Go's color types is a small struct here, with the Go name after `Color`: `ColorRGBA`, `ColorNRGBA`, `ColorGray16`, `ColorYCbCr` and the rest. The fields are Go's in lower case, so Go's `c.R` is `c.r`, and `NYCbCrA`'s embedded `YCbCr` is the field `y_cb_cr`.

A `Color` is Go's `color.Color` interface, and holds any of them. Each type has a function that turns one into a `Color`, and `color_rgba` asks a `Color` for its alpha-premultiplied red, green, blue and alpha in [0, 0xffff]. Go's `RGBA` has four results, and here they come back as one `ColorRGBAValue`:

<!-- example: ../examples/image/color.c#colors -->
```c
Color c = color_nrgba_as_color((ColorNRGBA){0xff, 0x80, 0x00, 0x80});
ColorRGBAValue v = color_rgba(c);
fmt_printf_v("%v is %#x %#x %#x %#x premultiplied\n",
             BURROW_ANY(TYPE_OF(Color), &c), v.r, v.g, v.b, v.a);
```

That prints:

```
{255 128 0 128} is 0x8080 0x4080 0x0 0x8080 premultiplied
```

Every other interface in burrow is a vtable and a pointer to the value. A `Color` keeps the value itself, inline, in a 16 byte union after the vtable. Go boxes a small value when it goes into an interface, and `image`'s `At` hands one back for every pixel it is asked about, so a `Color` that pointed somewhere would need an allocation for each pixel. All of the package's colors fit in 8 bytes. A color type of your own can keep up to 16 bytes there too, or a pointer in `data.ptr`. The first member of its vtable is its type descriptor, as with any interface.

`fmt` knows about the inline value, so a `Color` prints the way Go prints it with `%v`, `%+v` and `%#v`.

### Models

A model converts any color to one of its own type. The package's eleven models are `color_rgba_model`, `color_gray_model`, `color_y_cb_cr_model` and so on, and `color_model_convert` applies one:

<!-- example: ../examples/image/color.c#models -->
```c
Color g = color_model_convert(color_gray_model, c);
Color y = color_model_convert(color_y_cb_cr_model, c);
fmt_printf_v("gray %v, ycbcr %+v\n", BURROW_ANY(TYPE_OF(Color), &g),
             BURROW_ANY(TYPE_OF(Color), &y));
if (g.vt->self_type == TYPE_OF(ColorGray))
    fmt_printf_v("the gray is %d\n", g.data.gray.y);
```

That prints:

```
gray {76}, ycbcr {Y:76 Cb:85 Cr:165}
the gray is 76
```

The last two lines are Go's type assertion `g.(color.Gray)`: compare the vtable's type with the descriptor you want, and read the matching member of `data`. A model hands a color that is already its type back unchanged, as Go's do.

`color_model_func` is Go's `ModelFunc`, a model that calls a function of yours. The model keeps a pointer to the function value, so the function value has to outlive it:

<!-- example: ../examples/image/color.c#func -->
```c
ColorModelFunc f = {green_gray, NULL};
ColorModel m = color_model_func(&f);
Color out = color_model_convert(m, c);
fmt_printf_v("%#v\n", BURROW_ANY(TYPE_OF(Color), &out));
```

That prints:

```
color.Gray16{Y:0x4080}
```

`color_rgb_to_y_cb_cr`, `color_y_cb_cr_to_rgb`, `color_rgb_to_cmyk` and `color_cmyk_to_rgb` are Go's conversion functions, and they return the package's struct for the three or four values rather than several results.

### Palettes

A `ColorPalette` is a slice of `Color`. `color_palette_index` finds the entry nearest to a color, `color_palette_convert` returns it, and `color_palette_as_model` makes a model out of a palette. `burrow/image/color/palette.h` has Go's two palettes, `palette_plan9` and `palette_web_safe`:

<!-- example: ../examples/image/color.c#palette -->
```c
Color sky = color_rgba_as_color((ColorRGBA){0x40, 0x90, 0xe0, 0xff});
Int i = color_palette_index(palette_web_safe, sky);
Color near = color_palette_convert(palette_plan9, sky);
Color safe = ((const Color *)palette_web_safe.p)[i];
fmt_printf_v("web-safe %d is %v, closest in Plan 9 is %v\n", i,
             BURROW_ANY(TYPE_OF(Color), &safe), BURROW_ANY(TYPE_OF(Color), &near));
```

That prints:

```
web-safe 58 is {51 153 204 255}, closest in Plan 9 is {73 147 221 255}
```

An empty palette converts every color to a nil `Color`, which is what Go's does too.

### How close it is to Go

Every function, type and variable of both packages is here. The tests compare against transcripts of Go's own package: `RGBA` for every value of the one-channel types and random values of the rest, every model on colors of every type, both palettes and a small mixed one on random colors, and what `fmt` prints for each type. `RGBToYCbCr`, `YCbCrToRGB`, `RGBToCMYK` and `YCbCr.RGBA` are checked on all 2^24 of their inputs, and `CMYKToRGB` and `NYCbCrA.RGBA` on one input in every 257, by comparing a digest with the one Go gives.

The two differences in shape are the ones above: several results come back as one struct, and `ModelFunc` takes its function value by pointer.

## image

`burrow/image.h` is Go's `image`: points and rectangles, the image types that keep their pixels in memory, and the registry of formats that `image_decode` picks from.

### Points and rectangles

`ImagePoint` and `ImageRectangle` are Go's `Point` and `Rectangle`, and `image_pt` and `image_rect` are `image.Pt` and `image.Rect`. Their methods are functions with the type's prefix, and `fmt` prints them with Go's `String`:

<!-- example: ../examples/image/image.c#geometry -->
```c
ImageRectangle r = image_rect(0, 0, 4, 3);
ImageRectangle i = image_rectangle_intersect(r, image_rect(2, 1, 8, 8));
fmt_printf_v("%v %d %d %v\n", BURROW_ANY(TYPE_OF(ImageRectangle), &r),
             image_rectangle_dx(r), image_rectangle_dy(r),
             BURROW_ANY(TYPE_OF(ImageRectangle), &i));
ImagePoint p = image_pt(3, 2);
fmt_printf_v("%t %t\n", image_point_in(p, r),
             image_point_in(image_point_add(p, image_pt(1, 1)), r));
```

That prints:

```
(0,0)-(4,3) 4 3 (2,1)-(4,3)
true false
```

### Images in memory

There is a type for each of Go's pixel layouts: `ImageRGBA`, `ImageRGBA64`, `ImageNRGBA`, `ImageNRGBA64`, `ImageAlpha`, `ImageAlpha16`, `ImageGray`, `ImageGray16`, `ImageCMYK`, `ImagePaletted`, `ImageYCbCr` and `ImageNYCbCrA`. `image_new_rgba` and the rest allocate one, and the matching `_free` gives it back. The fields are Go's, so `pix`, `stride` and `rect` are there to read and write directly.

The functions that read or write one pixel take the concrete type, so a loop over them makes no indirect calls. A sub-image comes back by value and shares its pixels with the image it came from, as in Go:

<!-- example: ../examples/image/image.c#pixels -->
```c
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
```

That prints:

```
(1,1)-(3,3) 16 {128 128 128 255} {255 0 0 255} false
```

A paletted image keeps a byte per pixel and a `ColorPalette`, and setting a color stores the index of the nearest entry:

<!-- example: ../examples/image/image.c#paletted -->
```c
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
```

That prints:

```
1 {65535} 0
```

### The Image interface

An `Image` is Go's `image.Image`, a vtable and a pointer to the image, and each type has an `_as_image` function that makes one. `image_at`, `image_bounds` and `image_color_model` call through it.

Go code that is handed an `Image` often asks it for more with a type assertion, such as `RGBA64At`, `Set` or `Opaque`, and `image/draw` does that on every call. The vtable has a slot for each of those methods, and a slot is NULL when the image does not have that method. `ImageRGBA64Image` and `ImagePalettedImage` are other names for `Image`, used where the vtable promises the extra slot.

`image_new_uniform` is Go's `Uniform`, an image of one color that goes on forever, and `image_black`, `image_white`, `image_transparent` and `image_opaque` are Go's four. `image_rectangle_as_image` makes a rectangle into the mask image Go gets from a `Rectangle`.

### Formats

`image_register_format` adds a format, given its name, the magic bytes it starts with (`?` matches any byte) and a decoder and config decoder as function values. `image_decode` and `image_decode_config` look at the first bytes of the input, pick the format registered first that matches, and report its name. PNG, GIF and JPEG register themselves once they are here. This example registers a made-up format, the full decoders are at the top of `docs/examples/image/image.c`:

<!-- example: ../examples/image/image.c#formats -->
```c
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
```

That prints:

```
tiny 3 2 true
tiny (0,0)-(3,2) {80} true
image: unknown format true
```

The input is wrapped in a `bufio` reader unless it is one already, as in Go, so the magic bytes can be peeked without being lost. An input that matches no format fails with `image_err_format`.

### How close it is to Go

Every function, type and variable in the package is here. The tests compare against transcripts of Go's package: hundreds of random points and rectangles through every method, each image type on random rectangles with its pixels, reads, sub-images and `Opaque`, every Y'CbCr subsample ratio including the rectangles where Go panics and the message it panics with, the constructors' panics on bad sizes, and what `fmt` prints for each type.

The differences in shape are small. `SubImage` returns the concrete type rather than an `Image`, `Uniform`'s `RGBA` returns one struct like the color types do, and `RegisterFormat` takes its decoders as function values that can carry state.

## image/draw

`burrow/image/draw.h` is Go's `image/draw`: drawing one image onto another, with an optional mask, using the Porter-Duff Src and Over operators.

### Draw and DrawMask

`draw_draw` lines up `r.min` in the destination with `sp` in the source and draws the source over the rectangle `r`. With `DRAW_SRC` the source replaces what was there, and with `DRAW_OVER` it is blended on top. The destination is an `Image` whose `set` slot is filled in, which every image type in `image` has:

<!-- example: ../examples/image/draw.c#draw -->
```c
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
```

That prints:

```
{255 255 255 255} {255 127 127 255} {255 127 127 255} {255 255 255 255}
```

`draw_draw_mask` takes a mask as well, lined up at `mp`, and scales the source by the mask's alpha. A mask of `(Image){0}` is Go's nil mask and lets everything through:

<!-- example: ../examples/image/draw.c#mask -->
```c
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
```

That prints:

```
{255 255 255 255} {170 84 84 255} {85 42 42 255} {0 0 0 255}
```

### Drawers

`DrawDrawer` is Go's `Drawer` interface, and `draw_drawer_draw` calls it. `draw_op_as_drawer` makes one from a `DrawOp`, and `draw_floyd_steinberg` is Go's `FloydSteinberg`, which draws with error diffusion. Onto a paletted image it picks the nearest palette entry for each pixel and carries the error forward, so a gray ramp drawn onto black and white comes out dithered:

<!-- example: ../examples/image/draw.c#dither -->
```c
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
```

That prints:

```
00010111
```

### How close it is to Go

Every function, type and variable in the package is here. The fast paths for each pair of image types are Go's, with Go's integer arithmetic, and the tests run ten thousand random draws against a transcript of Go's package: every destination type, every source type including Y'CbCr at each subsample ratio, uniforms and images that have only the three `Image` methods, every mask type, both operators, random rectangles and points that clip in every direction, and `FloydSteinberg` onto random palettes. The pixels come out byte for byte the same, and the one case where Go panics panics with the same message.

`Quantizer` is here as `DrawQuantizer` for code that takes one, as in Go, where nothing in the standard library implements it.
