/* The CPU profile's log. See burrow/profbuf.h for what it is for.
 *
 * The two ends share three words, r, w and overflow, and nothing else. w is
 * how far the writer has got and r is how far the reader has let go of, each
 * counting data words in the low 32 bits and tags from bit 34 up, which is
 * Go's profIndex. Bit 32 of w says the reader is asleep and wants waking, and
 * bit 33 that there is overflow or an eof for it to see. A count only ever
 * goes up and wraps, so the distance between two of them is a subtraction,
 * done in 30 bits for the tags because that is all they have.
 *
 * Data is written before the compare and swap that moves w past it, and the
 * reader loads w before it reads the data, so that pair is the release and the
 * acquire that make a record visible whole. The same goes the other way for r
 * and the space the reader gives back.
 *
 * Derived from Go's src/runtime/profbuf.go.
 * Go source: go1.27.1.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/profbuf.h"

#include "burrow/atomic.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/note.h"
#include "burrow/runtime.h"

#include <string.h>

#define PROF_READER_SLEEPING ((uint64_t)1 << 32)
#define PROF_WRITE_EXTRA ((uint64_t)1 << 33)

struct burrow__ProfBuf {
    /* Accessed atomically. */
    uint64_t r;
    uint64_t w;
    /* How many writes were lost, in the low 32 bits, and how many times the
     * count has been taken, above that, so that a reader taking it and a writer
     * adding to it cannot both win with the same old value. */
    uint64_t overflow;
    uint64_t overflow_time;
    uint32_t eof;

    /* Fixed once the buffer is made. */
    Int hdrsize;
    uint64_t *data;
    Int ndata;
    void **tags;
    Int ntags;

    /* The reader's own. */
    uint64_t r_next;
    uint64_t *overflow_buf;
    burrow__Note wait;
};

static uint32_t data_count(uint64_t x) {
    return (uint32_t)x;
}

static uint32_t tag_count(uint64_t x) {
    return (uint32_t)(x >> 34);
}

/* x minus y, for two counts that wrap at 1<<30, as a signed number. */
static Int count_sub(uint32_t x, uint32_t y) {
    int32_t d = (int32_t)((x - y) << 2);
    return (Int)(d >> 2);
}

/* x with data words and tag tags added to its counts and the two flags
 * cleared. */
static uint64_t add_counts_and_clear_flags(uint64_t x, Int data, Int tag) {
    uint64_t tags = (x >> 34) + (uint64_t)(((uint32_t)tag << 2) >> 2);
    return tags << 34 | (uint64_t)((uint32_t)x + (uint32_t)data);
}

static bool has_overflow(burrow__ProfBuf *b) {
    return (uint32_t)burrow__atomic_load_u64(&b->overflow) > 0;
}

/* Takes the overflow count and resets it, with the time of the first write that
 * was lost. Zero and zero when there was none. */
static uint32_t take_overflow(burrow__ProfBuf *b, uint64_t *time) {
    uint64_t overflow = burrow__atomic_load_u64(&b->overflow);
    uint64_t t = burrow__atomic_load_u64(&b->overflow_time);
    for (;;) {
        if ((uint32_t)overflow == 0) {
            t = 0;
            break;
        }
        /* Up by one in the high bits and zero in the low ones. */
        if (burrow__atomic_cas_u64(&b->overflow, &overflow,
                                   ((overflow >> 32) + 1) << 32))
            break;
        t = burrow__atomic_load_u64(&b->overflow_time);
    }
    *time = t;
    return (uint32_t)overflow;
}

/* Counts one lost write. The time is kept for the first of a run of them. */
static void increment_overflow(burrow__ProfBuf *b, int64_t now) {
    for (;;) {
        uint64_t overflow = burrow__atomic_load_u64(&b->overflow);

        /* Only the writer adds to the count and the reader can only take it
         * back to zero, so a zero here stays zero until this store. */
        if ((uint32_t)overflow == 0) {
            burrow__atomic_store_u64(&b->overflow_time, (uint64_t)now);
            burrow__atomic_store_u64(&b->overflow, (((overflow >> 32) + 1) << 32) + 1);
            break;
        }
        /* Saturated. */
        if ((int32_t)(uint32_t)overflow == -1)
            break;
        if (burrow__atomic_cas_u64(&b->overflow, &overflow, overflow + 1))
            break;
    }
}

