/* Derived from Go's src/net/http/cookiejar/jar_test.go and punycode_test.go.
 * Go source: go1.27.1.
 *
 * The tables are Go's, turned into C by a script, with "{expires:N}" standing
 * for Go's expiresIn(N), which run fills in.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/cookiejar_internal.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/slices.h"
#include "burrow/strings.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define S BURROW_S

typedef burrow__CookiejarEntry Entry;

static Str cs(const char *s) {
    return s != NULL ? str_from_cstr(s) : (Str){0};
}

/* tNow, the made up time the tests take to be now. */
static Time t_now(void) {
    return time_date(2013, TIME_JANUARY, 1, 12, 0, 0, 0, time_utc_loc);
}

/* testPSL, a public suffix list with two rules, "co.uk" and the default "*".
 * It has two bugs on purpose: the suffix of "www.buggy.psl" is "xy" and that
 * of "www2.buggy.psl" is "com". */
static Str test_psl_public_suffix(void *self, Str d) {
    (void)self;
    if (str_eq(d, S("co.uk")) || strings_has_suffix(d, S(".co.uk")))
        return S("co.uk");
    if (str_eq(d, S("www.buggy.psl")))
        return S("xy");
    if (str_eq(d, S("www2.buggy.psl")))
        return S("com");
    Int i = strings_last_index(d, S("."));
    return str_from_bytes(d.p + i + 1, d.len - i - 1);
}

static Str test_psl_string(void *self, Alloc *a) {
    (void)self;
    (void)a;
    return S("testPSL");
}

static const CookiejarPublicSuffixListVT test_psl_vt = {NULL, test_psl_public_suffix,
                                                        test_psl_string};

static CookiejarPublicSuffixList test_psl(void) {
    return (CookiejarPublicSuffixList){&test_psl_vt, NULL};
}

/* newTestJar, an empty jar with testPSL. */
static CookiejarJar *new_test_jar(Alloc *a) {
    CookiejarOptions o = {test_psl()};
    Error err;
    CookiejarJar *j = cookiejar_new(a, &o, &err);
    if (j == NULL)
        panic_str(S("cookiejar_new failed"));
    return j;
}

typedef struct StrPair {
    const char *a;
    const char *b;
} StrPair;

typedef struct StrBool {
    const char *s;
    bool want;
} StrBool;

enum { NO_ERR, ERR_MALFORMED, ERR_ILLEGAL };

typedef struct DomainAndTypeTest {
    const char *host;        /* where the Set-Cookie header came from */
    const char *domain;      /* the domain attribute in it */
    const char *want_domain; /* the domain the cookie should get */
    bool want_host_only;     /* whether it should be a host cookie */
    int want_err;
} DomainAndTypeTest;

/* query, one call of Cookies and the cookies it should give, in order. */
typedef struct JarQuery {
    const char *to_url;
    const char *want;
} JarQuery;

/* jarTest, which does three things to a jar:
 *  1. SetCookies with from_url and the cookies in set_cookies, at tNow.
 *  2. Checks that what the jar holds is content, at tNow + 1001 ms.
 *  3. Checks that Cookies with each to_url in queries gives its want, query n
 *     at tNow + (n+2)*1001 ms. */
typedef struct JarTest {
    const char *description;
    const char *from_url;
    const char *set_cookies[16];
    const char *content;
    JarQuery queries[13];
} JarTest;

