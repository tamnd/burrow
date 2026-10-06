/* Derived from Go's src/crypto/des/des_test.go and internal_test.go.
 * Go source: go1.27.1.
 *
 * TestFeistelBox and TestKeySizeError are burrow's. The first works the
 * feistel table out from the S-boxes the way Go's initFeistelBox does and
 * checks it against the one des.c has written out, and the second checks the
 * key sizes NewCipher and NewTripleDESCipher refuse.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testcipher.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/des.h"
#include "burrow/encoding/hex.h"

#include "../src/crypto/des_internal.h"

#include <stdint.h>
#include <string.h>

/* A key, input and output, each as hex, NULL where Go's table has nil. */
typedef struct CryptTest {
    const char *key, *in, *out;
} CryptTest;

/* some custom tests for DES */
static const CryptTest encrypt_des_tests[] = {
    {"0000000000000000", "0000000000000000", "8ca64de9c1b123a7"},
    {"0000000000000000", "ffffffffffffffff", "355550b2150e2451"},
    {"0000000000000000", "0123456789abcdef", "617b3a0ce8f07100"},
    {"0000000000000000", "fedcba9876543210", "9231f236ff9aa95c"},
    {"ffffffffffffffff", "0000000000000000", "caaaaf4deaf1dbae"},
    {"ffffffffffffffff", "ffffffffffffffff", "7359b2163e4edc58"},
    {"ffffffffffffffff", "0123456789abcdef", "6dce0dc9006556a3"},
    {"ffffffffffffffff", "fedcba9876543210", "9e84c5f3170f8eff"},
    {"0123456789abcdef", "0000000000000000", "d5d44ff720683d0d"},
    {"0123456789abcdef", "ffffffffffffffff", "59732356f36fde06"},
    {"0123456789abcdef", "0123456789abcdef", "56cc09e7cfdc4cef"},
    {"0123456789abcdef", "fedcba9876543210", "12c626af058b433b"},
    {"fedcba9876543210", "0000000000000000", "a68cdca90c9021f9"},
    {"fedcba9876543210", "ffffffffffffffff", "2a2bb008df97c2f2"},
    {"fedcba9876543210", "0123456789abcdef", "ed39d950fa74bcc4"},
    {"fedcba9876543210", "fedcba9876543210", "a933f6183023b310"},
    {"0123456789abcdef", "1111111111111111", "17668dfc7292532d"},
    {"0123456789abcdef", "0101010101010101", "b4fd231647a5bec0"},
    {"0e329232ea6d0d73", "8787878787878787", "0000000000000000"},
    {"736563523374243b", "6120746573743132", "370dee2c1fb4f7a5"},
    {"6162636465666768", "6162636465666768", "2a8d69de9d5fdff9"},
    {"6162636465666768", "3132333435363738", "21c60da534248bce"},
    {"3132333435363738", "6162636465666768", "94d4436bc3b5b693"},
    {"1f79905f8801c888", "c7461873af485fb3", "b0935088f992446a"},
    {"e6f4f2db31425301", "ff3d255012e34ac5", "8608d3d16c2fd255"},
    {"69c19dc115c5fb2b", "1a225caf1f1da3f9", "64ba316756911ea7"},
    {"6e5ee247c4bff651", "11c957ff66890ef0", "94c535b2c58b3972"},
};

/* Use the known weak keys */
static const CryptTest weak_key_tests[] = {
    {"0101010101010101", "5574c0bd7cdff739", NULL},
    {"fefefefefefefefe", "e8e1a7c1de1189aa", NULL},
    {"e0e0e0e0f1f1f1f1", "506a4b943bed7ddc", NULL},
    {"1f1f1f1f0e0e0e0e", "88815638ec3b1c97", NULL},
    {"0000000000000000", "17a0836232fe9a0b", NULL},
    {"ffffffffffffffff", "ca8fca1f50c57b49", NULL},
    {"e1e1e1e1f0f0f0f0", "b1eaad7de7c37a43", NULL},
    {"1e1e1e1e0f0f0f0f", "ae747d6fef16bb81", NULL},
};

static const CryptTest semi_weak_key_tests[] = {
    {"011f011f010e010e", "12fa3116f9c50ae4", "1f011f010e010e01"},
    {"01e001e001f101f1", "b04c7aeed2e54db7", "e001e001f101f101"},
    {"01fe01fe01fe01fe", "a481cdb1646fd3bc", "fe01fe01fe01fe01"},
    {"1fe01fe00ef10ef1", "ee27dd884c22cdce", "e01fe01ff10ef10e"},
    {"1ffe1ffe0efe0efe", "193dcf9770fbabe1", "fe1ffe1ffe0efe0e"},
    {"e0fee0fef1fef1fe", "7c8269e41e8699d7", "fee0fee0fef1fef1"},
};

