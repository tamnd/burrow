#include "burrow/burrow.h"

static void put32(BytesBuffer *b, uint32_t v) {
    Byte p[4];
    binary_big_endian_put_uint32(slice_from(p, 4, 4, TYPE_BYTE), v);
    bytes_buffer_write(b, slice_from(p, 4, 4, TYPE_BYTE), NULL);
}

// An entry of a symbol table in the format of Go 1.0: a 4-byte value, the type
// letter with the top bit set, the name and a NUL, and a 4-byte Go type.
static void sym(BytesBuffer *b, uint32_t value, char type, const char *name) {
    put32(b, value);
    (void)bytes_buffer_write_byte(b, (Byte)(type | 0x80));
    bytes_buffer_write_string(b, str_from_cstr(name), NULL);
    (void)bytes_buffer_write_byte(b, 0);
    put32(b, 0);
}

// A 'z' entry has an empty name and then the 2-byte codes of 'f' entries,
// ending in a zero code. With no codes it ends the file the last one began.
static void path(BytesBuffer *b, uint32_t value, const uint16_t *codes, Int n) {
    put32(b, value);
    (void)bytes_buffer_write_byte(b, 'z' | 0x80);
    (void)bytes_buffer_write_byte(b, 0);
    for (Int i = 0; i <= n; i++) {
        uint16_t c = i < n ? codes[i] : 0;
        (void)bytes_buffer_write_byte(b, (Byte)(c >> 8));
        (void)bytes_buffer_write_byte(b, (Byte)c);
    }
    put32(b, 0);
}

// The symbol table of a small program from before Go 1.2: one source file,
// /tmp/hello.go, and two functions in it, then the end of the text.
static Slice tiny_symtab(Alloc *a) {
    BytesBuffer b = BYTES_BUFFER(a);
    sym(&b, 1, 'f', "/");
    sym(&b, 2, 'f', "tmp");
    sym(&b, 3, 'f', "hello.go");
    static const uint16_t hello[] = {1, 2, 3}; // "/", "tmp", "hello.go"
    path(&b, 1, hello, 3);
    path(&b, 100, NULL, 0);
    sym(&b, 0x1000, 'T', "main.main");
    sym(&b, 0x18, 'm', ".frame");
    sym(&b, 0x1020, 'T', "main.(*T).String");
    sym(&b, 8, 'p', "t");
    sym(&b, 0x1040, 'T', "runtime.etext");
    return bytes_buffer_bytes(&b);
}

// The line table that goes with it, also in the old format. A byte from 1 to
// 64 adds that many lines and moves one byte on, and one above 128 moves on
// that much less 128. main.main is on lines 3 to 5, and String on 8 and 9.
static Slice tiny_lines(void) {
    static Byte lines[] = {3, 143, 1, 142, 1, 3, 142, 1, 143};
    return slice_from(lines, sizeof lines, sizeof lines, TYPE_BYTE);
}

static void lookups(Alloc *a) {
    // doc: lookups
    Error err = BURROW_NO_ERROR;
    GosymLineTable *lt = gosym_new_line_table(a, tiny_lines(), 0x1000);
    GosymTable *tab = gosym_new_table(a, tiny_symtab(a), lt, &err);
    if (tab == NULL) {
        fmt_println_v(err);
        return;
    }
    for (Int i = 0; i < tab->funcs.len; i++) {
        GosymFunc *f = &((GosymFunc *)tab->funcs.p)[i];
        fmt_printf_v("%#x-%#x %s: package %s, receiver %q, name %s\n", f->entry, f->end,
                     f->sym->name, gosym_func_package_name(f),
                     gosym_func_receiver_name(f), gosym_func_base_name(f));
    }
    uint64_t pcs[] = {0x1004, 0x1010, 0x1025};
    for (size_t i = 0; i < 3; i++) {
        Int line = 0;
        GosymFunc *fn = NULL;
        Str file = gosym_table_pc_to_line(tab, pcs[i], &line, &fn);
        fmt_printf_v("%#x is %s:%d in %s\n", pcs[i], file, line, fn->sym->name);
    }
    GosymFunc *fn = NULL;
    uint64_t pc = gosym_table_line_to_pc(tab, BURROW_S("/tmp/hello.go"), 9, &fn, &err);
    fmt_printf_v("line 9 starts at %#x in %s\n", pc, fn->sym->name);
    gosym_table_free(tab);
    gosym_line_table_free(lt);
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
    Error err = BURROW_NO_ERROR;
    GosymLineTable *lt = gosym_new_line_table(a, tiny_lines(), 0x1000);
    GosymTable *tab = gosym_new_table(a, tiny_symtab(a), lt, &err);
    GosymFunc *fn = NULL;
    (void)gosym_table_line_to_pc(tab, BURROW_S("/tmp/bye.go"), 1, &fn, &err);
    fmt_println_v(err);
    err = BURROW_NO_ERROR;
    (void)gosym_table_line_to_pc(tab, BURROW_S("/tmp/hello.go"), 6, &fn, &err);
    fmt_println_v(err);
    gosym_table_free(tab);

    // The type of the first symbol loses its top bit.
    Slice symtab = tiny_symtab(a);
    ((Byte *)symtab.p)[4] &= 0x7f;
    err = BURROW_NO_ERROR;
    tab = gosym_new_table(a, symtab, lt, &err);
    if (tab == NULL)
        fmt_println_v(err);
    gosym_line_table_free(lt);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    lookups(a);
    errors(a);
    arena_free(&ar);
    return 0;
}

/* Output:
0x1000-0x1020 main.main: package main, receiver "", name main
0x1020-0x1040 main.(*T).String: package main, receiver "(*T)", name String
0x1004 is /tmp/hello.go:3 in main.main
0x1010 is /tmp/hello.go:4 in main.main
0x1025 is /tmp/hello.go:8 in main.(*T).String
line 9 starts at 0x102f in main.(*T).String
unknown file: /tmp/bye.go
no code at /tmp/hello.go:6
bad symbol type '102' at byte 0x4
*/
