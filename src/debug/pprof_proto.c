/* runtime/pprof: the builder that writes a profile as profile.proto, and the
 * mappings of the running program that go in it.
 *
 * Derived from Go's src/runtime/pprof: proto.go, protobuf.go, protomem.go,
 * map.go, elf.go, pe.go, proto_other.go, proto_darwin.go and proto_windows.go.
 * Go source: go1.27.1.
 *
 * burrow has no inlining information and no file or line for a frame, so
 * Go's pcDeck never merges two addresses here and every location is one
 * address with one line. The code below takes the shortcut that allows, and
 * the IDs and the bytes come out the way Go's longer path gives them.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "pprof_internal.h"

#include "burrow/compress/gzip.h"
#include "burrow/core.h"
#include "burrow/encoding/hex.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/math.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/platform.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* lostProfileEvent is the function to which lost profiling events are
 * attributed. (The name shows up in the pprof graphs.)
 *
 * Go's calls itself, which is only there to give it a body of its own. This
 * one is never called either, and the panic does the same job: a linker that
 * folds identical functions together has nothing to fold it with. */
BURROW_NOINLINE void burrow__pprof_lost_profile_event(void) {
    panic_str(BURROW_S("runtime/pprof: lostProfileEvent called"));
}

static BURROW_NORETURN void ppr_oom(void) {
    panic_str(BURROW_S("runtime/pprof: out of memory"));
}

/* symbolizeFlag keeps track of symbolization result.
 *
 *     0                  : no symbol lookup was performed
 *     1<<0 (lookupTried) : symbol lookup was performed
 *     1<<1 (lookupFailed): symbol lookup was performed but failed */
enum {
    PPR_LOOKUP_TRIED = 1 << 0,
    PPR_LOOKUP_FAILED = 1 << 1,
};

enum {
    /* message Profile */
    PPR_TAG_PROFILE_SAMPLE = 2,
    PPR_TAG_PROFILE_MAPPING = 3,
    PPR_TAG_PROFILE_LOCATION = 4,
    PPR_TAG_PROFILE_FUNCTION = 5,
    PPR_TAG_PROFILE_STRING_TABLE = 6,
    PPR_TAG_PROFILE_TIME_NANOS = 9,
    PPR_TAG_PROFILE_DURATION_NANOS = 10,

    /* message ValueType */
    PPR_TAG_VALUE_TYPE_TYPE = 1,
    PPR_TAG_VALUE_TYPE_UNIT = 2,

    /* message Sample */
    PPR_TAG_SAMPLE_LOCATION = 1,
    PPR_TAG_SAMPLE_VALUE = 2,

    /* message Label */
    PPR_TAG_LABEL_KEY = 1,
    PPR_TAG_LABEL_STR = 2,
    PPR_TAG_LABEL_NUM = 3,

    /* message Mapping */
    PPR_TAG_MAPPING_ID = 1,
    PPR_TAG_MAPPING_START = 2,
    PPR_TAG_MAPPING_LIMIT = 3,
    PPR_TAG_MAPPING_OFFSET = 4,
    PPR_TAG_MAPPING_FILENAME = 5,
    PPR_TAG_MAPPING_BUILD_ID = 6,
    PPR_TAG_MAPPING_HAS_FUNCTIONS = 7,

    /* message Location */
    PPR_TAG_LOCATION_ID = 1,
    PPR_TAG_LOCATION_MAPPING_ID = 2,
    PPR_TAG_LOCATION_ADDRESS = 3,
    PPR_TAG_LOCATION_LINE = 4,

    /* message Line */
    PPR_TAG_LINE_FUNCTION_ID = 1,
    PPR_TAG_LINE_LINE = 2,

    /* message Function */
    PPR_TAG_FUNCTION_ID = 1,
    PPR_TAG_FUNCTION_NAME = 2,
    PPR_TAG_FUNCTION_SYSTEM_NAME = 3,
    PPR_TAG_FUNCTION_FILENAME = 4,
    PPR_TAG_FUNCTION_START_LINE = 5,
};

/* ------------------------------------------------------------- the encoder */

/* A protobuf is a simple protocol buffer encoder. */
typedef struct PprProtobuf {
    Alloc *a;
    Byte *data;
    Int len;
    Int cap;
    Byte tmp[16];
    int nest;
} PprProtobuf;

static void ppr_pb_grow(PprProtobuf *b, Int n) {
    if (b->len + n <= b->cap)
        return;
    Int cap = b->cap < 256 ? 256 : b->cap;
    while (cap < b->len + n)
        cap *= 2;
    Byte *p = (Byte *)mem_realloc(b->a, b->data, (size_t)b->cap, (size_t)cap, 1);
    if (p == NULL)
        ppr_oom();
    b->data = p;
    b->cap = cap;
}

static void ppr_pb_varint(PprProtobuf *b, uint64_t x) {
    ppr_pb_grow(b, 10);
    while (x >= 128) {
        b->data[b->len++] = (Byte)(x | 0x80);
        x >>= 7;
    }
    b->data[b->len++] = (Byte)x;
}

static void ppr_pb_length(PprProtobuf *b, Int tag, Int len) {
    ppr_pb_varint(b, (uint64_t)tag << 3 | 2);
    ppr_pb_varint(b, (uint64_t)len);
}

/* Moves the bytes from n2 to the end in front of the bytes from n1 to n2,
 * which is how a length goes in front of what it is the length of. */
static void ppr_pb_rotate(PprProtobuf *b, Int n1, Int n2) {
    Int n3 = b->len;
    memcpy(b->tmp, b->data + n2, (size_t)(n3 - n2));
    memmove(b->data + n1 + (n3 - n2), b->data + n1, (size_t)(n2 - n1));
    memcpy(b->data + n1, b->tmp, (size_t)(n3 - n2));
}

static void ppr_pb_uint64(PprProtobuf *b, Int tag, uint64_t x) {
    /* append varint to b.data */
    ppr_pb_varint(b, (uint64_t)tag << 3 | 0);
    ppr_pb_varint(b, x);
}

static void ppr_pb_uint64s(PprProtobuf *b, Int tag, const uint64_t *x, Int n) {
    if (n > 2) {
        /* Use packed encoding */
        Int n1 = b->len;
        for (Int i = 0; i < n; i++)
            ppr_pb_varint(b, x[i]);
        Int n2 = b->len;
        ppr_pb_length(b, tag, n2 - n1);
        ppr_pb_rotate(b, n1, n2);
        return;
    }
    for (Int i = 0; i < n; i++)
        ppr_pb_uint64(b, tag, x[i]);
}

static void ppr_pb_uint64_opt(PprProtobuf *b, Int tag, uint64_t x) {
    if (x == 0)
        return;
    ppr_pb_uint64(b, tag, x);
}

static void ppr_pb_int64(PprProtobuf *b, Int tag, int64_t x) {
    ppr_pb_uint64(b, tag, (uint64_t)x);
}

