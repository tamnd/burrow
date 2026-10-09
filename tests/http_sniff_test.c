/* Derived from Go's src/net/http/sniff_test.go.
 * Go source: go1.27.1.
 *
 * The test cases are Go's sniffTests. The tests that sniff through a server
 * wait for the server. TestDetectContentTypeLimit is burrow's own: only the
 * first 512 bytes are looked at.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/net/http.h"

#include <string.h>

static void TestDetectContentType(TestingT *t) {
    static const struct {
        const char *desc;
        const char *data;
        Int len;
        const char *content_type;
    } tests[] = {
        {"Empty", "", 0, "text/plain; charset=utf-8"},
        {"Binary", "\x01\x02\x03", 3, "application/octet-stream"},
        {"HTML document #1", "<HtMl><bOdY>blah blah blah</body></html>", 40,
         "text/html; charset=utf-8"},
        {"HTML document #2", "<HTML></HTML>", 13, "text/html; charset=utf-8"},
        {"HTML document #3 (leading whitespace)", "   <!DOCTYPE HTML>...", 21,
         "text/html; charset=utf-8"},
        {"HTML document #4 (leading CRLF)", "\x0D\x0A<html>...", 11,
         "text/html; charset=utf-8"},
        {"Plain text", "This is not HTML. It has \xE2\x98\x83 though.", 36,
         "text/plain; charset=utf-8"},
        {"XML", "\x0A<\x3Fxml!", 7, "text/xml; charset=utf-8"},
        {"Windows icon", "\x00\x00\x01\x00", 4, "image/x-icon"},
        {"Windows cursor", "\x00\x00\x02\x00", 4, "image/x-icon"},
        {"BMP image", "BM...", 5, "image/bmp"},
        {"GIF 87a", "GIF87a", 6, "image/gif"},
        {"GIF 89a", "GIF89a...", 9, "image/gif"},
        {"WEBP image", "RIFF\x00\x00\x00\x00WEBPVP", 14, "image/webp"},
        {"PNG image", "\x89PNG\x0D\x0A\x1A\x0A", 8, "image/png"},
        {"JPEG image", "\xFF\xD8\xFF", 3, "image/jpeg"},
        {"MIDI audio", "MThd\x00\x00\x00\x06\x00\x01", 10, "audio/midi"},
        {"MP3 audio/MPEG audio", "ID3\x03\x00\x00\x00\x00\x0F", 9, "audio/mpeg"},
        {"WAV audio #1", "RIFFb\xB8\x00\x00WAVEfmt \x12\x00\x00\x00\x06", 21,
         "audio/wave"},
        {"WAV audio #2", "RIFF,\x00\x00\x00WAVEfmt \x12\x00\x00\x00\x06", 21,
         "audio/wave"},
        {"AIFF audio #1",
         "FORM\x00\x00\x00\x00"
         "AIFFCOMM\x00\x00\x00\x12\x00\x01\x00\x00WU\x00\x10@\x0D\xF3"
         "4",
         32, "audio/aiff"},
        {"OGG audio",
         "OggS\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00~"
         "F\x00\x00\x00\x00\x00\x00\x1F\xF6\xB4\xFC\x01\x1E\x01vor",
         32, "application/ogg"},
        {"Must not match OGG", "owow\x00", 5, "application/octet-stream"},
        {"Must not match OGG", "oooS\x00", 5, "application/octet-stream"},
        {"Must not match OGG", "oggS\x00", 5, "application/octet-stream"},
        {"MP4 video",
         "\x00\x00\x00\x18"
         "ftypmp42\x00\x00\x00\x00mp42isom<\x06t\xBFmdat",
         32, "video/mp4"},
        {"AVI video #1",
         "RIFF,O\x0A\x00"
         "AVI LIST\xC3\x80",
         18, "video/avi"},
        {"AVI video #2",
         "RIFF,\x0A\x00\x00"
         "AVI LIST\xC3\x80",
         18, "video/avi"},
        {"TTF sample  I", "\x00\x01\x00\x00\x00\x17\x01\x00\x00\x04\x01`O", 13,
         "font/ttf"},
        {"TTF sample II", "\x00\x01\x00\x00\x00\x0E\x00\x80\x00\x03\x00`F", 13,
         "font/ttf"},
        {"OTTO sample  I", "OTTO\x00\x0E\x00\x80\x00\x03\x00`BASE", 16, "font/otf"},
        {"woff sample  I",
         "wOFF\x00\x01\x00\x00\x00\x00"
         "0T\x00\x0D\x00\x00",
         16, "font/woff"},
        {"woff2 sample", "wOF2\x00\x01\x00\x00\x00", 9, "font/woff2"},
        {"wasm sample",
         "\x00"
         "asm\x01\x00",
         6, "application/wasm"},
        {"RAR v1.5-v4.0", "Rar!\x1A\x07\x00", 7, "application/x-rar-compressed"},
        {"RAR v5+", "Rar!\x1A\x07\x01\x00", 8, "application/x-rar-compressed"},
        {"Incorrect RAR v1.5-v4.0", "Rar \x1A\x07\x00", 7, "application/octet-stream"},
        {"Incorrect RAR v5+", "Rar \x1A\x07\x01\x00", 8, "application/octet-stream"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Slice data = slice_from((void *)(uintptr_t)tests[i].data, tests[i].len,
                                tests[i].len, TYPE_BYTE);
        Str ct = http_detect_content_type(data);
        if (!str_eq(ct, str_from_cstr(tests[i].content_type)))
            testing_t_errorf_v(t, "%s: DetectContentType = %q, want %q", tests[i].desc,
                               ct, tests[i].content_type);
    }
    /* No bytes at all, with no pointer either. */
    Str ct = http_detect_content_type(slice_from(NULL, 0, 0, TYPE_BYTE));
    if (!str_eq(ct, BURROW_S("text/plain; charset=utf-8")))
        testing_t_errorf_v(t, "nil: DetectContentType = %q, want %q", ct,
                           "text/plain; charset=utf-8");
}

static void TestDetectContentTypeLimit(TestingT *t) {
    /* Text for 512 bytes and a binary byte after that is text, and the same
     * with the binary byte at 511 is not. */
    Byte data[600];
    memset(data, 'a', sizeof data);
    data[512] = 0x01;
    Str ct =
        http_detect_content_type(slice_from(data, sizeof data, sizeof data, TYPE_BYTE));
    if (!str_eq(ct, BURROW_S("text/plain; charset=utf-8")))
        testing_t_errorf_v(t, "binary byte at 512: DetectContentType = %q", ct);
    data[511] = 0x01;
    ct =
        http_detect_content_type(slice_from(data, sizeof data, sizeof data, TYPE_BYTE));
    if (!str_eq(ct, BURROW_S("application/octet-stream")))
        testing_t_errorf_v(t, "binary byte at 511: DetectContentType = %q", ct);
}

#define TESTS(X)                                                                       \
    X(TestDetectContentType)                                                           \
    X(TestDetectContentTypeLimit)

TESTING_MAIN(TESTS)
