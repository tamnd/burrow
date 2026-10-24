/* Derived from Go's src/crypto/internal/fips140test/acvp_test.go,
 * acvp_fips140v1.28_test.go and acvp_mldsa_test.go.
 * Go source: go1.27.1.
 *
 * The capabilities are Go's files, copied into tests/fips140test. The
 * variables are defined in tests/fips140test/fips140_acvp_test_embed.c, which
 *
 *     tools/burrow-gen embed tests/fips140_acvp_test.c \
 *         --dir tests/fips140test \
 *         -o tests/fips140test/fips140_acvp_test_embed.c
 *
 * writes and tools/check-gen.sh keeps up to date.
 *
 * The config TestACVP gives check_expected.go is Go's file too, in the same
 * directory.
 *
 * Not in Go: Go's handlers close over the hash or the direction they were
 * made with, and here those are fields of the command, which is passed to its
 * handler. TestACVP needs the go command on $PATH, and the program it gives
 * acvptool as the module wrapper is the one from fipstest_program.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* A module wrapper adapting the Go FIPS module to the protocol used by the
 * BoringSSL project's `acvptool`.
 *
 * The `acvptool` "lowers" the NIST ACVP server JSON test vectors into a simpler
 * stdin/stdout protocol that can be implemented by a module shim. The tool
 * will fork this binary, request the supported configuration, and then provide
 * test cases over stdin, expecting results to be returned on stdout.
 *
 * See "Testing other FIPS modules"[0] from the BoringSSL ACVP.md documentation
 * for a more detailed description of the protocol used between the acvptool
 * and module wrappers.
 *
 * [0]: https://boringssl.googlesource.com/boringssl/+/refs/heads/master/util/fipstools/acvp/ACVP.md#testing-other-fips-modules */

#include "check.h"
#include "fetchmodule.h"
#include "fipstest.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/pbkdf2.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha3.h"
#include "burrow/crypto/sha512.h"
#include "burrow/crypto/subtle.h"
#include "burrow/embed.h"
#include "burrow/encoding/binary.h"
#include "burrow/math/big.h"

#include "../src/crypto/aes_internal.h"
#include "../src/crypto/drbg_internal.h"
#include "../src/crypto/ecdh_internal.h"
#include "../src/crypto/ecdsa_internal.h"
#include "../src/crypto/edwards25519.h"
#include "../src/crypto/entropy_internal.h"
#include "../src/crypto/fips140_module.h"
#include "../src/crypto/mldsa_internal.h"
#include "../src/crypto/mlkem_internal.h"
#include "../src/crypto/rsa_internal.h"
#include "../src/crypto/ssh_internal.h"
#include "../src/crypto/tls_internal.h"

#include <string.h>

BURROW_EMBED_BYTES(capabilities_json, "acvp_capabilities_fips140v1.28.json");

/* Not in Go: the tests run from the top of the tree, so the path starts with
 * the directory the file is in. */
static const Str test_config_file =
    BURROW_S_INIT("tests/fips140test/acvp_test_fips140v1.28.config.json");

/* Separate capabilities specific to testing the entropy source's SHA2-384
 * implementation. This implementation differs from the FIPS module's SHA2-384
 * in its supported input sizes. Set the GOENTROPYSOURCEACVP environment
 * variable to use these capabilities in place of capabilitiesJson */
BURROW_EMBED_BYTES(entropy_capabilities_json, "acvp_capabilities.entropy.json");

static bool no_paapai;

/* Use the capabilities, configuration and commands for the entropy source.
 * This is used to test the separate entropy source in crypto/internal/entropy
 * since the algorithm name alone can't indicate which to test. */
static bool entropy_testing;

/* Not in Go: a [][]byte, which is what requests and responses are made of. */
typedef struct AcvpArgs {
    Slice *v;
    Int len;
} AcvpArgs;

typedef struct AcvpRequest {
    Str name;
    AcvpArgs args;
} AcvpRequest;

typedef enum EcdsaSigType {
    ECDSA_SIG_TYPE_NORMAL,
    ECDSA_SIG_TYPE_DETERMINISTIC,
} EcdsaSigType;

typedef enum AesDirection {
    AES_ENCRYPT,
    AES_DECRYPT,
} AesDirection;

typedef struct AcvpCommand AcvpCommand;

/* Not in Go: c is the command, for what Go's handlers close over, and the
 * response is made from a. */
typedef Error (*AcvpHandler)(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                             AcvpArgs *out);

struct AcvpCommand {
    const char *name;
    /* requiredArgs enforces that an exact number of arguments are provided to
     * the handler. */
    Int required_args;
    AcvpHandler handler;
    /* Not in Go: what the handler closes over. */
    HashNewFunc h;
    Sha3SHAKE *(*shake)(Alloc *a);
    Sha3SHAKE *(*cshake)(Alloc *a, Slice n, Slice s);
    const SshDirection *direction;
    EcdsaSigType sig_type;
    AesDirection aes_direction;
    bool rand_nonce;
    const EcdhCurve *(*ecdh_curve)(void);
    const MldsaParameters *(*mldsa)(void);
    const char *hash_name;
    bool pss;
};

/* ------------------------------------------------------------- helpers */

/* Not in Go: the hashes sha3.New224 and the rest make, as HashNewFuncs. */
static Hash new_sha3_224(Alloc *a) {
    return sha3_as_hash(sha3_new224(a));
}

static Hash new_sha3_256(Alloc *a) {
    return sha3_as_hash(sha3_new256(a));
}

static Hash new_sha3_384(Alloc *a) {
    return sha3_as_hash(sha3_new384(a));
}

static Hash new_sha3_512(Alloc *a) {
    return sha3_as_hash(sha3_new512(a));
}

/* Not in Go: make([]byte, n). */
static Slice make_bytes(Alloc *a, Int n) {
    return slice_make(a, TYPE_BYTE, n, n);
}

/* Not in Go: a response of n byte strings, for the handler to fill in. */
static AcvpArgs make_args(Alloc *a, Int n) {
    AcvpArgs r = {BURROW_NEW_N(a, Slice, n > 0 ? (size_t)n : 1), n};
    for (Int i = 0; i < n; i++)
        r.v[i] = slice_nil(TYPE_BYTE);
    return r;
}

static AcvpArgs args1(Alloc *a, Slice x) {
    AcvpArgs r = make_args(a, 1);
    r.v[0] = x;
    return r;
}

static Str bytes_str(Slice b) {
    return str_from_bytes(b.p, b.len);
}

static Slice str_bytes(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static uint32_t le32(Slice b) {
    return binary_little_endian_uint32(b);
}

static Slice hash_digest(Alloc *a, HashNewFunc f, Slice msg) {
    Hash h = f(a);
    hash_write(h, msg, NULL);
    return hash_sum(a, h, make_bytes(a, 0));
}

/* --------------------------------------------------------- the protocol */

static Error read_args(Alloc *a, IoReader reader, uint32_t required_args,
                       AcvpArgs *out);
static Error write_response(Alloc *a, IoWriter writer, AcvpArgs args);
static const AcvpCommand *lookup_command(Str name);

static Error read_request(Alloc *a, IoReader reader, AcvpRequest *req) {
    /* Per ACVP.md:
     *   Requests consist of one or more byte strings and responses consist
     *   of zero or more byte strings. A request contains: the number of byte
     *   strings, the length of each byte string, and the contents of each byte
     *   string. All numbers are 32-bit little-endian and values are
     *   concatenated in the order specified. */
    uint32_t num_args = 0;
    Error err = binary_read(a, reader, binary_little_endian,
                            BURROW_ANY(TYPE_UINT32, &num_args));
    if (BURROW_FAILED(err))
        return err;
    if (num_args == 0)
        return errors_new(error_allocator(), BURROW_S("invalid request: zero args"));

    AcvpArgs args;
    err = read_args(a, reader, num_args, &args);
    if (BURROW_FAILED(err))
        return err;

    req->name = bytes_str(args.v[0]);
    req->args = (AcvpArgs){args.v + 1, args.len - 1};
    return BURROW_NO_ERROR;
}

static Error read_args(Alloc *a, IoReader reader, uint32_t required_args,
                       AcvpArgs *out) {
    uint32_t *arg_lengths =
        BURROW_NEW_N(a, uint32_t, required_args > 0 ? required_args : 1);
    AcvpArgs args = make_args(a, (Int)required_args);

    for (uint32_t i = 0; i < required_args; i++) {
        Error err = binary_read(a, reader, binary_little_endian,
                                BURROW_ANY(TYPE_UINT32, &arg_lengths[i]));
        if (BURROW_FAILED(err))
            return fmt_errorf_v("invalid request: failed to read %d-th arg len: %w",
                                (Int)i, err);
    }

    for (uint32_t i = 0; i < required_args; i++) {
        Slice buf = make_bytes(a, (Int)arg_lengths[i]);
        Error err = BURROW_NO_ERROR;
        io_read_full(reader, buf, &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("invalid request: failed to read %d-th arg data: %w",
                                (Int)i, err);
        args.v[i] = buf;
    }

    *out = args;
    return BURROW_NO_ERROR;
}

static Error write_response(Alloc *a, IoWriter writer, AcvpArgs args) {
    /* See `readRequest` for details on the base format. Per ACVP.md:
     *   A response has the same format except that there may be zero byte
     *   strings and the first byte string has no special meaning. */
    uint32_t num_args = (uint32_t)args.len;
    Error err = binary_write(a, writer, binary_little_endian,
                             BURROW_ANY(TYPE_UINT32, &num_args));
    if (BURROW_FAILED(err))
        return fmt_errorf_v("writing arg count: %w", err);

    for (Int i = 0; i < args.len; i++) {
        uint32_t n = (uint32_t)args.v[i].len;
        err =
            binary_write(a, writer, binary_little_endian, BURROW_ANY(TYPE_UINT32, &n));
        if (BURROW_FAILED(err))
            return fmt_errorf_v("writing %d-th arg length: %w", i, err);
    }

    for (Int i = 0; i < args.len; i++) {
        writer.vt->write(writer.data, args.v[i], &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("writing %d-th arg data: %w", i, err);
    }

    return BURROW_NO_ERROR;
}

static Error processing_loop(IoReader reader, IoWriter writer) {
    /* Per ACVP.md:
     *   The protocol is request–response: the subprocess only speaks in
     *   response to a request and there is exactly one response for every
     *   request. */
    for (;;) {
        /* Not in Go: what a request needs is made from an arena of its own. */
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);

        AcvpRequest req;
        Error err = read_request(a, reader, &req);
        if (errors_is(err, io_eof)) {
            arena_free(&ar);
            break;
        } else if (BURROW_FAILED(err)) {
            arena_free(&ar);
            return fmt_errorf_v("reading request: %w", err);
        }

        if (entropy_testing && strings_has_prefix(req.name, BURROW_S("SHA2-384")))
            req.name = fmt_sprintf_v(a, "%s/ENTROPY", req.name);

        const AcvpCommand *cmd = lookup_command(req.name);
        if (cmd == NULL) {
            err = fmt_errorf_v("unknown command: %q", req.name);
            arena_free(&ar);
            return err;
        }

        Int got_args = req.args.len;
        if (got_args != cmd->required_args) {
            err = fmt_errorf_v("command %q expected %d args, got %d", req.name,
                               cmd->required_args, got_args);
            arena_free(&ar);
            return err;
        }

        AcvpArgs response = {NULL, 0};
        err = cmd->handler(a, cmd, req.args, &response);
        if (BURROW_FAILED(err)) {
            err = fmt_errorf_v("command %q failed: %w", req.name, err);
            arena_free(&ar);
            return err;
        }

        err = write_response(a, writer, response);
        if (BURROW_FAILED(err)) {
            err = fmt_errorf_v("command %q response failed: %w", req.name, err);
            arena_free(&ar);
            return err;
        }
        arena_free(&ar);
    }

    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------ commands */

/* "All implementations must support the getConfig command
 * which takes no arguments and returns a single byte string
 * which is a JSON blob of ACVP algorithm configuration." */
static Error cmd_get_config(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                            AcvpArgs *out) {
    (void)c;
    (void)args;
    /* If GOENTROPYSOURCEACVP is set, then use the entropyCapabilitiesJson
     * instead of capabilitiesJson. */
    Slice capabilities = capabilities_json;
    if (entropy_testing)
        capabilities = entropy_capabilities_json;
    *out = args1(a, capabilities);
    return BURROW_NO_ERROR;
}

/* cmdHashAft returns a command handler for the specified hash
 * algorithm for algorithm functional test (AFT) test cases.
 *
 * This shape of command expects a message as the sole argument,
 * and writes the resulting digest as a response.
 *
 * See https://pages.nist.gov/ACVP/draft-celi-acvp-sha.html */
#define CMD_HASH_AFT(f)                                                                \
    .required_args = 1, /* Message to hash. */ .handler = cmd_hash_aft, .h = (f)

static Error cmd_hash_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                          AcvpArgs *out) {
    *out = args1(a, hash_digest(a, c->h, args.v[0]));
    return BURROW_NO_ERROR;
}

/* cmdHashMct returns a command handler for the specified hash
 * algorithm for monte carlo test (MCT) test cases.
 *
 * This shape of command expects a seed as the sole argument,
 * and writes the resulting digest as a response. It implements
 * the "standard" flavour of the MCT, not the "alternative".
 *
 * This algorithm was ported from `HashMCT` in BSSL's `modulewrapper.cc`
 * Note that it differs slightly from the upstream NIST MCT[0] algorithm
 * in that it does not perform the outer 100 iterations itself. See
 * footnote #1 in the ACVP.md docs[1], the acvptool handles this.
 *
 * [0]: https://pages.nist.gov/ACVP/draft-celi-acvp-sha.html#section-6.2
 * [1]: https://boringssl.googlesource.com/boringssl/+/refs/heads/master/util/fipstools/acvp/ACVP.md#testing-other-fips-modules */
#define CMD_HASH_MCT(f)                                                                \
    .required_args = 1, /* Seed message. */ .handler = cmd_hash_mct, .h = (f)

static Error cmd_hash_mct(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                          AcvpArgs *out) {
    Hash h = c->h(a);
    Int h_size = hash_size(h);
    Slice seed = args.v[0];

    Int seed_len = seed.len;
    if (seed_len != h_size)
        return fmt_errorf_v("invalid seed size: expected %d got %d", h_size, seed_len);

    Slice digest = make_bytes(a, h_size);
    Byte *buf = BURROW_NEW_N(a, Byte, (size_t)(3 * h_size));
    for (Int i = 0; i < 3; i++)
        memcpy(buf + i * h_size, seed.p, (size_t)h_size);
    Slice bs = slice_from(buf, 3 * h_size, 3 * h_size, TYPE_BYTE);

    for (int i = 0; i < 1000; i++) {
        hash_reset(h);
        hash_write(h, bs, NULL);
        digest = hash_sum(a, h, slice_sub(digest, 0, 0));

        memmove(buf, buf + h_size, (size_t)(2 * h_size));
        memcpy(buf + 2 * h_size, digest.p, (size_t)h_size);
    }

    *out = args1(a, slice_sub(bs, h_size * 2, bs.len));
    return BURROW_NO_ERROR;
}

/* cmdEntropyHashEntropySha384Aft returns a command handler that tests the
 * entropy package's SHA2-384 digest for AFT inputs. */
static Error cmd_entropy_hash_entropy_sha384_aft(Alloc *a, const AcvpCommand *c,
                                                 AcvpArgs args, AcvpArgs *out) {
    (void)c;
    Slice digest = make_bytes(a, 48);
    burrow__entropy_testing_only_sha384(digest.p, args.v[0]);
    *out = args1(a, digest);
    return BURROW_NO_ERROR;
}

/* cmdEntropyHashEntropySha384Mct returns a command handler that tests the
 * entropy package's SHA2-384 digest for MCT inputs. */
static Error cmd_entropy_hash_entropy_sha384_mct(Alloc *a, const AcvpCommand *c,
                                                 AcvpArgs args, AcvpArgs *out) {
    (void)c;
    Int h_size = 48;
    Slice seed = args.v[0];

    Int n = 3 * seed.len;
    /* Not in Go: buf[2*hSize:] panics in Go when the seed is shorter than 32
     * bytes, and this returns an error instead. */
    if (n < 2 * h_size)
        return fmt_errorf_v("invalid seed size: %d", seed.len);
    Byte *buf = BURROW_NEW_N(a, Byte, (size_t)n);
    for (Int i = 0; i < 3; i++)
        memcpy(buf + i * seed.len, seed.p, (size_t)seed.len);
    Slice bs = slice_from(buf, n, n, TYPE_BYTE);

    for (int i = 0; i < 1000; i++) {
        Byte digest_raw[48];
        burrow__entropy_testing_only_sha384(digest_raw, bs);

        /* Not in Go: copy(buf, buf[hSize:]) and copy(buf[2*hSize:], digest),
         * each copying no more than fits. */
        memmove(buf, buf + h_size, (size_t)(n - h_size));
        Int k = n - 2 * h_size < h_size ? n - 2 * h_size : h_size;
        memcpy(buf + 2 * h_size, digest_raw, (size_t)k);
    }

    *out = args1(a, slice_sub(bs, h_size * 2, n));
    return BURROW_NO_ERROR;
}

/* cmdSha3Mct returns a command handler for the specified hash
 * algorithm for SHA-3 monte carlo test (MCT) test cases.
 *
 * This shape of command expects a seed as the sole argument,
 * and writes the resulting digest as a response. It implements
 * the "standard" flavour of the MCT, not the "alternative".
 *
 * This algorithm was ported from the "standard" MCT algorithm
 * specified in  draft-celi-acvp-sha3[0]. Note this differs from
 * the SHA2-* family of MCT tests handled by cmdHashMct. However,
 * like that handler it does not perform the outer 100 iterations.
 *
 * [0]: https://pages.nist.gov/ACVP/draft-celi-acvp-sha3.html#section-6.2.1 */
#define CMD_SHA3_MCT(f)                                                                \
    .required_args = 1, /* Seed message. */ .handler = cmd_sha3_mct, .h = (f)

static Error cmd_sha3_mct(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                          AcvpArgs *out) {
    Hash h = c->h(a);
    Slice md = args.v[0];

    for (int i = 1; i <= 1000; i++) {
        hash_reset(h);
        hash_write(h, md, NULL);
        md = hash_sum(a, h, make_bytes(a, 0));
    }

    *out = args1(a, md);
    return BURROW_NO_ERROR;
}

#define CMD_SHAKE_AFT_VOT(f)                                                           \
    .required_args = 2, /* Message, output length (bytes) */                           \
        .handler = cmd_shake_aft_vot, .shake = (f)

static Error cmd_shake_aft_vot(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                               AcvpArgs *out) {
    Slice msg = args.v[0];

    uint32_t out_len_bytes = le32(args.v[1]);
    Slice digest = make_bytes(a, (Int)out_len_bytes);

    Sha3SHAKE *h = c->shake(a);
    sha3_shake_write(h, msg, NULL);
    sha3_shake_read(h, digest, NULL);

    *out = args1(a, digest);
    return BURROW_NO_ERROR;
}

#define CMD_SHAKE_MCT(f)                                                               \
    .required_args =                                                                   \
        4, /* Seed message, min output length (bytes), max output length  \
                           (bytes), output length (bytes) */            \
        .handler = cmd_shake_mct, .shake = (f)

static Error cmd_shake_mct(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                           AcvpArgs *out) {
    Slice md = args.v[0];
    uint32_t min_out_bytes = le32(args.v[1]);
    uint32_t max_out_bytes = le32(args.v[2]);

    uint32_t output_len_bytes = le32(args.v[3]);
    if (output_len_bytes < 2)
        return fmt_errorf_v("invalid output length: %d", (Int)output_len_bytes);

    uint32_t range_bytes = max_out_bytes - min_out_bytes + 1;
    if (range_bytes == 0)
        return fmt_errorf_v("invalid maxOutBytes and minOutBytes: %d, %d",
                            (Int)max_out_bytes, (Int)min_out_bytes);

    Sha3SHAKE *h = c->shake(a);
    for (int i = 0; i < 1000; i++) {
        /* "The MSG[i] input to SHAKE MUST always contain at least 128 bits. If
         * this is not the case as the previous digest was too short, append
         * empty bits to the rightmost side of the digest." */
        Int boundary = md.len < 16 ? md.len : 16;
        Slice msg = make_bytes(a, 16);
        memset(msg.p, 0, 16);
        if (boundary > 0)
            memcpy(msg.p, md.p, (size_t)boundary);

        /*  MD[i] = SHAKE(MSG[i], OutputLen * 8) */
        sha3_shake_reset(h);
        sha3_shake_write(h, msg, NULL);
        Slice digest = make_bytes(a, (Int)output_len_bytes);
        sha3_shake_read(h, digest, NULL);
        md = digest;

        /* RightmostOutputBits = 16 rightmost bits of MD[i] as an integer
         * OutputLen = minOutBytes + (RightmostOutputBits % Range) */
        const Byte *p = md.p;
        uint32_t rightmost_output =
            (uint32_t)p[output_len_bytes - 2] << 8 | (uint32_t)p[output_len_bytes - 1];
        output_len_bytes = min_out_bytes + (rightmost_output % range_bytes);
    }

    Slice encoded_output_len_bytes = make_bytes(a, 4);
    binary_little_endian_put_uint32(encoded_output_len_bytes, output_len_bytes);

    *out = make_args(a, 2);
    out->v[0] = md;
    out->v[1] = encoded_output_len_bytes;
    return BURROW_NO_ERROR;
}

#define CMD_CSHAKE_AFT(f)                                                              \
    .required_args =                                                                   \
        4, /* Message, output length bytes, function name, customization  \
                         */            \
        .handler = cmd_cshake_aft, .cshake = (f)

static Error cmd_cshake_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                            AcvpArgs *out) {
    Slice msg = args.v[0];
    uint32_t out_len_bytes = le32(args.v[1]);
    Slice function_name = args.v[2];
    Slice customization = args.v[3];

    Sha3SHAKE *h = c->cshake(a, function_name, customization);
    sha3_shake_write(h, msg, NULL);

    Slice o = make_bytes(a, (Int)out_len_bytes);
    sha3_shake_read(h, o, NULL);

    *out = args1(a, o);
    return BURROW_NO_ERROR;
}

#define CMD_CSHAKE_MCT(f)                                                              \
    .required_args =                                                                   \
        6, /* Message, min output length (bits), max output length        \
                           (bits), output length (bits), increment (bits),             \
                           customization */            \
        .handler = cmd_cshake_mct, .cshake = (f)

static Error cmd_cshake_mct(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                            AcvpArgs *out) {
    Slice message = args.v[0];
    uint32_t min_out_len_bytes = le32(args.v[1]);
    uint32_t max_out_len_bytes = le32(args.v[2]);
    uint32_t output_len_bytes = le32(args.v[3]);
    uint32_t increment_bytes = le32(args.v[4]);
    Slice customization = args.v[5];

    if (output_len_bytes < 2)
        return fmt_errorf_v("invalid output length: %d", (Int)output_len_bytes);

    uint32_t range_bits = (max_out_len_bytes * 8 - min_out_len_bytes * 8) + 1;
    if (range_bits == 0)
        return fmt_errorf_v("invalid maxOutLenBytes and minOutLenBytes: %d, %d",
                            (Int)max_out_len_bytes, (Int)min_out_len_bytes);

    /* cSHAKE Monte Carlo test inner loop:
     *   https://pages.nist.gov/ACVP/draft-celi-acvp-xof.html#section-6.2.1 */
    for (int i = 0; i < 1000; i++) {
        /* InnerMsg = Left(Output[i-1] || ZeroBits(128), 128); */
        Int boundary = message.len < 16 ? message.len : 16;
        Slice inner_msg = make_bytes(a, 16);
        memset(inner_msg.p, 0, 16);
        if (boundary > 0)
            memcpy(inner_msg.p, message.p, (size_t)boundary);

        /* Output[i] = CSHAKE(InnerMsg, OutputLen, FunctionName,
         * Customization); */
        Sha3SHAKE *h =
            c->cshake(a, slice_nil(TYPE_BYTE),
                      customization); /* Note: function name fixed to "" for MCT. */
        sha3_shake_write(h, inner_msg, NULL);
        Slice digest = make_bytes(a, (Int)output_len_bytes);
        sha3_shake_read(h, digest, NULL);
        message = digest;

        /* Rightmost_Output_bits = Right(Output[i], 16); */
        Slice rightmost_output =
            slice_sub(digest, (Int)output_len_bytes - 2, digest.len);
        /* IMPORTANT: the specification says:
         *   NOTE: For the "Rightmost_Output_bits % Range" operation, the
         *   Rightmost_Output_bits bit string should be interpreted as a little
         *   endian-encoded number.
         * This is **a lie**! It has to be interpreted as a big-endian
         * number. */
        uint16_t rightmost_output_be = binary_big_endian_uint16(rightmost_output);

        /* OutputLen = MinOutLen + (floor((Rightmost_Output_bits % Range) /
         * OutLenIncrement) * OutLenIncrement); */
        uint32_t increment_bits = increment_bytes * 8;
        uint32_t output_len_bits =
            (min_out_len_bytes * 8) +
            (((uint32_t)rightmost_output_be % range_bits) / increment_bits) *
                increment_bits;
        output_len_bytes = output_len_bits / 8;

        /* Customization = BitsToString(InnerMsg || Rightmost_Output_bits); */
        Slice msg_with_bits = make_bytes(a, 18);
        memcpy(msg_with_bits.p, inner_msg.p, 16);
        memcpy((Byte *)msg_with_bits.p + 16, rightmost_output.p, 2);
        customization = make_bytes(a, msg_with_bits.len);
        for (Int j = 0; j < msg_with_bits.len; j++)
            ((Byte *)customization.p)[j] =
                (Byte)((((const Byte *)msg_with_bits.p)[j] % 26) + 65);
    }

    Slice encoded_output_len_bytes = make_bytes(a, 4);
    binary_little_endian_put_uint32(encoded_output_len_bytes, output_len_bytes);

    *out = make_args(a, 3);
    out->v[0] = message;
    out->v[1] = encoded_output_len_bytes;
    out->v[2] = customization;
    return BURROW_NO_ERROR;
}

#define CMD_HMAC_AFT(f)                                                                \
    .required_args = 2, /* Message and key */ .handler = cmd_hmac_aft, .h = (f)

static Error cmd_hmac_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                          AcvpArgs *out) {
    Slice msg = args.v[0];
    Slice key = args.v[1];
    Hash mac = burrow__hmac_new(a, c->h, key);
    hash_write(mac, msg, NULL);
    *out = args1(a, hash_sum(a, mac, make_bytes(a, 0)));
    return BURROW_NO_ERROR;
}

