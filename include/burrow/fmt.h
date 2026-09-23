/* fmt, Go's formatted printing.
 *
 * The verbs, the flags, the widths and precisions, the argument indexes and
 * the error strings for a bad verb are Go's, and the tests are Go's table run
 * through this code. What changes is how the arguments arrive. Go packs them
 * into a []any at the call site without being asked; C has no such step, so
 * every function here takes a Slice of Any, and a _v macro next to each one
 * builds that Slice from the values you write:
 *
 *     fmt_printf_v("%s is %d years old\n", name, age);
 *     Str s = fmt_sprintf_v(a, "%08.3f", x);
 *
 * The macro turns each argument into an Any with BURROW_ANY_OF, which picks the
 * Go type from the C type, and a value whose type it does not know is a compile
 * error rather than a crash. burrow/iface.h has the table. A struct goes in
 * through BURROW_ANY with its descriptor, and then %v, %+v and %#v print it the
 * way Go does, field names and all.
 *
 * Methods work through the descriptor too. A type whose descriptor lists a
 * String, Error, GoString or Format method with Go's signature is printed by
 * that method, which is how Stringer and Formatter are satisfied here: by
 * declaring the method with BURROW_STRUCT_DEFINE_METHODS, and not by filling in
 * a vtable. The interface types below exist for code that wants to hold one.
 *
 * The functions that write return the byte count and report a write error
 * through a trailing Error pointer, which may be NULL. The ones that build a
 * string take an allocator for it, and fmt_errorf puts its error in the calling
 * goroutine's error arena like every other error in the library.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package fmt */

#ifndef BURROW_FMT_H
#define BURROW_FMT_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------- printing
 *
 * Nine functions in three families, as in Go. The f family takes a format.
 * The plain family puts a space between two operands when neither is a string.
 * The ln family always puts a space between operands and ends with a newline.
 *
 * Each family writes to an IoWriter, to standard output, to a new string, or
 * onto the end of a byte slice. Standard output here is C's stdout, so output
 * from fmt_printf and from printf comes out in the order it was written. */

Int fmt_fprintf(IoWriter w, Str format, Slice args, Error *err);
Int fmt_printf(Str format, Slice args, Error *err);
BURROW_OWNS(ret) Str fmt_sprintf(Alloc *a, Str format, Slice args);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice fmt_appendf(Alloc *a, Slice b, Str format,
                                                          Slice args);

Int fmt_fprint(IoWriter w, Slice args, Error *err);
Int fmt_print(Slice args, Error *err);
BURROW_OWNS(ret) Str fmt_sprint(Alloc *a, Slice args);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice fmt_append(Alloc *a, Slice b, Slice args);

Int fmt_fprintln(IoWriter w, Slice args, Error *err);
Int fmt_println(Slice args, Error *err);
BURROW_OWNS(ret) Str fmt_sprintln(Alloc *a, Slice args);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice fmt_appendln(Alloc *a, Slice b,
                                                           Slice args);

/* Go's Errorf. The message is formatted as by fmt_sprintf, and each %w operand
 * must be an Error, which the result then wraps: one %w gives an error that
 * errors_unwrap sees through, and several give one that errors_is and
 * errors_as search the way they search errors_join's. %w on anything else is a
 * bad verb, as in Go.
 *
 * The result lives in the calling goroutine's error arena, as do the errors it
 * wraps as far as it is concerned, since it holds them rather than copying
 * them. error_retain copies the whole chain out. */
BURROW_BORROWS(ret) Error fmt_errorf(Str format, Slice args);

/* ---------------------------------------------------------------- interfaces */

/* fmt.State, what a Format method is handed. Write adds to the output, and the
 * rest report the flags, width and precision of the verb being formatted. flag
 * takes the flag's character, such as '#' or '-'. */
typedef struct FmtStateVT {
    const Type *self_type;
    Int (*write)(void *self, Slice b, Error *err);
    Int (*width)(void *self, bool *ok);
    Int (*precision)(void *self, bool *ok);
    bool (*flag)(void *self, Int c);
} FmtStateVT;

typedef struct FmtState {
    const FmtStateVT *vt;
    void *data;
} FmtState;

/* The descriptor, which a Format method names in its signature:
 *
 *     #define T_SIG_Format(IN, OUT) IN(0, FmtState) IN(1, Rune)
 */
