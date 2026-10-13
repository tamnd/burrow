#include "burrow/burrow.h"

static void put32(Byte *p, uint32_t v) {
    binary_little_endian_put_uint32(slice_from(p, 4, 4, TYPE_BYTE), v);
}

static void put64(Byte *p, uint64_t v) {
    binary_little_endian_put_uint64(slice_from(p, 8, 8, TYPE_BYTE), v);
}

// A small 64-bit x86-64 Mach-O executable: the header, a __TEXT segment with a
// three-byte __text section, and an rpath, 216 bytes in all. A real one would
// come from os_read_file or macho_open.
static Slice tiny_macho(Alloc *a) {
    Slice b = slice_make(a, TYPE_BYTE, 216, 216);
    Byte *p = b.p;
    put32(p, 0xfeedfacf);    // MH_MAGIC_64
    put32(p + 4, 0x1000007); // x86-64
    put32(p + 8, 3);
    put32(p + 12, 2); // MH_EXECUTE
    put32(p + 16, 2);
    put32(p + 20, 176);
    // The segment command, with its one section after it.
    Byte *c = p + 32;
    put32(c, 0x19); // LC_SEGMENT_64
    put32(c + 4, 152);
    memcpy(c + 8, "__TEXT", 6);
    put64(c + 24, 0x100000000);
    put64(c + 32, 0x1000);
    put64(c + 48, 216);
    put32(c + 56, 5);
    put32(c + 60, 5);
    put32(c + 64, 1);
    Byte *s = c + 72;
    memcpy(s, "__text", 6);
    memcpy(s + 16, "__TEXT", 6);
    put64(s + 32, 0x1000000d0);
    put64(s + 40, 3);
    put32(s + 48, 208);
    put32(s + 64, 0x80000400);
    // The rpath command.
    c = p + 184;
    put32(c, 0x8000001c); // LC_RPATH
    put32(c + 4, 24);
    put32(c + 8, 12);
    memcpy(c + 12, "/usr/lib", 8);
    memcpy(p + 208, "\61\300\303", 3); // xor eax, eax; ret
    return b;
}

static void loads(Alloc *a) {
    // doc: loads
    Slice data = tiny_macho(a);
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, data);
    MachoFile *f = macho_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (f == NULL) {
        fmt_println_v(err);
        return;
    }
    MachoFileHeader h = f->file_header;
    fmt_println_v(macho_cpu_string(h.cpu, a), macho_type_string(h.type, a), h.ncmd,
                  "load commands");
    for (Int i = 0; i < f->loads.len; i++) {
        MachoLoad l = ((MachoLoad *)f->loads.p)[i];
        if (l->kind == MACHO_LOAD_KIND_SEGMENT) {
            MachoSegment *seg = (MachoSegment *)l;
            MachoSegmentHeader sh = seg->segment_header;
            fmt_printf_v("segment %q addr %#x size %#x\n", sh.name, sh.addr, sh.memsz);
        } else if (l->kind == MACHO_LOAD_KIND_RPATH) {
            fmt_printf_v("rpath %q\n", ((MachoRpath *)l)->path);
        }
    }
    MachoSection *text = macho_file_section(f, BURROW_S("__text"));
    Slice code = macho_section_data(text, a, &err);
    fmt_printf_v("%s,%s holds % x\n", text->section_header.seg,
                 text->section_header.name, code);
    macho_file_free(f);
    // doc: end
}

static void fat(Alloc *a) {
    // doc: fat
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, tiny_macho(a));
    MachoFatFile *ff = macho_new_fat_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (ff == NULL && errors_is(err, macho_err_not_fat))
        fmt_println_v("not universal:", err);

    Str text = BURROW_S("This is a text file, not an executable.");
    br = bytes_new_reader(a, slice_from_str(a, text));
    MachoFile *f = macho_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    const MachoFormatError *fe = errors_as(err, TYPE_MACHO_FORMAT_ERROR);
    if (f == NULL && fe != NULL)
        fmt_println_v("at offset", fe->off, "of the file:", fe->msg);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    loads(a);
    fat(a);
    arena_free(&ar);
    return 0;
}

/* Output:
CpuAmd64 Exec 2 load commands
segment "__TEXT" addr 0x100000000 size 0x1000
rpath "/usr/lib"
__TEXT,__text holds 31 c0 c3
not universal: not a fat Mach-O file in record at byte 0x0
at offset 0 of the file: invalid magic number
*/
