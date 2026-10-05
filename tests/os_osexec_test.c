/* Derived from Go's src/os/exec/exec_test.go, exec_posix_test.go, env_test.go,
 * dot_test.go, lp_test.go and lp_unix_test.go.
 * Go source: go1.27.1.
 *
 * The commands the tests run are this program again. BURROW_EXEC_HELPER in
 * the environment makes it a helper, and its first argument says which, the
 * way Go's TestMain does it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os/exec.h"

#include "burrow/burrow.h"
#include "burrow/context.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/path/filepath.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync/atomic.h"
#include "burrow/thread.h"
#include "burrow/time.h"

#include "check.h"

#include <string.h>

#define S(lit) BURROW_S(lit)
#define HELPER_VAR "BURROW_EXEC_HELPER"
#define STDIN_CLOSE_TEST_STRING "Some test string."

static Arena ar;
static Alloc *a;

static Slice strs(const Str *v, Int n) {
    Slice s = slice_make(a, TYPE_STRING, n, n);
    for (Int i = 0; i < n; i++)
        BURROW_AT(Str, s, i) = v[i];
    return s;
}

static Str exe_path(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    Error e = BURROW_NO_ERROR;
    Str exe = os_executable(a, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Executable: %s", error_text(e));
    return exe;
}

/* helperCommand: this program, running the helper called name. */
static ExecCmd *helper(TestingT *t, Context ctx, Str name, const Str *args, Int n) {
    Str v[16];
    v[0] = name;
    for (Int i = 0; i < n; i++)
        v[i + 1] = args[i];
    Str exe = exe_path(t);
    if (ctx.vt != NULL)
        return exec_command_context(a, ctx, exe, strs(v, n + 1));
    return exec_command(a, exe, strs(v, n + 1));
}

#define HELPER(t, name, ...)                                                           \
    helper(                                                                            \
        (t), (Context){0}, S(name), (const Str[]){BURROW_STR_EMPTY, __VA_ARGS__} + 1,  \
        (Int)(sizeof((const Str[]){BURROW_STR_EMPTY, __VA_ARGS__}) / sizeof(Str)) - 1)
#define HELPER0(t, name) helper((t), (Context){0}, S(name), NULL, 0)
#define HELPER_CTX0(t, ctx, name) helper((t), (ctx), S(name), NULL, 0)

static Str text(Slice b) {
    return str_from_bytes(b.p, b.len);
}

#define CHECK_S(got, want)                                                             \
    do {                                                                               \
        Str g_ = (got), w_ = (want);                                                   \
        if (!str_eq(g_, w_))                                                           \
            testing_t_errorf_v(t, "%s = %q, want %q", #got, g_, w_);                   \
    } while (0)

#define CHECK_OK(what, e)                                                              \
    do {                                                                               \
        Error e_ = (e);                                                                \
        if (BURROW_FAILED(e_))                                                         \
            testing_t_fatalf_v(t, "%s: %s", S(what), error_text(e_));                  \
    } while (0)

static const ExecExitError *exit_error(Error err) {
    return (const ExecExitError *)errors_as(err, TYPE_EXEC_EXIT_ERROR);
}

/* A writer on the heap, since the copying threads write to it. */
typedef struct HeapWriter {
    BytesBuffer b;
} HeapWriter;

static IoWriter heap_writer(HeapWriter *w) {
    w->b = BYTES_BUFFER(heap_allocator());
    return bytes_buffer_as_io_writer(&w->b);
}

static Str heap_writer_take(HeapWriter *w) {
    Slice b = bytes_buffer_bytes(&w->b);
    Str s = str_clone(a, str_from_bytes(b.p, b.len));
    bytes_buffer_free(&w->b);
    return s;
}

/* One line from r, without the newline. */
static Str read_line(TestingT *t, IoReader r, const char *what) {
    Slice line = slice_make(a, TYPE_BYTE, 0, 64);
    Byte c;
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Int n = r.vt->read(r.data, slice_from(&c, 1, 1, TYPE_BYTE), &e);
        if (n == 1) {
            if (c == '\n')
                break;
            line = BURROW_APPEND(Byte, a, line, c);
            continue;
        }
        if (BURROW_FAILED(e))
            testing_t_fatalf_v(t, "%s: %s", str_from_cstr(what), error_text(e));
    }
    return text(line);
}

static void write_all(IoWriter w, Str s, Error *err) {
    *err = BURROW_NO_ERROR;
    (void)w.vt->write(w.data,
                      slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE), err);
}

/* ------------------------------------------------------------ the helpers */

static void out_str(OsFile *f, Str s) {
    Error e = BURROW_NO_ERROR;
    os_file_write_string(f, s, &e);
}

static void cmd_echo(Slice args) {
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < args.len; i++) {
        if (i > 0)
            (void)strings_builder_write_byte(&b, ' ');
        (void)strings_builder_write_string(&b, BURROW_AT(Str, args, i), NULL);
    }
    (void)strings_builder_write_byte(&b, '\n');
    out_str(os_stdout, strings_builder_string(&b));
}

static void cmd_echo_env(Slice args) {
    for (Int i = 0; i < args.len; i++) {
        out_str(os_stdout, os_getenv(a, BURROW_AT(Str, args, i)));
        out_str(os_stdout, S("\n"));
    }
}

static void copy_out(OsFile *from) {
    Error e = BURROW_NO_ERROR;
    (void)io_copy(a, os_file_as_io_writer(os_stdout), os_file_as_io_reader(from), &e);
}

static void cmd_cat(Slice args) {
    if (args.len == 0) {
        copy_out(os_stdin);
        return;
    }
    Int code = 0;
    for (Int i = 0; i < args.len; i++) {
        Error e = BURROW_NO_ERROR;
        OsFile *f = os_open(a, BURROW_AT(Str, args, i), &e);
        if (f == NULL) {
            out_str(os_stderr, fmt_sprintf_v(a, "Error: %s\n", error_text(e)));
            code = 2;
            continue;
        }
        copy_out(f);
        (void)os_file_close(f);
    }
    os_exit(code);
}

/* Reads stdin a byte at a time, so that nothing is left in a buffer when the
 * parent waits for an answer. */
static bool stdin_line(Slice *line) {
    *line = slice_make(a, TYPE_BYTE, 0, 64);
    for (;;) {
        Byte c;
        Error e = BURROW_NO_ERROR;
        Int n = os_file_read(os_stdin, slice_from(&c, 1, 1, TYPE_BYTE), &e);
        if (n == 1) {
            if (c == '\n')
                return true;
            *line = BURROW_APPEND(Byte, a, *line, c);
            continue;
        }
        if (BURROW_FAILED(e)) {
            if (errors_is(e, io_eof))
                return line->len > 0;
            os_exit(1);
        }
    }
}

static void cmd_pipe_test(void) {
    Slice line;
    while (stdin_line(&line)) {
        Str l = text(line);
        if (strings_has_prefix(l, S("O:"))) {
            out_str(os_stdout, l);
            out_str(os_stdout, S("\n"));
        } else if (strings_has_prefix(l, S("E:"))) {
            out_str(os_stderr, l);
            out_str(os_stderr, S("\n"));
        } else {
            os_exit(1);
        }
    }
}

static void cmd_stdin_close(void) {
    Error e = BURROW_NO_ERROR;
    Slice b = io_read_all(a, os_file_as_io_reader(os_stdin), &e);
    if (BURROW_FAILED(e)) {
        out_str(os_stderr, fmt_sprintf_v(a, "Error: %s\n", error_text(e)));
        os_exit(1);
    }
    if (!str_eq(text(b), S(STDIN_CLOSE_TEST_STRING))) {
        out_str(os_stderr, fmt_sprintf_v(a, "Error: Read %q, want %q", text(b),
                                         S(STDIN_CLOSE_TEST_STRING)));
        os_exit(1);
    }
}

static Int atoi_or(Str s, Int def) {
    Error e = BURROW_NO_ERROR;
    Int n = strconv_atoi(s, &e);
    return BURROW_FAILED(e) ? def : n;
}

static void cmd_yes(void) {
    for (;;) {
        Error e = BURROW_NO_ERROR;
        os_file_write_string(os_stdout, S("y\n"), &e);
        if (BURROW_FAILED(e))
            os_exit(1);
    }
}

