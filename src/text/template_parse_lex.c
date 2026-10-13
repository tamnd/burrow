/* text/template/parse: the lexer.
 *
 * Go's lexer is a set of state functions, each returning the next. C has no
 * function type that returns itself, so the states are an enum here and
 * next_item runs them in a loop, which is otherwise the same machine.
 *
 * Derived from Go's src/text/template/parse/lex.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/strings.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"
#include "template_parse_internal.h"

#include <string.h>

#define TP_EOF ((Rune) - 1)

/* spaceChars, trimMarker and trimMarkerLen: the marker plus the space before
 * or after it. */
#define TP_SPACE_CHARS BURROW_S(" \t\r\n")
#define TP_TRIM_MARKER '-'
#define TP_TRIM_MARKER_LEN ((ParsePos)2)

#define TP_LEFT_COMMENT BURROW_S("/*")
#define TP_RIGHT_COMMENT BURROW_S("*/")

static Str tp_from(Str s, Int i) {
    return str_from_bytes(s.p + i, s.len - i);
}

static Str tp_slice(Str s, Int lo, Int hi) {
    return str_from_bytes(s.p + lo, hi - lo);
}

static bool tp_is_space(Rune r) {
    return r == ' ' || r == '\t' || r == '\r' || r == '\n';
}

static bool tp_is_alpha_numeric(Rune r) {
    return r == '_' || unicode_is_letter(r) || unicode_is_digit(r);
}

static bool tp_has_left_trim_marker(Str s) {
    return s.len >= 2 && s.p[0] == TP_TRIM_MARKER && tp_is_space((Rune)s.p[1]);
}

static bool tp_has_right_trim_marker(Str s) {
    return s.len >= 2 && tp_is_space((Rune)s.p[0]) && s.p[1] == TP_TRIM_MARKER;
}

/* ------------------------------------------------------------------ items */

static const char *const tp_keyword_names[] = {
    "block", "break", "continue", "define",   "else", "end",
    "if",    "nil",   "range",    "template", "with",
};

static const burrow__ItemType tp_keyword_types[] = {
    BURROW__ITEM_BLOCK,    BURROW__ITEM_BREAK, BURROW__ITEM_CONTINUE,
    BURROW__ITEM_DEFINE,   BURROW__ITEM_ELSE,  BURROW__ITEM_END,
    BURROW__ITEM_IF,       BURROW__ITEM_NIL,   BURROW__ITEM_RANGE,
    BURROW__ITEM_TEMPLATE, BURROW__ITEM_WITH,
};

/* key[word], with "." as dot. Zero, which is an error item, when word is not a
 * keyword, as a missing key reads as zero in Go. */
static burrow__ItemType tp_key(Str word) {
    if (word.len == 1 && word.p[0] == '.')
        return BURROW__ITEM_DOT;
    for (size_t i = 0; i < sizeof tp_keyword_names / sizeof tp_keyword_names[0]; i++)
        if (str_eq(word, str_from_cstr(tp_keyword_names[i])))
            return tp_keyword_types[i];
    return BURROW__ITEM_ERROR;
}

Str burrow__parse_item_string(burrow__Item i, Alloc *a) {
    if (i.typ == BURROW__ITEM_EOF)
        return BURROW_S("EOF");
    if (i.typ == BURROW__ITEM_ERROR)
        return i.val;
    if (i.typ > BURROW__ITEM_KEYWORD)
        return fmt_sprintf_v(a, "<%s>", i.val);
    if (i.val.len > 10)
        return fmt_sprintf_v(a, "%.10q...", i.val);
    return fmt_sprintf_v(a, "%q", i.val);
}

/* ------------------------------------------------------------ the scanner */

typedef enum TpState {
    TP_STOP,
    TP_LEX_TEXT,
    TP_LEX_LEFT_DELIM,
    TP_LEX_COMMENT,
    TP_LEX_RIGHT_DELIM,
    TP_LEX_INSIDE_ACTION,
    TP_LEX_SPACE,
    TP_LEX_IDENTIFIER,
    TP_LEX_FIELD,
    TP_LEX_VARIABLE,
    TP_LEX_CHAR,
    TP_LEX_NUMBER,
    TP_LEX_QUOTE,
    TP_LEX_RAW_QUOTE,
} TpState;