static void ppr_pb_int64_opt(PprProtobuf *b, Int tag, int64_t x) {
    if (x == 0)
        return;
    ppr_pb_int64(b, tag, x);
}

static void ppr_pb_int64s(PprProtobuf *b, Int tag, const int64_t *x, Int n) {
    if (n > 2) {
        /* Use packed encoding */
        Int n1 = b->len;
        for (Int i = 0; i < n; i++)
            ppr_pb_varint(b, (uint64_t)x[i]);
        Int n2 = b->len;
        ppr_pb_length(b, tag, n2 - n1);
        ppr_pb_rotate(b, n1, n2);
        return;
    }
    for (Int i = 0; i < n; i++)
        ppr_pb_int64(b, tag, x[i]);
}

static void ppr_pb_string(PprProtobuf *b, Int tag, Str x) {
    ppr_pb_length(b, tag, x.len);
    ppr_pb_grow(b, x.len);
    if (x.len > 0)
        memcpy(b->data + b->len, x.p, (size_t)x.len);
    b->len += x.len;
}

static void ppr_pb_bool(PprProtobuf *b, Int tag, bool x) {
    ppr_pb_uint64(b, tag, x ? 1 : 0);
}

static Int ppr_pb_start_message(PprProtobuf *b) {
    b->nest++;
    return b->len;
}

static void ppr_pb_end_message(PprProtobuf *b, Int tag, Int start) {
    Int n2 = b->len;
    ppr_pb_length(b, tag, n2 - start);
    ppr_pb_rotate(b, start, n2);
    b->nest--;
}

/* ------------------------------------------------------------ the profMap */

/* A profMapEntry is a single entry in the profMap. */
typedef struct PprMapEntry {
    struct PprMapEntry *next_hash; /* next in hash list */
    struct PprMapEntry *next_all;  /* next in list of all entries */
    Uintptr *stk;
    Int nstk;
    void *tag;
    int64_t count;
} PprMapEntry;

/* A profMap is a map from (stack, tag) to mapEntry. It grows without bound,
 * but that's assumed to be OK. */
typedef struct PprProfMap {
    Map *hash; /* of Uintptr to PprMapEntry pointers */
    PprMapEntry *all;
    PprMapEntry *last;
    PprMapEntry *free;
    Int nfree;
    Uintptr *free_stk;
    Int nfree_stk;
} PprProfMap;

static PprMapEntry *ppr_map_lookup(Alloc *a, PprProfMap *m, const uint64_t *stk, Int n,
                                   void *tag) {
    /* Compute hash of (stk, tag). */
    Uintptr h = 0;
    for (Int i = 0; i < n; i++) {
        h = h << 8 | (h >> (8 * (sizeof h - 1)));
        h += (Uintptr)stk[i] * 41;
    }
    h = h << 8 | (h >> (8 * (sizeof h - 1)));
    h += (Uintptr)tag * 41;

    /* Find entry if present. */
    void **head = (void **)map_get(m->hash, &h);
    PprMapEntry *last = NULL;
    for (PprMapEntry *e = head != NULL ? (PprMapEntry *)*head : NULL; e != NULL;
         last = e, e = e->next_hash) {
        if (e->nstk != n || e->tag != tag)
            continue;
        bool same = true;
        for (Int j = 0; j < n; j++) {
            if (e->stk[j] != (Uintptr)stk[j]) {
                same = false;
                break;
            }
        }
        if (!same)
            continue;
        /* Move to front. */
        if (last != NULL) {
            last->next_hash = e->next_hash;
            e->next_hash = (PprMapEntry *)*head;
            *head = e;
        }
        return e;
    }

    /* Add new entry. */
    if (m->nfree < 1) {
        m->free = (PprMapEntry *)mem_alloc_array(a, 128, sizeof(PprMapEntry),
                                                 _Alignof(PprMapEntry));
        if (m->free == NULL)
            ppr_oom();
        m->nfree = 128;
    }
    PprMapEntry *e = m->free++;
    m->nfree--;
    e->next_hash = head != NULL ? (PprMapEntry *)*head : NULL;
    e->tag = tag;

    if (m->nfree_stk < n) {
        /* Go makes 1024 here, which no stack it is given is longer than. */
        Int k = n > 1024 ? n : 1024;
        m->free_stk = (Uintptr *)mem_alloc_array(a, (size_t)k, sizeof(Uintptr),
                                                 _Alignof(Uintptr));
        if (m->free_stk == NULL)
            ppr_oom();
        m->nfree_stk = k;
    }
    e->stk = m->free_stk;
    e->nstk = n;
    if (n > 0) {
        /* An empty stack has nothing to take, and may come before anything
         * has been allocated to take it from. */
        m->free_stk += n;
        m->nfree_stk -= n;
    }

    for (Int j = 0; j < n; j++)
        e->stk[j] = (Uintptr)stk[j];
    void *ev = e;
    if (!map_set(m->hash, &h, &ev))
        ppr_oom();
    if (m->all == NULL) {
        m->all = e;
        m->last = e;
    } else {
        m->last->next_all = e;
        m->last = e;
    }
    return e;
}

/* ------------------------------------------------------------ the builder */

typedef struct PprMemMap {
    /* initialized as reading mapping */
    uint64_t start; /* Address at which the binary (or DLL) is loaded into memory. */
    uint64_t end;   /* The limit of the address range occupied by this mapping. */
    uint64_t
        offset; /* Offset in the binary that corresponds to the first mapped address. */
    Str file;   /* The object this entry is loaded from. */
    Str build_id; /* A string that uniquely identifies a particular program version with
                     high probability. */

    int funcs;
    bool fake; /* map entry was faked; /proc/self/maps wasn't available */
} PprMemMap;

struct PprBuilder {
    Arena arena;
    Alloc *a;
    Time start;
    Time end;
    bool have_period;
    int64_t period;
    PprProfMap m;

    /* encoding state */
    IoWriter w;
    GzipWriter *zw;
    PprProtobuf pb;
    Slice strings;   /* of Str */
    Map *string_map; /* of Str to Int */
    Map *locs;       /* of Uintptr to the uint64_t location ID */
    Map *funcs;      /* of Str to Int, function name to Function.ID */
    PprMemMap *mem;
    Int nmem;
    Int cap_mem;
};

/* stringIndex adds s to the string table if not already present and returns
 * the index of s in the string table. */
static int64_t ppr_string_index(PprBuilder *b, Str s) {
    Int *id = (Int *)map_get(b->string_map, &s);
    if (id != NULL)
        return (int64_t)*id;
    Int n = b->strings.len;
    Slice grown = slice_append(b->a, b->strings, &s, 1);
    if (grown.len != n + 1)
        ppr_oom();
    b->strings = grown;
    if (!map_set(b->string_map, &s, &n))
        ppr_oom();
    return (int64_t)n;
}