#define CMD_HKDF_AFT(f)                                                                \
    .required_args = 4, /* Key, salt, info, length bytes */ .handler = cmd_hkdf_aft,   \
    .h = (f)

static Error cmd_hkdf_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                          AcvpArgs *out) {
    Slice key = args.v[0];
    Slice salt = args.v[1];
    Slice info = args.v[2];
    Int key_len = (Int)le32(args.v[3]);

    /* Not in Go: the module's hkdf.Key is hkdf.Extract and then hkdf.Expand,
     * which is what this does. */
    Error err = BURROW_NO_ERROR;
    Slice prk = burrow__hkdf_extract(a, c->h, key, salt, &err);
    if (BURROW_FAILED(err))
        return err;
    Slice k = burrow__hkdf_expand(a, c->h, prk, bytes_str(info), key_len, &err);
    if (BURROW_FAILED(err))
        return err;
    *out = args1(a, k);
    return BURROW_NO_ERROR;
}

#define CMD_HKDF_EXTRACT_AFT(f)                                                        \
    .required_args = 2, /* secret, salt */ .handler = cmd_hkdf_extract_aft, .h = (f)

static Error cmd_hkdf_extract_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                  AcvpArgs *out) {
    Slice secret = args.v[0];
    Slice salt = args.v[1];

    Error err = BURROW_NO_ERROR;
    Slice prk = burrow__hkdf_extract(a, c->h, secret, salt, &err);
    if (BURROW_FAILED(err))
        return err;
    *out = args1(a, prk);
    return BURROW_NO_ERROR;
}

#define CMD_HKDF_EXPAND_LABEL_AFT(f)                                                   \
    .required_args = 4, /* output length, secret, label, transcript hash */            \
        .handler = cmd_hkdf_expand_label_aft, .h = (f)

static Error cmd_hkdf_expand_label_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                       AcvpArgs *out) {
    Int key_len = (Int)le32(args.v[0]);
    Slice secret = args.v[1];
    Slice label = args.v[2];
    Slice transcript_hash = args.v[3];

    *out = args1(a, burrow__tls13_expand_label(a, c->h, secret, bytes_str(label),
                                               transcript_hash, key_len));
    return BURROW_NO_ERROR;
}

static Error lookup_hash(Str name, HashNewFunc *h) {
    *h = NULL;
    if (str_eq(name, BURROW_S("SHA2-224")))
        *h = sha256_new224;
    else if (str_eq(name, BURROW_S("SHA2-256")))
        *h = sha256_new;
    else if (str_eq(name, BURROW_S("SHA2-384")))
        *h = sha512_new384;
    else if (str_eq(name, BURROW_S("SHA2-512")))
        *h = sha512_new;
    else if (str_eq(name, BURROW_S("SHA2-512/224")))
        *h = sha512_new512224;
    else if (str_eq(name, BURROW_S("SHA2-512/256")))
        *h = sha512_new512256;
    else if (str_eq(name, BURROW_S("SHA3-224")))
        *h = new_sha3_224;
    else if (str_eq(name, BURROW_S("SHA3-256")))
        *h = new_sha3_256;
    else if (str_eq(name, BURROW_S("SHA3-384")))
        *h = new_sha3_384;
    else if (str_eq(name, BURROW_S("SHA3-512")))
        *h = new_sha3_512;
    else
        return fmt_errorf_v("unknown hash name: %q", name);

    return BURROW_NO_ERROR;
}

/* Hash name, key length, salt, password, iteration count */
#define CMD_PBKDF() .required_args = 5, .handler = cmd_pbkdf

static Error cmd_pbkdf(Alloc *a, const AcvpCommand *c, AcvpArgs args, AcvpArgs *out) {
    (void)c;
    HashNewFunc h;
    Error err = lookup_hash(bytes_str(args.v[0]), &h);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("PBKDF2 failed: %w", err);

    uint32_t key_len = le32(args.v[1]) / 8;
    Slice salt = args.v[2];
    Slice password = args.v[3];
    uint32_t iteration_count = le32(args.v[4]);

    /* Not in Go: the module's pbkdf2.Key is static in src/crypto/pbkdf2.c, so
     * this calls pbkdf2_key, which checks what fips140=only allows first and
     * is the module's otherwise. */
    Slice derived_key = pbkdf2_key(a, h, bytes_str(password), salt,
                                   (Int)iteration_count, (Int)key_len, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("PBKDF2 failed: %w", err);

    *out = args1(a, derived_key);
    return BURROW_NO_ERROR;
}

/* Not in Go: [][]byte{{b}}. */
static AcvpArgs byte_args(Alloc *a, Byte b) {
    AcvpArgs r = args1(a, make_bytes(a, 1));
    ((Byte *)r.v[0].p)[0] = b;
    return r;
}

/* Not in Go: Go calls the module's ed25519 package. Here that is the
 * ed25519_* functions, which are the module's code, with the length checks of
 * the module's NewPrivateKeyFromSeed and NewPublicKey done here, since the
 * public functions panic instead. */
#define CMD_EDDSA_KEY_GEN_AFT()                                                        \
    .required_args = 1 /* Curve name */, .handler = cmd_eddsa_key_gen_aft

static Error cmd_eddsa_key_gen_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                   AcvpArgs *out) {
    (void)c;
    if (!str_eq(bytes_str(args.v[0]), BURROW_S("ED-25519")))
        return fmt_errorf_v("unsupported EDDSA curve: %q", bytes_str(args.v[0]));

    Ed25519PrivateKey sk = slice_nil(TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Ed25519PublicKey pk = ed25519_generate_key(a, (IoReader){0}, &sk, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating EDDSA keypair: %w", err);

    /* EDDSA/keyGen/AFT responses are d & q, described[0] as:
     *   d	The encoded private key point
     *   q	The encoded public key point
     *
     * Contrary to the description of a "point", d is the private key
     * seed bytes per FIPS.186-5[1] A.2.3.
     *
     * [0]: https://pages.nist.gov/ACVP/draft-celi-acvp-eddsa.html#section-9.1
     * [1]: https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.186-5.pdf */
    *out = make_args(a, 2);
    out->v[0] = ed25519_private_key_seed(sk, a);
    out->v[1] = pk;
    return BURROW_NO_ERROR;
}

#define CMD_EDDSA_KEY_VER_AFT()                                                        \
    .required_args = 2 /* Curve name, Q */, .handler = cmd_eddsa_key_ver_aft

static Error cmd_eddsa_key_ver_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                   AcvpArgs *out) {
    (void)c;
    if (!str_eq(bytes_str(args.v[0]), BURROW_S("ED-25519")))
        return fmt_errorf_v("unsupported EDDSA curve: %q", bytes_str(args.v[0]));

    /* Verify the point is on the curve. The higher-level ed25519 API does
     * this at signature verification time so we have to use the lower-level
     * edwards25519 package to do it here in absence of a signature to verify. */
    Edwards25519Point p;
    Error err = BURROW_NO_ERROR;
    if (burrow__ge_set_bytes(&p, args.v[1], &err) == NULL) {
        *out = byte_args(a, 0);
        return BURROW_NO_ERROR;
    }

    *out = byte_args(a, 1);
    return BURROW_NO_ERROR;
}

#define CMD_EDDSA_SIG_GEN_AFT_BFT()                                                    \
    .required_args = 5, /* Curve name, private key seed, message, prehash, context */  \
        .handler = cmd_eddsa_sig_gen_aft_bft

static Error cmd_eddsa_sig_gen_aft_bft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                       AcvpArgs *out) {
    (void)c;
    if (!str_eq(bytes_str(args.v[0]), BURROW_S("ED-25519")))
        return fmt_errorf_v("unsupported EDDSA curve: %q", bytes_str(args.v[0]));

    if (args.v[1].len != ED25519_SEED_SIZE) {
        Error err = fmt_errorf_v("ed25519: bad seed length: %d", args.v[1].len);
        return fmt_errorf_v("error creating private key: %w", err);
    }
    Ed25519PrivateKey sk = ed25519_new_key_from_seed(a, args.v[1]);
    Slice msg = args.v[2];
    Slice prehash = args.v[3];
    Str context = bytes_str(args.v[4]);

    Slice sig;
    if (((const Byte *)prehash.p)[0] == 1) {
        msg = hash_digest(a, sha512_new, msg);

        /* With ed25519 the context is only specified for sigGen tests when using
         * prehashing. See
         * https://pages.nist.gov/ACVP/draft-celi-acvp-eddsa.html#section-8.6 */
        Ed25519Options opts = {CRYPTO_SHA512, context};
        Error err = BURROW_NO_ERROR;
        sig = ed25519_private_key_sign(sk, a, (IoReader){0}, msg,
                                       ed25519_options_as_signer_opts(&opts), &err);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("error signing message: %w", err);
    } else {
        sig = ed25519_sign(a, sk, msg);
    }

    *out = args1(a, sig);
    return BURROW_NO_ERROR;
}

#define CMD_EDDSA_SIG_VER_AFT()                                                        \
    .required_args = 5, /* Curve name, message, public key, signature, prehash */      \
        .handler = cmd_eddsa_sig_ver_aft

