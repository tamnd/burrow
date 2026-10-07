/* golang.org/x/net/http2/hpack, the copy Go vendors.
 *
 * HPACK, the header compression of HTTP/2 (RFC 7541): a Decoder that turns a
 * header block into fields as the bytes arrive, an Encoder that writes fields
 * out against its own dynamic table, and the Huffman code both of them use.
 * net/http's HTTP/2 is built on this, as Go's is. It is an internal of
 * burrow's because it is an internal of Go's.
 *
 * Go hands each decoded field to a func and the strings in it are the
 * garbage collector's to keep alive. Here the emit function gets a context
 * pointer next to the field, and the field's name and value are good until
 * that call returns: they point into the bytes being decoded, into the
 * decoder's buffers or into its table, and any of those can change by the
 * next field. Copy what you keep. decode_full does that for you, into the
 * allocator it is given.
 *
 * The tables own copies of what they hold, from the Alloc the decoder or the
 * encoder was made with, and give them back as entries are evicted. The
 * encoder writes to an IoWriter.
 *
 * Errors are Go's. A DecodingError reads "decoding error: " and then the error
 * it holds, and does not unwrap, as in Go. hpack_err_string_length and
 * hpack_err_invalid_huffman are Go's ErrStringLength and ErrInvalidHuffman.
 * Running out of memory, which Go has no error for, is
 * burrow_err_out_of_memory.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package xnet/hpack */

#ifndef BURROW_SRC_XNET_HPACK_H
#define BURROW_SRC_XNET_HPACK_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

/* HeaderField. A name and a value, both bytes the package does not look
 * inside. sensitive means the field is never to be indexed. */
typedef struct HpackHeaderField {
    Str name;
    Str value;
    bool sensitive;
} HpackHeaderField;

/* IsPseudo: whether the name starts with a colon. */
bool burrow__hpack_header_field_is_pseudo(HpackHeaderField f);

/* String, which is `header field "name" = "value"`, and " (sensitive)" after
 * it for a sensitive field. */
BURROW_OWNS(ret) Str burrow__hpack_header_field_string(Alloc *a, HpackHeaderField f);

/* Size, the size of an entry per RFC 7541 section 4.1: the two lengths and 32.
 * It wraps for a field too big to come off the wire, as Go's does. */
uint32_t burrow__hpack_header_field_size(HpackHeaderField f);

/* An array of fields, which is what decode_full gives back. */
typedef struct HpackHeaderFields {
    HpackHeaderField *p;
    Int len;
    Int cap;
} HpackHeaderFields;

/* Gives back an array from decode_full: the names, the values and the array.
 * With an arena there is nothing to give back. */
void burrow__hpack_header_fields_free(Alloc *a, HpackHeaderFields *fs);

/* Bytes a decoder or an encoder owns and grows, from its Alloc. A buffer that
 * could not grow says so in failed and stays as it was. */
typedef struct burrow__HpackBuf {
    Byte *p;
    Int len;
    Int cap;
    bool failed;
} burrow__HpackBuf;

/* ---------------------------------------------------------------- errors */

/* ErrStringLength and ErrInvalidHuffman. */
extern const Error burrow__hpack_err_string_length;
extern const Error burrow__hpack_err_invalid_huffman;

/* Go's errNeedMore, which never leaves the package. */
extern const Error burrow__hpack_err_need_more;

/* Go's errVarintOverflow, a DecodingError holding "varint integer overflow".
 * Go's tests compare with it, so it is one value here too. */
extern const Error burrow__hpack_err_varint_overflow;

/* Whether err is a DecodingError, and the error inside it. */
bool burrow__hpack_error_decoding(Error err, Error *inner);

/* Whether err is an InvalidIndexError, and its index. Go's is an int made from
 * the uint64 off the wire, so an index past the top of an int64 comes out
 * negative, as it does in Go. */
bool burrow__hpack_error_invalid_index(Error err, Int *index);

/* ---------------------------------------------------------------- tables */

/* headerFieldTable. Entries go in at the end and come out at the start, which
 * Go does by copying the slice down and this does with a ring. Each entry has
 * an id, from one, that does not change as older ones are evicted, and the
 * maps keep the id of the newest entry with a name and with a name and value.
 *
 * The static table does not use this: it is a sorted constant, and searching
 * it is a binary search. is_static is for Go's TestHeaderFieldTable, which
 * swaps a table in for the static one to check how ids become indexes. */
typedef struct HpackTable {
    Alloc *a;
    HpackHeaderField *ents; /* a ring of cap, from head */
    Int head;
    Int len;
    Int cap;
    uint64_t evict_count;
    Map *by_name;       /* name -> id */
    Map *by_name_value; /* the length of the name, the name and the value -> id */
    Byte *key;          /* room to build a by_name_value key in */
    Int key_cap;
    bool is_static;
} HpackTable;

/* init. False when the maps cannot be made. */
bool burrow__hpack_table_init(HpackTable *t, Alloc *a);
void burrow__hpack_table_free(HpackTable *t);

