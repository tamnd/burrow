/* internal/pkgbits: the bitstream the gc compiler writes a package's export
 * data in, the Unified IR format, and reads it back with.
 *
 * Derived from Go's src/internal/pkgbits. Go source: go1.27.1.
 *
 * The data is a header, a table of where each element ends, and the elements
 * themselves, grouped by section. Every element starts with a table of the
 * other elements it refers to, and the rest of it is varints, bools and
 * references into that table. go/internal/gcimporter reads one with a
 * PbPkgDecoder, and the compiler writes one with a PbPkgEncoder.
 *
 * Not in Go: Go panics with an error on data it cannot make sense of, and so
 * does this, with the same text. A desync with sync markers on prints what Go
 * prints and exits, as Go does, without the reader's stack, which C cannot
 * walk. An encoder with sync frames turned on writes no frames for the same
 * reason.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_GO_PKGBITS_H
#define BURROW_GO_PKGBITS_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/go/constant.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"

#include <stdbool.h>
#include <stdint.h>

/* --------------------------------------------------------------- versions */

/* Version: the version of the file format. */
typedef uint32_t PbVersion;

enum {
    PB_V0 = 0, /* the initial prototype */
    PB_V1,     /* adds the Flags uint32 word */
    PB_V2,     /* removes unused legacy fields, type parameters on aliases */
    PB_V3,     /* a more compact format for composite literal element lists */
    PB_V4,     /* generic methods as standalone function objects */
    PB_NUM_VERSIONS
};

/* Field: something a version of the format has or does not have. */
typedef int PbField;

enum {
    PB_FIELD_FLAGS = 0,             /* the Flags uint32 in the header */
    PB_FIELD_HAS_INIT,              /* deprecated: whether there are inits */
    PB_FIELD_DERIVED_FUNC_INSTANCE, /* deprecated: an object is an instance */
    PB_FIELD_ALIAS_TYPE_PARAM_NAMES,
    PB_FIELD_DERIVED_INFO_NEEDED, /* deprecated: a type is derived */
    PB_FIELD_COMPACT_COMP_LITERALS,
    PB_FIELD_GENERIC_METHODS,
    PB_NUM_FIELDS
};

/* Version.Has: whether v has the field f. */
bool burrow__pb_version_has(PbVersion v, PbField f);

/* --------------------------------------------------------------- sections */

/* SectionKind: which section an element is in. */
typedef int32_t PbSectionKind;

enum {
    PB_SECTION_STRING = 0,
    PB_SECTION_META,
    PB_SECTION_POS_BASE,
    PB_SECTION_PKG,
    PB_SECTION_NAME,
    PB_SECTION_TYPE,
    PB_SECTION_OBJ,
    PB_SECTION_OBJ_EXT,
    PB_SECTION_OBJ_DICT,
    PB_SECTION_BODY,
    PB_NUM_RELOCS
};

/* Index: an element's index within its section, Go's RelElemIdx. */
typedef int32_t PbIndex;

/* RefTableEntry: an entry in an element's table of references. */
typedef struct PbRefTableEntry {
    PbSectionKind kind;
    PbIndex idx;
} PbRefTableEntry;

#define PB_PUBLIC_ROOT_IDX ((PbIndex)0)
#define PB_PRIVATE_ROOT_IDX ((PbIndex)1)

/* ----------------------------------------------------------- sync markers */

/* SyncMarker: what the next thing in a bitstream is, for checking that the
 * reader and the writer agree. */
typedef int PbSyncMarker;

enum {
    /* Public markers (known to go/types importers). */

    /* Low-level coding markers. */
    PB_SYNC_EOF = 1,
    PB_SYNC_BOOL,
    PB_SYNC_INT64,
    PB_SYNC_UINT64,
    PB_SYNC_STRING,
    PB_SYNC_VALUE,
    PB_SYNC_VAL,
    PB_SYNC_RELOCS,
    PB_SYNC_RELOC,
    PB_SYNC_USE_RELOC,

