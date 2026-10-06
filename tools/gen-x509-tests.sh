#!/bin/sh
# Regenerates tests/x509_test_gen.h, tests/x509_parse_test_gen.h,
# tests/x509_verify_test_gen.h, tests/x509_name_constraints_test_gen.h and
# tests/x509_root_test_gen.h: the
# keys, certificates, PEM blocks, tables and numbers in Go's crypto/x509 tests
# that the tests/x509_*_test.c files use, read out of the Go sources so none of
# them is copied by hand. testingKey's
# "TESTING KEY" becomes "PRIVATE KEY" the same as in Go.
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

# The certificates, CSRs and CRLs that the parsing tests in parser_test.go and
# x509_test.go read go to a header of their own for tests/x509_parse_test.c,
# which tests/x509_create_test.c shares.
python3 - "$goroot/src/crypto/x509" "$root/tests/x509_parse_test_gen.h" <<'PY'
import re
import sys

src, out = sys.argv[1], sys.argv[2]


def read(name):
    with open(f"{src}/{name}") as f:
        return f.read()


parser_test = read("parser_test.go")
x509_test = read("x509_test.go")


def one(pattern, text):
    m = re.findall(pattern, text, re.S)
    assert len(m) == 1, (pattern, len(m))
    return m[0]


def func(name, text):
    return one(rf"\nfunc {name}\(t \*testing.T\) \{{(.*?)\n\}}\n", text)


def go_string(name, text):
    """The value of a top level string, in backquotes or quoted pieces."""
    m = re.findall(rf"\n(?:var|const) {name} = `(.*?)`", text, re.S)
    if m:
        assert len(m) == 1, name
        return m[0]
    m = re.findall(rf'\n(?:var|const) {name} = ((?:"[^"\\]*"(?:\s*\+\s*)?)+)', text)
    assert len(m) == 1, name
    return "".join(re.findall(r'"([^"]*)"', m[0]))


def pem_only(s):
    """s from its first PEM boundary on, leaving out openssl's text dump."""
    i = s.index("-----BEGIN")
    return s[i:].strip() + "\n"


def c_str(s, indent="    "):
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


def define_parts(name, value):
    """value cut into pieces short enough for C99, NULL at the end."""
    parts = [value[i : i + 4000] for i in range(0, len(value), 4000)]
    body = ",\n".join(c_str(p, "    ") for p in parts)
    defs.append(f"static const char *const {name}[] = {{\n{body},\n    NULL,\n}};\n")


# parser_test.go
define("policy_pem", go_string("policyPEM", parser_test))
neg = re.findall(
    r"(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)",
    func("TestParseCertificateNegativeMaxPathLength", parser_test),
    re.S,
)
assert len(neg) == 2
for i, c in enumerate(neg):
    define(f"negative_max_path_len_{i}", c + "\n")
define(
    "name_types_pem",
    one(r"(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)", func("TestParseNameTypes", parser_test))
    + "\n",
)
for name in ["policy_leaf_duplicate", "policy_leaf_invalid"]:
    define(f"{name}_pem", read(f"testdata/{name}.pem"))

