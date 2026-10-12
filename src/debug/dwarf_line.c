/* debug/dwarf: the line tables.
 *
 * Derived from Go's src/debug/dwarf/line.go.
 * Go source: go1.27.1.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "dwarf_internal.h"

#include "burrow/core.h"
#include "burrow/debug/dwarf.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/path.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DWL_S(lit) BURROW_S(lit)

struct DwarfLineReader {
    DwBuf buf;
    Alloc *a;

    /* The whole line section, which Seek and Reset cut again. */
    Byte *section;
    Int section_len;

    Slice str;      /* .debug_str */
    Slice line_str; /* .debug_line_str */

    /* The header. */
    uint16_t version;
    Int addrsize;
    Int segment_selector_size;
    Int min_instruction_length;
    Int max_ops_per_instruction;
    bool default_is_stmt;
    Int line_base;
    Int line_range;
    Int opcode_base;
    Int *opcode_lengths;
    Str *directories;
    Int ndirectory;

    /* The file table. Past nfile up to capfile the slots are NULL until a
     * DW_LNE_define_file fills them, and then they keep what it put there, so
     * that reading the same one again after a Seek or a Reset gives the same
     * DwarfLineFile. */
    DwarfLineFile **files;
    Int nfile;
    Int capfile;

    DwarfOffset program_offset; /* where the program starts */
    DwarfOffset end_offset;     /* and the first byte after it */

    Int initial_file_entries;

    /* The state machine's registers. */
    DwarfLineEntry state;
    Int file_index;
};

/* Go's int arithmetic, which wraps. */
static Int dwl_add(Int x, Int y) {
    return (Int)((uint64_t)x + (uint64_t)y);
}

static bool dwl_same_error(Error x, Error y) {
    return x.vt == y.vt && x.data == y.data;
}

static Error dwl_decode_error(const DwarfLineReader *r, DwarfOffset off,
                              const DwMsg *m) {
    return burrow__dwarf_decode_error(r->buf.dwarf, DWL_S("line"), off, dw_msg_text(m));
}

static Error dwl_decode_error_s(const DwarfLineReader *r, DwarfOffset off, Str s) {
    return burrow__dwarf_decode_error(r->buf.dwarf, DWL_S("line"), off, s);
}

/* make([]T, n) with Go's limit, past which it panics rather than trying. */
static void *dwl_make(Alloc *a, uint64_t n, size_t size) {
    uint64_t max_alloc = sizeof(void *) == 8 ? (uint64_t)1 << 48 : (uint64_t)INTPTR_MAX;
    if (n > (uint64_t)INTPTR_MAX || n > max_alloc / size)
        runtime_panic(DWL_S("runtime error: makeslice: len out of range"));
    return burrow__dwarf_alloc(a, (size_t)n * size);
}

/* --------------------------------------------------------------- paths */

static Str dwl_sub(Str s, Int lo, Int hi) {
    Str r = {s.p + lo, hi - lo};
    return r;
}

/* splitDrive: the DOS drive letter or UNC share at the start of path, if any,
 * and the rest. */
static void dwl_split_drive(Str path, Str *drive, Str *rest) {
    const Byte *p = path.p;
    if (path.len >= 2 && p[1] == ':') {
        Byte c = p[0];
        if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z')) {
            *drive = dwl_sub(path, 0, 2);
            *rest = dwl_sub(path, 2, path.len);
            return;
        }
    }
    if (path.len > 3 && (p[0] == '\\' || p[0] == '/') &&
        (p[1] == '\\' || p[1] == '/')) {
        /* Go turns every / into \ first and then looks for \. */
        Int slash1 = 1;
        for (Int i = 2; i < path.len; i++) {
            if (p[i] == '\\' || p[i] == '/') {
                slash1 = i;
                break;
            }
        }
        if (slash1 > 2) {
            /* The mount point, which Go means to be non-empty. */
            Int slash2 = slash1;
            for (Int i = slash1 + 1; i < path.len; i++) {
                if (p[i] == '\\' || p[i] == '/') {
                    slash2 = i;
                    break;
                }
            }
            if (slash2 > slash1) {
                *drive = dwl_sub(path, 0, slash2);
                *rest = dwl_sub(path, slash2, path.len);
                return;
            }
        }
    }
    *drive = BURROW_STR_EMPTY;
    *rest = path;
}

