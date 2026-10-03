/* os/exec: running other programs.
 *
 *     Error err;
 *     ExecCmd *c = exec_command_v(a, BURROW_S("git"), 2, BURROW_S("rev-parse"),
 *                                 BURROW_S("HEAD"));
 *     Slice out = exec_cmd_output(c, a, &err);
 *     exec_cmd_free(c);
 *
 * This is os/exec on top of os_start_process, and like Go's it does not go
 * through a shell. Nothing expands globs or variables, and nothing splits a
 * command line into words. A name with no separator in it is looked up in
 * $PATH by exec_look_path, and one with a separator is used as it is.
 *
 * As of Go 1.19, a lookup that would find the program through a relative entry
 * in $PATH, "." included, is refused, and so it is here. The ExecError you get
 * back wraps exec_err_dot, the Path is still filled in, and clearing c->err is
 * how a caller who really means it says so. Go's GODEBUG=execerrdot=0 is not
 * read.
 *
 * Go copies to and from the child's standard streams on goroutines when they
 * are not files. Here the copying runs on threads of its own, so a Cmd works
 * from a plain main as well as from a goroutine and does not hold a P while it
 * waits on a pipe. What a caller sees is the same: Wait does not come back
 * until the copying is done. It does mean stdout_ and stderr_ are written to,
 * and stdin_ read from, on another thread until Wait returns, and that the
 * cancel function runs on a thread of its own when the context is done. An
 * arena is not safe to share between threads, so a writer handed to a Cmd
 * should not be allocating from one that something else is using meanwhile.
 *
 * On Windows the arguments are joined into one command line with the quoting
 * CommandLineToArgvW undoes, which is what Go does. cmd.exe and batch files
 * parse a command line their own way, as Go's documentation warns.
 *
 * A Cmd and everything it makes come from the Alloc it was made with, and
 * exec_cmd_free gives them back. The errors it returns come from
 * error_allocator, as everywhere else, and do not point into the Cmd.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package os/exec */

#ifndef BURROW_OS_EXEC_H
#define BURROW_OS_EXEC_H

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/slice.h"
#include "burrow/syscall.h"
#include "burrow/time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* exec.ErrNotFound: no executable by that name was found in the path. The text
 * is "executable file not found in $PATH", and "%PATH%" on Windows. */
extern const Error exec_err_not_found;

/* exec.ErrDot: the path lookup found the program through a relative entry,
 * which it will not run. Test for it with errors_is, since it comes wrapped in
 * an ExecError. */
extern const Error exec_err_dot;

/* exec.ErrWaitDelay: the program exited successfully but its output pipes
 * were still open when wait_delay ran out. */
extern const Error exec_err_wait_delay;

/* exec.Error, what exec_look_path says when it cannot use a file. The text is
 * "exec: " + the quoted name + ": " + the text of err, and errors_is sees
 * through to err. errors_as with TYPE_EXEC_ERROR gets you the struct. */
typedef struct ExecError {
    Str name;
    Error err;
} ExecError;

extern const Type *const TYPE_EXEC_ERROR;

/* exec.ExitError, the error from a program that ran and did not exit with 0.
 * The text is os_process_state_string's, "exit status 2" or "signal: killed".
 * Go embeds the *os.ProcessState, so ee.ExitCode() works there. Here it is
 * os_process_state_exit_code(ee->process_state). stderr_ holds the start and
 * the end of what the program wrote to its standard error, when
 * exec_cmd_output was collecting it, and is nil otherwise. */
typedef struct ExecExitError {
    OsProcessState *process_state;
    Slice stderr_; /* of Byte */
} ExecExitError;

extern const Type *const TYPE_EXEC_EXIT_ERROR;

/* What the Cmd's cancel field holds: Go's func() error. */
BURROW_FUNC0(ExecCancelFunc, Error);

/* The Cmd's own state, behind a pointer. */
typedef struct burrow__ExecState burrow__ExecState;

/* exec.Cmd, a program being got ready or being run.
 *
 * The fields before the line are Go's and are the caller's to set between
 * exec_command and exec_cmd_start. stdin, stdout and stderr are macros in
 * <stdio.h>, so those three have an underscore after them. A Cmd cannot be
 * started twice. */
typedef struct ExecCmd {
    /* The program to run. A relative path is taken from dir. */
    Str path;
    /* The arguments, a Slice of Str, with the program's name first. A nil
     * Slice runs it with path as the only argument. */
    Slice args;
    /* The environment, a Slice of "key=value" Strs. A nil Slice gives the
     * child ours. When a key comes up more than once the last one wins. On
     * Windows SYSTEMROOT is added if it is missing. */
    Slice env;
    /* The working directory, and the empty Str for ours. On Unix, when env is
     * nil, PWD is set to match it. */
    Str dir;
    /* What the child reads, nil for the null device. An OsFile is handed to
     * the child as it is. Anything else is copied in over a pipe, and Wait
     * waits for that copying to finish. */
    IoReader stdin_;
    /* Where the child's output goes, the same way. When both are the same
     * writer, only one thing writes to it at a time. */
    IoWriter stdout_;
    IoWriter stderr_;
    /* More OsFile pointers for the child, the first of them its descriptor 3.
     * Not supported on Windows. */
    Slice extra_files;
    /* syscall.SysProcAttr, which may be NULL. */
    const SyscallSysProcAttr *sys_proc_attr;
    /* The running program, once started. */
    OsProcess *process;
    /* How it ended, once exec_cmd_wait has seen it. */
    OsProcessState *process_state;
    /* The error from looking path up, if there was one. Start returns it. */
    Error err;
    /* For a Cmd from exec_command_context: what to do when the context is
     * done. It kills the process unless you change it, and a nil one does
     * nothing. If it succeeds and the program then exits with 0, Wait
     * reports the context's error, and if it fails with an error other than
     * os_err_process_done, Wait reports that. */
    ExecCancelFunc cancel;
    /* How long Wait gives a program to exit after the context is done, and
     * its pipes to close after it has exited, before it kills the program
     * and closes them. Zero waits as long as it takes. */
    Duration wait_delay;

    /* ---- what follows is the package's own */
    Alloc *alloc;
    Context ctx;
    Str look_in, look_out; /* the extension cached for an absolute path */
    bool start_called;
    burrow__ExecState *st;
} ExecCmd;

