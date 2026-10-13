/* Derived from Go's src/debug/gosym/pclntab_test.go and symtab_test.go. Go
 * source: go1.27.1.
 *
 * Go's tests read the Go 1.15 pclntab in its testdata, build pclinetest from
 * its testdata and walk two functions written in assembly, and check how the
 * names of symbols split up. All of that is here, with pclinetest built by
 * tools/gen-debug-tests.sh gosym. On top of that, those two tables, the Go
 * 1.15 one with a symbol table next to it, and symbol tables in the old
 * formats with an old line table each go through a dump of everything
 * debug/gosym can say about them, which the generator made with Go's package,
 * and the dump this makes has to be the same text. Each is damaged a few dozen
 * ways too, and then the dumps have to hash the same.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/sha256.h"
#include "burrow/debug/gosym.h"
#include "burrow/encoding/binary.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"
#include "burrow/sort.h"
#include "burrow/strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_dump.h"

#include "gosym_test_gen.h"

/* --------------------------------------------------------------------- dump */

/* The dump below is the one in tools/gen-debug-tests.sh, line for line. */

/* Prints a hash of all the lines and then some of them: the first dozen,
 * every 128th and the last. */
static void gosym_list(Alloc *a, IoWriter w, const char *label, Slice lines) {
    BytesBuffer all = BYTES_BUFFER(a);
    for (Int i = 0; i < lines.len; i++)
        fmt_fprintf_v(bytes_buffer_as_io_writer(&all), "%s\n",
                      BURROW_AT(Str, lines, i));
    Sha256Sum256Ret h = sha256_sum256(bytes_buffer_bytes(&all));
    fmt_fprintf_v(w, "%s n=%d sha=%s\n", label, lines.len,
                  hex_encode_to_string(a, (Slice){h.a, 8, 8, TYPE_BYTE}));
    for (Int i = 0; i < lines.len; i++) {
        if (i < 12 || i % 128 == 0 || i == lines.len - 1)
            fmt_fprintf_v(w, "  %s\n", BURROW_AT(Str, lines, i));
    }
}

/* What Go prints for a []string with %q. */
static Str quote_list(Alloc *a, Slice syms) {
    BytesBuffer b = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&b);
    fmt_fprintf_v(w, "[");
    for (Int i = 0; i < syms.len; i++)
        fmt_fprintf_v(w, i > 0 ? " %q" : "%q", BURROW_AT(GosymSym *, syms, i)->name);
    fmt_fprintf_v(w, "]");
    return bytes_buffer_string(&b, a);
}

static Int func_index(const GosymTable *t, const GosymFunc *f) {
    const GosymFunc *base = (const GosymFunc *)t->funcs.p;
    if (f == NULL || base == NULL || f < base || f >= base + t->funcs.len)
        return -1;
    return (Int)(f - base);
}

static Int sym_index(const GosymTable *t, const GosymSym *s) {
    const GosymSym *base = (const GosymSym *)t->syms.p;
    if (s == NULL || base == NULL || s < base || s >= base + t->syms.len)
        return -1;
    return (Int)(s - base);
}

static Int obj_index(const GosymTable *t, const GosymObj *o) {
    const GosymObj *base = (const GosymObj *)t->objs.p;
    if (o == NULL || base == NULL || o < base || o >= base + t->objs.len)
        return -1;
    return (Int)(o - base);
}

/* Runs fn(ctx) and says whether it panicked, which is gosymTry. */
static bool gosym_try(void (*fn)(void *ctx), void *ctx) {
    volatile bool panicked = true;
    BURROW_TRY {
        fn(ctx);
        panicked = false;
    }
    BURROW_CATCH(p) {
        (void)p;
    }
    BURROW_TRY_END;
    return panicked;
}

typedef struct NewTableCall {
    Alloc *a;
    Slice symtab;
    GosymLineTable *lt;
    GosymTable *tab;
    Error err;
} NewTableCall;

static void call_new_table(void *ctx) {
    NewTableCall *c = (NewTableCall *)ctx;
    c->tab = gosym_new_table(c->a, c->symtab, c->lt, &c->err);
}

