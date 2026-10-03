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

As in Go, this does not stop a symbolic link inside the directory from pointing outside it. When that matters, use a root, which is covered below.

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

## Roots

`os_open_root` opens a directory as an `OsRoot`, and everything done through the root stays inside that directory. Names are relative to it, and a name that would leave it fails, whether it leaves through `..` or through a symbolic link that points outside. Links that stay inside are followed. The root's functions are the ones from the rest of this page with `os_root_` in front:

<!-- example: ../examples/os/root.c#openroot -->
```c
OsRoot *root = os_open_root(a, dir, &err);
err = os_root_mkdir_all(root, BURROW_S("logs/old"), 0755);
err = os_root_write_file(root, BURROW_S("logs/today.txt"), BURROW_B("started\n"),
                         0644);
Slice data = os_root_read_file(root, a, BURROW_S("logs/today.txt"), &err);
/* "started\n" */
```

A name that escapes gets a PathError wrapping "path escapes from parent". That includes a name that goes down and then back up past the top, like `logs/../../x`:

<!-- example: ../examples/os/root.c#escape -->
```c
Error out = BURROW_NO_ERROR, link_out = BURROW_NO_ERROR;
OsFile *f = os_root_open(root, a, BURROW_S("../secret.txt"), &out);
/* NULL, openat ../secret.txt: path escapes from parent */
os_root_readlink(root, a, BURROW_S("logs/../../x"), &link_out);
/* readlinkat logs/../../x: path escapes from parent */
```

`os_root_fs` gives the root as an `Fs`, with the same checks behind it. Unlike `os_dir_fs`, a link inside the directory cannot be used to read something outside it:

<!-- example: ../examples/os/root.c#rootfs -->
```c
Fs fsys = os_root_fs(root);
Slice entries = fs_read_dir(a, fsys, BURROW_S("logs"), &err);
for (Int i = 0; i < entries.len; i++) {
    FsDirEntry *e = (FsDirEntry *)slice_at(entries, i);
    Str entry = fs_format_dir_entry(a, *e); /* "d old/", then "- today.txt" */
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(entry));
}
```

`os_open_in_root(a, dir, name, &err)` is the short way to open one file this way. It opens the root, opens the file and closes the root again.

`os_root_close` closes the root. Calls that come after it fail with `os_err_closed`, and `os_root_free` gives the memory back, closing the root first if it is still open:

<!-- example: ../examples/os/root.c#close -->
```c
err = os_root_close(root); /* later calls fail with "file already closed" */
os_root_free(root);
```

On Linux, macOS and the BSDs every step goes through a directory descriptor with `openat` and its relatives, as Go does, so a directory that is renamed while you work on it cannot pull you out of the root. On Windows this first version works by name. It walks the name one component at a time, reads every link it meets and refuses anything that leaves, and then does the operation on the full path. That stops `..` and links that escape, but unlike Go's Windows version it does not hold handles open while it works, so a directory swapped for a link between the check and the operation is not caught. Go uses the same approach on the platforms that have no `openat`.

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

## Users and groups

`burrow/os/user.h` is Go's `os/user`. `user_current`, `user_lookup` and `user_lookup_id` give a `User` with the uid, the primary gid, the login name, the full name and the home directory, all as strings, as Go has them. `user_lookup_group` and `user_lookup_group_id` give a `UserGroup`, and `user_group_ids` lists the groups a user is in:

<!-- example: ../examples/os/user.c#lookup -->
```c
Error err = BURROW_NO_ERROR;
User *root = user_lookup_id(a, BURROW_S("0"), &err);
if (BURROW_FAILED(err))
    return 1;
printf(BURROW_STR_FMT " " BURROW_STR_FMT "\n", BURROW_STR_ARG(root->username),
       BURROW_STR_ARG(root->uid)); /* root 0 */

UserGroup *g = user_lookup_group_id(a, root->gid, &err);
if (BURROW_FAILED(err))
    return 1;
printf("group " BURROW_STR_FMT "\n", BURROW_STR_ARG(g->gid));
```

On Unix the answers come from the C library, through `getpwnam_r` and the rest, so they see whatever the system is set up to use, LDAP and NIS included. Building with `BURROW_OSUSERGO` reads `/etc/passwd` and `/etc/group` instead, which is what Go does with the `osusergo` build tag. On Windows ids are SIDs such as `S-1-5-18`, usernames look like `NT AUTHORITY\SYSTEM`, and the answers come from the process token, `LookupAccountName` and `NetUserGetInfo`, as in Go. An account that doesn't exist gives the system's error there, not `UnknownUserError`, which is what Go does too. `user_current` asks once and keeps the answer, so ask again with `user_lookup_id` if the process changes its uid. Each result is one allocation, freed with `user_free` or `user_group_free`.

A name or id that is not there gives one of four error types, which you can pick out with `errors_as`:

<!-- example: ../examples/os/user.c#unknown -->
```c
(void)user_lookup(a, BURROW_S("no-such-user"), &err);
if (errors_as(err, TYPE_USER_UNKNOWN_USER_ERROR) != NULL)
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
```

## Processes

`os_start_process` is Go's `StartProcess`, the low level way to run a program. It takes the program's path, the whole argument list with the name first, and an `OsProcAttr` with the directory, the environment and the files the child gets. A NULL `env` gives the child ours, and `files` is the child's descriptor table, so leaving it empty starts the child with nothing open. Most programs want os/exec, below, which finds the program on `PATH` and wires up the pipes.

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

## Running programs

`burrow/os/exec.h` is Go's `os/exec`. `exec_command` makes an `ExecCmd` for a program and its arguments, looking the name up on `PATH` when it has no separator in it, and `exec_cmd_output` runs it and gives back what it wrote to its standard output. There is no shell in between, so nothing expands globs or variables:

