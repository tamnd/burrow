/* Derived from Go's src/crypto/cipher/gcm_test.go.
 * Go source: go1.27.1.
 *
 * TestGCMExtraMethods is left out for the reason aes_test.c gives for
 * TestExtraMethods, and TestGCMNoncesFIPSV1 because it is about the FIPS 140
 * module's internal constructors and service indicator, which burrow does not
 * have. TestGCMWycheproof waits for the Wycheproof vectors.
 *
 * Go skips TestGCMAsm where there is no assembly. Here the AES path and the
 * wrapped path are different code even without the instructions, batched
 * counter blocks against one block at a time, so it always runs.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testcipher.h"

#include "burrow/burrow.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/rand.h"
#include "burrow/encoding/hex.h"

#include <string.h>

/* wrap hides the AES block behind a table of its own, so that GCM cannot see
 * it is AES and takes the general path. */
typedef struct Wrapper {
    CipherBlock block;
} Wrapper;

static Int wrapper_block_size(void *self) {
    return cipher_block_block_size(((Wrapper *)self)->block);
}

static void wrapper_encrypt(void *self, Slice dst, Slice src) {
    cipher_block_encrypt(((Wrapper *)self)->block, dst, src);
}

static void wrapper_decrypt(void *self, Slice dst, Slice src) {
    cipher_block_decrypt(((Wrapper *)self)->block, dst, src);
}

static const CipherBlockVT wrapper_vt = {NULL, wrapper_block_size, wrapper_encrypt,
                                         wrapper_decrypt};

static CipherBlock wrap(Alloc *a, CipherBlock b) {
    Wrapper *w = BURROW_NEW(a, Wrapper);
    w->block = b;
    CipherBlock out = {&wrapper_vt, w};
    return out;
}

typedef CipherBlock (*NewCipher)(TestingT *t, Alloc *a, Slice key);

static CipherBlock new_aes(TestingT *t, Alloc *a, Slice key) {
    Error err = BURROW_NO_ERROR;
    CipherBlock c = aes_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    return c;
}

static CipherBlock new_wrapped(TestingT *t, Alloc *a, Slice key) {
    return wrap(a, new_aes(t, a, key));
}

typedef void (*GcmTest)(TestingT *t, NewCipher new_cipher);

static void impl_aes(void *env, TestingT *t) {
    (*(const GcmTest *)env)(t, new_aes);
}

static void impl_fallback(void *env, TestingT *t) {
    (*(const GcmTest *)env)(t, new_wrapped);
}

static void test_all_implementations(TestingT *t, GcmTest f) {
    testcipher_all_implementations(t, impl_aes, &f);
    testing_t_run(t, BURROW_S("Fallback"), BURROW_FN(TestingTFunc, impl_fallback, &f));
}

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("bad hex in a test table"));
    return b;
}

