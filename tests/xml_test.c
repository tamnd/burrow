/* Derived from Go's src/encoding/xml/xml_test.go and the token tests in
 * marshal_test.go. The tests that go through Marshal, Unmarshal or Decode wait
 * for the parts of the package that have those.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/xml.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/declare.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

#include "../src/encoding/xml_internal.h"

#include "check.h"

#include <string.h>

#define S BURROW_S

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

static Str bstr(Slice b) {
    return (Str){(const Byte *)b.p, b.len};
}

static Slice sbytes(Str s) {
    return (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
}

static IoReader strings_io(Str s) {
    return strings_reader_as_io_reader(strings_new_reader(a, s));
}

static XmlDecoder *new_decoder(Str s) {
    return xml_new_decoder(NULL, strings_io(s));
}

static bool is_syntax_error(Error err) {
    return errors_as(err, TYPE_XML_SYNTAX_ERROR) != NULL;
}

/* ------------------------------------------------------------------ tokens */

static XmlToken tok_start(XmlName name, XmlAttr *attr, Int n) {
    XmlToken t = {XML_START_ELEMENT,
                  .start = {name, {attr, n, n, &burrow_type_XmlAttr}}};
    return t;
}

static XmlToken tok_end(XmlName name) {
    XmlToken t = {XML_END_ELEMENT, .end = {name}};
    return t;
}

static XmlToken tok_bytes(XmlTokenKind kind, Str s) {
    XmlToken t = {kind, .char_data = sbytes(s)};
    return t;
}

static XmlToken tok_pi(Str target, Str inst) {
    XmlToken t = {XML_PROC_INST, .proc_inst = {target, sbytes(inst)}};
    return t;
}

#define NM(sp, lo) ((XmlName){S(sp), S(lo)})
#define AT(sp, lo, v) {{S(sp), S(lo)}, S(v)}
#define START0(sp, lo) tok_start(NM(sp, lo), NULL, 0)
#define START(sp, lo, ...)                                                             \
    tok_start(NM(sp, lo), (XmlAttr[]){__VA_ARGS__},                                    \
              (Int)(sizeof((XmlAttr[]){__VA_ARGS__}) / sizeof(XmlAttr)))
#define END(sp, lo) tok_end(NM(sp, lo))
#define CD(s) tok_bytes(XML_CHAR_DATA, S(s))
#define COMMENT(s) tok_bytes(XML_COMMENT, S(s))
#define DIR(s) tok_bytes(XML_DIRECTIVE, S(s))
#define PI(target, inst) tok_pi(S(target), S(inst))
#define NTOKS(arr) ((Int)(sizeof(arr) / sizeof((arr)[0])))

static bool name_eq(XmlName x, XmlName y) {
    return str_eq(x.space, y.space) && str_eq(x.local, y.local);
}

/* reflect.DeepEqual, except that no attributes and an empty list of them are
 * the same thing here. */
static bool tok_eq(XmlToken x, XmlToken y) {
    if (x.kind != y.kind)
        return false;
    switch (x.kind) {
    case XML_START_ELEMENT: {
        if (!name_eq(x.start.name, y.start.name) ||
            x.start.attr.len != y.start.attr.len)
            return false;
        const XmlAttr *xa = x.start.attr.p, *ya = y.start.attr.p;
        for (Int i = 0; i < x.start.attr.len; i++)
            if (!name_eq(xa[i].name, ya[i].name) || !str_eq(xa[i].value, ya[i].value))
                return false;
        return true;
    }
    case XML_END_ELEMENT:
        return name_eq(x.end.name, y.end.name);
    case XML_CHAR_DATA:
    case XML_COMMENT:
    case XML_DIRECTIVE:
        return bytes_equal(x.char_data, y.char_data);
    case XML_PROC_INST:
        return str_eq(x.proc_inst.target, y.proc_inst.target) &&
               bytes_equal(x.proc_inst.inst, y.proc_inst.inst);
    case XML_TOKEN_NONE:
        return true;
    default:
        return false;
    }
}

/* The token the way %#v prints it, near enough. */
static Str tok_show(XmlToken t) {
    StringsBuilder b = STRINGS_BUILDER(a);
    IoWriter w = strings_builder_as_io_writer(&b);
    switch (t.kind) {
    case XML_START_ELEMENT: {
        Str sp = t.start.name.space, lo = t.start.name.local;
        fmt_fprintf_v(w, "StartElement{Name{%q, %q}, [", sp, lo);
        const XmlAttr *at = t.start.attr.p;
        for (Int i = 0; i < t.start.attr.len; i++) {
            Str asp = at[i].name.space, alo = at[i].name.local, v = at[i].value;
            fmt_fprintf_v(w, "%s{Name{%q, %q}, %q}", i > 0 ? ", " : "", asp, alo, v);
        }
        fmt_fprintf_v(w, "]}");
        break;
    }
    case XML_END_ELEMENT: {
        Str sp = t.end.name.space, lo = t.end.name.local;
        fmt_fprintf_v(w, "EndElement{Name{%q, %q}}", sp, lo);
        break;
    }
    case XML_CHAR_DATA: {
        Str s = bstr(t.char_data);
        fmt_fprintf_v(w, "CharData(%q)", s);
        break;
    }
    case XML_COMMENT: {
        Str s = bstr(t.comment);
        fmt_fprintf_v(w, "Comment(%q)", s);
        break;
    }
    case XML_DIRECTIVE: {
        Str s = bstr(t.directive);
        fmt_fprintf_v(w, "Directive(%q)", s);
        break;
    }
    case XML_PROC_INST: {
        Str target = t.proc_inst.target, inst = bstr(t.proc_inst.inst);
        fmt_fprintf_v(w, "ProcInst{%q, %q}", target, inst);
        break;
    }
    case XML_TOKEN_NONE:
    default:
        fmt_fprintf_v(w, "nil");
        break;
    }
    return strings_builder_string(&b);
}

/* A TokenReader that hands out a list, as Go's toks does. */
typedef struct Toks {
    bool early_eof;
    XmlToken *t;
    Int n;
} Toks;

static XmlToken toks_token(void *self, Error *err) {
    Toks *t = self;
    if (t->n == 0) {
        *err = io_eof;
        return (XmlToken){0};
    }
    XmlToken tok = t->t[0];
    t->t++;
    t->n--;
    *err = t->early_eof && t->n == 0 ? io_eof : BURROW_NO_ERROR;
    return tok;
}

static const XmlTokenReaderVT toks_vt = {NULL, toks_token};

/* ------------------------------------------------------------ test inputs */

static const Str test_input =
    BURROW_S_INIT("\n<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
                  "  \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
                  "<body xmlns:foo=\"ns1\" xmlns=\"ns2\" xmlns:tag=\"ns3\" \r\n\t  >\n"
                  "  <hello lang=\"en\">World &lt;&gt;&apos;&quot; "
                  "&#x767d;&#40300;\xe7\xbf\x94</hello>\n"
                  "  <query>&\xe4\xbd\x95; &is-it;</query>\n"
                  "  <goodbye />\n"
                  "  <outer foo:attr=\"value\" xmlns:tag=\"ns4\">\n"
                  "    <inner/>\n"
                  "  </outer>\n"
                  "  <tag:name>\n"
                  "    <![CDATA[Some text here.]]>\n"
                  "  </tag:name>\n"
                  "</body><!-- missing final newline -->");

static Map *test_entity(void) {
    Map *m = map_make(a, TYPE_STRING, TYPE_STRING, 0);
    BURROW_MAP_SET(Str, Str, m, S("\xe4\xbd\x95"), S("What"));
    BURROW_MAP_SET(Str, Str, m, S("is-it"), S("is it?"));
    return m;
}

#define DOCTYPE_TEXT                                                                   \
    "DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"                 \
    "  \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\""
#define HELLO_TEXT "World <>'\" \xe7\x99\xbd\xe9\xb5\xac\xe7\xbf\x94"

/* testRawToken. */
static void check_raw_tokens(TestingT *t, XmlDecoder *d, Str raw, const XmlToken *want,
                             Int n) {
    int64_t last_end = 0;
    for (Int i = 0; i < n; i++) {
        int64_t start = xml_decoder_input_offset(d);
        Error err;
        XmlToken have = xml_decoder_raw_token(d, &err);
        int64_t end = xml_decoder_input_offset(d);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "token %d: unexpected error: %s", (int)i,
                               error_text(err));
        if (!tok_eq(have, want[i])) {
            Str sh = tok_show(have), sw = tok_show(want[i]);
            testing_t_errorf_v(t, "token %d = %s, want %s", (int)i, sh, sw);
        }

        /* Check that InputOffset returned actual token. */
        if (start < last_end) {
            testing_t_errorf_v(t, "token %d: position [%d,%d) is before previous token",
                               (int)i, start, end);
        } else if (start >= end) {
            /* Special case: EndElement can be synthesized. */
            if (!(start == end && end == last_end))
                testing_t_errorf_v(t, "token %d: position [%d,%d) is empty", (int)i,
                                   start, end);
        } else if (end > raw.len) {
            testing_t_errorf_v(t, "token %d: position [%d,%d) extends beyond input",
                               (int)i, start, end);
        } else {
            Str text = {raw.p + start, (Int)(end - start)};
            if (strings_contains_any(text, S("<>")) &&
                (!strings_has_prefix(text, S("<")) ||
                 !strings_has_suffix(text, S(">"))))
                testing_t_errorf_v(t, "token %d: misaligned raw token %q", (int)i,
                                   text);
        }
        last_end = end;
    }
}

static void TestRawToken(TestingT *t) {
    XmlToken raw_tokens[] = {
        CD("\n"),
        PI("xml", "version=\"1.0\" encoding=\"UTF-8\""),
        CD("\n"),
        DIR(DOCTYPE_TEXT),
        CD("\n"),
        START("", "body", AT("xmlns", "foo", "ns1"), AT("", "xmlns", "ns2"),
              AT("xmlns", "tag", "ns3")),
        CD("\n  "),
        START("", "hello", AT("", "lang", "en")),
        CD(HELLO_TEXT),
        END("", "hello"),
        CD("\n  "),
        START0("", "query"),
        CD("What is it?"),
        END("", "query"),
        CD("\n  "),
        START0("", "goodbye"),
        END("", "goodbye"),
        CD("\n  "),
        START("", "outer", AT("foo", "attr", "value"), AT("xmlns", "tag", "ns4")),
        CD("\n    "),
        START0("", "inner"),
        END("", "inner"),
        CD("\n  "),
        END("", "outer"),
        CD("\n  "),
        START0("tag", "name"),
        CD("\n    "),
        CD("Some text here."),
        CD("\n  "),
        END("tag", "name"),
        CD("\n"),
        END("", "body"),
        COMMENT(" missing final newline "),
    };
    XmlDecoder *d = new_decoder(test_input);
    d->entity = test_entity();
    check_raw_tokens(t, d, test_input, raw_tokens, NTOKS(raw_tokens));
    xml_decoder_free(d);
}

