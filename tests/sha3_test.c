/* Derived from Go's src/crypto/sha3/sha3_test.go.
 * Go source: go1.27.1.
 *
 * Go's file has no table of known answers, since cryptotest and the ACVP
 * wrapper check those, so TestGolden here is a table that Go's crypto/sha3
 * produced, for every function this package has. TestClone and
 * TestUnmarshalErrors are not in Go's file either: the first stands in for the
 * Clone half of cryptotest.TestHash and the second checks the three errors
 * UnmarshalBinary can give. TestAllocations has nothing to check here, since
 * nothing allocates except what is asked for.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testhash.h"

#include "burrow/burrow.h"
#include "burrow/crypto/sha3.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"

#include <string.h>

static const char test_string[] = "brekeccakkeccak koax koax";

typedef struct Sha3Golden {
    const char *rep; /* the input is rep, count times */
    Int count;
    const char *sum224, *sum256, *sum384, *sum512;
    const char *shake128, *shake256;   /* 32 and 64 bytes */
    const char *cshake128, *cshake256; /* the same, with Go's N and S below */
} Sha3Golden;

static const Sha3Golden golden[] = {
    {"", 1, "6b4e03423667dbb73b6e15454f0eb1abd4597f9a1b078e3f5b5a6bc7",
     "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
     "0c63a75b845e4f7d01107d852e4c2485c51a50aaaa94fc61995e71bbee983a2ac3713831264adb47f"
     "b6bd1e058d5f004",
     "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b2123af1f5f94c1"
     "1e3e9402c3ac558f500199d95b6d3e301758586281dcd26",
     "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26",
     "46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762fd75dc4ddd8c0f200c"
     "b05019d67b592f6fc821c49479ab48640292eacb3b7c4be",
     "048b83542024f3cc044374841e9e482dda0638299717d21b5ce7bf691b9c402f",
     "28fb914e8ff378887e204d6cbd4051bd5be2bf6cdb50e158bdcaec86427ce52ecdaa78cc498735a4b"
     "e6277256fa1e28539cda41991e05f3e0a706ab8dfd2907a"},
    {"a", 1, "9e86ff69557ca95f405f081269685b38e3a819b309ee942f482b6a8b",
     "80084bf2fba02475726feb2cab2d8215eab14bc6bdd8bfb2c8151257032ecd8b",
     "1815f774f320491b48569efec794d249eeb59aae46d22bf77dafe25c5edc28d7ea44f93ee1234aa88"
     "f61c91912a4ccd9",
     "697f2d856172cb8309d6b8b97dac4de344b549d4dee61edfb4962d8698b7fa803f4f93ff24393586e"
     "28b5b957ac3d1d369420ce53332712f997bd336d09ab02a",
     "85c8de88d28866bf0868090b3961162bf82392f690d9e4730910f4af7c6ab3ee",
     "867e2cb04f5a04dcbd592501a5e8fe9ceaafca50255626ca736c138042530ba436b7b1ec0e06a279b"
     "c790733bb0aee6fa802683c7b355063c434e91189b0c651",
     "e5b73596ec166f9c03d6cb53c06634d6ec3d36ff290f586997f57c3a94890028",
     "1b1a6bb964b1df17c846c471eebc6aaeadfa039c6c7ed19c556aebc8c0cb15e58175458be7ce58bff"
     "37e6314f2d41eb25460cf6e66d23129334f77e6224ffe8d"},
    {"abc", 1, "e642824c3f8cf24ad09234ee7d3c766fc9a3a5168d0c94ad73b46fdf",
     "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532",
     "ec01498288516fc926459f58e2c6ad8df9b473cb0fc08c2596da7cf0e49be4b298d88cea927ac7f53"
     "9f1edf228376d25",
     "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91"
     "a7ec57647e3934057340b4cf408d5a56592f8274eec53f0",
     "5881092dd818bf5cf8a3ddb793fbcba74097d5c526a6d35f97b83351940f2cc8",
     "483366601360a8771c6863080cc4114d8db44530f8f1e1ee4f94ea37e78b5739d5a15bef186a5386c"
     "75744c0527e1faa9f8726e462a12a4feb06bd8801e751e4",
     "efb240a8d878539bdd5404ac7e0628f822fdf6c7ae4eef202564958a3cf56e07",
     "4465b8bd9a7e8fea02c9bb187b7d5996095a78cf178ed4b6b1ea8540fe6f0bdb1f6fec4a9ee84f408"
     "6566f917fcfeb0ed735cce1814d0f589b27a27b13f5c92a"},
    {"abcdefghijklmnopqrstuvwxyz", 1,
     "5cdeca81e123f87cad96b9cba999f16f6d41549608d4e0f4681b8239",
     "7cab2dc765e21b241dbc1c255ce620b29f527c6d5e7f5f843e56288f0d707521",
     "fed399d2217aaf4c717ad0c5102c15589e1c990cc2b9a5029056a7f7485888d6ab65db2370077a5ca"
     "db53fc9280d278f",
     "af328d17fa28753a3c9f5cb72e376b90440b96f0289e5703b729324a975ab384eda565fc92aaded14"
     "3669900d761861687acdc0a5ffa358bd0571aaad80aca68",
     "961c919c0854576e561320e81514bf3724197d0715e16a364520384ee997f6ef",
     "b7b78b04a3dd30a265c8886c33fda94799853de5d3d10541fd4e9f4613701c61075249bed16b07811"
     "08fcfe086dbf38a7fb8300807cea85cc649328d07d4ff2b",
     "1f5e9a614c605ae289245f89c8a48901cc01f84710207153ae1821f0c4d6bca6",
     "cc7595b1772de813ebba99e5bce01d318871294dbeef4c1709dba6640fee05263ca6818ecdd2a54db"
     "3f57fd46a9e587b87c374fc4dde16d85f0a7574d9c55b2e"},
    {"The quick brown fox jumps over the lazy dog", 1,
     "d15dadceaa4d5d7bb3b48f446421d542e08ad8887305e28d58335795",
     "69070dda01975c8c120c3aada1b282394e7f032fa9cf32f4cb2259a0897dfc04",
     "7063465e08a93bce31cd89d2e3ca8f602498696e253592ed26f07bf7e703cf328581e1471a7ba7ab1"
     "19b1a9ebdf8be41",
     "01dedd5de4ef14642445ba5f5b97c15e47b9ad931326e4b0727cd94cefc44fff23f07bf543139939b"
     "49128caf436dc1bdee54fcb24023a08d9403f9b4bf0d450",
     "f4202e3c5852f9182a0430fd8144f0a74b95e7417ecae17db0f8cfeed0e3e66e",
     "2f671343d9b2e1604dc9dcf0753e5fe15c7c64a0d283cbbf722d411a0e36f6ca1d01d1369a23539cd"
     "80f7c054b6e5daf9c962cad5b8ed5bd11998b40d5734442",
     "091fd9c891ca9b81149a3feb016d917e93e5fb7e598729b0e35725b2f3027b07",
     "36784f0440a270cd14a87cf6141bcb847cd07855118eefa2a35fd8af522291e54136456443d2ac18d"
     "24b45fc55643fb682c9c28f0621f0af2e305a334cfa66df"},
    {"a", 135, "f9f28c21a2b0884bbd3594cae82bf811c0c1ede427e083d5576e909d",
     "8094bb53c44cfb1e67b7c30447f9a1c33696d2463ecc1d9c92538913392843c9",
     "a2d51907c0611e25c058f0675042e8f53cc473dc347c5ea8a813d886b3aa8f8dcab61a236237d94de"
     "404cd66606243f9",
     "4be1e70276f9122f470a54c27240c7d0709dab7469958b48a950d69da6dd07ca135826d9d23e975cb"
     "9283e7d236ef98a80451dca8e311f52096308b2c8d70cc7",
     "a5e2b2278d1b75866c7877a0ffa24737e91def84e20944b23f1854012e29148a",
     "55b991ece1e567b6e7c2c714444dd201cd51f4f3832d08e1d26bebc63e07a3d7ddeed4a5aa6df7a15"
     "f89f2050566f75d9cf1a4dea4ed1f578df0985d5706d49e",
     "f8036c7be6844978e8dd53ef0e4c1c19d48cdfeeafca6b0d2db0a0a5edf5d65f",
     "0b258a402711a6d8dab484566b52afb007d288fca6978bb544905496d7894ff584402593352092c4a"
     "5a955b5250e73f5cdef32a1fce4953c61e58cec922d791d"},
    {"a", 136, "96136a6a094433b4aa855f163829a2ce6bca7d56cfd2163b47f1f1c4",
     "3fc5559f14db8e453a0a3091edbd2bc25e11528d81c66fa570a4efdcc2695ee1",
     "cbbcb466417a2f6d466479bb6dc659434d9589de3a53acc9b427580482e305948888c8fa6d069c5e6"
     "a899aa34a9af15a",
     "e50392c91ed95768c8dcf52a12e5db1ecd0347fb995f7ff4ea06994649bbd1a0de7ae36a62aadc00a"
     "704d730b52bda191b72951e2afc9b6fb6824787b2086257",
     "0d0158d446783a9b18a6908c08bb5de6f9aab1be71b56b11a4b1c9cbb4d0f422",
     "8fcc5a08f0a1f6827c9cf64ee8d16e0443106359ca6c8efd230759256f44996a703c7fa566b8308f7"
     "050f4c717418c5ef75f512d1ba01f4f1ff5984e1bc89efd",
     "63d9bf6edd87c1a4ace9337a7cc872510d2ab94c74dd572b2c6631bdcaeeb78c",
     "d92ed34d133fae5e940bde632bda0d1abeeca9b2fabae35f74a1a4008cb222fdec625ef1b5e217c42"
     "1d8b9edb9673cc58579f2bd2ff2b10f33d38271e02a1240"},
    {"a", 137, "d0c9e8452b199b5149b9d06ec79e70ccd82ffa317bf61196f12b7207",
     "f8d6846cedd2ccfadf15c5879ef95af724d799eed7391fb1c91f95344e738614",
     "8a9e401af96cfcdc6ee9e848a2ba4d94be808a753e7673df1252c9706fdef18943dbc7487cdecca0e"
     "fcfef152891ab03",
     "c1a51bff785ff8443c873d0f9b9534222f99b476b357091b00f52bcbf214be6c9febe2ab320f6f24c"
     "9d770d4ed2708611b4d6f3c03bcd7aec27a1d1d6b5f8768",
     "4f2d1aa440b032179a015caa08f16a3b88fdb00cadf9caf3486f542f1d9e76a6",
     "a44e1a438dad6273d540be65ee26386c59588efb09139dc086385d2db0c257821b522ae4b16246bcd"
     "0f4ef921a1883ccce79f29a70192e9085e9d282bc12b326",
     "b6adf6d087f77c3ed454dbd5c6c54110e867e1957da3019646ae7746f0b1e108",
     "1154246844210754c070502ccae6af77be1e21c5f89cd8d759fea2b3cf54001704d5539c14b87f36b"
     "2231282d75eb257f60a64291c93be8fed7a88c78b83e7fb"},
    {"a", 167, "f5e4ec9dbfaa88f80792cf0ff2a94e5406d5472461236be9a6ce337f",
     "421a819d6eb16a424962dbfae34bf368c70a669a0ca8565d1161ad7a84c730b6",
     "17e14182639e77e6825182895bdc1ea9d745bbe2dad68eb41e6141cc0377b0851dbc77898be6c28ba"
     "e9cb79d67db9bac",
     "901471cac45c8801e8ccf63e269ced774937e05e23c193dc24b63627d837c9ce35598f1277d25a278"
     "2c33e3a8e4b530e2f5711c887fad29cefc5b6132bb58696",
     "4f5c6c53ae8190a8ff8a55b2125d28703052d10278570960c2066a905d916c34",
     "8526df0997fe3ed14212114d4aadaec80bf53089aded73544b4972da4059cdc35fdf4f1acd1dd4d2b"
     "c0ff2e2ce0e0fc9f3a47b3f8dae6c73391083433daea865",
     "d001f6487a7a7b302f28581341572f1fc6085b9af81d52348c2e8ee1a4d344ad",
     "a4654086e15cbc7f0b42abc58e52a21a871d6e61aaeaa2737a172b297c64672684a60e579e74f1a27"
     "b9b70a548e1907cff538a795c3f86527a4ad21e4699feff"},
    {"a", 168, "b5d9317fa0b59cffa6e8dc1d38f159a6aca7452cc4338539b4622be4",
     "c52d6aca1cfca7d65381a876ec63388df4213032e871f4345d997f57e65456dc",
     "8fb1adb926137828713e62c4fd0e04618eb2d56d70d8f0e2cc9c3049ef00117a8bdcd47cf9135a6b1"
     "d2665b4650d862f",
     "a670c29284c90471f56b22f912e004858dfccb1dd5e15b6e8c6b729173301cdcf93fe59391a19a2a1"
     "78b9d8e4196d4bb83f8f8b193b1d73c5ddd0b0b072a242e",
     "c22e11586c22b713bde373fce93314d76829de2c21d940a28eb659b8dec953a2",
     "59c2280a81850afb30402f81a1df0eff8eefd3f7af8b038c2705065677e34531f7ea8aadc85a1fc3c"
     "907e16e883445e3ac952847efb94c4f30a1121e5d254269",
     "541cb37327c42cd9c7b2d05481211b9ed35b7fe804bcb8d849e7fb307a1807ea",
     "4720cb8226491fe87d2499ea5bbd17cde9ea40cb279a74dfd12d5e551333ff8ea6fe40ddfa1ce69a9"
     "eba8413717d45ebe296e166987b0a1fd522073cf0d00ff1"},
    {"0123456789", 100, "55998fdac382f991caa0e13c578c819da3b383ef97f3f0a07fcb0b46",
     "118903585de22d50a3aa140b1a7efb15e4bbfbb637085db6295aa3730b120922",
     "18356d36983de995c251a853b711adc06cbf19a66e5dfc6eecbb14351f881e2444a4ae10934895155"
     "49432f8b4ab0f5f",
     "570e5aefbab0d6c94f67597a946b5683a35f65c5cacb59caba4dd481ea728033e499dddcbb18df17f"
     "96cb1f8484ee69e09cc3cb05976fcad59fe0021a63ef70d",
     "509d35149bdfb08db8cab7ade858359290d7d5d0e8c6e46b7205c523dcafa2b8",
     "0ef470c0dc75c32b84eebd102bb8a7f550d34719d113816029d2924076855702d5f72457db90058c6"
     "f5c0d93dbf60a28a1352b365015c2af8147270a38ae850c",
     "ff6ddef9998b10b043859c06774c3f7c701489a140c1f6c9260860059d369737",
     "c195327afacea75c7b5084d59eaf0f31073a2ed873019c7a6d8bfc1e133579eac2b0be7a31e8ab17a"
     "cb4ec4b32c462682144778a1ca5aefaba01ddf658e4a6ac"},
};

