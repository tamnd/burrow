/* internal/coverage: the meta-data and counter data files a coverage run
 * writes, and the encoders and decoders for them.
 *
 * Derived from Go's src/internal/coverage: defs.go, and the packages
 * stringtab, uleb128, slicereader, slicewriter, encodemeta, decodemeta,
 * encodecounter and decodecounter. Go source: go1.27.1.
 *
 * Not in Go: everything a reader, writer or builder makes comes from the
 * allocator it was made with, and nothing frees it on its own, so an arena is
 * the easy choice. A string a reader hands back points into the data it was
 * given when the reader is read-only and is a copy otherwise, as in Go. Where
 * Go would index past the end of a malformed blob and panic, this panics with
 * the same runtime error.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_DEBUG_COVERAGE_INTERNAL_H
#define BURROW_DEBUG_COVERAGE_INTERNAL_H

/* burrow:package internal/coverage */

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/own.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

/* Types and constants related to the output files written by code coverage
 * tooling. When a coverage-instrumented binary is run, it emits two output
 * files: a meta-data output file, and a counter data output file. */

/* .....................................................................
 *
 * Meta-data definitions:
 *
 * The meta-data file is composed of a file header, a series of meta-data
 * blobs/sections (one per instrumented package), and an offsets area storing
 * the offsets of each section. Format of the meta-data file looks like:
 *
 * --header----------
 *  | magic: [4]byte magic string
 *  | version
 *  | total length of meta-data file in bytes
 *  | numPkgs: number of package entries in file
 *  | hash: [16]byte hash of entire meta-data payload
 *  | offset to string table section
 *  | length of string table
 *  | number of entries in string table
 *  | counter mode
 *  | counter granularity
 *  --package offsets table------
 *  <offset to pkg 0>
 *  <offset to pkg 1>
 *  ...
 *  --package lengths table------
 *  <length of pkg 0>
 *  <length of pkg 1>
 *  ...
 *  --string table------
 *  <uleb128 len> 8
 *  <data> "somestring"
 *  ...
 *  --package payloads------
 *  <meta-symbol for pkg 0>
 *  <meta-symbol for pkg 1>
 *  ...
 *
 * Each package payload is a stand-alone blob emitted by the compiler, and does
 * not depend on anything else in the meta-data file. In particular, each blob
 * has it's own string table. Note that the file-level string table is expected
 * to be very short (most strings will be in the meta-data blobs themselves). */

/* CovMetaMagic holds the magic string for a meta-data file. */
extern const Byte burrow__cov_meta_magic[4];

/* MetaFilePref is a prefix used when emitting meta-data files; these files are
 * of the form "covmeta.<hash>", where hash is a hash computed from the hashes
 * of all the package meta-data symbols in the program. */
#define COV_META_FILE_PREF "covmeta"

/* MetaFileVersion contains the current (most recent) meta-data file
 * version. */
#define COV_META_FILE_VERSION 1

/* CounterMode tracks the "flavor" of the coverage counters being used in a
 * given coverage-instrumented program. */
typedef uint8_t CovCounterMode;

enum {
    COV_CTR_MODE_INVALID = 0,
    COV_CTR_MODE_SET,       /* "set" mode */
    COV_CTR_MODE_COUNT,     /* "count" mode */
    COV_CTR_MODE_ATOMIC,    /* "atomic" mode */
    COV_CTR_MODE_REG_ONLY,  /* registration-only pseudo-mode */
    COV_CTR_MODE_TEST_MAIN  /* testmain pseudo-mode */
};

/* CounterMode.String. */
BURROW_STATIC(ret) Str burrow__cov_counter_mode_string(CovCounterMode cm);

/* ParseCounterMode. */
CovCounterMode burrow__cov_parse_counter_mode(Str mode);

/* CounterGranularity tracks the granularity of the coverage counters being
 * used in a given coverage-instrumented program. */
typedef uint8_t CovCounterGranularity;

enum {
    COV_CTR_GRANULARITY_INVALID = 0,
    COV_CTR_GRANULARITY_PER_BLOCK,
    COV_CTR_GRANULARITY_PER_FUNC
};