# x509_test.go
define("cert_bytes", go_string("certBytes", x509_test))
for go, c in [
    ("rsaPSSSelfSignedPEM", "rsa_pss_self_signed_pem"),
    ("rsaPSSSelfSignedOpenSSL110PEM", "rsa_pss_self_signed_openssl110_pem"),
    ("ed25519Certificate", "ed25519_certificate"),
    ("dsaCertPem", "dsa_cert_pem"),
    ("ecdsaSHA256p256CertPem", "ecdsa_sha256_p256_cert_pem"),
    ("ecdsaSHA256p384CertPem", "ecdsa_sha256_p384_cert_pem"),
    ("ecdsaSHA384p521CertPem", "ecdsa_sha384_p521_cert_pem"),
    ("ecdsaSHA1CertPem", "ecdsa_sha1_cert_pem"),
    ("md5cert", "md5_cert"),
    ("certMissingRSANULL", "cert_missing_rsa_null"),
    ("certISOOID", "cert_iso_oid"),
    ("certMultipleRDN", "cert_multiple_rdn"),
    ("emptyNameConstraintsPEM", "empty_name_constraints_pem"),
    ("criticalNameConstraintWithUnknownTypePEM", "critical_name_constraint_with_unknown_type_pem"),
    ("badIPMaskPEM", "bad_ip_mask_pem"),
    ("additionalGeneralSubtreePEM", "additional_general_subtree_pem"),
    ("multipleURLsInCRLDPPEM", "multiple_urls_in_crldp_pem"),
    ("pemCertificate", "pem_certificate"),
    ("mismatchingSigAlgIDPEM", "mismatching_sig_alg_id_pem"),
    ("mismatchingSigAlgParamPEM", "mismatching_sig_alg_param_pem"),
    ("optionalAuthKeyIDPEM", "optional_auth_key_id_pem"),
    ("largeOIDPEM", "large_oid_pem"),
    ("uniqueIDPEM", "unique_id_pem"),
    ("negativeSerialCert", "negative_serial_cert"),
    ("dupExtCert", "dup_ext_cert"),
    ("dupExtCSR", "dup_ext_csr"),
    ("dupAttCSR", "dup_att_csr"),
]:
    define(c, pem_only(go_string(go, x509_test)))
rows = re.findall(
    r"\{(ECDSAWith\w+), (\w+)\}", one(r"\nvar ecdsaTests = (.*?)\n\}\n", x509_test)
)
assert [r[1] for r in rows] == ["ecdsaSHA256p256CertPem", "ecdsaSHA256p384CertPem", "ecdsaSHA384p521CertPem"]
defs.append(
    "static const X509SignatureAlgorithm ecdsa_test_sig_algo[] = {"
    + ", ".join("X509_" + r[0].replace("ECDSAWith", "ECDSA_WITH_").upper() for r in rows)
    + "};\n"
)
define_parts("der_crl_base64", go_string("derCRLBase64", x509_test))
define("pem_crl_base64", go_string("pemCRLBase64", x509_test))
define("crl_without_expiry_base64", one(r'fromBase64\("([^"]+)"\)', func("TestCRLWithoutExpiry", x509_test)))
csrs = re.findall(r'\n\t"([A-Za-z0-9+/=]+)",', one(r"\nvar csrBase64Array = (.*?)\n\}\n", x509_test))
assert len(csrs) == 2
for i, c in enumerate(csrs):
    define(f"csr_base64_{i}", c)
define(
    "critical_csr_base64",
    one(r'const csrBase64 = "([^"]+)"', func("TestCriticalFlagInCSRRequestedExtensions", x509_test)),
)
define("ipv4_mapped_san_cert", go_string("ipv4MappedSANCert", x509_test))
define("ipv4_mapped_constraint_cert", go_string("ipv4MappedConstraintCert", x509_test))

# The certificates and keys the creation tests sign with or read.
define("ed25519_crl_certificate", pem_only(go_string("ed25519CRLCertificate", x509_test)))
define(
    "ed25519_crl_key",
    one(r"\nvar ed25519CRLKey = testingKey\(`(.*?)`\)", x509_test),
)
create_crl = func("TestCreateRevocationList", x509_test)
define("utf8_ca_base64", one(r'utf8CAStr := "([^"]+)"', create_crl))
define("utf8_key_base64", one(r'utf8KeyStr := "([^"]+)"', create_crl))
for name in ["44", "65", "87"]:
    define_parts(
        f"mldsa{name}_certificate_pem",
        pem_only(one(rf"\nvar rfc9881ExampleCertificateMLDSA{name} = `(.*?)`", x509_test)),
    )
ia5 = re.findall(r'cert: +"([0-9a-f]+)",', func("TestIA5SANEnforcement", x509_test))
assert len(ia5) == 3
for i, c in enumerate(ia5):
    define(f"ia5_unmarshal_cert_{i}", c)