static void cmd_stderr_big(Slice args) {
    Int n = atoi_or(BURROW_AT(Str, args, 0), 0);
    Slice b = slice_make(a, TYPE_BYTE, n, n);
    for (Int i = 0; i < n; i++)
        BURROW_AT(Byte, b, i) = (Byte)('a' + i % 26);
    Error e = BURROW_NO_ERROR;
    os_file_write(os_stderr, b, &e);
    os_exit(1);
}

/* hang MS [subsleep=MS] [probe=MS] [read]: Go's cmdHang without the signal
 * handling, which waits for os/signal. A subsleep leaves a grandchild holding
 * our stdin and stderr. A probe writes to stderr every so often and exits
 * when that fails. */
static void cmd_hang(Slice args) {
    Int sleep_ms = atoi_or(BURROW_AT(Str, args, 0), 0);
    Int subsleep = 0, probe = 0;
    bool read = false;
    for (Int i = 1; i < args.len; i++) {
        Str f = BURROW_AT(Str, args, i);
        if (strings_has_prefix(f, S("subsleep=")))
            subsleep = atoi_or(strings_trim_prefix(f, S("subsleep=")), 0);
        else if (strings_has_prefix(f, S("probe=")))
            probe = atoi_or(strings_trim_prefix(f, S("probe=")), 0);
        else if (str_eq(f, S("read")))
            read = true;
    }
    Int pid = os_getpid();

    if (subsleep != 0) {
        Error e = BURROW_NO_ERROR;
        Str exe = os_executable(a, &e);
        Str v[4] = {S("hang"), strconv_itoa(a, subsleep), S("read"),
                    fmt_sprintf_v(a, "probe=%d", probe)};
        ExecCmd *c = exec_command(a, exe, strs(v, 4));
        c->stdin_ = os_file_as_io_reader(os_stdin);
        c->stderr_ = os_file_as_io_writer(os_stderr);
        IoReadCloser out = exec_cmd_stdout_pipe(c, &e);
        if (BURROW_FAILED(e)) {
            out_str(os_stderr, error_text(e));
            os_exit(1);
        }
        e = exec_cmd_start(c);
        if (BURROW_FAILED(e)) {
            out_str(os_stderr, error_text(e));
            os_exit(1);
        }
        Slice got = io_read_all(a, io_read_closer_as_io_reader(out), &e);
        (void)got;
        if (BURROW_FAILED(e)) {
            out_str(os_stderr, error_text(e));
            (void)os_process_kill(c->process);
            os_exit(1);
        }
        out_str(os_stderr, fmt_sprintf_v(a, "%d: started %d\n", pid, c->process->pid));
        /* The grandchild is left to outlive us, which is the point. */
    }

    (void)os_file_close(os_stdout);

    if (read) {
        Slice line;
        while (stdin_line(&line))
            out_str(os_stderr, fmt_sprintf_v(a, "%d: read %s\n", pid, text(line)));
        out_str(os_stderr, fmt_sprintf_v(a, "%d: finished read\n", pid));
    }

    Time until = time_add(time_now(), (Duration)sleep_ms * TIME_MILLISECOND);
    while (time_before(time_now(), until)) {
        if (probe == 0) {
            time_sleep(time_until(until));
            break;
        }
        time_sleep((Duration)probe * TIME_MILLISECOND);
        Error e = BURROW_NO_ERROR;
        os_file_write_string(os_stderr, fmt_sprintf_v(a, "%d: ok\n", pid), &e);
        if (BURROW_FAILED(e))
            os_exit(1);
    }
    out_str(os_stderr, fmt_sprintf_v(a, "%d: slept %dms\n", pid, sleep_ms));
}

static void run_helper(void) {
    Slice args = os_args();
    if (args.len < 2) {
        out_str(os_stderr, S("No command\n"));
        os_exit(2);
    }
    Str cmd = BURROW_AT(Str, args, 1);
    Slice rest = slice_from((Str *)args.p + 2, args.len - 2, args.len - 2, TYPE_STRING);
    if (str_eq(cmd, S("echo")))
        cmd_echo(rest);
    else if (str_eq(cmd, S("echoenv")))
        cmd_echo_env(rest);
    else if (str_eq(cmd, S("cat")))
        cmd_cat(rest);
    else if (str_eq(cmd, S("pipetest")))
        cmd_pipe_test();
    else if (str_eq(cmd, S("stdinClose")))
        cmd_stdin_close();
    else if (str_eq(cmd, S("exit")))
        os_exit(atoi_or(BURROW_AT(Str, rest, 0), 0));
    else if (str_eq(cmd, S("stderrfail"))) {
        out_str(os_stderr, S("some stderr text\n"));
        os_exit(1);
    } else if (str_eq(cmd, S("stderrbig")))
        cmd_stderr_big(rest);
    else if (str_eq(cmd, S("yes")))
        cmd_yes();
    else if (str_eq(cmd, S("hang")))
        cmd_hang(rest);
    else if (str_eq(cmd, S("pwd"))) {
        Error e = BURROW_NO_ERROR;
        out_str(os_stdout, os_getwd(a, &e));
        out_str(os_stdout, S("\n"));
    } else {
        out_str(os_stderr, fmt_sprintf_v(a, "Unknown command %q\n", cmd));
        os_exit(2);
    }
    os_exit(0);
}

/* ------------------------------------------------------------------ tests */

static void TestEcho(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    ExecCmd *c = HELPER(t, "echo", S("foo bar"), S("baz"));
    Slice bs = exec_cmd_output(c, a, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "echo: %s", error_text(err));
    CHECK_S(text(bs), S("foo bar baz\n"));
    exec_cmd_free(c);
}

static void TestCommandRelativeName(TestingT *t) {
    ExecCmd *c = HELPER(t, "echo", S("foo"));
    Str exe = exe_path(t);
    Str base = filepath_base(exe);
    Str dir = filepath_dir(a, exe);
    if (str_eq(dir, S(".")))
        testing_t_skip_v(t, "skipping; running test at root somehow");
    Str parent = filepath_dir(a, dir);
    Str dir_base = filepath_base(dir);
    if (str_eq(dir_base, S(".")))
        testing_t_skipf_v(t, "skipping; unexpected shallow dir of %q", dir);
    c->path = filepath_join_v(a, 2, dir_base, base);
    c->dir = parent;
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_output(c, a, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "echo: %s", error_text(err));
    CHECK_S(text(out), S("foo\n"));
    exec_cmd_free(c);
}

static void TestCatStdin(TestingT *t) {
    Str input = S("Input string\nLine 2");
    ExecCmd *c = HELPER0(t, "cat");
    StringsReader r;
    strings_reader_reset(&r, input);
    c->stdin_ = strings_reader_as_io_reader(&r);
    Error err = BURROW_NO_ERROR;
    Slice bs = exec_cmd_output(c, a, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "cat: %s", error_text(err));
    CHECK_S(text(bs), input);
    exec_cmd_free(c);
}

typedef struct StdinWrite {
    IoWriteCloser w;
    Str s;
    bool close;
    char err[160]; /* the error's text, empty for none */
} StdinWrite;

/* An error's text kept in buf, since the thread's error arena goes when the
 * thread does. */
static void keep_error(char *buf, size_t n, Error e) {
    buf[0] = 0;
    if (BURROW_OK(e))
        return;
    Str s = error_text(e);
    size_t k = (size_t)s.len < n - 1 ? (size_t)s.len : n - 1;
    memcpy(buf, s.p, k);
    buf[k] = 0;
}

static void write_stdin(void *arg) {
    StdinWrite *sw = (StdinWrite *)arg;
    pal_signal_mask(PAL_SIGPIPE, true, NULL);
    ArenaMark m = error_mark();
    Error e = BURROW_NO_ERROR;
    write_all(io_write_closer_as_io_writer(sw->w), sw->s, &e);
    if (sw->close) {
        Error ce = sw->w.vt->closer.close(sw->w.data);
        if (BURROW_OK(e) && BURROW_FAILED(ce) && !errors_is(ce, os_err_closed))
            e = ce;
    }
    keep_error(sw->err, sizeof sw->err, e);
    error_release(m);
    burrow__error_thread_exit();
}

