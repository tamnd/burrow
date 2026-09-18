#!/bin/sh
# Every file that came from Go has to say so, and say which file it came from.
#
# This is a licence obligation, but that is not the reason it is worth
# enforcing. The reason is that a port of this size is only reviewable by diff:
# a reviewer who knows that src/strings/strings.c came from go1.27.1's
# src/strings/strings.go can put the two side by side and see what changed. A
# file that does not name its origin is a file nobody can check.
#
# Files that are burrow's own carry a plain copyright header and no upstream
# line, and that is fine. What is not fine is a file under a package directory
# with no header at all, because that is almost always somebody forgetting.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

GOROOT_SRC="${GOROOT_SRC:-$(go env GOROOT 2>/dev/null || echo '')/src}"

status=0
checked=0
derived=0

# --others picks up files that are written but not staged yet, which is the
# state a file is in exactly when somebody runs this before committing.
sources=$(git ls-files --cached --others --exclude-standard \
	'src/*.c' 'src/**/*.c' 'include/**/*.h' 'tests/**/*.c' 2>/dev/null || true)
[ -n "$sources" ] || exit 0

for f in $sources; do
	checked=$((checked + 1))
	head=$(head -40 "$f")

	case "$head" in
	*"Use of this source code is governed by a BSD-style licence"*) ;;
	*)
		printf '%s: no licence header in the first 40 lines\n' "$f" >&2
		status=1
		continue
		;;
	esac

	# "Derived from Go's src/strings/strings.go." is the marker. If it is there,
	# the path after it has to be a file that actually exists in the Go tree we
	# are porting against, and the Go release has to be named.
	upstream=$(printf '%s\n' "$head" | sed -n "s|.*Derived from Go's \([^ ]*\)\.\$|\1|p" | head -1)
	[ -n "$upstream" ] || continue

	derived=$((derived + 1))

	case "$head" in
	*"Go source: go1."*) ;;
	*)
		printf '%s: derived from %s but does not name the Go release it was read at\n' "$f" "$upstream" >&2
		status=1
		;;
	esac

	case "$head" in
	*"Copyright 2009 The Go Authors"*) ;;
	*)
		printf '%s: derived from %s but does not carry the Go Authors copyright\n' "$f" "$upstream" >&2
		status=1
		;;
	esac

	# Only checkable on a machine that has the Go tree. CI has one, a
	# contributor might not, and the check degrades to the shape tests above
	# rather than failing for a reason that is not their fault.
	if [ -d "$GOROOT_SRC" ]; then
		rel=${upstream#src/}
		if [ ! -f "$GOROOT_SRC/$rel" ]; then
			printf '%s: names %s as its upstream, which is not a file in %s\n' "$f" "$upstream" "$GOROOT_SRC" >&2
			status=1
		fi
	fi
done

if [ "$status" -eq 0 ]; then
	echo "ok	headers	$checked files, $derived derived from Go"
fi

exit "$status"