define(
    "ocsp_tbs_hex",
    one(r'ocspTBSHex := "([0-9a-f]+)"', func("TestDisableSHA1ForCertOnly", x509_test)),
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
        "#ifndef BURROW_TESTS_X509_PARSE_TEST_GEN_H\n#define BURROW_TESTS_X509_PARSE_TEST_GEN_H\n\n"
        '#include "burrow/crypto/x509.h"\n\n#include <stddef.h>\n\n'
    )
    f.write("\n".join(defs))
    f.write("\n#endif\n")
PY

# The certificates in verify_test.go and the verifyTests table that reads them,
# for tests/x509_verify_test.c.
python3 - "$goroot/src/crypto/x509" "$root/tests/x509_verify_test_gen.h" <<'PY'
import re
import sys

src, out = sys.argv[1], sys.argv[2]

with open(f"{src}/verify_test.go") as f:
    verify_test = f.read()


def one(pattern, text):
    m = re.findall(pattern, text, re.S)
    assert len(m) == 1, (pattern, len(m))
    return m[0]


def snake(name):
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", name).lower()


def c_str(s, indent="    "):
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


def q(s):
    """s as one C string literal."""
    assert "\\" not in s
    return '"' + s.replace('"', '\\"') + '"'


defs = []
pems = {}
for name, value in re.findall(r"\n(?:var|const) (\w+) = `(.*?)`", verify_test, re.S):
    assert len(value) <= 4000, name
    pems[name] = snake(name)
    defs.append(f"static const char {snake(name)}[] =\n{c_str(value)};\n")


def entries(table):
    """The top level { ... } entries of a Go composite literal body."""
    out, depth, start = [], 0, None
    i = 0
    while i < len(table):
        c = table[i]
        if c == '"':
            j = i + 1
            while table[j] != '"':
                j += 2 if table[j] == "\\" else 1
            i = j
        elif c == "{":
            if depth == 0:
                start = i + 1
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                out.append(table[start:i])
        i += 1
    return out


def strings_in(s):
    return [x.replace('\\"', '"') for x in re.findall(r'"((?:[^"\\]|\\.)*)"', s)]


def cert_ref(expr):
    """A PEM constant, or (count, san) for generatePEMCertWithRepeatSAN."""
    expr = expr.strip()
    m = re.fullmatch(r"generatePEMCertWithRepeatSAN\((\d+), (\d+), \"([^\"]+)\"\)", expr)
    if m:
        return (int(m.group(2)), m.group(3), int(m.group(1)))
    return pems[expr]


EXPECT = {
    "expectExpired": "X509_EXPECT_EXPIRED",
    "expectUsageError": "X509_EXPECT_USAGE",
    "expectAuthorityUnknown": "X509_EXPECT_AUTHORITY_UNKNOWN",
    "expectHashError": "X509_EXPECT_HASH",
    "expectNameConstraintsError": "X509_EXPECT_NAME_CONSTRAINTS",
    "expectNotAuthorizedError": "X509_EXPECT_NOT_AUTHORIZED",
    "expectUnhandledCriticalExtension": "X509_EXPECT_UNHANDLED_CRITICAL_EXTENSION",
}

table = one(r"\nvar verifyTests = \[\]verifyTest\{(.*?)\n\}\n", verify_test)
cases = []
for e in entries(table):
    e = re.sub(r"//[^\n]*", "", e)
    c = {"intermediates": [], "roots": [], "chains": [], "key_usages": []}
    m = re.search(r'\bname: +"([^"]*)"', e)
    c["name"] = m.group(1) if m else ""
    leaf = one(r"\bleaf: +([^\n]+),\n", e)
    c["leaf"] = cert_ref(leaf)
    for field in ["intermediates", "roots"]:
        m = re.search(rf"\b{field}: +\[\]string\{{(.*?)\}},\n", e, re.S)
        if m:
            parts = re.findall(r"generatePEMCertWithRepeatSAN\([^)]*\)|\w+", m.group(1))
            c[field] = [cert_ref(p) for p in parts]
    c["current_time"] = int(one(r"\bcurrentTime: +(\d+),", e))
    m = re.search(r'\bdnsName: +"([^"]*)"', e)
    c["dns_name"] = m.group(1) if m else ""
    c["system_skip"] = re.search(r"\bsystemSkip: +true", e) is not None
    c["system_lax"] = re.search(r"\bsystemLax: +true", e) is not None
    m = re.search(r"\bkeyUsages: +\[\]ExtKeyUsage\{(.*?)\}", e)
    if m:
        c["key_usages"] = [
            "X509_EXT_KEY_USAGE_" + snake(k.strip()[len("ExtKeyUsage") :]).upper()
            for k in m.group(1).split(",")
        ]
    c["expect"], c["expect_msg"] = "X509_EXPECT_NOTHING", None
    m = re.search(r"\berrorCallback: +(\w+)(?:\((\"[^\n]*\")\))?,", e)
    if m:
        if m.group(1) == "expectHostnameError":
            c["expect"] = "X509_EXPECT_HOSTNAME"
            c["expect_msg"] = strings_in(m.group(2))[0]
        else:
            c["expect"] = EXPECT[m.group(1)]
    m = re.search(r"\bexpectedChains: +\[\]\[\]string\{(.*)\}", e, re.S)
    if m:
        c["chains"] = [strings_in(x) for x in entries(m.group(1))]
    cases.append(c)

