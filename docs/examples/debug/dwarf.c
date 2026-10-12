#include "burrow/burrow.h"

static void entries(Alloc *a) {
    // doc: entries
    // What a C compiler writes for `int answer;` in hello.c. The abbreviations
    // say which attributes each kind of entry has and how they are stored, and
    // the info section is the entries themselves.
    Byte abbrev[] = {
        1, 0x11, 1, 0x03, 0x08, 0x13, 0x0b, 0,    0,          // compile unit
        2, 0x24, 0, 0x03, 0x08, 0x0b, 0x0b, 0x3e, 0x0b, 0, 0, // base type
        3, 0x34, 0, 0x03, 0x08, 0x49, 0x13, 0,    0,          // variable
        0,
    };
    Byte info[] = {
        37, 0,   0,   0,   4,   0,   0,   0,   0,  0,  8,    // unit header, DWARF 4
        1,  'h', 'e', 'l', 'l', 'o', '.', 'c', 0,  12,       // hello.c, in C99
        2,  'i', 'n', 't', 0,   4,   5,                      // int, 4 bytes, signed
        3,  'a', 'n', 's', 'w', 'e', 'r', 0,   21, 0,  0, 0, // answer, of type int
        0, // end of the unit's children
    };
    Slice none = slice_from(NULL, 0, 0, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    DwarfData *d =
        dwarf_new(a, slice_from(abbrev, sizeof abbrev, sizeof abbrev, TYPE_BYTE), none,
                  none, slice_from(info, sizeof info, sizeof info, TYPE_BYTE), none,
                  none, none, none, &err);
    if (d == NULL) {
        fmt_println_v(err);
        return;
    }
    DwarfReader *r = dwarf_data_reader(d, a);
    for (DwarfEntry *e; (e = dwarf_reader_next(r, &err)) != NULL;) {
        Str *name = any_assert(dwarf_entry_val(e, DWARF_ATTR_NAME), TYPE_STRING);
        fmt_printf_v("%#x %-12s %q\n", e->offset, dwarf_tag_string(e->tag, a),
                     name != NULL ? *name : BURROW_STR_EMPTY);
        DwarfOffset *ref =
            any_assert(dwarf_entry_val(e, DWARF_ATTR_TYPE), TYPE_DWARF_OFFSET);
        if (ref == NULL)
            continue;
        DwarfType t = dwarf_data_type(d, *ref, &err);
        if (t != NULL)
            fmt_printf_v("     is %s, %d bytes\n", dwarf_type_string(t, a),
                         dwarf_type_size(t));
    }
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    entries(a);
    arena_free(&ar);
    return 0;
}
