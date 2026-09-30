/* flag, command line flag parsing.
 *
 * Go's flag. Define the flags, parse, then read them:
 *
 *     Int *n = flag_int(BURROW_S("n"), 1234, BURROW_S("help message for flag n"));
 *     bool verbose;
 *     flag_bool_var(&verbose, BURROW_S("v"), false, BURROW_S("say more"));
 *     flag_parse();
 *     for (Int i = 0; i < flag_n_arg(); i++)
 *         handle(flag_arg(i));
 *
 * The syntax is Go's. -flag, --flag, -flag=x, and -flag x for anything that is
 * not a boolean. Parsing stops at the first argument that is not a flag, and
 * after "--". Integer flags take 1234, 0664 and 0x1234 and may be negative,
 * boolean flags take what strconv_parse_bool takes, and duration flags take
 * what time_parse_duration takes.
 *
 * The functions without flag_flag_set_ in the name work on flag_command_line,
 * the set for the program's own arguments, which flag_parse fills from the
 * command line. A FlagFlagSet of your own does the same for any list of
 * arguments, which is how a subcommand gets flags of its own.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package flag */

/* WHAT OWNS WHAT
 *
 * A set owns its flags, and the storage behind the pointers that flag_int and
 * the rest return, and all of it goes when the set is freed. The command line
 * set is never freed. A set copies each flag's name and usage, so a caller can
 * build them in a temporary buffer.
 *
 * A string flag's value is not copied. It is the Str it was set from: an
 * element of the arguments given to flag_flag_set_parse, the value given to
 * flag_flag_set_set, or the default. Parse borrows the arguments too, since
 * flag_flag_set_args is a piece of them, so they have to outlive the set's
 * use. The command line's own arguments live until the program ends.
 *
 * ERRORS
 *
 * A set's FlagErrorHandling says what a bad command line does. The default for
 * a new set is to return the error. flag_command_line prints a message and the
 * usage and ends the program with status 2, or 0 for -help, which is Go's
 * behaviour and what a command line tool wants. Stdio is flushed first.
 *
 * Defining a flag badly is a mistake in the program rather than in its input,
 * and panics as in Go: a name that starts with - or has = in it, a name
 * defined twice, and a name that flag_flag_set_set was given before the flag
 * existed.
 *
 * None of this is safe to use from two threads at once, as in Go. Define and
 * parse on one thread before starting others, and read after. */

#ifndef BURROW_FLAG_H
#define BURROW_FLAG_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/own.h"
#include "burrow/time.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* flag.ErrHelp, what parsing returns for -help or -h when no flag of that
 * name was defined. */
extern const Error flag_err_help;

/* flag.ErrorHandling, what a set does when parsing fails. */
typedef enum FlagErrorHandling {
    FLAG_CONTINUE_ON_ERROR, /* return the error */
    FLAG_EXIT_ON_ERROR,     /* end the program with status 2, or 0 for -help */
    FLAG_PANIC_ON_ERROR,    /* panic with the error */
} FlagErrorHandling;

/* ------------------------------------------------------------------ Value */

/* flag.Value, the interface every flag is, which is how a flag of your own
 * type is made: fill in a FlagValueVT and pass it to flag_flag_set_var.
 *
 * string is the value as text. a is scratch memory that the caller lets go of
 * all at once, so the result can live in a, or be a literal, or be anything
 * that outlives the call, and the caller copies what it keeps. It is called on
 * the default to get the default's text, and by flag_flag_set_print_defaults on a zeroed value of
 * self_type to find out whether the default is worth printing, so it has to
 * cope with a zeroed self. A panic in it there is caught and reported.
 *
 * set parses s and stores it. A failure's Error is what the user sees after
 * "invalid value", and it goes in the goroutine's error arena like every other.
 *
 * The last two are optional, and NULL on a value without them. get is Go's
 * Getter, and gives the value back as an Any pointing at it. is_bool_flag is
 * the method Go's parser looks for: a flag whose is_bool_flag says true is set
 * to "true" by -name alone, and has to be given -name=false to be turned off.
 *
 * self_type may be NULL. The one thing it is needed for is the zero value, and
 * without it the default is left out of the usage when its text is empty and
 * printed otherwise. */
