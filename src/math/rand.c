/* Derived from Go's src/math/rand and src/math/rand/v2, and from
 * src/internal/chacha8rand for the ChaCha8 generator.
 * Go source: go1.27.1.
 *
 * Both versions are in this one file because they share the ziggurat tables
 * for the normal and exponential distributions, and because v1's generator and
 * v2's are short enough that splitting them would mostly add declarations.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/math/rand.h"
#include "burrow/math/rand/v2.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/lock.h"
#include "burrow/math.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* No fused multiply and add. The ziggurat's float32 comparison and Zipf's
 * arithmetic round after every operation in Go, and a fused one would change
 * which side of a threshold a draw lands on. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

/* ------------------------------------------------------------------- tables
 *
 * The ziggurat tables from normal.go and exp.go, which are the same in both
 * versions, and the cooked values from rng.go that v1's seeding mixes in. They
 * were copied from Go's source by a script rather than recomputed, since the
 * point is to match Go bit for bit and Go's are the ones it uses. The cooked
 * values are written as their bits, because the most negative int64 has no
 * literal in C. */

static const uint32_t rand_kn[128] = {
    0x76ad2212, 0x0,        0x600f1b53, 0x6ce447a6, 0x725b46a2, 0x7560051d, 0x774921eb,
    0x789a25bd, 0x799045c3, 0x7a4bce5d, 0x7adf629f, 0x7b5682a6, 0x7bb8a8c6, 0x7c0ae722,
    0x7c50cce7, 0x7c8cec5b, 0x7cc12cd6, 0x7ceefed2, 0x7d177e0b, 0x7d3b8883, 0x7d5bce6c,
    0x7d78dd64, 0x7d932886, 0x7dab0e57, 0x7dc0dd30, 0x7dd4d688, 0x7de73185, 0x7df81cea,
    0x7e07c0a3, 0x7e163efa, 0x7e23b587, 0x7e303dfd, 0x7e3beec2, 0x7e46db77, 0x7e51155d,
    0x7e5aabb3, 0x7e63abf7, 0x7e6c222c, 0x7e741906, 0x7e7b9a18, 0x7e82adfa, 0x7e895c63,
    0x7e8fac4b, 0x7e95a3fb, 0x7e9b4924, 0x7ea0a0ef, 0x7ea5b00d, 0x7eaa7ac3, 0x7eaf04f3,
    0x7eb3522a, 0x7eb765a5, 0x7ebb4259, 0x7ebeeafd, 0x7ec2620a, 0x7ec5a9c4, 0x7ec8c441,
    0x7ecbb365, 0x7ece78ed, 0x7ed11671, 0x7ed38d62, 0x7ed5df12, 0x7ed80cb4, 0x7eda175c,
    0x7edc0005, 0x7eddc78e, 0x7edf6ebf, 0x7ee0f647, 0x7ee25ebe, 0x7ee3a8a9, 0x7ee4d473,
    0x7ee5e276, 0x7ee6d2f5, 0x7ee7a620, 0x7ee85c10, 0x7ee8f4cd, 0x7ee97047, 0x7ee9ce59,
    0x7eea0eca, 0x7eea3147, 0x7eea3568, 0x7eea1aab, 0x7ee9e071, 0x7ee98602, 0x7ee90a88,
    0x7ee86d08, 0x7ee7ac6a, 0x7ee6c769, 0x7ee5bc9c, 0x7ee48a67, 0x7ee32efc, 0x7ee1a857,
    0x7edff42f, 0x7ede0ffa, 0x7edbf8d9, 0x7ed9ab94, 0x7ed7248d, 0x7ed45fae, 0x7ed1585c,
    0x7ece095f, 0x7eca6ccb, 0x7ec67be2, 0x7ec22eee, 0x7ebd7d1a, 0x7eb85c35, 0x7eb2c075,
    0x7eac9c20, 0x7ea5df27, 0x7e9e769f, 0x7e964c16, 0x7e8d44ba, 0x7e834033, 0x7e781728,
    0x7e6b9933, 0x7e5d8a1a, 0x7e4d9ded, 0x7e3b737a, 0x7e268c2f, 0x7e0e3ff5, 0x7df1aa5d,
    0x7dcf8c72, 0x7da61a1e, 0x7d72a0fb, 0x7d30e097, 0x7cd9b4ab, 0x7c600f1a, 0x7ba90bdc,
    0x7a722176, 0x77d664e5,
};

static const float rand_wn[128] = {
    1.7290405e-09f, 1.2680929e-10f, 1.6897518e-10f, 1.9862688e-10f, 2.2232431e-10f,
    2.4244937e-10f, 2.601613e-10f,  2.7611988e-10f, 2.9073963e-10f, 3.042997e-10f,
    3.1699796e-10f, 3.289802e-10f,  3.4035738e-10f, 3.5121603e-10f, 3.616251e-10f,
    3.7164058e-10f, 3.8130857e-10f, 3.9066758e-10f, 3.9975012e-10f, 4.08584e-10f,
    4.1719309e-10f, 4.2559822e-10f, 4.338176e-10f,  4.418672e-10f,  4.497613e-10f,
    4.5751258e-10f, 4.651324e-10f,  4.7263105e-10f, 4.8001775e-10f, 4.87301e-10f,
    4.944885e-10f,  5.015873e-10f,  5.0860405e-10f, 5.155446e-10f,  5.2241467e-10f,
    5.2921934e-10f, 5.359635e-10f,  5.426517e-10f,  5.4928817e-10f, 5.5587696e-10f,
    5.624219e-10f,  5.6892646e-10f, 5.753941e-10f,  5.818282e-10f,  5.882317e-10f,
    5.946077e-10f,  6.00959e-10f,   6.072884e-10f,  6.135985e-10f,  6.19892e-10f,
    6.2617134e-10f, 6.3243905e-10f, 6.386974e-10f,  6.449488e-10f,  6.511956e-10f,
    6.5744005e-10f, 6.6368433e-10f, 6.699307e-10f,  6.7618144e-10f, 6.824387e-10f,
    6.8870465e-10f, 6.949815e-10f,  7.012715e-10f,  7.075768e-10f,  7.1389966e-10f,
    7.202424e-10f,  7.266073e-10f,  7.329966e-10f,  7.394128e-10f,  7.4585826e-10f,
    7.5233547e-10f, 7.58847e-10f,   7.653954e-10f,  7.719835e-10f,  7.7861395e-10f,
    7.852897e-10f,  7.920138e-10f,  7.987892e-10f,  8.0561924e-10f, 8.125073e-10f,
    8.194569e-10f,  8.2647167e-10f, 8.3355556e-10f, 8.407127e-10f,  8.479473e-10f,
    8.55264e-10f,   8.6266755e-10f, 8.7016316e-10f, 8.777562e-10f,  8.8545243e-10f,
    8.932582e-10f,  9.0117996e-10f, 9.09225e-10f,   9.174008e-10f,  9.2571584e-10f,
    9.341788e-10f,  9.427997e-10f,  9.515889e-10f,  9.605579e-10f,  9.697193e-10f,
    9.790869e-10f,  9.88676e-10f,   9.985036e-10f,  1.0085882e-09f, 1.0189509e-09f,
    1.0296151e-09f, 1.0406069e-09f, 1.0519566e-09f, 1.063698e-09f,  1.0758702e-09f,
    1.0885183e-09f, 1.1016947e-09f, 1.1154611e-09f, 1.1298902e-09f, 1.1450696e-09f,
    1.1611052e-09f, 1.1781276e-09f, 1.1962995e-09f, 1.2158287e-09f, 1.2369856e-09f,
    1.2601323e-09f, 1.2857697e-09f, 1.3146202e-09f, 1.347784e-09f,  1.3870636e-09f,
    1.4357403e-09f, 1.5008659e-09f, 1.6030948e-09f,
};

static const float rand_fn[128] = {
    1.0f,         0.9635997f,    0.9362827f,   0.9130436f,   0.89228165f,  0.87324303f,
    0.8555006f,   0.8387836f,    0.8229072f,   0.8077383f,   0.793177f,    0.7791461f,
    0.7655842f,   0.7524416f,    0.73967725f,  0.7272569f,   0.7151515f,   0.7033361f,
    0.69178915f,  0.68049186f,   0.6694277f,   0.658582f,    0.6479418f,   0.63749546f,
    0.6272325f,   0.6171434f,    0.6072195f,   0.5974532f,   0.58783704f,  0.5783647f,
    0.56903f,     0.5598274f,    0.5507518f,   0.54179835f,  0.5329627f,   0.52424055f,
    0.5156282f,   0.50712204f,   0.49871865f,  0.49041483f,  0.48220766f,  0.4740943f,
    0.46607214f,  0.4581387f,    0.45029163f,  0.44252872f,  0.43484783f,  0.427247f,
    0.41972435f,  0.41227803f,   0.40490642f,  0.39760786f,  0.3903808f,   0.3832238f,
    0.37613547f,  0.36911446f,   0.3621595f,   0.35526937f,  0.34844297f,  0.34167916f,
    0.33497685f,  0.3283351f,    0.3217529f,   0.3152294f,   0.30876362f,  0.30235484f,
    0.29600215f,  0.28970486f,   0.2834622f,   0.2772735f,   0.27113807f,  0.2650553f,
    0.25902456f,  0.2530453f,    0.24711695f,  0.241239f,    0.23541094f,  0.22963232f,
    0.2239027f,   0.21822165f,   0.21258877f,  0.20700371f,  0.20146611f,  0.19597565f,
    0.19053204f,  0.18513499f,   0.17978427f,  0.17447963f,  0.1692209f,   0.16400786f,
    0.15884037f,  0.15371831f,   0.14864157f,  0.14361008f,  0.13862377f,  0.13368265f,
    0.12878671f,  0.12393598f,   0.119130544f, 0.11437051f,  0.10965602f,  0.104987256f,
    0.10036444f,  0.095787846f,  0.0912578f,   0.08677467f,  0.0823389f,   0.077950984f,
    0.073611505f, 0.06932112f,   0.06508058f,  0.06089077f,  0.056752663f, 0.0526674f,
    0.048636295f, 0.044660863f,  0.040742867f, 0.03688439f,  0.033087887f, 0.029356318f,
    0.025693292f, 0.022103304f,  0.018592102f, 0.015167298f, 0.011839478f, 0.008624485f,
    0.005548995f, 0.0026696292f,
};

static const uint32_t rand_ke[256] = {
    0xe290a139, 0x0,        0x9beadebc, 0xc377ac71, 0xd4ddb990, 0xde893fb8, 0xe4a8e87c,
    0xe8dff16a, 0xebf2deab, 0xee49a6e8, 0xf0204efd, 0xf19bdb8e, 0xf2d458bb, 0xf3da104b,
    0xf4b86d78, 0xf577ad8a, 0xf61de83d, 0xf6afb784, 0xf730a573, 0xf7a37651, 0xf80a5bb6,
    0xf867189d, 0xf8bb1b4f, 0xf9079062, 0xf94d70ca, 0xf98d8c7d, 0xf9c8928a, 0xf9ff175b,
    0xfa319996, 0xfa6085f8, 0xfa8c3a62, 0xfab5084e, 0xfadb36c8, 0xfaff0410, 0xfb20a6ea,
    0xfb404fb4, 0xfb5e2951, 0xfb7a59e9, 0xfb95038c, 0xfbae44ba, 0xfbc638d8, 0xfbdcf892,
    0xfbf29a30, 0xfc0731df, 0xfc1ad1ed, 0xfc2d8b02, 0xfc3f6c4d, 0xfc5083ac, 0xfc60ddd1,
    0xfc708662, 0xfc7f8810, 0xfc8decb4, 0xfc9bbd62, 0xfca9027c, 0xfcb5c3c3, 0xfcc20864,
    0xfccdd70a, 0xfcd935e3, 0xfce42ab0, 0xfceebace, 0xfcf8eb3b, 0xfd02c0a0, 0xfd0c3f59,
    0xfd156b7b, 0xfd1e48d6, 0xfd26daff, 0xfd2f2552, 0xfd372af7, 0xfd3eeee5, 0xfd4673e7,
    0xfd4dbc9e, 0xfd54cb85, 0xfd5ba2f2, 0xfd62451b, 0xfd68b415, 0xfd6ef1da, 0xfd750047,
    0xfd7ae120, 0xfd809612, 0xfd8620b4, 0xfd8b8285, 0xfd90bcf5, 0xfd95d15e, 0xfd9ac10b,
    0xfd9f8d36, 0xfda43708, 0xfda8bf9e, 0xfdad2806, 0xfdb17141, 0xfdb59c46, 0xfdb9a9fd,
    0xfdbd9b46, 0xfdc170f6, 0xfdc52bd8, 0xfdc8ccac, 0xfdcc542d, 0xfdcfc30b, 0xfdd319ef,
    0xfdd6597a, 0xfdd98245, 0xfddc94e5, 0xfddf91e6, 0xfde279ce, 0xfde54d1f, 0xfde80c52,
    0xfdeab7de, 0xfded5034, 0xfdefd5be, 0xfdf248e3, 0xfdf4aa06, 0xfdf6f984, 0xfdf937b6,
    0xfdfb64f4, 0xfdfd818d, 0xfdff8dd0, 0xfe018a08, 0xfe03767a, 0xfe05536c, 0xfe07211c,
    0xfe08dfc9, 0xfe0a8fab, 0xfe0c30fb, 0xfe0dc3ec, 0xfe0f48b1, 0xfe10bf76, 0xfe122869,
    0xfe1383b4, 0xfe14d17c, 0xfe1611e7, 0xfe174516, 0xfe186b2a, 0xfe19843e, 0xfe1a9070,
    0xfe1b8fd6, 0xfe1c8289, 0xfe1d689b, 0xfe1e4220, 0xfe1f0f26, 0xfe1fcfbc, 0xfe2083ed,
    0xfe212bc3, 0xfe21c745, 0xfe225678, 0xfe22d95f, 0xfe234ffb, 0xfe23ba4a, 0xfe241849,
    0xfe2469f2, 0xfe24af3c, 0xfe24e81e, 0xfe25148b, 0xfe253474, 0xfe2547c7, 0xfe254e70,
    0xfe25485a, 0xfe25356a, 0xfe251586, 0xfe24e88f, 0xfe24ae64, 0xfe2466e1, 0xfe2411df,
    0xfe23af34, 0xfe233eb4, 0xfe22c02c, 0xfe22336b, 0xfe219838, 0xfe20ee58, 0xfe20358c,
    0xfe1f6d92, 0xfe1e9621, 0xfe1daef0, 0xfe1cb7ac, 0xfe1bb002, 0xfe1a9798, 0xfe196e0d,
    0xfe1832fd, 0xfe16e5fe, 0xfe15869d, 0xfe141464, 0xfe128ed3, 0xfe10f565, 0xfe0f478c,
    0xfe0d84b1, 0xfe0bac36, 0xfe09bd73, 0xfe07b7b5, 0xfe059a40, 0xfe03644c, 0xfe011504,
    0xfdfeab88, 0xfdfc26e9, 0xfdf98629, 0xfdf6c83b, 0xfdf3ec01, 0xfdf0f04a, 0xfdedd3d1,
    0xfdea953d, 0xfde7331e, 0xfde3abe9, 0xfddffdfb, 0xfddc2791, 0xfdd826cd, 0xfdd3f9a8,
    0xfdcf9dfc, 0xfdcb1176, 0xfdc65198, 0xfdc15bb3, 0xfdbc2ce2, 0xfdb6c206, 0xfdb117be,
    0xfdab2a63, 0xfda4f5fd, 0xfd9e7640, 0xfd97a67a, 0xfd908192, 0xfd8901f2, 0xfd812182,
    0xfd78d98e, 0xfd7022bb, 0xfd66f4ed, 0xfd5d4732, 0xfd530f9c, 0xfd48432b, 0xfd3cd59a,
    0xfd30b936, 0xfd23dea4, 0xfd16349e, 0xfd07a7a3, 0xfcf8219b, 0xfce7895b, 0xfcd5c220,
    0xfcc2aadb, 0xfcae1d5e, 0xfc97ed4e, 0xfc7fe6d4, 0xfc65ccf3, 0xfc495762, 0xfc2a2fc8,
    0xfc07ee19, 0xfbe213c1, 0xfbb8051a, 0xfb890078, 0xfb5411a5, 0xfb180005, 0xfad33482,
    0xfa839276, 0xfa263b32, 0xf9b72d1c, 0xf930a1a2, 0xf889f023, 0xf7b577d2, 0xf69c650c,
    0xf51530f0, 0xf2cb0e3c, 0xeeefb15d, 0xe6da6ecf,
};