typedef struct PCToLineCall {
    GosymTable *tab;
    uint64_t pc;
    Str file;
    Int line;
    GosymFunc *fn;
} PCToLineCall;

static void call_pc_to_line(void *ctx) {
    PCToLineCall *c = (PCToLineCall *)ctx;
    c->file = gosym_table_pc_to_line(c->tab, c->pc, &c->line, &c->fn);
}

typedef struct LineToPCCall {
    GosymTable *tab;
    Str file;
    Int line;
    uint64_t pc;
    GosymFunc *fn;
    Error err;
} LineToPCCall;

static void call_line_to_pc(void *ctx) {
    LineToPCCall *c = (LineToPCCall *)ctx;
    c->pc = gosym_table_line_to_pc(c->tab, c->file, c->line, &c->fn, &c->err);
}

/* LineTable.Info in the generator's copy of debug/gosym. Go reads the header
 * there with isGo12, which is not exported, so this asks for the line of a pc,
 * which reads it the same way and changes nothing else. */
static void dump_line_table_info(IoWriter w, GosymLineTable *lt) {
    (void)gosym_line_table_pc_to_line(lt, 0);
    fmt_fprintf_v(
        w,
        "linetable version=%d quantum=%d ptrsize=%d textstart=%#x nfunctab=%d "
        "nfiletab=%d tabs=%d,%d,%d,%d,%d,%d",
        lt->version, lt->quantum, lt->ptrsize, lt->text_start, lt->nfunctab,
        lt->nfiletab, lt->funcnametab.len, lt->cutab.len, lt->funcdata.len,
        lt->functab.len, lt->filetab.len, lt->pctab.len);
    if (lt->version >= 2)
        fmt_fprintf_v(w, lt->big_endian ? " big" : " little");
    fmt_fprintf_v(w, "\n");
}

static Slice sym_lines(Alloc *a, GosymTable *tab) {
    Slice lines = slice_make(a, TYPE_STRING, 0, 0);
    for (Int i = 0; i < tab->syms.len; i++) {
        const GosymSym *s = (const GosymSym *)tab->syms.p + i;
        Str line = fmt_sprintf_v(
            a, "%#x %d %q gotype=%#x func=%d static=%t pkg=%q recv=%q base=%q",
            s->value, s->type, s->name, s->go_type, func_index(tab, s->func),
            (bool)gosym_sym_static(s), gosym_sym_package_name(s),
            gosym_sym_receiver_name(s), gosym_sym_base_name(s));
        lines = BURROW_APPEND(Str, a, lines, line);
    }
    return lines;
}

static Slice func_lines(Alloc *a, GosymTable *tab, GosymLineTable *lt) {
    Slice lines = slice_make(a, TYPE_STRING, 0, 0);
    for (Int i = 0; i < tab->funcs.len; i++) {
        const GosymFunc *f = (const GosymFunc *)tab->funcs.p + i;
        Str desc = BURROW_S("nil");
        if (f->line_table == lt)
            desc = BURROW_S("shared");
        else if (f->line_table != NULL)
            desc = fmt_sprintf_v(a, "pc=%#x line=%d len=%d", f->line_table->pc,
                                 f->line_table->line, f->line_table->data.len);
        Str line = fmt_sprintf_v(a, "%#x-%#x %q frame=%d params=%s locals=%s obj=%d %s",
                                 f->entry, f->end, f->sym->name, f->frame_size,
                                 quote_list(a, f->params), quote_list(a, f->locals),
                                 obj_index(tab, f->obj), desc);
        lines = BURROW_APPEND(Str, a, lines, line);
    }
    return lines;
}

static Slice obj_lines(Alloc *a, GosymTable *tab) {
    Slice lines = slice_make(a, TYPE_STRING, 0, 0);
    for (Int i = 0; i < tab->objs.len; i++) {
        const GosymObj *o = (const GosymObj *)tab->objs.p + i;
        Int first = -1;
        if (o->funcs.len > 0)
            first = func_index(tab, (const GosymFunc *)o->funcs.p);
        BytesBuffer b = BYTES_BUFFER(a);
        IoWriter w = bytes_buffer_as_io_writer(&b);
        fmt_fprintf_v(w, "[");
        for (Int k = 0; k < o->paths.len; k++) {
            const GosymSym *s = (const GosymSym *)o->paths.p + k;
            Str path = fmt_sprintf_v(a, "%#x:%s", s->value, s->name);
            fmt_fprintf_v(w, k > 0 ? " %q" : "%q", path);
        }
        fmt_fprintf_v(w, "]");
        Str line = fmt_sprintf_v(a, "funcs=%d first=%d paths=%s", o->funcs.len, first,
                                 bytes_buffer_string(&b, a));
        lines = BURROW_APPEND(Str, a, lines, line);
    }
    return lines;
}

