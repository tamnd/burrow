/* SecTrust, for the systemVerify in Go's crypto/x509/root_darwin.go.
 *
 * This is the part of Go's systemVerify that talks to the Security framework,
 * with the wrappers from its crypto/x509/internal/macos. What Go does with the
 * result is in crypto/x509. burrow links nothing but libc, so Security and
 * CoreFoundation are loaded here, the way src/pal/resolve_posix.c loads
 * libresolv, and the types are opaque pointers so that no framework header is
 * needed to build it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)

#include "burrow/pal.h"

#include "internal.h"

#include <dlfcn.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* CFTypeRef and the rest, which are all pointers to something opaque. */
typedef const void *TrustRef;

typedef TrustRef (*CFArrayCreateMutableFn)(TrustRef, long, const void *);
typedef void (*CFArrayAppendValueFn)(TrustRef, const void *);
typedef long (*CFArrayGetCountFn)(TrustRef);
typedef const void *(*CFArrayGetValueAtIndexFn)(TrustRef, long);
typedef TrustRef (*CFDataCreateFn)(TrustRef, const uint8_t *, long);
typedef long (*CFDataGetLengthFn)(TrustRef);
typedef const uint8_t *(*CFDataGetBytePtrFn)(TrustRef);
typedef TrustRef (*CFStringCreateWithBytesFn)(TrustRef, const uint8_t *, long, uint32_t,
                                              unsigned char);
typedef TrustRef (*CFStringCreateExternalRepresentationFn)(TrustRef, TrustRef, uint32_t,
                                                           uint8_t);
typedef TrustRef (*CFDateCreateFn)(TrustRef, double);
typedef TrustRef (*CFErrorCopyDescriptionFn)(TrustRef);
typedef long (*CFErrorGetCodeFn)(TrustRef);
typedef void (*CFReleaseFn)(TrustRef);
typedef TrustRef (*SecCertificateCreateWithDataFn)(TrustRef, TrustRef);
typedef TrustRef (*SecCertificateCopyDataFn)(TrustRef);
typedef TrustRef (*SecPolicyCreateSSLFn)(unsigned char, TrustRef);
typedef int32_t (*SecTrustCreateWithCertificatesFn)(TrustRef, TrustRef, TrustRef *);
typedef int32_t (*SecTrustSetVerifyDateFn)(TrustRef, TrustRef);
typedef bool (*SecTrustEvaluateWithErrorFn)(TrustRef, TrustRef *);
typedef TrustRef (*SecTrustCopyCertificateChainFn)(TrustRef);

typedef struct TrustApi {
    CFArrayCreateMutableFn array_create;
    CFArrayAppendValueFn array_append;
    CFArrayGetCountFn array_count;
    CFArrayGetValueAtIndexFn array_get;
    CFDataCreateFn data_create;
    CFDataGetLengthFn data_len;
    CFDataGetBytePtrFn data_ptr;
    CFStringCreateWithBytesFn string_create;
    CFStringCreateExternalRepresentationFn string_external;
    CFDateCreateFn date_create;
    CFErrorCopyDescriptionFn error_description;
    CFErrorGetCodeFn error_code;
    CFReleaseFn release;
    SecCertificateCreateWithDataFn cert_create;
    SecCertificateCopyDataFn cert_data;
    SecPolicyCreateSSLFn policy_ssl;
    SecTrustCreateWithCertificatesFn trust_create;
    SecTrustSetVerifyDateFn trust_set_date;
    SecTrustEvaluateWithErrorFn trust_evaluate;
    SecTrustCopyCertificateChainFn trust_chain;
} TrustApi;

/* Go's kCFStringEncodingUTF8. */
#define TRUST_UTF8 0x08000100u

/* The paths Go imports the functions from. iOS frameworks have no Versions
 * directory, so there the library is at the top of the framework. */
#if defined(BURROW_OS_IOS)
#define TRUST_SECURITY "/System/Library/Frameworks/Security.framework/Security"
#define TRUST_CF "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
#else
#define TRUST_SECURITY                                                                 \
    "/System/Library/Frameworks/Security.framework/Versions/A/Security"
#define TRUST_CF                                                                       \
    "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"
#endif

/* dlsym gives a void *, and a function pointer cannot be cast from one in ISO
 * C, so it goes through a union. */
typedef union TrustSym {
    void *object;
    void (*fn)(void);
} TrustSym;

static void (*trust_sym(void *lib, const char *name))(void) {
    TrustSym u;
    u.object = dlsym(lib, name);
    return u.fn;
}

/* Both libraries stay open, since the addresses are only good while they are,
 * and opening one already open costs a reference count. */
