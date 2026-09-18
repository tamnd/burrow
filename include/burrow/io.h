/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* io: Reader, Writer, and the interfaces everything that moves bytes speaks.
 *
 * This is the smallest useful package in Go and one of the most important,
 * because io.Reader is the type that lets a gzip decompressor read from a file,
 * a socket, a string or a test fixture without knowing that any of those exist.
 * Nothing else in the library works until this does.
 *
 * What is here is the interfaces, the sentinel errors and the four functions
 * that need nothing but an interface to do their work. The implementations live
 * where they live in Go: os.File is in os, bytes.Buffer is in bytes, and this
 * header does not know about either of them.
 *
 * Implementing one is a vtable and a constructor, both of which you write once
 * next to your type:
 *
 *     static Int counter_read(void *self, Slice p, Error *err) {
 *         Counter *c = (Counter *)self;
 *         ...
 *     }
 *
 *     static const IoReaderVT counter_reader_vt = {TYPE_COUNTER, counter_read};
 *
 *     IoReader counter_as_io_reader(Counter *c) {
 *         IoReader r = {&counter_reader_vt, c};
 *         return r;
 *     }
 *
 * The vtable is const and static, so it costs a few words of read only memory
 * per type rather than per value, and the constructor compiles to two moves.
 * The naming is rule R10 in docs/design/08-naming-abi.md: an adapter from a
 * concrete type to an interface is <type>_as_<interface>.
 *
 * Reading is the part people get wrong, in Go as well, so the contract is worth
 * stating once. A Read that returns n greater than zero has read n bytes and
 * they are yours whatever the error says, so handle n first and the error
 * second. Read returning io_eof means end of input and is not a failure. A Read
 * may return fewer bytes than the buffer holds for any reason at all and that is
 * not an error either, which is why io_read_full exists.
 */

#ifndef BURROW_IO_H
#define BURROW_IO_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ the interfaces
 *
 * Every one of these is a vtable pointer and a data pointer, every vtable
 * starts with self_type, and a zeroed value is nil. Those three rules are
 * explained once in burrow/iface.h and are not repeated per interface here. */

/* io.Reader.
 *
 * Read fills as much of p as it can and returns how many bytes it wrote there.
 * Zero bytes and no error is allowed but is discouraged, exactly as in Go: it
 * means nothing happened and a caller in a loop will simply ask again. The end
 * of the input is io_eof through err, and a reader that has bytes and then hits
 * the end may report both at once or report the end on the next call. */
typedef struct IoReaderVT {
    const Type *self_type;
    Int (*read)(void *self, Slice p, Error *err);
} IoReaderVT;

typedef struct IoReader {
    const IoReaderVT *vt;
    void *data;
} IoReader;

/* io.Writer.
 *
 * Write must write all of p or return an error saying why it did not. That is
 * the opposite of Read and it is the single most useful thing about the pair,
 * because it means a caller never loops on a write. A writer that returns n
 * less than p's length without an error has broken the contract, and the
 * helpers here report io_err_short_write when they catch one doing it. */
typedef struct IoWriterVT {
    const Type *self_type;
    Int (*write)(void *self, Slice p, Error *err);
} IoWriterVT;

typedef struct IoWriter {
    const IoWriterVT *vt;
    void *data;
} IoWriter;

/* io.Closer. Closing twice is undefined in Go and is whatever the concrete type
 * does here too, so close once and check the error, particularly on a writer
 * where the flush happens in Close and the disk full arrives there. */
typedef struct IoCloserVT {
    const Type *self_type;
    Error (*close)(void *self);
} IoCloserVT;

typedef struct IoCloser {
    const IoCloserVT *vt;
    void *data;
} IoCloser;

/* Where a seek counts from. Go's io.SeekStart, io.SeekCurrent and io.SeekEnd,
 * with the same values, because they are the same values as lseek's SEEK_SET,
 * SEEK_CUR and SEEK_END and every operating system already agrees on them. */
#define BURROW_IO_SEEK_START 0
#define BURROW_IO_SEEK_CURRENT 1
#define BURROW_IO_SEEK_END 2

