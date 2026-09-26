/* Derived from Go's src/text/scanner/scanner.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/text/scanner.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const Type text_scanner_desc = {
    {(const Byte *)"Scanner", 7},
    {(const Byte *)"text/scanner", 12},
    KIND_STRUCT,
    (uint32_t)sizeof(TextScanner),
    (uint16_t)_Alignof(TextScanner),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7363616eU, /* "scan" */
    NULL,
};

const Type *const TYPE_TEXT_SCANNER = &text_scanner_desc;

enum { sc_buf_len = BURROW__TEXT_SCANNER_BUF_LEN, sc_skip_comment = -9 };

bool text_scanner_position_is_valid(const TextScannerPosition *pos) {
    return pos->line > 0;
}

Str text_scanner_position_string(TextScannerPosition pos, Alloc *a) {
    Str s = pos.filename;
    if (s.len == 0)
        s = BURROW_S("<input>");
    if (text_scanner_position_is_valid(&pos))
        return fmt_sprintf_v(a, "%s:%d:%d", s, pos.line, pos.column);
    return fmt_sprintf_v(a, "%s", s);
}

bool text_scanner_is_valid(const TextScanner *s) {
    return text_scanner_position_is_valid(&s->position);
}

Str text_scanner_string(const TextScanner *s, Alloc *a) {
    return text_scanner_position_string(s->position, a);
}

Str text_scanner_token_string(Alloc *a, Rune tok) {
    static const char *const names[] = {
        "EOF", "Ident", "Int", "Float", "Char", "String", "RawString", "Comment",
    };
    if (tok <= TEXT_SCANNER_EOF && tok >= TEXT_SCANNER_COMMENT)
        return fmt_sprintf_v(a, "%s", str_from_cstr(names[-tok - 1]));
    /* string(tok) turns anything that is not a valid rune into U+FFFD. */
    if (!utf8_valid_rune(tok))
        tok = UTF8_RUNE_ERROR;
    Byte buf[UTF8_UTF_MAX];
    Int n = utf8_encode_rune((Slice){buf, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE}, tok);
    return strconv_quote(a, str_from_bytes(buf, n));
}

static Alloc *sc_alloc(TextScanner *s) {
    if (s->a == NULL)
        s->a = heap_allocator();
    return s->a;
}

TextScanner *text_scanner_init(TextScanner *s, IoReader src) {
    s->src = src;

    /* Initialize the source buffer. The first call to next fills it by
     * calling src.Read. */
    s->src_buf[0] = (Byte)UTF8_RUNE_SELF; /* sentinel */
    s->src_pos = 0;
    s->src_end = 0;

    /* Initialize the source position. */
    s->src_buf_offset = 0;
    s->line = 1;
    s->column = 0;
    s->last_line_len = 0;
    s->last_char_len = 0;

    /* Initialize the token text buffer, which the first call to next needs. */
    if (s->tok_buf.a == NULL)
        s->tok_buf = BYTES_BUFFER(sc_alloc(s));
    bytes_buffer_reset(&s->tok_buf);
    s->tok_pos = -1;

    /* Initialize the one character look-ahead: no char read yet, not EOF. */
    s->ch = -2;

    /* Initialize the public fields. */
    s->error = (TextScannerErrorFunc){NULL, NULL};
    s->error_count = 0;
    s->mode = TEXT_SCANNER_GO_TOKENS;
    s->whitespace = TEXT_SCANNER_GO_WHITESPACE;
    s->position.line = 0; /* invalidate the token position */

    return s;
}

void text_scanner_free(TextScanner *s) {
    if (s == NULL)
        return;
    bytes_buffer_free(&s->tok_buf);
    s->tok_buf = BYTES_BUFFER(s->a);
}

static void sc_error(TextScanner *s, Str msg) {
    s->tok_end = s->src_pos - s->last_char_len; /* make sure token text is terminated */
    s->error_count++;
    if (!BURROW_FUNC_IS_NIL(s->error)) {
        BURROW_CALLF(s->error, s, msg);
        return;
    }
    TextScannerPosition pos = s->position;
    if (!text_scanner_position_is_valid(&pos))
        pos = text_scanner_pos(s);
    Alloc *a = sc_alloc(s);
    Str p = text_scanner_position_string(pos, a);
    fprintf(stderr, "%.*s: %.*s\n", (int)p.len, (const char *)p.p, (int)msg.len,
            (const char *)msg.p);
    mem_free(a, (void *)(uintptr_t)p.p, (size_t)p.len, 1);
}

