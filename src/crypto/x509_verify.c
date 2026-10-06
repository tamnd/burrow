/* crypto/x509: certificate pools, the system roots and chain verification,
 * from Go's cert_pool.go, root.go and verify.go.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/path/filepath.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include "x509_internal.h"

#include <string.h>

/* ------------------------------------------------------------- CertPool */

/* lazyCert, without the laziness: cert is parsed once and kept. next is the
 * index of the next certificate with the same raw subject, or -1. */
typedef struct X509PoolCert {
    Str raw_subject;
    Byte sum[28];
    const X509Certificate *cert;
    X509CertConstraint constraint;
    Int next;
} X509PoolCert;

struct X509CertPool {
    Alloc *parent;
    Arena arena;
    Alloc *a;
    /* cert.RawSubject as a string to the index of the first certificate with
     * it. */
    Map *by_name;
    /* The SHA-224 of each certificate's DER as a string, to true. */
    Map *have_sum;
    X509PoolCert *certs;
    Int len;
    Int cap;
    bool system_pool;
};

static void x509_pool_init(X509CertPool *p, Alloc *a) {
    memset(p, 0, sizeof *p);
    p->parent = a;
    arena_init(&p->arena, a, 0);
    p->a = arena_allocator(&p->arena);
    p->by_name = map_make(p->a, TYPE_STRING, TYPE_INT, 0);
    p->have_sum = map_make(p->a, TYPE_STRING, TYPE_BOOL, 0);
    if (p->by_name == NULL || p->have_sum == NULL)
        panic_str(BURROW_S("x509: out of memory"));
}

X509CertPool *x509_new_cert_pool(Alloc *a) {
    X509CertPool *p = mem_alloc_nozero(a, sizeof *p, _Alignof(X509CertPool));
    if (p == NULL)
        panic_str(BURROW_S("x509: out of memory"));
    x509_pool_init(p, a);
    return p;
}

void x509_cert_pool_free(X509CertPool *s) {
    if (s == NULL)
        return;
    Alloc *a = s->parent;
    arena_free(&s->arena);
    mem_free(a, s, sizeof *s, _Alignof(X509CertPool));
}

Int burrow__x509_cert_pool_len(const X509CertPool *s) {
    return s == NULL ? 0 : s->len;
}

const X509Certificate *burrow__x509_cert_pool_cert(const X509CertPool *s, Int n,
                                                   X509CertConstraint *constraint) {
    if (constraint != NULL)
        *constraint = s->certs[n].constraint;
    return s->certs[n].cert;
}

static Str x509_sum_key(const Byte *sum) {
    return str_from_bytes(sum, 28);
}

/* addCertFunc, with the certificate already in hand. */
static void x509_pool_add(X509CertPool *s, const Byte sum[28], Str raw_subject,
                          const X509Certificate *cert, X509CertConstraint constraint) {
    if (cert == NULL)
        panic_str(BURROW_S("getCert can't be nil"));
    Str sum_key = x509_sum_key(sum);
    if (map_get(s->have_sum, &sum_key) != NULL)
        return;
    if (s->len == s->cap) {
        Int cap = s->cap == 0 ? 8 : s->cap * 2;
        X509PoolCert *certs =
            mem_alloc_nozero(s->a, (size_t)cap * sizeof *certs, _Alignof(X509PoolCert));
        if (certs == NULL)
            panic_str(BURROW_S("x509: out of memory"));
        if (s->len > 0)
            memcpy(certs, s->certs, (size_t)s->len * sizeof *certs);
        s->certs = certs;
        s->cap = cap;
    }
    X509PoolCert *lc = &s->certs[s->len];
    memcpy(lc->sum, sum, 28);
    lc->raw_subject = str_clone(s->a, raw_subject);
    lc->cert = cert;
    lc->constraint = constraint;
    lc->next = -1;
    Int index = s->len++;

    /* The key points into the entry's own copy, which lives as long as the
     * pool does. */
    sum_key = x509_sum_key(lc->sum);
    bool yes = true;
    map_set(s->have_sum, &sum_key, &yes);
    Int *first = map_get(s->by_name, &lc->raw_subject);
    if (first == NULL) {
        map_set(s->by_name, &lc->raw_subject, &index);
        return;
    }
    Int i = *first;
    while (s->certs[i].next >= 0)
        i = s->certs[i].next;
    s->certs[i].next = index;
}

static void x509_pool_add_cert(X509CertPool *s, const X509Certificate *cert,
                               X509CertConstraint constraint) {
    if (cert == NULL)
        panic_str(BURROW_S("adding nil Certificate to CertPool"));
    Sha256Sum224Ret sum = sha256_sum224(cert->raw);
    x509_pool_add(s, sum.a, str_from_bytes(cert->raw_subject.p, cert->raw_subject.len),
                  cert, constraint);
}

