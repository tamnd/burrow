# Debug information

`burrow/debug/elf.h` is Go's `debug/elf`. It reads ELF files, the format of executables, shared libraries, object files and core dumps on Linux, the BSDs and most other Unix systems. `burrow/debug/dwarf.h` is Go's `debug/dwarf`, which reads the DWARF debugging information that compilers put in those files: the compilation units, functions, variables and types of a program, and the line tables that map machine addresses back to source lines. `burrow/debug/macho.h` is Go's `debug/macho`, which reads Mach-O, the format of executables and libraries on macOS and iOS, including the universal files that hold one for each processor. `burrow/debug/pe.h` is Go's `debug/pe`, which reads the PE and COFF files of Windows, its executables, DLLs and object files. Go's own `nm`, `objdump`, `addr2line` and `pprof` read binaries through them.

## ELF files

`elf_open` opens a file by name and `elf_new_file` reads one from any `IoReaderAt`, which has to stay readable while the result is in use. Both read the file header and the section and program headers at once and leave the rest until it is asked for. `tiny_elf` in the example file writes a 288-byte executable by hand, so this runs the same everywhere:

<!-- example: ../examples/debug/elf.c#sections -->
```c
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
```

That prints:

```
ELFCLASS64 ELFDATA2LSB ET_EXEC EM_X86_64
entry 0x401000
""           SHT_NULL     addr 0x0 size 0
".text"      SHT_PROGBITS addr 0x401000 size 3
".shstrtab"  SHT_STRTAB   addr 0x0 size 17
.text holds 31 c0 c3
```

The file header and the sections are plain structs. `ElfFile.sections` is a slice of `ElfSection` pointers in the order of the section table, and `elf_file_section` finds one by name. `elf_section_data` reads a section's contents, uncompressing them first when the section is compressed with zlib or zstd, and `elf_section_open` gives an `IoReadSeeker` over them for sections too big to read at once. Every named integer type has a `_string` function that gives Go's name for a value, and Go's own way of printing one it does not know.

The symbol tables, the dynamic section and the GNU version tables are read when asked for. `elf_file_symbols` and `elf_file_dynamic_symbols` give the two symbol tables, `elf_file_imported_symbols` and `elf_file_imported_libraries` give what a dynamically linked program needs from elsewhere, and `elf_file_dyn_string` and `elf_file_dyn_value` read single entries of the dynamic section, such as `DT_NEEDED` or `DT_FLAGS`.

An `ElfFile` keeps what it read at the start in an arena of its own, and `elf_file_free` gives all of it back. What the other functions return, such as the symbols or a section's contents, comes from the allocator they are given, so it outlives the file and can be freed on its own. A file from `elf_open` also has to be closed, which `elf_file_free` does as well.

## Errors

A file that is not ELF, or that is ELF but malformed, gives `NULL` and the error. For a malformed file the error is usually an `ElfFormatError`, which says what was wrong and at which offset:

<!-- example: ../examples/debug/elf.c#errors -->
```c
Error err = BURROW_NO_ERROR;
Str text = BURROW_S("This is a text file, not an executable.");
BytesReader *br = bytes_new_reader(a, slice_from_str(a, text));
ElfFile *f = elf_new_file(a, bytes_reader_as_io_reader_at(br), &err);
if (f == NULL)
    fmt_println_v("no file:", err);

const ElfFormatError *fe = errors_as(err, TYPE_ELF_FORMAT_ERROR);
if (fe != NULL)
    fmt_println_v("at offset", fe->off, "of the file:", fe->msg);
```

That prints:

```
no file: bad magic number '[84 104 105 115]' in record at byte 0x0
at offset 0 of the file: bad magic number
```

Like Go's, the reader checks only as much as it needs to read a file without going out of bounds. A file that passes can still make no sense, and a malicious one can make it allocate a lot, so take care with files from somewhere you do not trust.

## DWARF

`elf_file_dwarf` collects the debug sections of an ELF file, uncompressing them as needed, and gives back a `DwarfData`. Other container formats get theirs from `dwarf_new`, which takes the raw contents of each section. The example builds the two sections a C compiler would write for one variable and walks the entries:

<!-- example: ../examples/debug/dwarf.c#entries -->
```c
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
    if (e->tag == 0)
        continue; // the end of a list of children
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
```

That prints:

```
0xb CompileUnit  "hello.c"
0x15 BaseType     "int"
0x1c Variable     "answer"
     is int, 4 bytes
```

The entries form a tree. `dwarf_reader_next` walks it depth first and gives an entry with a zero tag at the end of each list of children. `dwarf_reader_skip_children` jumps over the children of the entry it just returned, which is how you stay at the top level of units. `dwarf_entry_val` gives an attribute's value as an `Any` holding the same type as in Go, so `any_assert` with `TYPE_STRING`, `TYPE_INT64`, `TYPE_UINT64` or `TYPE_DWARF_OFFSET` gets at it.

`dwarf_data_type` turns the entry at an offset into a type. Go's `Type` is an interface, and here it is a pointer to the `DwarfCommonType` at the start of each type struct, with `type_kind` saying which struct it is. Types are kept in the `DwarfData` once read, so the same offset gives the same pointer every time. A `DwarfData` and everything it reads live in the allocator it was made with, so give it an arena.

Go reads types by recursion and lets its stack grow as far as a type goes. A C thread's stack does not grow, so `dwarf_data_type` keeps an eye on how much is left, and on a type nested deep enough to run it out it gives a `DwarfDecodeError` saying "types nested too deeply" where Go would have kept going. Nothing a real compiler writes comes near that.

## Addresses and lines