/* errorf for a rune: the message is format with %q of ch in it. */
static void sc_errorf_rune(TextScanner *s, const char *format, Rune ch) {
    Alloc *a = sc_alloc(s);
    Str msg = fmt_sprintf_v(a, format, ch);
    sc_error(s, msg);
    mem_free(a, (void *)(uintptr_t)msg.p, (size_t)msg.len, 1);
}

/* The token's head, saved from src_buf before a refill. Go's bytes.Buffer
 * cannot fail. This one can run out of memory, and then the head is lost and
 * the scanner says so through the usual error path. */
static void sc_save_token(TextScanner *s, Int from, Int to) {
    Error err = BURROW_NO_ERROR;
    bytes_buffer_write(
        &s->tok_buf, (Slice){s->src_buf + from, to - from, to - from, TYPE_BYTE}, &err);
    if (BURROW_FAILED(err))
        sc_error(s, error_text(err));
}

/* next reads and returns the next Unicode character. It is designed such that
 * only a minimal amount of work needs to be done in the common ASCII case (one
 * test to check for both ASCII and end-of-buffer, and one test to check for
 * newlines). */
static Rune sc_next(TextScanner *s) {
    Rune ch = s->src_buf[s->src_pos];
    Int width = 1;

    if (ch >= UTF8_RUNE_SELF) {
        /* Uncommon case: not ASCII or not enough bytes. */
        while (s->src_pos + UTF8_UTF_MAX > s->src_end &&
               !utf8_full_rune((Slice){s->src_buf + s->src_pos, s->src_end - s->src_pos,
                                       s->src_end - s->src_pos, TYPE_BYTE})) {
            /* Not enough bytes: read some more, but first save away token
             * text if any. */
            if (s->tok_pos >= 0) {
                sc_save_token(s, s->tok_pos, s->src_pos);
                s->tok_pos = 0;
                /* s->tok_end is set by Scan. */
            }
            /* Move unread bytes to the beginning of the buffer. */
            memmove(s->src_buf, s->src_buf + s->src_pos,
                    (size_t)(s->src_end - s->src_pos));
            s->src_buf_offset += s->src_pos;
            /* Read more bytes. An IoReader must return io_eof when it reaches
             * the end of what it is reading. Simply returning 0 makes this
             * loop retry forever, but then the error is in the reader. */
            Int i = s->src_end - s->src_pos;
            Error err = BURROW_NO_ERROR;
            Int n = BURROW_CALL(
                s->src, read,
                (Slice){s->src_buf + i, sc_buf_len - i, sc_buf_len - i, TYPE_BYTE},
                &err);
            s->src_pos = 0;
            s->src_end = i + n;
            s->src_buf[s->src_end] = (Byte)UTF8_RUNE_SELF; /* sentinel */
            if (BURROW_FAILED(err)) {
                if (err.vt != io_eof.vt || err.data != io_eof.data)
                    sc_error(s, error_text(err));
                if (s->src_end == 0) {
                    if (s->last_char_len > 0) {
                        /* The previous character was not EOF. */
                        s->column++;
                    }
                    s->last_char_len = 0;
                    return TEXT_SCANNER_EOF;
                }
                /* If err is EOF, we won't be getting more bytes; break to
                 * avoid an infinite loop. If err is something else, we don't
                 * know if we can get more bytes; thus also break. */
                break;
            }
        }
        /* At least one byte. */
        ch = s->src_buf[s->src_pos];
        if (ch >= UTF8_RUNE_SELF) {
            /* Uncommon case: not ASCII. */
            ch = utf8_decode_rune((Slice){s->src_buf + s->src_pos,
                                          s->src_end - s->src_pos,
                                          s->src_end - s->src_pos, TYPE_BYTE},
                                  &width);
            if (ch == UTF8_RUNE_ERROR && width == 1) {
                /* Advance for the correct error position. */
                s->src_pos += width;
                s->last_char_len = width;
                s->column++;
                sc_error(s, BURROW_S("invalid UTF-8 encoding"));
                return ch;
            }
        }
    }

    /* Advance. */
    s->src_pos += width;
    s->last_char_len = width;
    s->column++;

    /* Special situations. */
    switch (ch) {
    case 0:
        /* For compatibility with other tools. */
        sc_error(s, BURROW_S("invalid character NUL"));
        break;
    case '\n':
        s->line++;
        s->last_line_len = s->column;
        s->column = 0;
        break;
    default:
        break;
    }

    return ch;
}