static const struct {
    const char *key, *nonce, *plaintext, *ad, *result;
} aes_gcm_tests[] = {
    /* key=16, plaintext=null */
    {"11754cd72aec309bf52f7687212e8957", "3c819d9a9bed087615030b65", "", "",
     "250327c674aaf477aef2675748cf6971"},
    /* key=24, plaintext=null */
    {"e2e001a36c60d2bf40d69ff5b2b1161ea218db263be16a4e", "3c819d9a9bed087615030b65", "",
     "", "c7b8da1fe2e3dccc4071ba92a0a57ba8"},
    /* key=32, plaintext=null */
    {"5394e890d37ba55ec9d5f327f15680f6a63ef5279c79331643ad0af6d2623525",
     "3c819d9a9bed087615030b65", "", "", "d9b260d4bc4630733ffb642f5ce45726"},
    {"ca47248ac0b6f8372a97ac43508308ed", "ffd2b598feabc9019262d2be", "", "",
     "60d20404af527d248d893ae495707d1a"},
    {"fbe3467cc254f81be8e78d765a2e6333", "c6697351ff4aec29cdbaabf2", "", "67",
     "3659cdc25288bf499ac736c03bfc1159"},
    {"8a7f9d80d08ad0bd5a20fb689c88f9fc", "88b7b27d800937fda4f47301", "",
     "50edd0503e0d7b8c91608eb5a1", "ed6f65322a4740011f91d2aae22dd44e"},
    {"051758e95ed4abb2cdc69bb454110e82", "c99a66320db73158a35a255d", "",
     "67c6697351ff4aec29cdbaabf2fbe3467cc254f81be8e78d765a2e63339f",
     "6ce77f1a5616c505b6aec09420234036"},
    {"77be63708971c4e240d1cb79e8d77feb", "e0e00f19fed7ba0136a797f3", "",
     "7a43ec1d9c0a5a78a0b16533a6213cab", "209fcc8d3675ed938e9c7166709dd946"},
    {"7680c5d3ca6154758e510f4d25b98820", "f8f105f9c3df4965780321f8", "",
     "c94c410194c765e3dcc7964379758ed3", "94dca8edfcf90bb74b153c8d48a17930"},
    /* key=16, plaintext=16 */
    {"7fddb57453c241d03efbed3ac44e371c", "ee283a3fc75575e33efd4887",
     "d5de42b461646c255c87bd2962d3b9a2", "",
     "2ccda4a5415cb91e135c2a0f78c9b2fdb36d1df9b9d5e596f83e8b7f52971cb3"},
    {"ab72c77b97cb5fe9a382d9fe81ffdbed", "54cc7dc2c37ec006bcc6d1da",
     "007c5e5b3e59df24a7c355584fc1518d", "",
     "0e1bde206a07a9c2c1b65300f8c649972b4401346697138c7a4891ee59867d0c"},
    /* key=24, plaintext=16 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c", "54cc7dc2c37ec006bcc6d1da",
     "007c5e5b3e59df24a7c355584fc1518d", "",
     "7bd53594c28b6c6596feb240199cad4c9badb907fd65bde541b8df3bd444d3a8"},
    /* key=32, plaintext=16 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
     "54cc7dc2c37ec006bcc6d1da", "007c5e5b3e59df24a7c355584fc1518d", "",
     "d50b9e252b70945d4240d351677eb10f937cdaef6f2822b6a3191654ba41b197"},
    /* key=16, plaintext=23 */
    {"ab72c77b97cb5fe9a382d9fe81ffdbed", "54cc7dc2c37ec006bcc6d1da",
     "007c5e5b3e59df24a7c355584fc1518dabcdefab", "",
     "0e1bde206a07a9c2c1b65300f8c64997b73381a6ff6bc24c5146fbd73361f4fe"},
    /* key=24, plaintext=23 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c", "54cc7dc2c37ec006bcc6d1da",
     "007c5e5b3e59df24a7c355584fc1518dabcdefab", "",
     "7bd53594c28b6c6596feb240199cad4c23b86a96d423cffa929e68541dc16b28"},
    /* key=32, plaintext=23 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
     "54cc7dc2c37ec006bcc6d1da", "007c5e5b3e59df24a7c355584fc1518dabcdefab", "",
     "d50b9e252b70945d4240d351677eb10f27fd385388ad3b72b96a2d5dea1240ae"},
    /* key=16, plaintext=51 */
    {"fe47fcce5fc32665d2ae399e4eec72ba", "5adb9609dbaeb58cbd6e7275",
     "7c0e88c88899a779228465074797cd4c2e1498d259b54390b85e3eef1c02df60e743f1b840382c4bc"
     "caf3bafb4ca8429bea063",
     "88319d6e1d3ffa5f987199166c8a9b56c2aeba5a",
     "98f4826f05a265e6dd2be82db241c0fbbbf9ffb1c173aa83964b7cf5393043736365253ddbc5db877"
     "8371495da76d269e5db3e291ef1982e4defedaa2249f898556b47"},
    {"ec0c2ba17aa95cd6afffe949da9cc3a8", "296bce5b50b7d66096d627ef",
     "b85b3753535b825cbe5f632c0b843c741351f18aa484281aebec2f45bb9eea2d79d987b764b9611f6"
     "c0f8641843d5d58f3a242",
     "f8d00f05d22bf68599bcdeb131292ad6e2df5d14",
     "a7443d31c26bdf2a1c945e29ee4bd344a99cfaf3aa71f8b3f191f83c2adfc7a07162995506fde6309"
     "ffc19e716eddf1a828c5a890147971946b627c40016da1ecf3e77"},
    {"2c1f21cf0f6fb3661943155c3e3d8492", "23cb5ff362e22426984d1907",
     "42f758836986954db44bf37c6ef5e4ac0adaf38f27252a1b82d02ea949c8a1a2dbc0d68b5615ba7c1"
     "220ff6510e259f06655d8",
     "5d3624879d35e46849953e45a32a624d6a6c536ed9857c613b572b0333e701557a713e3f010ecdf9a"
     "6bd6c9e3e44b065208645aff4aabee611b391528514170084ccf587177f4488f33cfb5e979e42b6e1"
     "cfc0a60238982a7aec",
     "81824f0e0d523db30d3da369fdc0d60894c7a0a20646dd015073ad2732bd989b14a222b6ad57af43e"
     "1895df9dca2a5344a62cc57a3ee28136e94c74838997ae9823f3a"},
    {"d9f7d2411091f947b4d6f1e2d1f0fb2e", "e1934f5db57cc983e6b180e7",
     "73ed042327f70fe9c572a61545eda8b2a0c6e1d6c291ef19248e973aee6c312012f490c2c6f6166f4"
     "a59431e182663fcaea05a",
     "0a8a18a7150e940c3d87b38e73baee9a5c049ee21795663e264b694a949822b639092d0e67015e863"
     "63583fcf0ca645af9f43375f05fdb4ce84f411dcbca73c2220dea03a20115d2e51398344b16bee1ed"
     "7c499b353d6c597af8",
     "aaadbd5c92e9151ce3db7210b8714126b73e43436d242677afa50384f2149b831f1d573c7891c2a91"
     "fbc48db29967ec9542b2321b51ca862cb637cdd03b99a0f93b134"},
    /* key=24 plaintext=51 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c", "e1934f5db57cc983e6b180e7",
     "73ed042327f70fe9c572a61545eda8b2a0c6e1d6c291ef19248e973aee6c312012f490c2c6f6166f4"
     "a59431e182663fcaea05a",
     "0a8a18a7150e940c3d87b38e73baee9a5c049ee21795663e264b694a949822b639092d0e67015e863"
     "63583fcf0ca645af9f43375f05fdb4ce84f411dcbca73c2220dea03a20115d2e51398344b16bee1ed"
     "7c499b353d6c597af8",
     "0736378955001d50773305975b3a534a4cd3614dd7300916301ae508cb7b45aa16e79435ca16b5557"
     "bcad5991bc52b971806863b15dc0b055748919b8ee91bc8477f68"},
    /* key-32 plaintext=51 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
     "e1934f5db57cc983e6b180e7",
     "73ed042327f70fe9c572a61545eda8b2a0c6e1d6c291ef19248e973aee6c312012f490c2c6f6166f4"
     "a59431e182663fcaea05a",
     "0a8a18a7150e940c3d87b38e73baee9a5c049ee21795663e264b694a949822b639092d0e67015e863"
     "63583fcf0ca645af9f43375f05fdb4ce84f411dcbca73c2220dea03a20115d2e51398344b16bee1ed"
     "7c499b353d6c597af8",
     "fc1ae2b5dcd2c4176c3f538b4c3cc21197f79e608cc3730167936382e4b1e5a7b75ae1678bcebd876"
     "705477eb0e0fdbbcda92fb9a0dc58c8d8f84fb590e0422e6077ef"},
    /* key=16 plaintext=138 */
    {"d9f7d2411091f947b4d6f1e2d1f0fb2e", "e1934f5db57cc983e6b180e7",
     "67c6697351ff4aec29cdbaabf2fbe3467cc254f81be8e78d765a2e63339fc99a66320db73158a35a2"
     "55d051758e95ed4abb2cdc69bb454110e827441213ddc8770e93ea141e1fc673e017e97eadc6b968f"
     "385c2aecb03bfb32af3c54ec18db5c021afe43fbfaaa3afb29d1e6053c7c9475d8be6189f95cbba89"
     "90f95b1ebf1b3aabbccddee",
     "0a8a18a7150e940c3d87b38e73baee9a5c049ee21795663e264b694a949822b639092d0e67015e863"
     "63583fcf0ca645af9f43375f05fdb4ce84f411dcbca73c2220dea03a20115d2e51398344b16bee1ed"
     "7c499b353d6c597af8",
     "be86d00ce4e150190f646eae0f670ad26b3af66db45d2ee3fd71badd2fe763396bdbca498f3f779c7"
     "0b80ed2695943e15139b406e5147b3855a1441dfb7bd64954b581e3db0ddf26b1c759e2276a4c18a8"
     "e4ad4b890f473e61c78e60074bd0633961e87e66d0a1be77c51ab6b9bb3318ccdd43794ffc18a03a8"
     "3c1d368eeea590a13407c7ef48efc66e26047f3ab9deed0412ce89e"},
    /* key=24 plaintext=138 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c", "e1934f5db57cc983e6b180e7",
     "67c6697351ff4aec29cdbaabf2fbe3467cc254f81be8e78d765a2e63339fc99a66320db73158a35a2"
     "55d051758e95ed4abb2cdc69bb454110e827441213ddc8770e93ea141e1fc673e017e97eadc6b968f"
     "385c2aecb03bfb32af3c54ec18db5c021afe43fbfaaa3afb29d1e6053c7c9475d8be6189f95cbba89"
     "90f95b1ebf1b3aabbccddee",
     "0a8a18a7150e940c3d87b38e73baee9a5c049ee21795663e264b694a949822b639092d0e67015e863"
     "63583fcf0ca645af9f43375f05fdb4ce84f411dcbca73c2220dea03a20115d2e51398344b16bee1ed"
     "7c499b353d6c597af8",
     "131d5ad9230858559b8c1929ec2c18be90d7d4630e49018262ce5c511688bd10622109403db800601"
     "4ce93905b0a16bf1d1411acc9e14edf09518bd5967ff4bc202805d4c2810810a093e996a0f56c9a3e"
     "3e593c783f68528c1a282ff6f4925902bb2b3d4cdd04b873663bf5fd9dd53b5df462e0424d038f249"
     "b10a99c0523200f8c92c3e8a178a25ee8e23b71308c88ec2cfe047e"},
    /* key-32 plaintext=138 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
     "e1934f5db57cc983e6b180e7",
     "67c6697351ff4aec29cdbaabf2fbe3467cc254f81be8e78d765a2e63339fc99a66320db73158a35a2"
     "55d051758e95ed4abb2cdc69bb454110e827441213ddc8770e93ea141e1fc673e017e97eadc6b968f"
     "385c2aecb03bfb32af3c54ec18db5c021afe43fbfaaa3afb29d1e6053c7c9475d8be6189f95cbba89"
     "90f95b1ebf1b3aabbccddee",
     "0a8a18a7150e940c3d87b38e73baee9a5c049ee21795663e264b694a949822b639092d0e67015e863"
     "63583fcf0ca645af9f43375f05fdb4ce84f411dcbca73c2220dea03a20115d2e51398344b16bee1ed"
     "7c499b353d6c597af8",
     "e8318fe5aada811280804f35fb2a89e54bf32b4e55ba7b953547dadb39421d1dc39c7c127c6008b20"
     "8010177f02fc093c8bbb8b3834d0e060d96dda96ba386c7c01224a4cac1edebffda4f9a64692bfbff"
     "b9f7c2999069fab84205224978a10d815d5ab8fa31e4e11630ba01c3b6cb99bef5772357ce86b83b4"
     "fb45ea7146402d560b6ad07de635b9366865e788a6bcdb132dcd079"},
    /* key=16, plaintext=13 */
    {"fe9bb47deb3a61e423c2231841cfd1fb", "4d328eb776f500a2f7fb47aa",
     "f1cc3818e421876bb6b8bbd6c9", "",
     "b88c5c1977b35b517b0aeae96743fd4727fe5cdb4b5b42818dea7ef8c9"},
    /* key=16, plaintext=13 */
    {"6703df3701a7f54911ca72e24dca046a", "12823ab601c350ea4bc2488c",
     "793cd125b0b84a043e3ac67717", "",
     "b2051c80014f42f08735a7b0cd38e6bcd29962e5f2c13626b85a877101"},
    /* key=24, plaintext=13 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c", "12823ab601c350ea4bc2488c",
     "793cd125b0b84a043e3ac67717", "",
     "e888c2f438caedd4189d26c59f53439b8a7caec29e98c33ebf7e5712d6"},
    /* key=32, plaintext=13 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
     "12823ab601c350ea4bc2488c", "793cd125b0b84a043e3ac67717", "",
     "e796c39074c7783a38193e3f8d46b355adacca7198d16d879fbfeac6e3"},
    /* key=16, plaintext=0 */
    {"1672c3537afa82004c6b8a46f6f0d026", "05", "", "",
     "8e2ad721f9455f74d8b53d3141f27e8e"},
    /* key=16, plaintext=32 */
    {"9a4fea86a621a91ab371e492457796c0", "75",
     "ca6131faf0ff210e4e693d6c31c109fc5b6f54224eb120f37de31dc59ec669b6",
     "4f6e2585c161f05a9ae1f2f894e9f0ab52b45d0f",
     "5698c0a384241d30004290aac56bb3ece6fe8eacc5c4be98954deb9c3ff6aebf5d50e1af100509e1f"
     "ba2a5e8a0af9670"},
    /* key=24, plaintext=32 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c", "75",
     "ca6131faf0ff210e4e693d6c31c109fc5b6f54224eb120f37de31dc59ec669b6",
     "4f6e2585c161f05a9ae1f2f894e9f0ab52b45d0f",
     "2709b357ec8334a074dbd5c4c352b216cfd1c8bd66343c5d43bfc6bd3b2b6cd0e3a82315d56ea5e49"
     "61c9ef3bc7e4042"},
    /* key=32, plaintext=32 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", "75",
     "ca6131faf0ff210e4e693d6c31c109fc5b6f54224eb120f37de31dc59ec669b6",
     "4f6e2585c161f05a9ae1f2f894e9f0ab52b45d0f",
     "d73bebe722c5e312fe910ba71d5a6a063a4297203f819103dfa885a8076d095545a999affde3dbac2"
     "b5be6be39195ed0"},
    /* key=16, plaintext=0 */
    {"d0f1f4defa1e8c08b4b26d576392027c",
     "42b4f01eb9f5a1ea5b1eb73b0fb0baed54f387ecaa0393c7d7dffc6af50146ecc021abf7eb9038d43"
     "03d91f8d741a11743166c0860208bcc02c6258fd9511a2fa626f96d60b72fcff773af4e88e7a92350"
     "6e4916ecbd814651e9f445adef4ad6a6b6c7290cc13b956130eef5b837c939fcac0cbbcc9656cd75b"
     "13823ee5acdac",
     "", "", "7ab49b57ddf5f62c427950111c5c4f0d"},
    /* key=16, plaintext=13 */
    {"4a0c00a3d284dea9d4bf8b8dde86685e",
     "f8cbe82588e784bcacbe092cd9089b51e01527297f635bf294b3aa787d91057ef23869789698ac960"
     "707857f163ecb242135a228ad93964f5dc4a4d7f88fd7b3b07dd0a5b37f9768fb05a523639f108c34"
     "c661498a56879e501a2321c8a4a94d7e1b89db255ac1f685e185263368e99735ebe62a7f2931b4728"
     "2be8eb165e4d7",
     "6d4bf87640a6a48a50d28797b7", "8d8c7ffc55086d539b5a8f0d1232654c",
     "0d803ec309482f35b8e6226f2b56303239298e06b281c2d51aaba3c125"},
    /* key=16, plaintext=128 */
    {"0e18a844ac5bf38e4cd72d9b0942e506", "0870d4b28a2954489a0abcd5",
     "67c6697351ff4aec29cdbaabf2fbe3467cc254f81be8e78d765a2e63339fc99a66320db73158a35a2"
     "55d051758e95ed4abb2cdc69bb454110e827441213ddc8770e93ea141e1fc673e017e97eadc6b968f"
     "385c2aecb03bfb32af3c54ec18db5c021afe43fbfaaa3afb29d1e6053c7c9475d8be6189f95cbba89"
     "90f95b1ebf1b3",
     "05eff700e9a13ae5ca0bcbd0484764bd1f231ea81c7b64c514735ac55e4b79633b706424119e09dca"
     "ad4acf21b10af3b33cde3504847155cbb6f2219ba9b7df50be11a1c7f23f829f8a41b13b5ca4ee898"
     "3238e0794d3d34bc5f4e77facb6c05ac86212baa1a55a2be70b5733b045cd33694b3afe2f0e49e4f3"
     "21549fd824ea9",
     "cace28f4976afd72e3c5128167eb788fbf6634dda0a2f53148d00f6fa557f5e9e8f736c12e450894a"
     "f56cb67f7d99e1027258c8571bd91ee3b7360e0d508aa1f382411a16115f9c05251cc326d4016f62e"
     "0eb8151c048465b0c6c8ff12558d43310e18b2cb1889eec91557ce21ba05955cf4c1d4847aadfb1b0"
     "a83f3a3b82b7efa62a5f03c5d6eda381a85dd78dbc55c"},
    /* key=24, plaintext=128 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c", "0870d4b28a2954489a0abcd5",
     "67c6697351ff4aec29cdbaabf2fbe3467cc254f81be8e78d765a2e63339fc99a66320db73158a35a2"
     "55d051758e95ed4abb2cdc69bb454110e827441213ddc8770e93ea141e1fc673e017e97eadc6b968f"
     "385c2aecb03bfb32af3c54ec18db5c021afe43fbfaaa3afb29d1e6053c7c9475d8be6189f95cbba89"
     "90f95b1ebf1b3",
     "05eff700e9a13ae5ca0bcbd0484764bd1f231ea81c7b64c514735ac55e4b79633b706424119e09dca"
     "ad4acf21b10af3b33cde3504847155cbb6f2219ba9b7df50be11a1c7f23f829f8a41b13b5ca4ee898"
     "3238e0794d3d34bc5f4e77facb6c05ac86212baa1a55a2be70b5733b045cd33694b3afe2f0e49e4f3"
     "21549fd824ea9",
     "303157d398376a8d51e39eabdd397f45b65f81f09acbe51c726ae85867e1675cad178580bb31c7f37"
     "c1af3644bd36ac436e9459139a4903d95944f306e415da709134dccde9d2b2d7d196b6740c196d9d1"
     "0caa45296cf577a6e15d7ddf3576c20c503617d6a9e6b6d2be09ae28410a1210700a463a5b3b8d391"
     "abe9dac217e76a6f78306b5ebe759a5986b7d6682db0b"},
    /* key=32, plaintext=128 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
     "0870d4b28a2954489a0abcd5",
     "67c6697351ff4aec29cdbaabf2fbe3467cc254f81be8e78d765a2e63339fc99a66320db73158a35a2"
     "55d051758e95ed4abb2cdc69bb454110e827441213ddc8770e93ea141e1fc673e017e97eadc6b968f"
     "385c2aecb03bfb32af3c54ec18db5c021afe43fbfaaa3afb29d1e6053c7c9475d8be6189f95cbba89"
     "90f95b1ebf1b3",
     "05eff700e9a13ae5ca0bcbd0484764bd1f231ea81c7b64c514735ac55e4b79633b706424119e09dca"
     "ad4acf21b10af3b33cde3504847155cbb6f2219ba9b7df50be11a1c7f23f829f8a41b13b5ca4ee898"
     "3238e0794d3d34bc5f4e77facb6c05ac86212baa1a55a2be70b5733b045cd33694b3afe2f0e49e4f3"
     "21549fd824ea9",
     "e4f13934744125b9c35935ed4c5ac7d0c16434d52eadef1da91c6abb62bc757f01e3e42f628f030d7"
     "50826adceb961f0675b81de48376b181d8781c6a0ccd0f34872ef6901b97ff7c2e152426b3257fb91"
     "f6a43f47befaaf7a2136fd0c97de8c48517ce047a5641141092c717b151b44f0794a164b5861f0a77"
     "271d1bdbc332e9e43d3b9828ccfdbd4ae338da5baf7a9"},
    /* key=16, plaintext=512 */
    {"1f6c3a3bc0542aabba4ef8f6c7169e73", "f3584606472b260e0dd2ebb2",
     "67c6697351ff4aec29cdbaabf2fbe3467cc254f81be8e78d765a2e63339fc99a66320db73158a35a2"
     "55d051758e95ed4abb2cdc69bb454110e827441213ddc8770e93ea141e1fc673e017e97eadc6b968f"
     "385c2aecb03bfb32af3c54ec18db5c021afe43fbfaaa3afb29d1e6053c7c9475d8be6189f95cbba89"
     "90f95b1ebf1b305eff700e9a13ae5ca0bcbd0484764bd1f231ea81c7b64c514735ac55e4b79633b70"
     "6424119e09dcaad4acf21b10af3b33cde3504847155cbb6f2219ba9b7df50be11a1c7f23f829f8a41"
     "b13b5ca4ee8983238e0794d3d34bc5f4e77facb6c05ac86212baa1a55a2be70b5733b045cd33694b3"
     "afe2f0e49e4f321549fd824ea90870d4b28a2954489a0abcd50e18a844ac5bf38e4cd72d9b0942e50"
     "6c433afcda3847f2dadd47647de321cec4ac430f62023856cfbb20704f4ec0bb920ba86c33e05f1ec"
     "d96733b79950a3e314d3d934f75ea0f210a8f6059401beb4bc4478fa4969e623d01ada696a7e4c7e5"
     "125b34884533a94fb319990325744ee9bbce9e525cf08f5e9e25e5360aad2b2d085fa54d835e8d466"
     "826498d9a8877565705a8a3f62802944de7ca5894e5759d351adac869580ec17e485f18c0c66f17cc"
     "07cbb22fce466da610b63af62bc83b4692f3affaf271693ac071fb86d11342d8def4f89d4b66335c1"
     "c7e4248367d8ed9612ec453902d8e50af89d7709d1a596c1f41f",
     "95aa82ca6c49ae90cd1668baac7aa6f2b4a8ca99b2c2372acb08cf61c9c3805e6e0328da4cd76a19e"
     "dd2d3994c798b0022569ad418d1fee4d9cd45a391c601ffc92ad91501432fee150287617c13629e69"
     "fc7281cd7165a63eab49cf714bce3a75a74f76ea7e64ff81eb61fdfec39b67bf0de98c7e4e32bdf97"
     "c8c6ac75ba43c02f4b2ed7216ecf3014df000108b67cf99505b179f8ed4980a6103d1bca70dbe9bbf"
     "ab0ed59801d6e5f2d6f67d3ec5168e212e2daf02c6b963c98a1f7097de0c56891a2b211b01070dd8f"
     "d8b16c2a1a4e3cfd292d2984b3561d555d16c33ddc2bcf7edde13efe520c7e2abdda44d81881c531a"
     "eeeb66244c3b791ea8acfb6a68",
     "55864065117e07650ca650a0f0d9ef4b02aee7c58928462fddb49045bf85355b4653fa26158210a7f"
     "3ef5b3ca48612e8b7adf5c025c1b821960af770d935df1c9a1dd25077d6b1c7f937b2e20ce981b079"
     "80880214698f3fad72fa370b3b7da257ce1d0cf352bc5304fada3e0f8927bd4e5c1abbffa563bdedc"
     "b567daa64faaed748cb361732200ba3506836a3c1c82aafa14c76dc07f6c4277ff2c61325f91fdbd6"
     "c1883e745fcaadd5a6d692eeaa5ad56eead6a9d74a595d22757ed89532a4b8831e2b9e2315baea70a"
     "9b95d228f09d491a5ed5ab7076766703457e3159bbb9b17b329525669863153079448c68cd2f200c0"
     "be9d43061a60639cb59d50993d276c05caaa565db8ce633b2673e4012bebbca02b1a64d779d04066f"
     "3e949ece173825885ec816468c819a8129007cc05d8785c48077d09eb1abcba14508dde85a6f16a74"
     "4bc95faef24888d53a8020515ab20307efaecbdf143a26563c67989bceedc2d6d2bb9699bb6c615d9"
     "3767e4158c1124e3b6c723aaa47796e59a60d3696cd85adfae9a62f2c02c22009f80ed494bdc587f3"
     "1dd892c253b5c6d6b7db078fa72d23474ee54f8144d6561182d71c862941dbc0b2cb37a4d4b23cbad"
     "5637e6be901cc73f16d5aec39c60dddee631511e57b47520b61ae1892d2d1bd2b486e30faec892f17"
     "1b6de98d96108016fac805604761f8e74742b3bb7dc8a290a46bf697c3e4446e6e65832cbae7cf1aa"
     "ad1"},
    /* key=24, plaintext=512 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c", "f3584606472b260e0dd2ebb2",
     "67c6697351ff4aec29cdbaabf2fbe3467cc254f81be8e78d765a2e63339fc99a66320db73158a35a2"
     "55d051758e95ed4abb2cdc69bb454110e827441213ddc8770e93ea141e1fc673e017e97eadc6b968f"
     "385c2aecb03bfb32af3c54ec18db5c021afe43fbfaaa3afb29d1e6053c7c9475d8be6189f95cbba89"
     "90f95b1ebf1b305eff700e9a13ae5ca0bcbd0484764bd1f231ea81c7b64c514735ac55e4b79633b70"
     "6424119e09dcaad4acf21b10af3b33cde3504847155cbb6f2219ba9b7df50be11a1c7f23f829f8a41"
     "b13b5ca4ee8983238e0794d3d34bc5f4e77facb6c05ac86212baa1a55a2be70b5733b045cd33694b3"
     "afe2f0e49e4f321549fd824ea90870d4b28a2954489a0abcd50e18a844ac5bf38e4cd72d9b0942e50"
     "6c433afcda3847f2dadd47647de321cec4ac430f62023856cfbb20704f4ec0bb920ba86c33e05f1ec"
     "d96733b79950a3e314d3d934f75ea0f210a8f6059401beb4bc4478fa4969e623d01ada696a7e4c7e5"
     "125b34884533a94fb319990325744ee9bbce9e525cf08f5e9e25e5360aad2b2d085fa54d835e8d466"
     "826498d9a8877565705a8a3f62802944de7ca5894e5759d351adac869580ec17e485f18c0c66f17cc"
     "07cbb22fce466da610b63af62bc83b4692f3affaf271693ac071fb86d11342d8def4f89d4b66335c1"
     "c7e4248367d8ed9612ec453902d8e50af89d7709d1a596c1f41f",
     "95aa82ca6c49ae90cd1668baac7aa6f2b4a8ca99b2c2372acb08cf61c9c3805e6e0328da4cd76a19e"
     "dd2d3994c798b0022569ad418d1fee4d9cd45a391c601ffc92ad91501432fee150287617c13629e69"
     "fc7281cd7165a63eab49cf714bce3a75a74f76ea7e64ff81eb61fdfec39b67bf0de98c7e4e32bdf97"
     "c8c6ac75ba43c02f4b2ed7216ecf3014df000108b67cf99505b179f8ed4980a6103d1bca70dbe9bbf"
     "ab0ed59801d6e5f2d6f67d3ec5168e212e2daf02c6b963c98a1f7097de0c56891a2b211b01070dd8f"
     "d8b16c2a1a4e3cfd292d2984b3561d555d16c33ddc2bcf7edde13efe520c7e2abdda44d81881c531a"
     "eeeb66244c3b791ea8acfb6a68",
     "9daa466c7174dfde72b435fb6041ed7ff8ab8b1b96edb90437c3cc2e7e8a7c2c3629bae3bcaede99e"
     "e926ef4c55571e504e1c516975f6c719611c4da74acc23bbc79b3a67491f84d573e0293aa0cf5d775"
     "dde93fc466d5babd3e93a6506c0261021ac184f571ab190df83c32b41a67eaaa8dde27c02b08f15ca"
     "bc75e46d1f9634f32f9233b2cb975386ff3a5e16b6ea2e2e4215cb33beb4de39a861d7f4a02165cd7"
     "63f8252b2d60ac45d65a70735a8806a8fec3ca9d37c2cdcb21d2bd5c08d350e4bbdfb11dca344b9be"
     "e17e71ee0df3449fd9f9581c6b5483843b457534afb4240585f38ac22aa59a68a167fed6f1be0a5b0"
     "72b2461f16c976b9aa0f5f2f5988818b01faa025ac7788212d92d222f7c14fe6e8f644c8cd117bb8d"
     "ef5a0217dad4f05cbb334ff9ccf819a4a085ed7c19928ddc40edc931b47339f456ccd423b5c0c1cdc"
     "96278006b29de945cdceb0737771e14562fff2aba40606f6046da5031647308682060412812317962"
     "bb68be3b42876f0905d52da51ec6345677fe86613828f488cc5685a4b973e48babd109a56d1a1effb"
     "286133dc2a94b4ada5707d3a7825941fea1a7502693afc7fe5d810bb0050d98aa6b80801e13b56395"
     "4a35c31f57d5ba1ddb1a2be26426e2fe7bcd13ba183d80ac1c556b7ec2069b01de1450431a1c2e278"
     "48e1f5f4af013bce9080aebd2bb0f1de9f7bb460771c266d48ff4cf84a66f82630657db861c032971"
     "079"},
    /* key=32, plaintext=512 */
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
     "f3584606472b260e0dd2ebb2",
     "67c6697351ff4aec29cdbaabf2fbe3467cc254f81be8e78d765a2e63339fc99a66320db73158a35a2"
     "55d051758e95ed4abb2cdc69bb454110e827441213ddc8770e93ea141e1fc673e017e97eadc6b968f"
     "385c2aecb03bfb32af3c54ec18db5c021afe43fbfaaa3afb29d1e6053c7c9475d8be6189f95cbba89"
     "90f95b1ebf1b305eff700e9a13ae5ca0bcbd0484764bd1f231ea81c7b64c514735ac55e4b79633b70"
     "6424119e09dcaad4acf21b10af3b33cde3504847155cbb6f2219ba9b7df50be11a1c7f23f829f8a41"
     "b13b5ca4ee8983238e0794d3d34bc5f4e77facb6c05ac86212baa1a55a2be70b5733b045cd33694b3"
     "afe2f0e49e4f321549fd824ea90870d4b28a2954489a0abcd50e18a844ac5bf38e4cd72d9b0942e50"
     "6c433afcda3847f2dadd47647de321cec4ac430f62023856cfbb20704f4ec0bb920ba86c33e05f1ec"
     "d96733b79950a3e314d3d934f75ea0f210a8f6059401beb4bc4478fa4969e623d01ada696a7e4c7e5"
     "125b34884533a94fb319990325744ee9bbce9e525cf08f5e9e25e5360aad2b2d085fa54d835e8d466"
     "826498d9a8877565705a8a3f62802944de7ca5894e5759d351adac869580ec17e485f18c0c66f17cc"
     "07cbb22fce466da610b63af62bc83b4692f3affaf271693ac071fb86d11342d8def4f89d4b66335c1"
     "c7e4248367d8ed9612ec453902d8e50af89d7709d1a596c1f41f",
     "95aa82ca6c49ae90cd1668baac7aa6f2b4a8ca99b2c2372acb08cf61c9c3805e6e0328da4cd76a19e"
     "dd2d3994c798b0022569ad418d1fee4d9cd45a391c601ffc92ad91501432fee150287617c13629e69"
     "fc7281cd7165a63eab49cf714bce3a75a74f76ea7e64ff81eb61fdfec39b67bf0de98c7e4e32bdf97"
     "c8c6ac75ba43c02f4b2ed7216ecf3014df000108b67cf99505b179f8ed4980a6103d1bca70dbe9bbf"
     "ab0ed59801d6e5f2d6f67d3ec5168e212e2daf02c6b963c98a1f7097de0c56891a2b211b01070dd8f"
     "d8b16c2a1a4e3cfd292d2984b3561d555d16c33ddc2bcf7edde13efe520c7e2abdda44d81881c531a"
     "eeeb66244c3b791ea8acfb6a68",
     "793d34afb982ab70b0e204e1e7243314a19e987d9ab7662f58c3dc6064c9be35667ad53b115c610cf"
     "c229f4e5b3e8aae7aac97ce66d1d20b92da3860701b5006dd1385e173e3af7a5a9bb7da85c0434cd5"
     "5a40fb9c482a0b36f0782846a7f16d05b40a08f0ad9a633f9a1e99e69e6b8039a0f2a91be40f193f4"
     "ce3bed1886dab1b0a6112f91503684c1e5afb938b9497166a7147badd1cc19c73e8b9f22e0dcbd189"
     "96868d7ad47755e677ee6e6ec87094cab7ee35feb96017c474261ba7391b18a72451e6daa7f38e162"
     "358c5d84788c974e614acc362b887c56b756df5aeacdda09b11d35a1f97daaceb5ca1b40a78b6058f"
     "7e1d26ad945be6ef74a8e72729f9ab2e3e7dda88d8f803e26e84a34ac07a7cecf5b6be23a4aa1ac68"
     "97f23169d894d53369b27673cf2438af9c6b53a2fa412c74dc075c617029e571f4c2951b1cdd63d33"
     "765af9d9d20e12430a83784c2bca8603f11521fa97f2e45398b4a385176701c6f416720ca0816bf51"
     "a3e0b4c7a28a89f0616a296423760f0f2f471e1def8a2f43956f79790a6b64dfdbb8159236ebd7fe1"
     "049e8e005e231e5f1936bfdccbda8cf0cb5116af758dfd6732dfa77ac3e6faf0996c13473292da363"
     "f01ddcb6a524dbf1d5d608f57c146173a9b169f979e101fe581f749764fd87119ae301958c8e9a9bf"
     "d16249e564ffbb304bc2ca4c34713a20fb858b47c83ce768e04f149884504c0515345631401f829e3"
     "259"},
    /* key=16, plaintext=293 */
    {"0795d80bc7f40f4d41c280271a2e4f7f", "ff824c906594aff365d3cb1f",
     "1ad4e74d127f935beee57cff920665babe7ce56227377afe570ba786193ded3412d4812453157f42f"
     "afc418c02a746c1232c234a639d49baa8f041c12e2ef540027764568ce49886e0d913e28059a3a485"
     "c6eee96337a30b28e4cd5612c2961539fa6bc5de034cbedc5fa15db844013e0bef276e27ca7a4faf4"
     "7a5c1093bd643354108144454d221b3737e6cb87faac36ed131959babe44af2890cfcc4e23ffa2447"
     "0e689ce0894f5407bb0c8665cff536008ad2ac6f1c9ef8289abd0bd9b72f21c597bda5210cf928c80"
     "5af2dd4a464d52e36819d521f967bba5386930ab5b4cf4c71746d7e6e964673457348e9d71d170d9e"
     "b560bd4bdb779e610ba816bf776231ebd0af5966f5cdab6815944032ab4dd060ad8dab880549e910f"
     "1ffcf6862005432afad",
     "98a47a430d8fd74dc1829a91e3481f8ed024d8ba34c9b903321b04864db333e558ae28653dffb2",
     "3b8f91443480e647473a0a0b03d571c622b7e70e4309a02c9bb7980053010d865e6aec161354dc9f4"
     "81b2cd5213e09432b57ec4e58fbd0a8549dd15c8c4e74a6529f75fad0ce5a9e20e2beeb2f91eb638b"
     "f88999968de438d2f1cedbfb0a1c81f9e8e7362c738e0fddd963692a4f4df9276b7f040979ce874cf"
     "6fa3de26da0713784bdb25e4efcb840554ef5b38b5fe8380549a496bd8e423a7456df6f4ae78a07eb"
     "e2276a8e22fc2243ec4f78abe0c99c733fd67c8c492699fa5ee2289cdd0a8d469bf883520ee74efb8"
     "54bfadc7366a49ee65ca4e894e3335e2b672618d362eee12a577dd8dc2ba55c49c1fc3ad68180e9b1"
     "12d0234d4aa28f5661f1e036450ca6f18be0166676bd80f8a4890c6ddea306fabb7ff3cb2860aa32a"
     "827e3a312912a2dfa70f6bc1c07de238448f2d751bd0cf15bf7"},
    /* key=24, plaintext=293 */
    {"e2e001a36c60d2bf40d69ff5b2b1161ea218db263be16a4e", "84230643130d05425826641e",
     "adb034f3f4a7ca45e2993812d113a9821d50df151af978bccc6d3bc113e15bc0918fb385377dca191"
     "6022ce816d56a332649484043c0fc0f2d37d040182b00a9bbb42ef231f80b48fb3730110d9a4433e3"
     "8c73264c703579a705b9c031b969ec6d98de9f90e9e78b21179c2eb1e061946cd4bbb844f031ecf6e"
     "aac27a4151311adf1b03eda97c9fbae66295f468af4b35faf6ba39f9d8f95873bbc2b51cf3dfec0ed"
     "3c9b850696336cc093b24a8765a936d14dd56edc6bf518272169f75e67b74ba452d0aae90416a997c"
     "8f31e2e9d54ffea296dc69462debc8347b3e1af6a2d53bdfdfda601134f98db42b609df0a08c93475"
     "90c8d86e845bb6373d65a26ab85f67b50569c85401a396b8ad76c2b53ff62bcfbf033e435ef47b9b5"
     "91d05117c6dc681d68e",
     "d5d7316b8fdee152942148bff007c22e4b2022c6bc7be3c18c5f2e52e004e0b5dc12206bf002bd",
     "f2c39423ee630dfe961da81909159dba018ce09b1073a12a477108316af5b7a31f86be6a0548b572d"
     "604bd115ea737dde899e0bd7f7ac9b23e38910dc457551ecc15c814a9f46d8432a1a36097dc1afe27"
     "12d1ba0838fa88cb55d9f65a2e9bece0dbf8999562503989041a2c87d7eb80ef649769d2f4978ce5c"
     "f9664f2bd0849646aa81cb976e45e1ade2f17a8126219e917aadbb4bae5e2c4b3f57bbc7f13fcc807"
     "df7842d9727a1b389e0b749e5191482adacabd812627c6eae2c7a30caf0844ad2a22e08f39edddf0a"
     "e10413e47db433dfe3febbb5a5cec9ade21fbba1e548247579395880b747669a8eb7e2ec0c1bff7fe"
     "d2defdb92b07a14edf07b1bde29c31ab052ff1214e6b5ebbefcb8f21b5d6f8f6e07ee57ad6e14d4e1"
     "42cb3f51bb465ab3a28a2a12f01b7514ad0463f2bde0d71d221"},
    /* key=32, plaintext=293 */
    {"5394e890d37ba55ec9d5f327f15680f6a63ef5279c79331643ad0af6d2623525",
     "815e840b7aca7af3b324583f",
     "8e63067cd15359f796b43c68f093f55fdf3589fc5f2fdfad5f9d156668a617f7091d73da71cdd2078"
     "10e6f71a165d0809a597df9885ca6e8f9bb4e616166586b83cc45f49917fc1a256b8bc7d05c476ab5"
     "c4633e20092619c4747b26dad3915e9fd65238ee4e5213badeda8a3a22f5efe6582d0762532026c89"
     "b4ca26fdd000eb45347a2a199b55b7790e6b1b2dba19833ce9f9522c0bcea5b088ccae68dd99ae020"
     "3c81b9f1dd3181c3e2339e83ccd1526b67742b235e872bea5111772aab574ae7d904d9b6355a79178"
     "e179b5ae8edc54f61f172bf789ea9c9af21f45b783e4251421b077776808f04972a5e801723cf7814"
     "42378ce0e0568f014aea7a882dcbcb48d342be53d1c2ebfb206b12443a8a587cc1e55ca23beca385d"
     "61d0d03e9d84cbc1b0a",
     "0feccdfae8ed65fa31a0858a1c466f79e8aa658c2f3ba93c3f92158b4e30955e1c62580450beff",
     "b69a7e17bb5af688883274550a4ded0d1aff49a0b18343f4b382f745c163f7f714c9206a32a1ff012"
     "427e19431951edd0a755e5f491b0eedfd7df68bbc6085dd2888607a2f998c3e881eb1694109250db2"
     "8291e71f4ad344a125624fb92e16ea9815047cd1111cabfdc9cb8c3b4b0f40aa91d31774009781231"
     "400789ed545404af6c3f76d07ddc984a7bd8f52728159782832e298cc4d529be96d17be898efd83e4"
     "4dc7b0e2efc645849fd2bba61fef0ae7be0dcab233cc4e2b7ba4e887de9c64b97f2a1818aa54371a8"
     "d629dae37975f7784e5e3cc77055ed6e975b1e5f55e6bbacdc9f295ce4ada2c16113cd5b323cf78b7"
     "dde39f4a87aa8c141a31174e3584ccbd380cf5ec6d1dba539928b084fa9683e9c0953acf47cc3ac38"
     "4a2c38914f1da01fb2cfd78905c2b58d36b2574b9df15535d82"},
    {"89c54b0d3bc3c397d5039058c220685f", "bc7f45c00868758d62d4bb4d",
     "582670b0baf5540a3775b6615605bd05", "48d16cda0337105a50e2ed76fd18e114",
     "fc2d4c4eee2209ddbba6663c02765e6955e783b00156f5da0446e2970b877f"},
    {"bad6049678bf75c9087b3e3ae7e72c13", "a0a017b83a67d8f1b883e561",
     "a1be93012f05a1958440f74a5311f4a1", "f7c27b51d5367161dc2ff1e9e3edc6f2",
     "36f032f7e3dc3275ca22aedcdc68436b99a2227f8bb69d45ea5d8842cd08"},
    {"66a3c722ccf9709525650973ecc100a9", "1621d42d3a6d42a2d2bf9494",
     "61fa9dbbed2190fbc2ffabf5d2ea4ff8", "d7a9b6523b8827068a6354a6d166c6b9",
     "fef3b20f40e08a49637cc82f4c89b8603fd5c0132acfab97b5fff651c4"},
    {"562ae8aadb8d23e0f271a99a7d1bd4d1", "f7a5e2399413b89b6ad31aff",
     "bbdc3504d803682aa08a773cde5f231a", "2b9680b886b3efb7c6354b38c63b5373",
     "e2b7e5ed5ff27fc8664148f5a628a46dcbf2015184fffb82f2651c36"},
    {"11754cd72aec309bf52f7687212e8957", "", "", "",
     "250327c674aaf477aef2675748cf6971"},

};