<!-- example: ../examples/os/exec.c#output -->
```c
ExecCmd *c = exec_command_v(a, exe, 1, BURROW_S("greet"));
Slice out = exec_cmd_output(c, a, &err);
if (BURROW_FAILED(err))
    return fail(err);
printf("%.*s", (int)out.len, (const char *)out.p); /* hello from the child */
exec_cmd_free(c);
```

`stdin_`, `stdout_` and `stderr_` can be set to any reader or writer. A file is handed to the child as it is, and anything else is copied over a pipe. Go does that copying on goroutines, and burrow does it on threads of its own, so the reader and writers you give a Cmd are used from another thread until `exec_cmd_wait` returns. Don't give it a writer that allocates from an arena something else is using at the same time:

<!-- example: ../examples/os/exec.c#stdin -->
```c
StringsReader in;
strings_reader_reset(&in, BURROW_S("some input\n"));
c = exec_command_v(a, exe, 1, BURROW_S("upper"));
c->stdin_ = strings_reader_as_io_reader(&in); /* copied to the child on a pipe */
out = exec_cmd_output(c, a, &err);
if (BURROW_FAILED(err))
    return fail(err);
printf("%.*s", (int)out.len, (const char *)out.p); /* SOME INPUT */
exec_cmd_free(c);
```

A program that exits with anything but 0 gives an `ExecExitError`. When `exec_cmd_output` was collecting the output and `stderr_` was not set, the error holds the start and the end of what the program wrote to its standard error, as Go's does:

<!-- example: ../examples/os/exec.c#exit -->
```c
c = exec_command_v(a, exe, 1, BURROW_S("fail"));
(void)exec_cmd_output(c, a, &err);
const ExecExitError *ee = errors_as(err, TYPE_EXEC_EXIT_ERROR);
if (ee != NULL) {
    printf(BURROW_STR_FMT "\n",
           BURROW_STR_ARG(error_text(err))); /* exit status 3 */
    printf("code %d, stderr %.*s",
           (int)os_process_state_exit_code(ee->process_state), (int)ee->stderr_.len,
           (const char *)ee->stderr_.p);
}
exec_cmd_free(c);
```

`exec_command_context` ties the program to a context. When the context is done before the program has exited, the Cmd's `cancel` runs, which kills the program unless you set it to something else, and `wait_delay` bounds how long Wait then waits for the program and its pipes. A context with a deadline puts its timer on the runtime, so this part has to run under `runtime_main`:

<!-- example: ../examples/os/exec.c#context -->
```c
ContextCancelFunc cancel;
Context ctx =
    context_with_timeout(a, context_background(), 100 * TIME_MILLISECOND, &cancel);
ExecCmd *c = exec_command_context_v(a, ctx, exe, 1, BURROW_S("sleep"));
Error err = exec_cmd_run(c); /* killed after a tenth of a second */
if (BURROW_FAILED(err))
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(context_err(ctx))));
exec_cmd_free(c);
BURROW_CALLF0(cancel);
context_release(ctx);
```

`exec_look_path` is the lookup on its own. A name that isn't found gives an `ExecError` wrapping `exec_err_not_found`, and one found through a relative entry in `PATH`, `.` included, wraps `exec_err_dot` and is not run, which is what Go has done since 1.19:

<!-- example: ../examples/os/exec.c#lookpath -->
```c
Str path = exec_look_path(a, BURROW_S("no-such-program"), &err);
if (errors_is(err, exec_err_not_found))
    printf("not found, path %d bytes\n", (int)path.len);
```

`exec_cmd_start` and `exec_cmd_wait` are the two halves of `exec_cmd_run`, and `exec_cmd_stdin_pipe`, `exec_cmd_stdout_pipe` and `exec_cmd_stderr_pipe` give the parent's end of a pipe to talk to the program while it runs. `exec_cmd_free` gives back everything the Cmd holds, and kills and waits for a program that was started and never waited for. Writing to a stdin pipe after the program has exited raises `SIGPIPE` on Unix, which ends the process unless it is ignored or handled, where Go's runtime turns it into an error.

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

## System constants

Go's `syscall` package has thousands of constants: address families, socket options, open flags, ioctl requests, system call numbers and the sizes of the system's structs. They are all in `burrow/syscall.h` with a `SYSCALL_` prefix, and the value is the one Go has for the system and architecture you build for, so they differ between machines the way the system's own do:

<!-- example: ../examples/syscall/consts.c#consts -->
```c
Int family = SYSCALL_AF_INET6;   /* 10 on Linux, 30 on macOS, 23 on Windows */
Int kind = SYSCALL_SOCK_STREAM;  /* 1, but 2 on Linux on MIPS */
Int proto = SYSCALL_IPPROTO_TCP; /* 6 */
```

A name Go writes in capitals keeps its spelling, so `IPV6_V6ONLY` is `SYSCALL_IPV6_V6ONLY`. The others are split into words like every other name here, so `SizeofSockaddrInet6` is `SYSCALL_SIZEOF_SOCKADDR_INET6`:

<!-- example: ../examples/syscall/consts.c#unix -->
```c
Int size = SYSCALL_SIZEOF_SOCKADDR_INET6; /* 28 */
Int call = SYSCALL_SYS_GETPID; /* 39 on Linux on amd64, 172 on arm64, 20 on macOS */
```

The values come from Go's tables, not from your system's headers. Go's `syscall` is frozen, so a few counts that newer kernels raised, such as `SYSCALL_SOMAXCONN`, are the older number, as they are in Go. `tests/syscall_zconst_test.c` compares every name that both sides define and lists the ones that differ, with the reason.