/* io.Seeker. The result is the new offset from the start of the file, so a seek
 * of zero from the current position is how you ask where you are. Seeking
 * before the start is an error and seeking past the end is not. */
typedef struct IoSeekerVT {
    const Type *self_type;
    int64_t (*seek)(void *self, int64_t offset, int whence, Error *err);
} IoSeekerVT;

typedef struct IoSeeker {
    const IoSeekerVT *vt;
    void *data;
} IoSeeker;

/* ------------------------------------------------------------- the combinations
 *
 * Go builds these by embedding, and so does this, by holding the embedded
 * vtables as named members rather than by repeating their function pointers.
 * One vtable of each kind exists per type no matter how many combinations that
 * type satisfies, and converting down is taking the address of a member.
 *
 * The alternative, a vtable laid out so that a pointer to it can be cast to a
 * pointer to the embedded one, works for whichever interface happens to be
 * first and silently does not for the second. The argument is in iface.h and in
 * docs/design/04-core-types.md section 5. */

typedef struct IoReadWriterVT {
    IoReaderVT reader;
    IoWriterVT writer;
} IoReadWriterVT;

typedef struct IoReadWriter {
    const IoReadWriterVT *vt;
    void *data;
} IoReadWriter;

typedef struct IoReadCloserVT {
    IoReaderVT reader;
    IoCloserVT closer;
} IoReadCloserVT;

typedef struct IoReadCloser {
    const IoReadCloserVT *vt;
    void *data;
} IoReadCloser;

typedef struct IoWriteCloserVT {
    IoWriterVT writer;
    IoCloserVT closer;
} IoWriteCloserVT;

typedef struct IoWriteCloser {
    const IoWriteCloserVT *vt;
    void *data;
} IoWriteCloser;

typedef struct IoReadWriteCloserVT {
    IoReaderVT reader;
    IoWriterVT writer;
    IoCloserVT closer;
} IoReadWriteCloserVT;

typedef struct IoReadWriteCloser {
    const IoReadWriteCloserVT *vt;
    void *data;
} IoReadWriteCloser;

/* Go's implicit conversion from a combination to one of its parts, written out.
 *
 * In Go you pass an io.ReadWriter to a function taking an io.Reader and the
 * compiler builds the narrower value for you. C has no such step, so these do
 * it, and they are cheap enough that it does not matter: each one is the
 * address of a member and a copy of the data pointer, and a nil value in gives
 * a nil value out.
 *
 *     io_copy(a, w, io_read_writer_as_io_reader(rw), &err);
 */
IoReader io_read_writer_as_io_reader(IoReadWriter rw);
IoWriter io_read_writer_as_io_writer(IoReadWriter rw);

IoReader io_read_closer_as_io_reader(IoReadCloser rc);
IoCloser io_read_closer_as_io_closer(IoReadCloser rc);

IoWriter io_write_closer_as_io_writer(IoWriteCloser wc);
IoCloser io_write_closer_as_io_closer(IoWriteCloser wc);

IoReader io_read_write_closer_as_io_reader(IoReadWriteCloser rwc);
IoWriter io_read_write_closer_as_io_writer(IoReadWriteCloser rwc);
IoCloser io_read_write_closer_as_io_closer(IoReadWriteCloser rwc);

/* There is no conversion from one combination to another, no
 * io_read_write_closer_as_io_read_writer, and the absence is deliberate. Those
 * two exist, so the value is two words and has nowhere to keep a vtable, and a
 * function cannot return a pointer to one it made on its stack. Pretending the
 * first two members of the wider vtable line up with the narrower one is
 * exactly the layout assumption this design exists to avoid.
 *
 * The answer is the same one Go uses without saying so: go back to the concrete
 * type. os.File has an adapter for each interface it satisfies, so ask it for
 * the one you need rather than whittling down a value you already narrowed. */

/* ---------------------------------------------------------------- the errors
 *
 * Go's io errors, with Go's messages, because these travel: a program prints
 * one, somebody searches for the text, and the answers they find should be
 * about the same condition. */