/* next: the next rune in the input, or TP_EOF. */
static Rune tp_next(burrow__Lexer *l) {
    if (l->pos >= l->input.len) {
        l->at_eof = true;
        return TP_EOF;
    }
    Int w = 0;
    Rune r = utf8_decode_rune_in_string(tp_from(l->input, l->pos), &w);
    l->pos += w;
    if (r == '\n')
        l->line++;
    return r;
}

/* backup: steps back one rune. */
static void tp_backup(burrow__Lexer *l) {
    if (!l->at_eof && l->pos > 0) {
        Int w = 0;
        Rune r = utf8_decode_last_rune_in_string(tp_slice(l->input, 0, l->pos), &w);
        l->pos -= w;
        /* Correct newline count. */
        if (r == '\n')
            l->line--;
    }
}

/* peek: the next rune, without taking it. */
static Rune tp_peek(burrow__Lexer *l) {
    Rune r = tp_next(l);
    tp_backup(l);
    return r;
}

/* thisItem: the item of type t that the input from start to pos makes. */
static burrow__Item tp_this_item(burrow__Lexer *l, burrow__ItemType t) {
    burrow__Item i = {t, l->start, tp_slice(l->input, l->start, l->pos), l->start_line};
    l->start = l->pos;
    l->start_line = l->line;
    return i;
}

/* emitItem: hands i to the parser, which ends the run of states. */
static TpState tp_emit_item(burrow__Lexer *l, burrow__Item i) {
    l->item = i;
    return TP_STOP;
}

static TpState tp_emit(burrow__Lexer *l, burrow__ItemType t) {
    return tp_emit_item(l, tp_this_item(l, t));
}

/* ignore: skips the input before this point. */
static void tp_ignore(burrow__Lexer *l) {
    l->line += strings_count(tp_slice(l->input, l->start, l->pos), BURROW_S("\n"));
    l->start = l->pos;
    l->start_line = l->line;
}

/* accept: takes the next rune if it is in valid. */
static bool tp_accept(burrow__Lexer *l, Str valid) {
    if (strings_contains_rune(valid, tp_next(l)))
        return true;
    tp_backup(l);
    return false;
}

/* acceptRun: takes a run of runes from valid. */
static void tp_accept_run(burrow__Lexer *l, Str valid) {
    while (strings_contains_rune(valid, tp_next(l))) {
    }
    tp_backup(l);
}

/* errorf: an error item, and an end to the scan. Clearing the input makes the
 * next call to next_item give EOF, as Go's does. */
static TpState tp_errorf(burrow__Lexer *l, Str msg) {
    l->item = (burrow__Item){BURROW__ITEM_ERROR, l->start, msg, l->start_line};
    l->start = 0;
    l->pos = 0;
    l->input.len = 0;
    return TP_STOP;
}

static TpState tp_error(burrow__Lexer *l, const char *msg) {
    return tp_errorf(l, str_from_cstr(msg));
}

/* rightTrimLength: how much space s ends with. */
static ParsePos tp_right_trim_length(Str s) {
    return s.len - strings_trim_right(s, TP_SPACE_CHARS).len;
}

/* leftTrimLength: how much space s starts with. */
static ParsePos tp_left_trim_length(Str s) {
    return s.len - strings_trim_left(s, TP_SPACE_CHARS).len;
}

/* atRightDelim: whether the input is at the right delimiter, possibly with a
 * trim marker before it. */
static bool tp_at_right_delim(burrow__Lexer *l, bool *trim_spaces) {
    if (tp_has_right_trim_marker(tp_from(l->input, l->pos)) &&
        l->pos + TP_TRIM_MARKER_LEN <= l->input.len &&
        strings_has_prefix(tp_from(l->input, l->pos + TP_TRIM_MARKER_LEN),
                           l->right_delim)) { /* With trim marker. */
        *trim_spaces = true;
        return true;
    }
    *trim_spaces = false;
    /* Without trim marker. */
    return strings_has_prefix(tp_from(l->input, l->pos), l->right_delim);
}

