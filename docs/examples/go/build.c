#include "burrow/burrow.h"

/* Writes a small package into a new directory and returns the directory. */
static Str write_package(Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("greet-*"), &err);
    if (BURROW_FAILED(err))
        return BURROW_STR_EMPTY;
    (void)os_write_file(filepath_join_v(a, 2, dir, BURROW_S("greet.go")),
                        BURROW_B("// Package greet says hello.\n"
                                 "package greet\n"
                                 "\n"
                                 "import \"fmt\"\n"
                                 "\n"
                                 "func Hello() { fmt.Println(\"hello\") }\n"),
                        0644);
    (void)os_write_file(filepath_join_v(a, 2, dir, BURROW_S("greet_linux.go")),
                        BURROW_B("package greet\n"
                                 "\n"
                                 "import \"os\"\n"
                                 "\n"
                                 "var pid = os.Getpid()\n"),
                        0644);
    (void)os_write_file(filepath_join_v(a, 2, dir, BURROW_S("greet_windows.go")),
                        BURROW_B("package greet\n"
                                 "\n"
                                 "import \"syscall\"\n"
                                 "\n"
                                 "var pid = syscall.Getpid()\n"),
                        0644);
    (void)os_write_file(filepath_join_v(a, 2, dir, BURROW_S("gen.go")),
                        BURROW_B("//go:build ignore\n"
                                 "\n"
                                 "package main\n"),
                        0644);
    (void)os_write_file(filepath_join_v(a, 2, dir, BURROW_S("greet_test.go")),
                        BURROW_B("package greet\n"
                                 "\n"
                                 "import \"testing\"\n"
                                 "\n"
                                 "func TestHello(t *testing.T) { Hello() }\n"),
                        0644);
    return dir;
}

static void import_dir(Alloc *a, Str dir) {
    // doc: import
    BuildContext ctxt = build_default(a);
    ctxt.goos = BURROW_S("linux");
    ctxt.goarch = BURROW_S("amd64");
    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_context_import_dir(&ctxt, a, dir, 0, &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("%s\n", error_text(err));
        return;
    }
    Str sp = BURROW_S(" ");
    fmt_printf_v("package %s: %s\n", p->name, p->doc);
    fmt_printf_v("go files: %s\n", strings_join(a, p->go_files, sp));
    fmt_printf_v("ignored: %s\n", strings_join(a, p->ignored_go_files, sp));
    fmt_printf_v("test files: %s\n", strings_join(a, p->test_go_files, sp));
    fmt_printf_v("imports: %s\n", strings_join(a, p->imports, sp));
    fmt_printf_v("test imports: %s\n", strings_join(a, p->test_imports, sp));
    fmt_printf_v("tags seen: %s\n", strings_join(a, p->all_tags, sp));
    // doc: end
}

static void match_file(Alloc *a, Str dir) {
    // doc: match
    BuildContext ctxt = build_default(a);
    ctxt.goos = BURROW_S("windows");
    ctxt.goarch = BURROW_S("arm64");
    const char *names[] = {"greet.go", "greet_linux.go", "greet_windows.go", "gen.go"};
    for (int i = 0; i < 4; i++) {
        Error err = BURROW_NO_ERROR;
        Str name = str_from_cstr(names[i]);
        bool ok = build_context_match_file(&ctxt, a, dir, name, &err);
        fmt_printf_v("%s: %v\n", name, ok);
    }
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str dir = write_package(a);
    if (dir.len > 0) {
        import_dir(a, dir);
        match_file(a, dir);
        (void)os_remove_all(dir);
    }
    arena_free(&ar);
    return 0;
}