static void TestNonStrictRawToken(TestingT *t) {
    static const Str input = BURROW_S_INIT("\n"
                                           "<tag>non&entity</tag>\n"
                                           "<tag>&unknown;entity</tag>\n"
                                           "<tag>&#123</tag>\n"
                                           "<tag>&#zzz;</tag>\n"
                                           "<tag>&\xe3\x81\xaa\xe3\x81\xbe\xe3\x81\x88"
                                           "3;</tag>\n"
                                           "<tag>&lt-gt;</tag>\n"
                                           "<tag>&;</tag>\n"
                                           "<tag>&0a;</tag>\n");
    XmlToken want[] = {
        CD("\n"),
        START0("", "tag"),
        CD("non&entity"),
        END("", "tag"),
        CD("\n"),
        START0("", "tag"),
        CD("&unknown;entity"),
        END("", "tag"),
        CD("\n"),
        START0("", "tag"),
        CD("&#123"),
        END("", "tag"),
        CD("\n"),
        START0("", "tag"),
        CD("&#zzz;"),
        END("", "tag"),
        CD("\n"),
        START0("", "tag"),
        CD("&\xe3\x81\xaa\xe3\x81\xbe\xe3\x81\x88"
           "3;"),
        END("", "tag"),
        CD("\n"),
        START0("", "tag"),
        CD("&lt-gt;"),
        END("", "tag"),
        CD("\n"),
        START0("", "tag"),
        CD("&;"),
        END("", "tag"),
        CD("\n"),
        START0("", "tag"),
        CD("&0a;"),
        END("", "tag"),
        CD("\n"),
    };
    XmlDecoder *d = new_decoder(input);
    d->strict = false;
    check_raw_tokens(t, d, input, want, NTOKS(want));
    xml_decoder_free(d);
}

/* downCaser: an io.ByteReader that lowers ASCII letters, with a Read that
 * must not be called. */
typedef struct DownCaser {
    TestingT *t;
    IoReader r;
    const Method *read_byte;
} DownCaser;

static Byte down_caser_read_byte(DownCaser *d, Error *err) {
    Byte c = 0;
    IoErrorArg ea = err;
    void *args[1] = {&ea};
    void *rets[1] = {&c};
    d->read_byte->thunk(d->r.data, args, rets);
    if (c >= 'A' && c <= 'Z')
        c = (Byte)(c + ('a' - 'A'));
    return c;
}

static Int down_caser_read(void *self, Slice p, Error *err) {
    (void)p;
    (void)err;
    DownCaser *d = self;
    testing_t_fatalf_v(d->t, "unexpected Read call on downCaser reader");
    return 0;
}

#define DOWN_CASER_METHODS(M, T) M(T, ReadByte, down_caser_read_byte, IO_SIG_READ_BYTE)
BURROW_METHODS_DEFINE(DownCaser, DOWN_CASER_METHODS);

static const Type down_caser_type = {
    {(const Byte *)"downCaser", 9},
    {(const Byte *)"xml", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(DownCaser),
    (uint16_t)_Alignof(DownCaser),
    0,
    (uint16_t)(sizeof burrow__methods_DownCaser / sizeof burrow__methods_DownCaser[0]),
    NULL,
    burrow__methods_DownCaser,
    NULL,
    NULL,
    0,
    0x64636173U, /* "dcas" */
    NULL,
};

static const IoReaderVT down_caser_vt = {&down_caser_type, down_caser_read};

static IoReader alt_charset(void *env, Str charset, IoReader input, Error *err) {
    TestingT *t = env;
    if (!str_eq(charset, S("x-testing-uppercase")))
        testing_t_fatalf_v(t, "unexpected charset %q", charset);
    DownCaser *dc = mem_alloc(a, sizeof *dc, _Alignof(DownCaser));
    dc->t = t;
    dc->r = input;
    dc->read_byte = burrow__io_read_byte_method(input);
    if (dc->read_byte == NULL)
        testing_t_fatalf_v(t, "the input is not an io.ByteReader");
    *err = BURROW_NO_ERROR;
    return (IoReader){&down_caser_vt, dc};
}

static const Str test_input_alt_encoding =
    BURROW_S_INIT("\n<?xml version=\"1.0\" encoding=\"x-testing-uppercase\"?>\n"
                  "<TAG>VALUE</TAG>");

static void TestRawTokenAltEncoding(TestingT *t) {
    XmlToken want[] = {
        CD("\n"),    PI("xml", "version=\"1.0\" encoding=\"x-testing-uppercase\""),
        CD("\n"),    START0("", "tag"),
        CD("value"), END("", "tag"),
    };
    XmlDecoder *d = new_decoder(test_input_alt_encoding);
    d->charset_reader = BURROW_FN(XmlCharsetReader, alt_charset, t);
    check_raw_tokens(t, d, test_input_alt_encoding, want, NTOKS(want));
    xml_decoder_free(d);
}

static void TestRawTokenAltEncodingNoConverter(TestingT *t) {
    XmlDecoder *d = new_decoder(test_input_alt_encoding);
    Error err;
    XmlToken token = xml_decoder_raw_token(d, &err);
    if (token.kind == XML_TOKEN_NONE)
        testing_t_fatalf_v(t, "expected a token on first RawToken call");
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    token = xml_decoder_raw_token(d, &err);
    if (token.kind != XML_TOKEN_NONE) {
        Str s = tok_show(token);
        testing_t_errorf_v(t, "expected a nil token; got %s", s);
    }
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected an error on second RawToken call");
    if (!strings_contains(error_text(err), S("x-testing-uppercase")))
        testing_t_errorf_v(t, "expected error to contain %q; got error: %s",
                           "x-testing-uppercase", error_text(err));
    xml_decoder_free(d);
}

/* A CharsetReader that fails, and one that hands back no reader. */
static IoReader failing_charset(void *env, Str charset, IoReader input, Error *err) {
    (void)env;
    (void)charset;
    (void)input;
    *err = errors_new(error_allocator(), S("no such charset"));
    return (IoReader){NULL, NULL};
}

static void TestCharsetReaderError(TestingT *t) {
    XmlDecoder *d = new_decoder(test_input_alt_encoding);
    d->charset_reader = BURROW_FN(XmlCharsetReader, failing_charset, NULL);
    Error err;
    for (xml_decoder_raw_token(d, &err); BURROW_OK(err);
         xml_decoder_raw_token(d, &err)) {
    }
    Str want = S("xml: opening charset \"x-testing-uppercase\": no such charset");
    if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "error = %q, want %q", error_text(err), want);
    xml_decoder_free(d);
}

static IoReader nil_charset(void *env, Str charset, IoReader input, Error *err) {
    (void)env;
    (void)charset;
    (void)input;
    *err = BURROW_NO_ERROR;
    return (IoReader){NULL, NULL};
}

static void nil_charset_run(void *env) {
    XmlDecoder *d = env;
    Error err;
    for (xml_decoder_raw_token(d, &err); BURROW_OK(err);
         xml_decoder_raw_token(d, &err)) {
    }
}

static void TestCharsetReaderNil(TestingT *t) {
    XmlDecoder *d = new_decoder(test_input_alt_encoding);
    d->charset_reader = BURROW_FN(XmlCharsetReader, nil_charset, NULL);
    volatile bool panicked = false;
    Str want = S("CharsetReader returned a nil Reader for charset x-testing-uppercase");
    BURROW_TRY {
        nil_charset_run(d);
    }
    BURROW_CATCH(r) {
        panicked = true;
        if (!str_eq(panic_text(r), want))
            testing_t_errorf_v(t, "panic = %q, want %q", panic_text(r), want);
    }
    BURROW_TRY_END;
    if (!panicked)
        testing_t_errorf_v(t, "no panic, want %q", want);
    xml_decoder_free(d);
}

/* Ensure that directives (specifically !DOCTYPE) include the complete text of
 * any nested directives, noting that < and > do not change nesting depth if
 * they are in single or double quotes. */
static void TestNestedDirectives(TestingT *t) {
    static const Str input = BURROW_S_INIT(
        "\n"
        "<!DOCTYPE [<!ENTITY rdf \"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">]>\n"
        "<!DOCTYPE [<!ENTITY xlt \">\">]>\n"
        "<!DOCTYPE [<!ENTITY xlt \"<\">]>\n"
        "<!DOCTYPE [<!ENTITY xlt '>'>]>\n"
        "<!DOCTYPE [<!ENTITY xlt '<'>]>\n"
        "<!DOCTYPE [<!ENTITY xlt '\">'>]>\n"
        "<!DOCTYPE [<!ENTITY xlt \"'<\">]>\n");
    XmlToken want[] = {
        CD("\n"),
        DIR("DOCTYPE [<!ENTITY rdf \"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">]"),
        CD("\n"),
        DIR("DOCTYPE [<!ENTITY xlt \">\">]"),
        CD("\n"),
        DIR("DOCTYPE [<!ENTITY xlt \"<\">]"),
        CD("\n"),
        DIR("DOCTYPE [<!ENTITY xlt '>'>]"),
        CD("\n"),
        DIR("DOCTYPE [<!ENTITY xlt '<'>]"),
        CD("\n"),
        DIR("DOCTYPE [<!ENTITY xlt '\">'>]"),
        CD("\n"),
        DIR("DOCTYPE [<!ENTITY xlt \"'<\">]"),
        CD("\n"),
    };
    XmlDecoder *d = new_decoder(input);
    for (Int i = 0; i < NTOKS(want); i++) {
        Error err;
        XmlToken have = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "token %d: unexpected error: %s", (int)i,
                               error_text(err));
        if (!tok_eq(have, want[i])) {
            Str sh = tok_show(have), sw = tok_show(want[i]);
            testing_t_errorf_v(t, "token %d = %s want %s", (int)i, sh, sw);
        }
    }
    xml_decoder_free(d);
}

static void TestToken(TestingT *t) {
    XmlToken cooked_tokens[] = {
        CD("\n"),
        PI("xml", "version=\"1.0\" encoding=\"UTF-8\""),
        CD("\n"),
        DIR(DOCTYPE_TEXT),
        CD("\n"),
        START("ns2", "body", AT("xmlns", "foo", "ns1"), AT("", "xmlns", "ns2"),
              AT("xmlns", "tag", "ns3")),
        CD("\n  "),
        START("ns2", "hello", AT("", "lang", "en")),
        CD(HELLO_TEXT),
        END("ns2", "hello"),
        CD("\n  "),
        START0("ns2", "query"),
        CD("What is it?"),
        END("ns2", "query"),
        CD("\n  "),
        START0("ns2", "goodbye"),
        END("ns2", "goodbye"),
        CD("\n  "),
        START("ns2", "outer", AT("ns1", "attr", "value"), AT("xmlns", "tag", "ns4")),
        CD("\n    "),
        START0("ns2", "inner"),
        END("ns2", "inner"),
        CD("\n  "),
        END("ns2", "outer"),
        CD("\n  "),
        START0("ns3", "name"),
        CD("\n    "),
        CD("Some text here."),
        CD("\n  "),
        END("ns3", "name"),
        CD("\n"),
        END("ns2", "body"),
        COMMENT(" missing final newline "),
    };
    XmlDecoder *d = new_decoder(test_input);
    d->entity = test_entity();
    for (Int i = 0; i < NTOKS(cooked_tokens); i++) {
        Error err;
        XmlToken have = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "token %d: unexpected error: %s", (int)i,
                               error_text(err));
        if (!tok_eq(have, cooked_tokens[i])) {
            Str sh = tok_show(have), sw = tok_show(cooked_tokens[i]);
            testing_t_errorf_v(t, "token %d = %s want %s", (int)i, sh, sw);
        }
    }
    xml_decoder_free(d);
}

