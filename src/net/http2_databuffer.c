/* net/http/internal/http2's databuffer.go: the chunked buffer a stream's DATA
 * frames are read into.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http2.h"

#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"

#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(burrow__http2_err_read_empty, "read from empty dataBuffer");

static Alloc *db_alloc(const Http2DataBuffer *b) {
    return b->a != NULL ? b->a : heap_allocator();
}

/* getDataBufferChunk's size classes. */
static Int db_chunk_size(int64_t size) {
    if (size <= 1 << 10)
        return 1 << 10;
    if (size <= 2 << 10)
        return 2 << 10;
    if (size <= 4 << 10)
        return 4 << 10;
    if (size <= 8 << 10)
        return 8 << 10;
    return 16 << 10;
}

static void db_put_chunk(Http2DataBuffer *b, Slice c) {
    mem_free(db_alloc(b), c.p, (size_t)c.len, 1);
}

static Slice db_bytes_from_first_chunk(const Http2DataBuffer *b) {
    Slice c = b->chunks[0];
    if (b->nchunks == 1)
        return slice_sub(c, b->r, b->w);
    return slice_sub(c, b->r, c.len);
}

Int burrow__http2_data_buffer_read(Http2DataBuffer *b, Slice p, Error *err) {
    if (b->size == 0) {
        BURROW_OUT(err, burrow__http2_err_read_empty);
        return 0;
    }
    Int ntotal = 0;
    while (p.len > 0 && b->size > 0) {
        Slice from = db_bytes_from_first_chunk(b);
        Int n = slice_copy(p, from);
        p = slice_sub(p, n, p.len);
        ntotal += n;
        b->r += n;
        b->size -= n;
        /* The first chunk is used up, so on to the next. */
        if (b->r == b->chunks[0].len) {
            db_put_chunk(b, b->chunks[0]);
            memmove(b->chunks, b->chunks + 1, (size_t)(b->nchunks - 1) * sizeof(Slice));
            b->nchunks--;
            b->r = 0;
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ntotal;
}

Int burrow__http2_data_buffer_len(const Http2DataBuffer *b) {
    return b->size;
}

/* lastChunkOrAlloc. A chunk with room left at the end, or a new one, or a
 * Slice with no p when the allocation fails. */
static Slice db_last_chunk_or_alloc(Http2DataBuffer *b, int64_t want) {
    if (b->nchunks != 0) {
        Slice last = b->chunks[b->nchunks - 1];
        if (b->w < last.len)
            return last;
    }
    Alloc *a = db_alloc(b);
    if (b->nchunks == b->chunks_cap) {
        Int ncap = b->chunks_cap == 0 ? 4 : b->chunks_cap * 2;
        Slice *nc = mem_realloc(a, b->chunks, (size_t)b->chunks_cap * sizeof(Slice),
                                (size_t)ncap * sizeof(Slice), _Alignof(Slice));
        if (nc == NULL)
            return (Slice){NULL, 0, 0, TYPE_BYTE};
        b->chunks = nc;
        b->chunks_cap = ncap;
    }
    Int size = db_chunk_size(want);
    Byte *p = mem_alloc_nozero(a, (size_t)size, 1);
    if (p == NULL)
        return (Slice){NULL, 0, 0, TYPE_BYTE};
    Slice chunk = {p, size, size, TYPE_BYTE};
    b->chunks[b->nchunks++] = chunk;
    b->w = 0;
    return chunk;
}

Int burrow__http2_data_buffer_write(Http2DataBuffer *b, Slice p, Error *err) {
    Int ntotal = 0;
    while (p.len > 0) {
        /* Make the new chunk big enough for p and what is still expected,
         * which may still be smaller than p. */
        int64_t want = b->expected > (int64_t)p.len ? b->expected : (int64_t)p.len;
        Slice chunk = db_last_chunk_or_alloc(b, want);
        if (chunk.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return ntotal;
        }
        Int n = slice_copy(slice_sub(chunk, b->w, chunk.len), p);
        p = slice_sub(p, n, p.len);
        b->w += n;
        b->size += n;
        b->expected -= n;
        ntotal += n;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ntotal;
}

void burrow__http2_data_buffer_free(Http2DataBuffer *b) {
    for (Int i = 0; i < b->nchunks; i++)
        db_put_chunk(b, b->chunks[i]);
    if (b->chunks != NULL)
        mem_free(db_alloc(b), b->chunks, (size_t)b->chunks_cap * sizeof(Slice),
                 _Alignof(Slice));
    b->chunks = NULL;
    b->nchunks = 0;
    b->chunks_cap = 0;
    b->r = 0;
    b->w = 0;
    b->size = 0;
}

static Int db_pb_len(void *self) {
    return burrow__http2_data_buffer_len((Http2DataBuffer *)self);
}

static Int db_pb_read(void *self, Slice p, Error *err) {
    return burrow__http2_data_buffer_read((Http2DataBuffer *)self, p, err);
}

static Int db_pb_write(void *self, Slice p, Error *err) {
    return burrow__http2_data_buffer_write((Http2DataBuffer *)self, p, err);
}

static const Http2PipeBufferVT db_pipe_buffer_vt = {db_pb_len, db_pb_read, db_pb_write};

Http2PipeBuffer burrow__http2_data_buffer_as_pipe_buffer(Http2DataBuffer *b) {
    Http2PipeBuffer pb = {&db_pipe_buffer_vt, b};
    return pb;
}
