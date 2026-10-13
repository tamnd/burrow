/* debug/gosym, the symbol and line number tables of Go binaries.
 *
 * A Go binary carries a table that maps program counters to functions, files
 * and lines, the pclntab, which every Go since 1.2 writes in one of four
 * formats. Binaries from before 1.3 also carry a symbol table of their own,
 * and before 1.2 the line table was a different, simpler thing. This reads
 * all of them. The bytes come from the binary's sections, which debug/elf,
 * debug/macho or debug/pe give: ".gopclntab" and ".gosymtab" for ELF, and the
 * address of ".text" for the start of the code.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     GosymLineTable *lt = gosym_new_line_table(a, pclntab, text_addr);
 *     Error err = BURROW_NO_ERROR;
 *     GosymTable *t = gosym_new_table(a, symtab, lt, &err);
 *     if (t == NULL)
 *         ...
 *     Int line = 0;
 *     GosymFunc *fn = NULL;
 *     Str file = gosym_table_pc_to_line(t, pc, &line, &fn);
 *     ...
 *     gosym_table_free(t);
 *     gosym_line_table_free(lt);
 *     arena_free(&ar);
 *
 * Neither a GosymLineTable nor a GosymTable copies the bytes it is given, so
 * those have to stay as they are while either is used. A GosymTable keeps its
 * symbols, functions and names in an arena of its own on the allocator it was
 * made with, and gosym_table_free gives all of it back. The line table passed
 * to gosym_new_table has to outlive the table, and the file names that come
 * back from a lookup point into the line table's bytes or into the table.
 *
 * Like Go's, this checks only enough to read the tables safely. A malformed
 * table gives wrong answers, a function of -1 for a line, or no functions at
 * all, rather than an error.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package debug/gosym */

#ifndef BURROW_DEBUG_GOSYM_H
#define BURROW_DEBUG_GOSYM_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GosymFunc GosymFunc;
typedef struct GosymObj GosymObj;
typedef struct GosymLineTable GosymLineTable;

/* gosym.Sym, one symbol. type is its letter, such as 'T' for a function in
 * the text segment or 'D' for data, and func is the function a text symbol
 * starts, or NULL. go_version is the table's format, which package_name needs
 * to know. */
typedef struct GosymSym {
    uint64_t value;
    uint8_t type;
    Str name;
    uint64_t go_type;
    GosymFunc *func;

    int go_version;
} GosymSym;
extern const Type burrow_type_GosymSym;
#define TYPE_GOSYM_SYM TYPE_OF(GosymSym)

/* Sym.Static: whether the symbol is local to its file, which a lower case
 * letter says. */
bool gosym_sym_static(const GosymSym *s);

/* Sym.PackageName: the package part of the name, such as "fmt" for
 * "fmt.Println" or "example.com/a/b" for "example.com/a/b.F", or "" for none.
 * Brackets of a generic name are skipped, so the dots inside them do not
 * count. The result is part of s->name. */
BURROW_BORROWS(ret, s) Str gosym_sym_package_name(const GosymSym *s);

/* Sym.ReceiverName: the receiver's type of a method, such as "Buffer" for
 * "bytes.(*Buffer).Write", without the brackets of a generic one, or "" for
 * a name that is not a method. The result is part of s->name. */
BURROW_BORROWS(ret, s) Str gosym_sym_receiver_name(const GosymSym *s);

/* Sym.BaseName: the name after the package and the receiver. The result is
 * part of s->name. */
BURROW_BORROWS(ret, s) Str gosym_sym_base_name(const GosymSym *s);

/* gosym.Func, one function. Go embeds the *Sym, so its name and value are
 * sym's. end is where the next function starts. params and locals hold a
 * GosymSym * each, and are empty for Go 1.3 and later, whose symbol tables
 * no longer have them. line_table is the table that answers for the
 * function's code, and obj the object file it came from. */
