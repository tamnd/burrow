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
 * Not here yet: reading directories, MkdirAll and RemoveAll, temporary files,
 * DirFS, the environment, processes, File.Chmod, Chown and Chdir, Lchown,
 * Pipe and Root. FileInfo.Sys gives a nil Any for now, where Go gives a
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
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
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
IoSeeker os_file_as_io_seeker(OsFile *f);
IoReaderAt os_file_as_io_reader_at(OsFile *f);
IoWriterAt os_file_as_io_writer_at(OsFile *f);
IoStringWriter os_file_as_io_string_writer(OsFile *f);
IoReadWriteSeeker os_file_as_io_read_write_seeker(OsFile *f);

/* f as an fs.File, borrowing it. Its read_dir slot is NULL for now. */
FsFile os_file_as_fs_file(OsFile *f);

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

/* os.Chtimes: sets the access and modification times. A zero Time leaves that
 * one as it is. */
BURROW_STATIC(ret) Error os_chtimes(Str name, Time atime, Time mtime);

/* os.Truncate: makes name size bytes long. */
BURROW_STATIC(ret) Error os_truncate(Str name, int64_t size);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_OS_H */
