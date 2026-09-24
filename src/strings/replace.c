/* Derived from Go's src/strings/replace.go and src/strings/search.go.
 * Go source: go1.27.1.
 *
 * The four replacers Go picks between, picked the same way: a byte table when
 * every old and new string is one byte, a table of strings when every old one
 * is, Boyer-Moore for a single pair with a longer old string, and a trie for
 * the rest. Go's is an interface with four implementations and this is a tag
 * and a switch.
 *
 * Everything the build makes comes from the replacer's allocator, and the
 * replacer keeps a list of it so strings_replacer_free can give it back.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strings.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include "internal.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------ stringFinder */

/* Boyer-Moore, from search.go. */
typedef struct StringFinder {
    /* pattern is the string that we are searching for in the text. */
    Str pattern;

    /* badCharSkip[b] contains the distance between the last byte of pattern
     * and the rightmost occurrence of b in pattern. If b is not in pattern,
     * badCharSkip[b] is len(pattern). */
    Int bad_char_skip[256];

    /* goodSuffixSkip[i] defines how far we can shift the matching frame given
     * that the suffix pattern[i+1:] matches, but the byte pattern[i] does not.
     * There are two cases to consider, see search.go. */
    Int *good_suffix_skip;
} StringFinder;

static Int longest_common_suffix(Str a, Str b) {
    Int i = 0;
    for (; i < a.len && i < b.len; i++) {
        if (a.p[a.len - 1 - i] != b.p[b.len - 1 - i])
            break;
    }
    return i;
}

static void finder_init(StringFinder *f, Str pattern, Int *good_suffix_skip) {
    f->pattern = pattern;
    f->good_suffix_skip = good_suffix_skip;
    Int last = pattern.len - 1;

    /* Build bad character table. Bytes not in the pattern can skip one
     * pattern's length. */
    for (Int i = 0; i < 256; i++)
        f->bad_char_skip[i] = pattern.len;
    /* The loop condition is < instead of <= so that the last byte does not
     * have a zero distance to itself. Finding this byte out of place implies
     * that it is not in the last position. */
    for (Int i = 0; i < last; i++)
        f->bad_char_skip[pattern.p[i]] = last - i;

    /* Build good suffix table. First pass: set each value to the next index
     * which starts a prefix of pattern. */
    Int last_prefix = last;
    for (Int i = last; i >= 0; i--) {
        Str suffix = {pattern.p + i + 1, pattern.len - i - 1};
        if (strings_has_prefix(pattern, suffix))
            last_prefix = i + 1;
        /* lastPrefix is the shift, and (last-i) is len(suffix). */
        f->good_suffix_skip[i] = last_prefix + last - i;
    }
    /* Second pass: find repeats of pattern's suffix starting from the front. */
    for (Int i = 0; i < last; i++) {
        Str mid = {pattern.p + 1, i};
        Int len_suffix = longest_common_suffix(pattern, mid);
        if (pattern.p[i - len_suffix] != pattern.p[last - len_suffix])
            /* (last-i) is the shift, and lenSuffix is len(suffix). */
            f->good_suffix_skip[last - len_suffix] = len_suffix + last - i;
    }
}

static Int finder_next(const StringFinder *f, Str text) {
    Int i = f->pattern.len - 1;
    while (i < text.len) {
        /* Compare backwards from the end until the first unmatching character. */
        Int j = f->pattern.len - 1;
        while (j >= 0 && text.p[i] == f->pattern.p[j]) {
            i--;
            j--;
        }
        if (j < 0)
            return i + 1; /* match */
        Int bad = f->bad_char_skip[text.p[i]];
        Int good = f->good_suffix_skip[j];
        i += bad > good ? bad : good;
    }
    return -1;
}

/* ------------------------------------------------------------ the replacer */

typedef struct TrieNode TrieNode;

/* trieNode from replace.go. A node with a table branches on the next byte,
 * one with a prefix follows it to next, and priority is how early in the list
 * the pair ending here came, higher for earlier, zero for none. */
struct TrieNode {
    Str value;
    Int priority;
    Str prefix;
    TrieNode *next;
    TrieNode **table;
};

typedef enum ReplacerKind {
    REPLACER_GENERIC,
    REPLACER_SINGLE_STRING,
    REPLACER_BYTE,
    REPLACER_BYTE_STRING,
} ReplacerKind;

