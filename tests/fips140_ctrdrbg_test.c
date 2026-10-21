/* Derived from Go's src/crypto/internal/fips140test/ctrdrbg_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/subtle.h"
#include "burrow/encoding/hex.h"

#include "../src/crypto/drbg_internal.h"

#include <string.h>

#define S(x) BURROW_S(x)

/* decodeHex, from Go's fips_test.go. */
static Slice decode_hex(TestingT *t, Alloc *a, Str s) {
    testing_t_helper(t);
    s = strings_replace_all(a, s, S(" "), S(""));
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, s, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, error_text(err));
    return b;
}

static void TestCounterDRBG(TestingT *t) {
    /* https://github.com/usnistgov/ACVP-Server/blob/fb44dce/gen-val/json-files/ctrDRBG-1.0/prompt.json#L4447-L4482 */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Slice entropy_input = decode_hex(t, a,
                                     S("9FCBB4CCC0135C484BDED061DA9FD70748682FE84166B97"
                                       "FF53F9AA1909B2E95D3D529C0F453B3AC"
                                       "575D12AA441CC5CD"));
    Slice perso_string = decode_hex(t, a,
                                    S("2C9FED0B39556CDBE699EBCA2A0EC7EECB287E8744475050"
                                      "C572FA8AE9ED0A4A7D6F1CABF1C42785"
                                      "32FB20AF7D64BD32"));
    Slice reseed_entropy = decode_hex(t, a,
                                      S("913C0DA19B010EDDD55A7A4F3F713EEF5B1534D34360A7"
                                        "EC376AE71A6B340043CC7726F762CB8534"
                                        "53F399B3A645062A"));
    Slice reseed_additional = decode_hex(t, a,
                                         S("2D9D4EC141A22E6CD2F6EE4F6719CF6BDF95CFE50B8"
                                           "D5EA6C87D38B4B872706FFF80B0380BB90E9C"
                                           "42D11D6526E56C29"));
    Slice additional1 = decode_hex(t, a,
                                   S("A642F06D327828F3E84564A3E37D60C157073B95864CA0798"
                                     "1B0189668A0D978CD5DC68F06801CEF"
                                     "F0DC839A312B028E"));
    Slice additional2 = decode_hex(t, a,
                                   S("9DB14BABFA9107C88BA92073C0B4A65E89147EA06D74B8941"
                                     "42979482F452915B35B5636F9B8A951"
                                     "759735ADE7C8D5D1"));
    Slice returned_bits = decode_hex(
        t, a,
        S("F10C645683FF0131254052ED4C698122B46B563654C29D728AC191CA4AAEFE649EEFE4C6FC33"
          "B25B"
          "B739294DD5CF578099F856C98D98000CBF971F1E6EA900822FF8C110118F6520471744D3F8A3"
          "F5C7"
          "D568494240E57F5488AF9C9F9F4E7322F56CCD843C0DBFCE9170C02E205389420527F23EDB33"
          "69D9"
          "FCC5E34901B5BA4EB71B973FC7982FFE0899FF7FE53EE0C4F51A3EF93EF9C6D4D279DD7536F8"
          "776B"
          "E94AAA05E89EF6E6AEE8832B4B42FFCA5FB91EC0273F9EF945865512889B0C5EE141D1B38DF8"
          "27D2"
          "A694835561628C6F9B093A01A835F07ADBB9E03FEBF93389E8F3B86E1E0ABF1F9958FA286AD9"
          "9528"
          "9C2F606D1A9043A166C1AFE8D00769C712650819C9068A4BD22717C98338395A7BA6E95B5178"
          "BFBF"
          "4EFB0F05A91713BA8BF2127A6BA1EDFA6D1CAB05C03EE0D2AFE1DA4EB8F2C579EC872FF4B602"
          "027E"
          "F4BDCF2F4B01423F8E600A13D7CACB6AB83263BA58F907694AF614A6724FD0E4C627A0D91DDC"
          "6716"
          "C697FACE6F4808A4F37B731DE4E0CD4766CEADAAAF47992505299C72AC1A6E9A8335B8D7E501"
          "B384"
          "1188D0DA4DE5267674444DC2B0CF9F010756FA865A25CA3F1B24C34E845B2259926B6A867A76"
          "84DE"
          "68A6137C4FB0F47A2E54AE9E6455BEBA0B0A9629644FE9E378EE95386443BA977124FFD1192E"
          "9F46"
          "0684C7B09FA99F5F93F04F56FD7955E042187887CE696F1934017E458B16B5C9"));

    /* We don't support personalization strings, but the pre-generated JSON
     * vectors always use them, so just pre-mix them. */
    Byte seed[DRBG_SEED_SIZE];
    subtle_xor_bytes(slice_from(seed, DRBG_SEED_SIZE, DRBG_SEED_SIZE, TYPE_BYTE),
                     entropy_input, perso_string);
    DrbgCounter c;
    burrow__drbg_new_counter(&c, seed);

    burrow__drbg_counter_reseed(&c, reseed_entropy.p, reseed_additional.p);

    Slice buf = slice_make(a, TYPE_BYTE, returned_bits.len, returned_bits.len);
    (void)burrow__drbg_counter_generate(&c, buf, additional1.p);

    (void)burrow__drbg_counter_generate(&c, buf, additional2.p);
    if (!bytes_equal(buf, returned_bits))
        testing_t_errorf_v(t, "unexpected output:\n%x\n%x", buf, returned_bits);
    arena_free(&ar);
}

#define TESTS(X) X(TestCounterDRBG)

TESTING_MAIN(TESTS)