assert len(cases) == len(re.findall(r"\n\t\tleaf: ", table)), len(cases)
n_int = max(len(c["intermediates"]) for c in cases) + 1
n_root = max(len(c["roots"]) for c in cases) + 1
n_chain = max(len(c["chains"]) for c in cases) + 1
n_link = max((len(x) for c in cases for x in c["chains"]), default=0) + 1
n_eku = max(len(c["key_usages"]) for c in cases) + 1

defs.append(
    "/* What a verifyTests entry's errorCallback checks for. */\n"
    "enum {\n"
    "    X509_EXPECT_NOTHING,\n"
    "    X509_EXPECT_HOSTNAME,\n"
    + "".join(f"    {v},\n" for v in EXPECT.values())
    + "};\n"
)
defs.append(
    "/* A certificate in a verifyTests entry: a PEM constant, or when pem is NULL\n"
    " * one generatePEMCertWithRepeatSAN makes at the time given. */\n"
    "typedef struct X509VerifyCert {\n"
    "    const char *pem;\n"
    "    Int repeat;\n"
    "    const char *san;\n"
    "    int64_t at;\n"
    "} X509VerifyCert;\n"
)
defs.append(
    "typedef struct X509VerifyCase {\n"
    "    const char *name;\n"
    "    X509VerifyCert leaf;\n"
    f"    X509VerifyCert intermediates[{n_int}];\n"
    f"    X509VerifyCert roots[{n_root}];\n"
    "    int64_t current_time;\n"
    "    const char *dns_name;\n"
    "    bool system_skip;\n"
    "    bool system_lax;\n"
    f"    X509ExtKeyUsage key_usages[{n_eku}];\n"
    "    Int n_key_usages;\n"
    "    int expect;\n"
    "    const char *expect_msg;\n"
    f"    const char *chains[{n_chain}][{n_link}];\n"
    "} X509VerifyCase;\n"
)


def cert_init(ref):
    if isinstance(ref, tuple):
        count, san, at = ref
        return f"{{NULL, {count}, {q(san)}, {at}}}"
    return f"{{{ref}, 0, NULL, 0}}"


def brace(items):
    # An empty {} is C23, so an empty list is {0}.
    return "{" + (", ".join(items) if items else "0") + "}"


rows = []
for c in cases:
    chains = [brace([q(x) for x in ch]) for ch in c["chains"]]
    rows.append(
        "    {\n"
        f"        {q(c['name'])},\n"
        f"        {cert_init(c['leaf'])},\n"
        f"        {brace([cert_init(r) for r in c['intermediates']])},\n"
        f"        {brace([cert_init(r) for r in c['roots']])},\n"
        f"        {c['current_time']},\n"
        f"        {q(c['dns_name'])},\n"
        f"        {'true' if c['system_skip'] else 'false'},\n"
        f"        {'true' if c['system_lax'] else 'false'},\n"
        f"        {brace(c['key_usages'])},\n"
        f"        {len(c['key_usages'])},\n"
        f"        {c['expect']},\n"
        f"        {q(c['expect_msg']) if c['expect_msg'] is not None else 'NULL'},\n"
        f"        {brace(chains)},\n"
        "    },\n"
    )