static Slice bytes_of(const char *s) {
    return slice_from((void *)(uintptr_t)s, (Int)strlen(s), (Int)strlen(s), TYPE_BYTE);
}

static Hash new224(Alloc *a) {
    return sha3_as_hash(sha3_new224(a));
}

static Hash new256(Alloc *a) {
    return sha3_as_hash(sha3_new256(a));
}

static Hash new384(Alloc *a) {
    return sha3_as_hash(sha3_new384(a));
}

static Hash new512(Alloc *a) {
    return sha3_as_hash(sha3_new512(a));
}

static Hash new_zero(Alloc *a) {
    Sha3 *d = BURROW_NEW(a, Sha3);
    return sha3_as_hash(d);
}

typedef struct Digest {
    const char *name;
    TesthashMake make;
} Digest;

static const Digest test_digests[] = {
    {"SHA3-224", new224}, {"SHA3-256", new256},    {"SHA3-384", new384},
    {"SHA3-512", new512}, {"SHA3-Zero", new_zero},
};

typedef Sha3SHAKE *(*NewCSHAKE)(Alloc *a, Slice n, Slice s);

static Sha3SHAKE *new_zero_shake(Alloc *a, Slice n, Slice s) {
    (void)n;
    (void)s;
    return BURROW_NEW(a, Sha3SHAKE);
}