static const float rand_we[256] = {
    2.0249555e-09f,  1.486674e-11f,   2.4409617e-11f,  3.1968806e-11f, 3.844677e-11f,
    4.4228204e-11f,  4.9516443e-11f,  5.443359e-11f,   5.905944e-11f,  6.344942e-11f,
    6.7643814e-11f,  7.1672945e-11f,  7.556032e-11f,   7.932458e-11f,  8.298079e-11f,
    8.654132e-11f,   9.0016515e-11f,  9.3415074e-11f,  9.674443e-11f,  1.0001099e-10f,
    1.03220314e-10f, 1.06377254e-10f, 1.09486115e-10f, 1.1255068e-10f, 1.1557435e-10f,
    1.1856015e-10f,  1.2151083e-10f,  1.2442886e-10f,  1.2731648e-10f, 1.3017575e-10f,
    1.3300853e-10f,  1.3581657e-10f,  1.3860142e-10f,  1.4136457e-10f, 1.4410738e-10f,
    1.4683108e-10f,  1.4953687e-10f,  1.5222583e-10f,  1.54899e-10f,   1.5755733e-10f,
    1.6020171e-10f,  1.6283301e-10f,  1.6545203e-10f,  1.6805951e-10f, 1.7065617e-10f,
    1.732427e-10f,   1.7581973e-10f,  1.7838787e-10f,  1.8094774e-10f, 1.8349985e-10f,
    1.8604476e-10f,  1.8858298e-10f,  1.9111498e-10f,  1.9364126e-10f, 1.9616223e-10f,
    1.9867835e-10f,  2.0119004e-10f,  2.0369768e-10f,  2.0620168e-10f, 2.087024e-10f,
    2.1120022e-10f,  2.136955e-10f,   2.1618855e-10f,  2.1867974e-10f, 2.2116936e-10f,
    2.2365775e-10f,  2.261452e-10f,   2.2863202e-10f,  2.311185e-10f,  2.3360494e-10f,
    2.360916e-10f,   2.3857874e-10f,  2.4106667e-10f,  2.4355562e-10f, 2.4604588e-10f,
    2.485377e-10f,   2.5103128e-10f,  2.5352695e-10f,  2.560249e-10f,  2.585254e-10f,
    2.6102867e-10f,  2.6353494e-10f,  2.6604446e-10f,  2.6855745e-10f, 2.7107416e-10f,
    2.7359479e-10f,  2.761196e-10f,   2.7864877e-10f,  2.8118255e-10f, 2.8372119e-10f,
    2.8626485e-10f,  2.888138e-10f,   2.9136826e-10f,  2.939284e-10f,  2.9649452e-10f,
    2.9906677e-10f,  3.016454e-10f,   3.0423064e-10f,  3.0682268e-10f, 3.0942177e-10f,
    3.1202813e-10f,  3.1464195e-10f,  3.1726352e-10f,  3.19893e-10f,   3.2253064e-10f,
    3.251767e-10f,   3.2783135e-10f,  3.3049485e-10f,  3.3316744e-10f, 3.3584938e-10f,
    3.3854083e-10f,  3.4124212e-10f,  3.4395342e-10f,  3.46675e-10f,   3.4940711e-10f,
    3.5215003e-10f,  3.5490397e-10f,  3.5766917e-10f,  3.6044595e-10f, 3.6323455e-10f,
    3.660352e-10f,   3.6884823e-10f,  3.7167386e-10f,  3.745124e-10f,  3.773641e-10f,
    3.802293e-10f,   3.8310827e-10f,  3.860013e-10f,   3.8890866e-10f, 3.918307e-10f,
    3.9476775e-10f,  3.9772008e-10f,  4.0068804e-10f,  4.0367196e-10f, 4.0667217e-10f,
    4.09689e-10f,    4.1272286e-10f,  4.1577405e-10f,  4.1884296e-10f, 4.2192994e-10f,
    4.250354e-10f,   4.281597e-10f,   4.313033e-10f,   4.3446652e-10f, 4.3764986e-10f,
    4.408537e-10f,   4.4407847e-10f,  4.4732465e-10f,  4.5059267e-10f, 4.5388301e-10f,
    4.571962e-10f,   4.6053267e-10f,  4.6389292e-10f,  4.6727755e-10f, 4.70687e-10f,
    4.741219e-10f,   4.7758275e-10f,  4.810702e-10f,   4.845848e-10f,  4.8812715e-10f,
    4.9169796e-10f,  4.9529775e-10f,  4.989273e-10f,   5.0258725e-10f, 5.0627835e-10f,
    5.100013e-10f,   5.1375687e-10f,  5.1754584e-10f,  5.21369e-10f,   5.2522725e-10f,
    5.2912136e-10f,  5.330522e-10f,   5.370208e-10f,   5.4102806e-10f, 5.45075e-10f,
    5.491625e-10f,   5.532918e-10f,   5.5746385e-10f,  5.616799e-10f,  5.6594107e-10f,
    5.7024857e-10f,  5.746037e-10f,   5.7900773e-10f,  5.834621e-10f,  5.8796823e-10f,
    5.925276e-10f,   5.971417e-10f,   6.018122e-10f,   6.065408e-10f,  6.113292e-10f,
    6.1617933e-10f,  6.2109295e-10f,  6.260722e-10f,   6.3111916e-10f, 6.3623595e-10f,
    6.4142497e-10f,  6.4668854e-10f,  6.5202926e-10f,  6.5744976e-10f, 6.6295286e-10f,
    6.6854156e-10f,  6.742188e-10f,   6.79988e-10f,    6.858526e-10f,  6.9181616e-10f,
    6.978826e-10f,   7.04056e-10f,    7.103407e-10f,   7.167412e-10f,  7.2326256e-10f,
    7.2990985e-10f,  7.366886e-10f,   7.4360473e-10f,  7.5066453e-10f, 7.5787476e-10f,
    7.6524265e-10f,  7.7277595e-10f,  7.80483e-10f,    7.883728e-10f,  7.9645507e-10f,
    8.047402e-10f,   8.1323964e-10f,  8.219657e-10f,   8.309319e-10f,  8.401528e-10f,
    8.496445e-10f,   8.594247e-10f,   8.6951274e-10f,  8.799301e-10f,  8.9070046e-10f,
    9.018503e-10f,   9.134092e-10f,   9.254101e-10f,   9.378904e-10f,  9.508923e-10f,
    9.644638e-10f,   9.786603e-10f,   9.935448e-10f,   1.0091913e-09f, 1.025686e-09f,
    1.0431306e-09f,  1.0616465e-09f,  1.08138e-09f,    1.1025096e-09f, 1.1252564e-09f,
    1.1498986e-09f,  1.1767932e-09f,  1.206409e-09f,   1.2393786e-09f, 1.276585e-09f,
    1.3193139e-09f,  1.3695435e-09f,  1.4305498e-09f,  1.508365e-09f,  1.6160854e-09f,
    1.7921248e-09f,
};

static const float rand_fe[256] = {
    1.0f,           0.9381437f,    0.90046996f,   0.87170434f,   0.8477855f,
    0.8269933f,     0.8084217f,    0.7915276f,    0.77595687f,   0.7614634f,
    0.7478686f,     0.7350381f,    0.72286767f,   0.71127474f,   0.70019263f,
    0.6895665f,     0.67935055f,   0.6695063f,    0.66000086f,   0.65080583f,
    0.6418967f,     0.63325197f,   0.6248527f,    0.6166822f,    0.60872537f,
    0.60096896f,    0.5934009f,    0.58601034f,   0.5787874f,    0.57172304f,
    0.5648092f,     0.5580383f,    0.5514034f,    0.5448982f,    0.5385169f,
    0.53225386f,    0.5261042f,    0.52006316f,   0.5141264f,    0.50828975f,
    0.5025495f,     0.496902f,     0.49134386f,   0.485872f,     0.48048335f,
    0.4751752f,     0.46994483f,   0.46478975f,   0.45970762f,   0.45469615f,
    0.44975325f,    0.44487688f,   0.44006512f,   0.43531612f,   0.43062815f,
    0.42599955f,    0.42142874f,   0.4169142f,    0.41245446f,   0.40804818f,
    0.403694f,      0.3993907f,    0.39513698f,   0.39093173f,   0.38677382f,
    0.38266218f,    0.37859577f,   0.37457356f,   0.37059465f,   0.3666581f,
    0.362763f,      0.35890847f,   0.35509375f,   0.351318f,     0.3475805f,
    0.34388044f,    0.34021714f,   0.3365899f,    0.33299807f,   0.32944095f,
    0.32591796f,    0.3224285f,    0.3189719f,    0.31554767f,   0.31215525f,
    0.30879408f,    0.3054636f,    0.3021634f,    0.29889292f,   0.2956517f,
    0.29243928f,    0.28925523f,   0.28609908f,   0.28297043f,   0.27986884f,
    0.27679393f,    0.2737453f,    0.2707226f,    0.2677254f,    0.26475343f,
    0.26180625f,    0.25888354f,   0.25598502f,   0.2531103f,    0.25025907f,
    0.24743107f,    0.24462597f,   0.24184346f,   0.23908329f,   0.23634516f,
    0.23362878f,    0.23093392f,   0.2282603f,    0.22560766f,   0.22297576f,
    0.22036438f,    0.21777324f,   0.21520215f,   0.21265087f,   0.21011916f,
    0.20760682f,    0.20511365f,   0.20263945f,   0.20018397f,   0.19774707f,
    0.19532852f,    0.19292815f,   0.19054577f,   0.1881812f,    0.18583426f,
    0.18350479f,    0.1811926f,    0.17889754f,   0.17661946f,   0.17435817f,
    0.17211354f,    0.1698854f,    0.16767362f,   0.16547804f,   0.16329853f,
    0.16113494f,    0.15898713f,   0.15685499f,   0.15473837f,   0.15263714f,
    0.15055119f,    0.14848037f,   0.14642459f,   0.14438373f,   0.14235765f,
    0.14034624f,    0.13834943f,   0.13636707f,   0.13439907f,   0.13244532f,
    0.13050574f,    0.1285802f,    0.12666863f,   0.12477092f,   0.12288698f,
    0.12101672f,    0.119160056f,  0.1173169f,    0.115487166f,  0.11367077f,
    0.11186763f,    0.11007768f,   0.10830083f,   0.10653701f,   0.10478614f,
    0.10304816f,    0.101323f,     0.09961058f,   0.09791085f,   0.09622374f,
    0.09454919f,    0.09288713f,   0.091237515f,  0.08960028f,   0.087975375f,
    0.08636274f,    0.08476233f,   0.083174095f,  0.081597984f,  0.08003395f,
    0.07848195f,    0.076941945f,  0.07541389f,   0.07389775f,   0.072393484f,
    0.07090106f,    0.069420435f,  0.06795159f,   0.066494495f,  0.06504912f,
    0.063615434f,   0.062193416f,  0.060783047f,  0.059384305f,  0.057997175f,
    0.05662164f,    0.05525769f,   0.053905312f,  0.052564494f,  0.051235236f,
    0.049917534f,   0.048611384f,  0.047316793f,  0.046033762f,  0.0447623f,
    0.043502413f,   0.042254124f,  0.041017443f,  0.039792392f,  0.038578995f,
    0.037377283f,   0.036187284f,  0.035009038f,  0.033842582f,  0.032687962f,
    0.031545233f,   0.030414443f,  0.02929566f,   0.02818895f,   0.027094385f,
    0.026012046f,   0.024942026f,  0.023884421f,  0.022839336f,  0.021806888f,
    0.020787204f,   0.019780423f,  0.0187867f,    0.0178062f,    0.016839107f,
    0.015885621f,   0.014945968f,  0.014020392f,  0.013109165f,  0.012212592f,
    0.011331013f,   0.01046481f,   0.009614414f,  0.008780315f,  0.007963077f,
    0.0071633533f,  0.006381906f,  0.0056196423f, 0.0048776558f, 0.004157295f,
    0.0034602648f,  0.0027887989f, 0.0021459677f, 0.0015362998f, 0.0009672693f,
    0.00045413437f,
};

