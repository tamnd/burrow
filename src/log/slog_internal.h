/* What the three slog files share and nothing outside them needs. The default
 * handler's constructor is here as well, for the test that checks its output
 * without going through log's standard logger.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_LOG_SLOG_INTERNAL_H
#define BURROW_SRC_LOG_SLOG_INTERNAL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/log/slog.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>

#define BURROW__SLOG_BAD_KEY BURROW_S("!BADKEY")

/* Value.isEmptyGroup and Attr.isEmpty. */
bool burrow__slog_value_is_empty_group(SlogValue v);
bool burrow__slog_attr_is_empty(SlogAttr a);

/* countEmptyGroups, over a Slice of SlogAttr. */
Int burrow__slog_count_empty_groups(Slice as);

/* argsToAttr: the attribute that starts at args[*i], moving *i past what it
 * used. */
BURROW_BORROWS(ret, args) SlogAttr burrow__slog_args_to_attr(Slice args, Int *i);

/* countAttrs: how many attributes args makes. */
Int burrow__slog_count_attrs(Slice args);

/* argsToAttrSlice, made in a. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, args) Slice burrow__slog_args_to_attrs(Alloc *a,
                                                                            Slice args);

/* Value.append: v as Value.String writes it, added to dst. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice burrow__slog_value_append(SlogValue v,
                                                                          Alloc *a,
                                                                          Slice dst);

/* Source.group, made in a. */
BURROW_OWNS(ret) SlogValue burrow__slog_source_group(Alloc *a, const SlogSource *s);

/* logLoggerLevel: the level the default handler is enabled at, and the level
 * a log package line becomes once slog_set_default has taken log over. */
extern SlogLevelVar burrow__slog_log_logger_level;

/* The default handler slog_default starts with, which is static, and whether
 * a handler is that one or one made from it, which is Go's check for
 * *defaultHandler in SetDefault. */
SlogHandler burrow__slog_std_handler(void);
bool burrow__slog_is_default_handler(SlogHandler h);

/* A default handler that writes each line through output rather than through
 * log's standard logger, as newDefaultHandler does with its output func. */
typedef Error (*burrow__SlogOutputFunc)(void *ctx, Str file, Int line, Str s);
BURROW_OWNS(ret) SlogHandler
burrow__slog_new_default_handler(Alloc *a, burrow__SlogOutputFunc output, void *ctx);

#endif /* BURROW_SRC_LOG_SLOG_INTERNAL_H */
