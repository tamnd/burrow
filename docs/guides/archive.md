# Archives

## tar

`burrow/archive/tar.h` is Go's `archive/tar`. A tar archive is a run of files, each one a 512 byte header and then its contents padded out to a whole block, with two zero blocks at the end. A writer takes a header and then exactly that many bytes for each file:

<!-- example: ../examples/archive/tar.c#write -->
```c
BytesBuffer buf = BYTES_BUFFER(a);
TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&buf));
if (tw == NULL)
    return 1;
static const char *const files[][2] = {
    {"readme.txt", "This archive contains some text files."},
    {"gopher.txt", "Gopher names:\nGeorge\nGeoffrey\nGonzo"},
    {"todo.txt", "Get animal handling license."},
};
for (int i = 0; i < 3; i++) {
    Str body = str_from_cstr(files[i][1]);
    TarHeader hdr = {
        .name = str_from_cstr(files[i][0]),
        .mode = 0600,
        .size = body.len,
    };
    err = tar_writer_write_header(tw, &hdr);
    if (BURROW_FAILED(err))
        return 1;
    tar_writer_write(tw, slice_from_str(a, body), &err);
    if (BURROW_FAILED(err))
        return 1;
}
err = tar_writer_close(tw);
tar_writer_free(tw);
```

That makes 4096 bytes, the same as Go: a header block and a data block for each of the three files, and the two blocks that end the archive. `tar_writer_write` gives `tar_err_write_too_long` if a file gets more than its header's `size`, and a header written before the last file is full gives an error. `tar_writer_close` does not close the writer underneath, so the buffer is still there to read.

A reader hands out one header at a time, and in between it reads as the contents of the current file:

<!-- example: ../examples/archive/tar.c#read -->
```c
TarReader *tr = tar_new_reader(a, bytes_buffer_as_io_reader(&buf));
if (tr == NULL)
    return 1;
for (;;) {
    TarHeader *hdr = tar_reader_next(tr, &err);
    if (errors_is(err, io_eof))
        break;
    if (BURROW_FAILED(err))
        return 1;
    printf("Contents of " BURROW_STR_FMT ":\n", BURROW_STR_ARG(hdr->name));
    Slice data = io_read_all(a, tar_reader_as_io_reader(tr), &err);
    printf("%.*s\n", (int)data.len, (const char *)data.p);
}
tar_reader_free(tr);
```

That prints each name and its text. The header and every string in it come from the reader's allocator, and belong to the caller, who can give them back with `tar_header_free` or let an arena take care of it. Whatever is left unread of a file is skipped by the next `tar_reader_next`, with a seek when the input has a `Seek` method, as a `BytesReader` does. A sparse file reads with its holes filled with zeros.

Archives come in four formats: the original V7 layout, USTAR, PAX and GNU. The reader reads all of them, including GNU's long names and the old and new GNU sparse maps, and sets `format` to the one it found. The writer picks the oldest format that can hold the header, so a plain file goes out as USTAR, and a field too big for USTAR makes it add a PAX header in front. Setting `format` makes it use that format or fail:

<!-- example: ../examples/archive/tar.c#format -->
```c
TarHeader big = {.name = BURROW_S("big"), .uid = 1 << 21, .size = 0};
tw = tar_new_writer(a, io_discard);
err = tar_writer_write_header(tw, &big);
printf("PAX ok: %s\n", BURROW_OK(err) ? "yes" : "no");
big.format = TAR_FORMAT_USTAR;
err = tar_writer_write_header(tw, &big);
printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
tar_writer_free(tw);
```

The first header works because a uid past what USTAR's octal field holds goes into a PAX record. The second asks for USTAR, and the error says why it cannot have it, word for word as Go's does. The writer carries on after a header error, so the next header can still go in.

To go between headers and files on disk, `tar_header_file_info` gives an `FsFileInfo` for a header and `tar_file_info_header` makes a header from one. A directory's name gets its slash back on the way in:

<!-- example: ../examples/archive/tar.c#fileinfo -->
```c
TarHeader dir = {
    .typeflag = TAR_TYPE_DIR, .name = BURROW_S("src/cmd/"), .mode = 0755};
FsFileInfo fi = tar_header_file_info(&dir);
printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(fs_format_file_info(a, fi)));
TarHeader *back = tar_file_info_header(a, fi, (Str){NULL, 0}, &err);
printf(BURROW_STR_FMT " %c\n", BURROW_STR_ARG(back->name), back->typeflag);
```

That prints `drwxr-xr-x 0 0001-01-01 00:00:00 cmd/` and then `cmd/ 5`. To fill in the owner's names, give the file info's type `Uname` and `Gname` methods with the shape `TAR_SIG_FILE_INFO_NAME`, which is Go's `FileInfoNames`. `tar_writer_add_fs` adds every file and directory in an `Fs` in one call, as Go's `Writer.AddFS` does.