/* pathIsAbs: whether path is absolute in Unix or DOS terms. DWARF says paths
 * are in whatever form the host uses, and which host that was is not known. */
static bool dwl_path_is_abs(Str path) {
    Str drive;
    Str rest;
    dwl_split_drive(path, &drive, &rest);
    return rest.len > 0 && (rest.p[0] == '/' || rest.p[0] == '\\');
}

static Str dwl_concat(Alloc *a, int n, const Str *parts) {
    Int len = 0;
    for (int i = 0; i < n; i++)
        len += parts[i].len;
    Byte *p = (Byte *)burrow__dwarf_alloc(a, (size_t)len);
    Int at = 0;
    for (int i = 0; i < n; i++) {
        if (parts[i].len > 0)
            memcpy(p + at, parts[i].p, (size_t)parts[i].len);
        at += parts[i].len;
    }
    Str s = {p, len};
    return s;
}

/* pathJoin: filename, which is relative, in the directory dirname, where
 * either can be a Unix or a DOS path. */
static Str dwl_path_join(Alloc *a, Str dirname, Str filename) {
    if (dirname.len == 0)
        return filename;
    /* dirname ought to be absolute, so a drive letter or a UNC share is a
     * fair sign of a DOS path. */
    Str drive;
    Str dir;
    dwl_split_drive(dirname, &drive, &dir);
    if (drive.len == 0) {
        Str joined = path_join_v(a, 2, dir, filename);
        if (joined.len == 0)
            burrow__dwarf_oom();
        return joined;
    }
    Str drive2;
    Str file;
    dwl_split_drive(filename, &drive2, &file);
    if (drive2.len > 0 && !strings_equal_fold(drive, drive2)) {
        /* Different drives, and nothing better to do than leave the
         * directory out. */
        Str parts[2] = {drive2, file};
        return dwl_concat(a, 2, parts);
    }
    Str sep = BURROW_STR_EMPTY;
    if (dir.len > 0 && dir.p[dir.len - 1] != '/' && dir.p[dir.len - 1] != '\\')
        sep = dir.p[0] == '/' ? DWL_S("/") : DWL_S("\\");
    Str parts[4] = {drive, dir, sep, file};
    return dwl_concat(a, 4, parts);
}

/* -------------------------------------------------------------- header */

/* The lengths the standard opcodes have to have, in operands, or -1 for the
 * ones that are not checked: DW_LNS_set_column, which Go leaves out, and
 * DW_LNS_fixed_advance_pc, whose one operand is a uint16 and no LEB128. */
static Int dwl_known_opcode_length(Int op) {
    switch (op) {
    case DW_LNS_COPY:
    case DW_LNS_NEGATE_STMT:
    case DW_LNS_SET_BASIC_BLOCK:
    case DW_LNS_CONST_ADD_PC:
    case DW_LNS_SET_PROLOGUE_END:
    case DW_LNS_SET_EPILOGUE_BEGIN:
        return 0;
    case DW_LNS_ADVANCE_PC:
    case DW_LNS_ADVANCE_LINE:
    case DW_LNS_SET_FILE:
    case DW_LNS_SET_ISA:
        return 1;
    default:
        return -1;
    }
}

static void dwl_files_reserve(DwarfLineReader *r, Int n) {
    if (n <= r->capfile)
        return;
    Int ncap = r->capfile < 4 ? 4 : r->capfile * 2;
    if (ncap < n)
        ncap = n;
    DwarfLineFile **f = (DwarfLineFile **)burrow__dwarf_alloc(
        r->a, (size_t)ncap * sizeof(DwarfLineFile *));
    if (r->nfile > 0)
        memcpy((void *)f, (const void *)r->files, (size_t)r->capfile * sizeof f[0]);
    if (r->capfile > 0)
        mem_free(r->a, (void *)r->files, (size_t)r->capfile * sizeof f[0],
                 _Alignof(DwarfLineFile *));
    r->files = f;
    r->capfile = ncap;
}