static const JarTest basics_tests[] = {
    {
        "Retrieval of a plain host cookie.",
        "http://www.host.test/",
        {"A=a"},
        "A=a",
        {
            {"http://www.host.test", "A=a"},
            {"http://www.host.test/", "A=a"},
            {"http://www.host.test/some/path", "A=a"},
            {"https://www.host.test", "A=a"},
            {"https://www.host.test/", "A=a"},
            {"https://www.host.test/some/path", "A=a"},
            {"ftp://www.host.test", ""},
            {"ftp://www.host.test/", ""},
            {"ftp://www.host.test/some/path", ""},
            {"http://www.other.org", ""},
            {"http://sibling.host.test", ""},
            {"http://deep.www.host.test", ""},
        },
    },
    {
        "Secure cookies are not returned to http.",
        "http://www.host.test/",
        {"A=a; secure"},
        "A=a",
        {
            {"http://www.host.test", ""},
            {"http://www.host.test/", ""},
            {"http://www.host.test/some/path", ""},
            {"https://www.host.test", "A=a"},
            {"https://www.host.test/", "A=a"},
            {"https://www.host.test/some/path", "A=a"},
        },
    },
    {
        "Secure cookies are sent for localhost",
        "http://localhost:8910/",
        {"A=a; secure"},
        "A=a",
        {
            {"http://localhost:8910", "A=a"},
            {"http://localhost:8910/", "A=a"},
            {"http://localhost:8910/some/path", "A=a"},
            {"https://localhost:8910", "A=a"},
            {"https://localhost:8910/", "A=a"},
            {"https://localhost:8910/some/path", "A=a"},
        },
    },
    {
        "Secure cookies are sent for localhost (tld)",
        "http://example.LOCALHOST:8910/",
        {"A=a; secure"},
        "A=a",
        {
            {"http://example.LOCALHOST:8910", "A=a"},
            {"http://example.LOCALHOST:8910/", "A=a"},
            {"http://example.LOCALHOST:8910/some/path", "A=a"},
            {"https://example.LOCALHOST:8910", "A=a"},
            {"https://example.LOCALHOST:8910/", "A=a"},
            {"https://example.LOCALHOST:8910/some/path", "A=a"},
        },
    },
    {
        "Secure cookies are sent for localhost (ipv6)",
        "http://[::1]:8910/",
        {"A=a; secure"},
        "A=a",
        {
            {"http://[::1]:8910", "A=a"},
            {"http://[::1]:8910/", "A=a"},
            {"http://[::1]:8910/some/path", "A=a"},
            {"https://[::1]:8910", "A=a"},
            {"https://[::1]:8910/", "A=a"},
            {"https://[::1]:8910/some/path", "A=a"},
        },
    },
    {
        "Localhost only if it's a segment",
        "http://notlocalhost/",
        {"A=a; secure"},
        "A=a",
        {
            {"http://notlocalhost", ""},
            {"http://notlocalhost/", ""},
            {"http://notlocalhost/some/path", ""},
            {"https://notlocalhost", "A=a"},
            {"https://notlocalhost/", "A=a"},
            {"https://notlocalhost/some/path", "A=a"},
        },
    },
    {
        "Explicit path.",
        "http://www.host.test/",
        {"A=a; path=/some/path"},
        "A=a",
        {
            {"http://www.host.test", ""},
            {"http://www.host.test/", ""},
            {"http://www.host.test/some", ""},
            {"http://www.host.test/some/", ""},
            {"http://www.host.test/some/path", "A=a"},
            {"http://www.host.test/some/paths", ""},
            {"http://www.host.test/some/path/foo", "A=a"},
            {"http://www.host.test/some/path/foo/", "A=a"},
        },
    },
    {
        "Implicit path #1: path is a directory.",
        "http://www.host.test/some/path/",
        {"A=a"},
        "A=a",
        {
            {"http://www.host.test", ""},
            {"http://www.host.test/", ""},
            {"http://www.host.test/some", ""},
            {"http://www.host.test/some/", ""},
            {"http://www.host.test/some/path", "A=a"},
            {"http://www.host.test/some/paths", ""},
            {"http://www.host.test/some/path/foo", "A=a"},
            {"http://www.host.test/some/path/foo/", "A=a"},
        },
    },
    {
        "Implicit path #2: path is not a directory.",
        "http://www.host.test/some/path/index.html",
        {"A=a"},
        "A=a",
        {
            {"http://www.host.test", ""},
            {"http://www.host.test/", ""},
            {"http://www.host.test/some", ""},
            {"http://www.host.test/some/", ""},
            {"http://www.host.test/some/path", "A=a"},
            {"http://www.host.test/some/paths", ""},
            {"http://www.host.test/some/path/foo", "A=a"},
            {"http://www.host.test/some/path/foo/", "A=a"},
        },
    },
    {
        "Implicit path #3: no path in URL at all.",
        "http://www.host.test",
        {"A=a"},
        "A=a",
        {
            {"http://www.host.test", "A=a"},
            {"http://www.host.test/", "A=a"},
            {"http://www.host.test/some/path", "A=a"},
        },
    },
    {
        "Cookies are sorted by path length.",
        "http://www.host.test/",
        {"A=a; path=/foo/bar", "B=b; path=/foo/bar/baz/qux", "C=c; path=/foo/bar/baz",
         "D=d; path=/foo"},
        "A=a B=b C=c D=d",
        {
            {"http://www.host.test/foo/bar/baz/qux", "B=b C=c A=a D=d"},
            {"http://www.host.test/foo/bar/baz/", "C=c A=a D=d"},
            {"http://www.host.test/foo/bar", "A=a D=d"},
        },
    },
    {
        "Creation time determines sorting on same length paths.",
        "http://www.host.test/",
        {"A=a; path=/foo/bar", "X=x; path=/foo/bar", "Y=y; path=/foo/bar/baz/qux",
         "B=b; path=/foo/bar/baz/qux", "C=c; path=/foo/bar/baz",
         "W=w; path=/foo/bar/baz", "Z=z; path=/foo", "D=d; path=/foo"},
        "A=a B=b C=c D=d W=w X=x Y=y Z=z",
        {
            {"http://www.host.test/foo/bar/baz/qux", "Y=y B=b C=c W=w A=a X=x Z=z D=d"},
            {"http://www.host.test/foo/bar/baz/", "C=c W=w A=a X=x Z=z D=d"},
            {"http://www.host.test/foo/bar", "A=a X=x Z=z D=d"},
        },
    },
    {
        "Sorting of same-name cookies.",
        "http://www.host.test/",
        {"A=1; path=/", "A=2; path=/path", "A=3; path=/quux", "A=4; path=/path/foo",
         "A=5; domain=.host.test; path=/path", "A=6; domain=.host.test; path=/quux",
         "A=7; domain=.host.test; path=/path/foo"},
        "A=1 A=2 A=3 A=4 A=5 A=6 A=7",
        {
            {"http://www.host.test/path", "A=2 A=5 A=1"},
            {"http://www.host.test/path/foo", "A=4 A=7 A=2 A=5 A=1"},
        },
    },
    {
        "Disallow domain cookie on public suffix.",
        "http://www.bbc.co.uk",
        {"a=1", "b=2; domain=co.uk"},
        "a=1",
        {
            {"http://www.bbc.co.uk", "a=1"},
        },
    },
    {
        "Host cookie on IP.",
        "http://192.168.0.10",
        {"a=1"},
        "a=1",
        {
            {"http://192.168.0.10", "a=1"},
        },
    },
    {
        "Domain cookies on IP.",
        "http://192.168.0.10",
        {"a=1; domain=192.168.0.10", "b=2; domain=172.31.9.9",
         "c=3; domain=.192.168.0.10"},
        "a=1",
        {
            {"http://192.168.0.10", "a=1"},
            {"http://172.31.9.9", ""},
            {"http://www.fancy.192.168.0.10", ""},
        },
    },
    {
        "Port is ignored #1.",
        "http://www.host.test/",
        {"a=1"},
        "a=1",
        {
            {"http://www.host.test", "a=1"},
            {"http://www.host.test:8080/", "a=1"},
        },
    },
    {
        "Port is ignored #2.",
        "http://www.host.test:8080/",
        {"a=1"},
        "a=1",
        {
            {"http://www.host.test", "a=1"},
            {"http://www.host.test:8080/", "a=1"},
            {"http://www.host.test:1234/", "a=1"},
        },
    },
    {
        "IPv6 zone is not treated as a host.",
        "https://example.com/",
        {"a=1"},
        "a=1",
        {
            {"https://[::1%25.example.com]:80/", ""},
        },
    },
    {
        "Retrieval of cookies with quoted values",
        "http://www.host.test/",
        {"cookie-1=\"quoted\"", "cookie-2=\"quoted with spaces\"",
         "cookie-3=\"quoted,with,commas\"", "cookie-4= ,"},
        "cookie-1=\"quoted\" cookie-2=\"quoted with spaces\" "
        "cookie-3=\"quoted,with,commas\" cookie-4=\" ,\"",
        {
            {"http://www.host.test",
             "cookie-1=\"quoted\" cookie-2=\"quoted with spaces\" "
             "cookie-3=\"quoted,with,commas\" cookie-4=\" ,\""},
        },
    },
};

static const JarTest update_and_delete_tests[] = {
    {
        "Set initial cookies.",
        "http://www.host.test",
        {"a=1", "b=2; secure", "c=3; httponly", "d=4; secure; httponly"},
        "a=1 b=2 c=3 d=4",
        {
            {"http://www.host.test", "a=1 c=3"},
            {"https://www.host.test", "a=1 b=2 c=3 d=4"},
        },
    },
    {
        "Update value via http.",
        "http://www.host.test",
        {"a=w", "b=x; secure", "c=y; httponly", "d=z; secure; httponly"},
        "a=w b=x c=y d=z",
        {
            {"http://www.host.test", "a=w c=y"},
            {"https://www.host.test", "a=w b=x c=y d=z"},
        },
    },
    {
        "Clear Secure flag from an http.",
        "http://www.host.test/",
        {"b=xx", "d=zz; httponly"},
        "a=w b=xx c=y d=zz",
        {
            {"http://www.host.test", "a=w b=xx c=y d=zz"},
        },
    },
    {
        "Delete all.",
        "http://www.host.test/",
        {"a=1; max-Age=-1", "b=2; {expires:-10}", "c=2; max-age=-1; {expires:-10}",
         "d=4; max-age=-1; {expires:10}"},
        "",
        {
            {"http://www.host.test", ""},
        },
    },
    {
        "Refill #1.",
        "http://www.host.test",
        {"A=1", "A=2; path=/foo", "A=3; domain=.host.test",
         "A=4; path=/foo; domain=.host.test"},
        "A=1 A=2 A=3 A=4",
        {
            {"http://www.host.test/foo", "A=2 A=4 A=1 A=3"},
        },
    },
    {
        "Refill #2.",
        "http://www.google.com",
        {"A=6", "A=7; path=/foo", "A=8; domain=.google.com",
         "A=9; path=/foo; domain=.google.com"},
        "A=1 A=2 A=3 A=4 A=6 A=7 A=8 A=9",
        {
            {"http://www.host.test/foo", "A=2 A=4 A=1 A=3"},
            {"http://www.google.com/foo", "A=7 A=9 A=6 A=8"},
        },
    },
    {
        "Delete A7.",
        "http://www.google.com",
        {"A=; path=/foo; max-age=-1"},
        "A=1 A=2 A=3 A=4 A=6 A=8 A=9",
        {
            {"http://www.host.test/foo", "A=2 A=4 A=1 A=3"},
            {"http://www.google.com/foo", "A=9 A=6 A=8"},
        },
    },
    {
        "Delete A4.",
        "http://www.host.test",
        {"A=; path=/foo; domain=host.test; max-age=-1"},
        "A=1 A=2 A=3 A=6 A=8 A=9",
        {
            {"http://www.host.test/foo", "A=2 A=1 A=3"},
            {"http://www.google.com/foo", "A=9 A=6 A=8"},
        },
    },
    {
        "Delete A6.",
        "http://www.google.com",
        {"A=; max-age=-1"},
        "A=1 A=2 A=3 A=8 A=9",
        {
            {"http://www.host.test/foo", "A=2 A=1 A=3"},
            {"http://www.google.com/foo", "A=9 A=8"},
        },
    },
    {
        "Delete A3.",
        "http://www.host.test",
        {"A=; domain=host.test; max-age=-1"},
        "A=1 A=2 A=8 A=9",
        {
            {"http://www.host.test/foo", "A=2 A=1"},
            {"http://www.google.com/foo", "A=9 A=8"},
        },
    },
    {
        "No cross-domain delete.",
        "http://www.host.test",
        {"A=; domain=google.com; max-age=-1",
         "A=; path=/foo; domain=google.com; max-age=-1"},
        "A=1 A=2 A=8 A=9",
        {
            {"http://www.host.test/foo", "A=2 A=1"},
            {"http://www.google.com/foo", "A=9 A=8"},
        },
    },
    {
        "Delete A8 and A9.",
        "http://www.google.com",
        {"A=; domain=google.com; max-age=-1",
         "A=; path=/foo; domain=google.com; max-age=-1"},
        "A=1 A=2",
        {
            {"http://www.host.test/foo", "A=2 A=1"},
            {"http://www.google.com/foo", ""},
        },
    },
};

