#!/bin/sh
# Writes the FIPS 140-3 checksum into a test program with burrow-gen fips, and
# runs the program's TestIntegrityCheck to see that the check passes.
#
#   tools/check-fips.sh PROGRAM
#
# make fips-check runs this on a copy of tests/fips140_check_test.c. Without
# burrow-gen fips, the test writes the checksum into a copy of itself and runs
# that, so this is the run that tests burrow-gen fips: finding the sections
# in the file, hashing what is in the file the way the program hashes what is
# in memory, and writing the sum where the program reads it. On macOS it also
# signs the program again, which the program must still run after.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

if [ $# -ne 1 ]; then
	echo "usage: $0 PROGRAM" >&2
	exit 2
fi
prog=$1
case $prog in
*/*) ;;
*) prog=./$prog ;;
esac
root=$(cd "$(dirname "$0")/.." && pwd)

"$root/tools/burrow-gen" fips "$prog"
"$root/tools/burrow-gen" fips --check "$prog"

if ! out=$("$prog" -test.v '-test.run=^TestIntegrityCheck$' 2>&1); then
	printf '%s\n' "$out"
	echo "check-fips: $prog failed after burrow-gen fips" >&2
	exit 1
fi
printf '%s\n' "$out"
case $out in
*": verified"*) ;;
*)
	echo "check-fips: $prog did not say verified" >&2
	exit 1
	;;
esac
echo "ok	fips	$prog"
