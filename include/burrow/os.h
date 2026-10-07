/* os, files and the file system the program is running on.
 *
 *     Error err = BURROW_NO_ERROR;
 *     OsFile *f = os_create(a, BURROW_S("notes.txt"), &err);
 *     if (BURROW_FAILED(err))
 *         return err;
 *     os_file_write_string(f, BURROW_S("hello\n"), &err);
 *     Error cerr = os_file_close(f);
 *     os_file_free(f);
 *
 *     Slice data = os_read_file(a, BURROW_S("notes.txt"), &err);
 *
 * The errors are Go's. A failure on a name is an OsPathError, which is
 * io/fs's PathError, with the operation, the name, and a SyscallErrno for
 * why, so errors_is(err, os_err_not_exist) works and so does errors_as with
 * TYPE_SYSCALL_ERRNO when you want the number. The text is Go's too, as in
 * "open notes.txt: no such file or directory".
 *
 * Closing and freeing a file are two calls, for the reasons given at OsFile
 * below. Reads and writes block the thread they run on, as Go's do for
 * regular files.
 *
 * Paths are Str and go to the system as they are, so "/" and, on Windows, "\"
 * both work. A name with a NUL byte in it fails with EINVAL, as Go's does.
 *
 * FileInfo.Sys gives a nil Any for now, where Go gives a
 * *syscall.Stat_t.
 *
 * Derived from Go's src/os/file.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package os */

#ifndef BURROW_OS_H
#define BURROW_OS_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/syscall.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- FileMode */

/* os.FileMode, which is fs.FileMode, and its bits and methods. */
typedef FsFileMode OsFileMode;

#define OS_MODE_DIR FS_MODE_DIR
#define OS_MODE_APPEND FS_MODE_APPEND
#define OS_MODE_EXCLUSIVE FS_MODE_EXCLUSIVE
#define OS_MODE_TEMPORARY FS_MODE_TEMPORARY
#define OS_MODE_SYMLINK FS_MODE_SYMLINK
#define OS_MODE_DEVICE FS_MODE_DEVICE
#define OS_MODE_NAMED_PIPE FS_MODE_NAMED_PIPE
#define OS_MODE_SOCKET FS_MODE_SOCKET
#define OS_MODE_SETUID FS_MODE_SETUID
#define OS_MODE_SETGID FS_MODE_SETGID
#define OS_MODE_CHAR_DEVICE FS_MODE_CHAR_DEVICE
#define OS_MODE_STICKY FS_MODE_STICKY
#define OS_MODE_IRREGULAR FS_MODE_IRREGULAR
#define OS_MODE_TYPE FS_MODE_TYPE
#define OS_MODE_PERM FS_MODE_PERM

#define os_file_mode_string fs_file_mode_string
#define os_file_mode_is_dir fs_file_mode_is_dir
#define os_file_mode_is_regular fs_file_mode_is_regular
#define os_file_mode_perm fs_file_mode_perm
#define os_file_mode_type fs_file_mode_type

/* os.FileInfo and os.DirEntry, which are io/fs's. */
typedef FsFileInfoVT OsFileInfoVT;
typedef FsFileInfo OsFileInfo;
typedef FsDirEntryVT OsDirEntryVT;
typedef FsDirEntry OsDirEntry;

/* ------------------------------------------------------------------- flags */

/* The flags os_open_file takes. Exactly one of the first three, ORed with any
 * of the rest. The numbers are burrow's own and the same everywhere, where
 * Go's are each system's, so do not mix them with a system's O_ constants. */
#define OS_O_RDONLY 0x0      /* open for reading only */
#define OS_O_WRONLY 0x1      /* open for writing only */
#define OS_O_RDWR 0x2        /* open for reading and writing */
#define OS_O_APPEND (1 << 2) /* every write goes to the end */
#define OS_O_CREATE (1 << 3) /* create the file if it is not there */
#define OS_O_EXCL (1 << 4)   /* with OS_O_CREATE, the file must not be there */
#define OS_O_SYNC (1 << 6)   /* synchronous I/O */
#define OS_O_TRUNC (1 << 5)  /* empty a regular file when it opens */

/* Where os_file_seek counts from. Go deprecated these for io.SeekStart and
 * the others, which have the same values. */
#define OS_SEEK_SET 0
#define OS_SEEK_CUR 1
#define OS_SEEK_END 2

/* The separators and the null device, as character and string literals. */
#if defined(BURROW_OS_WINDOWS)
#define OS_PATH_SEPARATOR '\\'
#define OS_PATH_LIST_SEPARATOR ';'
#define OS_DEV_NULL "NUL"
#else
#define OS_PATH_SEPARATOR '/'
#define OS_PATH_LIST_SEPARATOR ':'
#define OS_DEV_NULL "/dev/null"
#endif

/* os.IsPathSeparator: '/' everywhere, and '\' as well on Windows. */
bool os_is_path_separator(uint8_t c);

/* ------------------------------------------------------------------ errors */

