/* time/tzdata: Go's time/tzdata/tzdata.go.
 *
 * The database is Go's lib/time/zoneinfo.zip, the one Go turns into
 * zzipdata.go, embedded with burrow-gen embed into tzdata_zip.c. Every entry
 * in it is stored, not compressed, so a zone is a slice of the zip with no
 * copying.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/time/tzdata.h"

#include "burrow/core.h"
#include "burrow/embed.h"
#include "burrow/error.h"
#include "burrow/mem.h"

#include "time_internal.h"

#include <stdbool.h>
#include <string.h>

/* tzdata_zip.c is written by
 *
 *     tools/burrow-gen embed src/time/tzdata.c --include burrow/time/tzdata.h \
 *         -o src/time/tzdata_zip.c
 *
 * and tools/check-gen.sh checks it is up to date. */
BURROW_EMBED_FILE(burrow__tzdata_zip, "zoneinfo.zip");

static Int tzdata_get4(Str s, Int i) {
    if (i < 0 || s.len - i < 4)
        return 0;
    const Byte *p = s.p + i;
    return (Int)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                 (uint32_t)p[3] << 24);
}

static Int tzdata_get2(Str s, Int i) {
    if (i < 0 || s.len - i < 2)
        return 0;
    return (Int)((uint32_t)s.p[i] | (uint32_t)s.p[i + 1] << 8);
}

static Str tzdata_slice(Str s, Int i, Int n) {
    if (i < 0 || n < 0 || i > s.len || s.len - i < n)
        return (Str){NULL, -1};
    return (Str){s.p + i, n};
}

static Error tzdata_err(Str a, Str b, Str c) {
    Alloc *e = error_allocator();
    Byte *p = mem_alloc(e, (size_t)(a.len + b.len + c.len), 1);
    if (p == NULL)
        return burrow_err_out_of_memory;
    memcpy(p, a.p, (size_t)a.len);
    if (b.len > 0)
        memcpy(p + a.len, b.p, (size_t)b.len);
    memcpy(p + a.len + b.len, c.p, (size_t)c.len);
    return errors_new(e, (Str){p, a.len + b.len + c.len});
}

/* Go's loadFromEmbeddedTZData: the TZif data for name, false and no error if
 * the database has no such zone, which Go says with syscall.ENOENT. Go trusts
 * the zip and would panic on a bad offset. Here a bad offset is the same
 * error as a bad header. */
static bool tzdata_load(Str name, Str *data, Error *err) {
    enum {
        ZECHEADER = 0x06054b50,
        ZCHEADER = 0x02014b50,
        ZTAILSIZE = 22,

        ZHEADERSIZE = 30,
        ZHEADER = 0x04034b50,
    };
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str z = burrow__tzdata_zip;
    Str corrupt = BURROW_S("corrupt embedded tzdata");

    Int idx = z.len - ZTAILSIZE;
    Int n = tzdata_get2(z, idx + 10);
    idx = tzdata_get4(z, idx + 16);

    for (Int i = 0; i < n; i++) {
        /* See time.loadTzinfoFromZip for zip entry layout. */
        if (tzdata_get4(z, idx) != ZCHEADER)
            break;
        Int meth = tzdata_get2(z, idx + 10);
        Int size = tzdata_get4(z, idx + 24);
        Int namelen = tzdata_get2(z, idx + 28);
        Int xlen = tzdata_get2(z, idx + 30);
        Int fclen = tzdata_get2(z, idx + 32);
        Int off = tzdata_get4(z, idx + 42);
        Str zname = tzdata_slice(z, idx + 46, namelen);
        if (zname.len < 0) {
            BURROW_OUT(err, errors_new(error_allocator(), corrupt));
            return false;
        }
        idx += 46 + namelen + xlen + fclen;
        if (!str_eq(zname, name))
            continue;
        if (meth != 0) {
            BURROW_OUT(err, tzdata_err(BURROW_S("unsupported compression for "), name,
                                       BURROW_S(" in embedded tzdata")));
            return false;
        }

        /* See time.loadTzinfoFromZip for zip per-file header layout. */
        idx = off;
        Str hname = tzdata_slice(z, idx + 30, namelen);
        if (tzdata_get4(z, idx) != ZHEADER || tzdata_get2(z, idx + 8) != meth ||
            tzdata_get2(z, idx + 26) != namelen || hname.len < 0 ||
            !str_eq(hname, name)) {
            BURROW_OUT(err, errors_new(error_allocator(), corrupt));
            return false;
        }
        xlen = tzdata_get2(z, idx + 28);
        idx += ZHEADERSIZE + namelen + xlen;
        Str d = tzdata_slice(z, idx, size);
        if (d.len < 0) {
            BURROW_OUT(err, errors_new(error_allocator(), corrupt));
            return false;
        }
        *data = d;
        return true;
    }
    return false;
}

const TzEmbedded burrow__tzdata_embedded = {tzdata_load};

void tzdata_register(void) {
    burrow__time_register_embedded(&burrow__tzdata_embedded);
}
