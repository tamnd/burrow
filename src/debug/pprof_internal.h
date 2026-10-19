/* The parts of runtime/pprof that its own files and tests share: the label map
 * that a context and a goroutine hold, and the builder that writes a profile
 * as profile.proto.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_DEBUG_PPROF_INTERNAL_H
#define BURROW_DEBUG_PPROF_INTERNAL_H

/* burrow:package runtime/pprof */

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mprof.h"
#include "burrow/runtime.h"
#include "burrow/runtime/pprof.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* labelMap is the representation of the label set held in the context type.
 * list holds PprofLabel, sorted by key. It is what a goroutine's labels point
 * at, and never changes once made. */
typedef struct PprLabelMap {
    Slice list;
} PprLabelMap;

/* String returns key, value pairs in a consistent order, as {"k":"v", ...}.
 * Empty for NULL. */
BURROW_OWNS(ret) Str burrow__ppr_label_map_string(const PprLabelMap *l, Alloc *a);

/* lostProfileEvent is the function to which lost profiling events are
 * attributed. (The name shows up in the pprof graphs.) */
void burrow__pprof_lost_profile_event(void);

/* The frame for addr, an address from a stack as runtime_callers gives it.
 * This is what Go's runtime.CallersFrames gives for a slice of just addr: the
 * call's own address, one less than addr, and the function it is in. The one
 * address that is not a return address is the entry of
 * burrow__pprof_lost_profile_event, which Profile.Add uses for a stack with
 * nothing in it, and that is looked up as it is. Returns false, with pc still
 * set and the rest empty, for an address with no name. */
bool burrow__ppr_frame(Uintptr addr, RuntimeFrame *out);

/* The field numbers of profile.proto that the count profiles write. */
enum {
    PPR_TAG_PROFILE_SAMPLE_TYPE = 1,
    PPR_TAG_PROFILE_PERIOD_TYPE = 11,
    PPR_TAG_PROFILE_PERIOD = 12,
    PPR_TAG_SAMPLE_LABEL = 3,
    PPR_TAG_PROFILE_DEFAULT_SAMPLE_TYPE = 14,
};

/* A profileBuilder writes a profile incrementally from a stream of profile
 * samples delivered by the runtime. Everything it holds is its own, and
 * burrow__ppr_build or burrow__ppr_builder_free gives it back. */
typedef struct PprBuilder PprBuilder;

/* newProfileBuilder: a builder writing to w, with the mappings of the running
 * program already read. Panics when memory runs out. */
BURROW_OWNS(ret) PprBuilder *burrow__ppr_new_builder(IoWriter w);

/* addCPUData adds the CPU profiling data to the profile. data is a slice of
 * uint64_t holding a whole number of records, and tags a slice of
 * UnsafePointer, a PprLabelMap or NULL for each record. */
Error burrow__ppr_add_cpu_data(PprBuilder *b, Slice data, Slice tags);

/* build completes the profile and writes it out, then frees b. */
Error burrow__ppr_build(PprBuilder *b);

/* Frees a builder that is not going to be built. NULL is fine. */
void burrow__ppr_builder_free(PprBuilder *b);

/* The pieces of the encoder that printCountProfile uses. pbValueType, a
 * single int64 field written only when it is not zero, appendLocsForStack on
 * a slice of uint64_t taking a slice of Uintptr, and pbSample with its labels
 * written from a label map rather than a closure. values is a slice of
 * int64_t. */
void burrow__ppr_value_type(PprBuilder *b, Int tag, Str typ, Str unit);
void burrow__ppr_int64_opt(PprBuilder *b, Int tag, int64_t x);
BURROW_OWNS(ret) Slice burrow__ppr_append_locs_for_stack(PprBuilder *b, Slice locs,
                                                         Slice stk);
void burrow__ppr_sample(PprBuilder *b, Slice values, Slice locs,
                        const PprLabelMap *labels);

/* writeHeapProto writes the current heap profile in protobuf format to w. p is
 * n records. */
Error burrow__ppr_write_heap_proto(IoWriter w, const burrow__MemProfileRecord *p, Int n,
                                   int64_t rate, Str default_sample_type);

/* scaleHeapSample adjusts the data from a heap Sample to account for its
 * probability of appearing in the collected data. */
void burrow__ppr_scale_heap_sample(int64_t count, int64_t avg_size, int64_t rate,
                                   int64_t *objects, int64_t *bytes);

/* Whether the function named name is one of the allocator's own, which a heap
 * profile leaves out at the start of a stack, as Go's leaves out the runtime's.
 * Not in Go. */
bool burrow__ppr_alloc_frame(Str name);

/* What parseProcSelfMaps and the mapping readers report, one executable
 * mapping at a time. */
typedef void (*PprMappingFn)(void *env, uint64_t lo, uint64_t hi, uint64_t offset,
                             Str file, Str build_id);

/* parseProcSelfMaps: every executable mapping in the text of /proc/self/maps,
 * with the GNU build ID of each file that has one. The names and build IDs
 * come from a. */
void burrow__ppr_parse_proc_self_maps(Alloc *a, Slice data, PprMappingFn fn, void *env);

/* readMainModuleMapping: the mapping of the program itself on macOS and
 * Windows, and the error "not implemented" everywhere else. */
Error burrow__ppr_read_main_module_mapping(Alloc *a, uint64_t *start, uint64_t *end,
                                           Str *exe, Str *build_id);

/* elfBuildID returns the GNU build ID of the named ELF binary, in hex. */
BURROW_OWNS(ret) Str burrow__ppr_elf_build_id(Alloc *a, Str file, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_DEBUG_PPROF_INTERNAL_H */
