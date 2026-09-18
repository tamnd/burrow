#!/bin/sh
# Functions that are not allowed in burrow's own source.
#
# Most of these are banned everywhere in C and for the usual reasons. A few are
# banned for reasons specific to this project, and those are the ones worth
# explaining, so each group says why underneath.
#
# The check is grep rather than a clang plugin on purpose. It runs in under a
# second on every platform including the Windows runner, it has no build step of
# its own, and a developer can run it without installing anything. It will miss
# a call made through a function pointer, which is a trade we accept because the
# sanitisers catch what this misses and this catches what a reviewer misses.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

status=0

report() {
	printf '%s\n' "$2" >&2
	status=1
}

# One pattern per word. This is a loop rather than a case with alternatives
# because a | inside an expanded variable is not an alternation separator, it is
# a literal character, so a list written as 'a.c|b.c' matches neither file and
# does it quietly.
allowed() {
	file=$1
	shift
	for pattern in $*; do
		case "$file" in
		$pattern) return 0 ;;
		esac
	done
	return 1
}

# The string family. Every one of them either cannot express a bound or gets
# the bound wrong in a way that has shipped a CVE. burrow has Str, which carries
# its length, so there is never a reason to reach for these.
BANNED_STRING='strcpy|strcat|sprintf|vsprintf|gets|strtok|strncpy|strncat|alloca|atoi|atol|atof'

# Allocation. It is legal in exactly one file, which is the heap backend, and
# nowhere else, because the whole memory design is that allocation is something
# a caller hands you rather than something you reach for.
BANNED_ALLOC='\bmalloc\b|\bcalloc\b|\brealloc\b|\bfree\b|\baligned_alloc\b|\bposix_memalign\b|\b_aligned_malloc\b|\b_aligned_realloc\b|\b_aligned_free\b'
ALLOC_ALLOWED='src/mem/heap.c'

# Exiting. A library does not get to end the host's process. Failures come back
# as an Error, and the one case where that is impossible is allocation failure,
# which goes through the allocator's own out of memory handler so the host can
# decide.
BANNED_EXIT='\bexit\b|\babort\b|\b_Exit\b|\bassert\b'
EXIT_ALLOWED='src/runtime/panic.c tests/*'

# --others picks up files that are written but not staged yet, which is the
# state a file is in exactly when somebody runs this before committing. Without
# it the check quietly passes on a tree it never looked at.
sources=$(git ls-files --cached --others --exclude-standard \
	'src/*.c' 'src/**/*.c' 'include/**/*.h' 'tests/*.c' 'tests/*.h' 2>/dev/null || true)
[ -n "$sources" ] || exit 0

for f in $sources; do
	if hits=$(grep -nE "(^|[^a-zA-Z0-9_])($BANNED_STRING)[[:space:]]*\(" "$f"); then
		report "$f" "$f: banned libc string function
$hits
  burrow has Str, which carries its length. See docs/design/04-core-types.md."
	fi

	if ! allowed "$f" "$ALLOC_ALLOWED"; then
		if hits=$(grep -nE "(^|[^a-zA-Z0-9_.>])($BANNED_ALLOC)[[:space:]]*\(" "$f"); then
			report "$f" "$f: allocation outside the malloc backend
$hits
  Every function that allocates takes an Alloc *a. See docs/design/05-memory.md."
		fi
	fi

	if ! allowed "$f" "$EXIT_ALLOWED"; then
		if hits=$(grep -nE "(^|[^a-zA-Z0-9_])($BANNED_EXIT)[[:space:]]*\(" "$f"); then
			report "$f" "$f: a library does not end the host's process
$hits
  Return an Error. See docs/design/05-memory.md section 7."
		fi
	fi
done

if [ "$status" -eq 0 ]; then
	echo "ok	banned	$(printf '%s\n' "$sources" | wc -l | tr -d ' ') files"
fi

exit "$status"