extern const Type burrow_type_FmtState;
#define TYPE_FMT_STATE (&burrow_type_FmtState)

/* A State as an io.Writer, so that a Format method can print into it with
 * fmt_fprintf the way Go code does with fmt.Fprintf(f, ...). The writer points
 * at *st, which has to outlive it; the FmtState a Format method is given lives
 * for the whole call, which is as long as anything needs. */
IoWriter fmt_state_writer(FmtState *st);

/* fmt.Formatter, a type that formats itself for every verb. */
typedef struct FmtFormatterVT {
    const Type *self_type;
    void (*format)(void *self, FmtState f, Rune verb);
} FmtFormatterVT;

typedef struct FmtFormatter {
    const FmtFormatterVT *vt;
    void *data;
} FmtFormatter;

/* fmt.Stringer, a type with a String method, which %v and %s use. */
typedef struct FmtStringerVT {
    const Type *self_type;
    Str (*string)(void *self);
} FmtStringerVT;

typedef struct FmtStringer {
    const FmtStringerVT *vt;
    void *data;
} FmtStringer;

/* fmt.GoStringer, a type with a GoString method, which %#v uses. */
typedef struct FmtGoStringerVT {
    const Type *self_type;
    Str (*go_string)(void *self);
} FmtGoStringerVT;

typedef struct FmtGoStringer {
    const FmtGoStringerVT *vt;
    void *data;
} FmtGoStringer;

/* The directive that would reproduce state's flags, width and precision with
 * verb, such as "%-8.3f", for a Format method that handles some verbs itself
 * and passes the rest on to fmt_sprintf. */
BURROW_OWNS(ret) Str fmt_format_string(Alloc *a, FmtState state, Rune verb);

/* ----------------------------------------------------------------- scanning
 *
 * Nine functions in three families again. The plain family treats newlines as
 * space. The ln family stops at a newline and wants one, or the end of input,
 * after the last operand. The f family follows a format, where the verbs are
 * the printing verbs and anything else has to match the input.
 *
 * Each family reads from standard input, from a string, or from an IoReader.
 * Standard input is C's stdin, read a byte at a time, so what is left after a
 * scan is still there for getc, apart from the one rune a scan may have looked
 * at and put back, which Go loses too.
 *
 * An operand says where to store a value. It is an Any whose type is the type
 * stored and whose data points at the variable, so an Int to fill in is
 *
 *     BURROW_ANY(TYPE_INT, &n)
 *
 * and fmt_sscan_v and the other _v forms below build that from &n for you. The
 * value is picked by kind, so a type declared on top of int or string scans
 * the same way. A Slice operand holding bytes gets a new slice. Strings and
 * byte slices are allocated from a, which nothing else touches.
 *
 * All nine return how many operands were filled in, and the error, if any,
 * through err, which may be NULL. Running out of input before the first
 * operand is io_eof, and running out partway through one is
 * io_err_unexpected_eof. Errors live in the goroutine's error arena. */
Int fmt_scan(Alloc *a, Slice args, Error *err);
Int fmt_scanln(Alloc *a, Slice args, Error *err);
Int fmt_scanf(Alloc *a, Str format, Slice args, Error *err);

Int fmt_sscan(Alloc *a, Str str, Slice args, Error *err);
Int fmt_sscanln(Alloc *a, Str str, Slice args, Error *err);
Int fmt_sscanf(Alloc *a, Str str, Str format, Slice args, Error *err);

Int fmt_fscan(Alloc *a, IoReader r, Slice args, Error *err);
Int fmt_fscanln(Alloc *a, IoReader r, Slice args, Error *err);
Int fmt_fscanf(Alloc *a, IoReader r, Str format, Slice args, Error *err);

/* fmt.ScanState, what a Scan method is handed.
 *
 * read_rune gives the next rune and its size in bytes, with io_eof through err
 * at the end of the input or at the width the verb allows. unread_rune puts
 * the last one back. skip_space skips spaces, and newlines too when the scan
 * treats them as space. token skips space when asked and then returns the run
 * of runes that satisfy f, or that are not space when f is nil. The token
 * points into the scanner's buffer and lasts until the next call. width is the
 * verb's width, if it has one. read is there because Go has it, and it always
 * fails. */