defs.append("static const X509VerifyCase verify_tests[] = {\n" + "".join(rows) + "};\n")

uae = one(r"\nvar unknownAuthorityErrorTests = \[\]struct \{.*?\n\}\{(.*?)\n\}\n", verify_test)
urows = []
for e in entries(uae):
    m = re.fullmatch(r'"([^"]*)", (\w+), ("(?:[^"\\]|\\.)*")', e.strip())
    assert m, e
    urows.append(f"    {{{q(m.group(1))}, {pems[m.group(2)]}, {m.group(3)}}},\n")
assert len(urows) == 3
defs.append(
    "typedef struct X509UnknownAuthorityCase {\n"
    "    const char *name;\n"
    "    const char *cert;\n"
    "    const char *expected;\n"
    "} X509UnknownAuthorityCase;\n\n"
    "static const X509UnknownAuthorityCase unknown_authority_error_tests[] = {\n"
    + "".join(urows)
    + "};\n"
)

# The testdata certificates TestPoliciesValid and
# TestInvalidPolicyWithAnyKeyUsage read, as policy_root_pem and so on.
for path in sorted(set(re.findall(r'"testdata/(policy_[a-z_0-9]+)\.pem"', verify_test))):
    with open(f"{src}/testdata/{path}.pem") as f:
        value = f.read()
    assert len(value) <= 4000, path
    defs.append(f"static const char {path}_pem[] =\n{c_str(value)};\n")

# matchHostnamesTests and the certificate TestSystemCertPool appends, from
# x509_test.go.
with open(f"{src}/x509_test.go") as f:
    x509_test = f.read()
mrows = re.findall(
    r'\{("[^"\\]*"), ("[^"\\]*"), (true|false)\},',
    one(r"\nvar matchHostnamesTests = \[\]matchHostnamesTest\{\n(.*?)\n\}\n", x509_test),
)
assert len(mrows) == 38, len(mrows)
defs.append(
    "typedef struct X509MatchHostnamesTest {\n"
    "    const char *pattern, *host;\n"
    "    bool ok;\n"
    "} X509MatchHostnamesTest;\n\n"
    "static const X509MatchHostnamesTest match_hostnames_tests[] = {\n"
    + "".join(f"    {{{p}, {h}, {ok}}},\n" for p, h, ok in mrows)
    + "};\n"
)
pool = one(r"\nfunc TestSystemCertPool\(t \*testing.T\) \{(.*?)\n\}\n", x509_test)
defs.append(
    "static const char system_cert_pool_pem[] =\n"
    + c_str(one(r"AppendCertsFromPEM\(\[\]byte\(`(.*?)\t`\)\)", pool))
    + ";\n"
)

with open(out, "w") as f:
    f.write(
        "/* Generated by tools/gen-x509-tests.sh from Go's crypto/x509 tests. Do not\n"
        " * edit.\n"
        " *\n"
        " * Copyright 2011 The Go Authors. All rights reserved.\n"
        " * Copyright 2026 The burrow Authors. All rights reserved.\n"
        " * Use of this source code is governed by a BSD-style licence that can be found\n"
        " * in the LICENSE file. */\n\n"
        "#ifndef BURROW_TESTS_X509_VERIFY_TEST_GEN_H\n#define BURROW_TESTS_X509_VERIFY_TEST_GEN_H\n\n"
        '#include "burrow/crypto/x509.h"\n\n#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n\n'
    )
    f.write("\n".join(defs))
    f.write("\n#endif\n")
PY

# The nameConstraintsTests and rfc2821Tests tables in name_constraints_test.go,
# for tests/x509_name_constraints_test.c. The Go composite literals are read
# with a small parser that knows only the shapes those two tables use.
python3 - "$goroot/src/crypto/x509" "$root/tests/x509_name_constraints_test_gen.h" <<'PY'
import re
import sys

src, out = sys.argv[1], sys.argv[2]
with open(f"{src}/name_constraints_test.go", encoding="utf-8") as f:
    text = f.read()