static const char *const xml_input[] = {
    /* unexpected EOF cases */
    "<",
    "<t",
    "<t ",
    "<t/",
    "<!",
    "<!-",
    "<!--",
    "<!--c-",
    "<!--c--",
    "<!d",
    "<t></",
    "<t></t",
    "<?",
    "<?p",
    "<t a",
    "<t a=",
    "<t a='",
    "<t a=''",
    "<t/><![",
    "<t/><![C",
    "<t/><![CDATA[d",
    "<t/><![CDATA[d]",
    "<t/><![CDATA[d]]",

    /* other Syntax errors */
    "<>",
    "<t/a",
    "<0 />",
    "<?0 >",
    /*	"<!0 >",	// let the Token() caller handle */
    "</0>",
    "<t 0=''>",
    "<t a='&'>",
    "<t a='<'>",
    "<t>&nbspc;</t>",
    "<t a>",
    "<t a=>",
    "<t a=v>",
    /*	"<![CDATA[d]]>",	// let the Token() caller handle */
    "<t></e>",
    "<t></>",
    "<t></t!",
    "<t>cdata]]></t>",
};

static void TestSyntax(TestingT *t) {
    for (size_t i = 0; i < sizeof xml_input / sizeof xml_input[0]; i++) {
        XmlDecoder *d = new_decoder(str_from_cstr(xml_input[i]));
        Error err;
        for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err)) {
        }
        bool ok = is_syntax_error(err);
        xml_decoder_free(d);
        if (!ok)
            testing_t_fatalf_v(t, "xmlInput \"%s\": expected SyntaxError not received",
                               xml_input[i]);
    }
}

static void TestInputLinePos(TestingT *t) {
    static const Str input = BURROW_S_INIT("<root>\n"
                                           "<?pi\n"
                                           " ?>  <elt\n"
                                           "att\n"
                                           "=\n"
                                           "\"val\">\n"
                                           "<![CDATA[\n"
                                           "]]><!--\n"
                                           "\n"
                                           "--></elt>\n"
                                           "</root>");
    static const int line_pos[][2] = {
        {1, 7}, {2, 1},  {3, 4},   {3, 6},  {6, 7},  {7, 1},
        {8, 4}, {10, 4}, {10, 10}, {11, 1}, {11, 8},
    };
    XmlDecoder *dec = new_decoder(input);
    for (size_t i = 0; i < sizeof line_pos / sizeof line_pos[0]; i++) {
        Error err;
        xml_decoder_token(dec, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Unexpected error: %s", error_text(err));
            continue;
        }
        Int line, col;
        line = xml_decoder_input_pos(dec, &col);
        if (line != line_pos[i][0] || col != line_pos[i][1])
            testing_t_errorf_v(t, "dec.InputPos() = %d,%d, want %d,%d", line, col,
                               line_pos[i][0], line_pos[i][1]);
    }
    xml_decoder_free(dec);
}

static void TestIssue68387(TestingT *t) {
    XmlDecoder *dec = new_decoder(S("<item b=']]>'/>"));
    Error err;
    XmlToken tok1 = xml_decoder_raw_token(dec, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "RawToken() failed: %s", error_text(err));
    tok1 = xml_copy_token(a, tok1);
    XmlToken tok2 = xml_decoder_raw_token(dec, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "RawToken() failed: %s", error_text(err));
    tok2 = xml_copy_token(a, tok2);
    XmlToken tok3 = xml_decoder_raw_token(dec, &err);
    if (!errors_is(err, io_eof) || tok3.kind != XML_TOKEN_NONE)
        testing_t_fatalf_v(t, "Missed EOF");
    if (!tok_eq(tok1, START("", "item", AT("", "b", "]]>"))))
        testing_t_errorf_v(t, "Wrong start element");
    if (!tok_eq(tok2, END("", "item")))
        testing_t_errorf_v(t, "Wrong end element");
    xml_decoder_free(dec);
}

static void TestUnquotedAttrs(TestingT *t) {
    XmlDecoder *d = new_decoder(S("<tag attr=azAZ09:-_\t>"));
    d->strict = false;
    Error err;
    XmlToken token = xml_decoder_token(d, &err);
    if (is_syntax_error(err))
        testing_t_errorf_v(t, "Unexpected error: %s", error_text(err));
    if (token.kind != XML_START_ELEMENT || token.start.attr.len != 1)
        testing_t_fatalf_v(t, "Unexpected token: %s", tok_show(token));
    if (!str_eq(token.start.name.local, S("tag")))
        testing_t_errorf_v(t, "Unexpected tag name: %s", token.start.name.local);
    XmlAttr attr = ((const XmlAttr *)token.start.attr.p)[0];
    if (!str_eq(attr.value, S("azAZ09:-_")))
        testing_t_errorf_v(t, "Unexpected attribute value: %s", attr.value);
    if (!str_eq(attr.name.local, S("attr")))
        testing_t_errorf_v(t, "Unexpected attribute name: %s", attr.name.local);
    xml_decoder_free(d);
}

static void TestValuelessAttrs(TestingT *t) {
    static const char *const tests[][3] = {
        {"<p nowrap>", "p", "nowrap"},
        {"<p nowrap >", "p", "nowrap"},
        {"<input checked/>", "input", "checked"},
        {"<input checked />", "input", "checked"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        XmlDecoder *d = new_decoder(str_from_cstr(tests[i][0]));
        d->strict = false;
        Error err;
        XmlToken token = xml_decoder_token(d, &err);
        if (is_syntax_error(err))
            testing_t_errorf_v(t, "Unexpected error: %s", error_text(err));
        if (token.kind != XML_START_ELEMENT || token.start.attr.len != 1) {
            testing_t_errorf_v(t, "Unexpected token: %s", tok_show(token));
            xml_decoder_free(d);
            continue;
        }
        if (!str_eq(token.start.name.local, str_from_cstr(tests[i][1])))
            testing_t_errorf_v(t, "Unexpected tag name: %s", token.start.name.local);
        XmlAttr attr = ((const XmlAttr *)token.start.attr.p)[0];
        if (!str_eq(attr.value, str_from_cstr(tests[i][2])))
            testing_t_errorf_v(t, "Unexpected attribute value: %s", attr.value);
        if (!str_eq(attr.name.local, str_from_cstr(tests[i][2])))
            testing_t_errorf_v(t, "Unexpected attribute name: %s", attr.name.local);
        xml_decoder_free(d);
    }
}

static void TestCopyTokenCharData(TestingT *t) {
    char data[] = "same data";
    XmlToken tok1 = {XML_CHAR_DATA, .char_data = {data, 9, 9, TYPE_BYTE}};
    XmlToken tok2 = xml_copy_token(heap_allocator(), tok1);
    if (!tok_eq(tok1, tok2))
        testing_t_errorf_v(t, "CopyToken(CharData) != CharData");
    data[1] = 'o';
    if (tok_eq(tok1, tok2))
        testing_t_errorf_v(t, "CopyToken(CharData) uses same buffer.");
    xml_token_free(heap_allocator(), tok2);
}

static void TestCopyTokenStartElement(TestingT *t) {
    XmlAttr attr[] = {AT("", "lang", "en")};
    XmlToken tok1 = tok_start(NM("", "hello"), attr, 1);
    XmlToken tok2 = xml_copy_token(heap_allocator(), tok1);
    if (!str_eq(attr[0].value, S("en")))
        testing_t_errorf_v(t, "CopyToken overwrote Attr[0]");
    if (!tok_eq(tok1, tok2))
        testing_t_errorf_v(t, "CopyToken(StartElement) != StartElement");
    attr[0] = (XmlAttr)AT("", "lang", "de");
    if (tok_eq(tok1, tok2))
        testing_t_errorf_v(t, "CopyToken(CharData) uses same buffer.");
    xml_token_free(heap_allocator(), tok2);
}

static void TestCopyTokenComment(TestingT *t) {
    char data[] = "<!-- some comment -->";
    XmlToken tok1 = {XML_COMMENT, .comment = {data, 21, 21, TYPE_BYTE}};
    XmlToken tok2 = xml_copy_token(heap_allocator(), tok1);
    if (!tok_eq(tok1, tok2))
        testing_t_errorf_v(t, "CopyToken(Comment) != Comment");
    data[1] = 'o';
    if (tok_eq(tok1, tok2))
        testing_t_errorf_v(t, "CopyToken(Comment) uses same buffer.");
    xml_token_free(heap_allocator(), tok2);
}

/* Every kind of token copied and freed again, which the tracking allocator
 * the tests run under checks the sizes of. */
static void TestCopyTokenFree(TestingT *t) {
    XmlToken toks[] = {
        START("s", "l", AT("x", "y", "z"), AT("", "b", "")),
        START0("", "l"),
        START("", "", AT("", "", "")),
        END("s", "l"),
        END("", "l"),
        CD("text"),
        CD(""),
        COMMENT("c"),
        DIR("d"),
        PI("t", "i"),
        PI("t", ""),
        PI("", "i"),
        {XML_TOKEN_NONE, .char_data = {NULL, 0, 0, NULL}},
    };
    for (Int i = 0; i < NTOKS(toks); i++) {
        XmlToken c = xml_copy_token(heap_allocator(), toks[i]);
        if (!tok_eq(c, toks[i])) {
            Str sc = tok_show(c), sw = tok_show(toks[i]);
            testing_t_errorf_v(t, "copy %d = %s, want %s", (int)i, sc, sw);
        }
        xml_token_free(heap_allocator(), c);
    }
}

static void TestSyntaxErrorLineNum(TestingT *t) {
    XmlDecoder *d = new_decoder(S("<P>Foo<P>\n\n<P>Bar</>\n"));
    Error err;
    for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err)) {
    }
    const XmlSyntaxError *synerr = errors_as(err, TYPE_XML_SYNTAX_ERROR);
    if (synerr == NULL)
        testing_t_errorf_v(t, "Expected SyntaxError.");
    else if (synerr->line != 3)
        testing_t_errorf_v(t, "SyntaxError didn't have correct line number.");
    xml_decoder_free(d);
}

static void TestSyntaxErrorText(TestingT *t) {
    XmlSyntaxError e = {S("unexpected EOF"), 12};
    Str want = S("XML syntax error on line 12: unexpected EOF");
    CHECK(str_eq(xml_syntax_error_error(&e, a), want));
    Error err = xml_syntax_error_as_error(a, &e);
    CHECK(str_eq(error_text(err), want));
    const XmlSyntaxError *got = errors_as(err, TYPE_XML_SYNTAX_ERROR);
    CHECK(got != NULL && got->line == 12 && str_eq(got->msg, e.msg));
    Error kept = error_retain(a, err);
    got = errors_as(kept, TYPE_XML_SYNTAX_ERROR);
    CHECK(got != NULL && got->line == 12 && str_eq(error_text(kept), want));
}

static void TestTrailingRawToken(TestingT *t) {
    XmlDecoder *d = new_decoder(S("<FOO></FOO>  "));
    Error err;
    for (xml_decoder_raw_token(d, &err); BURROW_OK(err);
         xml_decoder_raw_token(d, &err)) {
    }
    if (!errors_is(err, io_eof))
        testing_t_fatalf_v(t, "d.RawToken() = _, %s, want _, io.EOF", error_text(err));
    xml_decoder_free(d);
}

static void TestTrailingToken(TestingT *t) {
    XmlDecoder *d = new_decoder(S("<FOO></FOO>  "));
    Error err;
    for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err)) {
    }
    if (!errors_is(err, io_eof))
        testing_t_fatalf_v(t, "d.Token() = _, %s, want _, io.EOF", error_text(err));
    xml_decoder_free(d);
}