static void TestEchoFileRace(TestingT *t) {
    ExecCmd *c = HELPER0(t, "echo");
    Error err = BURROW_NO_ERROR;
    IoWriteCloser in = exec_cmd_stdin_pipe(c, &err);
    CHECK_OK("StdinPipe", err);
    CHECK_OK("Start", exec_cmd_start(c));
    StdinWrite sw = {in, S("echo\n"), false, {0}};
    burrow__Thread th;
    CHECK(burrow__thread_start(&th, write_stdin, &sw, 0));
    CHECK_OK("Wait", exec_cmd_wait(c));
    burrow__thread_join(&th);
    exec_cmd_free(c);
}

static void TestCatGoodAndBadFile(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, S("osexec"), &err);
    CHECK_OK("MkdirTemp", err);
    Str good = filepath_join_v(a, 2, dir, S("good.txt"));
    Str body = S("func TestCatGoodAndBadFile(t *testing.T)\n");
    CHECK_OK("WriteFile", os_write_file(good,
                                        slice_from((void *)(uintptr_t)body.p, body.len,
                                                   body.len, TYPE_BYTE),
                                        0644));
    ExecCmd *c = HELPER(t, "cat", S("/bogus/file.foo"), good);
    Slice bs = exec_cmd_combined_output(c, a, &err);
    if (exit_error(err) == NULL)
        testing_t_errorf_v(t, "expected ExecExitError from cat combined; got %s",
                           error_text(err));
    Str rest;
    bool found = false;
    Str err_line = strings_cut(text(bs), S("\n"), &rest, &found);
    if (!found)
        testing_t_fatalf_v(t, "expected two lines from cat; got %q", text(bs));
    if (!strings_has_prefix(err_line, S("Error: open /bogus/file.foo")))
        testing_t_errorf_v(t, "expected stderr to complain about file; got %q",
                           err_line);
    if (!strings_contains(rest, S("func TestCatGoodAndBadFile(t *testing.T)")))
        testing_t_errorf_v(t, "expected test code; got %q (len %d)", rest, rest.len);
    exec_cmd_free(c);
    (void)os_remove_all(dir);
}

static void TestNoExistExecutable(TestingT *t) {
    ExecCmd *c = exec_command(a, S("/no-exist-executable"), slice_nil(TYPE_STRING));
    if (BURROW_OK(exec_cmd_run(c)))
        testing_t_error_v(t, "expected error from /no-exist-executable");
    exec_cmd_free(c);
}

static void TestExitStatus(TestingT *t) {
    ExecCmd *c = HELPER(t, "exit", S("42"));
    Error err = exec_cmd_run(c);
    const ExecExitError *ee = exit_error(err);
    if (ee == NULL)
        testing_t_fatalf_v(t, "expected ExecExitError from exit 42; got %s",
                           error_text(err));
    CHECK_S(error_text(err), S("exit status 42"));
    CHECK_S(exec_exit_error_error(ee, a), S("exit status 42"));
    exec_cmd_free(c);
}

static void TestExitCode(TestingT *t) {
    struct {
        const char *name;
        Str arg;
        Int want;
    } tests[] = {
        {"exit", S("42"), 42},
        {"/no-exist-executable", BURROW_STR_EMPTY, 2},
        {"exit", S("255"), 255},
        {"cat", BURROW_STR_EMPTY, 0},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        ExecCmd *c =
            tests[i].arg.len > 0
                ? helper(t, (Context){0}, str_from_cstr(tests[i].name), &tests[i].arg,
                         1)
                : helper(t, (Context){0}, str_from_cstr(tests[i].name), NULL, 0);
        (void)exec_cmd_run(c);
        Int got = os_process_state_exit_code(c->process_state);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "ExitCode got %d, want %d", got, tests[i].want);
        exec_cmd_free(c);
    }
    ExecCmd *c = HELPER0(t, "cat");
    Int got = os_process_state_exit_code(c->process_state);
    if (got != -1)
        testing_t_errorf_v(t, "ExitCode got %d, want %d", got, (Int)-1);
    exec_cmd_free(c);
}

static void TestPipes(TestingT *t) {
    ExecCmd *c = HELPER0(t, "pipetest");
    Error err = BURROW_NO_ERROR;
    IoWriteCloser in = exec_cmd_stdin_pipe(c, &err);
    CHECK_OK("StdinPipe", err);
    IoReadCloser out = exec_cmd_stdout_pipe(c, &err);
    CHECK_OK("StdoutPipe", err);
    IoReadCloser errp = exec_cmd_stderr_pipe(c, &err);
    CHECK_OK("StderrPipe", err);
    CHECK_OK("Start", exec_cmd_start(c));

    write_all(io_write_closer_as_io_writer(in), S("O:I am output\n"), &err);
    CHECK_OK("first stdin Write", err);
    CHECK_S(read_line(t, io_read_closer_as_io_reader(out), "first output line"),
            S("O:I am output"));
    write_all(io_write_closer_as_io_writer(in), S("E:I am error\n"), &err);
    CHECK_OK("second stdin Write", err);
    CHECK_S(read_line(t, io_read_closer_as_io_reader(errp), "first error line"),
            S("E:I am error"));
    write_all(io_write_closer_as_io_writer(in), S("O:I am output2\n"), &err);
    CHECK_OK("third stdin Write 3", err);
    CHECK_S(read_line(t, io_read_closer_as_io_reader(out), "second output line"),
            S("O:I am output2"));

    (void)in.vt->closer.close(in.data);
    CHECK_OK("Wait", exec_cmd_wait(c));
    exec_cmd_free(c);
}

static void TestStdinClose(TestingT *t) {
    ExecCmd *c = HELPER0(t, "stdinClose");
    Error err = BURROW_NO_ERROR;
    IoWriteCloser in = exec_cmd_stdin_pipe(c, &err);
    CHECK_OK("StdinPipe", err);
    if (in.data == NULL)
        testing_t_error_v(t, "can't access methods of underlying OsFile");
    CHECK_OK("Start", exec_cmd_start(c));
    StdinWrite sw = {in, S(STDIN_CLOSE_TEST_STRING), true, {0}};
    burrow__Thread th;
    CHECK(burrow__thread_start(&th, write_stdin, &sw, 0));
    CHECK_OK("Wait", exec_cmd_wait(c));
    burrow__thread_join(&th);
    if (sw.err[0] != 0)
        testing_t_errorf_v(t, "Copy or Close: %s", str_from_cstr(sw.err));
    exec_cmd_free(c);
}

static void kill_it(void *arg) {
    ArenaMark m = error_mark();
    (void)os_process_kill((OsProcess *)arg);
    error_release(m);
    burrow__error_thread_exit();
}

static void TestStdinCloseRace(TestingT *t) {
    ExecCmd *c = HELPER0(t, "stdinClose");
    Error err = BURROW_NO_ERROR;
    IoWriteCloser in = exec_cmd_stdin_pipe(c, &err);
    CHECK_OK("StdinPipe", err);
    CHECK_OK("Start", exec_cmd_start(c));
    burrow__Thread killer, writer;
    CHECK(burrow__thread_start(&killer, kill_it, c->process, 0));
    StdinWrite sw = {in, S("unexpected string"), true, {0}};
    CHECK(burrow__thread_start(&writer, write_stdin, &sw, 0));
    if (BURROW_OK(exec_cmd_wait(c)))
        testing_t_error_v(t, "Wait: succeeded unexpectedly");
    burrow__thread_join(&killer);
    burrow__thread_join(&writer);
    exec_cmd_free(c);
}

#if !defined(BURROW_OS_WINDOWS)
/* Go counts the descriptors below 100. The next descriptor the system hands
 * out says the same thing more cheaply: it is the lowest free one. */
static Uintptr next_fd(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    OsFile *f = os_open(a, S(OS_DEV_NULL), &err);
    CHECK_OK("open", err);
    Uintptr fd = os_file_fd(f);
    (void)os_file_close(f);
    return fd;
}