/* CounterGranularity.String. */
BURROW_STATIC(ret) Str burrow__cov_counter_granularity_string(CovCounterGranularity cm);

/* MetaFileHeader stores file header information for a meta-data file. */
typedef struct CovMetaFileHeader {
    Byte magic[4];
    uint32_t version;
    uint64_t total_length;
    uint64_t entries;
    Byte meta_file_hash[16];
    uint32_t str_tab_offset;
    uint32_t str_tab_length;
    CovCounterMode cmode;
    CovCounterGranularity cgranularity;
    /* _ [6]byte, padding */
} CovMetaFileHeader;

/* Not in Go: unsafe.Sizeof(MetaFileHeader{}). */
#define COV_META_FILE_HEADER_SIZE 56

/* MetaSymbolHeader stores header information for a single meta-data blob,
 * e.g. the coverage meta-data payload computed for a given Go package. */
typedef struct CovMetaSymbolHeader {
    uint32_t length;      /* size of meta-symbol payload in bytes */
    uint32_t pkg_name;    /* string table index */
    uint32_t pkg_path;    /* string table index */
    uint32_t module_path; /* string table index */
    Byte meta_hash[16];
    /* _ byte, currently unused */
    /* _ [3]byte, padding */
    uint32_t num_files;
    uint32_t num_funcs;
} CovMetaSymbolHeader;

/* keep in sync with above */
#define COV_META_HEADER_SIZE (16 + 4 + 4 + 4 + 4 + 4 + 4 + 4)

/* As an example, consider the following Go package:
 *
 * 01: package p
 * 02:
 * 03: var v, w, z int
 * 04:
 * 05: func small(x, y int) int {
 * 06:   v++
 * 07:   // comment
 * 08:   if y == 0 {
 * 09:     return x
 * 10:   }
 * 11:   return (x << 1) ^ (9 / y)
 * 12: }
 * 13:
 * 14: func Medium(q, r int) int {
 * 15:   s1 := small(q, r)
 * 16:   z += s1
 * 17:   s2 := small(r, q)
 * 18:   w -= s2
 * 19:   return w + z
 * 20: }
 *
 * The meta-data blob for the single package above might look like the
 * following:
 *
 * -- MetaSymbolHeader header----------
 *  | size: size of this blob in bytes
 *  | packagepath: <path to p>
 *  | modulepath: <modpath for p>
 *  | nfiles: 1
 *  | nfunctions: 2
 *  --func offsets table------
 *  <offset to func 0>
 *  <offset to func 1>
 *  --string table (contains all files and functions)------
 *  | <uleb128 len> 4
 *  | <data> "p.go"
 *  | <uleb128 len> 5
 *  | <data> "small"
 *  | <uleb128 len> 6
 *  | <data> "Medium"
 *  --func 0------
 *  | <uleb128> num units: 3
 *  | <uleb128> func name: S1 (index into string table)
 *  | <uleb128> file: S0 (index into string table)
 *  | <unit 0>:  S0   L6     L8    2
 *  | <unit 1>:  S0   L9     L9    1
 *  | <unit 2>:  S0   L11    L11   1
 *  --func 1------
 *  | <uleb128> num units: 1
 *  | <uleb128> func name: S2 (index into string table)
 *  | <uleb128> file: S0 (index into string table)
 *  | <unit 0>:  S0   L15    L19   5
 *  ---end----------- */

/* The following types and constants used by the meta-data encoder/decoder. */

/* CoverableUnit describes the source characteristics of a single program unit
 * for which we want to gather coverage info. Coverable units are either
 * "simple" or "intraline"; a "simple" coverable unit corresponds to a basic
 * block (region of straight-line code with no jumps or control transfers). An
 * "intraline" unit corresponds to a logical clause nested within some other
 * simple unit. A simple unit will have a zero Parent value; for an intraline
 * unit NxStmts will be zero and Parent will be set to 1 plus the index of the
 * containing simple statement. Example:
 *
 *	L7:   q := 1
 *	L8:   x := (y == 101 || launch() == false)
 *	L9:   r := x * 2
 *
 * For the code above we would have three simple units (one for each line),
 * then an intraline unit describing the "launch() == false" clause in line 8,
 * with Parent pointing to the index of the line 8 unit in the units array.
 *
 * Note: in the initial version of the coverage revamp, only simple units will
 * be in use. */