void x509_cert_pool_add_cert(X509CertPool *s, const X509Certificate *cert) {
    x509_pool_add_cert(s, cert, (X509CertConstraint){0});
}

void x509_cert_pool_add_cert_with_constraint(X509CertPool *s,
                                             const X509Certificate *cert,
                                             X509CertConstraint constraint) {
    x509_pool_add_cert(s, cert, constraint);
}

X509CertPool *x509_cert_pool_clone(const X509CertPool *s, Alloc *a) {
    X509CertPool *p = x509_new_cert_pool(a);
    p->system_pool = s->system_pool;
    for (Int i = 0; i < s->len; i++)
        x509_pool_add(p, s->certs[i].sum, s->certs[i].raw_subject, s->certs[i].cert,
                      s->certs[i].constraint);
    return p;
}

bool x509_cert_pool_append_certs_from_pem(X509CertPool *s, Slice pem_certs) {
    bool ok = false;
    Alloc *h = heap_allocator();
    while (pem_certs.len > 0) {
        Slice rest;
        PemBlock *block = pem_decode(h, pem_certs, &rest);
        if (block == NULL)
            break;
        pem_certs = rest;
        if (!str_eq(block->type, BURROW_S("CERTIFICATE")) ||
            map_len(block->headers) != 0) {
            pem_block_free(h, block);
            continue;
        }
        ArenaMark m = error_mark();
        Error err = BURROW_NO_ERROR;
        X509Certificate *cert = x509_parse_certificate(s->a, block->bytes, &err);
        pem_block_free(h, block);
        error_release(m);
        if (BURROW_FAILED(err))
            continue;
        x509_pool_add_cert(s, cert, (X509CertConstraint){0});
        ok = true;
    }
    return ok;
}

Slice x509_cert_pool_subjects(const X509CertPool *s, Alloc *a) {
    Int n = burrow__x509_cert_pool_len(s);
    Slice res = slice_make(a, TYPE_BYTES, n, n);
    if (res.p == NULL && n > 0)
        panic_str(BURROW_S("x509: out of memory"));
    Slice *out = res.p;
    for (Int i = 0; i < n; i++) {
        Str subject = s->certs[i].raw_subject;
        out[i] =
            (Slice){(void *)(uintptr_t)subject.p, subject.len, subject.len, TYPE_BYTE};
    }
    return res;
}

bool x509_cert_pool_equal(const X509CertPool *s, const X509CertPool *other) {
    if (s == NULL || other == NULL)
        return s == other;
    if (s->system_pool != other->system_pool || s->len != other->len)
        return false;
    for (Int i = 0; i < s->len; i++) {
        Str key = x509_sum_key(s->certs[i].sum);
        if (map_get(other->have_sum, &key) == NULL)
            return false;
    }
    return true;
}

bool burrow__x509_cert_pool_contains(const X509CertPool *s,
                                     const X509Certificate *cert) {
    if (s == NULL)
        return false;
    Sha256Sum224Ret sum = sha256_sum224(cert->raw);
    Str key = x509_sum_key(sum.a);
    return map_get(s->have_sum, &key) != NULL;
}

X509PotentialParent *burrow__x509_cert_pool_find_potential_parents(
    const X509CertPool *s, const X509Certificate *cert, Alloc *a, Int *n) {
    *n = 0;
    if (s == NULL)
        return NULL;
    Str issuer = str_from_bytes(cert->raw_issuer.p, cert->raw_issuer.len);
    const Int *first = map_get(s->by_name, &issuer);
    if (first == NULL)
        return NULL;
    Int found = 0;
    for (Int i = *first; i >= 0; i = s->certs[i].next)
        found++;
    X509PotentialParent *out =
        mem_alloc_nozero(a, (size_t)found * sizeof *out, _Alignof(X509PotentialParent));
    if (out == NULL)
        panic_str(BURROW_S("x509: out of memory"));
    /* matchingKeyID, then oneKeyID, then mismatchKeyID, each in the order the
     * pool has them. */
    for (int pass = 0; pass < 3; pass++) {
        for (Int i = *first; i >= 0; i = s->certs[i].next) {
            const X509Certificate *candidate = s->certs[i].cert;
            Int ski = candidate->subject_key_id.len;
            Int aki = cert->authority_key_id.len;
            int kind;
            if (bytes_equal(candidate->subject_key_id, cert->authority_key_id))
                kind = 0;
            else if ((ski == 0 && aki > 0) || (ski > 0 && aki == 0))
                kind = 1;
            else
                kind = 2;
            if (kind == pass)
                out[(*n)++] = (X509PotentialParent){candidate, s->certs[i].constraint};
        }
    }
    return out;
}

void burrow__x509_cert_pool_set_system(X509CertPool *s) {
    s->system_pool = true;
}

