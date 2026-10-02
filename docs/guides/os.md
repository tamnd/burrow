# Files

`burrow/os.h` is Go's `os` package, or the part of it that deals with files and paths: opening, reading, writing and closing files, looking at them with `Stat`, and renaming, linking and removing them. The names are Go's with `os_` in front, so `os.ReadFile` is `os_read_file` and the methods of `*os.File` are `os_file_read`, `os_file_close` and so on.

It also has the environment, the working directory, the process and user ids, the user's home, cache and config directories, and pipes. Starting and waiting for other processes comes in a later pull request in the same milestone.

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

An `OsFile` has Go's `ReadFrom` and `WriteTo` methods, so `io_copy` finds them when a file is on either end. `os_file_read_from` and `os_file_write_to` can also be called directly. Go hands some of these copies to `copy_file_range`, `splice` or `sendfile` on Linux. Here they always go through a buffer for now.

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

## Directories

`os_mkdir_temp` makes a new directory with a name nobody else has, and `os_create_temp` does the same for a file. The last `*` in the pattern is where the random part goes, and an empty directory means `os_temp_dir`, which is `$TMPDIR` or `/tmp` on Unix and `GetTempPath` on Windows:

<!-- example: ../examples/os/dir.c#temp -->
```c
Str dir = os_mkdir_temp(a, BURROW_S(""), BURROW_S("example-*"), &err);
if (BURROW_FAILED(err))
    return 1;
/* dir is something like /tmp/example-2851094712 */
```

`os_mkdir_all` makes a directory and any parents it needs, and does nothing if the directory is already there. If a file is in the way you get an error that says so:

<!-- example: ../examples/os/dir.c#mkdirall -->
```c
Str deep = fmt_sprintf_v(a, "%s%ca%cb%cc", dir, (Int)OS_PATH_SEPARATOR,
                         (Int)OS_PATH_SEPARATOR, (Int)OS_PATH_SEPARATOR);
err = os_mkdir_all(deep, 0755);         /* makes a, a/b and a/b/c */
Error again = os_mkdir_all(deep, 0755); /* nil, it is already there */
```

`os_read_dir` gives the entries of a directory sorted by name, as a slice of io/fs `FsDirEntry`. On an open directory, `os_file_read_dir`, `os_file_readdir` and `os_file_readdirnames` read it in batches the way Go's methods do: with n above zero you get at most n entries and `io_eof` at the end, and with n at zero or below you get the rest in one go. The entries, and the names in them, come from the allocator you pass, so an arena is the easy choice:

<!-- example: ../examples/os/dir.c#readdir -->
```c
Slice entries = os_read_dir(a, dir, &err);
for (Int i = 0; i < entries.len; i++) {
    FsDirEntry *e = (FsDirEntry *)slice_at(entries, i);
    Str entry = fs_format_dir_entry(a, *e); /* "d a/", then "- notes.txt" */
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(entry));
}
```

`os_dir_fs` turns a directory into an `Fs`, so the io/fs functions work on the real file system. Names are slash-separated and checked the way io/fs wants, so `..` and absolute paths are refused instead of escaping the directory:

<!-- example: ../examples/os/dir.c#dirfs -->
```c
Fs fsys = os_dir_fs(a, dir);
Slice data = fs_read_file(a, fsys, BURROW_S("notes.txt"), &err); /* "hello\n" */
Error out = BURROW_NO_ERROR;
fs_read_file(a, fsys, BURROW_S("../notes.txt"), &out);
/* readfile ../notes.txt: invalid argument */
```

As in Go, this does not stop a symbolic link inside the directory from pointing outside it.

`os_copy_fs` goes the other way and copies everything in an `Fs` into a directory, which it makes if it has to. Files are created with `OS_O_EXCL`, so copying over files that are already there fails and leaves them alone. Symbolic links are copied as links:

<!-- example: ../examples/os/dir.c#copyfs -->
```c
Str parent = os_mkdir_temp(a, BURROW_S(""), BURROW_S("example-*"), &err);
Str dst = fmt_sprintf_v(a, "%s%ccopy", parent, (Int)OS_PATH_SEPARATOR);
err = os_copy_fs(dst, os_dir_fs(a, dir)); /* dst/a/b/c and dst/notes.txt */
Error twice = os_copy_fs(dst, os_dir_fs(a, dir));
/* open .../copy/notes.txt: file exists */
```

`os_remove_all` removes a path and everything under it. A path that is not there is not an error:

<!-- example: ../examples/os/dir.c#removeall -->
```c
err = os_remove_all(dir); /* dir and everything under it */
```

## The environment and the process

`os_getenv`, `os_lookup_env`, `os_setenv` and `os_unsetenv` work on the environment the way Go's do. On Unix the environment is copied once, the first time you ask, and changes go to that copy and to the C library's, so C code in the same program sees them too. `os_expand_env` replaces `$NAME` and `${NAME}` with their values:

<!-- example: ../examples/os/env.c#env -->
```c
Error err = os_setenv(BURROW_S("GREETING"), BURROW_S("hello"));
bool found = false;
Str g = os_lookup_env(a, BURROW_S("GREETING"), &found); /* "hello", true */
Str msg = os_expand_env(a, BURROW_S("$GREETING, ${USER_NAME}!"));
/* "hello, !" since USER_NAME is not set */
err = os_unsetenv(BURROW_S("GREETING"));
```

`os_getwd` and `os_chdir` get and set the working directory, and `os_file_chdir` moves into a directory you have open:

<!-- example: ../examples/os/env.c#wd -->
```c
Str wd = os_getwd(a, &err);
Str tmp = os_temp_dir(a);
err = os_chdir(tmp); /* relative names now start from tmp */
Error back = os_chdir(wd);
```

`os_pipe` gives two connected files. It returns the reading end and puts the writing end in `w`. What goes into `w` comes out of the reader, and once `w` is closed the reader gets `io_eof`:

<!-- example: ../examples/os/env.c#pipe -->
```c
OsFile *w = NULL;
OsFile *r = os_pipe(a, &w, &err);
if (BURROW_FAILED(err))
    return 1;
os_file_write_string(w, BURROW_S("through the pipe"), &err);
Error cerr = os_file_close(w); /* the reader sees EOF after the data */
Byte buf[32];
Int n = os_file_read(r, slice_from(buf, 32, 32, TYPE_BYTE), &err);
```

`os_args`, `os_getpid`, `os_getppid`, `os_getuid` and the rest of the ids, `os_hostname`, `os_user_home_dir`, `os_user_cache_dir` and `os_user_config_dir` are there too, with the same answers and the same error texts as Go.

## Processes

`os_start_process` is Go's `StartProcess`, the low level way to run a program. It takes the program's path, the whole argument list with the name first, and an `OsProcAttr` with the directory, the environment and the files the child gets. A NULL `env` gives the child ours, and `files` is the child's descriptor table, so leaving it empty starts the child with nothing open. Most programs want os/exec, which finds the program on `PATH` and wires up the pipes, once that package is here.

`os_process_wait` waits for the child to finish and gives an `OsProcessState`, which says how it went:

<!-- example: ../examples/os/proc.c#start -->
```c
Error err = BURROW_NO_ERROR;
Str exe = os_executable(a, &err); /* the path of this program */
Str argv[] = {exe};
Str env[] = {BURROW_S("EXAMPLE_CHILD=1")};
OsProcAttr attr = {
    .env = slice_from(env, 1, 1, NULL),
    .files = {0}, /* nothing open in the child */
};
OsProcess *p = os_start_process(a, exe, slice_from(argv, 1, 1, NULL), &attr, &err);
if (BURROW_FAILED(err))
    return 1;
OsProcessState *ps = os_process_wait(p, &err);
Str how = os_process_state_string(ps, a);  /* "exit status 3" */
Int code = os_process_state_exit_code(ps); /* 3 */
bool ok = os_process_state_success(ps);    /* false */
```

`os_process_kill` and `os_process_signal` send a signal, and `os_interrupt` and `os_kill` are Go's two portable ones. `os_signal_from_syscall` wraps any other `SYSCALL_SIGx`. Windows can only kill. Once a process has been waited for, signalling it gives `os_err_process_done`, and after `os_process_release` it gives an error too, so a pid the system has handed to someone else is never signalled by mistake:

<!-- example: ../examples/os/proc.c#kill -->
```c
OsProcess *self = os_find_process(a, os_getpid(), &err);
Error rerr = os_process_release(self); /* self no longer refers to the process */
Error kerr = os_process_kill(self);    /* "os: process already released" on Unix */
```

`os_find_process` gives a process from a pid, `os_executable` gives the path of the running program, and `os_process_state_user_time` and `os_process_state_system_time` give the CPU time the child used. `os_process_free` and `os_process_state_free` give the memory back.

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