static void test_aes_gcm(TestingT *t, NewCipher new_cipher) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof aes_gcm_tests / sizeof aes_gcm_tests[0]; i++) {
        Slice key = unhex(a, aes_gcm_tests[i].key);
        CipherBlock aes = new_cipher(t, a, key);

        Slice nonce = unhex(a, aes_gcm_tests[i].nonce);
        Slice plaintext = unhex(a, aes_gcm_tests[i].plaintext);
        Slice ad = unhex(a, aes_gcm_tests[i].ad);
        Str result = str_from_cstr(aes_gcm_tests[i].result);
        Int tag_size = (result.len - (Int)strlen(aes_gcm_tests[i].plaintext)) / 2;

        Error err = BURROW_NO_ERROR;
        CipherAEAD aesgcm;
        if (tag_size != 16) {
            /* Non-standard tag sizes. */
            aesgcm = cipher_new_gcm_with_tag_size(a, aes, tag_size, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%v", err);
        } else if (nonce.len == 0) {
            /* A nonce size of 0 is an error, and that is all there is to check. */
            aesgcm = cipher_new_gcm_with_nonce_size(a, aes, 0, &err);
            if (!BURROW_FAILED(err))
                testing_t_fatalf_v(t, "expected error for zero nonce size");
            continue;
        } else if (nonce.len != 12) {
            /* Non-standard nonce sizes. */
            aesgcm = cipher_new_gcm_with_nonce_size(a, aes, nonce.len, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%v", err);
        } else {
            aesgcm = cipher_new_gcm(a, aes, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%v", err);
        }

        Slice ct =
            cipher_aead_seal(aesgcm, a, slice_nil(TYPE_BYTE), nonce, plaintext, ad);
        Str ct_hex = hex_encode_to_string(a, ct);
        if (!str_eq(ct_hex, result)) {
            testing_t_errorf_v(t, "#%d: got %s, want %s", (Int)i, ct_hex, result);
            continue;
        }

        Slice plaintext2 =
            cipher_aead_open(aesgcm, a, slice_nil(TYPE_BYTE), nonce, ct, ad, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: Open failed", (Int)i);
            continue;
        }
        if (!testhash_equal(plaintext, plaintext2)) {
            testing_t_errorf_v(t, "#%d: plaintext's don't match: got %x vs %x", (Int)i,
                               plaintext2, plaintext);
            continue;
        }

        if (ad.len > 0) {
            ((Byte *)ad.p)[0] ^= 0x80;
            err = BURROW_NO_ERROR;
            (void)cipher_aead_open(aesgcm, a, slice_nil(TYPE_BYTE), nonce, ct, ad,
                                   &err);
            if (!BURROW_FAILED(err))
                testing_t_errorf_v(
                    t, "#%d: Open was successful after altering additional data",
                    (Int)i);
            ((Byte *)ad.p)[0] ^= 0x80;
        }

        ((Byte *)nonce.p)[0] ^= 0x80;
        err = BURROW_NO_ERROR;
        (void)cipher_aead_open(aesgcm, a, slice_nil(TYPE_BYTE), nonce, ct, ad, &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d: Open was successful after altering nonce",
                               (Int)i);
        ((Byte *)nonce.p)[0] ^= 0x80;

        ((Byte *)ct.p)[0] ^= 0x80;
        err = BURROW_NO_ERROR;
        (void)cipher_aead_open(aesgcm, a, slice_nil(TYPE_BYTE), nonce, ct, ad, &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d: Open was successful after altering ciphertext",
                               (Int)i);
        ((Byte *)ct.p)[0] ^= 0x80;
    }
    arena_free(&ar);
}

static void TestAESGCM(TestingT *t) {
    test_all_implementations(t, test_aes_gcm);
}

static void test_gcm_invalid_tag_size(TestingT *t, NewCipher new_cipher) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CipherBlock aes = new_cipher(t, a, unhex(a, "ab72c77b97cb5fe9a382d9fe81ffdbed"));
    Int sizes[] = {0, 1, cipher_block_block_size(aes) + 1};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        Error err = BURROW_NO_ERROR;
        CipherAEAD aesgcm = cipher_new_gcm_with_tag_size(a, aes, sizes[i], &err);
        if (aesgcm.vt != NULL || !BURROW_FAILED(err))
            testing_t_fatalf_v(
                t, "NewGCMWithTagSize was successful with an invalid %d-byte tag size",
                sizes[i]);
    }
    arena_free(&ar);
}

