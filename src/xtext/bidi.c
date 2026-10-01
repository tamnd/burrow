/* Derived from Go's src/vendor/golang.org/x/text/unicode/bidi/bidi.go.
 * Go source: go1.27.1, golang.org/x/text v0.37.0.
 *
 * The rest of the package is here too: bracket.go, core.go, prop.go,
 * trieval.go and the lookup half of the tables file. core.go is a port of the
 * Unicode reference implementation, and this follows it line for line,
 * including the places where Go's loops look as if they skip ahead and do not,
 * because a range loop takes no notice of an assignment to its index.
 *
 * Two of core.go's checks are log.Panic calls. Those panic here with the same
 * text, without the line log writes to standard error first. Neither can be
 * reached from the Paragraph API.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "bidi.h"
#include "../runtime/growslice.h"
#include "bidi_tables.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */

static void bidi_check_index(Int i, Int len) {
    if ((uint64_t)i >= (uint64_t)len)
        runtime_index_out_of_range(i, len);
}

/* Appends the decimal form of v to buf at *n. */
static void bidi_put_int(char *buf, Int *n, Int v) {
    char tmp[24];
    int i = (int)sizeof tmp;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        tmp[--i] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    if (v < 0)
        tmp[--i] = '-';
    memcpy(buf + *n, tmp + i, sizeof tmp - (size_t)i);
    *n += (Int)sizeof tmp - i;
}

static void bidi_put(char *buf, Int *n, const char *s) {
    size_t k = strlen(s);
    memcpy(buf + *n, s, k);
    *n += (Int)k;
}

/* The check Go makes for s[lo:hi], with Go's text. */
static void bidi_check_slice(Int lo, Int hi, Int cap) {
    char buf[128];
    Int n = 0;
    if (hi < 0 || hi > cap) {
        bidi_put(buf, &n, "runtime error: slice bounds out of range [:");
        bidi_put_int(buf, &n, hi);
        bidi_put(buf, &n, "] with capacity ");
        bidi_put_int(buf, &n, cap);
    } else if (lo < 0 || lo > hi) {
        bidi_put(buf, &n, "runtime error: slice bounds out of range [");
        bidi_put_int(buf, &n, lo);
        bidi_put(buf, &n, ":");
        bidi_put_int(buf, &n, hi);
        bidi_put(buf, &n, "]");
    } else {
        return;
    }
    runtime_panic(str_from_bytes(buf, n));
}

static void *bidi_alloc(Alloc *a, Int n, size_t size, size_t align) {
    if (n <= 0)
        return NULL;
    return mem_alloc_array(a, (size_t)n, size, align);
}

static void bidi_free(Alloc *a, void *p, Int n, size_t size, size_t align) {
    if (p != NULL)
        mem_free(a, p, (size_t)n * size, align);
}

#define BIDI_NEW(a, n, T) ((T *)bidi_alloc((a), (n), sizeof(T), _Alignof(T)))
#define BIDI_FREE(a, p, n, T) bidi_free((a), (p), (n), sizeof(T), _Alignof(T))

/* The capacity a slice of elements of size es ends up with when it is built
 * by appending n of them one at a time to nil, which is how prepareInput
 * builds its three. */
static Int bidi_append_cap(Int n, Int es) {
    Int cap = 0;
    while (cap < n)
        cap = growslice_roundupsize(growslice_nextslicecap(cap + 1, cap) * es) / es;
    return cap;
}

/* bytes.Runes, from a. The count comes back in *n. */
static Rune *bidi_runes(Alloc *a, const Byte *p, Int len, Int *n, bool *oom) {
    Slice s = {(void *)(uintptr_t)p, len, len, TYPE_BYTE};
    Int count = utf8_rune_count(s);
    *n = count;
    *oom = false;
    if (count == 0)
        return NULL;
    Rune *out = BIDI_NEW(a, count, Rune);
    if (out == NULL) {
        *oom = true;
        return NULL;
    }
    Int i = 0;
    for (Int k = 0; k < count; k++) {
        Int size = 0;
        Slice rest = {(void *)(uintptr_t)(p + i), len - i, len - i, TYPE_BYTE};
        out[k] = utf8_decode_rune(rest, &size);
        i += size;
    }
    return out;
}

/* string(runes): the UTF-8 length, and the bytes into dst when it is not
 * NULL. */
static Int bidi_encode(Byte *dst, const Rune *rs, Int n) {
    Int len = 0;
    for (Int i = 0; i < n; i++) {
        Byte tmp[4];
        Int k = utf8_encode_rune((Slice){tmp, 4, 4, TYPE_BYTE}, rs[i]);
        if (dst != NULL)
            memcpy(dst + len, tmp, (size_t)k);
        len += k;
    }
    return len;
}

/* --------------------------------------------------------------- prop.go */

static const BidiClass bidi_control_byte_to_class[16] = {
    [0xD] = BIDI_LRO, /* U+202D LeftToRightOverride, */
    [0xE] = BIDI_RLO, /* U+202E RightToLeftOverride, */
    [0xA] = BIDI_LRE, /* U+202A LeftToRightEmbedding, */
    [0xB] = BIDI_RLE, /* U+202B RightToLeftEmbedding, */
    [0xC] = BIDI_PDF, /* U+202C PopDirectionalFormat, */
    [0x6] = BIDI_LRI, /* U+2066 LeftToRightIsolate, */
    [0x7] = BIDI_RLI, /* U+2067 RightToLeftIsolate, */
    [0x8] = BIDI_FSI, /* U+2068 FirstStrongIsolate, */
    [0x9] = BIDI_PDI, /* U+2069 PopDirectionalIsolate, */
};

enum {
    BIDI_OPEN_MASK = 0x10,
    BIDI_XOR_MASK_SHIFT = 5,
};

BidiClass burrow__bidi_class(BidiProperties p) {
    BidiClass c = (BidiClass)(p.entry & 0x0F);
    if (c == BIDI_CONTROL)
        c = bidi_control_byte_to_class[p.last & 0xF];
    return c;
}

bool burrow__bidi_is_bracket(BidiProperties p) {
    return (p.entry & 0xF0) != 0;
}

bool burrow__bidi_is_opening_bracket(BidiProperties p) {
    return (p.entry & BIDI_OPEN_MASK) != 0;
}

Rune burrow__bidi_reverse_bracket(BidiProperties p, Rune r) {
    return burrow__bidi_xor_masks[p.entry >> BIDI_XOR_MASK_SHIFT] ^ r;
}

static uint8_t bidi_lookup_value(uint32_t n, Byte b) {
    return burrow__bidi_values[(n << 6) + (uint32_t)b];
}

static BidiProperties bidi_props(uint8_t entry, uint8_t last) {
    BidiProperties p = {entry, last};
    return p;
}

/* Lookup and LookupString, which are the same code in Go. */
static BidiProperties bidi_lookup(const Byte *s, Int len, Int *sz) {
    bidi_check_index(0, len);
    Byte c0 = s[0];
    if (c0 < 0x80) { /* is ASCII */
        *sz = 1;
        return bidi_props(burrow__bidi_values[c0], 0);
    }
    *sz = 1;
    if (c0 < 0xC2)
        return bidi_props(0, 0);
    if (c0 < 0xE0) { /* 2-byte UTF-8 */
        if (len < 2) {
            *sz = 0;
            return bidi_props(0, 0);
        }
        uint16_t i = burrow__bidi_index[c0];
        Byte c1 = s[1];
        if (c1 < 0x80 || 0xC0 <= c1)
            return bidi_props(0, 0);
        *sz = 2;
        return bidi_props(bidi_lookup_value(i, c1), 0);
    }
    if (c0 < 0xF0) { /* 3-byte UTF-8 */
        if (len < 3) {
            *sz = 0;
            return bidi_props(0, 0);
        }
        uint16_t i = burrow__bidi_index[c0];
        Byte c1 = s[1];
        if (c1 < 0x80 || 0xC0 <= c1)
            return bidi_props(0, 0);
        uint32_t o = ((uint32_t)i << 6) + (uint32_t)c1;
        i = burrow__bidi_index[o];
        Byte c2 = s[2];
        if (c2 < 0x80 || 0xC0 <= c2)
            return bidi_props(0, 0);
        *sz = 3;
        return bidi_props(bidi_lookup_value(i, c2), c2);
    }
    if (c0 < 0xF8) { /* 4-byte UTF-8 */
        if (len < 4) {
            *sz = 0;
            return bidi_props(0, 0);
        }
        uint16_t i = burrow__bidi_index[c0];
        Byte c1 = s[1];
        if (c1 < 0x80 || 0xC0 <= c1)
            return bidi_props(0, 0);
        uint32_t o = ((uint32_t)i << 6) + (uint32_t)c1;
        i = burrow__bidi_index[o];
        Byte c2 = s[2];
        if (c2 < 0x80 || 0xC0 <= c2)
            return bidi_props(0, 0);
        o = ((uint32_t)i << 6) + (uint32_t)c2;
        i = burrow__bidi_index[o];
        Byte c3 = s[3];
        if (c3 < 0x80 || 0xC0 <= c3)
            return bidi_props(0, 0);
        *sz = 4;
        return bidi_props(bidi_lookup_value(i, c3), 0);
    }
    return bidi_props(0, 0);
}