/* The errors to test for with errors_is. The first five are io/fs's. */
#define os_err_invalid fs_err_invalid       /* "invalid argument" */
#define os_err_permission fs_err_permission /* "permission denied" */
#define os_err_exist fs_err_exist           /* "file already exists" */
#define os_err_not_exist fs_err_not_exist   /* "file does not exist" */
#define os_err_closed fs_err_closed         /* "file already closed" */

/* "file type does not support deadline", from the deadline setters on a file
 * that cannot have one, which is every file here for now. */
extern const Error os_err_no_deadline;

/* "i/o timeout", for a read or write that ran past its deadline. Its type has
 * Timeout and Temporary methods that say true, so os_is_timeout says true. */
extern const Error os_err_deadline_exceeded;

/* os.PathError, which is fs.PathError. */
typedef FsPathError OsPathError;
#define TYPE_OS_PATH_ERROR TYPE_FS_PATH_ERROR
#define os_path_error_error fs_path_error_error
#define os_path_error_unwrap fs_path_error_unwrap
#define os_path_error_timeout fs_path_error_timeout
#define os_path_error_new fs_path_error_new

/* os.LinkError: op, the two names, and why. Its text is
 * op + " " + old + " " + new + ": " + the text of err. new_ has the
 * underscore because new is a word C++ keeps for itself. */
typedef struct OsLinkError {
    Str op;
    Str old;
    Str new_;
    Error err;
} OsLinkError;

extern const Type *const TYPE_OS_LINK_ERROR;

/* The text, built in a. */
BURROW_OWNS(ret) Str os_link_error_error(const OsLinkError *e, Alloc *a);

/* e->err. */
BURROW_BORROWS(ret, e) Error os_link_error_unwrap(const OsLinkError *e);

/* One you fill in with the names copied into a, the shape of
 * fs_path_error_new. */
BURROW_OWNS(ret) Error os_link_error_new(Alloc *a, Str op, Str old, Str new_,
                                         Error err);

/* os.SyscallError: the name of a system call and why it failed. Its text is
 * syscall + ": " + the text of err. */
typedef struct OsSyscallError {
    Str syscall;
    Error err;
} OsSyscallError;

extern const Type *const TYPE_OS_SYSCALL_ERROR;

/* The text, built in a. */
BURROW_OWNS(ret) Str os_syscall_error_error(const OsSyscallError *e, Alloc *a);

/* e->err. */
BURROW_BORROWS(ret, e) Error os_syscall_error_unwrap(const OsSyscallError *e);

/* SyscallError.Timeout: whether e->err has a Timeout method that says true. */
bool os_syscall_error_timeout(const OsSyscallError *e);

/* os.NewSyscallError: err with the name of the call it came from, in a, or no
 * error when err is none. */
BURROW_OWNS(ret) Error os_new_syscall_error(Alloc *a, Str syscall, Error err);

/* os.IsExist, IsNotExist and IsPermission. They look one level inside an
 * OsPathError, OsLinkError or OsSyscallError and nowhere else, and then ask
 * whether that is the sentinel or a SyscallErrno that counts as it, which is
 * how Go's have always worked. errors_is with the sentinel looks all the way
 * down and is what new code should use. */
bool os_is_exist(Error err);
bool os_is_not_exist(Error err);
bool os_is_permission(Error err);

/* os.IsTimeout: whether the error inside, found the same way, has a Timeout
 * method that says true. */
bool os_is_timeout(Error err);

/* -------------------------------------------------------------------- File */

/* os.File, an open file.
 *
 * An OsFile is not a descriptor. It carries the reference count and the locks
 * Go keeps in internal/poll, so a read racing a close on another goroutine
 * either finishes or fails with os_err_closed, and never reads a descriptor
 * that has been closed and handed to somebody else. That is why closing and
 * freeing are two calls. os_file_close closes the descriptor and waits for
 * whatever is using it to finish. The OsFile itself stays, so a second close
 * and a late read get os_err_closed rather than freed memory. os_file_free
 * closes the file if it is still open and gives the memory back, and is the
 * call to make even when the OsFile came from an arena, since dropping the
 * arena would leave the descriptor open. */
typedef struct OsFile OsFile;

extern const Type *const TYPE_OS_FILE;

/* The descriptor TYPE_OS_FILE points at, for a table in another file that
 * needs it as a constant. Use TYPE_OS_FILE. */
extern const Type burrow__os_file_desc;

/* os.Open: name for reading. */
BURROW_OWNS(ret) OsFile *os_open(Alloc *a, Str name, Error *err);

/* os.Create: name for reading and writing, created with mode 0666 less the
 * umask if it is not there, and emptied if it is. */
BURROW_OWNS(ret) OsFile *os_create(Alloc *a, Str name, Error *err);

/* os.OpenFile: name with the OS_O_ flags in flag. perm, less the umask, is
 * the mode a file OS_O_CREATE makes gets. The OsFile and a copy of name come
 * from a. NULL with an OsPathError of op "open" when it fails. */
