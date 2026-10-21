#!/bin/sh
# Every function defined in a file of the FIPS 140-3 module is marked with
# CRYPTO_FIPS_TEXT, so that it goes in the section the integrity check hashes.
#
#   tools/check-fips-module.sh
#
# The files are the ones listed in MODULE below, the ports of Go's packages
# under crypto/internal/fips140, and the headers they include that hold module
# code. A function that is left unmarked still works, and the check passes
# without it, so nothing else would notice that it is not covered. See
# src/crypto/fips140_module.h.
#
# What it will miss: a function that a macro defines, like the ones in
# mlkem.c's MLKEM_FUNCS, whose lines are part of a #define. Those are marked by
# hand inside the macro.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

cd "$(dirname "$0")/.."

MODULE="aes bigmod cipher cmac ecdh ecdsa ed25519 edwards25519
edwards25519_field edwards25519_scalar edwards25519_table fips140_cast
fips140_check gcm hkdf hmac mldsa mlkem nistec nistec_p224 nistec_p256
nistec_p384 nistec_p521 pbkdf2 rsa sha256 sha3 sha512 subtle tls_key_schedule
tls_prf"

set --
for m in $MODULE; do
	set -- "$@" "src/crypto/$m.c"
done
set -- "$@" src/crypto/nistec_p256_point.h src/crypto/fiat_*.h

# A definition starts at the left margin and ends in ") {", maybe some lines
# further on. Directives and the lines that continue them are skipped, and so
# are comments, which can start at the margin too, and a line that only uses a
# macro.
bad=$(awk '
FNR == 1 { sig = ""; cont = 0; incomment = 0 }
{
	line = $0
	wascont = cont
	cont = (line ~ /\\[ \t]*$/)
	if (wascont || line ~ /^[ \t]*#/) { sig = ""; next }
	if (incomment) {
		if (index(line, "*/") > 0) incomment = 0
		next
	}
	if (line ~ /^[ \t]*\/\*/) {
		if (index(line, "*/") == 0) incomment = 1
		next
	}
	if (sig == "") {
		if (line !~ /^[A-Za-z_]/) next
		if (line ~ /^(typedef|struct|union|enum|extern)[^A-Za-z0-9_]/) next
		# A macro that defines functions, used on a line of its own.
		if (line ~ /^[A-Z][A-Z0-9_]*\([^()]*\)[ \t]*$/) next
		sig = line
		start = FNR
	} else {
		sig = sig " " line
	}
	if (sig ~ /\)[ \t]*\{[ \t]*$/) {
		if (sig ~ /\(/ && sig !~ /=/ && sig !~ /^CRYPTO_FIPS_TEXT /)
			printf "%s:%d: %s\n", FILENAME, start, sig
		sig = ""
	} else if (sig ~ /[;{}][ \t]*$/) {
		sig = ""
	}
}
' "$@")

if [ -n "$bad" ]; then
	printf '%s\n' "$bad" | sed 's/  */ /g'
	echo
	echo "A function of the FIPS 140-3 module is not marked CRYPTO_FIPS_TEXT."
	echo "See src/crypto/fips140_module.h."
	exit 1
fi
printf 'ok\tfips-module\t%d files\n' $#