static const uint64_t rand_rng_cooked[607] = {
    0xc5f74a33eb98ffeaU, 0xc07b4a41ba965f5bU, 0x135ec513cb81af0fU, 0x4a04fb5ca74395ebU,
    0xa7e8831c7710cf8eU, 0x7d5de4e88f37c1c3U, 0x6321cf7743ff85a9U, 0x42cb06556a9ff961U,
    0x6e26c8d140e8d7d2U, 0x49a7511f3411faa0U, 0x71ed774922a65055U, 0x9d5c9fc65fea2d5cU,
    0x3fbdab394a014368U, 0x70fa33596992340bU, 0xae10fb4ecea66a02U, 0xf79fbca98bbcd18aU,
    0x97f7ccb12ea380acU, 0x0191d794caf32b1bU, 0x3f6a323424e32cafU, 0xc642cc921f01142bU,
    0xa122869f4ff38068U, 0xa5b1a31127af3920U, 0x5b07a4f9bdc29d96U, 0x6a2a973c29812586U,
    0x6aa433dbf77a52e1U, 0x8394216e3b8ac99eU, 0xdb80a9c5515b997cU, 0x03c3f2eff3cbf769U,
    0xa6b5e431eeff210cU, 0x0ec85334992953f5U, 0x2e6a1432df10d1e9U, 0xbde4629959d1b149U,
    0x67994c3876387951U, 0x0e15d461559bae63U, 0xbdbf96c4e9d2a077U, 0xb04f62866ada20f3U,
    0x2190da1646db97bfU, 0x5071873c1832957aU, 0x3e3b503a19da904bU, 0x33e388a5f2c78528U,
    0xbec70bac19d0e877U, 0x00256970f81bf072U, 0xf7d18a14f64fa886U, 0xa46ef978b4d4734cU,
    0x782ff1573127a78eU, 0x00241b0ad59beba6U, 0xdac549591fe95b2fU, 0x6195faf2884c2d39U,
    0x569c2bbdb0ef345dU, 0x0b8b6039508ce21eU, 0xef1576705435d2bbU, 0x1d5e900af5f1d8f7U,
    0x1e902d187d7fd437U, 0x7f227b7c8fe0eb79U, 0x38790e6201ebbb98U, 0x44818eac7922e171U,
    0x0658d6c550dad340U, 0x84bfb894cc72969aU, 0xa7855141e3b329bdU, 0x9cb31556b1bae746U,
    0x3ca09004f9686f49U, 0x0c50146d7e508769U, 0xcd35d8ff935ac970U, 0xde9818b4aa1554d8U,
    0x4df37efd6433daa0U, 0xf591e9ad05b2e056U, 0x1767eb0bdbf83e29U, 0xb8247292535299f2U,
    0xbae645975a5e4cebU, 0xecea78155c3bc32bU, 0x4e295e5357ace86eU, 0x8b906a9a4fadbb08U,
    0xfd50086af64463c9U, 0xad0385acfd19d6fcU, 0xbbe6d4a5e1d13a9fU, 0xfc099f33b79b039cU,
    0xd9782224840c5a35U, 0x174e38bc45d51acfU, 0x53f82f9f54de4e3eU, 0xe10dfa0a87b3ef0eU,
    0xea09d9ec7b439769U, 0x519ec3c7cc4c5011U, 0x0b47990fc413e3d5U, 0x3f07e07ecfce5be2U,
    0xa5d2d0b8b71922beU, 0x451eea1fc87cc761U, 0xc8505edef7f1d6ddU, 0xfb6ed78e001ce905U,
    0xd7cc0a3644f468baU, 0x14c231db2e4429d0U, 0xe21ccd9a90943dd3U, 0x664869f5288fc2d1U,
    0xddc56f604e168fb5U, 0x4151e4abb0b1faf6U, 0xec9647e8cea7b927U, 0x76a742187dbe61deU,
    0x80778d4049780e3cU, 0x92714446c26ce413U, 0x65475bc019e7700bU, 0x0e08781d60d86d17U,
    0x9f0f0fe9d5f571e0U, 0x476e62bf33886e57U, 0x8c710cff524eb4e6U, 0x24e856a59e3899a5U,
    0x549f7c8b5783189aU, 0x9808539c06020014U, 0xb8fc59eeb2515467U, 0xf38fff8b8499a614U,
    0xf5a62be73a3d5222U, 0x508b82832b4c4838U, 0xf5109fc7682ff8deU, 0xd31cadf15cc53f0cU,
    0xc1720509406b5826U, 0xe900715210bc96faU, 0x0704fe3536d393fbU, 0xc65c9b6eccaefb4aU,
    0x8bafc73092be4019U, 0x07f0b599d77889eaU, 0x18de0a49b8fa589bU, 0x2f174e71c9830ff0U,
    0xb470b780a26785ddU, 0x20a6b76123b162beU, 0x91efbd24cdc58322U, 0xfb7bb75026b32623U,
    0xd03d1caf9a1f04d0U, 0xa5f0f909bcaee921U, 0x46aa7ced9d1c8e0bU, 0xfcd8a79e7a62b6f9U,
    0x3ac4cf05bce83f14U, 0x45c579a008a037c4U, 0x4053888e527912d0U, 0xf88fa0b2289f7b17U,
    0x51fbaf5d2002bddaU, 0x8ae403a812ca4784U, 0x0d7ea237329fa450U, 0xcb3f04bf02b6f208U,
    0xc796643e8a39ed33U, 0xfd92d79ed75b6419U, 0x216821c8b258e8acU, 0xe93d3aff7b46e931U,
    0x36b0858cfcb72b6aU, 0xae5e3c9fe7ff1167U, 0x2af4c395391cc2a5U, 0xe6c4fd076feb7676U,
    0x50efb5fb31ede773U, 0x6d7f243359dbfcaeU, 0x7193e740340e0fdbU, 0x8ff51f73a4c6dd92U,
    0x94534b6efa1ca966U, 0xc06a724520d70d41U, 0x30cd16bd19120592U, 0x8d456b580c2cd6dcU,
    0x5c62bc0d6a8429efU, 0x48744ea9b8e9395eU, 0x18ef14825c84aee1U, 0x4a1e32b73a71e234U,
    0x0fee6f27c45228c0U, 0x464e2e3be0d2e4a1U, 0x23bb67f482e16008U, 0x0f11b541e8247327U,
    0xae7df90c7d2aafd7U, 0x54e1f496142c391aU, 0x1d400ef625fa4274U, 0x9906966610ce9e13U,
    0x22455934d7c1ccdaU, 0xe6414239b7da9220U, 0xb49f9091f6275556U, 0x82916b3d64288abdU,
    0x9f52d0a1c7066fa0U, 0x642a666cfb8cf0a1U, 0x8c9d981db063ad83U, 0x63dfa26158b67e4aU,
    0xce3cfe36ebecc2dfU, 0xb6e4eb2ba55364ffU, 0x6fd2179e0d7717d1U, 0xb971a2b7362247dbU,
    0xdbe5cf25d510c53bU, 0x930f28ad73735261U, 0x5a95dd01047feb8bU, 0x30adb1407358b45aU,
    0x7fb44776784c1083U, 0x1b20e8f4eb075406U, 0xdc275f40b6c7c0d9U, 0x732df8dda67b0f27U,
    0xb62637afc484c885U, 0x61c98649838bf631U, 0xdf061fee30277f58U, 0x95d322052dac1455U,
    0x7ad30486e478aee9U, 0x3267221f4aced145U, 0x479807052c3566b8U, 0xa8025350a77ea0b9U,
    0x421a7ec2fc169cd3U, 0x57273ab05129a700U, 0xbae4f7fb719f43f2U, 0x1c5bd857e8b8bf48U,
    0xdad594186971f359U, 0x3f518aad9c427014U, 0x4c43c3b8cc8e8218U, 0x981fba566f92dfb7U,
    0x07acaac55435c0acU, 0x5fb0881038cbbc8fU, 0xf243b5556cbeba73U, 0x63149f303be79fd4U,
    0xf6aade431f712475U, 0x4cd2a557b6cbc24bU, 0x21bc6975c169de72U, 0xcaf78cf40c768134U,
    0xf13cdd624a12159fU, 0xd573d0041b390d3eU, 0x69ea8bd93bade806U, 0x578baf3d78cd61f2U,
    0x3318a93a7f689dc9U, 0x2da7501cbdbe5235U, 0x5b6d448c9bb11200U, 0x036566ca66f231b4U,
    0xb89e51e9e2b737e4U, 0x704687541c9b68cfU, 0x28df28a35d2937c9U, 0x8bfeb2756944b0b5U,
    0x29c24143c2234e9cU, 0xf4fd794f8d02068fU, 0x0590f068df15e57dU, 0xe4ad98adab8dc009U,
    0x4155b1828194a406U, 0xce463027d4ff743fU, 0xd306231168ec0845U, 0xae01984986c0c339U,
    0xcc61983656980e5aU, 0xb6b4b212af3d051aU, 0x70aaf14a706ce17aU, 0x3547cbc995de3a40U,
    0x6202b8d9e9e8565fU, 0x8a49ea181ddbac34U, 0x53a7bcb92853ce41U, 0xa2aa132546a16f3aU,
    0xd6c166634bc92862U, 0x92ff6acebc93a071U, 0x7a403280a96cea51U, 0x5a183b8f81665a24U,
    0x222b4c80d2c0e464U, 0x859e3d4d58ba3aa6U, 0xb9a28389cb2fe563U, 0x83a20cb89512fee4U,
    0xcbd691b061b30cbdU, 0x8839baf8f3ac9f09U, 0x5ddf03b3edd1c4b8U, 0xf62694c4cb82ed09U,
    0xd0c603b9879bd360U, 0x8558daf780426657U, 0xc87fdb1b629e32abU, 0xd77938d516862fe1U,
    0xe33a56962759ec00U, 0x051bc82f174b90d9U, 0xab3b5debcad25c86U, 0xa8766bd0dd9b35dbU,
    0x425f26b683ca1487U, 0x5d3c2b40a076f653U, 0x7464e98a72f8d634U, 0x2f02dd3eea4e7841U,
    0x40977fe842527877U, 0xfbf01ad4922b1d62U, 0xca959c2a8b4eea29U, 0x69ca7ca110533802U,
    0x6b36496357247bd9U, 0x7f7c07645ca7b232U, 0xb8c63fa50ae3677bU, 0xa0b7fd74703947d8U,
    0x231fb1a0dc8ce079U, 0x16b63342b7c96e77U, 0xcd39a909d7e68027U, 0x1427685b19caecddU,
    0x9c9c6cd28e91b74dU, 0x91c1fb790e0d3a54U, 0x7e67d3bdb81bb611U, 0xd396fc0d89f6e211U,
    0x457eacfbb049eb22U, 0x1de1979bf5654dd5U, 0x5d436ca7585d28caU, 0xd3d7e2863218d979U,
    0x2fa7be3ccf9d1645U, 0x1fb7a7e17b9eccb0U, 0x2b2b1fced929c282U, 0xd85a90375e64310dU,
    0xcb336a1611227dbdU, 0x636925f5baf870cdU, 0x43e0d0254b84715eU, 0x080ec57577219247U,
    0x175ee40f58fbd971U, 0x3d78cc365badbce1U, 0x8495a07683903295U, 0x8f89c0d3396a3b53U,
    0x639ba81c88d888a6U, 0x3faf8d9c775ca781U, 0xeac40d3f519d2893U, 0x01fe31d8c7df7dc4U,
    0x036d8003387dd6dbU, 0x8c0621f768220a97U, 0x59d6a8cc7a082707U, 0x2aea572af8467560U,
    0x1c910136c9fdf9c3U, 0xc73eddfe111385e2U, 0x79edb9a2a6a387dfU, 0xd556d17adcc5f6fbU,
    0xf72709f96b28a3c7U, 0x552d69f0428dce6aU, 0x9cbbb4be9a92c216U, 0x4f37aef6caf41c5dU,
    0xd3431110cdd7a998U, 0x3c7c1de5b3cc1dcaU, 0x048311a7625eabd9U, 0x5ee41cb40ca81e2cU,
    0x43385b68e51dbffcU, 0xd36a1f70d4f9a2ffU, 0xcaeaccb927b0cb5cU, 0x3fddae4163e010e4U,
    0x0e7f5452a8c91ee0U, 0x7e61315458970ccbU, 0x8b821c0b10054c75U, 0xc444cf11f9840608U,
    0x2536b7a7602f6b4dU, 0x331786d5a126f97fU, 0xc9a9cd708372db93U, 0xd7e75a07542451daU,
    0xa4e5a10d28659104U, 0x9024ebcbf8b8e6afU, 0x08137c3380f32523U, 0x32a31118a95e425fU,
    0xbd640b6140802b1eU, 0x967e3cd0f12b1c5fU, 0x031089e87fbab9a7U, 0xf11bfbb3ba3e0841U,
    0x3b7fc3ad0d1cd36bU, 0xbfb2f4d968b759c3U, 0x4c8475564905d202U, 0xe0a80b2a6236fa83U,
    0x86a5b8671a833ae3U, 0x79608828eb8fe58dU, 0x032bcb9f5721e717U, 0x12d224e5dc7e16deU,
    0x29e68b8251e0d495U, 0x2bc04a7508cb08a3U, 0x85f835a648329d25U, 0x4d2799f797ec19a8U,
    0x443aadc076f2ed10U, 0x89357de277e46717U, 0xaf7907aeb63c9d6cU, 0x27ce6be004db5b35U,
    0x9bbf0228d95bf1d8U, 0xae31e17b25e1b363U, 0x23e9429e957db925U, 0x950e5fab3c7e6825U,
    0x2aea3671bf9f5933U, 0xb2d2ef433932102cU, 0x0d7ed21ec77fc8c1U, 0xea5f1280f48e6247U,
    0x17b747cdf311ae42U, 0xe001060ea9fa838eU, 0xaca7508c5bccfc84U, 0xa3aaa0843f04f963U,
    0xf949f7a4e8ebd3a5U, 0x22876bdefd41df6cU, 0xc124724f07e69e7cU, 0xbf470df23d5aa6b4U,
    0x2998352099f590b8U, 0x11cc90f45895653dU, 0x03550da1ab28f8d1U, 0x7842cc5c2b1b34dbU,
    0x08b79b3694ac8400U, 0xbf72d50b84f96e2cU, 0x947079f8eb08f590U, 0x696cb72182050872U,
    0xd4885bc9ac36d5a7U, 0x6cb888b579fe97e1U, 0x629482105204e4fdU, 0x3cef790816b93701U,
    0xbda77a26be136556U, 0xfb8f861314dda828U, 0x08a2b7b803ae5bcfU, 0xd3c8a91ed0323140U,
    0x870001a0b867ec3eU, 0xe4f6c1f9b093dde5U, 0x8e086becbc0b8378U, 0x9e755197c06ffcc7U,
    0x246705ee34f204f8U, 0xc7927582d516264cU, 0xb3e17ba47546133cU, 0xa33f111b5caab4e2U,
    0xa23d8e161443255eU, 0x48f128b344d0819cU, 0x6cd122fb39ab204dU, 0x5c2910bf238f36aaU,
    0x90aeac10e05f9bbaU, 0x9d54b6b00fe42acaU, 0xe88f64387a0fcdcbU, 0xc45a205f01473609U,
    0xc768ed8eb6b8c82dU, 0xf225920d43fd23acU, 0x25bdad5a8a817964U, 0x7ebc9cefb9015aabU,
    0x3c1ee78fa4ea733dU, 0xe30e85d1757922a4U, 0x61123dc5e081fe21U, 0xdc86c11079674e66U,
    0x8918973fb91ab9fbU, 0x00d4ee55ca6d8c2aU, 0xd500476d5215b5b5U, 0xfc5068b3e2ea6d2aU,
    0x205da910dd23cb85U, 0x979b5e11369f775bU, 0x2bc872747d65ead7U, 0x96ba83aa2ac19de7U,
    0xa2258710d3a06bd5U, 0x02bcfbd9169343e7U, 0x88eecd99246b6273U, 0x234b301b2d3c0302U,
    0x3c61095d0931ecbfU, 0xc1fbeae253e44498U, 0x9bf8d56e39d28810U, 0xef025e3275852770U,
    0x3b81d83bc6a5b26dU, 0x740e3af17358a76bU, 0x5a0491610c217837U, 0xb234c04ee9707bbaU,
    0xe8d67481cf12c324U, 0x3dc1a80712de14dfU, 0x9646cd056265e405U, 0xc9ed5b85fe5de223U,
    0xc034e6309514ca00U, 0xa55cb1a9ea35e9bdU, 0x83585dac1418cf4cU, 0xc6bfe6b67ae71dfaU,
    0xedde6a8095841598U, 0x8c478074a66108d3U, 0x49f4fb28492601c4U, 0x6c1f1e9a65e7acb8U,
    0x44c3f220e0a3a35fU, 0xc4422797ac160d7eU, 0xa9bed55a11ad2c15U, 0x29bdbe2cde52ac8dU,
    0xa2486ed01638fb0cU, 0x7be91fdd8fa25058U, 0x6d06c94a2118cf50U, 0xbe19f2b9fcb649e5U,
    0x6da310d0ddefe702U, 0x9db4d6197fe76870U, 0xa1a8e96dd484eb63U, 0xa2803dfd82f85916U,
    0x3a1a0a44d7644c46U, 0x1a25188bb552aa62U, 0xfa3eab18c620bd11U, 0x33d24f4edd10ea99U,
    0x386d39256536cc6cU, 0x607e759efb2ba1e8U, 0x921ebb3d7c6c6071U, 0x96ba6b088f86692fU,
    0xb2bd2aeaeafed279U, 0xb70cc2def3ab10acU, 0x2b1c1284b0b6db2bU, 0xd7d4c4db5bbdff92U,
    0x3f5b8190a0a0241eU, 0x1b79f6457e60cfaeU, 0xa05c318c94181e91U, 0x3480a671b033e3d5U,
    0x7579955efbbaa755U, 0x2777c9f6d0c6b0d7U, 0x2a0f21c2f6662c43U, 0xc286b4bc45869bbcU,
    0x3de266b1c86ed249U, 0x0596478ee80942c5U, 0x6fd3618b2ee6ae1bU, 0x3f08073a3e5dea77U,
    0x0e7847d01d6e71afU, 0xcd1f81afc4fbaafeU, 0x24be71c60778c940U, 0x1dc2b6df046a9621U,
    0xd0944e93950f6ccfU, 0xe2c767041ea34007U, 0x521bf9c9206576c6U, 0x866b7a2bd7715e51U,
    0x7a97753c8c793641U, 0x52daac19784a65e4U, 0x07d3110b7c73f7ebU, 0xa66b273fbcc429b7U,
    0x16302dc85a75540aU, 0xc840d3700a56e4deU, 0xd6875303fcd05bffU, 0xabbeed06a5e32588U,
    0x758a6526f8636a3aU, 0x01be7adae8e62038U, 0xc9b4bec75bc4f40aU, 0xdc7a69ce0a9fc1c3U,
    0xf93f853c0adbaa64U, 0xbe48b8658e39d5b6U, 0x5233cc88f307cb85U, 0x81f48c566746e404U,
    0xe6e72b2f3e111192U, 0x6ee3ad5d7b4333d5U, 0x3bebf80abf44e35eU, 0x324f3dfa2996372cU,
    0xa60900d584d3294fU, 0xcfb39707d3e017c8U, 0xd33cd1090a56f237U, 0xfae17ac7e5605d43U,
    0xc8c0106f0ead4419U, 0x89bba8037b2a60beU, 0x6999983c12a6557dU, 0x29ecb289d4fc6351U,
    0xdc4e3ec308888d22U, 0x2492188c7c1156a7U, 0x476502781cf85a10U, 0x8e5d5db3b1c10b44U,
    0x15bff6623ba91eb7U, 0x716a28bba3f09c65U, 0xd7f47607c92f30f1U, 0x9e04907834879dadU,
    0xa611c59795db7099U, 0x9a74c74315758aa3U, 0x536d4c40116f3239U, 0x52539ac587ad97d0U,
    0x1f1a1675f28959c3U, 0x8c6d78f5619e8324U, 0x2af1cfe563eff7adU, 0x3f9d2212e5c94b1dU,
    0x7e023d467b37b61fU, 0xfaab76adabeee96bU, 0xcff95dd9926b62d1U, 0x3aa278a62172db76U,
    0x9982abe519c8c867U, 0x34c2118df1f5857eU, 0xfa7376b640136c80U, 0x98a7d04ca29ade2fU,
    0x6d9f14b04d77e77eU, 0x4b3dad048bed47a1U, 0x4c9ee5e4931d3231U, 0xbaf4eba543f33d0aU,
    0x793aafa07373d926U, 0x986ea60fe6b39a32U, 0xe31f344f666361e6U, 0xf21cbedc44c049cbU,
    0xc4f752a0eb5e5403U, 0xaf1b99e27d410520U, 0x592740a0e5b8c3ddU, 0x04fc4ea5fb446c97U,
    0xad6e587f47afe7a6U, 0xde542c137b8384a4U, 0x2a0ffc0f7b301e5cU, 0x820547300a5d8af4U,
    0xd3c639c2087dc079U, 0x0a467ca8709a2bb6U, 0xe242bb6bb4da36deU, 0x6079b0531ec90376U,
    0x058ccf98dc1843f7U, 0x8417b417b011aec1U, 0xecdedd4ff0dd641bU, 0x8c3787282349e832U,
    0xd5f0bb8816e7e8fbU, 0x85b650480fd8c2f4U, 0x7d45aa734f760b6dU, 0x5962428553d20814U,
    0x400e2ba01968e7c4U, 0x077e19b9a3f76000U, 0x1d17558f6164030cU, 0x79753148c0e6c59fU,
    0x9a77d2f2ef3bd843U, 0x666eb88d8701e16fU, 0x5572e7ce34288668U, 0xeb8d82b15491d607U,
    0x77c265a71aebbe54U, 0x4947dddd6f3e9e07U, 0x628dfe635c795586U, 0x22673e39d43ff99bU,
    0x637e086dc78cf522U, 0xee37e3669f7e9c66U, 0x92d7bb47a4afe838U, 0x1f9eabdfdbe2b7bdU,
    0x324fca6af2b5c1d1U, 0x59e4a152525b1b29U, 0x7c91d10a8d75740eU, 0xcc7c533a7fca95d8U,
    0x1266fbf77386b1dbU, 0x689a66ba46d1c3bdU, 0x4bd671b124b5701aU, 0x2c43fd0a9eaec04dU,
    0x58143117e4e16555U, 0x8358c5bb1248ac23U, 0xaaf677bd80e30ac5U, 0xfc997227a4f07f8fU,
    0xa848cc5d9cb0e150U, 0x640a711eb0e49b41U, 0x64fcbfa01dca3978U, 0x980aab2fff6bc784U,
    0x395c6750aa42e65eU, 0x287fea4c9695f3f4U, 0x9516361749f6428aU, 0xc977d592999f2194U,
    0xc45166d9f6935ff8U, 0xa7f503a28f431ca9U, 0x51d3a3c351be10ecU, 0xf579e080162896e9U,
    0xe5b5e9385b202824U, 0xfbba1e4a59b0c60cU, 0xa8de655829aab207U, 0xe1de048dc78b382eU,
    0x74535a96cc7adfd7U, 0x7e57a19b735ef03bU, 0x39a00a3a31c025c6U,
};