struct GosymFunc {
    uint64_t entry;
    GosymSym *sym;
    uint64_t end;
    Slice params;
    Slice locals;
    Int frame_size;
    GosymLineTable *line_table;
    GosymObj *obj;
};
extern const Type burrow_type_GosymFunc;
#define TYPE_GOSYM_FUNC TYPE_OF(GosymFunc)

/* The methods Func gets from its embedded *Sym, which is f->sym. */
bool gosym_func_static(const GosymFunc *f);
BURROW_BORROWS(ret, f) Str gosym_func_package_name(const GosymFunc *f);
BURROW_BORROWS(ret, f) Str gosym_func_receiver_name(const GosymFunc *f);
BURROW_BORROWS(ret, f) Str gosym_func_base_name(const GosymFunc *f);

/* gosym.Obj, one object file: its functions, a GosymFunc each, and the 'z'
 * and 'Z' symbols that name its source files, a GosymSym each. Both point
 * into the GosymTable's lists. For Go 1.2 and later there is one Obj for the
 * whole binary and paths is empty. */
struct GosymObj {
    Slice funcs;
    Slice paths;
};
extern const Type burrow_type_GosymObj;
#define TYPE_GOSYM_OBJ TYPE_OF(GosymObj)

/* gosym.LineTable, a pclntab, or for binaries from before Go 1.2 the old line
 * table that starts at pc with line line. data is the table's bytes. The rest
 * is the LineTable's: what it learns of the format the first time it is used,
 * and a map of file names it builds when it needs one. */
struct GosymLineTable {
    Slice data;
    uint64_t pc;
    Int line;

    SyncMutex mu;
    int version;
    bool big_endian;
    uint32_t quantum;
    uint32_t ptrsize;
    uint64_t text_start;
    Slice funcnametab;
    Slice cutab;
    Slice funcdata;
    Slice functab;
    uint32_t nfunctab;
    Slice filetab;
    Slice pctab;
    uint32_t nfiletab;
    Map *file_map;
    Alloc *a;
    Alloc *parent;
    Arena arena;
};

/* gosym.NewLineTable: the line table in data, for code that starts at text,
 * which is the address of the binary's text segment. Nothing is read until
 * the table is used, and data is not copied. Free it with
 * gosym_line_table_free. */
BURROW_OWNS(ret) GosymLineTable *gosym_new_line_table(Alloc *a, Slice data,
                                                      uint64_t text);

/* Gives back a table gosym_new_line_table made. NULL is fine. The ones a
 * GosymTable makes for the functions of an old binary are the table's, and
 * go with it. */
void gosym_line_table_free(GosymLineTable *t);

/* LineTable.PCToLine: the line for pc, or -1 for a pclntab that has no
 * function at pc. Go deprecates it in favour of gosym_table_pc_to_line. */
Int gosym_line_table_pc_to_line(GosymLineTable *t, uint64_t pc);

/* LineTable.LineToPC: the first pc below maxpc that has line, or 0. It only
 * works for the old tables and gives 0 for a pclntab. Go deprecates it in
 * favour of gosym_table_line_to_pc. */
uint64_t gosym_line_table_line_to_pc(GosymLineTable *t, Int line, uint64_t maxpc);

/* gosym.Table, a binary's symbols and functions. syms holds a GosymSym for
 * each entry of the symbol table, and is empty for Go 1.3 and later. funcs
 * holds a GosymFunc for each function, sorted by entry, and objs a GosymObj
 * for each object file. files maps each source file's name, a Str, to its
 * object file, a GosymObj *; read it with map_get2. The rest is the Table's. */
typedef struct GosymTable {
    Slice syms;
    Slice funcs;
    Map *files;
    Slice objs;

    GosymLineTable *go12line;
    Alloc *a;
    Alloc *parent;
    Arena arena;
} GosymTable;

/* gosym.NewTable: the table for the symbol table symtab and the line table
 * pcln, which must not be NULL. symtab can be empty, as it is in any binary
 * from Go 1.3 on. A symbol table that cannot be decoded gives NULL and a
 * GosymDecodingError.
 *
 * Like Go's, this rewrites the "·" in a name of symtab to "." where it finds
 * it, in place, so symtab has to be writable. */
