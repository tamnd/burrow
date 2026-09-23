// strconv asked everything it can be asked about one byte string.
//
// fuzz/strconv.c writes the same thing from burrow, line for line. A change
// here is a change there.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"strconv"
)

const floatFmts = "beEfgGxX"

func errText(err error) string {
	if err == nil {
		return " nil\n"
	}
	return " " + err.Error() + "\n"
}

func formats(out []byte, f float64, prec, bitSize int) []byte {
	for _, c := range []byte(floatFmts) {
		out = fmt.Appendf(out, "format %c %d %s %s\n", c, bitSize,
			strconv.FormatFloat(f, c, -1, bitSize), strconv.FormatFloat(f, c, prec, bitSize))
	}
	return out
}

func strconvReport(b []byte) []byte {
	var out []byte
	s := string(b)
	prec := 3
	if len(b) > 0 {
		prec = int(b[0] % 24)
	}

	for _, base := range []int{0, 2, 8, 10, 16, 36} {
		for _, size := range []int{8, 32, 64} {
			v, err := strconv.ParseInt(s, base, size)
			out = fmt.Appendf(out, "parse_int %d %d %d%s", base, size, v, errText(err))
			u, err := strconv.ParseUint(s, base, size)
			out = fmt.Appendf(out, "parse_uint %d %d %d%s", base, size, u, errText(err))
		}
	}
	ai, err := strconv.Atoi(s)
	out = fmt.Appendf(out, "atoi %d%s", ai, errText(err))
	bv, err := strconv.ParseBool(s)
	out = fmt.Appendf(out, "parse_bool %d%s", b2i(bv), errText(err))

	f, err := strconv.ParseFloat(s, 64)
	out = fmt.Appendf(out, "parse_float 64 %016x%s", math.Float64bits(f), errText(err))
	out = formats(out, f, prec, 64)
	f, err = strconv.ParseFloat(s, 32)
	out = fmt.Appendf(out, "parse_float 32 %016x%s", math.Float64bits(f), errText(err))
	out = formats(out, f, prec, 32)

	for _, size := range []int{64, 128} {
		c, err := strconv.ParseComplex(s, size)
		out = fmt.Appendf(out, "parse_complex %d %016x %016x%s", size,
			math.Float64bits(real(c)), math.Float64bits(imag(c)), errText(err))
		out = fmt.Appendf(out, "format_complex %s %s\n", strconv.FormatComplex(c, 'g', -1, size),
			strconv.FormatComplex(c, 'e', prec, size))
	}

	out = fmt.Appendf(out, "quote %s\n", strconv.Quote(s))
	out = fmt.Appendf(out, "quote_to_ascii %s\n", strconv.QuoteToASCII(s))
	out = fmt.Appendf(out, "quote_to_graphic %s\n", strconv.QuoteToGraphic(s))
	out = fmt.Appendf(out, "can_backquote %d\n", b2i(strconv.CanBackquote(s)))

	uq, err := strconv.Unquote(s)
	out = fmt.Appendf(out, "unquote %x%s", uq, errText(err))
	qp, err := strconv.QuotedPrefix(s)
	out = fmt.Appendf(out, "quoted_prefix %x%s", qp, errText(err))

	for _, q := range []byte{'"', '\'', 0} {
		for rest := s; len(rest) > 0; {
			r, mb, tail, err := strconv.UnquoteChar(rest, q)
			used := 0
			if err == nil {
				used = len(rest) - len(tail)
			}
			out = fmt.Appendf(out, "unquote_char %d %d %d %d%s", q, r, b2i(mb), used, errText(err))
			if err != nil {
				break
			}
			rest = tail
		}
	}

	for i := 0; i+4 <= len(b); i += 4 {
		u := binary.LittleEndian.Uint32(b[i:])
		r := rune(int32(u))
		out = fmt.Appendf(out, "rune %d %d %d %s %s %s\n", r, b2i(strconv.IsPrint(r)),
			b2i(strconv.IsGraphic(r)), strconv.QuoteRune(r), strconv.QuoteRuneToASCII(r),
			strconv.QuoteRuneToGraphic(r))
		out = formats(out, float64(math.Float32frombits(u)), prec, 32)
	}
	for i := 0; i+8 <= len(b); i += 8 {
		v := binary.LittleEndian.Uint64(b[i:])
		base := 2 + int(b[i]%35)
		out = fmt.Appendf(out, "format_int %d %s %s\n", base,
			strconv.FormatInt(int64(v), base), strconv.FormatUint(v, base))
		out = formats(out, math.Float64frombits(v), prec, 64)
	}
	return out
}