static const CryptTest encrypt_triple_des_tests[] = {
    {"0000000000000000ffffffffffffffff0000000000000000", "0000000000000000",
     "9295b59bb384736e"},
    {"0000000000000000ffffffffffffffff0000000000000000", "ffffffffffffffff",
     "c197f558748a20e7"},
    {"ffffffffffffffff0000000000000000ffffffffffffffff", "0000000000000000",
     "3e680aa78b75df18"},
    {"ffffffffffffffff0000000000000000ffffffffffffffff", "ffffffffffffffff",
     "6d6a4a644c7b8c91"},
    {"616263646566676831323334353637384142434445464748", "3030303030303030",
     "e461b759688bff66"},
    {"616263646566676831323334353637384142434445464748", "3132333435363738",
     "dbd092def834ff58"},
    {"616263646566676831323334353637384142434445464748", "f0c58222d3e612d2",
     "bae441b13c374df4"},
    {"d37d45ee22e9cf52f465a24f70d1818a3dbe2f39c771d2e9", "4953c3e978df9faf",
     "53405124d83cf988"},
    {"cb107dda7e96570ae8ebe8078e87d357b26112b82a90b72f", "a3c260b10bb7286e",
     "56737dfbb5a1c3de"},
};

static const CryptTest table_a1_tests[] = {
    {NULL, "8000000000000000", "95f8a5e5dd31d900"},
    {NULL, "4000000000000000", "dd7f121ca5015619"},
    {NULL, "2000000000000000", "2e8653104f3834ea"},
    {NULL, "1000000000000000", "4bd388ff6cd81d4f"},
    {NULL, "0800000000000000", "20b9e767b2fb1456"},
    {NULL, "0400000000000000", "55579380d77138ef"},
    {NULL, "0200000000000000", "6cc5defaaf04512f"},
    {NULL, "0100000000000000", "0d9f279ba5d87260"},
    {NULL, "0080000000000000", "d9031b0271bd5a0a"},
    {NULL, "0040000000000000", "424250b37c3dd951"},
    {NULL, "0020000000000000", "b8061b7ecd9a21e5"},
    {NULL, "0010000000000000", "f15d0f286b65bd28"},
    {NULL, "0008000000000000", "add0cc8d6e5deba1"},
    {NULL, "0004000000000000", "e6d5f82752ad63d1"},
    {NULL, "0002000000000000", "ecbfe3bd3f591a5e"},
    {NULL, "0001000000000000", "f356834379d165cd"},
    {NULL, "0000800000000000", "2b9f982f20037fa9"},
    {NULL, "0000400000000000", "889de068a16f0be6"},
    {NULL, "0000200000000000", "e19e275d846a1298"},
    {NULL, "0000100000000000", "329a8ed523d71aec"},
    {NULL, "0000080000000000", "e7fce22557d23c97"},
    {NULL, "0000040000000000", "12a9f5817ff2d65d"},
    {NULL, "0000020000000000", "a484c3ad38dc9c19"},
    {NULL, "0000010000000000", "fbe00a8a1ef8ad72"},
    {NULL, "0000008000000000", "750d079407521363"},
    {NULL, "0000004000000000", "64feed9c724c2faf"},
    {NULL, "0000002000000000", "f02b263b328e2b60"},
    {NULL, "0000001000000000", "9d64555a9a10b852"},
    {NULL, "0000000800000000", "d106ff0bed5255d7"},
    {NULL, "0000000400000000", "e1652c6b138c64a5"},
    {NULL, "0000000200000000", "e428581186ec8f46"},
    {NULL, "0000000100000000", "aeb5f5ede22d1a36"},
    {NULL, "0000000080000000", "e943d7568aec0c5c"},
    {NULL, "0000000040000000", "df98c8276f54b04b"},
    {NULL, "0000000020000000", "b160e4680f6c696f"},
    {NULL, "0000000010000000", "fa0752b07d9c4ab8"},
    {NULL, "0000000008000000", "ca3a2b036dbc8502"},
    {NULL, "0000000004000000", "5e0905517bb59bcf"},
    {NULL, "0000000002000000", "814eeb3b91d90726"},
    {NULL, "0000000001000000", "4d49db1532919c9f"},
    {NULL, "0000000000800000", "25eb5fc3f8cf0621"},
    {NULL, "0000000000400000", "ab6a20c0620d1c6f"},
    {NULL, "0000000000200000", "79e90dbc98f92cca"},
    {NULL, "0000000000100000", "866ecedd8072bb0e"},
    {NULL, "0000000000080000", "8b54536f2f3e64a8"},
    {NULL, "0000000000040000", "ea51d3975595b86b"},
    {NULL, "0000000000020000", "caffc6ac4542de31"},
    {NULL, "0000000000010000", "8dd45a2ddf90796c"},
    {NULL, "0000000000008000", "1029d55e880ec2d0"},
    {NULL, "0000000000004000", "5d86cb23639dbea9"},
    {NULL, "0000000000002000", "1d1ca853ae7c0c5f"},
    {NULL, "0000000000001000", "ce332329248f3228"},
    {NULL, "0000000000000800", "8405d1abe24fb942"},
    {NULL, "0000000000000400", "e643d78090ca4207"},
    {NULL, "0000000000000200", "48221b9937748a23"},
    {NULL, "0000000000000100", "dd7c0bbd61fafd54"},
    {NULL, "0000000000000080", "2fbc291a570db5c4"},
    {NULL, "0000000000000040", "e07c30d7e4e26e12"},
    {NULL, "0000000000000020", "0953e2258e8e90a1"},
    {NULL, "0000000000000010", "5b711bc4ceebf2ee"},
    {NULL, "0000000000000008", "cc083f1e6d9e85f6"},
    {NULL, "0000000000000004", "d2fd8867d50d2dfe"},
    {NULL, "0000000000000002", "06e7ea22ce92708f"},
    {NULL, "0000000000000001", "166b40b44aba4bd6"},
};