`dwarf_reader_seek_pc` finds the unit holding a program counter, and `dwarf_data_ranges` gives the address ranges of a unit or a function. `dwarf_data_line_reader` opens the line table of a unit. `dwarf_line_reader_next` steps through its rows, each a `DwarfLineEntry` with an address, a file, a line and a column, and `dwarf_line_reader_seek_pc` finds the row for an address directly. `dwarf_line_reader_tell` and `dwarf_line_reader_seek` save and restore a position in the table, as in Go. These read DWARF 2 to 5. Like Go, they do not follow split DWARF out to the `.dwo` files that GCC writes with `-gsplit-dwarf`, but the skeleton units left in the executable read as usual.

## Mach-O files

`macho_open` and `macho_new_file` read a Mach-O file the way the ELF ones read an ELF file. They read the header and every load command at once, which gives the segments and their sections, the symbol table, the dynamic symbol table, the libraries the file links against and its rpaths. `MachoFile.loads` holds the load commands in file order. Go's `Load` is an interface, and here each one is a pointer to a `MachoLoadBytes` with a `kind` saying which struct it starts, so a `MACHO_LOAD_KIND_SEGMENT` one is a `MachoSegment`. `tiny_macho` in the example file writes a 216-byte executable by hand:

<!-- example: ../examples/debug/macho.c#loads -->
```c
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
```

That prints:

```
CpuAmd64 Exec 2 load commands
segment "__TEXT" addr 0x100000000 size 0x1000
rpath "/usr/lib"
__TEXT,__text holds 31 c0 c3
```

`macho_file_segment` and `macho_file_section` find a segment or a section by name, and `macho_section_data`, `macho_section_open` and the segment versions read their contents. `macho_file_dwarf` gives the DWARF data from the `__debug_` and `__zdebug_` sections, uncompressing the ones with a `ZLIB` header first. `macho_file_imported_symbols` and `macho_file_imported_libraries` give what the program needs from elsewhere. Memory works as it does for ELF: a `MachoFile` keeps what it read in an arena of its own, `macho_file_free` gives it back and closes a file from `macho_open`, and what the other functions return comes from the allocator they are given.

A universal file starts with a table of the files inside it. `macho_new_fat_file` and `macho_open_fat` read that table and every file it lists, and give each one as a `MachoFatArch` holding a `MachoFile`. A plain Mach-O file is not a universal one, and gives `macho_err_not_fat`, so a program that takes either kind can try the universal reader first and fall back to the plain one on that error. Anything else that is wrong with a file is usually a `MachoFormatError`:

<!-- example: ../examples/debug/macho.c#fat -->
```c
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
```

That prints:

```
not universal: not a fat Mach-O file in record at byte 0x0
at offset 0 of the file: invalid magic number
```

Go's `ImportedSymbols` slices the symbol table with the numbers in the dynamic symbol table without checking them first, so a file that lies about them makes it panic. `macho_file_imported_symbols` panics in the same cases with the same message. The panic is a plain one rather than the runtime error Go's would be, so recover from it as you would from any panic.

## PE files

`pe_open` and `pe_new_file` read a PE image or a COFF object file. They read the COFF header, the optional header, the section headers and their relocations, the COFF symbol table and the string table at once. An image starts with an MS-DOS header that says where the PE signature and the COFF header are, and an object file starts with the COFF header itself, so both kinds read the same way. `PeFile.optional_header` is Go's `any`: its `t` is `TYPE_PE_OPTIONAL_HEADER32` or `TYPE_PE_OPTIONAL_HEADER64` and its `data` points at the header, and an object file, which has none, leaves it nil. `tiny_pe` in the example file writes a 536-byte executable by hand that imports one function:

<!-- example: ../examples/debug/pe.c#image -->
```c
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
```

That prints:

```
PE32+ for machine 0x8664, image base 0x140000000, entry 0x1000
.text   va 0x1000 size 3
.idata  va 0x2000 size 104
.text holds 31 c0 c3
imports ExitProcess:KERNEL32.dll
```

`pe_file_section`, `pe_section_data`, `pe_section_open` and `pe_section_read_at` find and read sections as the ELF and Mach-O ones do, and a section with no data in the file, such as `.bss`, fails to read with an error instead of giving zeros. `PeFile.symbols` holds the symbols with their full names, and `PeFile.coff_symbols` holds every record of the table as it is in the file, auxiliary records included, so that `pe_file_coff_symbol_read_section_def_aux` can give the section definition record after a static symbol. `pe_file_dwarf` reads the DWARF sections that MinGW and Clang put in Windows binaries, and `pe_file_imported_symbols` gives each imported function as `name:library`, as Go does. Like Go, `pe_file_imported_libraries` always gives nothing. A file that is not right gives an error saying what is wrong with it:

<!-- example: ../examples/debug/pe.c#errors -->
```c
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
```

That prints:

```
invalid PE file signature: 50 45 00 01
optional header has unexpected Magic of 0x20c
```

## How it is tested

The tests check the output against Go's own. `tools/gen-debug-tests.sh` runs Go's `debug/elf` and `debug/dwarf` over every file in `debug/elf`'s testdata and the ELF files in `debug/dwarf`'s and writes down a text dump of everything they read: headers, sections, symbols, dynamic tables, every DWARF entry and type, and every line table row. The C tests build the same dump and compare it line by line. They do the same for about 1,850 damaged copies of the files, comparing a hash of the dump, so a malformed file gets the same error here as in Go. `tools/gen-debug-tests.sh macho` does the same for the files in `debug/macho`'s testdata and the Mach-O files in `debug/dwarf`'s, reading each one both as a universal file and as a plain one, and `tools/gen-debug-tests.sh pe` for the files in `debug/pe`'s testdata and the PE file in `debug/dwarf`'s. The tests in Go's packages that look at one value at a time are ported as well.