/* Every block the build allocates, so they can all be freed. */
typedef struct ReplacerBlock {
    struct ReplacerBlock *next;
    size_t size;
} ReplacerBlock;

struct StringsReplacer {
    SyncOnce once;
    Alloc *a;
    Str *oldnew; /* the copied list, dropped after the build */
    Int n;
    ReplacerKind kind;
    bool failed; /* the build ran out of memory */
    ReplacerBlock *blocks;

    /* byteReplacer */
    Byte bytes[256];

    /* byteStringReplacer. replacements[b] is set when has[b] is, since an
     * empty replacement is still one. to_replace is the distinct old bytes. */
    Str replacements[256];
    bool has[256];
    Byte to_replace[256];
    Int n_to_replace;

    /* singleStringReplacer */
    StringFinder finder;
    Str value;

    /* genericReplacer. mapping maps from key bytes to a dense index for
     * trieNode.table, and bytes in no key map to table_size. */
    TrieNode root;
    Int table_size;
    Byte mapping[256];
};

static void *replacer_alloc(StringsReplacer *r, size_t size) {
    size_t hdr = (sizeof(ReplacerBlock) + _Alignof(max_align_t) - 1) &
                 ~(size_t)(_Alignof(max_align_t) - 1);
    ReplacerBlock *b =
        (ReplacerBlock *)mem_alloc(r->a, hdr + size, _Alignof(max_align_t));
    if (b == NULL) {
        r->failed = true;
        return NULL;
    }
    b->next = r->blocks;
    b->size = hdr + size;
    r->blocks = b;
    return (Byte *)b + hdr;
}

static TrieNode *new_node(StringsReplacer *r) {
    return (TrieNode *)replacer_alloc(r, sizeof(TrieNode));
}

static TrieNode **new_table(StringsReplacer *r) {
    return (TrieNode **)replacer_alloc(r, (size_t)r->table_size * sizeof(TrieNode *));
}

static Str trie_tail(Str s, Int lo) {
    Str t = {lo == 0 ? s.p : s.p + lo, s.len - lo};
    return t;
}

static void trie_add(TrieNode *t, Str key, Str val, Int priority, StringsReplacer *r) {
    if (r->failed)
        return;
    if (key.len == 0) {
        if (t->priority == 0) {
            t->value = val;
            t->priority = priority;
        }
        return;
    }

    if (t->prefix.len != 0) {
        /* Need to split the prefix among multiple nodes. */
        Int n = 0; /* length of the longest common prefix */
        for (; n < t->prefix.len && n < key.len; n++) {
            if (t->prefix.p[n] != key.p[n])
                break;
        }
        if (n == t->prefix.len) {
            trie_add(t->next, trie_tail(key, n), val, priority, r);
        } else if (n == 0) {
            /* First byte differs, start a new lookup table here. Looking up
             * what is currently t.prefix[0] will lead to prefixNode, and
             * looking up key[0] will lead to keyNode. */
            TrieNode *prefix_node;
            if (t->prefix.len == 1) {
                prefix_node = t->next;
            } else {
                prefix_node = new_node(r);
                if (prefix_node == NULL)
                    return;
                prefix_node->prefix = trie_tail(t->prefix, 1);
                prefix_node->next = t->next;
            }
            TrieNode *key_node = new_node(r);
            TrieNode **table = new_table(r);
            if (key_node == NULL || table == NULL)
                return;
            t->table = table;
            t->table[r->mapping[t->prefix.p[0]]] = prefix_node;
            t->table[r->mapping[key.p[0]]] = key_node;
            t->prefix = BURROW_STR_EMPTY;
            t->next = NULL;
            trie_add(key_node, trie_tail(key, 1), val, priority, r);
        } else {
            /* Insert new node after the common section of the prefix. */
            TrieNode *next = new_node(r);
            if (next == NULL)
                return;
            next->prefix = trie_tail(t->prefix, n);
            next->next = t->next;
            t->prefix.len = n;
            t->next = next;
            trie_add(next, trie_tail(key, n), val, priority, r);
        }
    } else if (t->table != NULL) {
        /* Insert into existing table. */
        Byte m = r->mapping[key.p[0]];
        if (t->table[m] == NULL) {
            t->table[m] = new_node(r);
            if (t->table[m] == NULL)
                return;
        }
        trie_add(t->table[m], trie_tail(key, 1), val, priority, r);
    } else {
        t->prefix = key;
        t->next = new_node(r);
        if (t->next == NULL)
            return;
        trie_add(t->next, BURROW_STR_EMPTY, val, priority, r);
    }
}

