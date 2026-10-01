#!/bin/sh
# Regenerates include/burrow/syscall/zerrors.h and src/syscall/zerrors.c from
# the syscall tables of the Go on PATH, which should be the release burrow
# tracks. tools/gen-syscall/errors.go does the work.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/include/burrow/syscall" "$root/src/syscall"
cd "$root/tools/gen-syscall"
GO111MODULE=off go run errors.go "$(go env GOROOT)" "$(go env GOVERSION)" "$root/include/burrow/syscall/zerrors.h" "$root/src/syscall/zerrors.c"