typedef struct Shake {
    const char *name;
    NewCSHAKE constructor;
    const char *def_algo_name;
    const char *def_custom_str;
} Shake;

static const Shake test_shakes[] = {
    /* NewCSHAKE without customization produces same result as SHAKE */
    {"SHAKE128", sha3_new_cshake128, "", ""},
    {"SHAKE256", sha3_new_cshake256, "", ""},
    {"cSHAKE128", sha3_new_cshake128, "CSHAKE128", "CustomString"},
    {"cSHAKE256", sha3_new_cshake256, "CSHAKE256", "CustomString"},
    {"SHAKE-Zero", new_zero_shake, "", ""},
};

static Sha3SHAKE *make_shake(Alloc *a, const Shake *v) {
    return v->constructor(a, bytes_of(v->def_algo_name), bytes_of(v->def_custom_str));
}

#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

static void check_hex(TestingT *t, Alloc *a, const char *what, Int row, Slice got,
                      const char *want) {
    Str s = hex_encode_to_string(a, got);
    if (!str_eq(s, str_from_cstr(want)))
        testing_t_errorf_v(t, "%s, row %d: got %s, want %s", str_from_cstr(what), row,
                           s, str_from_cstr(want));
}

static void TestGolden(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < NELEM(golden); i++) {
        const Sha3Golden *g = &golden[i];
        Int row = (Int)i;
        Slice in = testhash_repeat(a, g->rep, (Int)strlen(g->rep), g->count, "");

        Sha3Sum224Ret s224 = sha3_sum224(in);
        Sha3Sum256Ret s256 = sha3_sum256(in);
        Sha3Sum384Ret s384 = sha3_sum384(in);
        Sha3Sum512Ret s512 = sha3_sum512(in);
        check_hex(t, a, "Sum224", row, slice_from(s224.a, 28, 28, TYPE_BYTE),
                  g->sum224);
        check_hex(t, a, "Sum256", row, slice_from(s256.a, 32, 32, TYPE_BYTE),
                  g->sum256);
        check_hex(t, a, "Sum384", row, slice_from(s384.a, 48, 48, TYPE_BYTE),
                  g->sum384);
        check_hex(t, a, "Sum512", row, slice_from(s512.a, 64, 64, TYPE_BYTE),
                  g->sum512);
        check_hex(t, a, "SumSHAKE128", row, sha3_sum_shake128(a, in, 32), g->shake128);
        check_hex(t, a, "SumSHAKE256", row, sha3_sum_shake256(a, in, 64), g->shake256);

        /* The same through the types, written a byte at a time and then all
         * at once. */
        const char *want[] = {g->sum224, g->sum256, g->sum384, g->sum512, g->sum256};
        for (size_t j = 0; j < NELEM(test_digests); j++) {
            Hash h = test_digests[j].make(a);
            for (Int k = 0; k < in.len; k++)
                hash_write(h, slice_sub(in, k, k + 1), NULL);
            check_hex(t, a, test_digests[j].name, row,
                      hash_sum(a, h, slice_nil(TYPE_BYTE)), want[j]);
            hash_reset(h);
            hash_write(h, in, NULL);
            check_hex(t, a, test_digests[j].name, row,
                      hash_sum(a, h, slice_nil(TYPE_BYTE)), want[j]);
        }

        const char *xwant[] = {g->shake128, g->shake256, g->cshake128, g->cshake256,
                               g->shake256};
        Int xlen[] = {32, 64, 32, 64, 64};
        for (size_t j = 0; j < NELEM(test_shakes); j++) {
            Sha3SHAKE *x = make_shake(a, &test_shakes[j]);
            sha3_shake_write(x, in, NULL);
            Slice out = testhash_bytes(a, xlen[j]);
            sha3_shake_read(x, out, NULL);
            check_hex(t, a, test_shakes[j].name, row, out, xwant[j]);
        }
    }
    arena_free(&ar);
}

