/* The patterns ServeMux routes by and the tree and index it keeps them in, from
 * Go's pattern.go, routing_tree.go, routing_index.go and mapping.go. None of it
 * is exported, but the mux and Go's tests for these files use it.
 *
 * Everything here allocates from the Alloc it is handed and frees nothing, so
 * that Alloc is meant to be an arena that goes when the mux does.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_NET_HTTP_ROUTING_H
#define BURROW_SRC_NET_HTTP_ROUTING_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"

#include <stdbool.h>

/* ---------------------------------------------------------------- patterns */

/* segment. A piece of a pattern that matches one or more path segments, or a
 * trailing slash.
 *
 * Not wild, it matches the literal s, or a trailing slash when s is "/", which
 * is how "/{$}" is kept. Wild and not multi, it is "{s}" and matches one
 * segment. Wild and multi, it is "{s...}" and matches all the rest, and a
 * pattern ending in '/' ends in one of these with an empty s. */
typedef struct burrow__HttpSegment {
    Str s;
    bool wild;
    bool multi;
} burrow__HttpSegment;

/* pattern. Something a request can match: an optional method, an optional host
 * and a path. str is the pattern as it was written, and loc is where it was
 * registered, for the messages about conflicts. */
typedef struct burrow__HttpPattern {
    Str str;
    Str method;
    Str host;
    burrow__HttpSegment *segments;
    Int nsegments;
    Str loc;
} burrow__HttpPattern;

/* relationship. How the requests two patterns, p1 and p2, match compare. */
typedef enum burrow__HttpRelationship {
    BURROW__HTTP_EQUIVALENT,    /* both match the same requests */
    BURROW__HTTP_MORE_GENERAL,  /* p1 matches everything p2 does and more */
    BURROW__HTTP_MORE_SPECIFIC, /* p2 matches everything p1 does and more */
    BURROW__HTTP_DISJOINT,      /* no request matches both */
    BURROW__HTTP_OVERLAPS,      /* some request matches both, neither is more
                                   specific */
} burrow__HttpRelationship;

/* The name Go gives r, such as "moreGeneral". */
BURROW_STATIC(ret) Str burrow__http_relationship_string(burrow__HttpRelationship r);

/* parsePattern. s is "[METHOD] [HOST]/[PATH]", where each segment of PATH is a
 * literal or a wildcard, "{name}", "{name...}" or "{$}". The pattern and its
 * strings come from a, s included, so s need not outlive it. The errors say
 * where in s the problem is, "at offset 3: bad wildcard segment ...". */
BURROW_OWNS(ret) burrow__HttpPattern *burrow__http_parse_pattern(Alloc *a, Str s,
                                                                 Error *err);

/* lastSegment. */
BURROW_BORROWS(ret, p) burrow__HttpSegment
burrow__http_pattern_last_segment(const burrow__HttpPattern *p);

/* conflictsWith. Whether some request matches both and neither pattern takes
 * precedence over the other. */
bool burrow__http_pattern_conflicts_with(const burrow__HttpPattern *p1,
                                         const burrow__HttpPattern *p2);

burrow__HttpRelationship
burrow__http_pattern_compare_paths_and_methods(const burrow__HttpPattern *p1,
                                               const burrow__HttpPattern *p2);
burrow__HttpRelationship
burrow__http_pattern_compare_methods(const burrow__HttpPattern *p1,
                                     const burrow__HttpPattern *p2);
burrow__HttpRelationship
burrow__http_pattern_compare_paths(const burrow__HttpPattern *p1,
                                   const burrow__HttpPattern *p2);

/* inverseRelationship. If p1 has relationship r to p2, p2 has this one to p1. */
burrow__HttpRelationship burrow__http_inverse_relationship(burrow__HttpRelationship r);

/* describeConflict. Why p1 and p2 conflict, in words. */
BURROW_OWNS(ret) Str burrow__http_describe_conflict(Alloc *a,
                                                    const burrow__HttpPattern *p1,
                                                    const burrow__HttpPattern *p2);

/* commonPath. A path both match, when there is one. */
BURROW_OWNS(ret) Str burrow__http_common_path(Alloc *a, const burrow__HttpPattern *p1,
                                              const burrow__HttpPattern *p2);

/* differencePath. A path p1 matches and p2 does not, when there is one. */
BURROW_OWNS(ret) Str burrow__http_difference_path(Alloc *a,
                                                  const burrow__HttpPattern *p1,
                                                  const burrow__HttpPattern *p2);

/* pathUnescape. url_path_unescape, or path itself when it is not escaped
 * properly. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str burrow__http_path_unescape(Alloc *a,
                                                                          Str path);

/* cleanPath. path_clean of p made absolute, keeping a trailing slash. A failed
 * allocation gives the empty string. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, p) Str burrow__http_clean_path(Alloc *a, Str p);

/* ----------------------------------------------------------------- mapping */