/* ---------------------------------------------------------- system roots */

#if defined(BURROW_OS_LINUX)
static const char *const x509_cert_files[] = {
    "/etc/ssl/certs/ca-certificates.crt",                /* Debian/Ubuntu/Gentoo etc. */
    "/etc/pki/tls/certs/ca-bundle.crt",                  /* Fedora/RHEL 6 */
    "/etc/ssl/ca-bundle.pem",                            /* OpenSUSE */
    "/etc/pki/tls/cacert.pem",                           /* OpenELEC */
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* CentOS/RHEL 7 */
    "/etc/ssl/cert.pem",                                 /* Alpine Linux */
    NULL,
};

static const char *const x509_cert_directories[] = {
    "/etc/ssl/certs",     /* SLES10/SLES11, https://golang.org/issue/12139 */
    "/etc/pki/tls/certs", /* Fedora/RHEL */
#if defined(BURROW_OS_ANDROID)
    "/system/etc/security/cacerts",    /* Android system roots */
    "/data/misc/keychain/certs-added", /* User trusted CA folder */
#endif
    NULL,
};
#elif defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_OPENBSD) ||                      \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_DRAGONFLY)
static const char *const x509_cert_files[] = {
    "/usr/local/etc/ssl/cert.pem",            /* FreeBSD */
    "/etc/ssl/cert.pem",                      /* OpenBSD */
    "/usr/local/share/certs/ca-root-nss.crt", /* DragonFly */
    "/etc/openssl/certs/ca-certificates.crt", /* NetBSD */
    NULL,
};

static const char *const x509_cert_directories[] = {
    "/etc/ssl/certs",         /* FreeBSD 12.2+ */
    "/usr/local/share/certs", /* FreeBSD */
    "/etc/openssl/certs",     /* NetBSD */
    NULL,
};
#else
/* Windows and macOS ask the platform to verify, and only read files when
 * SSL_CERT_FILE or SSL_CERT_DIR says to. */
static const char *const x509_cert_files[] = {NULL};
static const char *const x509_cert_directories[] = {NULL};
#endif

/* isSameDirSymlink: whether entry f of dir is a link to a name in dir. */
static bool x509_same_dir_symlink(Alloc *a, FsDirEntry f, Str dir) {
    if ((f.vt->type(f.data) & FS_MODE_SYMLINK) == 0)
        return false;
    Error err = BURROW_NO_ERROR;
    Str target = os_readlink(a, filepath_join_v(a, 2, dir, f.vt->name(f.data)), &err);
    return !BURROW_FAILED(err) && !strings_contains_rune(target, FILEPATH_SEPARATOR);
}

/* Reads every file named in files until one can be read, then every file in
 * dirs, into roots. The first error that is not a missing file goes to
 * *first_err. */
static void x509_load_on_disk(X509CertPool *roots, Alloc *a, Slice files, Slice dirs,
                              Error *first_err) {
    const Str *f = files.p;
    for (Int i = 0; i < files.len; i++) {
        Error err = BURROW_NO_ERROR;
        Slice data = os_read_file(a, f[i], &err);
        if (!BURROW_FAILED(err)) {
            x509_cert_pool_append_certs_from_pem(roots, data);
            break;
        }
        if (!BURROW_FAILED(*first_err) && !os_is_not_exist(err))
            *first_err = err;
    }
    const Str *d = dirs.p;
    for (Int i = 0; i < dirs.len; i++) {
        Error err = BURROW_NO_ERROR;
        Slice fis = os_read_dir(a, d[i], &err);
        if (BURROW_FAILED(err)) {
            if (!BURROW_FAILED(*first_err) && !os_is_not_exist(err))
                *first_err = err;
            continue;
        }
        for (Int j = 0; j < fis.len; j++) {
            FsDirEntry fi = *(FsDirEntry *)slice_at(fis, j);
            if (x509_same_dir_symlink(a, fi, d[i]))
                continue;
            Error rerr = BURROW_NO_ERROR;
            Slice data = os_read_file(
                a, filepath_join_v(a, 2, d[i], fi.vt->name(fi.data)), &rerr);
            if (!BURROW_FAILED(rerr))
                x509_cert_pool_append_certs_from_pem(roots, data);
        }
    }
}

static Slice x509_cstr_list(Alloc *a, const char *const *list) {
    Slice s = slice_nil(TYPE_STRING);
    for (; *list != NULL; list++) {
        Str x = str_from_cstr(*list);
        s = slice_append(a, s, &x, 1);
    }
    return s;
}

/* loadOnDiskRoots, with a pool from a. A pool comes back unless nothing was
 * found and something failed. */
