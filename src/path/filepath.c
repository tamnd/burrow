/* Derived from Go's src/path/filepath/path.go, path_unix.go, path_windows.go
 * and match.go, and from src/internal/filepathlite/path.go, path_unix.go,
 * path_windows.go and path_nonwindows.go, which is where most of filepath
 * lives now. Go source: go1.27.1.
 *
 * Abs, EvalSymlinks, Glob, Walk and WalkDir look at the file system, and are
 * in filepath_os.c. Everything else is here, for both kinds of system: each
 * function takes win, and the public ones pass the host's. See
 * filepath_internal.h.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/path/filepath.h"
#include "filepath_internal.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdarg.h>
#include <string.h>

#if defined(BURROW_OS_WINDOWS)
#define FP_HOST true
#else
#define FP_HOST false
#endif

BURROW_SENTINEL_ERROR(filepath_err_bad_pattern, "syntax error in pattern");
BURROW_SENTINEL_ERROR(burrow__filepath_err_invalid_path, "invalid path");

static Str fp_str(const Byte *p, Int n) {
    Str s = {p, n};
    return s;
}

/* s[i:j]. The empty string can have a NULL p, and NULL + 0 is undefined. */
static Str fp_sub(Str s, Int i, Int j) {
    return fp_str(s.p == NULL ? NULL : s.p + i, j - i);
}

static Str fp_tail(Str s, Int i) {
    return fp_sub(s, i, s.len);
}

static Byte fp_sep(bool win) {
    return win ? '\\' : '/';
}

static bool fp_is_sep(Byte c, bool win) {
    return c == '/' || (win && c == '\\');
}

static bool fp_eq(Str s, const char *lit) {
    size_t n = strlen(lit);
    return (size_t)s.len == n && (n == 0 || memcmp(s.p, lit, n) == 0);
}

static bool fp_has_prefix(Str s, const char *lit) {
    size_t n = strlen(lit);
    return (size_t)s.len >= n && memcmp(s.p, lit, n) == 0;
}

static Byte fp_upper(Byte c) {
    return c >= 'a' && c <= 'z' ? (Byte)(c - ('a' - 'A')) : c;
}

/* An allocation a result may live in, so that a caller who only wanted a look
 * at it can give it back. p is NULL when the result borrows its input. */
typedef struct FpOwn {
    Byte *p;
    Int n;
} FpOwn;

static void fp_own_free(Alloc *a, FpOwn own) {
    if (own.p != NULL)
        mem_free(a, own.p, (size_t)own.n, 1);
}

/* s with every old byte replaced by new, in a fresh copy if there is one to
 * replace and s itself if there is not. */
static Str fp_replace_byte(Alloc *a, Str s, Byte old, Byte new_, FpOwn *own) {
    if (own != NULL)
        own->p = NULL;
    if (s.len == 0 || memchr(s.p, old, (size_t)s.len) == NULL)
        return s;
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
    if (b == NULL)
        return BURROW_STR_EMPTY;
    for (Int i = 0; i < s.len; i++)
        b[i] = s.p[i] == old ? new_ : s.p[i];
    if (own != NULL)
        *own = (FpOwn){b, s.len};
    return fp_str(b, s.len);
}

/* x + y in a fresh allocation, or one of them alone when the other is empty,
 * which is how Go's string concatenation behaves. */
static Str fp_concat(Alloc *a, Str x, Str y, FpOwn *own) {
    if (own != NULL)
        own->p = NULL;
    if (x.len == 0)
        return y;
    if (y.len == 0)
        return x;
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)(x.len + y.len), 1);
    if (b == NULL)
        return BURROW_STR_EMPTY;
    memcpy(b, x.p, (size_t)x.len);
    memcpy(b + x.len, y.p, (size_t)y.len);
    if (own != NULL)
        *own = (FpOwn){b, x.len + y.len};
    return fp_str(b, x.len + y.len);
}

/* ------------------------------------------------------------ volume names */

/* cutPath: path around its first separator. */
static bool fp_cut_path(Str path, Str *before, Str *after, bool win) {
    for (Int i = 0; i < path.len; i++) {
        if (fp_is_sep(path.p[i], win)) {
            *before = fp_str(path.p, i);
            *after = fp_tail(path, i + 1);
            return true;
        }
    }
    *before = path;
    *after = BURROW_STR_EMPTY;
    return false;
}

/* pathHasPrefixFold: s starts with prefix, ignoring ASCII case and treating
 * both separators alike, and if s is longer the next byte is a separator. */
static bool fp_has_prefix_fold(Str s, const char *prefix) {
    Int n = (Int)strlen(prefix);
    if (s.len < n)
        return false;
    for (Int i = 0; i < n; i++) {
        Byte c = (Byte)prefix[i];
        if (fp_is_sep(c, true)) {
            if (!fp_is_sep(s.p[i], true))
                return false;
        } else if (fp_upper(c) != fp_upper(s.p[i])) {
            return false;
        }
    }
    return !(s.len > n && !fp_is_sep(s.p[n], true));
}

/* uncLen: the length of the volume of a UNC path, whose host starts at
 * prefix_len. */