/* The end of the input. Not a failure, and the one error in Go that is returned
 * on the happy path more often than anything else is returned at all. Functions
 * that read a known amount report io_err_unexpected_eof instead, because the
 * input ending halfway through a record is a failure. */
extern const Error io_eof;

/* The input ended in the middle of something that was supposed to be whole. */
extern const Error io_err_unexpected_eof;

/* A Writer wrote less than it was given and did not say why. Somebody's Write
 * has a bug, and this is what says so rather than a silent short file. */
extern const Error io_err_short_write;

/* A buffer passed to a read helper is smaller than the amount being asked for,
 * which is a caller mistake and is caught before any reading happens. */
extern const Error io_err_short_buffer;

/* A Reader returned zero bytes and no error enough times in a row that whoever
 * was calling it gave up. Nothing in this file produces it. It is declared here
 * because Go declares it here, and because the package that does produce it,
 * bufio, has to name something that callers of any reader can compare
 * against. */
extern const Error io_err_no_progress;

/* ------------------------------------------------------------- the functions
 *
 * All four take the error through a pointer, which is what a Go function
 * returning two values becomes here, and all four accept NULL for it if you
 * genuinely do not want to know. They can afford to, because each of them holds
 * the error itself while it loops and only copies it out at the end.
 *
 * A vtable method is the other way round and may assume the Error * it is
 * handed is not NULL, since nothing in the library ever calls one without a
 * place to put the answer. */

/* io.ReadFull. Reads exactly as many bytes as buf holds.
 *
 * Returns the number read, which is buf's length when there is no error. An
 * input that ends before a single byte arrives gives io_eof, and one that ends
 * partway through gives io_err_unexpected_eof. That distinction is the whole
 * point of the function: the first means there was no next record and the
 * second means the file is truncated. */
Int io_read_full(IoReader r, Slice buf, Error *err);

/* io.ReadAtLeast. Reads until at least min bytes are in buf, or an error. Fills
 * more than min when the reader offers more and the buffer has room, because
 * throwing away bytes that have already arrived would mean reading them again.
 *
 * A min larger than buf is io_err_short_buffer and reads nothing. */
Int io_read_at_least(IoReader r, Slice buf, Int min, Error *err);

/* io.CopyBuffer. Copies from src to dst through buf until src reports the end,
 * and returns how many bytes made it across.
 *
 * io_eof is where the copy stops and is not reported as an error, which is Go's
 * rule and is why a copy that succeeds returns no error at all. Any other read
 * error and every write error stops the copy and comes back through err, with
 * the count of what had already been written by then.
 *
 * buf must have room for at least one byte. A nil or empty buffer is
 * io_err_short_buffer rather than an infinite loop.
 *
 * The count is an int64_t and not an Int, which is Go's choice and is the right
 * one: a copy is the one operation in the library whose result does not fit in
 * a word on a 32 bit machine, and a file over two gigabytes is not exotic. */
int64_t io_copy_buffer(IoWriter dst, IoReader src, Slice buf, Error *err);

/* io.Copy. The same thing with the buffer taken from the allocator, 32 KiB of
 * it, which is the size Go uses.
 *
 * The buffer is freed before this returns, so nothing is left behind in an
 * arena a caller is going to reset anyway, and an allocator that refuses gives
 * burrow_err_out_of_memory with nothing copied.
 *
 * Go's io.Copy asks whether either side implements ReaderFrom or WriterTo and
 * hands the work over when one does, which is how copying a file to a socket
 * becomes one sendfile call. That needs a type assertion from one interface to
 * another, which needs the method set on the descriptor, and the descriptors do
 * not carry method sets yet. When they do, this gets the same shortcut and the
 * signature does not change. */
int64_t io_copy(Alloc *a, IoWriter dst, IoReader src, Error *err);

#if defined(BURROW_SHORT) && BURROW_SHORT
#define IO_SEEK_START BURROW_IO_SEEK_START
#define IO_SEEK_CURRENT BURROW_IO_SEEK_CURRENT
#define IO_SEEK_END BURROW_IO_SEEK_END
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IO_H */