static const CryptTest table_a2_tests[] = {
    {"800101010101010180010101010101018001010101010101", NULL, "95a8d72813daa94d"},
    {"400101010101010140010101010101014001010101010101", NULL, "0eec1487dd8c26d5"},
    {"200101010101010120010101010101012001010101010101", NULL, "7ad16ffb79c45926"},
    {"100101010101010110010101010101011001010101010101", NULL, "d3746294ca6a6cf3"},
    {"080101010101010108010101010101010801010101010101", NULL, "809f5f873c1fd761"},
    {"040101010101010104010101010101010401010101010101", NULL, "c02faffec989d1fc"},
    {"020101010101010102010101010101010201010101010101", NULL, "4615aa1d33e72f10"},
    {"018001010101010101800101010101010180010101010101", NULL, "2055123350c00858"},
    {"014001010101010101400101010101010140010101010101", NULL, "df3b99d6577397c8"},
    {"012001010101010101200101010101010120010101010101", NULL, "31fe17369b5288c9"},
    {"011001010101010101100101010101010110010101010101", NULL, "dfdd3cc64dae1642"},
    {"010801010101010101080101010101010108010101010101", NULL, "178c83ce2b399d94"},
    {"010401010101010101040101010101010104010101010101", NULL, "50f636324a9b7f80"},
    {"010201010101010101020101010101010102010101010101", NULL, "a8468ee3bc18f06d"},
    {"010180010101010101018001010101010101800101010101", NULL, "a2dc9e92fd3cde92"},
    {"010140010101010101014001010101010101400101010101", NULL, "cac09f797d031287"},
    {"010120010101010101012001010101010101200101010101", NULL, "90ba680b22aeb525"},
    {"010110010101010101011001010101010101100101010101", NULL, "ce7a24f350e280b6"},
    {"010108010101010101010801010101010101080101010101", NULL, "882bff0aa01a0b87"},
    {"010104010101010101010401010101010101040101010101", NULL, "25610288924511c2"},
    {"010102010101010101010201010101010101020101010101", NULL, "c71516c29c75d170"},
    {"010101800101010101010180010101010101018001010101", NULL, "5199c29a52c9f059"},
    {"010101400101010101010140010101010101014001010101", NULL, "c22f0a294a71f29f"},
    {"010101200101010101010120010101010101012001010101", NULL, "ee371483714c02ea"},
    {"010101100101010101010110010101010101011001010101", NULL, "a81fbd448f9e522f"},
    {"010101080101010101010108010101010101010801010101", NULL, "4f644c92e192dfed"},
    {"010101040101010101010104010101010101010401010101", NULL, "1afa9a66a6df92ae"},
    {"010101020101010101010102010101010101010201010101", NULL, "b3c1cc715cb879d8"},
    {"010101018001010101010101800101010101010180010101", NULL, "19d032e64ab0bd8b"},
    {"010101014001010101010101400101010101010140010101", NULL, "3cfaa7a7dc8720dc"},
    {"010101012001010101010101200101010101010120010101", NULL, "b7265f7f447ac6f3"},
    {"010101011001010101010101100101010101010110010101", NULL, "9db73b3c0d163f54"},
    {"010101010801010101010101080101010101010108010101", NULL, "8181b65babf4a975"},
    {"010101010401010101010101040101010101010104010101", NULL, "93c9b64042eaa240"},
    {"010101010201010101010101020101010101010102010101", NULL, "5570530829705592"},
    {"010101010180010101010101018001010101010101800101", NULL, "8638809e878787a0"},
    {"010101010140010101010101014001010101010101400101", NULL, "41b9a79af79ac208"},
    {"010101010120010101010101012001010101010101200101", NULL, "7a9be42f2009a892"},
    {"010101010110010101010101011001010101010101100101", NULL, "29038d56ba6d2745"},
    {"010101010108010101010101010801010101010101080101", NULL, "5495c6abf1e5df51"},
    {"010101010104010101010101010401010101010101040101", NULL, "ae13dbd561488933"},
    {"010101010102010101010101010201010101010101020101", NULL, "024d1ffa8904e389"},
    {"010101010101800101010101010180010101010101018001", NULL, "d1399712f99bf02e"},
    {"010101010101400101010101010140010101010101014001", NULL, "14c1d7c1cffec79e"},
    {"010101010101200101010101010120010101010101012001", NULL, "1de5279dae3bed6f"},
    {"010101010101100101010101010110010101010101011001", NULL, "e941a33f85501303"},
    {"010101010101080101010101010108010101010101010801", NULL, "da99dbbc9a03f379"},
    {"010101010101040101010101010104010101010101010401", NULL, "b7fc92f91d8e92e9"},
    {"010101010101020101010101010102010101010101010201", NULL, "ae8e5caa3ca04e85"},
    {"010101010101018001010101010101800101010101010180", NULL, "9cc62df43b6eed74"},
    {"010101010101014001010101010101400101010101010140", NULL, "d863dbb5c59a91a0"},
    {"010101010101012001010101010101200101010101010120", NULL, "a1ab2190545b91d7"},
    {"010101010101011001010101010101100101010101010110", NULL, "0875041e64c570f7"},
    {"010101010101010801010101010101080101010101010108", NULL, "5a594528bebef1cc"},
    {"010101010101010401010101010101040101010101010104", NULL, "fcdb3291de21f0c0"},
    {"010101010101010201010101010101020101010101010102", NULL, "869efd7f9f265a09"},
};

