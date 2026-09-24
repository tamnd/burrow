// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

// Every shape burrow-gen tests knows, in one file. The tests here test
// nothing, they only have to come out as C that compiles and runs.

package strconv

import (
	"math"
	"strings"
	"testing"
)

type quoteTest struct {
	in    string
	out   string
	r     rune
	err   error
	quote func(string) string
}

var quotetests = []quoteTest{
	{"", `""`, 'a', nil, Quote},
	{"\a\b\f\r\n\t\v\\\"", `"\a\b\f\r\n\t\v\\\""`, '\n', ErrSyntax, nil},
	{"\xff" + "fe", `"\xfffe"`, '\\', ErrRange, Quote},
	{"☺ \U0010ffff", "??=", 0x263a, nil, nil},
}

type numberTest struct {
	i   int
	i64 int64
	u64 uint64
	f   float64
	f32 float32
	ok  bool
	b   []byte
}

var numbertests = []numberTest{
	{0, math.MinInt64, math.MaxUint64, 1.5, 0.1, true, []byte("abc")},
	{-1 << 31, 1 << 40, 7, math.Inf(1), float32(math.NaN()), false, nil},
	{1e3, -1, 1 << 63, math.Copysign(0, -1), math.MaxFloat32, true, []byte{0, 1, 0xff}},
}

type nested struct {
	name  string
	words []string
	num   NumError
	ptr   *NumError
	pair  [2]float64
}

var long = strings.Repeat("ab", 3) + tail

var tail = "!"

var nestedtests = []nested{
	{name: long, words: []string{"a", "b"}},
	{
		name:  "keyed",
		num:   NumError{"ParseInt", "x", ErrSyntax},
		ptr:   &NumError{Func: "Atoi", Err: ErrRange},
		pair:  [2]float64{1, -2},
		words: nil,
	},
	{name: "by hand", words: strings.Fields(" a b ")},
}

var pairs = [][2]float64{{1, 2}, {math.Inf(-1), 0}}

var plain = []string{"one", "two"}

func TestQuote(t *testing.T) {
	for _, tt := range quotetests {
		if got := Quote(tt.in); got != tt.out {
			t.Errorf("Quote(%q) = %q, want %q", tt.in, got, tt.out)
		}
	}
}

func TestNumbers(t *testing.T) {
	var n int
	for i := 0; i < len(numbertests); i++ {
		n += numbertests[i].i
	}
	if n == 0 {
		t.Log("zero")
	}
}

func TestLocalTable(t *testing.T) {
	tests := []struct {
		in   string
		want string
		err  error
	}{
		{"\"a\"", "a", nil},
		{"'", "", ErrSyntax},
	}
	for i, tc := range tests {
		got, err := Unquote(tc.in)
		if got != tc.want || err != tc.err {
			t.Errorf("%d: Unquote(%q) = %q, %v", i, tc.in, got, err)
		}
	}
}

func TestPairs(t *testing.T) {
	for _, p := range pairs {
		_ = p
	}
	_ = plain
	_ = nestedtests
}

func TestNotATable(t *testing.T) {
	if Quote("x") != "x" {
		t.Fatal("/* not */ a table")
	}
}

func BenchmarkQuote(b *testing.B) {
	for b.Loop() {
		Quote("x")
	}
}