/* readFileEntry: a file from the header or from DW_LNE_define_file. *done is
 * set at the empty name that ends the list. */
static Error dwl_read_file_entry(DwarfLineReader *r, bool *done) {
    *done = false;
    Str name = dw_buf_string(&r->buf);
    if (BURROW_FAILED(r->buf.err))
        return r->buf.err;
    if (name.len == 0) {
        *done = true;
        return BURROW_NO_ERROR;
    }
    DwarfOffset off = r->buf.off;
    Int dir_index = (Int)dw_buf_uint(&r->buf);
    if (!dwl_path_is_abs(name)) {
        if (dir_index >= r->ndirectory)
            return dwl_decode_error_s(r, off, DWL_S("directory index too large"));
        if (dir_index < 0)
            runtime_index_out_of_range(dir_index, r->ndirectory);
        name = dwl_path_join(r->a, r->directories[dir_index], name);
    }
    uint64_t mtime = dw_buf_uint(&r->buf);
    Int length = (Int)dw_buf_uint(&r->buf);

    /* A file added by the program that the reader has been back over
     * already keeps the entry it had. */
    if (r->nfile < r->capfile && r->files[r->nfile] != NULL) {
        r->nfile++;
        return BURROW_NO_ERROR;
    }
    DwarfLineFile *f = DW_NEW(r->a, DwarfLineFile);
    f->name = name;
    f->mtime = mtime;
    f->length = length;
    dwl_files_reserve(r, r->nfile + 1);
    r->files[r->nfile++] = f;
    return BURROW_NO_ERROR;
}

/* lnctForm, one entry of a DWARF 5 directory or file name format. */
typedef struct DwlForm {
    Int lnct;
    uint32_t form;
} DwlForm;

/* readLNCTFormat. */
static DwlForm *dwl_read_lnct_format(DwarfLineReader *r, Int *n) {
    uint8_t c = dw_buf_uint8(&r->buf);
    DwlForm *ret = (DwlForm *)burrow__dwarf_alloc(r->a, (size_t)c * sizeof(DwlForm));
    for (Int i = 0; i < c; i++) {
        ret[i].lnct = (Int)dw_buf_uint(&r->buf);
        ret[i].form = (uint32_t)dw_buf_uint(&r->buf);
    }
    *n = c;
    return ret;
}