/* t.ents[k], oldest first. */
BURROW_BORROWS(ret, t) const HpackHeaderField *
burrow__hpack_table_at(const HpackTable *t, Int k);

/* addEntry. The table keeps a copy of f. False when it could not. */
bool burrow__hpack_table_add(HpackTable *t, HpackHeaderField f);

/* evictOldest. Panics, as Go does, if n is more than there are. */
void burrow__hpack_table_evict_oldest(HpackTable *t, Int n);

/* search: the HPACK index of the newest entry matching f by name and value,
 * with *name_value_match set, or else by name alone, or 0. A sensitive field
 * only ever matches by name. */
uint64_t burrow__hpack_table_search(HpackTable *t, HpackHeaderField f,
                                    bool *name_value_match);

/* The same search in the static table. */
uint64_t burrow__hpack_static_search(HpackHeaderField f, bool *name_value_match);

/* dynamicTable. */
typedef struct HpackDynamicTable {
    HpackTable table;
    uint32_t size;             /* in bytes */
    uint32_t max_size;         /* the current max */
    uint32_t allowed_max_size; /* max_size may go up to this */
} HpackDynamicTable;

void burrow__hpack_dynamic_table_set_max_size(HpackDynamicTable *dt, uint32_t v);
bool burrow__hpack_dynamic_table_add(HpackDynamicTable *dt, HpackHeaderField f);

/* --------------------------------------------------------------- decoder */

typedef void (*HpackEmitFunc)(void *ctx, HpackHeaderField f);

/* Decoder. Make one with new_decoder and leave the fields alone. */
typedef struct HpackDecoder {
    HpackDynamicTable dyn_tab;
    Alloc *a;
    HpackEmitFunc emit;
    void *emit_ctx;
    Int max_str_len; /* 0 is no limit */
    /* What Write is working through, which is the caller's bytes or save's. */
    const Byte *buf;
    Int buf_len;
    /* The start of a field that ended before the bytes did, kept for the next
     * Write. Go's saveBuf. */
    burrow__HpackBuf save;
    /* Where a Huffman name and value are decoded to, and where a name from
     * the dynamic table is copied to before the field goes into the table. */
    burrow__HpackBuf str[3];
    bool emit_enabled;
    bool first_field;
} HpackDecoder;

/* NewDecoder. emit may be NULL if every write goes through decode_full, which
 * sets its own. NULL when a cannot give the memory. */
BURROW_OWNS(ret) HpackDecoder *
burrow__hpack_new_decoder(Alloc *a, uint32_t max_dynamic_table_size, HpackEmitFunc emit,
                          void *ctx);
void burrow__hpack_decoder_free(HpackDecoder *d);

/* SetMaxStringLength: the longest a name or a value may be, after any Huffman
 * decoding, or 0 for no limit, which is what a new decoder has. Past it,
 * Write fails with hpack_err_string_length. */
void burrow__hpack_decoder_set_max_string_length(HpackDecoder *d, Int n);

/* SetEmitFunc. It does not change whether emitting is enabled. */
void burrow__hpack_decoder_set_emit_func(HpackDecoder *d, HpackEmitFunc emit,
                                         void *ctx);

/* SetEmitEnabled and EmitEnabled. A decoder with emitting off still decodes
 * and keeps its table in step, which is how a server applies
 * MAX_HEADER_LIST_SIZE without decoding strings it will throw away. */
void burrow__hpack_decoder_set_emit_enabled(HpackDecoder *d, bool v);
bool burrow__hpack_decoder_emit_enabled(const HpackDecoder *d);

void burrow__hpack_decoder_set_max_dynamic_table_size(HpackDecoder *d, uint32_t v);

/* SetAllowedMaxDynamicTableSize: the most a size update in the stream may set
 * the table to. */
void burrow__hpack_decoder_set_allowed_max_dynamic_table_size(HpackDecoder *d,
                                                              uint32_t v);

/* at: the field at HPACK index i, static first and then dynamic, newest
 * first. False for 0 and past the end. */
bool burrow__hpack_decoder_at(const HpackDecoder *d, uint64_t i, HpackHeaderField *hf);

/* Write. Decodes what it can of p and keeps the rest of a field that p ends
 * in the middle of. Returns len(p), or 0 with ErrStringLength when what is
 * kept would be too long for any string that could be allowed. */
Int burrow__hpack_decoder_write(HpackDecoder *d, Slice p, Error *err);

/* Close: the end of a header block. An error if a field was left unfinished.
 * The decoder is ready for the next block either way. */
Error burrow__hpack_decoder_close(HpackDecoder *d);

/* DecodeFull: Write and Close on a whole block, with the fields copied into
 * a. The emit function in place before is put back after. */
BURROW_OWNS(ret) HpackHeaderFields burrow__hpack_decoder_decode_full(HpackDecoder *d,
                                                                     Alloc *a, Slice p,
                                                                     Error *err);

/* readVarInt: the integer at the start of p, with an n bit prefix, where n
 * is 1 to 8. *used is how many bytes it took, and 0 on an error, which is
 * hpack_err_need_more when p stops in the middle of it. */