BURROW_OWNS(ret) OsFile *os_open_file(Alloc *a, Str name, Int flag, OsFileMode perm,
                                      Error *err);

/* os.NewFile: an OsFile for a descriptor, or a HANDLE on Windows, that is
 * already open. name is what errors call it. NULL when fd is not a valid
 * descriptor or a refuses. The OsFile owns fd from here on and closes it. */
BURROW_OWNS(ret) OsFile *os_new_file(Alloc *a, Uintptr fd, Str name);

/* File.Name: the name it was opened with. */
BURROW_BORROWS(ret, f) Str os_file_name(const OsFile *f);

/* File.Fd: the descriptor, or HANDLE, or ~0 once it is closed. */
Uintptr os_file_fd(OsFile *f);

/* File.Read: up to p.len bytes. io_eof at the end of the file, with 0 bytes.
 * An empty p reads nothing and is not an error. */
Int os_file_read(OsFile *f, Slice p, Error *err);

/* File.ReadAt: p.len bytes from off, without moving the offset Read uses. Fewer
 * means an error, io_eof when the file ended first. A negative off is an
 * OsPathError of op "readat". */
Int os_file_read_at(OsFile *f, Slice p, int64_t off, Error *err);

/* File.Write: all of p, or how much was written and why it stopped. */
Int os_file_write(OsFile *f, Slice p, Error *err);

/* File.WriteString: os_file_write with s. */
Int os_file_write_string(OsFile *f, Str s, Error *err);

/* File.WriteAt: all of p at off, without moving the offset Write uses. A file
 * opened with OS_O_APPEND refuses, with "os: invalid use of WriteAt on file
 * opened with O_APPEND", and a negative off is an OsPathError of op
 * "writeat". */
Int os_file_write_at(OsFile *f, Slice p, int64_t off, Error *err);

/* File.Seek: moves the offset to offset counted from whence, one of the
 * OS_SEEK_ values, and gives the new one. */
int64_t os_file_seek(OsFile *f, int64_t offset, Int whence, Error *err);

/* File.Stat. The FileInfo comes from a. */
BURROW_OWNS(ret) OsFileInfo os_file_stat(OsFile *f, Alloc *a, Error *err);

/* File.Sync: asks the system to put what has been written on the disk. */
BURROW_STATIC(ret) Error os_file_sync(OsFile *f);

/* File.Truncate: makes the file size bytes long, without moving the offset. */
BURROW_STATIC(ret) Error os_file_truncate(OsFile *f, int64_t size);

/* File.Chmod: os_chmod on the open file. */
BURROW_STATIC(ret) Error os_file_chmod(OsFile *f, OsFileMode mode);

/* File.Chown: os_chown on the open file. Windows says EWINDOWS. */
BURROW_STATIC(ret) Error os_file_chown(OsFile *f, Int uid, Int gid);

/* File.Chdir: makes the open file, which has to be a directory, the working
 * directory. */
BURROW_STATIC(ret) Error os_file_chdir(OsFile *f);

/* os.Pipe: a connected pair of files, called "|0" and "|1". What is written to
 * *w can be read from the one returned. Both come from a, and the caller
 * closes and frees both. On failure both are NULL. */
BURROW_OWNS(ret) OsFile *os_pipe(Alloc *a, OsFile **w, Error *err);

/* File.SetDeadline, SetReadDeadline and SetWriteDeadline. Go supports them on
 * pipes and the like, which go through its poller. burrow does not put files
 * on the poller yet, so they give os_err_no_deadline, which is what Go gives
 * for a regular file. */
BURROW_STATIC(ret) Error os_file_set_deadline(OsFile *f, Time t);
BURROW_STATIC(ret) Error os_file_set_read_deadline(OsFile *f, Time t);
BURROW_STATIC(ret) Error os_file_set_write_deadline(OsFile *f, Time t);

/* File.Close. A second close is an OsPathError of op "close" wrapping
 * os_err_closed, and so is anything else done with f afterwards. If another
 * goroutine is in the middle of using f, this waits for it, and the descriptor
 * closes when it is done. f stays, for os_file_free. */
BURROW_STATIC(ret) Error os_file_close(OsFile *f);

/* Closes f if it is still open and gives its memory back. Only once nothing
 * else can be using it. NULL is fine. os_stdin and the others are not yours to
 * free and this leaves them alone. */
void os_file_free(OsFile *f);

/* f as the io interfaces, borrowing it. */
IoReader os_file_as_io_reader(OsFile *f);
IoWriter os_file_as_io_writer(OsFile *f);
IoCloser os_file_as_io_closer(OsFile *f);
IoReadCloser os_file_as_io_read_closer(OsFile *f);
IoWriteCloser os_file_as_io_write_closer(OsFile *f);
IoSeeker os_file_as_io_seeker(OsFile *f);
IoReaderAt os_file_as_io_reader_at(OsFile *f);
IoWriterAt os_file_as_io_writer_at(OsFile *f);
IoStringWriter os_file_as_io_string_writer(OsFile *f);
IoReadWriteSeeker os_file_as_io_read_write_seeker(OsFile *f);