static void TestEntityInsideCDATA(TestingT *t) {
    XmlDecoder *d = new_decoder(S("<test><![CDATA[ &val=foo ]]></test>"));
    Error err;
    for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err)) {
    }
    if (!errors_is(err, io_eof))
        testing_t_fatalf_v(t, "d.Token() = _, %s, want _, io.EOF", error_text(err));
    xml_decoder_free(d);
}

static void TestDisallowedCharacters(TestingT *t) {
    static const struct {
        const char *in;
        const char *err;
    } tests[] = {
        {"\x12<doc/>", "illegal character code U+0012"},
        {"<?xml version=\"1.0\"?>\x0b<doc/>", "illegal character code U+000B"},
        {"\xef\xbf\xbe<doc/>", "illegal character code U+FFFE"},
        {"<?xml version=\"1.0\"?><doc>\r\n<hiya/>\x07<toots/></doc>",
         "illegal character code U+0007"},
        {"<?xml version=\"1.0\"?><doc \x12='value'>what's up</doc>",
         "expected attribute name in element"},
        {"<doc>&abc\x01;</doc>", "invalid character entity &abc (no semicolon)"},
        {"<doc>&\x01;</doc>", "invalid character entity & (no semicolon)"},
        {"<doc>&\xef\xbf\xbe;</doc>", "invalid character entity &\xef\xbf\xbe;"},
        {"<doc>&hello;</doc>", "invalid character entity &hello;"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        XmlDecoder *d = new_decoder(str_from_cstr(tests[i].in));
        Error err = BURROW_NO_ERROR;
        while (BURROW_OK(err))
            xml_decoder_token(d, &err);
        xml_decoder_free(d);
        const XmlSyntaxError *synerr = errors_as(err, TYPE_XML_SYNTAX_ERROR);
        if (synerr == NULL)
            testing_t_fatalf_v(t, "input %d d.Token() = _, %s, want _, *SyntaxError",
                               (int)i, error_text(err));
        else if (!str_eq(synerr->msg, str_from_cstr(tests[i].err)))
            testing_t_fatalf_v(t, "input %d synerr.Msg wrong: want %q, got %q", (int)i,
                               tests[i].err, synerr->msg);
    }
}

/* Past the Basic Multilingual Plane, where the message has more digits. */
static void TestDisallowedCharacterWide(TestingT *t) {
    /* U+10FFFF is allowed, U+DFFFF and U+1FFFE are not ranges XML excludes, so
     * the one to try is a surrogate, which UTF-8 cannot carry, and U+FFFF. */
    XmlDecoder *d = new_decoder(S("<doc>\xef\xbf\xbf</doc>"));
    Error err = BURROW_NO_ERROR;
    while (BURROW_OK(err))
        xml_decoder_token(d, &err);
    xml_decoder_free(d);
    const XmlSyntaxError *synerr = errors_as(err, TYPE_XML_SYNTAX_ERROR);
    CHECK(synerr != NULL && str_eq(synerr->msg, S("illegal character code U+FFFF")));

    d = new_decoder(S("<doc>\xf4\x8f\xbf\xbf</doc>"));
    Error e2;
    int n = 0;
    for (xml_decoder_token(d, &e2); BURROW_OK(e2); xml_decoder_token(d, &e2))
        n++;
    CHECK(errors_is(e2, io_eof) && n == 3);
    xml_decoder_free(d);
}

static void TestIsInCharacterRange(TestingT *t) {
    static const Rune invalid[] = {
        UTF8_MAX_RUNE + 1,
        0xD800, /* surrogate min */
        0xDFFF, /* surrogate max */
        -1,
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        if (burrow__xml_is_in_character_range(invalid[i]))
            testing_t_errorf_v(t, "rune %U considered valid", invalid[i]);
}

static void TestProcInstEncoding(TestingT *t) {
    static const struct {
        const char *input;
        const char *expect[2];
    } tests[] = {
        {"version=\"1.0\" encoding=\"utf-8\"", {"1.0", "utf-8"}},
        {"version=\"1.0\" encoding='utf-8'", {"1.0", "utf-8"}},
        {"version=\"1.0\" encoding='utf-8' ", {"1.0", "utf-8"}},
        {"version=\"1.0\" encoding=utf-8", {"1.0", ""}},
        {"encoding=\"FOO\" ", {"", "FOO"}},
        {"version=2.0 version=\"1.0\" encoding=utf-7 encoding='utf-8'",
         {"1.0", "utf-8"}},
        {"version= encoding=", {"", ""}},
        {"encoding=\"version=1.0\"", {"", "version=1.0"}},
        {"", {"", ""}},
        /* TODO: what's the right approach to handle these nested cases? */
        {"encoding=\"version='1.0'\"", {"1.0", "version='1.0'"}},
        {"version=\"encoding='utf-8'\"", {"encoding='utf-8'", "utf-8"}},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str in = str_from_cstr(tests[i].input);
        Str got = burrow__xml_proc_inst(S("version"), in);
        if (!str_eq(got, str_from_cstr(tests[i].expect[0])))
            testing_t_errorf_v(t, "procInst(version, %q) = %q; want %q", in, got,
                               tests[i].expect[0]);
        got = burrow__xml_proc_inst(S("encoding"), in);
        if (!str_eq(got, str_from_cstr(tests[i].expect[1])))
            testing_t_errorf_v(t, "procInst(encoding, %q) = %q; want %q", in, got,
                               tests[i].expect[1]);
    }
}

/* Ensure that directives with comments include the complete text of any
 * nested directives. */
static void TestDirectivesWithComments(TestingT *t) {
    static const Str input = BURROW_S_INIT(
        "\n"
        "<!DOCTYPE [<!-- a comment --><!ENTITY rdf "
        "\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">]>\n"
        "<!DOCTYPE [<!ENTITY go \"Golang\"><!-- a comment-->]>\n"
        "<!DOCTYPE <!-> <!> <!----> <!-->--> <!--->--> [<!ENTITY go \"Golang\"><!-- a "
        "comment-->]>\n");
    XmlToken want[] = {
        CD("\n"),
        DIR("DOCTYPE [ <!ENTITY rdf \"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">]"),
        CD("\n"),
        DIR("DOCTYPE [<!ENTITY go \"Golang\"> ]"),
        CD("\n"),
        DIR("DOCTYPE <!-> <!>       [<!ENTITY go \"Golang\"> ]"),
        CD("\n"),
    };
    XmlDecoder *d = new_decoder(input);
    for (Int i = 0; i < NTOKS(want); i++) {
        Error err;
        XmlToken have = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "token %d: unexpected error: %s", (int)i,
                               error_text(err));
        if (!tok_eq(have, want[i])) {
            Str sh = tok_show(have), sw = tok_show(want[i]);
            testing_t_errorf_v(t, "token %d = %s want %s", (int)i, sh, sw);
        }
    }
    xml_decoder_free(d);
}

/* Writer whose Write method always returns an error. */
static Int err_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = errors_new(error_allocator(), S("unwritable"));
    return 0;
}

static const IoWriterVT err_writer_vt = {NULL, err_write};

static void TestEscapeTextIOErrors(TestingT *t) {
    Error err = xml_escape_text((IoWriter){&err_writer_vt, NULL}, sbytes(S("A")));
    if (BURROW_OK(err) || !str_eq(error_text(err), S("unwritable")))
        testing_t_errorf_v(t, "have %s, want unwritable", error_text(err));
}

static void TestEscapeTextInvalidChar(TestingT *t) {
    static const char input[] = "A \x00 terminated string.";
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = xml_escape_text(strings_builder_as_io_writer(&b),
                                (Slice){(void *)(uintptr_t)input, sizeof input - 1,
                                        sizeof input - 1, TYPE_BYTE});
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "have %s, want nil", error_text(err));
    Str text = strings_builder_string(&b);
    Str expected = S("A \xef\xbf\xbd terminated string.");
    if (!str_eq(text, expected))
        testing_t_errorf_v(t, "have %s, want %s", text, expected);
}

static void TestEscape(TestingT *t) {
    StringsBuilder b = STRINGS_BUILDER(a);
    xml_escape(strings_builder_as_io_writer(&b),
               sbytes(S("<a href=\"x\">'&'\t\n\r</a>")));
    Str want = S("&lt;a href=&#34;x&#34;&gt;&#39;&amp;&#39;&#x9;&#xA;&#xD;&lt;/a&gt;");
    Str got = strings_builder_string(&b);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "have %q, want %q", got, want);
}

static void TestIssue11405(TestingT *t) {
    static const char *const cases[] = {"<root>", "<root><foo>", "<root><foo></foo>"};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        XmlDecoder *d = new_decoder(str_from_cstr(cases[i]));
        Error err;
        for (;;) {
            xml_decoder_token(d, &err);
            if (BURROW_FAILED(err))
                break;
        }
        if (!is_syntax_error(err))
            testing_t_errorf_v(t, "%s: Token: Got error %s, want SyntaxError", cases[i],
                               error_text(err));
        xml_decoder_free(d);
    }
}

static void TestIssue12417(TestingT *t) {
    static const struct {
        const char *s;
        bool ok;
    } cases[] = {
        {"<?xml encoding=\"UtF-8\" version=\"1.0\"?><root/>", true},
        {"<?xml encoding=\"UTF-8\" version=\"1.0\"?><root/>", true},
        {"<?xml encoding=\"utf-8\" version=\"1.0\"?><root/>", true},
        {"<?xml encoding=\"uuu-9\" version=\"1.0\"?><root/>", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        XmlDecoder *d = new_decoder(str_from_cstr(cases[i].s));
        Error err;
        for (;;) {
            xml_decoder_token(d, &err);
            if (BURROW_FAILED(err)) {
                if (errors_is(err, io_eof))
                    err = BURROW_NO_ERROR;
                break;
            }
        }
        xml_decoder_free(d);
        if (BURROW_FAILED(err) && cases[i].ok) {
            testing_t_errorf_v(t, "%q: Encoding charset: expected no error, got %s",
                               cases[i].s, error_text(err));
            continue;
        }
        if (BURROW_OK(err) && !cases[i].ok)
            testing_t_errorf_v(t, "%q: Encoding charset: expected error, got nil",
                               cases[i].s);
    }
}

/* The token half of TestIssue20396, which Go runs through Unmarshal. */
static void TestIssue20396(TestingT *t) {
#define ATTR_ERROR "XML syntax error on line 1: expected attribute name in element"
#define NAME_ERROR "XML syntax error on line 1: expected element name after <"
    static const struct {
        const char *s;
        const char *want;
    } cases[] = {
        {"<a:te:st xmlns:a=\"abcd\"/>", NAME_ERROR},
        {"<a:te=st xmlns:a=\"abcd\"/>", ATTR_ERROR},
        {"<a:te&st xmlns:a=\"abcd\"/>", ATTR_ERROR},
        {"<a:test xmlns:a=\"abcd\"/>", NULL},
        {"<a:te:st xmlns:a=\"abcd\">1</a:te:st>", NAME_ERROR},
        {"<a:te=st xmlns:a=\"abcd\">1</a:te=st>", ATTR_ERROR},
        {"<a:te&st xmlns:a=\"abcd\">1</a:te&st>", ATTR_ERROR},
        {"<a:test xmlns:a=\"abcd\">1</a:test>", NULL},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        XmlDecoder *d = new_decoder(str_from_cstr(cases[i].s));
        Error err;
        for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err)) {
        }
        if (errors_is(err, io_eof)) {
            if (cases[i].want != NULL)
                testing_t_errorf_v(t, "%s: Unexpected success, want %s", cases[i].s,
                                   cases[i].want);
        } else if (cases[i].want == NULL) {
            testing_t_errorf_v(t, "%s: Unexpected error, got %s", cases[i].s,
                               error_text(err));
        } else if (!str_eq(error_text(err), str_from_cstr(cases[i].want))) {
            testing_t_errorf_v(t, "%s: got %s, want %s", cases[i].s, error_text(err),
                               cases[i].want);
        }
        xml_decoder_free(d);
    }
