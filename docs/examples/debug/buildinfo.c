#include "burrow/burrow.h"

// The module information the go command hands the linker: runtime/debug's
// text, with 16 bytes the go command picks on each side of it.
static const char mod_text[] =
    "\x30\x77\xaf\x0c\x92\x74\x08\x02\x41\xe1\xc1\x07\xe6\xd6\x18\xe6"
    "path\texample.com/hello\n"
    "mod\texample.com/hello\t(devel)\t\n"
    "dep\tgolang.org/x/text\tv0.21.0\n"
    "build\t-compiler=gc\n"
    "build\t-ldflags=\"-s -w\"\n"
    "build\tGOARCH=386\n"
    "build\tGOOS=plan9\n"
    "\xf9\x32\x43\x31\x86\x18\x20\x72\x00\x82\x42\x10\x41\x16\xd8\xf2";

static Slice append_string(Alloc *a, Slice b, const char *s, size_t n) {
    b = binary_append_uvarint(a, b, n);
    return slice_append(a, b, s, (Int)n);
}

// A Plan 9 386 executable with no code, only a data segment that holds the
// build information the way the Go linker writes it: a 32-byte header that
// starts with a magic string, and then the Go release and the module
// information, each with its length in front. A real binary would come from
// buildinfo_read_file.
static Slice tiny_go_binary(Alloc *a) {
    Byte info[32] = {0};
    memcpy(info, "\xff Go buildinf:", 14);
    info[14] = 4; // the size of a pointer
    info[15] = 2; // the strings follow the header
    Slice data = slice_make(a, TYPE_BYTE, 0, 512);
    data = slice_append(a, data, info, 32);
    data = append_string(a, data, "go1.27.1", 8);
    data = append_string(a, data, mod_text, sizeof mod_text - 1);

    // The data segment starts right after the header.
    Byte hdr[32] = {0};
    binary_big_endian_put_uint32(slice_from(hdr, 4, 4, TYPE_BYTE), PLAN9OBJ_MAGIC386);
    binary_big_endian_put_uint32(slice_from(hdr + 8, 4, 4, TYPE_BYTE),
                                 (uint32_t)data.len);
    Slice b = slice_make(a, TYPE_BYTE, 0, 32 + data.len);
    b = slice_append(a, b, hdr, 32);
    return slice_append(a, b, data.p, data.len);
}

static void read_binary(Alloc *a) {
    // doc: read
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, tiny_go_binary(a));
    Buildinfo *bi = buildinfo_read(a, bytes_reader_as_io_reader_at(br), &err);
    if (bi == NULL) {
        fmt_println_v(err);
        return;
    }
    fmt_printf_v("%s, built with %s\n", bi->path, bi->go_version);
    fmt_printf_v("module %s %s\n", bi->main.path, bi->main.version);
    for (Int i = 0; i < bi->deps.len; i++) {
        DebugModule *m = ((DebugModule **)bi->deps.p)[i];
        fmt_printf_v("depends on %s %s\n", m->path, m->version);
    }
    for (Int i = 0; i < bi->settings.len; i++) {
        DebugBuildSetting *s = &((DebugBuildSetting *)bi->settings.p)[i];
        fmt_printf_v("  %s: %s\n", s->key, s->value);
    }
    debug_build_info_free(bi);
    // doc: end
}

static void bad_files(Alloc *a) {
    // doc: errors
    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, BURROW_B("hello"));
    Buildinfo *bi = buildinfo_read(a, bytes_reader_as_io_reader_at(br), &err);
    if (bi == NULL)
        fmt_println_v(err);

    // One byte of the magic changes, so the record is not found.
    Slice b = tiny_go_binary(a);
    ((Byte *)b.p)[32 + 2] = 'N';
    br = bytes_new_reader(a, b);
    err = BURROW_NO_ERROR;
    bi = buildinfo_read(a, bytes_reader_as_io_reader_at(br), &err);
    if (bi == NULL)
        fmt_println_v(err);
    // doc: end
}

static void parse_text(Alloc *a) {
    // doc: parse
    Str text = BURROW_S("path\texample.com/hello\n"
                        "build\t-tags=netgo\n"
                        "build\tvcs.modified=true\n");
    Error err = BURROW_NO_ERROR;
    DebugBuildInfo *bi = debug_parse_build_info(a, text, &err);
    if (bi == NULL) {
        fmt_println_v(err);
        return;
    }
    bi->go_version = BURROW_S("go1.27.1");
    DebugBuildSetting extra = {BURROW_S("-ldflags"), BURROW_S("-X main.version=1.2.0")};
    bi->settings = slice_append(bi->a, bi->settings, &extra, 1);
    fmt_print_v(debug_build_info_string(bi, a));
    debug_build_info_free(bi);

    // A module line with only a path in it.
    bi = debug_parse_build_info(a, BURROW_S("path\tp\nmod\texample.com/hello\n"), &err);
    if (bi == NULL)
        fmt_println_v(err);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    read_binary(a);
    bad_files(a);
    parse_text(a);
    arena_free(&ar);
    return 0;
}

/* Output:
example.com/hello, built with go1.27.1
module example.com/hello (devel)
depends on golang.org/x/text v0.21.0
  -compiler: gc
  -ldflags: -s -w
  GOARCH: 386
  GOOS: plan9
unrecognized file format
not a Go executable
go	go1.27.1
path	example.com/hello
build	-tags=netgo
build	vcs.modified=true
build	-ldflags="-X main.version=1.2.0"
could not parse Go build info: line 2: expected 2 or 3 columns; got 1
*/