typedef struct CovCoverableUnit {
    uint32_t st_line, st_col;
    uint32_t en_line, en_col;
    uint32_t nx_stmts;
    uint32_t parent;
} CovCoverableUnit;

/* Not in Go: the element types for a Slice of CovCoverableUnit, and for the
 * Slice of byte Slices that the meta-data file writer takes. */
extern const Type burrow__cov_coverable_unit_type;
extern const Type burrow__cov_blob_type;

/* FuncDesc encapsulates the meta-data definitions for a single Go function.
 * This version assumes that we're looking at a function before inlining; if we
 * want to capture a post-inlining view of the world, the representations of
 * source positions would need to be a good deal more complicated.
 *
 * Not in Go: units is a Slice of CovCoverableUnit. */
typedef struct CovFuncDesc {
    Str funcname;
    Str srcfile;
    Slice units;
    bool lit; /* true if this is a function literal */
} CovFuncDesc;

/* Name of file within the "go test -cover" temp coverdir directory containing
 * a list of meta-data files for packages being tested in a "go test
 * -coverpkg=... ..." run. This constant is shared by the Go command and by the
 * coverage runtime. */
#define COV_META_FILES_FILE_NAME "metafiles.txt"

/* .....................................................................
 *
 * Counter data definitions:
 */

/* A counter data file is composed of a file header followed by one or more
 * "segments" (each segment representing a given run or partial run of a give
 * binary) followed by a footer. */

/* CovCounterMagic holds the magic string for a coverage counter-data file. */
extern const Byte burrow__cov_counter_magic[4];

/* CounterFileVersion stores the most recent counter data file version. */
#define COV_COUNTER_FILE_VERSION 1

/* CounterFlavor describes how function and counters are stored/represented in
 * the counter section of the file. */
typedef uint8_t CovCounterFlavor;

enum {
    /* "Raw" representation: all values (pkg ID, func ID, num counters, and
     * counters themselves) are stored as uint32's. */
    COV_CTR_RAW = 1,

    /* "ULeb" representation: all values (pkg ID, func ID, num counters, and
     * counters themselves) are stored with ULEB128 encoding. */
    COV_CTR_ULEB128
};

/* CounterFileHeader stores files header information for a counter-data
 * file. */
typedef struct CovCounterFileHeader {
    Byte magic[4];
    uint32_t version;
    Byte meta_hash[16];
    CovCounterFlavor cflavor;
    bool big_endian;
    /* _ [6]byte, padding */
} CovCounterFileHeader;

/* Not in Go: unsafe.Sizeof(CounterFileHeader{}). */
#define COV_COUNTER_FILE_HEADER_SIZE 32

/* CounterSegmentHeader encapsulates information about a specific segment in a
 * counter data file, which at the moment contains counters data from a single
 * execution of a coverage-instrumented program. Following the segment header
 * will be the string table and args table, and then (possibly) padding bytes
 * to bring the byte size of the preamble up to a multiple of 4. Immediately
 * following that will be the counter payloads.
 *
 * The "args" section of a segment is used to store annotations describing
 * where the counter data came from; this section is basically a series of
 * key-value pairs (can be thought of as an encoded 'map[string]string'). At
 * the moment we only write os.Args() data to this section, using pairs of the
 * form "argc=<integer>", "argv0=<os.Args[0]>", "argv1=<os.Args[1]>", and so
 * on. In the future the args table may also include things like GOOS/GOARCH
 * values, and/or tags indicating which tests were run to generate the counter
 * data. */
typedef struct CovCounterSegmentHeader {
    uint64_t fcn_entries;
    uint32_t str_tab_len;
    uint32_t args_len;
} CovCounterSegmentHeader;

/* Not in Go: unsafe.Sizeof(CounterSegmentHeader{}). */
#define COV_COUNTER_SEGMENT_HEADER_SIZE 16

