/* crypto/x509: name constraints, checked over a whole chain, from Go's
 * constraints.go.
 *
 * Each set of constraints is sorted and pruned, so that no constraint is
 * inside one before it, and a name is then matched with a binary search for
 * the nearest constraint below it. That keeps a certificate with many names
 * and many constraints from taking quadratic time.
 *
 * Everything here borrows from the certificates. What the checks allocate
 * comes from the scratch allocator the caller passes in, and the errors from
 * error_allocator().
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/net/url.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strings.h"

#include "x509_internal.h"

#include <string.h>

/* ------------------------------------------------------------ the sets */

/* nameConstraintsSet.sortAndPrune. subset(a, b) says whether b is inside a,
 * and takes pointers to elements. */
static void x509_sort_and_prune(Slice *set, SlicesCmpFunc cmp,
                                bool (*subset)(const void *a, const void *b)) {
    if (set->len < 2)
        return;
    slices_sort_func(*set, cmp);
    Byte *p = set->p;
    size_t size = set->elem->size;
    Int write = 1;
    for (Int read = 1; read < set->len; read++) {
        if (!subset(p + (size_t)(write - 1) * size, p + (size_t)read * size)) {
            memmove(p + (size_t)write * size, p + (size_t)read * size, size);
            write++;
        }
    }
    set->len = write;
}

/* nameConstraintsSet.search: the constraint s is in, or NULL. cmp compares an
 * element with s, and match(element, s) says whether it covers s. */
static const void *x509_search(Slice set, const void *s, SlicesCmpFunc cmp,
                               bool (*match)(const void *c, const void *s)) {
    if (set.len == 0)
        return NULL;
    bool found = false;
    Int i = slices_binary_search_func(set, s, cmp, &found);
    const Byte *p = set.p;
    size_t size = set.elem->size;
    if (found)
        return p + (size_t)i * size;
    const void *constraint = p + (size_t)(i == 0 ? 0 : i - 1) * size;
    return match(constraint, s) ? constraint : NULL;
}

/* ------------------------------------------------------------------ IPs */

static bool x509_ip_network_subset(const void *pa, const void *pb) {
    const NetIPNet *a = *(NetIPNet *const *)pa;
    const NetIPNet *b = *(NetIPNet *const *)pb;
    if (!net_ip_net_contains(a, b->ip))
        return false;
    Byte buf[16];
    if (b->ip.len > 16 || b->mask.len < b->ip.len)
        return false;
    const Byte *ip = b->ip.p;
    const Byte *mask = b->mask.p;
    for (Int i = 0; i < b->ip.len; i++)
        buf[i] = (Byte)(ip[i] | (Byte)~mask[i]);
    return net_ip_net_contains(a, slice_from(buf, b->ip.len, b->ip.len, TYPE_BYTE));
}

static int x509_ip_network_compare(void *env, const void *pa, const void *pb) {
    (void)env;
    const NetIPNet *a = *(NetIPNet *const *)pa;
    const NetIPNet *b = *(NetIPNet *const *)pb;
    Int i = bytes_compare(a->ip, b->ip);
    if (i != 0)
        return (int)i;
    return (int)bytes_compare(a->mask, b->mask);
}

static int x509_ip_binary_search(void *env, const void *pc, const void *target) {
    (void)env;
    const NetIPNet *c = *(NetIPNet *const *)pc;
    return (int)bytes_compare(c->ip, *(const NetIP *)target);
}

static bool x509_ip_match(const void *pc, const void *target) {
    return net_ip_net_contains(*(NetIPNet *const *)pc, *(const NetIP *)target);
}

/* ipConstraints. NULL is Go's nil interface, no constraints at all. */
typedef struct X509IPConstraints {
    Slice ipv4, ipv6;
} X509IPConstraints;