#undef ATTR_ERROR
#undef NAME_ERROR
}

static void TestIssue20685(TestingT *t) {
    static const struct {
        const char *s;
        bool ok;
    } cases[] = {
        {"<x:book xmlns:x=\"abcd\" xmlns:y=\"abcd\"><unclosetag>one</x:book>", false},
        {"<x:book xmlns:x=\"abcd\" xmlns:y=\"abcd\">one</x:book>", true},
        {"<x:book xmlns:x=\"abcd\" xmlns:y=\"abcd\">one</y:book>", false},
        {"<x:book xmlns:y=\"abcd\" xmlns:x=\"abcd\">one</y:book>", false},
        {"<x:book xmlns:x=\"abcd\">one</y:book>", false},
        {"<x:book>one</y:book>", false},
        {"<xbook>one</ybook>", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        XmlDecoder *d = new_decoder(str_from_cstr(cases[i].s));
        Error err;
        for (;;) {
            xml_decoder_token(d, &err);
            if (BURROW_FAILED(err)) {
                if (errors_is(err, io_eof))
                    err = BURROW_NO_ERROR;
                break;
            }
        }
        xml_decoder_free(d);
        if (BURROW_FAILED(err) && cases[i].ok) {
            testing_t_errorf_v(
                t, "%q: Closing tag with namespace : expected no error, got %s",
                cases[i].s, error_text(err));
            continue;
        }
        if (BURROW_OK(err) && !cases[i].ok)
            testing_t_errorf_v(
                t, "%q: Closing tag with namespace : expected error, got nil",
                cases[i].s);
    }
}

/* A namespace a nested element redeclares comes back when it closes, and one
 * it declares goes away. */
static void TestNamespaceScope(TestingT *t) {
    static const Str input =
        BURROW_S_INIT("<a xmlns:p=\"one\" xmlns=\"d1\">"
                      "<b xmlns:p=\"two\" xmlns:q=\"three\" xmlns=\"d2\">"
                      "<p:c q:x=\"1\"/></b>"
                      "<p:c q:x=\"2\"/><e/></a>");
    XmlToken want[] = {
        START("d1", "a", AT("xmlns", "p", "one"), AT("", "xmlns", "d1")),
        START("d2", "b", AT("xmlns", "p", "two"), AT("xmlns", "q", "three"),
              AT("", "xmlns", "d2")),
        START("two", "c", AT("three", "x", "1")),
        END("two", "c"),
        END("d2", "b"),
        START("one", "c", AT("q", "x", "2")),
        END("one", "c"),
        START0("d1", "e"),
        END("d1", "e"),
        END("d1", "a"),
    };
    XmlDecoder *d = new_decoder(input);
    for (Int i = 0; i < NTOKS(want); i++) {
        Error err;
        XmlToken have = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "token %d: unexpected error: %s", (int)i,
                               error_text(err));
        if (!tok_eq(have, want[i])) {
            Str sh = tok_show(have), sw = tok_show(want[i]);
            testing_t_errorf_v(t, "token %d = %s want %s", (int)i, sh, sw);
        }
    }
    Error err;
    xml_decoder_token(d, &err);
    CHECK(errors_is(err, io_eof));
    xml_decoder_free(d);
}

/* Go's default_space, and the xml prefix, which needs no declaration. */
static void TestDefaultSpace(TestingT *t) {
    XmlDecoder *d = new_decoder(S("<a xml:lang=\"en\"><b/></a>"));
    d->default_space = S("urn:default");
    Error err;
    XmlToken tok = xml_decoder_token(d, &err);
    CHECK(tok_eq(tok, START("urn:default", "a",
                            AT("http://www.w3.org/XML/1998/namespace", "lang", "en"))));
    tok = xml_decoder_token(d, &err);
    CHECK(tok_eq(tok, START0("urn:default", "b")));
    xml_decoder_free(d);
}

/* Non-strict mode closes an element with whatever end tag comes, and hands
 * the end tag it was given out next. */
static void TestNonStrictMismatch(TestingT *t) {
    XmlDecoder *d = new_decoder(S("<a><b></a>"));
    d->strict = false;
    XmlToken want[] = {START0("", "a"), START0("", "b"), END("", "b"), END("", "a")};
    for (Int i = 0; i < NTOKS(want); i++) {
        Error err;
        XmlToken have = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "token %d: unexpected error: %s", (int)i,
                               error_text(err));
        if (!tok_eq(have, want[i])) {
            Str sh = tok_show(have), sw = tok_show(want[i]);
            testing_t_errorf_v(t, "token %d = %s want %s", (int)i, sh, sw);
        }
    }
    xml_decoder_free(d);
}

static void TestSkip(TestingT *t) {
    XmlDecoder *d = new_decoder(S("<a><b><c/>text</b><d/></a>"));
    Error err;
    xml_decoder_token(d, &err);
    xml_decoder_token(d, &err);
    CHECK(BURROW_OK(xml_decoder_skip(d)));
    XmlToken tok = xml_decoder_token(d, &err);
    CHECK(tok_eq(tok, START0("", "d")));
    xml_decoder_free(d);
}

static void TestNewTokenDecoderIdempotent(TestingT *t) {
    XmlDecoder *d = new_decoder(S("<br>"));
    XmlDecoder *d2 = xml_new_token_decoder(NULL, xml_decoder_as_token_reader(d));
    if (d != d2)
        testing_t_errorf_v(t, "NewTokenDecoder did not detect underlying Decoder");
    xml_decoder_free(d);
}

/* mapper: a TokenReader that passes each token through a function. */
typedef struct Mapper {
    XmlTokenReader t;
} Mapper;

static XmlToken mapper_token(void *self, Error *err) {
    Mapper *m = self;
    XmlToken tok = xml_token_reader_token(m->t, err);
    if (BURROW_FAILED(*err))
        return (XmlToken){0};
    if (tok.kind == XML_START_ELEMENT && str_eq(tok.start.name.local, S("quote")))
        tok.start.name.local = S("blocking");
    if (tok.kind == XML_END_ELEMENT && str_eq(tok.end.name.local, S("quote")))
        tok.end.name.local = S("blocking");
    return tok;
}

static const XmlTokenReaderVT mapper_vt = {NULL, mapper_token};

/* TestWrapDecoder reads the tokens instead of decoding into a struct, which
 * waits for Unmarshal. */
static void TestWrapDecoder(TestingT *t) {
    XmlDecoder *inner =
        new_decoder(S("<quote>[Re-enter Clown with a letter, and FABIAN]</quote>"));
    Mapper m = {xml_decoder_as_token_reader(inner)};
    XmlDecoder *d = xml_new_token_decoder(NULL, (XmlTokenReader){&mapper_vt, &m});
    XmlToken want[] = {
        START0("", "blocking"),
        CD("[Re-enter Clown with a letter, and FABIAN]"),
        END("", "blocking"),
    };
    for (Int i = 0; i < NTOKS(want); i++) {
        Error err;
        XmlToken have = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "token %d: unexpected error: %s", (int)i,
                               error_text(err));
        if (!tok_eq(have, want[i])) {
            Str sh = tok_show(have), sw = tok_show(want[i]);
            testing_t_errorf_v(t, "token %d = %s want %s", (int)i, sh, sw);
        }
    }
    Error err;
    xml_decoder_token(d, &err);
    CHECK(errors_is(err, io_eof));
    xml_decoder_free(d);
    xml_decoder_free(inner);
}

/* The token half of TestDecodeEOF: a token decoder checks the tokens it is
 * given the way it checks the ones it parses. */
static void TestTokenDecoderEOF(TestingT *t) {
    for (int eof = 0; eof < 2; eof++) {
        XmlToken ok[] = {START0("", "test"), END("", "test")};
        Toks tk = {eof == 1, ok, 2};
        XmlDecoder *d = xml_new_token_decoder(NULL, (XmlTokenReader){&toks_vt, &tk});
        Error err;
        for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err)) {
        }
        if (!errors_is(err, io_eof))
            testing_t_errorf_v(t, "OK/earlyEOF=%d: %s", eof, error_text(err));
        xml_decoder_free(d);

        XmlToken bad[] = {START0("", "test"), START0("", "bad"), END("", "test")};
        tk = (Toks){eof == 1, bad, 3};
        d = xml_new_token_decoder(NULL, (XmlTokenReader){&toks_vt, &tk});
        for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err)) {
        }
        if (!is_syntax_error(err))
            testing_t_errorf_v(t,
                               "Malformed/earlyEOF=%d: expected syntax error, got %s",
                               eof, error_text(err));
        xml_decoder_free(d);
    }
}

/* A token decoder translates a copy of the attributes it is given, and leaves
 * the reader's own alone. */
static void TestTokenDecoderAttrs(TestingT *t) {
    XmlAttr attrs[] = {AT("xmlns", "p", "urn:p"), AT("p", "x", "1")};
    XmlToken in[] = {tok_start(NM("p", "a"), attrs, 2), END("p", "a")};
    Toks tk = {false, in, 2};
    XmlDecoder *d = xml_new_token_decoder(NULL, (XmlTokenReader){&toks_vt, &tk});
    Error err;
    XmlToken tok = xml_decoder_token(d, &err);
    CHECK(tok_eq(
        tok, START("urn:p", "a", AT("xmlns", "p", "urn:p"), AT("urn:p", "x", "1"))));
    CHECK(str_eq(attrs[1].name.space, S("p")));
    tok = xml_decoder_token(d, &err);
    CHECK(tok_eq(tok, END("urn:p", "a")));
    xml_decoder_free(d);
}

/* testRoundTrip. */
static void round_trip(TestingT *t, Str input) {
    XmlDecoder *d = new_decoder(input);
    XmlToken tokens[64];
    Int ntokens = 0;
    StringsBuilder buf = STRINGS_BUILDER(a);
    XmlEncoder *e = xml_new_encoder(NULL, strings_builder_as_io_writer(&buf));
    for (;;) {
        Error err;
        XmlToken tok = xml_decoder_token(d, &err);
        if (errors_is(err, io_eof))
            break;
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "invalid input: %s", error_text(err));
        err = xml_encoder_encode_token(e, tok);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to re-encode input: %s", error_text(err));
        if (ntokens == 64)
            testing_t_fatalf_v(t, "too many tokens");
        tokens[ntokens++] = xml_copy_token(a, tok);
    }
    Error err = xml_encoder_flush(e);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    xml_encoder_free(e);
    xml_decoder_free(d);

    d = new_decoder(strings_builder_string(&buf));
    Int i = 0;
    for (;;) {
        XmlToken tok = xml_decoder_token(d, &err);
        if (errors_is(err, io_eof))
            break;
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to decode output: %s", error_text(err));
        if (i == ntokens)
            testing_t_fatalf_v(t, "unexpected token: %s", tok_show(tok));
        if (!tok_eq(tokens[i], tok)) {
            Str sa = tok_show(tokens[i]), sb = tok_show(tok);
            testing_t_fatalf_v(t, "token mismatch: %s vs %s", sa, sb);
        }
        i++;
    }
    if (i < ntokens)
        testing_t_fatalf_v(t, "lost tokens: %d", (int)(ntokens - i));
    xml_decoder_free(d);
}