static const JarTest chromium_basics_tests[] = {
    {
        "DomainWithTrailingDotTest.",
        "http://www.google.com/",
        {"a=1; domain=.www.google.com.", "b=2; domain=.www.google.com.."},
        "",
        {
            {"http://www.google.com", ""},
        },
    },
    {
        "ValidSubdomainTest #1.",
        "http://a.b.c.d.com",
        {"a=1; domain=.a.b.c.d.com", "b=2; domain=.b.c.d.com", "c=3; domain=.c.d.com",
         "d=4; domain=.d.com"},
        "a=1 b=2 c=3 d=4",
        {
            {"http://a.b.c.d.com", "a=1 b=2 c=3 d=4"},
            {"http://b.c.d.com", "b=2 c=3 d=4"},
            {"http://c.d.com", "c=3 d=4"},
            {"http://d.com", "d=4"},
        },
    },
    {
        "ValidSubdomainTest #2.",
        "http://a.b.c.d.com",
        {"a=1; domain=.a.b.c.d.com", "b=2; domain=.b.c.d.com", "c=3; domain=.c.d.com",
         "d=4; domain=.d.com", "X=bcd; domain=.b.c.d.com", "X=cd; domain=.c.d.com"},
        "X=bcd X=cd a=1 b=2 c=3 d=4",
        {
            {"http://b.c.d.com", "b=2 c=3 d=4 X=bcd X=cd"},
            {"http://c.d.com", "c=3 d=4 X=cd"},
        },
    },
    {
        "InvalidDomainTest #1.",
        "http://foo.bar.com",
        {"a=1; domain=.yo.foo.bar.com", "b=2; domain=.foo.com",
         "c=3; domain=.bar.foo.com", "d=4; domain=.foo.bar.com.net",
         "e=5; domain=ar.com", "f=6; domain=.", "g=7; domain=/",
         "h=8; domain=http://foo.bar.com", "i=9; domain=..foo.bar.com",
         "j=10; domain=..bar.com", "k=11; domain=.foo.bar.com\?blah",
         "l=12; domain=.foo.bar.com/blah", "m=12; domain=.foo.bar.com:80",
         "n=14; domain=.foo.bar.com:", "o=15; domain=.foo.bar.com#sup"},
        "",
        {
            {"http://foo.bar.com", ""},
        },
    },
    {
        "InvalidDomainTest #2.",
        "http://foo.com.com",
        {"a=1; domain=.foo.com.com.com"},
        "",
        {
            {"http://foo.bar.com", ""},
        },
    },
    {
        "DomainWithoutLeadingDotTest #1.",
        "http://manage.hosted.filefront.com",
        {"a=1; domain=filefront.com"},
        "a=1",
        {
            {"http://www.filefront.com", "a=1"},
        },
    },
    {
        "DomainWithoutLeadingDotTest #2.",
        "http://www.google.com",
        {"a=1; domain=www.google.com"},
        "a=1",
        {
            {"http://www.google.com", "a=1"},
            {"http://sub.www.google.com", "a=1"},
            {"http://something-else.com", ""},
        },
    },
    {
        "CaseInsensitiveDomainTest.",
        "http://www.google.com",
        {"a=1; domain=.GOOGLE.COM", "b=2; domain=.www.gOOgLE.coM"},
        "a=1 b=2",
        {
            {"http://www.google.com", "a=1 b=2"},
        },
    },
    {
        "TestIpAddress #1.",
        "http://1.2.3.4/foo",
        {"a=1; path=/"},
        "a=1",
        {
            {"http://1.2.3.4/foo", "a=1"},
        },
    },
    {
        "TestIpAddress #2.",
        "http://1.2.3.4/foo",
        {"a=1; domain=.1.2.3.4", "b=2; domain=.3.4"},
        "",
        {
            {"http://1.2.3.4/foo", ""},
        },
    },
    {
        "TestIpAddress #3.",
        "http://1.2.3.4/foo",
        {"a=1; domain=1.2.3.3"},
        "",
        {
            {"http://1.2.3.4/foo", ""},
        },
    },
    {
        "TestIpAddress #4.",
        "http://1.2.3.4/foo",
        {"a=1; domain=1.2.3.4"},
        "a=1",
        {
            {"http://1.2.3.4/foo", "a=1"},
        },
    },
    {
        "TestNonDottedAndTLD #2.",
        "http://com./index.html",
        {"a=1"},
        "a=1",
        {
            {"http://com./index.html", "a=1"},
            {"http://no-cookies.com./index.html", ""},
        },
    },
    {
        "TestNonDottedAndTLD #3.",
        "http://a.b",
        {"a=1; domain=.b", "b=2; domain=b"},
        "",
        {
            {"http://bar.foo", ""},
        },
    },
    {
        "TestNonDottedAndTLD #4.",
        "http://google.com",
        {"a=1; domain=.com", "b=2; domain=com"},
        "",
        {
            {"http://google.com", ""},
        },
    },
    {
        "TestNonDottedAndTLD #5.",
        "http://google.co.uk",
        {"a=1; domain=.co.uk", "b=2; domain=.uk"},
        "",
        {
            {"http://google.co.uk", ""},
            {"http://else.co.com", ""},
            {"http://else.uk", ""},
        },
    },
    {
        "TestHostEndsWithDot.",
        "http://www.google.com",
        {"a=1", "b=2; domain=.www.google.com."},
        "a=1",
        {
            {"http://www.google.com", "a=1"},
        },
    },
    {
        "PathTest",
        "http://www.google.izzle",
        {"a=1; path=/wee"},
        "a=1",
        {
            {"http://www.google.izzle/wee", "a=1"},
            {"http://www.google.izzle/wee/", "a=1"},
            {"http://www.google.izzle/wee/war", "a=1"},
            {"http://www.google.izzle/wee/war/more/more", "a=1"},
            {"http://www.google.izzle/weehee", ""},
            {"http://www.google.izzle/", ""},
        },
    },
};

