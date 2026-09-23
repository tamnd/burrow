// unicode/utf8 asked everything it can be asked about one byte string.
//
// fuzz/utf8.c writes the same thing from burrow, line for line. A change here
// is a change there.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package main

import (
	"encoding/binary"
	"fmt"
	"unicode/utf8"
)

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

func utf8Report(b []byte) []byte {
	var out []byte
	s := string(b)

	out = fmt.Appendf(out, "valid %d\n", b2i(utf8.Valid(b)))
	out = fmt.Appendf(out, "valid_string %d\n", b2i(utf8.ValidString(s)))
	out = fmt.Appendf(out, "rune_count %d\n", utf8.RuneCount(b))
	out = fmt.Appendf(out, "rune_count_in_string %d\n", utf8.RuneCountInString(s))
	out = fmt.Appendf(out, "full_rune %d\n", b2i(utf8.FullRune(b)))
	out = fmt.Appendf(out, "full_rune_in_string %d\n", b2i(utf8.FullRuneInString(s)))
	if len(b) > 0 {
		out = fmt.Appendf(out, "rune_start %d\n", b2i(utf8.RuneStart(b[0])))
	}

	// Forwards and backwards over the whole input, the two ways a caller
	// walks a string, one rune at a time.
	for p := b; len(p) > 0; {
		r, size := utf8.DecodeRune(p)
		out = fmt.Appendf(out, "decode %d %d\n", r, size)
		p = p[size:]
	}
	for p := s; len(p) > 0; {
		r, size := utf8.DecodeRuneInString(p)
		out = fmt.Appendf(out, "decode_in_string %d %d\n", r, size)
		p = p[size:]
	}
	for p := b; len(p) > 0; {
		r, size := utf8.DecodeLastRune(p)
		out = fmt.Appendf(out, "decode_last %d %d\n", r, size)
		p = p[:len(p)-size]
	}
	for p := s; len(p) > 0; {
		r, size := utf8.DecodeLastRuneInString(p)
		out = fmt.Appendf(out, "decode_last_in_string %d %d\n", r, size)
		p = p[:len(p)-size]
	}

	// The input read four bytes at a time as runes, which reaches the
	// surrogates, the values past the maximum and the negative ones that a
	// walk over valid text never produces.
	for p := b; len(p) >= 4; p = p[4:] {
		r := rune(binary.LittleEndian.Uint32(p))
		var buf [utf8.UTFMax]byte
		n := utf8.EncodeRune(buf[:], r)
		out = fmt.Appendf(out, "rune %d len %d valid %d encode %x\n", r, utf8.RuneLen(r), b2i(utf8.ValidRune(r)), buf[:n])
		out = fmt.Appendf(out, "append %x\n", utf8.AppendRune(p[:min(len(p), 3):min(len(p), 3)], r))
	}
	return out
}