static void ppr_flush(PprBuilder *b) {
    enum { DATA_FLUSH = 4096 };
    if (b->pb.nest == 0 && b->pb.len > DATA_FLUSH) {
        Error err;
        gzip_writer_write(
            b->zw, slice_from(b->pb.data, b->pb.len, b->pb.len, TYPE_UINT8), &err);
        b->pb.len = 0;
    }
}

/* pbValueType encodes a ValueType message to b.pb. */
void burrow__ppr_value_type(PprBuilder *b, Int tag, Str typ, Str unit) {
    Int start = ppr_pb_start_message(&b->pb);
    ppr_pb_int64(&b->pb, PPR_TAG_VALUE_TYPE_TYPE, ppr_string_index(b, typ));
    ppr_pb_int64(&b->pb, PPR_TAG_VALUE_TYPE_UNIT, ppr_string_index(b, unit));
    ppr_pb_end_message(&b->pb, tag, start);
}

void burrow__ppr_int64_opt(PprBuilder *b, Int tag, int64_t x) {
    ppr_pb_int64_opt(&b->pb, tag, x);
}

/* pbLabel encodes a Label message to b.pb. */
static void ppr_pb_label(PprBuilder *b, Int tag, Str key, Str str, int64_t num) {
    Int start = ppr_pb_start_message(&b->pb);
    ppr_pb_int64_opt(&b->pb, PPR_TAG_LABEL_KEY, ppr_string_index(b, key));
    ppr_pb_int64_opt(&b->pb, PPR_TAG_LABEL_STR, ppr_string_index(b, str));
    ppr_pb_int64_opt(&b->pb, PPR_TAG_LABEL_NUM, num);
    ppr_pb_end_message(&b->pb, tag, start);
}

/* pbSample encodes a Sample message to b.pb. */
void burrow__ppr_sample(PprBuilder *b, Slice values, Slice locs,
                        const PprLabelMap *labels) {
    Int start = ppr_pb_start_message(&b->pb);
    ppr_pb_int64s(&b->pb, PPR_TAG_SAMPLE_VALUE, (const int64_t *)values.p, values.len);
    ppr_pb_uint64s(&b->pb, PPR_TAG_SAMPLE_LOCATION, (const uint64_t *)locs.p, locs.len);
    if (labels != NULL) {
        const PprofLabel *l = (const PprofLabel *)labels->list.p;
        for (Int i = 0; i < labels->list.len; i++)
            ppr_pb_label(b, PPR_TAG_SAMPLE_LABEL, l[i].key, l[i].value, 0);
    }
    ppr_pb_end_message(&b->pb, PPR_TAG_PROFILE_SAMPLE, start);
    ppr_flush(b);
}

/* pbLine encodes a Line message to b.pb. */
static void ppr_pb_line(PprBuilder *b, Int tag, uint64_t func_id, int64_t line) {
    Int start = ppr_pb_start_message(&b->pb);
    ppr_pb_uint64_opt(&b->pb, PPR_TAG_LINE_FUNCTION_ID, func_id);
    ppr_pb_int64_opt(&b->pb, PPR_TAG_LINE_LINE, line);
    ppr_pb_end_message(&b->pb, tag, start);
}

/* pbMapping encodes a Mapping message to b.pb. */
static void ppr_pb_mapping(PprBuilder *b, Int tag, uint64_t id, const PprMemMap *m,
                           bool has_funcs) {
    Int start = ppr_pb_start_message(&b->pb);
    ppr_pb_uint64_opt(&b->pb, PPR_TAG_MAPPING_ID, id);
    ppr_pb_uint64_opt(&b->pb, PPR_TAG_MAPPING_START, m->start);
    ppr_pb_uint64_opt(&b->pb, PPR_TAG_MAPPING_LIMIT, m->end);
    ppr_pb_uint64_opt(&b->pb, PPR_TAG_MAPPING_OFFSET, m->offset);
    ppr_pb_int64_opt(&b->pb, PPR_TAG_MAPPING_FILENAME, ppr_string_index(b, m->file));
    ppr_pb_int64_opt(&b->pb, PPR_TAG_MAPPING_BUILD_ID,
                     ppr_string_index(b, m->build_id));
    /* TODO: we set HasFunctions if all symbols from samples were symbolized
     * (hasFuncs). Decide what to do about HasInlineFrames and HasLineNumbers.
     * Also, another approach to handle the mapping entry with incomplete
     * symbolization results is to duplicate the mapping entry (but with
     * different Has* fields values) and use different entries for symbolized
     * locations and unsymbolized locations. */
    if (has_funcs)
        ppr_pb_bool(&b->pb, PPR_TAG_MAPPING_HAS_FUNCTIONS, true);
    ppr_pb_end_message(&b->pb, tag, start);
}

bool burrow__ppr_frame(Uintptr addr, RuntimeFrame *out) {
    memset(out, 0, sizeof *out);
    RuntimeFunc fn;
    if (addr == (Uintptr)burrow__pprof_lost_profile_event &&
        runtime_func_for_pc(addr, &fn) && fn.entry == addr) {
        out->pc = addr;
        out->entry = fn.entry;
        out->function = fn.name;
        return true;
    }
    if (addr != 0 && runtime_func_for_pc(addr - 1, &fn)) {
        out->pc = addr - 1;
        out->entry = fn.entry;
        out->function = fn.name;
        return true;
    }
    out->pc = addr - 1;
    return false;
}

/* emitLocation emits the new location and function information for addr and
 * returns the location ID encoded in the profile protobuf. It emits to b.pb,
 * so there must be no message encoding in progress.
 *
 * Go's allFrames comes first. It hands back the frame for addr and whether
 * looking it up worked, which here it never fully does, since there is no file
 * or line to find. */