def tokens(s):
    i = 0
    toks = []
    while i < len(s):
        c = s[i]
        if c in " \t\n":
            i += 1
        elif s.startswith("//", i):
            i = s.index("\n", i)
        elif c == '"':
            j = i + 1
            while s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            toks.append(("str", s[i : j + 1]))
            i = j + 1
        elif c.isalnum() or c == "_":
            m = re.compile(r"\w+").match(s, i)
            toks.append(("id", m.group(0)))
            i = m.end()
        elif c in "{}[](),:":
            toks.append(("p", c))
            i += 1
        else:
            raise SystemExit(f"gen-x509-tests: unexpected {c!r} in name_constraints_test.go")
    return toks


def go_string(lit):
    """The bytes of a Go interpreted string literal."""
    body = lit[1:-1]
    out = bytearray()
    i = 0
    simple = {'"': b'"', "\\": b"\\", "n": b"\n", "t": b"\t"}
    while i < len(body):
        c = body[i]
        if c == "\\":
            e = body[i + 1]
            if e in simple:
                out += simple[e]
                i += 2
            elif e == "x":
                out.append(int(body[i + 2 : i + 4], 16))
                i += 4
            else:
                raise SystemExit(f"gen-x509-tests: escape \\{e} in {lit}")
        else:
            out += c.encode("utf-8")
            i += 1
    return bytes(out)


class Parser:
    def __init__(self, toks):
        self.t = toks
        self.i = 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else (None, None)

    def take(self, want=None):
        tok = self.t[self.i]
        if want is not None and tok[1] != want:
            raise SystemExit(f"gen-x509-tests: want {want!r}, got {tok!r}")
        self.i += 1
        return tok

    def type_(self):
        depth = 0
        while self.peek()[1] == "[":
            self.take("[")
            self.take("]")
            depth += 1
        return depth, self.take()[1]

    def value(self):
        kind, v = self.peek()
        if kind == "str":
            self.take()
            return go_string(v)
        if v == "make":
            self.take()
            self.take("(")
            self.type_()
            self.take(",")
            n = int(self.take()[1])
            self.take(")")
            return [{}] * n
        if v in ("true", "false"):
            self.take()
            return v == "true"
        if v == "{":
            return self.composite()
        if v == "[" or (kind == "id" and self.peek(1)[1] == "{"):
            self.type_()
            return self.composite()
        if kind == "id":
            self.take()
            return v
        raise SystemExit(f"gen-x509-tests: unexpected {v!r}")

    def composite(self):
        self.take("{")
        items, keyed = [], {}
        while self.peek()[1] != "}":
            if self.peek()[0] == "id" and self.peek(1)[1] == ":":
                k = self.take()[1]
                self.take(":")
                keyed[k] = self.value()
            else:
                items.append(self.value())
            if self.peek()[1] == ",":
                self.take(",")
        self.take("}")
        if keyed and items:
            raise SystemExit("gen-x509-tests: mixed composite literal")
        return keyed if keyed or not items else items


def table(name):
    m = re.search(r"\nvar " + name + r" = (.*?)\n}\n", text, re.S)
    if m is None:
        raise SystemExit(f"gen-x509-tests: no {name} in name_constraints_test.go")
    p = Parser(tokens(m.group(1) + "\n}"))
    if p.peek(2)[1] != "struct":
        p.type_()
    else:
        p.take("[")
        p.take("]")
        p.take("struct")
        p.take("{")
        while p.peek()[1] != "}":
            p.take()
        p.take("}")
    v = p.composite()
    if p.i != len(p.t):
        raise SystemExit(f"gen-x509-tests: trailing tokens after {name}")
    return v


def c_str(b):
    if b is None:
        return "NULL"
    s = '"'
    prev = ""
    for ch in b:
        c = chr(ch)
        if c in '"\\':
            e = "\\" + c
        elif c == "?" and prev == "?":
            e = "\\?"
        elif 32 <= ch < 127:
            e = c
        else:
            e = "\\%03o" % ch
        s += e
        prev = c
    return s + '"'