static Int fp_unc_len(Str path, Int prefix_len) {
    Int count = 0;
    for (Int i = prefix_len; i < path.len; i++) {
        if (fp_is_sep(path.p[i], true)) {
            count++;
            if (count == 2)
                return i;
        }
    }
    return path.len;
}

/* validVolumeNameLen: n, unless path[:n] has a ".." element in it. */
static Int fp_valid_volume_name_len(Str path, Int n) {
    Str p = fp_str(path.p, n);
    while (p.len > 0) {
        Str part;
        fp_cut_path(p, &part, &p, true);
        if (fp_eq(part, ".."))
            return 0;
    }
    return n;
}

/* volumeNameLen, which is always 0 outside Windows. */
static Int fp_volume_name_len(Str path, bool win) {
    if (!win)
        return 0;
    if (path.len >= 2 && path.p[1] == ':') {
        /* A drive letter. Like Go, any byte will do as the letter. */
        return 2;
    }
    if (path.len == 0 || !fp_is_sep(path.p[0], true))
        return 0;
    if (fp_has_prefix_fold(path, "\\\\.") || fp_has_prefix_fold(path, "\\\\?") ||
        fp_has_prefix_fold(path, "\\??")) {
        /* A device path: \\.\ for a local device, \\?\ or \??\ for a root
         * local device. */
        if (path.len == 3)
            return 3;
        if (fp_has_prefix_fold(fp_tail(path, 4), "UNC"))
            return fp_valid_volume_name_len(path, fp_unc_len(path, 8));
        /* The element after the prefix counts as part of the volume, so that
         * cleaning \\?\c:\ keeps its last backslash. */
        Str first, rest;
        if (!fp_cut_path(fp_tail(path, 4), &first, &rest, true))
            return fp_valid_volume_name_len(path, path.len);
        return fp_valid_volume_name_len(path, path.len - rest.len - 1);
    }
    if (path.len >= 2 && fp_is_sep(path.p[1], true))
        return fp_valid_volume_name_len(path, fp_unc_len(path, 2));
    return 0;
}

/* ------------------------------------------------------------------ Clean */

/* Go's lazybuf. The output is read from path for as long as it is a prefix
 * of it, and only copied out once it differs. The copy lives in mem, with
 * room in front for the volume name and for the two bytes postClean may put
 * before it, so the finished path is one allocation and needs no second
 * copy. buf is mem + off and holds blen bytes, which is Go's len(out.buf):
 * like Go's it is zeroed, and postClean reads all of it and not only the
 * part before w. */
typedef struct FpBuf {
    Alloc *a;
    Str path;
    Int vol;
    Byte *mem;
    Int nmem;
    Int off;
    Int blen;
    Int w;
    bool oom;
} FpBuf;

static Byte fpbuf_index(const FpBuf *b, Int i) {
    return b->mem != NULL ? b->mem[b->off + i] : b->path.p[i];
}

static void fpbuf_append(FpBuf *b, Byte c) {
    if (b->mem == NULL) {
        if (b->w < b->path.len && b->path.p[b->w] == c) {
            b->w++;
            return;
        }
        if (b->oom)
            return;
        b->nmem = b->vol + 2 + b->path.len;
        b->mem = (Byte *)mem_alloc(b->a, (size_t)b->nmem, 1);
        if (b->mem == NULL) {
            b->oom = true;
            return;
        }
        b->off = b->vol + 2;
        b->blen = b->path.len;
        memcpy(b->mem + b->off, b->path.p, (size_t)b->w);
    }
    b->mem[b->off + b->w] = c;
    b->w++;
}

/* postClean, for Windows: a relative path must not come out of Clean as one
 * with a drive letter or a \??\ prefix. */
static void fp_post_clean(FpBuf *out) {
    if (out->vol != 0 || out->mem == NULL)
        return;
    const Byte *buf = out->mem + out->off;
    /* A colon in the first element would make a drive letter of it, as in
     * a/../c:, so put .\ in front. */
    for (Int i = 0; i < out->blen; i++) {
        if (fp_is_sep(buf[i], true))
            break;
        if (buf[i] == ':') {
            out->off -= 2;
            out->mem[out->off] = '.';
            out->mem[out->off + 1] = '\\';
            out->blen += 2;
            out->w += 2;
            return;
        }
    }
    /* And \a\..\??\c:\x would become \??\c:\x, which is c:\x, so put \. in
     * front of that. */
    if (out->blen >= 3 && fp_is_sep(buf[0], true) && buf[1] == '?' && buf[2] == '?') {
        out->off -= 2;
        out->mem[out->off] = '\\';
        out->mem[out->off + 1] = '.';
        out->blen += 2;
        out->w += 2;
    }
}