enum {
    RNG_LEN = 607,
    RNG_TAP = 273,
};

#define RNG_MASK ((uint64_t)INT64_MAX)
#define INT32_MAX_ ((int32_t)INT32_MAX)

/* The start of the ziggurat's tails. */
#define RAND_RN 3.442619855899
#define RAND_RE 7.69711747013104972

static uint32_t rand_abs_int32(int32_t i) {
    return i < 0 ? (uint32_t)0 - (uint32_t)i : (uint32_t)i;
}

/* The one error nobody can get, from a Read that cannot fail. */
static void rand_no_error(Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;
}

/* ============================================================ math/rand/v2 */

/* ------------------------------------------------------------------- PCG */

void mathrand2_pcg_seed(Mathrand2PCG *p, uint64_t seed1, uint64_t seed2) {
    p->hi = seed1;
    p->lo = seed2;
}

Mathrand2PCG *mathrand2_new_pcg(Alloc *a, uint64_t seed1, uint64_t seed2) {
    Mathrand2PCG *p = BURROW_NEW(a, Mathrand2PCG);
    if (p != NULL)
        mathrand2_pcg_seed(p, seed1, seed2);
    return p;
}

/* The 128 bit LCG step and then the DXSM permutation of the new state, which
 * is the version of PCG numpy uses as well. */
static inline uint64_t pcg_next(Mathrand2PCG *p) {
    const uint64_t mul_hi = 2549297995355413924U;
    const uint64_t mul_lo = 4865540595714422341U;
    const uint64_t inc_hi = 6364136223846793005U;
    const uint64_t inc_lo = 1442695040888963407U;

    uint64_t lo;
    uint64_t hi = bits_mul64(p->lo, mul_lo, &lo);
    hi += p->hi * mul_lo + p->lo * mul_hi;
    uint64_t c;
    lo = bits_add64(lo, inc_lo, 0, &c);
    hi = hi + inc_hi + c;
    p->lo = lo;
    p->hi = hi;

    const uint64_t cheap_mul = 0xda942042e4dd58b5U;
    hi ^= hi >> 32;
    hi *= cheap_mul;
    hi ^= hi >> 48;
    hi *= (lo | 1);
    return hi;
}