typedef struct FlagValueVT {
    const Type *self_type;
    Str (*string)(void *self, Alloc *a);
    Error (*set)(void *self, Str s);
    Any (*get)(void *self);
    bool (*is_bool_flag)(void *self);
} FlagValueVT;

typedef struct FlagValue {
    const FlagValueVT *vt;
    void *data;
} FlagValue;

/* flag.Getter: a Value whose get is set. Every flag the package makes is
 * one, apart from the ones flag_func and flag_bool_func make. */
typedef FlagValueVT FlagGetterVT;
typedef FlagValue FlagGetter;

/* A flag.Func or flag.BoolFunc function, Go's func(string) error. */
BURROW_FUNC(FlagFunc, Error, Str s);

/* ------------------------------------------------------------------- Flag */

/* flag.Flag, one defined flag. The set owns it, and it lives as long as the
 * set does. */
typedef struct Flag {
    Str name;  /* as it appears on the command line */
    Str usage; /* the help message */
    FlagValue value;
    Str def_value; /* the default as text, for the usage message */
} Flag;

/* What flag_visit and flag_visit_all call, Go's func(*Flag). */
BURROW_FUNC(FlagVisitFunc, void, Flag *f);

/* flag.UnquoteUsage. The name in back quotes in f's usage and the usage with
 * the quotes taken out: "a `name` to show" gives "name" and "a name to show".
 * Without back quotes the name is a guess from the flag's type, "int" for an
 * integer, "string" and so on, "value" for a type of your own, and nothing for
 * a boolean.
 *
 * The name borrows from f's usage, or is a literal. The usage goes into a,
 * even when there was nothing to take out. */
BURROW_BORROWS(ret, f) Str flag_unquote_usage(Alloc *a, const Flag *f, Str *usage);

/* ---------------------------------------------------------------- FlagSet */

/* flag.FlagSet, a set of flags and the arguments left after them.
 *
 * usage is Go's Usage field, what the set calls when parsing fails or when it
 * sees -help. A zeroed one prints the flags, as Go's default does, so a set
 * that flag_flag_set_init set up as a zeroed struct works as one from
 * flag_new_flag_set does. Everything below usage is the set's own. */
typedef struct FlagFlagSet {
    Func usage;

    Alloc *a;
    Str name;
    bool parsed;
    Flag **formal; /* every flag, sorted by name */
    Int nformal;
    Int capformal;
    Flag **actual; /* the ones that were set, sorted by name */
    Int nactual;
    Int capactual;
    Slice args; /* what is left after the flags, borrowed */
    FlagErrorHandling error_handling;
    IoWriter output; /* a zeroed one is standard error */
    Str *undef;      /* names set before they were defined, and where */
    Int nundef;
    Int capundef;
} FlagFlagSet;

/* flag.NewFlagSet. The set, and everything it makes, comes from a. NULL when
 * a refuses. */
BURROW_OWNS(ret) FlagFlagSet *flag_new_flag_set(Alloc *a, Str name,
                                                FlagErrorHandling error_handling);

/* Frees a set from flag_new_flag_set, its flags and the storage behind them. */
void flag_flag_set_free(FlagFlagSet *f);

/* FlagSet.Init, for a set in memory of your own, such as a zeroed struct. It
 * uses the heap until something has given it an allocator. A set set up this
 * way is let go with flag_flag_set_destroy, which frees what it holds and not
 * the struct. */
void flag_flag_set_init(FlagFlagSet *f, Str name, FlagErrorHandling error_handling);
void flag_flag_set_destroy(FlagFlagSet *f);

/* FlagSet.Name and FlagSet.ErrorHandling. */
BURROW_BORROWS(ret, f) Str flag_flag_set_name(FlagFlagSet *f);
FlagErrorHandling flag_flag_set_error_handling(FlagFlagSet *f);

/* FlagSet.Output and FlagSet.SetOutput. Messages and usage go here, and a
 * zeroed writer means standard error, which is what output returns then. */
IoWriter flag_flag_set_output(FlagFlagSet *f);
void flag_flag_set_set_output(FlagFlagSet *f, IoWriter output);

/* The definitions. Each _var form stores through p, which has to outlive the
 * set. The others make the storage in the set and return it. value is the
 * default, and is written to the storage straight away. */