typedef struct FmtScanStateVT {
    const Type *self_type;
    Rune (*read_rune)(void *self, Int *size, Error *err);
    Error (*unread_rune)(void *self);
    void (*skip_space)(void *self);
    BURROW_BORROWS(ret, self) Slice (*token)(void *self, bool skip_space, RuneFunc f,
                                             Error *err);
    Int (*width)(void *self, bool *ok);
    Int (*read)(void *self, Slice buf, Error *err);
} FmtScanStateVT;

typedef struct FmtScanState {
    const FmtScanStateVT *vt;
    void *data;
} FmtScanState;

/* The descriptor, which a Scan method names in its signature:
 *
 *     #define T_SIG_Scan(IN, OUT) IN(0, FmtScanState) IN(1, Rune) OUT(Error)
 *
 * A type whose descriptor lists a Scan method with that signature is a
 * Scanner, and every scan hands its operands of that type to the method. */
extern const Type burrow_type_FmtScanState;
#define TYPE_FMT_SCAN_STATE (&burrow_type_FmtScanState)

/* A ScanState as an IoReader, so that a Scan method can scan into its own
 * fields with fmt_fscan the way Go code does with fmt.Fscan(state, ...). The
 * nested scan reads through the state, which keeps the outer scan's width and
 * position right. The reader points at *st, which has to outlive it. */
IoReader fmt_scan_state_reader(FmtScanState *st);

/* fmt.Scanner, a type that scans itself. */
typedef struct FmtScannerVT {
    const Type *self_type;
    Error (*scan)(void *self, FmtScanState state, Rune verb);
} FmtScannerVT;

typedef struct FmtScanner {
    const FmtScannerVT *vt;
    void *data;
} FmtScanner;

/* The operand for a pointer, which is what the scanning _v forms wrap each of
 * their arguments in. It knows the pointers to every builtin number type, to
 * bool, to Str and to Slice, which is a byte slice, and it passes an Any
 * through untouched, which is how a type with a Scan method or a type of your
 * own goes in. Anything else is a compile error. */
typedef struct burrow__ScanBox {
    Type t;
} burrow__ScanBox;

