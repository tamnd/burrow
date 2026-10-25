# Fill in a text template

Print a report from a struct with `text/template`. The struct is declared with `BURROW_STRUCT`, so the template can name its fields, range over its slice and test its bool. A template that names a field the struct doesn't have parses fine and fails when it runs, with the template's name and the place in it.

<!-- example: ../examples/cookbook/template_report.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

#define ITEM_FIELDS(F, T)                                                              \
    F(T, Str, Name, "")                                                                \
    F(T, Int, Qty, "")                                                                 \
    F(T, double, Price, "")
BURROW_STRUCT(Item, ITEM_FIELDS);
BURROW_SLICE_TYPE(Items, Item);

#define ORDER_FIELDS(F, T)                                                             \
    F(T, Str, Customer, "")                                                            \
    F(T, Items, Lines, "")                                                             \
    F(T, bool, Paid, "")
BURROW_STRUCT(Order, ORDER_FIELDS);

static const char report[] =
    "Order for {{.Customer}}\n"
    "{{range .Lines}}  {{.Qty}} x {{printf \"%-8s\" .Name}} "
    "{{printf \"%6.2f\" .Price}}\n"
    "{{end}}"
    "{{if .Paid}}Paid, thank you.{{else}}Not paid yet.{{end}}\n";

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    TextTemplate *t = text_template_new(a, S("report"));
    Error err = NO_ERROR;
    t = text_template_parse(t, str_from_cstr(report), &err);
    t = text_template_must(t, err); /* panics if the template doesn't parse */

    Order o = {S("Ada"), slice_nil(TYPE_OF(Item)), false};
    Item tea = {S("tea"), 2, 3.5};
    Item scones = {S("scones"), 6, 1.25};
    o.Lines = APPEND(Item, a, o.Lines, tea);
    o.Lines = APPEND(Item, a, o.Lines, scones);

    IoWriter out = os_file_as_io_writer(os_stdout);
    err = text_template_execute(t, out, ANY(TYPE_OF(Order), &o));
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }

    /* A template that names a field the struct doesn't have fails when it
     * runs, with the template's name and where in it. */
    TextTemplate *bad = text_template_new(a, S("bad"));
    bad = text_template_parse(bad, S("{{.Total}}\n"), &err);
    err = text_template_execute(bad, out, ANY(TYPE_OF(Order), &o));
    fmt_println_v(err);
    arena_free(&ar);
}
```

It prints:

```text
Order for Ada
  2 x tea        3.50
  6 x scones     1.25
Not paid yet.
template: bad:1:2: executing "bad" at <.Total>: can't evaluate field Total in type Order
```

The program is [docs/examples/cookbook/template_report.c](../examples/cookbook/template_report.c). CI builds and runs it and checks that it prints this.