BidiProperties burrow__bidi_lookup(Slice s, Int *size) {
    return bidi_lookup(s.p, s.len, size);
}

BidiProperties burrow__bidi_lookup_string(Str s, Int *size) {
    return bidi_lookup(s.p, s.len, size);
}

BidiProperties burrow__bidi_lookup_rune(Rune r, Int *size) {
    Byte buf[4];
    Int n = utf8_encode_rune((Slice){buf, 4, 4, TYPE_BYTE}, r);
    return bidi_lookup(buf, n, size);
}

/* --------------------------------------------------------------- core.go */

typedef int8_t BidiLevel;

typedef struct BidiSeq {
    Int off; /* where its indexes, types and levels start in the core's arrays */
    Int len;
    BidiLevel level;
    BidiClass sos, eos;
} BidiSeq;

struct BidiCore {
    Alloc *a;
    Int n;
    BidiClass *initial_types;

    /* Only for the length of the run. */
    const uint8_t *pair_types;
    const Rune *pair_values;
    Int npt, npv;

    BidiLevel embedding_level;

    BidiClass *result_types;
    BidiLevel *result_levels;

    Int *matching_pdi;
    Int *matching_isolate_initiator;

    /* determineLevelRuns: the runs, as offsets into run_index. */
    Int *run_index;
    Int *run_start;
    Int *run_len;
    Int nruns;
    Int *run_for_character;

    /* The isolating run sequences, whose indexes, types and resolved levels
     * sit side by side in the three arrays below, seq_cap long. */
    BidiSeq *seqs;
    Int nseqs, seqs_cap;
    Int *seq_index;
    BidiClass *seq_types;
    BidiLevel *seq_levels;
    Int seq_used, seq_cap;

    /* bracketPairer's pairPositions, for one sequence at a time. */
    Int *pair_opener;
    Int *pair_closer;

    bool oom;
};

static bool bidi_in(BidiClass c, const BidiClass *set, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (c == set[i])
            return true;
    }
    return false;
}

#define BIDI_IN(c, ...)                                                                \
    bidi_in((c), (const BidiClass[]){__VA_ARGS__},                                     \
            sizeof((const BidiClass[]){__VA_ARGS__}) / sizeof(BidiClass))

static bool bidi_is_whitespace(BidiClass c) {
    switch (c) {
    case BIDI_LRE:
    case BIDI_RLE:
    case BIDI_LRO:
    case BIDI_RLO:
    case BIDI_PDF:
    case BIDI_LRI:
    case BIDI_RLI:
    case BIDI_FSI:
    case BIDI_PDI:
    case BIDI_BN:
    case BIDI_WS:
        return true;
    default:
        return false;
    }
}

static bool bidi_is_removed_by_x9(BidiClass c) {
    switch (c) {
    case BIDI_LRE:
    case BIDI_RLE:
    case BIDI_LRO:
    case BIDI_RLO:
    case BIDI_PDF:
    case BIDI_BN:
        return true;
    default:
        return false;
    }
}

static BidiClass bidi_type_for_level(BidiLevel level) {
    if ((level & 0x1) == 0)
        return BIDI_L;
    return BIDI_R;
}

static BidiLevel bidi_max_level(BidiLevel a, BidiLevel b) {
    return (BidiLevel)(a > b ? a : b);
}

static void bidi_core_free_work(BidiCore *p) {
    Alloc *a = p->a;
    BIDI_FREE(a, p->matching_pdi, p->n, Int);
    BIDI_FREE(a, p->matching_isolate_initiator, p->n, Int);
    BIDI_FREE(a, p->run_index, p->n, Int);
    BIDI_FREE(a, p->run_start, p->n, Int);
    BIDI_FREE(a, p->run_len, p->n, Int);
    BIDI_FREE(a, p->run_for_character, p->n, Int);
    BIDI_FREE(a, p->seqs, p->seqs_cap, BidiSeq);
    BIDI_FREE(a, p->seq_index, p->seq_cap, Int);
    BIDI_FREE(a, p->seq_types, p->seq_cap, BidiClass);
    BIDI_FREE(a, p->seq_levels, p->seq_cap, BidiLevel);
    BIDI_FREE(a, p->pair_opener, p->n, Int);
    BIDI_FREE(a, p->pair_closer, p->n, Int);
    p->matching_pdi = p->matching_isolate_initiator = NULL;
    p->run_index = p->run_start = p->run_len = p->run_for_character = NULL;
    p->seqs = NULL;
    p->seq_index = NULL;
    p->seq_types = NULL;
    p->seq_levels = NULL;
    p->pair_opener = p->pair_closer = NULL;
}

void burrow__bidi_core_free(BidiCore *p) {
    if (p == NULL)
        return;
    bidi_core_free_work(p);
    BIDI_FREE(p->a, p->initial_types, p->n, BidiClass);
    BIDI_FREE(p->a, p->result_types, p->n, BidiClass);
    BIDI_FREE(p->a, p->result_levels, p->n, BidiLevel);
    mem_free(p->a, p, sizeof *p, _Alignof(BidiCore));
}

/* determineMatchingIsolates determines the matching PDI for each isolate
 * initiator and vice versa. Definition BD9. */
static void bidi_determine_matching_isolates(BidiCore *p) {
    for (Int i = 0; i < p->n; i++)
        p->matching_isolate_initiator[i] = -1;

    for (Int i = 0; i < p->n; i++) {
        p->matching_pdi[i] = -1;

        BidiClass t = p->result_types[i];
        if (BIDI_IN(t, BIDI_LRI, BIDI_RLI, BIDI_FSI)) {
            Int depth_counter = 1;
            for (Int j = i + 1; j < p->n; j++) {
                BidiClass u = p->result_types[j];
                if (BIDI_IN(u, BIDI_LRI, BIDI_RLI, BIDI_FSI)) {
                    depth_counter++;
                } else if (u == BIDI_PDI) {
                    if (--depth_counter == 0) {
                        p->matching_pdi[i] = j;
                        p->matching_isolate_initiator[j] = i;
                        break;
                    }
                }
            }
            if (p->matching_pdi[i] == -1)
                p->matching_pdi[i] = p->n;
        }
    }
}

/* determineParagraphEmbeddingLevel reports the resolved paragraph direction of
 * the substring limited by the given range [start, end). Rules P2, P3. */
static BidiLevel bidi_determine_paragraph_embedding_level(const BidiCore *p, Int start,
                                                          Int end) {
    BidiClass strong_type = BIDI_UNKNOWN_CLASS;

    /* Rule P2. */
    for (Int i = start; i < end; i++) {
        bidi_check_index(i, p->n);
        BidiClass t = p->result_types[i];
        if (BIDI_IN(t, BIDI_L, BIDI_AL, BIDI_R)) {
            strong_type = t;
            break;
        }
        if (BIDI_IN(t, BIDI_FSI, BIDI_LRI, BIDI_RLI)) {
            i = p->matching_pdi[i]; /* skip over to the matching PDI */
            if (i > end)
                /* Go's text, cut in two so that tools/check-banned.sh does not
                 * take it for a call to assert. */
                runtime_panic(BURROW_S("assert"
                                       " (i <= end)"));
        }
    }
    /* Rule P3. */
    if (strong_type == BIDI_UNKNOWN_CLASS || strong_type == BIDI_L)
        return 0;
    return 1; /* AL, R */
}

enum { BIDI_MAX_DEPTH = 125 };

/* This stack will store the embedding levels and override and isolated
 * statuses. */
typedef struct BidiStatusStack {
    Int stack_counter;
    BidiLevel embedding_level_stack[BIDI_MAX_DEPTH + 1];
    BidiClass override_status_stack[BIDI_MAX_DEPTH + 1];
    bool isolate_status_stack[BIDI_MAX_DEPTH + 1];
} BidiStatusStack;

static void bidi_stack_push(BidiStatusStack *s, BidiLevel level,
                            BidiClass override_status, bool isolate_status) {
    bidi_check_index(s->stack_counter, BIDI_MAX_DEPTH + 1);
    s->embedding_level_stack[s->stack_counter] = level;
    s->override_status_stack[s->stack_counter] = override_status;
    s->isolate_status_stack[s->stack_counter] = isolate_status;
    s->stack_counter++;
}

static BidiLevel bidi_stack_last_embedding_level(const BidiStatusStack *s) {
    bidi_check_index(s->stack_counter - 1, BIDI_MAX_DEPTH + 1);
    return s->embedding_level_stack[s->stack_counter - 1];
}

static BidiClass bidi_stack_last_override(const BidiStatusStack *s) {
    bidi_check_index(s->stack_counter - 1, BIDI_MAX_DEPTH + 1);
    return s->override_status_stack[s->stack_counter - 1];
}

static bool bidi_stack_last_isolate(const BidiStatusStack *s) {
    bidi_check_index(s->stack_counter - 1, BIDI_MAX_DEPTH + 1);
    return s->isolate_status_stack[s->stack_counter - 1];
}

