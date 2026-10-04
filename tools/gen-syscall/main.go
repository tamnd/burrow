//go:build ignore

// Command gen-syscall writes syscall's generated tables:
// include/burrow/syscall/zconst.h, the constants, and
// include/burrow/syscall/ztypes.h, the types, for every system and
// architecture burrow builds for, and the tests/ files that check them.
// tools/gen-syscall-tables.sh runs it.
//
// Both come from type checking Go's syscall package from source once for
// each GOOS and GOARCH, so the values and the layouts are the ones the Go
// compiler would use, and nothing has to be parsed by hand.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

func main() {
	args := os.Args[1:]
	verbose := len(args) > 0 && args[0] == "-v"
	if verbose {
		args = args[1:]
	}
	if len(args) != 3 {
		die("usage: gen-syscall [-v] <goroot> <go version> <burrow root>")
	}
	dir := filepath.Join(args[0], "src", "syscall")
	ver := args[1]
	root := args[2]

	cw := newConstWriter(root, ver)
	tw := newTypeWriter(root, ver, verbose)
	for si, sys := range systems {
		var archs []string
		if sys.goos == "windows" {
			archs = windowsArchs
		} else {
			files, _ := filepath.Glob(filepath.Join(dir, "zerrors_"+sys.goos+"_*.go"))
			for _, f := range files {
				a := strings.TrimSuffix(strings.TrimPrefix(filepath.Base(f), "zerrors_"+sys.goos+"_"), ".go")
				if archMacro[a] != "" {
					archs = append(archs, a)
				}
			}
		}
		if len(archs) == 0 {
			die("no architectures for %s", sys.goos)
		}
		var plats []*platform
		for _, a := range archs {
			fmt.Fprintf(os.Stderr, "gen-syscall: %s/%s\n", sys.goos, a)
			plats = append(plats, load(sys.goos, a))
		}
		cw.system(si, plats)
		tw.system(si, plats)
	}
	cw.finish(root, ver, filepath.Join(root, "include/burrow/syscall/zconst.h"))
	tw.finish(root, filepath.Join(root, "include/burrow/syscall/ztypes.h"))
}