static Error cmd_eddsa_sig_ver_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                   AcvpArgs *out) {
    (void)c;
    if (!str_eq(bytes_str(args.v[0]), BURROW_S("ED-25519")))
        return fmt_errorf_v("unsupported EDDSA curve: %q", bytes_str(args.v[0]));

    Slice msg = args.v[1];
    Ed25519PublicKey pk = args.v[2];
    Error err = BURROW_NO_ERROR;
    if (pk.len != ED25519_PUBLIC_KEY_SIZE) {
        err = fmt_errorf_v("ed25519: bad public key length: %d", pk.len);
        return fmt_errorf_v("invalid public key: %w", err);
    }
    Edwards25519Point p;
    if (burrow__ge_set_bytes(&p, pk, &err) == NULL) {
        err = errors_new(error_allocator(), BURROW_S("ed25519: bad public key"));
        return fmt_errorf_v("invalid public key: %w", err);
    }
    Slice sig = args.v[3];
    Slice prehash = args.v[4];

    bool ok;
    if (((const Byte *)prehash.p)[0] == 1) {
        msg = hash_digest(a, sha512_new, msg);
        /* Context is only specified for sigGen, not sigVer.
         * See https://pages.nist.gov/ACVP/draft-celi-acvp-eddsa.html#section-8.6 */
        Ed25519Options opts = {CRYPTO_SHA512, BURROW_S("")};
        ok = !BURROW_FAILED(ed25519_verify_with_options(pk, msg, sig, &opts));
    } else {
        ok = ed25519_verify(pk, msg, sig);
    }

    if (!ok) {
        *out = byte_args(a, 0);
        return BURROW_NO_ERROR;
    }

    *out = byte_args(a, 1);
    return BURROW_NO_ERROR;
}

static Error lookup_curve(Str name, EllipticCurve *c);

/* Not in Go: the module's curve for curve, which Go picks with a switch on
 * curve.Params() in each handler, or NULL for its default case. That case
 * prints the curve, and here it is the curve's name. */
static const EcdsaFipsCurve *ecdsa_fips_curve(EllipticCurve curve) {
    const EllipticCurveParams *p = elliptic_curve_params(curve);
    if (p == elliptic_curve_params(elliptic_p224()))
        return burrow__ecdsa_fips_p224();
    if (p == elliptic_curve_params(elliptic_p256()))
        return burrow__ecdsa_fips_p256();
    if (p == elliptic_curve_params(elliptic_p384()))
        return burrow__ecdsa_fips_p384();
    if (p == elliptic_curve_params(elliptic_p521()))
        return burrow__ecdsa_fips_p521();
    return NULL;
}

/* Not in Go: Go calls the module's ecdsa.GenerateKey. Here that is
 * ecdsa_generate_key, which calls the module's code for the four curves. */
#define CMD_ECDSA_KEY_GEN_AFT()                                                        \
    .required_args = 1 /* Curve name */, .handler = cmd_ecdsa_key_gen_aft

static Error cmd_ecdsa_key_gen_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                   AcvpArgs *out) {
    (void)c;
    EllipticCurve curve;
    Error err = lookup_curve(bytes_str(args.v[0]), &curve);
    if (BURROW_FAILED(err))
        return err;

    if (ecdsa_fips_curve(curve) == NULL)
        return fmt_errorf_v("unsupported curve: %v",
                            elliptic_curve_params(curve)->name);
    EcdsaPrivateKey *sk = ecdsa_generate_key(a, curve, crypto_rand_reader, &err);

    if (BURROW_FAILED(err))
        return err;

    Slice sk_bytes = ecdsa_private_key_bytes(sk, a, &err);
    if (BURROW_FAILED(err))
        return err;
    Slice pub_bytes = ecdsa_public_key_bytes(&sk->public_key, a, &err);
    if (BURROW_FAILED(err))
        return err;
    Int byte_len = (elliptic_curve_params(curve)->bit_size + 7) / 8;

    *out = make_args(a, 3);
    out->v[0] = sk_bytes;
    out->v[1] = slice_sub(pub_bytes, 1, 1 + byte_len);
    out->v[2] = slice_sub(pub_bytes, 1 + byte_len, pub_bytes.len);
    return BURROW_NO_ERROR;
}

#define CMD_ECDSA_KEY_VER_AFT()                                                        \
    .required_args = 3 /* Curve name, X, Y */, .handler = cmd_ecdsa_key_ver_aft

static Error cmd_ecdsa_key_ver_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                   AcvpArgs *out) {
    (void)c;
    EllipticCurve curve;
    Error err = lookup_curve(bytes_str(args.v[0]), &curve);
    if (BURROW_FAILED(err))
        return err;

    BigInt x = BIG_INT(a), y = BIG_INT(a);
    big_int_set_bytes(&x, args.v[1]);
    big_int_set_bytes(&y, args.v[2]);

    if (elliptic_curve_is_on_curve(curve, &x, &y)) {
        *out = byte_args(a, 1);
        return BURROW_NO_ERROR;
    }

    *out = byte_args(a, 0);
    return BURROW_NO_ERROR;
}

/* pointFromAffine is used to convert the PublicKey to a nistec SetBytes input.
 * Duplicated from crypto/ecdsa.go's pointFromAffine. */
static Error point_from_affine(Alloc *a, EllipticCurve curve, const BigInt *x,
                               const BigInt *y, Slice *out) {
    Int bit_size = elliptic_curve_params(curve)->bit_size;
    /* Reject values that would not get correctly encoded. */
    if (big_int_sign(x) < 0 || big_int_sign(y) < 0)
        return errors_new(error_allocator(), BURROW_S("negative coordinate"));
    if (big_int_bit_len(x) > bit_size || big_int_bit_len(y) > bit_size)
        return errors_new(error_allocator(), BURROW_S("overflowing coordinate"));
    /* Encode the coordinates and let SetBytes reject invalid points. */
    Int byte_len = (bit_size + 7) / 8;
    Slice buf = make_bytes(a, 1 + 2 * byte_len);
    ((Byte *)buf.p)[0] = 4; /* uncompressed point */
    big_int_fill_bytes(x, slice_sub(buf, 1, 1 + byte_len));
    big_int_fill_bytes(y, slice_sub(buf, 1 + byte_len, 1 + 2 * byte_len));
    *out = buf;
    return BURROW_NO_ERROR;
}

/* Not in Go: the signature is r and s, made from a. */
static Error sign_ecdsa(Alloc *a, const EcdsaFipsCurve *c, HashNewFunc h,
                        EcdsaSigType sig_type, Slice q, Slice sk, Slice digest,
                        Slice *r, Slice *s) {
    Error err = BURROW_NO_ERROR;
    if (!burrow__ecdsa_fips_new_private_key(c, sk, q, &err))
        return fmt_errorf_v("invalid private key: %w", err);

    Int n = burrow__ecdsa_fips_size(c);
    *r = make_bytes(a, n);
    *s = make_bytes(a, n);
    bool ok;
    if (sig_type == ECDSA_SIG_TYPE_NORMAL)
        ok = burrow__ecdsa_fips_sign(c, h, sk, crypto_rand_reader, digest, r->p, s->p,
                                     &err);
    else if (sig_type == ECDSA_SIG_TYPE_DETERMINISTIC)
        ok = burrow__ecdsa_fips_sign_deterministic(c, h, sk, digest, r->p, s->p, &err);
    else
        return fmt_errorf_v("unsupported signature type: %v", (Int)sig_type);
    if (!ok)
        return fmt_errorf_v("signing failed: %w", err);

    return BURROW_NO_ERROR;
}

#define CMD_ECDSA_SIG_GEN_AFT(t)                                                       \
    .required_args = 4, /* Curve name, private key, hash name, message */              \
        .handler = cmd_ecdsa_sig_gen_aft, .sig_type = (t)

static Error cmd_ecdsa_sig_gen_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                   AcvpArgs *out) {
    EllipticCurve curve;
    Error err = lookup_curve(bytes_str(args.v[0]), &curve);
    if (BURROW_FAILED(err))
        return err;

    Slice sk = args.v[1];

    HashNewFunc new_h;
    err = lookup_hash(bytes_str(args.v[2]), &new_h);
    if (BURROW_FAILED(err))
        return err;

    Slice msg = args.v[3];
    Slice digest = hash_digest(a, new_h, msg);

    BigInt d = BIG_INT(a);
    big_int_set_bytes(&d, sk);
    BigInt *y = NULL;
    BigInt *x = elliptic_curve_scalar_base_mult(curve, a, big_int_bytes(&d, a), &y);
    Slice q;
    err = point_from_affine(a, curve, x, y, &q);
    if (BURROW_FAILED(err))
        return err;

    const EcdsaFipsCurve *fc = ecdsa_fips_curve(curve);
    if (fc == NULL)
        return fmt_errorf_v("unsupported curve: %v",
                            elliptic_curve_params(curve)->name);
    Slice r, s;
    err = sign_ecdsa(a, fc, new_h, c->sig_type, q, sk, digest, &r, &s);
    if (BURROW_FAILED(err))
        return err;

    *out = make_args(a, 2);
    out->v[0] = r;
    out->v[1] = s;
    return BURROW_NO_ERROR;
}

#define CMD_ECDSA_SIG_VER_AFT()                                                        \
    .required_args = 7, /* Curve name, hash name, message, X, Y, R, S */               \
        .handler = cmd_ecdsa_sig_ver_aft

static Error verify_ecdsa(const EcdsaFipsCurve *c, Slice q, Slice digest, Slice r,
                          Slice s);

static Error cmd_ecdsa_sig_ver_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                   AcvpArgs *out) {
    (void)c;
    EllipticCurve curve;
    Error err = lookup_curve(bytes_str(args.v[0]), &curve);
    if (BURROW_FAILED(err))
        return err;

    HashNewFunc new_h;
    err = lookup_hash(bytes_str(args.v[1]), &new_h);
    if (BURROW_FAILED(err))
        return err;

    Slice msg = args.v[2];
    Slice digest = hash_digest(a, new_h, msg);

    BigInt x = BIG_INT(a), y = BIG_INT(a);
    big_int_set_bytes(&x, args.v[3]);
    big_int_set_bytes(&y, args.v[4]);
    Slice q;
    err = point_from_affine(a, curve, &x, &y, &q);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("invalid x/y coordinates: %v", err);

    const EcdsaFipsCurve *fc = ecdsa_fips_curve(curve);
    if (fc == NULL)
        return fmt_errorf_v("unsupported curve: %v",
                            elliptic_curve_params(curve)->name);
    err = verify_ecdsa(fc, q, digest, args.v[5], args.v[6]);

    if (!BURROW_FAILED(err)) {
        *out = byte_args(a, 1);
        return BURROW_NO_ERROR;
    }

    *out = byte_args(a, 0);
    return BURROW_NO_ERROR;
}

static Error verify_ecdsa(const EcdsaFipsCurve *c, Slice q, Slice digest, Slice r,
                          Slice s) {
    Error err = BURROW_NO_ERROR;
    if (!burrow__ecdsa_fips_new_public_key(c, q, &err))
        return fmt_errorf_v("invalid public key: %w", err);

    if (!burrow__ecdsa_fips_verify(c, q, digest, r, s, &err))
        return err;
    return BURROW_NO_ERROR;
}

/* Not in Go: Go calls the module's mlkem package. The mlkem_* functions are
 * the module's code, and what only the module has comes from
 * mlkem_internal.h. */
#define CMD_ML_KEM768_KEY_GEN_AFT()                                                    \
    .required_args = 1 /* Seed */, .handler = cmd_ml_kem768_key_gen_aft