static const JarTest chromium_domain_tests[] = {
    {
        "Fill #1.",
        "http://www.google.izzle",
        {"A=B"},
        "A=B",
        {
            {"http://www.google.izzle", "A=B"},
        },
    },
    {
        "Fill #2.",
        "http://www.google.izzle",
        {"C=D; domain=.google.izzle"},
        "A=B C=D",
        {
            {"http://www.google.izzle", "A=B C=D"},
        },
    },
    {
        "Verify A is a host cookie and not accessible from subdomain.",
        "http://unused.nil",
        {NULL},
        "A=B C=D",
        {
            {"http://foo.www.google.izzle", "C=D"},
        },
    },
    {
        "Verify domain cookies are found on proper domain.",
        "http://www.google.izzle",
        {"E=F; domain=.www.google.izzle"},
        "A=B C=D E=F",
        {
            {"http://www.google.izzle", "A=B C=D E=F"},
        },
    },
    {
        "Leading dots in domain attributes are optional.",
        "http://www.google.izzle",
        {"G=H; domain=www.google.izzle"},
        "A=B C=D E=F G=H",
        {
            {"http://www.google.izzle", "A=B C=D E=F G=H"},
        },
    },
    {
        "Verify domain enforcement works #1.",
        "http://www.google.izzle",
        {"K=L; domain=.bar.www.google.izzle"},
        "A=B C=D E=F G=H",
        {
            {"http://bar.www.google.izzle", "C=D E=F G=H"},
        },
    },
    {
        "Verify domain enforcement works #2.",
        "http://unused.nil",
        {NULL},
        "A=B C=D E=F G=H",
        {
            {"http://www.google.izzle", "A=B C=D E=F G=H"},
        },
    },
};

static const JarTest chromium_deletion_tests[] = {
    {
        "Create session cookie a1.",
        "http://www.google.com",
        {"a=1"},
        "a=1",
        {
            {"http://www.google.com", "a=1"},
        },
    },
    {
        "Delete sc a1 via MaxAge.",
        "http://www.google.com",
        {"a=1; max-age=-1"},
        "",
        {
            {"http://www.google.com", ""},
        },
    },
    {
        "Create session cookie b2.",
        "http://www.google.com",
        {"b=2"},
        "b=2",
        {
            {"http://www.google.com", "b=2"},
        },
    },
    {
        "Delete sc b2 via Expires.",
        "http://www.google.com",
        {"b=2; {expires:-10}"},
        "",
        {
            {"http://www.google.com", ""},
        },
    },
    {
        "Create persistent cookie c3.",
        "http://www.google.com",
        {"c=3; max-age=3600"},
        "c=3",
        {
            {"http://www.google.com", "c=3"},
        },
    },
    {
        "Delete pc c3 via MaxAge.",
        "http://www.google.com",
        {"c=3; max-age=-1"},
        "",
        {
            {"http://www.google.com", ""},
        },
    },
    {
        "Create persistent cookie d4.",
        "http://www.google.com",
        {"d=4; max-age=3600"},
        "d=4",
        {
            {"http://www.google.com", "d=4"},
        },
    },
    {
        "Delete pc d4 via Expires.",
        "http://www.google.com",
        {"d=4; {expires:-10}"},
        "",
        {
            {"http://www.google.com", ""},
        },
    },
};

static const JarTest domain_handling_tests[] = {
    {
        "Host cookie",
        "http://www.host.test",
        {"a=1"},
        "a=1",
        {
            {"http://www.host.test", "a=1"},
            {"http://host.test", ""},
            {"http://bar.host.test", ""},
            {"http://foo.www.host.test", ""},
            {"http://other.test", ""},
            {"http://test", ""},
        },
    },
    {
        "Domain cookie #1",
        "http://www.host.test",
        {"a=1; domain=host.test"},
        "a=1",
        {
            {"http://www.host.test", "a=1"},
            {"http://host.test", "a=1"},
            {"http://bar.host.test", "a=1"},
            {"http://foo.www.host.test", "a=1"},
            {"http://other.test", ""},
            {"http://test", ""},
        },
    },
    {
        "Domain cookie #2",
        "http://www.host.test",
        {"a=1; domain=.host.test"},
        "a=1",
        {
            {"http://www.host.test", "a=1"},
            {"http://host.test", "a=1"},
            {"http://bar.host.test", "a=1"},
            {"http://foo.www.host.test", "a=1"},
            {"http://other.test", ""},
            {"http://test", ""},
        },
    },
    {
        "Host cookie on IDNA domain #1",
        "http://www.b\303\274cher.test",
        {"a=1"},
        "a=1",
        {
            {"http://www.b\303\274cher.test", "a=1"},
            {"http://www.xn--bcher-kva.test", "a=1"},
            {"http://b\303\274cher.test", ""},
            {"http://xn--bcher-kva.test", ""},
            {"http://bar.b\303\274cher.test", ""},
            {"http://bar.xn--bcher-kva.test", ""},
            {"http://foo.www.b\303\274cher.test", ""},
            {"http://foo.www.xn--bcher-kva.test", ""},
            {"http://other.test", ""},
            {"http://test", ""},
        },
    },
    {
        "Host cookie on IDNA domain #2",
        "http://www.xn--bcher-kva.test",
        {"a=1"},
        "a=1",
        {
            {"http://www.b\303\274cher.test", "a=1"},
            {"http://www.xn--bcher-kva.test", "a=1"},
            {"http://b\303\274cher.test", ""},
            {"http://xn--bcher-kva.test", ""},
            {"http://bar.b\303\274cher.test", ""},
            {"http://bar.xn--bcher-kva.test", ""},
            {"http://foo.www.b\303\274cher.test", ""},
            {"http://foo.www.xn--bcher-kva.test", ""},
            {"http://other.test", ""},
            {"http://test", ""},
        },
    },
    {
        "Domain cookie on IDNA domain #1",
        "http://www.b\303\274cher.test",
        {"a=1; domain=xn--bcher-kva.test"},
        "a=1",
        {
            {"http://www.b\303\274cher.test", "a=1"},
            {"http://www.xn--bcher-kva.test", "a=1"},
            {"http://b\303\274cher.test", "a=1"},
            {"http://xn--bcher-kva.test", "a=1"},
            {"http://bar.b\303\274cher.test", "a=1"},
            {"http://bar.xn--bcher-kva.test", "a=1"},
            {"http://foo.www.b\303\274cher.test", "a=1"},
            {"http://foo.www.xn--bcher-kva.test", "a=1"},
            {"http://other.test", ""},
            {"http://test", ""},
        },
    },
    {
        "Domain cookie on IDNA domain #2",
        "http://www.xn--bcher-kva.test",
        {"a=1; domain=xn--bcher-kva.test"},
        "a=1",
        {
            {"http://www.b\303\274cher.test", "a=1"},
            {"http://www.xn--bcher-kva.test", "a=1"},
            {"http://b\303\274cher.test", "a=1"},
            {"http://xn--bcher-kva.test", "a=1"},
            {"http://bar.b\303\274cher.test", "a=1"},
            {"http://bar.xn--bcher-kva.test", "a=1"},
            {"http://foo.www.b\303\274cher.test", "a=1"},
            {"http://foo.www.xn--bcher-kva.test", "a=1"},
            {"http://other.test", ""},
            {"http://test", ""},
        },
    },
    {
        "Host cookie on TLD.",
        "http://com",
        {"a=1"},
        "a=1",
        {
            {"http://com", "a=1"},
            {"http://any.com", ""},
            {"http://any.test", ""},
        },
    },
    {
        "Domain cookie on TLD becomes a host cookie.",
        "http://com",
        {"a=1; domain=com"},
        "a=1",
        {
            {"http://com", "a=1"},
            {"http://any.com", ""},
            {"http://any.test", ""},
        },
    },
    {
        "Host cookie on public suffix.",
        "http://co.uk",
        {"a=1"},
        "a=1",
        {
            {"http://co.uk", "a=1"},
            {"http://uk", ""},
            {"http://some.co.uk", ""},
            {"http://foo.some.co.uk", ""},
            {"http://any.uk", ""},
        },
    },
    {
        "Domain cookie on public suffix is ignored.",
        "http://some.co.uk",
        {"a=1; domain=co.uk"},
        "",
        {
            {"http://co.uk", ""},
            {"http://uk", ""},
            {"http://some.co.uk", ""},
            {"http://foo.some.co.uk", ""},
            {"http://any.uk", ""},
        },
    },
};