burrow__ProfBuf *burrow__prof_buf_new(Int hdrsize, Int bufwords, Int tags) {
    Int min = 2 + hdrsize + 1;
    if (bufwords < min)
        bufwords = min;

    /* The counts are 32 bits and the tag count 30 of them, and a buffer this
     * big has no reason to be. */
    if (bufwords >= (Int)1 << 28 || tags >= (Int)1 << 28)
        runtime_throw(BURROW_S("newProfBuf: buffer too large"));

    /* Powers of two, so the index into each is the count mod the length. */
    Int i;
    for (i = 1; i < bufwords; i <<= 1) {
    }
    bufwords = i;
    for (i = 1; i < tags; i <<= 1) {
    }
    tags = i;

    Alloc *a = heap_allocator();
    burrow__ProfBuf *b = BURROW_NEW(a, burrow__ProfBuf);
    if (b == NULL)
        return NULL;
    memset(b, 0, sizeof *b);
    b->hdrsize = hdrsize;
    b->ndata = bufwords;
    b->ntags = tags;
    b->data = BURROW_NEW_N(a, uint64_t, (size_t)bufwords);
    b->tags = BURROW_NEW_N(a, void *, (size_t)tags);
    /* Transient, because the reader frees the buffer as soon as it sees the eof
     * that close's wake gave it, and that wake may not be done yet. */
    b->overflow_buf = BURROW_NEW_N(a, uint64_t, (size_t)(2 + hdrsize + 1));
    if (b->data == NULL || b->tags == NULL || b->overflow_buf == NULL ||
        !burrow__note_init_transient(&b->wait)) {
        burrow__prof_buf_free(b);
        return NULL;
    }
    memset(b->data, 0, (size_t)bufwords * sizeof *b->data);
    memset((void *)b->tags, 0, (size_t)tags * sizeof *b->tags);
    return b;
}

void burrow__prof_buf_free(burrow__ProfBuf *b) {
    if (b == NULL)
        return;

    Alloc *a = heap_allocator();
    if (b->data != NULL)
        mem_free(a, b->data, (size_t)b->ndata * sizeof *b->data, _Alignof(uint64_t));
    if (b->tags != NULL)
        mem_free(a, (void *)b->tags, (size_t)b->ntags * sizeof *b->tags,
                 _Alignof(void *));
    if (b->overflow_buf != NULL)
        mem_free(a, b->overflow_buf,
                 (size_t)(2 + b->hdrsize + 1) * sizeof *b->overflow_buf,
                 _Alignof(uint64_t));
    burrow__note_free(&b->wait);
    mem_free(a, b, sizeof *b, _Alignof(burrow__ProfBuf));
}

/* Whether there is room for one more record with an nstk word stack. */
static bool can_write_record(burrow__ProfBuf *b, Int nstk) {
    uint64_t br = burrow__atomic_load_acquire_u64(&b->r);
    uint64_t bw = burrow__atomic_load_u64(&b->w);

    /* A tag slot. */
    if (count_sub(tag_count(br), tag_count(bw)) + b->ntags < 1)
        return false;

    /* Room for the record in one piece, which may mean skipping the end of the
     * buffer and starting again at the front. */
    Int nd = count_sub(data_count(br), data_count(bw)) + b->ndata;
    Int want = 2 + b->hdrsize + nstk;
    Int i = (Int)(data_count(bw) % (uint32_t)b->ndata);
    if (i + want > b->ndata)
        nd -= b->ndata - i;
    return nd >= want;
}

/* The same for two records in a row, which is what a write needs when it has
 * an overflow record to put in front of its own. */
static bool can_write_two_records(burrow__ProfBuf *b, Int nstk1, Int nstk2) {
    uint64_t br = burrow__atomic_load_acquire_u64(&b->r);
    uint64_t bw = burrow__atomic_load_u64(&b->w);

    if (count_sub(tag_count(br), tag_count(bw)) + b->ntags < 2)
        return false;

    Int nd = count_sub(data_count(br), data_count(bw)) + b->ndata;

    Int want = 2 + b->hdrsize + nstk1;
    Int i = (Int)(data_count(bw) % (uint32_t)b->ndata);
    if (i + want > b->ndata) {
        nd -= b->ndata - i;
        i = 0;
    }
    i += want;
    nd -= want;

    want = 2 + b->hdrsize + nstk2;
    if (i + want > b->ndata) {
        /* Can't fit in trailing fragment of slice. Skip over that. Go starts i
         * over at 0 here too, but nothing reads it after. */
        nd -= b->ndata - i;
    }
    return nd >= want;
}

