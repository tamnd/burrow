#!/bin/sh
# Regenerates tests/x509_test_gen.h: the keys, PEM blocks and numbers in Go's
# crypto/x509 tests that tests/x509_test.c uses, read out of the Go sources so
# none of them is copied by hand. testingKey's "TESTING KEY" becomes "PRIVATE
# KEY" the same as in Go.
#
# The Go on PATH should be the release the port follows. GOROOT can be set to
# point at another source tree.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/x509_test_gen.h"
goroot=${GOROOT:-$(go env GOROOT)}

python3 - "$goroot/src/crypto/x509" "$out" <<'PY'
import re
import sys

src, out = sys.argv[1], sys.argv[2]


def read(name):
    with open(f"{src}/{name}") as f:
        return f.read()


x509_test = read("x509_test.go")
sec1_test = read("sec1_test.go")
pkcs8_test = read("pkcs8_test.go")
pem_test = read("pem_decrypt_test.go")


def testing_key(s):
    return s.replace("TESTING KEY", "PRIVATE KEY")


def one(pattern, text):
    m = re.findall(pattern, text, re.S)
    assert len(m) == 1, (pattern, len(m))
    return m[0]


def c_str(s, indent="    "):
    """s as C string literals, one line of s or 64 characters each."""
    pieces = []
    lines = s.split("\n")
    for i, line in enumerate(lines):
        nl = "\\n" if i < len(lines) - 1 else ""
        if line == "" and nl == "":
            continue
        while len(line) > 64:
            pieces.append(line[:64])
            line = line[64:]
        pieces.append(line + nl)
    if not pieces:
        pieces = [""]
    for p in pieces:
        assert '"' not in p and "\\" not in p.replace("\\n", "")
    return "\n".join(f'{indent}"{p}"' for p in pieces)


defs = []


def define(name, value):
    assert len(value) <= 4000, name
    defs.append(f"static const char {name}[] =\n{c_str(value)};\n")


# sec1_test.go
ec = re.findall(r'\{"([0-9a-f]+)", (true|false)\}', one(r"var ecKeyTests = (.*?)\n\}\n", sec1_test))
assert len(ec) == 3
for i, (h, _) in enumerate(ec):
    define(f"ec_key_test_{i}", h)
defs.append(
    "static const bool ec_key_test_reserialize[] = {"
    + ", ".join(r for _, r in ec)
    + "};\n"
)
define("hex_ec_test_pkcs1_key", one(r'const hexECTestPKCS1Key = "([0-9a-f]+)"', sec1_test))
define("hex_ec_test_pkcs8_key", one(r'const hexECTestPKCS8Key = "([0-9a-f]+)"', sec1_test))

# pkcs8_test.go
for name, c in [
    ("RSA", "rsa"),
    ("P224", "p224"),
    ("P256", "p256"),
    ("P384", "p384"),
    ("P521", "p521"),
    ("Ed25519", "ed25519"),
    ("X25519", "x25519"),
]:
    define(f"pkcs8_{c}_private_key_hex", one(rf"var pkcs8{name}PrivateKeyHex = `([0-9a-f]+)`", pkcs8_test))
define("hex_pkcs8_test_pkcs1_key", one(r'const hexPKCS8TestPKCS1Key = "([0-9a-f]+)"', pkcs8_test))
define("hex_pkcs8_test_ec_key", one(r'const hexPKCS8TestECKey = "([0-9a-f]+)"', pkcs8_test))

# pem_decrypt_test.go
entries = re.findall(
    r'kind:\s+PEMCipher(\w+),\s+password: \[\]byte\("([^"]*)"\),\s+'
    r"pemData: \[\]byte\(testingKey\(`(.*?)`\)\),\s+plainDER: `(.*?)`",
    pem_test,
    re.S,
)
assert len(entries) == 6, len(entries)
kinds = {
    "DES": "X509_PEM_CIPHER_DES",
    "3DES": "X509_PEM_CIPHER3_DES",
    "AES128": "X509_PEM_CIPHER_AES128",
    "AES192": "X509_PEM_CIPHER_AES192",
    "AES256": "X509_PEM_CIPHER_AES256",
}
rows = []
for i, (kind, password, pem, der) in enumerate(entries):
    define(f"pem_test_{i}_pem", testing_key(pem))
    define(f"pem_test_{i}_der", der.replace("\n", ""))
    rows.append(f'    {{{kinds[kind]}, "{password}", pem_test_{i}_pem, pem_test_{i}_der}},')