/* atTerminator: whether the next rune can follow an identifier. */
static bool tp_at_terminator(burrow__Lexer *l) {
    Rune r = tp_peek(l);
    if (tp_is_space(r))
        return true;
    switch (r) {
    case TP_EOF:
    case '.':
    case ',':
    case '|':
    case ':':
    case ')':
    case '(':
        return true;
    default:
        break;
    }
    return strings_has_prefix(tp_from(l->input, l->pos), l->right_delim);
}

/* lexText: scans until an opening action delimiter. */
static TpState tp_lex_text(burrow__Lexer *l) {
    Int x = strings_index(tp_from(l->input, l->pos), l->left_delim);
    if (x >= 0) {
        if (x > 0) {
            l->pos += x;
            /* Do we trim any trailing space? */
            ParsePos trim_length = 0;
            ParsePos delim_end = l->pos + l->left_delim.len;
            if (tp_has_left_trim_marker(tp_from(l->input, delim_end)))
                trim_length =
                    tp_right_trim_length(tp_slice(l->input, l->start, l->pos));
            l->pos -= trim_length;
            l->line +=
                strings_count(tp_slice(l->input, l->start, l->pos), BURROW_S("\n"));
            burrow__Item i = tp_this_item(l, BURROW__ITEM_TEXT);
            l->pos += trim_length;
            tp_ignore(l);
            if (i.val.len > 0)
                return tp_emit_item(l, i);
        }
        return TP_LEX_LEFT_DELIM;
    }
    l->pos = l->input.len;
    /* Correctly reached EOF. */
    if (l->pos > l->start) {
        l->line += strings_count(tp_slice(l->input, l->start, l->pos), BURROW_S("\n"));
        return tp_emit(l, BURROW__ITEM_TEXT);
    }
    return tp_emit(l, BURROW__ITEM_EOF);
}

/* lexLeftDelim: scans the left delimiter, which is known to be present,
 * possibly with a trim marker. A comment is lexed as a whole here. */
static TpState tp_lex_left_delim(burrow__Lexer *l) {
    l->pos += l->left_delim.len;
    bool trim_space = tp_has_left_trim_marker(tp_from(l->input, l->pos));
    ParsePos after_marker = trim_space ? TP_TRIM_MARKER_LEN : 0;
    if (strings_has_prefix(tp_from(l->input, l->pos + after_marker), TP_LEFT_COMMENT)) {
        l->pos += after_marker;
        tp_ignore(l);
        return TP_LEX_COMMENT;
    }
    burrow__Item i = tp_this_item(l, BURROW__ITEM_LEFT_DELIM);
    l->inside_action = true;
    l->pos += after_marker;
    tp_ignore(l);
    l->paren_depth = 0;
    return tp_emit_item(l, i);
}

/* lexComment: scans a comment. The left comment marker is known to be
 * present. */
static TpState tp_lex_comment(burrow__Lexer *l) {
    l->pos += TP_LEFT_COMMENT.len;
    Int x = strings_index(tp_from(l->input, l->pos), TP_RIGHT_COMMENT);
    if (x < 0)
        return tp_error(l, "unclosed comment");
    l->pos += x + TP_RIGHT_COMMENT.len;
    bool trim_space = false;
    if (!tp_at_right_delim(l, &trim_space))
        return tp_error(l, "comment ends before closing delimiter");
    l->line += strings_count(tp_slice(l->input, l->start, l->pos), BURROW_S("\n"));
    burrow__Item i = tp_this_item(l, BURROW__ITEM_COMMENT);
    if (trim_space)
        l->pos += TP_TRIM_MARKER_LEN;
    l->pos += l->right_delim.len;
    if (trim_space)
        l->pos += tp_left_trim_length(tp_from(l->input, l->pos));
    tp_ignore(l);
    if (l->options.emit_comment)
        return tp_emit_item(l, i);
    return TP_LEX_TEXT;
}

/* lexRightDelim: scans the right delimiter, which is known to be present,
 * possibly with a trim marker. */
static TpState tp_lex_right_delim(burrow__Lexer *l) {
    bool trim_space = false;
    tp_at_right_delim(l, &trim_space);
    if (trim_space) {
        l->pos += TP_TRIM_MARKER_LEN;
        tp_ignore(l);
    }
    l->pos += l->right_delim.len;
    burrow__Item i = tp_this_item(l, BURROW__ITEM_RIGHT_DELIM);
    if (trim_space) {
        l->pos += tp_left_trim_length(tp_from(l->input, l->pos));
        tp_ignore(l);
    }
    l->inside_action = false;
    return tp_emit_item(l, i);
}

