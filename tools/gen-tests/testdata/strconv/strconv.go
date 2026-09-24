// Package strconv is a stand-in for the real one, with only what
// fixture_test.go needs to type check. burrow-gen tests reads the two together
// the way it reads a package in Go's tree, and the C it writes is
// gen_tests_fixture_test.c.
package strconv

import "errors"

var ErrSyntax = errors.New("invalid syntax")

var ErrRange = errors.New("value out of range")

func Quote(s string) string { return s }

func Unquote(s string) (string, error) { return s, nil }

type NumError struct {
	Func string
	Num  string
	Err  error
}