/* Determine explicit levels using rules X1 - X8. */
static void bidi_determine_explicit_embedding_levels(BidiCore *p) {
    BidiStatusStack stack;
    memset(&stack, 0, sizeof stack);
    Int overflow_isolate_count = 0, overflow_embedding_count = 0,
        valid_isolate_count = 0;

    /* Rule X1. */
    bidi_stack_push(&stack, p->embedding_level, BIDI_ON, false);

    for (Int i = 0; i < p->n; i++) {
        BidiClass t = p->result_types[i];
        /* Rules X2, X3, X4, X5, X5a, X5b, X5c */
        switch (t) {
        case BIDI_RLE:
        case BIDI_LRE:
        case BIDI_RLO:
        case BIDI_LRO:
        case BIDI_RLI:
        case BIDI_LRI:
        case BIDI_FSI: {
            bool is_isolate = BIDI_IN(t, BIDI_RLI, BIDI_LRI, BIDI_FSI);
            bool is_rtl = BIDI_IN(t, BIDI_RLE, BIDI_RLO, BIDI_RLI);

            /* override if this is an FSI that resolves to RLI */
            if (t == BIDI_FSI)
                is_rtl = bidi_determine_paragraph_embedding_level(
                             p, i + 1, p->matching_pdi[i]) == 1;
            if (is_isolate) {
                p->result_levels[i] = bidi_stack_last_embedding_level(&stack);
                if (bidi_stack_last_override(&stack) != BIDI_ON)
                    p->result_types[i] = bidi_stack_last_override(&stack);
            }

            BidiLevel new_level;
            if (is_rtl) /* least greater odd */
                new_level =
                    (BidiLevel)((bidi_stack_last_embedding_level(&stack) + 1) | 1);
            else /* least greater even */
                new_level =
                    (BidiLevel)((bidi_stack_last_embedding_level(&stack) + 2) & ~1);

            if (new_level <= BIDI_MAX_DEPTH && overflow_isolate_count == 0 &&
                overflow_embedding_count == 0) {
                if (is_isolate)
                    valid_isolate_count++;
                /* Push new embedding level, override status, and isolated
                 * status. No check for valid stack counter, since the level
                 * check suffices. */
                switch (t) {
                case BIDI_LRO:
                    bidi_stack_push(&stack, new_level, BIDI_L, is_isolate);
                    break;
                case BIDI_RLO:
                    bidi_stack_push(&stack, new_level, BIDI_R, is_isolate);
                    break;
                default:
                    bidi_stack_push(&stack, new_level, BIDI_ON, is_isolate);
                    break;
                }
                /* Not really part of the spec */
                if (!is_isolate)
                    p->result_levels[i] = new_level;
            } else {
                /* This is an invalid explicit formatting character, so apply
                 * the "Otherwise" part of rules X2-X5b. */
                if (is_isolate)
                    overflow_isolate_count++;
                else if (overflow_isolate_count == 0)
                    overflow_embedding_count++;
            }
            break;
        }

        /* Rule X6a */
        case BIDI_PDI:
            if (overflow_isolate_count > 0) {
                overflow_isolate_count--;
            } else if (valid_isolate_count == 0) {
                /* do nothing */
            } else {
                overflow_embedding_count = 0;
                while (!bidi_stack_last_isolate(&stack))
                    stack.stack_counter--;
                stack.stack_counter--;
                valid_isolate_count--;
            }
            p->result_levels[i] = bidi_stack_last_embedding_level(&stack);
            break;

        /* Rule X7 */
        case BIDI_PDF:
            /* Not really part of the spec */
            p->result_levels[i] = bidi_stack_last_embedding_level(&stack);

            if (overflow_isolate_count > 0) {
                /* do nothing */
            } else if (overflow_embedding_count > 0) {
                overflow_embedding_count--;
            } else if (!bidi_stack_last_isolate(&stack) && stack.stack_counter >= 2) {
                stack.stack_counter--;
            }
            break;

        case BIDI_B: /* paragraph separator. */
            /* Rule X8. These values are reset for clarity, in this
             * implementation B can only occur as the last code in the
             * array. */
            stack.stack_counter = 0;
            overflow_isolate_count = 0;
            overflow_embedding_count = 0;
            valid_isolate_count = 0;
            p->result_levels[i] = p->embedding_level;
            break;

        default:
            p->result_levels[i] = bidi_stack_last_embedding_level(&stack);
            if (bidi_stack_last_override(&stack) != BIDI_ON)
                p->result_types[i] = bidi_stack_last_override(&stack);
            break;
        }
    }
}

/* Makes room for n more sequence entries. */
static bool bidi_seq_reserve(BidiCore *p, Int n) {
    if (p->seq_used + n <= p->seq_cap)
        return true;
    Int cap = p->seq_cap * 2;
    if (cap < p->seq_used + n)
        cap = p->seq_used + n;
    Int *idx = BIDI_NEW(p->a, cap, Int);
    BidiClass *types = BIDI_NEW(p->a, cap, BidiClass);
    BidiLevel *levels = BIDI_NEW(p->a, cap, BidiLevel);
    if (idx == NULL || types == NULL || levels == NULL) {
        BIDI_FREE(p->a, idx, cap, Int);
        BIDI_FREE(p->a, types, cap, BidiClass);
        BIDI_FREE(p->a, levels, cap, BidiLevel);
        p->oom = true;
        return false;
    }
    if (p->seq_used > 0) {
        memcpy(idx, p->seq_index, (size_t)p->seq_used * sizeof *idx);
        memcpy(types, p->seq_types, (size_t)p->seq_used * sizeof *types);
        memcpy(levels, p->seq_levels, (size_t)p->seq_used * sizeof *levels);
    }
    BIDI_FREE(p->a, p->seq_index, p->seq_cap, Int);
    BIDI_FREE(p->a, p->seq_types, p->seq_cap, BidiClass);
    BIDI_FREE(p->a, p->seq_levels, p->seq_cap, BidiLevel);
    p->seq_index = idx;
    p->seq_types = types;
    p->seq_levels = levels;
    p->seq_cap = cap;
    return true;
}

static bool bidi_seqs_reserve(BidiCore *p) {
    if (p->nseqs < p->seqs_cap)
        return true;
    Int cap = p->seqs_cap == 0 ? 8 : p->seqs_cap * 2;
    BidiSeq *s = BIDI_NEW(p->a, cap, BidiSeq);
    if (s == NULL) {
        p->oom = true;
        return false;
    }
    if (p->nseqs > 0)
        memcpy(s, p->seqs, (size_t)p->nseqs * sizeof *s);
    BIDI_FREE(p->a, p->seqs, p->seqs_cap, BidiSeq);
    p->seqs = s;
    p->seqs_cap = cap;
    return true;
}

/* Rule X10, second bullet: Determine the start-of-sequence (sos) and
 * end-of-sequence (eos) types, either L or R, for the isolating run sequence
 * whose indexes were just added at off. */
static void bidi_isolating_run_sequence(BidiCore *p, Int off, Int length) {
    const Int *indexes = p->seq_index + off;
    BidiClass *types = p->seq_types + off;
    for (Int i = 0; i < length; i++)
        types[i] = p->result_types[indexes[i]];

    /* assign level, sos and eos */
    Int prev_char = indexes[0] - 1;
    while (prev_char >= 0 && bidi_is_removed_by_x9(p->initial_types[prev_char]))
        prev_char--;
    BidiLevel prev_level = p->embedding_level;
    if (prev_char >= 0)
        prev_level = p->result_levels[prev_char];

    BidiLevel succ_level;
    BidiClass last_type = types[length - 1];
    if (BIDI_IN(last_type, BIDI_LRI, BIDI_RLI, BIDI_FSI)) {
        succ_level = p->embedding_level;
    } else {
        /* the first character after the end of run sequence */
        Int limit = indexes[length - 1] + 1;
        while (limit < p->n && bidi_is_removed_by_x9(p->initial_types[limit]))
            limit++;
        succ_level = p->embedding_level;
        if (limit < p->n)
            succ_level = p->result_levels[limit];
    }
    BidiLevel level = p->result_levels[indexes[0]];
    BidiSeq *s = &p->seqs[p->nseqs++];
    s->off = off;
    s->len = length;
    s->level = level;
    s->sos = bidi_type_for_level(bidi_max_level(prev_level, level));
    s->eos = bidi_type_for_level(bidi_max_level(succ_level, level));
}

/* Return the limit of the run consisting only of the types in valid starting
 * at index. */
static Int bidi_find_run_limit(const BidiClass *types, Int len, Int index,
                               const BidiClass *valid, size_t nvalid) {
    for (; index < len; index++) {
        if (!bidi_in(types[index], valid, nvalid))
            return index; /* didn't find a match in validSet */
    }
    return len;
}

/* Algorithm validation. Assert that all values in types are in the provided
 * set. */
static void bidi_assert_only(const BidiCore *p, const BidiSeq *s,
                             const BidiClass *codes, size_t ncodes) {
    const BidiClass *types = p->seq_types + s->off;
    for (Int i = 0; i < s->len; i++) {
        if (bidi_in(types[i], codes, ncodes))
            continue;
        char buf[128];
        Int n = 0;
        bidi_put(buf, &n, "invalid bidi code ");
        bidi_put_int(buf, &n, (Int)types[i]);
        bidi_put(buf, &n, " present in assertOnly at position ");
        bidi_put_int(buf, &n, p->seq_index[s->off + i]);
        runtime_panic(str_from_bytes(buf, n));
    }
}

