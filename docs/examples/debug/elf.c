#include "burrow/burrow.h"

// The smallest useful ELF file: a 64-bit little-endian x86-64 executable with a
// three-byte .text and the table of section names, 288 bytes in all. A real one
// would come from os_read_file or elf_open.
static Slice tiny_elf(Alloc *a) {
    Slice b = slice_make(a, TYPE_BYTE, 288, 288);
    Byte *p = b.p;
    memcpy(p, "\177ELF\2\1\1", 7);
    binary_little_endian_put_uint16(slice_from(p + 16, 2, 2, TYPE_BYTE), 2);  // ET_EXEC
    binary_little_endian_put_uint16(slice_from(p + 18, 2, 2, TYPE_BYTE), 62); // x86-64
    binary_little_endian_put_uint32(slice_from(p + 20, 4, 4, TYPE_BYTE), 1);
    binary_little_endian_put_uint64(slice_from(p + 24, 8, 8, TYPE_BYTE), 0x401000);
    binary_little_endian_put_uint64(slice_from(p + 40, 8, 8, TYPE_BYTE), 96);
    binary_little_endian_put_uint16(slice_from(p + 52, 2, 2, TYPE_BYTE), 64);
    binary_little_endian_put_uint16(slice_from(p + 54, 2, 2, TYPE_BYTE), 56);
    binary_little_endian_put_uint16(slice_from(p + 58, 2, 2, TYPE_BYTE), 64);
    binary_little_endian_put_uint16(slice_from(p + 60, 2, 2, TYPE_BYTE), 3);
    binary_little_endian_put_uint16(slice_from(p + 62, 2, 2, TYPE_BYTE), 2);
    memcpy(p + 64, "\61\300\303", 3); // xor eax, eax; ret
    memcpy(p + 72, "\0.text\0.shstrtab", 17);
    // Section 1 is .text and section 2 the names. Section 0 is all zeros.
    Byte *s = p + 96 + 64;
    binary_little_endian_put_uint32(slice_from(s, 4, 4, TYPE_BYTE), 1);
    binary_little_endian_put_uint32(slice_from(s + 4, 4, 4, TYPE_BYTE), 1);
    binary_little_endian_put_uint64(slice_from(s + 8, 8, 8, TYPE_BYTE), 6);
    binary_little_endian_put_uint64(slice_from(s + 16, 8, 8, TYPE_BYTE), 0x401000);
    binary_little_endian_put_uint64(slice_from(s + 24, 8, 8, TYPE_BYTE), 64);
    binary_little_endian_put_uint64(slice_from(s + 32, 8, 8, TYPE_BYTE), 3);
    binary_little_endian_put_uint64(slice_from(s + 48, 8, 8, TYPE_BYTE), 16);
    s += 64;
    binary_little_endian_put_uint32(slice_from(s, 4, 4, TYPE_BYTE), 7);
    binary_little_endian_put_uint32(slice_from(s + 4, 4, 4, TYPE_BYTE), 3);
    binary_little_endian_put_uint64(slice_from(s + 24, 8, 8, TYPE_BYTE), 72);
    binary_little_endian_put_uint64(slice_from(s + 32, 8, 8, TYPE_BYTE), 17);
    binary_little_endian_put_uint64(slice_from(s + 48, 8, 8, TYPE_BYTE), 1);
    return b;
}

static void sections(Alloc *a) {
    // doc: sections
    Slice data = tiny_elf(a);
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, data);
    ElfFile *f = elf_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (f == NULL) {
        fmt_println_v(err);
        return;
    }
    ElfFileHeader h = f->file_header;
    fmt_println_v(elf_class_string(h.cls, a), elf_data_string(h.data, a),
                  elf_type_string(h.type, a), elf_machine_string(h.machine, a));
    fmt_printf_v("entry %#x\n", h.entry);
    for (Int i = 0; i < f->sections.len; i++) {
        ElfSection *s = ((ElfSection **)f->sections.p)[i];
        ElfSectionHeader sh = s->section_header;
        fmt_printf_v("%-12q %-12s addr %#x size %d\n", sh.name,
                     elf_section_type_string(sh.type, a), sh.addr, sh.size);
    }
    ElfSection *text = elf_file_section(f, BURROW_S(".text"));
    Slice code = elf_section_data(text, a, &err);
    fmt_printf_v(".text holds % x\n", code);
    elf_file_free(f);
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
    Error err = BURROW_NO_ERROR;
    Str text = BURROW_S("This is a text file, not an executable.");
    BytesReader *br = bytes_new_reader(a, slice_from_str(a, text));
    ElfFile *f = elf_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (f == NULL)
        fmt_println_v("no file:", err);

    const ElfFormatError *fe = errors_as(err, TYPE_ELF_FORMAT_ERROR);
    if (fe != NULL)
        fmt_println_v("at offset", fe->off, "of the file:", fe->msg);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    sections(a);
    errors(a);
    arena_free(&ar);
    return 0;
}