static void TestRoundTrip(TestingT *t) {
    round_trip(t, S("<foo abc:=\"x\"></foo>"));
    round_trip(
        t, S("<!ENTITY x<!<!-- c1 [ \" -->--x --> > <e></e> <!DOCTYPE xxx [ x<!-- c2 "
             "\" -->--x ]>"));
}

static void TestParseErrors(TestingT *t) {
#define WITH_HEADER(s) "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" s
    static const struct {
        const char *src;
        const char *err;
    } tests[] = {
        {WITH_HEADER("</foo>"), "unexpected end element </foo>"},
        {WITH_HEADER("<x:foo></y:foo>"),
         "element <foo> in space x closed by </foo> in space y"},
        {WITH_HEADER("<? not ok ?>"), "expected target name after <?"},
        {WITH_HEADER("<!- not ok -->"), "invalid sequence <!- not part of <!--"},
        {WITH_HEADER("<!-? not ok -->"), "invalid sequence <!- not part of <!--"},
        {WITH_HEADER("<![not ok]>"), "invalid <![ sequence"},
        {WITH_HEADER("<zzz:foo xmlns:zzz=\"http://example.com\"><bar>baz</bar></foo>"),
         "element <foo> in space zzz closed by </foo> in space \"\""},
        {WITH_HEADER("\xf1"), "invalid UTF-8"},

        /* Header-related errors. */
        {"<?xml version=\"1.1\" encoding=\"UTF-8\"?>",
         "unsupported version \"1.1\"; only version 1.0 is supported"},

        /* Cases below are for "no errors". */
        {WITH_HEADER("<?ok?>"), ""},
        {WITH_HEADER("<?ok version=\"ok\"?>"), ""},
    };
#undef WITH_HEADER
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        XmlDecoder *d = new_decoder(str_from_cstr(tests[i].src));
        Error err;
        for (;;) {
            xml_decoder_token(d, &err);
            if (BURROW_FAILED(err))
                break;
        }
        xml_decoder_free(d);
        if (tests[i].err[0] == 0) {
            if (!errors_is(err, io_eof))
                testing_t_errorf_v(t, "parse %s: have %q error, expected none",
                                   tests[i].src, error_text(err));
            continue;
        }
        if (errors_is(err, io_eof)) {
            testing_t_errorf_v(t, "parse %s: unexpected EOF", tests[i].src);
            continue;
        }
        if (!strings_contains(error_text(err), str_from_cstr(tests[i].err)))
            testing_t_errorf_v(t, "parse %s: can't find %q error substring\nerror: %q",
                               tests[i].src, tests[i].err, error_text(err));
    }
}

static void TestHTMLAutoClose(TestingT *t) {
    static const Str input =
        BURROW_S_INIT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<br>\n"
                      "<br/><br/>\n"
                      "<br><br>\n"
                      "<br></br>\n"
                      "<BR>\n"
                      "<BR/><BR/>\n"
                      "<Br></Br>\n"
                      "<BR><span id=\"test\">abc</span><br/><br/>");
    XmlToken want[] = {
        PI("xml", "version=\"1.0\" encoding=\"UTF-8\""),
        CD("\n"),
        START0("", "br"),
        END("", "br"),
        CD("\n"),
        START0("", "br"),
        END("", "br"),
        START0("", "br"),
        END("", "br"),
        CD("\n"),
        START0("", "br"),
        END("", "br"),
        START0("", "br"),
        END("", "br"),
        CD("\n"),
        START0("", "br"),
        END("", "br"),
        CD("\n"),
        START0("", "BR"),
        END("", "BR"),
        CD("\n"),
        START0("", "BR"),
        END("", "BR"),
        START0("", "BR"),
        END("", "BR"),
        CD("\n"),
        START0("", "Br"),
        END("", "Br"),
        CD("\n"),
        START0("", "BR"),
        END("", "BR"),
        START("", "span", AT("", "id", "test")),
        CD("abc"),
        END("", "span"),
        START0("", "br"),
        END("", "br"),
        START0("", "br"),
        END("", "br"),
    };
    XmlDecoder *d = new_decoder(input);
    d->strict = false;
    d->auto_close = xml_html_auto_close;
    d->entity = xml_html_entity();
    XmlToken have[64];
    Int nhave = 0;
    for (;;) {
        Error err;
        XmlToken tok = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err)) {
            if (errors_is(err, io_eof))
                break;
            testing_t_fatalf_v(t, "unexpected error: %s", error_text(err));
        }
        if (nhave < 64)
            have[nhave++] = xml_copy_token(a, tok);
    }
    xml_decoder_free(d);
    if (nhave != NTOKS(want))
        testing_t_errorf_v(t, "tokens count mismatch: have %d, want %d", (int)nhave,
                           (int)NTOKS(want));
    for (Int i = 0; i < NTOKS(want); i++) {
        if (i >= nhave) {
            Str sw = tok_show(want[i]);
            testing_t_errorf_v(t, "token[%d] expected %s, have no token", (int)i, sw);
        } else if (!tok_eq(have[i], want[i])) {
            Str sh = tok_show(have[i]), sw = tok_show(want[i]);
            testing_t_errorf_v(t, "token[%d] mismatch:\nhave: %s\nwant: %s", (int)i, sh,
                               sw);
        }
    }
}

static void TestHTMLEntity(TestingT *t) {
    Map *m = xml_html_entity();
    CHECK(m != NULL && map_len(m) == 252);
    CHECK(xml_html_entity() == m);
    Str *v = BURROW_MAP_GET(Str, Str, m, S("eacute"));
    CHECK(v != NULL && str_eq(*v, S("\xc3\xa9")));
    XmlDecoder *d = new_decoder(S("<p>caf&eacute; &nbsp;&amp;</p>"));
    d->entity = m;
    Error err;
    xml_decoder_token(d, &err);
    XmlToken tok = xml_decoder_token(d, &err);
    CHECK(tok_eq(tok, CD("caf\xc3\xa9 \xc2\xa0&")));
    xml_decoder_free(d);
}

/* A token is good until the next call, and one auto_close held back survives
 * the call that held it. */
static void TestTokenLifetime(TestingT *t) {
    XmlDecoder *d = new_decoder(S("<a><br><p x=\"1\">t</p></a>"));
    d->strict = false;
    d->auto_close = xml_html_auto_close;
    XmlToken want[] = {
        START0("", "a"), START0("", "br"),
        END("", "br"),   START("", "p", AT("", "x", "1")),
        CD("t"),         END("", "p"),
        END("", "a"),
    };
    for (Int i = 0; i < NTOKS(want); i++) {
        Error err;
        XmlToken have = xml_decoder_token(d, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "token %d: unexpected error: %s", (int)i,
                               error_text(err));
        if (!tok_eq(have, want[i])) {
            Str sh = tok_show(have), sw = tok_show(want[i]);
            testing_t_errorf_v(t, "token %d = %s want %s", (int)i, sh, sw);
        }
    }
    xml_decoder_free(d);
}

/* ------------------------------------------------------------- the encoder */