static Str fp_clean_own(Alloc *a, Str path, bool win, FpOwn *own) {
    Byte sep = fp_sep(win);
    Str original = path;
    own->p = NULL;
    Int vol = fp_volume_name_len(path, win);
    path = fp_tail(path, vol);
    if (path.len == 0) {
        if (vol > 1 && fp_is_sep(original.p[0], win) && fp_is_sep(original.p[1], win)) {
            /* A UNC volume and nothing else. */
            return fp_replace_byte(a, original, '/', '\\', own);
        }
        return fp_concat(a, original, BURROW_S("."), own);
    }
    bool rooted = fp_is_sep(path.p[0], win);

    /* r is the next byte of path to read and out.w the next to write.
     * dotdot is where a .. must stop backtracking, either because it is the
     * root or because it is the end of a leading run of ../../.. */
    Int n = path.len;
    const Byte *p = path.p;
    FpBuf out = {a, path, vol, NULL, 0, 0, 0, 0, false};
    Int r = 0, dotdot = 0;
    if (rooted) {
        fpbuf_append(&out, sep);
        r = 1;
        dotdot = 1;
    }

    while (r < n && !out.oom) {
        if (fp_is_sep(p[r], win)) {
            /* An empty element. */
            r++;
        } else if (p[r] == '.' && (r + 1 == n || fp_is_sep(p[r + 1], win))) {
            /* A . element. */
            r++;
        } else if (p[r] == '.' && p[r + 1] == '.' &&
                   (r + 2 == n || fp_is_sep(p[r + 2], win))) {
            /* A .. element: remove back to the last separator. */
            r += 2;
            if (out.w > dotdot) {
                out.w--;
                while (out.w > dotdot && !fp_is_sep(fpbuf_index(&out, out.w), win))
                    out.w--;
            } else if (!rooted) {
                /* Cannot backtrack, and not rooted, so keep the .. */
                if (out.w > 0)
                    fpbuf_append(&out, sep);
                fpbuf_append(&out, '.');
                fpbuf_append(&out, '.');
                dotdot = out.w;
            }
        } else {
            /* A real element. Add a separator if one is needed, then copy
             * it. */
            if ((rooted && out.w != 1) || (!rooted && out.w != 0))
                fpbuf_append(&out, sep);
            for (; r < n && !fp_is_sep(p[r], win); r++)
                fpbuf_append(&out, p[r]);
        }
    }

    /* The empty path is ".". */
    if (out.w == 0)
        fpbuf_append(&out, '.');
    if (out.oom)
        return BURROW_STR_EMPTY;
    if (win)
        fp_post_clean(&out);

    Str s;
    if (out.mem == NULL) {
        s = fp_str(original.p, vol + out.w);
    } else {
        Byte *start = out.mem + out.off - vol;
        memcpy(start, original.p, (size_t)vol);
        s = fp_str(start, vol + out.w);
        *own = (FpOwn){out.mem, out.nmem};
    }
    if (!win)
        return s;
    /* FromSlash, in place when the bytes are already ours. */
    if (own->p != NULL) {
        Byte *b = (Byte *)(Uintptr)s.p;
        for (Int i = 0; i < s.len; i++) {
            if (b[i] == '/')
                b[i] = '\\';
        }
        return s;
    }
    return fp_replace_byte(a, s, '/', '\\', own);
}

Str burrow__filepath_clean(Alloc *a, Str path, bool win) {
    FpOwn own;
    return fp_clean_own(a, path, win, &own);
}

/* ---------------------------------------------------------------- IsLocal */

static bool fp_equal_fold_ascii(Str a, const char *b) {
    if ((size_t)a.len != strlen(b))
        return false;
    for (Int i = 0; i < a.len; i++) {
        if (fp_upper(a.p[i]) != fp_upper((Byte)b[i]))
            return false;
    }
    return true;
}

/* isReservedBaseName. */
static bool fp_is_reserved_base_name(Str name) {
    if (name.len == 3) {
        if (fp_equal_fold_ascii(name, "CON") || fp_equal_fold_ascii(name, "PRN") ||
            fp_equal_fold_ascii(name, "AUX") || fp_equal_fold_ascii(name, "NUL"))
            return true;
    }
    if (name.len >= 4) {
        Str head = fp_str(name.p, 3);
        if (fp_equal_fold_ascii(head, "COM") || fp_equal_fold_ascii(head, "LPT")) {
            if (name.len == 4 && name.p[3] >= '1' && name.p[3] <= '9')
                return true;
            /* Superscript one, two and three count as digits too. */
            Str d = fp_tail(name, 3);
            return fp_eq(d, "\xc2\xb2") || fp_eq(d, "\xc2\xb3") || fp_eq(d, "\xc2\xb9");
        }
    }
    /* CONIN$ and CONOUT$ open the console the way CON does. */
    if (name.len == 6 && name.p[5] == '$' && fp_equal_fold_ascii(name, "CONIN$"))
        return true;
    if (name.len == 7 && name.p[6] == '$' && fp_equal_fold_ascii(name, "CONOUT$"))
        return true;
    return false;
}

/* isReservedName: whether name is a Windows device name. A device name can
 * have anything after a dot or a colon, and trailing spaces do not count.
 * Since Windows 11 a reserved name with an extension, such as CON.txt, can be
 * an ordinary file, so for those Go asks RtlIsDosDeviceName_U, and so does
 * pal_is_dos_device_name. Only Windows can answer that, and anywhere else the
 * answer is no, so the Windows rules run on another system (which only the
 * tests do) call a name like "nul." local where Windows would not. */