/* Tells a sleeping reader there is overflow or an eof to look at. */
static void wakeup_extra(burrow__ProfBuf *b) {
    for (;;) {
        uint64_t old = burrow__atomic_load_u64(&b->w);
        uint64_t next = (old | PROF_WRITE_EXTRA) & ~PROF_READER_SLEEPING;
        if (!burrow__atomic_cas_u64(&b->w, &old, next))
            continue;
        if ((old & PROF_READER_SLEEPING) != 0)
            burrow__note_wake(&b->wait);
        break;
    }
}

void burrow__prof_buf_write(burrow__ProfBuf *b, void *const *tag_ptr, int64_t now,
                            const uint64_t *hdr, Int nhdr, const uintptr_t *stk,
                            Int nstk) {
    if (b == NULL)
        return;
    if (nhdr > b->hdrsize)
        runtime_throw(BURROW_S("misuse of profBuf.write"));

    bool overflowed = has_overflow(b);
    if (overflowed && can_write_two_records(b, 1, nstk)) {
        /* Room for the overflow record and this one, so the overflow goes first,
         * with its own time, and then this. */
        uint64_t time;
        uint32_t count = take_overflow(b, &time);
        if (count > 0) {
            uintptr_t one = (uintptr_t)count;
            burrow__prof_buf_write(b, NULL, (int64_t)time, NULL, 0, &one, 1);
        }
    } else if (overflowed || !can_write_record(b, nstk)) {
        /* Either still behind with the overflow or no room for this one. Either
         * way this is lost, and counted, and the reader told. */
        increment_overflow(b, now);
        wakeup_extra(b);
        return;
    }

    /* Only the writer moves w, so the w read here is still w at the compare and
     * swap below, apart from the flags. */
    uint64_t br = burrow__atomic_load_acquire_u64(&b->r);
    uint64_t bw = burrow__atomic_load_u64(&b->w);

    Int wt = (Int)(tag_count(bw) % (uint32_t)b->ntags);
    if (tag_ptr != NULL)
        b->tags[wt] = *tag_ptr;

    Int wd = (Int)(data_count(bw) % (uint32_t)b->ndata);
    Int nd = count_sub(data_count(br), data_count(bw)) + b->ndata;
    Int skip = 0;
    if (wd + 2 + b->hdrsize + nstk > b->ndata) {
        /* A zero length says the rest of the buffer is empty and the record
         * starts again at the front. */
        b->data[wd] = 0;
        skip = b->ndata - wd;
        nd -= skip;
        wd = 0;
    }
    (void)nd;

    uint64_t *data = b->data + wd;
    data[0] = (uint64_t)(2 + b->hdrsize + nstk);
    data[1] = (uint64_t)now;
    Int i = 0;
    for (; i < nhdr; i++)
        data[2 + i] = hdr[i];
    for (; i < b->hdrsize; i++)
        data[2 + i] = 0;
    for (Int j = 0; j < nstk; j++)
        data[2 + b->hdrsize + j] = (uint64_t)stk[j];

    for (;;) {
        uint64_t old = burrow__atomic_load_u64(&b->w);
        uint64_t next =
            add_counts_and_clear_flags(old, skip + 2 + nstk + b->hdrsize, 1);

        /* Go wakes the reader only once the buffer is half full, so it reads in
         * batches rather than a record at a time. */
        Int unread =
            count_sub(data_count(next), data_count(burrow__atomic_load_u64(&b->r)));
        if (unread < 0)
            unread += b->ndata;
        Int threshold = b->ndata / 2;
        if (unread < threshold)
            next |= old & PROF_READER_SLEEPING;
        if (!burrow__atomic_cas_release_u64(&b->w, &old, next))
            continue;
        if (unread >= threshold && (old & PROF_READER_SLEEPING) != 0)
            burrow__note_wake(&b->wait);
        break;
    }
}

void burrow__prof_buf_close(burrow__ProfBuf *b) {
    if (burrow__atomic_load_u32(&b->eof) > 0)
        runtime_throw(BURROW_S("runtime: profBuf already closed"));
    burrow__atomic_store_u32(&b->eof, 1);
    wakeup_extra(b);
}

/* What an overflow record's tag reads as. */
static void *const overflow_tag[1] = {NULL};