/* Iterate down the trie to the end, and grab the value and keylen with the
 * highest priority. */
static bool generic_lookup(const StringsReplacer *r, Str s, bool ignore_root, Str *val,
                           Int *keylen) {
    Int best_priority = 0;
    const TrieNode *node = &r->root;
    Int n = 0;
    bool found = false;
    *val = BURROW_STR_EMPTY;
    *keylen = 0;
    while (node != NULL) {
        if (node->priority > best_priority && !(ignore_root && node == &r->root)) {
            best_priority = node->priority;
            *val = node->value;
            *keylen = n;
            found = true;
        }

        if (s.len == 0)
            break;
        if (node->table != NULL) {
            Byte index = r->mapping[s.p[0]];
            if ((Int)index == r->table_size)
                break;
            node = node->table[index];
            s = trie_tail(s, 1);
            n++;
        } else if (node->prefix.len != 0 && strings_has_prefix(s, node->prefix)) {
            n += node->prefix.len;
            s = trie_tail(s, node->prefix.len);
            node = node->next;
        } else {
            break;
        }
    }
    return found;
}

static void build_generic(StringsReplacer *r) {
    r->kind = REPLACER_GENERIC;
    /* Find each byte used, then assign them each an index. */
    for (Int i = 0; i < r->n; i += 2) {
        Str key = r->oldnew[i];
        for (Int j = 0; j < key.len; j++)
            r->mapping[key.p[j]] = 1;
    }

    for (Int i = 0; i < 256; i++)
        r->table_size += r->mapping[i];

    Byte index = 0;
    for (Int i = 0; i < 256; i++) {
        if (r->mapping[i] == 0) {
            r->mapping[i] = (Byte)r->table_size;
        } else {
            r->mapping[i] = index;
            index++;
        }
    }
    /* Ensure root node uses a lookup table (for performance). */
    r->root.table = new_table(r);
    if (r->root.table == NULL)
        return;

    for (Int i = 0; i < r->n; i += 2)
        trie_add(&r->root, r->oldnew[i], r->oldnew[i + 1], r->n - i, r);
}

static void build(StringsReplacer *r) {
    Str *oldnew = r->oldnew;
    if (r->n == 2 && oldnew[0].len > 1) {
        r->kind = REPLACER_SINGLE_STRING;
        Int *skip = (Int *)replacer_alloc(r, (size_t)oldnew[0].len * sizeof(Int));
        if (skip == NULL)
            return;
        finder_init(&r->finder, oldnew[0], skip);
        r->value = oldnew[1];
        return;
    }

    bool all_new_bytes = true;
    for (Int i = 0; i < r->n; i += 2) {
        if (oldnew[i].len != 1) {
            build_generic(r);
            return;
        }
        if (oldnew[i + 1].len != 1)
            all_new_bytes = false;
    }

    if (all_new_bytes) {
        r->kind = REPLACER_BYTE;
        for (Int i = 0; i < 256; i++)
            r->bytes[i] = (Byte)i;
        /* The first occurrence of old->new map takes precedence over the others
         * with the same old string. */
        for (Int i = r->n - 2; i >= 0; i -= 2)
            r->bytes[oldnew[i].p[0]] = oldnew[i + 1].p[0];
        return;
    }

    r->kind = REPLACER_BYTE_STRING;
    /* The first occurrence of old->new map takes precedence over the others
     * with the same old string. */
    for (Int i = r->n - 2; i >= 0; i -= 2) {
        Byte o = oldnew[i].p[0];
        if (!r->has[o])
            r->to_replace[r->n_to_replace++] = o;
        r->replacements[o] = oldnew[i + 1];
        r->has[o] = true;
    }
}

static void build_once(void *env) {
    StringsReplacer *r = (StringsReplacer *)env;
    build(r);
    mem_free(r->a, r->oldnew, (size_t)r->n * sizeof(Str), _Alignof(Str));
    r->oldnew = NULL;
}