static void run_testhash(void *env, TestingT *t) {
    testhash_without_clone(t, ((const Digest *)env)->make);
}

static void TestSHA3Hash(TestingT *t) {
    for (size_t i = 0; i < NELEM(test_digests); i++)
        testing_t_run(
            t, str_from_cstr(test_digests[i].name),
            BURROW_FN(TestingTFunc, run_testhash, (void *)(uintptr_t)&test_digests[i]));
}

/* The Clone half of cryptotest.TestHash: a clone taken part way carries on
 * from there, and neither copy disturbs the other. */
static void TestClone(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice buf = testhash_bytes(a, 300);
    for (Int i = 0; i < buf.len; i++)
        ((Byte *)buf.p)[i] = (Byte)i;
    for (size_t i = 0; i < NELEM(test_digests); i++) {
        Hash h = test_digests[i].make(a);
        hash_write(h, buf, NULL);
        Slice want = hash_sum(a, h, slice_nil(TYPE_BYTE));

        Hash h2 = test_digests[i].make(a);
        hash_write(h2, slice_sub(buf, 0, 150), NULL);
        Error err = BURROW_NO_ERROR;
        HashCloner c = sha3_clone((Sha3 *)h2.data, a, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: Clone: %v", str_from_cstr(test_digests[i].name),
                               err);
            continue;
        }
        if (c.vt == NULL) {
            testing_t_errorf_v(t, "%s: Clone returned a nil hash",
                               str_from_cstr(test_digests[i].name));
            continue;
        }
        Sha3 *hc = (Sha3 *)c.data;
        sha3_write(hc, slice_sub(buf, 150, 300), NULL);
        hash_write(h2, bytes_of("garbage"), NULL);
        Slice got = sha3_sum(hc, a, slice_nil(TYPE_BYTE));
        if (!testhash_equal(got, want))
            testing_t_errorf_v(t, "%s: clone got %x, want %x",
                               str_from_cstr(test_digests[i].name), got, want);

        /* And through the interface. */
        HashCloner c2 = hash_cloner_clone(a, c, &err);
        if (c2.vt == NULL) {
            testing_t_errorf_v(t, "%s: Clone returned a nil hash",
                               str_from_cstr(test_digests[i].name));
            continue;
        }
        if (!testhash_equal(c2.vt->hash.sum(c2.data, a, slice_nil(TYPE_BYTE)), want))
            testing_t_errorf_v(t, "%s: clone of a clone differs",
                               str_from_cstr(test_digests[i].name));
    }
    arena_free(&ar);
}