Rune text_scanner_next(TextScanner *s) {
    s->tok_pos = -1;      /* don't collect token text */
    s->position.line = 0; /* invalidate the token position */
    Rune ch = text_scanner_peek(s);
    if (ch != TEXT_SCANNER_EOF)
        s->ch = sc_next(s);
    return ch;
}

Rune text_scanner_peek(TextScanner *s) {
    if (s->ch == -2) {
        /* This code is only run for the very first character. */
        s->ch = sc_next(s);
        if (s->ch == 0xFEFF)
            s->ch = sc_next(s); /* ignore the BOM */
    }
    return s->ch;
}

static bool sc_is_ident_rune(TextScanner *s, Rune ch, Int i) {
    if (!BURROW_FUNC_IS_NIL(s->is_ident_rune))
        return ch != TEXT_SCANNER_EOF && BURROW_CALLF(s->is_ident_rune, ch, i);
    return ch == '_' || unicode_is_letter(ch) || (unicode_is_digit(ch) && i > 0);
}

static Rune sc_scan_identifier(TextScanner *s) {
    /* We know the zero'th rune is OK; start scanning at the next one. */
    Rune ch = sc_next(s);
    for (Int i = 1; sc_is_ident_rune(s, ch, i); i++)
        ch = sc_next(s);
    return ch;
}

/* lower returns the lower-case ch iff ch is an ASCII letter. */
static Rune sc_lower(Rune ch) {
    return ('a' - 'A') | ch;
}
static bool sc_is_decimal(Rune ch) {
    return '0' <= ch && ch <= '9';
}
static bool sc_is_hex(Rune ch) {
    return ('0' <= ch && ch <= '9') || ('a' <= sc_lower(ch) && sc_lower(ch) <= 'f');
}

/* digits accepts the sequence { digit | '_' } starting with ch0. If base <=
 * 10, digits accepts any decimal digit but records the first invalid digit >=
 * base in *invalid if *invalid == 0. digits returns the first rune that is
 * not part of the sequence anymore, and sets *digsep to a bitset describing
 * whether the sequence contained digits (bit 0 is set), or separators '_'
 * (bit 1 is set). */
static Rune sc_digits(TextScanner *s, Rune ch0, int base, Rune *invalid, int *digsep) {
    Rune ch = ch0;
    *digsep = 0;
    if (base <= 10) {
        Rune max = '0' + base;
        while (sc_is_decimal(ch) || ch == '_') {
            int ds = 1;
            if (ch == '_')
                ds = 2;
            else if (ch >= max && *invalid == 0)
                *invalid = ch;
            *digsep |= ds;
            ch = sc_next(s);
        }
    } else {
        while (sc_is_hex(ch) || ch == '_') {
            int ds = 1;
            if (ch == '_')
                ds = 2;
            *digsep |= ds;
            ch = sc_next(s);
        }
    }
    return ch;
}

static const char *sc_litname(Rune prefix) {
    switch (prefix) {
    case 'x':
        return "hexadecimal literal";
    case 'o':
    case '0':
        return "octal literal";
    case 'b':
        return "binary literal";
    default:
        return "decimal literal";
    }
}

/* invalidSep returns the index of the first invalid separator in x, or -1. */
static Int sc_invalid_sep(Str x) {
    Rune x1 = ' '; /* prefix char, we only care if it's 'x' */
    Rune d = '.';  /* digit, one of '_', '0' (a digit), or '.' (anything else) */
    Int i = 0;

    /* A prefix counts as a digit. */
    if (x.len >= 2 && x.p[0] == '0') {
        x1 = sc_lower(x.p[1]);
        if (x1 == 'x' || x1 == 'o' || x1 == 'b') {
            d = '0';
            i = 2;
        }
    }

    /* Mantissa and exponent. */
    for (; i < x.len; i++) {
        Rune p = d; /* previous digit */
        d = x.p[i];
        if (d == '_') {
            if (p != '0')
                return i;
        } else if (sc_is_decimal(d) || (x1 == 'x' && sc_is_hex(d))) {
            d = '0';
        } else {
            if (p == '_')
                return i - 1;
            d = '.';
        }
    }
    if (d == '_')
        return x.len - 1;

    return -1;
}