static const JarTest expiration_test = {
    "Expiration.",
    "http://www.host.test",
    {"a=1", "b=2; max-age=3", "c=3; {expires:3}", "d=4; max-age=5", "e=5; {expires:5}",
     "f=6; max-age=100"},
    "a=1 b=2 c=3 d=4 e=5 f=6",
    {
        {"http://www.host.test", "a=1 b=2 c=3 d=4 e=5 f=6"},
        {"http://www.host.test", "a=1 d=4 e=5 f=6"},
        {"http://www.host.test", "a=1 d=4 e=5 f=6"},
        {"http://www.host.test", "a=1 f=6"},
        {"http://www.host.test", "a=1 f=6"},
    },
};

static const StrPair has_dot_suffix_tests[] = {
    {"", ""},
    {"", "."},
    {"", "x"},
    {".", ""},
    {".", "."},
    {".", ".."},
    {".", "x"},
    {".", "x."},
    {".", ".x"},
    {".", ".x."},
    {"x", ""},
    {"x", "."},
    {"x", ".."},
    {"x", "x"},
    {"x", "x."},
    {"x", ".x"},
    {"x", ".x."},
    {".x", ""},
    {".x", "."},
    {".x", ".."},
    {".x", "x"},
    {".x", "x."},
    {".x", ".x"},
    {".x", ".x."},
    {"x.", ""},
    {"x.", "."},
    {"x.", ".."},
    {"x.", "x"},
    {"x.", "x."},
    {"x.", ".x"},
    {"x.", ".x."},
    {"com", ""},
    {"com", "m"},
    {"com", "om"},
    {"com", "com"},
    {"com", ".com"},
    {"com", "x.com"},
    {"com", "xcom"},
    {"com", "xorg"},
    {"com", "org"},
    {"com", "rg"},
    {"foo.com", ""},
    {"foo.com", "m"},
    {"foo.com", "om"},
    {"foo.com", "com"},
    {"foo.com", ".com"},
    {"foo.com", "o.com"},
    {"foo.com", "oo.com"},
    {"foo.com", "foo.com"},
    {"foo.com", ".foo.com"},
    {"foo.com", "x.foo.com"},
    {"foo.com", "xfoo.com"},
    {"foo.com", "xfoo.org"},
    {"foo.com", "foo.org"},
    {"foo.com", "oo.org"},
    {"foo.com", "o.org"},
    {"foo.com", ".org"},
    {"foo.com", "org"},
    {"foo.com", "rg"},
};

static const StrPair canonical_host_tests[] = {
    {"www.example.com", "www.example.com"},
    {"WWW.EXAMPLE.COM", "www.example.com"},
    {"wWw.eXAmple.CoM", "www.example.com"},
    {"www.example.com:80", "www.example.com"},
    {"192.168.0.10", "192.168.0.10"},
    {"192.168.0.5:8080", "192.168.0.5"},
    {"2001:4860:0:2001::68", "2001:4860:0:2001::68"},
    {"[2001:4860:0:::68]:8080", "2001:4860:0:::68"},
    {"www.b\303\274cher.de", "www.xn--bcher-kva.de"},
    {"www.example.com.", "www.example.com"},
    {".", ""},
    {"..", "."},
    {"...", ".."},
    {".net", ".net"},
    {".net.", ".net"},
    {"a..", "a."},
    {"b.a..", "b.a."},
    {"weird.stuff...", "weird.stuff.."},
    {"[bad.unmatched.bracket:", "error"},
};

static const StrBool has_port_tests[] = {
    {"www.example.com", false},
    {"www.example.com:80", true},
    {"127.0.0.1", false},
    {"127.0.0.1:8080", true},
    {"2001:4860:0:2001::68", false},
    {"[2001::0:::68]:80", true},
};

static const StrPair jar_key_tests[] = {
    {"foo.www.example.com", "example.com"},
    {"www.example.com", "example.com"},
    {"example.com", "example.com"},
    {"com", "com"},
    {"foo.www.bbc.co.uk", "bbc.co.uk"},
    {"www.bbc.co.uk", "bbc.co.uk"},
    {"bbc.co.uk", "bbc.co.uk"},
    {"co.uk", "co.uk"},
    {"uk", "uk"},
    {"192.168.0.5", "192.168.0.5"},
    {"www.buggy.psl", "www.buggy.psl"},
    {"www2.buggy.psl", "buggy.psl"},
    {"", ""},
    {".", "."},
    {"..", "."},
    {".net", ".net"},
    {"a.", "a."},
    {"b.a.", "a."},
    {"weird.stuff..", "."},
};

static const StrPair jar_key_nil_psl_tests[] = {
    {"foo.www.example.com", "example.com"},
    {"www.example.com", "example.com"},
    {"example.com", "example.com"},
    {"com", "com"},
    {"foo.www.bbc.co.uk", "co.uk"},
    {"www.bbc.co.uk", "co.uk"},
    {"bbc.co.uk", "co.uk"},
    {"co.uk", "co.uk"},
    {"uk", "uk"},
    {"192.168.0.5", "192.168.0.5"},
    {"", ""},
    {".", "."},
    {"..", ".."},
    {".net", ".net"},
    {"a.", "a."},
    {"b.a.", "a."},
    {"weird.stuff..", "stuff.."},
};

static const StrBool is_ip_tests[] = {
    {"127.0.0.1", true},
    {"1.2.3.4", true},
    {"2001:4860:0:2001::68", true},
    {"::1%zone", true},
    {"example.com", false},
    {"1.1.1.300", false},
    {"www.foo.bar.net", false},
    {"123.foo.bar.net", false},
};

static const StrPair default_path_tests[] = {
    {"/", "/"},
    {"/abc", "/"},
    {"/abc/", "/abc"},
    {"/abc/xyz", "/abc"},
    {"/abc/xyz/", "/abc/xyz"},
    {"/a/b/c.html", "/a/b"},
    {"", "/"},
    {"strange", "/"},
    {"//", "/"},
    {"/a//b", "/a/"},
    {"/a/./b", "/a/."},
    {"/a/../b", "/a/.."},
};

