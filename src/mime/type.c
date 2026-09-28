/* mime's extension table: TypeByExtension, ExtensionsByType and
 * AddExtensionType.
 *
 * Derived from Go's src/mime/type.go, type_unix.go and type_windows.go.
 * Go source: go1.27.1.
 *
 * Go keeps three sync.Maps and fills them on first use: the built in table,
 * then what the system knows. On Unix that is the first globs2 file of the
 * freedesktop shared MIME database that opens, or failing that every mime.types
 * file in a list. On Windows it is the registry. The same happens here, with
 * one reader and writer lock over three maps instead of sync.Map.
 *
 * The strings in the table are never freed, so what mime_type_by_extension and
 * mime_extensions_by_type hand out stays good for the life of the program, the
 * way a Go string would. A later mime_add_extension_type for the same extension
 * adds a new string and leaves the old one where it was.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include <string.h>

/* One extension of a media type, in a list per type. next is the index of the
 * one after it in mt_list, or -1. */
typedef struct MtExt {
    Str ext;
    Int next;
} MtExt;

/* mimeTypes, mimeTypesLower and extensions, from extension to type, from the
 * lower case extension to type, and from the bare type to the head of its list
 * in mt_list. The lock covers all of it. */
static SyncRWMutex mt_mu;
static SyncOnce mt_once;
static bool mt_ready;
static Arena mt_arena;
static Map *mt_types;
static Map *mt_lower;
static Map *mt_exts;
static MtExt *mt_list;
static Int mt_list_len;
static Int mt_list_cap;

