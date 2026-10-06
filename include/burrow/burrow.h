/* burrow: the Go standard library, in C.
 *
 * This is the umbrella header for the split form of the tree, the one you use
 * when you have checked the repository out. If you downloaded the amalgamation
 * you have a single burrow.h at the top of your project and you do not need
 * this file, because the generator produced that one from these.
 *
 * Include what you need instead of this, if you care about compile time:
 *
 *     #include "burrow/strings.h"
 *     #include "burrow/net/http.h"
 *
 * Two things to know before you read any other header.
 *
 * First, there is no prefix on anything you call. strings.Contains is
 * strings_contains, not burrow_strings_contains, because the package segment is
 * already a namespace and a second one on top of it would be noise repeated
 * 23,730 times. Types are CamelCase, functions are snake_case, and macros keep a
 * BURROW_ prefix because the preprocessor ignores every scoping mechanism C has.
 *
 * Second, every function that can allocate takes an allocator as its first
 * parameter. No exceptions, no hidden global, no per-object free function to
 * remember. Make an arena, pass it down, free it once.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Slice parts = strings_split(a, path, S("/"));
 *     arena_free(&ar);
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_H
#define BURROW_H

#include "burrow/platform.h"
#include "burrow/version.h"

#include "burrow/archive/tar.h"
#include "burrow/archive/zip.h"
#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/cmp.h"
#include "burrow/compress/bzip2.h"
#include "burrow/compress/flate.h"
#include "burrow/compress/gzip.h"
#include "burrow/compress/lzw.h"
#include "burrow/compress/zlib.h"
#include "burrow/container/heap.h"
#include "burrow/container/list.h"
#include "burrow/container/ring.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/des.h"
#include "burrow/crypto/dsa.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/hkdf.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/hpke.h"
#include "burrow/crypto/md5.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/mlkem/mlkemtest.h"
#include "burrow/crypto/pbkdf2.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/rc4.h"
#include "burrow/crypto/sha1.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha3.h"
#include "burrow/crypto/sha512.h"
#include "burrow/crypto/subtle.h"
#include "burrow/declare.h"
#include "burrow/defer.h"
#include "burrow/embed.h"
#include "burrow/encoding.h"
#include "burrow/encoding/ascii85.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/base32.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/binary.h"
#include "burrow/encoding/csv.h"
#include "burrow/encoding/gob.h"
#include "burrow/encoding/hex.h"
#include "burrow/encoding/json.h"
#include "burrow/encoding/json/jsontext.h"
#include "burrow/encoding/json/v2.h"
#include "burrow/encoding/pem.h"
#include "burrow/encoding/xml.h"
#include "burrow/error.h"
#include "burrow/flag.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/hash.h"
#include "burrow/hash/adler32.h"
#include "burrow/hash/crc32.h"
#include "burrow/hash/crc64.h"
#include "burrow/hash/fnv.h"
#include "burrow/hash/maphash.h"
#include "burrow/iface.h"
#include "burrow/image.h"
#include "burrow/image/color.h"
#include "burrow/image/color/palette.h"
#include "burrow/image/draw.h"
#include "burrow/image/gif.h"
#include "burrow/image/jpeg.h"
#include "burrow/image/png.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/iter.h"
#include "burrow/map.h"
#include "burrow/maps.h"
#include "burrow/math.h"
#include "burrow/math/big.h"
#include "burrow/math/bits.h"
#include "burrow/math/cmplx.h"
#include "burrow/math/rand.h"
#include "burrow/math/rand/v2.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/gc.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"
#include "burrow/mime.h"
#include "burrow/mime/multipart.h"
#include "burrow/mime/quotedprintable.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/num.h"
#include "burrow/os.h"
#include "burrow/os/exec.h"
#include "burrow/os/signal.h"
#include "burrow/os/user.h"
#include "burrow/own.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/path/filepath.h"
#include "burrow/proc.h"
#include "burrow/regexp.h"
#include "burrow/regexp/syntax.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/structs.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/synctest.h"
#include "burrow/syscall.h"
#include "burrow/testing.h"
#include "burrow/testing/fstest.h"
#include "burrow/testing/iotest.h"
#include "burrow/text/scanner.h"
#include "burrow/text/tabwriter.h"
#include "burrow/time.h"
#include "burrow/time/tzdata.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/unicode/utf16.h"
#include "burrow/unique.h"
#include "burrow/utf8.h"
#include "burrow/uuid.h"
#include "burrow/weak.h"

/* The rest of the library arrives here as it is written. The order is the
 * construction order from docs/design/06-runtime.md section 12, because the
 * headers have the same dependency shape the code does:
 *
 *   core types, allocators, runtime, reflect, then the packages in tier order.
 *
 * Allocators are done, and so are the core types: Str, the type descriptor,
 * Slice, Error, Map, the interface machinery, function values, the numeric
 * operations, and now runes and the UTF-8 decode path underneath them.
 * io.Reader and io.Writer are here as the first interfaces built on it, with
 * nothing behind them yet: os and bytes are what fill them in.
 *
 * The runtime is next, and then reflect. See the milestone issues. */

#endif /* BURROW_H */
