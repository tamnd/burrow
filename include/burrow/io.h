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
 * What is here is all of Go's io: the interfaces, the sentinel errors, the
 * helpers that need nothing but an interface to do their work, and the small
 * readers and writers built out of other ones, from LimitReader to Pipe. The
 * implementations of the interfaces live where they live in Go: os.File is in
 * os, bytes.Buffer is in bytes, and this header does not know about either of
 * them.
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

/* burrow:package io */

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

/* io.ByteReader. ReadByte returns the next byte, or an error and no byte, so a
 * caller that gets an error ignores the byte that came with it. */
typedef struct IoByteReaderVT {
    const Type *self_type;
    Byte (*read_byte)(void *self, Error *err);
} IoByteReaderVT;

typedef struct IoByteReader {
    const IoByteReaderVT *vt;
    void *data;
} IoByteReader;

/* io.ByteScanner: a ByteReader that can take back the byte it just gave.
 *
 * UnreadByte undoes the most recent ReadByte, and only that. Calling it twice
 * in a row, or after anything other than a ReadByte, may be an error, and which
 * it is depends on the type. The embedded reader is a member for the same
 * reason the combinations below hold theirs. */
typedef struct IoByteScannerVT {
    IoByteReaderVT byte_reader;
    Error (*unread_byte)(void *self);
} IoByteScannerVT;

typedef struct IoByteScanner {
    const IoByteScannerVT *vt;
    void *data;
} IoByteScanner;

/* io.ByteWriter. Writes one byte, or says why it could not. */
typedef struct IoByteWriterVT {
    const Type *self_type;
    Error (*write_byte)(void *self, Byte c);
} IoByteWriterVT;

typedef struct IoByteWriter {
    const IoByteWriterVT *vt;
    void *data;
} IoByteWriter;

/* io.RuneReader. Reads one UTF-8 encoded character and reports how many bytes
 * it took through size. Go returns the rune, the size and the error, and this
 * returns the rune with the other two through pointers, the way the readers in
 * bytes and strings already do. A byte that does not start a valid encoding is
 * utf8.RuneError with a size of one, which is not an error. */
typedef struct IoRuneReaderVT {
    const Type *self_type;
    Rune (*read_rune)(void *self, Int *size, Error *err);
} IoRuneReaderVT;

typedef struct IoRuneReader {
    const IoRuneReaderVT *vt;
    void *data;
} IoRuneReader;

/* io.RuneScanner: a RuneReader that can take back the rune it just gave, with
 * the same only-the-last-one rule as ByteScanner. */
typedef struct IoRuneScannerVT {
    IoRuneReaderVT rune_reader;
    Error (*unread_rune)(void *self);
} IoRuneScannerVT;

typedef struct IoRuneScanner {
    const IoRuneScannerVT *vt;
    void *data;
} IoRuneScanner;

/* io.StringWriter. Writes the bytes of s without the caller having to turn it
 * into a slice first, which in Go saves a copy and here mostly saves typing.
 * io_write_string is how to call it on a Writer that may or may not have one. */
typedef struct IoStringWriterVT {
    const Type *self_type;
    Int (*write_string)(void *self, Str s, Error *err);
} IoStringWriterVT;

typedef struct IoStringWriter {
    const IoStringWriterVT *vt;
    void *data;
} IoStringWriter;

/* io.ReaderAt. Reads len(p) bytes starting at off in the underlying input.
 *
 * Stricter than Read in one way that matters: fewer than len(p) bytes always
 * comes with an error saying why. It does not move any offset, so any number
 * of callers can use one ReaderAt at the same time if the type says it is safe
 * to, and a file is. At the end of the input the error is io_eof, and a read
 * that fills p exactly at the end may report either io_eof or nothing. */
typedef struct IoReaderAtVT {
    const Type *self_type;
    Int (*read_at)(void *self, Slice p, int64_t off, Error *err);
} IoReaderAtVT;

typedef struct IoReaderAt {
    const IoReaderAtVT *vt;
    void *data;
} IoReaderAt;

/* io.WriterAt. Writes p at off and returns how many bytes went, with an error
 * whenever that is fewer than len(p). Writes to ranges that do not overlap may
 * run at the same time. */