/* newIPNetConstraints */
static X509IPConstraints *x509_new_ip_constraints(Alloc *a, Slice l) {
    if (l.len == 0)
        return NULL;
    X509IPConstraints *c = BURROW_NEW(a, X509IPConstraints);
    if (c == NULL)
        return NULL;
    c->ipv4 = slice_nil(TYPE_X509_IP_NET_PTR);
    c->ipv6 = slice_nil(TYPE_X509_IP_NET_PTR);
    NetIPNet *const *nets = l.p;
    for (Int i = 0; i < l.len; i++) {
        NetIPNet *n = nets[i];
        /* Subtrees may carry host bits. Sorting and searching need the
         * network address, so mask a copy and leave the parsed one alone. */
        NetIP masked = net_ip_mask(n->ip, a, n->mask);
        if (masked.len != 0 && !net_ip_equal(masked, n->ip)) {
            NetIPNet *m = BURROW_NEW(a, NetIPNet);
            if (m == NULL)
                return NULL;
            m->ip = masked;
            m->mask = n->mask;
            n = m;
        }
        Slice *to = n->ip.len == NET_IPV4_LEN ? &c->ipv4 : &c->ipv6;
        *to = slice_append(a, *to, &n, 1);
    }
    SlicesCmpFunc cmp = BURROW_FN(SlicesCmpFunc, x509_ip_network_compare, NULL);
    x509_sort_and_prune(&c->ipv4, cmp, x509_ip_network_subset);
    x509_sort_and_prune(&c->ipv6, cmp, x509_ip_network_subset);
    return c;
}

static bool x509_ip_query(const X509IPConstraints *c, NetIP ip) {
    Slice set = ip.len == NET_IPV4_LEN ? c->ipv4 : c->ipv6;
    return x509_search(set, &ip, BURROW_FN(SlicesCmpFunc, x509_ip_binary_search, NULL),
                       x509_ip_match) != NULL;
}

/* ------------------------------------------------------------ DNS names */

/* dnsHasSuffix: whether b ends with the labels of a, ignoring ASCII case. A
 * constraint with a leading period matches below it and not itself. */
static bool x509_dns_has_suffix(Str a, Str b) {
    Int len_a = a.len, len_b = b.len;
    if (len_a > len_b)
        return false;
    Int offset = len_a - len_b;
    for (Int i = len_a - 1; i >= 0; i--) {
        Byte ar = a.p[i], br = b.p[i - offset];
        if (ar == br)
            continue;
        if (br < ar) {
            Byte t = ar;
            ar = br;
            br = t;
        }
        if ('A' <= ar && ar <= 'Z' && br == ar + 'a' - 'A')
            continue;
        return false;
    }
    if (a.p[0] != '.' && len_b > len_a && b.p[len_b - len_a - 1] != '.')
        return false;
    return true;
}

static bool x509_dns_subset(const void *a, const void *b) {
    return x509_dns_has_suffix(*(const Str *)a, *(const Str *)b);
}

/* dnsCompareTable: lower case, and the period below everything so that a
 * name sorts next to the names under it. */
static Byte x509_dns_fold(Byte c) {
    if (c == '.')
        return 0;
    if ('A' <= c && c <= 'Z')
        return (Byte)(c + 'a' - 'A');
    return c;
}

/* dnsCompare: strings.Compare from the end, folded as above. */
static int x509_dns_compare_str(Str a, Str b) {
    Int ia = a.len - 1, ib = b.len - 1;
    while (ia >= 0 && ib >= 0) {
        Byte ba = x509_dns_fold(a.p[ia]), bb = x509_dns_fold(b.p[ib]);
        if (ba == bb) {
            ia--;
            ib--;
            continue;
        }
        return ba < bb ? -1 : 1;
    }
    if (ia < ib)
        return -1;
    if (ib < ia)
        return 1;
    return 0;
}

static int x509_dns_compare(void *env, const void *a, const void *b) {
    (void)env;
    return x509_dns_compare_str(*(const Str *)a, *(const Str *)b);
}

