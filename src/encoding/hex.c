/* Derived from Go's src/encoding/hex/hex.go.
 * Go source: go1.27.1.
 *
 * A straight port. The one change of shape is InvalidByteError: Go builds its
 * message with fmt when somebody asks, and an ErrorVT message slot cannot
 * allocate, so the 256 messages are a table here. Go printed them, with
 * fmt.Sprintf("%#U", rune(b)), so the text is Go's to the byte.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/hex.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

static const char hex_table[] = "0123456789abcdef";

/* The value of each hex digit, and 0xff for everything else. */
/* clang-format off */
static const Byte hex_reverse[256] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
/* clang-format on */

/* ------------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(hex_err_length, "encoding/hex: odd length hex string");

/* Go makes a new one with errors.New each time, so nobody can compare against
 * it, and one static error does the same job. */
static const Str hex_err_dumper_closed__text = {
    (const Byte *)"encoding/hex: dumper closed",
    (Int)(sizeof("encoding/hex: dumper closed") - 1)};
static const Error hex_err_dumper_closed = {&burrow_sentinel_error_vt,
                                            &hex_err_dumper_closed__text};

/* The byte first, so the data pointer of the Error is a pointer to a
 * HexInvalidByteError and errors_as hands it straight back. */
typedef struct HexInvalidByte {
    HexInvalidByteError b;
    Str message;
} HexInvalidByte;

#define HEX_IBE_PREFIX "encoding/hex: invalid byte: "
#define HEX_IBE(v, s)                                                                  \
    {                                                                                  \
        (v), {                                                                         \
            (const Byte *)(HEX_IBE_PREFIX s), (Int)(sizeof(HEX_IBE_PREFIX s) - 1)      \
        }                                                                              \
    }

