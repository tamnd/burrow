# Debug information

`burrow/debug/elf.h` is Go's `debug/elf`. It reads ELF files, the format of executables, shared libraries, object files and core dumps on Linux, the BSDs and most other Unix systems. `burrow/debug/dwarf.h` is Go's `debug/dwarf`, which reads the DWARF debugging information that compilers put in those files: the compilation units, functions, variables and types of a program, and the line tables that map machine addresses back to source lines. `burrow/debug/macho.h` is Go's `debug/macho`, which reads Mach-O, the format of executables and libraries on macOS and iOS, including the universal files that hold one for each processor. `burrow/debug/pe.h` is Go's `debug/pe`, which reads the PE and COFF files of Windows, its executables, DLLs and object files. `burrow/debug/plan9obj.h` is Go's `debug/plan9obj`, which reads the a.out executables of Plan 9. `burrow/debug/gosym.h` is Go's `debug/gosym`, which reads the tables a Go binary keeps to turn a program counter into a function, a file and a line. Go's own `nm`, `objdump`, `addr2line` and `pprof` read binaries through them.

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

## Plan 9 a.out files

`plan9obj_open` and `plan9obj_new_file` read a Plan 9 executable. The file has no section headers, just a header of eight big-endian words that gives the size of each part, so the five sections, `text`, `data`, `syms`, `spsz` and `pcsz`, are made up from those sizes and always all there. A 64-bit file has its entry point in eight more bytes after the header, and `ptr_size` in the header says which kind it is. `plan9obj_file_symbols` decodes the symbol table. A `'z'` symbol's name is a path made of the `'f'` symbols before it, which is how Plan 9 records the source files, and it comes back put together. `tiny_plan9` in the example file writes an 88-byte executable by hand:

<!-- example: ../examples/debug/plan9obj.c#symbols -->
```c
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
```

That prints:

```
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
```

The checks are few, as in Go: the magic number has to be one of the three known ones and the header has to be all there. A symbol table that stops in the middle of an entry makes `plan9obj_file_symbols` fail, and a path with a code that no `'f'` symbol gave is cut short at that code, the same as Go:

<!-- example: ../examples/debug/plan9obj.c#errors -->
```c
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
```

That prints:

```
bad magic number '[127 69 76 70]' in record at byte 0x0
0 unexpected EOF in record at byte 0x2f
```

## Go symbol tables

Every Go binary carries a table that maps program counters to functions, files and lines, the pclntab, and binaries from before Go 1.3 also carry a symbol table of their own. `debug/gosym` reads both. It does not read the binary itself: take the bytes of the `.gopclntab` and `.gosymtab` sections and the address of `.text` from `burrow/debug/elf.h`, `macho.h` or `pe.h`, give the pclntab and the address to `gosym_new_line_table`, and give that and the symbol table to `gosym_new_table`. With a pclntab from Go 1.2 or later the symbol table can be empty, and the functions come from the pclntab. Before that the line table is a plain run of bytes that only makes sense next to the symbol table, and the example writes such a pair by hand, a file `/tmp/hello.go` with two functions in it:

<!-- example: ../examples/debug/gosym.c#lookups -->
```c
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
```

That prints:

```
0x1000-0x1020 main.main: package main, receiver "", name main
0x1020-0x1040 main.(*T).String: package main, receiver "(*T)", name String
0x1004 is /tmp/hello.go:3 in main.main
0x1010 is /tmp/hello.go:4 in main.main
0x1025 is /tmp/hello.go:8 in main.(*T).String
line 9 starts at 0x102f in main.(*T).String
```

A file the table does not know and a line with no code on it are two different errors, and a symbol table that does not decode makes `gosym_new_table` fail with the byte it stopped at:

<!-- example: ../examples/debug/gosym.c#errors -->
```c
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
```

That prints:

```
unknown file: /tmp/bye.go
no code at /tmp/hello.go:6
bad symbol type '102' at byte 0x4
```

A damaged pclntab mostly shows up as a read out of range. The lookups on a Go 1.2 or later table catch that the way Go's do and give back -1, 0 or an empty name. Where Go's code has no such catch, the C version panics at the same point Go would, with a burrow panic that `BURROW_TRY` catches. Either way it never reads past the bytes it was given.

## Go build information

Every Go binary carries a record of how it was built: the Go release, the main package and its module, the modules it depends on, and settings such as the target and the linker flags. `buildinfo_read_file` and `buildinfo_read` find that record in an ELF, PE, Mach-O, XCOFF or Plan 9 binary, for any target, and give it back as a `Buildinfo`, which is runtime/debug's `DebugBuildInfo`. The binary below is a Plan 9 one made in memory, so the example has something to read:

<!-- example: ../examples/debug/buildinfo.c#read -->
```c
Error err = BURROW_NO_ERROR;
BytesReader *br = bytes_new_reader(a, tiny_go_binary(a));
Buildinfo *bi = buildinfo_read(a, bytes_reader_as_io_reader_at(br), &err);
if (bi == NULL) {
    fmt_println_v(err);
    return;
}
fmt_printf_v("%s, built with %s\n", bi->path, bi->go_version);
fmt_printf_v("module %s %s\n", bi->main.path, bi->main.version);
for (Int i = 0; i < bi->deps.len; i++) {
    DebugModule *m = ((DebugModule **)bi->deps.p)[i];
    fmt_printf_v("depends on %s %s\n", m->path, m->version);
}
for (Int i = 0; i < bi->settings.len; i++) {
    DebugBuildSetting *s = &((DebugBuildSetting *)bi->settings.p)[i];
    fmt_printf_v("  %s: %s\n", s->key, s->value);
}
debug_build_info_free(bi);
```

That prints:

```
OUTPUT_READ
```

A file that is not a binary of a kind it knows gives "unrecognized file format". One that is, but has no Go build information in it, such as a C program, gives "not a Go executable". `buildinfo_read_file` puts the name of the file in front of either one, and when the file cannot be opened it wraps the `FsPathError` instead:

<!-- example: ../examples/debug/buildinfo.c#errors -->
```c
Error err = BURROW_NO_ERROR;
BytesReader *br = bytes_new_reader(a, BURROW_B("hello"));
Buildinfo *bi = buildinfo_read(a, bytes_reader_as_io_reader_at(br), &err);
if (bi == NULL)
    fmt_println_v(err);

// One byte of the magic changes, so the record is not found.
Slice b = tiny_go_binary(a);
((Byte *)b.p)[32 + 2] = 'N';
br = bytes_new_reader(a, b);
err = BURROW_NO_ERROR;
bi = buildinfo_read(a, bytes_reader_as_io_reader_at(br), &err);
if (bi == NULL)
    fmt_println_v(err);
```

That prints:

```
OUTPUT_ERRORS
```

The record itself is text, one line per item. `debug_parse_build_info` turns it into a `DebugBuildInfo` and `debug_build_info_string` turns one back into text, quoting the settings that need it. Both work the way Go's `ParseBuildInfo` and `BuildInfo.String` do, so a record that Go would reject is rejected here with the same message and line number:

<!-- example: ../examples/debug/buildinfo.c#parse -->
```c
Str text = BURROW_S("path\texample.com/hello\n"
                    "mod\texample.com/hello\tv1.2.0\n"
                    "build\t-tags=netgo\n"
                    "build\tvcs.modified=true\n");
Error err = BURROW_NO_ERROR;
DebugBuildInfo *bi = debug_parse_build_info(a, text, &err);
if (bi == NULL) {
    fmt_println_v(err);
    return;
}
bi->go_version = BURROW_S("go1.27.1");
DebugBuildSetting extra = {BURROW_S("-ldflags"), BURROW_S("-X main.version=1.2.0")};
bi->settings = slice_append(bi->a, bi->settings, &extra, 1);
fmt_print_v(debug_build_info_string(bi, a));
debug_build_info_free(bi);

// A module line with only a path in it.
bi = debug_parse_build_info(a, BURROW_S("path\tp\nmod\texample.com/hello\n"), &err);
if (bi == NULL)
    fmt_println_v(err);
```

That prints:

```
OUTPUT_PARSE
```

`debug_read_build_info`, which in Go gives the record of the running program, gives NULL and false, since a C program has none.

## How it is tested

The tests check the output against Go's own. `tools/gen-debug-tests.sh` runs Go's `debug/elf` and `debug/dwarf` over every file in `debug/elf`'s testdata and the ELF files in `debug/dwarf`'s and writes down a text dump of everything they read: headers, sections, symbols, dynamic tables, every DWARF entry and type, and every line table row. The C tests build the same dump and compare it line by line. They do the same for about 1,850 damaged copies of the files, comparing a hash of the dump, so a malformed file gets the same error here as in Go. `tools/gen-debug-tests.sh macho` does the same for the files in `debug/macho`'s testdata and the Mach-O files in `debug/dwarf`'s, reading each one both as a universal file and as a plain one, `tools/gen-debug-tests.sh pe` for the files in `debug/pe`'s testdata and the PE file in `debug/dwarf`'s, and `tools/gen-debug-tests.sh plan9obj` for the two files in `debug/plan9obj`'s testdata, with the damage aimed at the symbol table. `tools/gen-debug-tests.sh gosym` builds `debug/gosym`'s `pclinetest` program with the Go on the machine and runs it, the Go 1.15 pclntab in its testdata, and symbol tables in each of the old formats, through every lookup the package has. The tests in Go's packages that look at one value at a time are ported as well. `tools/gen-debug-tests.sh buildinfo` builds a small Go program for Linux, Windows, macOS, AIX and Plan 9 on several architectures, as plain and position independent executables, as a universal macOS binary, and in GOPATH mode, and records what `debug/buildinfo` reads from each of them and from the binaries in its testdata, whole and damaged. It also records what `runtime/debug`'s `ParseBuildInfo` makes of a few hundred inputs, good and bad.
