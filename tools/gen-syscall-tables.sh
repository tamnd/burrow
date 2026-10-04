#!/bin/sh
# Regenerates syscall's tables from the syscall package of the Go on PATH,
# which should be the release burrow tracks: include/burrow/syscall/zconst.h
# and tests/syscall_zconst_check.inc, the constants, and
# include/burrow/syscall/ztypes.h and tests/syscall_ztypes_check.inc, the
# types. tools/gen-syscall/main.go does the work, and type checks syscall once
# for each system and architecture, so it takes a minute or two. With -v it
# also says which types it left out and why.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root/tools/gen-syscall"
GO111MODULE=off go run main.go common.go consts.go types.go "$@" "$(go env GOROOT)" "$(go env GOVERSION)" "$root"