/* readLNCT: the path, time and size of one directory or file. */
static Error dwl_read_lnct(DwarfLineReader *r, const DwlForm *s, Int n, bool dwarf64,
                           Str *path, uint64_t *mtime, uint64_t *size) {
    DwBuf *b = &r->buf;
    Str dir = BURROW_STR_EMPTY;
    *path = BURROW_STR_EMPTY;
    *mtime = 0;
    *size = 0;
    for (Int i = 0; i < n; i++) {
        Str str = BURROW_STR_EMPTY;
        uint64_t val = 0;
        switch (s[i].form) {
        case DW_FORM_STRING:
            str = dw_buf_string(b);
            break;
        case DW_FORM_STRP:
        case DW_FORM_LINE_STRP: {
            uint64_t off = dwarf64 ? dw_buf_uint64(b) : dw_buf_uint32(b);
            if (!dw_fits_int(off))
                return dwl_decode_error_s(r, b->off,
                                          DWL_S("strp/line_strp offset out of range"));
            Slice sec = s[i].form == DW_FORM_STRP ? r->str : r->line_str;
            DwBuf b1 = dw_make_buf(b->dwarf, b->format,
                                   s[i].form == DW_FORM_STRP ? DWL_S("str")
                                                             : DWL_S("line_str"),
                                   0, (Byte *)sec.p, sec.len);
            dw_buf_skip(&b1, (Int)off);
            str = dw_buf_string(&b1);
            if (BURROW_FAILED(b1.err))
                return dwl_decode_error_s(r, b->off, error_text(b1.err));
            break;
        }
        case DW_FORM_STRP_SUP:
            /* Supplementary sections are not read yet. */
            if (dwarf64)
                (void)dw_buf_uint64(b);
            else
                (void)dw_buf_uint32(b);
            break;
        case DW_FORM_STRX:
            /* Nor are .debug_line.dwo sections. */
            (void)dw_buf_uint(b);
            break;
        case DW_FORM_STRX1:
            (void)dw_buf_uint8(b);
            break;
        case DW_FORM_STRX2:
            (void)dw_buf_uint16(b);
            break;
        case DW_FORM_STRX3:
            (void)dw_buf_uint24(b);
            break;
        case DW_FORM_STRX4:
            (void)dw_buf_uint32(b);
            break;
        case DW_FORM_DATA1:
            val = dw_buf_uint8(b);
            break;
        case DW_FORM_DATA2:
            val = dw_buf_uint16(b);
            break;
        case DW_FORM_DATA4:
            val = dw_buf_uint32(b);
            break;
        case DW_FORM_DATA8:
            val = dw_buf_uint64(b);
            break;
        case DW_FORM_DATA16:
            (void)dw_buf_bytes(b, 16);
            break;
        case DW_FORM_DWARF_BLOCK:
            (void)dw_buf_bytes(b, (Int)dw_buf_uint(b));
            break;
        case DW_FORM_UDATA:
            val = dw_buf_uint(b);
            break;
        default:
            break;
        }

        switch (s[i].lnct) {
        case DW_LNCT_PATH:
            *path = str;
            break;
        case DW_LNCT_DIRECTORY_INDEX:
            if (val >= (uint64_t)r->ndirectory)
                return dwl_decode_error_s(r, b->off,
                                          DWL_S("directory index out of range"));
            dir = r->directories[val];
            break;
        case DW_LNCT_TIMESTAMP:
            *mtime = val;
            break;
        case DW_LNCT_SIZE:
            *size = val;
            break;
        default:
            /* DW_LNCT_MD5 and anything newer is skipped. */
            break;
        }
    }
    if (dir.len > 0 && path->len > 0)
        *path = dwl_path_join(r->a, dir, *path);
    return BURROW_NO_ERROR;
}

