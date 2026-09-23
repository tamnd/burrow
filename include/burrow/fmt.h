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

/* The operands as a Slice of Any, one macro per count. */
#define BURROW__FMT_A1(x1) BURROW__FMT_SLICE(1, BURROW_ANY_OF(x1))
#define BURROW__FMT_A2(x1, x2)                                                         \
    BURROW__FMT_SLICE(2, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2))
#define BURROW__FMT_A3(x1, x2, x3)                                                     \
    BURROW__FMT_SLICE(3, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3))
#define BURROW__FMT_A4(x1, x2, x3, x4)                                                 \
    BURROW__FMT_SLICE(4, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),      \
                      BURROW_ANY_OF(x4))
#define BURROW__FMT_A5(x1, x2, x3, x4, x5)                                             \
    BURROW__FMT_SLICE(5, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),      \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5))
#define BURROW__FMT_A6(x1, x2, x3, x4, x5, x6)                                         \
    BURROW__FMT_SLICE(6, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),      \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6))
#define BURROW__FMT_A7(x1, x2, x3, x4, x5, x6, x7)                                     \
    BURROW__FMT_SLICE(7, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),      \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7))
#define BURROW__FMT_A8(x1, x2, x3, x4, x5, x6, x7, x8)                                 \
    BURROW__FMT_SLICE(8, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),      \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8))
#define BURROW__FMT_A9(x1, x2, x3, x4, x5, x6, x7, x8, x9)                             \
    BURROW__FMT_SLICE(9, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),      \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9))
#define BURROW__FMT_A10(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10)                       \
    BURROW__FMT_SLICE(10, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10))
#define BURROW__FMT_A11(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11)                  \
    BURROW__FMT_SLICE(11, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11))
#define BURROW__FMT_A12(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12)             \
    BURROW__FMT_SLICE(12, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12))
#define BURROW__FMT_A13(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13)        \
    BURROW__FMT_SLICE(13, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13))
#define BURROW__FMT_A14(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14)   \
    BURROW__FMT_SLICE(14, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14))
#define BURROW__FMT_A15(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15)                                                           \
    BURROW__FMT_SLICE(15, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15))
#define BURROW__FMT_A16(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16)                                                      \
    BURROW__FMT_SLICE(16, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16))
#define BURROW__FMT_A17(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17)                                                 \
    BURROW__FMT_SLICE(17, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17))
#define BURROW__FMT_A18(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18)                                            \
    BURROW__FMT_SLICE(18, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18))
#define BURROW__FMT_A19(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19)                                       \
    BURROW__FMT_SLICE(19, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19))
#define BURROW__FMT_A20(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20)                                  \
    BURROW__FMT_SLICE(20, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20))
#define BURROW__FMT_A21(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21)                             \
    BURROW__FMT_SLICE(21, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21))
#define BURROW__FMT_A22(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22)                        \
    BURROW__FMT_SLICE(22, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22))
#define BURROW__FMT_A23(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23)                   \
    BURROW__FMT_SLICE(23, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22), BURROW_ANY_OF(x23))
#define BURROW__FMT_A24(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24)              \
    BURROW__FMT_SLICE(24, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22), BURROW_ANY_OF(x23), BURROW_ANY_OF(x24))
#define BURROW__FMT_A25(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25)         \
    BURROW__FMT_SLICE(25, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22), BURROW_ANY_OF(x23), BURROW_ANY_OF(x24),      \
                      BURROW_ANY_OF(x25))
#define BURROW__FMT_A26(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26)    \
    BURROW__FMT_SLICE(26, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22), BURROW_ANY_OF(x23), BURROW_ANY_OF(x24),      \
                      BURROW_ANY_OF(x25), BURROW_ANY_OF(x26))
#define BURROW__FMT_A27(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,    \
                        x27)                                                           \
    BURROW__FMT_SLICE(27, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22), BURROW_ANY_OF(x23), BURROW_ANY_OF(x24),      \
                      BURROW_ANY_OF(x25), BURROW_ANY_OF(x26), BURROW_ANY_OF(x27))
#define BURROW__FMT_A28(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,    \
                        x27, x28)                                                      \
    BURROW__FMT_SLICE(28, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22), BURROW_ANY_OF(x23), BURROW_ANY_OF(x24),      \
                      BURROW_ANY_OF(x25), BURROW_ANY_OF(x26), BURROW_ANY_OF(x27),      \
                      BURROW_ANY_OF(x28))
#define BURROW__FMT_A29(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,    \
                        x27, x28, x29)                                                 \
    BURROW__FMT_SLICE(29, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22), BURROW_ANY_OF(x23), BURROW_ANY_OF(x24),      \
                      BURROW_ANY_OF(x25), BURROW_ANY_OF(x26), BURROW_ANY_OF(x27),      \
                      BURROW_ANY_OF(x28), BURROW_ANY_OF(x29))