static const CryptTest table_a3_tests[] = {
    {"104691348998013110469134899801311046913489980131", NULL, "88d55e54f54c97b4"},
    {"100710348998802010071034899880201007103489988020", NULL, "0c0cc00c83ea48fd"},
    {"10071034c898012010071034c898012010071034c8980120", NULL, "83bc8ef3a6570183"},
    {"104610348998802010461034899880201046103489988020", NULL, "df725dcad94ea2e9"},
    {"108691151919010110869115191901011086911519190101", NULL, "e652b53b550be8b0"},
    {"108691151958010110869115195801011086911519580101", NULL, "af527120c485cbb0"},
    {"5107b015195801015107b015195801015107b01519580101", NULL, "0f04ce393db926d5"},
    {"1007b015191901011007b015191901011007b01519190101", NULL, "c9f00ffc74079067"},
    {"310791549808010131079154980801013107915498080101", NULL, "7cfd82a593252b4e"},
    {"310791949808010131079194980801013107919498080101", NULL, "cb49a2f9e91363e3"},
    {"10079115b908014010079115b908014010079115b9080140", NULL, "00b588be70d23f56"},
    {"310791159808014031079115980801403107911598080140", NULL, "406a9a6ab43399ae"},
    {"1007d015899801011007d015899801011007d01589980101", NULL, "6cb773611dca9ada"},
    {"910791158998010191079115899801019107911589980101", NULL, "67fd21c17dbb5d70"},
    {"9107d015891901019107d015891901019107d01589190101", NULL, "9592cb4110430787"},
    {"1007d015989801201007d015989801201007d01598980120", NULL, "a6b7ff68a318ddd3"},
    {"100794049819010110079404981901011007940498190101", NULL, "4d102196c914ca16"},
    {"010791049119040101079104911904010107910491190401", NULL, "2dfa9f4573594965"},
    {"010791049119010101079104911901010107910491190101", NULL, "b46604816c0e0774"},
    {"010794049119040101079404911904010107940491190401", NULL, "6e7e6221a4f34e87"},
    {"19079210981a010119079210981a010119079210981a0101", NULL, "aa85e74643233199"},
    {"100791199819080110079119981908011007911998190801", NULL, "2e5a19db4d1962d6"},
    {"10079119981a080110079119981a080110079119981a0801", NULL, "23a866a809d30894"},
    {"100792109819010110079210981901011007921098190101", NULL, "d812d961f017d320"},
    {"100791159819010b100791159819010b100791159819010b", NULL, "055605816e58608f"},
    {"100480159819010110048015981901011004801598190101", NULL, "abd88e8b1b7716f1"},
    {"100480159819010210048015981901021004801598190102", NULL, "537ac95be69da1e1"},
    {"100480159819010810048015981901081004801598190108", NULL, "aed0f6ae3c25cdd8"},
    {"100291159810010410029115981001041002911598100104", NULL, "b3e35a5ee53e7b8d"},
    {"100291159819010410029115981901041002911598190104", NULL, "61c79c71921a2ef8"},
    {"100291159810020110029115981002011002911598100201", NULL, "e2f5728f0995013c"},
    {"100291169810010110029116981001011002911698100101", NULL, "1aeac39a61f0a464"},
};