static Slice sorted_files(Alloc *a, GosymTable *tab) {
    Slice files = slice_make(a, TYPE_STRING, 0, 0);
    const void *k = NULL;
    void *v = NULL;
    for (MapIter it = map_iter(tab->files); map_next(&it, &k, &v);)
        files = BURROW_APPEND(Str, a, files, *(const Str *)k);
    sort_strings(files);
    return files;
}

static void dump_queries(Alloc *a, IoWriter w, GosymTable *tab, uint64_t text,
                         Slice files) {
    /* PCToFunc, SymByAddr and PCToLine at the edges of some functions. */
    Slice pcs = slice_make(a, TYPE_UINT64, 0, 0);
    uint64_t edges[] = {0, text - 1, text, ~UINT64_C(0)};
    for (size_t i = 0; i < sizeof edges / sizeof edges[0]; i++)
        pcs = BURROW_APPEND(uint64_t, a, pcs, edges[i]);
    Int n = tab->funcs.len;
    for (Int i = 0; i < n; i++) {
        if (i < 4 || i % 256 == 0 || i == n - 1) {
            const GosymFunc *f = (const GosymFunc *)tab->funcs.p + i;
            pcs = BURROW_APPEND(uint64_t, a, pcs, f->entry);
            pcs = BURROW_APPEND(uint64_t, a, pcs, f->entry + 1);
            pcs = BURROW_APPEND(uint64_t, a, pcs, f->end - 1);
            pcs = BURROW_APPEND(uint64_t, a, pcs, f->end);
        }
    }
    Str back_file[3];
    Int back_line[3];
    int nback = 0;
    for (Int i = 0; i < pcs.len; i++) {
        uint64_t pc = BURROW_AT(uint64_t, pcs, i);
        Str name = BURROW_S("nil");
        GosymSym *s = gosym_table_sym_by_addr(tab, pc);
        if (s != NULL)
            name = fmt_sprintf_v(a, "%q", s->name);
        PCToLineCall c = {tab, pc, BURROW_STR_EMPTY, 0, NULL};
        Str where = BURROW_S("panic");
        if (!gosym_try(call_pc_to_line, &c)) {
            where =
                fmt_sprintf_v(a, "%q:%d fn=%d", c.file, c.line, func_index(tab, c.fn));
            if (c.fn != NULL && nback < 3) {
                back_file[nback] = c.file;
                back_line[nback] = c.line;
                nback++;
            }
        }
        fmt_fprintf_v(w, "pc %#x func=%d sym=%s line %s\n", pc,
                      func_index(tab, gosym_table_pc_to_func(tab, pc)), name, where);
    }

    /* LineToPC for some of the files, a file that is not there, and the lines
     * PCToLine gave above. */
    Str try_file[16];
    Int try_line[16];
    int ntry = 0;
    if (files.len > 0) {
        Int ks[] = {0, files.len / 2, files.len - 1};
        Int lines[] = {0, 1, 9, 40};
        for (size_t i = 0; i < 3; i++) {
            for (size_t j = 0; j < 4; j++) {
                try_file[ntry] = BURROW_AT(Str, files, ks[i]);
                try_line[ntry] = lines[j];
                ntry++;
            }
        }
    }
    try_file[ntry] = BURROW_S("nope.go");
    try_line[ntry] = 1;
    ntry++;
    for (int i = 0; i < ntry + nback; i++) {
        LineToPCCall c = {tab,
                          i < ntry ? try_file[i] : back_file[i - ntry],
                          i < ntry ? try_line[i] : back_line[i - ntry],
                          0,
                          NULL,
                          BURROW_NO_ERROR};
        if (gosym_try(call_line_to_pc, &c)) {
            fmt_fprintf_v(w, "ltp %q:%d panic\n", c.file, c.line);
            continue;
        }
        fmt_fprintf_v(w, "ltp %q:%d pc=%#x fn=%d%s\n", c.file, c.line, c.pc,
                      func_index(tab, c.fn), errs(a, c.err));
    }

    Slice names = slice_make(a, TYPE_STRING, 0, 0);
    names = BURROW_APPEND(Str, a, names, BURROW_S("main.main"));
    names = BURROW_APPEND(Str, a, names, BURROW_S("runtime.main"));
    names = BURROW_APPEND(Str, a, names, BURROW_S("nope"));
    for (Int i = 0; i < 2 && i < tab->syms.len; i++)
        names = BURROW_APPEND(Str, a, names, ((GosymSym *)tab->syms.p)[i].name);
    for (Int i = 0; i < 2 && i < tab->funcs.len; i++)
        names = BURROW_APPEND(Str, a, names, ((GosymFunc *)tab->funcs.p)[i].sym->name);
    for (Int i = 0; i < names.len; i++) {
        Str name = BURROW_AT(Str, names, i);
        fmt_fprintf_v(w, "lookup %q sym=%d func=%d\n", name,
                      sym_index(tab, gosym_table_lookup_sym(tab, name)),
                      func_index(tab, gosym_table_lookup_func(tab, name)));
    }
}