typedef struct IoWriterAtVT {
    const Type *self_type;
    Int (*write_at)(void *self, Slice p, int64_t off, Error *err);
} IoWriterAtVT;

typedef struct IoWriterAt {
    const IoWriterAtVT *vt;
    void *data;
} IoWriterAt;

/* io.ReaderFrom. Reads r until it ends or fails and returns how many bytes it
 * took. The end of r is not an error. */
typedef struct IoReaderFromVT {
    const Type *self_type;
    int64_t (*read_from)(void *self, IoReader r, Error *err);
} IoReaderFromVT;

typedef struct IoReaderFrom {
    const IoReaderFromVT *vt;
    void *data;
} IoReaderFrom;

/* io.WriterTo. Writes everything it has to w and returns how many bytes that
 * was. */
typedef struct IoWriterToVT {
    const Type *self_type;
    int64_t (*write_to)(void *self, IoWriter w, Error *err);
} IoWriterToVT;

typedef struct IoWriterTo {
    const IoWriterToVT *vt;
    void *data;
} IoWriterTo;

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

typedef struct IoReadSeekerVT {
    IoReaderVT reader;
    IoSeekerVT seeker;
} IoReadSeekerVT;

typedef struct IoReadSeeker {
    const IoReadSeekerVT *vt;
    void *data;
} IoReadSeeker;

typedef struct IoReadSeekCloserVT {
    IoReaderVT reader;
    IoSeekerVT seeker;
    IoCloserVT closer;
} IoReadSeekCloserVT;

typedef struct IoReadSeekCloser {
    const IoReadSeekCloserVT *vt;
    void *data;
} IoReadSeekCloser;

typedef struct IoWriteSeekerVT {
    IoWriterVT writer;
    IoSeekerVT seeker;
} IoWriteSeekerVT;

typedef struct IoWriteSeeker {
    const IoWriteSeekerVT *vt;
    void *data;
} IoWriteSeeker;

typedef struct IoReadWriteSeekerVT {
    IoReaderVT reader;
    IoWriterVT writer;
    IoSeekerVT seeker;
} IoReadWriteSeekerVT;

typedef struct IoReadWriteSeeker {
    const IoReadWriteSeekerVT *vt;
    void *data;
} IoReadWriteSeeker;

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

IoReader io_read_seeker_as_io_reader(IoReadSeeker rs);
IoSeeker io_read_seeker_as_io_seeker(IoReadSeeker rs);

IoReader io_read_seek_closer_as_io_reader(IoReadSeekCloser rsc);
IoSeeker io_read_seek_closer_as_io_seeker(IoReadSeekCloser rsc);
IoCloser io_read_seek_closer_as_io_closer(IoReadSeekCloser rsc);

IoWriter io_write_seeker_as_io_writer(IoWriteSeeker ws);
IoSeeker io_write_seeker_as_io_seeker(IoWriteSeeker ws);

IoReader io_read_write_seeker_as_io_reader(IoReadWriteSeeker rws);
IoWriter io_read_write_seeker_as_io_writer(IoReadWriteSeeker rws);
IoSeeker io_read_write_seeker_as_io_seeker(IoReadWriteSeeker rws);

IoByteReader io_byte_scanner_as_io_byte_reader(IoByteScanner bs);
IoRuneReader io_rune_scanner_as_io_rune_reader(IoRuneScanner rs);

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

/* A read or a write on a pipe whose own end has already been closed. */
extern const Error io_err_closed_pipe;

/* ------------------------------------------------------------- the functions
 *
 * All of these take the error through a pointer, which is what a Go function
 * returning two values becomes here, and all of them accept NULL for it if you
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
 * If src has a WriteTo method, or failing that dst has a ReadFrom method, the
 * copy is handed to it and buf is not used, exactly as in Go. The section on
 * method sets at the end of this file says how a type gets one.
 *
 * buf must have room for at least one byte. A nil or empty buffer is
 * io_err_short_buffer rather than an infinite loop, where Go would allocate for
 * a nil one and panic on an empty one. There is no allocator here to allocate
 * from, and io_copy is the version that has one.
 *
 * The count is an int64_t and not an Int, which is Go's choice and is the right
 * one: a copy is the one operation in the library whose result does not fit in
 * a word on a 32 bit machine, and a file over two gigabytes is not exotic. */
