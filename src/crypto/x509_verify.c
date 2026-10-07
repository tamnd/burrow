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
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/path/filepath.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

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
        /* The certificate keeps pointing into the DER it was parsed from, so
         * that has to live as long as the pool, and the block does not. A
         * copy that fails is nil and does not parse. */
        Slice der = bytes_clone(s->a, block->bytes);
        pem_block_free(h, block);
        X509Certificate *cert = x509_parse_certificate(s->a, der, &err);
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

bool burrow__x509_cert_pool_is_system(const X509CertPool *s) {
    return s->system_pool;
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

Slice burrow__x509_read_unique_directory_entries(Alloc *a, Str dir, Error *err) {
    Slice files = os_read_dir(a, dir, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_FS_DIR_ENTRY);
    Int n = 0;
    for (Int i = 0; i < files.len; i++) {
        FsDirEntry f = *(FsDirEntry *)slice_at(files, i);
        if (!x509_same_dir_symlink(a, f, dir))
            *(FsDirEntry *)slice_at(files, n++) = f;
    }
    files.len = n;
    return files;
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
        Slice fis = burrow__x509_read_unique_directory_entries(a, d[i], &err);
        if (BURROW_FAILED(err)) {
            if (!BURROW_FAILED(*first_err) && !os_is_not_exist(err))
                *first_err = err;
            continue;
        }
        for (Int j = 0; j < fis.len; j++) {
            FsDirEntry fi = *(FsDirEntry *)slice_at(fis, j);
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

/* loadOnDiskRoots, with a pool from a, and cert_files and cert_directories
 * as the places to look when the environment does not say. A pool comes back
 * unless nothing was found and something failed. */
static X509CertPool *x509_load_on_disk_roots(Alloc *a, Str cert_file, Str cert_dir,
                                             Slice cert_files, Slice cert_directories,
                                             Error *err) {
    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *s = arena_allocator(&scratch);
    X509CertPool *roots = x509_new_cert_pool(a);
    Slice files = cert_file.len > 0
                      ? slice_append(s, slice_nil(TYPE_STRING), &cert_file, 1)
                      : cert_files;
    Slice dirs = cert_dir.len > 0 ? filepath_split_list(s, cert_dir) : cert_directories;
    Error first_err = BURROW_NO_ERROR;
    x509_load_on_disk(roots, s, files, dirs, &first_err);
    arena_free(&scratch);
    if (roots->len > 0 || !BURROW_FAILED(first_err))
        return roots;
    x509_cert_pool_free(roots);
    BURROW_OUT(err, first_err);
    return NULL;
}

X509CertPool *burrow__x509_load_system_roots(Alloc *a, Slice cert_files,
                                             Slice cert_directories, Error *err) {
    Str cert_file = os_getenv(a, BURROW_S("SSL_CERT_FILE"));
    Str cert_dir = os_getenv(a, BURROW_S("SSL_CERT_DIR"));
#if defined(BURROW_OS_WINDOWS) || defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
    if ((cert_file.len == 0 && cert_dir.len == 0) || burrow__x509_no_cert_override()) {
        X509CertPool *p = x509_new_cert_pool(a);
        p->system_pool = true;
        return p;
    }
#endif
    return x509_load_on_disk_roots(a, cert_file, cert_dir, cert_files, cert_directories,
                                   err);
}

/* loadSystemRoots */
static X509CertPool *x509_load_system_roots(Alloc *a, Error *err) {
    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *s = arena_allocator(&scratch);
    X509CertPool *p =
        burrow__x509_load_system_roots(a, x509_cstr_list(s, x509_cert_files),
                                       x509_cstr_list(s, x509_cert_directories), err);
    arena_free(&scratch);
    return p;
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

X509CertPool *burrow__x509_swap_system_roots(X509CertPool *p) {
    sync_once_do(&x509_roots_once, BURROW_FN(Func, x509_init_system_roots, NULL));
    sync_rw_mutex_lock(&x509_roots_mu);
    X509CertPool *old = x509_system_roots;
    x509_system_roots = p;
    sync_rw_mutex_unlock(&x509_roots_mu);
    return old;
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

void burrow__x509_reset_fallbacks(void) {
    sync_rw_mutex_lock(&x509_roots_mu);
    x509_fallbacks_set = false;
    x509_use_fallback_roots = false;
    sync_rw_mutex_unlock(&x509_roots_mu);
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

/* ------------------------------------------------------- verify errors */

#define X509_ERROR_DESC(desc, T, gotype)                                               \
    static const Type desc = {                                                         \
        {(const Byte *)(gotype), (Int)(sizeof(gotype) - 1)},                           \
        {(const Byte *)"crypto/x509", 11},                                             \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* s copied into a, or "" when a says no. */
static Str x509_vclone(Alloc *a, Str s) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Byte *p = mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len);
}

/* Every verify error below is its public struct with the message after it,
 * so errors_as hands back a pointer to the struct. */

/* CertificateInvalidError */

typedef struct X509InvalidBox {
    X509CertificateInvalidError e;
    Str message;
} X509InvalidBox;

X509_ERROR_DESC(x509_invalid_desc, X509CertificateInvalidError,
                "CertificateInvalidError");
const Type *const TYPE_X509_CERTIFICATE_INVALID_ERROR = &x509_invalid_desc;

Str x509_certificate_invalid_error_error(X509CertificateInvalidError e, Alloc *a) {
    const char *msg = "x509: unknown error";
    bool detail = false;
    switch (e.reason) {
    case X509_NOT_AUTHORIZED_TO_SIGN:
        msg = "x509: certificate is not authorized to sign other certificates";
        break;
    case X509_EXPIRED:
        msg = "x509: certificate has expired or is not yet valid: ";
        detail = true;
        break;
    case X509_CA_NOT_AUTHORIZED_FOR_THIS_NAME:
        msg = "x509: a root or intermediate certificate is not authorized to sign for "
              "this name: ";
        detail = true;
        break;
    case X509_CA_NOT_AUTHORIZED_FOR_EXT_KEY_USAGE:
        msg = "x509: a root or intermediate certificate is not authorized for an "
              "extended key usage: ";
        detail = true;
        break;
    case X509_TOO_MANY_INTERMEDIATES:
        msg = "x509: too many intermediates for path length constraint";
        break;
    case X509_INCOMPATIBLE_USAGE:
        msg = "x509: certificate specifies an incompatible key usage";
        break;
    case X509_NAME_MISMATCH:
        msg = "x509: issuer name does not match subject from issuing certificate";
        break;
    case X509_NAME_CONSTRAINTS_WITHOUT_SANS:
        msg = "x509: issuer has name constraints but leaf doesn't have a SAN extension";
        break;
    case X509_UNCONSTRAINED_NAME:
        msg = "x509: issuer has name constraints but leaf contains unknown or "
              "unconstrained name: ";
        detail = true;
        break;
    case X509_NO_VALID_CHAINS:
        if (e.detail.len == 0)
            return x509_vclone(a, BURROW_S("x509: no valid chains built"));
        return fmt_sprintf_v(a, "%s: %s", BURROW_S("x509: no valid chains built"),
                             e.detail);
    default:
        break;
    }
    if (!detail)
        return x509_vclone(a, str_from_cstr(msg));
    return fmt_sprintf_v(a, "%s%s", str_from_cstr(msg), e.detail);
}

static Str x509_invalid_text(const void *self) {
    return ((const X509InvalidBox *)self)->message;
}

static Error x509_invalid_clone(const void *self, Alloc *a) {
    return x509_certificate_invalid_error_as_error(((const X509InvalidBox *)self)->e,
                                                   a);
}

static const ErrorVT x509_invalid_vt = {
    .self_type = &x509_invalid_desc,
    .message = x509_invalid_text,
    .clone = x509_invalid_clone,
};

Error x509_certificate_invalid_error_as_error(X509CertificateInvalidError e, Alloc *a) {
    X509InvalidBox *b = BURROW_NEW(a, X509InvalidBox);
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e = e;
    b->e.detail = x509_vclone(a, e.detail);
    b->message = x509_certificate_invalid_error_error(e, a);
    return (Error){&x509_invalid_vt, b};
}

/* The CertificateInvalidError Verify gives, from the error allocator. */
static Error x509_invalid(const X509Certificate *c, X509InvalidReason reason,
                          Str detail) {
    X509CertificateInvalidError e = {c, reason, detail};
    return x509_certificate_invalid_error_as_error(e, error_allocator());
}

/* HostnameError */

typedef struct X509HostnameBox {
    X509HostnameError e;
    Str message;
} X509HostnameBox;

X509_ERROR_DESC(x509_hostname_desc, X509HostnameError, "HostnameError");
const Type *const TYPE_X509_HOSTNAME_ERROR = &x509_hostname_desc;

static Byte x509_fold(Byte c) {
    return 'A' <= c && c <= 'Z' ? (Byte)(c + 'a' - 'A') : c;
}

static bool x509_fold_equal(Str a, Str b) {
    if (a.len != b.len)
        return false;
    for (Int i = 0; i < a.len; i++)
        if (x509_fold(a.p[i]) != x509_fold(b.p[i]))
            return false;
    return true;
}

/* The label of s up to the next period, and s after that period. *more says
 * whether there was one, which is how strings.Split tells "a" from "a.". */
static Str x509_next_label(Str *s, bool *more) {
    const Byte *dot = memchr(s->p, '.', (size_t)s->len);
    if (dot == NULL) {
        Str label = *s;
        *s = BURROW_STR_EMPTY;
        *more = false;
        return label;
    }
    Str label = str_from_bytes(s->p, dot - s->p);
    *s = str_from_bytes(dot + 1, s->len - (dot - s->p) - 1);
    *more = true;
    return label;
}

/* splitHostname: host without one trailing period. Its labels are what
 * matchHostnames compares, in lower case. */
static Str x509_split_hostname(Str host) {
    if (host.len > 0 && host.p[host.len - 1] == '.')
        host.len--;
    return host;
}

/* matchHostnames(pattern, splitHostname(host)), with host already through
 * x509_split_hostname. Only ASCII letters fold, as toLowerCaseASCII does. */
static bool x509_match_hostnames(Str pattern, Str host) {
    if (pattern.len == 0)
        return false;
    bool pmore = true, hmore = true;
    for (Int i = 0; pmore || hmore; i++) {
        if (!pmore || !hmore)
            return false;
        Str p = x509_next_label(&pattern, &pmore);
        Str h = x509_next_label(&host, &hmore);
        if (i == 0 && p.len == 1 && p.p[0] == '*')
            continue;
        if (!x509_fold_equal(p, h))
            return false;
    }
    return true;
}

/* validHostname */
static bool x509_valid_hostname(Str host, bool is_pattern) {
    if (!is_pattern && host.len > 0 && host.p[host.len - 1] == '.')
        host.len--;
    if (host.len == 0)
        return false;
    if (host.len == 1 && host.p[0] == '*')
        return false;
    bool more = true;
    for (Int i = 0; more; i++) {
        Str part = x509_next_label(&host, &more);
        if (part.len == 0)
            return false;
        if (is_pattern && i == 0 && part.len == 1 && part.p[0] == '*')
            continue;
        for (Int j = 0; j < part.len; j++) {
            Byte c = part.p[j];
            if (('a' <= c && c <= 'z') || ('0' <= c && c <= '9') ||
                ('A' <= c && c <= 'Z') || (c == '-' && j != 0) || c == '_')
                continue;
            return false;
        }
    }
    return true;
}

bool burrow__x509_valid_hostname_pattern(Str host) {
    return x509_valid_hostname(host, true);
}

bool burrow__x509_valid_hostname_input(Str host) {
    return x509_valid_hostname(host, false);
}

/* matchExactly */
static bool x509_match_exactly(Str a, Str b) {
    if (a.len == 0 || (a.len == 1 && a.p[0] == '.') || b.len == 0 ||
        (b.len == 1 && b.p[0] == '.'))
        return false;
    return x509_fold_equal(a, b);
}

/* The names in names, joined with ", ". */
static Str x509_join(Alloc *a, Slice names) {
    return strings_join(a, names, BURROW_S(", "));
}

Str x509_hostname_error_error(X509HostnameError h, Alloc *a) {
    const X509Certificate *c = h.certificate;
    const Int max_names_included = 100;
    if (!burrow__x509_has_san_extension(c) &&
        x509_match_hostnames(c->subject.common_name, x509_split_hostname(h.host)))
        return x509_vclone(
            a,
            BURROW_S("x509: certificate relies on legacy Common Name field, use SANs "
                     "instead"));
    Str valid;
    if (net_parse_ip(a, h.host).len != 0) {
        if (c->ip_addresses.len == 0)
            return fmt_sprintf_v(a,
                                 "x509: cannot validate certificate for %s because it "
                                 "doesn't contain any IP SANs",
                                 h.host);
        if (c->ip_addresses.len >= max_names_included)
            return fmt_sprintf_v(
                a, "x509: certificate is valid for %d IP SANs, but none matched %s",
                c->ip_addresses.len, h.host);
        Slice ips =
            slice_make(a, TYPE_STRING, c->ip_addresses.len, c->ip_addresses.len);
        if (ips.p == NULL)
            return BURROW_STR_EMPTY;
        const NetIP *ip = c->ip_addresses.p;
        Str *out = ips.p;
        for (Int i = 0; i < c->ip_addresses.len; i++)
            out[i] = net_ip_string(ip[i], a);
        valid = x509_join(a, ips);
    } else {
        if (c->dns_names.len >= max_names_included)
            return fmt_sprintf_v(
                a, "x509: certificate is valid for %d names, but none matched %s",
                c->dns_names.len, h.host);
        valid = x509_join(a, c->dns_names);
    }
    if (valid.len == 0)
        return fmt_sprintf_v(
            a, "x509: certificate is not valid for any names, but wanted to match %s",
            h.host);
    return fmt_sprintf_v(a, "x509: certificate is valid for %s, not %s", valid, h.host);
}

static Str x509_hostname_text(const void *self) {
    return ((const X509HostnameBox *)self)->message;
}

static Error x509_hostname_clone(const void *self, Alloc *a) {
    return x509_hostname_error_as_error(((const X509HostnameBox *)self)->e, a);
}

static const ErrorVT x509_hostname_vt = {
    .self_type = &x509_hostname_desc,
    .message = x509_hostname_text,
    .clone = x509_hostname_clone,
};

Error x509_hostname_error_as_error(X509HostnameError e, Alloc *a) {
    X509HostnameBox *b = BURROW_NEW(a, X509HostnameBox);
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e = e;
    b->e.host = x509_vclone(a, e.host);
    b->message = x509_hostname_error_error(e, a);
    return (Error){&x509_hostname_vt, b};
}

/* UnknownAuthorityError */

typedef struct X509UnknownBox {
    X509UnknownAuthorityError e;
    Str message;
} X509UnknownBox;

X509_ERROR_DESC(x509_unknown_desc, X509UnknownAuthorityError, "UnknownAuthorityError");
const Type *const TYPE_X509_UNKNOWN_AUTHORITY_ERROR = &x509_unknown_desc;

/* UnknownAuthorityError.Error with Go's hintErr and hintCert. */
static Str x509_unknown_message(Alloc *a, Error hint_err,
                                const X509Certificate *hint_cert) {
    Str s = BURROW_S("x509: certificate signed by unknown authority");
    if (!BURROW_FAILED(hint_err))
        return x509_vclone(a, s);
    Str name = hint_cert->subject.common_name;
    if (name.len == 0) {
        if (hint_cert->subject.organization.len > 0)
            name = ((const Str *)hint_cert->subject.organization.p)[0];
        else
            name = fmt_sprintf_v(a, "serial:%s",
                                 big_int_string(hint_cert->serial_number, a));
    }
    return fmt_sprintf_v(a,
                         "%s (possibly because of %q while trying to verify candidate "
                         "authority certificate %q)",
                         s, error_text(hint_err), name);
}

Str burrow__x509_unknown_authority_message(Alloc *a, Error hint_err,
                                           const X509Certificate *hint_cert) {
    return x509_unknown_message(a, hint_err, hint_cert);
}

static Str x509_unknown_text(const void *self) {
    return ((const X509UnknownBox *)self)->message;
}

static Error x509_unknown_new(const X509Certificate *cert, Str message, Alloc *a);

static Error x509_unknown_clone(const void *self, Alloc *a) {
    const X509UnknownBox *b = self;
    return x509_unknown_new(b->e.cert, b->message, a);
}

static const ErrorVT x509_unknown_vt = {
    .self_type = &x509_unknown_desc,
    .message = x509_unknown_text,
    .clone = x509_unknown_clone,
};

static Error x509_unknown_new(const X509Certificate *cert, Str message, Alloc *a) {
    X509UnknownBox *b = BURROW_NEW(a, X509UnknownBox);
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e.cert = cert;
    b->message = x509_vclone(a, message);
    return (Error){&x509_unknown_vt, b};
}

Str x509_unknown_authority_error_error(X509UnknownAuthorityError e, Alloc *a) {
    (void)e;
    return x509_unknown_message(a, BURROW_NO_ERROR, NULL);
}

Error x509_unknown_authority_error_as_error(X509UnknownAuthorityError e, Alloc *a) {
    X509UnknownBox *b = BURROW_NEW(a, X509UnknownBox);
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e = e;
    b->message = x509_unknown_message(a, BURROW_NO_ERROR, NULL);
    return (Error){&x509_unknown_vt, b};
}

/* SystemRootsError */

typedef struct X509SystemRootsBox {
    X509SystemRootsError e;
    Str message;
} X509SystemRootsBox;

X509_ERROR_DESC(x509_sysroots_desc, X509SystemRootsError, "SystemRootsError");
const Type *const TYPE_X509_SYSTEM_ROOTS_ERROR = &x509_sysroots_desc;

Str x509_system_roots_error_error(X509SystemRootsError e, Alloc *a) {
    Str msg = BURROW_S("x509: failed to load system roots and no roots provided");
    if (BURROW_FAILED(e.err))
        return fmt_sprintf_v(a, "%s; %s", msg, error_text(e.err));
    return x509_vclone(a, msg);
}

Error x509_system_roots_error_unwrap(X509SystemRootsError e) {
    return e.err;
}

static Str x509_sysroots_text(const void *self) {
    return ((const X509SystemRootsBox *)self)->message;
}

static Error x509_sysroots_unwrap(const void *self) {
    return ((const X509SystemRootsBox *)self)->e.err;
}

static Error x509_sysroots_clone(const void *self, Alloc *a) {
    X509SystemRootsError e = ((const X509SystemRootsBox *)self)->e;
    if (BURROW_FAILED(e.err))
        e.err = error_retain(a, e.err);
    return x509_system_roots_error_as_error(e, a);
}

static const ErrorVT x509_sysroots_vt = {
    .self_type = &x509_sysroots_desc,
    .message = x509_sysroots_text,
    .unwrap = x509_sysroots_unwrap,
    .clone = x509_sysroots_clone,
};

Error x509_system_roots_error_as_error(X509SystemRootsError e, Alloc *a) {
    X509SystemRootsBox *b = BURROW_NEW(a, X509SystemRootsBox);
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->e = e;
    b->message = x509_system_roots_error_error(e, a);
    return (Error){&x509_sysroots_vt, b};
}

/* ---------------------------------------------------------- host names */

Error x509_certificate_verify_hostname(const X509Certificate *c, Str h) {
    /* An IP address, which may be in brackets. */
    Str candidate_ip = h;
    if (h.len >= 3 && h.p[0] == '[' && h.p[h.len - 1] == ']')
        candidate_ip = str_from_bytes(h.p + 1, h.len - 2);
    Error perr = BURROW_NO_ERROR;
    NetipAddr addr = netip_parse_addr(candidate_ip, &perr);
    if (!BURROW_FAILED(perr)) {
        NetIP ip = netip_addr_as_slice(addr, error_allocator());
        const NetIP *have = c->ip_addresses.p;
        for (Int i = 0; i < c->ip_addresses.len; i++)
            if (net_ip_equal(ip, have[i]))
                return BURROW_NO_ERROR;
        X509HostnameError e = {c, net_ip_string(ip, error_allocator())};
        return x509_hostname_error_as_error(e, error_allocator());
    }

    bool valid_candidate = x509_valid_hostname(h, false);
    Str host = x509_split_hostname(h);
    const Str *names = c->dns_names.p;
    for (Int i = 0; i < c->dns_names.len; i++) {
        /* Wildcards only for names that are valid host names, so that
         * nothing odd is matched with one. */
        if (valid_candidate && x509_valid_hostname(names[i], true)) {
            if (x509_match_hostnames(names[i], host))
                return BURROW_NO_ERROR;
        } else if (x509_match_exactly(names[i], h)) {
            return BURROW_NO_ERROR;
        }
    }
    X509HostnameError e = {c, h};
    return x509_hostname_error_as_error(e, error_allocator());
}

/* --------------------------------------------------------------- chains */

BURROW_SLICE_TYPE_DEFINE(X509CertificateChain, X509CertificatePtr);

BURROW_SENTINEL_ERROR(burrow__x509_err_not_parsed,
                      "x509: missing ASN.1 contents; use ParseCertificate");
BURROW_SENTINEL_ERROR(burrow__x509_err_signature_limit,
                      "x509: signature check attempts limit reached while verifying "
                      "certificate chain");

enum {
    X509_LEAF_CERTIFICATE,
    X509_INTERMEDIATE_CERTIFICATE,
    X509_ROOT_CERTIFICATE,
};

/* maxChainSignatureChecks: how many signatures one Verify will check before
 * it gives up on a pool built to make it work too hard. */
#define X509_MAX_CHAIN_SIGNATURE_CHECKS 100

static bool x509_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* isValid: whether c can go on the end of chain as a certificate of
 * cert_type. */
static Error x509_is_valid(const X509Certificate *c, int cert_type, Slice chain,
                           const X509VerifyOptions *opts) {
    if (c->unhandled_critical_extensions.len > 0)
        return x509_unhandled_critical_extension;
    if (chain.len > 0) {
        const X509Certificate *child =
            ((X509Certificate *const *)chain.p)[chain.len - 1];
        if (!bytes_equal(child->raw_issuer, c->raw_subject))
            return x509_invalid(c, X509_NAME_MISMATCH, BURROW_STR_EMPTY);
    }
    Time now = opts->current_time;
    if (time_is_zero(now))
        now = time_now();
    if (time_before(now, c->not_before)) {
        Alloc *ea = error_allocator();
        return x509_invalid(
            c, X509_EXPIRED,
            fmt_sprintf_v(ea, "current time %s is before %s",
                          time_format(now, ea, TIME_RFC3339),
                          time_format(c->not_before, ea, TIME_RFC3339)));
    }
    if (time_after(now, c->not_after)) {
        Alloc *ea = error_allocator();
        return x509_invalid(c, X509_EXPIRED,
                            fmt_sprintf_v(ea, "current time %s is after %s",
                                          time_format(now, ea, TIME_RFC3339),
                                          time_format(c->not_after, ea, TIME_RFC3339)));
    }
    if ((cert_type == X509_INTERMEDIATE_CERTIFICATE ||
         cert_type == X509_ROOT_CERTIFICATE) &&
        chain.len == 0)
        return errors_new(error_allocator(),
                          BURROW_S("x509: internal error: empty chain when appending "
                                   "CA cert"));
    if (cert_type == X509_INTERMEDIATE_CERTIFICATE &&
        (!c->basic_constraints_valid || !c->is_ca))
        return x509_invalid(c, X509_NOT_AUTHORIZED_TO_SIGN, BURROW_STR_EMPTY);
    if (c->basic_constraints_valid && c->max_path_len >= 0) {
        Int num_intermediates = chain.len - 1;
        if (num_intermediates > c->max_path_len)
            return x509_invalid(c, X509_TOO_MANY_INTERMEDIATES, BURROW_STR_EMPTY);
    }
    return BURROW_NO_ERROR;
}

/* appendToFreshChain */
static Slice x509_append_to_fresh_chain(Alloc *a, Slice chain,
                                        const X509Certificate *c) {
    Slice n = slice_make(a, TYPE_X509_CERTIFICATE_PTR, 0, chain.len + 1);
    n = slice_append(a, n, chain.p, chain.len);
    X509Certificate *p = (X509Certificate *)(uintptr_t)c;
    return slice_append(a, n, &p, 1);
}

/* alreadyInChain: whether chain has a certificate with the same subject, key
 * and SANs as candidate. Those would only make the chain longer and the
 * search slower, which is the point of a pool built to do that. */
static bool x509_already_in_chain(const X509Certificate *candidate, Slice chain) {
    const PkixExtension *candidate_san = burrow__x509_san_extension(candidate);
    X509Certificate *const *certs = chain.p;
    for (Int i = 0; i < chain.len; i++) {
        const X509Certificate *cert = certs[i];
        if (!bytes_equal(candidate->raw_subject, cert->raw_subject))
            continue;
        if (!bytes_equal(candidate->raw_subject_public_key_info,
                         cert->raw_subject_public_key_info))
            continue;
        const PkixExtension *cert_san = burrow__x509_san_extension(cert);
        if (candidate_san == NULL && cert_san == NULL)
            return true;
        if (candidate_san == NULL || cert_san == NULL)
            return false;
        if (bytes_equal(candidate_san->value, cert_san->value))
            return true;
    }
    return false;
}

/* buildChains. Go's version assigns isValid's error to its result and only
 * clears it when a chain is found, so a failing candidate's error can be what
 * comes back instead of an UnknownAuthorityError. That is kept. */
static Error x509_build_chains(Alloc *a, const X509Certificate *c, Slice current,
                               Int *sig_checks, const X509VerifyOptions *opts,
                               Slice *chains) {
    Error err = BURROW_NO_ERROR;
    Error hint_err = BURROW_NO_ERROR;
    const X509Certificate *hint_cert = NULL;
    *chains = slice_nil(TYPE_X509_CERTIFICATE_CHAIN);

    const struct {
        int cert_type;
        const X509CertPool *pool;
    } kinds[2] = {
        {X509_ROOT_CERTIFICATE, opts->roots},
        {X509_INTERMEDIATE_CERTIFICATE, opts->intermediates},
    };
    for (int k = 0; k < 2; k++) {
        Int np = 0;
        X509PotentialParent *parents =
            burrow__x509_cert_pool_find_potential_parents(kinds[k].pool, c, a, &np);
        for (Int j = 0; j < np; j++) {
            const X509Certificate *cand = parents[j].cert;
            if (++*sig_checks > X509_MAX_CHAIN_SIGNATURE_CHECKS) {
                err = burrow__x509_err_signature_limit;
                goto done;
            }
            if (cand->public_key.t == NULL || x509_already_in_chain(cand, current))
                continue;
            Error e = x509_certificate_check_signature_from(c, cand);
            if (BURROW_FAILED(e)) {
                if (!BURROW_FAILED(hint_err)) {
                    hint_err = e;
                    hint_cert = cand;
                }
                continue;
            }
            err = x509_is_valid(cand, kinds[k].cert_type, current, opts);
            if (BURROW_FAILED(err)) {
                if (!BURROW_FAILED(hint_err)) {
                    hint_err = err;
                    hint_cert = cand;
                }
                continue;
            }
            if (!BURROW_FUNC_IS_NIL(parents[j].constraint)) {
                e = BURROW_CALLF(parents[j].constraint, current);
                if (BURROW_FAILED(e)) {
                    if (!BURROW_FAILED(hint_err)) {
                        hint_err = e;
                        hint_cert = cand;
                    }
                    continue;
                }
            }
            Slice fresh = x509_append_to_fresh_chain(a, current, cand);
            if (kinds[k].cert_type == X509_ROOT_CERTIFICATE) {
                *chains = slice_append(a, *chains, &fresh, 1);
            } else {
                Slice child;
                err = x509_build_chains(a, cand, fresh, sig_checks, opts, &child);
                *chains = slice_append_slice(a, *chains, child);
            }
            if (x509_same_error(err, burrow__x509_err_signature_limit))
                goto done;
        }
    }
done:
    if (chains->len > 0)
        err = BURROW_NO_ERROR;
    if (chains->len == 0 && !BURROW_FAILED(err)) {
        Alloc *ea = error_allocator();
        err = x509_unknown_new(c, x509_unknown_message(ea, hint_err, hint_cert), ea);
    }
    return err;
}

/* checkChainForKeyUsage: whether every certificate in chain that limits its
 * extended key usages allows one of usages, the same one all the way up. */
static bool x509_check_chain_for_key_usage(Alloc *a, Slice chain, Slice key_usages) {
    if (chain.len == 0)
        return false;
    Int n = key_usages.len;
    X509ExtKeyUsage *usages = BURROW_NEW_N(a, X509ExtKeyUsage, (size_t)(n > 0 ? n : 1));
    if (usages == NULL)
        return false;
    memcpy(usages, key_usages.p, (size_t)n * sizeof(X509ExtKeyUsage));
    Int remaining = n;
    const X509ExtKeyUsage invalid_usage = -1;
    X509Certificate *const *certs = chain.p;
    for (Int i = chain.len - 1; i >= 0; i--) {
        const X509Certificate *cert = certs[i];
        if (cert->ext_key_usage.len == 0 && cert->unknown_ext_key_usage.len == 0)
            /* No extended key usages means any. */
            continue;
        const X509ExtKeyUsage *have = cert->ext_key_usage.p;
        bool any = false;
        for (Int j = 0; j < cert->ext_key_usage.len; j++)
            if (have[j] == X509_EXT_KEY_USAGE_ANY)
                any = true;
        if (any)
            continue;
        for (Int k = 0; k < n; k++) {
            if (usages[k] == invalid_usage)
                continue;
            bool found = false;
            for (Int j = 0; j < cert->ext_key_usage.len && !found; j++)
                found = usages[k] == have[j];
            if (found)
                continue;
            usages[k] = invalid_usage;
            if (--remaining == 0)
                return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------- policies */

/* The policy graph of RFC 9618, which replaces RFC 5280's policy tree and
 * cannot blow up the way the tree can. */

typedef struct X509PolicyNode {
    X509OID valid_policy;
    Slice expected_policy_set; /* X509OID */
    Map *parents;              /* X509PolicyNode * to bool */
    Map *children;
} X509PolicyNode;

BURROW_SLICE_TYPE(X509PolicyNodes, UnsafePointer);
BURROW_SLICE_TYPE(X509PolicyOIDs, X509OID);

/* anyPolicy, 2.5.29.32.0. */
static const Byte x509_any_policy_der[] = {0x55, 0x1D, 0x20, 0x00};

static X509OID x509_any_policy(void) {
    return (X509OID){slice_from((void *)(uintptr_t)x509_any_policy_der,
                                (Int)sizeof x509_any_policy_der,
                                (Int)sizeof x509_any_policy_der, TYPE_BYTE)};
}

static Str x509_oid_key(X509OID oid) {
    return str_from_bytes(oid.der.p, oid.der.len);
}

static bool x509_is_any_policy(X509OID oid) {
    return oid.der.len == (Int)sizeof x509_any_policy_der &&
           memcmp(oid.der.p, x509_any_policy_der, sizeof x509_any_policy_der) == 0;
}

typedef struct X509PolicyGraph {
    Alloc *a;
    Map **strata;      /* Str of the DER to X509PolicyNode * */
    Map *parent_index; /* Str of the DER to X509PolicyNodes */
    Int depth;
} X509PolicyGraph;

/* newPolicyGraphNode */
static X509PolicyNode *x509_new_policy_node(Alloc *a, X509OID valid, Slice parents) {
    X509PolicyNode *n = BURROW_NEW(a, X509PolicyNode);
    if (n == NULL)
        return NULL;
    n->valid_policy = valid;
    n->expected_policy_set = slice_append(a, slice_nil(TYPE_X509_OID), &valid, 1);
    n->children = map_make(a, TYPE_UNSAFE_POINTER, TYPE_BOOL, 0);
    n->parents = map_make(a, TYPE_UNSAFE_POINTER, TYPE_BOOL, 0);
    const bool t = true;
    X509PolicyNode *const *ps = parents.p;
    for (Int i = 0; i < parents.len; i++) {
        X509PolicyNode *p = ps[i];
        map_set(p->children, &n, &t);
        map_set(n->parents, &p, &t);
    }
    return n;
}

static X509PolicyNode *x509_policy_lookup(Map *m, X509OID oid) {
    Str key = x509_oid_key(oid);
    X509PolicyNode **n = map_get(m, &key);
    return n == NULL ? NULL : *n;
}

/* newPolicyGraph, with room for depth levels below the root. */
static bool x509_new_policy_graph(X509PolicyGraph *pg, Alloc *a, Int levels) {
    pg->a = a;
    pg->depth = 0;
    pg->parent_index = NULL;
    pg->strata = BURROW_NEW_N(a, Map *, (size_t)(levels + 1));
    if (pg->strata == NULL)
        return false;
    pg->strata[0] = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    X509PolicyNode *root =
        x509_new_policy_node(a, x509_any_policy(), slice_nil(TYPE_UNSAFE_POINTER));
    if (pg->strata[0] == NULL || root == NULL)
        return false;
    Str key = x509_oid_key(root->valid_policy);
    map_set(pg->strata[0], &key, &root);
    return true;
}

static void x509_policy_insert(X509PolicyGraph *pg, X509PolicyNode *n) {
    Str key = x509_oid_key(n->valid_policy);
    map_set(pg->strata[pg->depth], &key, &n);
}

static Slice x509_policy_parents_with_expected(X509PolicyGraph *pg, X509OID expected) {
    if (pg->depth == 0)
        return slice_nil(TYPE_UNSAFE_POINTER);
    Str key = x509_oid_key(expected);
    Slice *s = map_get(pg->parent_index, &key);
    return s == NULL ? slice_nil(TYPE_UNSAFE_POINTER) : *s;
}

static X509PolicyNode *x509_policy_parent_with_any_policy(X509PolicyGraph *pg) {
    if (pg->depth == 0)
        return NULL;
    return x509_policy_lookup(pg->strata[pg->depth - 1], x509_any_policy());
}

static void x509_policy_delete_leaf(X509PolicyGraph *pg, X509OID policy) {
    X509PolicyNode *n = x509_policy_lookup(pg->strata[pg->depth], policy);
    if (n == NULL)
        return;
    const void *k;
    void *v;
    MapIter it = map_iter(n->parents);
    while (map_next(&it, &k, &v))
        map_del((*(X509PolicyNode *const *)k)->children, &n);
    it = map_iter(n->children);
    while (map_next(&it, &k, &v))
        map_del((*(X509PolicyNode *const *)k)->parents, &n);
    Str key = x509_oid_key(policy);
    map_del(pg->strata[pg->depth], &key);
}

/* validPolicyNodes: the nodes whose only parent is anyPolicy. */
static Slice x509_policy_valid_nodes(X509PolicyGraph *pg) {
    Slice valid = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = pg->depth; i >= 0; i--) {
        MapIter it = map_iter(pg->strata[i]);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            X509PolicyNode *n = *(X509PolicyNode **)v;
            if (x509_is_any_policy(n->valid_policy))
                continue;
            if (map_len(n->parents) != 1)
                continue;
            MapIter pi = map_iter(n->parents);
            const void *pk;
            void *pv;
            if (map_next(&pi, &pk, &pv) &&
                x509_is_any_policy((*(X509PolicyNode *const *)pk)->valid_policy))
                valid = slice_append(pg->a, valid, &n, 1);
        }
    }
    return valid;
}

/* prune: the nodes above the leaves with no children left, from the bottom
 * up, so that taking one away can leave its parent bare in turn. */
static void x509_policy_prune(X509PolicyGraph *pg) {
    for (Int i = pg->depth - 1; i > 0; i--) {
        MapIter it = map_iter(pg->strata[i]);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            X509PolicyNode *n = *(X509PolicyNode **)v;
            if (map_len(n->children) != 0)
                continue;
            MapIter pi = map_iter(n->parents);
            const void *pk;
            void *pv;
            while (map_next(&pi, &pk, &pv))
                map_del((*(X509PolicyNode *const *)pk)->children, &n);
            Str key = x509_oid_key(n->valid_policy);
            map_del(pg->strata[i], &key);
        }
    }
}

/* Appends n to the list under key in m, a map of Str to X509PolicyNodes. */
static void x509_policy_index_add(Alloc *a, Map *m, Str key, X509PolicyNode *n) {
    Slice *have = map_get(m, &key);
    Slice s = have == NULL ? slice_nil(TYPE_UNSAFE_POINTER) : *have;
    s = slice_append(a, s, &n, 1);
    map_set(m, &key, &s);
}

static void x509_policy_incr_depth(X509PolicyGraph *pg) {
    pg->parent_index = map_make(pg->a, TYPE_STRING, TYPE_OF(X509PolicyNodes), 0);
    MapIter it = map_iter(pg->strata[pg->depth]);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        X509PolicyNode *n = *(X509PolicyNode **)v;
        const X509OID *e = n->expected_policy_set.p;
        for (Int i = 0; i < n->expected_policy_set.len; i++)
            x509_policy_index_add(pg->a, pg->parent_index, x509_oid_key(e[i]), n);
    }
    pg->depth++;
    pg->strata[pg->depth] = map_make(pg->a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
}

/* policiesValid: RFC 5280 section 6.1's policy processing over chain, leaf
 * first, done with the policy graph. */
static bool x509_policies_valid(Alloc *a, Slice chain, const X509VerifyOptions *opts) {
    /* A chain that is only a root has no policies to check. */
    if (chain.len == 1)
        return true;
    X509Certificate *const *certs = chain.p;
    Int n = chain.len - 1;

    X509PolicyGraph graph;
    if (!x509_new_policy_graph(&graph, a, n))
        return false;
    X509PolicyGraph *pg = &graph;
    Int inhibit_any_policy = 0, explicit_policy = 0, policy_mapping = 0;
    if (!opts->inhibit_any_policy)
        inhibit_any_policy = n + 1;
    if (!opts->require_explicit_policy)
        explicit_policy = n + 1;
    if (!opts->inhibit_policy_mapping)
        policy_mapping = n + 1;

    const bool t = true;
    Map *initial_user_policy_set = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    const X509OID *user = opts->certificate_policies.p;
    for (Int i = 0; i < opts->certificate_policies.len; i++) {
        Str key = x509_oid_key(user[i]);
        map_set(initial_user_policy_set, &key, &t);
    }
    Str any_key = x509_oid_key(x509_any_policy());
    if (map_len(initial_user_policy_set) == 0)
        map_set(initial_user_policy_set, &any_key, &t);

    for (Int i = n - 1; i >= 0; i--) {
        const X509Certificate *cert = certs[i];
        bool is_self_signed = bytes_equal(cert->raw_issuer, cert->raw_subject);

        if (cert->policies.len == 0)
            pg = NULL;
        if (explicit_policy == 0 && pg == NULL)
            return false;

        if (pg != NULL) {
            x509_policy_incr_depth(pg);
            bool has_any = false;
            const X509OID *policies = cert->policies.p;
            for (Int j = 0; j < cert->policies.len; j++) {
                X509OID policy = policies[j];
                if (x509_is_any_policy(policy)) {
                    has_any = true;
                    continue;
                }
                Slice parents = x509_policy_parents_with_expected(pg, policy);
                if (parents.len == 0) {
                    X509PolicyNode *any_parent = x509_policy_parent_with_any_policy(pg);
                    if (any_parent != NULL)
                        parents = slice_append(a, slice_nil(TYPE_UNSAFE_POINTER),
                                               &any_parent, 1);
                }
                if (parents.len > 0)
                    x509_policy_insert(pg, x509_new_policy_node(a, policy, parents));
            }

            if (has_any && (inhibit_any_policy > 0 || (n - i < n && is_self_signed))) {
                Map *missing = map_make(a, TYPE_STRING, TYPE_OF(X509PolicyNodes), 0);
                Map *leaves = pg->strata[pg->depth];
                if (pg->depth > 0) {
                    MapIter it = map_iter(pg->strata[pg->depth - 1]);
                    const void *k;
                    void *v;
                    while (map_next(&it, &k, &v)) {
                        X509PolicyNode *p = *(X509PolicyNode **)v;
                        const X509OID *e = p->expected_policy_set.p;
                        for (Int j = 0; j < p->expected_policy_set.len; j++)
                            if (x509_policy_lookup(leaves, e[j]) == NULL)
                                x509_policy_index_add(a, missing, x509_oid_key(e[j]),
                                                      p);
                    }
                }
                MapIter it = map_iter(missing);
                const void *k;
                void *v;
                while (map_next(&it, &k, &v)) {
                    Str oid = *(const Str *)k;
                    X509OID valid = {slice_from((void *)(uintptr_t)oid.p, oid.len,
                                                oid.len, TYPE_BYTE)};
                    x509_policy_insert(pg, x509_new_policy_node(a, valid, *(Slice *)v));
                }
            }

            x509_policy_prune(pg);

            if (i != 0 && cert->policy_mappings.len > 0) {
                /* The issuer domain policy to the subject domain policies it
                 * maps to. */
                Map *mappings = map_make(a, TYPE_STRING, TYPE_OF(X509PolicyOIDs), 0);
                const X509PolicyMapping *m = cert->policy_mappings.p;
                for (Int j = 0; j < cert->policy_mappings.len; j++) {
                    if (policy_mapping > 0) {
                        if (x509_is_any_policy(m[j].issuer_domain_policy) ||
                            x509_is_any_policy(m[j].subject_domain_policy))
                            /* Mapping anyPolicy is not allowed. */
                            return false;
                        Str key = x509_oid_key(m[j].issuer_domain_policy);
                        Slice *have = map_get(mappings, &key);
                        Slice s = have == NULL ? slice_nil(TYPE_X509_OID) : *have;
                        s = slice_append(a, s, &m[j].subject_domain_policy, 1);
                        map_set(mappings, &key, &s);
                    } else {
                        /* Mapping is inhibited, so the mapped policies go. */
                        x509_policy_delete_leaf(pg, m[j].issuer_domain_policy);
                    }
                }

                x509_policy_prune(pg);

                MapIter it = map_iter(mappings);
                const void *k;
                void *v;
                while (map_next(&it, &k, &v)) {
                    Str issuer = *(const Str *)k;
                    X509OID issuer_oid = {slice_from((void *)(uintptr_t)issuer.p,
                                                     issuer.len, issuer.len,
                                                     TYPE_BYTE)};
                    X509PolicyNode *matching =
                        x509_policy_lookup(pg->strata[pg->depth], issuer_oid);
                    if (matching != NULL) {
                        matching->expected_policy_set = *(Slice *)v;
                        continue;
                    }
                    matching =
                        x509_policy_lookup(pg->strata[pg->depth], x509_any_policy());
                    if (matching != NULL) {
                        Slice one = slice_append(a, slice_nil(TYPE_UNSAFE_POINTER),
                                                 &matching, 1);
                        X509PolicyNode *nn = x509_new_policy_node(a, issuer_oid, one);
                        nn->expected_policy_set = *(Slice *)v;
                        x509_policy_insert(pg, nn);
                    }
                }
            }
        }

        if (i != 0) {
            if (!is_self_signed) {
                if (explicit_policy > 0)
                    explicit_policy--;
                if (policy_mapping > 0)
                    policy_mapping--;
                if (inhibit_any_policy > 0)
                    inhibit_any_policy--;
            }
            if ((cert->require_explicit_policy > 0 ||
                 cert->require_explicit_policy_zero) &&
                cert->require_explicit_policy < explicit_policy)
                explicit_policy = cert->require_explicit_policy;
            if ((cert->inhibit_policy_mapping > 0 ||
                 cert->inhibit_policy_mapping_zero) &&
                cert->inhibit_policy_mapping < policy_mapping)
                policy_mapping = cert->inhibit_policy_mapping;
            if ((cert->inhibit_any_policy > 0 || cert->inhibit_any_policy_zero) &&
                cert->inhibit_any_policy < inhibit_any_policy)
                inhibit_any_policy = cert->inhibit_any_policy;
        }
    }

    if (explicit_policy > 0)
        explicit_policy--;
    if (certs[0]->require_explicit_policy_zero)
        explicit_policy = 0;

    Slice valid_nodes = slice_nil(TYPE_UNSAFE_POINTER);
    if (pg != NULL) {
        valid_nodes = x509_policy_valid_nodes(pg);
        X509PolicyNode *current_any =
            x509_policy_lookup(pg->strata[pg->depth], x509_any_policy());
        if (current_any != NULL)
            valid_nodes = slice_append(a, valid_nodes, &current_any, 1);
    }

    Map *authority = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    X509PolicyNode *const *vn = valid_nodes.p;
    for (Int i = 0; i < valid_nodes.len; i++) {
        Str key = x509_oid_key(vn[i]->valid_policy);
        map_set(authority, &key, &t);
    }
    Map *user_constrained = map_clone(a, authority);
    if (map_len(initial_user_policy_set) != 1 ||
        map_get(initial_user_policy_set, &any_key) == NULL) {
        MapIter it = map_iter(user_constrained);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            Str key = *(const Str *)k;
            if (map_get(initial_user_policy_set, &key) == NULL)
                map_del(user_constrained, &key);
        }
        if (map_get(authority, &any_key) != NULL) {
            it = map_iter(initial_user_policy_set);
            while (map_next(&it, &k, &v))
                map_set(user_constrained, k, &t);
        }
    }
    if (explicit_policy == 0 && map_len(user_constrained) == 0)
        return false;
    return true;
}

/* --------------------------------------------------------------- Verify */

/* The chains in from, copied into a. */
static Slice x509_copy_chains(Alloc *a, Slice from, Error *err) {
    Slice out = slice_make(a, TYPE_X509_CERTIFICATE_CHAIN, from.len, from.len);
    if (out.p == NULL && from.len > 0) {
        *err = burrow_err_out_of_memory;
        return slice_nil(TYPE_X509_CERTIFICATE_CHAIN);
    }
    const Slice *in = from.p;
    Slice *to = out.p;
    for (Int i = 0; i < from.len; i++) {
        to[i] = slice_make(a, TYPE_X509_CERTIFICATE_PTR, in[i].len, in[i].len);
        if (to[i].p == NULL && in[i].len > 0) {
            *err = burrow_err_out_of_memory;
            return slice_nil(TYPE_X509_CERTIFICATE_CHAIN);
        }
        if (in[i].len > 0)
            memcpy(to[i].p, in[i].p, (size_t)in[i].len * sizeof(X509Certificate *));
    }
    return out;
}

/* Verify, with scratch memory from s. */
Error burrow__x509_build_chains(Alloc *a, const X509Certificate *c, Slice current,
                                const X509VerifyOptions *opts, Slice *chains) {
    Int sig_checks = 0;
    return x509_build_chains(a, c, current, &sig_checks, opts, chains);
}

bool burrow__x509_policies_valid(Alloc *a, Slice chain, const X509VerifyOptions *opts) {
    return x509_policies_valid(a, chain, opts);
}

static Slice x509_verify(Alloc *s, const X509Certificate *c, X509VerifyOptions *opts,
                         Alloc *a, Error *err) {
    Slice none = slice_nil(TYPE_X509_CERTIFICATE_CHAIN);
    *err = x509_is_valid(c, X509_LEAF_CERTIFICATE, slice_nil(TYPE_X509_CERTIFICATE_PTR),
                         opts);
    if (BURROW_FAILED(*err))
        return none;
    if (opts->dns_name.len > 0) {
        *err = x509_certificate_verify_hostname(c, opts->dns_name);
        if (BURROW_FAILED(*err))
            return none;
    }

    Slice candidates;
    X509Certificate *leaf = (X509Certificate *)(uintptr_t)c;
    Slice first = slice_append(s, slice_nil(TYPE_X509_CERTIFICATE_PTR), &leaf, 1);
    if (burrow__x509_cert_pool_contains(opts->roots, c)) {
        candidates = slice_append(s, slice_nil(TYPE_X509_CERTIFICATE_CHAIN), &first, 1);
    } else {
        Int sig_checks = 0;
        *err = x509_build_chains(s, c, first, &sig_checks, opts, &candidates);
        if (BURROW_FAILED(*err))
            return none;
    }

    bool any_key_usage = false;
    const X509ExtKeyUsage *eku = opts->key_usages.p;
    for (Int i = 0; i < opts->key_usages.len; i++)
        if (eku[i] == X509_EXT_KEY_USAGE_ANY)
            any_key_usage = true;
    X509ExtKeyUsage server_auth = X509_EXT_KEY_USAGE_SERVER_AUTH;
    if (opts->key_usages.len == 0)
        opts->key_usages = slice_append(s, slice_nil(TYPE_INT), &server_auth, 1);

    Int invalid_policies_chains = 0, incompatible_key_usage_chains = 0;
    Error constraints_hint_err = BURROW_NO_ERROR;
    Slice *chains = candidates.p;
    Int kept = 0;
    for (Int i = 0; i < candidates.len; i++) {
        Slice chain = chains[i];
        if (!x509_policies_valid(s, chain, opts)) {
            invalid_policies_chains++;
            continue;
        }
        if (!any_key_usage &&
            !x509_check_chain_for_key_usage(s, chain, opts->key_usages)) {
            incompatible_key_usage_chains++;
            continue;
        }
        Error e = burrow__x509_check_chain_constraints(
            s, (const X509Certificate *const *)chain.p, chain.len);
        if (BURROW_FAILED(e)) {
            if (!BURROW_FAILED(constraints_hint_err))
                constraints_hint_err = x509_invalid(
                    c, X509_CA_NOT_AUTHORIZED_FOR_THIS_NAME, error_text(e));
            continue;
        }
        chains[kept++] = chain;
    }
    candidates.len = kept;

    if (kept == 0) {
        if (BURROW_FAILED(constraints_hint_err)) {
            /* What Verify gave before the other checks moved here. */
            *err = constraints_hint_err;
            return none;
        }
        Slice details = slice_nil(TYPE_STRING);
        if (incompatible_key_usage_chains > 0) {
            if (invalid_policies_chains == 0) {
                *err = x509_invalid(c, X509_INCOMPATIBLE_USAGE, BURROW_STR_EMPTY);
                return none;
            }
            Str d = fmt_sprintf_v(s, "%d candidate chains with incompatible key usage",
                                  incompatible_key_usage_chains);
            details = slice_append(s, details, &d, 1);
        }
        if (invalid_policies_chains > 0) {
            Str d = fmt_sprintf_v(s, "%d candidate chains with invalid policies",
                                  invalid_policies_chains);
            details = slice_append(s, details, &d, 1);
        }
        *err = x509_invalid(c, X509_NO_VALID_CHAINS,
                            strings_join(s, details, BURROW_S(", ")));
        return none;
    }
    return x509_copy_chains(a, candidates, err);
}

Slice x509_certificate_verify(const X509Certificate *c, Alloc *a,
                              X509VerifyOptions opts, Error *err) {
    Slice none = slice_nil(TYPE_X509_CERTIFICATE_CHAIN);
    *err = BURROW_NO_ERROR;
    if (c->raw.len == 0) {
        *err = burrow__x509_err_not_parsed;
        return none;
    }
    for (Int i = 0; i < burrow__x509_cert_pool_len(opts.intermediates); i++) {
        if (burrow__x509_cert_pool_cert(opts.intermediates, i, NULL)->raw.len == 0) {
            *err = burrow__x509_err_not_parsed;
            return none;
        }
    }

    /* On Windows, macOS and iOS Go hands a verify with no roots to the
     * platform. Until that is here the system pool stands in, and on those
     * it is empty unless SSL_CERT_FILE or SSL_CERT_DIR says otherwise. */
    if (opts.roots == NULL) {
        opts.roots = burrow__x509_system_roots_pool();
        if (opts.roots == NULL) {
            sync_rw_mutex_r_lock(&x509_roots_mu);
            X509SystemRootsError e = {x509_system_roots_err};
            sync_rw_mutex_r_unlock(&x509_roots_mu);
            *err = x509_system_roots_error_as_error(e, error_allocator());
            return none;
        }
    }

    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Slice chains = x509_verify(arena_allocator(&scratch), c, &opts, a, err);
    arena_free(&scratch);
    return chains;
}
