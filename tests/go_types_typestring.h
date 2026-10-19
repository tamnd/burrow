/* independentTestTypes from Go's src/go/types/typestring_test.go, which
 * typestring_test.go and eval_test.go both use. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef GO_TYPES_TYPESTRING_H
#define GO_TYPES_TYPESTRING_H

#include "burrow/core.h"

typedef struct {
    Str src, str;
} TestEntry;

/* types that don't depend on any other type declarations */
static const TestEntry independent_test_types[] = {
    /* basic types */
    {BURROW_S_INIT("int"), BURROW_S_INIT("int")},
    {BURROW_S_INIT("float32"), BURROW_S_INIT("float32")},
    {BURROW_S_INIT("string"), BURROW_S_INIT("string")},

    /* arrays */
    {BURROW_S_INIT("[10]int"), BURROW_S_INIT("[10]int")},

    /* slices */
    {BURROW_S_INIT("[]int"), BURROW_S_INIT("[]int")},
    {BURROW_S_INIT("[][]int"), BURROW_S_INIT("[][]int")},

    /* structs */
    {BURROW_S_INIT("struct{}"), BURROW_S_INIT("struct{}")},
    {BURROW_S_INIT("struct{x int}"), BURROW_S_INIT("struct{x int}")},
    {BURROW_S_INIT("struct {\n\t\tx, y int\n\t\tz float32 \"foo\"\n\t}"),
     BURROW_S_INIT("struct{x int; y int; z float32 \"foo\"}")},
    {BURROW_S_INIT("struct {\n\t\tstring\n\t\telems []complex128\n\t}"),
     BURROW_S_INIT("struct{string; elems []complex128}")},

    /* pointers */
    {BURROW_S_INIT("*int"), BURROW_S_INIT("*int")},
    {BURROW_S_INIT("***struct{}"), BURROW_S_INIT("***struct{}")},
    {BURROW_S_INIT("*struct{a int; b float32}"),
     BURROW_S_INIT("*struct{a int; b float32}")},

    /* functions */
    {BURROW_S_INIT("func()"), BURROW_S_INIT("func()")},
    {BURROW_S_INIT("func(x int)"), BURROW_S_INIT("func(x int)")},
    {BURROW_S_INIT("func(x, y int)"), BURROW_S_INIT("func(x int, y int)")},
    {BURROW_S_INIT("func(x, y int, z string)"),
     BURROW_S_INIT("func(x int, y int, z string)")},
    {BURROW_S_INIT("func(int)"), BURROW_S_INIT("func(int)")},
    {BURROW_S_INIT("func(int, string, byte)"),
     BURROW_S_INIT("func(int, string, byte)")},

    {BURROW_S_INIT("func() int"), BURROW_S_INIT("func() int")},
    {BURROW_S_INIT("func() (string)"), BURROW_S_INIT("func() string")},
    {BURROW_S_INIT("func() (u int)"), BURROW_S_INIT("func() (u int)")},
    {BURROW_S_INIT("func() (u, v int, w string)"),
     BURROW_S_INIT("func() (u int, v int, w string)")},

    {BURROW_S_INIT("func(int) string"), BURROW_S_INIT("func(int) string")},
    {BURROW_S_INIT("func(x int) string"), BURROW_S_INIT("func(x int) string")},
    {BURROW_S_INIT("func(x int) (u string)"), BURROW_S_INIT("func(x int) (u string)")},
    {BURROW_S_INIT("func(x, y int) (u string)"),
     BURROW_S_INIT("func(x int, y int) (u string)")},

    {BURROW_S_INIT("func(...int) string"), BURROW_S_INIT("func(...int) string")},
    {BURROW_S_INIT("func(x ...int) string"), BURROW_S_INIT("func(x ...int) string")},
    {BURROW_S_INIT("func(x ...int) (u string)"),
     BURROW_S_INIT("func(x ...int) (u string)")},
    {BURROW_S_INIT("func(x int, y ...int) (u string)"),
     BURROW_S_INIT("func(x int, y ...int) (u string)")},

    /* interfaces */
    {BURROW_S_INIT("interface{}"), BURROW_S_INIT("interface{}")},
    {BURROW_S_INIT("interface{m()}"), BURROW_S_INIT("interface{m()}")},
    {BURROW_S_INIT("interface{String() string; m(int) float32}"),
     BURROW_S_INIT("interface{String() string; m(int) float32}")},
    {BURROW_S_INIT("interface{int | float32 | complex128}"),
     BURROW_S_INIT("interface{int | float32 | complex128}")},
    {BURROW_S_INIT("interface{int | ~float32 | ~complex128}"),
     BURROW_S_INIT("interface{int | ~float32 | ~complex128}")},
    {BURROW_S_INIT("interface{comparable}"), BURROW_S_INIT("interface{comparable}")},
    /* TODO(gri) adjust test for EvalCompositeTest
     * {"error", "interface{Error() string}"},
     * {"any", "interface{}"},
     * {"comparable", "interface{comparable}"}, */

    /* maps */
    {BURROW_S_INIT("map[string]int"), BURROW_S_INIT("map[string]int")},
    {BURROW_S_INIT("map[struct{x, y int}][]byte"),
     BURROW_S_INIT("map[struct{x int; y int}][]byte")},

    /* channels */
    {BURROW_S_INIT("chan<- chan int"), BURROW_S_INIT("chan<- chan int")},
    {BURROW_S_INIT("chan<- <-chan int"), BURROW_S_INIT("chan<- <-chan int")},
    {BURROW_S_INIT("<-chan <-chan int"), BURROW_S_INIT("<-chan <-chan int")},
    {BURROW_S_INIT("chan (<-chan int)"), BURROW_S_INIT("chan (<-chan int)")},
    {BURROW_S_INIT("chan<- func()"), BURROW_S_INIT("chan<- func()")},
    {BURROW_S_INIT("<-chan []func() int"), BURROW_S_INIT("<-chan []func() int")},
};

#endif