/* f as an fs.File, borrowing it, with os_file_read_dir as its read_dir. */
FsFile os_file_as_fs_file(OsFile *f);

/* File.ReadFrom: everything r has, written to f, and how many bytes that was.
 * Go hands some copies to copy_file_range, splice or sendfile on Linux and
 * the BSDs. This always copies through a buffer, so the bytes and the errors
 * are the same and only the speed differs. io_copy into an OsFile comes here. */
int64_t os_file_read_from(OsFile *f, IoReader r, Error *err);

/* File.WriteTo: everything left in f, written to w. io_copy from an OsFile
 * comes here. */
int64_t os_file_write_to(OsFile *f, IoWriter w, Error *err);

/* File.SyscallConn: the descriptor under f as a SyscallRawConn, borrowing f.
 * Files here are not on a poller, so a Read or Write callback that returns
 * false gets "waiting for unsupported file type" on Unix, as a Go file that is
 * not pollable does. On Windows a Write callback that returns false gives
 * EWINDOWS and a Read callback WSAENOTSOCK, which is what Go's zero byte
 * socket read gives on a file handle. */
SyscallRawConn os_file_syscall_conn(OsFile *f, Error *err);

/* os.Stdin, os.Stdout and os.Stderr, opened the first time you ask. */
BURROW_STATIC(ret) OsFile *os_stdin_file(void);
BURROW_STATIC(ret) OsFile *os_stdout_file(void);
BURROW_STATIC(ret) OsFile *os_stderr_file(void);
#define os_stdin (os_stdin_file())
#define os_stdout (os_stdout_file())
#define os_stderr (os_stderr_file())

/* ------------------------------------------------------------------- names */

/* os.Stat: what name is, following symbolic links. The FileInfo comes from a,
 * and its name is the last element of name. */
BURROW_OWNS(ret) OsFileInfo os_stat(Alloc *a, Str name, Error *err);

/* os.Lstat: os_stat that describes a symbolic link rather than what it
 * points at. */
BURROW_OWNS(ret) OsFileInfo os_lstat(Alloc *a, Str name, Error *err);

/* os.SameFile: whether two FileInfos from os_stat, os_lstat or os_file_stat
 * describe the same file, by device and inode, or volume and file index on
 * Windows. False for FileInfos from anywhere else. */
bool os_same_file(OsFileInfo fi1, OsFileInfo fi2);

/* os.ReadFile: the whole file, from a. Reaching the end is not an error. */
BURROW_OWNS(ret) Slice os_read_file(Alloc *a, Str name, Error *err);

/* os.WriteFile: data as the whole of name, which is created with perm, less
 * the umask, if it is not there. */
BURROW_STATIC(ret) Error os_write_file(Str name, Slice data, OsFileMode perm);

/* os.Mkdir: a directory with perm, less the umask. */
BURROW_STATIC(ret) Error os_mkdir(Str name, OsFileMode perm);

/* os.Remove: the file or empty directory name. */
BURROW_STATIC(ret) Error os_remove(Str name);

/* os.Rename: moves oldpath to newpath, replacing a file that is there. On Unix
 * a directory at newpath is not replaced and the error says it exists. Errors
 * are OsLinkErrors. */
BURROW_STATIC(ret) Error os_rename(Str oldpath, Str newpath);

/* os.Link: newname as a hard link to oldname. */
BURROW_STATIC(ret) Error os_link(Str oldname, Str newname);

/* os.Symlink: newname as a symbolic link to oldname. */
BURROW_STATIC(ret) Error os_symlink(Str oldname, Str newname);

/* os.Readlink: where the symbolic link name points, from a. */
BURROW_OWNS(ret) Str os_readlink(Alloc *a, Str name, Error *err);

/* os.Chmod: sets the permission bits and, on Unix, setuid, setgid and sticky.
 * Windows only has the read only bit, which is 0200 clear. Symbolic links are
 * followed. */
BURROW_STATIC(ret) Error os_chmod(Str name, OsFileMode mode);

/* os.Chown: sets the owner and group, -1 for either leaving it alone. Symbolic
 * links are followed. Windows does not have it and says EWINDOWS. */
BURROW_STATIC(ret) Error os_chown(Str name, Int uid, Int gid);

/* os.Lchown: os_chown on a symbolic link itself, not what it points to.
 * Windows says EWINDOWS. */
BURROW_STATIC(ret) Error os_lchown(Str name, Int uid, Int gid);

/* os.Chtimes: sets the access and modification times. A zero Time leaves that
 * one as it is. */
BURROW_STATIC(ret) Error os_chtimes(Str name, Time atime, Time mtime);

/* os.Truncate: makes name size bytes long. */
BURROW_STATIC(ret) Error os_truncate(Str name, int64_t size);

/* ------------------------------------------------------------- directories */