BURROW_BORROWS(ret, v) Any burrow__scan_of_any(Any v, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_str(Str *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_slice(Slice *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_bool(bool *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_f32(float *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_f64(double *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_c64(Complex64 *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_c128(Complex128 *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_int(Int *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_uint(Uint *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_cint(int *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_cuint(unsigned *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_i8(int8_t *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_i16(int16_t *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_i32(int32_t *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_i64(int64_t *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_u8(uint8_t *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_u16(uint16_t *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_u32(uint32_t *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_u64(uint64_t *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_long(long *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_ulong(unsigned long *p,
                                                 burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_llong(long long *p, burrow__ScanBox *box);
BURROW_BORROWS(ret, p) Any burrow__scan_of_ullong(unsigned long long *p,
                                                  burrow__ScanBox *box);

/* In steps for the reason BURROW_ANY_OF is: a _Generic may not name the same
 * type twice, and which of these are the same type differs by platform. */
/* Where the chain ends for a pointer to anything else. It is not a function,
 * so the call that follows fails to compile and names the problem. Every
 * branch of a _Generic has to be valid whether or not it is chosen, which is
 * why the last one cannot simply be left out. */
extern const int burrow__scan_operand_type_not_supported;

#define BURROW__SCAN_OF5(p)                                                            \
    _Generic((p),                                                                      \
        long *: burrow__scan_of_long,                                                  \
        unsigned long *: burrow__scan_of_ulong,                                        \
        long long *: burrow__scan_of_llong,                                            \
        unsigned long long *: burrow__scan_of_ullong,                                  \
        default: burrow__scan_operand_type_not_supported)

#define BURROW__SCAN_OF4(p)                                                            \
    _Generic((p),                                                                      \
        int8_t *: burrow__scan_of_i8,                                                  \
        int16_t *: burrow__scan_of_i16,                                                \
        int32_t *: burrow__scan_of_i32,                                                \
        int64_t *: burrow__scan_of_i64,                                                \
        uint8_t *: burrow__scan_of_u8,                                                 \
        uint16_t *: burrow__scan_of_u16,                                               \
        uint32_t *: burrow__scan_of_u32,                                               \
        uint64_t *: burrow__scan_of_u64,                                               \
        default: BURROW__SCAN_OF5(p))

#define BURROW__SCAN_OF3(p)                                                            \
    _Generic((p),                                                                      \
        int *: burrow__scan_of_cint,                                                   \
        unsigned *: burrow__scan_of_cuint,                                             \
        default: BURROW__SCAN_OF4(p))

#define BURROW__SCAN_OF2(p)                                                            \
    _Generic((p),                                                                      \
        Int *: burrow__scan_of_int,                                                    \
        Uint *: burrow__scan_of_uint,                                                  \
        default: BURROW__SCAN_OF3(p))

#define BURROW_SCAN_OF(p)                                                              \
    _Generic((p),                                                                      \
        Any: burrow__scan_of_any,                                                      \
        Str *: burrow__scan_of_str,                                                    \
        Slice *: burrow__scan_of_slice,                                                \
        bool *: burrow__scan_of_bool,                                                  \
        float *: burrow__scan_of_f32,                                                  \
        double *: burrow__scan_of_f64,                                                 \
        Complex64 *: burrow__scan_of_c64,                                              \
        Complex128 *: burrow__scan_of_c128,                                            \
        default: BURROW__SCAN_OF2(p))((p), &(burrow__ScanBox){.t = {.size = 0}})

/* --------------------------------------------------------- the _v forms
 *
 * Each takes the operands as ordinary arguments, up to thirty two of them, and
 * wraps every one in BURROW_ANY_OF. The format may be a string literal, a
 * char * or a Str.
 *
 *     fmt_println_v("total", n, "items");
 *     Error err = fmt_errorf_v("open %s: %w", path, cause);
 *
 * The operands live in compound literals, which last until the end of the
 * enclosing block, so nothing here allocates to hold them. fmt_print_v,
 * fmt_println_v and their relatives need at least one operand, since C cannot
 * spell a macro call with none. */

#define BURROW__FMT_STR(f)                                                             \
    _Generic((f), Str: burrow__fmt_str_id, default: str_from_cstr)(f)
BURROW_BORROWS(ret, s) static inline Str burrow__fmt_str_id(Str s) {
    return s;
}

#define BURROW__FMT_NARGS(...)                                                         \
    BURROW__FMT_NARGS_(__VA_ARGS__, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22,    \
                       21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5,  \
                       4, 3, 2, 1, 0)
#define BURROW__FMT_NARGS_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13,     \
                           _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, \
                           _26, _27, _28, _29, _30, _31, _32, _33, N, ...)             \
    N
#define BURROW__FMT_CAT(a, b) BURROW__FMT_CAT_(a, b)
#define BURROW__FMT_CAT_(a, b) a##b

#define BURROW__FMT_SLICE(n, ...) ((Slice){(Any[]){__VA_ARGS__}, (n), (n), TYPE_ANY})

/* The operands as a Slice of Any, one macro per count, each one boxed by M. */
#define BURROW__FMT_A1(M, x1) BURROW__FMT_SLICE(1, M(x1))
#define BURROW__FMT_A2(M, x1, x2) BURROW__FMT_SLICE(2, M(x1), M(x2))
#define BURROW__FMT_A3(M, x1, x2, x3) BURROW__FMT_SLICE(3, M(x1), M(x2), M(x3))
#define BURROW__FMT_A4(M, x1, x2, x3, x4)                                              \
    BURROW__FMT_SLICE(4, M(x1), M(x2), M(x3), M(x4))
#define BURROW__FMT_A5(M, x1, x2, x3, x4, x5)                                          \
    BURROW__FMT_SLICE(5, M(x1), M(x2), M(x3), M(x4), M(x5))
#define BURROW__FMT_A6(M, x1, x2, x3, x4, x5, x6)                                      \
    BURROW__FMT_SLICE(6, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6))
#define BURROW__FMT_A7(M, x1, x2, x3, x4, x5, x6, x7)                                  \
    BURROW__FMT_SLICE(7, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7))
#define BURROW__FMT_A8(M, x1, x2, x3, x4, x5, x6, x7, x8)                              \
    BURROW__FMT_SLICE(8, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8))
#define BURROW__FMT_A9(M, x1, x2, x3, x4, x5, x6, x7, x8, x9)                          \
    BURROW__FMT_SLICE(9, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8), M(x9))
#define BURROW__FMT_A10(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10)                    \
    BURROW__FMT_SLICE(10, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10))
#define BURROW__FMT_A11(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11)               \
    BURROW__FMT_SLICE(11, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11))
#define BURROW__FMT_A12(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12)          \
    BURROW__FMT_SLICE(12, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12))
#define BURROW__FMT_A13(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13)     \
    BURROW__FMT_SLICE(13, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13))
#define BURROW__FMT_A14(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14)                                                           \
    BURROW__FMT_SLICE(14, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14))
#define BURROW__FMT_A15(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15)                                                      \
    BURROW__FMT_SLICE(15, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15))
#define BURROW__FMT_A16(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16)                                                 \
    BURROW__FMT_SLICE(16, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16))
#define BURROW__FMT_A17(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17)                                            \
    BURROW__FMT_SLICE(17, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17))
#define BURROW__FMT_A18(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18)                                       \
    BURROW__FMT_SLICE(18, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18))
#define BURROW__FMT_A19(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19)                                  \
    BURROW__FMT_SLICE(19, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19))
#define BURROW__FMT_A20(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20)                             \
    BURROW__FMT_SLICE(20, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20))
#define BURROW__FMT_A21(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21)                        \
    BURROW__FMT_SLICE(21, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21))
#define BURROW__FMT_A22(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22)                   \
    BURROW__FMT_SLICE(22, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22))
#define BURROW__FMT_A23(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23)              \
    BURROW__FMT_SLICE(23, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22), M(x23))
#define BURROW__FMT_A24(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24)         \
    BURROW__FMT_SLICE(24, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22), M(x23), M(x24))
#define BURROW__FMT_A25(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25)    \
    BURROW__FMT_SLICE(25, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22), M(x23), M(x24),  \
                      M(x25))
#define BURROW__FMT_A26(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26)                                                           \
    BURROW__FMT_SLICE(26, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22), M(x23), M(x24),  \
                      M(x25), M(x26))