uint64_t burrow__hpack_read_var_int(Byte n, const Byte *p, Int len, Int *used,
                                    Error *err);

/* --------------------------------------------------------------- encoder */

/* Encoder. Make one with new_encoder and leave the fields alone. */
typedef struct HpackEncoder {
    HpackDynamicTable dyn_tab;
    Alloc *a;
    IoWriter w;
    burrow__HpackBuf buf;
    /* The smallest size SetMaxDynamicTableSize was given since the last size
     * update went out. */
    uint32_t min_size;
    /* The most the table may be set to. */
    uint32_t max_size_limit;
    /* Whether the next field needs a size update in front of it. */
    bool table_size_update;
} HpackEncoder;

/* NewEncoder. The encoded fields go to w, which may be the zero IoWriter for
 * an encoder that is never written with. */
BURROW_OWNS(ret) HpackEncoder *burrow__hpack_new_encoder(Alloc *a, IoWriter w);
void burrow__hpack_encoder_free(HpackEncoder *e);

/* WriteField: f in one write to w, with a size update first if one is due. */
Error burrow__hpack_encoder_write_field(HpackEncoder *e, HpackHeaderField f);

/* SetMaxDynamicTableSize, held to the limit below. */
void burrow__hpack_encoder_set_max_dynamic_table_size(HpackEncoder *e, uint32_t v);
uint32_t burrow__hpack_encoder_max_dynamic_table_size(const HpackEncoder *e);

/* SetMaxDynamicTableSizeLimit, 4096 to start with. A table bigger than v is
 * cut down to it, and the next field says so. */
void burrow__hpack_encoder_set_max_dynamic_table_size_limit(HpackEncoder *e,
                                                            uint32_t v);

/* searchTable: the static table, and the dynamic one if that has no full
 * match. */
uint64_t burrow__hpack_encoder_search_table(HpackEncoder *e, HpackHeaderField f,
                                            bool *name_value_match);

/* The pieces WriteField is made of. Each appends to dst, in a, and returns
 * it. */
Slice burrow__hpack_append_var_int(Alloc *a, Slice dst, Byte n, uint64_t i);
Slice burrow__hpack_append_hpack_string(Alloc *a, Slice dst, Str s);
Slice burrow__hpack_append_indexed(Alloc *a, Slice dst, uint64_t i);
Slice burrow__hpack_append_new_name(Alloc *a, Slice dst, HpackHeaderField f,
                                    bool indexing);
Slice burrow__hpack_append_indexed_name(Alloc *a, Slice dst, HpackHeaderField f,
                                        uint64_t i, bool indexing);
Slice burrow__hpack_append_table_size(Alloc *a, Slice dst, uint32_t v);
Byte burrow__hpack_encode_type_byte(bool indexing, bool sensitive);

/* --------------------------------------------------------------- Huffman */

/* HuffmanDecode: v decoded and written to w in one write. Returns what the
 * write did. */
Int burrow__hpack_huffman_decode(IoWriter w, Slice v, Error *err);

/* HuffmanDecodeToString. */
BURROW_OWNS(ret) Str burrow__hpack_huffman_decode_to_string(Alloc *a, Slice v,
                                                            Error *err);

/* huffmanDecode: v decoded into out, which has room for cap bytes, and the
 * length in *n. With max_len above 0, a string longer than that is
 * hpack_err_string_length. Decoding never makes more bytes than v has times
 * 8 / 5, since no code is shorter than 5 bits, so cap = len * 8 / 5 + 1 is
 * always enough. */
Error burrow__hpack_huffman_decode_into(Byte *out, Int cap, Int *n, Int max_len,
                                        const Byte *v, Int len);

/* AppendHuffmanString and HuffmanEncodeLength. */
Slice burrow__hpack_append_huffman_string(Alloc *a, Slice dst, Str s);
uint64_t burrow__hpack_huffman_encode_length(Str s);

/* ---------------------------------------------------------- the constants */

enum {
    BURROW__HPACK_STATIC_LEN = 61,
    BURROW__HPACK_STATIC_NAMES = 52,
    BURROW__HPACK_TRIE_NODES = 15,
    /* A trie entry with this bit is an internal node, numbered in the low
     * bits. Without it, a nonzero entry is a leaf: the code length in the
     * high byte and the symbol in the low one. Zero is no code. */
    BURROW__HPACK_TRIE_NODE = 0x8000,
};

typedef struct burrow__HpackStaticName {
    Str name;
    uint8_t first; /* the HPACK indexes of the entries with this name */
    uint8_t last;
} burrow__HpackStaticName;

extern const uint32_t burrow__hpack_huffman_codes[256];
extern const uint8_t burrow__hpack_huffman_code_len[256];
extern const uint16_t burrow__hpack_trie[BURROW__HPACK_TRIE_NODES][256];
extern const HpackHeaderField burrow__hpack_static[BURROW__HPACK_STATIC_LEN];
extern const burrow__HpackStaticName
    burrow__hpack_static_names[BURROW__HPACK_STATIC_NAMES];

#endif
