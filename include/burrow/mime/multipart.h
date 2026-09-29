/* mime/multipart, the multipart bodies of RFC 2046.
 *
 * A multipart body is a list of parts, each with its own headers, split by a
 * boundary line. HTTP file uploads use it, as multipart/form-data, and so does
 * mail. MultipartReader reads the parts one at a time, or the whole form at
 * once with multipart_reader_read_form. MultipartWriter writes them:
 *
 *     MultipartWriter *w = multipart_new_writer(a, out);
 *     multipart_writer_write_field(w, BURROW_S("name"), BURROW_S("gopher"));
 *     IoWriter f = multipart_writer_create_form_file(w, BURROW_S("photo"),
 *                                                    BURROW_S("me.png"), &err);
 *     io_write_string(f, png, &err);
 *     err = multipart_writer_close(w);
 *     Str ct = multipart_writer_form_data_content_type(w, a);
 *     multipart_writer_free(w);
 *
 * Limits, as in Go: a part may have at most 10000 headers, and
 * multipart_reader_read_form takes at most 1000 parts and 10000 headers across
 * all its files. The GODEBUG settings multipartmaxheaders and multipartmaxparts
 * change them, and multipartfiles=distinct gives every file on disk its own
 * temporary file.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package mime/multipart */

#ifndef BURROW_MIME_MULTIPART_H
#define BURROW_MIME_MULTIPART_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/net/textproto.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* multipart.ErrMessageTooLarge: the form or a part's headers went over a
 * limit. */
extern const Error multipart_err_message_too_large;

/* ------------------------------------------------------------------ reading */

/* multipart.Part. header holds the part's headers, with the keys in canonical
 * form. Reading a part gives its body, with a quoted-printable body decoded
 * unless the part came from multipart_reader_next_raw_part, in which case the
 * Content-Transfer-Encoding header is left alone too. */
typedef struct MultipartPart MultipartPart;

extern const Type *const TYPE_MULTIPART_PART;

/* The part's headers. They belong to the part. */
BURROW_BORROWS(ret, p) TextprotoMIMEHeader
multipart_part_header(const MultipartPart *p);

/* Part.FormName. The name parameter of a Content-Disposition of form-data, or
 * the empty string. The result belongs to the part. */
BURROW_BORROWS(ret, p) Str multipart_part_form_name(MultipartPart *p);

/* Part.FileName. The last element of the filename parameter of the
 * Content-Disposition, with the directories taken off the way filepath.Base
 * does, or the empty string. The result belongs to the part. */
BURROW_BORROWS(ret, p) Str multipart_part_file_name(MultipartPart *p);

/* Part.Read. At the end of the part the error is io_eof. */
Int multipart_part_read(MultipartPart *p, Slice d, Error *err);

/* Part.Close. Reads what is left of the part and throws it away. */
BURROW_STATIC(ret) Error multipart_part_close(MultipartPart *p);

IoReader multipart_part_as_io_reader(MultipartPart *p);
IoReadCloser multipart_part_as_io_read_closer(MultipartPart *p);

/* multipart.Reader. */
typedef struct MultipartReader MultipartReader;

extern const Type *const TYPE_MULTIPART_READER;

/* multipart.NewReader. Reads the parts of r, split by boundary, which is the
 * boundary parameter of the Content-Type. Everything the reader and its parts
 * need comes from a. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, r) MultipartReader *
multipart_new_reader(Alloc *a, IoReader r, Str boundary);

/* Frees the reader and the part it last handed out. */
void multipart_reader_free(MultipartReader *r);

/* Reader.NextPart. The next part, or NULL with io_eof after the last one. The
 * part belongs to the reader and lasts until the next call to next_part,
 * next_raw_part or multipart_reader_free, so copy out anything you want to
 * keep. Moving on reads what is left of the part before it. */
BURROW_BORROWS(ret, r) MultipartPart *multipart_reader_next_part(MultipartReader *r,
                                                                 Error *err);

/* Reader.NextRawPart. The same, without decoding a quoted-printable body. */
BURROW_BORROWS(ret, r) MultipartPart *multipart_reader_next_raw_part(MultipartReader *r,
                                                                     Error *err);

/* multipart.FileHeader: a file part of a form. The body is in memory when it
 * was small enough and in a temporary file when it was not. */
typedef struct MultipartFileHeader {
    Str filename;
    TextprotoMIMEHeader header;
    int64_t size;

    Slice content;       /* the body, when it is in memory */
    const char *tmpfile; /* the temporary file, when it is not */
    int64_t tmpoff;      /* where the body starts in tmpfile */
    bool tmpshared;      /* tmpfile holds other bodies too */
} MultipartFileHeader;

extern const Type *const TYPE_MULTIPART_FILE_HEADER;

/* multipart.Form. value maps each field name to a Slice of Str, its values in
 * the order they came. file maps each file field name to a Slice of
 * MultipartFileHeader pointers. */