#define BURROW__FMT_A30(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,    \
                        x27, x28, x29, x30)                                            \
    BURROW__FMT_SLICE(30, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22), BURROW_ANY_OF(x23), BURROW_ANY_OF(x24),      \
                      BURROW_ANY_OF(x25), BURROW_ANY_OF(x26), BURROW_ANY_OF(x27),      \
                      BURROW_ANY_OF(x28), BURROW_ANY_OF(x29), BURROW_ANY_OF(x30))
#define BURROW__FMT_A31(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,    \
                        x27, x28, x29, x30, x31)                                       \
    BURROW__FMT_SLICE(31, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22), BURROW_ANY_OF(x23), BURROW_ANY_OF(x24),      \
                      BURROW_ANY_OF(x25), BURROW_ANY_OF(x26), BURROW_ANY_OF(x27),      \
                      BURROW_ANY_OF(x28), BURROW_ANY_OF(x29), BURROW_ANY_OF(x30),      \
                      BURROW_ANY_OF(x31))
#define BURROW__FMT_A32(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,    \
                        x27, x28, x29, x30, x31, x32)                                  \
    BURROW__FMT_SLICE(32, BURROW_ANY_OF(x1), BURROW_ANY_OF(x2), BURROW_ANY_OF(x3),     \
                      BURROW_ANY_OF(x4), BURROW_ANY_OF(x5), BURROW_ANY_OF(x6),         \
                      BURROW_ANY_OF(x7), BURROW_ANY_OF(x8), BURROW_ANY_OF(x9),         \
                      BURROW_ANY_OF(x10), BURROW_ANY_OF(x11), BURROW_ANY_OF(x12),      \
                      BURROW_ANY_OF(x13), BURROW_ANY_OF(x14), BURROW_ANY_OF(x15),      \
                      BURROW_ANY_OF(x16), BURROW_ANY_OF(x17), BURROW_ANY_OF(x18),      \
                      BURROW_ANY_OF(x19), BURROW_ANY_OF(x20), BURROW_ANY_OF(x21),      \
                      BURROW_ANY_OF(x22), BURROW_ANY_OF(x23), BURROW_ANY_OF(x24),      \
                      BURROW_ANY_OF(x25), BURROW_ANY_OF(x26), BURROW_ANY_OF(x27),      \
                      BURROW_ANY_OF(x28), BURROW_ANY_OF(x29), BURROW_ANY_OF(x30),      \
                      BURROW_ANY_OF(x31), BURROW_ANY_OF(x32))

/* A format and then its operands, as the two arguments a function takes. */
#define BURROW__FMT_F1(f) BURROW__FMT_STR(f), slice_nil(TYPE_ANY)
#define BURROW__FMT_F2(f, x1) BURROW__FMT_STR(f), BURROW__FMT_A1(x1)
#define BURROW__FMT_F3(f, x1, x2) BURROW__FMT_STR(f), BURROW__FMT_A2(x1, x2)
#define BURROW__FMT_F4(f, x1, x2, x3) BURROW__FMT_STR(f), BURROW__FMT_A3(x1, x2, x3)
#define BURROW__FMT_F5(f, x1, x2, x3, x4)                                              \
    BURROW__FMT_STR(f), BURROW__FMT_A4(x1, x2, x3, x4)
#define BURROW__FMT_F6(f, x1, x2, x3, x4, x5)                                          \
    BURROW__FMT_STR(f), BURROW__FMT_A5(x1, x2, x3, x4, x5)
#define BURROW__FMT_F7(f, x1, x2, x3, x4, x5, x6)                                      \
    BURROW__FMT_STR(f), BURROW__FMT_A6(x1, x2, x3, x4, x5, x6)
#define BURROW__FMT_F8(f, x1, x2, x3, x4, x5, x6, x7)                                  \
    BURROW__FMT_STR(f), BURROW__FMT_A7(x1, x2, x3, x4, x5, x6, x7)
#define BURROW__FMT_F9(f, x1, x2, x3, x4, x5, x6, x7, x8)                              \
    BURROW__FMT_STR(f), BURROW__FMT_A8(x1, x2, x3, x4, x5, x6, x7, x8)
#define BURROW__FMT_F10(f, x1, x2, x3, x4, x5, x6, x7, x8, x9)                         \
    BURROW__FMT_STR(f), BURROW__FMT_A9(x1, x2, x3, x4, x5, x6, x7, x8, x9)
#define BURROW__FMT_F11(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10)                    \
    BURROW__FMT_STR(f), BURROW__FMT_A10(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10)
#define BURROW__FMT_F12(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11)               \
    BURROW__FMT_STR(f), BURROW__FMT_A11(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11)
#define BURROW__FMT_F13(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12)          \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A12(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12)
#define BURROW__FMT_F14(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13)     \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A13(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13)
#define BURROW__FMT_F15(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14)                                                           \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A14(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14)
#define BURROW__FMT_F16(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15)                                                      \
    BURROW__FMT_STR(f), BURROW__FMT_A15(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11,  \
                                        x12, x13, x14, x15)
#define BURROW__FMT_F17(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16)                                                 \
    BURROW__FMT_STR(f), BURROW__FMT_A16(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11,  \
                                        x12, x13, x14, x15, x16)
