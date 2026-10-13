# Debug information

`burrow/debug/elf.h` is Go's `debug/elf`. It reads ELF files, the format of executables, shared libraries, object files and core dumps on Linux, the BSDs and most other Unix systems. `burrow/debug/dwarf.h` is Go's `debug/dwarf`, which reads the DWARF debugging information that compilers put in those files: the compilation units, functions, variables and types of a program, and the line tables that map machine addresses back to source lines. Go's own `nm`, `objdump`, `addr2line` and `pprof` read binaries through them.

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

## How it is tested

The tests check the output against Go's own. `tools/gen-debug-tests.sh` runs Go's `debug/elf` and `debug/dwarf` over every file in `debug/elf`'s testdata and the ELF files in `debug/dwarf`'s and writes down a text dump of everything they read: headers, sections, symbols, dynamic tables, every DWARF entry and type, and every line table row. The C tests build the same dump and compare it line by line. They do the same for about 1,850 damaged copies of the files, comparing a hash of the dump, so a malformed file gets the same error here as in Go. The tests in Go's packages that look at one value at a time are ported as well.