static const CryptTest table_a4_tests[] = {
    {"7ca110454a1a6e577ca110454a1a6e577ca110454a1a6e57", "01a1d6d039776742",
     "690f5b0d9a26939b"},
    {"0131d9619dc1376e0131d9619dc1376e0131d9619dc1376e", "5cd54ca83def57da",
     "7a389d10354bd271"},
    {"07a1133e4a0b268607a1133e4a0b268607a1133e4a0b2686", "0248d43806f67172",
     "868ebb51cab4599a"},
    {"3849674c2602319e3849674c2602319e3849674c2602319e", "51454b582ddf440a",
     "7178876e01f19b2a"},
    {"04b915ba43feb5b604b915ba43feb5b604b915ba43feb5b6", "42fd443059577fa2",
     "af37fb421f8c4095"},
    {"0113b970fd34f2ce0113b970fd34f2ce0113b970fd34f2ce", "059b5e0851cf143a",
     "86a560f10ec6d85b"},
    {"0170f175468fb5e60170f175468fb5e60170f175468fb5e6", "0756d8e0774761d2",
     "0cd3da020021dc09"},
    {"43297fad38e373fe43297fad38e373fe43297fad38e373fe", "762514b829bf486a",
     "ea676b2cb7db2b7a"},
    {"07a7137045da2a1607a7137045da2a1607a7137045da2a16", "3bdd119049372802",
     "dfd64a815caf1a0f"},
    {"04689104c2fd3b2f04689104c2fd3b2f04689104c2fd3b2f", "26955f6835af609a",
     "5c513c9c4886c088"},
    {"37d06bb516cb754637d06bb516cb754637d06bb516cb7546", "164d5e404f275232",
     "0a2aeeae3ff4ab77"},
    {"1f08260d1ac2465e1f08260d1ac2465e1f08260d1ac2465e", "6b056e18759f5cca",
     "ef1bf03e5dfa575a"},
    {"584023641aba6176584023641aba6176584023641aba6176", "004bd6ef09176062",
     "88bf0db6d70dee56"},
    {"025816164629b007025816164629b007025816164629b007", "480d39006ee762f2",
     "a1f9915541020b56"},
    {"49793ebc79b3258f49793ebc79b3258f49793ebc79b3258f", "437540c8698f3cfa",
     "6fbf1cafcffd0556"},
    {"4fb05e1515ab73a74fb05e1515ab73a74fb05e1515ab73a7", "072d43a077075292",
     "2f22e49bab7ca1ac"},
    {"49e95d6d4ca229bf49e95d6d4ca229bf49e95d6d4ca229bf", "02fe55778117f12a",
     "5a6b612cc26cce4a"},
    {"018310dc409b26d6018310dc409b26d6018310dc409b26d6", "1d9d5c5018f728c2",
     "5f4c038ed12b2e41"},
    {"1c587f1c13924fef1c587f1c13924fef1c587f1c13924fef", "305532286d6f295a",
     "63fac0d034d9f793"},
};

/* NIST Special Publication 800-20, Appendix A
 * Key for use with Table A.1 tests */
static const char table_a1_key[] = "010101010101010101010101010101010101010101010101";

static const char table_a2_plaintext[] = "0000000000000000";

static const char table_a3_plaintext[] = "0000000000000000";

#define LEN(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("bad hex in a test table"));
    return b;
}

static CipherBlock new_cipher(Alloc *a, Slice key) {
    Error err = BURROW_NO_ERROR;
    CipherBlock c = des_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        panic(BURROW_ANY(TYPE_ERROR, &err));
    return c;
}

static CipherBlock new_triple(Alloc *a, Slice key) {
    Error err = BURROW_NO_ERROR;
    return des_new_triple_des_cipher(a, key, &err);
}

static Slice encrypt(Alloc *a, CipherBlock c, Slice in) {
    Slice out = slice_make(a, TYPE_BYTE, in.len, in.len);
    cipher_block_encrypt(c, out, in);
    return out;
}

static Slice decrypt(Alloc *a, CipherBlock c, Slice in) {
    Slice out = slice_make(a, TYPE_BYTE, in.len, in.len);
    cipher_block_decrypt(c, out, in);
    return out;
}

static void expect(TestingT *t, Int i, Slice result, Slice want) {
    if (!bytes_equal(result, want))
        testing_t_errorf_v(t, "#%d: result: %x want: %x", i, result, want);
}

/* Use the known weak keys to test DES implementation */
static void TestWeakKeys(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(weak_key_tests); i++) {
        const CryptTest *tt = &weak_key_tests[i];
        CipherBlock c = new_cipher(a, unhex(a, tt->key));
        Slice in = unhex(a, tt->in);

        /* Encrypting twice with a DES weak key should reproduce the original
         * input */
        Slice result = encrypt(a, c, in);
        result = encrypt(a, c, result);
        expect(t, i, result, in);
    }
    arena_free(&ar);
}

/* Use the known semi-weak key pairs to test DES implementation */
static void TestSemiWeakKeyPairs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(semi_weak_key_tests); i++) {
        const CryptTest *tt = &semi_weak_key_tests[i];
        Slice in = unhex(a, tt->in);

        /* Encrypting with one member of the semi-weak pair and then
         * encrypting the result with the other member should reproduce the
         * original input. */
        Slice result = encrypt(a, new_cipher(a, unhex(a, tt->key)), in);
        result = encrypt(a, new_cipher(a, unhex(a, tt->out)), result);
        expect(t, i, result, in);
    }
    arena_free(&ar);
}

static void TestDESEncryptBlock(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(encrypt_des_tests); i++) {
        const CryptTest *tt = &encrypt_des_tests[i];
        CipherBlock c = new_cipher(a, unhex(a, tt->key));
        expect(t, i, encrypt(a, c, unhex(a, tt->in)), unhex(a, tt->out));
    }
    arena_free(&ar);
}