typedef struct MultipartForm {
    Map *value;
    Map *file;
} MultipartForm;

extern const Type *const TYPE_MULTIPART_FORM;

/* Reader.ReadForm. Reads the whole body as multipart/form-data. File bodies
 * that fit in max_memory bytes, together, stay in memory, and the rest go to
 * temporary files. Everything in the form comes from the reader's allocator,
 * and multipart_form_free gives it back. On an error the result is NULL and any
 * temporary files are removed. */
BURROW_OWNS(ret) MultipartForm *
multipart_reader_read_form(MultipartReader *r, int64_t max_memory, Error *err);

/* Form.RemoveAll. Removes the temporary files of the form. */
BURROW_STATIC(ret) Error multipart_form_remove_all(MultipartForm *f);

/* Frees the form. It does not remove the temporary files, so call
 * multipart_form_remove_all first. */
void multipart_form_free(MultipartForm *f);

/* multipart.File: an open file part. It reads, reads at an offset, seeks and
 * closes, like Go's interface of the same name. */
typedef struct MultipartFile MultipartFile;

extern const Type *const TYPE_MULTIPART_FILE;

/* FileHeader.Open. A new MultipartFile from a, which multipart_file_close
 * frees. */
BURROW_OWNS(ret) MultipartFile *
multipart_file_header_open(const MultipartFileHeader *fh, Alloc *a, Error *err);

Int multipart_file_read(MultipartFile *f, Slice p, Error *err);
Int multipart_file_read_at(MultipartFile *f, Slice p, int64_t off, Error *err);
int64_t multipart_file_seek(MultipartFile *f, int64_t offset, Int whence, Error *err);

/* Closes the file and frees it. */
BURROW_STATIC(ret) Error multipart_file_close(MultipartFile *f);

IoReader multipart_file_as_io_reader(MultipartFile *f);
IoReaderAt multipart_file_as_io_reader_at(MultipartFile *f);
IoSeeker multipart_file_as_io_seeker(MultipartFile *f);
IoReadSeekCloser multipart_file_as_io_read_seek_closer(MultipartFile *f);

/* ------------------------------------------------------------------ writing */

/* multipart.Writer. */
typedef struct MultipartWriter MultipartWriter;

extern const Type *const TYPE_MULTIPART_WRITER;

/* multipart.NewWriter. Writes to w, with a random boundary of 60 hex
 * digits. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, w) MultipartWriter *
multipart_new_writer(Alloc *a, IoWriter w);

void multipart_writer_free(MultipartWriter *w);

/* Writer.Boundary. The result belongs to the writer. */
BURROW_BORROWS(ret, w) Str multipart_writer_boundary(const MultipartWriter *w);

/* Writer.SetBoundary. Replaces the random boundary, which has to happen before
 * the first part. A boundary is 1 to 70 characters from RFC 2046's set, and
 * may not end in a space. */
BURROW_STATIC(ret) Error multipart_writer_set_boundary(MultipartWriter *w,
                                                       Str boundary);

/* Writer.FormDataContentType. The Content-Type for the body, with the boundary
 * quoted when it has to be. */
BURROW_OWNS(ret) Str multipart_writer_form_data_content_type(const MultipartWriter *w,
                                                             Alloc *a);

/* Writer.CreatePart. Starts a part with the headers in header, sorted by key,
 * and gives a writer for its body, which is good until the next part or
 * multipart_writer_close. header may be NULL. */
IoWriter multipart_writer_create_part(MultipartWriter *w, TextprotoMIMEHeader header,
                                      Error *err);

/* Writer.CreateFormFile. A part for a file, of type application/octet-stream. */
IoWriter multipart_writer_create_form_file(MultipartWriter *w, Str fieldname,
                                           Str filename, Error *err);

/* Writer.CreateFormField. A part for a field. */
IoWriter multipart_writer_create_form_field(MultipartWriter *w, Str fieldname,
                                            Error *err);

/* Writer.WriteField. A part for a field, with value as its body. */
BURROW_STATIC(ret) Error multipart_writer_write_field(MultipartWriter *w, Str fieldname,
                                                      Str value);

/* Writer.Close. Ends the last part and writes the closing boundary. */
BURROW_STATIC(ret) Error multipart_writer_close(MultipartWriter *w);

/* multipart.FileContentDisposition. The Content-Disposition for a file field,
 * with backslashes and quotes escaped and CR and LF percent encoded. */
BURROW_OWNS(ret) Str multipart_file_content_disposition(Alloc *a, Str fieldname,
                                                        Str filename);

/* Not API. Uses value as GODEBUG from now on, for tests, or with NULL goes
 * back to reading the environment. */
void burrow__multipart_godebug_set(const char *value);

/* Not API. Makes multipart_reader_read_form put its temporary files in dir, a
 * string that has to outlive the reader, instead of the system's directory. */
void burrow__multipart_reader_set_temp_dir(MultipartReader *r, const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MIME_MULTIPART_H */
