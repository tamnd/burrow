# Images

Go's image packages are coming over one at a time. `burrow/image/color.h` and `burrow/image/color/palette.h` are done. `image` itself, `image/draw` and the PNG, GIF and JPEG codecs come next.

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