int64_t io_copy_buffer(IoWriter dst, IoReader src, Slice buf, Error *err);

/* io.Copy. The same thing with the buffer taken from the allocator, 32 KiB of
 * it, which is the size Go uses, or less when src is a limited reader with
 * less than that left.
 *
 * The buffer is freed before this returns, so nothing is left behind in an
 * arena a caller is going to reset anyway, and an allocator that refuses gives
 * burrow_err_out_of_memory with nothing copied. When src has WriteTo or dst has
 * ReadFrom no buffer is allocated at all. */
int64_t io_copy(Alloc *a, IoWriter dst, IoReader src, Error *err);

/* io.CopyN. Copies n bytes, or fewer if an error comes first, and returns how
 * many it copied. The count is n exactly when there is no error. A source that
 * ends early gives io_eof, since running out before n is the one thing the
 * caller asked not to happen. */
int64_t io_copy_n(Alloc *a, IoWriter dst, IoReader src, int64_t n, Error *err);

/* io.ReadAll. Reads r until it ends and returns everything it read, in memory
 * from a. The end of the input is not an error, so a nil error means all of it
 * arrived. On any other error the bytes read before it are still returned. */
BURROW_OWNS(ret) Slice io_read_all(Alloc *a, IoReader r, Error *err);

/* io.WriteString. Writes s to w, through w's WriteString when it has one and
 * through Write otherwise. */
Int io_write_string(IoWriter w, Str s, Error *err);

/* ------------------------------------------------------------ LimitedReader
 *
 * io.LimitReader and io.LimitedReader. Reads from r and stops with io_eof
 * after n bytes, whatever r still has.
 *
 * Go returns a pointer to a fresh LimitedReader as a Reader. Here the struct is
 * returned by value and you keep it wherever suits you, on the stack for a
 * function that uses it and throws it away, which is most of them:
 *
 *     IoLimitedReader lr = io_limit_reader(src, 1024);
 *     io_copy(a, dst, io_limited_reader_as_io_reader(&lr), &err);
 *
 * Both fields are public as in Go, and reading n afterwards is how to find out
 * how much of the limit was left. */
typedef struct IoLimitedReader {
    IoReader r; /* the underlying reader */
    int64_t n;  /* bytes left before the limit */
} IoLimitedReader;

IoLimitedReader io_limit_reader(IoReader r, int64_t n);
Int io_limited_reader_read(IoLimitedReader *l, Slice p, Error *err);
IoReader io_limited_reader_as_io_reader(IoLimitedReader *l);

/* ------------------------------------------------------------ SectionReader
 *
 * io.SectionReader. Reads the n bytes of a ReaderAt that start at off, as a
 * Reader, a Seeker and a ReaderAt of their own, with offsets counted from the
 * start of the section. The ReaderAt is not moved, so any number of sections
 * over one file can be read at the same time.
 *
 * A section that would run past the largest int64_t stops there, as in Go,
 * which is what makes a section of "the rest of it" something you can ask for
 * without knowing how long the rest is. */
typedef struct IoSectionReader {
    IoReaderAt r;
    int64_t base;
    int64_t off;
    int64_t limit;
    int64_t n;
} IoSectionReader;

IoSectionReader io_new_section_reader(IoReaderAt r, int64_t off, int64_t n);
Int io_section_reader_read(IoSectionReader *s, Slice p, Error *err);
int64_t io_section_reader_seek(IoSectionReader *s, int64_t offset, Int whence,
                               Error *err);
Int io_section_reader_read_at(IoSectionReader *s, Slice p, int64_t off, Error *err);

/* The size of the section in bytes. */
int64_t io_section_reader_size(const IoSectionReader *s);

/* The ReaderAt and the two numbers the section was made with. Go returns the
 * three together and this returns the reader with the numbers through pointers,
 * either of which may be NULL. */
IoReaderAt io_section_reader_outer(const IoSectionReader *s, int64_t *off, int64_t *n);

IoReader io_section_reader_as_io_reader(IoSectionReader *s);
IoSeeker io_section_reader_as_io_seeker(IoSectionReader *s);
IoReaderAt io_section_reader_as_io_reader_at(IoSectionReader *s);
IoReadSeeker io_section_reader_as_io_read_seeker(IoSectionReader *s);