/* exec.Command: a Cmd for name with args, a Slice of Str, after it. A name with
 * no separator is looked up with exec_look_path, and if that fails the error
 * is put in c->err for exec_cmd_start to return. args[0] of the Cmd is always
 * name. The Strs are kept as they are, not copied, so they have to outlive
 * the Cmd. NULL when a refuses. */
BURROW_OWNS(ret) ExecCmd *exec_command(Alloc *a, Str name, Slice args);

/* exec_command without a Slice. Every argument after n must be a Str. */
BURROW_OWNS(ret) ExecCmd *exec_command_v(Alloc *a, Str name, int n, ...);

/* exec.CommandContext: exec_command, and when ctx is done before the program
 * has exited on its own, the Cmd's cancel runs. */
BURROW_OWNS(ret) ExecCmd *exec_command_context(Alloc *a, Context ctx, Str name,
                                               Slice args);
BURROW_OWNS(ret) ExecCmd *exec_command_context_v(Alloc *a, Context ctx, Str name, int n,
                                                 ...);

/* Gives back everything the Cmd holds and the Cmd itself. A Cmd that was
 * started and not waited for is killed and waited for first.
 *
 * A Cmd can also be filled in by hand, as in Go, starting from a zeroed
 * struct with path and args set. It takes what it needs from alloc, or from
 * the heap when alloc is NULL, and exec_cmd_free then gives that back and
 * leaves the struct itself alone. */
void exec_cmd_free(ExecCmd *c);

/* exec.LookPath: where file is, searching the directories in $PATH when it has
 * no separator in it. On Windows the extensions in %PATHEXT% are tried, and
 * the current directory is searched first unless
 * NoDefaultCurrentDirectoryInExePath is set, as cmd.exe does. A failure is an
 * ExecError. One that wraps exec_err_dot still answers the path. */
BURROW_OWNS(ret) Str exec_look_path(Alloc *a, Str file, Error *err);

/* Cmd.String: the path and the arguments with spaces between them, for a log
 * line. Not something to hand to a shell. */
BURROW_OWNS(ret) Str exec_cmd_string(const ExecCmd *c, Alloc *a);

/* Cmd.Run: exec_cmd_start, then exec_cmd_wait. A program that exits with
 * anything but 0 gives an ExecExitError. */
BURROW_STATIC(ret) Error exec_cmd_run(ExecCmd *c);

/* Cmd.Start: starts the program and does not wait for it. After it works,
 * exec_cmd_wait has to be called. */
BURROW_STATIC(ret) Error exec_cmd_start(ExecCmd *c);

/* Cmd.Wait: waits for the program to exit and for any copying to or from its
 * streams to finish, and then closes the pipes exec_cmd_*_pipe made. */
BURROW_STATIC(ret) Error exec_cmd_wait(ExecCmd *c);

/* Cmd.Output: runs the program and answers what it wrote to its standard
 * output, as a Slice of Byte from a. When stderr_ was nil and the program
 * failed, the ExecExitError holds what it wrote to its standard error. */
BURROW_OWNS(ret) Slice exec_cmd_output(ExecCmd *c, Alloc *a, Error *err);

/* Cmd.CombinedOutput: runs the program and answers its standard output and
 * standard error together, in the order it wrote them. */
BURROW_OWNS(ret) Slice exec_cmd_combined_output(ExecCmd *c, Alloc *a, Error *err);

/* Cmd.StdinPipe, StdoutPipe and StderrPipe: our end of a pipe to one of the
 * program's streams, made before the program starts. exec_cmd_wait closes it,
 * so read everything from an output pipe before calling it, and close the
 * input pipe yourself when the program needs to see the end of its input.
 * The pipe belongs to the Cmd. On Unix, writing to the input pipe after the
 * program has exited raises SIGPIPE, which ends the process unless it is
 * ignored or handled. Go's runtime turns that into an error instead. */
BURROW_BORROWS(ret, c) IoWriteCloser exec_cmd_stdin_pipe(ExecCmd *c, Error *err);
BURROW_BORROWS(ret, c) IoReadCloser exec_cmd_stdout_pipe(ExecCmd *c, Error *err);
BURROW_BORROWS(ret, c) IoReadCloser exec_cmd_stderr_pipe(ExecCmd *c, Error *err);

/* Cmd.Environ: the environment the program would get as the Cmd stands, a
 * Slice of Str from a. */
BURROW_OWNS(ret) Slice exec_cmd_environ(const ExecCmd *c, Alloc *a);

/* ExecError's and ExecExitError's text, built in a, and each as an Error. */
BURROW_OWNS(ret) Str exec_error_error(const ExecError *e, Alloc *a);
BURROW_OWNS(ret) Error exec_error_as_error(const ExecError *e, Alloc *a);
BURROW_OWNS(ret) Str exec_exit_error_error(const ExecExitError *e, Alloc *a);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_OS_EXEC_H */