void flag_flag_set_bool_var(FlagFlagSet *f, bool *p, Str name, bool value, Str usage);
void flag_flag_set_int_var(FlagFlagSet *f, Int *p, Str name, Int value, Str usage);
void flag_flag_set_int64_var(FlagFlagSet *f, int64_t *p, Str name, int64_t value,
                             Str usage);
void flag_flag_set_uint_var(FlagFlagSet *f, Uint *p, Str name, Uint value, Str usage);
void flag_flag_set_uint64_var(FlagFlagSet *f, uint64_t *p, Str name, uint64_t value,
                              Str usage);
void flag_flag_set_string_var(FlagFlagSet *f, Str *p, Str name, Str value, Str usage);
void flag_flag_set_float64_var(FlagFlagSet *f, double *p, Str name, double value,
                               Str usage);
void flag_flag_set_duration_var(FlagFlagSet *f, Duration *p, Str name, Duration value,
                                Str usage);

BURROW_BORROWS(ret, f) bool *flag_flag_set_bool(FlagFlagSet *f, Str name, bool value,
                                                Str usage);
BURROW_BORROWS(ret, f) Int *flag_flag_set_int(FlagFlagSet *f, Str name, Int value,
                                              Str usage);
BURROW_BORROWS(ret, f) int64_t *flag_flag_set_int64(FlagFlagSet *f, Str name,
                                                    int64_t value, Str usage);
BURROW_BORROWS(ret, f) Uint *flag_flag_set_uint(FlagFlagSet *f, Str name, Uint value,
                                                Str usage);
BURROW_BORROWS(ret, f) uint64_t *flag_flag_set_uint64(FlagFlagSet *f, Str name,
                                                      uint64_t value, Str usage);
BURROW_BORROWS(ret, f) Str *flag_flag_set_string(FlagFlagSet *f, Str name, Str value,
                                                 Str usage);
BURROW_BORROWS(ret, f) double *flag_flag_set_float64(FlagFlagSet *f, Str name,
                                                     double value, Str usage);
BURROW_BORROWS(ret, f) Duration *flag_flag_set_duration(FlagFlagSet *f, Str name,
                                                        Duration value, Str usage);

/* FlagSet.TextVar. p is the variable, an Any of its type pointing at it, and
 * the type has an UnmarshalText method, which is what set calls, and a
 * MarshalText method, which is what string calls. value is the default, an Any
 * of the same type, and is copied into p. Both are found through the type's
 * method set, as encoding_unmarshal_text finds them. */
void flag_flag_set_text_var(FlagFlagSet *f, Any p, Str name, Any value, Str usage);

/* FlagSet.Func and FlagSet.BoolFunc. fn is called with each value given. A
 * BoolFunc flag needs no value, and gets "true" when it has none. */
void flag_flag_set_func(FlagFlagSet *f, Str name, Str usage, FlagFunc fn);
void flag_flag_set_bool_func(FlagFlagSet *f, Str name, Str usage, FlagFunc fn);

/* FlagSet.Var, a flag of any type. The default is whatever value holds now. */
void flag_flag_set_var(FlagFlagSet *f, FlagValue value, Str name, Str usage);

/* FlagSet.Parse. Parses arguments, a Slice of Str without the program name,
 * which have to be defined flags. Borrows them: see the top of this header. */
BURROW_BORROWS(ret) Error flag_flag_set_parse(FlagFlagSet *f, Slice arguments);

/* FlagSet.Parsed, whether parse has been called. */
bool flag_flag_set_parsed(FlagFlagSet *f);

/* FlagSet.Arg, FlagSet.Args and FlagSet.NArg: the arguments after the flags.
 * arg is empty for an i out of range. args is a Slice of Str borrowed from
 * what parse was given. */
BURROW_BORROWS(ret, f) Str flag_flag_set_arg(FlagFlagSet *f, Int i);
BURROW_BORROWS(ret, f) Slice flag_flag_set_args(FlagFlagSet *f);
Int flag_flag_set_n_arg(FlagFlagSet *f);

/* FlagSet.NFlag, how many flags have been set. */
Int flag_flag_set_n_flag(FlagFlagSet *f);