static Error cmd_ml_kem768_key_gen_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                       AcvpArgs *out) {
    (void)c;
    Slice seed = args.v[0];

    Error err = BURROW_NO_ERROR;
    MlkemDecapsulationKey768 *dk = mlkem_new_decapsulation_key768(a, seed, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating ML-KEM 768 decapsulation key: %w", err);

    /* Important: we must return the full encoding of dk, not the seed. */
    *out = make_args(a, 2);
    out->v[0] = mlkem_encapsulation_key768_bytes(
        mlkem_decapsulation_key768_encapsulation_key(dk), a);
    out->v[1] = burrow__mlkem_testing_only_expanded_bytes768(dk, a);
    return BURROW_NO_ERROR;
}

#define CMD_ML_KEM768_ENCAP_AFT()                                                      \
    .required_args = 2 /* Public key, entropy */, .handler = cmd_ml_kem768_encap_aft

static Error cmd_ml_kem768_encap_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                     AcvpArgs *out) {
    (void)c;
    Slice pk = args.v[0];
    Slice entropy = args.v[1];

    Error err = BURROW_NO_ERROR;
    MlkemEncapsulationKey768 *ek = mlkem_new_encapsulation_key768(a, pk, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating ML-KEM 768 encapsulation key: %w", err);

    if (entropy.len != 32)
        return fmt_errorf_v("wrong entropy length: got %d, want 32", entropy.len);

    Slice ct = slice_nil(TYPE_BYTE);
    Slice shared_key = burrow__mlkem_encapsulate_internal768(ek, a, entropy.p, &ct);

    *out = make_args(a, 2);
    out->v[0] = ct;
    out->v[1] = shared_key;
    return BURROW_NO_ERROR;
}

#define CMD_ML_KEM768_DECAP_AFT()                                                      \
    .required_args = 2 /* Private key, ciphertext */, .handler = cmd_ml_kem768_decap_aft

static Error cmd_ml_kem768_decap_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                     AcvpArgs *out) {
    (void)c;
    Slice pk = args.v[0];
    Slice ct = args.v[1];

    Error err = BURROW_NO_ERROR;
    MlkemDecapsulationKey768 *dk =
        burrow__mlkem_testing_only_new_decapsulation_key768(a, pk, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating ML-KEM 768 decapsulation key: %w", err);

    Slice shared_key = mlkem_decapsulation_key768_decapsulate(dk, a, ct, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("decapsulating ML-KEM 768 ciphertext: %w", err);

    *out = args1(a, shared_key);
    return BURROW_NO_ERROR;
}

#define CMD_ML_KEM1024_KEY_GEN_AFT()                                                   \
    .required_args = 1 /* Seed */, .handler = cmd_ml_kem1024_key_gen_aft

static Error cmd_ml_kem1024_key_gen_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                        AcvpArgs *out) {
    (void)c;
    Slice seed = args.v[0];

    Error err = BURROW_NO_ERROR;
    MlkemDecapsulationKey1024 *dk = mlkem_new_decapsulation_key1024(a, seed, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating ML-KEM 1024 decapsulation key: %w", err);

    /* Important: we must return the full encoding of dk, not the seed. */
    *out = make_args(a, 2);
    out->v[0] = mlkem_encapsulation_key1024_bytes(
        mlkem_decapsulation_key1024_encapsulation_key(dk), a);
    out->v[1] = burrow__mlkem_testing_only_expanded_bytes1024(dk, a);
    return BURROW_NO_ERROR;
}

#define CMD_ML_KEM1024_ENCAP_AFT()                                                     \
    .required_args = 2 /* Public key, entropy */, .handler = cmd_ml_kem1024_encap_aft

static Error cmd_ml_kem1024_encap_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                      AcvpArgs *out) {
    (void)c;
    Slice pk = args.v[0];
    Slice entropy = args.v[1];

    Error err = BURROW_NO_ERROR;
    MlkemEncapsulationKey1024 *ek = mlkem_new_encapsulation_key1024(a, pk, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating ML-KEM 1024 encapsulation key: %w", err);

    if (entropy.len != 32)
        return fmt_errorf_v("wrong entropy length: got %d, want 32", entropy.len);

    Slice ct = slice_nil(TYPE_BYTE);
    Slice shared_key = burrow__mlkem_encapsulate_internal1024(ek, a, entropy.p, &ct);

    *out = make_args(a, 2);
    out->v[0] = ct;
    out->v[1] = shared_key;
    return BURROW_NO_ERROR;
}

#define CMD_ML_KEM1024_DECAP_AFT()                                                     \
    .required_args = 2 /* Private key, ciphertext */,                                  \
    .handler = cmd_ml_kem1024_decap_aft

static Error cmd_ml_kem1024_decap_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                      AcvpArgs *out) {
    (void)c;
    Slice pk = args.v[0];
    Slice ct = args.v[1];

    Error err = BURROW_NO_ERROR;
    MlkemDecapsulationKey1024 *dk =
        burrow__mlkem_testing_only_new_decapsulation_key1024(a, pk, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating ML-KEM 1024 decapsulation key: %w", err);

    Slice shared_key = mlkem_decapsulation_key1024_decapsulate(dk, a, ct, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("decapsulating ML-KEM 1024 ciphertext: %w", err);

    *out = args1(a, shared_key);
    return BURROW_NO_ERROR;
}

static Error lookup_curve(Str name, EllipticCurve *c) {
    if (str_eq(name, BURROW_S("P-224")))
        *c = elliptic_p224();
    else if (str_eq(name, BURROW_S("P-256")))
        *c = elliptic_p256();
    else if (str_eq(name, BURROW_S("P-384")))
        *c = elliptic_p384();
    else if (str_eq(name, BURROW_S("P-521")))
        *c = elliptic_p521();
    else
        return fmt_errorf_v("unknown curve name: %q", name);

    return BURROW_NO_ERROR;
}

/* Not in Go: Go calls the module's aes package. Here that is aes_new_cipher
 * and the CBC, CTR and GCM of crypto/cipher, which hand an AES block to the
 * module's code. */
#define CMD_AES_CBC(d)                                                                 \
    .required_args = 4, /* Key, ciphertext or plaintext, IV, num iterations */         \
        .handler = cmd_aes_cbc, .aes_direction = (d)

static Error cmd_aes_cbc(Alloc *a, const AcvpCommand *c, AcvpArgs args, AcvpArgs *out) {
    AesDirection direction = c->aes_direction;
    if (direction != AES_ENCRYPT && direction != AES_DECRYPT)
        panic_str(BURROW_S("invalid AES direction"));

    Slice key = args.v[0];
    Slice input = args.v[1];
    Slice iv = args.v[2];
    uint32_t num_iterations = le32(args.v[3]);

    Error err = BURROW_NO_ERROR;
    CipherBlock block_cipher = aes_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("creating AES block cipher with key len %d: %w", key.len,
                            err);

    Int block_size = cipher_block_block_size(block_cipher);
    if (input.len % block_size != 0 || input.len == 0)
        return fmt_errorf_v(
            "invalid ciphertext/plaintext size %d: not a multiple of block size %d",
            input.len, block_size);

    if (block_size != iv.len)
        return fmt_errorf_v("invalid IV size: expected %d, got %d", block_size, iv.len);

    Slice result = make_bytes(a, input.len);
    Slice prev_result = make_bytes(a, input.len);
    Slice prev_input = make_bytes(a, input.len);

    for (uint32_t i = 0; i < num_iterations; i++) {
        slice_copy(prev_result, result);

        if (i > 0) {
            if (direction == AES_ENCRYPT)
                slice_copy(iv, result);
            else
                slice_copy(iv, prev_input);
        }

        if (direction == AES_ENCRYPT) {
            CipherBlockMode cbc_enc = cipher_new_cbc_encrypter(a, block_cipher, iv);
            cipher_block_mode_crypt_blocks(cbc_enc, result, input);
        } else {
            CipherBlockMode cbc_dec = cipher_new_cbc_decrypter(a, block_cipher, iv);
            cipher_block_mode_crypt_blocks(cbc_dec, result, input);
        }

        if (direction == AES_DECRYPT)
            slice_copy(prev_input, input);

        if (i == 0)
            slice_copy(input, iv);
        else
            slice_copy(input, prev_result);
    }

    *out = make_args(a, 2);
    out->v[0] = result;
    out->v[1] = prev_result;
    return BURROW_NO_ERROR;
}

#define CMD_AES_CTR(d)                                                                 \
    .required_args =                                                                   \
        4, /* Key, ciphertext or plaintext, initial counter, num          \
                           iterations (constant 1) */            \
        .handler = cmd_aes_ctr, .aes_direction = (d)

static Error cmd_aes_ctr(Alloc *a, const AcvpCommand *c, AcvpArgs args, AcvpArgs *out) {
    AesDirection direction = c->aes_direction;
    if (direction != AES_ENCRYPT && direction != AES_DECRYPT)
        panic_str(BURROW_S("invalid AES direction"));

    Slice key = args.v[0];
    Slice input = args.v[1];
    Slice iv = args.v[2];
    uint32_t num_iterations = le32(args.v[3]);
    if (num_iterations != 1)
        return fmt_errorf_v("invalid num iterations: expected 1, got %d",
                            (Int)num_iterations);

    if (iv.len != AES_BLOCK_SIZE)
        return fmt_errorf_v("invalid IV size: expected %d, got %d", (Int)AES_BLOCK_SIZE,
                            iv.len);

    Error err = BURROW_NO_ERROR;
    CipherBlock block_cipher = aes_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("creating AES block cipher with key len %d: %w", key.len,
                            err);

    Slice result = make_bytes(a, input.len);
    CipherStream stream = cipher_new_ctr(a, block_cipher, iv);
    cipher_stream_xor_key_stream(stream, result, input);

    *out = args1(a, result);
    return BURROW_NO_ERROR;
}

#define CMD_AES_GCM_SEAL(r)                                                            \
    .required_args =                                                                   \
        5, /* tag len, key, plaintext, nonce (empty for randNonce),       \
                           additional data */            \
        .handler = cmd_aes_gcm_seal, .rand_nonce = (r)

static Error cmd_aes_gcm_seal(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                              AcvpArgs *out) {
    uint32_t tag_len = le32(args.v[0]);
    Slice key = args.v[1];
    Slice plaintext = args.v[2];
    Slice nonce = args.v[3];
    Slice additional_data = args.v[4];

    Error err = BURROW_NO_ERROR;
    CipherBlock block_cipher = aes_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("creating AES block cipher with key len %d: %w", key.len,
                            err);

    CipherAEAD aes_gcm =
        cipher_new_gcm_with_tag_size(a, block_cipher, (Int)tag_len, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("creating AES-GCM with tag len %d: %w", (Int)tag_len, err);

    Slice ct = slice_nil(TYPE_BYTE);
    if (!c->rand_nonce) {
        ct = cipher_aead_seal(aes_gcm, a, slice_nil(TYPE_BYTE), nonce, plaintext,
                              additional_data);
    } else {
        /* Not in Go: gcm.SealWithRandomNonce is not outside the module here, so
         * this seals with cipher_new_gcm_with_random_nonce, which uses the same
         * DRBG for the nonce and puts it in front of the ciphertext, and moves
         * the nonce to the end. Its tag is 16 bytes, as SealWithRandomNonce's
         * output length check makes Go's. */
        CipherAEAD random_gcm = cipher_new_gcm_with_random_nonce(a, block_cipher, &err);
        if (BURROW_FAILED(err))
            return err;
        Slice sealed =
            cipher_aead_seal(random_gcm, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE),
                             plaintext, additional_data);
        Slice internal_nonce = slice_sub(sealed, 0, 12);
        ct = bytes_clone(a, slice_sub(sealed, 12, sealed.len));
        /* acvptool expects the internally generated nonce to be appended to the
         * end of the ciphertext. */
        ct = slice_append(a, ct, internal_nonce.p, internal_nonce.len);
    }

    *out = args1(a, ct);
    return BURROW_NO_ERROR;
}

#define CMD_AES_GCM_OPEN(r)                                                            \
    .required_args =                                                                   \
        5, /* tag len, key, ciphertext, nonce (empty for randNonce),      \
                           additional data */            \
        .handler = cmd_aes_gcm_open, .rand_nonce = (r)

static Error cmd_aes_gcm_open(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                              AcvpArgs *out) {
    uint32_t tag_len = le32(args.v[0]);
    Slice key = args.v[1];
    Slice ciphertext = args.v[2];
    Slice nonce = args.v[3];
    Slice additional_data = args.v[4];

    Error err = BURROW_NO_ERROR;
    CipherBlock block_cipher = aes_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("creating AES block cipher with key len %d: %w", key.len,
                            err);

    CipherAEAD aes_gcm =
        cipher_new_gcm_with_tag_size(a, block_cipher, (Int)tag_len, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("creating AES-GCM with tag len %d: %w", (Int)tag_len, err);

    if (c->rand_nonce) {
        /* for randNonce tests acvptool appends the nonce to the end of the
         * ciphertext. */
        nonce = slice_sub(ciphertext, ciphertext.len - 12, ciphertext.len);
        ciphertext = slice_sub(ciphertext, 0, ciphertext.len - 12);
    }

    Slice pt = cipher_aead_open(aes_gcm, a, slice_nil(TYPE_BYTE), nonce, ciphertext,
                                additional_data, &err);
    *out = make_args(a, 2);
    out->v[0] = make_bytes(a, 1);
    if (BURROW_FAILED(err))
        return BURROW_NO_ERROR;

    ((Byte *)out->v[0].p)[0] = 1;
    out->v[1] = pt;
    return BURROW_NO_ERROR;
}

/* Not in Go: the AesBlock aes.New would have made for key, in b. */
static Error new_aes_block(Alloc *a, Slice key, AesBlock **b) {
    Error err = BURROW_NO_ERROR;
    CipherBlock block_cipher = aes_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        return err;
    *b = burrow__aes_block_of(block_cipher);
    return BURROW_NO_ERROR;
}

/* Number of output bytes, key, message */
#define CMD_CMAC_AES_AFT() .required_args = 3, .handler = cmd_cmac_aes_aft

static Error cmd_cmac_aes_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                              AcvpArgs *out) {
    (void)c;
    /* safe to truncate to int based on our capabilities describing a max MAC
     * output len of 128 bits. */
    Int output_len = (Int)le32(args.v[0]);
    Slice key = args.v[1];
    Slice message = args.v[2];

    AesBlock *block_cipher = NULL;
    Error err = new_aes_block(a, key, &block_cipher);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("creating AES block cipher with key len %d: %w", key.len,
                            err);

    Cmac *cmac = BURROW_NEW(a, Cmac);
    burrow__cmac_init(cmac, block_cipher);
    Slice tag = make_bytes(a, AES_BLOCK_SIZE);
    burrow__cmac_mac(cmac, tag.p, message);

    if (output_len > tag.len)
        return fmt_errorf_v("invalid output length: expected %d, got %d", output_len,
                            tag.len);

    *out = args1(a, slice_sub(tag, 0, output_len));
    return BURROW_NO_ERROR;
}

/* Key, message, claimed MAC */
#define CMD_CMAC_AES_VERIFY_AFT() .required_args = 3, .handler = cmd_cmac_aes_verify_aft

static Error cmd_cmac_aes_verify_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                     AcvpArgs *out) {
    (void)c;
    Slice key = args.v[0];
    Slice message = args.v[1];
    Slice claimed_mac = args.v[2];

    AesBlock *block_cipher = NULL;
    Error err = new_aes_block(a, key, &block_cipher);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("creating AES block cipher with key len %d: %w", key.len,
                            err);

    Cmac *cmac = BURROW_NEW(a, Cmac);
    burrow__cmac_init(cmac, block_cipher);
    Slice tag = make_bytes(a, AES_BLOCK_SIZE);
    burrow__cmac_mac(cmac, tag.p, message);

    *out = args1(a, make_bytes(a, 1));
    if (subtle_constant_time_compare(slice_sub(tag, 0, claimed_mac.len), claimed_mac) !=
        1)
        return BURROW_NO_ERROR;

    ((Byte *)out->v[0].p)[0] = 1;
    return BURROW_NO_ERROR;
}

#define CMD_TLS_KDF12_AFT(f)                                                           \
    .required_args = 5, /* Number output bytes, secret, label, seed1, seed2 */         \
        .handler = cmd_tls_kdf12_aft, .h = (f)

static Error cmd_tls_kdf12_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                               AcvpArgs *out) {
    uint32_t output_len = le32(args.v[0]);
    Slice secret = args.v[1];
    Str label = bytes_str(args.v[2]);
    Slice seed1 = args.v[3];
    Slice seed2 = args.v[4];

    Slice seed = bytes_clone(a, seed1);
    seed = slice_append(a, seed, seed2.p, seed2.len);
    *out = args1(a, burrow__tls_prf12(a, c->h, secret, label, seed, (Int)output_len));
    return BURROW_NO_ERROR;
}

#define CMD_SSH_KDF_AFT(f, d)                                                          \
    .required_args = 4, /* K, H, SessionID, cipher */ .handler = cmd_ssh_kdf_aft,      \
    .h = (f), .direction = (d)

static Error cmd_ssh_kdf_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                             AcvpArgs *out) {
    Slice k = args.v[0];
    Slice h = args.v[1];
    Slice session_id = args.v[2];
    Str cipher = bytes_str(args.v[3]);

    Int key_len;
    if (str_eq(cipher, BURROW_S("AES-128")))
        key_len = 16;
    else if (str_eq(cipher, BURROW_S("AES-192")))
        key_len = 24;
    else if (str_eq(cipher, BURROW_S("AES-256")))
        key_len = 32;
    else
        return fmt_errorf_v("unsupported cipher: %q", cipher);

    Slice iv_key, enc_key, int_key;
    Error err = BURROW_NO_ERROR;
    burrow__ssh_keys(a, c->h, *c->direction, k, h, session_id, 16, key_len,
                     hash_size(c->h(a)), &iv_key, &enc_key, &int_key, &err);
    if (BURROW_FAILED(err))
        return err;
    *out = make_args(a, 3);
    out->v[0] = iv_key;
    out->v[1] = enc_key;
    out->v[2] = int_key;
    return BURROW_NO_ERROR;
}

/* Not in Go: Go calls the module's ecdh package. Here that is the ecdh_*
 * functions, which are the module's code, and P-224, which crypto/ecdh does
 * not have, comes from ecdh_internal.h. */
#define CMD_ECDH_AFT_VAL(f)                                                            \
    .required_args = 3, /* X, Y, private key (empty for Val type tests) */             \
        .handler = cmd_ecdh_aft_val, .ecdh_curve = (f)

static Error cmd_ecdh_aft_val(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                              AcvpArgs *out) {
    const EcdhCurve *curve = c->ecdh_curve();
    Slice peer_x = args.v[0];
    Slice peer_y = args.v[1];
    Slice raw_sk = args.v[2];

    /* 4 for uncompressed point format */
    Slice uncompressed_pk = make_bytes(a, 1 + peer_x.len + peer_y.len);
    ((Byte *)uncompressed_pk.p)[0] = 4;
    if (peer_x.len > 0)
        memcpy((Byte *)uncompressed_pk.p + 1, peer_x.p, (size_t)peer_x.len);
    if (peer_y.len > 0)
        memcpy((Byte *)uncompressed_pk.p + 1 + peer_x.len, peer_y.p,
               (size_t)peer_y.len);
    Error err = BURROW_NO_ERROR;
    EcdhPublicKey *pk = ecdh_curve_new_public_key(curve, a, uncompressed_pk, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("invalid peer public key x,y: %v", err);

    EcdhPrivateKey *sk;
    if (raw_sk.len > 0)
        sk = ecdh_curve_new_private_key(curve, a, raw_sk, &err);
    else
        sk = ecdh_curve_generate_key(curve, a, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("private key error: %v", err);

    Slice pub_bytes = ecdh_public_key_bytes(ecdh_private_key_public_key(sk), a);
    Int coord_len = (pub_bytes.len - 1) / 2;
    Slice x = slice_sub(pub_bytes, 1, 1 + coord_len);
    Slice y = slice_sub(pub_bytes, 1 + coord_len, pub_bytes.len);

    Slice secret = ecdh_private_key_ecdh(sk, a, pk, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("key agreement failed: %v", err);

    *out = make_args(a, 3);
    out->v[0] = x;
    out->v[1] = y;
    out->v[2] = secret;
    return BURROW_NO_ERROR;
}

#define CMD_HMAC_DRBG_AFT(f)                                                           \
    .required_args = 6, /* Output length, entropy, personalization, ad1, ad2, nonce */ \
        .handler = cmd_hmac_drbg_aft, .h = (f)

static Error cmd_hmac_drbg_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                               AcvpArgs *out) {
    uint32_t out_len = le32(args.v[0]);
    Slice entropy = args.v[1];
    Slice personalization = args.v[2];
    Slice ad1 = args.v[3];
    Slice ad2 = args.v[4];
    Slice nonce = args.v[5];

    /* Our capabilities describe no additional data support. */
    if (ad1.len != 0 || ad2.len != 0)
        return errors_new(error_allocator(), BURROW_S("additional data not supported"));

    /* Our capabilities describe no prediction resistance (requires reseed) and
     * no reseed. So the test procedure is:
     *   * Instantiate DRBG
     *   * Generate but don't output
     *   * Generate output
     *   * Uninstantiate
     * See Table 7 in draft-vassilev-acvp-drbg */
    Slice result = make_bytes(a, (Int)out_len);
    EcdsaDrbg *drbg = burrow__ecdsa_new_drbg(a, c->h, entropy, nonce, personalization);
    burrow__ecdsa_drbg_generate(drbg, result);
    burrow__ecdsa_drbg_generate(drbg, result);

    *out = args1(a, result);
    return BURROW_NO_ERROR;
}

typedef struct AcvpCtrDrbg {
    uint32_t out_len;
    Slice entropy;
    Slice personalization;
    Slice ad1;
    Slice ad2;
    Slice nonce;
    Slice reseed_ad;      /* May be empty for no reseed */
    Slice reseed_entropy; /* May be empty for no reseed */
} AcvpCtrDrbg;

static Error require48_bytes(Slice input, const Byte **out) {
    *out = NULL;
    Int input_len = input.len;
    if (input_len != 48)
        return fmt_errorf_v("invalid length: %d", input_len);
    *out = input.p;
    return BURROW_NO_ERROR;
}

static Error acvp_ctr_drbg_process(Alloc *a, AcvpCtrDrbg args, AcvpArgs *out) {
    /* Our capability describes no personalization support. */
    if (args.personalization.len > 0)
        return errors_new(error_allocator(),
                          BURROW_S("personalization string not supported"));

    /* Our capability describes no derivation function support, so the nonce
     * should be empty. */
    if (args.nonce.len > 0)
        return errors_new(error_allocator(), BURROW_S("unexpected nonce value"));

    /* Our capability describes entropy input len of 384 bits. */
    const Byte *entropy = NULL;
    Error err = require48_bytes(args.entropy, &entropy);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("entropy: %w", err);

    /* Our capability describes additional input len of 384 bits. */
    const Byte *ad1 = NULL;
    err = require48_bytes(args.ad1, &ad1);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("AD1: %w", err);
    const Byte *ad2 = NULL;
    err = require48_bytes(args.ad2, &ad2);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("AD2: %w", err);

    bool with_reseed = args.reseed_ad.len > 0;
    const Byte *reseed_ad = NULL;
    const Byte *reseed_entropy = NULL;
    if (with_reseed) {
        /* Ditto RE: entropy and additional data lengths for reseeding. */
        err = require48_bytes(args.reseed_ad, &reseed_ad);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("reseed AD: %w", err);
        err = require48_bytes(args.reseed_entropy, &reseed_entropy);
        if (BURROW_FAILED(err))
            return fmt_errorf_v("reseed entropy: %w", err);
    }

    /* Our capabilities describe no prediction resistance and allow both
     * reseed and no reseed, so the test procedure is:
     *   * Instantiate DRBG
     *   * Reseed (if enabled)
     *   * Generate but don't output
     *   * Generate output
     *   * Uninstantiate
     * See Table 7 in draft-vassilev-acvp-drbg */
    Slice result = make_bytes(a, (Int)args.out_len);
    DrbgCounter *ctr_drbg = BURROW_NEW(a, DrbgCounter);
    burrow__drbg_new_counter(ctr_drbg, entropy);
    if (with_reseed)
        burrow__drbg_counter_reseed(ctr_drbg, reseed_entropy, reseed_ad);
    burrow__drbg_counter_generate(ctr_drbg, result, ad1);
    burrow__drbg_counter_generate(ctr_drbg, result, ad2);

    *out = args1(a, result);
    return BURROW_NO_ERROR;
}

#define CMD_CTR_DRBG_AFT()                                                             \
    .required_args = 6, /* Output length, entropy, personalization, ad1, ad2, nonce */ \
        .handler = cmd_ctr_drbg_aft

static Error cmd_ctr_drbg_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                              AcvpArgs *out) {
    (void)c;
    AcvpCtrDrbg d = {
        .out_len = le32(args.v[0]),
        .entropy = args.v[1],
        .personalization = args.v[2],
        .ad1 = args.v[3],
        .ad2 = args.v[4],
        .nonce = args.v[5],
    };
    return acvp_ctr_drbg_process(a, d, out);
}

#define CMD_CTR_DRBG_RESEED_AFT()                                                      \
    .required_args =                                                                   \
        8, /* Output length, entropy, personalization, reseedAD,          \
                           reseedEntropy, ad1, ad2, nonce */            \
        .handler = cmd_ctr_drbg_reseed_aft

static Error cmd_ctr_drbg_reseed_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                     AcvpArgs *out) {
    (void)c;
    AcvpCtrDrbg d = {
        .out_len = le32(args.v[0]),
        .entropy = args.v[1],
        .personalization = args.v[2],
        .reseed_ad = args.v[3],
        .reseed_entropy = args.v[4],
        .ad1 = args.v[5],
        .ad2 = args.v[6],
        .nonce = args.v[7],
    };
    return acvp_ctr_drbg_process(a, d, out);
}