static X509CertPool *x509_load_on_disk_roots(Alloc *a, Str cert_file, Str cert_dir,
                                             Error *err) {
    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *s = arena_allocator(&scratch);
    X509CertPool *roots = x509_new_cert_pool(a);
    Slice files = cert_file.len > 0
                      ? slice_append(s, slice_nil(TYPE_STRING), &cert_file, 1)
                      : x509_cstr_list(s, x509_cert_files);
    Slice dirs = cert_dir.len > 0 ? filepath_split_list(s, cert_dir)
                                  : x509_cstr_list(s, x509_cert_directories);
    Error first_err = BURROW_NO_ERROR;
    x509_load_on_disk(roots, s, files, dirs, &first_err);
    arena_free(&scratch);
    if (roots->len > 0 || !BURROW_FAILED(first_err))
        return roots;
    x509_cert_pool_free(roots);
    BURROW_OUT(err, first_err);
    return NULL;
}

/* loadSystemRoots */
static X509CertPool *x509_load_system_roots(Alloc *a, Error *err) {
    Str cert_file = os_getenv(a, BURROW_S("SSL_CERT_FILE"));
    Str cert_dir = os_getenv(a, BURROW_S("SSL_CERT_DIR"));
#if defined(BURROW_OS_WINDOWS) || defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
    if ((cert_file.len == 0 && cert_dir.len == 0) || burrow__x509_no_cert_override()) {
        X509CertPool *p = x509_new_cert_pool(a);
        p->system_pool = true;
        return p;
    }
#endif
    return x509_load_on_disk_roots(a, cert_file, cert_dir, err);
}

static SyncOnce x509_roots_once;
static SyncRWMutex x509_roots_mu;
static X509CertPool *x509_system_roots;
static Error x509_system_roots_err;
static bool x509_fallbacks_set;
static bool x509_use_fallback_roots;

/* Whether p has roots in it or stands for the platform's. */
static bool x509_system_certs_avail(const X509CertPool *p) {
    return p != NULL && (p->len > 0 || p->system_pool);
}

/* initSystemRoots. The pool lives as long as the program does. */
static void x509_init_system_roots(void *env) {
    (void)env;
    sync_rw_mutex_lock(&x509_roots_mu);
    X509CertPool *fallback_roots = x509_system_roots;
    Error err = BURROW_NO_ERROR;
    x509_system_roots = x509_load_system_roots(heap_allocator(), &err);
    x509_system_roots_err =
        BURROW_FAILED(err) ? error_retain(heap_allocator(), err) : err;
    if (BURROW_FAILED(err))
        x509_system_roots = NULL;
    if (fallback_roots != NULL &&
        (x509_use_fallback_roots || !x509_system_certs_avail(x509_system_roots))) {
        x509_system_roots = fallback_roots;
        x509_system_roots_err = BURROW_NO_ERROR;
    }
    sync_rw_mutex_unlock(&x509_roots_mu);
}

/* systemRootsPool */
X509CertPool *burrow__x509_system_roots_pool(void) {
    sync_once_do(&x509_roots_once, BURROW_FN(Func, x509_init_system_roots, NULL));
    sync_rw_mutex_r_lock(&x509_roots_mu);
    X509CertPool *p = x509_system_roots;
    sync_rw_mutex_r_unlock(&x509_roots_mu);
    return p;
}

X509CertPool *x509_system_cert_pool(Alloc *a, Error *err) {
    X509CertPool *sys_roots = burrow__x509_system_roots_pool();
    if (sys_roots != NULL)
        return x509_cert_pool_clone(sys_roots, a);
    return x509_load_system_roots(a, err);
}

static void x509_unreachable(void *env) {
    (void)env;
    panic_str(BURROW_S("unreachable"));
}

void x509_set_fallback_roots(X509CertPool *roots) {
    if (roots == NULL)
        panic_str(BURROW_S("roots must be non-nil"));
    sync_rw_mutex_lock(&x509_roots_mu);
    if (x509_fallbacks_set) {
        sync_rw_mutex_unlock(&x509_roots_mu);
        panic_str(BURROW_S("SetFallbackRoots has already been called"));
    }
    x509_fallbacks_set = true;
    if (x509_system_roots == NULL && !BURROW_FAILED(x509_system_roots_err)) {
        x509_system_roots = roots;
        x509_use_fallback_roots = burrow__x509_use_fallback_roots();
        sync_rw_mutex_unlock(&x509_roots_mu);
        return;
    }
    /* Asserts that the system roots were indeed loaded before. */
    sync_once_do(&x509_roots_once, BURROW_FN(Func, x509_unreachable, NULL));
    if (burrow__x509_use_fallback_roots() ||
        !x509_system_certs_avail(x509_system_roots)) {
        x509_system_roots = roots;
        x509_system_roots_err = BURROW_NO_ERROR;
    }
    sync_rw_mutex_unlock(&x509_roots_mu);
}