/* clang-format off */
static const HexInvalidByte hex_invalid_bytes[256] = {
    HEX_IBE(0x00, "U+0000"),
    HEX_IBE(0x01, "U+0001"),
    HEX_IBE(0x02, "U+0002"),
    HEX_IBE(0x03, "U+0003"),
    HEX_IBE(0x04, "U+0004"),
    HEX_IBE(0x05, "U+0005"),
    HEX_IBE(0x06, "U+0006"),
    HEX_IBE(0x07, "U+0007"),
    HEX_IBE(0x08, "U+0008"),
    HEX_IBE(0x09, "U+0009"),
    HEX_IBE(0x0a, "U+000A"),
    HEX_IBE(0x0b, "U+000B"),
    HEX_IBE(0x0c, "U+000C"),
    HEX_IBE(0x0d, "U+000D"),
    HEX_IBE(0x0e, "U+000E"),
    HEX_IBE(0x0f, "U+000F"),
    HEX_IBE(0x10, "U+0010"),
    HEX_IBE(0x11, "U+0011"),
    HEX_IBE(0x12, "U+0012"),
    HEX_IBE(0x13, "U+0013"),
    HEX_IBE(0x14, "U+0014"),
    HEX_IBE(0x15, "U+0015"),
    HEX_IBE(0x16, "U+0016"),
    HEX_IBE(0x17, "U+0017"),
    HEX_IBE(0x18, "U+0018"),
    HEX_IBE(0x19, "U+0019"),
    HEX_IBE(0x1a, "U+001A"),
    HEX_IBE(0x1b, "U+001B"),
    HEX_IBE(0x1c, "U+001C"),
    HEX_IBE(0x1d, "U+001D"),
    HEX_IBE(0x1e, "U+001E"),
    HEX_IBE(0x1f, "U+001F"),
    HEX_IBE(0x20, "U+0020 ' '"),
    HEX_IBE(0x21, "U+0021 '!'"),
    HEX_IBE(0x22, "U+0022 '\"'"),
    HEX_IBE(0x23, "U+0023 '#'"),
    HEX_IBE(0x24, "U+0024 '$'"),
    HEX_IBE(0x25, "U+0025 '%'"),
    HEX_IBE(0x26, "U+0026 '&'"),
    HEX_IBE(0x27, "U+0027 '''"),
    HEX_IBE(0x28, "U+0028 '('"),
    HEX_IBE(0x29, "U+0029 ')'"),
    HEX_IBE(0x2a, "U+002A '*'"),
    HEX_IBE(0x2b, "U+002B '+'"),
    HEX_IBE(0x2c, "U+002C ','"),
    HEX_IBE(0x2d, "U+002D '-'"),
    HEX_IBE(0x2e, "U+002E '.'"),
    HEX_IBE(0x2f, "U+002F '/'"),
    HEX_IBE(0x30, "U+0030 '0'"),
    HEX_IBE(0x31, "U+0031 '1'"),
    HEX_IBE(0x32, "U+0032 '2'"),
    HEX_IBE(0x33, "U+0033 '3'"),
    HEX_IBE(0x34, "U+0034 '4'"),
    HEX_IBE(0x35, "U+0035 '5'"),
    HEX_IBE(0x36, "U+0036 '6'"),
    HEX_IBE(0x37, "U+0037 '7'"),
    HEX_IBE(0x38, "U+0038 '8'"),
    HEX_IBE(0x39, "U+0039 '9'"),
    HEX_IBE(0x3a, "U+003A ':'"),
    HEX_IBE(0x3b, "U+003B ';'"),
    HEX_IBE(0x3c, "U+003C '<'"),
    HEX_IBE(0x3d, "U+003D '='"),
    HEX_IBE(0x3e, "U+003E '>'"),
    HEX_IBE(0x3f, "U+003F '?'"),
    HEX_IBE(0x40, "U+0040 '@'"),
    HEX_IBE(0x41, "U+0041 'A'"),
    HEX_IBE(0x42, "U+0042 'B'"),
    HEX_IBE(0x43, "U+0043 'C'"),
    HEX_IBE(0x44, "U+0044 'D'"),
    HEX_IBE(0x45, "U+0045 'E'"),
    HEX_IBE(0x46, "U+0046 'F'"),
    HEX_IBE(0x47, "U+0047 'G'"),
    HEX_IBE(0x48, "U+0048 'H'"),
    HEX_IBE(0x49, "U+0049 'I'"),
    HEX_IBE(0x4a, "U+004A 'J'"),
    HEX_IBE(0x4b, "U+004B 'K'"),
    HEX_IBE(0x4c, "U+004C 'L'"),
    HEX_IBE(0x4d, "U+004D 'M'"),
    HEX_IBE(0x4e, "U+004E 'N'"),
    HEX_IBE(0x4f, "U+004F 'O'"),
    HEX_IBE(0x50, "U+0050 'P'"),
    HEX_IBE(0x51, "U+0051 'Q'"),
    HEX_IBE(0x52, "U+0052 'R'"),
    HEX_IBE(0x53, "U+0053 'S'"),
    HEX_IBE(0x54, "U+0054 'T'"),
    HEX_IBE(0x55, "U+0055 'U'"),
    HEX_IBE(0x56, "U+0056 'V'"),
    HEX_IBE(0x57, "U+0057 'W'"),
    HEX_IBE(0x58, "U+0058 'X'"),
    HEX_IBE(0x59, "U+0059 'Y'"),
    HEX_IBE(0x5a, "U+005A 'Z'"),
    HEX_IBE(0x5b, "U+005B '['"),
    HEX_IBE(0x5c, "U+005C '\\'"),
    HEX_IBE(0x5d, "U+005D ']'"),
    HEX_IBE(0x5e, "U+005E '^'"),
    HEX_IBE(0x5f, "U+005F '_'"),
    HEX_IBE(0x60, "U+0060 '`'"),
    HEX_IBE(0x61, "U+0061 'a'"),
    HEX_IBE(0x62, "U+0062 'b'"),
    HEX_IBE(0x63, "U+0063 'c'"),
    HEX_IBE(0x64, "U+0064 'd'"),
    HEX_IBE(0x65, "U+0065 'e'"),
    HEX_IBE(0x66, "U+0066 'f'"),
    HEX_IBE(0x67, "U+0067 'g'"),
    HEX_IBE(0x68, "U+0068 'h'"),
    HEX_IBE(0x69, "U+0069 'i'"),
    HEX_IBE(0x6a, "U+006A 'j'"),
    HEX_IBE(0x6b, "U+006B 'k'"),
    HEX_IBE(0x6c, "U+006C 'l'"),
    HEX_IBE(0x6d, "U+006D 'm'"),
    HEX_IBE(0x6e, "U+006E 'n'"),
    HEX_IBE(0x6f, "U+006F 'o'"),
    HEX_IBE(0x70, "U+0070 'p'"),
    HEX_IBE(0x71, "U+0071 'q'"),
    HEX_IBE(0x72, "U+0072 'r'"),
    HEX_IBE(0x73, "U+0073 's'"),
    HEX_IBE(0x74, "U+0074 't'"),
    HEX_IBE(0x75, "U+0075 'u'"),
    HEX_IBE(0x76, "U+0076 'v'"),
    HEX_IBE(0x77, "U+0077 'w'"),
    HEX_IBE(0x78, "U+0078 'x'"),
    HEX_IBE(0x79, "U+0079 'y'"),
    HEX_IBE(0x7a, "U+007A 'z'"),
    HEX_IBE(0x7b, "U+007B '{'"),
    HEX_IBE(0x7c, "U+007C '|'"),
    HEX_IBE(0x7d, "U+007D '}'"),
    HEX_IBE(0x7e, "U+007E '~'"),
    HEX_IBE(0x7f, "U+007F"),
    HEX_IBE(0x80, "U+0080"),
    HEX_IBE(0x81, "U+0081"),
    HEX_IBE(0x82, "U+0082"),
    HEX_IBE(0x83, "U+0083"),
    HEX_IBE(0x84, "U+0084"),
    HEX_IBE(0x85, "U+0085"),
    HEX_IBE(0x86, "U+0086"),
    HEX_IBE(0x87, "U+0087"),
    HEX_IBE(0x88, "U+0088"),
    HEX_IBE(0x89, "U+0089"),
    HEX_IBE(0x8a, "U+008A"),
    HEX_IBE(0x8b, "U+008B"),
    HEX_IBE(0x8c, "U+008C"),
    HEX_IBE(0x8d, "U+008D"),
    HEX_IBE(0x8e, "U+008E"),
    HEX_IBE(0x8f, "U+008F"),
    HEX_IBE(0x90, "U+0090"),
    HEX_IBE(0x91, "U+0091"),
    HEX_IBE(0x92, "U+0092"),
    HEX_IBE(0x93, "U+0093"),
    HEX_IBE(0x94, "U+0094"),
    HEX_IBE(0x95, "U+0095"),
    HEX_IBE(0x96, "U+0096"),
    HEX_IBE(0x97, "U+0097"),
    HEX_IBE(0x98, "U+0098"),
    HEX_IBE(0x99, "U+0099"),
    HEX_IBE(0x9a, "U+009A"),
    HEX_IBE(0x9b, "U+009B"),
    HEX_IBE(0x9c, "U+009C"),
    HEX_IBE(0x9d, "U+009D"),
    HEX_IBE(0x9e, "U+009E"),
    HEX_IBE(0x9f, "U+009F"),
    HEX_IBE(0xa0, "U+00A0"),
    HEX_IBE(0xa1, "U+00A1 '\xc2\xa1'"),
    HEX_IBE(0xa2, "U+00A2 '\xc2\xa2'"),
    HEX_IBE(0xa3, "U+00A3 '\xc2\xa3'"),
    HEX_IBE(0xa4, "U+00A4 '\xc2\xa4'"),
    HEX_IBE(0xa5, "U+00A5 '\xc2\xa5'"),
    HEX_IBE(0xa6, "U+00A6 '\xc2\xa6'"),
    HEX_IBE(0xa7, "U+00A7 '\xc2\xa7'"),
    HEX_IBE(0xa8, "U+00A8 '\xc2\xa8'"),
    HEX_IBE(0xa9, "U+00A9 '\xc2\xa9'"),
    HEX_IBE(0xaa, "U+00AA '\xc2\xaa'"),
    HEX_IBE(0xab, "U+00AB '\xc2\xab'"),
    HEX_IBE(0xac, "U+00AC '\xc2\xac'"),
    HEX_IBE(0xad, "U+00AD"),
    HEX_IBE(0xae, "U+00AE '\xc2\xae'"),
    HEX_IBE(0xaf, "U+00AF '\xc2\xaf'"),
    HEX_IBE(0xb0, "U+00B0 '\xc2\xb0'"),
    HEX_IBE(0xb1, "U+00B1 '\xc2\xb1'"),
    HEX_IBE(0xb2, "U+00B2 '\xc2\xb2'"),
    HEX_IBE(0xb3, "U+00B3 '\xc2\xb3'"),
    HEX_IBE(0xb4, "U+00B4 '\xc2\xb4'"),
    HEX_IBE(0xb5, "U+00B5 '\xc2\xb5'"),
    HEX_IBE(0xb6, "U+00B6 '\xc2\xb6'"),
    HEX_IBE(0xb7, "U+00B7 '\xc2\xb7'"),
    HEX_IBE(0xb8, "U+00B8 '\xc2\xb8'"),
    HEX_IBE(0xb9, "U+00B9 '\xc2\xb9'"),
    HEX_IBE(0xba, "U+00BA '\xc2\xba'"),
    HEX_IBE(0xbb, "U+00BB '\xc2\xbb'"),
    HEX_IBE(0xbc, "U+00BC '\xc2\xbc'"),
    HEX_IBE(0xbd, "U+00BD '\xc2\xbd'"),
    HEX_IBE(0xbe, "U+00BE '\xc2\xbe'"),
    HEX_IBE(0xbf, "U+00BF '\xc2\xbf'"),
    HEX_IBE(0xc0, "U+00C0 '\xc3\x80'"),
    HEX_IBE(0xc1, "U+00C1 '\xc3\x81'"),
    HEX_IBE(0xc2, "U+00C2 '\xc3\x82'"),
    HEX_IBE(0xc3, "U+00C3 '\xc3\x83'"),
    HEX_IBE(0xc4, "U+00C4 '\xc3\x84'"),
    HEX_IBE(0xc5, "U+00C5 '\xc3\x85'"),
    HEX_IBE(0xc6, "U+00C6 '\xc3\x86'"),
    HEX_IBE(0xc7, "U+00C7 '\xc3\x87'"),
    HEX_IBE(0xc8, "U+00C8 '\xc3\x88'"),
    HEX_IBE(0xc9, "U+00C9 '\xc3\x89'"),
    HEX_IBE(0xca, "U+00CA '\xc3\x8a'"),
    HEX_IBE(0xcb, "U+00CB '\xc3\x8b'"),
    HEX_IBE(0xcc, "U+00CC '\xc3\x8c'"),
    HEX_IBE(0xcd, "U+00CD '\xc3\x8d'"),
    HEX_IBE(0xce, "U+00CE '\xc3\x8e'"),
    HEX_IBE(0xcf, "U+00CF '\xc3\x8f'"),
    HEX_IBE(0xd0, "U+00D0 '\xc3\x90'"),
    HEX_IBE(0xd1, "U+00D1 '\xc3\x91'"),
    HEX_IBE(0xd2, "U+00D2 '\xc3\x92'"),
    HEX_IBE(0xd3, "U+00D3 '\xc3\x93'"),
    HEX_IBE(0xd4, "U+00D4 '\xc3\x94'"),
    HEX_IBE(0xd5, "U+00D5 '\xc3\x95'"),
    HEX_IBE(0xd6, "U+00D6 '\xc3\x96'"),
    HEX_IBE(0xd7, "U+00D7 '\xc3\x97'"),
    HEX_IBE(0xd8, "U+00D8 '\xc3\x98'"),
    HEX_IBE(0xd9, "U+00D9 '\xc3\x99'"),
    HEX_IBE(0xda, "U+00DA '\xc3\x9a'"),
    HEX_IBE(0xdb, "U+00DB '\xc3\x9b'"),
    HEX_IBE(0xdc, "U+00DC '\xc3\x9c'"),
    HEX_IBE(0xdd, "U+00DD '\xc3\x9d'"),
    HEX_IBE(0xde, "U+00DE '\xc3\x9e'"),
    HEX_IBE(0xdf, "U+00DF '\xc3\x9f'"),
    HEX_IBE(0xe0, "U+00E0 '\xc3\xa0'"),
    HEX_IBE(0xe1, "U+00E1 '\xc3\xa1'"),
    HEX_IBE(0xe2, "U+00E2 '\xc3\xa2'"),
    HEX_IBE(0xe3, "U+00E3 '\xc3\xa3'"),
    HEX_IBE(0xe4, "U+00E4 '\xc3\xa4'"),
    HEX_IBE(0xe5, "U+00E5 '\xc3\xa5'"),
    HEX_IBE(0xe6, "U+00E6 '\xc3\xa6'"),
    HEX_IBE(0xe7, "U+00E7 '\xc3\xa7'"),
    HEX_IBE(0xe8, "U+00E8 '\xc3\xa8'"),
    HEX_IBE(0xe9, "U+00E9 '\xc3\xa9'"),
    HEX_IBE(0xea, "U+00EA '\xc3\xaa'"),
    HEX_IBE(0xeb, "U+00EB '\xc3\xab'"),
    HEX_IBE(0xec, "U+00EC '\xc3\xac'"),
    HEX_IBE(0xed, "U+00ED '\xc3\xad'"),
    HEX_IBE(0xee, "U+00EE '\xc3\xae'"),
    HEX_IBE(0xef, "U+00EF '\xc3\xaf'"),
    HEX_IBE(0xf0, "U+00F0 '\xc3\xb0'"),
    HEX_IBE(0xf1, "U+00F1 '\xc3\xb1'"),
    HEX_IBE(0xf2, "U+00F2 '\xc3\xb2'"),
    HEX_IBE(0xf3, "U+00F3 '\xc3\xb3'"),
    HEX_IBE(0xf4, "U+00F4 '\xc3\xb4'"),
    HEX_IBE(0xf5, "U+00F5 '\xc3\xb5'"),
    HEX_IBE(0xf6, "U+00F6 '\xc3\xb6'"),
    HEX_IBE(0xf7, "U+00F7 '\xc3\xb7'"),
    HEX_IBE(0xf8, "U+00F8 '\xc3\xb8'"),
    HEX_IBE(0xf9, "U+00F9 '\xc3\xb9'"),
    HEX_IBE(0xfa, "U+00FA '\xc3\xba'"),
    HEX_IBE(0xfb, "U+00FB '\xc3\xbb'"),
    HEX_IBE(0xfc, "U+00FC '\xc3\xbc'"),
    HEX_IBE(0xfd, "U+00FD '\xc3\xbd'"),
    HEX_IBE(0xfe, "U+00FE '\xc3\xbe'"),
    HEX_IBE(0xff, "U+00FF '\xc3\xbf'"),
};
/* clang-format on */