/* sequentialBytes, which starts the buffer at a random offset from an eight
 * byte boundary to find alignment bugs. */
static Slice sequential_bytes(Alloc *a, Int size, TesthashRand *r) {
    Byte off;
    testhash_read(r, slice_from(&off, 1, 1, TYPE_BYTE));
    Int alignment_offset = off % 8;
    Slice result = slice_sub(testhash_bytes(a, size + alignment_offset),
                             alignment_offset, size + alignment_offset);
    Byte *p = (Byte *)result.p;
    for (Int i = 0; p != NULL && i < result.len; i++)
        p[i] = (Byte)i;
    return result;
}

static const Int unaligned_offsets[17] = {1,  2,  3,  4,  5,  6,  7,  8, 9,
                                          10, 11, 12, 13, 14, 15, 16, 1};

/* TestUnalignedWrite tests that writing data in an arbitrary pattern with
 * small input buffers. */
static void TestUnalignedWrite(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand r = testhash_new_rand(t);
    Slice buf = sequential_bytes(a, 0x10000, &r);
    for (size_t k = 0; k < NELEM(test_digests); k++) {
        Hash d = test_digests[k].make(a);
        hash_reset(d);
        hash_write(d, buf, NULL);
        Slice want = hash_sum(a, d, slice_nil(TYPE_BYTE));
        hash_reset(d);
        for (Int i = 0; i < buf.len;) {
            /* Cycle through offsets which make a 137 byte sequence. Because
             * 137 is prime this sequence should exercise all corner cases. */
            for (int o = 0; o < 17; o++) {
                Int j = unaligned_offsets[o];
                if (buf.len - i < j)
                    j = buf.len - i;
                hash_write(d, slice_sub(buf, i, i + j), NULL);
                i += j;
            }
        }
        Slice got = hash_sum(a, d, slice_nil(TYPE_BYTE));
        if (!testhash_equal(got, want))
            testing_t_errorf_v(t, "Unaligned writes, alg=%s\ngot %x, want %x",
                               str_from_cstr(test_digests[k].name), got, want);
    }

    /* Same for SHAKE */
    for (size_t k = 0; k < NELEM(test_shakes); k++) {
        Slice want = testhash_bytes(a, 16);
        Slice got = testhash_bytes(a, 16);
        Sha3SHAKE *d = make_shake(a, &test_shakes[k]);
        sha3_shake_reset(d);
        sha3_shake_write(d, buf, NULL);
        sha3_shake_read(d, want, NULL);
        sha3_shake_reset(d);
        for (Int i = 0; i < buf.len;) {
            for (int o = 0; o < 17; o++) {
                Int j = unaligned_offsets[o];
                if (buf.len - i < j)
                    j = buf.len - i;
                sha3_shake_write(d, slice_sub(buf, i, i + j), NULL);
                i += j;
            }
        }
        sha3_shake_read(d, got, NULL);
        if (!testhash_equal(got, want))
            testing_t_errorf_v(t, "Unaligned writes, alg=%s\ngot %x, want %x",
                               str_from_cstr(test_shakes[k].name), got, want);
    }
    arena_free(&ar);
}

/* TestAppend checks that appending works when reallocation is necessary. */
static void TestAppend(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Sha3 *d = sha3_new224(a);
    for (Int capacity = 2; capacity <= 66; capacity += 64) {
        /* The first time around the loop, Sum will have to reallocate. The
         * second time, it will not. */
        Slice buf = slice_make(a, TYPE_BYTE, 2, capacity);
        sha3_reset(d);
        Byte cc = 0xcc;
        sha3_write(d, slice_from(&cc, 1, 1, TYPE_BYTE), NULL);
        buf = sha3_sum(d, a, buf);
        Str got = strings_to_upper(a, hex_encode_to_string(a, buf));
        Str expected =
            BURROW_S("0000DF70ADC49B2E76EEE3A6931B93FA41841C3AF2CDF5B32A18B5478C39");
        if (!str_eq(got, expected))
            testing_t_errorf_v(t, "got %s, want %s", got, expected);
    }
    arena_free(&ar);
}

/* TestAppendNoRealloc tests that appending works when no reallocation is
 * necessary. */
static void TestAppendNoRealloc(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice buf = slice_make(a, TYPE_BYTE, 1, 200);
    Sha3 *d = sha3_new224(a);
    Byte cc = 0xcc;
    sha3_write(d, slice_from(&cc, 1, 1, TYPE_BYTE), NULL);
    Slice out = sha3_sum(d, a, buf);
    Str got = strings_to_upper(a, hex_encode_to_string(a, out));
    Str expected =
        BURROW_S("00DF70ADC49B2E76EEE3A6931B93FA41841C3AF2CDF5B32A18B5478C39");
    if (!str_eq(got, expected))
        testing_t_errorf_v(t, "got %s, want %s", got, expected);
    if (out.p != buf.p)
        testing_t_errorf_v(t, "Sum reallocated a buffer with room");
    arena_free(&ar);
}