/* FlagSet.Lookup, the flag called name, or NULL. */
BURROW_BORROWS(ret, f) Flag *flag_flag_set_lookup(FlagFlagSet *f, Str name);

/* FlagSet.Set, which sets a flag the way the command line would.
 *
 * Go remembers where a name that was not defined yet was set from, so that
 * defining it afterwards can say where, and this does the same with the
 * caller's file and line. The macro is what passes them, so taking the
 * function's address gives a version that reports "?:0". */
BURROW_BORROWS(ret) Error flag_flag_set_set(FlagFlagSet *f, Str name, Str value);
BURROW_BORROWS(ret) Error burrow__flag_set_at(FlagFlagSet *f, Str name, Str value,
                                              const char *file, Int line);
#define flag_flag_set_set(f, name, value)                                              \
    burrow__flag_set_at((f), (name), (value), __FILE__, __LINE__)

/* FlagSet.Visit and FlagSet.VisitAll: fn for each flag that has been set, or
 * for every flag, in name order. */
void flag_flag_set_visit(FlagFlagSet *f, FlagVisitFunc fn);
void flag_flag_set_visit_all(FlagFlagSet *f, FlagVisitFunc fn);

/* FlagSet.PrintDefaults, the flags and their usage to the set's output, as
 * Go lays them out. */
void flag_flag_set_print_defaults(FlagFlagSet *f);

/* ----------------------------------------------------------- CommandLine */

/* flag.CommandLine, the set for the program's own arguments. Its name is the
 * program name, it exits on error, and its usage calls flag_usage. A program
 * can point this at a set of its own, as Go code can assign CommandLine. */
extern FlagFlagSet *flag_command_line;

/* flag.Usage, what the command line set calls to explain itself. The default
 * prints "Usage of" the program name and the flags. Assign a Func to replace
 * it. */
extern Func flag_usage;

/* flag.Parse, which parses the program's arguments, from the one after its
 * name. Call it after the flags are defined and before they are read. */
void flag_parse(void);

/* The rest are the FlagSet functions on flag_command_line. */
bool flag_parsed(void);
BURROW_STATIC(ret) Str flag_arg(Int i);
BURROW_STATIC(ret) Slice flag_args(void);
Int flag_n_arg(void);
Int flag_n_flag(void);
BURROW_STATIC(ret) Flag *flag_lookup(Str name);
BURROW_BORROWS(ret) Error flag_set(Str name, Str value);
#define flag_set(name, value)                                                          \
    burrow__flag_set_at(flag_command_line, (name), (value), __FILE__, __LINE__)
void flag_visit(FlagVisitFunc fn);
void flag_visit_all(FlagVisitFunc fn);
void flag_print_defaults(void);

void flag_bool_var(bool *p, Str name, bool value, Str usage);
void flag_int_var(Int *p, Str name, Int value, Str usage);
void flag_int64_var(int64_t *p, Str name, int64_t value, Str usage);
void flag_uint_var(Uint *p, Str name, Uint value, Str usage);
void flag_uint64_var(uint64_t *p, Str name, uint64_t value, Str usage);
void flag_string_var(Str *p, Str name, Str value, Str usage);
void flag_float64_var(double *p, Str name, double value, Str usage);
void flag_duration_var(Duration *p, Str name, Duration value, Str usage);

BURROW_STATIC(ret) bool *flag_bool(Str name, bool value, Str usage);
BURROW_STATIC(ret) Int *flag_int(Str name, Int value, Str usage);
BURROW_STATIC(ret) int64_t *flag_int64(Str name, int64_t value, Str usage);
BURROW_STATIC(ret) Uint *flag_uint(Str name, Uint value, Str usage);
BURROW_STATIC(ret) uint64_t *flag_uint64(Str name, uint64_t value, Str usage);
BURROW_STATIC(ret) Str *flag_string(Str name, Str value, Str usage);
BURROW_STATIC(ret) double *flag_float64(Str name, double value, Str usage);
BURROW_STATIC(ret) Duration *flag_duration(Str name, Duration value, Str usage);

void flag_text_var(Any p, Str name, Any value, Str usage);
void flag_func(Str name, Str usage, FlagFunc fn);
void flag_bool_func(Str name, Str usage, FlagFunc fn);
void flag_var(FlagValue value, Str name, Str usage);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_FLAG_H */
