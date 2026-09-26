/* text/scanner, a tokenizer for text that looks like Go.
 *
 * Go's text/scanner. A TextScanner reads UTF-8 text from an IoReader and
 * hands it back either a character at a time with text_scanner_next, or a
 * token at a time with text_scanner_scan, which knows Go's identifiers,
 * numbers, strings, raw strings, character literals and comments. It is not
 * a Go parser. It is the lexer you reach for when you want to read a config
 * format, a small language or a test input that borrows Go's token rules.
 *
 *     StringsReader r;
 *     strings_reader_reset(&r, BURROW_S("x := 3.5 // three and a half"));
 *     TextScanner s;
 *     text_scanner_init(&s, strings_reader_as_io_reader(&r));
 *     Rune tok;
 *     while ((tok = text_scanner_scan(&s)) != TEXT_SCANNER_EOF)
 *         fmt_printf_v("%v: %s\n", s.position.line,
 *                      text_scanner_token_text(&s));
 *     text_scanner_free(&s);
 *
 * text_scanner_scan returns one of the TEXT_SCANNER_ tokens below, which are
 * negative, or the character itself for anything else, so '=' comes back as
 * '='.
 *
 * Errors in the input, such as a string that never ends, do not stop the
 * scan. Each one goes to the error function if there is one and to stderr if
 * there is not, and error_count goes up by one.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package text/scanner */

#ifndef BURROW_TEXT_SCANNER_H
#define BURROW_TEXT_SCANNER_H

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* scanner.Position, a place in the source. It is valid when line is above
 * zero. */
typedef struct TextScannerPosition {
    Str filename; /* filename, if any */
    Int offset;   /* byte offset, starting at 0 */
    Int line;     /* line number, starting at 1 */
    Int column;   /* column number, starting at 1, counted in characters */
} TextScannerPosition;

/* scanner.Position.IsValid. */
bool text_scanner_position_is_valid(const TextScannerPosition *pos);

/* scanner.Position.String. "file:line:column", with "<input>" for the file
 * when there is no name, and no line and column when pos is not valid. */
BURROW_OWNS(ret) Str text_scanner_position_string(TextScannerPosition pos, Alloc *a);

/* The tokens text_scanner_scan returns, as in Go. Anything else it returns is a
 * character. */
enum {
    TEXT_SCANNER_EOF = -1,
    TEXT_SCANNER_IDENT = -2,
    TEXT_SCANNER_INT = -3,
    TEXT_SCANNER_FLOAT = -4,
    TEXT_SCANNER_CHAR = -5,
    TEXT_SCANNER_STRING = -6,
    TEXT_SCANNER_RAW_STRING = -7,
    TEXT_SCANNER_COMMENT = -8,
};

/* The mode bits, which say which tokens text_scanner_scan knows. A token
 * whose bit is off comes back as the characters it is made of. */
enum {
    TEXT_SCANNER_SCAN_IDENTS = 1 << 2,
    TEXT_SCANNER_SCAN_INTS = 1 << 3,
    TEXT_SCANNER_SCAN_FLOATS = 1 << 4, /* includes ints and hexadecimal floats */
    TEXT_SCANNER_SCAN_CHARS = 1 << 5,
    TEXT_SCANNER_SCAN_STRINGS = 1 << 6,
    TEXT_SCANNER_SCAN_RAW_STRINGS = 1 << 7,
    TEXT_SCANNER_SCAN_COMMENTS = 1 << 8,
    /* With TEXT_SCANNER_SCAN_COMMENTS, comments are skipped like white
     * space. */
    TEXT_SCANNER_SKIP_COMMENTS = 1 << 9,
    /* Every Go literal token and Go identifiers, with comments skipped. This
     * is the mode text_scanner_init sets. */
    TEXT_SCANNER_GO_TOKENS = TEXT_SCANNER_SCAN_IDENTS | TEXT_SCANNER_SCAN_FLOATS |
                             TEXT_SCANNER_SCAN_CHARS | TEXT_SCANNER_SCAN_STRINGS |
                             TEXT_SCANNER_SCAN_RAW_STRINGS |
                             TEXT_SCANNER_SCAN_COMMENTS | TEXT_SCANNER_SKIP_COMMENTS,
};

/* scanner.GoWhitespace, the default for the whitespace field: tab, newline,
 * carriage return and space. */
#define TEXT_SCANNER_GO_WHITESPACE                                                     \
    ((uint64_t)1 << '\t' | (uint64_t)1 << '\n' | (uint64_t)1 << '\r' |                 \
     (uint64_t)1 << ' ')

/* scanner.TokenString. The name of a token, such as "Ident", or the character
 * quoted as Go's %q quotes it. */