/* CounterFileFooter appears at the tail end of a counter data file, and stores
 * the number of segments it contains. */
typedef struct CovCounterFileFooter {
    Byte magic[4];
    /* _ [4]byte, padding */
    uint32_t num_segments;
    /* _ [4]byte, padding */
} CovCounterFileFooter;

/* Not in Go: unsafe.Sizeof(CounterFileFooter{}). */
#define COV_COUNTER_FILE_FOOTER_SIZE 16

/* CounterFilePref is the file prefix used when emitting coverage data output
 * files. CounterFileTemplate describes the format of the file name: prefix
 * followed by meta-file hash followed by process ID followed by emit
 * UnixNanoTime. */
#define COV_COUNTER_FILE_PREF "covcounters"
#define COV_COUNTER_FILE_TEMPL "%s.%x.%d.%d"
#define COV_COUNTER_FILE_REGEXP "^%s\\.(\\S+)\\.(\\d+)\\.(\\d+)+$"

/* Round4. */
static inline Int burrow__cov_round4(Int x) {
    return (x + 3) & ~(Int)3;
}

/* .....................................................................
 *
 * Runtime counter data definitions.
 */

/* At runtime within a coverage-instrumented program, the "counters" object we
 * associated with instrumented function can be thought of as a struct of the
 * following form:
 *
 * struct {
 *     numCtrs uint32
 *     pkgid uint32
 *     funcid uint32
 *     counterArray [numBlocks]uint32
 * }
 *
 * where "numCtrs" is the number of blocks / coverable units within the
 * function, "pkgid" is the unique index assigned to this package by the
 * runtime, "funcid" is the index of this function within its containing
 * package, and "counterArray" stores the actual counters.
 *
 * The counter variable itself is created not as a struct but as a flat array
 * of uint32's; we then use the offsets below to index into it. */

#define COV_NUM_CTRS_OFFSET 0
#define COV_PKG_ID_OFFSET 1
#define COV_FUNC_ID_OFFSET 2
#define COV_FIRST_CTR_OFFSET 3

/* ----------------------------------------------------------------- uleb128 */

/* uleb128.AppendUleb128. */
BURROW_OWNS(ret)
BURROW_BORROWS(ret, b) Slice burrow__cov_append_uleb128(Alloc *a, Slice b, uint64_t v);

/* ------------------------------------------------------------- slicereader */

/* This file contains the helper "SliceReader", a utility for reading values
 * from a byte slice that may or may not be backed by a read-only mmap'd
 * region. */
typedef struct CovSliceReader {
    Slice b;
    bool readonly;
    int64_t off;
    Alloc *a; /* Not in Go: where ReadString's copies come from */
} CovSliceReader;

BURROW_OWNS(ret)
CovSliceReader *burrow__cov_new_slice_reader(Alloc *a, Slice b, bool readonly);
Int burrow__cov_slice_reader_read(CovSliceReader *r, Slice b, Error *err);
int64_t burrow__cov_slice_reader_seek(CovSliceReader *r, int64_t offset, int whence,
                                      Error *err);
int64_t burrow__cov_slice_reader_offset(const CovSliceReader *r);
uint8_t burrow__cov_slice_reader_read_uint8(CovSliceReader *r);
uint32_t burrow__cov_slice_reader_read_uint32(CovSliceReader *r);
uint64_t burrow__cov_slice_reader_read_uint64(CovSliceReader *r);
uint64_t burrow__cov_slice_reader_read_uleb128(CovSliceReader *r);
BURROW_BORROWS(ret, r) Str burrow__cov_slice_reader_read_string(CovSliceReader *r,
                                                               int64_t len);

/* Not in Go: r as an io.Reader, borrowing it. */
IoReader burrow__cov_slice_reader_as_io_reader(CovSliceReader *r);

/* ------------------------------------------------------------- slicewriter */

/* WriteSeeker is a helper object that implements the io.WriteSeeker
 * interface. Clients can create a WriteSeeker, make a series of Write calls to
 * add data to it (and possibly Seek calls to update previously written
 * portions), then finally invoke BytesWritten() to get a pointer to the
 * constructed byte slice.
 *
 * Not in Go: a zero CovWriteSeeker with a set to an allocator is ready to
 * use, as &WriteSeeker{} is in Go. */