/* readHeader: the line number program's header, from r->buf. */
static Error dwl_read_header(DwarfLineReader *r, Str comp_dir) {
    DwBuf *buf = &r->buf;

    /* DWARF 2 section 6.2.4. */
    DwarfOffset hdr_offset = buf->off;
    bool dwarf64 = false;
    DwarfOffset unit_length = dw_buf_unit_length(buf, &dwarf64);
    r->end_offset = buf->off + unit_length;
    DwarfOffset section_end = buf->off + (DwarfOffset)buf->len;
    if (r->end_offset > section_end) {
        DwMsg m = {.len = 0};
        dw_msg_str(&m, DWL_S("line table end "));
        dw_msg_uint(&m, r->end_offset);
        dw_msg_str(&m, DWL_S(" exceeds section size "));
        dw_msg_uint(&m, section_end);
        return dwl_decode_error(r, hdr_offset, &m);
    }
    r->version = dw_buf_uint16(buf);
    if (BURROW_OK(buf->err) && (r->version < 2 || r->version > 5)) {
        /* DWARF works hard to keep new opcodes backward compatible and then
         * puts new fields in the middle of the header, so only the versions
         * known here are read. */
        DwMsg m = {.len = 0};
        dw_msg_str(&m, DWL_S("unknown line table version "));
        dw_msg_uint(&m, r->version);
        return dwl_decode_error(r, hdr_offset, &m);
    }
    if (r->version >= 5) {
        r->addrsize = dw_buf_uint8(buf);
        r->segment_selector_size = dw_buf_uint8(buf);
    } else {
        r->addrsize = dw_format_addrsize(buf->format);
        r->segment_selector_size = 0;
    }
    DwarfOffset header_length =
        dwarf64 ? (DwarfOffset)dw_buf_uint64(buf) : (DwarfOffset)dw_buf_uint32(buf);
    DwarfOffset program_offset = buf->off + header_length;
    if (program_offset > r->end_offset) {
        DwMsg m = {.len = 0};
        dw_msg_str(&m, DWL_S("malformed line table: program offset "));
        dw_msg_uint(&m, program_offset);
        dw_msg_str(&m, DWL_S(" exceeds end offset "));
        dw_msg_uint(&m, r->end_offset);
        return dwl_decode_error(r, hdr_offset, &m);
    }
    r->program_offset = program_offset;
    r->min_instruction_length = dw_buf_uint8(buf);
    /* DWARF 4 section 6.2.4. */
    r->max_ops_per_instruction = r->version >= 4 ? dw_buf_uint8(buf) : 1;
    r->default_is_stmt = dw_buf_uint8(buf) != 0;
    r->line_base = (int8_t)dw_buf_uint8(buf);
    r->line_range = dw_buf_uint8(buf);

    if (BURROW_FAILED(buf->err))
        return buf->err;
    if (r->max_ops_per_instruction == 0)
        return dwl_decode_error_s(
            r, hdr_offset, DWL_S("invalid maximum operations per instruction: 0"));
    if (r->line_range == 0)
        return dwl_decode_error_s(r, hdr_offset, DWL_S("invalid line range: 0"));

    /* The lengths of the standard opcodes, from opcode 1. */
    r->opcode_base = dw_buf_uint8(buf);
    r->opcode_lengths =
        (Int *)burrow__dwarf_alloc(r->a, (size_t)r->opcode_base * sizeof(Int));
    for (Int i = 1; i < r->opcode_base; i++)
        r->opcode_lengths[i] = dw_buf_uint8(buf);
    if (BURROW_FAILED(buf->err))
        return buf->err;
    for (Int i = 0; i < r->opcode_base; i++) {
        Int known = dwl_known_opcode_length(i);
        if (known >= 0 && known != r->opcode_lengths[i]) {
            DwMsg m = {.len = 0};
            dw_msg_str(&m, DWL_S("opcode "));
            dw_msg_int(&m, i);
            dw_msg_str(&m, DWL_S(" expected to have length "));
            dw_msg_int(&m, known);
            dw_msg_str(&m, DWL_S(", but has length "));
            dw_msg_int(&m, r->opcode_lengths[i]);
            return dwl_decode_error(r, hdr_offset, &m);
        }
    }

    if (r->version < 5) {
        /* The include directories, after the compilation directory, which is
         * directory 0 without being written down. */
        Int cap = 4;
        r->directories = (Str *)burrow__dwarf_alloc(r->a, (size_t)cap * sizeof(Str));
        r->directories[0] = comp_dir;
        r->ndirectory = 1;
        for (;;) {
            Str directory = dw_buf_string(buf);
            if (BURROW_FAILED(buf->err))
                return buf->err;
            if (directory.len == 0)
                break;
            if (!dwl_path_is_abs(directory)) {
                /* A relative one is in the compilation directory. */
                directory = dwl_path_join(r->a, comp_dir, directory);
            }
            if (r->ndirectory == cap) {
                Str *d =
                    (Str *)burrow__dwarf_alloc(r->a, (size_t)cap * 2 * sizeof(Str));
                memcpy(d, r->directories, (size_t)cap * sizeof(Str));
                mem_free(r->a, r->directories, (size_t)cap * sizeof(Str),
                         _Alignof(Str));
                r->directories = d;
                cap *= 2;
            }
            r->directories[r->ndirectory++] = directory;
        }

        /* The files, numbered from 1, so entry 0 is NULL. */
        dwl_files_reserve(r, 1);
        r->nfile = 1;
        for (;;) {
            bool done = false;
            Error err = dwl_read_file_entry(r, &done);
            if (BURROW_FAILED(err))
                return err;
            if (done)
                break;
        }
    } else {
        Int ndir_format = 0;
        DwlForm *dir_format = dwl_read_lnct_format(r, &ndir_format);
        uint64_t c = dw_buf_uint(buf);
        r->directories = (Str *)dwl_make(r->a, c, sizeof(Str));
        r->ndirectory = (Int)c;
        for (Int i = 0; i < r->ndirectory; i++) {
            Str dir;
            uint64_t mtime;
            uint64_t size;
            Error err =
                dwl_read_lnct(r, dir_format, ndir_format, dwarf64, &dir, &mtime, &size);
            if (BURROW_FAILED(err))
                return err;
            r->directories[i] = dir;
        }
        Int nfile_format = 0;
        DwlForm *file_format = dwl_read_lnct_format(r, &nfile_format);
        c = dw_buf_uint(buf);
        r->files = (DwarfLineFile **)dwl_make(r->a, c, sizeof(DwarfLineFile *));
        r->nfile = (Int)c;
        r->capfile = (Int)c;
        for (Int i = 0; i < r->nfile; i++) {
            Str name;
            uint64_t mtime;
            uint64_t size;
            Error err = dwl_read_lnct(r, file_format, nfile_format, dwarf64, &name,
                                      &mtime, &size);
            if (BURROW_FAILED(err))
                return err;
            DwarfLineFile *f = DW_NEW(r->a, DwarfLineFile);
            f->name = name;
            f->mtime = mtime;
            f->length = (Int)size;
            r->files[i] = f;
        }
    }

    r->initial_file_entries = r->nfile;
    return buf->err;
}

