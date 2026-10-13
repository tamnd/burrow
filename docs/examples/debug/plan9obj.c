#include "burrow/burrow.h"

static void put32(Byte *p, uint32_t v) {
    binary_big_endian_put_uint32(slice_from(p, 4, 4, TYPE_BYTE), v);
}

// A small 386 Plan 9 executable: the 32-byte header, three bytes of text, and
// a symbol table of 53 bytes, 88 bytes in all. The table has three file name
// pieces, a path made of them, and the symbol main. A real file would come
// from os_read_file or plan9obj_open.
static Slice tiny_plan9(Alloc *a) {
    Slice b = slice_make(a, TYPE_BYTE, 88, 88);
    Byte *p = b.p;
    put32(p, PLAN9OBJ_MAGIC386);
    put32(p + 4, 3);                  // text size
    put32(p + 16, 53);                // symbol table size
    put32(p + 20, 0x1020);            // entry point
    memcpy(p + 32, "\61\300\303", 3); // xor eax, eax; ret
    // Each entry is a 4-byte value, a type letter with the top bit set, and a
    // name that ends in a NUL. A 'z' entry's name is a list of 2-byte codes of
    // 'f' entries instead, which ends in two NULs.
    Byte *s = p + 35;
    put32(s, 1);
    memcpy(s + 4, "\346/", 3); // 'f' | 0x80, "/"
    put32(s + 7, 2);
    memcpy(s + 11, "\346usr", 5);
    put32(s + 16, 3);
    memcpy(s + 20, "\346hello.c", 9);
    put32(s + 29, 1);
    memcpy(s + 33, "\372\0\0\1\0\2\0\3\0", 10); // 'z' | 0x80, codes 1, 2 and 3
    put32(s + 43, 0x1020);
    memcpy(s + 47, "\324main", 6); // 'T' | 0x80
    return b;
}

static void symbols(Alloc *a) {
    // doc: symbols
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, tiny_plan9(a));
    Plan9objFile *f = plan9obj_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (f == NULL) {
        fmt_println_v(err);
        return;
    }
    Plan9objFileHeader h = f->file_header;
    fmt_printf_v("magic %#x, entry %#x, %d-byte pointers\n", h.magic, h.entry,
                 h.ptr_size);
    for (Int i = 0; i < f->sections.len; i++) {
        Plan9objSection *s = ((Plan9objSection **)f->sections.p)[i];
        Plan9objSectionHeader sh = s->section_header;
        fmt_printf_v("%s  offset %d size %d\n", sh.name, sh.offset, sh.size);
    }
    Slice syms = plan9obj_file_symbols(f, a, &err);
    for (Int i = 0; i < syms.len; i++) {
        Plan9objSym *s = &((Plan9objSym *)syms.p)[i];
        fmt_printf_v("%c %#x %s\n", s->type, s->value, s->name);
    }
    plan9obj_file_free(f);
    // doc: end
}

static void bad_files(Alloc *a) {
    // doc: errors
    Error err = BURROW_NO_ERROR;
    Slice data = tiny_plan9(a);
    memcpy(data.p, "\177ELF", 4);
    BytesReader *br = bytes_new_reader(a, data);
    Plan9objFile *f = plan9obj_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (f == NULL)
        fmt_println_v(err);

    // The symbol table now stops after main's value.
    data = tiny_plan9(a);
    put32((Byte *)data.p + 16, 47);
    br = bytes_new_reader(a, data);
    f = plan9obj_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    Slice syms = plan9obj_file_symbols(f, a, &err);
    if (BURROW_FAILED(err))
        fmt_println_v(syms.len, err);
    plan9obj_file_free(f);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    symbols(a);
    bad_files(a);
    arena_free(&ar);
    return 0;
}

/* Output:
magic 0x1eb, entry 0x1020, 4-byte pointers
text  offset 32 size 3
data  offset 35 size 0
syms  offset 35 size 53
spsz  offset 88 size 0
pcsz  offset 88 size 0
f 0x1 /
f 0x2 usr
f 0x3 hello.c
z 0x1 /usr/hello.c
T 0x1020 main
bad magic number '[127 69 76 70]' in record at byte 0x0
0 unexpected EOF in record at byte 0x2f
*/