/* lexInsideAction: scans the elements inside action delimiters. */
static TpState tp_lex_inside_action(burrow__Lexer *l) {
    /* Either number, quoted string, or identifier. Spaces separate arguments;
     * runs of spaces turn into itemSpace. Pipe symbols separate and are
     * emitted. */
    bool trim = false;
    if (tp_at_right_delim(l, &trim)) {
        if (l->paren_depth == 0)
            return TP_LEX_RIGHT_DELIM;
        return tp_error(l, "unclosed left paren");
    }
    Rune r = tp_next(l);
    if (r == TP_EOF)
        return tp_error(l, "unclosed action");
    if (tp_is_space(r)) {
        tp_backup(l); /* Put space back in case we have " -}}". */
        return TP_LEX_SPACE;
    }
    switch (r) {
    case '=':
        return tp_emit(l, BURROW__ITEM_ASSIGN);
    case ':':
        if (tp_next(l) != '=')
            return tp_error(l, "expected :=");
        return tp_emit(l, BURROW__ITEM_DECLARE);
    case '|':
        return tp_emit(l, BURROW__ITEM_PIPE);
    case '"':
        return TP_LEX_QUOTE;
    case '`':
        return TP_LEX_RAW_QUOTE;
    case '$':
        return TP_LEX_VARIABLE;
    case '\'':
        return TP_LEX_CHAR;
    case '.':
        /* Special look-ahead for ".field" so we don't break l.backup(). */
        if (l->pos < l->input.len) {
            Byte c = l->input.p[l->pos];
            if (c < '0' || '9' < c)
                return TP_LEX_FIELD;
        }
        /* '.' can start a number. */
        tp_backup(l);
        return TP_LEX_NUMBER;
    case '(':
        l->paren_depth++;
        return tp_emit(l, BURROW__ITEM_LEFT_PAREN);
    case ')':
        l->paren_depth--;
        if (l->paren_depth < 0)
            return tp_error(l, "unexpected right paren");
        return tp_emit(l, BURROW__ITEM_RIGHT_PAREN);
    default:
        break;
    }
    if (r == '+' || r == '-' || ('0' <= r && r <= '9')) {
        tp_backup(l);
        return TP_LEX_NUMBER;
    }
    if (tp_is_alpha_numeric(r)) {
        tp_backup(l);
        return TP_LEX_IDENTIFIER;
    }
    if (r <= UNICODE_MAX_ASCII && unicode_is_print(r))
        return tp_emit(l, BURROW__ITEM_CHAR);
    return tp_errorf(l, fmt_sprintf_v(error_allocator(),
                                      "unrecognized character in action: %#U", r));
}

/* lexSpace: scans a run of space characters. One space has already been seen,
 * and it might be the start of a trim marker before the right delimiter. */
static TpState tp_lex_space(burrow__Lexer *l) {
    Int num_spaces = 0;
    for (;;) {
        Rune r = tp_peek(l);
        if (!tp_is_space(r))
            break;
        tp_next(l);
        num_spaces++;
    }
    /* Be careful about a trim-marked closing delimiter, which has a minus after
     * a space. We know there is a space, so check for the '-' that might
     * follow. */
    if (tp_has_right_trim_marker(tp_from(l->input, l->pos - 1)) &&
        strings_has_prefix(tp_from(l->input, l->pos - 1 + TP_TRIM_MARKER_LEN),
                           l->right_delim)) {
        tp_backup(l); /* Before the space. */
        if (num_spaces == 1)
            return TP_LEX_RIGHT_DELIM; /* On the delim, so go right to that. */
    }
    return tp_emit(l, BURROW__ITEM_SPACE);
}