static uint64_t ppr_emit_location(PprBuilder *b, Uintptr addr) {
    RuntimeFrame frame;
    bool found = burrow__ppr_frame(addr, &frame);
    int symbolize_result = PPR_LOOKUP_TRIED;
    if (!found || frame.function.len == 0 || frame.file.len == 0 || frame.line == 0)
        symbolize_result |= PPR_LOOKUP_FAILED;

    uint64_t id = (uint64_t)map_len(b->locs) + 1;
    if (!map_set(b->locs, &addr, &id))
        ppr_oom();

    /* We can't write out functions while in the middle of the Location
     * message, so record new functions we encounter and write them out after
     * the Location. */
    uint64_t new_func = 0;

    Int start = ppr_pb_start_message(&b->pb);
    ppr_pb_uint64_opt(&b->pb, PPR_TAG_LOCATION_ID, id);
    ppr_pb_uint64_opt(&b->pb, PPR_TAG_LOCATION_ADDRESS, (uint64_t)frame.pc);
    /* Write out each line in frame expansion. */
    Int *fid = (Int *)map_get(b->funcs, &frame.function);
    uint64_t func_id = fid != NULL ? (uint64_t)*fid : 0;
    if (func_id == 0) {
        Int next = map_len(b->funcs) + 1;
        if (!map_set(b->funcs, &frame.function, &next))
            ppr_oom();
        func_id = (uint64_t)next;
        new_func = func_id;
    }
    ppr_pb_line(b, PPR_TAG_LOCATION_LINE, func_id, (int64_t)frame.line);
    for (Int i = 0; i < b->nmem; i++) {
        PprMemMap *m = &b->mem[i];
        if ((m->start <= (uint64_t)addr && (uint64_t)addr < m->end) || m->fake) {
            ppr_pb_uint64_opt(&b->pb, PPR_TAG_LOCATION_MAPPING_ID, (uint64_t)(i + 1));
            m->funcs |= symbolize_result;
            break;
        }
    }
    ppr_pb_end_message(&b->pb, PPR_TAG_PROFILE_LOCATION, start);

    /* Write out functions we found during frame expansion. burrow has no start
     * line to give. */
    if (new_func != 0) {
        start = ppr_pb_start_message(&b->pb);
        ppr_pb_uint64_opt(&b->pb, PPR_TAG_FUNCTION_ID, new_func);
        ppr_pb_int64_opt(&b->pb, PPR_TAG_FUNCTION_NAME,
                         ppr_string_index(b, frame.function));
        ppr_pb_int64_opt(&b->pb, PPR_TAG_FUNCTION_SYSTEM_NAME,
                         ppr_string_index(b, frame.function));
        ppr_pb_int64_opt(&b->pb, PPR_TAG_FUNCTION_FILENAME,
                         ppr_string_index(b, frame.file));
        ppr_pb_end_message(&b->pb, PPR_TAG_PROFILE_FUNCTION, start);
    }

    ppr_flush(b);
    return id;
}

/* appendLocsForStack appends the location IDs for the given stack trace to the
 * given location ID slice, locs. The addresses in the stack are return PCs as
 * the runtime traceback function returns.
 *
 * It may emit to b.pb, so there must be no message encoding in progress.
 *
 * Go holds each address back in its pcDeck until it has seen the next one, in
 * case the next is an inlined call in the same location, and emits the
 * location when it is not. With nothing inlined the next one never is, so the
 * location is emitted straight away, and that is the same order Go emits it
 * in. */
Slice burrow__ppr_append_locs_for_stack(PprBuilder *b, Slice locs, Slice stk) {
    const Uintptr *pcs = (const Uintptr *)stk.p;
    for (Int i = 0; i < stk.len; i++) {
        Uintptr addr = pcs[i];
        uint64_t *cached = (uint64_t *)map_get(b->locs, &addr);
        uint64_t id = cached != NULL ? *cached : ppr_emit_location(b, addr);
        Int n = locs.len;
        locs = slice_append(b->a, locs, &id, 1);
        if (locs.len != n + 1)
            ppr_oom();
    }
    return locs;
}

static void ppr_add_mapping_entry(PprBuilder *b, uint64_t lo, uint64_t hi,
                                  uint64_t offset, Str file, Str build_id, bool fake) {
    if (b->nmem == b->cap_mem) {
        Int cap = b->cap_mem < 8 ? 8 : b->cap_mem * 2;
        PprMemMap *p = (PprMemMap *)mem_realloc(
            b->a, b->mem, (size_t)b->cap_mem * sizeof(PprMemMap),
            (size_t)cap * sizeof(PprMemMap), _Alignof(PprMemMap));
        if (p == NULL)
            ppr_oom();
        b->mem = p;
        b->cap_mem = cap;
    }
    PprMemMap *m = &b->mem[b->nmem++];
    memset(m, 0, sizeof *m);
    m->start = lo;
    m->end = hi;
    m->offset = offset;
    m->file = file;
    m->build_id = build_id;
    m->fake = fake;
}

#if !defined(BURROW_OS_WASI)
static void ppr_add_mapping(void *env, uint64_t lo, uint64_t hi, uint64_t offset,
                            Str file, Str build_id) {
    ppr_add_mapping_entry((PprBuilder *)env, lo, hi, offset, file, build_id, false);
}
#endif

/* ------------------------------------------------------------ the mappings */

BURROW_SENTINEL_ERROR(ppr_err_bad_elf, "malformed ELF binary");
BURROW_SENTINEL_ERROR(ppr_err_no_build_id, "no NT_GNU_BUILD_ID found in ELF binary");

static uint16_t ppr_u16(const Byte *b, bool big) {
    return big ? (uint16_t)(b[0] << 8 | b[1]) : (uint16_t)(b[1] << 8 | b[0]);
}

static uint32_t ppr_u32(const Byte *b, bool big) {
    if (big)
        return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    return (uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0];
}

static uint64_t ppr_u64(const Byte *b, bool big) {
    uint64_t hi = ppr_u32(big ? b : b + 4, big);
    uint64_t lo = ppr_u32(big ? b + 4 : b, big);
    return hi << 32 | lo;
}