/* File.ReadDir: up to n entries of the directory f has open, a Slice of
 * OsDirEntry from a, in the order the directory has them. Each call carries on
 * from the last. With n > 0 there is at least one entry or an error, which at
 * the end of the directory is io_eof. With n <= 0 it is everything left and no
 * error at the end. An entry knows its name and type without a stat, and its
 * info slot does an os_lstat. A file that goes away between the read and a
 * stat that had to be done is left out. */
BURROW_OWNS(ret) Slice os_file_read_dir(OsFile *f, Alloc *a, Int n, Error *err);

/* File.Readdir: os_file_read_dir with an os_lstat of each entry, a Slice of
 * OsFileInfo. */
BURROW_OWNS(ret) Slice os_file_readdir(OsFile *f, Alloc *a, Int n, Error *err);

/* File.Readdirnames: os_file_read_dir with only the names, a Slice of Str,
 * which needs no stat at all. */
BURROW_OWNS(ret) Slice os_file_readdirnames(OsFile *f, Alloc *a, Int n, Error *err);

/* os.ReadDir: every entry of the directory name, sorted by name. What was read
 * before an error comes back with it. */
BURROW_OWNS(ret) Slice os_read_dir(Alloc *a, Str name, Error *err);

/* os.MkdirAll: name and any parents it needs, each with perm less the umask.
 * Nothing to do and no error when name is a directory already. */
BURROW_STATIC(ret) Error os_mkdir_all(Str path, OsFileMode perm);

/* os.RemoveAll: name and everything under it. Nothing there is not an error,
 * and neither is an empty name. A name ending in "." is EINVAL. */
BURROW_STATIC(ret) Error os_remove_all(Str path);

/* os.TempDir: the directory for temporary files, from a. $TMPDIR or /tmp on
 * Unix, and what GetTempPath2 says on Windows, without the trailing
 * backslash. */
BURROW_OWNS(ret) Str os_temp_dir(Alloc *a);

/* os.CreateTemp: a new file in dir, or os_temp_dir when dir is empty, open
 * for reading and writing with mode 0600. Its name is pattern with a random
 * number put in place of the last "*", or added at the end when there is no
 * "*". A pattern with a separator in it is an error. Remove the file when you
 * are done with it, since nothing else will. */
BURROW_OWNS(ret) OsFile *os_create_temp(Alloc *a, Str dir, Str pattern, Error *err);

/* os.MkdirTemp: os_create_temp for a directory, made with mode 0700, and its
 * name from a. */
BURROW_OWNS(ret) Str os_mkdir_temp(Alloc *a, Str dir, Str pattern, Error *err);

/* os.DirFS: the tree under dir as an Fs. The names it takes are io/fs's, with
 * "/" between elements on every system. It has the stat, read_file, read_dir,
 * read_link and lstat slots. dir is copied into a, and when a refuses the Fs
 * is nil, with a NULL vt.
 *
 * Files it opens are OsFiles from the Alloc given to open. Closing one through
 * the FsFile closes the descriptor but cannot give the OsFile back, so use an
 * arena with it as with everything else an Fs hands out. */
BURROW_OWNS(ret) Fs os_dir_fs(Alloc *a, Str dir);

/* os.CopyFS: every file, directory and symbolic link in fsys, copied under
 * dir. Directories get 0777 and files 0666 with fsys's execute bits, both
 * less the umask. A file that is already there is an error, since files are
 * created with OS_O_EXCL. Anything that is not a file, a directory or a link
 * is an OsPathError of op "CopyFS". */
BURROW_STATIC(ret) Error os_copy_fs(Str dir, Fs fsys);

/* --------------------------------------------------------- environment */

/* os.LookupEnv: the value of key, copied into a. *found says whether it was
 * set at all, since a set but empty variable also gives an empty Str. found
 * can be NULL. On Unix the environment is read once, as in Go, and later
 * changes go through os_setenv, os_unsetenv and os_clearenv. */
BURROW_OWNS(ret) Str os_lookup_env(Alloc *a, Str key, bool *found);

/* os.Getenv: os_lookup_env without found. */
BURROW_OWNS(ret) Str os_getenv(Alloc *a, Str key);

/* os.Setenv. A key or value with NUL in it gives a SyscallError "setenv"
 * with EINVAL, and on Unix so does a key that is empty or has '=' in it. */
BURROW_STATIC(ret) Error os_setenv(Str key, Str value);

/* os.Unsetenv. */
BURROW_STATIC(ret) Error os_unsetenv(Str key);

/* os.Clearenv: unsets every variable. */
void os_clearenv(void);

/* os.Environ: a Slice of Str, each "key=value", all copied into a. */
BURROW_OWNS(ret) Slice os_environ(Alloc *a);

/* os.Expand: s with each $var or ${var} replaced by what mapping gives for
 * it. What mapping gives is copied, so it may borrow, and the result is a
 * copy in a even when nothing changed. */
BURROW_OWNS(ret) Str os_expand(Alloc *a, Str s, StrFunc mapping);