static const Type hex_invalid_byte_desc = {
    {(const Byte *)"InvalidByteError", 16},
    {(const Byte *)"encoding/hex", 12},
    KIND_UINT8,
    (uint32_t)sizeof(HexInvalidByteError),
    (uint16_t)_Alignof(HexInvalidByteError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68696265U, /* "hibe" */
    NULL,
};

const Type *const TYPE_HEX_INVALID_BYTE_ERROR = &hex_invalid_byte_desc;

static Str hex_invalid_byte_message(const void *self) {
    return ((const HexInvalidByte *)self)->message;
}

/* No clone: the errors are static and never need retaining. */
static const ErrorVT hex_invalid_byte_vt = {
    .self_type = &hex_invalid_byte_desc,
    .message = hex_invalid_byte_message,
};

Str hex_invalid_byte_error_error(HexInvalidByteError e) {
    return hex_invalid_bytes[e].message;
}

Error hex_invalid_byte_error_as_error(HexInvalidByteError e) {
    Error err = {&hex_invalid_byte_vt, (void *)(uintptr_t)&hex_invalid_bytes[e]};
    return err;
}

/* ------------------------------------------------------------------ lengths */

Int hex_encoded_len(Int n) {
    return n * 2;
}

Int hex_decoded_len(Int x) {
    return x / 2;
}

/* ----------------------------------------------------------------- one shot */

static void hex_encode_raw(Byte *dst, const Byte *src, Int n) {
    for (Int i = 0; i < n; i++) {
        Byte v = src[i];
        dst[2 * i] = (Byte)hex_table[v >> 4];
        dst[2 * i + 1] = (Byte)hex_table[v & 0x0f];
    }
}

Int hex_encode(Slice dst, Slice src) {
    Int n = hex_encoded_len(src.len);
    /* Go writes until it runs off the end, and the index it runs off at is
     * always len(dst), odd or even. */
    if (dst.len < n)
        runtime_index_out_of_range(dst.len, dst.len);
    hex_encode_raw((Byte *)dst.p, (const Byte *)src.p, src.len);
    return n;
}

/* Grows dst by n and returns where the new bytes start, or NULL when it could
 * not. A zero Slice is Go's nil []byte. */
static Byte *hex_grow(Alloc *a, Slice *dst, Int n) {
    if (dst->elem == NULL)
        *dst = slice_nil(TYPE_BYTE);
    Int old = dst->len;
    *dst = slice_append(a, *dst, NULL, n);
    return dst->p == NULL ? NULL : (Byte *)dst->p + old;
}

Slice hex_append_encode(Alloc *a, Slice dst, Slice src) {
    Int n = hex_encoded_len(src.len);
    Byte *p = hex_grow(a, &dst, n);
    if (p != NULL)
        hex_encode_raw(p, (const Byte *)src.p, src.len);
    return dst;
}

/* Decodes n digits of src into dst, which has room for cap bytes, and returns
 * how many it wrote. */
static Int hex_decode_raw(Byte *dst, Int cap, const Byte *src, Int n, Error *err) {
    Int i = 0, j = 0;
    for (; j < n - 1; j += 2) {
        Byte p = src[j], q = src[j + 1];
        Byte a = hex_reverse[p], b = hex_reverse[q];
        if (a > 0x0f) {
            BURROW_OUT(err, hex_invalid_byte_error_as_error(p));
            return i;
        }
        if (b > 0x0f) {
            BURROW_OUT(err, hex_invalid_byte_error_as_error(q));
            return i;
        }
        if (i >= cap)
            runtime_index_out_of_range(i, cap);
        dst[i++] = (Byte)((a << 4) | b);
    }
    if (n % 2 == 1) {
        /* A bad byte before the odd length, since it comes first. */
        if (hex_reverse[src[j]] > 0x0f)
            BURROW_OUT(err, hex_invalid_byte_error_as_error(src[j]));
        else
            BURROW_OUT(err, hex_err_length);
        return i;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return i;
}

Int hex_decode(Slice dst, Slice src, Error *err) {
    return hex_decode_raw((Byte *)dst.p, dst.len, (const Byte *)src.p, src.len, err);
}

Slice hex_append_decode(Alloc *a, Slice dst, Slice src, Error *err) {
    Int n = hex_decoded_len(src.len);
    Int old = dst.len;
    Byte *p = hex_grow(a, &dst, n);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return dst;
    }
    Int got = hex_decode_raw(p, n, (const Byte *)src.p, src.len, err);
    dst.len = old + got;
    return dst;
}

Str hex_encode_to_string(Alloc *a, Slice src) {
    Int n = hex_encoded_len(src.len);
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    hex_encode_raw(p, (const Byte *)src.p, src.len);
    return str_from_bytes(p, n);
}

Slice hex_decode_string(Alloc *a, Str s, Error *err) {
    Int n = hex_decoded_len(s.len);
    Slice dst = slice_make(a, TYPE_BYTE, n, n);
    if (n > 0 && dst.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return dst;
    }
    dst.len = hex_decode_raw((Byte *)dst.p, n, s.p, s.len, err);
    return dst;
}

/* --------------------------------------------------------------------- dump */

/* A full line is 79 bytes, and a last line of n bytes is 63 + n: the offset
 * and its two spaces, sixteen cells of three, the gap after the eighth, the
 * space and bar before the text, the text, and the bar and newline after it. */
#define HEX_DUMP_LINE 79

static Byte hex_to_char(Byte b) {
    return (b < 32 || b > 126) ? '.' : b;
}

Str hex_dump(Alloc *a, Slice data) {
    if (data.len == 0)
        return BURROW_STR_EMPTY;
    Int full = data.len / 16, rest = data.len % 16;
    Int size = full * HEX_DUMP_LINE + (rest > 0 ? 63 + rest : 0);
    Byte *out = (Byte *)mem_alloc_nozero(a, (size_t)size, 1);
    if (out == NULL)
        return BURROW_STR_EMPTY;

    const Byte *src = (const Byte *)data.p;
    Byte *o = out;
    for (Int off = 0; off < data.len; off += 16) {
        Int n = data.len - off < 16 ? data.len - off : 16;
        /* Go's dumper keeps a 32 bit offset and so does this. */
        uint32_t at = (uint32_t)off;
        Byte be[4] = {(Byte)(at >> 24), (Byte)(at >> 16), (Byte)(at >> 8), (Byte)at};
        hex_encode_raw(o, be, 4);
        o += 8;
        *o++ = ' ';
        *o++ = ' ';
        for (Int k = 0; k < 16; k++) {
            if (k < n) {
                hex_encode_raw(o, src + off + k, 1);
            } else {
                o[0] = ' ';
                o[1] = ' ';
            }
            o[2] = ' ';
            o += 3;
            if (k == 7)
                *o++ = ' ';
        }
        *o++ = ' ';
        *o++ = '|';
        for (Int k = 0; k < n; k++)
            *o++ = hex_to_char(src[off + k]);
        *o++ = '|';
        *o++ = '\n';
    }
    return str_from_bytes(out, size);
}

/* ---------------------------------------------------------------- streaming */

/* The number of hex digits the encoder and decoder hold at once. */
#define HEX_BUFFER_SIZE 1024

typedef struct HexEncoder {
    IoWriter w;
    Error err;
    Byte out[HEX_BUFFER_SIZE];
} HexEncoder;

static Int hex_encoder_write(void *self, Slice p, Error *err) {
    HexEncoder *e = (HexEncoder *)self;
    const Byte *src = (const Byte *)p.p;
    Int left = p.len, n = 0;
    while (left > 0 && BURROW_OK(e->err)) {
        Int chunk = left < HEX_BUFFER_SIZE / 2 ? left : HEX_BUFFER_SIZE / 2;
        hex_encode_raw(e->out, src, chunk);
        Error werr = BURROW_NO_ERROR;
        Int written = e->w.vt->write(
            e->w.data, slice_from(e->out, chunk * 2, HEX_BUFFER_SIZE, TYPE_BYTE),
            &werr);
        e->err = werr;
        n += written / 2;
        src += chunk;
        left -= chunk;
    }
    BURROW_OUT(err, e->err);
    return n;
}

static const IoWriterVT hex_encoder_vt = {NULL, hex_encoder_write};

IoWriter hex_new_encoder(Alloc *a, IoWriter w) {
    HexEncoder *e = BURROW_NEW(a, HexEncoder);
    if (e == NULL)
        return (IoWriter){NULL, NULL};
    e->w = w;
    return (IoWriter){&hex_encoder_vt, e};
}

typedef struct HexDecoder {
    IoReader r;
    Error err;
    Int off, len; /* the undecoded digits, arr[off:off+len] */
    Byte arr[HEX_BUFFER_SIZE];
} HexDecoder;

static Int hex_decoder_read(void *self, Slice p, Error *err) {
    HexDecoder *d = (HexDecoder *)self;

    /* Refill when fewer than two digits are left, keeping the odd one. */
    if (d->len < 2 && BURROW_OK(d->err)) {
        if (d->len == 1)
            d->arr[0] = d->arr[d->off];
        Int kept = d->len;
        Error rerr = BURROW_NO_ERROR;
        Int got = d->r.vt->read(d->r.data,
                                slice_from(d->arr + kept, HEX_BUFFER_SIZE - kept,
                                           HEX_BUFFER_SIZE - kept, TYPE_BYTE),
                                &rerr);
        d->err = rerr;
        d->off = 0;
        d->len = kept + got;
        /* Go compares with ==, so an error wrapping EOF is not the end. */
        if (d->err.vt == io_eof.vt && d->err.data == io_eof.data && d->len % 2 != 0) {
            Byte last = d->arr[d->len - 1];
            if (hex_reverse[last] > 0x0f)
                d->err = hex_invalid_byte_error_as_error(last);
            else
                d->err = io_err_unexpected_eof;
        }
    }

    Int want = p.len;
    if (want > d->len / 2)
        want = d->len / 2;
    Error derr = BURROW_NO_ERROR;
    Int dec = hex_decode_raw((Byte *)p.p, want, d->arr + d->off, want * 2, &derr);
    d->off += 2 * dec;
    d->len -= 2 * dec;
    if (BURROW_FAILED(derr)) {
        /* A bad digit: drop the rest of the input. */
        d->off = d->len = 0;
        d->err = derr;
    }

    if (d->len < 2) {
        BURROW_OUT(err, d->err);
        return dec;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return dec;
}

static const IoReaderVT hex_decoder_vt = {NULL, hex_decoder_read};

IoReader hex_new_decoder(Alloc *a, IoReader r) {
    HexDecoder *d = BURROW_NEW(a, HexDecoder);
    if (d == NULL)
        return (IoReader){NULL, NULL};
    d->r = r;
    return (IoReader){&hex_decoder_vt, d};
}

typedef struct HexDumper {
    IoWriter w;
    Byte right[18];
    Byte buf[14];
    Int used;   /* bytes in the current line */
    uint32_t n; /* bytes in all, of which Go prints 32 bits */
    bool closed;
} HexDumper;

static Error hex_dumper_put(HexDumper *h, Byte *p, Int n) {
    Error err = BURROW_NO_ERROR;
    h->w.vt->write(h->w.data, slice_from(p, n, n, TYPE_BYTE), &err);
    return err;
}

/* Lines look like this, and each byte is written as it arrives:
 *
 * 00000010  2e 2f 30 31 32 33 34 35  36 37 38 39 3a 3b 3c 3d  |./0123456789:;<=|
 * ^ offset                          ^ extra space              ^ text of the line */
static Int hex_dumper_write(void *self, Slice data, Error *err) {
    HexDumper *h = (HexDumper *)self;
    if (h->closed) {
        BURROW_OUT(err, hex_err_dumper_closed);
        return 0;
    }
    const Byte *src = (const Byte *)data.p;
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    for (Int i = 0; i < data.len; i++) {
        if (h->used == 0) {
            h->buf[0] = (Byte)(h->n >> 24);
            h->buf[1] = (Byte)(h->n >> 16);
            h->buf[2] = (Byte)(h->n >> 8);
            h->buf[3] = (Byte)h->n;
            hex_encode_raw(h->buf + 4, h->buf, 4);
            h->buf[12] = ' ';
            h->buf[13] = ' ';
            e = hex_dumper_put(h, h->buf + 4, 10);
            if (BURROW_FAILED(e))
                break;
        }
        hex_encode_raw(h->buf, src + i, 1);
        h->buf[2] = ' ';
        Int l = 3;
        if (h->used == 7) {
            h->buf[3] = ' ';
            l = 4;
        } else if (h->used == 15) {
            h->buf[3] = ' ';
            h->buf[4] = '|';
            l = 5;
        }
        e = hex_dumper_put(h, h->buf, l);
        if (BURROW_FAILED(e))
            break;
        n++;
        h->right[h->used] = hex_to_char(src[i]);
        h->used++;
        h->n++;
        if (h->used == 16) {
            h->right[16] = '|';
            h->right[17] = '\n';
            e = hex_dumper_put(h, h->right, 18);
            if (BURROW_FAILED(e))
                break;
            h->used = 0;
        }
    }
    BURROW_OUT(err, e);
    return n;
}

static Error hex_dumper_close(void *self) {
    HexDumper *h = (HexDumper *)self;
    if (h->closed)
        return BURROW_NO_ERROR;
    h->closed = true;
    if (h->used == 0)
        return BURROW_NO_ERROR;
    h->buf[0] = ' ';
    h->buf[1] = ' ';
    h->buf[2] = ' ';
    h->buf[3] = ' ';
    h->buf[4] = '|';
    Int bytes = h->used;
    while (h->used < 16) {
        Int l = 3;
        if (h->used == 7)
            l = 4;
        else if (h->used == 15)
            l = 5;
        Error e = hex_dumper_put(h, h->buf, l);
        if (BURROW_FAILED(e))
            return e;
        h->used++;
    }
    h->right[bytes] = '|';
    h->right[bytes + 1] = '\n';
    return hex_dumper_put(h, h->right, bytes + 2);
}

static const IoWriteCloserVT hex_dumper_vt = {
    {NULL, hex_dumper_write},
    {NULL, hex_dumper_close},
};

IoWriteCloser hex_dumper(Alloc *a, IoWriter w) {
    HexDumper *h = BURROW_NEW(a, HexDumper);
    if (h == NULL)
        return (IoWriteCloser){NULL, NULL};
    h->w = w;
    return (IoWriteCloser){&hex_dumper_vt, h};
}