    /* Higher-level object and type markers. */
    PB_SYNC_PUBLIC,
    PB_SYNC_POS,
    PB_SYNC_POS_BASE,
    PB_SYNC_OBJECT,
    PB_SYNC_OBJECT1,
    PB_SYNC_PKG,
    PB_SYNC_PKG_DEF,
    PB_SYNC_METHOD,
    PB_SYNC_TYPE,
    PB_SYNC_TYPE_IDX,
    PB_SYNC_TYPE_PARAM_NAMES,
    PB_SYNC_SIGNATURE,
    PB_SYNC_PARAMS,
    PB_SYNC_PARAM,
    PB_SYNC_CODE_OBJ,
    PB_SYNC_SYM,
    PB_SYNC_LOCAL_IDENT,
    PB_SYNC_SELECTOR,

    /* Private markers (only known to cmd/compile). */
    PB_SYNC_PRIVATE,

    PB_SYNC_FUNC_EXT,
    PB_SYNC_VAR_EXT,
    PB_SYNC_TYPE_EXT,
    PB_SYNC_PRAGMA,

    PB_SYNC_EXPR_LIST,
    PB_SYNC_EXPRS,
    PB_SYNC_EXPR,
    PB_SYNC_EXPR_TYPE,
    PB_SYNC_ASSIGN,
    PB_SYNC_OP,
    PB_SYNC_FUNC_LIT,
    PB_SYNC_COMP_LIT,

    PB_SYNC_DECL,
    PB_SYNC_FUNC_BODY,
    PB_SYNC_OPEN_SCOPE,
    PB_SYNC_CLOSE_SCOPE,
    PB_SYNC_CLOSE_ANOTHER_SCOPE,
    PB_SYNC_DECL_NAMES,
    PB_SYNC_DECL_NAME,

    PB_SYNC_STMTS,
    PB_SYNC_BLOCK_STMT,
    PB_SYNC_IF_STMT,
    PB_SYNC_FOR_STMT,
    PB_SYNC_SWITCH_STMT,
    PB_SYNC_RANGE_STMT,
    PB_SYNC_CASE_CLAUSE,
    PB_SYNC_COMM_CLAUSE,
    PB_SYNC_SELECT_STMT,
    PB_SYNC_DECLS,
    PB_SYNC_LABELED_STMT,
    PB_SYNC_USE_OBJ_LOCAL,
    PB_SYNC_ADD_LOCAL,
    PB_SYNC_LINKNAME,
    PB_SYNC_STMT1,
    PB_SYNC_STMTS_END,
    PB_SYNC_LABEL,
    PB_SYNC_OPT_LABEL,

    PB_SYNC_MULTI_EXPR,
    PB_SYNC_RTYPE,
    PB_SYNC_CONV_RTTI
};

/* SyncMarker.String: "EOF" and so on, and "SyncMarker(n)" for anything
 * else. */
BURROW_OWNS(ret) Str burrow__pb_sync_marker_string(Alloc *a, PbSyncMarker i);

/* ------------------------------------------------------------------ codes */

/* CodeVal: the kind of a scalar constant. Its marker is PB_SYNC_VAL. */
enum {
    PB_VAL_BOOL = 0,
    PB_VAL_STRING,
    PB_VAL_INT64,
    PB_VAL_BIG_INT,
    PB_VAL_BIG_RAT,
    PB_VAL_BIG_FLOAT
};

/* CodeType: the kind of a type. Its marker is PB_SYNC_TYPE. */
enum {
    PB_TYPE_BASIC = 0,
    PB_TYPE_NAMED,
    PB_TYPE_POINTER,
    PB_TYPE_SLICE,
    PB_TYPE_ARRAY,
    PB_TYPE_CHAN,
    PB_TYPE_MAP,
    PB_TYPE_SIGNATURE,
    PB_TYPE_STRUCT,
    PB_TYPE_INTERFACE,
    PB_TYPE_UNION,
    PB_TYPE_TYPE_PARAM
};

/* CodeObj: the kind of an object. Its marker is PB_SYNC_CODE_OBJ. */
enum {
    PB_OBJ_ALIAS = 0,
    PB_OBJ_CONST,
    PB_OBJ_TYPE,
    PB_OBJ_FUNC,
    PB_OBJ_VAR,
    PB_OBJ_STUB
};

/* ---------------------------------------------------------------- decoding */

/* PkgDecoder: the export data of a package, ready to read elements from.
 *
 * Not in Go: it borrows the input, which has to outlive it, and the decoders
 * it makes take their reference tables from a. */
