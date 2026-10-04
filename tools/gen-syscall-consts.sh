#!/bin/sh
# Regenerates include/burrow/syscall/zconst.h and
# tests/syscall_zconst_check.inc from the syscall package of the Go on PATH,
# which should be the release burrow tracks. tools/gen-syscall/consts.go does
# the work, and type checks syscall once for each system and architecture, so
# it takes a minute or two.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root/tools/gen-syscall"
GO111MODULE=off go run consts.go "$(go env GOROOT)" "$(go env GOVERSION)" "$root" "$root/include/burrow/syscall/zconst.h"