/* builtinTypesLower. */
static const struct {
    const char *ext;
    const char *type;
} mt_builtin[] = {
    {".ai", "application/postscript"},
    {".apk", "application/vnd.android.package-archive"},
    {".apng", "image/apng"},
    {".avif", "image/avif"},
    {".bin", "application/octet-stream"},
    {".bmp", "image/bmp"},
    {".com", "application/octet-stream"},
    {".css", "text/css; charset=utf-8"},
    {".csv", "text/csv; charset=utf-8"},
    {".doc", "application/msword"},
    {".docx",
     "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".ehtml", "text/html; charset=utf-8"},
    {".eml", "message/rfc822"},
    {".eps", "application/postscript"},
    {".exe", "application/octet-stream"},
    {".flac", "audio/flac"},
    {".gif", "image/gif"},
    {".gz", "application/gzip"},
    {".htm", "text/html; charset=utf-8"},
    {".html", "text/html; charset=utf-8"},
    {".ico", "image/vnd.microsoft.icon"},
    {".ics", "text/calendar; charset=utf-8"},
    {".jfif", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".jpg", "image/jpeg"},
    {".js", "text/javascript; charset=utf-8"},
    {".json", "application/json"},
    {".m4a", "audio/mp4"},
    {".mjs", "text/javascript; charset=utf-8"},
    {".mp3", "audio/mpeg"},
    {".mp4", "video/mp4"},
    {".oga", "audio/ogg"},
    {".ogg", "audio/ogg"},
    {".ogv", "video/ogg"},
    {".opus", "audio/ogg"},
    {".pdf", "application/pdf"},
    {".pjp", "image/jpeg"},
    {".pjpeg", "image/jpeg"},
    {".png", "image/png"},
    {".ppt", "application/vnd.ms-powerpoint"},
    {".pptx",
     "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".ps", "application/postscript"},
    {".rdf", "application/rdf+xml"},
    {".rtf", "application/rtf"},
    {".shtml", "text/html; charset=utf-8"},
    {".svg", "image/svg+xml"},
    {".text", "text/plain; charset=utf-8"},
    {".tif", "image/tiff"},
    {".tiff", "image/tiff"},
    {".txt", "text/plain; charset=utf-8"},
    {".vtt", "text/vtt; charset=utf-8"},
    {".wasm", "application/wasm"},
    {".wav", "audio/wav"},
    {".weba", "audio/webm"},
    {".webm", "video/webm"},
    {".webp", "image/webp"},
    {".xbl", "text/xml; charset=utf-8"},
    {".xbm", "image/x-xbitmap"},
    {".xht", "application/xhtml+xml"},
    {".xhtml", "application/xhtml+xml"},
    {".xls", "application/vnd.ms-excel"},
    {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".xml", "text/xml; charset=utf-8"},
    {".xsl", "text/xml; charset=utf-8"},
    {".zip", "application/zip"},
};

/* ------------------------------------------------------------ the table */

/* Empties the table, with mt_mu held for writing. The strings from before are
 * freed with it, which only the tests do. */
static bool mt_clear_locked(void) {
    if (mt_ready) {
        map_free(mt_types);
        map_free(mt_lower);
        map_free(mt_exts);
        mem_free(heap_allocator(), mt_list, (size_t)mt_list_cap * sizeof(MtExt),
                 _Alignof(MtExt));
        arena_free(&mt_arena);
    }
    mt_list = NULL;
    mt_list_len = mt_list_cap = 0;
    arena_init(&mt_arena, heap_allocator(), 0);
    mt_types = map_make(heap_allocator(), TYPE_STRING, TYPE_STRING, 0);
    mt_lower = map_make(heap_allocator(), TYPE_STRING, TYPE_STRING, 0);
    mt_exts = map_make(heap_allocator(), TYPE_STRING, TYPE_INT, 0);
    mt_ready = true;
    return mt_types != NULL && mt_lower != NULL && mt_exts != NULL;
}

/* s in the table's arena. */
static bool mt_keep(Str s, Str *out) {
    *out = strings_clone(arena_allocator(&mt_arena), s);
    return out->p != NULL || s.len == 0;
}

/* Adds ext to the list of type, with mt_mu held for writing. With dedup set,
 * an ext already there is left alone, which is setExtensionType. Without it the
 * ext is added regardless, which is setMimeTypes. */
static bool mt_add_ext_locked(Str type, Str ext, bool dedup) {
    const Int *headp = (const Int *)map_get(mt_exts, &type);
    Int head = headp == NULL ? -1 : *headp;
    if (dedup) {
        for (Int i = head; i >= 0; i = mt_list[i].next)
            if (str_eq(mt_list[i].ext, ext))
                return true;
    }
    if (mt_list_len == mt_list_cap) {
        Int cap = mt_list_cap == 0 ? 64 : mt_list_cap * 2;
        MtExt *grown = (MtExt *)mem_alloc_nozero(
            heap_allocator(), (size_t)cap * sizeof(MtExt), _Alignof(MtExt));
        if (grown == NULL)
            return false;
        if (mt_list_len > 0)
            memcpy(grown, mt_list, (size_t)mt_list_len * sizeof(MtExt));
        if (mt_list != NULL)
            mem_free(heap_allocator(), mt_list, (size_t)mt_list_cap * sizeof(MtExt),
                     _Alignof(MtExt));
        mt_list = grown;
        mt_list_cap = cap;
    }
    Str kt, ke;
    if (headp == NULL) {
        if (!mt_keep(type, &kt))
            return false;
    } else {
        kt = type;
    }
    if (!mt_keep(ext, &ke))
        return false;
    Int at = mt_list_len;
    mt_list[at].ext = ke;
    mt_list[at].next = head;
    if (!map_set(mt_exts, &kt, &at))
        return false;
    mt_list_len++;
    return true;
}

/* setMimeTypes with the built in table for both maps, with mt_mu held for
 * writing. */
static bool mt_set_builtin_locked(void) {
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof mt_builtin / sizeof mt_builtin[0]; i++) {
        Str ext = str_from_cstr(mt_builtin[i].ext),
            type = str_from_cstr(mt_builtin[i].type);
        Str ke, kt;
        ok = mt_keep(ext, &ke) && mt_keep(type, &kt) && map_set(mt_types, &ke, &kt) &&
             map_set(mt_lower, &ke, &kt);
        if (!ok)
            break;
        Error err;
        Str just = mime_parse_media_type(arena_allocator(&scratch), type, NULL, &err);
        ok = BURROW_OK(err) && mt_add_ext_locked(just, ke, false);
    }
    arena_free(&scratch);
    return ok;
}

/* setExtensionType. */
static Error mt_set_extension_type(Str extension, Str mime_type) {
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Alloc *sa = arena_allocator(&scratch);
    Map *param;
    Error err;
    Str just = mime_parse_media_type(sa, mime_type, &param, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&scratch);
        return err;
    }
    Str charset = BURROW_S("charset");
    const Str *cs = (const Str *)map_get(param, &charset);
    if (strings_has_prefix(mime_type, BURROW_S("text/")) &&
        (cs == NULL || cs->len == 0)) {
        Str utf8 = BURROW_S("utf-8");
        if (!map_set(param, &charset, &utf8)) {
            arena_free(&scratch);
            return burrow__mime_err_no_memory;
        }
        /* FormatMediaType gives the empty string for a type it cannot write,
         * and Go stores that too. */
        mime_type = mime_format_media_type(sa, just, param);
    }
    Str ext_lower = strings_to_lower(sa, extension);
    if (ext_lower.p == NULL && extension.len > 0) {
        arena_free(&scratch);
        return burrow__mime_err_no_memory;
    }

    sync_rw_mutex_lock(&mt_mu);
    Str ke, kl, kt;
    bool ok = mt_keep(extension, &ke) && mt_keep(ext_lower, &kl) &&
              mt_keep(mime_type, &kt) && map_set(mt_types, &ke, &kt) &&
              map_set(mt_lower, &kl, &kt) && mt_add_ext_locked(just, kl, true);
    sync_rw_mutex_unlock(&mt_mu);
    arena_free(&scratch);
    return ok ? BURROW_NO_ERROR : burrow__mime_err_no_memory;
}

