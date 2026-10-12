/* The small parsers the resolver reads its files with.
 *
 * Derived from Go's src/net/parse.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/slice.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* Go's open reads through a buffer of 64 KiB, and a line longer than that
 * comes back in pieces, the same here. */
enum { NP_BUF = 64 * 1024 };

burrow__NetFile *burrow__net_open(Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    OsFile *fd = os_open(a, name, err);
    if (fd == NULL)
        return NULL;
    burrow__NetFile *f = (burrow__NetFile *)mem_alloc(a, sizeof(burrow__NetFile),
                                                      _Alignof(burrow__NetFile));
    Byte *data = (Byte *)mem_alloc_nozero(a, NP_BUF, 1);
    if (f == NULL || data == NULL) {
        os_file_free(fd);
        if (f != NULL)
            mem_free(a, f, sizeof(burrow__NetFile), _Alignof(burrow__NetFile));
        if (data != NULL)
            mem_free(a, data, NP_BUF, 1);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    f->file = fd;
    f->data = data;
    f->start = 0;
    f->end = 0;
    f->at_eof = false;
    return f;
}

void burrow__net_file_close(burrow__NetFile *f, Alloc *a) {
    if (f == NULL)
        return;
    os_file_free(f->file);
    mem_free(a, f->data, NP_BUF, 1);
    mem_free(a, f, sizeof(burrow__NetFile), _Alignof(burrow__NetFile));
}

/* getLineFromData. Go moves what is left to the front of the buffer after
 * every line. This leaves it where it is until a read needs the room, so the
 * line can point into the buffer until the next call. */
static bool np_line_from_data(burrow__NetFile *f, Str *s) {
    const Byte *p = f->data + f->start;
    Int n = f->end - f->start;
    const Byte *nl = n > 0 ? (const Byte *)memchr(p, '\n', (size_t)n) : NULL;
    if (nl != NULL) {
        Int i = (Int)(nl - p);
        *s = str_from_bytes(p, i);
        f->start += i + 1;
        return true;
    }
    if (f->at_eof && n > 0) {
        *s = str_from_bytes(p, n);
        f->start = f->end;
        return true;
    }
    return false;
}

bool burrow__net_file_read_line(burrow__NetFile *f, Str *line) {
    *line = BURROW_STR_EMPTY;
    if (np_line_from_data(f, line))
        return true;
    if (f->start > 0) {
        Int n = f->end - f->start;
        if (n > 0)
            memmove(f->data, f->data + f->start, (size_t)n);
        f->start = 0;
        f->end = n;
    }
    if (f->end < NP_BUF) {
        Error err = BURROW_NO_ERROR;
        Int n = io_read_full(
            os_file_as_io_reader(f->file),
            slice_from(f->data + f->end, NP_BUF - f->end, NP_BUF - f->end, TYPE_BYTE),
            &err);
        if (n >= 0)
            f->end += n;
        if (errors_is(err, io_eof) || errors_is(err, io_err_unexpected_eof))
            f->at_eof = true;
    }
    return np_line_from_data(f, line);
}

/* countAnyByte and splitAtBytes. */
static bool np_any(Byte c, Str t) {
    return t.len > 0 && memchr(t.p, c, (size_t)t.len) != NULL;
}

Slice burrow__net_split_at_bytes(Alloc *a, Str s, Str t) {
    Int count = 0;
    for (Int i = 0; i < s.len; i++)
        if (np_any(s.p[i], t))
            count++;
    Slice out = slice_make(a, TYPE_STRING, 0, 1 + count);
    if (out.p == NULL)
        return out;
    Int last = 0;
    for (Int i = 0; i < s.len; i++) {
        if (np_any(s.p[i], t)) {
            if (last < i) {
                Str f = str_from_bytes(s.p + last, i - last);
                out = slice_append(a, out, &f, 1);
            }
            last = i + 1;
        }
    }
    if (last < s.len) {
        Str f = str_from_bytes(s.p + last, s.len - last);
        out = slice_append(a, out, &f, 1);
    }
    return out;
}

Slice burrow__net_get_fields(Alloc *a, Str s) {
    return burrow__net_split_at_bytes(a, s, BURROW_S(" \r\t\n"));
}

bool burrow__net_dtoi(Str s, Int *n, Int *used) {
    Int v = 0;
    Int i = 0;
    for (; i < s.len && s.p[i] >= '0' && s.p[i] <= '9'; i++) {
        v = v * 10 + (s.p[i] - '0');
        if (v >= BURROW__NET_BIG) {
            *n = BURROW__NET_BIG;
            *used = i;
            return false;
        }
    }
    *n = v;
    *used = i;
    return i != 0;
}

bool burrow__net_has_upper_case(Str s) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] >= 'A' && s.p[i] <= 'Z')
            return true;
    return false;
}

void burrow__net_lower_ascii_bytes(Byte *x, Int n) {
    for (Int i = 0; i < n; i++)
        if (x[i] >= 'A' && x[i] <= 'Z')
            x[i] = (Byte)(x[i] + ('a' - 'A'));
}

static Byte np_lower(Byte b) {
    if (b >= 'A' && b <= 'Z')
        return (Byte)(b + ('a' - 'A'));
    return b;
}

bool burrow__net_equal_fold(Str s, Str t) {
    if (s.len != t.len)
        return false;
    for (Int i = 0; i < s.len; i++)
        if (np_lower(s.p[i]) != np_lower(t.p[i]))
            return false;
    return true;
}

bool burrow__net_has_suffix_fold(Str s, Str suffix) {
    return s.len >= suffix.len &&
           burrow__net_equal_fold(str_from_bytes(s.p + s.len - suffix.len, suffix.len),
                                  suffix);
}

Str burrow__net_cat(Alloc *a, int n, ...) {
    va_list ap;
    va_start(ap, n);
    Int total = 0;
    for (int i = 0; i < n; i++)
        total += va_arg(ap, Str).len;
    va_end(ap);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)total + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Int off = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        Str s = va_arg(ap, Str);
        if (s.len > 0)
            memcpy(p + off, s.p, (size_t)s.len);
        off += s.len;
    }
    va_end(ap);
    p[off] = 0;
    return str_from_bytes(p, total);
}

bool burrow__net_godebug(const char *key, Str *val) {
    const char *env = NULL;
    for (const char *const *e = pal_environ(); e != NULL && *e != NULL; e++) {
        if (strncmp(*e, "GODEBUG=", 8) == 0) {
            env = *e + 8;
            break;
        }
    }
    if (env == NULL)
        return false;
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