/* os.ExpandEnv: os_expand with os_getenv as the mapping. */
BURROW_OWNS(ret) Str os_expand_env(Alloc *a, Str s);

/* ------------------------------------------------------------- process */

/* os.Args: the command line, a Slice of Str that lives as long as the
 * program. Do not change it. */
BURROW_BORROWS(ret) Slice os_args(void);

/* os.Getuid, Geteuid, Getgid and Getegid. Windows gives -1 for all four. */
Int os_getuid(void);
Int os_geteuid(void);
Int os_getgid(void);
Int os_getegid(void);

/* os.Getgroups: the supplementary group ids, a Slice of Int from a. Windows
 * gives a SyscallError "getgroups" with EWINDOWS. */
BURROW_OWNS(ret) Slice os_getgroups(Alloc *a, Error *err);

/* os.Getpid, Getppid and Getpagesize. */
Int os_getpid(void);
Int os_getppid(void);
Int os_getpagesize(void);

/* os.Exit: ends the program now with code. Deferred work, buffered output
 * that was not flushed and atexit handlers are skipped, as in Go. */
void os_exit(Int code);

/* os.Hostname: the name the kernel has for this machine. */
BURROW_OWNS(ret) Str os_hostname(Alloc *a, Error *err);

/* os.Getwd: a rooted path for the working directory. When $PWD names the same
 * directory it is what you get, as in Go. */
BURROW_OWNS(ret) Str os_getwd(Alloc *a, Error *err);

/* os.Chdir: makes dir the working directory. */
BURROW_STATIC(ret) Error os_chdir(Str dir);

/* os.UserHomeDir, UserCacheDir and UserConfigDir, by Go's rules for each
 * system: $HOME, %USERPROFILE%, $XDG_CACHE_HOME, %LocalAppData%,
 * ~/Library/Caches and so on. When the variable they need is not set the
 * error says so in Go's words. */
BURROW_OWNS(ret) Str os_user_home_dir(Alloc *a, Error *err);
BURROW_OWNS(ret) Str os_user_cache_dir(Alloc *a, Error *err);
BURROW_OWNS(ret) Str os_user_config_dir(Alloc *a, Error *err);

/* ------------------------------------------------------------ processes */

/* os.Signal: anything with String and Signal. The ones os sends are
 * syscall.Signal values, which os_signal_from_syscall wraps. */
typedef struct OsSignalVT {
    const Type *self_type;
    Str (*string)(void *self, Alloc *a);
    void (*signal)(void *self);
} OsSignalVT;

typedef struct OsSignal {
    const OsSignalVT *vt;
    void *data;
} OsSignal;

/* s as an os.Signal, with nothing allocated. Its self_type is
 * TYPE_SYSCALL_SIGNAL. A number outside 0 to 255, which no system uses, gives
 * the zero OsSignal, and sending that is "os: unsupported signal type". */
OsSignal os_signal_from_syscall(SyscallSignal s);

/* The SyscallSignal inside sig, or -1 when sig is not one. */
SyscallSignal os_signal_to_syscall(OsSignal sig);

/* os.Interrupt and os.Kill: SIGINT and SIGKILL. On Windows only Kill can be
 * sent to another process. */
extern const OsSignal os_interrupt;
extern const OsSignal os_kill;

/* The descriptor for os.Signal, which is what a channel for os/signal is made
 * with: chan_make(a, TYPE_OS_SIGNAL, 1). */
extern const Type *const TYPE_OS_SIGNAL;

/* "os: process already finished", from Signal and Kill once Wait has
 * returned, and from Wait a second time on Windows. */
extern const Error os_err_process_done;

/* "os: process handle unavailable", from os_process_with_handle when there
 * is no handle, which is every process on Unix for now. */
extern const Error os_err_no_handle;

/* os.ProcAttr: how os_start_process sets up the child.
 *
 * dir is its working directory, and the empty Str leaves it ours. env is a
 * slice of "key=value" Strs, and a nil slice gives it os_environ. files are
 * the OsFile pointers that become its descriptors 0, 1, 2 and so on, and a
 * NULL entry leaves that one closed. On Windows the first three become its
 * standard handles and nothing else is passed down. sys may be NULL. */
typedef struct OsProcAttr {
    Str dir;
    Slice env;   /* of Str */
    Slice files; /* of OsFile * */
    const SyscallSysProcAttr *sys;
} OsProcAttr;

/* os.Process, from os_start_process or os_find_process. pid is the system's
 * id for it, and -1 after os_process_release on Unix. The rest belongs to
 * the package. */
typedef struct OsProcess {
    Int pid;
    Alloc *alloc;
    SyncAtomicUint32 state;
    SyncRWMutex sig_mu;
    /* On Windows, the handle and how many are using it. Unix has none. */
    bool has_handle;
    int64_t handle;
    SyncAtomicInt32 refs;
} OsProcess;

/* os.ProcessState: what os_process_wait found. A NULL one is Go's nil, which
 * os_process_state_string calls "<nil>" and os_process_state_exit_code calls
 * -1. */