/* ------------------------------------------------------ what the system has */

/* The whole of the file at path, in a, or false when it cannot be read. */
static bool mt_read_file(Alloc *a, const char *path, Str *out) {
    PalErrno e;
    int64_t fd = pal_open(path, PAL_O_RDONLY, 0, &e);
    if (fd < 0)
        return false;
    StringsBuilder b = STRINGS_BUILDER(a);
    Byte buf[8192];
    bool ok = true;
    for (;;) {
        int64_t n = pal_read(fd, buf, (int64_t)sizeof buf, &e);
        if (n == 0)
            break;
        if (n < 0) {
            if (e == PAL_EINTR)
                continue;
            ok = false;
            break;
        }
        Error err;
        Str chunk = {buf, (Int)n};
        strings_builder_write_string(&b, chunk, &err);
        if (BURROW_FAILED(err)) {
            ok = false;
            break;
        }
    }
    pal_close(fd, &e);
    *out = strings_builder_string(&b);
    return ok;
}

/* The next line of *rest, the way bufio.ScanLines cuts them: at \n, with a \r
 * before it dropped, and a last line without a \n still a line. */
static bool mt_next_line(Str *rest, Str *line) {
    if (rest->len == 0)
        return false;
    bool found;
    Str after;
    *line = strings_cut(*rest, BURROW_S("\n"), &after, &found);
    *rest = found ? after : (Str){NULL, 0};
    if (line->len > 0 && line->p[line->len - 1] == '\r')
        line->len--;
    return true;
}