typedef struct PbPkgDecoder {
    PbVersion version;
    bool sync;    /* the file has sync markers */
    Str pkg_path; /* the path of the package the data is for */
    Str elem_data;
    /* Where each element ends in elem_data, by absolute index. The last 8
     * bytes of elem_data are the fingerprint. */
    const uint32_t *elem_ends;
    Int nelem_ends;
    /* Where each section ends in elem_ends. */
    uint32_t elem_ends_ends[PB_NUM_RELOCS];
    PbRefTableEntry *scratch;
    Int scratch_cap;
    Alloc *a;
} PbPkgDecoder;

/* NewPkgDecoder: a decoder for input, the Unified IR data written for the
 * package pkg_path. It panics on input that is not that. */
PbPkgDecoder burrow__pb_new_pkg_decoder(Alloc *a, Str pkg_path, Str input);

/* PkgPath, SyncMarkers, NumElems and TotalElems. */
Str burrow__pb_pkg_decoder_pkg_path(const PbPkgDecoder *pr);
bool burrow__pb_pkg_decoder_sync_markers(const PbPkgDecoder *pr);
Int burrow__pb_num_elems(const PbPkgDecoder *pr, PbSectionKind k);
Int burrow__pb_total_elems(const PbPkgDecoder *pr);

/* Fingerprint: the package's fingerprint, into fp. */
void burrow__pb_fingerprint(const PbPkgDecoder *pr, Byte fp[8]);

/* AbsIdx: the absolute index of idx in section k. Out of bounds panics. */
Int burrow__pb_abs_idx(const PbPkgDecoder *pr, PbSectionKind k, PbIndex idx);

/* DataIdx: the bytes of element idx of section k, borrowed from the input. */
Str burrow__pb_data_idx(const PbPkgDecoder *pr, PbSectionKind k, PbIndex idx);

/* StringIdx: the string with index idx. */
Str burrow__pb_string_idx(const PbPkgDecoder *pr, PbIndex idx);

/* Decoder: the reader of one element. */
typedef struct PbDecoder {
    PbPkgDecoder *common;
    PbRefTableEntry *relocs;
    Int nrelocs;
    Int relocs_cap;
    Str data;
    Int off; /* how much of data has been read */
    PbSectionKind k;
    PbIndex idx;
} PbDecoder;

/* NewDecoder: a decoder for element idx of section k, which reads marker
 * first. NewDecoderRaw does not read a marker. */
PbDecoder burrow__pb_new_decoder(PbPkgDecoder *pr, PbSectionKind k, PbIndex idx,
                                 PbSyncMarker marker);
PbDecoder burrow__pb_new_decoder_raw(PbPkgDecoder *pr, PbSectionKind k, PbIndex idx);

/* TempDecoder and TempDecoderRaw: the same, reusing the reference table of
 * the last decoder given back with RetireDecoder. */
PbDecoder burrow__pb_temp_decoder(PbPkgDecoder *pr, PbSectionKind k, PbIndex idx,
                                  PbSyncMarker marker);
PbDecoder burrow__pb_temp_decoder_raw(PbPkgDecoder *pr, PbSectionKind k, PbIndex idx);
void burrow__pb_retire_decoder(PbPkgDecoder *pr, PbDecoder *d);

/* Sync: reads a marker and checks that it is want. It does nothing when the
 * file has no markers. */
void burrow__pb_sync(PbDecoder *r, PbSyncMarker want);

/* Bool, Int64, Uint64, Len, Int and Uint: a value of that type. Len is one
 * that cannot be negative. */
bool burrow__pb_bool(PbDecoder *r);
int64_t burrow__pb_int64(PbDecoder *r);
uint64_t burrow__pb_uint64(PbDecoder *r);
Int burrow__pb_len(PbDecoder *r);
Int burrow__pb_int(PbDecoder *r);
Uint burrow__pb_uint(PbDecoder *r);

/* Code: a code, after the marker that goes with its type. */
Int burrow__pb_code(PbDecoder *r, PbSyncMarker mark);

/* Reloc: a reference to an element of section k. */
PbIndex burrow__pb_reloc(PbDecoder *r, PbSectionKind k);

/* String: a string, borrowed from the input. Strings gives a Slice of Str
 * from the package decoder's allocator. */
