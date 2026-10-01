/* golang.org/x/text/unicode/bidi, the copy Go vendors.
 *
 * The bidi class of every rune and the Unicode Bidirectional Algorithm, UAX #9.
 * Go's standard library only reaches it through secure/bidirule, which the IDNA
 * code needs, so like transform.h and norm.h it is one of burrow's internals.
 *
 * Everything the package has is here under burrow__bidi_ names. Two things
 * change shape on the way to C. An Option is a value rather than a function,
 * since DefaultDirection is the only one there is. And an Ordering owns its
 * memory: Order and Line hand back a fresh one each call, which the caller
 * frees with burrow__bidi_ordering_free, where Go's would share arrays with the
 * Paragraph. Nothing in the API can tell the two apart.
 *
 * Go's code panics in a few places, Direction on an Ordering with no runs and
 * Line given a range the paragraph does not have among them. This panics in the
 * same places with the same text. Line can also read past the end of the
 * paragraph's classes, up to the capacity Go's appends left them with, so the
 * Paragraph keeps those capacities to know where the panic comes.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package xtext/unicode/bidi */

#ifndef BURROW_SRC_XTEXT_BIDI_H
#define BURROW_SRC_XTEXT_BIDI_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdint.h>

/* Class is the bidi class of a rune, Go's uint. */
typedef Uint BidiClass;

enum {
    BIDI_L,       /* LeftToRight */
    BIDI_R,       /* RightToLeft */
    BIDI_EN,      /* EuropeanNumber */
    BIDI_ES,      /* EuropeanSeparator */
    BIDI_ET,      /* EuropeanTerminator */
    BIDI_AN,      /* ArabicNumber */
    BIDI_CS,      /* CommonSeparator */
    BIDI_B,       /* ParagraphSeparator */
    BIDI_S,       /* SegmentSeparator */
    BIDI_WS,      /* WhiteSpace */
    BIDI_ON,      /* OtherNeutral */
    BIDI_BN,      /* BoundaryNeutral */
    BIDI_NSM,     /* NonspacingMark */
    BIDI_AL,      /* ArabicLetter */
    BIDI_CONTROL, /* Control LRO - PDI */

    BIDI_NUM_CLASS,

    BIDI_LRO, /* LeftToRightOverride */
    BIDI_RLO, /* RightToLeftOverride */
    BIDI_LRE, /* LeftToRightEmbedding */
    BIDI_RLE, /* RightToLeftEmbedding */
    BIDI_PDF, /* PopDirectionalFormat */
    BIDI_LRI, /* LeftToRightIsolate */
    BIDI_RLI, /* RightToLeftIsolate */
    BIDI_FSI, /* FirstStrongIsolate */
    BIDI_PDI, /* PopDirectionalIsolate */
};

#define BIDI_UNKNOWN_CLASS (~(BidiClass)0)

/* Direction, Go's int. */
typedef Int BidiDirection;

enum {
    /* All runes are left to right. */
    BIDI_LEFT_TO_RIGHT,
    /* All runes are right to left. */
    BIDI_RIGHT_TO_LEFT,
    /* Both directions are present. */
    BIDI_MIXED,
    /* No direction, as in an empty string. */
    BIDI_NEUTRAL,
};

/* ------------------------------------------------------------ properties */

/* What the tables say about a rune. */
typedef struct BidiProperties {
    uint8_t entry;
    uint8_t last;
} BidiProperties;

/* Lookup and LookupString: the properties of the first rune in s and its
 * width. The width is 1 for invalid UTF-8 and 0 for an encoding s cuts short.
 * s must not be empty, and an empty one panics the way Go's does. */
BidiProperties burrow__bidi_lookup(Slice s, Int *size);
BidiProperties burrow__bidi_lookup_string(Str s, Int *size);
BidiProperties burrow__bidi_lookup_rune(Rune r, Int *size);

BidiClass burrow__bidi_class(BidiProperties p);
bool burrow__bidi_is_bracket(BidiProperties p);
bool burrow__bidi_is_opening_bracket(BidiProperties p);
/* reverseBracket, which Go keeps to itself and its tests use. */
Rune burrow__bidi_reverse_bracket(BidiProperties p, Rune r);

/* ----------------------------------------------------------- the ordering */

/* A Run is a stretch of runes in one direction. Its runes belong to the
 * Ordering it came from. */
typedef struct BidiRun {
    const Rune *runes;
    Int len;
    BidiDirection direction;
    Int startpos;
} BidiRun;

/* String and Bytes are UTF-8, in memory from a. */
BURROW_OWNS(ret) Str burrow__bidi_run_string(Alloc *a, const BidiRun *r);
BURROW_OWNS(ret) Slice burrow__bidi_run_bytes(Alloc *a, const BidiRun *r);
BidiDirection burrow__bidi_run_direction(const BidiRun *r);
/* Pos: the positions of the first and last rune, as rune offsets. */
void burrow__bidi_run_pos(const BidiRun *r, Int *start, Int *end);