/* TestSqueezing checks that squeezing the full output a single time produces
 * the same output as repeatedly squeezing the instance. */
static void TestSqueezing(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t k = 0; k < NELEM(test_shakes); k++) {
        Sha3SHAKE *d0 = make_shake(a, &test_shakes[k]);
        sha3_shake_write(d0, bytes_of(test_string), NULL);
        Slice ref = testhash_bytes(a, 32);
        sha3_shake_read(d0, ref, NULL);

        Sha3SHAKE *d1 = make_shake(a, &test_shakes[k]);
        sha3_shake_write(d1, bytes_of(test_string), NULL);
        Slice multiple = slice_nil(TYPE_BYTE);
        for (Int i = 0; i < ref.len; i++) {
            sha3_shake_read(d1, testhash_bytes(a, 0), NULL);
            Byte one;
            sha3_shake_read(d1, slice_from(&one, 1, 1, TYPE_BYTE), NULL);
            multiple = slice_append(a, multiple, &one, 1);
        }
        if (!testhash_equal(ref, multiple))
            testing_t_errorf_v(t, "%s: squeezing %d bytes one at a time failed",
                               str_from_cstr(test_shakes[k].name), ref.len);
    }
    arena_free(&ar);
}

static void TestReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand r = testhash_new_rand(t);
    Slice out1 = testhash_bytes(a, 32);
    Slice out2 = testhash_bytes(a, 32);
    Byte s[] = {0x99, 0x98};
    for (size_t k = 0; k < NELEM(test_shakes); k++) {
        /* Calculate hash for the first time */
        Sha3SHAKE *c = test_shakes[k].constructor(a, slice_nil(TYPE_BYTE),
                                                  slice_from(s, 2, 2, TYPE_BYTE));
        sha3_shake_write(c, sequential_bytes(a, 0x100, &r), NULL);
        sha3_shake_read(c, out1, NULL);

        /* Calculate hash again */
        sha3_shake_reset(c);
        sha3_shake_write(c, sequential_bytes(a, 0x100, &r), NULL);
        sha3_shake_read(c, out2, NULL);

        if (!testhash_equal(out1, out2))
            testing_t_errorf_v(t, "\nExpected:\n %x \ngot:\n %x", out1, out2);
    }
    arena_free(&ar);
}

/* io.CopyN(dst, src, n) for two SHAKEs: n bytes read from src and written to
 * dst. */
static void copy_n(Sha3SHAKE *dst, Sha3SHAKE *src, Slice scratch, Int n) {
    Slice p = slice_sub(scratch, 0, n);
    sha3_shake_read(src, p, NULL);
    sha3_shake_write(dst, p, NULL);
}

static void cshake_accumulated(TestingT *t, NewCSHAKE new_cshake, Int rate,
                               const char *exp) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Sha3SHAKE *rnd = new_cshake(a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE));
    Sha3SHAKE *acc = new_cshake(a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE));
    Slice scratch = testhash_bytes(a, 200);
    Slice n_buf = testhash_bytes(a, 200);
    Slice s_buf = testhash_bytes(a, 200);
    /* Each cSHAKE is allocated from its own arena, reset every time, so the
     * 40,000 of them do not pile up. */
    Arena inner;
    arena_init(&inner, NULL, 0);
    for (Int n = 0; n < 200; n++) {
        Slice nn = slice_sub(n_buf, 0, n);
        sha3_shake_read(rnd, nn, NULL);
        for (Int s = 0; s < 200; s++) {
            Slice ss = slice_sub(s_buf, 0, s);
            sha3_shake_read(rnd, ss, NULL);

            arena_reset(&inner);
            Sha3SHAKE *c = new_cshake(arena_allocator(&inner), nn, ss);
            copy_n(c, rnd, scratch, 100 /* < rate */);
            copy_n(acc, c, scratch, 200);

            sha3_shake_reset(c);
            copy_n(c, rnd, scratch, rate);
            copy_n(acc, c, scratch, 200);

            sha3_shake_reset(c);
            copy_n(c, rnd, scratch, 200 /* > rate */);
            copy_n(acc, c, scratch, 200);
        }
    }
    arena_free(&inner);
    Slice out = testhash_bytes(a, 32);
    sha3_shake_read(acc, out, NULL);
    Str got = hex_encode_to_string(a, out);
    if (!str_eq(got, str_from_cstr(exp)))
        testing_t_errorf_v(t, "got %s, want %s", got, str_from_cstr(exp));
    arena_free(&ar);
}

static void cshake_accumulated128_env(void *env, TestingT *t) {
    (void)env;
    cshake_accumulated(
        t, sha3_new_cshake128, (1600 - 256) / 8,
        "bb14f8657c6ec5403d0b0e2ef3d3393497e9d3b1a9a9e8e6c81dbaa5fd809252");
}

static void cshake_accumulated256_env(void *env, TestingT *t) {
    (void)env;
    cshake_accumulated(
        t, sha3_new_cshake256, (1600 - 512) / 8,
        "0baaf9250c6e25f0c14ea5c7f9bfde54c8a922c8276437db28f3895bdf6eeeef");
}

static void TestCSHAKEAccumulated(TestingT *t) {
    /* Go's expected values were generated with pycryptodome 3.20.0 and
     * @noble/hashes 1.5.0. */
    testing_t_run(t, BURROW_S("cSHAKE128"),
                  BURROW_FN(TestingTFunc, cshake_accumulated128_env, NULL));
    testing_t_run(t, BURROW_S("cSHAKE256"),
                  BURROW_FN(TestingTFunc, cshake_accumulated256_env, NULL));
}