typedef struct CovWriteSeeker {
    Slice payload;
    int64_t off;
    Alloc *a; /* Not in Go: where the payload grows into */
} CovWriteSeeker;

Int burrow__cov_write_seeker_write(CovWriteSeeker *sws, Slice p, Error *err);

/* Seek repositions the read/write position of the WriteSeeker within its
 * internally maintained slice. Note that it is not possible to expand the size
 * of the slice using SEEK_SET; trying to seek outside the slice will result in
 * an error. */
int64_t burrow__cov_write_seeker_seek(CovWriteSeeker *sws, int64_t offset, int whence,
                                      Error *err);

/* BytesWritten returns the underlying byte slice for the WriteSeeker,
 * containing the data written to it via Write/Seek calls. */
BURROW_BORROWS(ret, sws) Slice burrow__cov_write_seeker_bytes_written(CovWriteSeeker *sws);

Int burrow__cov_write_seeker_read(CovWriteSeeker *sws, Slice p, Error *err);

/* Not in Go: sws as an io.WriteSeeker, borrowing it. */
IoWriteSeeker burrow__cov_write_seeker_as_io_write_seeker(CovWriteSeeker *sws);

/* --------------------------------------------------------------- stringtab */

/* This package implements string table writer and reader utilities, for use
 * in emitting and reading/decoding coverage meta-data and counter-data
 * files. */

/* Writer implements a string table writing utility. */
typedef struct CovStrtabWriter {
    Map *stab; /* string to uint32_t */
    Slice strs; /* of Str */
    Slice tmp;
    bool frozen;
    Alloc *a; /* Not in Go: where the table and its strings live */
} CovStrtabWriter;

/* InitWriter initializes a stringtab.Writer.
 *
 * Not in Go: a is where the writer keeps its table, and a string the table
 * takes in is copied there. */
void burrow__cov_strtab_init_writer(CovStrtabWriter *stw, Alloc *a);

/* Nentries returns the number of strings interned so far. */
uint32_t burrow__cov_strtab_nentries(const CovStrtabWriter *stw);

/* Lookup looks up string 's' in the writer's table, adding a new entry if
 * need be, and returning an index into the table. */
uint32_t burrow__cov_strtab_lookup(CovStrtabWriter *stw, Str s);

/* Size computes the memory in bytes needed for the serialized version of a
 * stringtab.Writer. */
uint32_t burrow__cov_strtab_size(CovStrtabWriter *stw);

/* Write writes the string table in serialized form to the specified
 * io.Writer. */
BURROW_BORROWS(ret) Error burrow__cov_strtab_write(CovStrtabWriter *stw, IoWriter w);

/* Freeze sends a signal to the writer that no more additions are allowed, only
 * lookups of existing strings (if a lookup triggers addition, a panic will
 * result). Useful as a mechanism for "finalizing" a string table prior to
 * writing it out. */
void burrow__cov_strtab_freeze(CovStrtabWriter *stw);

/* Reader is a helper for reading a string table previously serialized by a
 * Writer.Write call. */
typedef struct CovStrtabReader {
    CovSliceReader *r;
    Slice strs; /* of Str */
} CovStrtabReader;

/* NewReader creates a stringtab.Reader to read the contents of a string table
 * from 'r'. */
BURROW_OWNS(ret) CovStrtabReader *burrow__cov_new_strtab_reader(Alloc *a, CovSliceReader *r);

/* Read reads/decodes a string table using the reader provided. */
void burrow__cov_strtab_reader_read(CovStrtabReader *str);

/* Entries returns the number of decoded entries in a string table. */
Int burrow__cov_strtab_reader_entries(const CovStrtabReader *str);

/* Get returns string 'idx' within the string table. */
BURROW_BORROWS(ret, str) Str burrow__cov_strtab_reader_get(const CovStrtabReader *str,
                                                          uint32_t idx);

/* -------------------------------------------------------------- encodemeta */