static void TestGCMInvalidTagSize(TestingT *t) {
    test_all_implementations(t, test_gcm_invalid_tag_size);
}

/* The AES-NI GCM code in Go decrypts and authenticates at the same time, so it
 * writes the output before it knows the tag is bad. Every implementation is
 * meant to behave the same way, and then to zero what it wrote. */
static void test_tag_failure_overwrite(TestingT *t, NewCipher new_cipher) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice key = unhex(a, "ab72c77b97cb5fe9a382d9fe81ffdbed");
    Slice nonce = unhex(a, "54cc7dc2c37ec006bcc6d1db");
    Slice ciphertext =
        unhex(a, "0e1bde206a07a9c2c1b65300f8c649972b4401346697138c7a4891ee59867d0c");

    CipherBlock aes = new_cipher(t, a, key);
    Error err = BURROW_NO_ERROR;
    CipherAEAD aesgcm = cipher_new_gcm(a, aes, &err);

    Slice dst = testhash_bytes(a, ciphertext.len - 16);
    memset(dst.p, 42, (size_t)dst.len);

    Slice result = cipher_aead_open(aesgcm, a, slice_sub(dst, 0, 0), nonce, ciphertext,
                                    slice_nil(TYPE_BYTE), &err);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Bad Open still resulted in nil error.");
    if (!slice_is_nil(result))
        testing_t_fatalf_v(t, "Failed Open returned non-nil result.");
    for (Int i = 0; i < dst.len; i++)
        if (((const Byte *)dst.p)[i] != 0)
            testing_t_fatalf_v(t, "Failed Open didn't zero dst buffer");
    arena_free(&ar);
}

