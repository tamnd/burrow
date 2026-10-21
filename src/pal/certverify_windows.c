/* Certificate chains from CryptoAPI, for the systemVerify in Go's
 * crypto/x509/root_windows.go.
 *
 * This is Go's createStoreContext and the CertGetCertificateChain call of its
 * systemVerify, and the CertVerifyCertificateChainPolicy call of its
 * checkChainSSLServerPolicy. Everything Go does with what comes back is in
 * crypto/x509, which gets each chain context through fn. The functions are in
 * crypt32, and burrow links nothing but kernel32, so it is loaded here, which
 * is what src/pal/random_windows.c says to do in this case.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <windows.h>

/* windows.h leaves it out under WIN32_LEAN_AND_MEAN, which another file of the
 * amalgamation may have set first. */
#include <wincrypt.h>

#include <string.h>

typedef PCCERT_CONTEXT(WINAPI *CertCreateCertificateContextFn)(DWORD, const BYTE *,
                                                               DWORD);
typedef BOOL(WINAPI *CertFreeCertificateContextFn)(PCCERT_CONTEXT);
typedef HCERTSTORE(WINAPI *CertOpenStoreFn)(LPCSTR, DWORD, HCRYPTPROV_LEGACY, DWORD,
                                            const void *);
typedef BOOL(WINAPI *CertCloseStoreFn)(HCERTSTORE, DWORD);
typedef BOOL(WINAPI *CertAddCertificateContextToStoreFn)(HCERTSTORE, PCCERT_CONTEXT,
                                                         DWORD, PCCERT_CONTEXT *);
typedef BOOL(WINAPI *CertGetCertificateChainFn)(HCERTCHAINENGINE, PCCERT_CONTEXT,
                                                LPFILETIME, HCERTSTORE,
                                                PCERT_CHAIN_PARA, DWORD, LPVOID,
                                                PCCERT_CHAIN_CONTEXT *);
typedef VOID(WINAPI *CertFreeCertificateChainFn)(PCCERT_CHAIN_CONTEXT);
typedef BOOL(WINAPI *CertVerifyCertificateChainPolicyFn)(LPCSTR, PCCERT_CHAIN_CONTEXT,
                                                         PCERT_CHAIN_POLICY_PARA,
                                                         PCERT_CHAIN_POLICY_STATUS);

typedef struct CertApi {
    HMODULE lib;
    CertCreateCertificateContextFn create_context;
    CertFreeCertificateContextFn free_context;
    CertOpenStoreFn open_store;
    CertCloseStoreFn close_store;
    CertAddCertificateContextToStoreFn add_context;
    CertGetCertificateChainFn get_chain;
    CertFreeCertificateChainFn free_chain;
    CertVerifyCertificateChainPolicyFn verify_policy;
} CertApi;

/* Go's syscall.X509_ASN_ENCODING | syscall.PKCS_7_ASN_ENCODING. */
#define CERT_ENCODING (X509_ASN_ENCODING | PKCS_7_ASN_ENCODING)

/* The default is to return only the highest quality chain, setting this flag
 * will add additional lower quality contexts. These are returned in the
 * LowerQualityChains field. */
#define CERT_RETURN_LOWER_QUALITY 0x00000080

static bool cert_api_load(CertApi *api, PalErrno *err) {
    memset(api, 0, sizeof *api);
    api->lib = LoadLibraryW(L"crypt32.dll");
    if (api->lib == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }
    api->create_context = (CertCreateCertificateContextFn)(void (*)(
        void))GetProcAddress(api->lib, "CertCreateCertificateContext");
    api->free_context = (CertFreeCertificateContextFn)(void (*)(void))GetProcAddress(
        api->lib, "CertFreeCertificateContext");
    api->open_store =
        (CertOpenStoreFn)(void (*)(void))GetProcAddress(api->lib, "CertOpenStore");
    api->close_store =
        (CertCloseStoreFn)(void (*)(void))GetProcAddress(api->lib, "CertCloseStore");
    api->add_context = (CertAddCertificateContextToStoreFn)(void (*)(
        void))GetProcAddress(api->lib, "CertAddCertificateContextToStore");
    api->get_chain = (CertGetCertificateChainFn)(void (*)(void))GetProcAddress(
        api->lib, "CertGetCertificateChain");
    api->free_chain = (CertFreeCertificateChainFn)(void (*)(void))GetProcAddress(
        api->lib, "CertFreeCertificateChain");
    api->verify_policy = (CertVerifyCertificateChainPolicyFn)(void (*)(
        void))GetProcAddress(api->lib, "CertVerifyCertificateChainPolicy");
    if (api->create_context == NULL || api->free_context == NULL ||
        api->open_store == NULL || api->close_store == NULL ||
        api->add_context == NULL || api->get_chain == NULL || api->free_chain == NULL ||
        api->verify_policy == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        FreeLibrary(api->lib);
        return false;
    }
    return true;
}