uint64_t mathrand2_pcg_uint64(Mathrand2PCG *p) {
    return pcg_next(p);
}

static void rand_put_be64(Byte *b, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        b[i] = (Byte)v;
        v >>= 8;
    }
}

static uint64_t rand_be64(const Byte *b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | b[i];
    return v;
}

static void rand_put_le64(Byte *b, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        b[i] = (Byte)v;
        v >>= 8;
    }
}

static uint64_t rand_le64(const Byte *b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | b[i];
    return v;
}

/* append for bytes, saying whether the allocator kept up. */
static Slice rand_append(Alloc *a, Slice b, const Byte *p, Int n, bool *ok) {
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    Int want = b.len + n;
    b = slice_append(a, b, p, n);
    if (b.len != want)
        *ok = false;
    return b;
}

static const Str pcg_invalid_text = {(const Byte *)"invalid PCG encoding", 20};
static const Error pcg_err_invalid = {&burrow_sentinel_error_vt, &pcg_invalid_text};

Slice mathrand2_pcg_append_binary(Mathrand2PCG *p, Alloc *a, Slice b, Error *err) {
    Byte buf[20];
    memcpy(buf, "pcg:", 4);
    rand_put_be64(buf + 4, p->hi);
    rand_put_be64(buf + 12, p->lo);
    bool ok = true;
    b = rand_append(a, b, buf, 20, &ok);
    BURROW_OUT(err, ok ? BURROW_NO_ERROR : burrow_err_out_of_memory);
    return b;
}

Slice mathrand2_pcg_marshal_binary(Mathrand2PCG *p, Alloc *a, Error *err) {
    return mathrand2_pcg_append_binary(p, a, slice_make(a, TYPE_BYTE, 0, 20), err);
}

Error mathrand2_pcg_unmarshal_binary(Mathrand2PCG *p, Slice data) {
    const Byte *b = (const Byte *)data.p;
    if (data.len != 20 || memcmp(b, "pcg:", 4) != 0)
        return pcg_err_invalid;
    p->hi = rand_be64(b + 4);
    p->lo = rand_be64(b + 12);
    return BURROW_NO_ERROR;
}

static uint64_t pcg_vt_uint64(void *self) {
    return pcg_next((Mathrand2PCG *)self);
}