/* This package contains APIs and helpers for encoding the meta-data "blob" for
 * a single Go package, created when coverage instrumentation is turned on. */

typedef struct CovMetaDataBuilder CovMetaDataBuilder;

/* NewCoverageMetaDataBuilder. NULL and an error through err for an empty
 * package path. */
BURROW_OWNS(ret)
CovMetaDataBuilder *burrow__cov_new_meta_data_builder(Alloc *a, Str pkgpath, Str pkgname,
                                                      Str modulepath, Error *err);

/* AddFunc registers a new function with the meta data builder. */
uint64_t burrow__cov_meta_data_builder_add_func(CovMetaDataBuilder *b, CovFuncDesc f);

/* Emit writes the meta-data accumulated so far in this builder to 'w'.
 * Returns a hash of the meta-data payload and an error.
 *
 * Not in Go: the hash goes into digest. */
BURROW_BORROWS(ret)
Error burrow__cov_meta_data_builder_emit(CovMetaDataBuilder *b, IoWriteSeeker w,
                                         Byte digest[16]);

/* HashFuncDesc computes an md5 sum of a coverage.FuncDesc and returns a digest
 * for it. */
void burrow__cov_hash_func_desc(const CovFuncDesc *f, Byte digest[16]);

/* This package contains APIs and helpers for writing out a meta-data file
 * (composed of a file header, offsets/lengths, and then a series of meta-data
 * blobs emitted by the compiler, one per Go package). */

typedef struct CovMetaFileWriter CovMetaFileWriter;

/* NewCoverageMetaFileWriter. */
BURROW_OWNS(ret)
CovMetaFileWriter *burrow__cov_new_meta_file_writer(Alloc *a, Str mfname, IoWriter w);

/* CoverageMetaFileWriter.Write. blobs is a Slice of Slices of bytes. */
BURROW_BORROWS(ret)
Error burrow__cov_meta_file_writer_write(CovMetaFileWriter *m, const Byte final_hash[16],
                                         Slice blobs, CovCounterMode mode,
                                         CovCounterGranularity granularity);

/* -------------------------------------------------------------- decodemeta */

/* This package contains APIs and helpers for decoding a single package's meta
 * data "blob" emitted by the compiler when coverage instrumentation is turned
 * on. */

/* See comments in the encodecovmeta package for details on the format. */

typedef struct CovMetaDataDecoder {
    CovSliceReader *r;
    CovMetaSymbolHeader hdr;
    CovStrtabReader *strtab;
} CovMetaDataDecoder;

/* NewCoverageMetaDataDecoder. */
BURROW_OWNS(ret)
CovMetaDataDecoder *burrow__cov_new_meta_data_decoder(Alloc *a, Slice b, bool readonly,
                                                      Error *err);

BURROW_BORROWS(ret, d) Str burrow__cov_meta_data_decoder_package_path(CovMetaDataDecoder *d);
BURROW_BORROWS(ret, d) Str burrow__cov_meta_data_decoder_package_name(CovMetaDataDecoder *d);
BURROW_BORROWS(ret, d) Str burrow__cov_meta_data_decoder_module_path(CovMetaDataDecoder *d);
uint32_t burrow__cov_meta_data_decoder_num_funcs(const CovMetaDataDecoder *d);

/* ReadFunc reads the coverage meta-data for the function with index 'findex',
 * filling it into the FuncDesc pointed to by 'f'.
 *
 * Not in Go: f's units grow in the decoder's allocator. */
BURROW_BORROWS(ret)
Error burrow__cov_meta_data_decoder_read_func(CovMetaDataDecoder *d, uint32_t fidx,
                                              CovFuncDesc *f);

/* This package contains APIs and helpers for reading and decoding meta-data
 * output files emitted by the runtime when a coverage-instrumented binary
 * executes. A meta-data file contains top-level info (counter mode, number of
 * packages) and then a separate self-contained meta-data section for each Go
 * package. */

/* CoverageMetaFileReader provides state and methods for reading a meta-data
 * file from a code coverage run. */
typedef struct CovMetaFileReader CovMetaFileReader;