static void TestPipeLookPathLeak(TestingT *t) {
    Uintptr before = next_fd(t);
    for (int i = 0; i < 6; i++) {
        ExecCmd *c = exec_command(a, S("something-that-does-not-exist-executable"),
                                  slice_nil(TYPE_STRING));
        Error err = BURROW_NO_ERROR;
        (void)exec_cmd_stdout_pipe(c, &err);
        (void)exec_cmd_stderr_pipe(c, &err);
        (void)exec_cmd_stdin_pipe(c, &err);
        if (BURROW_OK(exec_cmd_run(c)))
            testing_t_fatal_v(t, "unexpected success");
        exec_cmd_free(c);
    }
    Uintptr after = next_fd(t);
    if (after != before)
        testing_t_errorf_v(t, "leaked file descriptors: next is %d, was %d", (Int)after,
                           (Int)before);
}
#endif

/* delayedInfiniteReader: x after x after a tenth of a second. */
static Int infinite_read(void *self, Slice b, Error *err) {
    (void)self;
    *err = BURROW_NO_ERROR;
    time_sleep(100 * TIME_MILLISECOND);
    memset(b.p, 'x', (size_t)b.len);
    return b.len;
}

static const IoReaderVT infinite_vt = {NULL, infinite_read};

static void ignore_pipe_error(TestingT *t, IoReader r) {
    ExecCmd *c = HELPER(t, "echo", S("foo"));
    HeapWriter out;
    c->stdin_ = r;
    c->stdout_ = heap_writer(&out);
    Error err = exec_cmd_run(c);
    Str got = heap_writer_take(&out);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    CHECK_S(got, S("foo\n"));
    exec_cmd_free(c);
}

static void TestIgnorePipeErrorOnSuccess(TestingT *t) {
    StringsReader r;
    strings_reader_reset(&r, strings_repeat(a, S("x"), 10 << 20));
    ignore_pipe_error(t, strings_reader_as_io_reader(&r));
    ignore_pipe_error(t, (IoReader){&infinite_vt, NULL});
}

static Int bad_write(void *self, Slice data, Error *err) {
    (void)self;
    (void)data;
    *err = io_err_unexpected_eof;
    return 0;
}

static const IoWriterVT bad_writer_vt = {NULL, bad_write};

static void TestClosePipeOnCopyError(TestingT *t) {
    ExecCmd *c = HELPER0(t, "yes");
    c->stdout_ = (IoWriter){&bad_writer_vt, NULL};
    if (BURROW_OK(exec_cmd_run(c)))
        testing_t_error_v(t, "yes unexpectedly completed successfully");
    exec_cmd_free(c);
}

static void TestOutputStderrCapture(TestingT *t) {
    ExecCmd *c = HELPER0(t, "stderrfail");
    Error err = BURROW_NO_ERROR;
    (void)exec_cmd_output(c, a, &err);
    const ExecExitError *ee = exit_error(err);
    if (ee == NULL)
        testing_t_fatalf_v(t, "Output error = %s; want ExitError", error_text(err));
    CHECK_S(text(ee->stderr_), S("some stderr text\n"));
    exec_cmd_free(c);
}

/* TestPrefixSuffixSaver, from the outside: what Output keeps of a long
 * standard error. */
static void TestOutputStderrTruncated(TestingT *t) {
    Int n = (32 << 10) * 2 + 1000;
    ExecCmd *c = HELPER(t, "stderrbig", strconv_itoa(a, n));
    Error err = BURROW_NO_ERROR;
    (void)exec_cmd_output(c, a, &err);
    const ExecExitError *ee = exit_error(err);
    if (ee == NULL)
        testing_t_fatalf_v(t, "Output error = %s; want ExitError", error_text(err));
    Str got = text(ee->stderr_);
    Str note = S("\n... omitting 1000 bytes ...\n");
    Int want_len = (32 << 10) * 2 + note.len;
    if (got.len != want_len)
        testing_t_fatalf_v(t, "len(Stderr) = %d, want %d", got.len, want_len);
    CHECK_S(str_from_bytes(got.p + (32 << 10), note.len), note);
    /* Byte i of what was written is 'a' + i%26, so the start and the end are
     * easy to check. */
    if (got.p[0] != 'a' || got.p[(32 << 10) - 1] != (Byte)('a' + ((32 << 10) - 1) % 26))
        testing_t_error_v(t, "prefix is wrong");
    if (got.p[got.len - 1] != (Byte)('a' + (n - 1) % 26))
        testing_t_error_v(t, "suffix is wrong");
    exec_cmd_free(c);
}

static void TestContext(TestingT *t) {
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    ExecCmd *c = HELPER_CTX0(t, ctx, "pipetest");
    Error err = BURROW_NO_ERROR;
    IoWriteCloser in = exec_cmd_stdin_pipe(c, &err);
    CHECK_OK("StdinPipe", err);
    IoReadCloser out = exec_cmd_stdout_pipe(c, &err);
    CHECK_OK("StdoutPipe", err);
    CHECK_OK("Start", exec_cmd_start(c));
    write_all(io_write_closer_as_io_writer(in), S("O:hi\n"), &err);
    CHECK_OK("Write", err);
    Byte buf[5];
    Int n = io_read_full(io_read_closer_as_io_reader(out),
                         slice_from(buf, 5, 5, TYPE_BYTE), &err);
    if (n != 5 || BURROW_FAILED(err) || !str_eq(str_from_bytes(buf, n), S("O:hi\n")))
        testing_t_fatalf_v(t, "ReadFull = %d, %s, %q", n, error_text(err),
                           str_from_bytes(buf, n));
    BURROW_CALLF0(cancel);
    if (BURROW_OK(exec_cmd_wait(c)))
        testing_t_fatal_v(t, "expected Wait failure");
    exec_cmd_free(c);
    context_release(ctx);
}

static void TestContextCancel(TestingT *t) {
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    ExecCmd *c = HELPER_CTX0(t, ctx, "cat");
    Error err = BURROW_NO_ERROR;
    IoWriteCloser in = exec_cmd_stdin_pipe(c, &err);
    CHECK_OK("StdinPipe", err);
    CHECK_OK("Start", exec_cmd_start(c));
    write_all(io_write_closer_as_io_writer(in), S("echo"), &err);
    CHECK_OK("WriteString", err);
    BURROW_CALLF0(cancel);

    Time start = time_now();
    Duration delay = TIME_MILLISECOND;
    for (;;) {
        write_all(io_write_closer_as_io_writer(in), S("echo"), &err);
        if (BURROW_FAILED(err))
            break;
        if (time_since(start) > TIME_MINUTE)
            testing_t_fatal_v(t, "canceling context did not stop program");
        delay *= 2;
        if (delay > TIME_SECOND)
            delay = TIME_SECOND;
        time_sleep(delay);
    }
    err = exec_cmd_wait(c);
    if (BURROW_OK(err))
        testing_t_error_v(t, "program unexpectedly exited successfully");
    else
        testing_t_logf_v(t, "exit status: %s", error_text(err));
    (void)in.vt->closer.close(in.data);
    exec_cmd_free(c);
    context_release(ctx);
}

static void TestDedupEnvEcho(TestingT *t) {
    ExecCmd *c = HELPER(t, "echoenv", S("FOO"));
    Slice env = exec_cmd_environ(c, a);
    env = BURROW_APPEND(Str, a, env, S("FOO=bad"));
    env = BURROW_APPEND(Str, a, env, S("FOO=good"));
    c->env = env;
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_combined_output(c, a, &err);
    CHECK_OK("CombinedOutput", err);
    CHECK_S(strings_trim_space(text(out)), S("good"));
    exec_cmd_free(c);
}

static void TestEnvNULCharacter(TestingT *t) {
    ExecCmd *c = HELPER(t, "echoenv", S("FOO"), S("BAR"));
    Slice env = exec_cmd_environ(c, a);
    env = BURROW_APPEND(Str, a, env, str_from_bytes("FOO=foo\0BAR=bar", 15));
    c->env = env;
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_combined_output(c, a, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "output = %q; want error", text(out));
    exec_cmd_free(c);
}

