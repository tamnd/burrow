#!/bin/sh
# Regenerates tests/encoding_jsontext_test_gen.h: what Go's encoding/json/jsontext
# does with its own test tables. The tables are unexported and the package
# imports encoding/json/internal, so rather than copy it out this lays a file
# over the package with go test -overlay that runs each case and prints what
# happened as C. Recording what Go did, instead of what the tables say it
# should do, means the C test checks the same thing either way and also sees
# the pointers, offsets and error texts the tables leave out. Nothing here
# depends on the machine, so it runs wherever go does.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/encoding_jsontext_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pkg=$(go env GOROOT)/src/encoding/json/jsontext

cat > "$tmp/zz_gen_test.go" <<'GO'
//go:build goexperiment.jsonv2

package jsontext

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"strings"
	"testing"

	"encoding/json/internal/jsonflags"
	"encoding/json/internal/jsonopts"
)

var w strings.Builder

func pf(format string, args ...any) { fmt.Fprintf(&w, format, args...) }

// C string literal of s, with octal escapes so nothing runs into the next
// character.
func lit(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '?':
			b.WriteString(`\077`)
		case c >= ' ' && c <= '~':
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func qs(s string) string { return "QS(" + lit(s) + ")" }

func errText(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

func optsLit(opts []Options) string {
	var s jsonopts.Struct
	s.Join(opts...)
	return fmt.Sprintf("{%#xULL, %#xULL, %s, %s, %d, %d}", s.Flags.Presence, s.Flags.Values,
		qs(s.Indent), qs(s.IndentPrefix), s.ByteLimit, s.DepthLimit)
}

// How the C test builds the same token: z for the zero Token, r for one
// over raw text, and s, f, F, i or u for what String, Float, Float32, Int and
// Uint make.
func tokLit(t Token) string {
	switch {
	case t.raw != nil:
		return fmt.Sprintf("{'r', %s, 0}", qs(string(t.raw.previousBuffer())))
	case t.num != 0:
		return fmt.Sprintf("{'%s', QS(\"\"), %#xULL}", t.str, t.num)
	case t.str != "":
		return fmt.Sprintf("{'s', %s, 0}", qs(t.str))
	default:
		return "{'z', QS(\"\"), 0}"
	}
}

func kindLit(k Kind) string {
	if k == 0 {
		return "0"
	}
	return fmt.Sprintf("'%c'", byte(k))
}

func genCoder() {
	var toks, ptrs, decs []string
	pf("static const JtCoderCase jt_coder_cases[] = {\n")
	for _, td := range coderTestdata {
		indented := td.outIndented
		if indented == "" {
			indented = td.outCompacted
		}
		canon := td.outCanonicalized
		if canon == "" {
			canon = td.outCompacted
		}
		tokStart := len(toks)
		for _, t := range td.tokens {
			toks = append(toks, tokLit(t))
		}
		ptrStart := len(ptrs)
		for _, p := range td.pointers {
			ptrs = append(ptrs, qs(string(p)))
		}
		// What Go's decoder reads, kind and String of each token.
		decStart := len(decs)
		dec := NewDecoder(bytes.NewBufferString(td.in))
		for {
			tok, err := dec.ReadToken()
			if err == io.EOF {
				break
			} else if err != nil {
				panic(err)
			}
			decs = append(decs, fmt.Sprintf("{%s, %s}", kindLit(tok.Kind()), qs(tok.String())))
		}
		pf("    {%s, %s, %s, %s, %s, %d, %d, %d, %d, %d},\n", lit(td.name.Name), qs(td.in),
			qs(td.outCompacted), qs(indented), qs(canon), tokStart, len(td.tokens),
			ptrStart, len(td.pointers), decStart)
		_ = td.pointers == nil
	}
	pf("};\n\n")
	pf("static const JtTok jt_coder_tokens[] = {\n")
	for _, s := range toks {
		pf("    %s,\n", s)
	}
	pf("};\n\n")
	pf("static const QStr jt_coder_pointers[] = {\n")
	for _, s := range ptrs {
		pf("    %s,\n", s)
	}
	pf("};\n\n")
	pf("static const JtRead jt_coder_decoded[] = {\n")
	for _, s := range decs {
		pf("    %s,\n", s)
	}
	pf("};\n\n")
}

func genEncoderErrors() {
	var calls []string
	pf("static const JtEncCase jt_enc_cases[] = {\n")
	for _, td := range encoderErrorTestdata {
		dst := new(bytes.Buffer)
		enc := NewEncoder(dst, td.opts...)
		start := len(calls)
		for _, call := range td.calls {
			var err error
			var in string
			switch tv := call.in.(type) {
			case Token:
				err = enc.WriteToken(tv)
				in = fmt.Sprintf("0, %s, QS(\"\")", tokLit(tv))
			case Value:
				err = enc.WriteValue(tv)
				in = fmt.Sprintf("1, {'z', QS(\"\"), 0}, %s", qs(string(tv)))
			}
			calls = append(calls, fmt.Sprintf("{%s, %s, %s, %d}", in, lit(errText(err)),
				qs(string(enc.StackPointer())), enc.StackDepth()))
		}
		got := dst.String() + string(enc.s.unflushedBuffer())
		pf("    {%s, %s, %d, %d, %s, %d},\n", lit(td.name.Name), optsLit(td.opts), start,
			len(td.calls), qs(got), enc.OutputOffset())
	}
	pf("};\n\n")
	pf("static const JtEncCall jt_enc_calls[] = {\n")
	for _, s := range calls {
		pf("    %s,\n", s)
	}
	pf("};\n\n")
}

func genDecoderErrors() {
	var calls []string
	pf("static const JtDecCase jt_dec_cases[] = {\n")
	for _, td := range decoderErrorTestdata {
		dec := NewDecoder(bytes.NewBufferString(td.in), td.opts...)
		start := len(calls)
		for _, call := range td.calls {
			kind := dec.PeekKind()
			var err error
			var out string
			isValue := 0
			switch call.wantOut.(type) {
			case Token:
				var tok Token
				tok, err = dec.ReadToken()
				out = tok.String()
			case Value:
				var v Value
				v, err = dec.ReadValue()
				out = string(v)
				isValue = 1
			}
			calls = append(calls, fmt.Sprintf("{%d, %s, %s, %s, %s, %d}", isValue, kindLit(kind),
				qs(out), lit(errText(err)), qs(string(dec.StackPointer())), dec.StackDepth()))
		}
		pf("    {%s, %s, %s, %d, %d, %d, %s},\n", lit(td.name.Name), optsLit(td.opts), qs(td.in),
			start, len(td.calls), dec.InputOffset(), qs(string(dec.s.unreadBuffer())))
	}
	pf("};\n\n")
	pf("static const JtDecCall jt_dec_calls[] = {\n")
	for _, s := range calls {
		pf("    %s,\n", s)
	}
	pf("};\n\n")
}

func genValues() {
	pf("static const JtValueCase jt_value_cases[] = {\n")
	for _, td := range valueTestdata {
		v := Value(td.in)
		valid := v.IsValid()
		c := Value(td.in)
		cerr := c.Compact()
		i := Value(td.in)
		ierr := i.Indent(WithIndentPrefix("\t"), WithIndent("    "))
		k := Value(td.in)
		kerr := k.Canonicalize()
		pf("    {%s, %s, %d, %s, %s, %s, %s, %s, %s},\n", lit(td.name.Name), qs(td.in), b2i(valid),
			qs(string(c)), lit(errText(cerr)), qs(string(i)), lit(errText(ierr)), qs(string(k)),
			lit(errText(kerr)))
	}
	pf("};\n\n")
}

// What each accessor gave back, or the text it panicked with.
func accessor[T any](f func() (T, error)) (v T, err error, msg string) {
	defer func() {
		if r := recover(); r != nil {
			msg = fmt.Sprint(r)
		}
	}()
	v, err = f()
	return v, err, ""
}

func genTokens() {
	pf("static const JtTokCase jt_token_cases[] = {\n")
	for _, tok := range tokenAccessorInputs() {
		b, _, bp := accessor(func() (bool, error) { return tok.Bool(), nil })
		f32, f32e, f32p := accessor(tok.Float32)
		f64, f64e, f64p := accessor(tok.Float)
		i64, i64e, i64p := accessor(tok.Int)
		u64, u64e, u64p := accessor(tok.Uint)
		pf("    {%s, %d, %s, %s, %#xU, %s, %s, %#xULL, %s, %s, %#xULL, %s, %s, %#xULL, %s, %s, %s},\n",
			tokLit(tok), b2i(b), lit(bp), qs(tok.String()), math.Float32bits(f32), lit(errText(f32e)),
			lit(f32p), math.Float64bits(f64), lit(errText(f64e)), lit(f64p), uint64(i64), lit(errText(i64e)),
			lit(i64p), u64, lit(errText(u64e)), lit(u64p), kindLit(tok.Kind()))
	}
	pf("};\n\n")
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

func TestZZGen(t *testing.T) {
	_ = errors.New
	_ = jsonflags.AllowDuplicateNames
	pf(`/* What Go's encoding/json/jsontext does with its own test tables, go1.27.1.
 * Generated by tools/gen-jsontext-tests.sh; do not edit. */

typedef struct QStr {
    const char *p;
    long long n;
} QStr;

#define QS(x) {x, (long long)sizeof(x) - 1}

/* A joined jsonopts.Struct: the flag bits are Go's, which are burrow's. */
typedef struct JtOpts {
    unsigned long long presence, values;
    QStr indent, prefix;
    long long byte_limit, depth_limit;
} JtOpts;

typedef struct JtTok {
    char how;
    QStr text;
    unsigned long long num;
} JtTok;

typedef struct JtRead {
    char kind;
    QStr text;
} JtRead;

typedef struct JtCoderCase {
    const char *name;
    QStr in, compacted, indented, canonicalized;
    int tok_start, ntok, ptr_start, nptr, dec_start;
} JtCoderCase;

typedef struct JtEncCall {
    int is_value;
    JtTok tok;
    QStr value;
    const char *err;
    QStr pointer;
    long long depth;
} JtEncCall;

typedef struct JtEncCase {
    const char *name;
    JtOpts opts;
    int call_start, ncall;
    QStr out;
    long long offset;
} JtEncCase;

typedef struct JtDecCall {
    int is_value;
    char kind;
    QStr out;
    const char *err;
    QStr pointer;
    long long depth;
} JtDecCall;

typedef struct JtDecCase {
    const char *name;
    JtOpts opts;
    QStr in;
    int call_start, ncall;
    long long offset;
    QStr unread;
} JtDecCase;

typedef struct JtValueCase {
    const char *name;
    QStr in;
    int valid;
    QStr compacted;
    const char *compact_err;
    QStr indented;
    const char *indent_err;
    QStr canonicalized;
    const char *canonicalize_err;
} JtValueCase;

/* One token and what each accessor did with it: the value and error text, or
 * the panic text when there is one. */
typedef struct JtTokCase {
    JtTok tok;
    int bool_value;
    const char *bool_panic;
    QStr string;
    unsigned f32;
    const char *f32_err, *f32_panic;
    unsigned long long f64;
    const char *f64_err, *f64_panic;
    unsigned long long i64; /* the int64's bits */
    const char *i64_err, *i64_panic;
    unsigned long long u64;
    const char *u64_err, *u64_panic;
    char kind;
} JtTokCase;

`)
	genCoder()
	genEncoderErrors()
	genDecoderErrors()
	genValues()
	genTokens()
	os.Stdout.WriteString(w.String())
}
GO

# The tokens TestTokenAccessors checks, lifted out of its table so the list
# stays Go's.
{
    printf '//go:build goexperiment.jsonv2\n\npackage jsontext\n\nimport "math"\n\n'
    printf 'func tokenAccessorInputs() []Token {\n\tnegZero := math.Copysign(0, -1)\n\treturn []Token{\n'
    sed -n '/^func TestTokenAccessors/,/^}/p' "$pkg/token_test.go" |
        sed -n 's/^\t\t{\(.*\), token{.*/\t\t\1,/p'
    printf '\t}\n}\n'
} > "$tmp/zz_gen_tokens_test.go"

printf '{"Replace":{"%s/zz_gen_test.go":"%s/zz_gen_test.go","%s/zz_gen_tokens_test.go":"%s/zz_gen_tokens_test.go"}}' \
    "$pkg" "$tmp" "$pkg" "$tmp" > "$tmp/overlay.json"
GOEXPERIMENT=jsonv2 go test -overlay "$tmp/overlay.json" -run '^TestZZGen$' -count=1 -v \
    encoding/json/jsontext > "$tmp/out.txt"
sed -e '/^=== RUN/d' -e '/^--- PASS/,$d' "$tmp/out.txt" > "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
echo "wrote $out"