static void TestEncodeToken(TestingT *t) {
    static const Str xml_url = BURROW_S_INIT("http://www.w3.org/XML/1998/namespace");
    struct {
        const char *desc;
        XmlToken toks[8];
        Int n;
        const char *want;
        const char *err;
    } tests[] = {
        {"start element with name space",
         {START0("space", "local")},
         1,
         "<local xmlns=\"space\">",
         NULL},
        {"start element with no name",
         {START0("space", "")},
         1,
         "",
         "xml: start tag with no name"},
        {"end element with no name",
         {END("space", "")},
         1,
         "",
         "xml: end tag with no name"},
        {"char data", {CD("foo")}, 1, "foo", NULL},
        {"char data with escaped chars", {CD(" \t\n")}, 1, " &#x9;\n", NULL},
        {"comment", {COMMENT("foo")}, 1, "<!--foo-->", NULL},
        {"comment with invalid content",
         {COMMENT("foo-->")},
         1,
         "",
         "xml: EncodeToken of Comment containing --> marker"},
        {"proc instruction",
         {PI("Target", "Instruction")},
         1,
         "<?Target Instruction?>",
         NULL},
        {"proc instruction with empty target",
         {PI("", "Instruction")},
         1,
         "",
         "xml: EncodeToken of ProcInst with invalid Target"},
        {"proc instruction with bad content",
         {PI("", "Instruction?>")},
         1,
         "",
         "xml: EncodeToken of ProcInst with invalid Target"},
        {"directive", {DIR("foo")}, 1, "<!foo>", NULL},
        {"more complex directive",
         {DIR("DOCTYPE doc [ <!ELEMENT doc '>'> <!-- com>ment --> ]")},
         1,
         "<!DOCTYPE doc [ <!ELEMENT doc '>'> <!-- com>ment --> ]>",
         NULL},
        {"directive instruction with bad name",
         {DIR("foo>")},
         1,
         "",
         "xml: EncodeToken of Directive containing wrong < or > markers"},
        {"end tag without start tag",
         {END("foo", "bar")},
         1,
         "",
         "xml: end tag </bar> without start tag"},
        {"mismatching end tag local name",
         {START0("", "foo"), END("", "bar")},
         2,
         "<foo>",
         "xml: end tag </bar> does not match start tag <foo>"},
        {"mismatching end tag namespace",
         {START0("space", "foo"), END("another", "foo")},
         2,
         "<foo xmlns=\"space\">",
         "xml: end tag </foo> in namespace another does not match start tag <foo> in "
         "namespace "
         "space"},
        {"start element with explicit namespace",
         {START("space", "local", AT("xmlns", "x", "space"),
                AT("space", "foo", "value"))},
         1,
         "<local xmlns=\"space\" xmlns:_xmlns=\"xmlns\" _xmlns:x=\"space\" "
         "xmlns:space=\"space\" space:foo=\"value\">",
         NULL},
        {"start element with explicit namespace and colliding prefix",
         {START("space", "local", AT("xmlns", "x", "space"),
                AT("space", "foo", "value"), AT("x", "bar", "other"))},
         1,
         "<local xmlns=\"space\" xmlns:_xmlns=\"xmlns\" _xmlns:x=\"space\" "
         "xmlns:space=\"space\" space:foo=\"value\" xmlns:x=\"x\" x:bar=\"other\">",
         NULL},
        {"start element using previously defined namespace",
         {START("", "local", AT("xmlns", "x", "space")),
          START("space", "foo", AT("space", "x", "y"))},
         2,
         "<local xmlns:_xmlns=\"xmlns\" _xmlns:x=\"space\"><foo xmlns=\"space\" "
         "xmlns:space=\"space\" space:x=\"y\">",
         NULL},
        {"nested name space with same prefix",
         {START("", "foo", AT("xmlns", "x", "space1")),
          START("", "foo", AT("xmlns", "x", "space2")),
          START("", "foo", AT("space1", "a", "space1 value"),
                AT("space2", "b", "space2 value")),
          END("", "foo"), END("", "foo"),
          START("", "foo", AT("space1", "a", "space1 value"),
                AT("space2", "b", "space2 value"))},
         6,
         "<foo xmlns:_xmlns=\"xmlns\" _xmlns:x=\"space1\"><foo "
         "_xmlns:x=\"space2\"><foo "
         "xmlns:space1=\"space1\" space1:a=\"space1 value\" xmlns:space2=\"space2\" "
         "space2:b=\"space2 value\"></foo></foo><foo xmlns:space1=\"space1\" "
         "space1:a=\"space1 "
         "value\" xmlns:space2=\"space2\" space2:b=\"space2 value\">",
         NULL},
        {"start element defining several prefixes for the same name space",
         {START("space", "foo", AT("xmlns", "a", "space"), AT("xmlns", "b", "space"),
                AT("space", "x", "value"))},
         1,
         "<foo xmlns=\"space\" xmlns:_xmlns=\"xmlns\" _xmlns:a=\"space\" "
         "_xmlns:b=\"space\" "
         "xmlns:space=\"space\" space:x=\"value\">",
         NULL},
        {"nested element redefines name space",
         {START("", "foo", AT("xmlns", "x", "space")),
          START("space", "foo", AT("xmlns", "y", "space"), AT("space", "a", "value"))},
         2,
         "<foo xmlns:_xmlns=\"xmlns\" _xmlns:x=\"space\"><foo xmlns=\"space\" "
         "_xmlns:y=\"space\" xmlns:space=\"space\" space:a=\"value\">",
         NULL},
        {"nested element creates alias for default name space",
         {START("space", "foo", AT("", "xmlns", "space")),
          START("space", "foo", AT("xmlns", "y", "space"), AT("space", "a", "value"))},
         2,
         "<foo xmlns=\"space\" xmlns=\"space\"><foo xmlns=\"space\" "
         "xmlns:_xmlns=\"xmlns\" "
         "_xmlns:y=\"space\" xmlns:space=\"space\" space:a=\"value\">",
         NULL},
        {"nested element defines default name space with existing prefix",
         {START("", "foo", AT("xmlns", "x", "space")),
          START("space", "foo", AT("", "xmlns", "space"), AT("space", "a", "value"))},
         2,
         "<foo xmlns:_xmlns=\"xmlns\" _xmlns:x=\"space\"><foo xmlns=\"space\" "
         "xmlns=\"space\" "
         "xmlns:space=\"space\" space:a=\"value\">",
         NULL},
        {"nested element uses empty attribute name space when default ns defined",
         {START("space", "foo", AT("", "xmlns", "space")),
          START("space", "foo", AT("", "attr", "value"))},
         2,
         "<foo xmlns=\"space\" xmlns=\"space\"><foo xmlns=\"space\" attr=\"value\">",
         NULL},
        {"redefine xmlns",
         {START("", "foo", AT("foo", "xmlns", "space"))},
         1,
         "<foo xmlns:foo=\"foo\" foo:xmlns=\"space\">",
         NULL},
        {"xmlns with explicit name space #1",
         {START("space", "foo", AT("xml", "xmlns", "space"))},
         1,
         "<foo xmlns=\"space\" xmlns:_xml=\"xml\" _xml:xmlns=\"space\">",
         NULL},
        {"xmlns with explicit name space #2",
         {tok_start(NM("space", "foo"),
                    (XmlAttr[]){{{xml_url, S("xmlns")}, S("space")}}, 1)},
         1,
         "<foo xmlns=\"space\" xml:xmlns=\"space\">",
         NULL},
        {"empty name space declaration is ignored",
         {START("", "foo", AT("xmlns", "foo", ""))},
         1,
         "<foo xmlns:_xmlns=\"xmlns\" _xmlns:foo=\"\">",
         NULL},
        {"attribute with no name is ignored",
         {START("", "foo", AT("", "", "value"))},
         1,
         "<foo>",
         NULL},
        {"namespace URL with non-valid name",
         {START("/34", "foo", AT("/34", "x", "value"))},
         1,
         "<foo xmlns=\"/34\" xmlns:_=\"/34\" _:x=\"value\">",
         NULL},
        {"nested element resets default namespace to empty",
         {START("space", "foo", AT("", "xmlns", "space")),
          START("", "foo", AT("", "xmlns", ""), AT("", "x", "value"),
                AT("space", "x", "value"))},
         2,
         "<foo xmlns=\"space\" xmlns=\"space\"><foo xmlns=\"\" x=\"value\" "
         "xmlns:space=\"space\" space:x=\"value\">",
         NULL},
        {"nested element requires empty default name space",
         {START("space", "foo", AT("", "xmlns", "space")), START0("", "foo")},
         2,
         "<foo xmlns=\"space\" xmlns=\"space\"><foo>",
         NULL},
        {"attribute uses name space from xmlns",
         {START("some/space", "foo", AT("", "attr", "value"),
                AT("some/space", "other", "other value"))},
         1,
         "<foo xmlns=\"some/space\" attr=\"value\" xmlns:space=\"some/space\" "
         "space:other=\"other value\">",
         NULL},
        {"default name space should not be used by attributes",
         {START("space", "foo", AT("", "xmlns", "space"), AT("xmlns", "bar", "space"),
                AT("space", "baz", "foo")),
          START0("space", "baz"), END("space", "baz"), END("space", "foo")},
         4,
         "<foo xmlns=\"space\" xmlns=\"space\" xmlns:_xmlns=\"xmlns\" "
         "_xmlns:bar=\"space\" "
         "xmlns:space=\"space\" space:baz=\"foo\"><baz xmlns=\"space\"></baz></foo>",
         NULL},
        {"default name space not used by attributes, not explicitly defined",
         {START("space", "foo", AT("", "xmlns", "space"), AT("space", "baz", "foo")),
          START0("space", "baz"), END("space", "baz"), END("space", "foo")},
         4,
         "<foo xmlns=\"space\" xmlns=\"space\" xmlns:space=\"space\" "
         "space:baz=\"foo\"><baz "
         "xmlns=\"space\"></baz></foo>",
         NULL},
        {"impossible xmlns declaration",
         {START("", "foo", AT("", "xmlns", "space")),
          START("space", "bar", AT("space", "attr", "value"))},
         2,
         "<foo xmlns=\"space\"><bar xmlns=\"space\" xmlns:space=\"space\" "
         "space:attr=\"value\">",
         NULL},
        {"reserved namespace prefix -- all lower case",
         {START("", "foo",
                AT("http://www.w3.org/2001/xmlSchema-instance", "nil", "true"))},
         1,
         "<foo xmlns:_xmlSchema-instance=\"http://www.w3.org/2001/xmlSchema-instance\" "
         "_xmlSchema-instance:nil=\"true\">",
         NULL},
        {"reserved namespace prefix -- all upper case",
         {START("", "foo",
                AT("http://www.w3.org/2001/XMLSchema-instance", "nil", "true"))},
         1,
         "<foo xmlns:_XMLSchema-instance=\"http://www.w3.org/2001/XMLSchema-instance\" "
         "_XMLSchema-instance:nil=\"true\">",
         NULL},
        {"reserved namespace prefix -- all mixed case",
         {START("", "foo",
                AT("http://www.w3.org/2001/XmLSchema-instance", "nil", "true"))},
         1,
         "<foo xmlns:_XmLSchema-instance=\"http://www.w3.org/2001/XmLSchema-instance\" "
         "_XmLSchema-instance:nil=\"true\">",
         NULL},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        StringsBuilder buf = STRINGS_BUILDER(a);
        XmlEncoder *enc = xml_new_encoder(NULL, strings_builder_as_io_writer(&buf));
        Error err = BURROW_NO_ERROR;
        bool skip = false;
        for (Int j = 0; j < tests[i].n; j++) {
            err = xml_encoder_encode_token(enc, tests[i].toks[j]);
            if (BURROW_FAILED(err) && j < tests[i].n - 1) {
                testing_t_errorf_v(t, "#%d %s token #%d: %s", (int)i, tests[i].desc,
                                   (int)j, error_text(err));
                skip = true;
                break;
            }
        }
        int last = (int)tests[i].n - 1;
        if (skip) {
        } else if (tests[i].err != NULL && BURROW_OK(err)) {
            testing_t_errorf_v(t, "#%d %s token #%d: expected error; got none", (int)i,
                               tests[i].desc, last);
        } else if (tests[i].err == NULL && BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d %s token #%d: got error: %s", (int)i,
                               tests[i].desc, last, error_text(err));
        } else if (tests[i].err != NULL &&
                   !str_eq(error_text(err), str_from_cstr(tests[i].err))) {
            testing_t_errorf_v(t, "#%d %s token #%d: error mismatch; got %s, want %s",
                               (int)i, tests[i].desc, last, error_text(err),
                               tests[i].err);
        } else {
            err = xml_encoder_flush(enc);
            Str got = strings_builder_string(&buf);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "#%d %s token #%d: %s", (int)i, tests[i].desc,
                                   last, error_text(err));
            else if (!str_eq(got, str_from_cstr(tests[i].want)))
                testing_t_errorf_v(t, "#%d %s token #%d:\ngot  %s\nwant %s", (int)i,
                                   tests[i].desc, last, got, tests[i].want);
        }
        xml_encoder_free(enc);
    }
}

static void TestMarshalFlush(TestingT *t) {
    StringsBuilder buf = STRINGS_BUILDER(a);
    XmlEncoder *enc = xml_new_encoder(NULL, strings_builder_as_io_writer(&buf));
    Error err = xml_encoder_encode_token(enc, CD("hello world"));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "enc.EncodeToken: %s", error_text(err));
    if (strings_builder_len(&buf) > 0)
        testing_t_fatalf_v(t, "enc.EncodeToken caused actual write: %q",
                           strings_builder_string(&buf));
    err = xml_encoder_flush(enc);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "enc.Flush: %s", error_text(err));
    if (!str_eq(strings_builder_string(&buf), S("hello world")))
        testing_t_fatalf_v(t, "after enc.Flush, buf.String() = %q, want %q",
                           strings_builder_string(&buf), "hello world");
    xml_encoder_free(enc);
}

static void TestProcInstEncodeToken(TestingT *t) {
    StringsBuilder buf = STRINGS_BUILDER(a);
    XmlEncoder *enc = xml_new_encoder(NULL, strings_builder_as_io_writer(&buf));
    Error err = xml_encoder_encode_token(enc, PI("xml", "Instruction"));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t,
                           "enc.EncodeToken: expected to be able to encode xml target "
                           "ProcInst as first token, %s",
                           error_text(err));
    err = xml_encoder_encode_token(enc, PI("Target", "Instruction"));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t,
                           "enc.EncodeToken: expected to be able to add non-xml target "
                           "ProcInst");
    err = xml_encoder_encode_token(enc, PI("xml", "Instruction"));
    if (BURROW_OK(err))
        testing_t_fatalf_v(t,
                           "enc.EncodeToken: expected to not be allowed to encode xml "
                           "target ProcInst when not first token");
    xml_encoder_free(enc);
}

static void TestDecodeEncode(TestingT *t) {
    StringsBuilder out = STRINGS_BUILDER(a);
    XmlDecoder *dec = new_decoder(S("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                    "<?Target Instruction?>\n"
                                    "<root>\n"
                                    "</root>\n"));
    XmlEncoder *enc = xml_new_encoder(NULL, strings_builder_as_io_writer(&out));
    Error err;
    for (XmlToken tok = xml_decoder_token(dec, &err); BURROW_OK(err);
         tok = xml_decoder_token(dec, &err)) {
        err = xml_encoder_encode_token(enc, tok);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "enc.EncodeToken: Unable to encode token (%s), %s",
                               tok_show(tok), error_text(err));
    }
    xml_encoder_free(enc);
    xml_decoder_free(dec);
}