static bool trust_api_load(TrustApi *api, PalErrno *err) {
    memset(api, 0, sizeof *api);
    void *cf = dlopen(TRUST_CF, RTLD_LAZY | RTLD_LOCAL);
    void *sec = dlopen(TRUST_SECURITY, RTLD_LAZY | RTLD_LOCAL);
    if (cf == NULL || sec == NULL) {
        BURROW_OUT(err, PAL_ENOSYS);
        return false;
    }
    api->array_create = (CFArrayCreateMutableFn)trust_sym(cf, "CFArrayCreateMutable");
    api->array_append = (CFArrayAppendValueFn)trust_sym(cf, "CFArrayAppendValue");
    api->array_count = (CFArrayGetCountFn)trust_sym(cf, "CFArrayGetCount");
    api->array_get = (CFArrayGetValueAtIndexFn)trust_sym(cf, "CFArrayGetValueAtIndex");
    api->data_create = (CFDataCreateFn)trust_sym(cf, "CFDataCreate");
    api->data_len = (CFDataGetLengthFn)trust_sym(cf, "CFDataGetLength");
    api->data_ptr = (CFDataGetBytePtrFn)trust_sym(cf, "CFDataGetBytePtr");
    api->string_create =
        (CFStringCreateWithBytesFn)trust_sym(cf, "CFStringCreateWithBytes");
    api->string_external = (CFStringCreateExternalRepresentationFn)trust_sym(
        cf, "CFStringCreateExternalRepresentation");
    api->date_create = (CFDateCreateFn)trust_sym(cf, "CFDateCreate");
    api->error_description =
        (CFErrorCopyDescriptionFn)trust_sym(cf, "CFErrorCopyDescription");
    api->error_code = (CFErrorGetCodeFn)trust_sym(cf, "CFErrorGetCode");
    api->release = (CFReleaseFn)trust_sym(cf, "CFRelease");
    api->cert_create =
        (SecCertificateCreateWithDataFn)trust_sym(sec, "SecCertificateCreateWithData");
    api->cert_data = (SecCertificateCopyDataFn)trust_sym(sec, "SecCertificateCopyData");
    api->policy_ssl = (SecPolicyCreateSSLFn)trust_sym(sec, "SecPolicyCreateSSL");
    api->trust_create = (SecTrustCreateWithCertificatesFn)trust_sym(
        sec, "SecTrustCreateWithCertificates");
    api->trust_set_date =
        (SecTrustSetVerifyDateFn)trust_sym(sec, "SecTrustSetVerifyDate");
    api->trust_evaluate =
        (SecTrustEvaluateWithErrorFn)trust_sym(sec, "SecTrustEvaluateWithError");
    api->trust_chain =
        (SecTrustCopyCertificateChainFn)trust_sym(sec, "SecTrustCopyCertificateChain");
    if (api->array_create == NULL || api->array_append == NULL ||
        api->array_count == NULL || api->array_get == NULL ||
        api->data_create == NULL || api->data_len == NULL || api->data_ptr == NULL ||
        api->string_create == NULL || api->string_external == NULL ||
        api->date_create == NULL || api->error_description == NULL ||
        api->error_code == NULL || api->release == NULL || api->cert_create == NULL ||
        api->cert_data == NULL || api->policy_ssl == NULL ||
        api->trust_create == NULL || api->trust_set_date == NULL ||
        api->trust_evaluate == NULL || api->trust_chain == NULL) {
        BURROW_OUT(err, PAL_ENOSYS);
        return false;
    }
    return true;
}

/* CFRelease, which crashes on NULL where Go's wrapper is never handed one. */
static void trust_release(const TrustApi *api, TrustRef ref) {
    if (ref != NULL)
        api->release(ref);
}

/* Go's ReleaseCFArray. The arrays are made without callbacks, so they do not
 * retain what goes in, and what goes in is released here. */
static void trust_release_array(const TrustApi *api, TrustRef array) {
    if (array == NULL)
        return;
    long n = api->array_count(array);
    for (long i = 0; i < n; i++)
        trust_release(api, api->array_get(array, i));
    api->release(array);
}

/* Go's SecCertificateCreateWithData, with BytesToCFData. NULL when the bytes
 * are not a certificate. */
static TrustRef trust_cert(const TrustApi *api, const uint8_t *der, int64_t n) {
    if (n < 0 || n > (int64_t)(LONG_MAX))
        return NULL;
    TrustRef data = api->data_create(NULL, der, (long)n);
    if (data == NULL)
        return NULL;
    TrustRef cert = api->cert_create(NULL, data);
    api->release(data);
    return cert;
}

/* SecTrustEvaluateWithError's error, as Go reads it: the description through
 * CFStringToString, which is empty when the string cannot be made UTF-8, and
 * the code. */
