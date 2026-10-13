/* What x509.c has inside, for tests/x509_test.c.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_X509_INTERNAL_H
#define BURROW_SRC_CRYPTO_X509_INTERNAL_H

#include "burrow/crypto/x509.h"

#include "burrow/error.h"

#include <stdbool.h>
#include <stdint.h>

/* errInvalidOID, "invalid oid", which the OID functions give for anything
 * that is not an OID. */
extern const Error burrow__x509_err_invalid_oid;

/* Reads the GODEBUG settings from value as if it were the environment's, or
 * forgets them when value is NULL so that the next use reads the environment
 * again. For tests, which cannot change the environment of a process that has
 * already looked at it. */
void burrow__x509_godebug_set(const char *value);

/* parseASN1String, which turns the ASN.1 string of tag in value into *out. */
Error burrow__x509_parse_asn1_string(Alloc *a, uint8_t tag, Slice value, Str *out);

/* domainNameValid, whether s is a domain name, or a constraint on one when
 * constraint is set. */
bool burrow__x509_domain_name_valid(Str s, bool constraint);

/* parseRFC2821Mailbox, with the unquoted local part made in a. */
bool burrow__x509_parse_rfc2821_mailbox(Alloc *a, Str in, Str *local, Str *domain);

#endif /* BURROW_SRC_CRYPTO_X509_INTERNAL_H */