static void TestDESDecryptBlock(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(encrypt_des_tests); i++) {
        const CryptTest *tt = &encrypt_des_tests[i];
        CipherBlock c = new_cipher(a, unhex(a, tt->key));
        expect(t, i, decrypt(a, c, unhex(a, tt->out)), unhex(a, tt->in));
    }
    arena_free(&ar);
}

static void TestEncryptTripleDES(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(encrypt_triple_des_tests); i++) {
        const CryptTest *tt = &encrypt_triple_des_tests[i];
        CipherBlock c = new_triple(a, unhex(a, tt->key));
        expect(t, i, encrypt(a, c, unhex(a, tt->in)), unhex(a, tt->out));
    }
    arena_free(&ar);
}

static void TestDecryptTripleDES(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(encrypt_triple_des_tests); i++) {
        const CryptTest *tt = &encrypt_triple_des_tests[i];
        CipherBlock c = new_triple(a, unhex(a, tt->key));
        expect(t, i, decrypt(a, c, unhex(a, tt->out)), unhex(a, tt->in));
    }
    arena_free(&ar);
}

/* Defined in Pub 800-20 */
static void TestVariablePlaintextKnownAnswer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(table_a1_tests); i++) {
        const CryptTest *tt = &table_a1_tests[i];
        CipherBlock c = new_triple(a, unhex(a, table_a1_key));
        expect(t, i, encrypt(a, c, unhex(a, tt->in)), unhex(a, tt->out));
    }
    arena_free(&ar);
}

/* Defined in Pub 800-20 */
static void TestVariableCiphertextKnownAnswer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(table_a1_tests); i++) {
        const CryptTest *tt = &table_a1_tests[i];
        CipherBlock c = new_triple(a, unhex(a, table_a1_key));
        expect(t, i, decrypt(a, c, unhex(a, tt->out)), unhex(a, tt->in));
    }
    arena_free(&ar);
}

/* Defined in Pub 800-20
 * Encrypting the Table A.1 ciphertext with the 0x01... key produces the
 * original plaintext */
static void TestInversePermutationKnownAnswer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(table_a1_tests); i++) {
        const CryptTest *tt = &table_a1_tests[i];
        CipherBlock c = new_triple(a, unhex(a, table_a1_key));
        expect(t, i, encrypt(a, c, unhex(a, tt->out)), unhex(a, tt->in));
    }
    arena_free(&ar);
}

/* Defined in Pub 800-20
 * Decrypting the Table A.1 plaintext with the 0x01... key produces the
 * corresponding ciphertext */
static void TestInitialPermutationKnownAnswer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(table_a1_tests); i++) {
        const CryptTest *tt = &table_a1_tests[i];
        CipherBlock c = new_triple(a, unhex(a, table_a1_key));
        expect(t, i, decrypt(a, c, unhex(a, tt->in)), unhex(a, tt->out));
    }
    arena_free(&ar);
}

/* Defined in Pub 800-20 */
static void TestVariableKeyKnownAnswerEncrypt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(table_a2_tests); i++) {
        const CryptTest *tt = &table_a2_tests[i];
        CipherBlock c = new_triple(a, unhex(a, tt->key));
        expect(t, i, encrypt(a, c, unhex(a, table_a2_plaintext)), unhex(a, tt->out));
    }
    arena_free(&ar);
}

/* Defined in Pub 800-20 */
static void TestVariableKeyKnownAnswerDecrypt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(table_a2_tests); i++) {
        const CryptTest *tt = &table_a2_tests[i];
        CipherBlock c = new_triple(a, unhex(a, tt->key));
        expect(t, i, decrypt(a, c, unhex(a, tt->out)), unhex(a, table_a2_plaintext));
    }
    arena_free(&ar);
}

/* Defined in Pub 800-20 */
static void TestPermutationOperationKnownAnswerEncrypt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(table_a3_tests); i++) {
        const CryptTest *tt = &table_a3_tests[i];
        CipherBlock c = new_triple(a, unhex(a, tt->key));
        expect(t, i, encrypt(a, c, unhex(a, table_a3_plaintext)), unhex(a, tt->out));
    }
    arena_free(&ar);
}

/* Defined in Pub 800-20 */
static void TestPermutationOperationKnownAnswerDecrypt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(table_a3_tests); i++) {
        const CryptTest *tt = &table_a3_tests[i];
        CipherBlock c = new_triple(a, unhex(a, tt->key));
        expect(t, i, decrypt(a, c, unhex(a, tt->out)), unhex(a, table_a3_plaintext));
    }
    arena_free(&ar);
}

/* Defined in Pub 800-20 */
static void TestSubstitutionTableKnownAnswerEncrypt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(table_a4_tests); i++) {
        const CryptTest *tt = &table_a4_tests[i];
        CipherBlock c = new_triple(a, unhex(a, tt->key));
        expect(t, i, encrypt(a, c, unhex(a, tt->in)), unhex(a, tt->out));
    }
    arena_free(&ar);
}

