# Files

`burrow/os.h` is Go's `os` package, or the part of it that deals with files and paths: opening, reading, writing and closing files, looking at them with `Stat`, and renaming, linking and removing them. The names are Go's with `os_` in front, so `os.ReadFile` is `os_read_file` and the methods of `*os.File` are `os_file_read`, `os_file_close` and so on.

Reading directories, `MkdirAll` and `RemoveAll`, temporary files, the environment and processes come in later pull requests in the same milestone.

## Whole files

`os_write_file` creates the file, or truncates it if it is there, and writes everything. `os_read_file` reads a file to the end into memory from the allocator you give it:

<!-- example: ../examples/os/os.c#whole -->
```c
Error err = os_write_file(name, BURROW_B("one\ntwo\n"), 0644);
if (BURROW_FAILED(err))
    return 1;
Slice data = os_read_file(a, name, &err); /* "one\ntwo\n", from a */
```

The permission bits go through the umask as they do in Go, and Windows only looks at whether the owner can write.

## Open files

`os_open` opens for reading, `os_create` opens for reading and writing and creates or truncates, and `os_open_file` takes Go's flags, `OS_O_RDONLY`, `OS_O_WRONLY` or `OS_O_RDWR` with any of `OS_O_APPEND`, `OS_O_CREATE`, `OS_O_EXCL`, `OS_O_SYNC` and `OS_O_TRUNC`:

<!-- example: ../examples/os/os.c#file -->
```c
OsFile *f = os_open_file(a, name, OS_O_RDWR | OS_O_APPEND, 0, &err);
if (f == NULL)
    return 1;
os_file_write_string(f, BURROW_S("three\n"), &err);
os_file_seek(f, 4, OS_SEEK_SET, &err);
Byte buf[64];
Int n =
    os_file_read(f, slice_from(buf, 64, 64, TYPE_BYTE), &err); /* "two\nthree\n" */
Error cerr = os_file_close(f);
os_file_free(f);
```

Reads and writes work the way Go's do. A read at the end of the file gives `io_eof`, a write keeps going until everything is written or something fails, and `os_file_read_at` and `os_file_write_at` work at an offset without moving the one `os_file_read` uses. `os_file_write_at` on a file opened with `OS_O_APPEND` is an error, as in Go.

Closing and freeing are separate calls. `os_file_close` closes the descriptor and waits for anything still reading or writing it on another goroutine. The `OsFile` stays, so a second close or a late read gets `os_err_closed` instead of touching freed memory, or a descriptor number that the system has already given to another file. `os_file_free` closes the file if you have not and gives the memory back. Call it even when the file came from an arena, because dropping the arena would leave the descriptor open.

To pass a file to something that takes an interface, use `os_file_as_io_reader`, `os_file_as_io_writer`, `os_file_as_fs_file` and the rest. `os_stdin`, `os_stdout` and `os_stderr` are the three standard files. They are never freed, so `os_file_free` on one of them does nothing.

## Stat

`os_stat` follows symbolic links and `os_lstat` does not. Both give an `OsFileInfo`, which is io/fs's `FileInfo`, so its methods are slots:

<!-- example: ../examples/os/os.c#stat -->
```c
OsFileInfo fi = os_stat(a, name, &err);
if (BURROW_FAILED(err))
    return 1;
Str base = fi.vt->name(fi.data);      /* "notes.txt" */
int64_t size = fi.vt->size(fi.data);  /* 14 */
bool is_dir = fi.vt->is_dir(fi.data); /* false */
```

The name is the last element of the path you asked about. `os_same_file` tells you whether two `FileInfo`s are the same file, by device and inode on Unix and by volume and file index on Windows. `Sys` gives a nil `Any` for now, where Go gives a `*syscall.Stat_t`.

## Errors

A failure on a path is an `OsPathError`, which is io/fs's `PathError`, holding the operation, the path and the system's error number as a `SyscallErrno`. Rename, link and symlink fail with an `OsLinkError`, which has both names. The text is Go's:

<!-- example: ../examples/os/os.c#errors -->
```c
Error rerr = os_remove(name);  /* gone */
Error again = os_remove(name); /* remove .../notes.txt: no such file or directory */
bool missing = os_is_not_exist(again);          /* true */
bool same = errors_is(again, os_err_not_exist); /* true */
```

Go suggests `errors_is` with `os_err_not_exist`, `os_err_exist` and `os_err_permission` for new code. `os_is_not_exist` and the other older predicates are here too, and they only look one level into the error, as Go's do. Use `errors_as` with `TYPE_SYSCALL_ERRNO` when you need the number itself.