/* ------------------------------------------------------------- OffsetWriter
 *
 * io.OffsetWriter. The mirror of a section: turns writes at the current offset
 * into WriteAt calls on w, starting at off. Offsets it is given and offsets it
 * reports are counted from off. It has no end, so seeking from the end is an
 * error. */
typedef struct IoOffsetWriter {
    IoWriterAt w;
    int64_t base;
    int64_t off;
} IoOffsetWriter;

IoOffsetWriter io_new_offset_writer(IoWriterAt w, int64_t off);
Int io_offset_writer_write(IoOffsetWriter *o, Slice p, Error *err);
Int io_offset_writer_write_at(IoOffsetWriter *o, Slice p, int64_t off, Error *err);
int64_t io_offset_writer_seek(IoOffsetWriter *o, int64_t offset, Int whence,
                              Error *err);

IoWriter io_offset_writer_as_io_writer(IoOffsetWriter *o);
IoWriterAt io_offset_writer_as_io_writer_at(IoOffsetWriter *o);
IoSeeker io_offset_writer_as_io_seeker(IoOffsetWriter *o);
IoWriteSeeker io_offset_writer_as_io_write_seeker(IoOffsetWriter *o);

/* ---------------------------------------------------------------- TeeReader
 *
 * io.TeeReader. Every byte read through it from r is also written to w before
 * the read returns, and a write that fails is reported as the read failing.
 * There is no buffering, so w sees the bytes in the same chunks the reader
 * returned them in. Go's type is unexported and this one has to be named so it
 * can be kept somewhere, which is the same trade LimitedReader makes. */
typedef struct IoTeeReader {
    IoReader r;
    IoWriter w;
} IoTeeReader;

IoTeeReader io_tee_reader(IoReader r, IoWriter w);
IoReader io_tee_reader_as_io_reader(IoTeeReader *t);

/* ----------------------------------------------------------------- Discard
 *
 * io.Discard. A Writer that takes everything and keeps nothing. It has a
 * ReadFrom, so io_copy into it reads through a small buffer of its own and
 * allocates nothing, and a WriteString, so writing a Str to it costs nothing
 * either. */
extern const IoWriter io_discard;

/* ---------------------------------------------------------------- NopCloser
 *
 * io.NopCloser. A ReadCloser whose Close does nothing, for handing a plain
 * reader to something that insists on being able to close it. If r has a
 * WriteTo so does the result, which keeps io_copy's shortcut working through
 * it, as in Go.
 *
 * The value holds only r, so it is kept by value like the others above and
 * io_nop_closer_as_io_read_closer takes its address. */
typedef struct IoNopCloser {
    IoReader r;
} IoNopCloser;

IoNopCloser io_nop_closer(IoReader r);
IoReadCloser io_nop_closer_as_io_read_closer(IoNopCloser *c);

/* ------------------------------------------------ MultiReader and MultiWriter
 *
 * io.MultiReader. The concatenation of the readers, read one after another.
 * The result reports io_eof once all of them have, and any other error from
 * one of them straight away.
 *
 * io.MultiWriter. Every write goes to each writer in turn, like the tee
 * command. The first one to fail stops the write and its error is the result.
 *
 * Go copies the list, and so does this, into memory from a, which is why these
 * two take an allocator and the readers above do not. A nested one is
 * flattened as in Go. NULL for a list of zero is fine. An allocator that
 * refuses gives a nil interface value, and the free functions take what the
 * constructor returned and give the memory back, which with an arena you can
 * skip. */
IoReader io_multi_reader(Alloc *a, const IoReader *readers, Int n);
void io_multi_reader_free(Alloc *a, IoReader r);

IoWriter io_multi_writer(Alloc *a, const IoWriter *writers, Int n);
void io_multi_writer_free(Alloc *a, IoWriter w);

/* --------------------------------------------------------------------- Pipe
 *
 * io.Pipe. A synchronous in-memory pipe: every Write blocks until one or more
 * Reads have taken all of its bytes, and there is no buffer in between. It
 * connects code expecting a Reader with code expecting a Writer, with one side
 * on a goroutine and the other on another, or on a thread of your own.
 *
 * Reads and Writes are safe from any number of goroutines at once. Writes are
 * handed over whole and in order, and one Write may be split over several
 * Reads.
 *
 * Closing the writer makes Reads return io_eof, or the error given to
 * io_pipe_writer_close_with_error. Closing the reader makes Writes return
 * io_err_closed_pipe, or the error given to its close_with_error. Closing either
 * end twice is fine and the first error is the one that sticks.
 *
 * The pipe is allocated from a and io_pipe_free gives it back. Go leaves that
 * to the collector, which works out when both ends are unreachable. Here it is
 * your job, and the rule is that nothing may be using either end when you do
 * it. On an allocator that refuses, both ends come back NULL. */