static bool fp_is_reserved_name(Str name) {
    Str base = name;
    for (Int i = 0; i < base.len; i++) {
        if (base.p[i] == ':' || base.p[i] == '.')
            base.len = i;
    }
    while (base.len > 0 && base.p[base.len - 1] == ' ')
        base.len--;
    if (!fp_is_reserved_base_name(base))
        return false;
    if (base.len == name.len)
        return true;
    return pal_is_dos_device_name((const char *)name.p, name.len);
}

/* unixIsLocal and the Windows isLocal. */
bool burrow__filepath_is_local(Str path, bool win) {
    if (path.len == 0)
        return false;
    bool has_dots = false;
    if (win) {
        /* Rooted in the current drive. */
        if (fp_is_sep(path.p[0], true))
            return false;
        /* A colon is only valid as a drive letter's, and refusing every
         * colon is conservative but safe. */
        if (memchr(path.p, ':', (size_t)path.len) != NULL)
            return false;
        for (Str p = path; p.len > 0;) {
            Str part;
            fp_cut_path(p, &part, &p, true);
            if (fp_eq(part, ".") || fp_eq(part, ".."))
                has_dots = true;
            if (fp_is_reserved_name(part))
                return false;
        }
    } else {
        if (path.p[0] == '/')
            return false;
        for (Str p = path; p.len > 0;) {
            Str part;
            fp_cut_path(p, &part, &p, false);
            if (fp_eq(part, ".") || fp_eq(part, "..")) {
                has_dots = true;
                break;
            }
        }
    }
    FpOwn own = {NULL, 0};
    Alloc *a = heap_allocator();
    if (has_dots)
        path = fp_clean_own(a, path, win, &own);
    bool local = !(fp_eq(path, "..") || fp_has_prefix(path, win ? "..\\" : "../"));
    fp_own_free(a, own);
    return local;
}

/* --------------------------------------------------------------- Localize */

Str burrow__filepath_localize(Alloc *a, Str path, bool win, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!fs_valid_path(path)) {
        BURROW_OUT(err, burrow__filepath_err_invalid_path);
        return BURROW_STR_EMPTY;
    }
    if (!win) {
        if (path.len > 0 && memchr(path.p, 0, (size_t)path.len) != NULL) {
            BURROW_OUT(err, burrow__filepath_err_invalid_path);
            return BURROW_STR_EMPTY;
        }
        return path;
    }
    for (Int i = 0; i < path.len; i++) {
        if (path.p[i] == ':' || path.p[i] == '\\' || path.p[i] == 0) {
            BURROW_OUT(err, burrow__filepath_err_invalid_path);
            return BURROW_STR_EMPTY;
        }
    }
    for (Str p = path; p.len > 0;) {
        const Byte *slash = (const Byte *)memchr(p.p, '/', (size_t)p.len);
        Str element = p;
        if (slash == NULL) {
            p = BURROW_STR_EMPTY;
        } else {
            element = fp_str(p.p, slash - p.p);
            p = fp_tail(p, element.len + 1);
        }
        if (fp_is_reserved_name(element)) {
            BURROW_OUT(err, burrow__filepath_err_invalid_path);
            return BURROW_STR_EMPTY;
        }
    }
    return fp_replace_byte(a, path, '/', '\\', NULL);
}

/* ------------------------------------------------------- slashes and lists */

Str burrow__filepath_to_slash(Alloc *a, Str path, bool win) {
    return win ? fp_replace_byte(a, path, '\\', '/', NULL) : path;
}

Str burrow__filepath_from_slash(Alloc *a, Str path, bool win) {
    return win ? fp_replace_byte(a, path, '/', '\\', NULL) : path;
}

/* strings.ReplaceAll(s, `"`, ``), which hands back s when there is nothing
 * to remove. */
static Str fp_remove_quotes(Alloc *a, Str s) {
    if (s.len == 0 || memchr(s.p, '"', (size_t)s.len) == NULL)
        return s;
    Int n = 0;
    for (Int i = 0; i < s.len; i++)
        n += s.p[i] != '"';
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (b == NULL)
        return BURROW_STR_EMPTY;
    Int w = 0;
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] != '"')
            b[w++] = s.p[i];
    }
    return fp_str(b, n);
}

Slice burrow__filepath_split_list(Alloc *a, Str path, bool win) {
    Byte lsep = win ? ';' : ':';
    if (path.len == 0)
        return slice_make(a, TYPE_STRING, 0, 0);
    /* Count first, so the Slice is made once at its final size. On Windows a
     * separator between double quotes does not count. */
    Int count = 1;
    bool quo = false;
    for (Int i = 0; i < path.len; i++) {
        if (win && path.p[i] == '"')
            quo = !quo;
        else if (path.p[i] == lsep && !quo)
            count++;
    }
    Slice list = slice_make(a, TYPE_STRING, count, count);
    if (list.p == NULL)
        return list;
    Str *out = (Str *)list.p;
    Int start = 0, k = 0;
    quo = false;
    for (Int i = 0; i < path.len; i++) {
        if (win && path.p[i] == '"') {
            quo = !quo;
        } else if (path.p[i] == lsep && !quo) {
            out[k++] = fp_str(path.p + start, i - start);
            start = i + 1;
        }
    }
    out[k] = fp_tail(path, start);
    if (win) {
        for (Int i = 0; i < count; i++)
            out[i] = fp_remove_quotes(a, out[i]);
    }
    return list;
}

