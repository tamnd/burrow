/* Derived from Go's src/crypto/internal/fips140test/sshkdf_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/hex.h"

#include "../src/crypto/ssh_internal.h"

static Slice decode_hex(TestingT *t, Alloc *a, const char *s) {
    testing_t_helper(t);
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(
        a, strings_replace_all(a, str_from_cstr(s), BURROW_S(" "), BURROW_S("")), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    return b;
}

static void TestSSHACVPVector(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* https://github.com/usnistgov/ACVP-Server/blob/3a7333f638/gen-val/json-files/kdf-components-ssh-1.0/prompt.json#L910-L915 */
    Slice K = decode_hex(t, a,
                         "0000010100E534CD9780786AF19994DD68C3FD7FE1E1F77C3938B2005C49B"
                         "080CF88A63A44079774A36F"
                         "23BA4D73470CB318C30524854D2F36BAB9A45AD73DBB3BC5DD39A547F62BC"
                         "921052E102E37F3DD0CD79A"
                         "04EB46ACC14B823B326096A89E33E8846624188BB3C8F16B320E7BB8F5EB0"
                         "5F080DCEE244A445DBED3A9"
                         "F3BA8C373D8BE62CDFE2FC5876F30F90F01F0A55E5251B23E0DBBFCFB1450"
                         "715E329BB00FB222E850DDB"
                         "11201460B8AEF3FC8965D3B6D3AFBB885A6C11F308F10211B82EA2028C7A8"
                         "4DD0BB8D5D6AC3A48D0C2B9"
                         "3609269C585E03889DB3621993E7F7C09A007FB6B5C06FFA532B0DBF11F71"
                         "F740D9CD8FAD2532E21B942"
                         "3BF3D85EE4E396BE32");
    Slice H = decode_hex(
        t, a, "8FB22F0864960DA5679FD377248E41C2D0390E5AB3BB7955A3B6C588FB75B20D");
    Slice sessionID = decode_hex(
        t, a, "269A512E7B560E13396E0F3F56BDA730E23EE122EE6D59C91C58FB07872BCCCC");

    /* https://github.com/usnistgov/ACVP-Server/blob/3a7333f638/gen-val/json-files/kdf-components-ssh-1.0/expectedResults.json#L1306-L1314 */
    Slice initialIVClient = decode_hex(t, a, "82321D9FE2ACD958D3F55F4D3FF5C79D");
    Slice initialIVServer = decode_hex(t, a, "03F336F61311770BD5346B41E04CDB1F");
    Slice encryptionKeyClient = decode_hex(t, a, "20E55008D0120C400F42E5D2E148AB75");
    Slice encryptionKeyServer = decode_hex(t, a, "8BF4DEBEC96F4ADBBE5BB43828D56E6D");
    Slice integrityKeyClient = decode_hex(
        t, a, "15F53BCCE2645D0AD1C539C09BF9054AA3A4B10B71E96B9E3A15672405341BB5");
    Slice integrityKeyServer = decode_hex(
        t, a, "00BB773FD63AC7B7281A7B54C130CCAD363EE8928104E67CA5A3211EE3BBAB93");

    Slice gotIVClient, gotKeyClient, gotIntegrityClient;
    Error err = BURROW_NO_ERROR;
    burrow__ssh_keys(a, sha256_new, burrow__ssh_client_keys, K, H, sessionID, 16, 16,
                     32, &gotIVClient, &gotKeyClient, &gotIntegrityClient, &err);
    /* Not in Go: burrow's Keys can run out of memory. */
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Slice gotIVServer, gotKeyServer, gotIntegrityServer;
    burrow__ssh_keys(a, sha256_new, burrow__ssh_server_keys, K, H, sessionID, 16, 16,
                     32, &gotIVServer, &gotKeyServer, &gotIntegrityServer, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);

    if (!bytes_equal(gotIVClient, initialIVClient))
        testing_t_errorf_v(t, "got IV client %x, want %x", gotIVClient,
                           initialIVClient);
    if (!bytes_equal(gotKeyClient, encryptionKeyClient))
        testing_t_errorf_v(t, "got key client %x, want %x", gotKeyClient,
                           encryptionKeyClient);
    if (!bytes_equal(gotIntegrityClient, integrityKeyClient))
        testing_t_errorf_v(t, "got integrity key client %x, want %x",
                           gotIntegrityClient, integrityKeyClient);
    if (!bytes_equal(gotIVServer, initialIVServer))
        testing_t_errorf_v(t, "got IV server %x, want %x", gotIVServer,
                           initialIVServer);
    if (!bytes_equal(gotKeyServer, encryptionKeyServer))
        testing_t_errorf_v(t, "got key server %x, want %x", gotKeyServer,
                           encryptionKeyServer);
    if (!bytes_equal(gotIntegrityServer, integrityKeyServer))
        testing_t_errorf_v(t, "got integrity key server %x, want %x",
                           gotIntegrityServer, integrityKeyServer);
    arena_free(&ar);
}

#define TESTS(X) X(TestSSHACVPVector)

TESTING_MAIN(TESTS)