#define BIDI_ASSERT_ONLY(p, s, ...)                                                    \
    bidi_assert_only((p), (s), (const BidiClass[]){__VA_ARGS__},                       \
                     sizeof((const BidiClass[]){__VA_ARGS__}) / sizeof(BidiClass))

static void bidi_set_types(BidiClass *types, Int n, BidiClass t) {
    for (Int i = 0; i < n; i++)
        types[i] = t;
}

/* Resolving weak types Rules W1-W7. Note that some weak types (EN, AN)
 * remain after this processing is complete. */
static void bidi_resolve_weak_types(BidiCore *p, const BidiSeq *s) {
    /* on entry, only these types remain */
    BIDI_ASSERT_ONLY(p, s, BIDI_L, BIDI_R, BIDI_AL, BIDI_EN, BIDI_ES, BIDI_ET, BIDI_AN,
                     BIDI_CS, BIDI_B, BIDI_S, BIDI_WS, BIDI_ON, BIDI_NSM, BIDI_LRI,
                     BIDI_RLI, BIDI_FSI, BIDI_PDI);

    BidiClass *types = p->seq_types + s->off;
    Int len = s->len;

    /* Rule W1. Changes all NSMs. */
    BidiClass preceding_character_type = s->sos;
    for (Int i = 0; i < len; i++) {
        BidiClass t = types[i];
        if (t == BIDI_NSM)
            types[i] = preceding_character_type;
        else
            preceding_character_type = t;
    }

    /* Rule W2. EN does not change at the start of the run, because sos !=
     * AL. */
    for (Int i = 0; i < len; i++) {
        if (types[i] != BIDI_EN)
            continue;
        for (Int j = i - 1; j >= 0; j--) {
            BidiClass t = types[j];
            if (BIDI_IN(t, BIDI_L, BIDI_R, BIDI_AL)) {
                if (t == BIDI_AL)
                    types[i] = BIDI_AN;
                break;
            }
        }
    }

    /* Rule W3. */
    for (Int i = 0; i < len; i++) {
        if (types[i] == BIDI_AL)
            types[i] = BIDI_R;
    }

    /* Rule W4. Since there must be values on both sides for this rule to
     * have an effect, the scan skips the first and last value. */
    for (Int i = 1; i < len - 1; i++) {
        BidiClass t = types[i];
        if (t == BIDI_ES || t == BIDI_CS) {
            BidiClass prev_sep_type = types[i - 1];
            BidiClass succ_sep_type = types[i + 1];
            if (prev_sep_type == BIDI_EN && succ_sep_type == BIDI_EN)
                types[i] = BIDI_EN;
            else if (types[i] == BIDI_CS && prev_sep_type == BIDI_AN &&
                     succ_sep_type == BIDI_AN)
                types[i] = BIDI_AN;
        }
    }

    /* Rule W5. Go's loop sets its index to the end of the run here, which a
     * range loop ignores, so every position of the run is visited in turn. */
    static const BidiClass et[] = {BIDI_ET};
    for (Int i = 0; i < len; i++) {
        if (types[i] != BIDI_ET)
            continue;
        /* locate end of sequence */
        Int run_start = i;
        Int run_end = bidi_find_run_limit(types, len, run_start, et, 1);

        /* check values at ends of sequence */
        BidiClass t = s->sos;
        if (run_start > 0)
            t = types[run_start - 1];
        if (t != BIDI_EN) {
            t = s->eos;
            if (run_end < len)
                t = types[run_end];
        }
        if (t == BIDI_EN)
            bidi_set_types(types + run_start, run_end - run_start, BIDI_EN);
    }

    /* Rule W6. */
    for (Int i = 0; i < len; i++) {
        if (BIDI_IN(types[i], BIDI_ES, BIDI_ET, BIDI_CS))
            types[i] = BIDI_ON;
    }

    /* Rule W7. */
    for (Int i = 0; i < len; i++) {
        if (types[i] != BIDI_EN)
            continue;
        /* set default if we reach start of run */
        BidiClass prev_strong_type = s->sos;
        for (Int j = i - 1; j >= 0; j--) {
            BidiClass t = types[j];
            if (t == BIDI_L || t == BIDI_R) { /* AL's have been changed to R */
                prev_strong_type = t;
                break;
            }
        }
        if (prev_strong_type == BIDI_L)
            types[i] = BIDI_L;
    }
}

/* Resolving neutral types Rules N1-N2. */
static void bidi_resolve_neutral_types(BidiCore *p, const BidiSeq *s) {
    /* on entry, only these types can be in resultTypes */
    BIDI_ASSERT_ONLY(p, s, BIDI_L, BIDI_R, BIDI_EN, BIDI_AN, BIDI_B, BIDI_S, BIDI_WS,
                     BIDI_ON, BIDI_RLI, BIDI_LRI, BIDI_FSI, BIDI_PDI);

    static const BidiClass neutrals[] = {BIDI_B,   BIDI_S,   BIDI_WS,  BIDI_ON,
                                         BIDI_RLI, BIDI_LRI, BIDI_FSI, BIDI_PDI};
    BidiClass *types = p->seq_types + s->off;
    Int len = s->len;

    /* Go's loop sets its index past the run here, which a range loop ignores,
     * as in rule W5. */
    for (Int i = 0; i < len; i++) {
        switch (types[i]) {
        case BIDI_WS:
        case BIDI_ON:
        case BIDI_B:
        case BIDI_S:
        case BIDI_RLI:
        case BIDI_LRI:
        case BIDI_FSI:
        case BIDI_PDI: {
            /* find bounds of run of neutrals */
            Int run_start = i;
            Int run_end = bidi_find_run_limit(types, len, run_start, neutrals,
                                              sizeof neutrals / sizeof neutrals[0]);

            /* determine effective types at ends of run. Note that the
             * character found can only be L, R, AN, or EN. */
            BidiClass lead_type, trail_type;
            if (run_start == 0) {
                lead_type = s->sos;
            } else {
                lead_type = types[run_start - 1];
                if (BIDI_IN(lead_type, BIDI_AN, BIDI_EN))
                    lead_type = BIDI_R;
            }
            if (run_end == len) {
                trail_type = s->eos;
            } else {
                trail_type = types[run_end];
                if (BIDI_IN(trail_type, BIDI_AN, BIDI_EN))
                    trail_type = BIDI_R;
            }

            BidiClass resolved_type;
            if (lead_type == trail_type) /* Rule N1. */
                resolved_type = lead_type;
            else /* Rule N2. The embedding level of the run is used. */
                resolved_type = bidi_type_for_level(s->level);

            bidi_set_types(types + run_start, run_end - run_start, resolved_type);
            break;
        }
        default:
            break;
        }
    }
}

/* Resolving implicit embedding levels Rules I1, I2. */
static void bidi_resolve_implicit_levels(BidiCore *p, const BidiSeq *s) {
    /* on entry, only these types can be in resultTypes */
    BIDI_ASSERT_ONLY(p, s, BIDI_L, BIDI_R, BIDI_EN, BIDI_AN);

    const BidiClass *types = p->seq_types + s->off;
    BidiLevel *levels = p->seq_levels + s->off;
    for (Int i = 0; i < s->len; i++)
        levels[i] = s->level;

    if ((s->level & 1) == 0) { /* even level */
        for (Int i = 0; i < s->len; i++) {
            /* Rule I1. */
            BidiClass t = types[i];
            if (t == BIDI_L) {
                /* no change */
            } else if (t == BIDI_R) {
                levels[i] = (BidiLevel)(levels[i] + 1);
            } else { /* t == AN || t == EN */
                levels[i] = (BidiLevel)(levels[i] + 2);
            }
        }
    } else { /* odd level */
        for (Int i = 0; i < s->len; i++) {
            /* Rule I2. */
            if (types[i] != BIDI_R) /* t == L || t == AN || t == EN */
                levels[i] = (BidiLevel)(levels[i] + 1);
        }
    }
}

/* Applies the levels and types resolved in rules W1-I2 to the resultLevels
 * array. */
static void bidi_apply_levels_and_types(BidiCore *p, const BidiSeq *s) {
    for (Int i = 0; i < s->len; i++) {
        Int x = p->seq_index[s->off + i];
        p->result_types[x] = p->seq_types[s->off + i];
        p->result_levels[x] = p->seq_levels[s->off + i];
    }
}

/* determineLevelRuns works out the level runs, leaving out the characters
 * rule X9 removes. */
static void bidi_determine_level_runs(BidiCore *p) {
    BidiLevel current_level = BIDI_IMPLICIT_LEVEL;
    Int used = 0;
    p->nruns = 0;
    for (Int i = 0; i < p->n; i++) {
        if (bidi_is_removed_by_x9(p->initial_types[i]))
            continue;
        if (p->result_levels[i] != current_level) {
            /* we just encountered a new run; wrap up last run */
            if (current_level >= 0) /* only wrap it up if there was a run */
                p->nruns++;
            /* Start new run */
            current_level = p->result_levels[i];
            p->run_start[p->nruns] = used;
            p->run_len[p->nruns] = 0;
        }
        p->run_index[used++] = i;
        p->run_len[p->nruns]++;
    }
    /* Wrap up the final run, if any */
    if (current_level >= 0 && p->run_len[p->nruns] > 0)
        p->nruns++;
}