/* ------------------------------------------------------ cutting paths up */

Str burrow__filepath_split(Str path, Str *file, bool win) {
    Int vol = fp_volume_name_len(path, win);
    Int i = path.len - 1;
    while (i >= vol && !fp_is_sep(path.p[i], win))
        i--;
    if (file != NULL)
        *file = fp_tail(path, i + 1);
    return fp_str(path.p, i + 1);
}

Str burrow__filepath_ext(Str path, bool win) {
    for (Int i = path.len - 1; i >= 0 && !fp_is_sep(path.p[i], win); i--) {
        if (path.p[i] == '.')
            return fp_tail(path, i);
    }
    return BURROW_STR_EMPTY;
}

Str burrow__filepath_base(Str path, bool win) {
    if (path.len == 0)
        return BURROW_S(".");
    /* Strip trailing separators. */
    while (path.len > 0 && fp_is_sep(path.p[path.len - 1], win))
        path.len--;
    /* Throw away the volume name. */
    path = fp_tail(path, fp_volume_name_len(path, win));
    /* Find the last element. */
    Int i = path.len - 1;
    while (i >= 0 && !fp_is_sep(path.p[i], win))
        i--;
    if (i >= 0)
        path = fp_tail(path, i + 1);
    /* If it is empty now, it was only separators. */
    if (path.len == 0)
        return win ? BURROW_S("\\") : BURROW_S("/");
    return path;
}

Str burrow__filepath_dir(Alloc *a, Str path, bool win) {
    Int vol = fp_volume_name_len(path, win);
    Int i = path.len - 1;
    while (i >= vol && !fp_is_sep(path.p[i], win))
        i--;
    FpOwn own;
    Str dir = fp_clean_own(a, fp_sub(path, vol, i + 1), win, &own);
    if (vol == 0)
        return dir;
    FpOwn vown;
    Str v = fp_replace_byte(a, fp_str(path.p, vol), '/', '\\', &vown);
    if (fp_eq(dir, ".") && vol > 2) {
        /* It must be UNC. */
        fp_own_free(a, own);
        return v;
    }
    Str r = fp_concat(a, v, dir, NULL);
    fp_own_free(a, own);
    fp_own_free(a, vown);
    return r;
}

bool burrow__filepath_is_abs(Str path, bool win) {
    if (!win)
        return path.len > 0 && path.p[0] == '/';
    Int l = fp_volume_name_len(path, true);
    if (l == 0)
        return false;
    /* A volume name that starts with two separators makes it absolute. */
    if (fp_is_sep(path.p[0], true) && fp_is_sep(path.p[1], true))
        return true;
    path = fp_tail(path, l);
    return path.len > 0 && fp_is_sep(path.p[0], true);
}

Str burrow__filepath_volume_name(Alloc *a, Str path, bool win) {
    return burrow__filepath_from_slash(a, fp_str(path.p, fp_volume_name_len(path, win)),
                                       win);
}

/* ------------------------------------------------------------------- Join */

/* Cleans the joined bytes in b, n of them out of an allocation of cap, and
 * gives b back unless the result lives in it. */
static Str fp_clean_joined(Alloc *a, Byte *b, Int n, Int cap, bool win) {
    FpOwn own;
    Str r = fp_clean_own(a, fp_str(b, n), win, &own);
    if (!(r.p >= b && r.p < b + cap))
        mem_free(a, b, (size_t)cap, 1);
    return r;
}

Str burrow__filepath_join(Alloc *a, Slice elem, bool win) {
    const Str *e = (const Str *)elem.p;
    Int size = 0;
    for (Int i = 0; i < elem.len; i++) {
        if (e[i].len > BURROW_INT_MAX - size - 2 * elem.len - 2)
            panic_str(BURROW_S("filepath: Join output length overflow"));
        size += e[i].len;
    }
    /* The most the separators and the .\ below can add. */
    Int cap = size + elem.len + 2;

    if (!win) {
        /* Everything from the first element that is not empty, joined. */
        Int first = 0;
        while (first < elem.len && e[first].len == 0)
            first++;
        if (first == elem.len)
            return BURROW_STR_EMPTY;
        Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
        if (b == NULL)
            return BURROW_STR_EMPTY;
        Int n = 0;
        for (Int i = first; i < elem.len; i++) {
            if (i > first)
                b[n++] = '/';
            if (e[i].len > 0)
                memcpy(b + n, e[i].p, (size_t)e[i].len);
            n += e[i].len;
        }
        return fp_clean_joined(a, b, n, cap, false);
    }

    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
    if (b == NULL)
        return BURROW_STR_EMPTY;
    Int n = 0;
    Byte last = 0;
    for (Int i = 0; i < elem.len; i++) {
        Str s = e[i];
        if (n == 0) {
            /* The first element that is not empty goes in as it is. */
        } else if (fp_is_sep(last, true)) {
            /* After a separator, drop the element's leading ones, so that
             * elements that are not UNC do not make a UNC path between them.
             * Join("\\", "host", "share") still gives \\host\share, because
             * the first element is kept whole. */
            while (s.len > 0 && fp_is_sep(s.p[0], true))
                s = fp_tail(s, 1);
            /* \ followed by ?? would be the root local device prefix \??\,
             * so make it \.\?? instead. */
            if (n == 1 && fp_has_prefix(s, "??") &&
                (s.len == 2 || fp_is_sep(s.p[2], true))) {
                b[n++] = '.';
                b[n++] = '\\';
            }
        } else if (last == ':') {
            /* After a colon, stay relative to the drive's working directory
             * and add nothing, so C: and f give C:f and C: and \f give C:\f. */
        } else {
            b[n++] = '\\';
            last = '\\';
        }
        if (s.len > 0) {
            memcpy(b + n, s.p, (size_t)s.len);
            n += s.len;
            last = s.p[s.len - 1];
        }
    }
    if (n == 0) {
        mem_free(a, b, (size_t)cap, 1);
        return BURROW_STR_EMPTY;
    }
    return fp_clean_joined(a, b, n, cap, true);
}