static void TestCSHAKELargeS(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping test in short mode.");

    /* See https://go.dev/issue/66232. */
    const Int s = 536870912 + 1000; /* (1 << 32) / 8 + 1000, so s * 8 > 2^32 */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice big = testhash_bytes(a, s);
    Sha3SHAKE *rnd = sha3_new_shake128(a);
    sha3_shake_read(rnd, big, NULL);
    Sha3SHAKE *c = sha3_new_cshake128(a, slice_nil(TYPE_BYTE), big);
    Slice scratch = testhash_bytes(a, 1000);
    copy_n(c, rnd, scratch, 1000);
    Slice out = testhash_bytes(a, 32);
    sha3_shake_read(c, out, NULL);

    /* Generated with pycryptodome 3.20.0. */
    const char *exp =
        "2cb9f237767e98f2614b8779cf096a52da9b3a849280bbddec820771ae529cf0";
    Str got = hex_encode_to_string(a, out);
    if (!str_eq(got, str_from_cstr(exp)))
        testing_t_errorf_v(t, "got %s, want %s", got, str_from_cstr(exp));
    arena_free(&ar);
}

static void marshal_unmarshal(TestingT *t, Alloc *a, Sha3 *h, TesthashRand *r) {
    Slice buf = testhash_bytes(a, 200);
    testhash_read(r, buf);
    Byte nb;
    testhash_read(r, slice_from(&nb, 1, 1, TYPE_BYTE));
    Int n = nb % 200;
    sha3_write(h, buf, NULL);
    Slice want = sha3_sum(h, a, slice_nil(TYPE_BYTE));
    sha3_reset(h);
    sha3_write(h, slice_sub(buf, 0, n), NULL);
    Error err = BURROW_NO_ERROR;
    Slice b = sha3_marshal_binary(h, a, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "MarshalBinary: %v", err);
    sha3_write(h, testhash_bytes(a, 200), NULL);
    err = sha3_unmarshal_binary(h, b);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "UnmarshalBinary: %v", err);
    sha3_write(h, slice_sub(buf, n, buf.len), NULL);
    Slice got = sha3_sum(h, a, slice_nil(TYPE_BYTE));
    if (!testhash_equal(got, want))
        testing_t_errorf_v(t, "got %x, want %x", got, want);
}

static void marshal_unmarshal_shake(TestingT *t, Alloc *a, Sha3SHAKE *h,
                                    TesthashRand *r) {
    Slice buf = testhash_bytes(a, 200);
    testhash_read(r, buf);
    Byte nb;
    testhash_read(r, slice_from(&nb, 1, 1, TYPE_BYTE));
    Int n = nb % 200;
    sha3_shake_write(h, buf, NULL);
    Slice want = testhash_bytes(a, 32);
    sha3_shake_read(h, want, NULL);
    sha3_shake_reset(h);
    sha3_shake_write(h, slice_sub(buf, 0, n), NULL);
    Error err = BURROW_NO_ERROR;
    Slice b = sha3_shake_marshal_binary(h, a, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "MarshalBinary: %v", err);
    sha3_shake_write(h, testhash_bytes(a, 200), NULL);
    err = sha3_shake_unmarshal_binary(h, a, b);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "UnmarshalBinary: %v", err);
    sha3_shake_write(h, slice_sub(buf, n, buf.len), NULL);
    Slice got = testhash_bytes(a, 32);
    sha3_shake_read(h, got, NULL);
    if (!testhash_equal(got, want))
        testing_t_errorf_v(t, "got %x, want %x", got, want);
}

static void TestMarshalUnmarshal(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand r = testhash_new_rand(t);
    marshal_unmarshal(t, a, sha3_new224(a), &r);
    marshal_unmarshal(t, a, sha3_new256(a), &r);
    marshal_unmarshal(t, a, sha3_new384(a), &r);
    marshal_unmarshal(t, a, sha3_new512(a), &r);
    marshal_unmarshal_shake(t, a, sha3_new_shake128(a), &r);
    marshal_unmarshal_shake(t, a, sha3_new_shake256(a), &r);
    marshal_unmarshal_shake(t, a, sha3_new_cshake128(a, bytes_of("N"), bytes_of("S")),
                            &r);
    marshal_unmarshal_shake(t, a, sha3_new_cshake256(a, bytes_of("N"), bytes_of("S")),
                            &r);
    arena_free(&ar);
}

/* What Go's MarshalBinary gives for SHA3-256 after "abc", and the errors each
 * kind of bad state gets, all from Go. */
static void TestUnmarshalErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Sha3 *h = sha3_new256(a);
    sha3_write(h, bytes_of("abc"), NULL);
    Slice m = sha3_marshal_binary(h, a, NULL);
    if (m.len != 207)
        testing_t_fatalf_v(t, "marshaled %d bytes, want 207", m.len);
    Str head = hex_encode_to_string(a, slice_sub(m, 0, 8));
    if (!str_eq(head, BURROW_S("7368610888616263")))
        testing_t_errorf_v(t, "marshaled state starts %s, want 7368610888616263", head);

    Sha3SHAKE *k = sha3_new_cshake128(a, bytes_of("N"), bytes_of("S"));
    Slice m2 = sha3_shake_marshal_binary(k, a, NULL);
    Str tail = hex_encode_to_string(a, slice_sub(m2, 207, m2.len));
    if (m2.len != 213 || !str_eq(tail, BURROW_S("01084e010853")))
        testing_t_errorf_v(
            t, "cSHAKE state is %d bytes ending %s, want 213 ending 01084e010853",
            m2.len, tail);

    struct {
        Sha3 *h;
        Slice b;
        const char *want;
    } tests[] = {
        {h, slice_sub(m, 0, 10), "sha3: invalid hash state"},
        {h, slice_sub(m2, 0, 207), "sha3: invalid hash state identifier"},
        {sha3_new512(a), m, "sha3: invalid hash state function"},
    };
    for (size_t i = 0; i < NELEM(tests); i++) {
        Error err = sha3_unmarshal_binary(tests[i].h, tests[i].b);
        Str got = error_text(err);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "test %d: got %s, want %s", (Int)i, got,
                               str_from_cstr(tests[i].want));
    }
    arena_free(&ar);
}