/* lexIdentifier: scans an alphanumeric word. */
static TpState tp_lex_identifier(burrow__Lexer *l) {
    for (;;) {
        Rune r = tp_next(l);
        if (tp_is_alpha_numeric(r))
            continue;
        tp_backup(l);
        Str word = tp_slice(l->input, l->start, l->pos);
        if (!tp_at_terminator(l))
            return tp_errorf(l,
                             fmt_sprintf_v(error_allocator(), "bad character %#U", r));
        burrow__ItemType item = tp_key(word);
        if (item > BURROW__ITEM_KEYWORD) {
            if ((item == BURROW__ITEM_BREAK && !l->options.break_ok) ||
                (item == BURROW__ITEM_CONTINUE && !l->options.continue_ok))
                return tp_emit(l, BURROW__ITEM_IDENTIFIER);
            return tp_emit(l, item);
        }
        if (word.p[0] == '.')
            return tp_emit(l, BURROW__ITEM_FIELD);
        if (str_eq(word, BURROW_S("true")) || str_eq(word, BURROW_S("false")))
            return tp_emit(l, BURROW__ITEM_BOOL);
        return tp_emit(l, BURROW__ITEM_IDENTIFIER);
    }
}

/* lexFieldOrVariable: scans a field or variable, [.$]Alphanumeric. The . or $
 * has been scanned. */
static TpState tp_lex_field_or_variable(burrow__Lexer *l, burrow__ItemType typ) {
    if (tp_at_terminator(l)) { /* Nothing interesting follows -> "." or "$". */
        if (typ == BURROW__ITEM_VARIABLE)
            return tp_emit(l, BURROW__ITEM_VARIABLE);
        return tp_emit(l, BURROW__ITEM_DOT);
    }
    Rune r;
    for (;;) {
        r = tp_next(l);
        if (!tp_is_alpha_numeric(r)) {
            tp_backup(l);
            break;
        }
    }
    if (!tp_at_terminator(l))
        return tp_errorf(l, fmt_sprintf_v(error_allocator(), "bad character %#U", r));
    return tp_emit(l, typ);
}

/* lexVariable: scans a variable, $Alphanumeric. The $ has been scanned. */
static TpState tp_lex_variable(burrow__Lexer *l) {
    if (tp_at_terminator(l)) /* Nothing interesting follows -> "$". */
        return tp_emit(l, BURROW__ITEM_VARIABLE);
    return tp_lex_field_or_variable(l, BURROW__ITEM_VARIABLE);
}

/* The body of a quoted constant up to the closing quote, which lexChar and
 * lexQuote share. False on an unterminated one. */
static bool tp_scan_quoted(burrow__Lexer *l, Rune quote) {
    for (;;) {
        Rune r = tp_next(l);
        if (r == '\\') {
            r = tp_next(l);
            if (r != TP_EOF && r != '\n')
                continue;
            return false;
        }
        if (r == TP_EOF || r == '\n')
            return false;
        if (r == quote)
            return true;
    }
}

/* lexChar: scans a character constant. The initial quote is already scanned.
 * Syntax checking is done by the parser. */
static TpState tp_lex_char(burrow__Lexer *l) {
    if (!tp_scan_quoted(l, '\''))
        return tp_error(l, "unterminated character constant");
    return tp_emit(l, BURROW__ITEM_CHAR_CONSTANT);
}

/* scanNumber: takes a number, which the parser checks properly. */
static bool tp_scan_number(burrow__Lexer *l) {
    /* Optional leading sign. */
    tp_accept(l, BURROW_S("+-"));
    /* Is it hex? */
    Str digits = BURROW_S("0123456789_");
    if (tp_accept(l, BURROW_S("0"))) {
        /* Note: Leading 0 does not mean octal in floats. */
        if (tp_accept(l, BURROW_S("xX")))
            digits = BURROW_S("0123456789abcdefABCDEF_");
        else if (tp_accept(l, BURROW_S("oO")))
            digits = BURROW_S("01234567_");
        else if (tp_accept(l, BURROW_S("bB")))
            digits = BURROW_S("01_");
    }
    tp_accept_run(l, digits);
    if (tp_accept(l, BURROW_S(".")))
        tp_accept_run(l, digits);
    if (digits.len == 10 + 1 && tp_accept(l, BURROW_S("eE"))) {
        tp_accept(l, BURROW_S("+-"));
        tp_accept_run(l, BURROW_S("0123456789_"));
    }
    if (digits.len == 16 + 6 + 1 && tp_accept(l, BURROW_S("pP"))) {
        tp_accept(l, BURROW_S("+-"));
        tp_accept_run(l, BURROW_S("0123456789_"));
    }
    /* Is it imaginary? */
    tp_accept(l, BURROW_S("i"));
    /* Next thing mustn't be alphanumeric. */
    if (tp_is_alpha_numeric(tp_peek(l))) {
        tp_next(l);
        return false;
    }
    return true;
}