/* NewCoverageMetaFileReader returns a new helper object for reading the
 * coverage meta-data output file 'f'. The param 'fileView' is a read-only
 * slice containing the contents of 'f' obtained by mmap'ing the file
 * read-only; 'fileView' may be nil, in which case the helper will read the
 * contents of the file using regular file Read operations. */
BURROW_OWNS(ret)
CovMetaFileReader *burrow__cov_new_meta_file_reader(Alloc *a, OsFile *f, Slice file_view,
                                                    Error *err);

/* NumPackages returns the number of packages for which this file contains
 * meta-data. */
uint64_t burrow__cov_meta_file_reader_num_packages(const CovMetaFileReader *r);

/* CounterMode returns the counter mode (set, count, atomic) used when building
 * for coverage for the program that produce this meta-data file. */
CovCounterMode burrow__cov_meta_file_reader_counter_mode(const CovMetaFileReader *r);

/* CounterGranularity returns the counter granularity (single counter per
 * function, or counter per block) selected when building for coverage for the
 * program that produce this meta-data file. */
CovCounterGranularity
burrow__cov_meta_file_reader_counter_granularity(const CovMetaFileReader *r);

/* FileHash returns the hash computed for all of the package meta-data blobs.
 * Coverage counter data files refer to this hash, and the hash will be encoded
 * into the meta-data file name. */
void burrow__cov_meta_file_reader_file_hash(const CovMetaFileReader *r, Byte out[16]);

/* GetPackageDecoder requests a decoder object for the package within the
 * meta-data file whose index is 'pkIdx'. If the CoverageMetaFileReader was set
 * up with a read-only file view, a pointer into that file view will be
 * returned, otherwise the buffer 'payloadbuf' will be written to (or if it is
 * not of sufficient size, a new buffer will be allocated). Return value is the
 * decoder, a byte slice with the encoded meta-data, and an error.
 *
 * Not in Go: the byte slice goes into *payload. */
BURROW_OWNS(ret)
CovMetaDataDecoder *burrow__cov_meta_file_reader_get_package_decoder(CovMetaFileReader *r,
                                                                     uint32_t pk_idx,
                                                                     Slice payloadbuf,
                                                                     Slice *payload,
                                                                     Error *err);

/* GetPackagePayload returns the raw (encoded) meta-data payload for the
 * package with index 'pkIdx'. As with GetPackageDecoder, if the
 * CoverageMetaFileReader was set up with a read-only file view, a pointer into
 * that file view will be returned, otherwise the buffer 'payloadbuf' will be
 * written to (or if it is not of sufficient size, a new buffer will be
 * allocated). Return value is the decoder, a byte slice with the encoded
 * meta-data, and an error. */
BURROW_OWNS(ret)
Slice burrow__cov_meta_file_reader_get_package_payload(CovMetaFileReader *r,
                                                       uint32_t pk_idx, Slice payloadbuf,
                                                       Error *err);

/* ----------------------------------------------------------- encodecounter */

/* This package contains APIs and helpers for encoding initial portions of the
 * counter data files emitted at runtime when coverage instrumentation is
 * enabled. Counter data files may contain multiple segments; the file header
 * and first segment are written via the "Write" method below, and additional
 * segments can then be added using "AddSegment". */

typedef struct CovCoverageDataWriter CovCoverageDataWriter;

/* NewCoverageDataWriter. */
BURROW_OWNS(ret)
CovCoverageDataWriter *burrow__cov_new_coverage_data_writer(Alloc *a, IoWriter w,
                                                            CovCounterFlavor flav);

/* CounterVisitorFn describes a callback function invoked when writing coverage
 * counter data.
 *
 * Not in Go: counters is a Slice of uint32_t, and env is what the caller
 * handed to visit_funcs along with the function. */
typedef Error (*CovCounterVisitorFn)(void *env, uint32_t pkid, uint32_t funcid,
                                     Slice counters);

/* CounterVisitor describes a helper object used during counter file writing;
 * when writing counter data files, clients pass a CounterVisitor to the
 * write/emit routines, then the expectation is that the VisitFuncs method will
 * then invoke the callback "f" with data for each function to emit to the
 * file. */