static void sc_error_lit(TextScanner *s, const char *before, Rune prefix,
                         const char *after) {
    Alloc *a = sc_alloc(s);
    Str msg = fmt_sprintf_v(a, "%s%s%s", str_from_cstr(before),
                            str_from_cstr(sc_litname(prefix)), str_from_cstr(after));
    sc_error(s, msg);
    mem_free(a, (void *)(uintptr_t)msg.p, (size_t)msg.len, 1);
}

static Rune sc_scan_number(TextScanner *s, Rune ch, bool seen_dot, Rune *next) {
    int base = 10;    /* number base */
    Rune prefix = 0;  /* one of 0 (decimal), '0' (0-octal), 'x', 'o', or 'b' */
    int digsep = 0;   /* bit 0: digit present, bit 1: '_' present */
    Rune invalid = 0; /* invalid digit in literal, or 0 */

    /* Integer part. */
    Rune tok = 0;
    int ds = 0;
    if (!seen_dot) {
        tok = TEXT_SCANNER_INT;
        if (ch == '0') {
            ch = sc_next(s);
            switch (sc_lower(ch)) {
            case 'x':
                ch = sc_next(s);
                base = 16;
                prefix = 'x';
                break;
            case 'o':
                ch = sc_next(s);
                base = 8;
                prefix = 'o';
                break;
            case 'b':
                ch = sc_next(s);
                base = 2;
                prefix = 'b';
                break;
            default:
                base = 8;
                prefix = '0';
                digsep = 1; /* leading 0 */
                break;
            }
        }
        ch = sc_digits(s, ch, base, &invalid, &ds);
        digsep |= ds;
        if (ch == '.' && (s->mode & TEXT_SCANNER_SCAN_FLOATS) != 0) {
            ch = sc_next(s);
            seen_dot = true;
        }
    }

    /* Fractional part. */
    if (seen_dot) {
        tok = TEXT_SCANNER_FLOAT;
        if (prefix == 'o' || prefix == 'b')
            sc_error_lit(s, "invalid radix point in ", prefix, "");
        ch = sc_digits(s, ch, base, &invalid, &ds);
        digsep |= ds;
    }

    if ((digsep & 1) == 0)
        sc_error_lit(s, "", prefix, " has no digits");

    /* Exponent. */
    Rune e = sc_lower(ch);
    if ((e == 'e' || e == 'p') && (s->mode & TEXT_SCANNER_SCAN_FLOATS) != 0) {
        if (e == 'e' && prefix != 0 && prefix != '0')
            sc_errorf_rune(s, "%q exponent requires decimal mantissa", ch);
        else if (e == 'p' && prefix != 'x')
            sc_errorf_rune(s, "%q exponent requires hexadecimal mantissa", ch);
        ch = sc_next(s);
        tok = TEXT_SCANNER_FLOAT;
        if (ch == '+' || ch == '-')
            ch = sc_next(s);
        Rune unused = 0;
        ch = sc_digits(s, ch, 10, &unused, &ds);
        digsep |= ds;
        if ((ds & 1) == 0)
            sc_error(s, BURROW_S("exponent has no digits"));
    } else if (prefix == 'x' && tok == TEXT_SCANNER_FLOAT) {
        sc_error(s, BURROW_S("hexadecimal mantissa requires a 'p' exponent"));
    }

    if (tok == TEXT_SCANNER_INT && invalid != 0) {
        Alloc *a = sc_alloc(s);
        Str msg = fmt_sprintf_v(a, "invalid digit %q in %s", invalid,
                                str_from_cstr(sc_litname(prefix)));
        sc_error(s, msg);
        mem_free(a, (void *)(uintptr_t)msg.p, (size_t)msg.len, 1);
    }

    if ((digsep & 2) != 0) {
        s->tok_end =
            s->src_pos - s->last_char_len; /* make sure token text is terminated */
        if (sc_invalid_sep(text_scanner_token_text(s)) >= 0)
            sc_error(s, BURROW_S("'_' must separate successive digits"));
    }

    *next = ch;
    return tok;
}

