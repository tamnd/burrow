/* internal/profile: an in-memory representation of
 * github.com/google/pprof/proto/profile.proto and the code to encode, decode
 * and merge profiles in that format.
 *
 * Derived from Go's src/internal/profile. Go source: go1.27.1.
 *
 * runtime/pprof's tests read the profiles it writes back with this, and
 * net/http/pprof uses it to take the difference of two profiles. graph.go is
 * left out, since only cmd/ uses it.
 *
 * Not in Go: a profile and everything in it comes from the Alloc it was made
 * with, and nothing is freed on its own, so make it from an arena and free the
 * arena. Go's map[string][]string and map[string][]int64 labels are slices of
 * PfStrLabels and PfNumLabels in the order the keys were added, which is one of
 * the orders Go's maps can give. Go's pointer slices are slices of
 * TYPE_UNSAFE_POINTER.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_DEBUG_PPROF_PROFILE_H
#define BURROW_DEBUG_PPROF_PROFILE_H

/* burrow:package internal/profile */

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/regexp.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ proto */

/* The state of an encode or a decode. a is where encoded bytes and decoded
 * values come from. */
typedef struct PfBuffer {
    Int field;
    Int typ;
    uint64_t u64;
    Slice data;
    Byte tmp[16];
    Alloc *a;
} PfBuffer;

/* The function that decodes one field of a message. */
typedef Error (*PfDecoder)(PfBuffer *b, void *m);

/* A protocol message: decoders is a table indexed by field number that gives
 * the function to decode that field, and encode encodes its receiver into the
 * given buffer. */
typedef struct PfMessage {
    const PfDecoder *decoders;
    Int ndecoders;
    void (*encode)(PfBuffer *b, const void *m);
} PfMessage;

BURROW_OWNS(ret) Slice burrow__pf_marshal(Alloc *a, const PfMessage *mt, const void *m);
Error burrow__pf_unmarshal(Alloc *a, Slice data, const PfMessage *mt, void *m);

void burrow__pf_encode_varint(PfBuffer *b, uint64_t x);
void burrow__pf_encode_length(PfBuffer *b, Int tag, Int len);
void burrow__pf_encode_uint64(PfBuffer *b, Int tag, uint64_t x);
void burrow__pf_encode_uint64s(PfBuffer *b, Int tag, Slice x);
void burrow__pf_encode_uint64_opt(PfBuffer *b, Int tag, uint64_t x);
void burrow__pf_encode_int64(PfBuffer *b, Int tag, int64_t x);
void burrow__pf_encode_int64_opt(PfBuffer *b, Int tag, int64_t x);
void burrow__pf_encode_int64s(PfBuffer *b, Int tag, Slice x);
void burrow__pf_encode_string(PfBuffer *b, Int tag, Str x);
void burrow__pf_encode_strings(PfBuffer *b, Int tag, Slice x);
void burrow__pf_encode_bool(PfBuffer *b, Int tag, bool x);
void burrow__pf_encode_bool_opt(PfBuffer *b, Int tag, bool x);
void burrow__pf_encode_message(PfBuffer *b, Int tag, const PfMessage *mt,
                               const void *m);

Error burrow__pf_decode_message(PfBuffer *b, const PfMessage *mt, void *m);
Error burrow__pf_decode_int64(PfBuffer *b, int64_t *x);
Error burrow__pf_decode_int64s(PfBuffer *b, Slice *x);
Error burrow__pf_decode_uint64(PfBuffer *b, uint64_t *x);
Error burrow__pf_decode_uint64s(PfBuffer *b, Slice *x);
Error burrow__pf_decode_string(PfBuffer *b, Str *x);
Error burrow__pf_decode_strings(PfBuffer *b, Slice *x);
Error burrow__pf_decode_bool(PfBuffer *b, bool *x);

/* ---------------------------------------------------------------- profile */

/* ValueType corresponds to Profile.ValueType */
typedef struct PfValueType {
    Str type; /* cpu, wall, inuse_space, etc */
    Str unit; /* seconds, nanoseconds, bytes, etc */

    int64_t type_x;
    int64_t unit_x;
} PfValueType;