static const DomainAndTypeTest domain_and_type_tests[] = {
    {"www.example.com", "", "www.example.com", true, NO_ERR},
    {"127.0.0.1", "", "127.0.0.1", true, NO_ERR},
    {"2001:4860:0:2001::68", "", "2001:4860:0:2001::68", true, NO_ERR},
    {"www.example.com", "example.com", "example.com", false, NO_ERR},
    {"www.example.com", ".example.com", "example.com", false, NO_ERR},
    {"www.example.com", "www.example.com", "www.example.com", false, NO_ERR},
    {"www.example.com", ".www.example.com", "www.example.com", false, NO_ERR},
    {"foo.sso.example.com", "sso.example.com", "sso.example.com", false, NO_ERR},
    {"bar.co.uk", "bar.co.uk", "bar.co.uk", false, NO_ERR},
    {"foo.bar.co.uk", ".bar.co.uk", "bar.co.uk", false, NO_ERR},
    {"127.0.0.1", "127.0.0.1", "127.0.0.1", true, NO_ERR},
    {"2001:4860:0:2001::68", "2001:4860:0:2001::68", "2001:4860:0:2001::68", true,
     NO_ERR},
    {"www.example.com", ".", "", false, ERR_MALFORMED},
    {"www.example.com", "..", "", false, ERR_MALFORMED},
    {"www.example.com", "other.com", "", false, ERR_ILLEGAL},
    {"www.example.com", "com", "", false, ERR_ILLEGAL},
    {"www.example.com", ".com", "", false, ERR_ILLEGAL},
    {"foo.bar.co.uk", ".co.uk", "", false, ERR_ILLEGAL},
    {"127.www.0.0.1", "127.0.0.1", "", false, ERR_ILLEGAL},
    {"com", "", "com", true, NO_ERR},
    {"com", "com", "com", true, NO_ERR},
    {"com", ".com", "com", true, NO_ERR},
    {"co.uk", "", "co.uk", true, NO_ERR},
    {"co.uk", "co.uk", "co.uk", true, NO_ERR},
    {"co.uk", ".co.uk", "co.uk", true, NO_ERR},
};

static const StrPair punycode_test_cases[] = {
    {"", ""},
    {"-", "--"},
    {"-a", "-a-"},
    {"-a-", "-a--"},
    {"a", "a-"},
    {"a-", "a--"},
    {"a-b", "a-b-"},
    {"books", "books-"},
    {"b\303\274cher", "bcher-kva"},
    {"Hello\344\270\226\347\225\214", "Hello-ck1hg65u"},
    {"\303\274", "tda"},
    {"\303\274\303\275", "tdac"},
    {"\331\204\331\212\331\207\331\205\330\247\330\250\330\252\331\203\331\204\331\205"
     "\331\210\330\264\330\271\330\261\330\250\331\212\330\237",
     "egbpdaj6bu4bxfgehfvwxn"},
    {"\344\273\226\344\273\254\344\270\272\344\273\200\344\271\210\344\270\215\350\257"
     "\264\344\270\255\346\226\207",
     "ihqwcrb4cv8a8dqg056pqjye"},
    {"\344\273\226\345\200\221\347\210\262\344\273\200\351\272\275\344\270\215\350\252"
     "\252\344\270\255\346\226\207",
     "ihqwctvzc91f659drss3x8bo0yb"},
    {"Pro\304\215prost\304\233nemluv\303\255\304\215esky",
     "Proprostnemluvesky-uyb24dma41a"},
    {"\327\234\327\236\327\224\327\224\327\235\327\244\327\251\327\225\327\230\327\234"
     "\327\220\327\236\327\223\327\221\327\250\327\231\327\235\327\242\327\221\327\250"
     "\327\231\327\252",
     "4dbcagdahymbxekheh6e0a7fei0b"},
    {"\340\244\257\340\244\271\340\244\262\340\245\213\340\244\227\340\244\271\340\244"
     "\277\340\244\250\340\245\215\340\244\246\340\245\200\340\244\225\340\245\215\340"
     "\244\257\340\245\213\340\244\202\340\244\250\340\244\271\340\245\200\340\244\202"
     "\340\244\254\340\245\213\340\244\262\340\244\270\340\244\225\340\244\244\340\245"
     "\207\340\244\271\340\245\210\340\244\202",
     "i1baa7eci9glrd9b2ae1bj0hfcgg6iyaf8o0a1dig0cd"},
    {"\343\201\252\343\201\234\343\201\277\343\202\223\343\201\252\346\227\245\346\234"
     "\254\350\252\236\343\202\222\350\251\261\343\201\227\343\201\246\343\201\217\343"
     "\202\214\343\201\252\343\201\204\343\201\256\343\201\213",
     "n8jok5ay5dzabd5bym9f0cm5685rrjetr6pdxa"},
    {"\354\204\270\352\263\204\354\235\230\353\252\250\353\223\240\354\202\254\353\236"
     "\214\353\223\244\354\235\264\355\225\234\352\265\255\354\226\264\353\245\274\354"
     "\235\264\355\225\264\355\225\234\353\213\244\353\251\264\354\226\274\353\247\210"
     "\353\202\230\354\242\213\354\235\204\352\271\214",
     "989aomsvi5e83db1d2a355cv1e0vak1dwrv93d5xbh15a0dt30a5jpsd879ccm6fea98c"},
    {"\320\277\320\276\321\207\320\265\320\274\321\203\320\266\320\265\320\276\320\275"
     "\320\270\320\275\320\265\320\263\320\276\320\262\320\276\321\200\321\217\321\202"
     "\320\277\320\276\321\200\321\203\321\201\321\201\320\272\320\270",
     "b1abfaaepdrnnbgefbadotcwatmq2g4l"},
    {"Porqu\303\251nopuedensimplementehablarenEspa\303\261ol",
     "PorqunopuedensimplementehablarenEspaol-fmd56a"},
    {"T\341\272\241isaoh\341\273\215kh\303\264ngth\341\273\203ch\341\273\211n\303\263it"
     "i\341\272\277ngVi\341\273\207t",
     "TisaohkhngthchnitingVit-kjcr8268qyxafd2f1b9g"},
    {"3\345\271\264B\347\265\204\351\207\221\345\205\253\345\205\210\347\224\237",
     "3B-ww4c5e180e575a65lsy2b"},
    {"\345\256\211\345\256\244\345\245\210\347\276\216\346\201\265-with-SUPER-MONKEYS",
     "-with-SUPER-MONKEYS-pc58ag80a8qai00g7n9n"},
    {"Hello-Another-Way-"
     "\343\201\235\343\202\214\343\201\236\343\202\214\343\201\256\345\240\264\346\211"
     "\200",
     "Hello-Another-Way--fc4qua05auwb3674vfr0b"},
    {"\343\201\262\343\201\250\343\201\244\345\261\213\346\240\271\343\201\256\344\270"
     "\2132",
     "2-u9tlzr9756bt3uc0v"},
    {"Maji\343\201\247Koi\343\201\231\343\202\2135\347\247\222\345\211\215",
     "MajiKoi5-783gue6qz075azm5e"},
    {"\343\203\221\343\203\225\343\202\243\343\203\274de\343\203\253\343\203\263\343"
     "\203\220",
     "de-jg4avhby1noc0d"},
    {"\343\201\235\343\201\256\343\202\271\343\203\224\343\203\274\343\203\211\343\201"
     "\247",
     "d9juau41awczczp"},
    {"-> $1.00 <-", "-> $1.00 <--"},
};