static int sc_digit_val(Rune ch) {
    if ('0' <= ch && ch <= '9')
        return (int)(ch - '0');
    if ('a' <= sc_lower(ch) && sc_lower(ch) <= 'f')
        return sc_lower(ch) - 'a' + 10;
    return 16; /* larger than any legal digit val */
}

static Rune sc_scan_digits(TextScanner *s, Rune ch, int base, int n) {
    while (n > 0 && sc_digit_val(ch) < base) {
        ch = sc_next(s);
        n--;
    }
    if (n > 0)
        sc_error(s, BURROW_S("invalid char escape"));
    return ch;
}

static Rune sc_scan_escape(TextScanner *s, Rune quote) {
    Rune ch = sc_next(s); /* read the character after '/' */
    switch (ch) {
    case 'a':
    case 'b':
    case 'f':
    case 'n':
    case 'r':
    case 't':
    case 'v':
    case '\\':
        /* Nothing to do. */
        ch = sc_next(s);
        break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
        ch = sc_scan_digits(s, ch, 8, 3);
        break;
    case 'x':
        ch = sc_scan_digits(s, sc_next(s), 16, 2);
        break;
    case 'u':
        ch = sc_scan_digits(s, sc_next(s), 16, 4);
        break;
    case 'U':
        ch = sc_scan_digits(s, sc_next(s), 16, 8);
        break;
    default:
        if (ch == quote) {
            ch = sc_next(s);
            break;
        }
        sc_error(s, BURROW_S("invalid char escape"));
        break;
    }
    return ch;
}

static Int sc_scan_string(TextScanner *s, Rune quote) {
    Int n = 0;
    Rune ch = sc_next(s); /* read the character after the quote */
    while (ch != quote) {
        if (ch == '\n' || ch < 0) {
            sc_error(s, BURROW_S("literal not terminated"));
            return n;
        }
        if (ch == '\\')
            ch = sc_scan_escape(s, quote);
        else
            ch = sc_next(s);
        n++;
    }
    return n;
}

static void sc_scan_raw_string(TextScanner *s) {
    Rune ch = sc_next(s); /* read the character after '`' */
    while (ch != '`') {
        if (ch < 0) {
            sc_error(s, BURROW_S("literal not terminated"));
            return;
        }
        ch = sc_next(s);
    }
}

static void sc_scan_char(TextScanner *s) {
    if (sc_scan_string(s, '\'') != 1)
        sc_error(s, BURROW_S("invalid char literal"));
}

static Rune sc_scan_comment(TextScanner *s, Rune ch) {
    /* ch == '/' || ch == '*' */
    if (ch == '/') {
        /* Line comment. */
        ch = sc_next(s); /* read the character after "//" */
        while (ch != '\n' && ch >= 0)
            ch = sc_next(s);
        return ch;
    }

    /* General comment. */
    ch = sc_next(s); /* read the character after "/ *" */
    for (;;) {
        if (ch < 0) {
            sc_error(s, BURROW_S("comment not terminated"));
            break;
        }
        Rune ch0 = ch;
        ch = sc_next(s);
        if (ch0 == '*' && ch == '/') {
            ch = sc_next(s);
            break;
        }
    }
    return ch;
}

static bool sc_is_whitespace(const TextScanner *s, Rune ch) {
    /* Go shifts by uint(ch), and a shift of 64 or more gives 0. */
    return (uint32_t)ch < 64 && (s->whitespace & ((uint64_t)1 << (uint32_t)ch)) != 0;
}