/* Label corresponds to Profile.Label */
typedef struct PfLabel {
    int64_t key_x;
    /* Exactly one of the two following values must be set */
    int64_t str_x;
    int64_t num_x; /* Integer value for this label */
} PfLabel;

/* Mapping corresponds to Profile.Mapping */
typedef struct PfMapping {
    uint64_t id;
    uint64_t start;
    uint64_t limit;
    uint64_t offset;
    Str file;
    Str build_id;
    bool has_functions;
    bool has_filenames;
    bool has_line_numbers;
    bool has_inline_frames;

    int64_t file_x;
    int64_t build_id_x;
} PfMapping;

/* Function corresponds to Profile.Function */
typedef struct PfFunction {
    uint64_t id;
    Str name;
    Str system_name;
    Str filename;
    int64_t start_line;

    int64_t name_x;
    int64_t system_name_x;
    int64_t filename_x;
} PfFunction;

/* Line corresponds to Profile.Line */
typedef struct PfLine {
    PfFunction *function;
    int64_t line;

    uint64_t function_idx;
} PfLine;

/* Location corresponds to Profile.Location */
typedef struct PfLocation {
    uint64_t id;
    PfMapping *mapping;
    uint64_t address;
    Slice line; /* PfLine */
    bool is_folded;

    uint64_t mapping_idx;
} PfLocation;

/* One key of Go's map[string][]string. */
typedef struct PfStrLabels {
    Str key;
    Slice values; /* Str */
} PfStrLabels;

/* One key of Go's map[string][]int64. */
typedef struct PfNumLabels {
    Str key;
    Slice values; /* int64_t */
} PfNumLabels;

/* Sample corresponds to Profile.Sample */
typedef struct PfSample {
    Slice location;  /* PfLocation * */
    Slice value;     /* int64_t */
    Slice label;     /* PfStrLabels * */
    Slice num_label; /* PfNumLabels * */
    Slice num_unit;  /* PfStrLabels * */

    Slice location_idx; /* uint64_t */
    Slice label_x;      /* PfLabel */
} PfSample;

/* Profile is an in-memory representation of profile.proto. */
typedef struct PfProfile {
    Alloc *a;

    Slice sample_type; /* PfValueType * */
    Str default_sample_type;
    Slice sample;   /* PfSample * */
    Slice mapping;  /* PfMapping * */
    Slice location; /* PfLocation * */
    Slice function; /* PfFunction * */
    Slice comments; /* Str */

    Str drop_frames;
    Str keep_frames;

    int64_t time_nanos;
    int64_t duration_nanos;
    PfValueType *period_type;
    int64_t period;

    Slice comment_x; /* int64_t */
    int64_t drop_frames_x;
    int64_t keep_frames_x;
    Slice string_table; /* Str */
    int64_t default_sample_type_x;
} PfProfile;

/* An empty profile whose parts come from a. */
BURROW_OWNS(ret) PfProfile *burrow__pf_new_profile(Alloc *a);

/* The element type of PfLocation.line, for making one. */
const Type *burrow__pf_line_type(void);

/* The values for key in a sample's labels, or NULL if it has none. */
PfStrLabels *burrow__pf_labels_get(Slice labels, Str key);
PfNumLabels *burrow__pf_num_labels_get(Slice labels, Str key);

/* labels[key] = append(labels[key], v). */
void burrow__pf_labels_add(Alloc *a, Slice *labels, Str key, Str v);
void burrow__pf_num_labels_add(Alloc *a, Slice *labels, Str key, int64_t v);

/* Parse parses a profile and checks for its validity. The input must be an
 * encoded pprof protobuf, which may optionally be gzip-compressed. */
BURROW_OWNS(ret) PfProfile *burrow__pf_parse(Alloc *a, IoReader r, Error *err);

extern const Error burrow__pf_err_malformed;
extern const Error burrow__pf_err_no_data;

/* Write writes the profile as a gzip-compressed marshaled protobuf. */
Error burrow__pf_profile_write(PfProfile *p, IoWriter w);

/* CheckValid tests whether the profile is valid. Checks include, but are
 * not limited to:
 *   - len(Profile.Sample[n].value) == len(Profile.value_unit)
 *   - Sample.id has a corresponding Profile.Location */
Error burrow__pf_profile_check_valid(PfProfile *p);