static const Type pcg_desc = {
    {(const Byte *)"PCG", 3},
    {(const Byte *)"rand", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(Mathrand2PCG),
    (uint16_t)_Alignof(Mathrand2PCG),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x70636738U, /* "pcg8" */
    NULL,
};

static const Mathrand2SourceVT pcg_source_vt = {&pcg_desc, pcg_vt_uint64};

Mathrand2Source mathrand2_pcg_as_source(Mathrand2PCG *p) {
    Mathrand2Source s = {&pcg_source_vt, p};
    return s;
}

/* --------------------------------------------------------------- ChaCha8 */

enum {
    CHACHA_CTR_INC = 4, /* the counter goes up by four blocks per refill */
    CHACHA_CTR_MAX = 16,
    CHACHA_CHUNK = 32, /* a refill is 32 words */
    CHACHA_RESEED = 4, /* and the last four of every 16th block are the new key */
};

/* Four ChaCha8 blocks at counters counter to counter+3, interleaved the way
 * Go's generic block interleaves them: word w of block k is at w*4+k, and
 * each output uint64 is two neighbouring words, low word first. That order is
 * what makes the output match the assembly versions, and it is the order Go
 * documents in its C2SP specification. Only the key words get the input added
 * back, which is Go's variation on ChaCha and is fine for a generator whose
 * output is never used as a keystream against known plaintext. */
#if defined(__GNUC__) || defined(__clang__)

/* With vector types the four blocks run side by side, one lane each, which is
 * what Go's assembly does and what the interleaved order is made for. Word w of
 * every block sits in v[w], so storing v[0] to v[15] in turn is already the
 * output order. */
typedef uint32_t ChachaV4 __attribute__((vector_size(16)));

#define CHACHA_ROTV(x, k) (((x) << (k)) | ((x) >> (32 - (k))))

#define CHACHA_QRV(a, b, c, d)                                                         \
    do {                                                                               \
        a += b;                                                                        \
        d ^= a;                                                                        \
        d = CHACHA_ROTV(d, 16);                                                        \
        c += d;                                                                        \
        b ^= c;                                                                        \
        b = CHACHA_ROTV(b, 12);                                                        \
        a += b;                                                                        \
        d ^= a;                                                                        \
        d = CHACHA_ROTV(d, 8);                                                         \
        c += d;                                                                        \
        b ^= c;                                                                        \
        b = CHACHA_ROTV(b, 7);                                                         \
    } while (0)

static void chacha_block(const uint64_t seed[4], uint64_t buf[32], uint32_t counter) {
    ChachaV4 k[8];
    for (int m = 0; m < 4; m++) {
        uint32_t lo = (uint32_t)seed[m];
        uint32_t hi = (uint32_t)(seed[m] >> 32);
        k[2 * m] = (ChachaV4){lo, lo, lo, lo};
        k[2 * m + 1] = (ChachaV4){hi, hi, hi, hi};
    }
    ChachaV4 x0 = {0x61707865U, 0x61707865U, 0x61707865U, 0x61707865U};
    ChachaV4 x1 = {0x3320646eU, 0x3320646eU, 0x3320646eU, 0x3320646eU};
    ChachaV4 x2 = {0x79622d32U, 0x79622d32U, 0x79622d32U, 0x79622d32U};
    ChachaV4 x3 = {0x6b206574U, 0x6b206574U, 0x6b206574U, 0x6b206574U};
    ChachaV4 x4 = k[0], x5 = k[1], x6 = k[2], x7 = k[3];
    ChachaV4 x8 = k[4], x9 = k[5], x10 = k[6], x11 = k[7];
    ChachaV4 x12 = {counter, counter + 1, counter + 2, counter + 3};
    ChachaV4 x13 = {0, 0, 0, 0}, x14 = {0, 0, 0, 0}, x15 = {0, 0, 0, 0};
    for (int round = 0; round < 4; round++) {
        CHACHA_QRV(x0, x4, x8, x12);
        CHACHA_QRV(x1, x5, x9, x13);
        CHACHA_QRV(x2, x6, x10, x14);
        CHACHA_QRV(x3, x7, x11, x15);
        CHACHA_QRV(x0, x5, x10, x15);
        CHACHA_QRV(x1, x6, x11, x12);
        CHACHA_QRV(x2, x7, x8, x13);
        CHACHA_QRV(x3, x4, x9, x14);
    }
    ChachaV4 v[16] = {x0,        x1,        x2,         x3,
                      x4 + k[0], x5 + k[1], x6 + k[2],  x7 + k[3],
                      x8 + k[4], x9 + k[5], x10 + k[6], x11 + k[7],
                      x12,       x13,       x14,        x15};
#if BURROW_LITTLE_ENDIAN
    memcpy(buf, v, sizeof v);
#else
    uint32_t w[64];
    memcpy(w, v, sizeof w);
    for (int i = 0; i < 32; i++)
        buf[i] = (uint64_t)w[2 * i] | (uint64_t)w[2 * i + 1] << 32;
#endif
}

#else

static inline uint32_t chacha_rotl(uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

#define CHACHA_QR(a, b, c, d)                                                          \
    do {                                                                               \
        a += b;                                                                        \
        d ^= a;                                                                        \
        d = chacha_rotl(d, 16);                                                        \
        c += d;                                                                        \
        b ^= c;                                                                        \
        b = chacha_rotl(b, 12);                                                        \
        a += b;                                                                        \
        d ^= a;                                                                        \
        d = chacha_rotl(d, 8);                                                         \
        c += d;                                                                        \
        b ^= c;                                                                        \
        b = chacha_rotl(b, 7);                                                         \
    } while (0)

static void chacha_block(const uint64_t seed[4], uint64_t buf[32], uint32_t counter) {
    uint32_t b[16][4];
    for (int k = 0; k < 4; k++) {
        b[0][k] = 0x61707865U;
        b[1][k] = 0x3320646eU;
        b[2][k] = 0x79622d32U;
        b[3][k] = 0x6b206574U;
        for (int m = 0; m < 4; m++) {
            b[4 + 2 * m][k] = (uint32_t)seed[m];
            b[5 + 2 * m][k] = (uint32_t)(seed[m] >> 32);
        }
        b[12][k] = counter + (uint32_t)k;
        b[13][k] = 0;
        b[14][k] = 0;
        b[15][k] = 0;
    }
    for (int k = 0; k < 4; k++) {
        uint32_t x0 = b[0][k], x1 = b[1][k], x2 = b[2][k], x3 = b[3][k];
        uint32_t x4 = b[4][k], x5 = b[5][k], x6 = b[6][k], x7 = b[7][k];
        uint32_t x8 = b[8][k], x9 = b[9][k], x10 = b[10][k], x11 = b[11][k];
        uint32_t x12 = b[12][k], x13 = b[13][k], x14 = b[14][k], x15 = b[15][k];
        for (int round = 0; round < 4; round++) {
            CHACHA_QR(x0, x4, x8, x12);
            CHACHA_QR(x1, x5, x9, x13);
            CHACHA_QR(x2, x6, x10, x14);
            CHACHA_QR(x3, x7, x11, x15);
            CHACHA_QR(x0, x5, x10, x15);
            CHACHA_QR(x1, x6, x11, x12);
            CHACHA_QR(x2, x7, x8, x13);
            CHACHA_QR(x3, x4, x9, x14);
        }
        b[0][k] = x0;
        b[1][k] = x1;
        b[2][k] = x2;
        b[3][k] = x3;
        b[4][k] += x4;
        b[5][k] += x5;
        b[6][k] += x6;
        b[7][k] += x7;
        b[8][k] += x8;
        b[9][k] += x9;
        b[10][k] += x10;
        b[11][k] += x11;
        b[12][k] = x12;
        b[13][k] = x13;
        b[14][k] = x14;
        b[15][k] = x15;
    }
    const uint32_t *w = &b[0][0];
    for (int i = 0; i < 32; i++)
        buf[i] = (uint64_t)w[2 * i] | (uint64_t)w[2 * i + 1] << 32;
}

#endif

static void chacha_init64(Mathrand2ChaCha8 *c, const uint64_t seed[4]) {
    memcpy(c->seed, seed, sizeof c->seed);
    chacha_block(c->seed, c->buf, 0);
    c->c = 0;
    c->i = 0;
    c->n = CHACHA_CHUNK;
}

static void chacha_refill(Mathrand2ChaCha8 *c) {
    c->c += CHACHA_CTR_INC;
    if (c->c == CHACHA_CTR_MAX) {
        /* Key erasure: the last four words, which were never handed out, are
         * the next key. */
        for (int k = 0; k < 4; k++)
            c->seed[k] = c->buf[32 - CHACHA_RESEED + k];
        c->c = 0;
    }
    chacha_block(c->seed, c->buf, c->c);
    c->i = 0;
    c->n = 32;
    if (c->c == CHACHA_CTR_MAX - CHACHA_CTR_INC)
        c->n = 32 - CHACHA_RESEED;
}

void mathrand2_cha_cha8_seed(Mathrand2ChaCha8 *c, const Byte seed[32]) {
    uint64_t s[4];
    for (int k = 0; k < 4; k++)
        s[k] = rand_le64(seed + 8 * k);
    chacha_init64(c, s);
    c->read_len = 0;
    memset(c->read_buf, 0, sizeof c->read_buf);
}

Mathrand2ChaCha8 *mathrand2_new_cha_cha8(Alloc *a, const Byte seed[32]) {
    Mathrand2ChaCha8 *c = BURROW_NEW(a, Mathrand2ChaCha8);
    if (c != NULL)
        mathrand2_cha_cha8_seed(c, seed);
    return c;
}

static inline uint64_t chacha_next(Mathrand2ChaCha8 *c) {
    for (;;) {
        uint32_t i = c->i;
        if (i < c->n) {
            c->i = i + 1;
            return c->buf[i & 31];
        }
        chacha_refill(c);
    }
}

uint64_t mathrand2_cha_cha8_uint64(Mathrand2ChaCha8 *c) {
    return chacha_next(c);
}

Int mathrand2_cha_cha8_read(Mathrand2ChaCha8 *c, Slice p, Error *err) {
    Byte *q = (Byte *)p.p;
    Int left = p.len;
    Int n = 0;
    if (c->read_len > 0) {
        Int k = c->read_len < left ? c->read_len : left;
        memcpy(q, c->read_buf + 8 - c->read_len, (size_t)k);
        c->read_len -= k;
        n = k;
        q += k;
        left -= k;
    }
    while (left >= 8) {
        rand_put_le64(q, chacha_next(c));
        q += 8;
        left -= 8;
        n += 8;
    }
    if (left > 0) {
        rand_put_le64(c->read_buf, chacha_next(c));
        memcpy(q, c->read_buf, (size_t)left);
        n += left;
        c->read_len = 8 - left;
    }
    rand_no_error(err);
    return n;
}

static const Str chacha_invalid_text = {(const Byte *)"invalid ChaCha8 encoding", 24};
static const Error chacha_err_invalid = {&burrow_sentinel_error_vt,
                                         &chacha_invalid_text};
static const Str chacha_invalid_readbuf_text = {
    (const Byte *)"invalid ChaCha8 Read buffer encoding", 36};
static const Error chacha_err_invalid_readbuf = {&burrow_sentinel_error_vt,
                                                 &chacha_invalid_readbuf_text};

Slice mathrand2_cha_cha8_append_binary(Mathrand2ChaCha8 *c, Alloc *a, Slice b,
                                       Error *err) {
    bool ok = true;
    if (c->read_len > 0) {
        Byte head[9];
        memcpy(head, "readbuf:", 8);
        head[8] = (Byte)c->read_len;
        b = rand_append(a, b, head, 9, &ok);
        b = rand_append(a, b, c->read_buf + 8 - c->read_len, c->read_len, &ok);
    }
    Byte buf[48];
    memcpy(buf, "chacha8:", 8);
    uint64_t used = (uint64_t)(c->c / CHACHA_CTR_INC) * CHACHA_CHUNK + c->i;
    rand_put_be64(buf + 8, used);
    for (int k = 0; k < 4; k++)
        rand_put_le64(buf + 16 + 8 * k, c->seed[k]);
    b = rand_append(a, b, buf, 48, &ok);
    BURROW_OUT(err, ok ? BURROW_NO_ERROR : burrow_err_out_of_memory);
    return b;
}

Slice mathrand2_cha_cha8_marshal_binary(Mathrand2ChaCha8 *c, Alloc *a, Error *err) {
    return mathrand2_cha_cha8_append_binary(c, a, slice_make(a, TYPE_BYTE, 0, 64), err);
}

Error mathrand2_cha_cha8_unmarshal_binary(Mathrand2ChaCha8 *c, Slice data) {
    const Byte *b = (const Byte *)data.p;
    Int n = data.len;
    if (n >= 8 && memcmp(b, "readbuf:", 8) == 0) {
        b += 8;
        n -= 8;
        /* Go adds the one in a byte, so a length of 255 wraps to zero, gets
         * past the check and then fails the slice expression. */
        if (n == 0 || n < (Int)(Byte)(1 + b[0]))
            return chacha_err_invalid_readbuf;
        Int k = b[0];
        if (k == 255)
            panic_str(BURROW_S("runtime error: slice bounds out of range [1:0]"));
        if (k > 8) {
            /* Go copies into readBuf[8-k:], which is out of range, and the
             * slice expression panics before anything is copied. */
            char msg[64];
            snprintf(msg, sizeof msg, "runtime error: slice bounds out of range [%d:]",
                     (int)(8 - k));
            panic_str(str_from_cstr(msg));
        }
        memcpy(c->read_buf + 8 - k, b + 1, (size_t)k);
        c->read_len = k;
        b += 1 + k;
        n -= 1 + k;
    }
    if (n != 48 || memcmp(b, "chacha8:", 8) != 0)
        return chacha_err_invalid;
    uint64_t used = rand_be64(b + 8);
    if (used > (CHACHA_CTR_MAX / CHACHA_CTR_INC) * CHACHA_CHUNK - CHACHA_RESEED)
        return chacha_err_invalid;
    for (int k = 0; k < 4; k++)
        c->seed[k] = rand_le64(b + 16 + 8 * k);
    c->c = CHACHA_CTR_INC * ((uint32_t)used / CHACHA_CHUNK);
    chacha_block(c->seed, c->buf, c->c);
    c->i = (uint32_t)used % CHACHA_CHUNK;
    c->n = CHACHA_CHUNK;
    if (c->c == CHACHA_CTR_MAX - CHACHA_CTR_INC)
        c->n = CHACHA_CHUNK - CHACHA_RESEED;
    return BURROW_NO_ERROR;
}

static uint64_t chacha_vt_uint64(void *self) {
    return chacha_next((Mathrand2ChaCha8 *)self);
}

static Int chacha_vt_read(void *self, Slice p, Error *err) {
    return mathrand2_cha_cha8_read((Mathrand2ChaCha8 *)self, p, err);
}

static const Type chacha_desc = {
    {(const Byte *)"ChaCha8", 7},
    {(const Byte *)"rand", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(Mathrand2ChaCha8),
    (uint16_t)_Alignof(Mathrand2ChaCha8),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x63686338U, /* "chc8" */
    NULL,
};

static const Mathrand2SourceVT chacha_source_vt = {&chacha_desc, chacha_vt_uint64};
static const IoReaderVT chacha_reader_vt = {&chacha_desc, chacha_vt_read};

Mathrand2Source mathrand2_cha_cha8_as_source(Mathrand2ChaCha8 *c) {
    Mathrand2Source s = {&chacha_source_vt, c};
    return s;
}

IoReader mathrand2_cha_cha8_as_io_reader(Mathrand2ChaCha8 *c) {
    IoReader r = {&chacha_reader_vt, c};
    return r;
}

/* ------------------------------------------------------------------ Rand */

static uint64_t runtime_vt_uint64(void *self) {
    (void)self;
    return runtime_rand64();
}

static const Mathrand2SourceVT runtime2_source_vt = {NULL, runtime_vt_uint64};

/* The shared generator. It is const because nothing ever writes to it: the
 * source has no state of its own, since each thread has its own in the
 * runtime. */
static const Mathrand2Rand mathrand2_global_rand = {{&runtime2_source_vt, NULL}};

Mathrand2Rand *burrow__mathrand2_global(void) {
    return (Mathrand2Rand *)(uintptr_t)&mathrand2_global_rand;
}

#define R2_GLOBAL burrow__mathrand2_global()

/* The next 64 bits. The sources in this file are called directly rather than
 * through the vtable, which is a compare the branch predictor gets right every
 * time and saves an indirect call per draw. */
static inline uint64_t r2_next(Mathrand2Rand *r) {
    const Mathrand2SourceVT *vt = r->src.vt;
    if (vt == &runtime2_source_vt)
        return runtime_rand64();
    if (vt == &pcg_source_vt)
        return pcg_next((Mathrand2PCG *)r->src.data);
    if (vt == &chacha_source_vt)
        return chacha_next((Mathrand2ChaCha8 *)r->src.data);
    return vt->uint64(r->src.data);
}

Mathrand2Rand *mathrand2_new(Alloc *a, Mathrand2Source src) {
    Mathrand2Rand *r = BURROW_NEW(a, Mathrand2Rand);
    if (r != NULL)
        r->src = src;
    return r;
}

int64_t mathrand2_rand_int64(Mathrand2Rand *r) {
    return (int64_t)(r2_next(r) & RNG_MASK);
}

uint32_t mathrand2_rand_uint32(Mathrand2Rand *r) {
    return (uint32_t)(r2_next(r) >> 32);
}

uint64_t mathrand2_rand_uint64(Mathrand2Rand *r) {
    return r2_next(r);
}

int32_t mathrand2_rand_int32(Mathrand2Rand *r) {
    return (int32_t)(r2_next(r) >> 33);
}

Int mathrand2_rand_int(Mathrand2Rand *r) {
    return (Int)((Uint)r2_next(r) << 1 >> 1);
}

Uint mathrand2_rand_uint(Mathrand2Rand *r) {
    return (Uint)r2_next(r);
}

/* A value in [0, n) for n > 0, by Lemire's multiply and reject. Go has a
 * second version for 32 bit machines that splits the multiply in two, and it
 * gives the same answers from the same draws, so this is the only one. */
static inline uint64_t r2_uint64n(Mathrand2Rand *r, uint64_t n) {
    if ((n & (n - 1)) == 0)
        return r2_next(r) & (n - 1);
    uint64_t lo;
    uint64_t hi = bits_mul64(r2_next(r), n, &lo);
    if (lo < n) {
        uint64_t thresh = (0 - n) % n;
        while (lo < thresh)
            hi = bits_mul64(r2_next(r), n, &lo);
    }
    return hi;
}

int64_t mathrand2_rand_int64_n(Mathrand2Rand *r, int64_t n) {
    if (n <= 0)
        panic_str(BURROW_S("invalid argument to Int64N"));
    return (int64_t)r2_uint64n(r, (uint64_t)n);
}

uint64_t mathrand2_rand_uint64_n(Mathrand2Rand *r, uint64_t n) {
    if (n == 0)
        panic_str(BURROW_S("invalid argument to Uint64N"));
    return r2_uint64n(r, n);
}

int32_t mathrand2_rand_int32_n(Mathrand2Rand *r, int32_t n) {
    if (n <= 0)
        panic_str(BURROW_S("invalid argument to Int32N"));
    return (int32_t)r2_uint64n(r, (uint64_t)n);
}

uint32_t mathrand2_rand_uint32_n(Mathrand2Rand *r, uint32_t n) {
    if (n == 0)
        panic_str(BURROW_S("invalid argument to Uint32N"));
    return (uint32_t)r2_uint64n(r, (uint64_t)n);
}

Int mathrand2_rand_int_n(Mathrand2Rand *r, Int n) {
    if (n <= 0)
        panic_str(BURROW_S("invalid argument to IntN"));
    return (Int)r2_uint64n(r, (uint64_t)n);
}

Uint mathrand2_rand_uint_n(Mathrand2Rand *r, Uint n) {
    if (n == 0)
        panic_str(BURROW_S("invalid argument to UintN"));
    return (Uint)r2_uint64n(r, (uint64_t)n);
}

int64_t burrow__mathrand2_rand_n_signed(Mathrand2Rand *r, int64_t n) {
    if (n <= 0)
        panic_str(BURROW_S("invalid argument to N"));
    return (int64_t)r2_uint64n(r, (uint64_t)n);
}

uint64_t burrow__mathrand2_rand_n_unsigned(Mathrand2Rand *r, uint64_t n) {
    if (n == 0)
        panic_str(BURROW_S("invalid argument to N"));
    return r2_uint64n(r, n);
}

static inline double r2_float64(Mathrand2Rand *r) {
    return (double)(r2_next(r) << 11 >> 11) / 9007199254740992.0;
}

double mathrand2_rand_float64(Mathrand2Rand *r) {
    return r2_float64(r);
}

float mathrand2_rand_float32(Mathrand2Rand *r) {
    uint32_t x = (uint32_t)(r2_next(r) >> 32);
    return (float)(x << 8 >> 8) / 16777216.0f;
}

void mathrand2_rand_shuffle(Mathrand2Rand *r, Int n, SwapFunc swap) {
    if (n < 0)
        panic_str(BURROW_S("invalid argument to Shuffle"));
    for (Int i = n - 1; i > 0; i--) {
        Int j = (Int)r2_uint64n(r, (uint64_t)(i + 1));
        swap.f(swap.env, i, j);
    }
}

Slice mathrand2_rand_perm(Mathrand2Rand *r, Alloc *a, Int n) {
    Slice s = slice_make(a, TYPE_INT, n, n);
    Int *p = (Int *)s.p;
    if (p == NULL)
        return s;
    for (Int i = 0; i < n; i++)
        p[i] = i;
    for (Int i = n - 1; i > 0; i--) {
        Int j = (Int)r2_uint64n(r, (uint64_t)(i + 1));
        Int t = p[i];
        p[i] = p[j];
        p[j] = t;
    }
    return s;
}

/* The float32 side of the ziggurat's slow path, spelled with a temporary for
 * each step so that every one of them rounds to float as Go's does, whatever
 * the compiler's FLT_EVAL_METHOD. */
static bool rand_zig_below(float f_i, float f_prev, double u, double y) {
    float fu = (float)u;
    float d = f_prev - f_i;
    float m = fu * d;
    float s = f_i + m;
    return s < (float)y;
}

double mathrand2_rand_norm_float64(Mathrand2Rand *r) {
    for (;;) {
        uint64_t u = r2_next(r);
        int32_t j = (int32_t)(uint32_t)u;
        uint32_t i = (uint32_t)(u >> 32) & 0x7F;
        double x = (double)j * (double)rand_wn[i];
        if (rand_abs_int32(j) < rand_kn[i])
            return x;
        if (i == 0) {
            /* The tail, by Marsaglia's method. */
            for (;;) {
                x = -math_log(r2_float64(r)) * (1.0 / RAND_RN);
                double y = -math_log(r2_float64(r));
                if (y + y >= x * x)
                    break;
            }
            return j > 0 ? RAND_RN + x : -RAND_RN - x;
        }
        if (rand_zig_below(rand_fn[i], rand_fn[i - 1], r2_float64(r),
                           math_exp(-.5 * x * x)))
            return x;
    }
}

double mathrand2_rand_exp_float64(Mathrand2Rand *r) {
    for (;;) {
        uint64_t u = r2_next(r);
        uint32_t j = (uint32_t)u;
        uint32_t i = (uint32_t)(u >> 32) & 0xFF;
        double x = (double)j * (double)rand_we[i];
        if (j < rand_ke[i])
            return x;
        if (i == 0)
            return RAND_RE - math_log(r2_float64(r));
        if (rand_zig_below(rand_fe[i], rand_fe[i - 1], r2_float64(r), math_exp(-x)))
            return x;
    }
}

/* ------------------------------------------------------------------ Zipf */

/* The rejection inversion method of Hörmann and Derflinger, shared by both
 * versions, which differ only in whose Float64 it calls. */
typedef struct RandZipf {
    void *r;
    double imax;
    double v;
    double q;
    double s;
    double one_minus_q;
    double one_minus_q_inv;
    double hxm;
    double hx0_minus_hxm;
} RandZipf;

static double zipf_h(const RandZipf *z, double x) {
    return math_exp(z->one_minus_q * math_log(z->v + x)) * z->one_minus_q_inv;
}

static double zipf_hinv(const RandZipf *z, double x) {
    return math_exp(z->one_minus_q_inv * math_log(z->one_minus_q * x)) - z->v;
}

static bool zipf_init(RandZipf *z, void *r, double s, double v, uint64_t imax) {
    if (s <= 1.0 || v < 1)
        return false;
    z->r = r;
    z->imax = (double)imax;
    z->v = v;
    z->q = s;
    z->one_minus_q = 1.0 - z->q;
    z->one_minus_q_inv = 1.0 / z->one_minus_q;
    z->hxm = zipf_h(z, z->imax + 0.5);
    z->hx0_minus_hxm = zipf_h(z, 0.5) - math_exp(math_log(z->v) * (-z->q)) - z->hxm;
    z->s = 1 - zipf_hinv(z, zipf_h(z, 1.5) - math_exp(-z->q * math_log(z->v + 1.0)));
    return true;
}

static uint64_t zipf_draw(const RandZipf *z, double (*float64)(void *r)) {
    double k = 0.0;
    for (;;) {
        double r = float64(z->r);
        double ur = z->hxm + r * z->hx0_minus_hxm;
        double x = zipf_hinv(z, ur);
        k = math_floor(x + 0.5);
        if (k - x <= z->s)
            break;
        if (ur >= zipf_h(z, k + 0.5) - math_exp(-math_log(k + z->v) * z->q))
            break;
    }
    return (uint64_t)k;
}

/* The two public Zipf structs have the same members as RandZipf with a typed
 * Rand pointer first, so the shared code can work on either. */
_Static_assert(sizeof(Mathrand2Zipf) == sizeof(RandZipf), "Zipf layout");
_Static_assert(sizeof(MathRandZipf) == sizeof(RandZipf), "Zipf layout");

static double r2_float64_any(void *r) {
    return r2_float64((Mathrand2Rand *)r);
}

Mathrand2Zipf *mathrand2_new_zipf(Alloc *a, Mathrand2Rand *r, double s, double v,
                                  uint64_t imax) {
    RandZipf z;
    if (!zipf_init(&z, r, s, v, imax))
        return NULL;
    Mathrand2Zipf *out = BURROW_NEW(a, Mathrand2Zipf);
    if (out == NULL)
        return NULL;
    out->r = r;
    out->imax = z.imax;
    out->v = z.v;
    out->q = z.q;
    out->s = z.s;
    out->one_minus_q = z.one_minus_q;
    out->one_minus_q_inv = z.one_minus_q_inv;
    out->hxm = z.hxm;
    out->hx0_minus_hxm = z.hx0_minus_hxm;
    return out;
}

static RandZipf zipf_from2(const Mathrand2Zipf *in) {
    RandZipf z = {in->r,
                  in->imax,
                  in->v,
                  in->q,
                  in->s,
                  in->one_minus_q,
                  in->one_minus_q_inv,
                  in->hxm,
                  in->hx0_minus_hxm};
    return z;
}

uint64_t mathrand2_zipf_uint64(Mathrand2Zipf *z) {
    if (z == NULL)
        panic_str(BURROW_S("rand: nil Zipf"));
    RandZipf in = zipf_from2(z);
    return zipf_draw(&in, r2_float64_any);
}

/* ------------------------------------------------- v2 top level functions */

int64_t mathrand2_int64(void) {
    return (int64_t)(runtime_rand64() & RNG_MASK);
}

uint32_t mathrand2_uint32(void) {
    return (uint32_t)(runtime_rand64() >> 32);
}

uint64_t mathrand2_uint64(void) {
    return runtime_rand64();
}

int32_t mathrand2_int32(void) {
    return (int32_t)(runtime_rand64() >> 33);
}

Int mathrand2_int(void) {
    return (Int)((Uint)runtime_rand64() << 1 >> 1);
}

Uint mathrand2_uint(void) {
    return (Uint)runtime_rand64();
}

int64_t mathrand2_int64_n(int64_t n) {
    return mathrand2_rand_int64_n(R2_GLOBAL, n);
}

int32_t mathrand2_int32_n(int32_t n) {
    return mathrand2_rand_int32_n(R2_GLOBAL, n);
}

Int mathrand2_int_n(Int n) {
    return mathrand2_rand_int_n(R2_GLOBAL, n);
}

uint64_t mathrand2_uint64_n(uint64_t n) {
    return mathrand2_rand_uint64_n(R2_GLOBAL, n);
}

uint32_t mathrand2_uint32_n(uint32_t n) {
    return mathrand2_rand_uint32_n(R2_GLOBAL, n);
}

Uint mathrand2_uint_n(Uint n) {
    return mathrand2_rand_uint_n(R2_GLOBAL, n);
}

double mathrand2_float64(void) {
    return mathrand2_rand_float64(R2_GLOBAL);
}

float mathrand2_float32(void) {
    return mathrand2_rand_float32(R2_GLOBAL);
}

double mathrand2_norm_float64(void) {
    return mathrand2_rand_norm_float64(R2_GLOBAL);
}

double mathrand2_exp_float64(void) {
    return mathrand2_rand_exp_float64(R2_GLOBAL);
}

Slice mathrand2_perm(Alloc *a, Int n) {
    return mathrand2_rand_perm(R2_GLOBAL, a, n);
}

void mathrand2_shuffle(Int n, SwapFunc swap) {
    mathrand2_rand_shuffle(R2_GLOBAL, n, swap);
}

/* =============================================================== math/rand */

/* ------------------------------------------------------------- rngSource */

/* Go's additive lagged Fibonacci generator: x[n] = x[n-273] + x[n-607], over
 * a state seeded through Park and Miller's minimal standard generator and
 * mixed with the cooked table. The words are uint64_t here so the addition
 * can wrap, where Go's are int64 and wrap anyway. */
typedef struct RngSource {
    int tap;
    int feed;
    uint64_t vec[RNG_LEN];
} RngSource;

static int32_t rng_seedrand(int32_t x) {
    const int32_t A = 48271;
    const int32_t Q = 44488;
    const int32_t R = 3399;
    int32_t hi = x / Q;
    int32_t lo = x % Q;
    x = A * lo - R * hi;
    if (x < 0)
        x += INT32_MAX_;
    return x;
}

static void rng_seed(RngSource *rng, int64_t seed) {
    rng->tap = 0;
    rng->feed = RNG_LEN - RNG_TAP;
    seed = seed % INT32_MAX_;
    if (seed < 0)
        seed += INT32_MAX_;
    if (seed == 0)
        seed = 89482311;
    int32_t x = (int32_t)seed;
    for (int i = -20; i < RNG_LEN; i++) {
        x = rng_seedrand(x);
        if (i >= 0) {
            uint64_t u = (uint64_t)(int64_t)x << 40;
            x = rng_seedrand(x);
            u ^= (uint64_t)(int64_t)x << 20;
            x = rng_seedrand(x);
            u ^= (uint64_t)(int64_t)x;
            u ^= rand_rng_cooked[i];
            rng->vec[i] = u;
        }
    }
}

static inline uint64_t rng_uint64(RngSource *rng) {
    if (--rng->tap < 0)
        rng->tap += RNG_LEN;
    if (--rng->feed < 0)
        rng->feed += RNG_LEN;
    uint64_t x = rng->vec[rng->feed] + rng->vec[rng->tap];
    rng->vec[rng->feed] = x;
    return x;
}

static int64_t rng_vt_int63(void *self) {
    return (int64_t)(rng_uint64((RngSource *)self) & RNG_MASK);
}

static void rng_vt_seed(void *self, int64_t seed) {
    rng_seed((RngSource *)self, seed);
}

static uint64_t rng_vt_uint64(void *self) {
    return rng_uint64((RngSource *)self);
}

static const Type rng_desc = {
    {(const Byte *)"rngSource", 9},
    {(const Byte *)"rand", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(RngSource),
    (uint16_t)_Alignof(RngSource),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x726e6773U, /* "rngs" */
    NULL,
};

static const MathRandSource64VT rng_source64_vt = {
    {&rng_desc, rng_vt_int63, rng_vt_seed}, rng_vt_uint64};

MathRandSource math_rand_new_source(Alloc *a, int64_t seed) {
    RngSource *rng = BURROW_NEW(a, RngSource);
    if (rng != NULL)
        rng_seed(rng, seed);
    MathRandSource s = {&rng_source64_vt.source, rng};
    return s;
}

/* ---------------------------------------------------------- lockedSource */

/* The shared generator once math_rand_seed has been called with
 * GODEBUG=randseednop=0, or from the start with randautoseed=0: the same
 * rngSource behind a lock. */
typedef struct LockedSource {
    burrow__Lock lk;
    bool seeded;
    RngSource s;
} LockedSource;

static int64_t locked_vt_int63(void *self) {
    LockedSource *r = (LockedSource *)self;
    burrow__lock(&r->lk);
    int64_t n = (int64_t)(rng_uint64(&r->s) & RNG_MASK);
    burrow__unlock(&r->lk);
    return n;
}

static uint64_t locked_vt_uint64(void *self) {
    LockedSource *r = (LockedSource *)self;
    burrow__lock(&r->lk);
    uint64_t n = rng_uint64(&r->s);
    burrow__unlock(&r->lk);
    return n;
}

static void locked_vt_seed(void *self, int64_t seed) {
    LockedSource *r = (LockedSource *)self;
    burrow__lock(&r->lk);
    rng_seed(&r->s, seed);
    r->seeded = true;
    burrow__unlock(&r->lk);
}

static const Type locked_desc = {
    {(const Byte *)"lockedSource", 12},
    {(const Byte *)"rand", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(LockedSource),
    (uint16_t)_Alignof(LockedSource),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6c6b7372U, /* "lksr" */
    NULL,
};

static const MathRandSource64VT locked_source64_vt = {
    {&locked_desc, locked_vt_int63, locked_vt_seed}, locked_vt_uint64};

/* --------------------------------------------------------- runtimeSource */

static int64_t runtime_vt_int63(void *self) {
    (void)self;
    return (int64_t)(runtime_rand64() & RNG_MASK);
}

static void runtime_vt_seed(void *self, int64_t seed) {
    (void)self;
    (void)seed;
    panic_str(BURROW_S("internal error: call to runtimeSource.Seed"));
}

static const Type runtime_desc = {
    {(const Byte *)"runtimeSource", 13},
    {(const Byte *)"rand", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(burrow__Lock),
    (uint16_t)_Alignof(burrow__Lock),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72747372U, /* "rtsr" */
    NULL,
};

static const MathRandSource64VT runtime_source64_vt = {
    {&runtime_desc, runtime_vt_int63, runtime_vt_seed}, runtime_vt_uint64};

/* ------------------------------------------------------------------ Rand */

static const Str rand_uint64_name = {(const Byte *)"Uint64", 6};

/* A Uint64 method in the source's method set, the way a type that is not one
 * of ours says it is a Source64. */
static const Method *rand_uint64_method(const Type *t) {
    if (t == NULL)
        return NULL;
    const Method *m = type_method_by_name(t, rand_uint64_name);
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return NULL;
    const Type *f = m->ftype;
    if (type_num_in(f) != 0 || type_num_out(f) != 1 || type_out(f, 0) != TYPE_UINT64)
        return NULL;
    return m;
}

static void rand_init(MathRandRand *r, MathRandSource src) {
    memset(r, 0, sizeof *r);
    r->src = src;
    const MathRandSourceVT *vt = src.vt;
    if (vt == &rng_source64_vt.source || vt == &locked_source64_vt.source ||
        vt == &runtime_source64_vt.source) {
        /* Ours: the Source64 table is the one the source's table is the
         * first member of. */
        r->s64.vt = (const MathRandSource64VT *)(const void *)vt;
        r->s64.data = src.data;
    } else if (vt != NULL) {
        r->u64 = rand_uint64_method(vt->self_type);
    }
}

MathRandRand *math_rand_new(Alloc *a, MathRandSource src) {
    MathRandRand *r = BURROW_NEW(a, MathRandRand);
    if (r != NULL)
        rand_init(r, src);
    return r;
}

static inline int64_t r1_int63(MathRandRand *r) {
    const MathRandSourceVT *vt = r->src.vt;
    if (vt == &rng_source64_vt.source)
        return (int64_t)(rng_uint64((RngSource *)r->src.data) & RNG_MASK);
    if (vt == &runtime_source64_vt.source)
        return (int64_t)(runtime_rand64() & RNG_MASK);
    return vt->int63(r->src.data);
}

void math_rand_rand_seed(MathRandRand *r, int64_t seed) {
    if (r->src.vt == &locked_source64_vt.source) {
        LockedSource *lk = (LockedSource *)r->src.data;
        burrow__lock(&lk->lk);
        rng_seed(&lk->s, seed);
        lk->seeded = true;
        r->read_pos = 0;
        burrow__unlock(&lk->lk);
        return;
    }
    r->src.vt->seed(r->src.data, seed);
    r->read_pos = 0;
}

int64_t math_rand_rand_int63(MathRandRand *r) {
    return r1_int63(r);
}

uint32_t math_rand_rand_uint32(MathRandRand *r) {
    return (uint32_t)(r1_int63(r) >> 31);
}

uint64_t math_rand_rand_uint64(MathRandRand *r) {
    if (r->s64.vt != NULL) {
        if (r->s64.vt == &rng_source64_vt)
            return rng_uint64((RngSource *)r->s64.data);
        if (r->s64.vt == &runtime_source64_vt)
            return runtime_rand64();
        return r->s64.vt->uint64(r->s64.data);
    }
    if (r->u64 != NULL) {
        uint64_t out = 0;
        void *rets[1] = {&out};
        method_call(r->u64, r->src.data, NULL, rets);
        return out;
    }
    uint64_t lo = (uint64_t)r1_int63(r) >> 31;
    return lo | (uint64_t)r1_int63(r) << 32;
}

int32_t math_rand_rand_int31(MathRandRand *r) {
    return (int32_t)(r1_int63(r) >> 32);
}

Int math_rand_rand_int(MathRandRand *r) {
    Uint u = (Uint)r1_int63(r);
    return (Int)(u << 1 >> 1);
}

int64_t math_rand_rand_int63n(MathRandRand *r, int64_t n) {
    if (n <= 0)
        panic_str(BURROW_S("invalid argument to Int63n"));
    if ((n & (n - 1)) == 0)
        return r1_int63(r) & (n - 1);
    int64_t max = (int64_t)((uint64_t)INT64_MAX - ((uint64_t)1 << 63) % (uint64_t)n);
    int64_t v = r1_int63(r);
    while (v > max)
        v = r1_int63(r);
    return v % n;
}

int32_t math_rand_rand_int31n(MathRandRand *r, int32_t n) {
    if (n <= 0)
        panic_str(BURROW_S("invalid argument to Int31n"));
    if ((n & (n - 1)) == 0)
        return math_rand_rand_int31(r) & (n - 1);
    int32_t max = (int32_t)((uint32_t)INT32_MAX - ((uint32_t)1 << 31) % (uint32_t)n);
    int32_t v = math_rand_rand_int31(r);
    while (v > max)
        v = math_rand_rand_int31(r);
    return v % n;
}

/* Lemire's method, which Go keeps unexported and uses only for Shuffle so as
 * not to change the values Int31n has always given. */
static int32_t r1_int31n_fast(MathRandRand *r, int32_t n) {
    uint32_t v = math_rand_rand_uint32(r);
    uint64_t prod = (uint64_t)v * (uint64_t)n;
    uint32_t low = (uint32_t)prod;
    if (low < (uint32_t)n) {
        uint32_t thresh = (0U - (uint32_t)n) % (uint32_t)n;
        while (low < thresh) {
            v = math_rand_rand_uint32(r);
            prod = (uint64_t)v * (uint64_t)n;
            low = (uint32_t)prod;
        }
    }
    return (int32_t)(prod >> 32);
}

Int math_rand_rand_intn(MathRandRand *r, Int n) {
    if (n <= 0)
        panic_str(BURROW_S("invalid argument to Intn"));
    if (n <= INT32_MAX)
        return (Int)math_rand_rand_int31n(r, (int32_t)n);
    return (Int)math_rand_rand_int63n(r, (int64_t)n);
}

static double r1_float64(MathRandRand *r) {
    for (;;) {
        double f = (double)r1_int63(r) / 9223372036854775808.0;
        if (f != 1)
            return f;
    }
}

static double r1_float64_any(void *r) {
    return r1_float64((MathRandRand *)r);
}

double math_rand_rand_float64(MathRandRand *r) {
    return r1_float64(r);
}

float math_rand_rand_float32(MathRandRand *r) {
    for (;;) {
        float f = (float)r1_float64(r);
        if (f != 1)
            return f;
    }
}

double math_rand_rand_norm_float64(MathRandRand *r) {
    for (;;) {
        int32_t j = (int32_t)math_rand_rand_uint32(r);
        int32_t i = j & 0x7F;
        double x = (double)j * (double)rand_wn[i];
        if (rand_abs_int32(j) < rand_kn[i])
            return x;
        if (i == 0) {
            for (;;) {
                x = -math_log(r1_float64(r)) * (1.0 / RAND_RN);
                double y = -math_log(r1_float64(r));
                if (y + y >= x * x)
                    break;
            }
            return j > 0 ? RAND_RN + x : -RAND_RN - x;
        }
        if (rand_zig_below(rand_fn[i], rand_fn[i - 1], r1_float64(r),
                           math_exp(-.5 * x * x)))
            return x;
    }
}

double math_rand_rand_exp_float64(MathRandRand *r) {
    for (;;) {
        uint32_t j = math_rand_rand_uint32(r);
        uint32_t i = j & 0xFF;
        double x = (double)j * (double)rand_we[i];
        if (j < rand_ke[i])
            return x;
        if (i == 0)
            return RAND_RE - math_log(r1_float64(r));
        if (rand_zig_below(rand_fe[i], rand_fe[i - 1], r1_float64(r), math_exp(-x)))
            return x;
    }
}

Slice math_rand_rand_perm(MathRandRand *r, Alloc *a, Int n) {
    Slice s = slice_make(a, TYPE_INT, n, n);
    Int *m = (Int *)s.p;
    if (m == NULL)
        return s;
    /* Go's inside out shuffle, which draws with Intn and so gives the same
     * permutation for the same seed as it always has. */
    for (Int i = 0; i < n; i++) {
        Int j = math_rand_rand_intn(r, i + 1);
        m[i] = m[j];
        m[j] = i;
    }
    return s;
}

void math_rand_rand_shuffle(MathRandRand *r, Int n, SwapFunc swap) {
    if (n < 0)
        panic_str(BURROW_S("invalid argument to Shuffle"));
    Int i = n - 1;
    for (; i > (Int)INT32_MAX - 1; i--) {
        Int j = (Int)math_rand_rand_int63n(r, (int64_t)i + 1);
        swap.f(swap.env, i, j);
    }
    for (; i > 0; i--) {
        Int j = (Int)r1_int31n_fast(r, (int32_t)(i + 1));
        swap.f(swap.env, i, j);
    }
}

/* Seven bytes from each Int63, low byte first, with what is left over kept in
 * the Rand for the next call. */
static Int r1_read(MathRandRand *r, Slice p, bool locked_rng) {
    Byte *q = (Byte *)p.p;
    int8_t pos = r->read_pos;
    int64_t val = r->read_val;
    for (Int n = 0; n < p.len; n++) {
        if (pos == 0) {
            if (locked_rng)
                val =
                    (int64_t)(rng_uint64(&((LockedSource *)r->src.data)->s) & RNG_MASK);
            else
                val = r1_int63(r);
            pos = 7;
        }
        q[n] = (Byte)val;
        val >>= 8;
        pos--;
    }
    r->read_pos = pos;
    r->read_val = val;
    return p.len;
}

/* The runtime source's lock, which only guards the read position of the one
 * Rand that uses it, since the numbers themselves come from per thread
 * state. */
static burrow__Lock rand_runtime_lock;

Int math_rand_rand_read(MathRandRand *r, Slice p, Error *err) {
    Int n;
    if (r->src.vt == &locked_source64_vt.source) {
        LockedSource *lk = (LockedSource *)r->src.data;
        burrow__lock(&lk->lk);
        n = r1_read(r, p, true);
        burrow__unlock(&lk->lk);
    } else if (r->src.vt == &runtime_source64_vt.source) {
        burrow__lock(&rand_runtime_lock);
        n = r1_read(r, p, false);
        burrow__unlock(&rand_runtime_lock);
    } else {
        n = r1_read(r, p, false);
    }
    rand_no_error(err);
    return n;
}

/* ------------------------------------------------------------------ Zipf */

MathRandZipf *math_rand_new_zipf(Alloc *a, MathRandRand *r, double s, double v,
                                 uint64_t imax) {
    RandZipf z;
    if (!zipf_init(&z, r, s, v, imax))
        return NULL;
    MathRandZipf *out = BURROW_NEW(a, MathRandZipf);
    if (out == NULL)
        return NULL;
    out->r = r;
    out->imax = z.imax;
    out->v = z.v;
    out->q = z.q;
    out->s = z.s;
    out->one_minus_q = z.one_minus_q;
    out->one_minus_q_inv = z.one_minus_q_inv;
    out->hxm = z.hxm;
    out->hx0_minus_hxm = z.hx0_minus_hxm;
    return out;
}

uint64_t math_rand_zipf_uint64(MathRandZipf *z) {
    if (z == NULL)
        panic_str(BURROW_S("rand: nil Zipf"));
    RandZipf in = {
        z->r,   z->imax,         z->v, z->q, z->s, z->one_minus_q, z->one_minus_q_inv,
        z->hxm, z->hx0_minus_hxm};
    return zipf_draw(&in, r1_float64_any);
}

/* ------------------------------------------------------- the shared Rand */

enum {
    RAND_DEBUG_KNOWN = 1 << 0,
    RAND_DEBUG_NO_AUTOSEED = 1 << 1, /* randautoseed=0 */
    RAND_DEBUG_SEED = 1 << 2,        /* randseednop=0 */
};

static uint32_t rand_debug_flags;

/* The value of key in GODEBUG, the last one when it is there twice. */
static bool rand_godebug(const char *env, const char *key, Str *val) {
    size_t kl = strlen(key);
    bool found = false;
    const char *p = env;
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        if ((size_t)(end - p) > kl && memcmp(p, key, kl) == 0 && p[kl] == '=') {
            *val = str_from_bytes(p + kl + 1, (Int)(end - p - (ptrdiff_t)kl - 1));
            found = true;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return found;
}

static bool rand_godebug_zero(const char *env, const char *key) {
    Str s;
    return env != NULL && rand_godebug(env, key, &s) && s.len == 1 && s.p[0] == '0';
}

static uint32_t rand_debug_parse(const char *v) {
    uint32_t f = RAND_DEBUG_KNOWN;
    if (rand_godebug_zero(v, "randautoseed"))
        f |= RAND_DEBUG_NO_AUTOSEED;
    if (rand_godebug_zero(v, "randseednop"))
        f |= RAND_DEBUG_SEED;
    burrow__atomic_store_relaxed_u32(&rand_debug_flags, f);
    return f;
}

static uint32_t rand_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&rand_debug_flags);
    if ((f & RAND_DEBUG_KNOWN) != 0)
        return f;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    return rand_debug_parse(v);
}

/* The two Rands the shared one can be. Go allocates a new locked one each
 * time it switches, which is at most once, so one of each in static storage
 * does the same job. rand_global_lock serializes the switch. */
static MathRandRand rand_runtime_rand;
static MathRandRand rand_locked_rand;
static LockedSource rand_locked_source;
static burrow__Lock rand_global_lock;
static void *rand_global;

/* Sets the locked Rand up, seeded with seed, and makes it the shared one.
 * Called with rand_global_lock held. */
static MathRandRand *rand_switch_locked(int64_t seed) {
    MathRandRand *r = &rand_locked_rand;
    if (burrow__atomic_load_ptr(&rand_global) != r) {
        MathRandSource src = {&locked_source64_vt.source, &rand_locked_source};
        rand_init(r, src);
    }
    math_rand_rand_seed(r, seed);
    burrow__atomic_store_ptr(&rand_global, r);
    return r;
}

static MathRandRand *rand_global_rand(void) {
    MathRandRand *r = (MathRandRand *)burrow__atomic_load_ptr(&rand_global);
    if (r != NULL)
        return r;
    burrow__lock(&rand_global_lock);
    r = (MathRandRand *)burrow__atomic_load_ptr(&rand_global);
    if (r == NULL) {
        if ((rand_debug_load() & RAND_DEBUG_NO_AUTOSEED) != 0) {
            r = rand_switch_locked(1);
        } else {
            r = &rand_runtime_rand;
            MathRandSource src = {&runtime_source64_vt.source, NULL};
            rand_init(r, src);
            burrow__atomic_store_ptr(&rand_global, r);
        }
    }
    burrow__unlock(&rand_global_lock);
    return r;
}

void math_rand_seed(int64_t seed) {
    if ((rand_debug_load() & RAND_DEBUG_SEED) == 0)
        return;
    burrow__lock(&rand_global_lock);
    (void)rand_switch_locked(seed);
    burrow__unlock(&rand_global_lock);
}

void burrow__math_rand_godebug_set(const char *value) {
    if (value == NULL)
        burrow__atomic_store_relaxed_u32(&rand_debug_flags, 0);
    else
        (void)rand_debug_parse(value);
    /* Forget the shared Rand as well, so the next use picks again under the
     * new settings. Only for tests, which do not race with this. */
    burrow__lock(&rand_global_lock);
    burrow__atomic_store_ptr(&rand_global, NULL);
    burrow__unlock(&rand_global_lock);
}

#define R1_GLOBAL rand_global_rand()

int64_t math_rand_int63(void) {
    return math_rand_rand_int63(R1_GLOBAL);
}

int32_t math_rand_int31(void) {
    return math_rand_rand_int31(R1_GLOBAL);
}

Int math_rand_int(void) {
    return math_rand_rand_int(R1_GLOBAL);
}

uint32_t math_rand_uint32(void) {
    return math_rand_rand_uint32(R1_GLOBAL);
}

uint64_t math_rand_uint64(void) {
    return math_rand_rand_uint64(R1_GLOBAL);
}

int64_t math_rand_int63n(int64_t n) {
    return math_rand_rand_int63n(R1_GLOBAL, n);
}

int32_t math_rand_int31n(int32_t n) {
    return math_rand_rand_int31n(R1_GLOBAL, n);
}

Int math_rand_intn(Int n) {
    return math_rand_rand_intn(R1_GLOBAL, n);
}

double math_rand_float64(void) {
    return math_rand_rand_float64(R1_GLOBAL);
}

float math_rand_float32(void) {
    return math_rand_rand_float32(R1_GLOBAL);
}

double math_rand_norm_float64(void) {
    return math_rand_rand_norm_float64(R1_GLOBAL);
}

double math_rand_exp_float64(void) {
    return math_rand_rand_exp_float64(R1_GLOBAL);
}

Slice math_rand_perm(Alloc *a, Int n) {
    return math_rand_rand_perm(R1_GLOBAL, a, n);
}

void math_rand_shuffle(Int n, SwapFunc swap) {
    math_rand_rand_shuffle(R1_GLOBAL, n, swap);
}

Int math_rand_read(Slice p, Error *err) {
    return math_rand_rand_read(R1_GLOBAL, p, err);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