Rune text_scanner_scan(TextScanner *s) {
    Rune ch = text_scanner_peek(s);

    /* Reset the token text position. */
    s->tok_pos = -1;
    s->position.line = 0;

    for (;;) {
        /* Skip white space. */
        while (sc_is_whitespace(s, ch))
            ch = sc_next(s);

        /* Start collecting token text. */
        bytes_buffer_reset(&s->tok_buf);
        s->tok_pos = s->src_pos - s->last_char_len;

        /* Set the token position. This is a slightly optimized version of
         * the code in Pos. */
        s->position.offset = s->src_buf_offset + s->tok_pos;
        if (s->column > 0) {
            /* Common case: the last character was not a '\n'. */
            s->position.line = s->line;
            s->position.column = s->column;
        } else {
            /* The last character was a '\n'. We cannot be at the beginning
             * of the source since we have called next at least once. */
            s->position.line = s->line - 1;
            s->position.column = s->last_line_len;
        }

        /* Determine the token value. */
        Rune tok = ch;
        if (sc_is_ident_rune(s, ch, 0)) {
            if ((s->mode & TEXT_SCANNER_SCAN_IDENTS) != 0) {
                tok = TEXT_SCANNER_IDENT;
                ch = sc_scan_identifier(s);
            } else {
                ch = sc_next(s);
            }
        } else if (sc_is_decimal(ch)) {
            if ((s->mode & (TEXT_SCANNER_SCAN_INTS | TEXT_SCANNER_SCAN_FLOATS)) != 0)
                tok = sc_scan_number(s, ch, false, &ch);
            else
                ch = sc_next(s);
        } else {
            switch (ch) {
            case TEXT_SCANNER_EOF:
                break;
            case '"':
                if ((s->mode & TEXT_SCANNER_SCAN_STRINGS) != 0) {
                    sc_scan_string(s, '"');
                    tok = TEXT_SCANNER_STRING;
                }
                ch = sc_next(s);
                break;
            case '\'':
                if ((s->mode & TEXT_SCANNER_SCAN_CHARS) != 0) {
                    sc_scan_char(s);
                    tok = TEXT_SCANNER_CHAR;
                }
                ch = sc_next(s);
                break;
            case '.':
                ch = sc_next(s);
                if (sc_is_decimal(ch) && (s->mode & TEXT_SCANNER_SCAN_FLOATS) != 0)
                    tok = sc_scan_number(s, ch, true, &ch);
                break;
            case '/':
                ch = sc_next(s);
                if ((ch == '/' || ch == '*') &&
                    (s->mode & TEXT_SCANNER_SCAN_COMMENTS) != 0) {
                    if ((s->mode & TEXT_SCANNER_SKIP_COMMENTS) != 0) {
                        s->tok_pos = -1; /* don't collect token text */
                        ch = sc_scan_comment(s, ch);
                        continue; /* Go's goto redo */
                    }
                    ch = sc_scan_comment(s, ch);
                    tok = TEXT_SCANNER_COMMENT;
                }
                break;
            case '`':
                if ((s->mode & TEXT_SCANNER_SCAN_RAW_STRINGS) != 0) {
                    sc_scan_raw_string(s);
                    tok = TEXT_SCANNER_RAW_STRING;
                }
                ch = sc_next(s);
                break;
            default:
                ch = sc_next(s);
                break;
            }
        }

        /* End of token text. */
        s->tok_end = s->src_pos - s->last_char_len;

        s->ch = ch;
        return tok;
    }
}

TextScannerPosition text_scanner_pos(const TextScanner *s) {
    TextScannerPosition pos;
    pos.filename = s->position.filename;
    pos.offset = s->src_buf_offset + s->src_pos - s->last_char_len;
    if (s->column > 0) {
        /* Common case: the last character was not a '\n'. */
        pos.line = s->line;
        pos.column = s->column;
    } else if (s->last_line_len > 0) {
        /* The last character was a '\n'. */
        pos.line = s->line - 1;
        pos.column = s->last_line_len;
    } else {
        /* At the beginning of the source. */
        pos.line = 1;
        pos.column = 1;
    }
    return pos;
}

Str text_scanner_token_text(TextScanner *s) {
    if (s->tok_pos < 0) {
        /* No token text. */
        return (Str){NULL, 0};
    }

    if (s->tok_end < s->tok_pos) {
        /* If EOF was reached, s->tok_end is set to -1 (s->src_pos == 0). */
        s->tok_end = s->tok_pos;
    }
    /* s->tok_end >= s->tok_pos */

    if (bytes_buffer_len(&s->tok_buf) == 0) {
        /* Common case: the entire token text is still in src_buf. */
        return str_from_bytes(s->src_buf + s->tok_pos, s->tok_end - s->tok_pos);
    }

    /* Part of the token text was saved in tok_buf: save the rest in tok_buf
     * as well and return its content. */
    sc_save_token(s, s->tok_pos, s->tok_end);
    s->tok_pos = s->tok_end; /* ensure idempotency of the TokenText call */
    Slice b = bytes_buffer_bytes(&s->tok_buf);
    return str_from_bytes(b.p, b.len);
}