/* Aggregate merges the locations in the profile into equivalence
 * classes preserving the request attributes. It also updates the
 * samples to point to the merged locations. */
Error burrow__pf_profile_aggregate(PfProfile *p, bool inline_frame, bool function,
                                   bool filename, bool linenumber, bool address);

/* Print dumps a text representation of a profile. Intended mainly
 * for debugging purposes. */
BURROW_OWNS(ret) Str burrow__pf_profile_string(PfProfile *p, Alloc *a);

/* Merge adds profile p adjusted by ratio r into profile p. Profiles
 * must be compatible (same Type and SampleType). */
Error burrow__pf_profile_merge(PfProfile *p, PfProfile *pb, double r);

/* Compatible determines if two profiles can be compared/merged.
 * returns nil if the profiles are compatible; otherwise an error with
 * details on the incompatibility. */
Error burrow__pf_profile_compatible(PfProfile *p, PfProfile *pb);

/* HasFunctions determines if all locations in this profile have
 * symbolized function information. */
bool burrow__pf_profile_has_functions(PfProfile *p);

/* HasFileLines determines if all locations in this profile have
 * symbolized file and line number information. */
bool burrow__pf_profile_has_file_lines(PfProfile *p);

/* Copy makes a fully independent copy of a profile. */
BURROW_OWNS(ret) PfProfile *burrow__pf_profile_copy(PfProfile *p);

/* Demangler maps symbol names to a human-readable form. This may
 * include C++ demangling and additional simplification. Names that
 * are not demangled may be missing from the resulting map.
 *
 * Not in Go: the map is Str to Str and comes from a. */
BURROW_FUNC(PfDemangler, Map *, Alloc *a, Slice name, Error *err);

/* Demangle attempts to demangle and optionally simplify any function
 * names referenced in the profile. It works on a best-effort basis:
 * it will silently preserve the original names in case of any errors. */
Error burrow__pf_profile_demangle(PfProfile *p, PfDemangler d);

/* Empty reports whether the profile contains no samples. */
bool burrow__pf_profile_empty(PfProfile *p);

/* Scale multiplies all sample values in a profile by a constant. */
void burrow__pf_profile_scale(PfProfile *p, double ratio);

/* ScaleN multiplies each sample values in a sample by a different amount.
 * ratios is a slice of double. */
Error burrow__pf_profile_scale_n(PfProfile *p, Slice ratios);

/* ------------------------------------------------------------------ merge */

/* Merge merges all the profiles in profs into a single Profile.
 * Returns a new profile independent of the input profiles. The merged
 * profile is compacted to eliminate unused samples, locations,
 * functions and mappings. Profiles must have identical profile sample
 * and period types or the merge will fail. profile.Period of the
 * resulting profile will be the maximum of all profiles, and
 * profile.TimeNanos will be the earliest nonzero one.
 *
 * srcs is a slice of PfProfile *. */
BURROW_OWNS(ret) PfProfile *burrow__pf_merge(Alloc *a, Slice srcs, Error *err);

/* Normalize normalizes the source profile by multiplying each value in profile
 * by the ratio of the sum of the base profile's values of that sample type to
 * the sum of the source profile's value of that sample type. */
Error burrow__pf_profile_normalize(PfProfile *p, PfProfile *pb);

/* ----------------------------------------------------------------- filter */

/* TagMatch selects tags for filtering */
BURROW_FUNC(PfTagMatch, bool, Str key, Str val, int64_t nval);

/* FilterSamplesByTag removes all samples from the profile, except
 * those that match focus and do not match the ignore regular
 * expression. fm and im may be NULL. */
void burrow__pf_profile_filter_samples_by_tag(PfProfile *p, PfTagMatch focus,
                                              PfTagMatch ignore, bool *fm, bool *im);

/* ------------------------------------------------------------------ prune */

/* Prune removes all nodes beneath a node matching dropRx, and not
 * matching keepRx. If the root node of a Sample matches, the sample
 * will have an empty stack. */
void burrow__pf_profile_prune(PfProfile *p, const Regexp *drop_rx,
                              const Regexp *keep_rx);

/* RemoveUninteresting prunes and elides profiles using built-in
 * tables of uninteresting function names. */
Error burrow__pf_profile_remove_uninteresting(PfProfile *p);

#endif