defs.append(
    "static const struct {\n    X509PEMCipher kind;\n    const char *password;\n"
    "    const char *pem_data;\n    const char *plain_der;\n} pem_test_data[] = {\n"
    + "\n".join(rows)
    + "\n};\n"
)
define("incomplete_block_pem", testing_key(one(r"var incompleteBlockPEM = testingKey\(`(.*?)`\)", pem_test)))

# x509_test.go
define("pem_public_key", one(r"var pemPublicKey = `(.*?)`", x509_test))
define("pem_private_key", testing_key(one(r"var pemPrivateKey = testingKey\(`(.*?)`\)", x509_test)))
define("pem_ed25519_key", one(r"var pemEd25519Key = `(.*?)`", x509_test))
define("pem_x25519_key", one(r"var pemX25519Key = `(.*?)`", x509_test))
define("pkix_public_key_hex", one(r'const pkixPublicKey = "([0-9a-f]+)"', x509_test))
define("pkcs1_public_key_hex", one(r'const pkcs1PublicKey = "([0-9a-f]+)"', x509_test))
define("hex_pkcs1_test_pkcs8_key", one(r'const hexPKCS1TestPKCS8Key = "([0-9a-f]+)"', x509_test))
define("hex_pkcs1_test_ec_key", one(r'const hexPKCS1TestECKey = "([0-9a-f]+)"', x509_test))

rsa = one(r"var rsaPrivateKey = &rsa.PrivateKey\{(.*?)\n\}\n", x509_test)
nums = re.findall(r'bigFromString\("([0-9]+)"\)', rsa)
assert len(nums) == 4 and "E: 65537," in rsa
for name, v in zip(["n", "d", "p", "q"], nums):
    define(f"rsa_private_key_{name}", v)

marshal = one(r"func TestMarshalRSAPrivateKey\(t \*testing.T\) \{(.*?)\n\}\n", x509_test)
nums = re.findall(r'fromBase10\("([0-9]+)"\)', marshal)
assert len(nums) == 5 and "E: 3," in marshal
for name, v in zip(["n", "d", "p", "q", "r"], nums):
    define(f"marshal_rsa_{name}", v)

invalid = one(r"func TestMarshalRSAPrivateKeyInvalid\(t \*testing.T\) \{(.*?)\n\}\n", x509_test)
define("rsa2048_pem", testing_key(one(r"`(-----BEGIN RSA TESTING KEY-----.*?)`", invalid)))

public = one(r"func TestMarshalRSAPublicKey\(t \*testing.T\) \{(.*?)\n\}\n", x509_test)
nums = re.findall(r'fromBase10\("([0-9]+)"\)', public)
assert len(nums) == 1 and "E: 3," in public
define("marshal_rsa_public_n", nums[0])

for name in ["44", "65", "87"]:
    define(
        f"mldsa{name}_private_key_pem",
        testing_key(one(rf"var rfc9881ExamplePrivateKeyMLDSA{name} = testingKey\(`(.*?)`\)", x509_test)),
    )
    define(f"mldsa{name}_public_key_pem", one(rf"var rfc9881ExamplePublicKeyMLDSA{name} = `(.*?)`", x509_test))
for name in ["Expanded", "Both"]:
    define(
        f"mldsa44_private_key_{name.lower()}_pem",
        testing_key(one(rf"var rfc9881ExamplePrivateKeyMLDSA44{name} = testingKey\(`(.*?)`\)", x509_test)),
    )

with open(out, "w") as f:
    f.write(
        "/* Generated by tools/gen-x509-tests.sh from Go's crypto/x509 tests. Do not\n"
        " * edit.\n"
        " *\n"
        " * Copyright 2009 The Go Authors. All rights reserved.\n"
        " * Copyright 2026 The burrow Authors. All rights reserved.\n"
        " * Use of this source code is governed by a BSD-style licence that can be found\n"
        " * in the LICENSE file. */\n\n"
        "#ifndef BURROW_TESTS_X509_TEST_GEN_H\n#define BURROW_TESTS_X509_TEST_GEN_H\n\n"
        '#include "burrow/crypto/x509.h"\n\n#include <stdbool.h>\n\n'
    )
    f.write("\n".join(defs))
    f.write("\n#endif\n")
PY