/* Definition BD13. Determine isolating run sequences. */
static void bidi_determine_isolating_run_sequences(BidiCore *p) {
    bidi_determine_level_runs(p);

    /* Compute the run that each character belongs to */
    for (Int i = 0; i < p->nruns; i++) {
        for (Int k = 0; k < p->run_len[i]; k++)
            p->run_for_character[p->run_index[p->run_start[i] + k]] = i;
    }

    for (Int r = 0; r < p->nruns; r++) {
        Int run = r;
        Int first = p->run_index[p->run_start[run]];
        if (p->initial_types[first] == BIDI_PDI &&
            p->matching_isolate_initiator[first] != -1)
            continue;
        Int off = p->seq_used;
        for (;;) {
            /* Copy this level run into currentRunSequence */
            if (!bidi_seq_reserve(p, p->run_len[run]))
                return;
            memcpy(p->seq_index + p->seq_used, p->run_index + p->run_start[run],
                   (size_t)p->run_len[run] * sizeof(Int));
            p->seq_used += p->run_len[run];

            Int last = p->seq_index[p->seq_used - 1];
            BidiClass last_t = p->initial_types[last];
            if (BIDI_IN(last_t, BIDI_LRI, BIDI_RLI, BIDI_FSI) &&
                p->matching_pdi[last] != p->n)
                run = p->run_for_character[p->matching_pdi[last]];
            else
                break;
        }
        if (!bidi_seqs_reserve(p))
            return;
        bidi_isolating_run_sequence(p, off, p->seq_used - off);
    }
}

/* ------------------------------------------------------------ bracket.go */

enum { BIDI_MAX_PAIRING_DEPTH = 63 };

typedef struct BidiBracketPairer {
    BidiCore *p;
    BidiClass sos; /* direction corresponding to start of sequence */

    /* openers, as a stack whose top is Go's front of the list. */
    Int openers[BIDI_MAX_PAIRING_DEPTH];
    Int nopeners;

    Int npairs;

    BidiClass *codes_isolated_run; /* directional bidi codes for an isolated run */
    const Int *indexes;            /* array of index values into the original string */
    Int len;
} BidiBracketPairer;