typedef struct OsProcessState {
    Int pid;
    SyscallWaitStatus status;
    SyscallRusage rusage;
    Alloc *alloc;
} OsProcessState;

/* os.StartProcess: starts name with argv, a slice of Str, as its arguments,
 * the first of them being what the program sees as its own name. name is not
 * looked up in $PATH, which is what os/exec is for. A failure is a PathError
 * with op "fork/exec", or with op "chdir" when attr->dir is not a directory
 * that can be used. attr may be NULL. The OsProcess comes from a. */
BURROW_OWNS(ret) OsProcess *os_start_process(Alloc *a, Str name, Slice argv,
                                             const OsProcAttr *attr, Error *err);

/* os.FindProcess. On Unix this always works, whether or not there is such a
 * process, and Signal is how to find out, as in Go. On Windows it opens the
 * process, and a failure is a SyscallError with op "OpenProcess". */
BURROW_OWNS(ret) OsProcess *os_find_process(Alloc *a, Int pid, Error *err);

/* Process.Release: gives up the process without waiting for it. Nothing can
 * be done with p afterwards but os_process_free. On Unix it sets pid to -1.
 * On Windows a second call is EINVAL. */
BURROW_STATIC(ret) Error os_process_release(OsProcess *p);

/* Process.Kill: os_process_signal with os_kill. */
BURROW_STATIC(ret) Error os_process_kill(OsProcess *p);

/* Process.Signal: sends sig. Once the process has been waited for this is
 * os_err_process_done, and after os_process_release it is "os: process
 * already released". On Windows anything but os_kill is "not supported by
 * windows". */
BURROW_STATIC(ret) Error os_process_signal(OsProcess *p, OsSignal sig);

/* Process.Wait: waits for p to exit and reaps it. The state comes from the
 * Alloc p was made with. A failure is a SyscallError with op "wait". */
BURROW_OWNS(ret) OsProcessState *os_process_wait(OsProcess *p, Error *err);

/* The function os_process_with_handle calls. */
BURROW_FUNC(OsHandleFunc, void, Uintptr handle);

/* Process.WithHandle: calls f with the process handle, which stays valid
 * until f returns. Only Windows has one here, so on Unix this is
 * os_err_no_handle. */
BURROW_STATIC(ret) Error os_process_with_handle(OsProcess *p, OsHandleFunc f);

/* Releases p if it has not been, and gives its memory back. */
void os_process_free(OsProcess *p);

/* The ProcessState methods. */
Int os_process_state_pid(const OsProcessState *ps);
bool os_process_state_exited(const OsProcessState *ps);
bool os_process_state_success(const OsProcessState *ps);
Int os_process_state_exit_code(const OsProcessState *ps);

/* ProcessState.String: "exit status 1", "signal: killed", "signal:
 * segmentation fault (core dumped)" and so on, as Go words them. */
BURROW_OWNS(ret) Str os_process_state_string(const OsProcessState *ps, Alloc *a);

/* ProcessState.Sys and SysUsage, typed, since C has no any to hand back. */
SyscallWaitStatus os_process_state_sys(const OsProcessState *ps);
BURROW_BORROWS(ret, ps) const SyscallRusage *
os_process_state_sys_usage(const OsProcessState *ps);

/* ProcessState.UserTime and SystemTime: the CPU time the process and its
 * children it waited for used, in user mode and in the kernel. */
Duration os_process_state_user_time(const OsProcessState *ps);
Duration os_process_state_system_time(const OsProcessState *ps);

void os_process_state_free(OsProcessState *ps);

/* os.Executable: the path of the program that is running. It may be a
 * symbolic link, as in Go. On Linux it is what /proc/self/exe says, on macOS
 * and Solaris a relative path is joined to the working directory, and on a
 * system with no way to ask the error is "Executable not implemented for"
 * and its name. */
BURROW_OWNS(ret) Str os_executable(Alloc *a, Error *err);

/* ---------------------------------------------------------------- Root
 *
 * os.Root: a directory that names are looked up in and cannot leave.
 *
 *     OsRoot *r = os_open_root(a, BURROW_S("data"), &err);
 *     OsFile *f = os_root_open(r, a, BURROW_S("sub/notes.txt"), &err);
 *     os_root_open(r, a, BURROW_S("../secret"), &err);
 *     // openat ../secret: path escapes from parent
 *     Error cerr = os_root_close(r);
 *     os_root_free(r);
 *
 * Every name is relative to the root. A name that is absolute, or that ".."
 * or a symbolic link would take outside the root, fails with an OsPathError
 * whose error is "path escapes from parent". Links inside the root are
 * followed.
 *
 * On Unix a root holds a descriptor for the directory and walks each name
 * one component at a time with openat and its relatives, the way Go does, so
 * a root keeps working on its directory if the directory is moved, and a
 * link swapped in during a walk cannot lead out. Root's Chmod, Chown and
 * Chtimes have the same race on Unix that Go documents for them: if the file
 * is replaced with a link between the check and the call, the call acts on
 * the link. The link is inside the root, so this cannot escape it.
 *
 * On Windows a root is a name for now, and each call checks the name with
 * Lstat and Readlink before using it, as Go does on js and plan9. That is
 * open to a link being swapped in between the check and the use. Go's
 * Windows version, which opens relative to a handle with NtCreateFile, is
 * still to come.
 *
 * A root is safe to use from several threads at once. Closing it and freeing
 * it are two calls, as for OsFile: a closed root fails every call with
 * os_err_closed, and os_root_free gives the memory back. Nothing may be using
 * the root by the time it is freed. */