StringsReplacer *strings_new_replacer(Alloc *a, Slice oldnew) {
    if (oldnew.len % 2 == 1)
        panic_str(BURROW_S("strings.NewReplacer: odd argument count"));
    StringsReplacer *r = BURROW_NEW(a, StringsReplacer);
    if (r == NULL)
        return NULL;
    r->a = a;
    r->n = oldnew.len;
    if (oldnew.len > 0) {
        r->oldnew =
            (Str *)mem_alloc_nozero(a, (size_t)oldnew.len * sizeof(Str), _Alignof(Str));
        if (r->oldnew == NULL) {
            mem_free(a, r, sizeof *r, _Alignof(StringsReplacer));
            return NULL;
        }
        memcpy(r->oldnew, oldnew.p, (size_t)oldnew.len * sizeof(Str));
    }
    return r;
}

void strings_replacer_free(StringsReplacer *r) {
    if (r == NULL)
        return;
    Alloc *a = r->a;
    for (ReplacerBlock *b = r->blocks; b != NULL;) {
        ReplacerBlock *next = b->next;
        mem_free(a, b, b->size, _Alignof(max_align_t));
        b = next;
    }
    if (r->oldnew != NULL)
        mem_free(a, r->oldnew, (size_t)r->n * sizeof(Str), _Alignof(Str));
    mem_free(a, r, sizeof *r, _Alignof(StringsReplacer));
}

static bool replacer_ready(StringsReplacer *r) {
    sync_once_do(&r->once, BURROW_FN(Func, build_once, r));
    return !r->failed;
}

/* ------------------------------------------------------------ writing out
 *
 * Go's replacers write through an io.StringWriter, and Replace hands them a
 * byte slice to append to. Here both are an IoWriter, with a builder behind it
 * for Replace. */

static Int write_out(IoWriter w, Str s, Error *err) {
    if (s.len == 0) {
        *err = BURROW_NO_ERROR;
        return 0;
    }
    return BURROW_CALL(w, write, burrow__strings_bytes(s), err);
}

static Str trie_sub(Str s, Int lo, Int hi) {
    Str t = {lo == 0 ? s.p : s.p + lo, hi - lo};
    return t;
}

static Int generic_write_string(StringsReplacer *r, IoWriter w, Str s, Error *err) {
    Int n = 0;
    Int last = 0;
    bool prev_match_empty = false;
    *err = BURROW_NO_ERROR;
    for (Int i = 0; i <= s.len;) {
        /* Fast path: s[i] is not a prefix of any pattern. */
        if (i != s.len && r->root.priority == 0) {
            Int index = r->mapping[s.p[i]];
            if (index == r->table_size || r->root.table[index] == NULL) {
                i++;
                continue;
            }
        }

        /* Ignore the empty match iff the previous loop found the empty match. */
        Str val;
        Int keylen;
        bool match =
            generic_lookup(r, trie_tail(s, i), prev_match_empty, &val, &keylen);
        prev_match_empty = match && keylen == 0;
        if (match) {
            n += write_out(w, trie_sub(s, last, i), err);
            if (BURROW_FAILED(*err))
                return n;
            n += write_out(w, val, err);
            if (BURROW_FAILED(*err))
                return n;
            i += keylen;
            last = i;
            continue;
        }
        i++;
    }
    if (last != s.len)
        n += write_out(w, trie_tail(s, last), err);
    return n;
}

static Int single_write_string(StringsReplacer *r, IoWriter w, Str s, Error *err) {
    Int n = 0;
    Int i = 0;
    for (;;) {
        Int match = finder_next(&r->finder, trie_tail(s, i));
        if (match == -1)
            break;
        n += write_out(w, trie_sub(s, i, i + match), err);
        if (BURROW_FAILED(*err))
            return n;
        n += write_out(w, r->value, err);
        if (BURROW_FAILED(*err))
            return n;
        i += match + r->finder.pattern.len;
    }
    n += write_out(w, trie_tail(s, i), err);
    return n;
}