#define CMD_KDF_COUNTER_AFT()                                                          \
    .required_args =                                                                   \
        5, /* Number output bytes, PRF name, counter location string,     \
                           key, number of counter bits */            \
        .handler = cmd_kdf_counter_aft

static Error cmd_kdf_counter_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                 AcvpArgs *out) {
    (void)c;
    uint32_t output_bytes = le32(args.v[0]);
    Str prf = bytes_str(args.v[1]);
    Str counter_location = bytes_str(args.v[2]);
    Slice key = args.v[3];
    uint32_t counter_bits = le32(args.v[4]);

    if (output_bytes != 32)
        return fmt_errorf_v("KDF received unsupported output length %d bytes",
                            (Int)output_bytes);
    if (!str_eq(prf, BURROW_S("CMAC-AES128")) &&
        !str_eq(prf, BURROW_S("CMAC-AES192")) && !str_eq(prf, BURROW_S("CMAC-AES256")))
        return fmt_errorf_v("KDF received unsupported PRF %q", prf);
    if (!str_eq(counter_location, BURROW_S("before fixed data")))
        return fmt_errorf_v("KDF received unsupported counter location %q",
                            counter_location);
    /* The spec doesn't describe the "deferred" property for a KDF counterMode
     * test case. BoringSSL's acvptool sends an empty key when deferred=true,
     * but with the capabilities we register all test cases have deferred=false
     * and provide a key from the populated keyIn property. */
    if (key.len == 0)
        return errors_new(error_allocator(),
                          BURROW_S("deferred test cases are not supported"));
    if (counter_bits != 16)
        return fmt_errorf_v("KDF received unsupported counter length %d",
                            (Int)counter_bits);

    AesBlock *block = NULL;
    Error err = new_aes_block(a, key, &block);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("failed to create cipher: %v", err);
    CounterKdf *kdf = BURROW_NEW(a, CounterKdf);
    burrow__counter_kdf_init(kdf, block);

    Byte label = 0;
    Byte context[12] = {0};
    crypto_rand_read(slice_from(context, 12, 12, TYPE_BYTE), NULL);

    Slice result = make_bytes(a, 32);
    burrow__counter_kdf_derive_key(kdf, result.p, label, context);

    /* 1 byte label, 1 null byte, 12 bytes context. */
    Slice fixed_data = make_bytes(a, 1 + 1 + 12);
    ((Byte *)fixed_data.p)[0] = label;
    memcpy((Byte *)fixed_data.p + 2, context, sizeof context);

    *out = make_args(a, 3);
    out->v[0] = key;
    out->v[1] = fixed_data;
    out->v[2] = result;
    return BURROW_NO_ERROR;
}

#define CMD_KDF_FEEDBACK_AFT()                                                         \
    .required_args =                                                                   \
        5, /* Number output bytes, PRF name, counter location string,     \
                           key, number of counter bits, IV */            \
        .handler = cmd_kdf_feedback_aft

static Error cmd_kdf_feedback_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                  AcvpArgs *out) {
    (void)c;
    /* The max supported output len for the KDF algorithm type is 4096 bits,
     * making an int cast here safe.
     * See https://pages.nist.gov/ACVP/draft-celi-acvp-kbkdf.html#section-7.3.2 */
    Int output_bytes = (Int)le32(args.v[0]);
    Str prf = bytes_str(args.v[1]);
    Str counter_location = bytes_str(args.v[2]);
    Slice key = args.v[3];
    uint32_t counter_bits = le32(args.v[4]);

    if (!strings_has_prefix(prf, BURROW_S("HMAC-")))
        return fmt_errorf_v("feedback KDF received unsupported PRF %q", prf);
    prf = str_from_bytes(prf.p + 5, prf.len - 5);

    HashNewFunc h;
    Error err = lookup_hash(prf, &h);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("feedback KDF received unsupported PRF %q: %w", prf, err);

    if (!str_eq(counter_location, BURROW_S("after fixed data")))
        return fmt_errorf_v("feedback KDF received unsupported counter location %q",
                            counter_location);

    /* The spec doesn't describe the "deferred" property for a KDF counterMode
     * test case. BoringSSL's acvptool sends an empty key when deferred=true,
     * but with the capabilities we register all test cases have deferred=false
     * and provide a key from the populated keyIn property. */
    if (key.len == 0)
        return errors_new(error_allocator(),
                          BURROW_S("deferred test cases are not supported"));

    if (counter_bits != 8)
        return fmt_errorf_v("feedback KDF received unsupported counter length %d",
                            (Int)counter_bits);

    Byte context[12] = {0};
    crypto_rand_read(slice_from(context, 12, 12, TYPE_BYTE), NULL);
    /* 1 byte label (we pick null), 1 null byte, 12 bytes context. */
    Slice fixed_data = make_bytes(a, 1 + 1 + 12);
    memcpy((Byte *)fixed_data.p + 2, context, sizeof context);

    Slice result =
        burrow__hkdf_expand(a, h, key, bytes_str(fixed_data), output_bytes, &err);
    /* Not in Go: the one error here is running out of memory. */
    if (BURROW_FAILED(err))
        return err;

    *out = make_args(a, 3);
    out->v[0] = key;
    out->v[1] = fixed_data;
    out->v[2] = result;
    return BURROW_NO_ERROR;
}

/* Not in Go: Go makes paddedE with copy(paddedE[4-len(eBytes):], eBytes),
 * which panics for more than 4 bytes, and this returns an error instead. */
static Error padded_e(Alloc *a, Slice e_bytes, Int *e) {
    if (e_bytes.len > 4)
        return fmt_errorf_v("invalid RSA public exponent length: %d", e_bytes.len);
    Slice padded = make_bytes(a, 4);
    memcpy((Byte *)padded.p + 4 - e_bytes.len, e_bytes.p, (size_t)e_bytes.len);
    *e = (Int)binary_big_endian_uint32(padded);
    return BURROW_NO_ERROR;
}

static Error get_rsa_key(Int bits, const RsaFipsKey **out);

#define CMD_RSA_KEY_GEN_AFT()                                                          \
    .required_args = 1 /* Modulus bit-size */, .handler = cmd_rsa_key_gen_aft

static Error cmd_rsa_key_gen_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                 AcvpArgs *out) {
    (void)c;
    uint32_t bit_size = le32(args.v[0]);

    const RsaFipsKey *key;
    Error err = get_rsa_key((Int)bit_size, &key);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating RSA key: %w", err);

    RsaFipsExport x = burrow__rsa_fips_export(key, a);

    Slice e_bytes = make_bytes(a, 4);
    binary_big_endian_put_uint32(e_bytes, (uint32_t)x.e);

    *out = make_args(a, 5);
    out->v[0] = e_bytes;
    out->v[1] = x.p;
    out->v[2] = x.q;
    out->v[3] = x.n;
    out->v[4] = x.d;
    return BURROW_NO_ERROR;
}

#define CMD_RSA_SIG_GEN_AFT(f, name, p)                                                \
    .required_args = 2, /* Modulus bit-size, message */                                \
        .handler = cmd_rsa_sig_gen_aft, .h = (f), .hash_name = (name), .pss = (p)

static Error cmd_rsa_sig_gen_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                 AcvpArgs *out) {
    uint32_t bit_size = le32(args.v[0]);
    Slice msg = args.v[1];

    const RsaFipsKey *key;
    Error err = get_rsa_key((Int)bit_size, &key);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating RSA key: %w", err);

    Hash h = c->h(a);
    hash_write(h, msg, NULL);
    Slice digest = hash_sum(a, h, make_bytes(a, 0));

    Slice sig;
    if (!c->pss) {
        if (!burrow__rsa_fips_sign_pkcs1_v15(key, str_from_cstr(c->hash_name), digest,
                                             a, &sig, &err))
            return fmt_errorf_v("signing RSA message: %w", err);
    } else {
        if (!burrow__rsa_fips_sign_pss(crypto_rand_reader, key, c->h(a), digest,
                                       hash_size(h), a, &sig, &err))
            return fmt_errorf_v("signing RSA message: %w", err);
    }

    RsaFipsExport x = burrow__rsa_fips_export(key, a);
    Slice e_bytes = make_bytes(a, 4);
    binary_big_endian_put_uint32(e_bytes, (uint32_t)x.e);

    *out = make_args(a, 3);
    out->v[0] = x.n;
    out->v[1] = e_bytes;
    out->v[2] = sig;
    return BURROW_NO_ERROR;
}

#define CMD_RSA_SIG_VER_AFT(f, name, p)                                                \
    .required_args = 4, /* n, e, message, signature */                                 \
        .handler = cmd_rsa_sig_ver_aft, .h = (f), .hash_name = (name), .pss = (p)

static Error cmd_rsa_sig_ver_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                 AcvpArgs *out) {
    Slice n_bytes = args.v[0];
    Slice e_bytes = args.v[1];
    Slice msg = args.v[2];
    Slice sig = args.v[3];

    Int e;
    Error err = padded_e(a, e_bytes, &e);
    if (BURROW_FAILED(err))
        return err;

    /* Not in Go: the module's functions take the public key as n and e,
     * which Go puts in an rsa.PublicKey. */
    BigmodModulus *n = bigmod_new_modulus(a, n_bytes, &err);
    if (n == NULL)
        return fmt_errorf_v("invalid RSA modulus: %w", err);

    Hash h = c->h(a);
    hash_write(h, msg, NULL);
    Slice digest = hash_sum(a, h, make_bytes(a, 0));

    if (!c->pss)
        err = burrow__rsa_fips_verify_pkcs1_v15(n, e, str_from_cstr(c->hash_name),
                                                digest, sig, a);
    else
        err = burrow__rsa_fips_verify_pss(n, e, c->h(a), digest, sig, a);
    if (BURROW_FAILED(err)) {
        *out = byte_args(a, 0);
        return BURROW_NO_ERROR;
    }

    *out = byte_args(a, 1);
    return BURROW_NO_ERROR;
}

/* rsaKeyCache caches generated keys by modulus bit-size.
 *
 * Not in Go: a list rather than a map. The keys and the list are from the
 * heap and are kept until the program exits, as Go's are. */
typedef struct RsaKeyCacheEntry {
    Int bits;
    RsaFipsKey *key;
    struct RsaKeyCacheEntry *next;
} RsaKeyCacheEntry;

static RsaKeyCacheEntry *rsa_key_cache;

/* getRSAKey returns a cached RSA private key with the specified modulus
 * bit-size or generates one if necessary. */
static Error get_rsa_key(Int bits, const RsaFipsKey **out) {
    for (RsaKeyCacheEntry *k = rsa_key_cache; k != NULL; k = k->next) {
        if (k->bits == bits) {
            *out = k->key;
            return BURROW_NO_ERROR;
        }
    }

    Error err = BURROW_NO_ERROR;
    RsaFipsKey *key =
        burrow__rsa_fips_generate_key(heap_allocator(), crypto_rand_reader, bits, &err);
    if (key == NULL)
        return err;

    RsaKeyCacheEntry *k = BURROW_NEW(heap_allocator(), RsaKeyCacheEntry);
    if (k == NULL) {
        burrow__rsa_fips_key_free(key);
        return burrow_err_out_of_memory;
    }
    k->bits = bits;
    k->key = key;
    k->next = rsa_key_cache;
    rsa_key_cache = k;
    *out = key;
    return BURROW_NO_ERROR;
}

#define CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(f)                                            \
    .required_args = 4, /* key, info, salt, outBytes */                                \
        .handler = cmd_one_step_no_counter_hmac_aft, .h = (f)

static Error cmd_one_step_no_counter_hmac_aft(Alloc *a, const AcvpCommand *c,
                                              AcvpArgs args, AcvpArgs *out) {
    Slice key = args.v[0];
    Slice info = args.v[1];
    Slice salt = args.v[2];
    uint32_t out_bytes = le32(args.v[3]);

    Hash mac = burrow__hmac_new(a, c->h, salt);

    if (out_bytes != (uint32_t)hash_size(mac))
        return fmt_errorf_v("invalid output length: got %d, want %d", (Int)out_bytes,
                            hash_size(mac));

    Slice data = slice_make(a, TYPE_BYTE, 0, key.len + info.len);
    data = slice_append(a, data, key.p, key.len);
    data = slice_append(a, data, info.p, info.len);

    hash_write(mac, data, NULL);
    *out = args1(a, hash_sum(a, mac, make_bytes(a, 0)));
    return BURROW_NO_ERROR;
}

#define CMD_KTS_IFC_INITIATOR_AFT(f)                                                   \
    .required_args = 3, /* output bytes, n bytes, e bytes */                           \
        .handler = cmd_kts_ifc_initiator_aft, .h = (f)

static Error cmd_kts_ifc_initiator_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                       AcvpArgs *out) {
    uint32_t output_bytes = le32(args.v[0]);
    Slice n_bytes = args.v[1];
    Slice e_bytes = args.v[2];

    Error err = BURROW_NO_ERROR;
    BigmodModulus *n = bigmod_new_modulus(a, n_bytes, &err);
    if (n == NULL)
        return fmt_errorf_v("invalid RSA modulus: %w", err);

    Int e;
    err = padded_e(a, e_bytes, &e);
    if (BURROW_FAILED(err))
        return err;
    if (e != 0x10001)
        return errors_new(error_allocator(), BURROW_S("e must be 0x10001"));

    Slice dkm = make_bytes(a, (Int)output_bytes);
    crypto_rand_read(dkm, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("failed to generate random DKM: %v", err);

    Slice iut_c;
    if (!burrow__rsa_fips_encrypt_oaep(c->h(a), c->h(a), crypto_rand_reader, n, e, dkm,
                                       slice_nil(TYPE_BYTE), a, &iut_c, &err))
        return fmt_errorf_v("OAEP encryption failed: %v", err);

    *out = make_args(a, 2);
    out->v[0] = iut_c;
    out->v[1] = dkm;
    return BURROW_NO_ERROR;
}

#define CMD_KTS_IFC_RESPONDER_AFT(f)                                                   \
    .required_args = 6, /* n bytes, e bytes, p bytes, q bytes, d bytes, c bytes */     \
        .handler = cmd_kts_ifc_responder_aft, .h = (f)

static Error cmd_kts_ifc_responder_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                       AcvpArgs *out) {
    Slice n_bytes = args.v[0];
    Slice e_bytes = args.v[1];

    Slice p_bytes = args.v[2];
    Slice q_bytes = args.v[3];
    Slice d_bytes = args.v[4];

    Slice c_bytes = args.v[5];

    Int e;
    Error err = padded_e(a, e_bytes, &e);
    if (BURROW_FAILED(err))
        return err;
    if (e != 0x10001)
        return errors_new(error_allocator(), BURROW_S("e must be 0x10001"));

    RsaFipsKey *priv = burrow__rsa_fips_new_private_key(a, a, n_bytes, e, d_bytes,
                                                        p_bytes, q_bytes, &err);
    if (priv == NULL)
        return fmt_errorf_v("failed to create private key: %v", err);

    Slice dkm;
    bool ok = burrow__rsa_fips_decrypt_oaep(c->h(a), c->h(a), priv, c_bytes,
                                            slice_nil(TYPE_BYTE), a, &dkm, &err);
    burrow__rsa_fips_key_free(priv);
    if (!ok)
        return fmt_errorf_v("OAEP decryption failed: %v", err);

    *out = args1(a, dkm);
    return BURROW_NO_ERROR;
}

/* From acvp_fips140v1.28_test.go. */
#define CMD_RSA_KEY_GEN_CRT_AFT()                                                      \
    .required_args = 1 /* Modulus bit-size */, .handler = cmd_rsa_key_gen_crt_aft

static Error cmd_rsa_key_gen_crt_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                     AcvpArgs *out) {
    (void)c;
    uint32_t bit_size = le32(args.v[0]);

    const RsaFipsKey *key;
    Error err = get_rsa_key((Int)bit_size, &key);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating RSA key: %w", err);

    RsaFipsExport x = burrow__rsa_fips_export(key, a);

    Slice e_bytes = make_bytes(a, 4);
    binary_big_endian_put_uint32(e_bytes, (uint32_t)x.e);

    *out = make_args(a, 8);
    out->v[0] = e_bytes;
    out->v[1] = x.p;
    out->v[2] = x.q;
    out->v[3] = x.n;
    out->v[4] = x.d;
    out->v[5] = x.dp;
    out->v[6] = x.dq;
    out->v[7] = x.qinv;
    return BURROW_NO_ERROR;
}