/* TestDedupEnv, through Environ, with the case rule of the system it runs on. */
static void TestDedupEnv(TestingT *t) {
    struct {
        bool no_case;
        Str in[4];
        Int nin;
        Str want[4];
        Int nwant;
    } tests[] = {
        {true, {S("k1=v1"), S("k2=v2"), S("K1=v3")}, 3, {S("k2=v2"), S("K1=v3")}, 2},
        {false, {S("k1=v1"), S("K1=V2"), S("k1=v3")}, 3, {S("K1=V2"), S("k1=v3")}, 2},
        {false,
         {S("=a"), S("=b"), S("foo"), S("bar")},
         4,
         {S("=b"), S("foo"), S("bar")},
         3},
        {true,
         {S("=C:=C:\\golang"), S("=D:=D:\\tmp"), S("=D:=D:\\")},
         3,
         {S("=C:=C:\\golang"), S("=D:=D:\\")},
         2},
        {false, {S("dodgy"), S("entries")}, 2, {S("dodgy"), S("entries")}, 2},
    };
#if defined(BURROW_OS_WINDOWS)
    bool no_case = true;
#else
    bool no_case = false;
#endif
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (tests[i].no_case != no_case && i != 2 && i != 4)
            continue;
        ExecCmd c = {0};
        c.env = strs(tests[i].in, tests[i].nin);
        Slice got = exec_cmd_environ(&c, a);
        Int k = 0;
        bool ok = true;
        for (Int j = 0; j < got.len; j++) {
            Str kv = BURROW_AT(Str, got, j);
            if (strings_has_prefix(kv, S("SYSTEMROOT=")))
                continue; /* addCriticalEnv, on Windows */
            if (k >= tests[i].nwant || !str_eq(kv, tests[i].want[k]))
                ok = false;
            k++;
        }
        if (!ok || k != tests[i].nwant)
            testing_t_errorf_v(t, "Dedup case %d = %v", (Int)i, got);
    }
}

static void TestString(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str echo = exec_look_path(a, S("echo"), &err);
    if (BURROW_FAILED(err))
        testing_t_skip_v(t, error_text(err));
    struct {
        Str args[2];
        Int n;
        Str want;
    } tests[] = {
        {{BURROW_STR_EMPTY}, 0, echo},
        {{S("a")}, 1, fmt_sprintf_v(a, "%s a", echo)},
        {{S("a"), S("b")}, 2, fmt_sprintf_v(a, "%s a b", echo)},
    };
    for (size_t i = 0; i < 3; i++) {
        ExecCmd *c = exec_command(a, S("echo"), strs(tests[i].args, tests[i].n));
        CHECK_S(exec_cmd_string(c, a), tests[i].want);
        exec_cmd_free(c);
    }
}

static void TestStringPathNotResolved(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    (void)exec_look_path(a, S("makemeasandwich"), &err);
    if (BURROW_OK(err))
        testing_t_skip_v(t, "wow, thanks");
    Str args[1] = {S("-lettuce")};
    ExecCmd *c = exec_command(a, S("makemeasandwich"), strs(args, 1));
    CHECK_S(exec_cmd_string(c, a), S("makemeasandwich -lettuce"));
    exec_cmd_free(c);
}

static void TestNoPath(TestingT *t) {
    ExecCmd c = {0};
    Error err = exec_cmd_start(&c);
    if (BURROW_OK(err) || !str_eq(error_text(err), S("exec: no command")))
        testing_t_errorf_v(t, "new(Cmd).Start() = %s, want %q", error_text(err),
                           S("exec: no command"));
    exec_cmd_free(&c);
}

/* A Cmd filled in by hand, with no allocator, runs and is tidied up. */
static void TestCmdLiteral(TestingT *t) {
    Str exe = exe_path(t);
    Str argv[3] = {exe, S("echo"), S("literal")};
    ExecCmd c = {0};
    c.path = exe;
    c.args = strs(argv, 3);
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_output(&c, a, &err);
    CHECK_OK("Output", err);
    CHECK_S(text(out), S("literal\n"));
    exec_cmd_free(&c);
}

typedef struct ReadAllArg {
    IoReader r;
    Slice out;
    char err[160];
} ReadAllArg;

static void read_all_thread(void *arg) {
    ReadAllArg *ra = (ReadAllArg *)arg;
    ArenaMark m = error_mark();
    BytesBuffer b = BYTES_BUFFER(heap_allocator());
    Error e = BURROW_NO_ERROR;
    (void)bytes_buffer_read_from(&b, ra->r, &e);
    Slice got = bytes_buffer_bytes(&b);
    ra->out = slice_make(heap_allocator(), TYPE_BYTE, got.len, got.len);
    if (got.len > 0)
        memcpy(ra->out.p, got.p, (size_t)got.len);
    bytes_buffer_free(&b);
    keep_error(ra->err, sizeof ra->err, e);
    error_release(m);
    burrow__error_thread_exit();
}

static void TestDoubleStartLeavesPipesOpen(TestingT *t) {
    ExecCmd *c = HELPER0(t, "pipetest");
    Error err = BURROW_NO_ERROR;
    IoWriteCloser in = exec_cmd_stdin_pipe(c, &err);
    CHECK_OK("StdinPipe", err);
    IoReadCloser out = exec_cmd_stdout_pipe(c, &err);
    CHECK_OK("StdoutPipe", err);
    CHECK_OK("Start", exec_cmd_start(c));
    err = exec_cmd_start(c);
    if (BURROW_OK(err) || !strings_has_suffix(error_text(err), S("already started")))
        testing_t_fatal_v(
            t, "second call to Start returned a nil; want an 'already started' error");

    ReadAllArg ra = {io_read_closer_as_io_reader(out), {0}, {0}};
    burrow__Thread th;
    CHECK(burrow__thread_start(&th, read_all_thread, &ra, 0));
    Str msg = S("O:Hello, pipe!\n");
    write_all(io_write_closer_as_io_writer(in), msg, &err);
    CHECK_OK("WriteString", err);
    (void)in.vt->closer.close(in.data);
    burrow__thread_join(&th);
    if (ra.err[0] != 0)
        testing_t_error_v(t, str_from_cstr(ra.err));
    CHECK_S(text(ra.out), msg);
    mem_free(heap_allocator(), ra.out.p, (size_t)ra.out.cap, 1);
    CHECK_OK("Wait", exec_cmd_wait(c));
    exec_cmd_free(c);
}

/* tickReader: the time, once a millisecond, forever. */
static Int tick_read(void *self, Slice b, Error *err) {
    (void)self;
    *err = BURROW_NO_ERROR;
    time_sleep(TIME_MILLISECOND);
    const char *s = "tick\n";
    Int n = b.len < 5 ? b.len : 5;
    memcpy(b.p, s, (size_t)n);
    return n;
}

static const IoReaderVT tick_vt = {NULL, tick_read};

typedef struct Hang {
    ExecCmd *c;
    HeapWriter stderr_;
} Hang;

static Error kill_cancel(void *env) {
    return os_process_kill(((ExecCmd *)env)->process);
}

/* startHang. A NULL interrupt leaves cancel nil, and anything else kills,
 * which is the only signal there is to send without os/signal. */
static void start_hang(TestingT *t, Hang *h, Context ctx, Int ms, bool kill,
                       Duration wait_delay, const Str *flags, Int nflags) {
    Str v[6];
    v[0] = strconv_itoa(a, ms);
    for (Int i = 0; i < nflags; i++)
        v[i + 1] = flags[i];
    ExecCmd *c = helper(t, ctx, S("hang"), v, nflags + 1);
    c->stdin_ = (IoReader){&tick_vt, NULL};
    c->stderr_ = heap_writer(&h->stderr_);
    c->cancel = kill ? (ExecCancelFunc){kill_cancel, c} : (ExecCancelFunc){NULL, NULL};
    c->wait_delay = wait_delay;
    Error err = BURROW_NO_ERROR;
    IoReadCloser out = exec_cmd_stdout_pipe(c, &err);
    CHECK_OK("StdoutPipe", err);
    testing_t_log_v(t, exec_cmd_string(c, a));
    CHECK_OK("Start", exec_cmd_start(c));
    Slice got = io_read_all(a, io_read_closer_as_io_reader(out), &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, error_text(err));
        (void)os_process_kill(c->process);
        (void)exec_cmd_wait(c);
        testing_t_fail_now(t);
    }
    if (got.len > 0)
        testing_t_logf_v(t, "stdout:\n%s", text(got));
    h->c = c;
}

static void finish_hang(TestingT *t, Hang *h, Error err) {
    testing_t_logf_v(t, "stderr:\n%s", heap_writer_take(&h->stderr_));
    testing_t_logf_v(t, "[%d] %s", h->c->process->pid,
                     BURROW_FAILED(err) ? error_text(err) : S("<nil>"));
    exec_cmd_free(h->c);
}