/* Defined in Pub 800-20 */
static void TestSubstitutionTableKnownAnswerDecrypt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(table_a4_tests); i++) {
        const CryptTest *tt = &table_a4_tests[i];
        CipherBlock c = new_triple(a, unhex(a, tt->key));
        expect(t, i, decrypt(a, c, unhex(a, tt->out)), unhex(a, tt->in));
    }
    arena_free(&ar);
}

static void des_block(void *env, TestingT *t) {
    (void)env;
    testcipher_block(t, 8, des_new_cipher);
}

static void triple_des_block(void *env, TestingT *t) {
    (void)env;
    testcipher_block(t, 24, des_new_triple_des_cipher);
}

/* Test DES against the general cipher.Block interface tester */
static void TestDESBlock(TestingT *t) {
    testing_t_run(t, BURROW_S("DES"), BURROW_FN(TestingTFunc, des_block, NULL));
    testing_t_run(t, BURROW_S("TripleDES"),
                  BURROW_FN(TestingTFunc, triple_des_block, NULL));
}

/* ------------------------------------------------------- internal_test.go */

/* Used to perform an initial permutation of a 64-bit input block. */
static const uint8_t initial_permutation[64] = {
    6, 14, 22, 30, 38, 46, 54, 62, 4, 12, 20, 28, 36, 44, 52, 60,
    2, 10, 18, 26, 34, 42, 50, 58, 0, 8,  16, 24, 32, 40, 48, 56,
    7, 15, 23, 31, 39, 47, 55, 63, 5, 13, 21, 29, 37, 45, 53, 61,
    3, 11, 19, 27, 35, 43, 51, 59, 1, 9,  17, 25, 33, 41, 49, 57,
};

/* Used to perform a final permutation of a 64-bit preoutput block. This is
 * the inverse of initialPermutation */
static const uint8_t final_permutation[64] = {
    24, 56, 16, 48, 8,  40, 0, 32, 25, 57, 17, 49, 9,  41, 1, 33,
    26, 58, 18, 50, 10, 42, 2, 34, 27, 59, 19, 51, 11, 43, 3, 35,
    28, 60, 20, 52, 12, 44, 4, 36, 29, 61, 21, 53, 13, 45, 5, 37,
    30, 62, 22, 54, 14, 46, 6, 38, 31, 63, 23, 55, 15, 47, 7, 39,
};

static void TestInitialPermute(TestingT *t) {
    for (unsigned i = 0; i < 64; i++) {
        uint64_t bit = (uint64_t)1 << i;
        uint64_t got = burrow__des_permute_initial_block(bit);
        uint64_t want = (uint64_t)1 << final_permutation[63 - i];
        if (got != want)
            testing_t_errorf_v(t, "permute(%x) = %x, want %x", bit, got, want);
    }
}

static void TestFinalPermute(TestingT *t) {
    for (unsigned i = 0; i < 64; i++) {
        uint64_t bit = (uint64_t)1 << i;
        uint64_t got = burrow__des_permute_final_block(bit);
        uint64_t want = (uint64_t)1 << initial_permutation[63 - i];
        if (got != want)
            testing_t_errorf_v(t, "permute(%x) = %x, want %x", bit, got, want);
    }
}

/* ------------------------------------------------------------- burrow's */

/* Yields a 32-bit output from a 32-bit input */
static const uint8_t permutation_function[32] = {
    16, 25, 12, 11, 3, 20, 4,  15, 31, 17, 9, 6,  27, 14, 1,  22,
    30, 24, 8,  18, 0, 5,  29, 23, 13, 19, 2, 26, 10, 21, 28, 7,
};

/* 8 S-boxes composed of 4 rows and 16 columns */
static const uint8_t s_boxes[8][4][16] = {
    {
        {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7},
        {0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8},
        {4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0},
        {15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13},
    },
    {
        {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10},
        {3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5},
        {0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15},
        {13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9},
    },
    {
        {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8},
        {13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1},
        {13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7},
        {1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12},
    },
    {
        {7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15},
        {13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9},
        {10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4},
        {3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14},
    },
    {
        {2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9},
        {14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6},
        {4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14},
        {11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3},
    },
    {
        {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11},
        {10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8},
        {9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6},
        {4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13},
    },
    {
        {4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1},
        {13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6},
        {1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2},
        {6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12},
    },
    {
        {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7},
        {1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2},
        {7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8},
        {2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11},
    },
};

/* Go's permuteBlock. */
static uint64_t permute_block(uint64_t src, const uint8_t *permutation, unsigned n) {
    uint64_t block = 0;
    for (unsigned position = 0; position < n; position++) {
        uint64_t bit = (src >> permutation[position]) & 1;
        block |= bit << ((n - 1) - position);
    }
    return block;
}