/* -------------------------------------------------------------------- Rel */

static bool fp_same_word(Str a, Str b, bool win) {
    if (win)
        return strings_equal_fold(a, b);
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

static Error fp_rel_error(Alloc *a, Str targ, Str base) {
    static const char head[] = "Rel: can't make ";
    static const char mid[] = " relative to ";
    Int hn = (Int)sizeof head - 1, mn = (Int)sizeof mid - 1;
    Int n = hn + targ.len + mn + base.len;
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (b == NULL)
        return errors_new(error_allocator(), BURROW_S("Rel: can't make path relative"));
    memcpy(b, head, (size_t)hn);
    if (targ.len > 0)
        memcpy(b + hn, targ.p, (size_t)targ.len);
    memcpy(b + hn + targ.len, mid, (size_t)mn);
    if (base.len > 0)
        memcpy(b + hn + targ.len + mn, base.p, (size_t)base.len);
    Error e = errors_new(error_allocator(), fp_str(b, n));
    mem_free(a, b, (size_t)n, 1);
    return e;
}

static Int fp_count_byte(Str s, Byte c) {
    Int n = 0;
    for (Int i = 0; i < s.len; i++)
        n += s.p[i] == c;
    return n;
}

Str burrow__filepath_rel(Alloc *a, Str base_path, Str targ_path, bool win, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Byte sep = fp_sep(win);
    FpOwn vown, town, bown, tgown;
    Str base_vol = fp_replace_byte(
        a, fp_str(base_path.p, fp_volume_name_len(base_path, win)), '/', '\\', &vown);
    Str targ_vol = fp_replace_byte(
        a, fp_str(targ_path.p, fp_volume_name_len(targ_path, win)), '/', '\\', &town);
    if (!win) {
        base_vol = BURROW_STR_EMPTY;
        targ_vol = BURROW_STR_EMPTY;
    }
    Str base = fp_clean_own(a, base_path, win, &bown);
    Str targ = fp_clean_own(a, targ_path, win, &tgown);
    Str result = BURROW_STR_EMPTY;
    bool keep_targ = false;

    if (fp_same_word(targ, base, win)) {
        result = BURROW_S(".");
        goto done;
    }
    base = fp_tail(base, base_vol.len);
    targ = fp_tail(targ, targ_vol.len);
    if (fp_eq(base, ".")) {
        base = BURROW_STR_EMPTY;
    } else if (base.len == 0 && fp_volume_name_len(base_vol, win) > 2) {
        /* A UNC volume. */
        base = win ? BURROW_S("\\") : BURROW_S("/");
    }

    /* Can't use IsAbs: `\a` and `a` are both relative on Windows. */
    bool base_slashed = base.len > 0 && base.p[0] == sep;
    bool targ_slashed = targ.len > 0 && targ.p[0] == sep;
    if (base_slashed != targ_slashed || !fp_same_word(base_vol, targ_vol, win)) {
        BURROW_OUT(err, fp_rel_error(a, targ_path, base_path));
        goto done;
    }
    /* Position base[b0:bi] and targ[t0:ti] at the first differing
     * elements. */
    Int bl = base.len, tl = targ.len;
    Int b0 = 0, bi = 0, t0 = 0, ti = 0;
    for (;;) {
        while (bi < bl && base.p[bi] != sep)
            bi++;
        while (ti < tl && targ.p[ti] != sep)
            ti++;
        if (!fp_same_word(fp_sub(targ, t0, ti), fp_sub(base, b0, bi), win))
            break;
        if (bi < bl)
            bi++;
        if (ti < tl)
            ti++;
        b0 = bi;
        t0 = ti;
    }
    if (fp_eq(fp_sub(base, b0, bi), "..")) {
        BURROW_OUT(err, fp_rel_error(a, targ_path, base_path));
        goto done;
    }
    if (b0 != bl) {
        /* Base elements left over. Climb up out of each, then down into
         * targ's. */
        Int seps = fp_count_byte(fp_sub(base, b0, bl), sep);
        Int size = 2 + seps * 3;
        if (tl != t0)
            size += 1 + tl - t0;
        Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)size, 1);
        if (buf == NULL)
            goto done;
        buf[0] = '.';
        buf[1] = '.';
        Int n = 2;
        for (Int i = 0; i < seps; i++) {
            buf[n] = sep;
            buf[n + 1] = '.';
            buf[n + 2] = '.';
            n += 3;
        }
        if (t0 != tl) {
            buf[n] = sep;
            memcpy(buf + n + 1, targ.p + t0, (size_t)(tl - t0));
        }
        result = fp_clean_joined(a, buf, size, size, win);
        goto done;
    }
    result = fp_tail(targ, t0);
    keep_targ = true;