/* ------------------------------------------------------------ the helpers */

static void TestHasDotSuffix(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof has_dot_suffix_tests / sizeof has_dot_suffix_tests[0];
         i++) {
        Str s = cs(has_dot_suffix_tests[i].a);
        Str suffix = cs(has_dot_suffix_tests[i].b);
        bool got = burrow__cookiejar_has_dot_suffix(s, suffix);
        Str dotted = fmt_sprintf_v(a, ".%s", suffix);
        bool want = strings_has_suffix(s, dotted);
        if (got != want)
            testing_t_errorf_v(t, "s=%q, suffix=%q: got %v, want %v", s, suffix, got,
                               want);
    }
    arena_free(&ar);
}

static void TestCanonicalHost(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof canonical_host_tests / sizeof canonical_host_tests[0];
         i++) {
        Str h = cs(canonical_host_tests[i].a);
        Str want = cs(canonical_host_tests[i].b);
        Error err;
        Str got = burrow__cookiejar_canonical_host(a, h, &err);
        if (str_eq(want, S("error"))) {
            if (BURROW_OK(err))
                testing_t_errorf_v(t, "%q: got %q and nil error, want non-nil", h, got);
            continue;
        }
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%q: %v", h, err);
            continue;
        }
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "%q: got %q, want %q", h, got, want);
    }
    arena_free(&ar);
}

static void TestHasPort(TestingT *t) {
    for (size_t i = 0; i < sizeof has_port_tests / sizeof has_port_tests[0]; i++) {
        Str host = cs(has_port_tests[i].s);
        bool got = burrow__cookiejar_has_port(host);
        if (got != has_port_tests[i].want)
            testing_t_errorf_v(t, "%q: got %t, want %t", host, got,
                               has_port_tests[i].want);
    }
}

static void TestJarKey(TestingT *t) {
    for (size_t i = 0; i < sizeof jar_key_tests / sizeof jar_key_tests[0]; i++) {
        Str host = cs(jar_key_tests[i].a);
        Str want = cs(jar_key_tests[i].b);
        Str got = burrow__cookiejar_jar_key(host, test_psl());
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "%q: got %q, want %q", host, got, want);
    }
}

static void TestJarKeyNilPSL(TestingT *t) {
    CookiejarPublicSuffixList none = {NULL, NULL};
    for (size_t i = 0;
         i < sizeof jar_key_nil_psl_tests / sizeof jar_key_nil_psl_tests[0]; i++) {
        Str host = cs(jar_key_nil_psl_tests[i].a);
        Str want = cs(jar_key_nil_psl_tests[i].b);
        Str got = burrow__cookiejar_jar_key(host, none);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "%q: got %q, want %q", host, got, want);
    }
}

static void TestIsIP(TestingT *t) {
    for (size_t i = 0; i < sizeof is_ip_tests / sizeof is_ip_tests[0]; i++) {
        Str host = cs(is_ip_tests[i].s);
        bool got = burrow__cookiejar_is_ip(host);
        if (got != is_ip_tests[i].want)
            testing_t_errorf_v(t, "%q: got %t, want %t", host, got,
                               is_ip_tests[i].want);
    }
}

static void TestDefaultPath(TestingT *t) {
    for (size_t i = 0; i < sizeof default_path_tests / sizeof default_path_tests[0];
         i++) {
        Str path = cs(default_path_tests[i].a);
        Str want = cs(default_path_tests[i].b);
        Str got = burrow__cookiejar_default_path(path);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "%q: got %q, want %q", path, got, want);
    }
}

static Error want_error(int code) {
    if (code == ERR_MALFORMED)
        return burrow__cookiejar_err_malformed_domain;
    if (code == ERR_ILLEGAL)
        return burrow__cookiejar_err_illegal_domain;
    return BURROW_NO_ERROR;
}

static void TestDomainAndType(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CookiejarJar *jar = new_test_jar(a);
    for (size_t i = 0;
         i < sizeof domain_and_type_tests / sizeof domain_and_type_tests[0]; i++) {
        const DomainAndTypeTest *tc = &domain_and_type_tests[i];
        bool host_only;
        Error err;
        Str domain = burrow__cookiejar_domain_and_type(
            jar, a, cs(tc->host), cs(tc->domain), &host_only, &err);
        Error want_err = want_error(tc->want_err);
        if (!errors_is(err, want_err) || BURROW_OK(err) != BURROW_OK(want_err)) {
            testing_t_errorf_v(t, "%q/%q: got %q error, want %v", cs(tc->host),
                               cs(tc->domain), err, want_err);
            continue;
        }
        if (BURROW_FAILED(err))
            continue;
        if (!str_eq(domain, cs(tc->want_domain)) || host_only != tc->want_host_only)
            testing_t_errorf_v(t, "%q/%q: got %q/%t want %q/%t", cs(tc->host),
                               cs(tc->domain), domain, host_only, cs(tc->want_domain),
                               tc->want_host_only);
    }
    cookiejar_jar_free(jar);
    arena_free(&ar);
}

/* --------------------------------------------------------------- jarTest */

/* expiresIn, an expires attribute delta seconds from tNow. */
static Str expires_in(Alloc *a, Int delta) {
    Time tm = time_add(t_now(), (Duration)delta * TIME_SECOND);
    return fmt_sprintf_v(a, "expires=%s", time_format(tm, a, TIME_RFC1123));
}

/* A Set-Cookie line from a table, with "{expires:N}" made into expiresIn(N). */
static Str set_cookie_line(Alloc *a, const char *line) {
    Str s = cs(line);
    Int i = strings_index(s, S("{expires:"));
    if (i < 0)
        return s;
    Int end = strings_index(str_from_bytes(s.p + i, s.len - i), S("}"));
    Str num = str_from_bytes(s.p + i + 9, end - 9);
    Error err;
    Int delta = strconv_atoi(num, &err);
    if (BURROW_FAILED(err))
        panic_str(S("bad {expires:N} in a table"));
    return fmt_sprintf_v(a, "%s%s", str_from_bytes(s.p, i), expires_in(a, delta));
}

/* mustParseURL. */
static Url *must_parse_url(Alloc *a, const char *s) {
    Error err;
    Url *u = url_parse(a, cs(s), &err);
    if (u == NULL || u->scheme.len == 0 || u->host.len == 0)
        panic_str(fmt_sprintf_v(a, "Unable to parse URL %s.", cs(s)));
    return u;
}

static int str_cmp_any(void *env, const void *x, const void *y) {
    (void)env;
    return str_cmp(*(const Str *)x, *(const Str *)y);
}