static void dump(Alloc *a, IoWriter w, Slice data) {
    if (data.len < 12) {
        fmt_fprintf_v(w, "short %d\n", data.len);
        return;
    }
    uint64_t text = binary_little_endian_uint64(data);
    const Byte *d = (const Byte *)data.p;
    Int l = (Int)((uint32_t)d[8] | (uint32_t)d[9] << 8 | (uint32_t)d[10] << 16 |
                  (uint32_t)d[11] << 24);
    if (l > data.len - 12) {
        fmt_fprintf_v(w, "short %d symtab=%d\n", data.len, l);
        return;
    }
    Slice symtab = slice_make(a, TYPE_BYTE, l, l);
    if (l > 0)
        memcpy(symtab.p, (const Byte *)data.p + 12, (size_t)l);
    Slice pcln = slice_sub3(data, 12 + l, data.len, data.cap);
    fmt_fprintf_v(w, "text=%#x symtab=%d pcln=%d\n", text, l, pcln.len);
    GosymLineTable *lt = gosym_new_line_table(a, pcln, text);
    if (lt == NULL)
        abort();
    dump_line_table_info(w, lt);
    uint64_t pcs[] = {text - 1,  text + 0,  text + 1,     text + 2,     text + 5,
                      text + 16, text + 64, text + 0x100, text + 0x1000};
    for (size_t i = 0; i < sizeof pcs / sizeof pcs[0]; i++)
        fmt_fprintf_v(w, "lt pc %#x line=%d\n", pcs[i],
                      gosym_line_table_pc_to_line(lt, pcs[i]));
    Int lines[] = {-1, 0, 1, 2, 3, 5, 10, 20, 50, 100};
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
        fmt_fprintf_v(w, "lt line %d pc=%#x\n", lines[i],
                      gosym_line_table_line_to_pc(lt, lines[i], text + 0x10000));

    NewTableCall c = {a, symtab, lt, NULL, BURROW_NO_ERROR};
    if (gosym_try(call_new_table, &c)) {
        fmt_fprintf_v(w, "newtable panic\n");
        return;
    }
    if (BURROW_FAILED(c.err)) {
        fmt_fprintf_v(w, "newtable err=%q\n", error_text(c.err));
        return;
    }
    GosymTable *tab = c.tab;
    fmt_fprintf_v(w, "table go12=%t\n", (bool)(tab->go12line != NULL));
    gosym_list(a, w, "syms", sym_lines(a, tab));
    gosym_list(a, w, "funcs", func_lines(a, tab, lt));
    gosym_list(a, w, "objs", obj_lines(a, tab));
    Slice files = sorted_files(a, tab);
    Slice flines = slice_make(a, TYPE_STRING, 0, 0);
    for (Int i = 0; i < files.len; i++) {
        Str name = BURROW_AT(Str, files, i);
        GosymObj *o = NULL;
        (void)map_get2(tab->files, &name, &o);
        flines = BURROW_APPEND(Str, a, flines,
                               fmt_sprintf_v(a, "%q obj=%d", name, obj_index(tab, o)));
    }
    gosym_list(a, w, "files", flines);
    dump_queries(a, w, tab, text, files);

    /* NewTable rewrote the names in symtab, so a second one sees them as they
     * are now. */
    NewTableCall c2 = {a, symtab, lt, NULL, BURROW_NO_ERROR};
    if (gosym_try(call_new_table, &c2)) {
        fmt_fprintf_v(w, "again panic\n");
    } else if (BURROW_FAILED(c2.err)) {
        fmt_fprintf_v(w, "again err=%q\n", error_text(c2.err));
    } else {
        Slice again = slice_make(a, TYPE_STRING, 0, 0);
        for (Int i = 0; i < c2.tab->syms.len; i++)
            again = BURROW_APPEND(
                Str, a, again,
                fmt_sprintf_v(a, "%q", ((GosymSym *)c2.tab->syms.p)[i].name));
        gosym_list(a, w, "again", again);
        gosym_table_free(c2.tab);
    }
    gosym_table_free(tab);
}