#define BURROW__FMT_F18(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17)                                            \
    BURROW__FMT_STR(f), BURROW__FMT_A17(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11,  \
                                        x12, x13, x14, x15, x16, x17)
#define BURROW__FMT_F19(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18)                                       \
    BURROW__FMT_STR(f), BURROW__FMT_A18(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11,  \
                                        x12, x13, x14, x15, x16, x17, x18)
#define BURROW__FMT_F20(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19)                                  \
    BURROW__FMT_STR(f), BURROW__FMT_A19(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11,  \
                                        x12, x13, x14, x15, x16, x17, x18, x19)
#define BURROW__FMT_F21(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20)                             \
    BURROW__FMT_STR(f), BURROW__FMT_A20(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11,  \
                                        x12, x13, x14, x15, x16, x17, x18, x19, x20)
#define BURROW__FMT_F22(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21)                        \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A21(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21)
#define BURROW__FMT_F23(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22)                   \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A22(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22)
#define BURROW__FMT_F24(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23)              \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A23(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23)
#define BURROW__FMT_F25(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24)         \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A24(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24)
#define BURROW__FMT_F26(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25)    \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A25(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25)
#define BURROW__FMT_F27(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26)                                                           \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A26(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26)
#define BURROW__FMT_F28(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27)                                                      \
    BURROW__FMT_STR(f), BURROW__FMT_A27(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11,  \
                                        x12, x13, x14, x15, x16, x17, x18, x19, x20,   \
                                        x21, x22, x23, x24, x25, x26, x27)
#define BURROW__FMT_F29(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28)                                                 \
    BURROW__FMT_STR(f), BURROW__FMT_A28(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11,  \
                                        x12, x13, x14, x15, x16, x17, x18, x19, x20,   \
                                        x21, x22, x23, x24, x25, x26, x27, x28)
#define BURROW__FMT_F30(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29)                                            \
    BURROW__FMT_STR(f), BURROW__FMT_A29(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11,  \
                                        x12, x13, x14, x15, x16, x17, x18, x19, x20,   \
                                        x21, x22, x23, x24, x25, x26, x27, x28, x29)
#define BURROW__FMT_F31(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30)                                       \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A30(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,    \
                        x27, x28, x29, x30)
#define BURROW__FMT_F32(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30, x31)                                  \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A31(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,    \
                        x27, x28, x29, x30, x31)
#define BURROW__FMT_F33(f, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,     \
                        x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25,    \
                        x26, x27, x28, x29, x30, x31, x32)                             \
    BURROW__FMT_STR(f),                                                                \
        BURROW__FMT_A32(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14,   \
                        x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,    \
                        x27, x28, x29, x30, x31, x32)

#define BURROW__FMT_ARGS(...)                                                          \
    BURROW__FMT_CAT(BURROW__FMT_A, BURROW__FMT_NARGS(__VA_ARGS__))(__VA_ARGS__)
#define BURROW__FMT_FARGS(...)                                                         \
    BURROW__FMT_CAT(BURROW__FMT_F, BURROW__FMT_NARGS(__VA_ARGS__))(__VA_ARGS__)

#define fmt_printf_v(...) fmt_printf(BURROW__FMT_FARGS(__VA_ARGS__), NULL)
#define fmt_fprintf_v(w, ...) fmt_fprintf((w), BURROW__FMT_FARGS(__VA_ARGS__), NULL)
#define fmt_sprintf_v(a, ...) fmt_sprintf((a), BURROW__FMT_FARGS(__VA_ARGS__))
#define fmt_appendf_v(a, b, ...) fmt_appendf((a), (b), BURROW__FMT_FARGS(__VA_ARGS__))
#define fmt_errorf_v(...) fmt_errorf(BURROW__FMT_FARGS(__VA_ARGS__))

#define fmt_print_v(...) fmt_print(BURROW__FMT_ARGS(__VA_ARGS__), NULL)
#define fmt_fprint_v(w, ...) fmt_fprint((w), BURROW__FMT_ARGS(__VA_ARGS__), NULL)
#define fmt_sprint_v(a, ...) fmt_sprint((a), BURROW__FMT_ARGS(__VA_ARGS__))
#define fmt_append_v(a, b, ...) fmt_append((a), (b), BURROW__FMT_ARGS(__VA_ARGS__))

#define fmt_println_v(...) fmt_println(BURROW__FMT_ARGS(__VA_ARGS__), NULL)
#define fmt_fprintln_v(w, ...) fmt_fprintln((w), BURROW__FMT_ARGS(__VA_ARGS__), NULL)
#define fmt_sprintln_v(a, ...) fmt_sprintln((a), BURROW__FMT_ARGS(__VA_ARGS__))
#define fmt_appendln_v(a, b, ...) fmt_appendln((a), (b), BURROW__FMT_ARGS(__VA_ARGS__))

#ifdef __cplusplus
}
#endif

#endif /* BURROW_FMT_H */