static void jar_test_run(TestingT *t, const JarTest *test, CookiejarJar *jar) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Time now = t_now();
    Str desc = cs(test->description);

    /* Fill the jar. */
    Slice set = slice_make(a, TYPE_HTTP_COOKIE, 0, 16);
    for (size_t i = 0; i < 16 && test->set_cookies[i] != NULL; i++) {
        Str line = set_cookie_line(a, test->set_cookies[i]);
        Error err;
        HttpCookie c = http_parse_set_cookie(a, line, &err);
        if (BURROW_FAILED(err))
            panic_str(fmt_sprintf_v(a, "Wrong cookie line %q", line));
        set = slice_append(a, set, &c, 1);
    }
    burrow__cookiejar_set_cookies_at(jar, must_parse_url(a, test->from_url), set, now);
    now = time_add(now, 1001 * TIME_MILLISECOND);

    /* What the jar holds that has not expired, as "name1=val1 name2=val2". */
    Entry *es;
    Int n = burrow__cookiejar_entries(jar, a, &es);
    Slice got_cs = slice_make(a, TYPE_STRING, 0, n);
    for (Int i = 0; i < n; i++) {
        if (!time_after(es[i].expires, now))
            continue;
        Str v = es[i].value;
        if (strings_contains_any(v, S(" ,")) || es[i].quoted)
            v = fmt_sprintf_v(a, "\"%s\"", v);
        Str kv = fmt_sprintf_v(a, "%s=%s", es[i].name, v);
        got_cs = slice_append(a, got_cs, &kv, 1);
    }
    slices_sort_func(got_cs, BURROW_FN(SlicesCmpFunc, str_cmp_any, NULL));
    Str got = strings_join(a, got_cs, S(" "));
    if (!str_eq(got, cs(test->content)))
        testing_t_errorf_v(t, "Test %q Content\ngot  %q\nwant %q", desc, got,
                           cs(test->content));

    /* The calls to Cookies. */
    for (size_t i = 0; i < 13 && test->queries[i].to_url != NULL; i++) {
        now = time_add(now, 1001 * TIME_MILLISECOND);
        Slice cookies = burrow__cookiejar_cookies_at(
            jar, a, must_parse_url(a, test->queries[i].to_url), now);
        Slice s = slice_make(a, TYPE_STRING, 0, cookies.len);
        for (Int k = 0; k < cookies.len; k++) {
            Str c = http_cookie_string(a, &((const HttpCookie *)cookies.p)[k]);
            s = slice_append(a, s, &c, 1);
        }
        Str q = strings_join(a, s, S(" "));
        if (!str_eq(q, cs(test->queries[i].want)))
            testing_t_errorf_v(t, "Test %q #%d\ngot  %q\nwant %q", desc, (int)i, q,
                               cs(test->queries[i].want));
    }
    arena_free(&ar);
}

/* Each test on a jar of its own. */
static void run_each(TestingT *t, const JarTest *tests, size_t n) {
    for (size_t i = 0; i < n; i++) {
        CookiejarJar *jar = new_test_jar(heap_allocator());
        jar_test_run(t, &tests[i], jar);
        cookiejar_jar_free(jar);
    }
}

/* All the tests on one jar. */
static void run_all(TestingT *t, const JarTest *tests, size_t n) {
    CookiejarJar *jar = new_test_jar(heap_allocator());
    for (size_t i = 0; i < n; i++)
        jar_test_run(t, &tests[i], jar);
    cookiejar_jar_free(jar);
}

static void TestBasics(TestingT *t) {
    run_each(t, basics_tests, sizeof basics_tests / sizeof basics_tests[0]);
}

static void TestUpdateAndDelete(TestingT *t) {
    run_all(t, update_and_delete_tests,
            sizeof update_and_delete_tests / sizeof update_and_delete_tests[0]);
}

static void TestExpiration(TestingT *t) {
    run_all(t, &expiration_test, 1);
}

static void TestChromiumBasics(TestingT *t) {
    run_each(t, chromium_basics_tests,
             sizeof chromium_basics_tests / sizeof chromium_basics_tests[0]);
}

static void TestChromiumDomain(TestingT *t) {
    run_all(t, chromium_domain_tests,
            sizeof chromium_domain_tests / sizeof chromium_domain_tests[0]);
}

static void TestChromiumDeletion(TestingT *t) {
    run_all(t, chromium_deletion_tests,
            sizeof chromium_deletion_tests / sizeof chromium_deletion_tests[0]);
}

static void TestDomainHandling(TestingT *t) {
    run_each(t, domain_handling_tests,
             sizeof domain_handling_tests / sizeof domain_handling_tests[0]);
}

static void TestIssue19384(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HttpCookie c = {.name = S("name"), .value = S("value")};
    Slice cookies = slice_from(&c, 1, 1, TYPE_HTTP_COOKIE);
    static const char *const hosts[] = {"", ".", "..", "..."};
    for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
        Error err;
        CookiejarJar *jar = cookiejar_new(heap_allocator(), NULL, &err);
        Url u = {.scheme = S("http"), .host = cs(hosts[i]), .path = S("/")};
        Slice got = cookiejar_jar_cookies(jar, a, &u);
        if (got.len != 0)
            testing_t_errorf_v(t, "host %q, got %d cookies", u.host, (int)got.len);
        cookiejar_jar_set_cookies(jar, &u, cookies);
        got = cookiejar_jar_cookies(jar, a, &u);
        if (got.len != 1 || !str_eq(((const HttpCookie *)got.p)[0].value, S("value")))
            testing_t_errorf_v(t, "host %q, got %d cookies", u.host, (int)got.len);
        cookiejar_jar_free(jar);
    }
    arena_free(&ar);
}

/* Not in Go: the jar as an HttpCookieJar is the same jar. */
static void TestAsCookieJar(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CookiejarJar *jar = new_test_jar(heap_allocator());
    HttpCookieJar j = cookiejar_jar_as_cookie_jar(jar);
    CHECK(j.vt->self_type == TYPE_COOKIEJAR_JAR);
    Url *u = must_parse_url(a, "https://www.host.test/a/b");
    Error err;
    HttpCookie c = http_parse_set_cookie(a, S("A=a; Path=/a"), &err);
    CHECK(BURROW_OK(err));
    http_cookie_jar_set_cookies(j, u, slice_from(&c, 1, 1, TYPE_HTTP_COOKIE));
    Slice got = cookiejar_jar_cookies(jar, a, u);
    CHECK_INT_EQ(got.len, 1);
    got = http_cookie_jar_cookies(j, a, must_parse_url(a, "https://www.host.test/"));
    CHECK_INT_EQ(got.len, 0);
    cookiejar_jar_free(jar);
    arena_free(&ar);
}

/* -------------------------------------------------------------- punycode */

static void TestPunycode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof punycode_test_cases / sizeof punycode_test_cases[0];
         i++) {
        Str s = cs(punycode_test_cases[i].a);
        Str encoded = cs(punycode_test_cases[i].b);
        Error err;
        Str got = burrow__cookiejar_encode(a, BURROW_STR_EMPTY, s, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "encode(\"\", %q): %v", s, err);
        else if (!str_eq(got, encoded))
            testing_t_errorf_v(t, "encode(\"\", %q): got %q, want %q", s, got, encoded);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestHasDotSuffix)                                                                \
    X(TestCanonicalHost)                                                               \
    X(TestHasPort)                                                                     \
    X(TestJarKey)                                                                      \
    X(TestJarKeyNilPSL)                                                                \
    X(TestIsIP)                                                                        \
    X(TestDefaultPath)                                                                 \
    X(TestDomainAndType)                                                               \
    X(TestBasics)                                                                      \
    X(TestUpdateAndDelete)                                                             \
    X(TestExpiration)                                                                  \
    X(TestChromiumBasics)                                                              \
    X(TestChromiumDomain)                                                              \
    X(TestChromiumDeletion)                                                            \
    X(TestDomainHandling)                                                              \
    X(TestIssue19384)                                                                  \
    X(TestAsCookieJar)                                                                 \
    X(TestPunycode)

TESTING_MAIN(TESTS)