/* lexNumber: scans a number: decimal, octal, hex, float, or imaginary. This
 * isn't a perfect number scanner, for instance it accepts "." and "0x0.2" and
 * "089", but when it's wrong the input is invalid and the parser (via strconv)
 * will notice. */
static TpState tp_lex_number(burrow__Lexer *l) {
    if (!tp_scan_number(l))
        return tp_errorf(l, fmt_sprintf_v(error_allocator(), "bad number syntax: %q",
                                          tp_slice(l->input, l->start, l->pos)));
    Rune sign = tp_peek(l);
    if (sign == '+' || sign == '-') {
        /* Complex: 1+2i. No spaces, must end in 'i'. */
        if (!tp_scan_number(l) || l->input.p[l->pos - 1] != 'i')
            return tp_errorf(l,
                             fmt_sprintf_v(error_allocator(), "bad number syntax: %q",
                                           tp_slice(l->input, l->start, l->pos)));
        return tp_emit(l, BURROW__ITEM_COMPLEX);
    }
    return tp_emit(l, BURROW__ITEM_NUMBER);
}

/* lexQuote: scans a quoted string. */
static TpState tp_lex_quote(burrow__Lexer *l) {
    if (!tp_scan_quoted(l, '"'))
        return tp_error(l, "unterminated quoted string");
    return tp_emit(l, BURROW__ITEM_STRING);
}

/* lexRawQuote: scans a raw quoted string. */
static TpState tp_lex_raw_quote(burrow__Lexer *l) {
    for (;;) {
        Rune r = tp_next(l);
        if (r == TP_EOF)
            return tp_error(l, "unterminated raw quoted string");
        if (r == '`')
            break;
    }
    return tp_emit(l, BURROW__ITEM_RAW_STRING);
}

static TpState tp_step(burrow__Lexer *l, TpState s) {
    switch (s) {
    case TP_LEX_TEXT:
        return tp_lex_text(l);
    case TP_LEX_LEFT_DELIM:
        return tp_lex_left_delim(l);
    case TP_LEX_COMMENT:
        return tp_lex_comment(l);
    case TP_LEX_RIGHT_DELIM:
        return tp_lex_right_delim(l);
    case TP_LEX_INSIDE_ACTION:
        return tp_lex_inside_action(l);
    case TP_LEX_SPACE:
        return tp_lex_space(l);
    case TP_LEX_IDENTIFIER:
        return tp_lex_identifier(l);
    case TP_LEX_FIELD:
        return tp_lex_field_or_variable(l, BURROW__ITEM_FIELD);
    case TP_LEX_VARIABLE:
        return tp_lex_variable(l);
    case TP_LEX_CHAR:
        return tp_lex_char(l);
    case TP_LEX_NUMBER:
        return tp_lex_number(l);
    case TP_LEX_QUOTE:
        return tp_lex_quote(l);
    case TP_LEX_RAW_QUOTE:
        return tp_lex_raw_quote(l);
    case TP_STOP:
    default:
        return TP_STOP;
    }
}

burrow__Item burrow__parse_lex_next_item(burrow__Lexer *l) {
    l->item = (burrow__Item){BURROW__ITEM_EOF, l->pos, BURROW_S("EOF"), l->start_line};
    TpState state = l->inside_action ? TP_LEX_INSIDE_ACTION : TP_LEX_TEXT;
    while ((state = tp_step(l, state)) != TP_STOP) {
    }
    return l->item;
}

void burrow__parse_lex(burrow__Lexer *l, Str name, Str input, Str left, Str right) {
    if (left.len == 0)
        left = BURROW_S("{{");
    if (right.len == 0)
        right = BURROW_S("}}");
    memset(l, 0, sizeof *l);
    l->name = name;
    l->input = input;
    l->left_delim = left;
    l->right_delim = right;
    l->line = 1;
    l->start_line = 1;
    l->inside_action = false;
}