/* loadMimeGlobsFile. False when the file cannot be read. */
bool burrow__mime_load_globs_file(const char *path) {
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Alloc *sa = arena_allocator(&scratch);
    Str text;
    if (!mt_read_file(sa, path, &text)) {
        arena_free(&scratch);
        return false;
    }
    Str line;
    while (mt_next_line(&text, &line)) {
        /* priority:type:glob, and flags after that nobody here reads. */
        Slice fields = strings_split(sa, line, BURROW_S(":"));
        const Str *f = (const Str *)fields.p;
        if (fields.len < 3 || f[0].len < 1 || f[2].len < 3)
            continue;
        if (f[0].p[0] == '#' || f[2].p[0] != '*' || f[2].p[1] != '.')
            continue;
        Str extension = {f[2].p + 1, f[2].len - 1};
        if (strings_contains_any(extension, BURROW_S("?*[")))
            continue;
        sync_rw_mutex_r_lock(&mt_mu);
        bool have = map_get(mt_types, &extension) != NULL;
        sync_rw_mutex_r_unlock(&mt_mu);
        if (have)
            continue;
        mt_set_extension_type(extension, f[1]);
    }
    arena_free(&scratch);
    return true;
}

/* loadMimeFile. */
void burrow__mime_load_types_file(const char *path) {
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Alloc *sa = arena_allocator(&scratch);
    Str text;
    if (!mt_read_file(sa, path, &text)) {
        arena_free(&scratch);
        return;
    }
    Str line;
    while (mt_next_line(&text, &line)) {
        Slice fields = strings_fields(sa, line);
        const Str *f = (const Str *)fields.p;
        if (fields.len <= 1 || f[0].p[0] == '#')
            continue;
        for (Int i = 1; i < fields.len; i++) {
            if (f[i].p[0] == '#')
                break;
            StringsBuilder b = STRINGS_BUILDER(sa);
            strings_builder_write_byte(&b, '.');
            Error err;
            strings_builder_write_string(&b, f[i], &err);
            if (BURROW_OK(err))
                mt_set_extension_type(strings_builder_string(&b), f[0]);
        }
    }
    arena_free(&scratch);
}

#if defined(BURROW_OS_WINDOWS)

static void mt_registry_entry(void *env, const char *ext, int64_t ext_len,
                              const char *type, int64_t type_len) {
    (void)env;
    Str e = {(const Byte *)ext, (Int)ext_len}, v = {(const Byte *)type, (Int)type_len};
    /* Windows has .js as text/plain on plenty of machines, which would break
     * JavaScript served from a Go program, so Go skips it. */
    if (str_eq(e, BURROW_S(".js")) &&
        (str_eq(v, BURROW_S("text/plain")) ||
         str_eq(v, BURROW_S("text/plain; charset=utf-8"))))
        return;
    mt_set_extension_type(e, v);
}

static void mt_init_os(void) {
    PalErrno e;
    pal_registry_content_types(mt_registry_entry, NULL, &e);
}

#else

static void mt_init_os(void) {
    static const char *const globs[] = {
        "/usr/local/share/mime/globs2",
        "/usr/share/mime/globs2",
    };
    static const char *const files[] = {
        "/etc/mime.types",           "/etc/apache2/mime.types",
        "/etc/apache/mime.types",    "/etc/httpd/conf/mime.types",
#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_OPENBSD) ||                        \
    defined(BURROW_OS_DRAGONFLY)
        "/usr/local/etc/mime.types",
#endif
    };
    for (size_t i = 0; i < sizeof globs / sizeof globs[0]; i++)
        if (burrow__mime_load_globs_file(globs[i]))
            return;
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
        burrow__mime_load_types_file(files[i]);
}

#endif

/* initMime. */
static void mt_init(void *env) {
    (void)env;
    sync_rw_mutex_lock(&mt_mu);
    bool ok = mt_clear_locked() && mt_set_builtin_locked();
    sync_rw_mutex_unlock(&mt_mu);
    if (ok)
        mt_init_os();
}

static void mt_init_none(void *env) {
    (void)env;
}

static void mt_once_do(void) {
    sync_once_do(&mt_once, BURROW_FN(Func, mt_init, NULL));
}

void burrow__mime_types_reset(bool builtin) {
    sync_once_do(&mt_once, BURROW_FN(Func, mt_init_none, NULL));
    sync_rw_mutex_lock(&mt_mu);
    if (mt_clear_locked() && builtin)
        mt_set_builtin_locked();
    sync_rw_mutex_unlock(&mt_mu);
}