#define CMD_RSA_SIG_GEN_SALT_LEN_AFT(f, name, p)                                       \
    .required_args = 3, /* Modulus bit-size, message, saltLen */                       \
        .handler = cmd_rsa_sig_gen_salt_len_aft, .h = (f), .hash_name = (name),        \
    .pss = (p)

static Error cmd_rsa_sig_gen_salt_len_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                          AcvpArgs *out) {
    uint32_t bit_size = le32(args.v[0]);
    Slice msg = args.v[1];
    uint32_t salt_len = le32(args.v[2]);

    const RsaFipsKey *key;
    Error err = get_rsa_key((Int)bit_size, &key);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating RSA key: %w", err);

    Hash h = c->h(a);
    hash_write(h, msg, NULL);
    Slice digest = hash_sum(a, h, make_bytes(a, 0));

    Slice sig;
    if (!c->pss) {
        if (!burrow__rsa_fips_sign_pkcs1_v15(key, str_from_cstr(c->hash_name), digest,
                                             a, &sig, &err))
            return fmt_errorf_v("signing RSA message: %w", err);
    } else {
        if (!burrow__rsa_fips_sign_pss(crypto_rand_reader, key, c->h(a), digest,
                                       (Int)salt_len, a, &sig, &err))
            return fmt_errorf_v("signing RSA message: %w", err);
    }

    RsaFipsExport x = burrow__rsa_fips_export(key, a);
    Slice e_bytes = make_bytes(a, 4);
    binary_big_endian_put_uint32(e_bytes, (uint32_t)x.e);

    *out = make_args(a, 3);
    out->v[0] = x.n;
    out->v[1] = e_bytes;
    out->v[2] = sig;
    return BURROW_NO_ERROR;
}

#define CMD_RSA_SIG_VER_RAND_EXP_AFT(f, name, p)                                       \
    .required_args = 4, /* n, e, message, signature */                                 \
        .handler = cmd_rsa_sig_ver_rand_exp_aft, .h = (f), .hash_name = (name),        \
    .pss = (p)

static Error cmd_rsa_sig_ver_rand_exp_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                          AcvpArgs *out) {
    Slice n_bytes = args.v[0];
    Slice e_bytes = args.v[1];
    Slice msg = args.v[2];
    Slice sig = args.v[3];

    Error err = BURROW_NO_ERROR;
    BigmodModulus *n = bigmod_new_modulus(a, n_bytes, &err);
    if (n == NULL)
        return fmt_errorf_v("invalid RSA modulus: %w", err);

    Hash h = c->h(a);
    hash_write(h, msg, NULL);
    Slice digest = hash_sum(a, h, make_bytes(a, 0));

    const Byte *eb = e_bytes.p;
    if (e_bytes.len < 32 / 8 || (e_bytes.len == 32 / 8 && eb[0] < 0x80)) {
        Slice padded_e_bytes = make_bytes(a, 4);
        memcpy((Byte *)padded_e_bytes.p + 4 - e_bytes.len, e_bytes.p,
               (size_t)e_bytes.len);
        Int e = (Int)binary_big_endian_uint32(padded_e_bytes);

        /* Not in Go: the module's functions take the public key as n and e,
         * which Go puts in an rsa.PublicKey. */
        if (!c->pss)
            err = burrow__rsa_fips_verify_pkcs1_v15(n, e, str_from_cstr(c->hash_name),
                                                    digest, sig, a);
        else
            err = burrow__rsa_fips_verify_pss(n, e, c->h(a), digest, sig, a);
    } else {
        RsaFipsLargeExponentPublicKey pub = {n, e_bytes};

        if (!c->pss)
            err = burrow__rsa_fips_testing_only_large_exponent_verify_pkcs1_v15(
                &pub, str_from_cstr(c->hash_name), digest, sig, a);
        else
            err = burrow__rsa_fips_testing_only_large_exponent_verify_pss(
                &pub, c->h(a), digest, sig, a);
    }

    if (BURROW_FAILED(err)) {
        *out = byte_args(a, 0);
        return BURROW_NO_ERROR;
    }

    *out = byte_args(a, 1);
    return BURROW_NO_ERROR;
}

/* required_args: n bytes, e bytes, p bytes, q bytes, dmp1 bytes, dmq1 bytes,
 * iqmp bytes, c bytes */
#define CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(f)                                      \
    .required_args = 8, .handler = cmd_kts_ifc_responder_rand_exp_crt_aft, .h = (f)

static Error cmd_kts_ifc_responder_rand_exp_crt_aft(Alloc *a, const AcvpCommand *c,
                                                    AcvpArgs args, AcvpArgs *out) {
    Slice n_bytes = args.v[0];
    Slice e_bytes = args.v[1];

    Slice p_bytes = args.v[2];
    Slice q_bytes = args.v[3];

    Slice dmp1_bytes = args.v[4];
    Slice dmq1_bytes = args.v[5];
    Slice iqmp_bytes = args.v[6];

    Slice c_bytes = args.v[7];

    /* We need to compute 'd' from the CRT values for
     * NewPrivateKeyWithPrecomputation to check consistency. */
    BigInt p = BIG_INT(a), q = BIG_INT(a);
    big_int_set_bytes(&p, p_bytes);
    big_int_set_bytes(&q, q_bytes);
    BigInt dP = BIG_INT(a), dQ = BIG_INT(a);
    big_int_set_bytes(&dP, dmp1_bytes);
    big_int_set_bytes(&dQ, dmq1_bytes);
    BigInt p_minus1 = BIG_INT(a), q_minus1 = BIG_INT(a);
    big_int_sub(&p_minus1, &p, big_new_int(a, 1));
    big_int_sub(&q_minus1, &q, big_new_int(a, 1));
    BigInt x = BIG_INT(a), y = BIG_INT(a);
    BigInt gcd = BIG_INT(a);
    big_int_gcd(&gcd, &x, &y, &p_minus1, &q_minus1);
    BigInt diff = BIG_INT(a);
    big_int_sub(&diff, &dQ, &dP);
    BigInt r = BIG_INT(a);
    if (big_int_sign(big_int_mod(&r, &diff, &gcd)) != 0)
        return errors_new(error_allocator(), BURROW_S("inconsistent CRT parameters"));
    BigInt lcm = BIG_INT(a);
    big_int_mul(&lcm, &p_minus1, &q_minus1);
    big_int_div(&lcm, &lcm, &gcd);
    BigInt d = BIG_INT(a);
    big_int_mul(&d, &x, &p_minus1);
    big_int_div(&d, big_int_mul(&d, &d, &diff), &gcd);
    big_int_mod(&d, big_int_add(&d, &d, &dP), &lcm);
    Slice d_bytes = big_int_bytes(&d, a);

    Slice dkm = slice_nil(TYPE_BYTE);
    Error err = BURROW_NO_ERROR;

    /* Not in Go: in Go, each branch declares an err of its own with :=, and
     * the error of the decryption goes to that one, not to the err checked
     * after the branches. So a decryption that fails gives an empty dkm and no
     * error, and branch_err does the same here. */
    const Byte *eb = e_bytes.p;
    if (e_bytes.len < 32 / 8 || (e_bytes.len == 32 / 8 && eb[0] < 0x80)) {
        /* Exponent fits in an int32, use standard PrivateKey. */
        Slice padded_e_bytes = make_bytes(a, 4);
        memcpy((Byte *)padded_e_bytes.p + 4 - e_bytes.len, e_bytes.p,
               (size_t)e_bytes.len);
        Int e = (Int)binary_big_endian_uint32(padded_e_bytes);

        Error branch_err = BURROW_NO_ERROR;
        RsaFipsKey *priv = burrow__rsa_fips_new_private_key_with_precomputation(
            a, a, n_bytes, e, d_bytes, p_bytes, q_bytes, dmp1_bytes, dmq1_bytes,
            iqmp_bytes, &branch_err);
        if (priv == NULL)
            return fmt_errorf_v("failed to create private key: %v", branch_err);

        if (!burrow__rsa_fips_decrypt_oaep(c->h(a), c->h(a), priv, c_bytes,
                                           slice_nil(TYPE_BYTE), a, &dkm, &branch_err))
            dkm = slice_nil(TYPE_BYTE);
        burrow__rsa_fips_key_free(priv);
    } else {
        /* Large exponent, use TestingOnlyLargeExponentPrivateKey. */
        Error branch_err = BURROW_NO_ERROR;
        RsaFipsLargeExponentKey *priv =
            burrow__rsa_fips_testing_only_new_large_exponent_private_key_with_precomputation(
                a, a, n_bytes, e_bytes, d_bytes, p_bytes, q_bytes, dmp1_bytes,
                dmq1_bytes, iqmp_bytes, &branch_err);
        if (priv == NULL)
            return fmt_errorf_v("failed to create private key: %v", branch_err);

        if (!burrow__rsa_fips_testing_only_large_exponent_decrypt_oaep(
                c->h(a), c->h(a), priv, c_bytes, slice_nil(TYPE_BYTE), a, &dkm,
                &branch_err))
            dkm = slice_nil(TYPE_BYTE);
        burrow__rsa_fips_large_exponent_key_free(priv);
    }
    if (BURROW_FAILED(err))
        return fmt_errorf_v("OAEP decryption failed: %v", err);

    *out = args1(a, dkm);
    return BURROW_NO_ERROR;
}

/* From acvp_mldsa_test.go. Not in Go: Go calls the module's mldsa package.
 * The mldsa_* functions are the module's code, and what only the module has
 * comes from mldsa_internal.h. */
#define CMD_ML_DSA_KEY_GEN_AFT(f)                                                      \
    .required_args = 1 /* Seed */, .handler = cmd_ml_dsa_key_gen_aft, .mldsa = (f)

