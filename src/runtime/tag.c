/* Struct tags: getting json:"name,omitempty" back out of a field.
 *
 * A tag is one string on the field and it holds several things at once, one per
 * package that cares. Go's convention, which this follows exactly, is a
 * space separated list of key:"value" pairs:
 *
 *     json:"id,omitempty" xml:"id,attr" db:"user_id"
 *
 * encoding/json reads the json one, encoding/xml reads the xml one, and neither
 * of them has to know the other is there. Nothing validates the format, on
 * purpose and the same way Go does not: a tag that does not parse is a tag
 * nobody finds a key in, rather than an error at a point where there is nothing
 * useful to do with one.
 *
 * WHY THE VALUE IS COPIED
 *
 * Go returns a slice of the tag itself when the value has no escapes in it, and
 * it can, because a Go string is immutable and the tag lives as long as the
 * program. burrow copies, always, and takes an allocator to do it.
 *
 * The reason is that the alternative is a function that sometimes owns what it
 * returns and sometimes borrows it, decided by whether the tag happened to
 * contain a backslash. There is no way to annotate that, no way for a caller to
 * know which it got, and the whole ownership scheme in this library rests on
 * that question having one answer per function. A copy of a short string, on an
 * arena, in code that caches its answer per type anyway, is not worth the hole
 * that would put in it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/type.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/strconv.h"

/* ------------------------------------------------------------- the lookup */

bool tag_lookup(Alloc *a, Str tag, Str key, Str *value) {
    if (value != NULL)
        *value = BURROW_STR_EMPTY;

    Int i = 0;

    while (i < tag.len) {
        /* Leading spaces, and only spaces. Go is strict about this and a tab
         * between two pairs ends the scan. */
        while (i < tag.len && tag.p[i] == ' ')
            i++;
        if (i >= tag.len)
            break;

        /* The key, up to the colon. A space, a quote or a control character in
         * here means the tag is not in the format, so stop rather than guess. */
        Int name_start = i;
        while (i < tag.len && (Byte)tag.p[i] > ' ' && tag.p[i] != ':' &&
               tag.p[i] != '"' && (Byte)tag.p[i] != 0x7f)
            i++;

        if (i == name_start || i + 1 >= tag.len || tag.p[i] != ':' ||
            tag.p[i + 1] != '"')
            break;

        Str name = str_from_bytes(tag.p + name_start, i - name_start);
        i++; /* now on the opening quote */

        /* The quoted value, skipping whatever a backslash escapes so that a
         * quote inside the literal does not end it. */
        Int quote_start = i;
        Int j = i + 1;
        while (j < tag.len && tag.p[j] != '"') {
            if (tag.p[j] == '\\')
                j++;
            j++;
        }
        if (j >= tag.len)
            break;

        Str quoted = str_from_bytes(tag.p + quote_start, j + 1 - quote_start);
        i = j + 1;

        if (!str_eq(name, key))
            continue;

        /* The value is a Go string literal, so strconv reads it. A literal
         * with nothing to undo comes back as a view of the tag, and it is
         * copied so that what the caller gets is always theirs. */
        Error err;
        Str v = strconv_unquote(a, quoted, &err);
        if (BURROW_FAILED(err))
            break;

        /* Present and empty, which is not the same as absent, so this is
         * still true. Nothing is allocated for it. */
        if (v.len > 0 && v.p > quoted.p && v.p < quoted.p + quoted.len) {
            v = str_clone(a, v);
            if (v.p == NULL)
                return false;
        }

        if (value != NULL)
            *value = v;

        return true;
    }

    return false;
}

Str tag_get(Alloc *a, Str tag, Str key) {
    Str value = BURROW_STR_EMPTY;

    (void)tag_lookup(a, tag, key, &value);
    return value;
}