As in Go, `GODEBUG=tarinsecurepath=0` makes `tar_reader_next` give `tar_err_insecure_path` along with the header for a name that is absolute or climbs out with `..`, so a program unpacking an archive can refuse it. The next call carries on.

## zip

`burrow/archive/zip.h` is Go's `archive/zip`. A ZIP archive keeps each file's contents behind a small local header, and then a central directory at the end lists every file again with where it starts. So a writer can stream, but a reader has to start at the end, and needs an `IoReaderAt` and the archive's size rather than a plain reader. A writer hands out an `IoWriter` for each file in turn, and compresses with DEFLATE unless the header says otherwise:

<!-- example: ../examples/archive/zip.c#write -->
```c
BytesBuffer buf = BYTES_BUFFER(a);
ZipWriter *zw = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
if (zw == NULL)
    return 1;
static const char *const files[][2] = {
    {"readme.txt", "This archive contains some text files."},
    {"gopher.txt", "Gopher names:\nGeorge\nGeoffrey\nGonzo"},
    {"todo.txt", "Get animal handling licence.\nWrite more examples."},
};
for (int i = 0; i < 3; i++) {
    IoWriter f = zip_writer_create(zw, str_from_cstr(files[i][0]), &err);
    if (BURROW_FAILED(err))
        return 1;
    io_write_string(f, str_from_cstr(files[i][1]), &err);
    if (BURROW_FAILED(err))
        return 1;
}
err = zip_writer_close(zw);
zip_writer_free(zw);
```

Opening the next file, or closing the writer, finishes the one before it, so there is nothing to close per file. `zip_writer_close` writes the central directory and does not close the writer underneath. `zip_writer_create_header` takes a whole `ZipFileHeader`, for a method, a time or a mode, and copies it, so the same header on the stack can be filled in again for the next file. `zip_writer_create_raw` writes data that is compressed already, and `zip_writer_copy` moves a file from one archive to another without decompressing it.

A reader reads the directory up front, and `file` is then the list of every file in the order the archive has them:

<!-- example: ../examples/archive/zip.c#read -->
```c
BytesReader br;
bytes_reader_reset(&br, bytes_buffer_bytes(&buf));
ZipReader *zr = zip_new_reader(a, bytes_reader_as_io_reader_at(&br),
                               bytes_buffer_len(&buf), &err);
if (zr == NULL)
    return 1;
for (Int i = 0; i < zr->file.len; i++) {
    ZipFile *f = ((ZipFile **)zr->file.p)[i];
    printf("Contents of " BURROW_STR_FMT " (%d bytes, method %d):\n",
           BURROW_STR_ARG(f->file_header.name),
           (int)f->file_header.uncompressed_size64, f->file_header.method);
    IoReadCloser rc = zip_file_open(f, a, &err);
    if (BURROW_FAILED(err))
        return 1;
    Slice data = io_read_all(a, (IoReader){&rc.vt->reader, rc.data}, &err);
    rc.vt->closer.close(rc.data);
    if (BURROW_FAILED(err))
        return 1;
    printf("%.*s\n", (int)data.len, (const char *)data.p);
}
```

Each `zip_file_open` gives a reader of its own, so several files can be read at once, and it checks the CRC-32 at the end, giving `zip_err_checksum` when the contents do not match it. `zip_open_reader` opens a file on disk and reads its directory in one go, and `zip_read_closer_close` closes it. Archives of 4 GiB and more, files that big, and more than 65535 files all go through the ZIP64 records, which the reader follows and the writer adds only when it has to.

A reader is also an `Fs`, with a directory tree made out of the names in the archive, so the rest of `io/fs` works on it:

<!-- example: ../examples/archive/zip.c#fs -->
```c
Fs fsys = zip_reader_as_fs(zr);
Slice todo = fs_read_file(a, fsys, BURROW_S("todo.txt"), &err);
printf("%d bytes in todo.txt\n", (int)todo.len);
fs_read_file(a, fsys, BURROW_S("missing.txt"), &err);
printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
zip_reader_free(zr);
```

That prints `49 bytes in todo.txt` and then `open missing.txt: file does not exist`, the same error as Go's. In the other direction `zip_writer_add_fs` puts every file and directory of an `Fs` into an archive. Names that would be unsafe to unpack are left out of the `Fs` view, and with `GODEBUG=zipinsecurepath=0`, as in Go, `zip_new_reader` gives `zip_err_insecure_path` along with the reader when the archive has one, so a program can choose to refuse it.

Methods other than store and DEFLATE can be added with `zip_register_compressor` and `zip_register_decompressor` for the whole process, or on a single writer or reader with `zip_writer_register_compressor` and `zip_reader_register_decompressor`, which is how a writer can use a different DEFLATE level than the default.