tests = table("nameConstraintsTests")
rfc = table("rfc2821Tests")
known = {"name", "roots", "intermediates", "leaf", "requestedEKUs", "expectedError", "noOpenSSL"}
for t in tests:
    extra = set(t) - known
    if extra:
        raise SystemExit(f"gen-x509-tests: unhandled fields {extra}")

specs = [s for t in tests for s in t.get("roots", [])]
specs += [s for t in tests for lvl in t.get("intermediates", []) for s in lvl]
max_names = max(
    [len(s.get(k, [])) for s in specs for k in ("ok", "bad", "ekus")]
    + [len(t["leaf"].get(k, [])) for t in tests for k in ("sans", "ekus")]
)
max_roots = max(len(t.get("roots", [])) for t in tests)
max_levels = max(len(t.get("intermediates", [])) for t in tests)
max_per_level = max([len(l) for t in tests for l in t.get("intermediates", [])] + [1])
max_ekus = max([len(t.get("requestedEKUs", [])) for t in tests] + [1])


def names(v):
    v = list(v or [])
    if not v:
        return "{0}"
    return "{" + ", ".join(c_str(x) for x in v) + "}"


def spec(s):
    return f"{{{names(s.get('ok'))}, {names(s.get('bad'))}, {names(s.get('ekus'))}}}"


def snake(n):
    return "X509_" + re.sub(r"(?<!^)([A-Z])", r"_\1", n).upper()


rows = []
for t in tests:
    roots = t.get("roots", [])
    inter = t.get("intermediates", [])
    leaf = t["leaf"]
    req = t.get("requestedEKUs", [])
    level_lens = "{" + ", ".join(str(len(l)) for l in inter) + "}" if inter else "{0}"
    levels = (
        "{" + ", ".join("{" + ", ".join(spec(s) for s in l) + "}" if l else "{0}" for l in inter) + "}"
        if inter
        else "{0}"
    )
    rows.append(
        "    {\n"
        f"        .name = {c_str(t['name'])},\n"
        f"        .nroots = {len(roots)},\n"
        f"        .roots = {'{' + ', '.join(spec(s) for s in roots) + '}' if roots else '{0}'},\n"
        f"        .nlevels = {len(inter)},\n"
        f"        .level_len = {level_lens},\n"
        f"        .intermediates = {levels},\n"
        f"        .leaf = {{{names(leaf.get('sans'))}, {names(leaf.get('ekus'))}, {c_str(leaf.get('cn'))}}},\n"
        f"        .nrequested = {len(req)},\n"
        f"        .requested_ekus = {{{', '.join(snake(e) for e in req) if req else '0'}}},\n"
        f"        .expected_error = {c_str(t.get('expectedError'))},\n"
        "    },\n"
    )