/* ---------------------------------------------------------------- the tests */

static void TestDump(TestingT *t) {
    debug_check_dumps(t, gosym_files, sizeof gosym_files / sizeof gosym_files[0], dump);
}

static void TestDamage(TestingT *t) {
    CHECK(debug_check_damage(t, gosym_files, sizeof gosym_files / sizeof gosym_files[0],
                             dump) > 300);
}

/* -------------------------------------------------------- Go's other tests */

static const DebugFile *find_file(const char *name) {
    for (size_t i = 0; i < sizeof gosym_files / sizeof gosym_files[0]; i++) {
        if (strcmp(gosym_files[i].name, name) == 0)
            return &gosym_files[i];
    }
    abort();
}

/* The pclntab of a test file, which comes after its text address and symbol
 * table. */
static Slice file_pcln(Alloc *a, const char *name, Slice *symtab, uint64_t *text) {
    Slice data = file_bytes(a, find_file(name));
    if (data.p == NULL || data.len < 12)
        abort();
    *text = binary_little_endian_uint64(data);
    const Byte *d = (const Byte *)data.p;
    Int l = (Int)((uint32_t)d[8] | (uint32_t)d[9] << 8 | (uint32_t)d[10] << 16 |
                  (uint32_t)d[11] << 24);
    *symtab = slice_sub(data, 12, 12 + l);
    return slice_sub(data, 12 + l, data.len);
}