static Error cmd_ml_dsa_key_gen_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                    AcvpArgs *out) {
    Slice seed = args.v[0];

    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = mldsa_new_private_key(a, c->mldsa(), seed, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("generating ML-DSA 44 private key: %w", err);

    /* Important: we must return the full encoding of sk, not the seed. */
    *out = make_args(a, 2);
    out->v[0] = mldsa_public_key_bytes(mldsa_private_key_public_key(sk), a);
    out->v[1] = burrow__mldsa_testing_only_private_key_semi_expanded_bytes(sk, a);
    return BURROW_NO_ERROR;
}

#define CMD_ML_DSA_SIG_GEN_AFT()                                                       \
    .required_args = 5, /* secret key, message, randomizer, mu, context */             \
        .handler = cmd_ml_dsa_sig_gen_aft

static Error cmd_ml_dsa_sig_gen_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                    AcvpArgs *out) {
    (void)c;
    Slice sk_smi_expanded = args.v[0];
    Slice message = args.v[1];          /* Optional, exclusive with mu */
    Slice randomizer = args.v[2];       /* Optional */
    Str context = bytes_str(args.v[3]); /* Optional */
    Slice mu = args.v[4];               /* Optional, exclusive with message */

    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = burrow__mldsa_testing_only_new_private_key_from_semi_expanded(
        a, sk_smi_expanded, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("making ML-DSA private key from semi-expanded form: %w",
                            err);

    bool have_message = message.len != 0;
    bool have_randomizer = randomizer.len != 0;
    bool have_mu = mu.len != 0;

    Slice sig;
    if (have_message && !have_randomizer && !have_mu) {
        MldsaOptions opts = {context};
        sig = mldsa_private_key_sign_deterministic(
            sk, a, message, mldsa_options_as_signer_opts(&opts), &err);
    } else if (have_message && have_randomizer && !have_mu) {
        sig = burrow__mldsa_testing_only_sign_with_random(sk, a, message, context,
                                                          randomizer, &err);
    } else if (!have_message && !have_randomizer && have_mu) {
        sig = burrow__mldsa_sign_external_mu_deterministic(sk, a, mu, NULL, &err);
    } else if (!have_message && have_randomizer && have_mu) {
        sig = burrow__mldsa_testing_only_sign_external_mu_with_random(sk, a, mu,
                                                                      randomizer, &err);
    } else {
        bool have_context = context.len != 0;
        return fmt_errorf_v("unsupported ML-DSA sigGen args: have message=%v have "
                            "randomizer=%v haveMu=%v haveContext=%v",
                            have_message, have_randomizer, have_mu, have_context);
    }

    if (BURROW_FAILED(err))
        return fmt_errorf_v("creating deterministic ML-DSA signature: %w", err);

    *out = args1(a, sig);
    return BURROW_NO_ERROR;
}

#define CMD_ML_DSA_SIG_VER_AFT(f)                                                      \
    .required_args = 5, /* public key, message, signature, context, mu */              \
        .handler = cmd_ml_dsa_sig_ver_aft, .mldsa = (f)

static Error cmd_ml_dsa_sig_ver_aft(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                                    AcvpArgs *out) {
    Slice pk_raw = args.v[0];
    Slice message = args.v[1]; /* Optional, exclusive with mu */
    Slice signature = args.v[2];
    Str context = bytes_str(args.v[3]); /* Optional */
    Slice mu = args.v[4];               /* Optional, exclusive with message */

    Error err = BURROW_NO_ERROR;
    MldsaPublicKey *pk = mldsa_new_public_key(a, c->mldsa(), pk_raw, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("loading ML-DSA public key: %w", err);

    bool have_message = message.len != 0;
    bool have_mu = mu.len != 0;
    if (have_message && !have_mu) {
        MldsaOptions opts = {context};
        err = mldsa_verify(pk, message, signature, &opts);
    } else if (!have_message && have_mu) {
        err = burrow__mldsa_verify_external_mu(pk, mu, signature);
    } else {
        bool have_context = context.len != 0;
        return fmt_errorf_v(
            "unsupported ML-DSA sigVer args: have message=%v haveMu=%v haveContext=%v",
            have_message, have_mu, have_context);
    }

    if (BURROW_FAILED(err)) {
        *out = byte_args(a, 0);
        return BURROW_NO_ERROR;
    }

    *out = byte_args(a, 1);
    return BURROW_NO_ERROR;
}

/* commands should reflect what config says we support. E.g. adding a command
 * here will be a NOP unless the configuration/acvp_capabilities.json indicates
 * the command's associated algorithm is supported. */
static const AcvpCommand commands[] = {
    {.name = "getConfig", .handler = cmd_get_config},

    {.name = "SHA2-224", CMD_HASH_AFT(sha256_new224)},
    {.name = "SHA2-224/MCT", CMD_HASH_MCT(sha256_new224)},
    {.name = "SHA2-256", CMD_HASH_AFT(sha256_new)},
    {.name = "SHA2-256/MCT", CMD_HASH_MCT(sha256_new)},
    {.name = "SHA2-384", CMD_HASH_AFT(sha512_new384)},
    {.name = "SHA2-384/MCT", CMD_HASH_MCT(sha512_new384)},
    {.name = "SHA2-512", CMD_HASH_AFT(sha512_new)},
    {.name = "SHA2-512/MCT", CMD_HASH_MCT(sha512_new)},
    {.name = "SHA2-512/224", CMD_HASH_AFT(sha512_new512224)},
    {.name = "SHA2-512/224/MCT", CMD_HASH_MCT(sha512_new512224)},
    {.name = "SHA2-512/256", CMD_HASH_AFT(sha512_new512256)},
    {.name = "SHA2-512/256/MCT", CMD_HASH_MCT(sha512_new512256)},

    {.name = "SHA3-256", CMD_HASH_AFT(new_sha3_256)},
    {.name = "SHA3-256/MCT", CMD_SHA3_MCT(new_sha3_256)},
    {.name = "SHA3-224", CMD_HASH_AFT(new_sha3_224)},
    {.name = "SHA3-224/MCT", CMD_SHA3_MCT(new_sha3_224)},
    {.name = "SHA3-384", CMD_HASH_AFT(new_sha3_384)},
    {.name = "SHA3-384/MCT", CMD_SHA3_MCT(new_sha3_384)},
    {.name = "SHA3-512", CMD_HASH_AFT(new_sha3_512)},
    {.name = "SHA3-512/MCT", CMD_SHA3_MCT(new_sha3_512)},

    /* Note: the "/ENTROPY" suffix is our own creation, and applied
     * conditionally based on the environment variable that indicates our
     * acvp_test module wrapper is being used for evaluating the separate
     * SHA-384 implementation for the CPU jitter entropy conditioning. Set
     * GOENTROPYSOURCEACVP=1 to use these commands in place of SHA2-384. */
    {.name = "SHA2-384/ENTROPY",
     .required_args = 1 /* Message to hash. */,
     .handler = cmd_entropy_hash_entropy_sha384_aft},
    {.name = "SHA2-384/MCT/ENTROPY",
     .required_args = 1 /* Seed message. */,
     .handler = cmd_entropy_hash_entropy_sha384_mct},

    /* Note: SHAKE AFT and VOT test types can be handled by the same command
     * handler impl, but use distinct acvptool command names, and so are
     * registered twice with the same digest: once under "SHAKE-xxx" for AFT,
     * and once under"SHAKE-xxx/VOT" for VOT. */
    {.name = "SHAKE-128", CMD_SHAKE_AFT_VOT(sha3_new_shake128)},
    {.name = "SHAKE-128/VOT", CMD_SHAKE_AFT_VOT(sha3_new_shake128)},
    {.name = "SHAKE-128/MCT", CMD_SHAKE_MCT(sha3_new_shake128)},
    {.name = "SHAKE-256", CMD_SHAKE_AFT_VOT(sha3_new_shake256)},
    {.name = "SHAKE-256/VOT", CMD_SHAKE_AFT_VOT(sha3_new_shake256)},
    {.name = "SHAKE-256/MCT", CMD_SHAKE_MCT(sha3_new_shake256)},

    {.name = "cSHAKE-128", CMD_CSHAKE_AFT(sha3_new_cshake128)},
    {.name = "cSHAKE-128/MCT", CMD_CSHAKE_MCT(sha3_new_cshake128)},
    {.name = "cSHAKE-256", CMD_CSHAKE_AFT(sha3_new_cshake256)},
    {.name = "cSHAKE-256/MCT", CMD_CSHAKE_MCT(sha3_new_cshake256)},

    {.name = "HMAC-SHA2-224", CMD_HMAC_AFT(sha256_new224)},
    {.name = "HMAC-SHA2-256", CMD_HMAC_AFT(sha256_new)},
    {.name = "HMAC-SHA2-384", CMD_HMAC_AFT(sha512_new384)},
    {.name = "HMAC-SHA2-512", CMD_HMAC_AFT(sha512_new)},
    {.name = "HMAC-SHA2-512/224", CMD_HMAC_AFT(sha512_new512224)},
    {.name = "HMAC-SHA2-512/256", CMD_HMAC_AFT(sha512_new512256)},
    {.name = "HMAC-SHA3-224", CMD_HMAC_AFT(new_sha3_224)},
    {.name = "HMAC-SHA3-256", CMD_HMAC_AFT(new_sha3_256)},
    {.name = "HMAC-SHA3-384", CMD_HMAC_AFT(new_sha3_384)},
    {.name = "HMAC-SHA3-512", CMD_HMAC_AFT(new_sha3_512)},

    {.name = "HKDF/SHA2-224", CMD_HKDF_AFT(sha256_new224)},
    {.name = "HKDF/SHA2-256", CMD_HKDF_AFT(sha256_new)},
    {.name = "HKDF/SHA2-384", CMD_HKDF_AFT(sha512_new384)},
    {.name = "HKDF/SHA2-512", CMD_HKDF_AFT(sha512_new)},
    {.name = "HKDF/SHA2-512/224", CMD_HKDF_AFT(sha512_new512224)},
    {.name = "HKDF/SHA2-512/256", CMD_HKDF_AFT(sha512_new512256)},
    {.name = "HKDF/SHA3-224", CMD_HKDF_AFT(new_sha3_224)},
    {.name = "HKDF/SHA3-256", CMD_HKDF_AFT(new_sha3_256)},
    {.name = "HKDF/SHA3-384", CMD_HKDF_AFT(new_sha3_384)},
    {.name = "HKDF/SHA3-512", CMD_HKDF_AFT(new_sha3_512)},

    {.name = "HKDFExtract/SHA2-256", CMD_HKDF_EXTRACT_AFT(sha256_new)},
    {.name = "HKDFExtract/SHA2-384", CMD_HKDF_EXTRACT_AFT(sha512_new384)},
    {.name = "HKDFExpandLabel/SHA2-256", CMD_HKDF_EXPAND_LABEL_AFT(sha256_new)},
    {.name = "HKDFExpandLabel/SHA2-384", CMD_HKDF_EXPAND_LABEL_AFT(sha512_new384)},

    {.name = "PBKDF", CMD_PBKDF()},

    {.name = "ML-KEM-768/keyGen", CMD_ML_KEM768_KEY_GEN_AFT()},
    {.name = "ML-KEM-768/encap", CMD_ML_KEM768_ENCAP_AFT()},
    {.name = "ML-KEM-768/decap", CMD_ML_KEM768_DECAP_AFT()},
    {.name = "ML-KEM-1024/keyGen", CMD_ML_KEM1024_KEY_GEN_AFT()},
    {.name = "ML-KEM-1024/encap", CMD_ML_KEM1024_ENCAP_AFT()},
    {.name = "ML-KEM-1024/decap", CMD_ML_KEM1024_DECAP_AFT()},

    {.name = "hmacDRBG/SHA2-224", CMD_HMAC_DRBG_AFT(sha256_new224)},
    {.name = "hmacDRBG/SHA2-256", CMD_HMAC_DRBG_AFT(sha256_new)},
    {.name = "hmacDRBG/SHA2-384", CMD_HMAC_DRBG_AFT(sha512_new384)},
    {.name = "hmacDRBG/SHA2-512", CMD_HMAC_DRBG_AFT(sha512_new)},
    {.name = "hmacDRBG/SHA2-512/224", CMD_HMAC_DRBG_AFT(sha512_new512224)},
    {.name = "hmacDRBG/SHA2-512/256", CMD_HMAC_DRBG_AFT(sha512_new512256)},
    {.name = "hmacDRBG/SHA3-224", CMD_HMAC_DRBG_AFT(new_sha3_224)},
    {.name = "hmacDRBG/SHA3-256", CMD_HMAC_DRBG_AFT(new_sha3_256)},
    {.name = "hmacDRBG/SHA3-384", CMD_HMAC_DRBG_AFT(new_sha3_384)},
    {.name = "hmacDRBG/SHA3-512", CMD_HMAC_DRBG_AFT(new_sha3_512)},

    {.name = "EDDSA/keyGen", CMD_EDDSA_KEY_GEN_AFT()},
    {.name = "EDDSA/keyVer", CMD_EDDSA_KEY_VER_AFT()},
    {.name = "EDDSA/sigGen", CMD_EDDSA_SIG_GEN_AFT_BFT()},
    {.name = "EDDSA/sigVer", CMD_EDDSA_SIG_VER_AFT()},

    {.name = "ECDSA/keyGen", CMD_ECDSA_KEY_GEN_AFT()},
    {.name = "ECDSA/keyVer", CMD_ECDSA_KEY_VER_AFT()},
    {.name = "ECDSA/sigGen", CMD_ECDSA_SIG_GEN_AFT(ECDSA_SIG_TYPE_NORMAL)},
    {.name = "ECDSA/sigVer", CMD_ECDSA_SIG_VER_AFT()},
    {.name = "DetECDSA/sigGen", CMD_ECDSA_SIG_GEN_AFT(ECDSA_SIG_TYPE_DETERMINISTIC)},

    {.name = "AES-CBC/encrypt", CMD_AES_CBC(AES_ENCRYPT)},
    {.name = "AES-CBC/decrypt", CMD_AES_CBC(AES_DECRYPT)},
    {.name = "AES-CTR/encrypt", CMD_AES_CTR(AES_ENCRYPT)},
    {.name = "AES-CTR/decrypt", CMD_AES_CTR(AES_DECRYPT)},
    {.name = "AES-GCM/seal", CMD_AES_GCM_SEAL(false)},
    {.name = "AES-GCM/open", CMD_AES_GCM_OPEN(false)},
    {.name = "AES-GCM-randnonce/seal", CMD_AES_GCM_SEAL(true)},
    {.name = "AES-GCM-randnonce/open", CMD_AES_GCM_OPEN(true)},

    {.name = "CMAC-AES", CMD_CMAC_AES_AFT()},
    {.name = "CMAC-AES/verify", CMD_CMAC_AES_VERIFY_AFT()},

    /* Note: Only SHA2-256, SHA2-384 and SHA2-512 are valid hash functions for
     * TLSKDF.
     * See https://pages.nist.gov/ACVP/draft-celi-acvp-kdf-tls.html#section-7.2.1 */
    {.name = "TLSKDF/1.2/SHA2-256", CMD_TLS_KDF12_AFT(sha256_new)},
    {.name = "TLSKDF/1.2/SHA2-384", CMD_TLS_KDF12_AFT(sha512_new384)},
    {.name = "TLSKDF/1.2/SHA2-512", CMD_TLS_KDF12_AFT(sha512_new)},

    /* Note: only SHA2-224, SHA2-256, SHA2-384 and SHA2-512 are valid hash
     * functions for SSHKDF.
     * See https://pages.nist.gov/ACVP/draft-celi-acvp-kdf-ssh.html#section-7.2.1 */
    {.name = "SSHKDF/SHA2-224/client",
     CMD_SSH_KDF_AFT(sha256_new224, &burrow__ssh_client_keys)},
    {.name = "SSHKDF/SHA2-224/server",
     CMD_SSH_KDF_AFT(sha256_new224, &burrow__ssh_server_keys)},
    {.name = "SSHKDF/SHA2-256/client",
     CMD_SSH_KDF_AFT(sha256_new, &burrow__ssh_client_keys)},
    {.name = "SSHKDF/SHA2-256/server",
     CMD_SSH_KDF_AFT(sha256_new, &burrow__ssh_server_keys)},
    {.name = "SSHKDF/SHA2-384/client",
     CMD_SSH_KDF_AFT(sha512_new384, &burrow__ssh_client_keys)},
    {.name = "SSHKDF/SHA2-384/server",
     CMD_SSH_KDF_AFT(sha512_new384, &burrow__ssh_server_keys)},
    {.name = "SSHKDF/SHA2-512/client",
     CMD_SSH_KDF_AFT(sha512_new, &burrow__ssh_client_keys)},
    {.name = "SSHKDF/SHA2-512/server",
     CMD_SSH_KDF_AFT(sha512_new, &burrow__ssh_server_keys)},

    {.name = "ECDH/P-224", CMD_ECDH_AFT_VAL(burrow__ecdh_p224)},
    {.name = "ECDH/P-256", CMD_ECDH_AFT_VAL(ecdh_p256)},
    {.name = "ECDH/P-384", CMD_ECDH_AFT_VAL(ecdh_p384)},
    {.name = "ECDH/P-521", CMD_ECDH_AFT_VAL(ecdh_p521)},

    {.name = "ctrDRBG/AES-256", CMD_CTR_DRBG_AFT()},
    {.name = "ctrDRBG-reseed/AES-256", CMD_CTR_DRBG_RESEED_AFT()},

    {.name = "RSA/keyGen", CMD_RSA_KEY_GEN_AFT()},

    {.name = "RSA/sigGen/SHA2-224/pkcs1v1.5",
     CMD_RSA_SIG_GEN_AFT(sha256_new224, "SHA-224", false)},
    {.name = "RSA/sigGen/SHA2-256/pkcs1v1.5",
     CMD_RSA_SIG_GEN_AFT(sha256_new, "SHA-256", false)},
    {.name = "RSA/sigGen/SHA2-384/pkcs1v1.5",
     CMD_RSA_SIG_GEN_AFT(sha512_new384, "SHA-384", false)},
    {.name = "RSA/sigGen/SHA2-512/pkcs1v1.5",
     CMD_RSA_SIG_GEN_AFT(sha512_new, "SHA-512", false)},
    {.name = "RSA/sigGen/SHA2-224/pss",
     CMD_RSA_SIG_GEN_AFT(sha256_new224, "SHA-224", true)},
    {.name = "RSA/sigGen/SHA2-256/pss",
     CMD_RSA_SIG_GEN_AFT(sha256_new, "SHA-256", true)},
    {.name = "RSA/sigGen/SHA2-384/pss",
     CMD_RSA_SIG_GEN_AFT(sha512_new384, "SHA-384", true)},
    {.name = "RSA/sigGen/SHA2-512/pss",
     CMD_RSA_SIG_GEN_AFT(sha512_new, "SHA-512", true)},

    {.name = "RSA/sigVer/SHA2-224/pkcs1v1.5",
     CMD_RSA_SIG_VER_AFT(sha256_new224, "SHA-224", false)},
    {.name = "RSA/sigVer/SHA2-256/pkcs1v1.5",
     CMD_RSA_SIG_VER_AFT(sha256_new, "SHA-256", false)},
    {.name = "RSA/sigVer/SHA2-384/pkcs1v1.5",
     CMD_RSA_SIG_VER_AFT(sha512_new384, "SHA-384", false)},
    {.name = "RSA/sigVer/SHA2-512/pkcs1v1.5",
     CMD_RSA_SIG_VER_AFT(sha512_new, "SHA-512", false)},
    {.name = "RSA/sigVer/SHA2-224/pss",
     CMD_RSA_SIG_VER_AFT(sha256_new224, "SHA-224", true)},
    {.name = "RSA/sigVer/SHA2-256/pss",
     CMD_RSA_SIG_VER_AFT(sha256_new, "SHA-256", true)},
    {.name = "RSA/sigVer/SHA2-384/pss",
     CMD_RSA_SIG_VER_AFT(sha512_new384, "SHA-384", true)},
    {.name = "RSA/sigVer/SHA2-512/pss",
     CMD_RSA_SIG_VER_AFT(sha512_new, "SHA-512", true)},

    {.name = "KDF-counter", CMD_KDF_COUNTER_AFT()},
    {.name = "KDF-feedback", CMD_KDF_FEEDBACK_AFT()},

    {.name = "OneStepNoCounter/HMAC-SHA2-224",
     CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(sha256_new224)},
    {.name = "OneStepNoCounter/HMAC-SHA2-256",
     CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(sha256_new)},
    {.name = "OneStepNoCounter/HMAC-SHA2-384",
     CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(sha512_new384)},
    {.name = "OneStepNoCounter/HMAC-SHA2-512",
     CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(sha512_new)},
    {.name = "OneStepNoCounter/HMAC-SHA2-512/224",
     CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(sha512_new512224)},
    {.name = "OneStepNoCounter/HMAC-SHA2-512/256",
     CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(sha512_new512256)},
    {.name = "OneStepNoCounter/HMAC-SHA3-224",
     CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(new_sha3_224)},
    {.name = "OneStepNoCounter/HMAC-SHA3-256",
     CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(new_sha3_256)},
    {.name = "OneStepNoCounter/HMAC-SHA3-384",
     CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(new_sha3_384)},
    {.name = "OneStepNoCounter/HMAC-SHA3-512",
     CMD_ONE_STEP_NO_COUNTER_HMAC_AFT(new_sha3_512)},

    {.name = "KTS-IFC/SHA2-224/initiator", CMD_KTS_IFC_INITIATOR_AFT(sha256_new224)},
    {.name = "KTS-IFC/SHA2-224/responder", CMD_KTS_IFC_RESPONDER_AFT(sha256_new224)},
    {.name = "KTS-IFC/SHA2-256/initiator", CMD_KTS_IFC_INITIATOR_AFT(sha256_new)},
    {.name = "KTS-IFC/SHA2-256/responder", CMD_KTS_IFC_RESPONDER_AFT(sha256_new)},
    {.name = "KTS-IFC/SHA2-384/initiator", CMD_KTS_IFC_INITIATOR_AFT(sha512_new384)},
    {.name = "KTS-IFC/SHA2-384/responder", CMD_KTS_IFC_RESPONDER_AFT(sha512_new384)},
    {.name = "KTS-IFC/SHA2-512/initiator", CMD_KTS_IFC_INITIATOR_AFT(sha512_new)},
    {.name = "KTS-IFC/SHA2-512/responder", CMD_KTS_IFC_RESPONDER_AFT(sha512_new)},
    {.name = "KTS-IFC/SHA2-512/224/initiator",
     CMD_KTS_IFC_INITIATOR_AFT(sha512_new512224)},
    {.name = "KTS-IFC/SHA2-512/224/responder",
     CMD_KTS_IFC_RESPONDER_AFT(sha512_new512224)},
    {.name = "KTS-IFC/SHA2-512/256/initiator",
     CMD_KTS_IFC_INITIATOR_AFT(sha512_new512256)},
    {.name = "KTS-IFC/SHA2-512/256/responder",
     CMD_KTS_IFC_RESPONDER_AFT(sha512_new512256)},
    {.name = "KTS-IFC/SHA3-224/initiator", CMD_KTS_IFC_INITIATOR_AFT(new_sha3_224)},
    {.name = "KTS-IFC/SHA3-224/responder", CMD_KTS_IFC_RESPONDER_AFT(new_sha3_224)},
    {.name = "KTS-IFC/SHA3-256/initiator", CMD_KTS_IFC_INITIATOR_AFT(new_sha3_256)},
    {.name = "KTS-IFC/SHA3-256/responder", CMD_KTS_IFC_RESPONDER_AFT(new_sha3_256)},
    {.name = "KTS-IFC/SHA3-384/initiator", CMD_KTS_IFC_INITIATOR_AFT(new_sha3_384)},
    {.name = "KTS-IFC/SHA3-384/responder", CMD_KTS_IFC_RESPONDER_AFT(new_sha3_384)},
    {.name = "KTS-IFC/SHA3-512/initiator", CMD_KTS_IFC_INITIATOR_AFT(new_sha3_512)},
    {.name = "KTS-IFC/SHA3-512/responder", CMD_KTS_IFC_RESPONDER_AFT(new_sha3_512)},

    /* Not in Go: acvp_mldsa_test.go adds these to the map in its init. */
    {.name = "ML-DSA-44/keyGen", CMD_ML_DSA_KEY_GEN_AFT(mldsa_mldsa44)},
    {.name = "ML-DSA-65/keyGen", CMD_ML_DSA_KEY_GEN_AFT(mldsa_mldsa65)},
    {.name = "ML-DSA-87/keyGen", CMD_ML_DSA_KEY_GEN_AFT(mldsa_mldsa87)},
    {.name = "ML-DSA-44/sigGen", CMD_ML_DSA_SIG_GEN_AFT()},
    {.name = "ML-DSA-65/sigGen", CMD_ML_DSA_SIG_GEN_AFT()},
    {.name = "ML-DSA-87/sigGen", CMD_ML_DSA_SIG_GEN_AFT()},
    {.name = "ML-DSA-44/sigVer", CMD_ML_DSA_SIG_VER_AFT(mldsa_mldsa44)},
    {.name = "ML-DSA-65/sigVer", CMD_ML_DSA_SIG_VER_AFT(mldsa_mldsa65)},
    {.name = "ML-DSA-87/sigVer", CMD_ML_DSA_SIG_VER_AFT(mldsa_mldsa87)},
};

/* From the init of acvp_fips140v1.28_test.go.
 *
 * Not in Go: Go's init sets these in the map, which replaces what is there
 * for the names commands already has, so lookup_command looks here first. */
static const AcvpCommand commands_v1_28[] = {
    /* RSA keyGen for keyFormat=crt capability. Previous versions used
     * standard */
    {.name = "RSA/keyGen/crt", CMD_RSA_KEY_GEN_CRT_AFT()},

    /* RSA sigGen additional hash algorithms (adds SHA2-512 truncated hashes,
     * SHA-3) Support for PSS saltLen argument. */
    {.name = "RSA/sigGen/SHA2-224/pkcs1v1.5",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha256_new224, "SHA-224", false)},
    {.name = "RSA/sigGen/SHA2-256/pkcs1v1.5",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha256_new, "SHA-256", false)},
    {.name = "RSA/sigGen/SHA2-384/pkcs1v1.5",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha512_new384, "SHA-384", false)},
    {.name = "RSA/sigGen/SHA2-512/pkcs1v1.5",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha512_new, "SHA-512", false)},
    {.name = "RSA/sigGen/SHA2-512/224/pkcs1v1.5",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha512_new512224, "SHA-512/224", false)},
    {.name = "RSA/sigGen/SHA2-512/256/pkcs1v1.5",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha512_new512256, "SHA-512/256", false)},
    {.name = "RSA/sigGen/SHA3-224/pkcs1v1.5",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(new_sha3_224, "SHA3-224", false)},
    {.name = "RSA/sigGen/SHA3-256/pkcs1v1.5",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(new_sha3_256, "SHA3-256", false)},
    {.name = "RSA/sigGen/SHA3-384/pkcs1v1.5",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(new_sha3_384, "SHA3-384", false)},
    {.name = "RSA/sigGen/SHA3-512/pkcs1v1.5",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(new_sha3_512, "SHA3-512", false)},
    {.name = "RSA/sigGen/SHA2-512/224/pss",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha512_new512224, "SHA-512/224", true)},
    {.name = "RSA/sigGen/SHA2-512/256/pss",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha512_new512256, "SHA-512/256", true)},
    {.name = "RSA/sigGen/SHA3-224/pss",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(new_sha3_224, "SHA3-224", true)},
    {.name = "RSA/sigGen/SHA3-256/pss",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(new_sha3_256, "SHA3-256", true)},
    {.name = "RSA/sigGen/SHA3-384/pss",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(new_sha3_384, "SHA3-384", true)},
    {.name = "RSA/sigGen/SHA3-512/pss",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(new_sha3_512, "SHA3-512", true)},
    {.name = "RSA/sigGen/SHA2-224/pss",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha256_new224, "SHA-224", true)},
    {.name = "RSA/sigGen/SHA2-256/pss",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha256_new, "SHA-256", true)},
    {.name = "RSA/sigGen/SHA2-384/pss",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha512_new384, "SHA-384", true)},
    {.name = "RSA/sigGen/SHA2-512/pss",
     CMD_RSA_SIG_GEN_SALT_LEN_AFT(sha512_new, "SHA-512", true)},

    /* RSA sigGen with pubExpMode=random capability. Previous versions used
     * fixed. Also adds SHA2-512 truncated hashes. Notably SHA-3 is not
     * supported by ACVP for RSA sigVer. */
    {.name = "RSA/sigVer/SHA2-224/pkcs1v1.5",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha256_new224, "SHA-224", false)},
    {.name = "RSA/sigVer/SHA2-256/pkcs1v1.5",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha256_new, "SHA-256", false)},
    {.name = "RSA/sigVer/SHA2-384/pkcs1v1.5",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha512_new384, "SHA-384", false)},
    {.name = "RSA/sigVer/SHA2-512/pkcs1v1.5",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha512_new, "SHA-512", false)},
    {.name = "RSA/sigVer/SHA2-512/224/pkcs1v1.5",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha512_new512224, "SHA-512/224", false)},
    {.name = "RSA/sigVer/SHA2-512/256/pkcs1v1.5",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha512_new512256, "SHA-512/256", false)},
    {.name = "RSA/sigVer/SHA2-224/pss",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha256_new224, "SHA-224", true)},
    {.name = "RSA/sigVer/SHA2-256/pss",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha256_new, "SHA-256", true)},
    {.name = "RSA/sigVer/SHA2-384/pss",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha512_new384, "SHA-384", true)},
    {.name = "RSA/sigVer/SHA2-512/pss",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha512_new, "SHA-512", true)},
    {.name = "RSA/sigVer/SHA2-512/224/pss",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha512_new512224, "SHA-512/224", true)},
    {.name = "RSA/sigVer/SHA2-512/256/pss",
     CMD_RSA_SIG_VER_RAND_EXP_AFT(sha512_new512256, "SHA-512/256", true)},

    /* KTS-IFC with keyGenerationMethods=rsakpg2-crt (random exponent, CRT
     * format). Previous version used rsakpg1-basic (fixed exponent, basic
     * format). */
    {.name = "KTS-IFC/SHA2-224/responder/crt",
     CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(sha256_new224)},
    {.name = "KTS-IFC/SHA2-256/responder/crt",
     CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(sha256_new)},
    {.name = "KTS-IFC/SHA2-384/responder/crt",
     CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(sha512_new384)},
    {.name = "KTS-IFC/SHA2-512/responder/crt",
     CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(sha512_new)},
    {.name = "KTS-IFC/SHA2-512/224/responder/crt",
     CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(sha512_new512224)},
    {.name = "KTS-IFC/SHA2-512/256/responder/crt",
     CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(sha512_new512256)},
    {.name = "KTS-IFC/SHA3-224/responder/crt",
     CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(new_sha3_224)},
    {.name = "KTS-IFC/SHA3-256/responder/crt",
     CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(new_sha3_256)},
    {.name = "KTS-IFC/SHA3-384/responder/crt",
     CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(new_sha3_384)},
    {.name = "KTS-IFC/SHA3-512/responder/crt",
     CMD_KTS_IFC_RESPONDER_RAND_EXP_CRT_AFT(new_sha3_512)},
};