Str burrow__pb_string(PbDecoder *r);
BURROW_OWNS(ret) Slice burrow__pb_strings(PbDecoder *r);

/* Value: a constant, made in the package decoder's allocator. */
ConstantValue burrow__pb_value(PbDecoder *r);

/* PeekPkgPath: the path of package idx. PeekObj: the package path, the name
 * and the CodeObj of object idx. */
Str burrow__pb_peek_pkg_path(PbPkgDecoder *pr, PbIndex idx);
Str burrow__pb_peek_obj(PbPkgDecoder *pr, PbIndex idx, Str *name, Int *tag);

/* Version: the version of the bitstream r reads. */
PbVersion burrow__pb_decoder_version(const PbDecoder *r);

/* ---------------------------------------------------------------- encoding */

/* PkgEncoder: export data being written.
 *
 * Not in Go: everything it makes comes from a, which it keeps. */
typedef struct PbPkgEncoder {
    PbVersion version;
    Slice elems[PB_NUM_RELOCS]; /* of Str, the elements of each section */
    Map *strings_idx;           /* Str to PbIndex, where each string is */
    /* How many frames to write at each sync marker. Negative turns the
     * markers off. */
    Int sync_frames;
    Alloc *a;
} PbPkgEncoder;

/* NewPkgEncoder: an encoder for version, with sync_frames as above. */
PbPkgEncoder burrow__pb_new_pkg_encoder(Alloc *a, PbVersion version, Int sync_frames);

bool burrow__pb_pkg_encoder_sync_markers(const PbPkgEncoder *pw);

/* DumpTo: writes the data to out and returns the fingerprint in fp. Go
 * panics if out fails, and so does this. */
void burrow__pb_dump_to(PbPkgEncoder *pw, IoWriter out, Byte fp[8]);

/* StringIdx: the index of s, which is added if it is new. */
PbIndex burrow__pb_encoder_string_idx(PbPkgEncoder *pw, Str s);

/* Encoder: the writer of one element. */
typedef struct PbEncoder {
    PbPkgEncoder *p;
    PbRefTableEntry *relocs;
    Int nrelocs;
    Int relocs_cap;
    Map *reloc_map; /* uint64 of kind and index to uint32, made when needed */
    Slice data;     /* of Byte, the element's bitstream so far */
    bool encoding_reloc_header;
    PbSectionKind k;
    PbIndex idx;
} PbEncoder;

/* NewEncoder: an encoder for a new element of section k, which starts with
 * marker. NewEncoderRaw writes no marker. */
BURROW_OWNS(ret) PbEncoder *burrow__pb_new_encoder(PbPkgEncoder *pw, PbSectionKind k,
                                                   PbSyncMarker marker);
BURROW_OWNS(ret) PbEncoder *burrow__pb_new_encoder_raw(PbPkgEncoder *pw,
                                                       PbSectionKind k);

/* Flush: puts the element in its section and returns its index. */
PbIndex burrow__pb_flush(PbEncoder *w);

void burrow__pb_encoder_sync(PbEncoder *w, PbSyncMarker m);
bool burrow__pb_encoder_bool(PbEncoder *w, bool b);
void burrow__pb_encoder_int64(PbEncoder *w, int64_t x);
void burrow__pb_encoder_uint64(PbEncoder *w, uint64_t x);
void burrow__pb_encoder_len(PbEncoder *w, Int x);
void burrow__pb_encoder_int(PbEncoder *w, Int x);
void burrow__pb_encoder_uint(PbEncoder *w, Uint x);
void burrow__pb_encoder_reloc(PbEncoder *w, PbSectionKind k, PbIndex idx);

/* Code: c, after mark, the marker that goes with its type. */
void burrow__pb_encoder_code(PbEncoder *w, PbSyncMarker mark, Int c);

void burrow__pb_encoder_string(PbEncoder *w, Str s);
void burrow__pb_encoder_string_ref(PbEncoder *w, PbIndex idx);
void burrow__pb_encoder_strings(PbEncoder *w, Slice ss);
void burrow__pb_encoder_value(PbEncoder *w, ConstantValue val);

PbVersion burrow__pb_encoder_version(const PbEncoder *w);

#endif /* BURROW_GO_PKGBITS_H */
