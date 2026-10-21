#!/bin/sh
# Counts the conditional branches in each function of an assembly listing of
# src/crypto/subtle.c and checks the counts against tools/ct-asm.txt.
#
#   tools/check-ct-asm.sh CC FILE.s
#
# make ct-asm writes the listing with the flags the library is built with and
# runs this on it. CC is the compiler that wrote it, which picks the counts.
#
# ctgrind finds a branch on a secret by running the code, so it only sees the
# paths its inputs take. This reads what the compiler wrote instead. A C
# compiler is free to turn a mask back into a branch when it can prove that the
# two do the same thing, and ct_hide is there to stop it, so a compiler upgrade
# that sees past ct_hide would ship a branch on a secret without a test noticing.
#
# The functions that take one value and give one, byte_eq, eq, select and
# less_or_eq, must have no conditional branch at all, with any compiler. The
# ones that walk a slice, compare, copy and xor_bytes, branch on the lengths
# and on whether the slices overlap, which are public. The two that set and
# restore the DIT flag branch on whether the CPU has it and whether it was on
# already. Those counts are written down per architecture and compiler major
# version, and a count that changes fails, so that somebody reads the new
# assembly before it ships. A compiler with no counts in the file fails too, and
# this prints the lines to add once its assembly has been read.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

if [ $# -ne 2 ]; then
	echo "usage: $0 CC FILE.s" >&2
	exit 2
fi
cc=$1
asm=$2
root=$(cd "$(dirname "$0")/.." && pwd)
want="$root/tools/ct-asm.txt"

# CC can be more than one word, like "ccache gcc", so it is not quoted.
# shellcheck disable=SC2086
arch=$($cc -dumpmachine | cut -d- -f1)
# shellcheck disable=SC2086
macros=$(echo | $cc -dM -E -x c -)
major=$(echo "$macros" | sed -n 's/^#define __clang_major__ \([0-9]*\)$/\1/p')
if [ -n "$major" ]; then
	compiler=clang-$major
else
	major=$(echo "$macros" | sed -n 's/^#define __GNUC__ \([0-9]*\)$/\1/p')
	if [ -z "$major" ]; then
		echo "check-ct-asm: $cc is neither gcc nor clang" >&2
		exit 1
	fi
	compiler=gcc-$major
fi

case $arch in
x86_64 | amd64)
	arch=x86_64
	# Every jump but jmp is conditional.
	branch='^[ \t]+j[a-z]+[ \t]'
	notbranch='^[ \t]+jmpq?[ \t]'
	;;
aarch64 | arm64)
	arch=aarch64
	# b.ne is clang's spelling and bne is gcc's. bl, blr and br are calls and
	# plain jumps, and bic is not a branch at all.
	branch='^[ \t]+(b[.]?(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)|cbn?z|tbn?z)[ \t]'
	notbranch='^$'
	;;
*)
	echo "check-ct-asm: no branch instructions known for $arch" >&2
	exit 1
	;;
esac

# One line per function, its name and its count. A label is the start of a
# function when it is a name at the start of a line, so .L labels are not, and
# the function runs to the .cfi_endproc or .size after it. gcc moves the cold
# end of a function under a label of its own with .cold after the name, and
# that part counts towards the function.
counts=$(awk -v branch="$branch" -v notbranch="$notbranch" '
/^[A-Za-z_][A-Za-z0-9_.]*:/ {
    fn = substr($0, 1, index($0, ":") - 1)
    sub(/\..*/, "", fn)
    if (!(fn in n)) {
        n[fn] = 0
        order[++nfn] = fn
    }
    next
}
/^[ \t]+\.cfi_endproc/ || /^[ \t]+\.size/ { fn = ""; next }
fn != "" && $0 ~ branch && $0 !~ notbranch { n[fn]++ }
END { for (i = 1; i <= nfn; i++) print order[i], n[order[i]] }
' "$asm")

for fn in subtle_constant_time_byte_eq subtle_constant_time_eq \
	subtle_constant_time_select subtle_constant_time_less_or_eq \
	subtle_constant_time_compare subtle_constant_time_copy subtle_xor_bytes; do
	if ! echo "$counts" | grep -q "^$fn "; then
		echo "check-ct-asm: $fn is not in $asm" >&2
		exit 1
	fi
done

if ! grep -q "^$arch $compiler " "$want"; then
	echo "check-ct-asm: tools/ct-asm.txt has no counts for $arch $compiler." >&2
	echo "Read the assembly in $asm, and if no branch depends on a secret, add:" >&2
	echo "$counts" | awk -v k="$arch $compiler" '$2 > 0 { print k, $1, $2 }' >&2
	exit 1
fi

# A function the file does not name must have no branches.
status=0
echo "$counts" | awk -v k="$arch $compiler" -v want="$want" -v asm="$asm" '
BEGIN {
    while ((getline line < want) > 0) {
        if (line ~ /^#/ || line == "")
            continue
        split(line, f, " ")
        if (f[1] " " f[2] == k)
            pinned[f[3]] = f[4]
    }
}
{
    got[$1] = 1
    w = ($1 in pinned) ? pinned[$1] : 0
    if ($2 != w) {
        printf "check-ct-asm: %s has %d conditional branches in %s and tools/ct-asm.txt says %d for %s\n", $1, $2, asm, w, k > "/dev/stderr"
        bad = 1
    }
}
END {
    for (fn in pinned)
        if (!(fn in got)) {
            printf "check-ct-asm: tools/ct-asm.txt has %s for %s, which is not in %s\n", fn, k, asm > "/dev/stderr"
            bad = 1
        }
    exit bad ? 1 : 0
}
' || status=1

if [ "$status" -ne 0 ]; then
	echo "" >&2
	echo "Read the new assembly. A branch on a secret is a bug in subtle.c; a" >&2
	echo "branch on a length is fine, and then the count in tools/ct-asm.txt changes." >&2
	exit 1
fi

echo "ok	ct-asm	$arch $compiler, $(echo "$counts" | wc -l | tr -d ' ') functions"
