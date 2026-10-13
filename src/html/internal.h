/* What the files of the html package share, and what its tests look at.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_HTML_INTERNAL_H
#define BURROW_SRC_HTML_INTERNAL_H

#include "burrow/html.h"

#include "burrow/core.h"

/* One named character reference: its name without the leading '&', with the
 * ';' when the name has one, and its code points. The second code point is 0
 * for a reference that stands for one. */
typedef struct burrow__HtmlEntity {
    Str name;
    Rune r[2];
} burrow__HtmlEntity;

/* Go's entity map, sorted by name. */
extern const burrow__HtmlEntity burrow__html_entity[];
extern const Int burrow__html_entity_len;

/* Go's entity2 map, the references that stand for two code points, sorted by
 * name. */
extern const burrow__HtmlEntity burrow__html_entity2[];
extern const Int burrow__html_entity2_len;

/* longestEntityWithoutSemicolon: every entity whose name does not end in ';'
 * is this many bytes long or less. */
enum { BURROW__HTML_LONGEST_ENTITY_WITHOUT_SEMICOLON = 6 };

#endif /* BURROW_SRC_HTML_INTERNAL_H */