static void TestTagFailureOverwrite(TestingT *t) {
    test_all_implementations(t, test_tag_failure_overwrite);
}

/* That the last 32 bits of the counter wrap correctly. */
static void test_gcm_counter_wrap(TestingT *t, NewCipher new_cipher) {
    static const struct {
        const char *nonce, *tag;
    } tests[] = {
        {"0fa72e25",
         "37e1948cdfff09fbde0c40ad99fee4a7"}, /* 7eb59e4d961dad0dfdd75aaffffffff0 */
        {"afe05cc1",
         "438f3aa9fee5e54903b1927bca26bbdf"}, /* 75d492a7e6e6bfc979ad3a8ffffffff4 */
        {"9ffecbef",
         "7b88ca424df9703e9e8611071ec7e16e"}, /* c8bb108b0ecdc71747b9d57ffffffff5 */
        {"ffc3e5b3",
         "38d49c86e0abe853ac250e66da54c01a"}, /* 706414d2de9b36ab3b900a9ffffffff6 */
        {"cfdd729d",
         "e08402eaac36a1a402e09b1bd56500e8"}, /* cd0b96fe36b04e750584e56ffffffff7 */
        {"010ae3d486",
         "5405bb490b1f95d01e2ba735687154bc"}, /* e36c18e69406c49722808104fffffff8 */
        {"01b1107a9d",
         "939a585f342e01e17844627492d44dbf"}, /* e6d56eaf9127912b6d62c6dcffffffff */
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CipherBlock key = new_cipher(t, a, testhash_bytes(a, 16));
    Slice plaintext = testhash_bytes(a, 16 * 17 + 1);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Slice nonce = unhex(a, tests[i].nonce);
        Slice want = unhex(a, tests[i].tag);
        Error err = BURROW_NO_ERROR;
        CipherAEAD aead = cipher_new_gcm_with_nonce_size(a, key, nonce.len, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%v", err);
        Slice got = cipher_aead_seal(aead, a, slice_nil(TYPE_BYTE), nonce, plaintext,
                                     slice_nil(TYPE_BYTE));
        Slice tag = slice_sub(got, plaintext.len, got.len);
        if (!testhash_equal(tag, want))
            testing_t_errorf_v(t, "test[%v]: got: %x, want: %x", (Int)i, tag, want);
        (void)cipher_aead_open(aead, a, slice_nil(TYPE_BYTE), nonce, got,
                               slice_nil(TYPE_BYTE), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "test[%v]: authentication failed", (Int)i);
    }
    arena_free(&ar);
}

static void TestGCMCounterWrap(TestingT *t) {
    test_all_implementations(t, test_gcm_counter_wrap);
}

/* One AEAD over the AES block and one over the same block wrapped. */
static bool new_aes_gcm(Alloc *a, Slice key, CipherAEAD *fast, CipherAEAD *generic) {
    Error err = BURROW_NO_ERROR;
    CipherBlock block = aes_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        return false;
    *fast = cipher_new_gcm(a, block, &err);
    if (BURROW_FAILED(err))
        return false;
    *generic = cipher_new_gcm(a, wrap(a, block), &err);
    return !BURROW_FAILED(err);
}

static const char *gcm_asm_test(Alloc *a, Int ks, Slice pt, Slice ad) {
    Slice key = testhash_bytes(a, ks);
    Slice nonce = testhash_bytes(a, 12);
    crypto_rand_read(key, NULL);
    crypto_rand_read(pt, NULL);
    crypto_rand_read(ad, NULL);
    crypto_rand_read(nonce, NULL);
    CipherAEAD fast, generic;
    if (!new_aes_gcm(a, key, &fast, &generic))
        return "NewGCM failed";
    Slice want = cipher_aead_seal(generic, a, slice_nil(TYPE_BYTE), nonce, pt, ad);
    Slice got = cipher_aead_seal(fast, a, slice_nil(TYPE_BYTE), nonce, pt, ad);
    if (!testhash_equal(want, got))
        return "incorrect Seal output";
    Error err = BURROW_NO_ERROR;
    got = cipher_aead_open(fast, a, slice_nil(TYPE_BYTE), nonce, want, ad, &err);
    if (BURROW_FAILED(err))
        return "authentication failed";
    if (!testhash_equal(pt, got))
        return "incorrect Open output";
    return NULL;
}

static void TestGCMAsm(TestingT *t) {
    static const Int lengths[] = {0, 156, 8192, 8193, 8208};
    static const Int all_key_sizes[] = {16, 24, 32};
    static const Int all_alignments[] = {0, 1, 2, 3};
    static const Int short_key_sizes[] = {16};
    static const Int short_alignments[] = {1};
    const Int *key_sizes = all_key_sizes, *alignments = all_alignments;
    Int nkey_sizes = 3, nalignments = 4;
    if (testing_short()) {
        key_sizes = short_key_sizes;
        alignments = short_alignments;
        nkey_sizes = 1;
        nalignments = 1;
    }

    struct {
        Int align, length;
    } perms[5 * 4];
    Int nperms = 0;
    for (Int l = 0; l < 5; l++) {
        for (Int i = 0; i < nalignments; i++) {
            if (alignments[i] != 0 && lengths[l] == 0)
                continue;
            perms[nperms].align = alignments[i];
            perms[nperms].length = lengths[l];
            nperms++;
        }
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < nperms; i++) {
        Slice ad = testhash_bytes(a, perms[i].align + perms[i].length);
        ad = slice_sub(ad, perms[i].align, ad.len);
        for (Int j = 0; j < nperms; j++) {
            Slice pt = testhash_bytes(a, perms[j].align + perms[j].length);
            pt = slice_sub(pt, perms[j].align, pt.len);
            for (Int k = 0; k < nkey_sizes; k++) {
                ArenaMark m = arena_mark(&ar);
                const char *msg = gcm_asm_test(a, key_sizes[k], pt, ad);
                arena_release(&ar, m);
                if (msg != NULL) {
                    testing_t_errorf_v(t, "%s", str_from_cstr(msg));
                    testing_t_errorf_v(t, "\tkey size: %v", key_sizes[k]);
                    testing_t_errorf_v(t, "\tplaintext alignment: %v", perms[j].align);
                    testing_t_errorf_v(t, "\tplaintext length: %v", perms[j].length);
                    testing_t_errorf_v(t, "\tadditionalData alignment: %v",
                                       perms[i].align);
                    testing_t_fatalf_v(t, "\tadditionalData length: %v",
                                       perms[i].length);
                }
            }
        }
    }
    arena_free(&ar);
}

/* The makers TestGCMAEAD hands to testcipher_aead. */
typedef struct GcmMake {
    CipherBlock block;
    int kind;
    Int size;
} GcmMake;

enum { GCM_STANDARD, GCM_TAG_SIZE, GCM_NONCE_SIZE, GCM_RANDOM_NONCE };

static CipherAEAD gcm_make(Alloc *a, void *env, Error *err) {
    const GcmMake *m = env;
    switch (m->kind) {
    case GCM_TAG_SIZE:
        return cipher_new_gcm_with_tag_size(a, m->block, m->size, err);
    case GCM_NONCE_SIZE:
        return cipher_new_gcm_with_nonce_size(a, m->block, m->size, err);
    case GCM_RANDOM_NONCE:
        return cipher_new_gcm_with_random_nonce(a, m->block, err);
    default:
        return cipher_new_gcm(a, m->block, err);
    }
}

static void gcm_aead_one(void *env, TestingT *t) {
    testcipher_aead(t, gcm_make, env);
}

static void gcm_aead_random_nonce(void *env, TestingT *t) {
    GcmMake *m = env;
    if (m->block.vt == &wrapper_vt)
        testing_t_skip_v(t, "NewGCMWithRandomNonce requires an AES block cipher");
    testcipher_aead(t, gcm_make, m);
}

typedef struct GcmAeadEnv {
    NewCipher new_cipher;
    Int key_size;
} GcmAeadEnv;

static void gcm_aead_key_size(void *env, TestingT *t) {
    const GcmAeadEnv *e = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand rng = testhash_new_rand(t);
    Slice key = testhash_bytes(a, e->key_size / 8);
    testhash_read(&rng, key);
    CipherBlock block = e->new_cipher(t, a, key);

    /* The standard nonce and tag sizes. */
    GcmMake standard = {block, GCM_STANDARD, 0};
    testcipher_aead(t, gcm_make, &standard);

    /* Non-standard tag sizes. */
    GcmMake min_tag = {block, GCM_TAG_SIZE, 12};
    testing_t_run(t, BURROW_S("MinTagSize"),
                  BURROW_FN(TestingTFunc, gcm_aead_one, &min_tag));

    /* Non-standard nonce sizes. */
    static const Int nonce_sizes[] = {1, 16, 100};
    for (size_t i = 0; i < sizeof nonce_sizes / sizeof nonce_sizes[0]; i++) {
        GcmMake m = {block, GCM_NONCE_SIZE, nonce_sizes[i]};
        testing_t_run(t, fmt_sprintf_v(a, "NonceSize-%d", nonce_sizes[i]),
                      BURROW_FN(TestingTFunc, gcm_aead_one, &m));
    }

    GcmMake random = {block, GCM_RANDOM_NONCE, 0};
    testing_t_run(t, BURROW_S("GCMWithRandomNonce"),
                  BURROW_FN(TestingTFunc, gcm_aead_random_nonce, &random));
    arena_free(&ar);
}

/* GCM against the general cipher.AEAD tester, with AES at each key size. */
static void test_gcm_aead(TestingT *t, NewCipher new_cipher) {
    static const Int key_sizes[] = {128, 192, 256};
    for (size_t i = 0; i < 3; i++) {
        GcmAeadEnv e = {new_cipher, key_sizes[i]};
        Str name = key_sizes[i] == 128   ? BURROW_S("AES-128")
                   : key_sizes[i] == 192 ? BURROW_S("AES-192")
                                         : BURROW_S("AES-256");
        testing_t_run(t, name, BURROW_FN(TestingTFunc, gcm_aead_key_size, &e));
    }
}

static void TestGCMAEAD(TestingT *t) {
    test_all_implementations(t, test_gcm_aead);
}

/* Not Go's: the random nonce AEAD's two refusals, and that what it seals
 * starts with the nonce it picked. */
static void TestGCMWithRandomNonce(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CipherBlock block = new_aes(t, a, testhash_bytes(a, 16));

    Error err = BURROW_NO_ERROR;
    CipherAEAD w = cipher_new_gcm_with_random_nonce(a, wrap(a, block), &err);
    if (w.vt != NULL || !BURROW_FAILED(err))
        testing_t_errorf_v(t, "NewGCMWithRandomNonce accepted a block that is not AES");
    else if (!str_eq(error_text(err),
                     BURROW_S("cipher: NewGCMWithRandomNonce requires aes.Block")))
        testing_t_errorf_v(t, "NewGCMWithRandomNonce error = %q", error_text(err));

    err = BURROW_NO_ERROR;
    CipherAEAD r = cipher_new_gcm_with_random_nonce(a, block, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    if (cipher_aead_nonce_size(r) != 0 || cipher_aead_overhead(r) != 28)
        testing_t_errorf_v(t, "NonceSize, Overhead = %d, %d, want 0, 28",
                           cipher_aead_nonce_size(r), cipher_aead_overhead(r));

    Slice pt = slice_from_str(a, BURROW_S("plaintext"));
    Slice ct = cipher_aead_seal(r, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE), pt,
                                slice_nil(TYPE_BYTE));
    CipherAEAD plain = cipher_new_gcm(a, block, &err);
    Slice got = cipher_aead_open(plain, a, slice_nil(TYPE_BYTE), slice_sub(ct, 0, 12),
                                 slice_sub(ct, 12, ct.len), slice_nil(TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || !testhash_equal(got, pt))
        testing_t_errorf_v(t, "the first 12 bytes of Seal's output are not its nonce");
    arena_free(&ar);
}

/* --------------------------------------------------------------- benchmarks */

/* From Go's benchmark_test.go. */
typedef struct GcmBench {
    Int size;
    bool open;
    Int key_size;
} GcmBench;

static void benchmark_aes_gcm(void *env, TestingB *b) {
    const GcmBench *g = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice buf = testhash_bytes(a, g->size);
    testing_b_set_bytes(b, (int64_t)buf.len);

    Slice key = testhash_bytes(a, g->key_size);
    Error err = BURROW_NO_ERROR;
    CipherBlock aes = aes_new_cipher(a, key, &err);
    CipherAEAD aesgcm = cipher_new_gcm(a, aes, &err);
    Slice nonce = testhash_bytes(a, cipher_aead_nonce_size(aesgcm));
    Byte ad_bytes[13] = {0};
    Slice ad = slice_from(ad_bytes, 13, 13, TYPE_BYTE);
    Slice out = testhash_bytes(a, buf.len + cipher_aead_overhead(aesgcm));
    Slice ct = cipher_aead_seal(aesgcm, a, slice_sub(out, 0, 0), nonce, buf, ad);
    Slice open_out = testhash_bytes(a, buf.len);

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        if (g->open) {
            (void)cipher_aead_open(aesgcm, a, slice_sub(open_out, 0, 0), nonce, ct, ad,
                                   &err);
        } else {
            (void)cipher_aead_seal(aesgcm, a, slice_sub(out, 0, 0), nonce, buf, ad);
        }
    }
    arena_free(&ar);
}

static void BenchmarkAESGCM(TestingB *b) {
    static const Int lengths[] = {64, 1350, 8 * 1024};
    static GcmBench cases[3 * 4];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n = 0;
    for (size_t i = 0; i < 3; i++) {
        GcmBench open128 = {lengths[i], true, 16};
        GcmBench seal128 = {lengths[i], false, 16};
        GcmBench open256 = {lengths[i], true, 32};
        GcmBench seal256 = {lengths[i], false, 32};
        cases[n] = open128;
        testing_b_run(b, fmt_sprintf_v(a, "Open-128-%d", lengths[i]),
                      BURROW_FN(TestingBFunc, benchmark_aes_gcm, &cases[n++]));
        cases[n] = seal128;
        testing_b_run(b, fmt_sprintf_v(a, "Seal-128-%d", lengths[i]),
                      BURROW_FN(TestingBFunc, benchmark_aes_gcm, &cases[n++]));
        cases[n] = open256;
        testing_b_run(b, fmt_sprintf_v(a, "Open-256-%d", lengths[i]),
                      BURROW_FN(TestingBFunc, benchmark_aes_gcm, &cases[n++]));
        cases[n] = seal256;
        testing_b_run(b, fmt_sprintf_v(a, "Seal-256-%d", lengths[i]),
                      BURROW_FN(TestingBFunc, benchmark_aes_gcm, &cases[n++]));
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestAESGCM)                                                                      \
    X(TestGCMInvalidTagSize)                                                           \
    X(TestTagFailureOverwrite)                                                         \
    X(TestGCMCounterWrap)                                                              \
    X(TestGCMAsm)                                                                      \
    X(TestGCMAEAD)                                                                     \
    X(TestGCMWithRandomNonce)                                                          \
    X(BenchmarkAESGCM)

TESTING_MAIN(TESTS)
