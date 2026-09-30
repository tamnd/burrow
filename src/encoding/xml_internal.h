/* What xml.c lets its tests see.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_ENCODING_XML_INTERNAL_H
#define BURROW_SRC_ENCODING_XML_INTERNAL_H

#include "burrow/encoding/xml.h"

#include "burrow/core.h"

/* Go's isInCharacterRange. */
bool burrow__xml_is_in_character_range(Rune r);

/* Go's procInst: the value of param in the text of an xml declaration, such
 * as 1.0 for version, or empty. It points into s. */
Str burrow__xml_proc_inst(Str param, Str s);
bool burrow__xml_is_valid_directive(Slice dir);

#endif /* BURROW_SRC_ENCODING_XML_INTERNAL_H */
