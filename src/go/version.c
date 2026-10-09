/* go/version: comparing Go versions.
 *
 * Go's go/version keeps the parsing in internal/gover, which the go command
 * shares. Nothing else here needs that package, so its parser lives in this
 * file. Every part of a parsed version is a view of the string it came from.
 *
 * Derived from Go's src/go/version/version.go and src/internal/gover/gover.go.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/version.h"

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/strings.h"

#include <string.h>

/* gover.Version. The zero value, every part empty, is what an invalid version
 * parses to. */
typedef struct GvVersion {
    Str major; /* decimal */
    Str minor; /* decimal or "" */
    Str patch; /* decimal or "" */
    Str kind;  /* "", "alpha", "beta", "rc" */
    Str pre;   /* decimal or "" */
} GvVersion;

static Str gv_sub(Str s, Int i, Int j) {
    return str_from_bytes(s.p + i, j - i);
}

static bool gv_is_zero(const GvVersion *v) {
    return v->major.len == 0 && v->minor.len == 0 && v->patch.len == 0 &&
           v->kind.len == 0 && v->pre.len == 0;
}

/* gover.cutInt: the decimal at the front of x and what follows it. A leading
 * zero is only allowed on "0" itself. */
static bool gv_cut_int(Str x, Str *n, Str *rest) {
    Int i = 0;
    while (i < x.len && '0' <= x.p[i] && x.p[i] <= '9')
        i++;
    if (i == 0 || (x.p[0] == '0' && i != 1))
        return false;
    *n = gv_sub(x, 0, i);
    *rest = gv_sub(x, i, x.len);
    return true;
}

/* gover.CmpInt: two decimals without leading zeros compare by length first. */
static Int gv_cmp_int(Str x, Str y) {
    if (str_eq(x, y))
        return 0;
    if (x.len < y.len)
        return -1;
    if (x.len > y.len)
        return +1;
    return str_cmp(x, y) < 0 ? -1 : +1;
}

/* gover.Parse. */
static GvVersion gv_parse(Str x) {
    GvVersion v = {0};
    const GvVersion zero = {0};

    /* Parse major version. */
    if (!gv_cut_int(x, &v.major, &x))
        return zero;
    if (x.len == 0) {
        /* Interpret "1" as "1.0.0". */
        v.minor = BURROW_S("0");
        v.patch = BURROW_S("0");
        return v;
    }

    /* Parse . before minor version. */
    if (x.p[0] != '.')
        return zero;

    /* Parse minor version. */
    if (!gv_cut_int(gv_sub(x, 1, x.len), &v.minor, &x))
        return zero;
    if (x.len == 0) {
        /* Patch missing is same as "0" for older versions. Starting in Go
         * 1.21, patch missing is different from explicit .0. */
        if (gv_cmp_int(v.minor, BURROW_S("21")) < 0)
            v.patch = BURROW_S("0");
        return v;
    }

    /* Parse patch if present. Prereleases of a patch release are not
     * allowed, because 1.21 < 1.21rc1 but 1.21.3rc1 would be < 1.21.3. */
    if (x.p[0] == '.') {
        if (!gv_cut_int(gv_sub(x, 1, x.len), &v.patch, &x) || x.len != 0)
            return zero;
        return v;
    }

    /* Parse prerelease. */
    Int i = 0;
    while (i < x.len && (x.p[i] < '0' || '9' < x.p[i])) {
        if (x.p[i] < 'a' || 'z' < x.p[i])
            return zero;
        i++;
    }
    if (i == 0)
        return zero;
    v.kind = gv_sub(x, 0, i);
    x = gv_sub(x, i, x.len);
    if (x.len == 0)
        return v;
    if (!gv_cut_int(x, &v.pre, &x) || x.len != 0)
        return zero;
    return v;
}

/* stripGo: "go1.21-custom" becomes "1.21". Without the "go" the result is
 * empty, which is a known invalid version. */
static Str gv_strip_go(Str v) {
    v = strings_cut(v, BURROW_S("-"), NULL, NULL); /* strip -custom suffix */
    if (v.len < 2 || v.p[0] != 'g' || v.p[1] != 'o')
        return BURROW_STR_EMPTY;
    return gv_sub(v, 2, v.len);
}

Str version_lang(Alloc *a, Str x) {
    /* gover.Lang */
    GvVersion v = gv_parse(gv_strip_go(x));
    if (v.minor.len == 0 ||
        (str_eq(v.major, BURROW_S("1")) && str_eq(v.minor, BURROW_S("0")))) {
        if (v.major.len == 0)
            return BURROW_STR_EMPTY;
        return gv_sub(x, 0, 2 + v.major.len);
    }
    /* "go" and the language version are the front of x, unless the parser
     * filled in the minor, which it does for a bare major like "go222". */
    Int n = v.major.len + 1 + v.minor.len;
    Str rest = gv_sub(x, 2 + v.major.len, x.len);
    if (rest.len > 0 && rest.p[0] == '.' &&
        strings_has_prefix(gv_sub(rest, 1, rest.len), v.minor))
        return gv_sub(x, 0, 2 + n);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(2 + n), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, "go", 2);
    memcpy(p + 2, v.major.p, (size_t)v.major.len);
    p[2 + v.major.len] = '.';
    memcpy(p + 3 + v.major.len, v.minor.p, (size_t)v.minor.len);
    return str_from_bytes(p, 2 + n);
}

Int version_compare(Str x, Str y) {
    /* gover.Compare */
    GvVersion vx = gv_parse(gv_strip_go(x));
    GvVersion vy = gv_parse(gv_strip_go(y));
    Int c = gv_cmp_int(vx.major, vy.major);
    if (c != 0)
        return c;
    c = gv_cmp_int(vx.minor, vy.minor);
    if (c != 0)
        return c;
    c = gv_cmp_int(vx.patch, vy.patch);
    if (c != 0)
        return c;
    c = str_cmp(vx.kind, vy.kind); /* "" < alpha < beta < rc */
    if (c != 0)
        return c < 0 ? -1 : +1;
    return gv_cmp_int(vx.pre, vy.pre);
}

bool version_is_valid(Str x) {
    GvVersion v = gv_parse(gv_strip_go(x));
    return !gv_is_zero(&v);
}