static void TestWaitInterrupt_Wait(TestingT *t) {
    Hang h;
    start_hang(t, &h, context_background(), 1, true, 0, NULL, 0);
    Error err = exec_cmd_wait(h.c);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Wait: %s; want <nil>", error_text(err));
    OsProcessState *ps = h.c->process_state;
    if (!os_process_state_exited(ps))
        testing_t_errorf_v(t, "cmd did not exit: %s", os_process_state_string(ps, a));
    else if (os_process_state_exit_code(ps) != 0)
        testing_t_errorf_v(t, "ExitCode() = %d; want 0",
                           os_process_state_exit_code(ps));
    finish_hang(t, &h, err);
}

static void TestWaitInterrupt_KillHang(TestingT *t) {
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    Hang h;
    Str flags[2] = {S("subsleep=600000"), S("probe=1")};
    start_hang(t, &h, ctx, 600000, true, 10 * TIME_MILLISECOND, flags, 2);
    BURROW_CALLF0(cancel);
    Error err = exec_cmd_wait(h.c);
    if (exit_error(err) == NULL)
        testing_t_errorf_v(t, "Wait error = %s; want ExecExitError",
                           BURROW_FAILED(err) ? error_text(err) : S("<nil>"));
    finish_hang(t, &h, err);
    context_release(ctx);
}

static void TestWaitInterrupt_ExitHang(TestingT *t) {
    Hang h;
    Str flags[2] = {S("subsleep=600000"), S("probe=1")};
    start_hang(t, &h, context_background(), 1, false, 10 * TIME_MILLISECOND, flags, 2);
    Error err = exec_cmd_wait(h.c);
    if (!errors_is(err, exec_err_wait_delay))
        testing_t_errorf_v(t, "Wait error = %s; want ErrWaitDelay",
                           BURROW_FAILED(err) ? error_text(err) : S("<nil>"));
    finish_hang(t, &h, err);
}

/* ------------------------------------------------------- TestCancelErrors */

static const Str arbitrary_text = {(const Byte *)"arbitrary error", 15};
static const Error err_arbitrary = {&burrow_sentinel_error_vt, &arbitrary_text};

typedef struct CancelEnv {
    ExecCmd *c;
    IoWriteCloser stdin_;
    SyncAtomicUint32 called;
    burrow__Note interrupt_called, done;
    bool wait_done;
    bool done_err;
} CancelEnv;

static Error cancel_close_stdin(void *env) {
    CancelEnv *ce = (CancelEnv *)env;
    (void)ce->stdin_.vt->closer.close(ce->stdin_.data);
    return err_arbitrary;
}

static Error cancel_after_done(void *env) {
    CancelEnv *ce = (CancelEnv *)env;
    burrow__note_wake(&ce->interrupt_called);
    burrow__note_sleep(&ce->done);
    return fmt_errorf_v("%w: stdout closed", os_err_process_done);
}

static Error cancel_arbitrary(void *env) {
    CancelEnv *ce = (CancelEnv *)env;
    sync_atomic_uint32_store(&ce->called, 1);
    if (ce->wait_done)
        burrow__note_wake(&ce->interrupt_called);
    return ce->done_err ? fmt_errorf_v("%w: stdout closed", os_err_process_done)
                        : err_arbitrary;
}

static void TestCancelErrors_SuccessAfterError(TestingT *t) {
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    ExecCmd *c = HELPER_CTX0(t, ctx, "pipetest");
    Error err = BURROW_NO_ERROR;
    CancelEnv ce = {0};
    ce.stdin_ = exec_cmd_stdin_pipe(c, &err);
    CHECK_OK("StdinPipe", err);
    c->cancel = (ExecCancelFunc){cancel_close_stdin, &ce};
    CHECK_OK("Start", exec_cmd_start(c));
    BURROW_CALLF0(cancel);
    err = exec_cmd_wait(c);
    if (!errors_is(err, err_arbitrary) ||
        (err.vt == err_arbitrary.vt && err.data == err_arbitrary.data))
        testing_t_errorf_v(t, "Wait error = %s; want an error wrapping %s",
                           BURROW_FAILED(err) ? error_text(err) : S("<nil>"),
                           error_text(err_arbitrary));
    CHECK_S(error_text(err), S("exec: canceling Cmd: arbitrary error"));
    exec_cmd_free(c);
    context_release(ctx);
}

static void TestCancelErrors_SuccessAfterProcessDone(TestingT *t) {
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    ExecCmd *c = HELPER_CTX0(t, ctx, "pipetest");
    Error err = BURROW_NO_ERROR;
    CancelEnv ce = {0};
    burrow__note_init(&ce.interrupt_called);
    burrow__note_init(&ce.done);
    IoWriteCloser in = exec_cmd_stdin_pipe(c, &err);
    CHECK_OK("StdinPipe", err);
    IoReadCloser out = exec_cmd_stdout_pipe(c, &err);
    CHECK_OK("StdoutPipe", err);
    c->cancel = (ExecCancelFunc){cancel_after_done, &ce};
    CHECK_OK("Start", exec_cmd_start(c));
    BURROW_CALLF0(cancel);
    burrow__note_sleep(&ce.interrupt_called);
    (void)in.vt->closer.close(in.data);
    (void)io_read_all(a, io_read_closer_as_io_reader(out),
                      &err); /* reaches EOF when the process exits */
    burrow__note_wake(&ce.done);
    err = exec_cmd_wait(c);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Wait error = %s; want nil", error_text(err));
    exec_cmd_free(c);
    context_release(ctx);
    burrow__note_free(&ce.interrupt_called);
    burrow__note_free(&ce.done);
}

static void killed_after(TestingT *t, bool done_err) {
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    ExecCmd *c = HELPER_CTX0(t, ctx, "pipetest");
    Error err = BURROW_NO_ERROR;
    CancelEnv ce = {0};
    IoWriteCloser in = exec_cmd_stdin_pipe(c, &err);
    CHECK_OK("StdinPipe", err);
    ce.done_err = done_err;
    c->cancel = (ExecCancelFunc){cancel_arbitrary, &ce};
    c->wait_delay = TIME_MILLISECOND;
    CHECK_OK("Start", exec_cmd_start(c));
    BURROW_CALLF0(cancel);
    err = exec_cmd_wait(c);
    if (sync_atomic_uint32_load(&ce.called) == 0)
        testing_t_error_v(t, "Cancel was not called when the context was canceled");
    if (exit_error(err) == NULL)
        testing_t_errorf_v(t, "Wait error = %s; want ExecExitError",
                           BURROW_FAILED(err) ? error_text(err) : S("<nil>"));
    (void)in.vt->closer.close(in.data);
    exec_cmd_free(c);
    context_release(ctx);
}

static void TestCancelErrors_KilledAfterError(TestingT *t) {
    killed_after(t, false);
}

static void TestCancelErrors_KilledAfterSpuriousProcessDone(TestingT *t) {
    killed_after(t, true);
}

static void TestCancelErrors_NonzeroExitAfterError(TestingT *t) {
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    ExecCmd *c = HELPER_CTX0(t, ctx, "stderrfail");
    Error err = BURROW_NO_ERROR;
    CancelEnv ce = {0};
    burrow__note_init(&ce.interrupt_called);
    ce.wait_done = true;
    IoReadCloser errp = exec_cmd_stderr_pipe(c, &err);
    CHECK_OK("StderrPipe", err);
    c->cancel = (ExecCancelFunc){cancel_arbitrary, &ce};
    CHECK_OK("Start", exec_cmd_start(c));
    BURROW_CALLF0(cancel);
    burrow__note_sleep(&ce.interrupt_called);
    (void)io_read_all(a, io_read_closer_as_io_reader(errp), &err);
    err = exec_cmd_wait(c);
    const ExecExitError *ee = exit_error(err);
    if (ee == NULL || os_process_state_exit_code(ee->process_state) != 1)
        testing_t_errorf_v(t, "Wait error = %s; want exit status 1",
                           BURROW_FAILED(err) ? error_text(err) : S("<nil>"));
    exec_cmd_free(c);
    context_release(ctx);
    burrow__note_free(&ce.interrupt_called);
}

