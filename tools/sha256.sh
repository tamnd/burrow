#!/bin/sh
# Print a sha256 line per file, in sha256sum's format, on every runner we build
# on.
#
# There is no single command for this and the differences are not obvious.
# Linux has sha256sum. macOS has shasum but no sha256sum. Git for Windows ships
# sha256sum but no shasum, which is what broke the first release: the Windows
# archive built fine and then the job died on the checksum line, so the whole
# publish step was skipped and nothing shipped.
#
# The output format is sha256sum's on purpose, because that is what
# `sha256sum -c` reads and what people expect in a file named .sha256.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

if command -v sha256sum >/dev/null 2>&1; then
	exec sha256sum "$@"
fi

if command -v shasum >/dev/null 2>&1; then
	exec shasum -a 256 "$@"
fi

if command -v openssl >/dev/null 2>&1; then
	for f in "$@"; do
		printf '%s  %s\n' "$(openssl dgst -sha256 -r "$f" | cut -d' ' -f1)" "$f"
	done
	exit 0
fi

echo "no sha256 tool on this machine: tried sha256sum, shasum and openssl" >&2
exit 1