/* mapping. A map from strings for the children of a routing node, which are
 * mostly few: a slice of up to BURROW__HTTP_MAX_SLICE entries, searched in
 * order, and a Map once there are more. A zeroed one is empty. */
#define BURROW__HTTP_MAX_SLICE 8

typedef struct burrow__HttpEntry {
    Str key;
    void *value;
} burrow__HttpEntry;

typedef struct burrow__HttpMapping {
    burrow__HttpEntry *s; /* NULL once m is in use */
    Int len;
    Map *m; /* of Str to UnsafePointer, NULL until it is needed */
} burrow__HttpMapping;

/* add. k must not be in h already. False when a says no. */
bool burrow__http_mapping_add(Alloc *a, burrow__HttpMapping *h, Str k, void *v);

/* find. The value for k, or NULL with *found false. found may be NULL. */
BURROW_BORROWS(ret, h) void *burrow__http_mapping_find(const burrow__HttpMapping *h,
                                                       Str k, bool *found);

/* eachPair. Calls f with each entry until it returns false. The order is the
 * order they were added in until the Map takes over, and the Map's after. */
typedef bool (*burrow__HttpMappingFunc)(void *env, Str k, void *v);
void burrow__http_mapping_each_pair(const burrow__HttpMapping *h,
                                    burrow__HttpMappingFunc f, void *env);

/* ------------------------------------------------------------ routing tree */

/* routingNode. A node of the tree the mux matches requests with. The root's
 * children are keyed by host, theirs by method and the rest by path segment,
 * with "" for a single wildcard. A node that ends a pattern has the pattern and
 * its handler, which is opaque here. */
typedef struct burrow__HttpRoutingNode {
    burrow__HttpPattern *pattern;
    void *handler;
    burrow__HttpMapping children;
    struct burrow__HttpRoutingNode *multi_child; /* the "{x...}" child */
    struct burrow__HttpRoutingNode *empty_child; /* the child with key "" */
} burrow__HttpRoutingNode;

/* addPattern. Adds p, with h for its handler, under root. False when a says
 * no. */
bool burrow__http_routing_add_pattern(Alloc *a, burrow__HttpRoutingNode *root,
                                      burrow__HttpPattern *p, void *h);

/* findChild. */
BURROW_BORROWS(ret, n) burrow__HttpRoutingNode *
burrow__http_routing_find_child(const burrow__HttpRoutingNode *n, Str key);

/* match. The leaf matching the request, or NULL. *matches is a slice of Str
 * with the values of the pattern's wildcards in order, made with a, which is
 * also where unescaped path segments go. */
BURROW_BORROWS(ret, root) burrow__HttpRoutingNode *
burrow__http_routing_match(const burrow__HttpRoutingNode *root, Alloc *a, Str host,
                           Str method, Str path, Slice *matches);

/* firstSegment. The first segment of path, unescaped, and in *rest the path
 * after it. path starts with '/', and the segment of "/" is "/". */
BURROW_OWNS(ret) Str burrow__http_first_segment(Alloc *a, Str path, Str *rest);

/* matchingMethods. Adds to method_set, a Map of Str to bool, each method that
 * some pattern for host and path has, and HEAD when it has GET. False when a
 * says no. */
bool burrow__http_routing_matching_methods(const burrow__HttpRoutingNode *root,
                                           Alloc *a, Str host, Str path,
                                           Map *method_set);

/* ----------------------------------------------------------- routing index */

/* routingIndex. Finds the patterns a new one might conflict with, so that
 * registering does not have to compare it with every pattern there is.
 * segments[pos] is a Map from the literal at position pos, or "" for a
 * wildcard, to a Slice * of the patterns with that there, which is Go's map
 * keyed by position and literal split by position. multis holds the patterns
 * ending in a multi. A zeroed one is empty. */
typedef struct burrow__HttpRoutingIndex {
    Map **segments;
    Int nsegments;
    Slice multis; /* of burrow__HttpPattern * */
} burrow__HttpRoutingIndex;

/* addPattern. False when a says no. */
bool burrow__http_routing_index_add_pattern(Alloc *a, burrow__HttpRoutingIndex *idx,
                                            burrow__HttpPattern *pat);

/* possiblyConflictingPatterns. Calls f with every pattern that might conflict
 * with pat, and perhaps some that do not, stopping at the first error f
 * returns, which is then returned. */
typedef Error (*burrow__HttpPatternFunc)(void *env, burrow__HttpPattern *p);
BURROW_BORROWS(ret) Error burrow__http_routing_index_possibly_conflicting(
    const burrow__HttpRoutingIndex *idx, const burrow__HttpPattern *pat,
    burrow__HttpPatternFunc f, void *env);

#endif