/* A context that is done before Start is the context's error, and nothing
 * runs. */
static void TestContextDoneBeforeStart(TestingT *t) {
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    BURROW_CALLF0(cancel);
    ExecCmd *c = HELPER_CTX0(t, ctx, "echo");
    Error err = exec_cmd_run(c);
    if (!errors_is(err, context_canceled))
        testing_t_errorf_v(t, "Run = %s; want context canceled",
                           BURROW_FAILED(err) ? error_text(err) : S("<nil>"));
    if (c->process != NULL)
        testing_t_error_v(t, "a process was started");
    exec_cmd_free(c);
    context_release(ctx);
}

static void TestCancelWithoutContext(TestingT *t) {
    ExecCmd *c = HELPER0(t, "echo");
    c->cancel = (ExecCancelFunc){kill_cancel, c};
    Error err = exec_cmd_start(c);
    CHECK_S(
        error_text(err),
        S("exec: command with a non-nil Cancel was not created with CommandContext"));
    exec_cmd_free(c);
}

static void TestAbsPathExec(TestingT *t) {
    Str exe = exe_path(t);
    Str args[1] = {S("echo")};
    ExecCmd *c = exec_command(a, exe, strs(args, 1));
    if (BURROW_FAILED(c->err))
        testing_t_errorf_v(t, "Command(%q).Err = %s; want nil", exe,
                           error_text(c->err));
    Error err = BURROW_NO_ERROR;
    (void)exec_cmd_output(c, a, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Output: %s", error_text(err));
    exec_cmd_free(c);

    /* Go's ExecCmd literal case: Path set, Args nil. */
    ExecCmd lit = {0};
    lit.path = exe;
    err = exec_cmd_run(&lit);
    if (exit_error(err) == NULL || os_process_state_exit_code(lit.process_state) != 2)
        testing_t_errorf_v(t, "Cmd{Path: exe}.Run() = %s; want exit status 2",
                           BURROW_FAILED(err) ? error_text(err) : S("<nil>"));
    exec_cmd_free(&lit);
}

static void TestStartTwice(TestingT *t) {
    ExecCmd *c = HELPER0(t, "echo");
    CHECK_OK("Start", exec_cmd_start(c));
    Error err = exec_cmd_start(c);
    CHECK_S(error_text(err), S("exec: already started"));
    CHECK_OK("Wait", exec_cmd_wait(c));
    err = exec_cmd_wait(c);
    CHECK_S(error_text(err), S("exec: Wait was already called"));
    exec_cmd_free(c);

    ExecCmd *d = HELPER0(t, "echo");
    err = exec_cmd_wait(d);
    CHECK_S(error_text(err), S("exec: not started"));
    exec_cmd_free(d);
}

static void TestPipeErrors(TestingT *t) {
    ExecCmd *c = HELPER0(t, "echo");
    Error err = BURROW_NO_ERROR;
    HeapWriter w;
    c->stdout_ = heap_writer(&w);
    (void)exec_cmd_stdout_pipe(c, &err);
    CHECK_S(error_text(err), S("exec: Stdout already set"));
    (void)exec_cmd_output(c, a, &err);
    CHECK_S(error_text(err), S("exec: Stdout already set"));
    (void)exec_cmd_combined_output(c, a, &err);
    CHECK_S(error_text(err), S("exec: Stdout already set"));
    bytes_buffer_free(&w.b);
    c->stdout_ = (IoWriter){NULL, NULL};
    CHECK_OK("Start", exec_cmd_start(c));
    (void)exec_cmd_stdin_pipe(c, &err);
    CHECK_S(error_text(err), S("exec: StdinPipe after process started"));
    (void)exec_cmd_stdout_pipe(c, &err);
    CHECK_S(error_text(err), S("exec: StdoutPipe after process started"));
    (void)exec_cmd_stderr_pipe(c, &err);
    CHECK_S(error_text(err), S("exec: StderrPipe after process started"));
    CHECK_OK("Wait", exec_cmd_wait(c));
    exec_cmd_free(c);
}

/* TestConcurrentExec's shape without goroutines: several Cmds started at
 * once, each with copying threads of its own, then all waited for. */
static void TestConcurrentExec(TestingT *t) {
    enum { N = 8 };
    ExecCmd *cs[N];
    HeapWriter outs[N];
    StringsReader ins[N];
    for (int i = 0; i < N; i++) {
        cs[i] = HELPER0(t, "cat");
        strings_reader_reset(&ins[i], fmt_sprintf_v(a, "input %d\n", (Int)i));
        cs[i]->stdin_ = strings_reader_as_io_reader(&ins[i]);
        cs[i]->stdout_ = heap_writer(&outs[i]);
        CHECK_OK("Start", exec_cmd_start(cs[i]));
    }
    for (int i = 0; i < N; i++) {
        CHECK_OK("Wait", exec_cmd_wait(cs[i]));
        CHECK_S(heap_writer_take(&outs[i]), fmt_sprintf_v(a, "input %d\n", (Int)i));
        exec_cmd_free(cs[i]);
    }
}

static void TestFreeWithoutWait(TestingT *t) {
    (void)t;
    Hang h;
    start_hang(t, &h, context_background(), 600000, false, 0, NULL, 0);
    /* Killed, reaped and freed, without hanging on the stdin copier. */
    exec_cmd_free(h.c);
    bytes_buffer_free(&h.stderr_.b);
}

#if !defined(BURROW_OS_WINDOWS)
static void TestImplicitPWD(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str cwd = os_getwd(a, &err);
    CHECK_OK("Getwd", err);
    Str up = filepath_dir(a, cwd);
    struct {
        Str dir, want;
    } cases[] = {
        {BURROW_STR_EMPTY, cwd},
        {S("."), cwd},
        {S(".."), up},
        {cwd, cwd},
        {fmt_sprintf_v(a, "%s/..", cwd), up},
    };
    bool have_pwd = false;
    (void)os_lookup_env(a, S("PWD"), &have_pwd);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ExecCmd *c = HELPER0(t, "pwd");
        c->dir = cases[i].dir;
        Slice env = exec_cmd_environ(c, a);
        Int npwd = 0;
        Str pwd = BURROW_STR_EMPTY;
        for (Int j = 0; j < env.len; j++) {
            Str kv = BURROW_AT(Str, env, j);
            if (strings_has_prefix(kv, S("PWD="))) {
                npwd++;
                pwd = strings_trim_prefix(kv, S("PWD="));
            }
        }
        Int want_n = cases[i].dir.len == 0 && !have_pwd ? 0 : 1;
        if (npwd != want_n ||
            (npwd == 1 && cases[i].dir.len > 0 && !str_eq(pwd, cases[i].want)))
            testing_t_errorf_v(t, "dir %q: PWD entries %d (%q), want %q", cases[i].dir,
                               npwd, pwd, cases[i].want);
        HeapWriter ew;
        c->stderr_ = heap_writer(&ew);
        Slice out = exec_cmd_output(c, a, &err);
        Str es = heap_writer_take(&ew);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s:\n%s", error_text(err), es);
        CHECK_S(strings_trim(text(out), S("\r\n")), cases[i].want);
        exec_cmd_free(c);
    }
}
#endif

/* ----------------------------------------------------------- LookPath */

static void TestLookPathNotFound(TestingT *t) {
    Str names[2] = {S("some-non-existent-path"), S("non-existent-path/slashed")};
    for (int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        Str path = exec_look_path(a, names[i], &err);
        if (BURROW_OK(err))
            testing_t_fatalf_v(t, "LookPath found %q in $PATH", names[i]);
        if (path.len != 0)
            testing_t_fatalf_v(t, "LookPath path == %q when err != nil", path);
        const ExecError *pe = (const ExecError *)errors_as(err, TYPE_EXEC_ERROR);
        if (pe == NULL)
            testing_t_fatal_v(t, "LookPath error is not an exec.Error");
        if (!str_eq(pe->name, names[i]))
            testing_t_fatalf_v(t, "want Error name %q, got %q", names[i], pe->name);
    }
}