/* Adds n bytes of DER to store, giving back the context in the store when out
 * is not NULL. */
static bool cert_add(const CertApi *api, HCERTSTORE store, const uint8_t *der,
                     int64_t n, PCCERT_CONTEXT *out, PalErrno *err) {
    if (n < 0 || (uint64_t)n > 0xffffffffu) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    PCCERT_CONTEXT ctx = api->create_context(CERT_ENCODING, der, (DWORD)n);
    if (ctx == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }
    BOOL ok = api->add_context(store, ctx, CERT_STORE_ADD_ALWAYS, out);
    DWORD e = GetLastError();
    api->free_context(ctx);
    if (!ok) {
        BURROW_OUT(err, burrow__pal_errno_win(e));
        return false;
    }
    return true;
}

/* Go's createStoreContext: the leaf in a store in memory with all of the
 * intermediates, and the leaf's context in that store. The store goes away
 * with the context. */
static PCCERT_CONTEXT cert_store_context(const CertApi *api,
                                         const PalCertVerifyOpts *opts, PalErrno *err) {
    HCERTSTORE store =
        api->open_store(CERT_STORE_PROV_MEMORY, 0, 0,
                        CERT_STORE_DEFER_CLOSE_UNTIL_LAST_FREE_FLAG, NULL);
    if (store == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return NULL;
    }
    PCCERT_CONTEXT leaf = NULL;
    bool ok = cert_add(api, store, opts->leaf, opts->leaf_len, &leaf, err);
    for (int64_t i = 0; ok && i < opts->n_intermediates; i++)
        ok = cert_add(api, store, opts->intermediates[i], opts->intermediate_lens[i],
                      NULL, err);
    if (!ok && leaf != NULL) {
        api->free_context(leaf);
        leaf = NULL;
    }
    api->close_store(store, 0);
    return leaf;
}

/* Go's syscall.NsecToFiletime. */
static FILETIME cert_filetime(int64_t nsec) {
    nsec /= 100;
    nsec += 116444736000000000;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)((uint64_t)nsec & 0xffffffffu);
    ft.dwHighDateTime = (DWORD)(((uint64_t)nsec >> 32) & 0xffffffffu);
    return ft;
}

/* The server name for the SSL policy as UTF-16, in a buffer from the process
 * heap. Go's UTF16PtrFromString fails on a NUL with EINVAL. */
static wchar_t *cert_server_name(const PalCertVerifyOpts *opts, PalErrno *err) {
    int64_t n = opts->server_name_len;
    if (n < 0 || (n > 0 && memchr(opts->server_name, 0, (size_t)n) != NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return NULL;
    }
    char *s = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)n + 1);
    wchar_t *w = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, ((size_t)n + 1) * sizeof *w);
    if (s == NULL || w == NULL) {
        if (s != NULL)
            HeapFree(GetProcessHeap(), 0, s);
        if (w != NULL)
            HeapFree(GetProcessHeap(), 0, w);
        BURROW_OUT(err, PAL_ENOMEM);
        return NULL;
    }
    if (n > 0)
        memcpy(s, opts->server_name, (size_t)n);
    s[n] = 0;
    bool ok = burrow__pal_widen(s, w, (size_t)n + 1, err);
    HeapFree(GetProcessHeap(), 0, s);
    if (!ok) {
        HeapFree(GetProcessHeap(), 0, w);
        return NULL;
    }
    return w;
}