/* trimFirstLabel: dnsName from its first period on, or "" with none. */
static Str x509_trim_first_label(Str s) {
    const Byte *dot = memchr(s.p, '.', (size_t)s.len);
    if (dot == NULL)
        return BURROW_STR_EMPTY;
    return str_from_bytes(dot, s.len - (dot - s.p));
}

/* dnsConstraints. parents is only made for excluded ones, and holds each
 * constraint without its first label, for wildcard names. */
typedef struct X509DnsConstraints {
    bool all;
    bool permitted;
    Slice set;
    Map *parents;
} X509DnsConstraints;

/* newDNSConstraints */
static X509DnsConstraints *x509_new_dns_constraints(Alloc *a, Slice l, bool permitted) {
    if (l.len == 0)
        return NULL;
    X509DnsConstraints *nc = BURROW_NEW(a, X509DnsConstraints);
    if (nc == NULL)
        return NULL;
    const Str *names = l.p;
    for (Int i = 0; i < l.len; i++) {
        if (names[i].len == 0) {
            nc->all = true;
            return nc;
        }
    }
    nc->permitted = permitted;
    nc->set = slice_append(a, slice_nil(TYPE_STRING), l.p, l.len);
    x509_sort_and_prune(&nc->set, BURROW_FN(SlicesCmpFunc, x509_dns_compare, NULL),
                        x509_dns_subset);
    if (!permitted) {
        const Str *set = nc->set.p;
        for (Int i = 0; i < nc->set.len; i++) {
            Str name = strings_to_lower(a, set[i]);
            Str trimmed = x509_trim_first_label(name);
            if (trimmed.len == 0)
                continue;
            if (nc->parents == NULL)
                nc->parents = map_make(a, TYPE_STRING, TYPE_STRING, 0);
            if (nc->parents != NULL)
                map_set(nc->parents, &trimmed, &name);
        }
    }
    return nc;
}