static Str ppr_elf_build_id_in(Alloc *a, OsFile *f, Error *err) {
    Byte buf[256];
    Slice whole = slice_from(buf, 256, 256, TYPE_UINT8);

    os_file_read_at(f, slice_sub(whole, 0, 64), 0, err);
    if (BURROW_FAILED(*err))
        return BURROW_STR_EMPTY;

    /* ELF file begins with \x7F E L F. */
    if (buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        *err = ppr_err_bad_elf;
        return BURROW_STR_EMPTY;
    }

    bool big;
    switch (buf[5]) {
    case 1: /* little-endian */
        big = false;
        break;
    case 2: /* big-endian */
        big = true;
        break;
    default:
        *err = ppr_err_bad_elf;
        return BURROW_STR_EMPTY;
    }

    Int shnum;
    int64_t shoff, shentsize;
    switch (buf[4]) {
    case 1: /* 32-bit file header */
        shoff = (int64_t)ppr_u32(buf + 32, big);
        shentsize = (int64_t)ppr_u16(buf + 46, big);
        if (shentsize != 40) {
            *err = ppr_err_bad_elf;
            return BURROW_STR_EMPTY;
        }
        shnum = (Int)ppr_u16(buf + 48, big);
        break;
    case 2: /* 64-bit file header */
        shoff = (int64_t)ppr_u64(buf + 40, big);
        shentsize = (int64_t)ppr_u16(buf + 58, big);
        if (shentsize != 64) {
            *err = ppr_err_bad_elf;
            return BURROW_STR_EMPTY;
        }
        shnum = (Int)ppr_u16(buf + 60, big);
        break;
    default:
        *err = ppr_err_bad_elf;
        return BURROW_STR_EMPTY;
    }

    for (Int i = 0; i < shnum; i++) {
        os_file_read_at(f, slice_sub(whole, 0, (Int)shentsize), shoff + i * shentsize,
                        err);
        if (BURROW_FAILED(*err))
            return BURROW_STR_EMPTY;
        if (ppr_u32(buf + 4, big) != 7) /* SHT_NOTE */
            continue;
        int64_t off, size;
        if (shentsize == 40) {
            /* 32-bit section header */
            off = (int64_t)ppr_u32(buf + 16, big);
            size = (int64_t)ppr_u32(buf + 20, big);
        } else {
            /* 64-bit section header */
            off = (int64_t)ppr_u64(buf + 24, big);
            size = (int64_t)ppr_u64(buf + 32, big);
        }
        size = (int64_t)((uint64_t)size + (uint64_t)off);
        while (off < size) {
            /* room for header + name GNU\x00 */
            os_file_read_at(f, slice_sub(whole, 0, 16), off, err);
            if (BURROW_FAILED(*err))
                return BURROW_STR_EMPTY;
            int64_t name_size = (int64_t)ppr_u32(buf, big);
            int64_t desc_size = (int64_t)ppr_u32(buf + 4, big);
            uint32_t note_type = ppr_u32(buf + 8, big);
            int64_t desc_off = off + (12 + ((name_size + 3) & ~(int64_t)3));
            off = desc_off + ((desc_size + 3) & ~(int64_t)3);
            /* want name GNU\x00 type 3 (NT_GNU_BUILD_ID) */
            if (name_size != 4 || note_type != 3 || buf[12] != 'G' || buf[13] != 'N' ||
                buf[14] != 'U' || buf[15] != '\0')
                continue;
            if (desc_size > (int64_t)sizeof buf) {
                *err = ppr_err_bad_elf;
                return BURROW_STR_EMPTY;
            }
            os_file_read_at(f, slice_sub(whole, 0, (Int)desc_size), desc_off, err);
            if (BURROW_FAILED(*err))
                return BURROW_STR_EMPTY;
            Str id = hex_encode_to_string(a, slice_sub(whole, 0, (Int)desc_size));
            if (desc_size > 0 && id.len == 0)
                ppr_oom();
            return id;
        }
    }
    *err = ppr_err_no_build_id;
    return BURROW_STR_EMPTY;
}

/* elfBuildID returns the GNU build ID of the named ELF binary, without
 * introducing a dependency on debug/elf and its dependencies. */
Str burrow__ppr_elf_build_id(Alloc *a, Str file, Error *err) {
    *err = BURROW_NO_ERROR;
    OsFile *f = os_open(a, file, err);
    if (BURROW_FAILED(*err))
        return BURROW_STR_EMPTY;
    Str id = ppr_elf_build_id_in(a, f, err);
    os_file_close(f);
    os_file_free(f);
    return id;
}

/* bytes.Cut on a Str, with Go's nil kept as a NULL pointer. */
static Str ppr_cut(Str s, Byte sep, Str *after) {
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] == sep) {
            *after = (Str){s.p + i + 1, s.len - i - 1};
            return (Str){s.p, i};
        }
    }
    *after = (Str){NULL, 0};
    return s;
}

/* bytes.TrimLeft(s, " "), which gives nil for what is left empty. */
static Str ppr_trim_spaces(Str s) {
    while (s.len > 0 && s.p[0] == ' ') {
        s.p++;
        s.len--;
    }
    return s.len == 0 ? (Str){NULL, 0} : s;
}

/* next removes and returns the next field in the line. It also removes from
 * line any spaces following the field. */
static Str ppr_next_field(Str *line) {
    Str rest;
    Str f = ppr_cut(*line, ' ', &rest);
    *line = ppr_trim_spaces(rest);
    return f;
}

void burrow__ppr_parse_proc_self_maps(Alloc *a, Slice data, PprMappingFn fn,
                                      void *env) {
    /* $ cat /proc/self/maps
     * 00400000-0040b000 r-xp 00000000 fc:01 787766                             /bin/cat
     * 0060a000-0060b000 r--p 0000a000 fc:01 787766                             /bin/cat
     * 0060b000-0060c000 rw-p 0000b000 fc:01 787766                             /bin/cat
     * 014ab000-014cc000 rw-p 00000000 00:00 0                                  [heap]
     * 7f7d76af8000-7f7d7797c000 r--p 00000000 fc:01 1318064                    /usr/lib/locale/locale-archive
     * 7f7d7797c000-7f7d77b36000 r-xp 00000000 fc:01 1180226                    /lib/x86_64-linux-gnu/libc-2.19.so
     * 7f7d77b36000-7f7d77d36000 ---p 001ba000 fc:01 1180226                    /lib/x86_64-linux-gnu/libc-2.19.so
     * 7f7d77d36000-7f7d77d3a000 r--p 001ba000 fc:01 1180226                    /lib/x86_64-linux-gnu/libc-2.19.so
     * 7f7d77d3a000-7f7d77d3c000 rw-p 001be000 fc:01 1180226                    /lib/x86_64-linux-gnu/libc-2.19.so
     * 7f7d77d3c000-7f7d77d41000 rw-p 00000000 00:00 0
     * 7f7d77d41000-7f7d77d64000 r-xp 00000000 fc:01 1180217                    /lib/x86_64-linux-gnu/ld-2.19.so
     * 7f7d77f3f000-7f7d77f42000 rw-p 00000000 00:00 0
     * 7f7d77f61000-7f7d77f63000 rw-p 00000000 00:00 0
     * 7f7d77f63000-7f7d77f64000 r--p 00022000 fc:01 1180217                    /lib/x86_64-linux-gnu/ld-2.19.so
     * 7f7d77f64000-7f7d77f65000 rw-p 00023000 fc:01 1180217                    /lib/x86_64-linux-gnu/ld-2.19.so
     * 7f7d77f65000-7f7d77f66000 rw-p 00000000 00:00 0
     * 7ffc342a2000-7ffc342c3000 rw-p 00000000 00:00 0                          [stack]
     * 7ffc34343000-7ffc34345000 r-xp 00000000 00:00 0                          [vdso]
     * ffffffffff600000-ffffffffff601000 r-xp 00000000 00:00 0                  [vsyscall] */

    Str rest = {(const Byte *)data.p, data.len};
    while (rest.len > 0) {
        Str line = ppr_cut(rest, '\n', &rest);
        Str addr = ppr_next_field(&line);
        Str hi_str;
        Str lo_str = ppr_cut(addr, '-', &hi_str);
        if (hi_str.p == NULL)
            continue;
        Error err;
        uint64_t lo = strconv_parse_uint(lo_str, 16, 64, &err);
        if (BURROW_FAILED(err))
            continue;
        uint64_t hi = strconv_parse_uint(hi_str, 16, 64, &err);
        if (BURROW_FAILED(err))
            continue;
        Str perm = ppr_next_field(&line);
        if (perm.len < 4 || perm.p[2] != 'x') {
            /* Only interested in executable mappings. */
            continue;
        }
        uint64_t offset = strconv_parse_uint(ppr_next_field(&line), 16, 64, &err);
        if (BURROW_FAILED(err))
            continue;
        ppr_next_field(&line);             /* dev */
        Str inode = ppr_next_field(&line); /* inode */
        if (line.p == NULL)
            continue;
        Str file = line;

        /* Trim deleted file marker. */
        static const char deleted[] = " (deleted)";
        Int deleted_len = (Int)sizeof deleted - 1;
        if (file.len >= deleted_len &&
            memcmp(file.p + file.len - deleted_len, deleted, (size_t)deleted_len) == 0)
            file.len -= deleted_len;

        if (inode.len == 1 && inode.p[0] == '0' && file.len == 0) {
            /* Huge-page text mappings list the initial fragment of mapped but
             * unpopulated memory as being inode 0. Don't report that part. But
             * [vdso] and [vsyscall] are inode 0, so let non-empty file names
             * through. */
            continue;
        }

        /* TODO: pprof's remapMappingIDs makes one adjustment:
         * 1. If there is an /anon_hugepage mapping first and it is consecutive
         * to a next mapping, drop the /anon_hugepage. There's no indication why
         * this is needed. Let's try not doing this and see what breaks. If we
         * do need it, it would go here, before we enter the mappings into b.mem
         * in the first place. */

        Str name = str_clone(a, file);
        if (file.len > 0 && name.len == 0)
            ppr_oom();
        Str build_id = burrow__ppr_elf_build_id(a, name, &err);
        fn(env, lo, hi, offset, name, build_id);
    }
}

