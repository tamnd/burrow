/* encoding/pem, the text wrapping around keys and certificates that RFC 1421
 * describes, the kind that starts with -----BEGIN CERTIFICATE-----.
 *
 * Decoding the blocks in a file one after another:
 *
 *     Slice rest = data;
 *     PemBlock *b;
 *     while ((b = pem_decode(a, rest, &rest)) != NULL) {
 *         ... b->type, b->headers and b->bytes ...
 *         pem_block_free(a, b);
 *     }
 *
 * and writing one:
 *
 *     PemBlock b = {.type = BURROW_S("CERTIFICATE"), .bytes = der};
 *     Error err = pem_encode(w, &b);
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/pem */

#ifndef BURROW_ENCODING_PEM_H
#define BURROW_ENCODING_PEM_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* pem.Block. type is the word or words after BEGIN, such as RSA PRIVATE KEY.
 * headers is a map[string]string of the Key: value lines before the data, and
 * NULL is fine for none when encoding. bytes is the decoded data, usually DER.
 *
 * size is not yours. It is how big the block pem_decode allocated is, and it
 * stays zero in a block you fill in yourself. */
typedef struct PemBlock {
    Str type;
    Map *headers;
    Slice bytes;

    size_t size;
} PemBlock;

extern const Type *const TYPE_PEM_BLOCK;

/* pem.Decode. Finds the next PEM block in data and returns it, with *rest set
 * to what follows it, a piece of data. With no block to find you get NULL and *rest is all of
 * data, and that is also what an allocation failure gives you.
 *
 * The block owns its text and bytes, all from a, so data can go away while it
 * lives. headers is a map made even when there are none, as in Go. Give the
 * block back with pem_block_free, or use an arena. */
BURROW_OWNS(ret) PemBlock *pem_decode(Alloc *a, Slice data, Slice *rest);

/* Gives back a block pem_decode returned, its headers map and bytes too. The
 * block has to be the way pem_decode handed it over, since its strings are
 * part of the same allocation. NULL is fine. */
void pem_block_free(Alloc *a, PemBlock *b);

/* pem.Encode. Writes b to out. Headers come out with Proc-Type first and the
 * rest sorted, and the data is base64 in lines of 64. A header key with a
 * colon in it is an error, and nothing is written in that case. */
BURROW_STATIC(ret) Error pem_encode(IoWriter out, const PemBlock *b);

/* pem.EncodeToMemory. b encoded into a new slice from a, or the nil slice when
 * pem_encode would fail or a runs out. */
BURROW_OWNS(ret) Slice pem_encode_to_memory(Alloc *a, const PemBlock *b);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_PEM_H */