burrow__ProfBufRead burrow__prof_buf_read(burrow__ProfBuf *b,
                                          burrow__ProfBufReadMode mode) {
    burrow__ProfBufRead out = {NULL, 0, NULL, 0, false};

    if (b == NULL) {
        out.eof = true;
        return out;
    }

    /* Give back what the last read returned. Its tags are cleared first, so a
     * label set is not kept alive by a slot nobody will read again. */
    uint64_t br = b->r_next;
    uint64_t r_prev = burrow__atomic_load_u64(&b->r);
    if (r_prev != br) {
        Int ntag = count_sub(tag_count(br), tag_count(r_prev));
        Int ti = (Int)(tag_count(r_prev) % (uint32_t)b->ntags);
        for (Int i = 0; i < ntag; i++) {
            b->tags[ti] = NULL;
            if (++ti == b->ntags)
                ti = 0;
        }
        burrow__atomic_store_release_u64(&b->r, br);
    }

    uint64_t bw;
    Int num_data;
    for (;;) {
        bw = burrow__atomic_load_acquire_u64(&b->w);
        num_data = count_sub(data_count(bw), data_count(br));
        if (num_data != 0)
            break;

        if (has_overflow(b)) {
            /* No data, but some was lost, so that is the record. */
            uint64_t time;
            uint32_t count = take_overflow(b, &time);
            if (count == 0)
                continue;
            uint64_t *dst = b->overflow_buf;
            dst[0] = (uint64_t)(2 + b->hdrsize + 1);
            dst[1] = time;
            for (Int i = 0; i < b->hdrsize; i++)
                dst[2 + i] = 0;
            dst[2 + b->hdrsize] = (uint64_t)count;
            out.data = dst;
            out.ndata = 2 + b->hdrsize + 1;
            out.tags = overflow_tag;
            out.ntags = 1;
            return out;
        }

        /* Nothing, and nothing coming. */
        if (burrow__atomic_load_u32(&b->eof) > 0) {
            out.eof = true;
            return out;
        }

        /* The writer said there was something, and the checks above have
         * seen it, so clear the flag and look again. */
        if ((bw & PROF_WRITE_EXTRA) != 0) {
            uint64_t expect = bw;
            (void)burrow__atomic_cas_u64(&b->w, &expect, bw & ~PROF_WRITE_EXTRA);
            continue;
        }

        if (mode == BURROW_PROF_BUF_NON_BLOCKING)
            return out;

        /* Say so and sleep. A write that lands between the load above and this
         * fails the compare and swap, and the loop looks again. */
        uint64_t expect = bw;
        if (!burrow__atomic_cas_u64(&b->w, &expect, bw | PROF_READER_SLEEPING))
            continue;
        burrow__note_sleep(&b->wait);
        burrow__note_clear(&b->wait);
    }

    const uint64_t *data = b->data + data_count(br) % (uint32_t)b->ndata;
    Int len = b->ndata - (Int)(data_count(br) % (uint32_t)b->ndata);
    if (len > num_data)
        len = num_data;
    else
        num_data -= len; /* what is left at the front, after a wrap */

    Int skip = 0;
    if (data[0] == 0) {
        /* The rest of the buffer was skipped, so the records start again at the
         * front. */
        skip = len;
        data = b->data;
        len = b->ndata;
        if (len > num_data)
            len = num_data;
    }

    Int ntag = count_sub(tag_count(bw), tag_count(br));
    if (ntag == 0)
        runtime_throw(
            BURROW_S("runtime: malformed profBuf buffer - tag and data out of sync"));
    void *const *tags = b->tags + tag_count(br) % (uint32_t)b->ntags;
    Int tlen = b->ntags - (Int)(tag_count(br) % (uint32_t)b->ntags);
    if (tlen > ntag)
        tlen = ntag;

    /* Whole records only, and no more of them than there are tags, which is
     * where a run of either can stop short at the end of the buffer. */
    Int di = 0;
    Int ti = 0;
    while (di < len && data[di] != 0 && ti < tlen) {
        if ((uint64_t)di + data[di] > (uint64_t)len)
            runtime_throw(BURROW_S("runtime: malformed profBuf buffer - invalid size"));
        di += (Int)data[di];
        ti++;
    }

    /* Kept back until the next read, so what is returned now stays put. */
    b->r_next = add_counts_and_clear_flags(br, skip + di, ti);

    out.data = data;
    out.ndata = di;
    out.tags = tags;
    out.ntags = ti;
    return out;
}

bool burrow__prof_buf_reader_sleeping(burrow__ProfBuf *b) {
    return (burrow__atomic_load_u64(&b->w) & PROF_READER_SLEEPING) != 0;
}