done:
    fp_own_free(a, vown);
    fp_own_free(a, town);
    fp_own_free(a, bown);
    if (!keep_targ)
        fp_own_free(a, tgown);
    return result;
}

/* ------------------------------------------------------------------ Match */

/* scanChunk: the next segment of pattern, a run of stars and then everything
 * up to the next star that is not inside a class. */
static bool fp_scan_chunk(Str *pattern, Str *chunk, bool win) {
    Str p = *pattern;
    bool star = false;
    while (p.len > 0 && p.p[0] == '*') {
        p = fp_tail(p, 1);
        star = true;
    }
    bool inrange = false;
    for (Int i = 0; i < p.len; i++) {
        switch (p.p[i]) {
        case '\\':
            /* The error check is done in fp_match_chunk. */
            if (!win && i + 1 < p.len)
                i++;
            break;
        case '[':
            inrange = true;
            break;
        case ']':
            inrange = false;
            break;
        case '*':
            if (!inrange) {
                *chunk = fp_str(p.p, i);
                *pattern = fp_tail(p, i);
                return star;
            }
            break;
        default:
            break;
        }
    }
    *chunk = p;
    *pattern = BURROW_STR_EMPTY;
    return star;
}

/* getEsc: one character of a class, with a backslash escape outside
 * Windows. */
static bool fp_get_esc(Str *chunk, Rune *r, bool win) {
    Str c = *chunk;
    if (c.len == 0 || c.p[0] == '-' || c.p[0] == ']')
        return false;
    if (c.p[0] == '\\' && !win) {
        c = fp_tail(c, 1);
        if (c.len == 0)
            return false;
    }
    Int n;
    *r = utf8_decode_rune_in_string(c, &n);
    bool ok = !(*r == UTF8_RUNE_ERROR && n == 1);
    *chunk = fp_tail(c, n);
    return ok && chunk->len > 0;
}

/* matchChunk: whether chunk matches the start of s, with the rest of s in
 * *rest. -1 is a malformed chunk, 0 no match and 1 a match. The whole chunk
 * is read even after the match has failed, so a malformed one is always
 * noticed. */
static int fp_match_chunk(Str chunk, Str s, Str *rest, bool win) {
    Byte sep = fp_sep(win);
    bool failed = false;
    while (chunk.len > 0) {
        failed = failed || s.len == 0;
        switch (chunk.p[0]) {
        case '[': {
            Rune r = 0;
            if (!failed) {
                Int n;
                r = utf8_decode_rune_in_string(s, &n);
                s = fp_tail(s, n);
            }
            chunk = fp_tail(chunk, 1);
            bool negated = false;
            if (chunk.len > 0 && chunk.p[0] == '^') {
                negated = true;
                chunk = fp_tail(chunk, 1);
            }
            bool match = false;
            Int nrange = 0;
            for (;;) {
                if (chunk.len > 0 && chunk.p[0] == ']' && nrange > 0) {
                    chunk = fp_tail(chunk, 1);
                    break;
                }
                Rune lo, hi;
                if (!fp_get_esc(&chunk, &lo, win))
                    return -1;
                hi = lo;
                if (chunk.p[0] == '-') {
                    chunk = fp_tail(chunk, 1);
                    if (!fp_get_esc(&chunk, &hi, win))
                        return -1;
                }
                match = match || (lo <= r && r <= hi);
                nrange++;
            }
            failed = failed || match == negated;
            break;
        }
        case '?':
            if (!failed) {
                failed = s.p[0] == sep;
                Int n;
                utf8_decode_rune_in_string(s, &n);
                s = fp_tail(s, n);
            }
            chunk = fp_tail(chunk, 1);
            break;
        case '\\':
            if (!win) {
                chunk = fp_tail(chunk, 1);
                if (chunk.len == 0)
                    return -1;
            }
            if (!failed) {
                failed = chunk.p[0] != s.p[0];
                s = fp_tail(s, 1);
            }
            chunk = fp_tail(chunk, 1);
            break;
        default:
            if (!failed) {
                failed = chunk.p[0] != s.p[0];
                s = fp_tail(s, 1);
            }
            chunk = fp_tail(chunk, 1);
            break;
        }
    }
    if (failed)
        return 0;
    *rest = s;
    return 1;
}

static bool fp_bad_pattern(Error *err) {
    BURROW_OUT(err, filepath_err_bad_pattern);
    return false;
}