/* -------------------------------------------------------------- reading */

/* updateFile: the file register after the index or the table changed. */
static void dwl_update_file(DwarfLineReader *r) {
    if (r->file_index < r->nfile) {
        if (r->file_index < 0)
            runtime_index_out_of_range(r->file_index, r->nfile);
        r->state.file = r->files[r->file_index];
    } else {
        r->state.file = NULL;
    }
}

/* resetState: the registers as DWARF 4 section 6.2.2 starts them. */
static void dwl_reset_state(DwarfLineReader *r) {
    memset(&r->state, 0, sizeof r->state);
    r->state.line = 1;
    r->state.is_stmt = r->default_is_stmt;
    r->file_index = 1;
    dwl_update_file(r);
}

/* section[lo:hi], as the buffer to read. */
static void dwl_set_data(DwarfLineReader *r, DwarfOffset lo, DwarfOffset hi) {
    if ((uint64_t)hi > (uint64_t)r->section_len || lo > hi)
        runtime_slice_bounds_out_of_range((Int)lo, (Int)hi, r->section_len);
    r->buf.off = lo;
    r->buf.data = r->section + lo;
    r->buf.len = (Int)(hi - lo);
}

void dwarf_line_reader_reset(DwarfLineReader *r) {
    if (r == NULL)
        burrow__dwarf_nil_panic();
    dwl_set_data(r, r->program_offset, r->end_offset);
    r->nfile = r->initial_file_entries;
    dwl_reset_state(r);
}

DwarfLineReader *dwarf_data_line_reader(DwarfData *d, Alloc *a, const DwarfEntry *cu,
                                        Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (d == NULL)
        burrow__dwarf_nil_panic();
    if (d->line.p == NULL)
        return NULL; /* no line tables at all */
    if (cu == NULL)
        burrow__dwarf_nil_panic();
    int64_t *stmt =
        (int64_t *)burrow__dwarf_entry_val_as(cu, DWARF_ATTR_STMT_LIST, TYPE_INT64);
    if (stmt == NULL)
        return NULL; /* none for cu */
    int64_t off = *stmt;
    if (off < 0 || off > (int64_t)d->line.len) {
        BURROW_OUT(err,
                   burrow__dwarf_error(d, DWL_S("AttrStmtList value out of range")));
        return NULL;
    }
    /* Without AttrCompDir every file name ought to be absolute. */
    Str *comp_dir =
        (Str *)burrow__dwarf_entry_val_as(cu, DWARF_ATTR_COMP_DIR, TYPE_STRING);

    Int ui = burrow__dwarf_offset_to_unit(d, cu->offset);
    if (ui < 0 || ui >= d->nunit)
        runtime_index_out_of_range(ui, d->nunit);
    DwUnit *u = &d->unit[ui];
    DwarfLineReader *r = DW_NEW(a, DwarfLineReader);
    r->a = a;
    r->buf = dw_make_buf(d, u, DWL_S("line"), (DwarfOffset)off, (Byte *)d->line.p + off,
                         d->line.len - (Int)off);
    r->section = (Byte *)d->line.p;
    r->section_len = d->line.len;
    r->str = d->str;
    r->line_str = d->line_str;

    Error herr = dwl_read_header(r, comp_dir != NULL ? *comp_dir : BURROW_STR_EMPTY);
    if (BURROW_FAILED(herr)) {
        BURROW_OUT(err, herr);
        return NULL;
    }
    dwarf_line_reader_reset(r);
    return r;
}