#define BURROW__FMT_A27(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27)                                                      \
    BURROW__FMT_SLICE(27, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22), M(x23), M(x24),  \
                      M(x25), M(x26), M(x27))
#define BURROW__FMT_A28(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28)                                                 \
    BURROW__FMT_SLICE(28, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22), M(x23), M(x24),  \
                      M(x25), M(x26), M(x27), M(x28))
#define BURROW__FMT_A29(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29)                                            \
    BURROW__FMT_SLICE(29, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22), M(x23), M(x24),  \
                      M(x25), M(x26), M(x27), M(x28), M(x29))
#define BURROW__FMT_A30(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30)                                       \
    BURROW__FMT_SLICE(30, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22), M(x23), M(x24),  \
                      M(x25), M(x26), M(x27), M(x28), M(x29), M(x30))
#define BURROW__FMT_A31(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30, x31)                                  \
    BURROW__FMT_SLICE(31, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22), M(x23), M(x24),  \
                      M(x25), M(x26), M(x27), M(x28), M(x29), M(x30), M(x31))
#define BURROW__FMT_A32(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30, x31, x32)                             \
    BURROW__FMT_SLICE(32, M(x1), M(x2), M(x3), M(x4), M(x5), M(x6), M(x7), M(x8),      \
                      M(x9), M(x10), M(x11), M(x12), M(x13), M(x14), M(x15), M(x16),   \
                      M(x17), M(x18), M(x19), M(x20), M(x21), M(x22), M(x23), M(x24),  \
                      M(x25), M(x26), M(x27), M(x28), M(x29), M(x30), M(x31), M(x32))

/* A format and then its operands, as the two arguments a function takes. */
#define BURROW__FMT_F1(M, f) BURROW__FMT_STR(f), slice_nil(TYPE_ANY)
#define BURROW__FMT_F2(M, f, x1) BURROW__FMT_STR(f), BURROW__FMT_A1(M, x1)
#define BURROW__FMT_F3(M, f, x1, x2) BURROW__FMT_STR(f), BURROW__FMT_A2(M, x1, x2)
#define BURROW__FMT_F4(M, f, x1, x2, x3)                                               \
    BURROW__FMT_STR(f), BURROW__FMT_A3(M, x1, x2, x3)
