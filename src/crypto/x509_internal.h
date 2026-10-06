/* What x509.c has inside, for tests/x509_test.c.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_X509_INTERNAL_H
#define BURROW_SRC_CRYPTO_X509_INTERNAL_H

#include "burrow/crypto/x509.h"

#include "burrow/error.h"

/* errInvalidOID, "invalid oid", which the OID functions give for anything
 * that is not an OID. */
extern const Error burrow__x509_err_invalid_oid;

/* Reads the GODEBUG settings from value as if it were the environment's, or
 * forgets them when value is NULL so that the next use reads the environment
 * again. For tests, which cannot change the environment of a process that has
 * already looked at it. */
void burrow__x509_godebug_set(const char *value);

#endif /* BURROW_SRC_CRYPTO_X509_INTERNAL_H */
