/* Turning an address back into a name.
 *
 * burrow/trace.h walks the stack and hands back a list of program counters,
 * which is half an answer. "0x10238c1f4" says the program was somewhere; the
 * name of the function it was in says what it was doing, and that is the part
 * anybody reading a crash report actually wants. This is the other half.
 *
 * The names come from tools/burrow-symtab, which runs nm over the objects the
 * build just produced and writes out an address and a name for every global
 * function it finds. That table compiles into the library like any other file,
 * so it is right for the platform being built and nobody has to keep three
 * checked in copies of it in step.
 *
 * Static functions are not in it, which is about a third of them, because a
 * static function has no name outside the file it is written in and the table
 * is another file. An address in one resolves to the nearest global function
 * below it, which is at worst the wrong name in the right neighbourhood, and
 * the offset in the answer is what makes that readable: a large one means the
 * name is a neighbour rather than a hit.
 *
 * There is no file and no line either, because nm has neither and reading them
 * out of the debug information is a DWARF parser and a PDB reader. Both arrive
 * with the amalgamation, which is one translation unit where a static function
 * can be named and where a generator can read the source exactly.
 * docs/design/06-runtime.md section 11 is where that is written down.
 *
 * What it costs is that naming every global function keeps every object that
 * defines one, so a program linking burrow statically gets all of it rather
 * than the part it called. Measured on one of the smaller test binaries, which
 * is the worst case because it uses almost none of the library: 173 kilobytes
 * without the table and 272 with. The table itself is 76 of those and the rest
 * is objects the linker could otherwise have dropped. SYMTAB=0 builds an empty
 * table and gives all of that back. It is on by default because a program that
 * uses the runtime is reachable from the runtime anyway, and because the
 * amalgamation is one object the linker keeps whole either way.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SYMTAB_H
#define BURROW_SYMTAB_H

#include "burrow/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The generated table: two arrays of the same length and a count.
 *
 * Parallel arrays rather than an array of structs, because the file that
 * defines them includes no header and so cannot name a struct type that
 * anything else agrees with. Everything here is spellable in plain C, which is
 * what makes that possible. See tools/burrow-symtab.
 *
 * In no particular order. Link order is not address order on every platform, so
 * sorting is this file's job rather than the generator's, and it happens once
 * on the first lookup.
 *
 * The count is an int rather than an Int for the same reason the arrays are
 * parallel: the generator cannot spell Int without a header. */
extern void (*const burrow__symbol_addrs[])(void);
extern const char *const burrow__symbol_names[];
extern const int burrow__symbol_count;

/* What a lookup answers.
 *
 * name borrows from the table, which is read only data with the lifetime of the
 * program, so it is safe to hold and never needs freeing. entry is where the
 * function starts, and the caller subtracts it from the address it asked about
 * to get the offset worth printing. */
typedef struct burrow__Frame {
    Str name;
    Uintptr entry;
} burrow__Frame;

/* The function containing pc, or false when there is nothing to say.
 *
 * False is a normal answer. It is what an address outside burrow gives, which
 * is most of a traceback in a program that calls into burrow rather than the
 * other way round, and it is what every address gives in a build with an empty
 * table. A caller that gets false prints the address, which is what it would
 * have printed anyway.
 *
 * pc is looked up as it is given. A return address points at the instruction
 * after the call, which for a call in tail position is the next function
 * entirely, so a caller holding one subtracts one before asking and then
 * computes the offset it prints from the address it actually had.
 *
 * Safe on any address, including zero and one that is not mapped. It compares
 * integers and never reads through pc, which matters because the first caller
 * of this is a program that is already failing.
 *
 * The first call sorts the table and may allocate to do it. If that allocation
 * fails the answer is the same one, found by walking the table instead, so a
 * traceback printed on the way out of memory still has names in it. */
bool burrow__symbolise(Uintptr pc, burrow__Frame *out);

/* How many names there are, which is zero in a build with SYMTAB=0.
 *
 * Here so that a test can tell an empty table from a broken lookup, and so that
 * a program can say which kind of build it is. */
Int burrow__symtab_len(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SYMTAB_H */