bool burrow__filepath_match(Str pattern, Str name, bool win, Error *err) {
    Byte sep = fp_sep(win);
    BURROW_OUT(err, BURROW_NO_ERROR);
    while (pattern.len > 0) {
        Str chunk, t;
        bool star = fp_scan_chunk(&pattern, &chunk, win);
        if (star && chunk.len == 0) {
            /* A trailing * matches the rest unless it has a separator. */
            return name.len == 0 || memchr(name.p, sep, (size_t)name.len) == NULL;
        }
        /* Look for a match at the current position. If this is the last
         * chunk, it has to use up the name, or a match the star could still
         * make would be missed. */
        int m = fp_match_chunk(chunk, name, &t, win);
        if (m == 1 && (t.len == 0 || pattern.len > 0)) {
            name = t;
            continue;
        }
        if (m < 0)
            return fp_bad_pattern(err);
        if (star) {
            /* Look for a match skipping i+1 bytes. A star cannot skip a
             * separator. */
            bool next = false;
            for (Int i = 0; i < name.len && name.p[i] != sep; i++) {
                m = fp_match_chunk(chunk, fp_tail(name, i + 1), &t, win);
                if (m == 1) {
                    if (pattern.len == 0 && t.len > 0)
                        continue;
                    name = t;
                    next = true;
                    break;
                }
                if (m < 0)
                    return fp_bad_pattern(err);
            }
            if (next)
                continue;
        }
        return false;
    }
    return name.len == 0;
}

/* -------------------------------------------------------------- HasPrefix */

bool burrow__filepath_has_prefix(Str p, Str prefix, bool win) {
    if (strings_has_prefix(p, prefix))
        return true;
    if (!win)
        return false;
    Alloc *a = heap_allocator();
    Str lp = strings_to_lower(a, p);
    Str lprefix = strings_to_lower(a, prefix);
    bool r = strings_has_prefix(lp, lprefix);
    if (lp.p != p.p)
        mem_free(a, (void *)(Uintptr)lp.p, (size_t)lp.len, 1);
    if (lprefix.p != prefix.p)
        mem_free(a, (void *)(Uintptr)lprefix.p, (size_t)lprefix.len, 1);
    return r;
}

/* ------------------------------------------------------- the host's rules */

bool filepath_is_path_separator(Byte c) {
    return fp_is_sep(c, FP_HOST);
}

Str filepath_clean(Alloc *a, Str path) {
    return burrow__filepath_clean(a, path, FP_HOST);
}

bool filepath_is_local(Str path) {
    return burrow__filepath_is_local(path, FP_HOST);
}

Str filepath_localize(Alloc *a, Str path, Error *err) {
    return burrow__filepath_localize(a, path, FP_HOST, err);
}

Str filepath_to_slash(Alloc *a, Str path) {
    return burrow__filepath_to_slash(a, path, FP_HOST);
}

Str filepath_from_slash(Alloc *a, Str path) {
    return burrow__filepath_from_slash(a, path, FP_HOST);
}

Slice filepath_split_list(Alloc *a, Str path) {
    return burrow__filepath_split_list(a, path, FP_HOST);
}

Str filepath_split(Str path, Str *file) {
    return burrow__filepath_split(path, file, FP_HOST);
}

Str filepath_join(Alloc *a, Slice elem) {
    return burrow__filepath_join(a, elem, FP_HOST);
}

Str filepath_join_v(Alloc *a, int n, ...) {
    Str stack[16];
    Str *e = stack;
    va_list ap;

    if (n <= 0)
        return BURROW_STR_EMPTY;
    if ((size_t)n > sizeof stack / sizeof stack[0]) {
        e = (Str *)mem_alloc_nozero(a, (size_t)n * sizeof(Str), _Alignof(Str));
        if (e == NULL)
            return BURROW_STR_EMPTY;
    }
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        e[i] = va_arg(ap, Str);
    va_end(ap);
    Str r = filepath_join(a, slice_from(e, n, n, TYPE_STRING));
    if (e != stack)
        mem_free(a, e, (size_t)n * sizeof(Str), _Alignof(Str));
    return r;
}

Str filepath_ext(Str path) {
    return burrow__filepath_ext(path, FP_HOST);
}

Str filepath_base(Str path) {
    return burrow__filepath_base(path, FP_HOST);
}

Str filepath_dir(Alloc *a, Str path) {
    return burrow__filepath_dir(a, path, FP_HOST);
}

bool filepath_is_abs(Str path) {
    return burrow__filepath_is_abs(path, FP_HOST);
}

Str filepath_volume_name(Alloc *a, Str path) {
    return burrow__filepath_volume_name(a, path, FP_HOST);
}

Str filepath_rel(Alloc *a, Str base, Str targ, Error *err) {
    return burrow__filepath_rel(a, base, targ, FP_HOST, err);
}

bool filepath_match(Str pattern, Str name, Error *err) {
    return burrow__filepath_match(pattern, name, FP_HOST, err);
}

Int burrow__filepath_volume_name_len(Str path, bool win) {
    return fp_volume_name_len(path, win);
}

bool filepath_has_prefix(Str p, Str prefix) {
    return burrow__filepath_has_prefix(p, prefix, FP_HOST);
}
