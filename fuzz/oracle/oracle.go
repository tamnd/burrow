// The other half of the differential fuzzer: Go's own standard library, built
// as a C archive so that a fuzzer written in C can ask it the same question it
// asks burrow.
//
// Every target is one exported function that takes the fuzzer's bytes and
// hands back a report, and the matching C function in fuzz/ writes the same
// report from burrow. The two reports are compared byte for byte, so a report
// is text in a fixed format that both sides can produce without a formatting
// library in common: one call per line, integers in decimal, bytes in hex.
//
// The report is malloc'd, and the C side frees it.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package main

/*
#include <stdlib.h>
*/
import "C"

import "unsafe"

// report copies a Go report into C memory for the caller to free.
func report(out []byte, outlen *C.size_t) *C.char {
	*outlen = C.size_t(len(out))
	if len(out) == 0 {
		// C.CBytes of nothing is still a valid pointer, but saying so here
		// saves the reader a trip to the cgo documentation.
		return (*C.char)(C.malloc(1))
	}
	return (*C.char)(C.CBytes(out))
}

func input(data *C.uchar, n C.size_t) []byte {
	return C.GoBytes(unsafe.Pointer(data), C.int(n))
}

//export oracle_utf8
func oracle_utf8(data *C.uchar, n C.size_t, outlen *C.size_t) *C.char {
	return report(utf8Report(input(data, n)), outlen)
}

func main() {}