#if !defined(BURROW_OS_WINDOWS)
static void TestLookPathUnixEmptyPath(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str tmp = os_mkdir_temp(a, BURROW_STR_EMPTY, S("osexec"), &err);
    CHECK_OK("MkdirTemp", err);
    Str old = os_getwd(a, &err);
    CHECK_OK("Getwd", err);
    CHECK_OK("Chdir", os_chdir(tmp));
    OsFile *f = os_open_file(a, S("exec_me"), OS_O_CREATE | OS_O_EXCL, 0700, &err);
    CHECK_OK("OpenFile", err);
    CHECK_OK("Close", os_file_close(f));
    bool had = false;
    Str path_env = os_lookup_env(a, S("PATH"), &had);
    (void)os_setenv(S("PATH"), BURROW_STR_EMPTY);
    Str path = exec_look_path(a, S("exec_me"), &err);
    (void)os_setenv(S("PATH"), path_env);
    (void)os_chdir(old);
    (void)os_remove_all(tmp);
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "LookPath found exec_me in empty $PATH");
    if (path.len != 0)
        testing_t_fatalf_v(t, "LookPath path == %q when err != nil", path);
}

/* dot_test.go's TestLookPath, on Unix. */
static void TestLookPathDot(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str base = os_mkdir_temp(a, BURROW_STR_EMPTY, S("osexec"), &err);
    CHECK_OK("MkdirTemp", err);
    Str tmp = filepath_join_v(a, 2, base, S("testdir"));
    CHECK_OK("Mkdir", os_mkdir(tmp, 0777));
    Byte junk[3] = {1, 2, 3};
    CHECK_OK("WriteFile", os_write_file(filepath_join_v(a, 2, tmp, S("execabs-test")),
                                        slice_from(junk, 3, 3, TYPE_BYTE), 0777));
    Str old = os_getwd(a, &err);
    CHECK_OK("Getwd", err);
    CHECK_OK("Chdir", os_chdir(tmp));
    Str orig = os_getenv(a, S("PATH"));

    Str dirs[2] = {S("."), S("../testdir")};
    for (int i = 0; i < 2; i++) {
        (void)os_setenv(S("PATH"), fmt_sprintf_v(a, "%s:%s", dirs[i], orig));
        Str good = fmt_sprintf_v(a, "%s/execabs-test", dirs[i]);
        Str found = exec_look_path(a, good, &err);
        if (BURROW_FAILED(err) || !strings_has_prefix(found, good))
            testing_t_errorf_v(t, "LookPath(%q) = %q, %s, want %q", good, found,
                               BURROW_FAILED(err) ? error_text(err) : S("<nil>"), good);
        (void)exec_look_path(a, S("execabs-test"), &err);
        if (!errors_is(err, exec_err_dot))
            testing_t_errorf_v(
                t, "LookPath returned unexpected error: want Is ErrDot, got %q",
                BURROW_FAILED(err) ? error_text(err) : S("<nil>"));
        ExecCmd *c = exec_command(a, S("execabs-test"), slice_nil(TYPE_STRING));
        if (!errors_is(c->err, exec_err_dot))
            testing_t_errorf_v(
                t, "Command returned unexpected error: want Is ErrDot, got %q",
                BURROW_FAILED(c->err) ? error_text(c->err) : S("<nil>"));
        c->err = BURROW_NO_ERROR;
        err = exec_cmd_run(c);
        if (BURROW_OK(err))
            testing_t_error_v(t, "Run did not fail: expected exec error");
        else if (errors_is(err, exec_err_dot))
            testing_t_errorf_v(t, "Run returned unexpected error ErrDot: %q",
                               error_text(err));
        exec_cmd_free(c);
    }

    (void)os_setenv(S("PATH"), fmt_sprintf_v(a, "%s:%s", tmp, orig));
    Str good = filepath_join_v(a, 2, tmp, S("execabs-test"));
    Str found = exec_look_path(a, S("execabs-test"), &err);
    if (BURROW_FAILED(err) || !strings_has_prefix(found, good))
        testing_t_errorf_v(t, "LookPath(execabs-test) = %q, want %q", found, good);
    ExecCmd *c = exec_command(a, S("execabs-test"), slice_nil(TYPE_STRING));
    if (BURROW_FAILED(c->err))
        testing_t_errorf_v(t, "Command.Err = %s; want nil", error_text(c->err));
    exec_cmd_free(c);

    (void)os_setenv(S("PATH"), BURROW_STR_EMPTY);
    Str bad[4] = {BURROW_STR_EMPTY, S("."), S("abc/.."), S("..")};
    for (int i = 0; i < 4; i++) {
        Str p = exec_look_path(a, bad[i], &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "%q: error expected, got nil", bad[i]);
        if (p.len != 0)
            testing_t_errorf_v(t, "%q: path returned should be \"\". Got %q", bad[i],
                               p);
    }

    (void)os_setenv(S("PATH"), orig);
    (void)os_chdir(old);
    (void)os_remove_all(base);
}
#endif

/* ------------------------------------------------------------------ main */

#if !defined(BURROW_OS_WINDOWS)
/* Writing to a pipe whose reader has gone is EPIPE for these tests, as in Go,
 * rather than the end of the test program. */
static bool on_sigpipe(int32_t sig, void *info, void *ctx) {
    (void)sig;
    (void)info;
    (void)ctx;
    return true;
}
#endif

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

#if !defined(BURROW_OS_WINDOWS)
#define UNIX_TESTS(X)                                                                  \
    X(TestPipeLookPathLeak)                                                            \
    X(TestImplicitPWD)                                                                 \
    X(TestLookPathUnixEmptyPath)                                                       \
    X(TestLookPathDot)
#else
#define UNIX_TESTS(X)
#endif

#define TESTS(X)                                                                       \
    X(TestEcho)                                                                        \
    X(TestCommandRelativeName)                                                         \
    X(TestCatStdin)                                                                    \
    X(TestEchoFileRace)                                                                \
    X(TestCatGoodAndBadFile)                                                           \
    X(TestNoExistExecutable)                                                           \
    X(TestExitStatus)                                                                  \
    X(TestExitCode)                                                                    \
    X(TestPipes)                                                                       \
    X(TestStdinClose)                                                                  \
    X(TestStdinCloseRace)                                                              \
    X(TestIgnorePipeErrorOnSuccess)                                                    \
    X(TestClosePipeOnCopyError)                                                        \
    X(TestOutputStderrCapture)                                                         \
    X(TestOutputStderrTruncated)                                                       \
    X(TestContext)                                                                     \
    X(TestContextCancel)                                                               \
    X(TestDedupEnvEcho)                                                                \
    X(TestEnvNULCharacter)                                                             \
    X(TestDedupEnv)                                                                    \
    X(TestString)                                                                      \
    X(TestStringPathNotResolved)                                                       \
    X(TestNoPath)                                                                      \
    X(TestCmdLiteral)                                                                  \
    X(TestDoubleStartLeavesPipesOpen)                                                  \
    X(TestWaitInterrupt_Wait)                                                          \
    X(TestWaitInterrupt_KillHang)                                                      \
    X(TestWaitInterrupt_ExitHang)                                                      \
    X(TestCancelErrors_SuccessAfterError)                                              \
    X(TestCancelErrors_SuccessAfterProcessDone)                                        \
    X(TestCancelErrors_KilledAfterError)                                               \
    X(TestCancelErrors_KilledAfterSpuriousProcessDone)                                 \
    X(TestCancelErrors_NonzeroExitAfterError)                                          \
    X(TestContextDoneBeforeStart)                                                      \
    X(TestCancelWithoutContext)                                                        \
    X(TestAbsPathExec)                                                                 \
    X(TestStartTwice)                                                                  \
    X(TestPipeErrors)                                                                  \
    X(TestConcurrentExec)                                                              \
    X(TestFreeWithoutWait)                                                             \
    X(TestLookPathNotFound)                                                            \
    UNIX_TESTS(X)

static int os_osexec_main(TestingM *m) {
    setup();
    if (os_getenv(a, S(HELPER_VAR)).len > 0)
        run_helper();
    (void)os_setenv(S(HELPER_VAR), S("1"));
#if defined(BURROW_OS_WINDOWS)
    (void)os_setenv(S("NoDefaultCurrentDirectoryInExePath"), S("TRUE"));
#else
    (void)pal_signal_install(PAL_SIGPIPE, on_sigpipe, NULL);
#endif
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(os_osexec_main, TESTS)