BURROW_OWNS(ret) GosymTable *gosym_new_table(Alloc *a, Slice symtab,
                                             GosymLineTable *pcln, Error *err);

/* Gives back everything t holds. NULL is fine. */
void gosym_table_free(GosymTable *t);

/* Table.PCToFunc: the function that has pc, or NULL. */
BURROW_BORROWS(ret, t) GosymFunc *gosym_table_pc_to_func(GosymTable *t, uint64_t pc);

/* Table.PCToLine: the file and line of pc, and through fn the function that
 * has it. With no function at pc, the file is "", the line 0 and fn NULL.
 * line and fn can be NULL. */
BURROW_BORROWS(ret, t) Str gosym_table_pc_to_line(GosymTable *t, uint64_t pc, Int *line,
                                                  GosymFunc **fn);

/* Table.LineToPC: the first pc of the line line of file, and through fn its
 * function. A file the table does not know gives a GosymUnknownFileError,
 * and a line with no code a GosymUnknownLineError. fn can be NULL. */
uint64_t gosym_table_line_to_pc(GosymTable *t, Str file, Int line, GosymFunc **fn,
                                Error *err);

/* Table.LookupSym: the text, data or bss symbol called name, or NULL. */
BURROW_BORROWS(ret, t) GosymSym *gosym_table_lookup_sym(GosymTable *t, Str name);

/* Table.LookupFunc: the function called name, or NULL. */
BURROW_BORROWS(ret, t) GosymFunc *gosym_table_lookup_func(GosymTable *t, Str name);

/* Table.SymByAddr: the text, data or bss symbol at addr, or NULL. */
BURROW_BORROWS(ret, t) GosymSym *gosym_table_sym_by_addr(GosymTable *t, uint64_t addr);

/* ------------------------------------------------------------------ errors */

/* gosym.UnknownFileError, the name of a file the table does not have. Its
 * text is "unknown file: " and the name, and errors_as with
 * TYPE_GOSYM_UNKNOWN_FILE_ERROR gives back the name. */
typedef Str GosymUnknownFileError;
extern const Type burrow_type_GosymUnknownFileError;
#define TYPE_GOSYM_UNKNOWN_FILE_ERROR TYPE_OF(GosymUnknownFileError)

/* UnknownFileError.Error, in a. */
BURROW_OWNS(ret) Str gosym_unknown_file_error_error(GosymUnknownFileError e, Alloc *a);

/* gosym.UnknownLineError, a line of a file that has no code, such as a
 * comment. Its text is "no code at file:line". */
typedef struct GosymUnknownLineError {
    Str file;
    Int line;
} GosymUnknownLineError;
extern const Type burrow_type_GosymUnknownLineError;
#define TYPE_GOSYM_UNKNOWN_LINE_ERROR TYPE_OF(GosymUnknownLineError)

/* UnknownLineError.Error, in a. */
BURROW_OWNS(ret) Str gosym_unknown_line_error_error(const GosymUnknownLineError *e,
                                                    Alloc *a);

/* gosym.DecodingError, a symbol table gosym_new_table could not decode: what
 * was wrong, the value that was, when there is one, and where. Go keeps the
 * value as an any and formats it when Error is called. Here val is that text
 * already, made with %v, and has_val says whether there was a value. */
typedef struct GosymDecodingError {
    Int off;
    Str msg;
    Str val;
    bool has_val;
} GosymDecodingError;
extern const Type burrow_type_GosymDecodingError;
#define TYPE_GOSYM_DECODING_ERROR TYPE_OF(GosymDecodingError)

/* DecodingError.Error: "invalid pointer size '3' at byte 0x7", in a. */
BURROW_OWNS(ret) Str gosym_decoding_error_error(const GosymDecodingError *e, Alloc *a);

#ifdef __cplusplus
}
#endif

#endif