/* The zero values are SHA3-256 and SHAKE256, as in Go. */
static void TestZeroValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Sha3 z = {0};
    sha3_write(&z, bytes_of("abc"), NULL);
    check_hex(t, a, "zero SHA3", 0, sha3_sum(&z, a, slice_nil(TYPE_BYTE)),
              "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");
    Sha3 z2 = {0};
    if (sha3_size(&z2) != 32 || sha3_block_size(&z2) != 136)
        testing_t_errorf_v(t,
                           "zero Sha3 has size %d and block size %d, want 32 and 136",
                           sha3_size(&z2), sha3_block_size(&z2));
    Sha3SHAKE x = {0};
    if (sha3_shake_block_size(&x) != 136)
        testing_t_errorf_v(t, "zero Sha3SHAKE has block size %d, want 136",
                           sha3_shake_block_size(&x));
    HashXOF xof = sha3_shake_as_xof(&x);
    io_write_string(hash_xof_as_io_writer(xof), BURROW_S(""), NULL);
    Slice out = testhash_bytes(a, 64);
    hash_xof_read(xof, out, NULL);
    check_hex(t, a, "zero SHAKE", 0, out, golden[0].shake256);
    arena_free(&ar);
}

static void bench_hash(TestingB *b, Sha3 *(*make)(Alloc *a), Int size, Int num) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Sha3 *h = make(a);
    Slice data = testhash_bytes(a, size);
    for (Int i = 0; i < size; i++)
        ((Byte *)data.p)[i] = (Byte)i;
    Slice state = testhash_bytes(a, 64);
    testing_b_set_bytes(b, (int64_t)size * num);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        for (Int j = 0; j < num; j++)
            sha3_write(h, data, NULL);
        sha3_sum(h, a, slice_sub(state, 0, 0));
    }
    arena_free(&ar);
}

static void bench_shake(TestingB *b, Sha3SHAKE *(*make)(Alloc *a), Int size, Int num) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Sha3SHAKE *h = make(a);
    Slice data = testhash_bytes(a, size);
    for (Int i = 0; i < size; i++)
        ((Byte *)data.p)[i] = (Byte)i;
    Slice d = testhash_bytes(a, 32);
    testing_b_set_bytes(b, (int64_t)size * num);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        sha3_shake_reset(h);
        for (Int j = 0; j < num; j++)
            sha3_shake_write(h, data, NULL);
        sha3_shake_read(h, d, NULL);
    }
    arena_free(&ar);
}

static void BenchmarkSha3_512_MTU(TestingB *b) {
    bench_hash(b, sha3_new512, 1350, 1);
}

static void BenchmarkSha3_384_MTU(TestingB *b) {
    bench_hash(b, sha3_new384, 1350, 1);
}

static void BenchmarkSha3_256_MTU(TestingB *b) {
    bench_hash(b, sha3_new256, 1350, 1);
}

static void BenchmarkSha3_224_MTU(TestingB *b) {
    bench_hash(b, sha3_new224, 1350, 1);
}

static void BenchmarkShake128_MTU(TestingB *b) {
    bench_shake(b, sha3_new_shake128, 1350, 1);
}

static void BenchmarkShake256_MTU(TestingB *b) {
    bench_shake(b, sha3_new_shake256, 1350, 1);
}

static void BenchmarkShake256_16x(TestingB *b) {
    bench_shake(b, sha3_new_shake256, 16, 1024);
}

static void BenchmarkShake256_1MiB(TestingB *b) {
    bench_shake(b, sha3_new_shake256, 1024, 1024);
}

static void BenchmarkSha3_512_1MiB(TestingB *b) {
    bench_hash(b, sha3_new512, 1024, 1024);
}

#define TESTS(X)                                                                       \
    X(TestGolden)                                                                      \
    X(TestSHA3Hash)                                                                    \
    X(TestClone)                                                                       \
    X(TestUnalignedWrite)                                                              \
    X(TestAppend)                                                                      \
    X(TestAppendNoRealloc)                                                             \
    X(TestSqueezing)                                                                   \
    X(TestReset)                                                                       \
    X(TestCSHAKEAccumulated)                                                           \
    X(TestCSHAKELargeS)                                                                \
    X(TestMarshalUnmarshal)                                                            \
    X(TestUnmarshalErrors)                                                             \
    X(TestZeroValue)                                                                   \
    X(BenchmarkSha3_512_MTU)                                                           \
    X(BenchmarkSha3_384_MTU)                                                           \
    X(BenchmarkSha3_256_MTU)                                                           \
    X(BenchmarkSha3_224_MTU)                                                           \
    X(BenchmarkShake128_MTU)                                                           \
    X(BenchmarkShake256_MTU)                                                           \
    X(BenchmarkShake256_16x)                                                           \
    X(BenchmarkShake256_1MiB)                                                          \
    X(BenchmarkSha3_512_1MiB)

TESTING_MAIN(TESTS)