typedef struct IoPipeReader IoPipeReader;
typedef struct IoPipeWriter IoPipeWriter;

void io_pipe(Alloc *a, IoPipeReader **r, IoPipeWriter **w);
void io_pipe_free(IoPipeReader *r);

Int io_pipe_reader_read(IoPipeReader *r, Slice data, Error *err);
BURROW_STATIC(ret) Error io_pipe_reader_close(IoPipeReader *r);
BURROW_STATIC(ret) Error io_pipe_reader_close_with_error(IoPipeReader *r, Error err);

Int io_pipe_writer_write(IoPipeWriter *w, Slice data, Error *err);
BURROW_STATIC(ret) Error io_pipe_writer_close(IoPipeWriter *w);
BURROW_STATIC(ret) Error io_pipe_writer_close_with_error(IoPipeWriter *w, Error err);

IoReader io_pipe_reader_as_io_reader(IoPipeReader *r);
IoCloser io_pipe_reader_as_io_closer(IoPipeReader *r);
IoReadCloser io_pipe_reader_as_io_read_closer(IoPipeReader *r);
IoWriter io_pipe_writer_as_io_writer(IoPipeWriter *w);
IoCloser io_pipe_writer_as_io_closer(IoPipeWriter *w);
IoWriteCloser io_pipe_writer_as_io_write_closer(IoPipeWriter *w);

/* -------------------------------------------------------------- method sets
 *
 * Go's io.Copy asks the reader whether it is also a WriterTo, and io.WriteString
 * asks the writer whether it is also a StringWriter. That is a type assertion
 * from one interface to another, and an IoReader's vtable only has Read in it,
 * so the question goes to the method set on the reader's descriptor instead,
 * the same way encoding finds MarshalText.
 *
 * A type that wants the shortcut lists the method with one of these
 * signatures, and the functions it already has for the job fit them:
 *
 *     int64_t buf_write_to(Buf *b, IoWriter w, Error *err);
 *
 *     #define BUF_METHODS(M, T) M(T, WriteTo, buf_write_to, IO_SIG_WRITE_TO)
 *     BURROW_STRUCT_DEFINE_METHODS(Buf, BUF_FIELDS, BUF_METHODS);
 *
 * The reader's vtable has to name that descriptor as its self_type, which is
 * what TYPE_OF(Buf) gives. bytes.Buffer, bytes.Reader, strings.Reader and
 * strings.Builder in this library have theirs. */
typedef Error *IoErrorArg;
extern const Type burrow_type_IoReader;
extern const Type burrow_type_IoWriter;
extern const Type burrow_type_IoErrorArg;

#define IO_SIG_WRITE_TO(IN, OUT) IN(0, IoWriter) IN(1, IoErrorArg) OUT(int64_t)
#define IO_SIG_READ_FROM(IN, OUT) IN(0, IoReader) IN(1, IoErrorArg) OUT(int64_t)
#define IO_SIG_WRITE_STRING(IN, OUT) IN(0, Str) IN(1, IoErrorArg) OUT(Int)

/* The assertions themselves, for packages like bufio that make the same
 * choice io_copy does. Each one calls the method and returns true when the
 * value has it, and returns false without doing anything when it does not. */
bool burrow__io_try_write_to(IoReader src, IoWriter dst, int64_t *n, Error *err);
bool burrow__io_try_read_from(IoWriter dst, IoReader src, int64_t *n, Error *err);
bool burrow__io_try_write_string(IoWriter w, Str s, Int *n, Error *err);

#if defined(BURROW_SHORT) && BURROW_SHORT
#define IO_SEEK_START BURROW_IO_SEEK_START
#define IO_SEEK_CURRENT BURROW_IO_SEEK_CURRENT
#define IO_SEEK_END BURROW_IO_SEEK_END
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IO_H */