#if defined(BURROW_OS_WINDOWS)
/* peBuildID: the file's name followed by its modification time, or the name
 * alone when it cannot be stat'd. */
static Str ppr_pe_build_id(Alloc *a, Str file) {
    Error err;
    OsFileInfo s = os_stat(a, file, &err);
    if (BURROW_FAILED(err))
        return file;
    Str t = time_string(s.vt->mod_time(s.data), a);
    Str id = fmt_sprintf_v(a, "%s%s", file, t);
    if (id.len == 0)
        ppr_oom();
    return id;
}
#endif

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_WINDOWS)
typedef struct PprPalEnv {
    Alloc *a;
    PprMappingFn fn;
    void *env;
} PprPalEnv;

static void ppr_pal_mapping(void *env, uint64_t lo, uint64_t hi, uint64_t offset,
                            const char *file, int64_t file_len) {
    PprPalEnv *e = (PprPalEnv *)env;
    Str name = str_clone(e->a, (Str){(const Byte *)file, (Int)file_len});
    if (file_len > 0 && name.len == 0)
        ppr_oom();
#if defined(BURROW_OS_WINDOWS)
    Str build_id = ppr_pe_build_id(e->a, name);
#else
    /* Note that currently no attempt is made to obtain the buildID
     * information. */
    Str build_id = BURROW_STR_EMPTY;
#endif
    e->fn(e->env, lo, hi, offset, name, build_id);
}
#endif

/* readMapping reads the mappings of the running program into b.mem. macOS has
 * machVMInfo and Windows a module snapshot, both behind pal_text_mappings, and
 * everything else reads /proc/self/maps.
 *
 * Not in Go: wasip1 always gets the fake entry. A wasm module's addresses are
 * offsets into its own memory and tables, and a /proc/self/maps it can open
 * belongs to the host, wasmtime for one, whose mappings say nothing about
 * them. */
static void ppr_read_mapping(PprBuilder *b) {
#if defined(BURROW_OS_WASI)
    ppr_add_mapping_entry(b, 0, 0, 0, BURROW_STR_EMPTY, BURROW_STR_EMPTY, true);
#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_WINDOWS)
    PprPalEnv env = {b->a, ppr_add_mapping, b};
    if (!pal_text_mappings(ppr_pal_mapping, &env)) {
        /* pprof expects a map entry, so fake one, when we haven't added
         * anything yet. */
        ppr_add_mapping_entry(b, 0, 0, 0, BURROW_STR_EMPTY, BURROW_STR_EMPTY, true);
    }
#else
    Error err;
    Slice data = os_read_file(b->a, BURROW_S("/proc/self/maps"), &err);
    burrow__ppr_parse_proc_self_maps(b->a, data, ppr_add_mapping, b);
    if (b->nmem == 0) {
        /* pprof expects a map entry, so fake one. */
        ppr_add_mapping_entry(b, 0, 0, 0, BURROW_STR_EMPTY, BURROW_STR_EMPTY, true);
    }
#endif
}

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_WINDOWS)
typedef struct PprFirst {
    bool first;
    uint64_t start;
    uint64_t end;
    Str exe;
    Str build_id;
} PprFirst;

static void ppr_first_mapping(void *env, uint64_t lo, uint64_t hi, uint64_t offset,
                              Str file, Str build_id) {
    (void)offset;
    PprFirst *f = (PprFirst *)env;
    if (f->first) {
        f->start = lo;
        f->end = hi;
        f->exe = file;
        f->build_id = build_id;
    }
    /* May see multiple text segments if rosetta is used for running the go
     * toolchain itself. */
    f->first = false;
}
#endif

#if defined(BURROW_OS_DARWIN)
BURROW_SENTINEL_ERROR(ppr_err_mach_vm_info, "machVMInfo failed");
#elif defined(BURROW_OS_WINDOWS)
/* Not in Go: Go returns the Windows error, which pal_text_mappings does not
 * pass on. */
BURROW_SENTINEL_ERROR(ppr_err_module_snapshot, "module snapshot failed");
#else
BURROW_SENTINEL_ERROR(ppr_err_not_implemented, "not implemented");
#endif

Error burrow__ppr_read_main_module_mapping(Alloc *a, uint64_t *start, uint64_t *end,
                                           Str *exe, Str *build_id) {
    *start = 0;
    *end = 0;
    *exe = BURROW_STR_EMPTY;
    *build_id = BURROW_STR_EMPTY;
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_WINDOWS)
    Error err = BURROW_NO_ERROR;
#if defined(BURROW_OS_WINDOWS)
    Str self = os_executable(a, &err);
    if (BURROW_FAILED(err))
        return err;
    err = ppr_err_module_snapshot;
#else
    err = ppr_err_mach_vm_info;
#endif
    PprFirst first = {true, 0, 0, BURROW_STR_EMPTY, BURROW_STR_EMPTY};
    PprPalEnv env = {a, ppr_first_mapping, &first};
    if (!pal_text_mappings(ppr_pal_mapping, &env) || first.first)
        return err;
    *start = first.start;
    *end = first.end;
#if defined(BURROW_OS_WINDOWS)
    *exe = self;
    *build_id = ppr_pe_build_id(a, self);
#else
    *exe = first.exe;
    *build_id = first.build_id;
#endif
    return BURROW_NO_ERROR;
#else
    (void)a;
    return ppr_err_not_implemented;
#endif
}

/* ----------------------------------------------------------- the builder */