#define BURROW__FMT_F5(M, f, x1, x2, x3, x4)                                           \
    BURROW__FMT_STR(f), BURROW__FMT_A4(M, x1, x2, x3, x4)
#define BURROW__FMT_F6(M, f, x1, x2, x3, x4, x5)                                       \
    BURROW__FMT_STR(f), BURROW__FMT_A5(M, x1, x2, x3, x4, x5)
#define BURROW__FMT_F7(M, f, x1, x2, x3, x4, x5, x6)                                   \
    BURROW__FMT_STR(f), BURROW__FMT_A6(M, x1, x2, x3, x4, x5, x6)
#define BURROW__FMT_F8(M, f, x1, x2, x3, x4, x5, x6, x7)                               \
    BURROW__FMT_STR(f), BURROW__FMT_A7(M, x1, x2, x3, x4, x5, x6, x7)
#define BURROW__FMT_F9(M, f, x1, x2, x3, x4, x5, x6, x7, x8)                           \
    BURROW__FMT_STR(f), BURROW__FMT_A8(M, x1, x2, x3, x4, x5, x6, x7, x8)
#define BURROW__FMT_F10(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9)                      \
    BURROW__FMT_STR(f), BURROW__FMT_A9(M, x1, x2, x3, x4, x5, x6, x7, x8, x9)
#define BURROW__FMT_F11(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10)                 \
    BURROW__FMT_STR(f), BURROW__FMT_A10(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10)
#define BURROW__FMT_F12(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11)            \
    BURROW__FMT_STR(f), BURROW__FMT_A11(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11)
#define BURROW__FMT_F13(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12)       \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A12(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12)
#define BURROW__FMT_F14(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13)  \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A13(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13)
#define BURROW__FMT_F15(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14)                                                           \
    BURROW__FMT_STR(f), BURROW__FMT_A14(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,    \
                                        x11, x12, x13, x14)
#define BURROW__FMT_F16(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15)                                                      \
    BURROW__FMT_STR(f), BURROW__FMT_A15(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,    \
                                        x11, x12, x13, x14, x15)
#define BURROW__FMT_F17(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16)                                                 \
    BURROW__FMT_STR(f), BURROW__FMT_A16(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,    \
                                        x11, x12, x13, x14, x15, x16)
#define BURROW__FMT_F18(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17)                                            \
    BURROW__FMT_STR(f), BURROW__FMT_A17(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,    \
                                        x11, x12, x13, x14, x15, x16, x17)
#define BURROW__FMT_F19(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18)                                       \
    BURROW__FMT_STR(f), BURROW__FMT_A18(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,    \
                                        x11, x12, x13, x14, x15, x16, x17, x18)
#define BURROW__FMT_F20(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19)                                  \
    BURROW__FMT_STR(f), BURROW__FMT_A19(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,    \
                                        x11, x12, x13, x14, x15, x16, x17, x18, x19)
#define BURROW__FMT_F21(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20)                             \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A20(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20)
#define BURROW__FMT_F22(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21)                        \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A21(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21)
#define BURROW__FMT_F23(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22)                   \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A22(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22)
#define BURROW__FMT_F24(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23)              \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A23(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23)
#define BURROW__FMT_F25(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24)         \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A24(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24)
#define BURROW__FMT_F26(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25)    \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A25(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25)
#define BURROW__FMT_F27(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26)                                                           \
    BURROW__FMT_STR(f), BURROW__FMT_A26(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,    \
                                        x11, x12, x13, x14, x15, x16, x17, x18, x19,   \
                                        x20, x21, x22, x23, x24, x25, x26)
#define BURROW__FMT_F28(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27)                                                      \
    BURROW__FMT_STR(f), BURROW__FMT_A27(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,    \
                                        x11, x12, x13, x14, x15, x16, x17, x18, x19,   \
                                        x20, x21, x22, x23, x24, x25, x26, x27)
#define BURROW__FMT_F29(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28)                                                 \
    BURROW__FMT_STR(f), BURROW__FMT_A28(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,    \
                                        x11, x12, x13, x14, x15, x16, x17, x18, x19,   \
                                        x20, x21, x22, x23, x24, x25, x26, x27, x28)