/* Go's initFeistelBox, against the table des.c has instead. */
static void TestFeistelBox(TestingT *t) {
    for (unsigned s = 0; s < 8; s++) {
        for (unsigned i = 0; i < 4; i++) {
            for (unsigned j = 0; j < 16; j++) {
                uint64_t f = (uint64_t)s_boxes[s][i][j] << (4 * (7 - s));
                f = permute_block(f, permutation_function, 32);

                /* Row is determined by the 1st and 6th bit. Column is the
                 * middle four bits. */
                unsigned row = ((i & 2) << 4) | (i & 1);
                unsigned col = j << 1;
                unsigned idx = row | col;

                f = (f << 1) | (f >> 31);

                uint32_t want = (uint32_t)f;
                uint32_t got = burrow__des_feistel_box[s][idx];
                if (got != want)
                    testing_t_errorf_v(t, "feistelBox[%d][%d] = %#08x, want %#08x",
                                       (Int)s, (Int)idx, got, want);
            }
        }
    }
}

static void TestKeySizeError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const Byte key[25] = {0};
    static const Int des_bad[] = {0, 1, 7, 9, 16, 24};
    static const Int triple_bad[] = {0, 8, 16, 23, 25};
    for (Int i = 0; i < LEN(des_bad) + LEN(triple_bad); i++) {
        bool triple = i >= LEN(des_bad);
        Int n = triple ? triple_bad[i - LEN(des_bad)] : des_bad[i];
        Slice k = slice_from((void *)(uintptr_t)key, n, n, TYPE_BYTE);
        Error err = BURROW_NO_ERROR;
        CipherBlock c =
            triple ? des_new_triple_des_cipher(a, k, &err) : des_new_cipher(a, k, &err);
        const char *name = triple ? "NewTripleDESCipher" : "NewCipher";
        if (!BURROW_FAILED(err) || c.vt != NULL) {
            testing_t_errorf_v(t, "%s(%d bytes) succeeded", name, n);
            continue;
        }
        Str want = fmt_sprintf_v(a, "crypto/des: invalid key size %d", n);
        if (!str_eq(error_text(err), want))
            testing_t_errorf_v(t, "%s(%d bytes) = %q, want %q", name, n,
                               error_text(err), want);
        const DesKeySizeError *k2 = errors_as(err, TYPE_DES_KEY_SIZE_ERROR);
        if (k2 == NULL || *k2 != n)
            testing_t_errorf_v(t, "%s(%d bytes): errors.As does not give the size",
                               name, n);
    }
    arena_free(&ar);
}

/* --------------------------------------------------------------- benchmarks */

static void bench(TestingB *b, bool triple, bool dec) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const CryptTest *tt = triple ? &encrypt_triple_des_tests[0] : &encrypt_des_tests[0];
    Error err = BURROW_NO_ERROR;
    Slice key = unhex(a, tt->key);
    CipherBlock c =
        triple ? des_new_triple_des_cipher(a, key, &err) : des_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        testing_b_fatal_v(b, "NewCipher:", err);
    Slice in = unhex(a, dec ? tt->out : tt->in);
    Slice out = slice_make(a, TYPE_BYTE, in.len, in.len);
    testing_b_set_bytes(b, out.len);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        if (dec)
            cipher_block_decrypt(c, out, in);
        else
            cipher_block_encrypt(c, out, in);
    }
    arena_free(&ar);
}

static void BenchmarkEncrypt(TestingB *b) {
    bench(b, false, false);
}

static void BenchmarkDecrypt(TestingB *b) {
    bench(b, false, true);
}

static void BenchmarkTDESEncrypt(TestingB *b) {
    bench(b, true, false);
}

static void BenchmarkTDESDecrypt(TestingB *b) {
    bench(b, true, true);
}

#define TESTS(X)                                                                       \
    X(TestWeakKeys)                                                                    \
    X(TestSemiWeakKeyPairs)                                                            \
    X(TestDESEncryptBlock)                                                             \
    X(TestDESDecryptBlock)                                                             \
    X(TestEncryptTripleDES)                                                            \
    X(TestDecryptTripleDES)                                                            \
    X(TestVariablePlaintextKnownAnswer)                                                \
    X(TestVariableCiphertextKnownAnswer)                                               \
    X(TestInversePermutationKnownAnswer)                                               \
    X(TestInitialPermutationKnownAnswer)                                               \
    X(TestVariableKeyKnownAnswerEncrypt)                                               \
    X(TestVariableKeyKnownAnswerDecrypt)                                               \
    X(TestPermutationOperationKnownAnswerEncrypt)                                      \
    X(TestPermutationOperationKnownAnswerDecrypt)                                      \
    X(TestSubstitutionTableKnownAnswerEncrypt)                                         \
    X(TestSubstitutionTableKnownAnswerDecrypt)                                         \
    X(TestDESBlock)                                                                    \
    X(TestInitialPermute)                                                              \
    X(TestFinalPermute)                                                                \
    X(TestFeistelBox)                                                                  \
    X(TestKeySizeError)                                                                \
    X(BenchmarkEncrypt)                                                                \
    X(BenchmarkDecrypt)                                                                \
    X(BenchmarkTDESEncrypt)                                                            \
    X(BenchmarkTDESDecrypt)

TESTING_MAIN(TESTS)