/* An Ordering is the runs of a paragraph or a line, in logical order. The
 * zero value has no runs. */
typedef struct BidiOrdering {
    Alloc *a;
    BidiRun *runs;
    Int nruns;
    Rune *runes;
    Int nrunes;
} BidiOrdering;

void burrow__bidi_ordering_free(BidiOrdering *o);
BidiDirection burrow__bidi_ordering_direction(const BidiOrdering *o);
Int burrow__bidi_ordering_num_runs(const BidiOrdering *o);
BURROW_BORROWS(ret, o) BidiRun burrow__bidi_ordering_run(const BidiOrdering *o, Int i);

/* ----------------------------------------------------------- the paragraph */

/* An Option for SetBytes and SetString. Go's are functions, and the only one
 * there is sets the direction a paragraph with no strong runes takes. */
typedef struct BidiOption {
    BidiDirection default_direction;
} BidiOption;

BidiOption burrow__bidi_default_direction(BidiDirection d);

/* A Paragraph holds a paragraph of text. Set it up with
 * burrow__bidi_paragraph_init and release it with burrow__bidi_paragraph_free.
 * The rest is private. */
typedef struct BidiParagraph {
    Alloc *a;
    BidiOrdering o;

    Int nopts;
    BidiDirection opt; /* the last option given, which is the one that wins */
    BidiDirection default_direction;

    BidiClass *types;
    uint8_t *pair_types;
    Rune *pair_values;
    Int ntypes;
    /* The capacities Go's appends would have left on the three. */
    Int types_cap, pair_types_cap, pair_values_cap;

    Rune *runes;
    Int nrunes;
} BidiParagraph;

void burrow__bidi_paragraph_init(BidiParagraph *p, Alloc *a);
void burrow__bidi_paragraph_free(BidiParagraph *p);

/* SetBytes and SetString: the text stops at the first paragraph separator,
 * and the return is how many bytes of it were taken, separator included. The
 * only error is running out of memory. */
Int burrow__bidi_paragraph_set_bytes(BidiParagraph *p, Slice b, const BidiOption *opts,
                                     Int nopts, Error *err);
Int burrow__bidi_paragraph_set_string(BidiParagraph *p, Str s, const BidiOption *opts,
                                      Int nopts, Error *err);

bool burrow__bidi_paragraph_is_left_to_right(const BidiParagraph *p);
BidiDirection burrow__bidi_paragraph_direction(const BidiParagraph *p);
/* RunAt, which reads the Ordering the last Order call made. */
BURROW_BORROWS(ret, p) BidiRun burrow__bidi_paragraph_run_at(const BidiParagraph *p,
                                                             Int pos);

/* Order and Line. The Ordering is the caller's to free. */
BURROW_OWNS(ret) BidiOrdering burrow__bidi_paragraph_order(BidiParagraph *p,
                                                           Error *err);
BURROW_OWNS(ret) BidiOrdering burrow__bidi_paragraph_line(BidiParagraph *p, Int start,
                                                          Int end, Error *err);

/* AppendReverse and ReverseString: in reversed, with each bracket swapped for
 * its pair. AppendReverse puts it after a copy of out. */
BURROW_OWNS(ret) Slice burrow__bidi_append_reverse(Alloc *a, Slice out, Slice in);
BURROW_OWNS(ret) Str burrow__bidi_reverse_string(Alloc *a, Str s);

/* ------------------------------------------------------- the core, for tests */

enum {
    BIDI_BP_NONE,
    BIDI_BP_OPEN,
    BIDI_BP_CLOSE,
};

#define BIDI_IMPLICIT_LEVEL ((int8_t)-1)

typedef struct BidiCore BidiCore;

/* newParagraph: types, pair_types and pair_values have n elements each, or
 * npt and npv where they differ, and a NULL pair_values is Go's nil. The
 * result is the caller's to free. */
BURROW_OWNS(ret) BidiCore *burrow__bidi_new_core(Alloc *a, const BidiClass *types,
                                                 Int n, const uint8_t *pair_types,
                                                 Int npt, const Rune *pair_values,
                                                 Int npv, int8_t level, Error *err);
void burrow__bidi_core_free(BidiCore *p);
int8_t burrow__bidi_core_embedding_level(const BidiCore *c);
/* getLevels and getReordering, for line breaks at the nbreaks offsets in
 * linebreaks. The results have one element per class and are the caller's. */
BURROW_OWNS(ret) int8_t *burrow__bidi_core_levels(const BidiCore *p,
                                                  const Int *linebreaks, Int nbreaks);
BURROW_OWNS(ret) Int *burrow__bidi_core_reordering(const BidiCore *p,
                                                   const Int *linebreaks, Int nbreaks);
Int burrow__bidi_core_len(const BidiCore *c);

#endif /* BURROW_SRC_XTEXT_BIDI_H */