/* The policy half of Go's checkChainSSLServerPolicy. */
static void cert_ssl_policy(const CertApi *api, PCCERT_CHAIN_CONTEXT ctx,
                            const PalCertVerifyOpts *opts, PalCertChain *out) {
    out->policy_checked = true;
    PalErrno e = PAL_OK;
    wchar_t *name = cert_server_name(opts, &e);
    if (name == NULL) {
        out->policy_err = e;
        return;
    }
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl;
    memset(&ssl, 0, sizeof ssl);
    ssl.cbSize = (DWORD)sizeof ssl;
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = name;
    CERT_CHAIN_POLICY_PARA para;
    memset(&para, 0, sizeof para);
    para.cbSize = (DWORD)sizeof para;
    para.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS status;
    memset(&status, 0, sizeof status);
    status.cbSize = (DWORD)sizeof status;
    if (api->verify_policy(CERT_CHAIN_POLICY_SSL, ctx, &para, &status)) {
        out->policy_ok = true;
        out->policy_status = (uint32_t)status.dwError;
    } else {
        out->policy_err = burrow__pal_errno_win(GetLastError());
    }
    HeapFree(GetProcessHeap(), 0, name);
}

/* Hands one chain context to fn, with the certificates of its last simple
 * chain, which is what Go's extractSimpleChain reads. */
static bool cert_chain(const CertApi *api, PCCERT_CHAIN_CONTEXT ctx,
                       const PalCertVerifyOpts *opts, PalCertChainFn fn, void *env,
                       PalErrno *err) {
    PalCertChain c;
    memset(&c, 0, sizeof c);
    c.trust_error = (uint32_t)ctx->TrustStatus.dwErrorStatus;
    if (c.trust_error == 0 && opts->ssl_policy)
        cert_ssl_policy(api, ctx, opts, &c);
    c.n = -1;
    const uint8_t **certs = NULL;
    int64_t *lens = NULL;
    if (ctx->rgpChain != NULL && ctx->cChain > 0) {
        PCERT_SIMPLE_CHAIN last = ctx->rgpChain[ctx->cChain - 1];
        DWORD n = last->cElement;
        certs =
            (const uint8_t **)HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof *certs);
        lens = (int64_t *)HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof *lens);
        if (certs == NULL || lens == NULL) {
            if (certs != NULL)
                HeapFree(GetProcessHeap(), 0, (void *)certs);
            if (lens != NULL)
                HeapFree(GetProcessHeap(), 0, lens);
            BURROW_OUT(err, PAL_ENOMEM);
            return false;
        }
        for (DWORD i = 0; i < n; i++) {
            PCCERT_CONTEXT cert = last->rgpElement[i]->pCertContext;
            certs[i] = cert->pbCertEncoded;
            lens[i] = (int64_t)cert->cbCertEncoded;
        }
        c.certs = certs;
        c.lens = lens;
        c.n = (int64_t)n;
    }
    fn(env, &c);
    if (certs != NULL) {
        HeapFree(GetProcessHeap(), 0, (void *)certs);
        HeapFree(GetProcessHeap(), 0, lens);
    }
    return true;
}

bool pal_cert_verify(const PalCertVerifyOpts *opts, PalCertChainFn fn, void *env,
                     PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (opts == NULL || fn == NULL || opts->leaf == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    CertApi api;
    if (!cert_api_load(&api, err))
        return false;
    PCCERT_CONTEXT leaf = cert_store_context(&api, opts, err);
    if (leaf == NULL) {
        FreeLibrary(api.lib);
        return false;
    }

    CERT_CHAIN_PARA para;
    memset(&para, 0, sizeof para);
    para.cbSize = (DWORD)sizeof para;
    if (opts->any_usage) {
        para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    } else {
        para.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
        para.RequestedUsage.Usage.cUsageIdentifier = (DWORD)opts->n_usages;
        para.RequestedUsage.Usage.rgpszUsageIdentifier =
            opts->n_usages > 0 ? (LPSTR *)(uintptr_t)opts->usages : NULL;
    }
    FILETIME at;
    LPFILETIME when = NULL;
    if (opts->has_time) {
        at = cert_filetime(opts->unix_nano);
        when = &at;
    }

    PCCERT_CHAIN_CONTEXT top = NULL;
    if (!api.get_chain(NULL, leaf, when, leaf->hCertStore, &para,
                       CERT_RETURN_LOWER_QUALITY, NULL, &top)) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        api.free_context(leaf);
        FreeLibrary(api.lib);
        return false;
    }
    bool ok = cert_chain(&api, top, opts, fn, env, err);
    for (DWORD i = 0; ok && i < top->cLowerQualityChainContext; i++)
        ok = cert_chain(&api, top->rgpLowerQualityChainContext[i], opts, fn, env, err);
    api.free_chain(top);
    api.free_context(leaf);
    FreeLibrary(api.lib);
    return ok;
}

#endif /* BURROW_OS_WINDOWS */