BURROW_OWNS(ret) Str text_scanner_token_string(Alloc *a, Rune tok);

typedef struct TextScanner TextScanner;

/* The type of the error field. msg is good for the length of the call. */
BURROW_FUNC(TextScannerErrorFunc, void, TextScanner *s, Str msg);

/* The type of the is_ident_rune field. Says whether ch can be the i'th
 * character of an identifier. */
BURROW_FUNC(TextScannerIdentFunc, bool, Rune ch, Int i);

enum { BURROW__TEXT_SCANNER_BUF_LEN = 1024 };

/* scanner.Scanner. The fields from error on are Go's public ones, and can be
 * set after text_scanner_init and changed at any time. The ones before it are
 * the scanner's own.
 *
 * The scanner takes memory from a only for a token that does not fit in its
 * buffer, which a token longer than a kilobyte or so can do. A zeroed a means
 * the heap. */
struct TextScanner {
    Alloc *a;
    IoReader src;

    Byte src_buf[BURROW__TEXT_SCANNER_BUF_LEN + 1]; /* +1 for the sentinel */
    Int src_pos;                                    /* reading position in src_buf */
    Int src_end;                                    /* end of the data in src_buf */

    Int src_buf_offset; /* byte offset of src_buf[0] in the source */
    Int line;           /* line count */
    Int column;         /* character count */
    Int last_line_len;  /* length of the last line in characters */
    Int last_char_len;  /* length of the last character in bytes */

    /* A token's text is usually all in src_buf. When the buffer had to be
     * refilled in the middle of one, its head is in tok_buf and its tail in
     * src_buf. */
    BytesBuffer tok_buf;
    Int tok_pos; /* start of the token's tail in src_buf, or -1 */
    Int tok_end; /* end of it */

    Rune ch; /* the character before src_pos */

    /* Called for each error. When it is nil the error is printed to stderr. */
    TextScannerErrorFunc error;

    /* One more for each error. */
    Int error_count;

    /* Which tokens text_scanner_scan knows, from the TEXT_SCANNER_SCAN_
     * bits. */
    Uint mode;

    /* The characters to skip, a bit for each character up to ' '. The
     * scanner's behaviour for bits above ' ' is undefined, as in Go. */
    uint64_t whitespace;

    /* What an identifier is made of. Nil means Go's identifiers. It must not
     * accept any white space character. */
    TextScannerIdentFunc is_ident_rune;

    /* Where the last token scanned starts. text_scanner_init and
     * text_scanner_next make it invalid. The filename is yours to set, and the
     * scanner never changes it. When an error comes up outside any token
     * this is invalid, and text_scanner_pos says where the scanner is. */
    TextScannerPosition position;
};

/* scanner.Scanner.Init. Sets s up to read from src, with no error function,
 * an error count of zero, TEXT_SCANNER_GO_TOKENS and
 * TEXT_SCANNER_GO_WHITESPACE. Start from a zeroed TextScanner, or one that has
 * been through init before, as in Go. Set the allocator before the first
 * init if it is not the heap. The filename and is_ident_rune are left alone.
 * Returns s. */
BURROW_BORROWS(ret, s) TextScanner *text_scanner_init(TextScanner *s, IoReader src);

/* Gives back what the scanner took from its allocator, and leaves it as
 * text_scanner_init found it. NULL is fine. */
void text_scanner_free(TextScanner *s);

/* scanner.Scanner.Next. The next character, or TEXT_SCANNER_EOF at the end. It
 * does not touch the position field. text_scanner_pos says where s is. */
Rune text_scanner_next(TextScanner *s);

/* scanner.Scanner.Peek. The next character without moving past it. */
Rune text_scanner_peek(TextScanner *s);

/* scanner.Scanner.Scan. The next token or character. See the mode field. */
Rune text_scanner_scan(TextScanner *s);

/* scanner.Scanner.Pos. The position just after the last character or token
 * that text_scanner_next or text_scanner_scan returned. */
TextScannerPosition text_scanner_pos(const TextScanner *s);

/* scanner.Scanner.TokenText. The text of the last token scanned, and empty
 * after text_scanner_next. It points into the scanner and is good until the
 * next call that reads, so copy it to keep it. Go makes a copy for you. */
BURROW_BORROWS(ret, s) Str text_scanner_token_text(TextScanner *s);

/* Position's IsValid and String, which Go promotes to Scanner because the
 * position is embedded. They are about the last token's position, so
 * text_scanner_string is text_scanner_position_string(s->position, a). */
bool text_scanner_is_valid(const TextScanner *s);
BURROW_OWNS(ret) Str text_scanner_string(const TextScanner *s, Alloc *a);

extern const Type *const TYPE_TEXT_SCANNER;

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TEXT_SCANNER_H */