/* newProfileBuilder returns a new profileBuilder. CPU profiling data obtained
 * from the runtime can be added by calling b.addCPUData, and then the
 * eventual profile can be obtained by calling b.finish. */
PprBuilder *burrow__ppr_new_builder(IoWriter w) {
    PprBuilder *b = (PprBuilder *)mem_alloc(heap_allocator(), sizeof(PprBuilder),
                                            _Alignof(PprBuilder));
    if (b == NULL)
        ppr_oom();
    memset(b, 0, sizeof *b);
    arena_init(&b->arena, heap_allocator(), 0);
    b->a = arena_allocator(&b->arena);
    b->w = w;
    Error err;
    b->zw = gzip_new_writer_level(b->a, w, GZIP_BEST_SPEED, &err);
    b->start = time_now();
    b->pb.a = b->a;
    b->strings = slice_make(b->a, TYPE_STRING, 0, 64);
    b->string_map = map_make(b->a, TYPE_STRING, TYPE_INT, 64);
    b->locs = map_make(b->a, TYPE_UINTPTR, TYPE_UINT64, 0);
    b->funcs = map_make(b->a, TYPE_STRING, TYPE_INT, 0);
    b->m.hash = map_make(b->a, TYPE_UINTPTR, TYPE_UNSAFE_POINTER, 0);
    if (b->zw == NULL || b->strings.p == NULL || b->string_map == NULL ||
        b->locs == NULL || b->funcs == NULL || b->m.hash == NULL) {
        burrow__ppr_builder_free(b);
        ppr_oom();
    }
    ppr_string_index(b, BURROW_STR_EMPTY);
    ppr_read_mapping(b);
    return b;
}

void burrow__ppr_builder_free(PprBuilder *b) {
    if (b == NULL)
        return;
    gzip_writer_free(b->zw);
    arena_free(&b->arena);
    mem_free(heap_allocator(), b, sizeof(PprBuilder), _Alignof(PprBuilder));
}

BURROW_SENTINEL_ERROR(ppr_err_truncated, "truncated profile");
BURROW_SENTINEL_ERROR(ppr_err_malformed, "malformed profile");
BURROW_SENTINEL_ERROR(ppr_err_mismatched, "mismatched profile records and tags");

/* addCPUData adds the CPU profiling data to the profile.
 *
 * The data must be a whole number of records, as delivered by the runtime.
 * len(tags) must be equal to the number of records in data. */
Error burrow__ppr_add_cpu_data(PprBuilder *b, Slice data, Slice tags) {
    const uint64_t *d = (const uint64_t *)data.p;
    Int n = data.len;
    void *const *t = (void *const *)tags.p;
    Int nt = tags.len;
    bool tags_nil = tags.p == NULL;

    if (!b->have_period) {
        /* first record is period */
        if (n < 3)
            return ppr_err_truncated;
        if (d[0] != 3 || d[2] == 0)
            return ppr_err_malformed;
        /* data[2] is sampling rate in Hz. Convert to sampling period in
         * nanoseconds. */
        b->period = 1000000000 / (int64_t)d[2];
        b->have_period = true;
        d += 3;
        n -= 3;
        /* Consume tag slot. Note that there isn't a meaningful tag value for
         * this record. */
        if (nt < 1)
            runtime_slice_bounds_out_of_range(1, nt, tags.cap);
        t++;
        nt--;
    }

    /* Parse CPU samples from the profile.
     * Each sample is 3+n uint64s:
     *     data[0] = 3+n
     *     data[1] = time stamp (ignored)
     *     data[2] = count
     *     data[3:3+n] = stack
     * If the count is 0 and the stack has length 1, that's an overflow record
     * inserted by the runtime to indicate that stack[0] samples were lost.
     * Otherwise the count is usually 1, but in a few special cases like lost
     * non-Go samples there can be larger counts. Because many samples with the
     * same stack arrive, we want to deduplicate immediately, which we do using
     * the b.m profMap. */
    while (n > 0) {
        if (n < 3 || d[0] > (uint64_t)n)
            return ppr_err_truncated;
        if (d[0] < 3 || (!tags_nil && nt < 1))
            return ppr_err_malformed;
        if (nt < 1 || t == NULL)
            return ppr_err_mismatched;
        uint64_t count = d[2];
        const uint64_t *stk = d + 3;
        Int nstk = (Int)d[0] - 3;
        Int rec = (Int)d[0];
        d += rec;
        n -= rec;
        void *tag = t[0];
        t++;
        nt--;

        uint64_t lost[1];
        if (count == 0 && nstk == 1) {
            /* overflow record */
            count = stk[0];
            /* gentraceback guarantees that PCs in the stack can be
             * unconditionally decremented and still be valid, so we must do
             * the same. */
            lost[0] = (uint64_t)(Uintptr)burrow__pprof_lost_profile_event + 1;
            stk = lost;
        }
        ppr_map_lookup(b->a, &b->m, stk, nstk, tag)->count += (int64_t)count;
    }

    if (nt != 0)
        return ppr_err_mismatched;
    return BURROW_NO_ERROR;
}

/* build completes and returns the constructed profile. */
Error burrow__ppr_build(PprBuilder *b) {
    b->end = time_now();

    ppr_pb_int64_opt(&b->pb, PPR_TAG_PROFILE_TIME_NANOS, time_unix_nano(b->start));
    if (b->have_period) { /* must be CPU profile */
        burrow__ppr_value_type(b, PPR_TAG_PROFILE_SAMPLE_TYPE, BURROW_S("samples"),
                               BURROW_S("count"));
        burrow__ppr_value_type(b, PPR_TAG_PROFILE_SAMPLE_TYPE, BURROW_S("cpu"),
                               BURROW_S("nanoseconds"));
        ppr_pb_int64_opt(&b->pb, PPR_TAG_PROFILE_DURATION_NANOS,
                         (int64_t)time_sub(b->end, b->start));
        burrow__ppr_value_type(b, PPR_TAG_PROFILE_PERIOD_TYPE, BURROW_S("cpu"),
                               BURROW_S("nanoseconds"));
        ppr_pb_int64_opt(&b->pb, PPR_TAG_PROFILE_PERIOD, b->period);
    }

    int64_t values[2] = {0, 0};
    Slice locs = slice_make(b->a, TYPE_UINT64, 0, 0);

    for (PprMapEntry *e = b->m.all; e != NULL; e = e->next_all) {
        values[0] = e->count;
        values[1] = e->count * b->period;

        locs = burrow__ppr_append_locs_for_stack(
            b, slice_sub(locs, 0, 0),
            slice_from(e->stk, e->nstk, e->nstk, TYPE_UINTPTR));

        burrow__ppr_sample(b, slice_from(values, 2, 2, TYPE_INT64), locs,
                           (const PprLabelMap *)e->tag);
    }

    for (Int i = 0; i < b->nmem; i++) {
        /* lookupTried but not lookupFailed */
        bool has_functions = b->mem[i].funcs == PPR_LOOKUP_TRIED;
        ppr_pb_mapping(b, PPR_TAG_PROFILE_MAPPING, (uint64_t)(i + 1), &b->mem[i],
                       has_functions);
    }

    /* TODO: Anything for tagProfile_DropFrames?
     * TODO: Anything for tagProfile_KeepFrames? */

    const Str *s = (const Str *)b->strings.p;
    for (Int i = 0; i < b->strings.len; i++)
        ppr_pb_string(&b->pb, PPR_TAG_PROFILE_STRING_TABLE, s[i]);
    Error err;
    gzip_writer_write(b->zw, slice_from(b->pb.data, b->pb.len, b->pb.len, TYPE_UINT8),
                      &err);
    if (BURROW_OK(err))
        err = gzip_writer_close(b->zw);
    burrow__ppr_builder_free(b);
    return err;
}