/* advancePC: the operation pointer, address and op_index together, moved on
 * by op_advance operations. */
static void dwl_advance_pc(DwarfLineReader *r, Int op_advance) {
    Int op_index = dwl_add(r->state.op_index, op_advance);
    Int q = op_index / r->max_ops_per_instruction;
    r->state.address += (uint64_t)r->min_instruction_length * (uint64_t)q;
    r->state.op_index = op_index % r->max_ops_per_instruction;
}

static void dwl_emit(DwarfLineReader *r, DwarfLineEntry *entry) {
    *entry = r->state;
    r->state.basic_block = false;
    r->state.prologue_end = false;
    r->state.epilogue_begin = false;
    r->state.discriminator = 0;
}

/* step: one opcode. When it makes a row, the row goes in *entry and the
 * result is true. */
static bool dwl_step(DwarfLineReader *r, DwarfLineEntry *entry) {
    DwBuf *buf = &r->buf;
    Int opcode = dw_buf_uint8(buf);

    if (opcode >= r->opcode_base) {
        /* A special opcode, DWARF 2 and 4 section 6.2.5.1. */
        Int adjusted = opcode - r->opcode_base;
        dwl_advance_pc(r, adjusted / r->line_range);
        Int line_delta = r->line_base + adjusted % r->line_range;
        r->state.line = dwl_add(r->state.line, line_delta);
        dwl_emit(r, entry);
        return true;
    }

    switch (opcode) {
    case 0: {
        /* An extended opcode, DWARF 2 section 6.2.5.3. */
        DwarfOffset length = (DwarfOffset)dw_buf_uint(buf);
        DwarfOffset start_off = buf->off;
        uint8_t ext = dw_buf_uint8(buf);
        switch (ext) {
        case DW_LNE_END_SEQUENCE:
            r->state.end_sequence = true;
            *entry = r->state;
            dwl_reset_state(r);
            break;
        case DW_LNE_SET_ADDRESS:
            switch (r->addrsize) {
            case 1:
                r->state.address = dw_buf_uint8(buf);
                break;
            case 2:
                r->state.address = dw_buf_uint16(buf);
                break;
            case 4:
                r->state.address = dw_buf_uint32(buf);
                break;
            case 8:
                r->state.address = dw_buf_uint64(buf);
                break;
            default:
                burrow__dwarf_buf_error(buf, DWL_S("unknown address size"));
                break;
            }
            break;
        case DW_LNE_DEFINE_FILE: {
            bool done = false;
            Error err = dwl_read_file_entry(r, &done);
            if (BURROW_FAILED(err)) {
                /* Go sets the error and leaves the data where it is. */
                buf->err = err;
                return false;
            }
            if (done) {
                buf->err = dwl_decode_error_s(
                    r, start_off, DWL_S("malformed DW_LNE_define_file operation"));
                return false;
            }
            dwl_update_file(r);
            break;
        }
        case DW_LNE_SET_DISCRIMINATOR:
            /* DWARF 4 section 6.2.5.3. */
            r->state.discriminator = (Int)dw_buf_uint(buf);
            break;
        default:
            break;
        }
        dw_buf_skip(buf, (Int)(DwarfOffset)(start_off + length - buf->off));
        return ext == DW_LNE_END_SEQUENCE;
    }

    /* The standard opcodes, DWARF 2 section 6.2.5.2. */
    case DW_LNS_COPY:
        dwl_emit(r, entry);
        return true;
    case DW_LNS_ADVANCE_PC:
        dwl_advance_pc(r, (Int)dw_buf_uint(buf));
        break;
    case DW_LNS_ADVANCE_LINE:
        r->state.line = dwl_add(r->state.line, (Int)dw_buf_int(buf));
        break;
    case DW_LNS_SET_FILE:
        r->file_index = (Int)dw_buf_uint(buf);
        dwl_update_file(r);
        break;
    case DW_LNS_SET_COLUMN:
        r->state.column = (Int)dw_buf_uint(buf);
        break;
    case DW_LNS_NEGATE_STMT:
        r->state.is_stmt = !r->state.is_stmt;
        break;
    case DW_LNS_SET_BASIC_BLOCK:
        r->state.basic_block = true;
        break;
    case DW_LNS_CONST_ADD_PC:
        dwl_advance_pc(r, (255 - r->opcode_base) / r->line_range);
        break;
    case DW_LNS_FIXED_ADVANCE_PC:
        r->state.address += dw_buf_uint16(buf);
        break;

    /* DWARF 3 section 6.2.5.2. */
    case DW_LNS_SET_PROLOGUE_END:
        r->state.prologue_end = true;
        break;
    case DW_LNS_SET_EPILOGUE_BEGIN:
        r->state.epilogue_begin = true;
        break;
    case DW_LNS_SET_ISA:
        r->state.isa = (Int)dw_buf_uint(buf);
        break;

    default:
        /* A standard opcode this does not know. The header says how many
         * operands to skip. */
        for (Int i = 0; i < r->opcode_lengths[opcode]; i++)
            (void)dw_buf_uint(buf);
        break;
    }
    return false;
}