static void TestIsValidDirective(TestingT *t) {
    static const char *const test_ok[] = {
        "<>",
        "< < > >",
        "<!DOCTYPE '<' '>' '>' <!--nothing-->>",
        "<!DOCTYPE doc [ <!ELEMENT doc ANY> <!ELEMENT doc ANY> ]>",
        "<!DOCTYPE doc [ <!ELEMENT doc \"ANY> '<' <!E\" LEMENT '>' doc ANY> ]>",
        "<!DOCTYPE doc <!-- just>>>> a < comment --> [ <!ITEM anything> ] >",
    };
    static const char *const test_ko[] = {
        "<",
        ">",
        "<!--",
        "-->",
        "< > > < < >",
        "<!dummy <!-- > -->",
        "<!DOCTYPE doc '>",
        "<!DOCTYPE doc '>'",
        "<!DOCTYPE doc <!--comment>",
    };
    for (size_t i = 0; i < sizeof test_ok / sizeof test_ok[0]; i++)
        if (!burrow__xml_is_valid_directive(sbytes(str_from_cstr(test_ok[i]))))
            testing_t_errorf_v(t, "Directive %q is expected to be valid", test_ok[i]);
    for (size_t i = 0; i < sizeof test_ko / sizeof test_ko[0]; i++)
        if (burrow__xml_is_valid_directive(sbytes(str_from_cstr(test_ko[i]))))
            testing_t_errorf_v(t, "Directive %q is expected to be invalid", test_ko[i]);
}

/* Issue 11719. EncodeToken used to silently eat tokens with an invalid type. */
static void TestSimpleUseOfEncodeToken(TestingT *t) {
    StringsBuilder buf = STRINGS_BUILDER(a);
    XmlEncoder *enc = xml_new_encoder(NULL, strings_builder_as_io_writer(&buf));
    Error err = xml_encoder_encode_token(enc, START0("", "object2"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "enc.EncodeToken: StartElement %s", error_text(err));
    err = xml_encoder_encode_token(enc, END("", "object2"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "enc.EncodeToken: EndElement %s", error_text(err));
    err = xml_encoder_encode_token(enc, (XmlToken){0});
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "enc.EncodeToken: invalid type not caught");
    else if (!str_eq(error_text(err), S("xml: EncodeToken of invalid token type")))
        testing_t_errorf_v(t, "enc.EncodeToken: %s", error_text(err));
    err = xml_encoder_flush(enc);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "enc.Flush: %s", error_text(err));
    Str want = S("<object2></object2>");
    if (!str_eq(strings_builder_string(&buf), want))
        testing_t_errorf_v(t, "enc.EncodeToken: expected %q; got %q", want,
                           strings_builder_string(&buf));
    xml_encoder_free(enc);
}

static void TestClose(TestingT *t) {
    struct {
        const char *desc;
        XmlToken toks[2];
        Int n;
        const char *want;
        const char *err;
    } tests[] = {
        {"unclosed start element",
         {START0("", "foo")},
         1,
         "<foo>",
         "unclosed tag <foo>"},
        {"closed element", {START0("", "foo"), END("", "foo")}, 2, "<foo></foo>", NULL},
        {"directive", {DIR("foo")}, 1, "<!foo>", NULL},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        StringsBuilder out = STRINGS_BUILDER(a);
        XmlEncoder *enc = xml_new_encoder(NULL, strings_builder_as_io_writer(&out));
        for (Int j = 0; j < tests[i].n; j++) {
            Error err = xml_encoder_encode_token(enc, tests[i].toks[j]);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s: token #%d: %s", tests[i].desc, (int)j,
                                   error_text(err));
        }
        Error err = xml_encoder_close(enc);
        if (tests[i].err != NULL && BURROW_OK(err))
            testing_t_errorf_v(t, "%s: expected error; got none", tests[i].desc);
        else if (tests[i].err == NULL && BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: got error: %s", tests[i].desc, error_text(err));
        else if (tests[i].err != NULL &&
                 !str_eq(error_text(err), str_from_cstr(tests[i].err)))
            testing_t_errorf_v(t, "%s: error mismatch; got %s, want %s", tests[i].desc,
                               error_text(err), tests[i].err);
        Str got = strings_builder_string(&out);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%s:\ngot  %s\nwant %s", tests[i].desc, got,
                               tests[i].want);
        err = xml_encoder_encode_token(enc, DIR("foo"));
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "%s: unexpected success when encoding after Close",
                               tests[i].desc);
        else if (!str_eq(error_text(err), S("use of closed Encoder")))
            testing_t_errorf_v(t, "%s: after Close: %s", tests[i].desc,
                               error_text(err));
        CHECK(BURROW_OK(xml_encoder_close(enc)));
        xml_encoder_free(enc);
    }
}

/* Indent, which Go tests through MarshalIndent. */
static void TestEncoderIndent(TestingT *t) {
    StringsBuilder out = STRINGS_BUILDER(a);
    XmlEncoder *enc = xml_new_encoder(NULL, strings_builder_as_io_writer(&out));
    xml_encoder_indent(enc, S(">"), S("  "));
    XmlToken toks[] = {
        START0("", "a"), START0("", "b"), CD("x"),      END("", "b"),
        START0("", "c"), END("", "c"),    END("", "a"),
    };
    for (Int i = 0; i < NTOKS(toks); i++)
        CHECK(BURROW_OK(xml_encoder_encode_token(enc, toks[i])));
    CHECK(BURROW_OK(xml_encoder_close(enc)));
    Str want = S(">\x3c\x61>\n>  <b>x</b>\n>  <c></c>\n></a>");
    Str got = strings_builder_string(&out);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q, want %q", got, want);
    xml_encoder_free(enc);
}

/* ----------------------------------------------------------- out of memory */

/* An allocator that refuses after a number of allocations and counts what is
 * live, so that nothing leaks when it runs out. */
typedef struct Budget {
    int left;
    long long live;
} Budget;

static void *budget_alloc(void *self, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *p = mem_alloc(heap_allocator(), size, align);
    if (p != NULL)
        b->live += (long long)size;
    return p;
}

static void *budget_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *q = mem_realloc(heap_allocator(), p, old, nsz, align);
    if (q != NULL)
        b->live += (long long)nsz - (long long)old;
    return q;
}

static void budget_free(void *self, void *p, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (p == NULL)
        return;
    b->live -= (long long)size;
    mem_free(heap_allocator(), p, size, align);
}

static const AllocVT budget_vt = {budget_alloc, NULL, budget_realloc,
                                  budget_free,  NULL, NULL};

/* Every allocation the decoder and encoder make, refused in turn. Each run
 * either fails with the out of memory error or gets the right tokens, and
 * gives back everything it took. */
static void TestNoMemory(TestingT *t) {
    static const Str input =
        BURROW_S_INIT("<?xml version=\"1.0\"?><a xmlns:p=\"urn:p\" "
                      "xmlns=\"urn:d\"><p:b x=\"1\" p:y=\"2\">"
                      "text &amp; more<![CDATA[cdata]]><!-- c --></p:b><br><c "
                      "xmlns:p=\"urn:q\"/></a>");
    bool done = false;
    for (int budget = 0; budget < 400 && !done; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        XmlDecoder *d = xml_new_decoder(&al, strings_io(input));
        XmlEncoder *e = NULL;
        if (d != NULL) {
            d->strict = false;
            d->auto_close = xml_html_auto_close;
            e = xml_new_encoder(&al, (IoWriter){&err_writer_vt, NULL});
            Error err;
            int n = 0;
            for (;;) {
                XmlToken tok = xml_decoder_token(d, &err);
                if (BURROW_FAILED(err))
                    break;
                n++;
                if (e != NULL)
                    xml_encoder_encode_token(e, tok);
            }
            if (errors_is(err, io_eof)) {
                if (n != 12)
                    testing_t_errorf_v(t, "budget %d: %d tokens, want 12", budget, n);
                done = e != NULL;
            } else if (!errors_is(err, burrow_err_out_of_memory)) {
                testing_t_errorf_v(t, "budget %d: %s", budget, error_text(err));
            }
        }
        xml_encoder_free(e);
        xml_decoder_free(d);
        if (b.live != 0)
            testing_t_errorf_v(t, "budget %d: %d bytes still live", budget,
                               (int)b.live);
    }
    if (!done)
        testing_t_errorf_v(t, "never had enough memory");
}

#define TESTS(X)                                                                       \
    X(TestRawToken)                                                                    \
    X(TestNonStrictRawToken)                                                           \
    X(TestRawTokenAltEncoding)                                                         \
    X(TestRawTokenAltEncodingNoConverter)                                              \
    X(TestCharsetReaderError)                                                          \
    X(TestCharsetReaderNil)                                                            \
    X(TestNestedDirectives)                                                            \
    X(TestToken)                                                                       \
    X(TestSyntax)                                                                      \
    X(TestInputLinePos)                                                                \
    X(TestIssue68387)                                                                  \
    X(TestUnquotedAttrs)                                                               \
    X(TestValuelessAttrs)                                                              \
    X(TestCopyTokenCharData)                                                           \
    X(TestCopyTokenStartElement)                                                       \
    X(TestCopyTokenComment)                                                            \
    X(TestCopyTokenFree)                                                               \
    X(TestSyntaxErrorLineNum)                                                          \
    X(TestSyntaxErrorText)                                                             \
    X(TestTrailingRawToken)                                                            \
    X(TestTrailingToken)                                                               \
    X(TestEntityInsideCDATA)                                                           \
    X(TestDisallowedCharacters)                                                        \
    X(TestDisallowedCharacterWide)                                                     \
    X(TestIsInCharacterRange)                                                          \
    X(TestProcInstEncoding)                                                            \
    X(TestDirectivesWithComments)                                                      \
    X(TestEscapeTextIOErrors)                                                          \
    X(TestEscapeTextInvalidChar)                                                       \
    X(TestEscape)                                                                      \
    X(TestIssue11405)                                                                  \
    X(TestIssue12417)                                                                  \
    X(TestIssue20396)                                                                  \
    X(TestIssue20685)                                                                  \
    X(TestNamespaceScope)                                                              \
    X(TestDefaultSpace)                                                                \
    X(TestNonStrictMismatch)                                                           \
    X(TestSkip)                                                                        \
    X(TestNewTokenDecoderIdempotent)                                                   \
    X(TestWrapDecoder)                                                                 \
    X(TestTokenDecoderEOF)                                                             \
    X(TestTokenDecoderAttrs)                                                           \
    X(TestRoundTrip)                                                                   \
    X(TestParseErrors)                                                                 \
    X(TestHTMLAutoClose)                                                               \
    X(TestHTMLEntity)                                                                  \
    X(TestTokenLifetime)                                                               \
    X(TestEncodeToken)                                                                 \
    X(TestMarshalFlush)                                                                \
    X(TestProcInstEncodeToken)                                                         \
    X(TestDecodeEncode)                                                                \
    X(TestIsValidDirective)                                                            \
    X(TestSimpleUseOfEncodeToken)                                                      \
    X(TestClose)                                                                       \
    X(TestEncoderIndent)                                                               \
    X(TestNoMemory)

static int TestMain(TestingM *m) {
    setup();
    int rc = testing_m_run(m);
    teardown();
    return rc;
}

TESTING_MAIN_WITH(TestMain, TESTS)