/* dnsConstraints.query: whether s is covered, and the constraint that does. */
static bool x509_dns_query(Alloc *a, const X509DnsConstraints *dnc, Str s,
                           Str *constraint) {
    *constraint = BURROW_STR_EMPTY;
    if (dnc->all)
        return true;
    const Str *found =
        x509_search(dnc->set, &s, BURROW_FN(SlicesCmpFunc, x509_dns_compare, NULL),
                    x509_dns_subset);
    if (found != NULL) {
        *constraint = *found;
        return true;
    }
    if (!dnc->permitted && s.len > 0 && s.p[0] == '*' && dnc->parents != NULL) {
        Str trimmed = x509_trim_first_label(strings_to_lower(a, s));
        const Str *parent = map_get(dnc->parents, &trimmed);
        if (parent != NULL) {
            *constraint = *parent;
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------- emails */

/* rfc2821Mailbox, with the domain in lower case. */
typedef struct X509Mailbox {
    Str local, domain;
} X509Mailbox;

/* The key for a mailbox in a map. A local part never has a NUL in it, so the
 * NUL between the two cannot be confused with anything. */
static Str x509_mailbox_key(Alloc *a, X509Mailbox m) {
    Byte *p = mem_alloc_nozero(a, (size_t)(m.local.len + 1 + m.domain.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, m.local.p, (size_t)m.local.len);
    p[m.local.len] = 0;
    memcpy(p + m.local.len + 1, m.domain.p, (size_t)m.domain.len);
    return str_from_bytes(p, m.local.len + 1 + m.domain.len);
}

/* rfc2821Mailbox.String, from a. */
static Str x509_mailbox_string(Alloc *a, X509Mailbox m) {
    return fmt_sprintf_v(a, "%s@%s", m.local, m.domain);
}

/* emailConstraints */
typedef struct X509EmailConstraints {
    X509DnsConstraints *dns;
    Map *full; /* mailbox key to bool */
} X509EmailConstraints;

/* newEmailConstraints */
static X509EmailConstraints *x509_new_email_constraints(Alloc *a, Slice l,
                                                        bool permitted) {
    if (l.len == 0)
        return NULL;
    X509EmailConstraints *ec = BURROW_NEW(a, X509EmailConstraints);
    if (ec == NULL)
        return NULL;
    ec->full = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    Slice domains = slice_nil(TYPE_STRING);
    const Str *names = l.p;
    for (Int i = 0; i < l.len; i++) {
        Str c = names[i];
        if (memchr(c.p, '@', (size_t)c.len) == NULL) {
            domains = slice_append(a, domains, &c, 1);
            continue;
        }
        X509Mailbox m;
        /* These parsed when the certificate did. Only a certificate changed
         * since then can fail here. */
        if (!burrow__x509_parse_rfc2821_mailbox(a, c, &m.local, &m.domain))
            continue;
        m.domain = strings_to_lower(a, m.domain);
        Str key = x509_mailbox_key(a, m);
        bool t = true;
        if (ec->full != NULL)
            map_set(ec->full, &key, &t);
    }
    if (domains.len > 0)
        ec->dns = x509_new_dns_constraints(a, domains, permitted);
    return ec;
}

/* emailConstraints.query */
static bool x509_email_query(Alloc *a, const X509EmailConstraints *ec, X509Mailbox s,
                             Str *constraint) {
    *constraint = BURROW_STR_EMPTY;
    if (ec->full != NULL && map_len(ec->full) > 0) {
        Str key = x509_mailbox_key(a, s);
        if (map_get(ec->full, &key) != NULL) {
            *constraint = x509_mailbox_string(a, s);
            return true;
        }
    }
    if (ec->dns == NULL)
        return false;
    return x509_dns_query(a, ec->dns, s.domain, constraint);
}

/* ---------------------------------------------------------------- chains */

/* chainConstraints, with each constraints pair spelt out. */
typedef struct X509ChainConstraints {
    X509IPConstraints *ip_permitted, *ip_excluded;
    X509DnsConstraints *dns_permitted, *dns_excluded;
    X509DnsConstraints *uri_permitted, *uri_excluded;
    X509EmailConstraints *email_permitted, *email_excluded;
    Int index;
    struct X509ChainConstraints *next;
} X509ChainConstraints;

/* checkConstraints, once the queries are done. what is the kind of name and
 * p the name as the message shows it. */
static Error x509_constraint_result(const char *what, Str p, bool has_permitted,
                                    bool permitted, bool excluded, Str constraint) {
    if (has_permitted && !permitted)
        return fmt_errorf_v("%s %q is not permitted by any constraint", what, p);
    if (excluded)
        return fmt_errorf_v("%s %q is excluded by constraint %q", what, p, constraint);
    return BURROW_NO_ERROR;
}

/* parsedURI */
typedef struct X509ParsedURI {
    const Url *uri;
    Str domain;
} X509ParsedURI;

/* chainConstraints.check */
static Error x509_chain_constraints_check(Alloc *a, const X509ChainConstraints *cc,
                                          Slice dns, const X509ParsedURI *uris,
                                          Int nuris, const X509Mailbox *emails,
                                          Int nemails, Slice ips) {
    Str constraint;
    const NetIP *ip = ips.p;
    for (Int i = 0; i < ips.len; i++) {
        bool permitted =
            cc->ip_permitted == NULL || x509_ip_query(cc->ip_permitted, ip[i]);
        bool excluded =
            cc->ip_excluded != NULL && x509_ip_query(cc->ip_excluded, ip[i]);
        if (!permitted || excluded) {
            /* The constraint goes in the message as Go's %q prints a
             * *net.IPNet, which is its CIDR form. */
            Str c = BURROW_STR_EMPTY;
            if (excluded) {
                Slice set = ip[i].len == NET_IPV4_LEN ? cc->ip_excluded->ipv4
                                                      : cc->ip_excluded->ipv6;
                NetIPNet *const *found = x509_search(
                    set, &ip[i], BURROW_FN(SlicesCmpFunc, x509_ip_binary_search, NULL),
                    x509_ip_match);
                /* excluded means the query found it, so found is set. */
                if (found != NULL)
                    c = net_ip_net_string(*found, error_allocator());
            }
            return x509_constraint_result(
                "IP address", net_ip_string(ip[i], error_allocator()),
                cc->ip_permitted != NULL, permitted, excluded, c);
        }
    }
    const Str *d = dns.p;
    for (Int i = 0; i < dns.len; i++) {
        if (!burrow__x509_domain_name_valid(d[i], false))
            return fmt_errorf_v("x509: cannot parse dnsName %q", d[i]);
        bool permitted = cc->dns_permitted == NULL ||
                         x509_dns_query(a, cc->dns_permitted, d[i], &constraint);
        bool excluded = cc->dns_excluded != NULL &&
                        x509_dns_query(a, cc->dns_excluded, d[i], &constraint);
        Error err = x509_constraint_result("DNS name", d[i], cc->dns_permitted != NULL,
                                           permitted, excluded, constraint);
        if (BURROW_FAILED(err))
            return err;
    }
    for (Int i = 0; i < nuris; i++) {
        if (!burrow__x509_domain_name_valid(uris[i].domain, false))
            return fmt_errorf_v("x509: internal error: URI SAN %q failed to parse",
                                url_string(uris[i].uri, error_allocator()));
        bool permitted =
            cc->uri_permitted == NULL ||
            x509_dns_query(a, cc->uri_permitted, uris[i].domain, &constraint);
        bool excluded =
            cc->uri_excluded != NULL &&
            x509_dns_query(a, cc->uri_excluded, uris[i].domain, &constraint);
        if (!permitted || excluded)
            return x509_constraint_result(
                "URI", url_string(uris[i].uri, error_allocator()),
                cc->uri_permitted != NULL, permitted, excluded, constraint);
    }
    for (Int i = 0; i < nemails; i++) {
        if (!burrow__x509_domain_name_valid(emails[i].domain, false))
            return fmt_errorf_v("x509: cannot parse rfc822Name %q",
                                x509_mailbox_string(error_allocator(), emails[i]));
        bool permitted =
            cc->email_permitted == NULL ||
            x509_email_query(a, cc->email_permitted, emails[i], &constraint);
        bool excluded = cc->email_excluded != NULL &&
                        x509_email_query(a, cc->email_excluded, emails[i], &constraint);
        if (!permitted || excluded)
            return x509_constraint_result(
                "email address", x509_mailbox_string(error_allocator(), emails[i]),
                cc->email_permitted != NULL, permitted, excluded, constraint);
    }
    return BURROW_NO_ERROR;
}

/* parseURIs */
static Error x509_parse_uris(Alloc *a, Slice uris, X509ParsedURI **out) {
    *out = BURROW_NEW_N(a, X509ParsedURI, (size_t)(uris.len > 0 ? uris.len : 1));
    if (*out == NULL)
        return burrow_err_out_of_memory;
    Url *const *u = uris.p;
    for (Int i = 0; i < uris.len; i++) {
        Str host = strings_to_lower(a, u[i]->host);
        if (host.len == 0)
            return fmt_errorf_v(
                "URI with empty host (%q) cannot be matched against constraints",
                url_string(u[i], error_allocator()));
        if (memchr(host.p, ':', (size_t)host.len) != NULL &&
            host.p[host.len - 1] != ']') {
            Error err = BURROW_NO_ERROR;
            host = net_split_host_port(u[i]->host, NULL, &err);
            if (BURROW_FAILED(err))
                return fmt_errorf_v("cannot parse URI host %q: %v", u[i]->host, err);
        }
        /* netip_parse_addr turns down the bracketed IPv6 form, so that is
         * looked for as well. */
        Error perr = BURROW_NO_ERROR;
        (void)netip_parse_addr(host, &perr);
        if (!BURROW_FAILED(perr) ||
            (host.len > 0 && host.p[0] == '[' && host.p[host.len - 1] == ']'))
            return fmt_errorf_v(
                "URI with IP (%q) cannot be matched against constraints",
                url_string(u[i], error_allocator()));
        (*out)[i].uri = u[i];
        (*out)[i].domain = host;
    }
    return BURROW_NO_ERROR;
}

/* parseMailboxes */
static Error x509_parse_mailboxes(Alloc *a, Slice emails, X509Mailbox **out) {
    *out = BURROW_NEW_N(a, X509Mailbox, (size_t)(emails.len > 0 ? emails.len : 1));
    if (*out == NULL)
        return burrow_err_out_of_memory;
    const Str *e = emails.p;
    for (Int i = 0; i < emails.len; i++) {
        X509Mailbox m;
        if (!burrow__x509_parse_rfc2821_mailbox(a, e[i], &m.local, &m.domain))
            return fmt_errorf_v("cannot parse rfc822Name %q", e[i]);
        m.domain = strings_to_lower(a, m.domain);
        (*out)[i] = m;
    }
    return BURROW_NO_ERROR;
}

Error burrow__x509_check_chain_constraints(Alloc *a,
                                           const X509Certificate *const *chain, Int n) {
    X509ChainConstraints *current = NULL, *last = NULL;
    for (Int i = 0; i < n; i++) {
        const X509Certificate *c = chain[i];
        if (!burrow__x509_has_name_constraints(c))
            continue;
        X509ChainConstraints *cc = BURROW_NEW(a, X509ChainConstraints);
        if (cc == NULL)
            return burrow_err_out_of_memory;
        cc->ip_permitted = x509_new_ip_constraints(a, c->permitted_ip_ranges);
        cc->ip_excluded = x509_new_ip_constraints(a, c->excluded_ip_ranges);
        cc->dns_permitted = x509_new_dns_constraints(a, c->permitted_dns_domains, true);
        cc->dns_excluded = x509_new_dns_constraints(a, c->excluded_dns_domains, false);
        cc->uri_permitted = x509_new_dns_constraints(a, c->permitted_uri_domains, true);
        cc->uri_excluded = x509_new_dns_constraints(a, c->excluded_uri_domains, false);
        cc->email_permitted =
            x509_new_email_constraints(a, c->permitted_email_addresses, true);
        cc->email_excluded =
            x509_new_email_constraints(a, c->excluded_email_addresses, false);
        cc->index = i;
        if (current == NULL)
            current = cc;
        else
            last->next = cc;
        last = cc;
    }
    if (current == NULL)
        return BURROW_NO_ERROR;

    for (Int i = 0; i < n; i++) {
        const X509Certificate *c = chain[i];
        if (!burrow__x509_has_san_extension(c))
            continue;
        /* A certificate is only held to the constraints of the ones above
         * it, which come later in the chain. */
        if (i >= current->index) {
            while (current->index <= i) {
                if (current->next == NULL)
                    return BURROW_NO_ERROR;
                current = current->next;
            }
        }
        X509ParsedURI *uris;
        Error err = x509_parse_uris(a, c->uris, &uris);
        if (BURROW_FAILED(err))
            return err;
        X509Mailbox *emails;
        err = x509_parse_mailboxes(a, c->email_addresses, &emails);
        if (BURROW_FAILED(err))
            return err;
        for (const X509ChainConstraints *cc = current; cc != NULL; cc = cc->next) {
            err = x509_chain_constraints_check(a, cc, c->dns_names, uris, c->uris.len,
                                               emails, c->email_addresses.len,
                                               c->ip_addresses);
            if (BURROW_FAILED(err))
                return err;
        }
    }
    return BURROW_NO_ERROR;
}