/* Not in Go: the command TestTooFewArgs and TestTooManyArgs add to the map as
 * "test", which is looked up first. */
static AcvpCommand test_command;

static const AcvpCommand *lookup_command(Str name) {
    if (test_command.handler != NULL && str_eq(name, BURROW_S("test")))
        return &test_command;
    for (size_t i = 0; i < sizeof commands_v1_28 / sizeof commands_v1_28[0]; i++)
        if (str_eq(name, str_from_cstr(commands_v1_28[i].name)))
            return &commands_v1_28[i];
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++)
        if (str_eq(name, str_from_cstr(commands[i].name)))
            return &commands[i];
    return NULL;
}

/* --------------------------------------------------------------- tests */

static IoReader mock_request(TestingT *t, Alloc *a, BytesBuffer *buf, Str cmd,
                             AcvpArgs args) {
    testing_t_helper(t);

    AcvpArgs msg_data = make_args(a, 1 + args.len);
    msg_data.v[0] = str_bytes(cmd);
    for (Int i = 0; i < args.len; i++)
        msg_data.v[1 + i] = args.v[i];

    Error err = write_response(a, bytes_buffer_as_io_writer(buf), msg_data);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "writeResponse error: %v", err);

    return bytes_buffer_as_io_reader(buf);
}

static AcvpArgs read_response(TestingT *t, Alloc *a, IoReader reader) {
    uint32_t num_args = 0;
    Error err = binary_read(a, reader, binary_little_endian,
                            BURROW_ANY(TYPE_UINT32, &num_args));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to read response args count: %v", err);

    AcvpArgs args;
    err = read_args(a, reader, num_args, &args);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to read %d response args: %v", (Int)num_args,
                           err);

    return args;
}

static Error test_handler(Alloc *a, const AcvpCommand *c, AcvpArgs args,
                          AcvpArgs *out) {
    (void)a;
    (void)c;
    *out = (AcvpArgs){NULL, 0};
    Int got_args = args.len;
    if (got_args != 1)
        return fmt_errorf_v("expected 1 args, got %d", got_args);
    return BURROW_NO_ERROR;
}

/* Not in Go: the memory of TestACVP, freed after its other cleanups, even when
 * it is skipped or fails. */
static void acvp_free_arena(void *env) {
    Arena *ar = env;
    arena_free(ar);
    mem_free(heap_allocator(), ar, sizeof *ar, _Alignof(Arena));
}

static void TestACVP(TestingT *t) {
    skip_if_short_and_slow(t);

    Arena *ar = BURROW_NEW(heap_allocator(), Arena);
    if (ar == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, acvp_free_arena, ar));
    Alloc *a = arena_allocator(ar);

    Str bssl_module = BURROW_S("boringssl.googlesource.com/boringssl.git");
    Str bssl_version = BURROW_S("v0.0.0-20260422110153-4ccbe2adaf4f");
    Str go_acvp_module = BURROW_S("github.com/geomys/acvp-testdata");
    Str go_acvp_version = BURROW_S("v0.0.0-20260526143807-16992c4b1561");

    /* In crypto/tls/bogo_shim_test.go the test is skipped if run on a builder
     * with runtime.GOOS == "windows" due to flaky networking. It may be
     * necessary to do the same here. */

    /* Stat the acvp test config file so the test will be re-run if it
     * changes, invalidating cached results from the old config. */
    Error err = BURROW_NO_ERROR;
    (void)os_stat(a, test_config_file, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to stat config file: %s", err);
        return;
    }

    /* Fetch the BSSL module and use the JSON output to find the absolute path
     * to the dir. */
    Str bssl_dir = fetch_module(t, a, bssl_module, bssl_version);

    testing_t_log_v(t, "building acvptool");

    /* Build the acvptool binary. */
    Str go = go_tool_path(t, a);
    Str tool_path =
        filepath_join_v(a, 2, fetchmodule_temp_dir(t, a), BURROW_S("acvptool.exe"));
    ExecCmd *cmd =
        exec_command_v(a, go, 4, BURROW_S("build"), BURROW_S("-o"), tool_path,
                       BURROW_S("./util/fipstools/acvp/acvptool"));
    cmd->dir = bssl_dir;
    Slice out = exec_cmd_combined_output(cmd, a, &err);
    exec_cmd_free(cmd);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to build acvptool: %s\n%s", err, bytes_str(out));
        return;
    }

    /* Similarly, fetch the ACVP data module that has vectors/expected
     * answers. */
    Str data_dir = fetch_module(t, a, go_acvp_module, go_acvp_version);

    Str cwd = os_getwd(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to fetch cwd: %s", err);
        return;
    }
    Str config_path = filepath_join_v(a, 2, cwd, test_config_file);

    /* Not in Go: testenv.Executable is this program, which Go's linker has
     * written the module's checksum into. Here it is fipstest_program, which
     * is a copy with the checksum written in unless burrow-gen fips has been
     * run on this one, and the module has to be one that can be turned on. */
    MUST_SUPPORT_FIPS140(t);
    Str wrapper_dir;
    Str wrapper = fipstest_program(t, a, &wrapper_dir);
    if (wrapper_dir.len > 0) {
        Str *p = BURROW_NEW(a, Str);
        *p = wrapper_dir;
        testing_t_cleanup(t, BURROW_FN(Func, fetchmodule_remove_all, p));
    }
    if (wrapper.len == 0)
        return;
    testing_t_logf_v(
        t,
        "running check_expected.go\ncwd: %q\ndata_dir: %q\nconfig: %q\ntool: "
        "%q\nmodule-wrapper: %q\n",
        cwd, data_dir, config_path, tool_path, wrapper);

    /* Run the check_expected test driver using the acvptool we built, and this
     * test binary as the module wrapper. The file paths in the config file are
     * specified relative to the dataDir root so we run the command from that
     * dir. */
    cmd = exec_command_v(
        a, go, 8, BURROW_S("run"),
        filepath_join_v(
            a, 2, bssl_dir,
            BURROW_S("util/fipstools/acvp/acvptool/test/check_expected.go")),
        BURROW_S("-tool"), tool_path,
        /* Note: module prefix must match Wrapper value in testConfigFile. */
        BURROW_S("-module-wrappers"), fmt_sprintf_v(a, "go:%s", wrapper),
        BURROW_S("-tests"), config_path);
    cmd->dir = data_dir;
    cmd->env = BURROW_APPEND(Str, a, os_environ(a), BURROW_S("ACVP_WRAPPER=1"));
    cmd->env = BURROW_APPEND(Str, a, cmd->env, BURROW_S("GODEBUG=fips140=on"));
    out = exec_cmd_combined_output(cmd, a, &err);
    exec_cmd_free(cmd);
    testing_t_logf_v(t, "\n%s", bytes_str(out));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to run acvp tests: %s", err);
        return;
    }
}

static void TestTooFewArgs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    test_command =
        (AcvpCommand){.name = "test", .required_args = 1, .handler = test_handler};

    BytesBuffer output = BYTES_BUFFER(a);
    BytesBuffer req = BYTES_BUFFER(a);
    Error err =
        processing_loop(mock_request(t, a, &req, BURROW_S("test"), (AcvpArgs){NULL, 0}),
                        bytes_buffer_as_io_writer(&output));
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "expected error, got nil");
    Str expected_err = BURROW_S("expected 1 args, got 0");
    if (!strings_contains(error_text(err), expected_err))
        testing_t_errorf_v(t, "expected error to contain %q, got %v", expected_err,
                           err);
    arena_free(&ar);
}

static void TestTooManyArgs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    test_command =
        (AcvpCommand){.name = "test", .required_args = 1, .handler = test_handler};

    AcvpArgs args = make_args(a, 2);
    args.v[0] = str_bytes(BURROW_S("one"));
    args.v[1] = str_bytes(BURROW_S("two"));

    BytesBuffer output = BYTES_BUFFER(a);
    BytesBuffer req = BYTES_BUFFER(a);
    Error err = processing_loop(mock_request(t, a, &req, BURROW_S("test"), args),
                                bytes_buffer_as_io_writer(&output));
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "expected error, got nil");
    Str expected_err = BURROW_S("expected 1 args, got 2");
    if (!strings_contains(error_text(err), expected_err))
        testing_t_errorf_v(t, "expected error to contain %q, got %v", expected_err,
                           err);
    arena_free(&ar);
}

static void TestGetConfig(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    BytesBuffer output = BYTES_BUFFER(a);
    BytesBuffer req = BYTES_BUFFER(a);
    Error err = processing_loop(
        mock_request(t, a, &req, BURROW_S("getConfig"), (AcvpArgs){NULL, 0}),
        bytes_buffer_as_io_writer(&output));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected error: %v", err);

    AcvpArgs resp_args = read_response(t, a, bytes_buffer_as_io_reader(&output));
    if (resp_args.len != 1)
        testing_t_fatalf_v(t, "expected 1 response arg, got %d", resp_args.len);

    if (!bytes_equal(resp_args.v[0], capabilities_json))
        testing_t_errorf_v(t, "expected config %q, got %q",
                           bytes_str(capabilities_json), bytes_str(resp_args.v[0]));
    arena_free(&ar);
}

static void TestSha2256(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Slice test_message = str_bytes(BURROW_S("gophers eat grass"));
    static const Byte expected[] = {
        188, 142, 10,  214, 48, 236, 72,  143, 70,  216, 223, 205, 219, 69, 53,  29,
        205, 207, 162, 6,   14, 70,  113, 60,  251, 170, 201, 236, 119, 39, 141, 172,
    };
    Slice expected_digest = slice_from((void *)(uintptr_t)expected, 32, 32, TYPE_BYTE);

    BytesBuffer output = BYTES_BUFFER(a);
    BytesBuffer req = BYTES_BUFFER(a);
    Error err = processing_loop(
        mock_request(t, a, &req, BURROW_S("SHA2-256"), args1(a, test_message)),
        bytes_buffer_as_io_writer(&output));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected error: %v", err);

    AcvpArgs resp_args = read_response(t, a, bytes_buffer_as_io_reader(&output));
    if (resp_args.len != 1)
        testing_t_fatalf_v(t, "expected 1 response arg, got %d", resp_args.len);

    if (!bytes_equal(resp_args.v[0], expected_digest))
        testing_t_errorf_v(t, "expected digest %v, got %v", expected_digest,
                           resp_args.v[0]);
    arena_free(&ar);
}

/* ---------------------------------------------------------------- main */

static void wrapper_main(void) {
    if (!burrow__fips140_enabled()) {
        fmt_fprintln_v(os_file_as_io_writer(os_stderr),
                       "ACVP wrapper must be run with GODEBUG=fips140=on");
        os_exit(2);
    }
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BufioReader *in = bufio_new_reader(a, os_file_as_io_reader(os_stdin));
    Error err =
        processing_loop(bufio_reader_as_io_reader(in), os_file_as_io_writer(os_stdout));
    if (BURROW_FAILED(err)) {
        fmt_fprintf_v(os_file_as_io_writer(os_stderr), "processing error: %v\n", err);
        os_exit(1);
    }
    arena_free(&ar);
    /* Not in Go: wrapperMain returns to TestMain, which then returns without
     * calling os.Exit, so the program ends with 0. */
    os_exit(0);
}

static int acvp_main(TestingM *m) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    no_paapai = str_eq(os_getenv(a, BURROW_S("GONOPAAPAI")), BURROW_S("1"));
    entropy_testing =
        str_eq(os_getenv(a, BURROW_S("GOENTROPYSOURCEACVP")), BURROW_S("1"));
    bool wrapper = str_eq(os_getenv(a, BURROW_S("ACVP_WRAPPER")), BURROW_S("1"));
    arena_free(&ar);

    /* Not in Go: burrow has no impl package, and AES is the one module
     * package with a switch for its assembly. The hashes' are turned off with
     * GODEBUG=cpu.all=off. */
    if (no_paapai)
        burrow__aes_set_portable(true);
    if (wrapper)
        wrapper_main();
    return testing_m_run(m);
}

#define TESTS(X)                                                                       \
    X(TestACVP) X(TestTooFewArgs) X(TestTooManyArgs) X(TestGetConfig) X(TestSha2256)

TESTING_MAIN_WITH(acvp_main, TESTS)