static void Test115PclnParsing(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice symtab;
    uint64_t text = 0;
    Slice dat = file_pcln(a, "gosym/pcln115", &symtab, &text);
    CHECK(text == 0x1001000);
    GosymLineTable *pcln = gosym_new_line_table(a, dat, 0x1001000);
    Error err = BURROW_NO_ERROR;
    GosymTable *tab = gosym_new_table(a, (Slice){NULL, 0, 0, TYPE_BYTE}, pcln, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    GosymFunc *f = NULL;
    uint64_t pc = gosym_table_line_to_pc(tab, BURROW_S("/tmp/hello.go"), 3, &f, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    /* Go's ver12. */
    if (pcln->version != 2)
        testing_t_errorf_v(t, "Expected pcln to parse as an older version");
    if (pc != 0x105c280)
        testing_t_errorf_v(t, "expect pc = 0x105c280, got %#x", pc);
    if (f == NULL || !str_eq(f->sym->name, BURROW_S("main.main")))
        testing_t_errorf_v(t, "expected to parse name as main.main");
    gosym_table_free(tab);
    gosym_line_table_free(pcln);
    arena_free(&ar);
}

/* TestPCLine reads the text of the two functions in pclinetest.s, whose bytes
 * say how many lines on each instruction is. Go reads it from the binary it
 * builds; it is in the generated header here. */
static void TestPCLine(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice symtab;
    uint64_t text = 0;
    Slice pclndat = file_pcln(a, "gosym/pclinetest", &symtab, &text);
    Slice textdat = gen_bytes(a, gosym_pcline_text);
    const Byte *td = (const Byte *)textdat.p;
    uint64_t addr = GOSYM_PCLINE_TEXT_ADDR;
    GosymLineTable *pcln = gosym_new_line_table(a, pclndat, text);
    Error err = BURROW_NO_ERROR;
    GosymTable *tab = gosym_new_table(a, symtab, pcln, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }

    /* PCToLine. */
    GosymFunc *sym = gosym_table_lookup_func(tab, BURROW_S("main.linefrompc"));
    if (sym == NULL) {
        testing_t_fatalf_v(t, "no main.linefrompc");
        return;
    }
    Int want_line = 0;
    for (uint64_t pc = sym->entry; pc < sym->end; pc++) {
        uint64_t off = pc - addr;
        if (off >= (uint64_t)textdat.len || td[off] == 255)
            break;
        want_line += td[off];
        GosymFunc *fn = NULL;
        Int line = 0;
        Str file = gosym_table_pc_to_line(tab, pc, &line, &fn);
        if (fn == NULL)
            testing_t_errorf_v(t, "failed to get line of PC %#x", pc);
        else if (!strings_has_suffix(file, BURROW_S("pclinetest.s")) ||
                 line != want_line || fn != sym)
            testing_t_errorf_v(t, "PCToLine(%#x) = %s:%d (%s), want %s:%d (%s)", pc,
                               file, line, fn->sym->name, BURROW_S("pclinetest.s"),
                               want_line, sym->sym->name);
    }

    /* LineToPC. */
    sym = gosym_table_lookup_func(tab, BURROW_S("main.pcfromline"));
    if (sym == NULL) {
        testing_t_fatalf_v(t, "no main.pcfromline");
        return;
    }
    Int lookupline = -1;
    want_line = 0;
    uint64_t off = 0;
    int checked = 0;
    for (uint64_t pc = sym->sym->value; pc < sym->end; pc += 2 + (uint64_t)td[off]) {
        GosymFunc *fn = NULL;
        Int line = 0;
        Str file = gosym_table_pc_to_line(tab, pc, &line, &fn);
        off = pc - addr;
        if (off >= (uint64_t)textdat.len || td[off] == 255)
            break;
        want_line += td[off];
        if (line != want_line) {
            testing_t_errorf_v(t, "expected line %d at PC %#x in pcfromline, got %d",
                               want_line, pc, line);
            off = pc + 1 - addr;
            continue;
        }
        if (lookupline == -1)
            lookupline = line;
        for (; lookupline <= line; lookupline++) {
            GosymFunc *fn2 = NULL;
            err = BURROW_NO_ERROR;
            uint64_t pc2 = gosym_table_line_to_pc(tab, file, lookupline, &fn2, &err);
            if (lookupline != line) {
                /* Should be nothing on this line. */
                if (BURROW_OK(err))
                    testing_t_errorf_v(t, "expected no PC at line %d, got %#x",
                                       lookupline, pc2);
            } else if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "failed to get PC of line %d: %s", lookupline,
                                   error_text(err));
            } else if (pc != pc2) {
                testing_t_errorf_v(t, "expected PC %#x at line %d, got PC %#x", pc,
                                   line, pc2);
            }
            checked++;
        }
        off = pc + 1 - addr;
    }
    CHECK(checked > 0);
    gosym_table_free(tab);
    gosym_line_table_free(pcln);
    arena_free(&ar);
}

typedef struct NameTest {
    const char *name;
    int go_version;
    const char *pkg;
    const char *recv;
    const char *base;
} NameTest;

/* From TestGenericMethods, TestStandardLibPackage, TestStandardLibPathPackage,
 * TestGenericNames and TestRemotePackage. A NULL is a part Go does not
 * check. */