Error dwarf_line_reader_next(DwarfLineReader *r, DwarfLineEntry *entry) {
    if (r == NULL)
        burrow__dwarf_nil_panic();
    if (BURROW_FAILED(r->buf.err))
        return r->buf.err;
    /* Opcodes until one makes a row. */
    for (;;) {
        if (r->buf.len == 0)
            return io_eof;
        bool emit = dwl_step(r, entry);
        if (BURROW_FAILED(r->buf.err))
            return r->buf.err;
        if (emit)
            return BURROW_NO_ERROR;
    }
}

DwarfLineReaderPos dwarf_line_reader_tell(DwarfLineReader *r) {
    if (r == NULL)
        burrow__dwarf_nil_panic();
    DwarfLineReaderPos pos = {r->buf.off, r->nfile, r->state, r->file_index};
    return pos;
}

void dwarf_line_reader_seek(DwarfLineReader *r, DwarfLineReaderPos pos) {
    if (r == NULL)
        burrow__dwarf_nil_panic();
    dwl_set_data(r, pos.off, r->end_offset);
    if (pos.num_file_entries < 0 || pos.num_file_entries > r->capfile)
        runtime_slice_bounds_out_of_range(0, pos.num_file_entries, r->capfile);
    r->nfile = pos.num_file_entries;
    r->state = pos.state;
    r->file_index = pos.file_index;
}

Slice dwarf_line_reader_files(DwarfLineReader *r) {
    if (r == NULL)
        burrow__dwarf_nil_panic();
    Slice s = {(void *)r->files, r->nfile, r->capfile, TYPE_UNSAFE_POINTER};
    return s;
}

Error dwarf_line_reader_seek_pc(DwarfLineReader *r, uint64_t pc,
                                DwarfLineEntry *entry) {
    Error err = dwarf_line_reader_next(r, entry);
    if (BURROW_FAILED(err))
        return err;
    if (entry->address > pc) {
        /* Past it, so from the top. */
        dwarf_line_reader_reset(r);
        err = dwarf_line_reader_next(r, entry);
        if (BURROW_FAILED(err))
            return err;
        if (entry->address > pc) {
            /* The whole table comes after pc. */
            dwarf_line_reader_reset(r);
            return dwarf_err_unknown_pc;
        }
    }

    /* On until a row is past pc, and then back one. */
    for (;;) {
        DwarfLineEntry next;
        DwarfLineReaderPos pos = dwarf_line_reader_tell(r);
        err = dwarf_line_reader_next(r, &next);
        if (BURROW_FAILED(err)) {
            if (dwl_same_error(err, io_eof))
                return dwarf_err_unknown_pc;
            return err;
        }
        if (next.address > pc) {
            if (entry->end_sequence) {
                /* pc is in a hole in the table. */
                return dwarf_err_unknown_pc;
            }
            /* entry is the row, and the reader goes back to next. */
            dwarf_line_reader_seek(r, pos);
            return BURROW_NO_ERROR;
        }
        *entry = next;
    }
}