typedef struct OsRoot OsRoot;

/* os.OpenRoot: the directory name as a root. It follows links in name. A name
 * that is not a directory is an OsPathError "open" with "not a directory". */
BURROW_OWNS(ret) OsRoot *os_open_root(Alloc *a, Str name, Error *err);

/* os.OpenInRoot: os_open_root(dir), os_root_open(name), and the root closed
 * again. The file stays open. */
BURROW_OWNS(ret) OsFile *os_open_in_root(Alloc *a, Str dir, Str name, Error *err);

/* Root.Name: the name the root was opened with. Fine after os_root_close. */
BURROW_BORROWS(ret, r) Str os_root_name(const OsRoot *r);

/* Root.Close: later calls on r fail with os_err_closed. Calls already under
 * way finish first, and the descriptor goes when the last one does. */
BURROW_STATIC(ret) Error os_root_close(OsRoot *r);

/* The memory behind r, closed first if it is still open. */
void os_root_free(OsRoot *r);

/* Root.Open, Create and OpenFile: os_open, os_create and os_open_file on a
 * name in the root. The file's name is the root's name joined to name. */
BURROW_OWNS(ret) OsFile *os_root_open(OsRoot *r, Alloc *a, Str name, Error *err);
BURROW_OWNS(ret) OsFile *os_root_create(OsRoot *r, Alloc *a, Str name, Error *err);
BURROW_OWNS(ret) OsFile *os_root_open_file(OsRoot *r, Alloc *a, Str name, Int flag,
                                           OsFileMode perm, Error *err);

/* Root.OpenRoot: a directory in the root as a root of its own. */
BURROW_OWNS(ret) OsRoot *os_root_open_root(OsRoot *r, Alloc *a, Str name, Error *err);

/* Root's versions of os_chmod, os_mkdir, os_mkdir_all, os_chown, os_lchown,
 * os_chtimes, os_remove, os_remove_all, os_rename, os_link and os_symlink.
 * For os_root_symlink, oldname is what the link says and is not checked,
 * since a link that points out of the root is harmless until the root is
 * asked to follow it, and then it fails. */
BURROW_STATIC(ret) Error os_root_chmod(OsRoot *r, Str name, OsFileMode mode);
BURROW_STATIC(ret) Error os_root_mkdir(OsRoot *r, Str name, OsFileMode perm);
BURROW_STATIC(ret) Error os_root_mkdir_all(OsRoot *r, Str name, OsFileMode perm);
BURROW_STATIC(ret) Error os_root_chown(OsRoot *r, Str name, Int uid, Int gid);
BURROW_STATIC(ret) Error os_root_lchown(OsRoot *r, Str name, Int uid, Int gid);
BURROW_STATIC(ret) Error os_root_chtimes(OsRoot *r, Str name, Time atime, Time mtime);
BURROW_STATIC(ret) Error os_root_remove(OsRoot *r, Str name);
BURROW_STATIC(ret) Error os_root_remove_all(OsRoot *r, Str name);
BURROW_STATIC(ret) Error os_root_rename(OsRoot *r, Str oldname, Str newname);
BURROW_STATIC(ret) Error os_root_link(OsRoot *r, Str oldname, Str newname);
BURROW_STATIC(ret) Error os_root_symlink(OsRoot *r, Str oldname, Str newname);

/* Root.Stat, Lstat and Readlink. */
BURROW_OWNS(ret) OsFileInfo os_root_stat(OsRoot *r, Alloc *a, Str name, Error *err);
BURROW_OWNS(ret) OsFileInfo os_root_lstat(OsRoot *r, Alloc *a, Str name, Error *err);
BURROW_OWNS(ret) Str os_root_readlink(OsRoot *r, Alloc *a, Str name, Error *err);

/* Root.ReadFile and WriteFile. */
BURROW_OWNS(ret) Slice os_root_read_file(OsRoot *r, Alloc *a, Str name, Error *err);
BURROW_STATIC(ret) Error os_root_write_file(OsRoot *r, Str name, Slice data,
                                            OsFileMode perm);

/* Root.FS: the root as an Fs, with Stat, ReadFile, ReadDir and ReadLink.
 * Names are io/fs names, so they are slash separated and "..", a leading "/"
 * and, on Windows, a backslash are refused before the root sees them. The Fs
 * borrows r. */
BURROW_BORROWS(ret, r) Fs os_root_fs(OsRoot *r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_OS_H */