/* locateBrackets locates matching bracket pairs according to BD16. */
static void bidi_locate_brackets(BidiBracketPairer *bp) {
    BidiCore *p = bp->p;
    for (Int i = 0; i < bp->len; i++) {
        Int index = bp->indexes[i];
        /* look at the bracket type for each character */
        bidi_check_index(index, p->npt);
        if (p->pair_types[index] == BIDI_BP_NONE ||
            bp->codes_isolated_run[i] != BIDI_ON)
            continue; /* continue scanning */
        switch (p->pair_types[index]) {
        case BIDI_BP_OPEN:
            /* check if maximum pairing depth reached */
            if (bp->nopeners == BIDI_MAX_PAIRING_DEPTH) {
                bp->nopeners = 0;
                return;
            }
            /* remember opener location, most recent first */
            bp->openers[bp->nopeners++] = i;
            break;

        case BIDI_BP_CLOSE: {
            /* see if there is a match */
            Int count = 0;
            for (Int e = bp->nopeners - 1; e >= 0; e--) {
                count++;
                Int opener = bp->openers[e];
                bidi_check_index(bp->indexes[opener], p->npv);
                bidi_check_index(bp->indexes[i], p->npv);
                if (p->pair_values[bp->indexes[opener]] ==
                    p->pair_values[bp->indexes[i]]) {
                    /* if the opener matches, add nested pair to the ordered
                     * list, and remove up to the opener */
                    Int k = bp->npairs++;
                    /* sort.Sort by opener, which is an insertion here as the
                     * rest are sorted already and no two share an opener. */
                    while (k > 0 && p->pair_opener[k - 1] > opener) {
                        p->pair_opener[k] = p->pair_opener[k - 1];
                        p->pair_closer[k] = p->pair_closer[k - 1];
                        k--;
                    }
                    p->pair_opener[k] = opener;
                    p->pair_closer[k] = i;
                    bp->nopeners -= count;
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

/* getStrongTypeN0 maps character's directional code to strong type as
 * required by rule N0. */
static BidiClass bidi_get_strong_type_n0(const BidiBracketPairer *bp, Int index) {
    switch (bp->codes_isolated_run[index]) {
    case BIDI_EN:
    case BIDI_AN:
    case BIDI_AL:
    case BIDI_R:
        return BIDI_R;
    case BIDI_L:
        return BIDI_L;
    default:
        return BIDI_ON;
    }
}

/* classifyPairContent reports the strong types contained inside a Bracket
 * Pair, assuming the given embedding direction. */
static BidiClass bidi_classify_pair_content(const BidiBracketPairer *bp, Int opener,
                                            Int closer, BidiClass dir_embed) {
    BidiClass dir_opposite = BIDI_ON;
    for (Int i = opener + 1; i < closer; i++) {
        BidiClass dir = bidi_get_strong_type_n0(bp, i);
        if (dir == BIDI_ON)
            continue;
        if (dir == dir_embed)
            return dir; /* type matching embedding direction found */
        dir_opposite = dir;
    }
    /* return ON if no strong type found, or class opposite to dirEmbed */
    return dir_opposite;
}

/* classBeforePair determines which strong types are present before a Bracket
 * Pair. */
static BidiClass bidi_class_before_pair(const BidiBracketPairer *bp, Int opener) {
    for (Int i = opener - 1; i >= 0; i--) {
        BidiClass dir = bidi_get_strong_type_n0(bp, i);
        if (dir != BIDI_ON)
            return dir;
    }
    /* no strong types found, return sos */
    return bp->sos;
}

/* setBracketsToType sets a bracket pair, and any NSMs right after either
 * bracket, to dirPair. */
static void bidi_set_brackets_to_type(BidiBracketPairer *bp, Int opener, Int closer,
                                      BidiClass dir_pair) {
    const BidiClass *initial_types = bp->p->initial_types;
    bp->codes_isolated_run[opener] = dir_pair;
    bp->codes_isolated_run[closer] = dir_pair;

    for (Int i = opener + 1; i < closer; i++) {
        Int index = bp->indexes[i];
        if (initial_types[index] != BIDI_NSM)
            break;
        bp->codes_isolated_run[i] = dir_pair;
    }

    for (Int i = closer + 1; i < bp->len; i++) {
        Int index = bp->indexes[i];
        if (initial_types[index] != BIDI_NSM)
            break;
        bp->codes_isolated_run[i] = dir_pair;
    }
}

/* assignBracketType implements rule N0 for a single bracket pair. */
static void bidi_assign_bracket_type(BidiBracketPairer *bp, Int opener, Int closer,
                                     BidiClass dir_embed) {
    /* rule "N0, a", inspect contents of pair */
    BidiClass dir_pair = bidi_classify_pair_content(bp, opener, closer, dir_embed);

    /* dirPair is now L, R, or N (no strong type found) */
    if (dir_pair == BIDI_ON)
        return; /* case "d" - nothing to do */

    if (dir_pair != dir_embed) {
        /* case "c": strong type found, opposite - check before (c.1) */
        dir_pair = bidi_class_before_pair(bp, opener);
        if (dir_pair == dir_embed || dir_pair == BIDI_ON) /* no strong opposite */
            dir_pair = dir_embed;
    }
    /* else: case "b", strong type found matching embedding, no explicit action
     * needed, as dirPair is already set to embedding direction */

    /* set the bracket types to the type found */
    bidi_set_brackets_to_type(bp, opener, closer, dir_pair);
}

/* resolvePairedBrackets runs the paired bracket part of the UBA algorithm. */
static void bidi_resolve_paired_brackets(BidiCore *p, const BidiSeq *s) {
    BidiBracketPairer bp;
    bp.p = p;
    bp.sos = s->sos;
    bp.nopeners = 0;
    bp.npairs = 0;
    bp.codes_isolated_run = p->seq_types + s->off;
    bp.indexes = p->seq_index + s->off;
    bp.len = s->len;
    BidiClass dir_embed = BIDI_L;
    if ((s->level & 1) != 0)
        dir_embed = BIDI_R;
    bidi_locate_brackets(&bp);
    for (Int k = 0; k < bp.npairs; k++)
        bidi_assign_bracket_type(&bp, p->pair_opener[k], p->pair_closer[k], dir_embed);
}

/* ---------------------------------------------------------- core.go, run */

/* Assign level information to characters removed by rule X9. */
static void bidi_assign_levels_to_characters_removed_by_x9(BidiCore *p) {
    for (Int i = 0; i < p->n; i++) {
        BidiClass t = p->initial_types[i];
        if (BIDI_IN(t, BIDI_LRE, BIDI_RLE, BIDI_LRO, BIDI_RLO, BIDI_PDF, BIDI_BN)) {
            p->result_types[i] = t;
            p->result_levels[i] = -1;
        }
    }
    /* now propagate forward the levels information */
    if (p->result_levels[0] == -1)
        p->result_levels[0] = p->embedding_level;
    for (Int i = 1; i < p->n; i++) {
        if (p->result_levels[i] == -1)
            p->result_levels[i] = p->result_levels[i - 1];
    }
}

/* The algorithm. Does not include line-based processing (Rules L1, L2). */
static void bidi_run(BidiCore *p) {
    bidi_determine_matching_isolates(p);

    /* 1) determining the paragraph level. Rules P2, P3. */
    if (p->embedding_level == BIDI_IMPLICIT_LEVEL)
        p->embedding_level = bidi_determine_paragraph_embedding_level(p, 0, p->n);

    /* Initialize result levels to paragraph embedding level. */
    for (Int i = 0; i < p->n; i++)
        p->result_levels[i] = p->embedding_level;

    /* 2) Explicit levels and directions. Rules X1-X8. */
    bidi_determine_explicit_embedding_levels(p);

    /* Rule X10. Run remainder of algorithm one isolating run sequence at a
     * time */
    bidi_determine_isolating_run_sequences(p);
    if (p->oom)
        return;
    for (Int k = 0; k < p->nseqs; k++) {
        const BidiSeq *seq = &p->seqs[k];
        /* 3) resolving weak types. Rules W1-W7. */
        bidi_resolve_weak_types(p, seq);
        /* 4a) resolving paired brackets. Rule N0 */
        bidi_resolve_paired_brackets(p, seq);
        /* 4b) resolving neutral types. Rules N1-N3. */
        bidi_resolve_neutral_types(p, seq);
        /* 5) resolving implicit embedding levels. Rules I1, I2. */
        bidi_resolve_implicit_levels(p, seq);
        /* Apply the computed levels and types */
        bidi_apply_levels_and_types(p, seq);
    }

    /* Assign appropriate levels to 'hide' LREs, RLEs, LROs, RLOs, PDFs, and
     * BNs. */
    bidi_assign_levels_to_characters_removed_by_x9(p);
}

/* The validations newParagraph makes, with fmt's text. */
static Error bidi_validate(const BidiClass *types, Int n, const uint8_t *pair_types,
                           Int npt, const Rune *pair_values, Int npv, int8_t level) {
    if (n == 0 || types == NULL)
        return fmt_errorf_v("types is null");
    for (Int i = 0; i < n - 1; i++) {
        if (types[i] == BIDI_B)
            return fmt_errorf_v("B type before end of paragraph at index: %d", i);
    }
    if (npt == 0 || pair_types == NULL)
        return fmt_errorf_v("pairTypes is null");
    for (Int i = 0; i < npt; i++) {
        switch (pair_types[i]) {
        case BIDI_BP_NONE:
        case BIDI_BP_OPEN:
        case BIDI_BP_CLOSE:
            break;
        default:
            return fmt_errorf_v("illegal pairType value at %d: %v", i, pair_types[i]);
        }
    }
    if (pair_values == NULL)
        return fmt_errorf_v("pairValues is null");
    if (npt != npv)
        return fmt_errorf_v("pairTypes is different length from pairValues");
    if (level != BIDI_IMPLICIT_LEVEL && level != 0 && level != 1)
        return fmt_errorf_v("illegal paragraph embedding level: %d", level);
    return BURROW_NO_ERROR;
}

BidiCore *burrow__bidi_new_core(Alloc *a, const BidiClass *types, Int n,
                                const uint8_t *pair_types, Int npt,
                                const Rune *pair_values, Int npv, int8_t level,
                                Error *err) {
    *err = bidi_validate(types, n, pair_types, npt, pair_values, npv, level);
    /* bidi_validate turns the null types away; the second test is for the
     * analyser, which cannot see that. */
    if (BURROW_FAILED(*err) || types == NULL)
        return NULL;

    BidiCore *p = mem_alloc(a, sizeof *p, _Alignof(BidiCore));
    if (p == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    p->a = a;
    p->n = n;
    p->embedding_level = level;
    p->pair_types = pair_types;
    p->pair_values = pair_values;
    p->npt = npt;
    p->npv = npv;
    p->initial_types = BIDI_NEW(a, n, BidiClass);
    p->result_types = BIDI_NEW(a, n, BidiClass);
    p->result_levels = BIDI_NEW(a, n, BidiLevel);
    p->matching_pdi = BIDI_NEW(a, n, Int);
    p->matching_isolate_initiator = BIDI_NEW(a, n, Int);
    p->run_index = BIDI_NEW(a, n, Int);
    p->run_start = BIDI_NEW(a, n, Int);
    p->run_len = BIDI_NEW(a, n, Int);
    p->run_for_character = BIDI_NEW(a, n, Int);
    p->pair_opener = BIDI_NEW(a, n, Int);
    p->pair_closer = BIDI_NEW(a, n, Int);
    if (p->initial_types == NULL || p->result_types == NULL ||
        p->result_levels == NULL || p->matching_pdi == NULL ||
        p->matching_isolate_initiator == NULL || p->run_index == NULL ||
        p->run_start == NULL || p->run_len == NULL || p->run_for_character == NULL ||
        p->pair_opener == NULL || p->pair_closer == NULL || !bidi_seq_reserve(p, n)) {
        burrow__bidi_core_free(p);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    memcpy(p->initial_types, types, (size_t)n * sizeof *types);
    memcpy(p->result_types, types, (size_t)n * sizeof *types);

    bidi_run(p);
    bidi_core_free_work(p);
    p->pair_types = NULL;
    p->pair_values = NULL;
    if (p->oom) {
        burrow__bidi_core_free(p);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    return p;
}

int8_t burrow__bidi_core_embedding_level(const BidiCore *c) {
    return c->embedding_level;
}

Int burrow__bidi_core_len(const BidiCore *c) {
    return c->n;
}

/* getLevels computes levels array breaking lines at offsets in linebreaks.
 * Rule L1. Go checks the line breaks and then ignores what it found, so a
 * bad one goes on to whatever index panic it leads to. */
int8_t *burrow__bidi_core_levels(const BidiCore *p, const Int *linebreaks,
                                 Int nbreaks) {
    BidiLevel *result = BIDI_NEW(p->a, p->n, BidiLevel);
    if (result == NULL)
        return NULL;
    memcpy(result, p->result_levels, (size_t)p->n);

    for (Int i = 0; i < p->n; i++) {
        BidiClass t = p->initial_types[i];
        if (BIDI_IN(t, BIDI_B, BIDI_S)) {
            /* Rule L1, clauses one and two. */
            result[i] = p->embedding_level;

            /* Rule L1, clause three. */
            for (Int j = i - 1; j >= 0; j--) {
                if (!bidi_is_whitespace(
                        p->initial_types[j])) /* including format codes */
                    break;
                result[j] = p->embedding_level;
            }
        }
    }

    /* Rule L1, clause four. */
    Int start = 0;
    for (Int k = 0; k < nbreaks; k++) {
        Int limit = linebreaks[k];
        for (Int j = limit - 1; j >= start; j--) {
            bidi_check_index(j, p->n);
            if (!bidi_is_whitespace(p->initial_types[j])) /* including format codes */
                break;
            result[j] = p->embedding_level;
        }
        start = limit;
    }
    return result;
}

/* computeReordering returns the reordering of one line for the given levels,
 * as a visual to logical map. Rule L2. */
static void bidi_compute_reordering(const BidiLevel *levels, Int n, Int *result) {
    /* initialize order */
    for (Int i = 0; i < n; i++)
        result[i] = i;

    /* locate highest level found on line. */
    BidiLevel highest_level = 0;
    BidiLevel lowest_odd_level = BIDI_MAX_DEPTH + 2;
    for (Int i = 0; i < n; i++) {
        BidiLevel level = levels[i];
        if (level > highest_level)
            highest_level = level;
        if ((level & 1) != 0 && level < lowest_odd_level)
            lowest_odd_level = level;
    }

    for (BidiLevel level = highest_level; level >= lowest_odd_level; level--) {
        for (Int i = 0; i < n; i++) {
            if (levels[i] >= level) {
                /* find range of text at or above this level */
                Int start = i;
                Int limit = i + 1;
                while (limit < n && levels[limit] >= level)
                    limit++;

                for (Int j = start, k = limit - 1; j < k; j++, k--) {
                    Int tmp = result[j];
                    result[j] = result[k];
                    result[k] = tmp;
                }
                /* skip to end of level run */
                i = limit;
            }
        }
    }
}

/* getReordering returns the reordering of lines from a visual index to a
 * logical index for line breaks at the given offsets. */
Int *burrow__bidi_core_reordering(const BidiCore *p, const Int *linebreaks,
                                  Int nbreaks) {
    BidiLevel *levels = burrow__bidi_core_levels(p, linebreaks, nbreaks);
    if (levels == NULL && p->n > 0)
        return NULL;
    Int *result = BIDI_NEW(p->a, p->n, Int);
    Int *order = BIDI_NEW(p->a, p->n, Int);
    if (result == NULL || order == NULL) {
        BIDI_FREE(p->a, result, p->n, Int);
        BIDI_FREE(p->a, order, p->n, Int);
        BIDI_FREE(p->a, levels, p->n, BidiLevel);
        return NULL;
    }

    /* computeMultilineReordering, which does not reorder across a line
     * break. */
    Int start = 0;
    for (Int k = 0; k < nbreaks; k++) {
        Int limit = linebreaks[k];
        if (limit - start < 0)
            runtime_panic(BURROW_S("runtime error: makeslice: len out of range"));
        bidi_check_slice(start, p->n, p->n);
        Int m = limit - start;
        /* tempLevels is levels[start:] cut or padded with zeroes to m. */
        BidiLevel *temp = BIDI_NEW(p->a, m, BidiLevel);
        if (temp == NULL && m > 0) {
            BIDI_FREE(p->a, result, p->n, Int);
            BIDI_FREE(p->a, order, p->n, Int);
            BIDI_FREE(p->a, levels, p->n, BidiLevel);
            return NULL;
        }
        Int c = p->n - start < m ? p->n - start : m;
        if (c > 0)
            memcpy(temp, levels + start, (size_t)c);
        Int *tmp_order = m <= p->n ? order : BIDI_NEW(p->a, m, Int);
        if (tmp_order == NULL && m > 0) {
            BIDI_FREE(p->a, temp, m, BidiLevel);
            BIDI_FREE(p->a, result, p->n, Int);
            BIDI_FREE(p->a, order, p->n, Int);
            BIDI_FREE(p->a, levels, p->n, BidiLevel);
            return NULL;
        }
        bidi_compute_reordering(temp, m, tmp_order);
        for (Int j = 0; j < m; j++) {
            bidi_check_index(start + j, p->n);
            result[start + j] = tmp_order[j] + start;
        }
        if (tmp_order != order)
            BIDI_FREE(p->a, tmp_order, m, Int);
        BIDI_FREE(p->a, temp, m, BidiLevel);
        start = limit;
    }
    BIDI_FREE(p->a, order, p->n, Int);
    BIDI_FREE(p->a, levels, p->n, BidiLevel);
    return result;
}

/* ---------------------------------------------------------------- bidi.go */

BidiOption burrow__bidi_default_direction(BidiDirection d) {
    BidiOption o = {d};
    return o;
}

void burrow__bidi_ordering_free(BidiOrdering *o) {
    if (o->a != NULL) {
        BIDI_FREE(o->a, o->runs, o->nruns, BidiRun);
        BIDI_FREE(o->a, o->runes, o->nrunes, Rune);
    }
    memset(o, 0, sizeof *o);
}

BidiDirection burrow__bidi_ordering_direction(const BidiOrdering *o) {
    bidi_check_index(0, o->nruns);
    return o->runs[0].direction;
}

Int burrow__bidi_ordering_num_runs(const BidiOrdering *o) {
    return o->nruns;
}

BidiRun burrow__bidi_ordering_run(const BidiOrdering *o, Int i) {
    bidi_check_index(i, o->nruns);
    return o->runs[i];
}

/* A copy of o, with its own memory. */
static bool bidi_ordering_clone(Alloc *a, const BidiOrdering *o, BidiOrdering *out) {
    memset(out, 0, sizeof *out);
    out->a = a;
    out->runs = BIDI_NEW(a, o->nruns, BidiRun);
    out->runes = BIDI_NEW(a, o->nrunes, Rune);
    if ((out->runs == NULL && o->nruns > 0) || (out->runes == NULL && o->nrunes > 0)) {
        burrow__bidi_ordering_free(out);
        return false;
    }
    out->nruns = o->nruns;
    out->nrunes = o->nrunes;
    if (o->nrunes > 0)
        memcpy(out->runes, o->runes, (size_t)o->nrunes * sizeof(Rune));
    for (Int i = 0; i < o->nruns; i++) {
        out->runs[i] = o->runs[i];
        out->runs[i].runes = out->runes + (o->runs[i].runes - o->runes);
    }
    return true;
}

/* calculateOrdering splits levels into runs of one direction, over a copy of
 * runes. */
static bool bidi_calculate_ordering(Alloc *a, const BidiLevel *levels, Int nlevels,
                                    const Rune *runes, Int nrunes, BidiOrdering *o) {
    memset(o, 0, sizeof *o);
    o->a = a;
    /* One run for each change of direction, and one at the end. */
    Int nruns = 1;
    for (Int i = 1; i < nlevels; i++) {
        if ((levels[i] % 2 == 0) != (levels[i - 1] % 2 == 0))
            nruns++;
    }
    o->runs = BIDI_NEW(a, nruns, BidiRun);
    o->runes = BIDI_NEW(a, nrunes, Rune);
    if (o->runs == NULL || (o->runes == NULL && nrunes > 0)) {
        BIDI_FREE(a, o->runs, nruns, BidiRun);
        BIDI_FREE(a, o->runes, nrunes, Rune);
        memset(o, 0, sizeof *o);
        return false;
    }
    o->nrunes = nrunes;
    if (nrunes > 0)
        memcpy(o->runes, runes, (size_t)nrunes * sizeof(Rune));

    BidiDirection cur_dir;
    BidiDirection prev_dir = BIDI_NEUTRAL;
    Int prev_i = 0;
    for (Int i = 0; i < nlevels; i++) {
        if (levels[i] % 2 == 0)
            cur_dir = BIDI_LEFT_TO_RIGHT;
        else
            cur_dir = BIDI_RIGHT_TO_LEFT;
        if (cur_dir != prev_dir) {
            if (i > 0) {
                bidi_check_slice(prev_i, i, nrunes);
                BidiRun r = {o->runes + prev_i, i - prev_i, prev_dir, prev_i};
                o->runs[o->nruns++] = r;
            }
            prev_i = i;
            prev_dir = cur_dir;
        }
    }
    bidi_check_slice(prev_i, nrunes, nrunes);
    BidiRun r = {o->runes + prev_i, nrunes - prev_i, prev_dir, prev_i};
    o->runs[o->nruns++] = r;
    return true;
}

Str burrow__bidi_run_string(Alloc *a, const BidiRun *r) {
    Int n = bidi_encode(NULL, r->runes, r->len);
    if (n == 0)
        return str_from_bytes("", 0);
    Byte *p = mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return str_from_bytes("", 0);
    bidi_encode(p, r->runes, r->len);
    return str_from_bytes(p, n);
}

Slice burrow__bidi_run_bytes(Alloc *a, const BidiRun *r) {
    Int n = bidi_encode(NULL, r->runes, r->len);
    Byte *p = mem_alloc_nozero(a, (size_t)(n > 0 ? n : 1), 1);
    if (p == NULL)
        return slice_nil(TYPE_BYTE);
    bidi_encode(p, r->runes, r->len);
    Slice s = {p, n, n, TYPE_BYTE};
    return s;
}

BidiDirection burrow__bidi_run_direction(const BidiRun *r) {
    return r->direction;
}

void burrow__bidi_run_pos(const BidiRun *r, Int *start, Int *end) {
    *start = r->startpos;
    *end = r->startpos + r->len - 1;
}

void burrow__bidi_paragraph_init(BidiParagraph *p, Alloc *a) {
    memset(p, 0, sizeof *p);
    p->a = a;
}

static void bidi_paragraph_drop_input(BidiParagraph *p) {
    BIDI_FREE(p->a, p->types, p->types_cap, BidiClass);
    BIDI_FREE(p->a, p->pair_types, p->pair_types_cap, uint8_t);
    BIDI_FREE(p->a, p->pair_values, p->pair_values_cap, Rune);
    BIDI_FREE(p->a, p->runes, p->nrunes, Rune);
    p->types = NULL;
    p->pair_types = NULL;
    p->pair_values = NULL;
    p->runes = NULL;
    p->ntypes = p->types_cap = p->pair_types_cap = p->pair_values_cap = 0;
    p->nrunes = 0;
}

void burrow__bidi_paragraph_free(BidiParagraph *p) {
    bidi_paragraph_drop_input(p);
    burrow__bidi_ordering_free(&p->o);
}

/* prepareInput reads the runes of the text and works out their classes and
 * bracket types, up to the first paragraph separator. */
static Int bidi_prepare_input(BidiParagraph *p, const Byte *b, Int len, Error *err) {
    *err = BURROW_NO_ERROR;
    bidi_paragraph_drop_input(p);
    bool oom = false;
    p->runes = bidi_runes(p->a, b, len, &p->nrunes, &oom);
    if (oom) {
        *err = burrow_err_out_of_memory;
        return 0;
    }

    /* How many classes there are, and so the capacities Go ends up with. */
    Int bytecount = 0;
    Int n = 0;
    for (Int k = 0; k < p->nrunes; k++) {
        Int i = 0;
        BidiProperties props = burrow__bidi_lookup_rune(p->runes[k], &i);
        bytecount += i;
        if (burrow__bidi_class(props) == BIDI_B)
            break;
        n++;
    }
    p->types_cap = bidi_append_cap(n, (Int)sizeof(Uint));
    p->pair_types_cap = bidi_append_cap(n, 1);
    p->pair_values_cap = bidi_append_cap(n, 4);
    p->types = BIDI_NEW(p->a, p->types_cap, BidiClass);
    p->pair_types = BIDI_NEW(p->a, p->pair_types_cap, uint8_t);
    p->pair_values = BIDI_NEW(p->a, p->pair_values_cap, Rune);
    if (n > 0 &&
        (p->types == NULL || p->pair_types == NULL || p->pair_values == NULL)) {
        bidi_paragraph_drop_input(p);
        *err = burrow_err_out_of_memory;
        return 0;
    }

    for (Int k = 0; k < n; k++) {
        Rune r = p->runes[k];
        Int i = 0;
        BidiProperties props = burrow__bidi_lookup_rune(r, &i);
        p->types[k] = burrow__bidi_class(props);
        if (burrow__bidi_is_opening_bracket(props)) {
            p->pair_types[k] = BIDI_BP_OPEN;
            p->pair_values[k] = r;
        } else if (burrow__bidi_is_bracket(props)) {
            p->pair_types[k] = BIDI_BP_CLOSE;
            p->pair_values[k] = r;
        } else {
            p->pair_types[k] = BIDI_BP_NONE;
            p->pair_values[k] = 0;
        }
    }
    p->ntypes = n;
    return bytecount;
}

static void bidi_set_opts(BidiParagraph *p, const BidiOption *opts, Int nopts) {
    p->nopts = nopts;
    if (nopts > 0)
        p->opt = opts[nopts - 1].default_direction;
}

Int burrow__bidi_paragraph_set_bytes(BidiParagraph *p, Slice b, const BidiOption *opts,
                                     Int nopts, Error *err) {
    bidi_set_opts(p, opts, nopts);
    return bidi_prepare_input(p, b.p, b.len, err);
}

Int burrow__bidi_paragraph_set_string(BidiParagraph *p, Str s, const BidiOption *opts,
                                      Int nopts, Error *err) {
    bidi_set_opts(p, opts, nopts);
    return bidi_prepare_input(p, s.p, s.len, err);
}

bool burrow__bidi_paragraph_is_left_to_right(const BidiParagraph *p) {
    return burrow__bidi_paragraph_direction(p) == BIDI_LEFT_TO_RIGHT;
}

BidiDirection burrow__bidi_paragraph_direction(const BidiParagraph *p) {
    return burrow__bidi_ordering_direction(&p->o);
}

/* RunAt. Every run that ends after pos sets the answer in turn, so the last
 * run is the one that comes back for any pos inside the paragraph, and the
 * first for any pos past it. That is Go's loop. */
BidiRun burrow__bidi_paragraph_run_at(const BidiParagraph *p, Int pos) {
    Int c = 0;
    Int run_number = 0;
    for (Int i = 0; i < p->o.nruns; i++) {
        c += p->o.runs[i].len;
        if (pos < c)
            run_number = i;
    }
    return burrow__bidi_ordering_run(&p->o, run_number);
}

/* The levels of a paragraph of n classes, for one line. */
static BidiLevel *bidi_levels(Alloc *a, const BidiClass *types, Int n,
                              const uint8_t *pair_types, const Rune *pair_values,
                              BidiLevel lvl, Error *err) {
    BidiCore *para =
        burrow__bidi_new_core(a, types, n, pair_types, n, pair_values, n, lvl, err);
    if (para == NULL)
        return NULL;
    Int brk = n;
    BidiLevel *levels = burrow__bidi_core_levels(para, &brk, 1);
    burrow__bidi_core_free(para);
    if (levels == NULL)
        *err = burrow_err_out_of_memory;
    return levels;
}

BidiOrdering burrow__bidi_paragraph_order(BidiParagraph *p, Error *err) {
    BidiOrdering none;
    memset(&none, 0, sizeof none);
    *err = BURROW_NO_ERROR;
    if (p->ntypes == 0)
        return none;

    if (p->nopts > 0)
        p->default_direction = p->opt;
    BidiLevel lvl = -1;
    if (p->default_direction == BIDI_RIGHT_TO_LEFT)
        lvl = 1;
    BidiLevel *levels =
        bidi_levels(p->a, p->types, p->ntypes, p->pair_types, p->pair_values, lvl, err);
    if (levels == NULL)
        return none;

    BidiOrdering o, keep;
    bool ok = bidi_calculate_ordering(p->a, levels, p->ntypes, p->runes, p->nrunes, &o);
    BIDI_FREE(p->a, levels, p->ntypes, BidiLevel);
    if (!ok) {
        *err = burrow_err_out_of_memory;
        return none;
    }
    if (!bidi_ordering_clone(p->a, &o, &keep)) {
        burrow__bidi_ordering_free(&o);
        *err = burrow_err_out_of_memory;
        return none;
    }
    burrow__bidi_ordering_free(&p->o);
    p->o = keep;
    return o;
}

BidiOrdering burrow__bidi_paragraph_line(BidiParagraph *p, Int start, Int end,
                                         Error *err) {
    BidiOrdering none;
    memset(&none, 0, sizeof none);
    *err = BURROW_NO_ERROR;

    /* p.types[start:end] and the other two, which may reach past the classes
     * into the zeroes up to the capacity. */
    bidi_check_slice(start, end, p->types_cap);
    bidi_check_slice(start, end, p->pair_types_cap);
    bidi_check_slice(start, end, p->pair_values_cap);
    Int n = end - start;
    const Rune *pair_values = p->pair_values != NULL ? p->pair_values + start : NULL;
    /* An empty slice of a non-nil one is not nil. */
    static const Rune bidi_no_rune = 0;
    if (pair_values == NULL && p->pair_values_cap > 0)
        pair_values = &bidi_no_rune;
    BidiLevel *levels = bidi_levels(
        p->a, p->types != NULL ? p->types + start : NULL, n,
        p->pair_types != NULL ? p->pair_types + start : NULL, pair_values, -1, err);
    if (levels == NULL)
        return none;

    /* p.runes[start:end] comes after newParagraph, so a bad range there only
     * panics once the classes have got through. */
    if (end > p->nrunes) {
        BIDI_FREE(p->a, levels, n, BidiLevel);
        bidi_check_slice(start, end, p->nrunes);
    }

    BidiOrdering o;
    bool ok = bidi_calculate_ordering(p->a, levels, n, p->runes + start, n, &o);
    BIDI_FREE(p->a, levels, n, BidiLevel);
    if (!ok) {
        *err = burrow_err_out_of_memory;
        return none;
    }
    return o;
}

BURROW_OWNS(ret) Slice burrow__bidi_append_reverse(Alloc *a, Slice out, Slice in) {
    Int total = in.len + out.len;
    Byte *ret = mem_alloc(a, (size_t)(total > 0 ? total : 1), 1);
    if (ret == NULL)
        return slice_nil(TYPE_BYTE);
    if (out.len > 0)
        memcpy(ret, out.p, (size_t)out.len);
    Int n = 0;
    bool oom = false;
    Rune *in_runes = bidi_runes(a, in.p, in.len, &n, &oom);
    if (oom) {
        mem_free(a, ret, (size_t)(total > 0 ? total : 1), 1);
        return slice_nil(TYPE_BYTE);
    }

    for (Int i = 0; i < n; i++) {
        Int sz = 0;
        BidiProperties prop = burrow__bidi_lookup_rune(in_runes[i], &sz);
        if (burrow__bidi_is_bracket(prop))
            in_runes[i] = burrow__bidi_reverse_bracket(prop, in_runes[i]);
    }

    for (Int i = 0, j = n - 1; i < j; i++, j--) {
        Rune tmp = in_runes[i];
        in_runes[i] = in_runes[j];
        in_runes[j] = tmp;
    }

    /* copy(ret[len(out):], string(inRunes)), which stops at whichever is
     * shorter. */
    Int m = bidi_encode(NULL, in_runes, n);
    Byte *enc = mem_alloc_nozero(a, (size_t)(m > 0 ? m : 1), 1);
    if (enc == NULL) {
        BIDI_FREE(a, in_runes, n, Rune);
        mem_free(a, ret, (size_t)(total > 0 ? total : 1), 1);
        return slice_nil(TYPE_BYTE);
    }
    bidi_encode(enc, in_runes, n);
    Int c = m < in.len ? m : in.len;
    if (c > 0)
        memcpy(ret + out.len, enc, (size_t)c);
    mem_free(a, enc, (size_t)(m > 0 ? m : 1), 1);
    BIDI_FREE(a, in_runes, n, Rune);

    Slice s = {ret, total, total, TYPE_BYTE};
    return s;
}

BURROW_OWNS(ret) Str burrow__bidi_reverse_string(Alloc *a, Str s) {
    Int li = 0;
    bool oom = false;
    Rune *input = bidi_runes(a, s.p, s.len, &li, &oom);
    if (oom)
        return str_from_bytes("", 0);
    Rune *ret = BIDI_NEW(a, li, Rune);
    if (ret == NULL && li > 0) {
        BIDI_FREE(a, input, li, Rune);
        return str_from_bytes("", 0);
    }
    for (Int i = 0; i < li; i++) {
        Rune r = input[i];
        Int sz = 0;
        BidiProperties prop = burrow__bidi_lookup_rune(r, &sz);
        if (burrow__bidi_is_bracket(prop))
            ret[li - i - 1] = burrow__bidi_reverse_bracket(prop, r);
        else
            ret[li - i - 1] = r;
    }
    Int m = bidi_encode(NULL, ret, li);
    Byte *p = m > 0 ? mem_alloc_nozero(a, (size_t)m, 1) : NULL;
    if (p != NULL)
        bidi_encode(p, ret, li);
    BIDI_FREE(a, ret, li, Rune);
    BIDI_FREE(a, input, li, Rune);
    if (p == NULL)
        return str_from_bytes("", 0);
    return str_from_bytes(p, m);
}