Error burrow__mime_set_extension_type(Str ext, Str type) {
    return mt_set_extension_type(ext, type);
}

/* --------------------------------------------------------------- the API */

static Str mt_load(Map *m, Str key) {
    const Str *v = (const Str *)map_get(m, &key);
    return v == NULL ? (Str){NULL, 0} : *v;
}

Str mime_type_by_extension(Str ext) {
    mt_once_do();
    Str none = {NULL, 0};
    sync_rw_mutex_r_lock(&mt_mu);
    if (!mt_ready) {
        sync_rw_mutex_r_unlock(&mt_mu);
        return none;
    }
    const Str *exact = (const Str *)map_get(mt_types, &ext);
    if (exact != NULL) {
        Str r = *exact;
        sync_rw_mutex_r_unlock(&mt_mu);
        return r;
    }
    /* The lower case key, on the stack when it fits, so a lookup allocates
     * nothing, which Go's TestLookupMallocs holds it to. */
    Byte small[64];
    Byte *buf = small;
    bool ascii = true;
    for (Int i = 0; i < ext.len; i++)
        if (ext.p[i] >= 0x80)
            ascii = false;
    Str r = none;
    if (!ascii) {
        Str lower = strings_to_lower(heap_allocator(), ext);
        if (lower.p != NULL || ext.len == 0)
            r = mt_load(mt_lower, lower);
        if (lower.p != NULL && lower.p != ext.p)
            mem_free(heap_allocator(), (void *)(uintptr_t)lower.p, (size_t)lower.len,
                     1);
        sync_rw_mutex_r_unlock(&mt_mu);
        return r;
    }
    if (ext.len > (Int)sizeof small) {
        buf = (Byte *)mem_alloc_nozero(heap_allocator(), (size_t)ext.len, 1);
        if (buf == NULL) {
            sync_rw_mutex_r_unlock(&mt_mu);
            return none;
        }
    }
    for (Int i = 0; i < ext.len; i++) {
        Byte c = ext.p[i];
        buf[i] = c >= 'A' && c <= 'Z' ? (Byte)(c + ('a' - 'A')) : c;
    }
    r = mt_load(mt_lower, (Str){buf, ext.len});
    sync_rw_mutex_r_unlock(&mt_mu);
    if (buf != small)
        mem_free(heap_allocator(), buf, (size_t)ext.len, 1);
    return r;
}

Slice mime_extensions_by_type(Alloc *a, Str typ, Error *err) {
    Slice none = {0};
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Str just = mime_parse_media_type(arena_allocator(&scratch), typ, NULL, err);
    if (BURROW_FAILED(*err)) {
        arena_free(&scratch);
        return none;
    }
    mt_once_do();
    sync_rw_mutex_r_lock(&mt_mu);
    const Int *headp = mt_ready ? (const Int *)map_get(mt_exts, &just) : NULL;
    arena_free(&scratch);
    if (headp == NULL) {
        sync_rw_mutex_r_unlock(&mt_mu);
        return none;
    }
    Int n = 0;
    for (Int i = *headp; i >= 0; i = mt_list[i].next)
        n++;
    Slice ret = slice_make(a, TYPE_STRING, n, n);
    if (ret.p == NULL) {
        sync_rw_mutex_r_unlock(&mt_mu);
        *err = burrow__mime_err_no_memory;
        return none;
    }
    Str *out = (Str *)ret.p;
    Int k = 0;
    for (Int i = *headp; i >= 0; i = mt_list[i].next)
        out[k++] = mt_list[i].ext;
    sync_rw_mutex_r_unlock(&mt_mu);
    sort_strings(ret);
    return ret;
}

Error mime_add_extension_type(Str ext, Str typ) {
    if (!strings_has_prefix(ext, BURROW_S(".")))
        return fmt_errorf_v("mime: extension %q missing leading dot", ext);
    mt_once_do();
    return mt_set_extension_type(ext, typ);
}