/* ---------------------------------------------------------------- protomem */

bool burrow__ppr_alloc_frame(Str name) {
    /* Go's runtime functions, which is what Go hides. */
    if (strings_has_prefix(name, BURROW_S("runtime.")) ||
        strings_has_prefix(name, BURROW_S("internal/runtime/")))
        return true;
    /* Not in Go: burrow's allocator, which is where a sampled stack starts.
     * See burrow/mem/heap.h for why the functions in heap.c are global. */
    static const Str alloc[] = {
        BURROW_S_INIT("burrow__heap_sample"),
        BURROW_S_INIT("burrow__heap_note"),
        BURROW_S_INIT("burrow__heap_alloc"),
        BURROW_S_INIT("burrow__heap_alloc_zeroed"),
        BURROW_S_INIT("burrow__heap_realloc"),
        BURROW_S_INIT("mem_alloc"),
        BURROW_S_INIT("mem_alloc_nozero"),
        BURROW_S_INIT("mem_alloc_array"),
        BURROW_S_INIT("mem_realloc"),
    };
    for (size_t i = 0; i < sizeof alloc / sizeof alloc[0]; i++)
        if (str_eq(name, alloc[i]))
            return true;
    return false;
}

/* writeHeapProto writes the current heap profile in protobuf format to w. */
Error burrow__ppr_write_heap_proto(IoWriter w, const burrow__MemProfileRecord *p, Int n,
                                   int64_t rate, Str default_sample_type) {
    PprBuilder *b = burrow__ppr_new_builder(w);
    burrow__ppr_value_type(b, PPR_TAG_PROFILE_PERIOD_TYPE, BURROW_S("space"),
                           BURROW_S("bytes"));
    ppr_pb_int64_opt(&b->pb, PPR_TAG_PROFILE_PERIOD, rate);
    burrow__ppr_value_type(b, PPR_TAG_PROFILE_SAMPLE_TYPE, BURROW_S("alloc_objects"),
                           BURROW_S("count"));
    burrow__ppr_value_type(b, PPR_TAG_PROFILE_SAMPLE_TYPE, BURROW_S("alloc_space"),
                           BURROW_S("bytes"));
    burrow__ppr_value_type(b, PPR_TAG_PROFILE_SAMPLE_TYPE, BURROW_S("inuse_objects"),
                           BURROW_S("count"));
    burrow__ppr_value_type(b, PPR_TAG_PROFILE_SAMPLE_TYPE, BURROW_S("inuse_space"),
                           BURROW_S("bytes"));
    if (default_sample_type.len != 0)
        ppr_pb_int64_opt(&b->pb, PPR_TAG_PROFILE_DEFAULT_SAMPLE_TYPE,
                         ppr_string_index(b, default_sample_type));

    int64_t values[4] = {0, 0, 0, 0};
    Slice locs = slice_nil(TYPE_UINT64);
    for (Int ri = 0; ri < n; ri++) {
        const burrow__MemProfileRecord *r = &p[ri];
        bool hide_runtime = true;
        for (int tries = 0; tries < 2; tries++) {
            const Uintptr *stk = r->stack;
            Int nstk = r->nstack;
            /* For heap profiles, all stack addresses are return PCs, which is
             * what appendLocsForStack expects. */
            if (hide_runtime) {
                for (Int i = 0; i < nstk; i++) {
                    RuntimeFunc f;
                    if (runtime_func_for_pc(stk[i], &f) &&
                        burrow__ppr_alloc_frame(f.name))
                        continue;
                    /* Found non-runtime. Show any runtime uses above it. */
                    stk += i;
                    nstk -= i;
                    break;
                }
            }
            locs = burrow__ppr_append_locs_for_stack(
                b, slice_sub(locs, 0, 0),
                slice_from((void *)(uintptr_t)stk, nstk, nstk, TYPE_UINTPTR));
            if (locs.len > 0)
                break;
            hide_runtime = false; /* try again, and show all frames next time. */
        }

        burrow__ppr_scale_heap_sample(r->alloc_objects, r->object_size, rate,
                                      &values[0], &values[1]);
        burrow__ppr_scale_heap_sample(r->alloc_objects - r->free_objects,
                                      r->object_size, rate, &values[2], &values[3]);
        Int start = ppr_pb_start_message(&b->pb);
        ppr_pb_int64s(&b->pb, PPR_TAG_SAMPLE_VALUE, values, 4);
        ppr_pb_uint64s(&b->pb, PPR_TAG_SAMPLE_LOCATION, (const uint64_t *)locs.p,
                       locs.len);
        ppr_pb_label(b, PPR_TAG_SAMPLE_LABEL, BURROW_S("bytes"), BURROW_S(""),
                     r->object_size);
        ppr_pb_end_message(&b->pb, PPR_TAG_PROFILE_SAMPLE, start);
        ppr_flush(b);
    }
    return burrow__ppr_build(b);
}

/* scaleHeapSample adjusts the data from a heap Sample to account for its
 * probability of appearing in the collected data. heap profiles are a sampling
 * of the memory allocations requests in a program. We estimate the unsampled
 * value by dividing each collected sample by its probability of appearing in
 * the profile. heap profiles rely on a poisson process to determine which
 * samples to collect, based on the desired average collection rate R. The
 * probability of a sample of size S to appear in that profile is
 * 1-exp(-S/R). */
void burrow__ppr_scale_heap_sample(int64_t count, int64_t avg_size, int64_t rate,
                                   int64_t *objects, int64_t *bytes) {
    if (count == 0 || avg_size == 0) {
        *objects = 0;
        *bytes = 0;
        return;
    }

    if (rate <= 1) {
        /* if rate==1 all samples were collected so no adjustment is needed. if
         * rate<1 treat as unknown and skip scaling. */
        *objects = count;
        *bytes = count * avg_size;
        return;
    }

    double scale = 1 / (1 - math_exp(-(double)avg_size / (double)rate));

    *objects = (int64_t)((double)count * scale);
    *bytes = (int64_t)((double)(count * avg_size) * scale);
}