rfc_rows = "".join(
    f"    {{{c_str(r[0])}, {c_str(r[1])}, {c_str(r[2])}}},\n" for r in rfc
)
with open(out, "w") as f:
    f.write(
        "/* Generated by tools/gen-x509-tests.sh from Go's crypto/x509 tests. Do not\n"
        " * edit.\n"
        " *\n"
        " * Copyright 2017 The Go Authors. All rights reserved.\n"
        " * Copyright 2026 The burrow Authors. All rights reserved.\n"
        " * Use of this source code is governed by a BSD-style licence that can be found\n"
        " * in the LICENSE file. */\n\n"
        "#ifndef BURROW_TESTS_X509_NAME_CONSTRAINTS_TEST_GEN_H\n"
        "#define BURROW_TESTS_X509_NAME_CONSTRAINTS_TEST_GEN_H\n\n"
        '#include "burrow/crypto/x509.h"\n\n#include <stddef.h>\n\n'
        f"#define X509_NC_MAX_NAMES {max_names + 1}\n"
        f"#define X509_NC_MAX_ROOTS {max_roots}\n"
        f"#define X509_NC_MAX_LEVELS {max_levels}\n"
        f"#define X509_NC_MAX_PER_LEVEL {max_per_level}\n"
        f"#define X509_NC_MAX_EKUS {max_ekus}\n\n"
        "/* constraintsSpec, each list ending at the first NULL. */\n"
        "typedef struct X509ConstraintsSpec {\n"
        "    const char *ok[X509_NC_MAX_NAMES];\n"
        "    const char *bad[X509_NC_MAX_NAMES];\n"
        "    const char *ekus[X509_NC_MAX_NAMES];\n"
        "} X509ConstraintsSpec;\n\n"
        "/* leafSpec */\n"
        "typedef struct X509LeafSpec {\n"
        "    const char *sans[X509_NC_MAX_NAMES];\n"
        "    const char *ekus[X509_NC_MAX_NAMES];\n"
        "    const char *cn;\n"
        "} X509LeafSpec;\n\n"
        "/* nameConstraintsTest, less noOpenSSL and ignoreCN, which only matter when\n"
        " * the chains are also checked with OpenSSL. */\n"
        "typedef struct X509NameConstraintsTest {\n"
        "    const char *name;\n"
        "    Int nroots;\n"
        "    X509ConstraintsSpec roots[X509_NC_MAX_ROOTS];\n"
        "    Int nlevels;\n"
        "    Int level_len[X509_NC_MAX_LEVELS];\n"
        "    X509ConstraintsSpec intermediates[X509_NC_MAX_LEVELS][X509_NC_MAX_PER_LEVEL];\n"
        "    X509LeafSpec leaf;\n"
        "    Int nrequested;\n"
        "    X509ExtKeyUsage requested_ekus[X509_NC_MAX_EKUS];\n"
        "    const char *expected_error;\n"
        "} X509NameConstraintsTest;\n\n"
        "static const X509NameConstraintsTest name_constraints_tests[] = {\n"
        + "".join(rows)
        + "};\n\n"
        "/* rfc2821Tests */\n"
        "typedef struct X509RFC2821Test {\n"
        "    const char *in;\n"
        "    const char *local_part;\n"
        "    const char *domain;\n"
        "} X509RFC2821Test;\n\n"
        "static const X509RFC2821Test rfc2821_tests[] = {\n" + rfc_rows + "};\n\n#endif\n"
    )
PY

# The certificates root_test.go reads: test-file.crt in the package directory,
# testdata/test-dir.crt, and gtsRoot and googleLeaf from verify_test.go, for
# tests/x509_root_test.c.
python3 - "$goroot/src/crypto/x509" "$root/tests/x509_root_test_gen.h" <<'PY'
import re
import sys

src, out = sys.argv[1], sys.argv[2]


def read(name):
    with open(f"{src}/{name}") as f:
        return f.read()


def c_str(s, indent="    "):
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
    for p in pieces:
        assert '"' not in p and "\\" not in p.replace("\\n", "")
    return "\n".join(f'{indent}"{p}"' for p in pieces)


verify_test = read("verify_test.go")
defs = []
for cname, value in [
    ("test_file_crt", read("test-file.crt")),
    ("test_dir_crt", read("testdata/test-dir.crt")),
    ("gts_root", re.findall(r"\nconst gtsRoot = `(.*?)`", verify_test, re.S)),
    ("google_leaf", re.findall(r"\nconst googleLeaf = `(.*?)`", verify_test, re.S)),
]:
    if isinstance(value, list):
        assert len(value) == 1, cname
        value = value[0]
    assert len(value) <= 4000, cname
    defs.append(f"static const char {cname}[] =\n{c_str(value)};\n")

with open(out, "w") as f:
    f.write(
        "/* Generated by tools/gen-x509-tests.sh from Go's crypto/x509 tests. Do not\n"
        " * edit.\n"
        " *\n"
        " * Copyright 2017 The Go Authors. All rights reserved.\n"
        " * Copyright 2026 The burrow Authors. All rights reserved.\n"
        " * Use of this source code is governed by a BSD-style licence that can be found\n"
        " * in the LICENSE file. */\n\n"
        "#ifndef BURROW_TESTS_X509_ROOT_TEST_GEN_H\n#define BURROW_TESTS_X509_ROOT_TEST_GEN_H\n\n"
    )
    f.write("\n".join(defs))
    f.write("\n#endif\n")
PY
