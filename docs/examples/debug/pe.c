#include "burrow/burrow.h"

static void put16(Byte *p, uint16_t v) {
    binary_little_endian_put_uint16(slice_from(p, 2, 2, TYPE_BYTE), v);
}

static void put32(Byte *p, uint32_t v) {
    binary_little_endian_put_uint32(slice_from(p, 4, 4, TYPE_BYTE), v);
}

static void put64(Byte *p, uint64_t v) {
    binary_little_endian_put_uint64(slice_from(p, 8, 8, TYPE_BYTE), v);
}

// A small 64-bit Windows executable: the MS-DOS header, the PE signature, the
// COFF and optional headers, a three-byte .text section and an .idata section
// that imports ExitProcess from KERNEL32.dll, 536 bytes in all. A real one would
// come from os_read_file or pe_open.
static Slice tiny_pe(Alloc *a) {
    Slice b = slice_make(a, TYPE_BYTE, 536, 536);
    Byte *p = b.p;
    memcpy(p, "MZ", 2);
    put32(p + 0x3c, 0x40); // where the PE signature is
    memcpy(p + 0x40, "PE\0\0", 4);
    // The COFF file header.
    Byte *h = p + 0x44;
    put16(h, PE_IMAGE_FILE_MACHINE_AMD64);
    put16(h + 2, 2);
    put16(h + 16, 240);
    put16(h + 18, PE_IMAGE_FILE_EXECUTABLE_IMAGE | PE_IMAGE_FILE_LARGE_ADDRESS_AWARE);
    // The PE32+ optional header, with all 16 data directories.
    Byte *o = p + 0x58;
    put16(o, 0x20b);
    put32(o + 16, 0x1000); // entry point
    put64(o + 24, 0x140000000);
    put32(o + 32, 0x1000);
    put32(o + 36, 0x10);
    put32(o + 56, 0x3000);
    put32(o + 60, 0x1a0);
    put16(o + 68, PE_IMAGE_SUBSYSTEM_WINDOWS_CUI);
    put32(o + 108, 16);
    put32(o + 120, 0x2000); // data directory 1, the import table
    put32(o + 124, 40);
    // The section headers.
    Byte *s = p + 0x148;
    memcpy(s, ".text", 5);
    put32(s + 8, 3);
    put32(s + 12, 0x1000);
    put32(s + 16, 3);
    put32(s + 20, 0x1a0);
    put32(s + 36,
          PE_IMAGE_SCN_CNT_CODE | PE_IMAGE_SCN_MEM_EXECUTE | PE_IMAGE_SCN_MEM_READ);
    s += 40;
    memcpy(s, ".idata", 6);
    put32(s + 8, 104);
    put32(s + 12, 0x2000);
    put32(s + 16, 104);
    put32(s + 20, 0x1b0);
    put32(s + 36, PE_IMAGE_SCN_CNT_INITIALIZED_DATA | PE_IMAGE_SCN_MEM_READ |
                      PE_IMAGE_SCN_MEM_WRITE);
    memcpy(p + 0x1a0, "\61\300\303", 3); // xor eax, eax; ret
    // The import directory, which ends with an empty entry, then the lookup
    // and address tables, the library name and the function's hint and name.
    Byte *d = p + 0x1b0;
    put32(d, 0x2028);
    put32(d + 12, 0x2048);
    put32(d + 16, 0x2038);
    put64(d + 0x28, 0x2058);
    put64(d + 0x38, 0x2058);
    memcpy(d + 0x48, "KERNEL32.dll", 12);
    memcpy(d + 0x5a, "ExitProcess", 11);
    return b;
}

static void image(Alloc *a) {
    // doc: image
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, tiny_pe(a));
    PeFile *f = pe_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (f == NULL) {
        fmt_println_v(err);
        return;
    }
    if (f->optional_header.t == TYPE_PE_OPTIONAL_HEADER64) {
        const PeOptionalHeader64 *oh = f->optional_header.data;
        fmt_printf_v("PE32+ for machine %#x, image base %#x, entry %#x\n",
                     f->file_header.machine, oh->image_base,
                     oh->address_of_entry_point);
    }
    for (Int i = 0; i < f->sections.len; i++) {
        PeSection *s = ((PeSection **)f->sections.p)[i];
        PeSectionHeader sh = s->section_header;
        fmt_printf_v("%-7s va %#x size %d\n", sh.name, sh.virtual_address, sh.size);
    }
    PeSection *text = pe_file_section(f, BURROW_S(".text"));
    Slice code = pe_section_data(text, a, &err);
    fmt_printf_v(".text holds % x\n", code);
    Slice imports = pe_file_imported_symbols(f, a, &err);
    for (Int i = 0; i < imports.len; i++)
        fmt_println_v("imports", ((Str *)imports.p)[i]);
    pe_file_free(f);
    // doc: end
}

static void bad_files(Alloc *a) {
    // doc: errors
    Slice data = tiny_pe(a);
    Byte *p = data.p;
    p[0x43] = 1; // the signature is now "PE\0\1"
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, data);
    PeFile *f = pe_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (f == NULL)
        fmt_println_v(err);

    data = tiny_pe(a);
    p = data.p;
    p[0x58] = 0x0c; // the optional header's magic is now 0x20c
    br = bytes_new_reader(a, data);
    f = pe_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (f == NULL)
        fmt_println_v(err);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    image(a);
    bad_files(a);
    arena_free(&ar);
    return 0;
}

/* Output:
PE32+ for machine 0x8664, image base 0x140000000, entry 0x1000
.text   va 0x1000 size 3
.idata  va 0x2000 size 104
.text holds 31 c0 c3
imports ExitProcess:KERNEL32.dll
invalid PE file signature: 50 45 00 01
optional header has unexpected Magic of 0x20c
*/