#define BURROW__FMT_F30(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29)                                            \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A29(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29)
#define BURROW__FMT_F31(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30)                                       \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A30(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30)
#define BURROW__FMT_F32(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30, x31)                                  \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A31(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30, x31)
#define BURROW__FMT_F33(M, f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,  \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30, x31, x32)                             \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A32(M, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30, x31, x32)

#define BURROW__FMT_ARGS(M, ...)                                                       \
    BURROW__FMT_CAT(BURROW__FMT_A, BURROW__FMT_NARGS(__VA_ARGS__))(M, __VA_ARGS__)
#define BURROW__FMT_FARGS(M, ...)                                                      \
    BURROW__FMT_CAT(BURROW__FMT_F, BURROW__FMT_NARGS(__VA_ARGS__))(M, __VA_ARGS__)

#define fmt_printf_v(...)                                                              \
    fmt_printf(BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__), NULL)
#define fmt_fprintf_v(w, ...)                                                          \
    fmt_fprintf((w), BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__), NULL)
#define fmt_sprintf_v(a, ...)                                                          \
    fmt_sprintf((a), BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define fmt_appendf_v(a, b, ...)                                                       \
    fmt_appendf((a), (b), BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define fmt_errorf_v(...) fmt_errorf(BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))

#define fmt_print_v(...) fmt_print(BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__), NULL)
#define fmt_fprint_v(w, ...)                                                           \
    fmt_fprint((w), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__), NULL)
#define fmt_sprint_v(a, ...)                                                           \
    fmt_sprint((a), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define fmt_append_v(a, b, ...)                                                        \
    fmt_append((a), (b), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))

#define fmt_println_v(...)                                                             \
    fmt_println(BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__), NULL)
#define fmt_fprintln_v(w, ...)                                                         \
    fmt_fprintln((w), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__), NULL)
#define fmt_sprintln_v(a, ...)                                                         \
    fmt_sprintln((a), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define fmt_appendln_v(a, b, ...)                                                      \
    fmt_appendln((a), (b), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))

/* The scanning _v forms take pointers, which BURROW_SCAN_OF turns into
 * operands, and the error pointer comes second, after the allocator, since it
 * cannot come last:
 *
 *     Int n, m;
 *     Error err;
 *     fmt_sscanf_v(a, &err, "3 of 4", "%d of %d", &n, &m);
 */
#define fmt_scan_v(a, err, ...)                                                        \
    fmt_scan((a), BURROW__FMT_ARGS(BURROW_SCAN_OF, __VA_ARGS__), (err))
#define fmt_scanln_v(a, err, ...)                                                      \
    fmt_scanln((a), BURROW__FMT_ARGS(BURROW_SCAN_OF, __VA_ARGS__), (err))
#define fmt_scanf_v(a, err, ...)                                                       \
    fmt_scanf((a), BURROW__FMT_FARGS(BURROW_SCAN_OF, __VA_ARGS__), (err))

#define fmt_sscan_v(a, err, str, ...)                                                  \
    fmt_sscan((a), BURROW__FMT_STR(str),                                               \
              BURROW__FMT_ARGS(BURROW_SCAN_OF, __VA_ARGS__), (err))
#define fmt_sscanln_v(a, err, str, ...)                                                \
    fmt_sscanln((a), BURROW__FMT_STR(str),                                             \
                BURROW__FMT_ARGS(BURROW_SCAN_OF, __VA_ARGS__), (err))
#define fmt_sscanf_v(a, err, str, ...)                                                 \
    fmt_sscanf((a), BURROW__FMT_STR(str),                                              \
               BURROW__FMT_FARGS(BURROW_SCAN_OF, __VA_ARGS__), (err))

#define fmt_fscan_v(a, err, r, ...)                                                    \
    fmt_fscan((a), (r), BURROW__FMT_ARGS(BURROW_SCAN_OF, __VA_ARGS__), (err))
#define fmt_fscanln_v(a, err, r, ...)                                                  \
    fmt_fscanln((a), (r), BURROW__FMT_ARGS(BURROW_SCAN_OF, __VA_ARGS__), (err))
#define fmt_fscanf_v(a, err, r, ...)                                                   \
    fmt_fscanf((a), (r), BURROW__FMT_FARGS(BURROW_SCAN_OF, __VA_ARGS__), (err))

#ifdef __cplusplus
}
#endif

#endif /* BURROW_FMT_H */