typedef struct CovCounterVisitor {
    Error (*visit_funcs)(void *self, CovCounterVisitorFn f, void *env);
    void *self;
} CovCounterVisitor;

/* Write writes the contents of the count-data file to the writer previously
 * supplied to NewCoverageDataWriter. Returns an error if something went wrong
 * somewhere with the write.
 *
 * Not in Go: args is a Map from string to string. */
BURROW_BORROWS(ret)
Error burrow__cov_coverage_data_writer_write(CovCoverageDataWriter *cfw,
                                             const Byte meta_file_hash[16], Map *args,
                                             CovCounterVisitor visitor);

/* AppendSegment appends a new segment to a counter data, with a new args
 * section followed by a payload of counter data clauses. */
BURROW_BORROWS(ret)
Error burrow__cov_coverage_data_writer_append_segment(CovCoverageDataWriter *cfw,
                                                      Map *args,
                                                      CovCounterVisitor visitor);

/* ----------------------------------------------------------- decodecounter */

/* This file contains helpers for reading counter data files created during the
 * executions of a coverage-instrumented binary. */

typedef struct CovCounterDataReader CovCounterDataReader;

/* NewCounterDataReader. */
BURROW_OWNS(ret)
CovCounterDataReader *burrow__cov_new_counter_data_reader(Alloc *a, Str fn,
                                                          IoReadSeeker rs, Error *err);

/* OsArgs returns the program arguments (saved from os.Args during the run of
 * the instrumented binary) read from the counter data file. Not all coverage
 * data files will have os.Args values; for example, if a data file is
 * produced by merging coverage data from two distinct runs, no os args will be
 * available (an empty list is returned).
 *
 * Not in Go: the list is a Slice of Str. */
BURROW_BORROWS(ret, cdr) Slice burrow__cov_counter_data_reader_os_args(CovCounterDataReader *cdr);

/* Goos returns the GOOS setting in effect for the "-cover" binary that
 * produced this counter data file. The GOOS value may be empty in the case
 * where the counter data file was produced from a merge in which more than one
 * GOOS value was present. */
BURROW_BORROWS(ret, cdr) Str burrow__cov_counter_data_reader_goos(CovCounterDataReader *cdr);

/* Goarch returns the GOARCH setting in effect for the "-cover" binary that
 * produced this counter data file. The GOARCH value may be empty in the case
 * where the counter data file was produced from a merge in which more than one
 * GOARCH value was present. */
BURROW_BORROWS(ret, cdr) Str burrow__cov_counter_data_reader_goarch(CovCounterDataReader *cdr);

/* FuncPayload encapsulates the counter data payload for a single function as
 * read from a counter data file.
 *
 * Not in Go: counters is a Slice of uint32_t. */
typedef struct CovFuncPayload {
    uint32_t pkg_idx;
    uint32_t func_idx;
    Slice counters;
} CovFuncPayload;

/* NumSegments returns the number of execution segments in the file. */
uint32_t burrow__cov_counter_data_reader_num_segments(const CovCounterDataReader *cdr);

/* BeginNextSegment sets up the reader to read the next segment, returning TRUE
 * if we do have another segment to read, or FALSE if we're done with all the
 * segments (also an error if something went wrong). */
bool burrow__cov_counter_data_reader_begin_next_segment(CovCounterDataReader *cdr,
                                                        Error *err);

/* NumFunctionsInSegment returns the number of live functions in the currently
 * selected segment. */
uint32_t burrow__cov_counter_data_reader_num_functions_in_segment(
    const CovCounterDataReader *cdr);

/* NextFunc reads data for the next function in this current segment into "p",
 * returning TRUE if the read was successful or FALSE if we've read all the
 * functions already (also an error if something went wrong with the read or we
 * hit a premature EOF).
 *
 * Not in Go: p's counters grow in the reader's allocator. */
bool burrow__cov_counter_data_reader_next_func(CovCounterDataReader *cdr, CovFuncPayload *p,
                                               Error *err);

#endif /* BURROW_DEBUG_COVERAGE_INTERNAL_H */
