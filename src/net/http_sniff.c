/* Derived from Go's src/net/http/sniff.go and src/net/http/internal/sniff.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "burrow/core.h"
#include "burrow/net/http.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The kinds of signature in Go's sniffSignatures, each of which is a type of
 * its own there. */
typedef enum HsKind {
    HS_HTML,   /* htmlSig: a tag, any case, after the leading space */
    HS_MASKED, /* maskedSig: each byte through a mask, then compared */
    HS_EXACT,  /* exactSig: a prefix */
    HS_MP4,    /* mp4Sig */
    HS_TEXT,   /* textSig, which is last */
} HsKind;

typedef struct HsSig {
    const char *pat;  /* the pattern, or the tag for HS_HTML */
    const char *mask; /* HS_MASKED only, as long as pat */
    const char *ct;
    uint8_t len; /* of pat, which may have NULs in it */
    uint8_t kind;
    bool skip_ws; /* HS_MASKED: match after the leading space */
} HsSig;

#define HS_SIG_HTML(tag)                                                               \
    {tag, NULL, "text/html; charset=utf-8", sizeof(tag) - 1, HS_HTML, false}
#define HS_SIG_EXACT(pat, ct) {pat, NULL, ct, sizeof(pat) - 1, HS_EXACT, false}
#define HS_SIG_MASKED(mask, pat, ct, skip)                                             \
    {pat, mask, ct, sizeof(pat) - 1, HS_MASKED, skip}

/* The table in section 6 of the spec, in Go's order, which matters. */
static const HsSig hs_signatures[] = {
    HS_SIG_HTML("<!DOCTYPE HTML"),
    HS_SIG_HTML("<HTML"),
    HS_SIG_HTML("<HEAD"),
    HS_SIG_HTML("<SCRIPT"),
    HS_SIG_HTML("<IFRAME"),
    HS_SIG_HTML("<H1"),
    HS_SIG_HTML("<DIV"),
    HS_SIG_HTML("<FONT"),
    HS_SIG_HTML("<TABLE"),
    HS_SIG_HTML("<A"),
    HS_SIG_HTML("<STYLE"),
    HS_SIG_HTML("<TITLE"),
    HS_SIG_HTML("<B"),
    HS_SIG_HTML("<BODY"),
    HS_SIG_HTML("<BR"),
    HS_SIG_HTML("<P"),
    HS_SIG_HTML("<!--"),
    HS_SIG_MASKED("\xFF\xFF\xFF\xFF\xFF", "<?xml", "text/xml; charset=utf-8", true),
    HS_SIG_EXACT("%PDF-", "application/pdf"),
    HS_SIG_EXACT("%!PS-Adobe-", "application/postscript"),

    /* UTF BOMs. */
    HS_SIG_MASKED("\xFF\xFF\x00\x00", "\xFE\xFF\x00\x00",
                  "text/plain; charset=utf-16be", false),
    HS_SIG_MASKED("\xFF\xFF\x00\x00", "\xFF\xFE\x00\x00",
                  "text/plain; charset=utf-16le", false),
    HS_SIG_MASKED("\xFF\xFF\xFF\x00", "\xEF\xBB\xBF\x00", "text/plain; charset=utf-8",
                  false),

    /* Image types. Go once gave "image/vnd.microsoft.icon" for an icon, from
     * an older draft, and now gives "image/x-icon" as section 6.2 of the spec
     * does. */
    HS_SIG_EXACT("\x00\x00\x01\x00", "image/x-icon"),
    HS_SIG_EXACT("\x00\x00\x02\x00", "image/x-icon"),
    HS_SIG_EXACT("BM", "image/bmp"),
    HS_SIG_EXACT("GIF87a", "image/gif"),
    HS_SIG_EXACT("GIF89a", "image/gif"),
    HS_SIG_MASKED("\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF",
                  "RIFF\x00\x00\x00\x00WEBPVP", "image/webp", false),
    HS_SIG_EXACT("\x89PNG\x0D\x0A\x1A\x0A", "image/png"),
    HS_SIG_EXACT("\xFF\xD8\xFF", "image/jpeg"),

    /* Audio and video types, in the order the spec says to try them. */
    HS_SIG_MASKED("\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF",
                  "FORM\x00\x00\x00\x00"
                  "AIFF",
                  "audio/aiff", false),
    HS_SIG_MASKED("\xFF\xFF\xFF", "ID3", "audio/mpeg", false),
    HS_SIG_MASKED("\xFF\xFF\xFF\xFF\xFF", "OggS\x00", "application/ogg", false),
    HS_SIG_MASKED("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", "MThd\x00\x00\x00\x06",
                  "audio/midi", false),
    HS_SIG_MASKED("\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF",
                  "RIFF\x00\x00\x00\x00"
                  "AVI ",
                  "video/avi", false),
    HS_SIG_MASKED("\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF",
                  "RIFF\x00\x00\x00\x00WAVE", "audio/wave", false),
    {NULL, NULL, "video/mp4", 0, HS_MP4, false},
    HS_SIG_EXACT("\x1A\x45\xDF\xA3", "video/webm"),

    /* Font types. The first is 34 NULs and "LP", with only the last two bytes
     * looked at. */
    HS_SIG_MASKED("\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                  "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                  "\xFF\xFF",
                  "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                  "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                  "LP",
                  "application/vnd.ms-fontobject", false),
    HS_SIG_EXACT("\x00\x01\x00\x00", "font/ttf"),
    HS_SIG_EXACT("OTTO", "font/otf"),
    HS_SIG_EXACT("ttcf", "font/collection"),
    HS_SIG_EXACT("wOFF", "font/woff"),
    HS_SIG_EXACT("wOF2", "font/woff2"),

    /* Archive types. The spec has RAR's signatures wrong, see
     * https://github.com/whatwg/mimesniff/issues/63, and these are the ones
     * RAR Labs gives at https://www.rarlab.com/technote.htm#rarsign, as in
     * Go. */
    HS_SIG_EXACT("\x1F\x8B\x08", "application/x-gzip"),
    HS_SIG_EXACT("PK\x03\x04", "application/zip"),
    HS_SIG_EXACT("Rar!\x1A\x07\x00", "application/x-rar-compressed"), /* v1.5 to v4.0 */
    HS_SIG_EXACT("Rar!\x1A\x07\x01\x00", "application/x-rar-compressed"), /* v5 on */
    HS_SIG_EXACT("\x00\x61\x73\x6D", "application/wasm"),

    {NULL, NULL, "text/plain; charset=utf-8", 0, HS_TEXT, false},
};