static const NameTest name_tests[] = {
    {"main.T[go.shape.int].M[go.shape.bool]", 0, "main", "T[go.shape.int]",
     "M[go.shape.bool]"},
    {"main.(*T[go.shape.int]).M[go.shape.bool]", 0, "main", "(*T[go.shape.int])",
     "M[go.shape.bool]"},
    {"main.T.M[go.shape.bool]", 0, "main", "T", "M[go.shape.bool]"},
    {"main.(*T).M[go.shape.bool]", 0, "main", "(*T)", "M[go.shape.bool]"},
    {"io.(*LimitedReader).Read", 0, "io", "(*LimitedReader)", NULL},
    {"io.NewSectionReader", 0, "io", "", NULL},
    {"debug/gosym.(*LineTable).PCToLine", 0, "debug/gosym", "(*LineTable)", NULL},
    {"debug/gosym.NewTable", 0, "debug/gosym", "", NULL},
    {"main.set[int]", 0, "main", "", "set[int]"},
    {"main.(*value[int]).get", 0, "main", "(*value[int])", "get"},
    {"a/b.absDifference[c/d.orderedAbs[float64]]", 0, "a/b", "",
     "absDifference[c/d.orderedAbs[float64]]"},
    {"main.testfunction[.shape.int]", 0, "main", "", "testfunction[.shape.int]"},
    {"github.com/docker/doc.ker/pkg/mflag.(*FlagSet).PrintDefaults", 0,
     "github.com/docker/doc.ker/pkg/mflag", "(*FlagSet)", NULL},
    {"github.com/docker/doc.ker/pkg/mflag.PrintDefaults", 0,
     "github.com/docker/doc.ker/pkg/mflag", "", NULL},
    /* TestIssue29551, where 5 is Go's ver120 and 4 its ver118. */
    {"type:.eq.[9]debug/elf.intName", 5, "", NULL, NULL},
    {"type:.hash.debug/elf.ProgHeader", 5, "", NULL, NULL},
    {"type:.eq.runtime._panic", 5, "", NULL, NULL},
    {"type:.hash.struct { runtime.gList; runtime.n int32 }", 5, "", NULL, NULL},
    {"go:(*struct { sync.Mutex; math/big.table [64]math/big", 5, "", NULL, NULL},
    {"go.uber.org/zap/buffer.(*Buffer).AppendString", 5, "go.uber.org/zap/buffer", NULL,
     NULL},
    {"type..eq.[9]debug/elf.intName", 4, "", NULL, NULL},
    {"type..hash.debug/elf.ProgHeader", 4, "", NULL, NULL},
    {"type..eq.runtime._panic", 4, "", NULL, NULL},
    {"type..hash.struct { runtime.gList; runtime.n int32 }", 4, "", NULL, NULL},
    {"go.(*struct { sync.Mutex; math/big.table [64]math/big", 4, "", NULL, NULL},
    /* Unfortunate, Go says. */
    {"go.uber.org/zap/buffer.(*Buffer).AppendString", 4, "", NULL, NULL},
};

static void check_name(TestingT *t, const char *what, Str name, Str got,
                       const char *want) {
    if (want != NULL && !str_eq(got, str_from_cstr(want)))
        testing_t_errorf_v(t, "Expected: %q Actual: %q for %s of %q", want, got, what,
                           name);
}

static void TestNames(TestingT *t) {
    for (size_t i = 0; i < sizeof name_tests / sizeof name_tests[0]; i++) {
        const NameTest *tt = &name_tests[i];
        GosymSym s;
        memset(&s, 0, sizeof s);
        s.name = str_from_cstr(tt->name);
        s.go_version = tt->go_version;
        check_name(t, "package", s.name, gosym_sym_package_name(&s), tt->pkg);
        check_name(t, "receiver", s.name, gosym_sym_receiver_name(&s), tt->recv);
        check_name(t, "base", s.name, gosym_sym_base_name(&s), tt->base);
    }
}

/* The errors have Go's text. */
static void TestErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK(str_eq(gosym_unknown_file_error_error(BURROW_S("x.go"), a),
                 BURROW_S("unknown file: x.go")));
    GosymUnknownLineError le = {BURROW_S("x.go"), 7};
    CHECK(
        str_eq(gosym_unknown_line_error_error(&le, a), BURROW_S("no code at x.go:7")));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestDump)                                                                        \
    X(TestDamage)                                                                      \
    X(Test115PclnParsing)                                                              \
    X(TestPCLine)                                                                      \
    X(TestNames)                                                                       \
    X(TestErrors)

TESTING_MAIN(TESTS)
