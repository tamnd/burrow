/* golang.org/x/net/idna, the copy Go vendors.
 *
 * IDNA2008 with the compatibility processing of UTS #46: the conversion of
 * domain names between the Unicode form people write and the ASCII form, with
 * "xn--" Punycode labels, that DNS carries. It is an internal of burrow's for
 * the same reason it is one of Go's, which is that net/http and the cookie
 * jar need it and nothing exports it.
 *
 * Go's Profile holds functions for its mapping, its Bidi Rule check and its
 * check of decoded labels, and an Option is a closure that sets them. Here a
 * profile says which of Go's functions it would hold, and an option is a kind
 * and a flag that burrow__idna_new applies in order, as Go applies its
 * closures. The four profiles Go exports are constants.
 *
 * Like Go's, the conversions return what they made of the name along with the
 * first error they found, because a browser may look up a name that was only
 * partly converted. The result borrows from s when it is s or a part of it,
 * and is in memory from a otherwise. The error is in memory from a too. Only
 * the error says that a has run out of memory: then the result is empty.
 *
 * Go's validateLabel loops forever on a label that has a zero width joiner in
 * it and ends in the middle of a UTF-8 sequence, when the profile checks
 * joiners without mapping or validating labels, so that nothing has looked at
 * the label before the joiner check. None of the profiles Go exports is like
 * that. New makes one from CheckJoiners without MapForLookup, ValidateLabels or
 * ValidateForRegistration. This loops in the same place.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package xnet/idna */

#ifndef BURROW_SRC_XNET_IDNA_H
#define BURROW_SRC_XNET_IDNA_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"

#include <stdbool.h>
#include <stdint.h>

/* Go's options.mapping: none, normalize, validateAndMap or
 * validateRegistration. */
typedef enum IdnaMapping {
    IDNA_MAP_NONE,
    IDNA_MAP_NORMALIZE,
    IDNA_MAP_VALIDATE_AND_MAP,
    IDNA_MAP_VALIDATE_REGISTRATION,
} IdnaMapping;

/* Go's Profile. trie is whether Go's would have its trie set, from_puny
 * whether its fromPuny is validateFromPunycode, which is also what makes
 * ValidateLabels on, and bidirule whether its bidirule is
 * bidirule.ValidString. */
typedef struct IdnaProfile {
    bool transitional;
    bool use_std3_rules;
    bool check_hyphens;
    bool check_joiners;
    bool verify_dns_length;
    bool remove_leading_dots;
    bool trie;
    bool from_puny;
    IdnaMapping mapping;
    bool bidirule;
} IdnaProfile;

typedef enum IdnaOptionKind {
    IDNA_OPT_TRANSITIONAL,
    IDNA_OPT_VERIFY_DNS_LENGTH,
    IDNA_OPT_REMOVE_LEADING_DOTS,
    IDNA_OPT_VALIDATE_LABELS,
    IDNA_OPT_CHECK_HYPHENS,
    IDNA_OPT_CHECK_JOINERS,
    IDNA_OPT_STRICT_DOMAIN_NAME,
    IDNA_OPT_BIDI_RULE,
    IDNA_OPT_VALIDATE_FOR_REGISTRATION,
    IDNA_OPT_MAP_FOR_LOOKUP,
} IdnaOptionKind;

/* Go's Option. enable is the argument of the options that take one. */
typedef struct IdnaOption {
    IdnaOptionKind kind;
    bool enable;
} IdnaOption;

IdnaOption burrow__idna_transitional(bool transitional);
IdnaOption burrow__idna_verify_dns_length(bool verify);
IdnaOption burrow__idna_remove_leading_dots(bool remove);
IdnaOption burrow__idna_validate_labels(bool enable);
IdnaOption burrow__idna_check_hyphens(bool enable);
IdnaOption burrow__idna_check_joiners(bool enable);
IdnaOption burrow__idna_strict_domain_name(bool use);
IdnaOption burrow__idna_bidi_rule(void);
IdnaOption burrow__idna_validate_for_registration(void);
IdnaOption burrow__idna_map_for_lookup(void);

/* New: a profile with the n options in opts applied in order. With none it
 * is the Punycode profile. */
IdnaProfile burrow__idna_new(const IdnaOption *opts, Int n);

/* Punycode, Lookup, Display and Registration. */
extern const IdnaProfile burrow__idna_punycode;
extern const IdnaProfile burrow__idna_lookup;
extern const IdnaProfile burrow__idna_display;
extern const IdnaProfile burrow__idna_registration;

/* Profile.ToASCII and Profile.ToUnicode. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) BURROW_OWNS(err) Str
burrow__idna_profile_to_ascii(Alloc *a, const IdnaProfile *p, Str s, Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) BURROW_OWNS(err) Str
burrow__idna_profile_to_unicode(Alloc *a, const IdnaProfile *p, Str s, Error *err);

/* ToASCII and ToUnicode, which use the Punycode profile. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) BURROW_OWNS(err) Str
burrow__idna_to_ascii(Alloc *a, Str s, Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) BURROW_OWNS(err) Str
burrow__idna_to_unicode(Alloc *a, Str s, Error *err);

/* Profile.String. Empty when a runs out of memory. */
BURROW_OWNS(ret) Str burrow__idna_profile_string(Alloc *a, const IdnaProfile *p);

/* The code Go's errors carry, from UTS #46's conformance tests: "P1", "V7",
 * "A4" and the rest. Empty for an error that is not one of these. */
BURROW_BORROWS(ret, err) Str burrow__idna_error_code(Error err);

/* The trie value of the first rune of s, which is not empty, and the bytes it
 * takes, as Go's trie.lookupString gives them. For the tests. */
uint16_t burrow__idna_lookup_string(Str s, Int *size);

/* The Punycode of RFC 3492, for the tests: decode, and encode with prefix in
 * front. The results are in memory from a, or borrow from s. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) BURROW_OWNS(err) Str
burrow__idna_punycode_decode(Alloc *a, Str s, Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) BURROW_OWNS(err) Str
burrow__idna_punycode_encode(Alloc *a, Str prefix, Str s, Error *err);

#endif /* BURROW_SRC_XNET_IDNA_H */