static void trust_eval_failed(const TrustApi *api, TrustRef e, PalTrustFn fn,
                              void *env) {
    PalTrustResult r;
    memset(&r, 0, sizeof r);
    r.failed = PAL_TRUST_EVALUATE;
    r.desc = "";
    TrustRef text = NULL;
    TrustRef data = NULL;
    if (e != NULL) {
        text = api->error_description(e);
        if (text != NULL)
            data = api->string_external(NULL, text, TRUST_UTF8, 0);
        if (data != NULL) {
            r.desc = (const char *)api->data_ptr(data);
            r.desc_len = (int64_t)api->data_len(data);
        }
        r.code = (int64_t)api->error_code(e);
    }
    fn(env, &r);
    trust_release(api, data);
    trust_release(api, text);
    trust_release(api, e);
}

/* The chain SecTrust built, given to fn and then to cert_fn, one certificate
 * at a time, which is Go's loop over exportCertificate. */
static void trust_chain(const TrustApi *api, TrustRef trust, PalTrustFn fn,
                        PalTrustCertFn cert_fn, void *env) {
    PalTrustResult r;
    memset(&r, 0, sizeof r);
    r.desc = "";
    TrustRef chain = api->trust_chain(trust);
    if (chain == NULL) {
        r.failed = PAL_TRUST_CHAIN;
        fn(env, &r);
        return;
    }
    long n = api->array_count(chain);
    r.n = (int64_t)n;
    fn(env, &r);
    for (long i = 0; i < n; i++) {
        TrustRef data = api->cert_data(api->array_get(chain, i));
        if (data == NULL) {
            cert_fn(env, (int64_t)i, NULL, -1);
            continue;
        }
        cert_fn(env, (int64_t)i, api->data_ptr(data), (int64_t)api->data_len(data));
        api->release(data);
    }
    api->release(chain);
}

/* Everything after the certificates are in: the policy, the trust object, the
 * date and the evaluation. */
static void trust_run(const TrustApi *api, const PalTrustOpts *opts, TrustRef certs,
                      PalTrustFn fn, PalTrustCertFn cert_fn, void *env) {
    PalTrustResult r;
    memset(&r, 0, sizeof r);
    r.desc = "";

    TrustRef policies = api->array_create(NULL, 0, NULL);
    TrustRef hostname = NULL;
    if (opts->server_name_len > 0 && opts->server_name_len <= (int64_t)LONG_MAX)
        hostname = api->string_create(NULL, (const uint8_t *)opts->server_name,
                                      (long)opts->server_name_len, TRUST_UTF8, 0);
    TrustRef ssl = api->policy_ssl(1, hostname);
    trust_release(api, hostname);
    if (ssl == NULL || policies == NULL) {
        trust_release(api, ssl);
        trust_release_array(api, policies);
        r.failed = PAL_TRUST_POLICY;
        fn(env, &r);
        return;
    }
    api->array_append(policies, ssl);

    TrustRef trust = NULL;
    int32_t st = api->trust_create(certs, policies, &trust);
    if (st != 0) {
        trust_release_array(api, policies);
        r.failed = PAL_TRUST_CREATE;
        r.status = st;
        fn(env, &r);
        return;
    }

    if (opts->has_date) {
        TrustRef date = api->date_create(NULL, opts->date);
        st = api->trust_set_date(trust, date);
        trust_release(api, date);
        if (st != 0) {
            api->release(trust);
            trust_release_array(api, policies);
            r.failed = PAL_TRUST_DATE;
            r.status = st;
            fn(env, &r);
            return;
        }
    }

    TrustRef e = NULL;
    if (!api->trust_evaluate(trust, &e))
        trust_eval_failed(api, e, fn, env);
    else
        trust_chain(api, trust, fn, cert_fn, env);
    api->release(trust);
    trust_release_array(api, policies);
}

bool pal_trust_evaluate(const PalTrustOpts *opts, PalTrustFn fn, PalTrustCertFn cert_fn,
                        void *env, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (opts == NULL || fn == NULL || cert_fn == NULL || opts->leaf == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    TrustApi api;
    if (!trust_api_load(&api, err))
        return false;

    PalTrustResult r;
    memset(&r, 0, sizeof r);
    r.desc = "";
    TrustRef certs = api.array_create(NULL, 0, NULL);
    if (certs == NULL) {
        BURROW_OUT(err, PAL_ENOMEM);
        return false;
    }
    TrustRef leaf = trust_cert(&api, opts->leaf, opts->leaf_len);
    if (leaf == NULL) {
        api.release(certs);
        r.failed = PAL_TRUST_LEAF;
        fn(env, &r);
        return true;
    }
    api.array_append(certs, leaf);
    for (int64_t i = 0; i < opts->n_intermediates; i++) {
        TrustRef c =
            trust_cert(&api, opts->intermediates[i], opts->intermediate_lens[i]);
        if (c == NULL) {
            trust_release_array(&api, certs);
            r.failed = PAL_TRUST_INTERMEDIATE;
            fn(env, &r);
            return true;
        }
        api.array_append(certs, c);
    }

    trust_run(&api, opts, certs, fn, cert_fn, env);
    trust_release_array(&api, certs);
    return true;
}

#endif /* BURROW_OS_DARWIN || BURROW_OS_IOS */