static Int byte_write_string(StringsReplacer *r, IoWriter w, Str s, Error *err) {
    Int n = 0;
    Int last = 0;
    *err = BURROW_NO_ERROR;
    for (Int i = 0; i < s.len; i++) {
        Byte b = s.p[i];
        bool change = r->kind == REPLACER_BYTE ? r->bytes[b] != b : r->has[b];
        if (!change)
            continue;
        if (last != i) {
            n += write_out(w, trie_sub(s, last, i), err);
            if (BURROW_FAILED(*err))
                return n;
        }
        last = i + 1;
        Str repl =
            r->kind == REPLACER_BYTE ? (Str){&r->bytes[b], 1} : r->replacements[b];
        /* Go writes the replacement with w.Write, which gets called even when
         * the replacement is empty. */
        n += BURROW_CALL(w, write, burrow__strings_bytes(repl), err);
        if (BURROW_FAILED(*err))
            return n;
    }
    if (last != s.len)
        n += write_out(w, trie_tail(s, last), err);
    return n;
}

Int strings_replacer_write_string(StringsReplacer *r, IoWriter w, Str s, Error *err) {
    Error e;
    if (!replacer_ready(r)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return 0;
    }
    Int n;
    switch (r->kind) {
    case REPLACER_SINGLE_STRING:
        n = single_write_string(r, w, s, &e);
        break;
    case REPLACER_BYTE:
    case REPLACER_BYTE_STRING:
        n = byte_write_string(r, w, s, &e);
        break;
    case REPLACER_GENERIC:
    default:
        n = generic_write_string(r, w, s, &e);
        break;
    }
    BURROW_OUT(err, e);
    return n;
}

/* ------------------------------------------------------------ Replace */

static Str byte_replace(StringsReplacer *r, Alloc *a, Str s) {
    Byte *buf = NULL; /* lazily allocated */
    for (Int i = 0; i < s.len; i++) {
        Byte b = s.p[i];
        if (r->bytes[b] != b) {
            if (buf == NULL) {
                buf = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
                if (buf == NULL)
                    return BURROW_STR_EMPTY;
                memcpy(buf, s.p, (size_t)s.len);
            }
            buf[i] = r->bytes[b];
        }
    }
    if (buf == NULL)
        return s;
    Str out = {buf, s.len};
    return out;
}

/* countCutOff controls the ratio of a string length to a number of replacements
 * at which (*byteStringReplacer).Replace switches algorithms. For strings with
 * higher ration of length to replacements than that value, we call Count, for
 * each replacement from toReplace. For strings, with a lower ratio we use
 * simple loop, because of Count overhead. countCutOff is an empirically
 * determined overhead multiplier. */
#define REPLACER_COUNT_CUT_OFF 8

static Str byte_string_replace(StringsReplacer *r, Alloc *a, Str s) {
    Int new_size = s.len;
    bool any_changes = false;
    /* Is it faster to use Count? */
    if (r->n_to_replace * REPLACER_COUNT_CUT_OFF <= s.len) {
        for (Int k = 0; k < r->n_to_replace; k++) {
            Byte x = r->to_replace[k];
            Str xs = {&r->to_replace[k], 1};
            Int c = strings_count(s, xs);
            if (c != 0) {
                /* The -1 is because we are replacing 1 byte with
                 * len(replacements[b]) bytes. */
                new_size += c * (r->replacements[x].len - 1);
                any_changes = true;
            }
        }
    } else {
        for (Int i = 0; i < s.len; i++) {
            Byte b = s.p[i];
            if (r->has[b]) {
                /* See above for explanation of -1 */
                new_size += r->replacements[b].len - 1;
                any_changes = true;
            }
        }
    }
    if (!any_changes)
        return s;
    if (new_size == 0)
        return BURROW_STR_EMPTY;
    Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)new_size, 1);
    if (buf == NULL)
        return BURROW_STR_EMPTY;
    Int j = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte b = s.p[i];
        if (r->has[b]) {
            Str repl = r->replacements[b];
            if (repl.len > 0)
                memcpy(buf + j, repl.p, (size_t)repl.len);
            j += repl.len;
        } else {
            buf[j] = b;
            j++;
        }
    }
    Str out = {buf, new_size};
    return out;
}

