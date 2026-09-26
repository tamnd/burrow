/* structs.HostLayout's descriptor. Go's structs has no code, and nor does
 * this, beyond saying what the type is.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/structs.h"

const Type burrow_type_StructsHostLayout = {
    {(const Byte *)"HostLayout", 10},
    {(const Byte *)"structs", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(StructsHostLayout),
    (uint16_t)_Alignof(StructsHostLayout),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};