/* 0xWS in the spec's terms. */
static bool hs_is_ws(Byte b) {
    switch (b) {
    case '\t':
    case '\n':
    case '\x0c':
    case '\r':
    case ' ':
        return true;
    default:
        return false;
    }
}

/* 0xTT, a byte that ends a tag. */
static bool hs_is_tt(Byte b) {
    return b == ' ' || b == '>';
}

static bool hs_html(const HsSig *s, const Byte *d, Int n) {
    if (n < (Int)s->len + 1)
        return false;
    for (Int i = 0; i < (Int)s->len; i++) {
        Byte b = (Byte)s->pat[i];
        Byte db = d[i];
        if ('A' <= b && b <= 'Z')
            db &= 0xDF;
        if (b != db)
            return false;
    }
    return hs_is_tt(d[s->len]);
}

static bool hs_masked(const HsSig *s, const Byte *d, Int n) {
    if (n < (Int)s->len)
        return false;
    for (Int i = 0; i < (Int)s->len; i++) {
        if ((d[i] & (Byte)s->mask[i]) != (Byte)s->pat[i])
            return false;
    }
    return true;
}

static uint32_t hs_be32(const Byte *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* https://mimesniff.spec.whatwg.org/#signature-for-mp4 */
static bool hs_mp4(const Byte *d, Int n) {
    if (n < 12)
        return false;
    Int box = (Int)hs_be32(d);
    if (n < box || box % 4 != 0)
        return false;
    if (memcmp(d + 4, "ftyp", 4) != 0)
        return false;
    for (Int st = 8; st < box; st += 4) {
        /* The four bytes at 12 are the major brand's version number. */
        if (st == 12)
            continue;
        if (memcmp(d + st, "mp4", 3) == 0)
            return true;
    }
    return false;
}

/* Section 5, step 4: no binary bytes after the leading space. */
static bool hs_text(const Byte *d, Int n) {
    for (Int i = 0; i < n; i++) {
        Byte b = d[i];
        if (b <= 0x08 || b == 0x0B || (0x0E <= b && b <= 0x1A) ||
            (0x1C <= b && b <= 0x1F))
            return false;
    }
    return true;
}

static bool hs_match(const HsSig *s, const Byte *d, Int n, Int ws) {
    switch ((HsKind)s->kind) {
    case HS_HTML:
        return hs_html(s, d + ws, n - ws);
    case HS_MASKED:
        return s->skip_ws ? hs_masked(s, d + ws, n - ws) : hs_masked(s, d, n);
    case HS_EXACT:
        return n >= (Int)s->len && memcmp(d, s->pat, s->len) == 0;
    case HS_MP4:
        return hs_mp4(d, n);
    case HS_TEXT:
        return hs_text(d + ws, n - ws);
    default:
        return false;
    }
}

Str http_detect_content_type(Slice data) {
    /* An empty slice may have no pointer, and d + 0 is not defined for that. */
    static const Byte none[1] = {0};
    const Byte *d = none;
    Int n = 0;
    if (data.p != NULL) {
        d = (const Byte *)data.p;
        n = data.len;
    }
    if (n > BURROW__HTTP_SNIFF_LEN)
        n = BURROW__HTTP_SNIFF_LEN;
    Int ws = 0;
    while (ws < n && hs_is_ws(d[ws]))
        ws++;
    for (size_t i = 0; i < sizeof hs_signatures / sizeof hs_signatures[0]; i++) {
        const HsSig *s = &hs_signatures[i];
        if (hs_match(s, d, n, ws))
            return str_from_cstr(s->ct);
    }
    return BURROW_S("application/octet-stream");
}