static Str single_replace(StringsReplacer *r, Alloc *a, Str s) {
    StringsBuilder buf = STRINGS_BUILDER(a);
    Int i = 0;
    bool matched = false;
    bool ok = true;
    Error err;
    for (;;) {
        Int match = finder_next(&r->finder, trie_tail(s, i));
        if (match == -1)
            break;
        matched = true;
        if (!strings_builder_grow(&buf, match + r->value.len))
            return BURROW_STR_EMPTY;
        strings_builder_write_string(&buf, trie_sub(s, i, i + match), &err);
        ok = ok && BURROW_OK(err);
        strings_builder_write_string(&buf, r->value, &err);
        ok = ok && BURROW_OK(err);
        i += match + r->finder.pattern.len;
    }
    if (!matched)
        return s;
    strings_builder_write_string(&buf, trie_tail(s, i), &err);
    ok = ok && BURROW_OK(err);
    return ok ? strings_builder_string(&buf) : BURROW_STR_EMPTY;
}

static Str generic_replace(StringsReplacer *r, Alloc *a, Str s) {
    StringsBuilder buf = STRINGS_BUILDER(a);
    if (s.len > 0 && !strings_builder_grow(&buf, s.len))
        return BURROW_STR_EMPTY;
    Error err;
    generic_write_string(r, strings_builder_as_io_writer(&buf), s, &err);
    if (BURROW_FAILED(err))
        return BURROW_STR_EMPTY;
    return strings_builder_string(&buf);
}

Str strings_replacer_replace(StringsReplacer *r, Alloc *a, Str s) {
    if (!replacer_ready(r))
        return BURROW_STR_EMPTY;
    switch (r->kind) {
    case REPLACER_SINGLE_STRING:
        return single_replace(r, a, s);
    case REPLACER_BYTE:
        return byte_replace(r, a, s);
    case REPLACER_BYTE_STRING:
        return byte_string_replace(r, a, s);
    case REPLACER_GENERIC:
    default:
        return generic_replace(r, a, s);
    }
}

/* ------------------------------------------------------------ for the tests
 *
 * What export_test.go gives Go's tests, declared in internal.h. */

const char *burrow__strings_replacer_kind(StringsReplacer *r) {
    if (!replacer_ready(r))
        return NULL;
    switch (r->kind) {
    case REPLACER_SINGLE_STRING:
        return "*strings.singleStringReplacer";
    case REPLACER_BYTE:
        return "*strings.byteReplacer";
    case REPLACER_BYTE_STRING:
        return "*strings.byteStringReplacer";
    case REPLACER_GENERIC:
    default:
        return "*strings.genericReplacer";
    }
}

static void print_node(StringsBuilder *b, const StringsReplacer *r, const TrieNode *t,
                       Int depth) {
    strings_builder_write_byte(b, t->priority > 0 ? '+' : '-');
    strings_builder_write_byte(b, '\n');
    if (t->prefix.len != 0) {
        for (Int i = 0; i < depth; i++)
            strings_builder_write_byte(b, '.');
        strings_builder_write_string(b, t->prefix, NULL);
        print_node(b, r, t->next, depth + t->prefix.len);
    } else if (t->table != NULL) {
        for (Int c = 0; c < 256; c++) {
            Int m = r->mapping[c];
            if (m != r->table_size && t->table[m] != NULL) {
                for (Int i = 0; i < depth; i++)
                    strings_builder_write_byte(b, '.');
                strings_builder_write_byte(b, (Byte)c);
                print_node(b, r, t->table[m], depth + 1);
            }
        }
    }
}

Str burrow__strings_print_trie(StringsReplacer *r, Alloc *a) {
    if (!replacer_ready(r) || r->kind != REPLACER_GENERIC)
        return BURROW_STR_EMPTY;
    StringsBuilder b = STRINGS_BUILDER(a);
    print_node(&b, r, &r->root, 0);
    return strings_builder_string(&b);
}

Int burrow__strings_string_find(Alloc *a, Str pattern, Str text) {
    StringFinder f;
    Int *skip =
        (Int *)mem_alloc(a, (size_t)pattern.len * sizeof(Int) + 1, _Alignof(Int));
    if (skip == NULL)
        return -2;
    finder_init(&f, pattern, skip);
    return finder_next(&f, text);
}

bool burrow__strings_dump_tables(Alloc *a, Str pattern, Int bad[256], Int **good) {
    StringFinder f;
    Int *skip =
        (Int *)mem_alloc(a, (size_t)pattern.len * sizeof(Int) + 1, _Alignof(Int));
    if (skip == NULL)
        return false;
    finder_init(&f, pattern, skip);
    memcpy(bad, f.bad_char_skip, sizeof f.bad_char_skip);
    *good = skip;
    return true;
}
