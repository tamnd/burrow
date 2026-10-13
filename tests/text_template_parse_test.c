/* Derived from Go's src/text/template/parse/lex_test.go and parse_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/text/template_parse_internal.h"
#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/text/template/parse.h"

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* ---------------------------------------------------------------- lexer */

typedef struct LexTest {
    Str name;
    Str input;
    const burrow__Item *items;
    Int n;
} LexTest;

static const burrow__Item lex_tests_0[] = {
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_1[] = {
    {BURROW__ITEM_TEXT, 0, S_(" \t\n"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_2[] = {
    {BURROW__ITEM_TEXT, 0, S_("now is the time"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_3[] = {
    {BURROW__ITEM_TEXT, 0, S_("hello-"), 0},
    {BURROW__ITEM_COMMENT, 0, S_("/* this is a comment */"), 0},
    {BURROW__ITEM_TEXT, 0, S_("-world"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_4[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_CHAR, 0, S_(","), 0},
    {BURROW__ITEM_CHAR, 0, S_("@"), 0},
    {BURROW__ITEM_CHAR, 0, S_("%"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_5[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_LEFT_PAREN, 0, S_("("), 0},
    {BURROW__ITEM_LEFT_PAREN, 0, S_("("), 0},
    {BURROW__ITEM_NUMBER, 0, S_("3"), 0},
    {BURROW__ITEM_RIGHT_PAREN, 0, S_(")"), 0},
    {BURROW__ITEM_RIGHT_PAREN, 0, S_(")"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_6[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_7[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_IDENTIFIER, 0, S_("for"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_8[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_BLOCK, 0, S_("block"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_STRING, 0, S_("\"foo\""), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_DOT, 0, S_("."), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_9[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_STRING, 0, S_("\"abc \\n\\t\\\" \""), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_10[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_RAW_STRING, 0, S_("`abc\\n\\t\\\" `"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_11[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_RAW_STRING, 0, S_("`now is{{\n}}the time`"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_12[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_NUMBER, 0, S_("1"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("02"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("0x14"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("0X14"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("-7.2i"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("1e3"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("1E3"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("+1.2e-4"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("4.2i"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_COMPLEX, 0, S_("1+2i"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("1_2"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("0x1.e_fp4"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("0X1.E_FP4"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_13[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_CHAR_CONSTANT, 0, S_("'a'"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_CHAR_CONSTANT, 0, S_("'\\n'"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_CHAR_CONSTANT, 0, S_("'\\''"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_CHAR_CONSTANT, 0, S_("'\\\\'"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_CHAR_CONSTANT, 0, S_("'\\u00FF'"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_CHAR_CONSTANT, 0, S_("'\\xFF'"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_CHAR_CONSTANT, 0, S_("'\346\234\254'"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_14[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},  {BURROW__ITEM_BOOL, 0, S_("true"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_BOOL, 0, S_("false"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0}, {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_15[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_DOT, 0, S_("."), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_16[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_NIL, 0, S_("nil"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_17[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},  {BURROW__ITEM_FIELD, 0, S_(".x"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_DOT, 0, S_("."), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_NUMBER, 0, S_(".2"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_FIELD, 0, S_(".x"), 0},
    {BURROW__ITEM_FIELD, 0, S_(".y"), 0},       {BURROW__ITEM_FIELD, 0, S_(".z"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0}, {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_18[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},  {BURROW__ITEM_RANGE, 0, S_("range"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_IF, 0, S_("if"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_ELSE, 0, S_("else"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_END, 0, S_("end"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_WITH, 0, S_("with"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0}, {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_19[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_VARIABLE, 0, S_("$c"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_DECLARE, 0, S_(":="), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_IDENTIFIER, 0, S_("printf"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_VARIABLE, 0, S_("$"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_VARIABLE, 0, S_("$hello"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_VARIABLE, 0, S_("$23"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_VARIABLE, 0, S_("$"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_VARIABLE, 0, S_("$var"), 0},
    {BURROW__ITEM_FIELD, 0, S_(".Field"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_FIELD, 0, S_(".Method"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_20[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},  {BURROW__ITEM_VARIABLE, 0, S_("$x"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_NUMBER, 0, S_("23"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0}, {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_21[] = {
    {BURROW__ITEM_TEXT, 0, S_("intro "), 0},
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_IDENTIFIER, 0, S_("echo"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_IDENTIFIER, 0, S_("hi"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("1.2"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_PIPE, 0, S_("|"), 0},
    {BURROW__ITEM_IDENTIFIER, 0, S_("noargs"), 0},
    {BURROW__ITEM_PIPE, 0, S_("|"), 0},
    {BURROW__ITEM_IDENTIFIER, 0, S_("args"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_NUMBER, 0, S_("1"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},
    {BURROW__ITEM_STRING, 0, S_("\"hi\""), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_TEXT, 0, S_(" outro"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_22[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},  {BURROW__ITEM_VARIABLE, 0, S_("$v"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_DECLARE, 0, S_(":="), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_NUMBER, 0, S_("3"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0}, {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_23[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},  {BURROW__ITEM_VARIABLE, 0, S_("$v"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_CHAR, 0, S_(","), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_VARIABLE, 0, S_("$w"), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_DECLARE, 0, S_(":="), 0},
    {BURROW__ITEM_SPACE, 0, S_(" "), 0},        {BURROW__ITEM_NUMBER, 0, S_("3"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0}, {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_24[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_LEFT_PAREN, 0, S_("("), 0},
    {BURROW__ITEM_FIELD, 0, S_(".X"), 0},
    {BURROW__ITEM_RIGHT_PAREN, 0, S_(")"), 0},
    {BURROW__ITEM_FIELD, 0, S_(".Y"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_25[] = {
    {BURROW__ITEM_TEXT, 0, S_("hello-"), 0}, {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_NUMBER, 0, S_("3"), 0},    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0},
    {BURROW__ITEM_TEXT, 0, S_("-world"), 0}, {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_26[] = {
    {BURROW__ITEM_TEXT, 0, S_("hello-"), 0},
    {BURROW__ITEM_COMMENT, 0, S_("/* hello */"), 0},
    {BURROW__ITEM_TEXT, 0, S_("-world"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_27[] = {
    {BURROW__ITEM_TEXT, 0, S_("#"), 0},
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_ERROR, 0, S_("unrecognized character in action: U+0001"), 0},
};
static const burrow__Item lex_tests_28[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_ERROR, 0, S_("unclosed action"), 0},
};
static const burrow__Item lex_tests_29[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_RANGE, 0, S_("range"), 0},
    {BURROW__ITEM_ERROR, 0, S_("unclosed action"), 0},
};
static const burrow__Item lex_tests_30[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_ERROR, 0, S_("unterminated quoted string"), 0},
};
static const burrow__Item lex_tests_31[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_ERROR, 0, S_("unterminated raw quoted string"), 0},
};
static const burrow__Item lex_tests_32[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_ERROR, 0, S_("unterminated character constant"), 0},
};
static const burrow__Item lex_tests_33[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_ERROR, 0, S_("bad number syntax: \"3k\""), 0},
};
static const burrow__Item lex_tests_34[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_LEFT_PAREN, 0, S_("("), 0},
    {BURROW__ITEM_NUMBER, 0, S_("3"), 0},
    {BURROW__ITEM_ERROR, 0, S_("unclosed left paren"), 0},
};
static const burrow__Item lex_tests_35[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},
    {BURROW__ITEM_NUMBER, 0, S_("3"), 0},
    {BURROW__ITEM_ERROR, 0, S_("unexpected right paren"), 0},
};
static const burrow__Item lex_tests_36[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 0},  {BURROW__ITEM_PIPE, 0, S_("|"), 0},
    {BURROW__ITEM_PIPE, 0, S_("|"), 0},         {BURROW__ITEM_PIPE, 0, S_("|"), 0},
    {BURROW__ITEM_PIPE, 0, S_("|"), 0},         {BURROW__ITEM_PIPE, 0, S_("|"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("}}"), 0}, {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_tests_37[] = {
    {BURROW__ITEM_TEXT, 0, S_("hello-"), 0},
    {BURROW__ITEM_ERROR, 0, S_("unclosed comment"), 0},
};
static const burrow__Item lex_tests_38[] = {
    {BURROW__ITEM_TEXT, 0, S_("hello-"), 0},
    {BURROW__ITEM_ERROR, 0, S_("comment ends before closing delimiter"), 0},
};
static const burrow__Item lex_tests_39[] = {
    {BURROW__ITEM_TEXT, 0, S_("hello-{.}}-world"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};

static const LexTest lex_tests[] = {
    {S_("empty"), S_(""), lex_tests_0, NELEM(lex_tests_0)},
    {S_("spaces"), S_(" \t\n"), lex_tests_1, NELEM(lex_tests_1)},
    {S_("text"), S_("now is the time"), lex_tests_2, NELEM(lex_tests_2)},
    {S_("text with comment"), S_("hello-{{/* this is a comment */}}-world"),
     lex_tests_3, NELEM(lex_tests_3)},
    {S_("punctuation"), S_("{{,@% }}"), lex_tests_4, NELEM(lex_tests_4)},
    {S_("parens"), S_("{{((3))}}"), lex_tests_5, NELEM(lex_tests_5)},
    {S_("empty action"), S_("{{}}"), lex_tests_6, NELEM(lex_tests_6)},
    {S_("for"), S_("{{for}}"), lex_tests_7, NELEM(lex_tests_7)},
    {S_("block"), S_("{{block \"foo\" .}}"), lex_tests_8, NELEM(lex_tests_8)},
    {S_("quote"), S_("{{\"abc \\n\\t\\\" \"}}"), lex_tests_9, NELEM(lex_tests_9)},
    {S_("raw quote"), S_("{{`abc\\n\\t\\\" `}}"), lex_tests_10, NELEM(lex_tests_10)},
    {S_("raw quote with newline"), S_("{{`now is{{\n}}the time`}}"), lex_tests_11,
     NELEM(lex_tests_11)},
    {S_("numbers"),
     S_("{{1 02 0x14 0X14 -7.2i 1e3 1E3 +1.2e-4 4.2i 1+2i 1_2 0x1.e_fp4 0X1.E_FP4}}"),
     lex_tests_12, NELEM(lex_tests_12)},
    {S_("characters"),
     S_("{{'a' '\\n' '\\'' '\\\\' '\\u00FF' '\\xFF' '\346\234\254'}}"), lex_tests_13,
     NELEM(lex_tests_13)},
    {S_("bools"), S_("{{true false}}"), lex_tests_14, NELEM(lex_tests_14)},
    {S_("dot"), S_("{{.}}"), lex_tests_15, NELEM(lex_tests_15)},
    {S_("nil"), S_("{{nil}}"), lex_tests_16, NELEM(lex_tests_16)},
    {S_("dots"), S_("{{.x . .2 .x.y.z}}"), lex_tests_17, NELEM(lex_tests_17)},
    {S_("keywords"), S_("{{range if else end with}}"), lex_tests_18,
     NELEM(lex_tests_18)},
    {S_("variables"), S_("{{$c := printf $ $hello $23 $ $var.Field .Method}}"),
     lex_tests_19, NELEM(lex_tests_19)},
    {S_("variable invocation"), S_("{{$x 23}}"), lex_tests_20, NELEM(lex_tests_20)},
    {S_("pipeline"), S_("intro {{echo hi 1.2 |noargs|args 1 \"hi\"}} outro"),
     lex_tests_21, NELEM(lex_tests_21)},
    {S_("declaration"), S_("{{$v := 3}}"), lex_tests_22, NELEM(lex_tests_22)},
    {S_("2 declarations"), S_("{{$v , $w := 3}}"), lex_tests_23, NELEM(lex_tests_23)},
    {S_("field of parenthesized expression"), S_("{{(.X).Y}}"), lex_tests_24,
     NELEM(lex_tests_24)},
    {S_("trimming spaces before and after"), S_("hello- {{- 3 -}} -world"),
     lex_tests_25, NELEM(lex_tests_25)},
    {S_("trimming spaces before and after comment"),
     S_("hello- {{- /* hello */ -}} -world"), lex_tests_26, NELEM(lex_tests_26)},
    {S_("badchar"), S_("#{{\001}}"), lex_tests_27, NELEM(lex_tests_27)},
    {S_("unclosed action"), S_("{{"), lex_tests_28, NELEM(lex_tests_28)},
    {S_("EOF in action"), S_("{{range"), lex_tests_29, NELEM(lex_tests_29)},
    {S_("unclosed quote"), S_("{{\"\n\"}}"), lex_tests_30, NELEM(lex_tests_30)},
    {S_("unclosed raw quote"), S_("{{`xx}}"), lex_tests_31, NELEM(lex_tests_31)},
    {S_("unclosed char constant"), S_("{{'\n}}"), lex_tests_32, NELEM(lex_tests_32)},
    {S_("bad number"), S_("{{3k}}"), lex_tests_33, NELEM(lex_tests_33)},
    {S_("unclosed paren"), S_("{{(3}}"), lex_tests_34, NELEM(lex_tests_34)},
    {S_("extra right paren"), S_("{{3)}}"), lex_tests_35, NELEM(lex_tests_35)},
    {S_("long pipeline deadlock"), S_("{{|||||}}"), lex_tests_36, NELEM(lex_tests_36)},
    {S_("text with bad comment"), S_("hello-{{/*/}}-world"), lex_tests_37,
     NELEM(lex_tests_37)},
    {S_("text with comment close separated from delim"), S_("hello-{{/* */ }}-world"),
     lex_tests_38, NELEM(lex_tests_38)},
    {S_("unmatched right delimiter"), S_("hello-{.}}-world"), lex_tests_39,
     NELEM(lex_tests_39)},
};

static const burrow__Item lex_delim_tests_0[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("$$"), 0},  {BURROW__ITEM_CHAR, 0, S_(","), 0},
    {BURROW__ITEM_CHAR, 0, S_("@"), 0},         {BURROW__ITEM_CHAR, 0, S_("%"), 0},
    {BURROW__ITEM_CHAR, 0, S_("{"), 0},         {BURROW__ITEM_CHAR, 0, S_("{"), 0},
    {BURROW__ITEM_CHAR, 0, S_("}"), 0},         {BURROW__ITEM_CHAR, 0, S_("}"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("@@"), 0}, {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_delim_tests_1[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("$$"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("@@"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_delim_tests_2[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("$$"), 0},
    {BURROW__ITEM_IDENTIFIER, 0, S_("for"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("@@"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_delim_tests_3[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("$$"), 0},
    {BURROW__ITEM_STRING, 0, S_("\"abc \\n\\t\\\" \""), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("@@"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};
static const burrow__Item lex_delim_tests_4[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("$$"), 0},
    {BURROW__ITEM_RAW_STRING, 0, S_("`abc\\n\\t\\\" `"), 0},
    {BURROW__ITEM_RIGHT_DELIM, 0, S_("@@"), 0},
    {BURROW__ITEM_EOF, 0, S_(""), 0},
};

static const LexTest lex_delim_tests[] = {
    {S_("punctuation"), S_("$$,@%{{}}@@"), lex_delim_tests_0, NELEM(lex_delim_tests_0)},
    {S_("empty action"), S_("$$@@"), lex_delim_tests_1, NELEM(lex_delim_tests_1)},
    {S_("for"), S_("$$for@@"), lex_delim_tests_2, NELEM(lex_delim_tests_2)},
    {S_("quote"), S_("$$\"abc \\n\\t\\\" \"@@"), lex_delim_tests_3,
     NELEM(lex_delim_tests_3)},
    {S_("raw quote"), S_("$$`abc\\n\\t\\\" `@@"), lex_delim_tests_4,
     NELEM(lex_delim_tests_4)},
};

static const burrow__Item lex_pos_tests_0[] = {
    {BURROW__ITEM_EOF, 0, S_(""), 1},
};
static const burrow__Item lex_pos_tests_1[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 1},
    {BURROW__ITEM_CHAR, 2, S_(","), 1},
    {BURROW__ITEM_CHAR, 3, S_("@"), 1},
    {BURROW__ITEM_CHAR, 4, S_("%"), 1},
    {BURROW__ITEM_CHAR, 5, S_("#"), 1},
    {BURROW__ITEM_RIGHT_DELIM, 6, S_("}}"), 1},
    {BURROW__ITEM_EOF, 8, S_(""), 1},
};
static const burrow__Item lex_pos_tests_2[] = {
    {BURROW__ITEM_TEXT, 0, S_("0123"), 1},
    {BURROW__ITEM_LEFT_DELIM, 4, S_("{{"), 1},
    {BURROW__ITEM_IDENTIFIER, 6, S_("hello"), 1},
    {BURROW__ITEM_RIGHT_DELIM, 11, S_("}}"), 1},
    {BURROW__ITEM_TEXT, 13, S_("xyz"), 1},
    {BURROW__ITEM_EOF, 16, S_(""), 1},
};
static const burrow__Item lex_pos_tests_3[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 1},
    {BURROW__ITEM_IDENTIFIER, 2, S_("x"), 1},
    {BURROW__ITEM_RIGHT_DELIM, 5, S_("}}"), 1},
    {BURROW__ITEM_LEFT_DELIM, 8, S_("{{"), 2},
    {BURROW__ITEM_IDENTIFIER, 10, S_("y"), 2},
    {BURROW__ITEM_RIGHT_DELIM, 11, S_("}}"), 2},
    {BURROW__ITEM_EOF, 13, S_(""), 2},
};
static const burrow__Item lex_pos_tests_4[] = {
    {BURROW__ITEM_LEFT_DELIM, 0, S_("{{"), 1},
    {BURROW__ITEM_IDENTIFIER, 2, S_("x"), 1},
    {BURROW__ITEM_RIGHT_DELIM, 3, S_("}}"), 1},
    {BURROW__ITEM_LEFT_DELIM, 6, S_("{{"), 2},
    {BURROW__ITEM_IDENTIFIER, 10, S_("y"), 2},
    {BURROW__ITEM_RIGHT_DELIM, 11, S_("}}"), 2},
    {BURROW__ITEM_EOF, 13, S_(""), 2},
};
static const burrow__Item lex_pos_tests_5[] = {
    {BURROW__ITEM_COMMENT, 2, S_("/*\n*/"), 1},
    {BURROW__ITEM_TEXT, 9, S_("\n"), 2},
    {BURROW__ITEM_LEFT_DELIM, 10, S_("{{"), 3},
    {BURROW__ITEM_IDENTIFIER, 12, S_("undefinedFunction"), 3},
    {BURROW__ITEM_SPACE, 29, S_(" "), 3},
    {BURROW__ITEM_STRING, 30, S_("\"test\""), 3},
    {BURROW__ITEM_RIGHT_DELIM, 36, S_("}}"), 3},
    {BURROW__ITEM_EOF, 38, S_(""), 3},
};

static const LexTest lex_pos_tests[] = {
    {S_("empty"), S_(""), lex_pos_tests_0, NELEM(lex_pos_tests_0)},
    {S_("punctuation"), S_("{{,@%#}}"), lex_pos_tests_1, NELEM(lex_pos_tests_1)},
    {S_("sample"), S_("0123{{hello}}xyz"), lex_pos_tests_2, NELEM(lex_pos_tests_2)},
    {S_("trimafter"), S_("{{x -}}\n{{y}}"), lex_pos_tests_3, NELEM(lex_pos_tests_3)},
    {S_("trimbefore"), S_("{{x}}\n{{- y}}"), lex_pos_tests_4, NELEM(lex_pos_tests_4)},
    {S_("longcomment"), S_("{{/*\n*/}}\n{{undefinedFunction \"test\"}}"),
     lex_pos_tests_5, NELEM(lex_pos_tests_5)},
};

/* collect gathers the emitted items into a slice. */
static Int collect(const LexTest *t, Str left, Str right, burrow__Item *items,
                   Int max) {
    burrow__Lexer l;
    burrow__parse_lex(&l, t->name, t->input, left, right);
    l.options = (burrow__LexOptions){
        .emit_comment = true,
        .break_ok = true,
        .continue_ok = true,
    };
    Int n = 0;
    while (n < max) {
        burrow__Item item = burrow__parse_lex_next_item(&l);
        items[n++] = item;
        if (item.typ == BURROW__ITEM_EOF || item.typ == BURROW__ITEM_ERROR)
            break;
    }
    return n;
}

static bool equal(const burrow__Item *i1, Int n1, const burrow__Item *i2, Int n2,
                  bool check_pos) {
    if (n1 != n2)
        return false;
    for (Int k = 0; k < n1; k++) {
        if (i1[k].typ != i2[k].typ)
            return false;
        if (!str_eq(i1[k].val, i2[k].val))
            return false;
        if (check_pos && i1[k].pos != i2[k].pos)
            return false;
        if (check_pos && i1[k].line != i2[k].line)
            return false;
    }
    return true;
}

/* The items as Go's %v prints a slice of them. */
static Str items_string(Alloc *a, const burrow__Item *items, Int n) {
    StringsBuilder sb = STRINGS_BUILDER(a);
    strings_builder_write_string(&sb, BURROW_S("["), NULL);
    for (Int i = 0; i < n; i++) {
        if (i > 0)
            strings_builder_write_string(&sb, BURROW_S(" "), NULL);
        strings_builder_write_string(&sb, burrow__parse_item_string(items[i], a), NULL);
    }
    strings_builder_write_string(&sb, BURROW_S("]"), NULL);
    return strings_builder_string(&sb);
}

enum { MAX_ITEMS = 64 };

static void TestLex(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(lex_tests); i++) {
        const LexTest *test = &lex_tests[i];
        burrow__Item items[MAX_ITEMS];
        Int n = collect(test, BURROW_STR_EMPTY, BURROW_STR_EMPTY, items, MAX_ITEMS);
        if (!equal(items, n, test->items, test->n, false)) {
            testing_t_errorf_v(t, "%s: got\n\t%s\nexpected\n\t%s", test->name,
                               items_string(a, items, n),
                               items_string(a, test->items, test->n));
            break; /* TODO */
        }
        testing_t_log_v(t, test->name, "OK");
    }
    arena_free(&ar);
}

static void TestDelims(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(lex_delim_tests); i++) {
        const LexTest *test = &lex_delim_tests[i];
        burrow__Item items[MAX_ITEMS];
        Int n = collect(test, BURROW_S("$$"), BURROW_S("@@"), items, MAX_ITEMS);
        if (!equal(items, n, test->items, test->n, false))
            testing_t_errorf_v(t, "%s: got\n\t%s\nexpected\n\t%s", test->name,
                               items_string(a, items, n),
                               items_string(a, test->items, test->n));
    }
    arena_free(&ar);
}

static void TestDelimsAlphaNumeric(TestingT *t) {
    static const burrow__Item want[] = {
        {BURROW__ITEM_LEFT_DELIM, 0, S_("{{hub"), 0},
        {BURROW__ITEM_SPACE, 0, S_(" "), 0},
        {BURROW__ITEM_FIELD, 0, S_(".host"), 0},
        {BURROW__ITEM_SPACE, 0, S_(" "), 0},
        {BURROW__ITEM_RIGHT_DELIM, 0, S_("hub}}"), 0},
        {BURROW__ITEM_EOF, 0, S_(""), 0},
    };
    static const LexTest test = {S_("right delimiter with alphanumeric start"),
                                 S_("{{hub .host hub}}"), want, NELEM(want)};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow__Item items[MAX_ITEMS];
    Int n = collect(&test, BURROW_S("{{hub"), BURROW_S("hub}}"), items, MAX_ITEMS);

    if (!equal(items, n, test.items, test.n, false))
        testing_t_errorf_v(t, "%s: got\n\t%s\nexpected\n\t%s", test.name,
                           items_string(a, items, n),
                           items_string(a, test.items, test.n));
    arena_free(&ar);
}

static void TestDelimsAndMarkers(TestingT *t) {
    static const burrow__Item want[] = {
        {BURROW__ITEM_LEFT_DELIM, 0, S_("{{- "), 0},
        {BURROW__ITEM_FIELD, 0, S_(".x"), 0},
        {BURROW__ITEM_RIGHT_DELIM, 0, S_(" -}}"), 0},
        {BURROW__ITEM_LEFT_DELIM, 0, S_("{{- "), 0},
        {BURROW__ITEM_FIELD, 0, S_(".x"), 0},
        {BURROW__ITEM_RIGHT_DELIM, 0, S_(" -}}"), 0},
        {BURROW__ITEM_EOF, 0, S_(""), 0},
    };
    static const LexTest test = {S_("delims that look like markers"),
                                 S_("{{- .x -}} {{- - .x - -}}"), want, NELEM(want)};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow__Item items[MAX_ITEMS];
    Int n = collect(&test, BURROW_S("{{- "), BURROW_S(" -}}"), items, MAX_ITEMS);

    if (!equal(items, n, test.items, test.n, false))
        testing_t_errorf_v(t, "%s: got\n\t%s\nexpected\n\t%s", test.name,
                           items_string(a, items, n),
                           items_string(a, test.items, test.n));
    arena_free(&ar);
}

/* The other tests don't check position, to make the test cases easier to
 * construct. This one does. */
static void TestPos(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(lex_pos_tests); i++) {
        const LexTest *test = &lex_pos_tests[i];
        burrow__Item items[MAX_ITEMS];
        Int n = collect(test, BURROW_STR_EMPTY, BURROW_STR_EMPTY, items, MAX_ITEMS);
        if (!equal(items, n, test->items, test->n, true)) {
            testing_t_errorf_v(t, "%s: got\n\t%s\nexpected\n\t%s", test->name,
                               items_string(a, items, n),
                               items_string(a, test->items, test->n));
            if (n == test->n) {
                /* Detailed print; avoid item.String() to expose the position
                 * value. */
                for (Int k = 0; k < n; k++) {
                    if (!equal(&items[k], 1, &test->items[k], 1, true)) {
                        burrow__Item i1 = items[k];
                        burrow__Item i2 = test->items[k];
                        testing_t_errorf_v(
                            t, "\t#%d: got {%d %d %q %d} expected {%d %d %q %d}", k,
                            (int)i1.typ, i1.pos, i1.val, i1.line, (int)i2.typ, i2.pos,
                            i2.val, i2.line);
                    }
                }
            }
        }
    }
    arena_free(&ar);
}

/* --------------------------------------------------------------- parser */

typedef struct NumberTest {
    Str text;
    bool is_int;
    bool is_uint;
    bool is_float;
    bool is_complex;
    int64_t int64;
    uint64_t uint64;
    double float64;
    Complex128 complex128;
} NumberTest;

static const NumberTest number_tests[] = {
    /* basics */
    {S_("0"), true, true, true, false, 0, 0, 0, {0, 0}},
    {S_("-0"), true, true, true, false, 0, 0, 0, {0, 0}}, /* check that -0 is a uint. */
    {S_("73"), true, true, true, false, 73, 73, 73, {0, 0}},
    {S_("7_3"), true, true, true, false, 73, 73, 73, {0, 0}},
    {S_("0b10_010_01"), true, true, true, false, 73, 73, 73, {0, 0}},
    {S_("0B10_010_01"), true, true, true, false, 73, 73, 73, {0, 0}},
    {S_("073"), true, true, true, false, 073, 073, 073, {0, 0}},
    {S_("0o73"), true, true, true, false, 073, 073, 073, {0, 0}},
    {S_("0O73"), true, true, true, false, 073, 073, 073, {0, 0}},
    {S_("0x73"), true, true, true, false, 0x73, 0x73, 0x73, {0, 0}},
    {S_("0X73"), true, true, true, false, 0x73, 0x73, 0x73, {0, 0}},
    {S_("0x7_3"), true, true, true, false, 0x73, 0x73, 0x73, {0, 0}},
    {S_("-73"), true, false, true, false, -73, 0, -73, {0, 0}},
    {S_("+73"), true, false, true, false, 73, 0, 73, {0, 0}},
    {S_("100"), true, true, true, false, 100, 100, 100, {0, 0}},
    {S_("1e9"), true, true, true, false, 1000000000, 1000000000, 1e9, {0, 0}},
    {S_("-1e9"), true, false, true, false, -1000000000, 0, -1e9, {0, 0}},
    {S_("-1.2"), false, false, true, false, 0, 0, -1.2, {0, 0}},
    {S_("1e19"), false, true, true, false, 0, 10000000000000000000u, 1e19, {0, 0}},
    {S_("1e1_9"), false, true, true, false, 0, 10000000000000000000u, 1e19, {0, 0}},
    {S_("1E19"), false, true, true, false, 0, 10000000000000000000u, 1e19, {0, 0}},
    {S_("-1e19"), false, false, true, false, 0, 0, -1e19, {0, 0}},
    {S_("0x_1p4"), true, true, true, false, 16, 16, 16, {0, 0}},
    {S_("0X_1P4"), true, true, true, false, 16, 16, 16, {0, 0}},
    {S_("0x_1p-4"), false, false, true, false, 0, 0, 1 / 16., {0, 0}},
    {S_("4i"), false, false, false, true, 0, 0, 0, {0, 4}},
    {S_("-1.2+4.2i"), false, false, false, true, 0, 0, 0, {-1.2, 4.2}},
    {S_("073i"), false, false, false, true, 0, 0, 0, {0, 73}}, /* not octal! */
    /* complex with 0 imaginary are float (and maybe integer) */
    {S_("0i"), true, true, true, true, 0, 0, 0, {0, 0}},
    {S_("-1.2+0i"), false, false, true, true, 0, 0, -1.2, {-1.2, 0}},
    {S_("-12+0i"), true, false, true, true, -12, 0, -12, {-12, 0}},
    {S_("13+0i"), true, true, true, true, 13, 13, 13, {13, 0}},
    /* funny bases */
    {S_("0123"), true, true, true, false, 0123, 0123, 0123, {0, 0}},
    {S_("-0x0"), true, true, true, false, 0, 0, 0, {0, 0}},
    {S_("0xdeadbeef"),
     true,
     true,
     true,
     false,
     0xdeadbeef,
     0xdeadbeef,
     0xdeadbeef,
     {0, 0}},
    /* character constants */
    {S_("'a'"), true, true, true, false, 'a', 'a', 'a', {0, 0}},
    {S_("'\\n'"), true, true, true, false, '\n', '\n', '\n', {0, 0}},
    {S_("'\\\\'"), true, true, true, false, '\\', '\\', '\\', {0, 0}},
    {S_("'\\''"), true, true, true, false, '\'', '\'', '\'', {0, 0}},
    {S_("'\\xFF'"), true, true, true, false, 0xFF, 0xFF, 0xFF, {0, 0}},
    {S_("'\343\203\221'"), true, true, true, false, 0x30d1, 0x30d1, 0x30d1, {0, 0}},
    {S_("'\\u30d1'"), true, true, true, false, 0x30d1, 0x30d1, 0x30d1, {0, 0}},
    {S_("'\\U000030d1'"), true, true, true, false, 0x30d1, 0x30d1, 0x30d1, {0, 0}},
    /* some broken syntax */
    {.text = S_("+-2")},
    {.text = S_("0x123.")},
    {.text = S_("1e.")},
    {.text = S_("0xi.")},
    {.text = S_("1+2.")},
    {.text = S_("'x")},
    {.text = S_("'xx'")},
    {.text = S_("'433937734937734969526500969526500'")}, /* Integer too large - issue
                                                            10634. */
    /* Issue 8622 - 0xe parsed as floating point. Very embarrassing. */
    {S_("0xef"), true, true, true, false, 0xef, 0xef, 0xef, {0, 0}},
};

static void TestNumberParse(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Go calls newNumber on a nil tree; this one only gives it the allocator. */
    ParseTree *tree = parse_new(a, BURROW_STR_EMPTY, (Slice){0});
    for (Int i = 0; i < NELEM(number_tests); i++) {
        const NumberTest *test = &number_tests[i];
        /* If fmt.Sscan thinks it's complex, it's complex. We can't trust the
         * output because imaginary comes out as a number. */
        Complex128 c = {0, 0};
        burrow__ItemType typ = BURROW__ITEM_NUMBER;
        if (test->text.p[0] == '\'') {
            typ = BURROW__ITEM_CHAR_CONSTANT;
        } else {
            Error err = BURROW_NO_ERROR;
            fmt_sscan_v(a, &err, test->text, &c);
            if (BURROW_OK(err))
                typ = BURROW__ITEM_COMPLEX;
        }
        Error err = BURROW_NO_ERROR;
        ParseNumberNode *n = burrow__parse_new_number(tree, 0, test->text, typ, &err);
        bool ok = test->is_int || test->is_uint || test->is_float || test->is_complex;
        if (ok && BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "unexpected error for %q: %s", test->text, err);
            continue;
        }
        if (!ok && BURROW_OK(err)) {
            testing_t_errorf_v(t, "expected error for %q", test->text);
            continue;
        }
        if (!ok)
            continue;
        if (n->is_complex != test->is_complex)
            testing_t_errorf_v(t, "complex incorrect for %q; should be %t", test->text,
                               test->is_complex);
        if (test->is_int) {
            if (!n->is_int)
                testing_t_errorf_v(t, "expected integer for %q", test->text);
            if (n->int64 != test->int64)
                testing_t_errorf_v(t, "int64 for %q should be %d Is %d", test->text,
                                   test->int64, n->int64);
        } else if (n->is_int) {
            testing_t_errorf_v(t, "did not expect integer for %q", test->text);
        }
        if (test->is_uint) {
            if (!n->is_uint)
                testing_t_errorf_v(t, "expected unsigned integer for %q", test->text);
            if (n->uint64 != test->uint64)
                testing_t_errorf_v(t, "uint64 for %q should be %d Is %d", test->text,
                                   test->uint64, n->uint64);
        } else if (n->is_uint) {
            testing_t_errorf_v(t, "did not expect unsigned integer for %q", test->text);
        }
        if (test->is_float) {
            if (!n->is_float)
                testing_t_errorf_v(t, "expected float for %q", test->text);
            if (n->float64 != test->float64)
                testing_t_errorf_v(t, "float64 for %q should be %g Is %g", test->text,
                                   test->float64, n->float64);
        } else if (n->is_float) {
            testing_t_errorf_v(t, "did not expect float for %q", test->text);
        }
        if (test->is_complex) {
            if (!n->is_complex)
                testing_t_errorf_v(t, "expected complex for %q", test->text);
            if (n->complex128.re != test->complex128.re ||
                n->complex128.im != test->complex128.im)
                testing_t_errorf_v(t, "complex128 for %q should be %g Is %g",
                                   test->text, test->complex128, n->complex128);
        } else if (n->is_complex) {
            testing_t_errorf_v(t, "did not expect complex for %q", test->text);
        }
    }
    arena_free(&ar);
}

typedef struct ParseTest {
    Str name;
    Str input;
    bool ok;
    Str result; /* what the user would see in an error message. */
} ParseTest;

enum { NO_ERROR = true, HAS_ERROR = false };

static const ParseTest parse_tests[] = {
    {S_("empty"), S_(""), NO_ERROR, S_("")},
    {S_("comment"), S_("{{/*\n\n\n*/}}"), NO_ERROR, S_("")},
    {S_("spaces"), S_(" \t\n"), NO_ERROR, S_("\" \\t\\n\"")},
    {S_("text"), S_("some text"), NO_ERROR, S_("\"some text\"")},
    {S_("emptyAction"), S_("{{}}"), HAS_ERROR, S_("{{}}")},
    {S_("field"), S_("{{.X}}"), NO_ERROR, S_("{{.X}}")},
    {S_("simple command"), S_("{{printf}}"), NO_ERROR, S_("{{printf}}")},
    {S_("$ invocation"), S_("{{$}}"), NO_ERROR, S_("{{$}}")},
    {S_("variable invocation"), S_("{{with $x := 3}}{{$x 23}}{{end}}"), NO_ERROR,
     S_("{{with $x := 3}}{{$x 23}}{{end}}")},
    {S_("variable with fields"), S_("{{$.I}}"), NO_ERROR, S_("{{$.I}}")},
    {S_("multi-word command"), S_("{{printf `%d` 23}}"), NO_ERROR,
     S_("{{printf `%d` 23}}")},
    {S_("pipeline"), S_("{{.X|.Y}}"), NO_ERROR, S_("{{.X | .Y}}")},
    {S_("pipeline with decl"), S_("{{$x := .X|.Y}}"), NO_ERROR,
     S_("{{$x := .X | .Y}}")},
    {S_("nested pipeline"), S_("{{.X (.Y .Z) (.A | .B .C) (.E)}}"), NO_ERROR,
     S_("{{.X (.Y .Z) (.A | .B .C) (.E)}}")},
    {S_("field applied to parentheses"), S_("{{(.Y .Z).Field}}"), NO_ERROR,
     S_("{{(.Y .Z).Field}}")},
    {S_("simple if"), S_("{{if .X}}hello{{end}}"), NO_ERROR,
     S_("{{if .X}}\"hello\"{{end}}")},
    {S_("if with else"), S_("{{if .X}}true{{else}}false{{end}}"), NO_ERROR,
     S_("{{if .X}}\"true\"{{else}}\"false\"{{end}}")},
    {S_("if with else if"), S_("{{if .X}}true{{else if .Y}}false{{end}}"), NO_ERROR,
     S_("{{if .X}}\"true\"{{else}}{{if .Y}}\"false\"{{end}}{{end}}")},
    {S_("if else chain"), S_("+{{if .X}}X{{else if .Y}}Y{{else if .Z}}Z{{end}}+"),
     NO_ERROR,
     S_("\"+\"{{if .X}}\"X\"{{else}}{{if .Y}}\"Y\"{{else}}{{if "
        ".Z}}\"Z\"{{end}}{{end}}{{end}}\"+\"")},
    {S_("simple range"), S_("{{range .X}}hello{{end}}"), NO_ERROR,
     S_("{{range .X}}\"hello\"{{end}}")},
    {S_("chained field range"), S_("{{range .X.Y.Z}}hello{{end}}"), NO_ERROR,
     S_("{{range .X.Y.Z}}\"hello\"{{end}}")},
    {S_("nested range"), S_("{{range .X}}hello{{range .Y}}goodbye{{end}}{{end}}"),
     NO_ERROR, S_("{{range .X}}\"hello\"{{range .Y}}\"goodbye\"{{end}}{{end}}")},
    {S_("range with else"), S_("{{range .X}}true{{else}}false{{end}}"), NO_ERROR,
     S_("{{range .X}}\"true\"{{else}}\"false\"{{end}}")},
    {S_("range over pipeline"), S_("{{range .X|.M}}true{{else}}false{{end}}"), NO_ERROR,
     S_("{{range .X | .M}}\"true\"{{else}}\"false\"{{end}}")},
    {S_("range []int"), S_("{{range .SI}}{{.}}{{end}}"), NO_ERROR,
     S_("{{range .SI}}{{.}}{{end}}")},
    {S_("range 1 var"), S_("{{range $x := .SI}}{{.}}{{end}}"), NO_ERROR,
     S_("{{range $x := .SI}}{{.}}{{end}}")},
    {S_("range 2 vars"), S_("{{range $x, $y := .SI}}{{.}}{{end}}"), NO_ERROR,
     S_("{{range $x, $y := .SI}}{{.}}{{end}}")},
    {S_("range with break"), S_("{{range .SI}}{{.}}{{break}}{{end}}"), NO_ERROR,
     S_("{{range .SI}}{{.}}{{break}}{{end}}")},
    {S_("range with continue"), S_("{{range .SI}}{{.}}{{continue}}{{end}}"), NO_ERROR,
     S_("{{range .SI}}{{.}}{{continue}}{{end}}")},
    {S_("constants"), S_("{{range .SI 1 -3.2i true false 'a' nil}}{{end}}"), NO_ERROR,
     S_("{{range .SI 1 -3.2i true false 'a' nil}}{{end}}")},
    {S_("template"), S_("{{template `x`}}"), NO_ERROR, S_("{{template \"x\"}}")},
    {S_("template with arg"), S_("{{template `x` .Y}}"), NO_ERROR,
     S_("{{template \"x\" .Y}}")},
    {S_("with"), S_("{{with .X}}hello{{end}}"), NO_ERROR,
     S_("{{with .X}}\"hello\"{{end}}")},
    {S_("with with else"), S_("{{with .X}}hello{{else}}goodbye{{end}}"), NO_ERROR,
     S_("{{with .X}}\"hello\"{{else}}\"goodbye\"{{end}}")},
    {S_("with with else with"), S_("{{with .X}}hello{{else with .Y}}goodbye{{end}}"),
     NO_ERROR, S_("{{with .X}}\"hello\"{{else}}{{with .Y}}\"goodbye\"{{end}}{{end}}")},
    {S_("with else chain"), S_("{{with .X}}X{{else with .Y}}Y{{else with .Z}}Z{{end}}"),
     NO_ERROR,
     S_("{{with .X}}\"X\"{{else}}{{with .Y}}\"Y\"{{else}}{{with "
        ".Z}}\"Z\"{{end}}{{end}}{{end}}")},
    {S_("trim left"), S_("x \r\n\t{{- 3}}"), NO_ERROR, S_("\"x\"{{3}}")},
    {S_("trim right"), S_("{{3 -}}\n\n\ty"), NO_ERROR, S_("{{3}}\"y\"")},
    {S_("trim left and right"), S_("x \r\n\t{{- 3 -}}\n\n\ty"), NO_ERROR,
     S_("\"x\"{{3}}\"y\"")},
    {S_("trim with extra spaces"), S_("x\n{{-  3   -}}\ny"), NO_ERROR,
     S_("\"x\"{{3}}\"y\"")},
    {S_("comment trim left"), S_("x \r\n\t{{- /* hi */}}"), NO_ERROR, S_("\"x\"")},
    {S_("comment trim right"), S_("{{/* hi */ -}}\n\n\ty"), NO_ERROR, S_("\"y\"")},
    {S_("comment trim left and right"), S_("x \r\n\t{{- /* */ -}}\n\n\ty"), NO_ERROR,
     S_("\"x\"\"y\"")},
    {S_("block definition"), S_("{{block \"foo\" .}}hello{{end}}"), NO_ERROR,
     S_("{{template \"foo\" .}}")},
    {S_("newline in assignment"), S_("{{ $x \n := \n 1 \n }}"), NO_ERROR,
     S_("{{$x := 1}}")},
    {S_("newline in empty action"), S_("{{\n}}"), HAS_ERROR, S_("{{\n}}")},
    {S_("newline in pipeline"), S_("{{\n\"x\"\n|\nprintf\n}}"), NO_ERROR,
     S_("{{\"x\" | printf}}")},
    {S_("newline in comment"), S_("{{/*\nhello\n*/}}"), NO_ERROR, S_("")},
    {S_("newline in comment"), S_("{{-\n/*\nhello\n*/\n-}}"), NO_ERROR, S_("")},
    {S_("spaces around continue"), S_("{{range .SI}}{{.}}{{ continue }}{{end}}"),
     NO_ERROR, S_("{{range .SI}}{{.}}{{continue}}{{end}}")},
    {S_("spaces around break"), S_("{{range .SI}}{{.}}{{ break }}{{end}}"), NO_ERROR,
     S_("{{range .SI}}{{.}}{{break}}{{end}}")},
    {S_("unclosed action"), S_("hello{{range"), HAS_ERROR, S_("")},
    {S_("unmatched end"), S_("{{end}}"), HAS_ERROR, S_("")},
    {S_("unmatched else"), S_("{{else}}"), HAS_ERROR, S_("")},
    {S_("unmatched else after if"), S_("{{if .X}}hello{{end}}{{else}}"), HAS_ERROR,
     S_("")},
    {S_("multiple else"), S_("{{if .X}}1{{else}}2{{else}}3{{end}}"), HAS_ERROR, S_("")},
    {S_("missing end"), S_("hello{{range .x}}"), HAS_ERROR, S_("")},
    {S_("missing end after else"), S_("hello{{range .x}}{{else}}"), HAS_ERROR, S_("")},
    {S_("undefined function"), S_("hello{{undefined}}"), HAS_ERROR, S_("")},
    {S_("undefined variable"), S_("{{$x}}"), HAS_ERROR, S_("")},
    {S_("variable undefined after end"), S_("{{with $x := 4}}{{end}}{{$x}}"), HAS_ERROR,
     S_("")},
    {S_("variable undefined in template"), S_("{{template $v}}"), HAS_ERROR, S_("")},
    {S_("declare with field"), S_("{{with $x.Y := 4}}{{end}}"), HAS_ERROR, S_("")},
    {S_("template with field ref"), S_("{{template .X}}"), HAS_ERROR, S_("")},
    {S_("template with var"), S_("{{template $v}}"), HAS_ERROR, S_("")},
    {S_("invalid punctuation"), S_("{{printf 3, 4}}"), HAS_ERROR, S_("")},
    {S_("multidecl outside range"), S_("{{with $v, $u := 3}}{{end}}"), HAS_ERROR,
     S_("")},
    {S_("too many decls in range"), S_("{{range $u, $v, $w := 3}}{{end}}"), HAS_ERROR,
     S_("")},
    {S_("dot applied to parentheses"), S_("{{printf (printf .).}}"), HAS_ERROR, S_("")},
    {S_("adjacent args"), S_("{{printf 3`x`}}"), HAS_ERROR, S_("")},
    {S_("adjacent args with ."), S_("{{printf `x`.}}"), HAS_ERROR, S_("")},
    {S_("extra end after if"), S_("{{if .X}}a{{else if .Y}}b{{end}}{{end}}"), HAS_ERROR,
     S_("")},
    {S_("break outside range"), S_("{{range .}}{{end}} {{break}}"), HAS_ERROR, S_("")},
    {S_("continue outside range"), S_("{{range .}}{{end}} {{continue}}"), HAS_ERROR,
     S_("")},
    {S_("break in range else"), S_("{{range .}}{{else}}{{break}}{{end}}"), HAS_ERROR,
     S_("")},
    {S_("continue in range else"), S_("{{range .}}{{else}}{{continue}}{{end}}"),
     HAS_ERROR, S_("")},
    {S_("bug0a"), S_("{{$x := 0}}{{$x}}"), NO_ERROR, S_("{{$x := 0}}{{$x}}")},
    {S_("bug0b"), S_("{{$x += 1}}{{$x}}"), HAS_ERROR, S_("")},
    {S_("bug0c"), S_("{{$x ! 2}}{{$x}}"), HAS_ERROR, S_("")},
    {S_("bug0d"), S_("{{$x % 3}}{{$x}}"), HAS_ERROR, S_("")},
    {S_("bug0e"), S_("{{range $x := $y := 3}}{{end}}"), HAS_ERROR, S_("")},
    {S_("bug1a"), S_("{{$x:=.}}{{$x!2}}"), HAS_ERROR, S_("")},
    {S_("bug1b"), S_("{{$x:=.}}{{$x+2}}"), HAS_ERROR, S_("")},
    {S_("bug1c"), S_("{{$x:=.}}{{$x +2}}"), NO_ERROR, S_("{{$x := .}}{{$x +2}}")},
    {S_("bug2a"), S_("{{range $x := 0}}{{$x}}{{end}}"), NO_ERROR,
     S_("{{range $x := 0}}{{$x}}{{end}}")},
    {S_("bug2b"), S_("{{range $x = 0}}{{$x}}{{end}}"), NO_ERROR,
     S_("{{range $x = 0}}{{$x}}{{end}}")},
    {S_("dot after integer"), S_("{{1.E}}"), HAS_ERROR, S_("")},
    {S_("dot after float"), S_("{{0.1.E}}"), HAS_ERROR, S_("")},
    {S_("dot after boolean"), S_("{{true.E}}"), HAS_ERROR, S_("")},
    {S_("dot after char"), S_("{{'a'.any}}"), HAS_ERROR, S_("")},
    {S_("dot after string"), S_("{{\"hello\".guys}}"), HAS_ERROR, S_("")},
    {S_("dot after dot"), S_("{{..E}}"), HAS_ERROR, S_("")},
    {S_("dot after nil"), S_("{{nil.E}}"), HAS_ERROR, S_("")},
    {S_("wrong pipeline dot"), S_("{{12|.}}"), HAS_ERROR, S_("")},
    {S_("wrong pipeline number"), S_("{{.|12|printf}}"), HAS_ERROR, S_("")},
    {S_("wrong pipeline string"), S_("{{.|printf|\"error\"}}"), HAS_ERROR, S_("")},
    {S_("wrong pipeline char"), S_("{{12|printf|'e'}}"), HAS_ERROR, S_("")},
    {S_("wrong pipeline boolean"), S_("{{.|true}}"), HAS_ERROR, S_("")},
    {S_("wrong pipeline nil"), S_("{{'c'|nil}}"), HAS_ERROR, S_("")},
    {S_("empty pipeline"), S_("{{printf \"%d\" ( ) }}"), HAS_ERROR, S_("")},
    {S_("block definition"), S_("{{block \"foo\"}}hello{{end}}"), HAS_ERROR, S_("")},
    {S_("paren nesting normal"), S_("{{ (( 1 )) }}"), NO_ERROR, S_("{{((1))}}")},
    {S_("paren nesting at limit"), S_("{{ ((( 1 ))) }}"), NO_ERROR, S_("{{(((1)))}}")},
    {S_("paren nesting exceeds limit"), S_("{{ (((( 1 )))) }}"), HAS_ERROR,
     S_("template: test:1: max expression depth exceeded")},
    {S_("paren nesting in pipeline"), S_("{{ ((( 1 ))) | printf }}"), NO_ERROR,
     S_("{{(((1))) | printf}}")},
    {S_("paren nesting in pipeline exceeds limit"), S_("{{ (((( 1 )))) | printf }}"),
     HAS_ERROR, S_("template: test:1: max expression depth exceeded")},
    {S_("paren nesting with other constructs"), S_("{{ if ((( true ))) }}YES{{ end }}"),
     NO_ERROR, S_("{{if (((true)))}}\"YES\"{{end}}")},
    {S_("paren nesting with other constructs exceeds limit"),
     S_("{{ if (((( true )))) }}YES{{ end }}"), HAS_ERROR,
     S_("template: test:1: max expression depth exceeded")},
};

static const ParseTest error_tests[] = {
    {S_("unclosed1"), S_("line1\n{{"), HAS_ERROR, S_("unclosed1:2: unclosed action")},
    {S_("unclosed2"), S_("line1\n{{define `x`}}line2\n{{"), HAS_ERROR,
     S_("unclosed2:3: unclosed action")},
    {S_("unclosed3"), S_("line1\n{{\"x\"\n\"y\"\n"), HAS_ERROR,
     S_("unclosed3:4: unclosed action started at unclosed3:2")},
    {S_("unclosed4"), S_("{{\n\n\n\n\n"), HAS_ERROR,
     S_("unclosed4:6: unclosed action started at unclosed4:1")},
    {S_("var1"), S_("line1\n{{\nx\n}}"), HAS_ERROR,
     S_("var1:3: function \"x\" not defined")},
    {S_("function"), S_("{{foo}}"), HAS_ERROR, S_("function \"foo\" not defined")},
    {S_("comment1"), S_("{{/*}}"), HAS_ERROR, S_("comment1:1: unclosed comment")},
    {S_("comment2"), S_("{{/*\nhello\n}}"), HAS_ERROR,
     S_("comment2:1: unclosed comment")},
    {S_("lparen"), S_("{{.X (1 2 3}}"), HAS_ERROR, S_("unclosed left paren")},
    {S_("rparen"), S_("{{.X 1 2 3 ) }}"), HAS_ERROR, S_("unexpected right paren")},
    {S_("rparen2"), S_("{{(.X 1 2 3"), HAS_ERROR, S_("unclosed action")},
    {S_("space"), S_("{{`x`3}}"), HAS_ERROR, S_("in operand")},
    {S_("idchar"), S_("{{a#}}"), HAS_ERROR, S_("'#'")},
    {S_("charconst"), S_("{{'a}}"), HAS_ERROR, S_("unterminated character constant")},
    {S_("stringconst"), S_("{{\"a}}"), HAS_ERROR, S_("unterminated quoted string")},
    {S_("rawstringconst"), S_("{{`a}}"), HAS_ERROR,
     S_("unterminated raw quoted string")},
    {S_("number"), S_("{{0xi}}"), HAS_ERROR, S_("number syntax")},
    {S_("multidefine"), S_("{{define `a`}}a{{end}}{{define `a`}}b{{end}}"), HAS_ERROR,
     S_("multiple definition of template")},
    {S_("eof"), S_("{{range .X}}"), HAS_ERROR, S_("unexpected EOF")},
    {S_("variable"), S_("{{$x := 23}}{{with $x.y := 3}}{{$x 23}}{{end}}"), HAS_ERROR,
     S_("unexpected \":=\"")},
    {S_("multidecl"), S_("{{$a,$b,$c := 23}}"), HAS_ERROR, S_("too many declarations")},
    {S_("undefvar"), S_("{{$a}}"), HAS_ERROR, S_("undefined variable")},
    {S_("wrongdot"), S_("{{true.any}}"), HAS_ERROR, S_("unexpected . after term")},
    {S_("wrongpipeline"), S_("{{12|false}}"), HAS_ERROR,
     S_("non executable command in pipeline")},
    {S_("emptypipeline"), S_("{{ ( ) }}"), HAS_ERROR,
     S_("missing value for parenthesized pipeline")},
    {S_("multilinerawstring"), S_("{{ $v := `\n` }} {{"), HAS_ERROR,
     S_("multilinerawstring:2: unclosed action")},
    {S_("rangeundefvar"), S_("{{range $k}}{{end}}"), HAS_ERROR,
     S_("undefined variable")},
    {S_("rangeundefvars"), S_("{{range $k, $v}}{{end}}"), HAS_ERROR,
     S_("undefined variable")},
    {S_("rangemissingvalue1"), S_("{{range $k,}}{{end}}"), HAS_ERROR,
     S_("missing value for range")},
    {S_("rangemissingvalue2"), S_("{{range $k, $v := }}{{end}}"), HAS_ERROR,
     S_("missing value for range")},
    {S_("rangenotvariable1"), S_("{{range $k, .}}{{end}}"), HAS_ERROR,
     S_("range can only initialize variables")},
    {S_("rangenotvariable2"), S_("{{range $k, 123 := .}}{{end}}"), HAS_ERROR,
     S_("range can only initialize variables")},
};

/* builtins: Go's has printf and contains, and only the names matter here. */
static Map *builtins(Alloc *a) {
    Map *m = map_make(a, TYPE_STRING, TYPE_ANY, 0);
    Any f = BURROW_ANY_VAL(TYPE_INT, Int, 1);
    Str printf_ = BURROW_S("printf");
    Str contains = BURROW_S("contains");
    map_set(m, &printf_, &f);
    map_set(m, &contains, &f);
    return m;
}

static Slice funcs_of(Map **m) {
    return (Slice){m, 1, 1, TYPE_UNSAFE_POINTER};
}

static Map *tree_set(Alloc *a) {
    return map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
}

static void test_parse(bool do_copy, TestingT *t) {
    burrow__parse_text_format_q = true;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Map *b = builtins(a);
    for (Int i = 0; i < NELEM(parse_tests); i++) {
        const ParseTest *test = &parse_tests[i];
        Error err = BURROW_NO_ERROR;
        ParseTree *tmpl = parse_tree_parse(
            parse_new(a, test->name, (Slice){0}), test->input, BURROW_STR_EMPTY,
            BURROW_STR_EMPTY, tree_set(a), funcs_of(&b), &err);
        if (BURROW_OK(err) && !test->ok) {
            testing_t_errorf_v(t, "%q: expected error; got none", test->name);
            continue;
        }
        if (BURROW_FAILED(err) && test->ok) {
            testing_t_errorf_v(t, "%q: unexpected error: %v", test->name, err);
            continue;
        }
        if (BURROW_FAILED(err) && !test->ok)
            continue; /* expected error, got one */
        Str result;
        if (do_copy)
            result =
                parse_list_node_string(parse_list_node_copy_list(tmpl->root, a), a);
        else
            result = parse_list_node_string(tmpl->root, a);
        if (!str_eq(result, test->result))
            testing_t_errorf_v(t, "%s=(%q): got\n\t%s\nexpected\n\t%s", test->name,
                               test->input, result, test->result);
    }
    arena_free(&ar);
    burrow__parse_text_format_q = false;
}

static void TestParse(TestingT *t) {
    test_parse(false, t);
}

/* Same as TestParse, but we copy the node first */
static void TestParseCopy(TestingT *t) {
    test_parse(true, t);
}

static const ParseTest with_comments_tests[] = {
    {S_("comment"), S_("{{/*\n\n\n*/}}"), NO_ERROR, S_("{{/*\n\n\n*/}}")},
    {S_("comment trim left"), S_("x \r\n\t{{- /* hi */}}"), NO_ERROR,
     S_("\"x\"{{/* hi */}}")},
    {S_("comment trim right"), S_("{{/* hi */ -}}\n\n\ty"), NO_ERROR,
     S_("{{/* hi */}}\"y\"")},
    {S_("comment trim left and right"), S_("x \r\n\t{{- /* */ -}}\n\n\ty"), NO_ERROR,
     S_("\"x\"{{/* */}}\"y\"")},
};

static void test_parse_with_comments(void *env, TestingT *t) {
    const ParseTest *test = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ParseTree *tr = parse_new(a, test->name, (Slice){0});
    tr->mode = PARSE_PARSE_COMMENTS;
    Error err = BURROW_NO_ERROR;
    ParseTree *tmpl = parse_tree_parse(tr, test->input, BURROW_STR_EMPTY,
                                       BURROW_STR_EMPTY, tree_set(a), (Slice){0}, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%q: expected error; got none", test->name);
    Str result =
        tmpl == NULL ? BURROW_STR_EMPTY : parse_list_node_string(tmpl->root, a);
    if (tmpl == NULL || !str_eq(result, test->result))
        testing_t_errorf_v(t, "%s=(%q): got\n\t%s\nexpected\n\t%s", test->name,
                           test->input, result, test->result);
    arena_free(&ar);
}

static void TestParseWithComments(TestingT *t) {
    burrow__parse_text_format_q = true;
    for (Int i = 0; i < NELEM(with_comments_tests); i++) {
        const ParseTest *test = &with_comments_tests[i];
        testing_t_run(
            t, test->name,
            BURROW_FN(TestingTFunc, test_parse_with_comments, (void *)(uintptr_t)test));
    }
    burrow__parse_text_format_q = false;
}

static void TestDelimsStringRoundtrip(TestingT *t) {
    /* Each input is already in canonical String form, so parsing it with the
     * custom delimiters and printing the resulting tree must reproduce the
     * input exactly. This exercises the String/writeTo methods of every node
     * type that emits delimiters. */
    static const Str inputs[] = {
        S_("[[.X]]"),                     /* ActionNode */
        S_("[[/* a comment */]]"),        /* CommentNode */
        S_("[[if .X]]y[[else]]z[[end]]"), /* BranchNode (if), with else and end */
        S_("[[range .X]][[break]][[continue]][[end]]"), /* RangeNode, BreakNode,
                                                           ContinueNode */
        S_("[[with .X]]y[[end]]"),                      /* BranchNode (with) */
        S_("[[template \"name\" .]]"),                  /* TemplateNode */
    };
    Str left = BURROW_S("[[");
    Str right = BURROW_S("]]");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(inputs); i++) {
        Str input = inputs[i];
        ParseTree *tr = parse_new(a, BURROW_S("test"), (Slice){0});
        tr->mode = PARSE_PARSE_COMMENTS;
        Error err = BURROW_NO_ERROR;
        ParseTree *tmpl =
            parse_tree_parse(tr, input, left, right, tree_set(a), (Slice){0}, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%q: unexpected parse error: %v", input, err);
            continue;
        }
        Str got = parse_list_node_string(tmpl->root, a);
        if (!str_eq(got, input))
            testing_t_errorf_v(t, "got\n\t%q\nexpected\n\t%q", got, input);
    }
    arena_free(&ar);
}

static void TestKeywordsAndFuncs(TestingT *t) {
    /* Check collisions between functions and new keywords like 'break'. When a
     * break function is provided, the parser should treat 'break' as a
     * function, not a keyword. */
    burrow__parse_text_format_q = true;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Str inp = BURROW_S("{{range .X}}{{break 20}}{{end}}");
    {
        /* 'break' is a defined function, don't treat it as a keyword: it
         * should accept an argument successfully. */
        Map *funcs_with_keyword_func = map_make(a, TYPE_STRING, TYPE_ANY, 0);
        Str name = BURROW_S("break");
        Any f = BURROW_ANY_VAL(TYPE_INT, Int, 1);
        map_set(funcs_with_keyword_func, &name, &f);
        Error err = BURROW_NO_ERROR;
        ParseTree *tmpl = parse_tree_parse(
            parse_new(a, BURROW_STR_EMPTY, (Slice){0}), inp, BURROW_STR_EMPTY,
            BURROW_STR_EMPTY, tree_set(a), funcs_of(&funcs_with_keyword_func), &err);
        if (BURROW_FAILED(err) || tmpl == NULL)
            testing_t_errorf_v(t, "with break func: unexpected error: %v", err);
    }

    {
        /* No function called 'break'; treat it as a keyword. Results in a
         * parse error. */
        Map *empty = map_make(a, TYPE_STRING, TYPE_ANY, 0);
        Error err = BURROW_NO_ERROR;
        ParseTree *tmpl = parse_tree_parse(parse_new(a, BURROW_STR_EMPTY, (Slice){0}),
                                           inp, BURROW_STR_EMPTY, BURROW_STR_EMPTY,
                                           tree_set(a), funcs_of(&empty), &err);
        if (BURROW_OK(err) || tmpl != NULL)
            testing_t_errorf_v(t, "without break func: expected error; got none");
    }
    arena_free(&ar);
    burrow__parse_text_format_q = false;
}

static void TestSkipFuncCheck(TestingT *t) {
    bool old_text_format = burrow__parse_text_format_q;
    burrow__parse_text_format_q = true;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ParseTree *tr = parse_new(a, BURROW_S("skip func check"), (Slice){0});
    tr->mode = PARSE_SKIP_FUNC_CHECK;
    Error err = BURROW_NO_ERROR;
    ParseTree *tmpl = parse_tree_parse(tr, BURROW_S("{{fn 1 2}}"), BURROW_STR_EMPTY,
                                       BURROW_STR_EMPTY, tree_set(a), (Slice){0}, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        burrow__parse_text_format_q = old_text_format;
        testing_t_fatalf_v(t, "unexpected error: %v", err);
    }
    Str expected = BURROW_S("{{fn 1 2}}");
    Str result = parse_list_node_string(tmpl->root, a);
    if (!str_eq(result, expected))
        testing_t_errorf_v(t, "got\n\t%s\nexpected\n\t%s", result, expected);
    arena_free(&ar);
    burrow__parse_text_format_q = old_text_format;
}

typedef struct IsEmptyTest {
    Str name;
    Str input;
    bool empty;
} IsEmptyTest;

static const IsEmptyTest is_empty_tests[] = {
    {S_("empty"), S_(""), true},
    {S_("nonempty"), S_("hello"), false},
    {S_("spaces only"), S_(" \t\n \t\n"), true},
    {S_("comment only"), S_("{{/* comment */}}"), true},
    {S_("definition"), S_("{{define \"x\"}}something{{end}}"), true},
    {S_("definitions and space"),
     S_("{{define `x`}}something{{end}}\n\n{{define `y`}}something{{end}}\n\n"), true},
    {S_("definitions and text"),
     S_("{{define `x`}}something{{end}}\nx\n{{define `y`}}something{{end}}\ny\n"),
     false},
    {S_("definition and action"),
     S_("{{define `x`}}something{{end}}{{if 3}}foo{{end}}"), false},
};

static void TestIsEmpty(TestingT *t) {
    if (!parse_is_empty_tree(NULL))
        testing_t_errorf_v(t, "nil tree is not empty");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Map *nil_map = NULL;
    for (Int i = 0; i < NELEM(is_empty_tests); i++) {
        const IsEmptyTest *test = &is_empty_tests[i];
        Error err = BURROW_NO_ERROR;
        ParseTree *tree = parse_tree_parse(
            parse_new(a, BURROW_S("root"), (Slice){0}), test->input, BURROW_STR_EMPTY,
            BURROW_STR_EMPTY, tree_set(a), funcs_of(&nil_map), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%q: unexpected error: %v", test->name, err);
            continue;
        }
        bool empty = parse_is_empty_tree(&tree->root->node_type);
        if (empty != test->empty)
            testing_t_errorf_v(t, "%q: expected %t got %t", test->name, test->empty,
                               empty);
    }
    arena_free(&ar);
}

static void TestErrorContextWithTreeCopy(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Map *nil_map = NULL;
    Error err = BURROW_NO_ERROR;
    ParseTree *tree = parse_tree_parse(
        parse_new(a, BURROW_S("root"), (Slice){0}), BURROW_S("{{if true}}{{end}}"),
        BURROW_STR_EMPTY, BURROW_STR_EMPTY, tree_set(a), funcs_of(&nil_map), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "unexpected tree parse failure: %v", err);
    }
    ParseTree *tree_copy = parse_tree_copy(tree, a);
    Str want_context = BURROW_STR_EMPTY;
    Str got_context = BURROW_STR_EMPTY;
    Str want_location = parse_tree_error_context(
        tree, a, BURROW_AT(ParseNode, tree->root->nodes, 0), &want_context);
    Str got_location = parse_tree_error_context(
        tree_copy, a, BURROW_AT(ParseNode, tree_copy->root->nodes, 0), &got_context);
    if (!str_eq(want_location, got_location))
        testing_t_errorf_v(t, "wrong error location want %q got %q", want_location,
                           got_location);
    if (!str_eq(want_context, got_context))
        testing_t_errorf_v(t, "wrong error location want %q got %q", want_context,
                           got_context);
    arena_free(&ar);
}

static void test_error(void *env, TestingT *t) {
    const ParseTest *test = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    parse_tree_parse(parse_new(a, test->name, (Slice){0}), test->input,
                     BURROW_STR_EMPTY, BURROW_STR_EMPTY, tree_set(a), (Slice){0}, &err);
    if (BURROW_OK(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "expected error %q, got nil", test->result);
    }
    if (!strings_contains(error_text(err), test->result)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "error %q does not contain %q", err, test->result);
    }
    arena_free(&ar);
}

static void TestErrors(TestingT *t) {
    for (Int i = 0; i < NELEM(error_tests); i++) {
        const ParseTest *test = &error_tests[i];
        testing_t_run(t, test->name,
                      BURROW_FN(TestingTFunc, test_error, (void *)(uintptr_t)test));
    }
}

static void TestBlock(TestingT *t) {
    Str input = BURROW_S("a{{block \"inner\" .}}bar{{.}}baz{{end}}b");
    Str outer = BURROW_S("a{{template \"inner\" .}}b");
    Str inner = BURROW_S("bar{{.}}baz");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Map *nil_map = NULL;
    Map *trees = tree_set(a);
    Error err = BURROW_NO_ERROR;
    ParseTree *tmpl = parse_tree_parse(parse_new(a, BURROW_S("outer"), (Slice){0}),
                                       input, BURROW_STR_EMPTY, BURROW_STR_EMPTY, trees,
                                       funcs_of(&nil_map), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    Str g = parse_list_node_string(tmpl->root, a);
    if (!str_eq(g, outer))
        testing_t_errorf_v(t, "outer template = %q, want %q", g, outer);
    Str inner_name = BURROW_S("inner");
    ParseTree **in_tmpl = map_get(trees, &inner_name);
    if (in_tmpl == NULL || *in_tmpl == NULL) {
        arena_free(&ar);
        testing_t_fatal_v(t, "block did not define template");
    }
    g = parse_list_node_string((*in_tmpl)->root, a);
    if (!str_eq(g, inner))
        testing_t_errorf_v(t, "inner template = %q, want %q", g, inner);
    arena_free(&ar);
}

static void TestLineNum(TestingT *t) {
    /* const count = 100 */
    enum { count = 3 };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str text = strings_repeat(a, BURROW_S("{{printf 1234}}\n"), count);
    Map *b = builtins(a);
    Error err = BURROW_NO_ERROR;
    ParseTree *tree = parse_tree_parse(parse_new(a, BURROW_S("bench"), (Slice){0}),
                                       text, BURROW_STR_EMPTY, BURROW_STR_EMPTY,
                                       tree_set(a), funcs_of(&b), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    /* Check the line numbers. Each line is an action containing a template,
     * followed by text. That's two nodes per line. */
    Slice nodes = tree->root->nodes;
    for (Int i = 0; i < nodes.len; i += 2) {
        Int line = 1 + i / 2;
        /* Action first. */
        ParseActionNode *action = (ParseActionNode *)BURROW_AT(ParseNode, nodes, i);
        if (action->line != line)
            testing_t_errorf_v(t, "line %d: action is line %d", line, action->line);
        ParsePipeNode *pipe = action->pipe;
        if (pipe->line != line)
            testing_t_errorf_v(t, "line %d: pipe is line %d", line, pipe->line);
    }
    arena_free(&ar);
}

/* ---------------------------------------------------------------- benchmarks */

static void BenchmarkParseLarge(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str text = strings_repeat(a, BURROW_S("{{1234}}\n"), 10000);
    Map *bi = builtins(a);
    for (Int i = 0; i < testing_b_n(b); i++) {
        ArenaMark m = arena_mark(&ar);
        Error err = BURROW_NO_ERROR;
        parse_tree_parse(parse_new(a, BURROW_S("bench"), (Slice){0}), text,
                         BURROW_STR_EMPTY, BURROW_STR_EMPTY, tree_set(a), funcs_of(&bi),
                         &err);
        if (BURROW_FAILED(err)) {
            arena_free(&ar);
            testing_b_fatal_v(b, err);
        }
        arena_release(&ar, m);
    }
    arena_free(&ar);
}

static void BenchmarkVariableString(TestingB *b) {
    static Str ident[] = {
        S_("$"),
        S_("A"),
        S_("BB"),
        S_("CCC"),
        S_("THIS_IS_THE_VARIABLE_BEING_PROCESSED"),
    };
    ParseVariableNode v = {
        .node_type = PARSE_NODE_VARIABLE,
        .ident = {ident, NELEM(ident), NELEM(ident), TYPE_STRING},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str sinkv = BURROW_STR_EMPTY;
    testing_b_reset_timer(b);
    testing_b_report_allocs(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        ArenaMark m = arena_mark(&ar);
        sinkv = parse_variable_node_string(&v, a);
        if (i + 1 < testing_b_n(b))
            arena_release(&ar, m);
    }
    if (sinkv.len == 0) {
        arena_free(&ar);
        testing_b_fatal_v(b, "Benchmark was not run");
    }
    arena_free(&ar);
}

static void BenchmarkListString(TestingB *b) {
    Str text =
        BURROW_S("\n"
                 "{{(printf .Field1.Field2.Field3).Value}}\n"
                 "{{$x := (printf .Field1.Field2.Field3).Value}}\n"
                 "{{$y := (printf $x.Field1.Field2.Field3).Value}}\n"
                 "{{$z := $y.Field1.Field2.Field3}}\n"
                 "{{if contains $y $z}}\n"
                 "\t{{printf \"%q\" $y}}\n"
                 "{{else}}\n"
                 "\t{{printf \"%q\" $x}}\n"
                 "{{end}}\n"
                 "{{with $z.Field1 | contains \"boring\"}}\n"
                 "\t{{printf \"%q\" . | printf \"%s\"}}\n"
                 "{{else}}\n"
                 "\t{{printf \"%d %d %d\" 11 11 11}}\n"
                 "\t{{printf \"%d %d %s\" 22 22 $x.Field1.Field2.Field3 | printf "
                 "\"%s\"}}\n"
                 "\t{{printf \"%v\" (contains $z.Field1.Field2 $y)}}\n"
                 "{{end}}\n");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Map *bi = builtins(a);
    Error err = BURROW_NO_ERROR;
    ParseTree *tree = parse_tree_parse(parse_new(a, BURROW_S("bench"), (Slice){0}),
                                       text, BURROW_STR_EMPTY, BURROW_STR_EMPTY,
                                       tree_set(a), funcs_of(&bi), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_b_fatal_v(b, err);
    }
    Str sinkl = BURROW_STR_EMPTY;
    testing_b_reset_timer(b);
    testing_b_report_allocs(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        ArenaMark m = arena_mark(&ar);
        sinkl = parse_list_node_string(tree->root, a);
        if (i + 1 < testing_b_n(b))
            arena_release(&ar, m);
    }
    if (sinkl.len == 0) {
        arena_free(&ar);
        testing_b_fatal_v(b, "Benchmark was not run");
    }
    arena_free(&ar);
}

/* Go's init: use a small stack limit for testing to avoid creating huge
 * expressions. */
static int parse_main(TestingM *m) {
    burrow__parse_max_stack_depth = 3;
    return testing_m_run(m);
}

#define TESTS(X)                                                                       \
    X(TestLex)                                                                         \
    X(TestDelims)                                                                      \
    X(TestDelimsAlphaNumeric)                                                          \
    X(TestDelimsAndMarkers)                                                            \
    X(TestPos)                                                                         \
    X(TestNumberParse)                                                                 \
    X(TestParse)                                                                       \
    X(TestParseCopy)                                                                   \
    X(TestParseWithComments)                                                           \
    X(TestDelimsStringRoundtrip)                                                       \
    X(TestKeywordsAndFuncs)                                                            \
    X(TestSkipFuncCheck)                                                               \
    X(TestIsEmpty)                                                                     \
    X(TestErrorContextWithTreeCopy)                                                    \
    X(TestErrors)                                                                      \
    X(TestBlock)                                                                       \
    X(TestLineNum)                                                                     \
    X(BenchmarkParseLarge)                                                             \
    X(BenchmarkVariableString)                                                         \
    X(BenchmarkListString)

TESTING_MAIN_WITH(parse_main, TESTS)
