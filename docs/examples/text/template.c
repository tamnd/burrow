#include "burrow/burrow.h"

// doc: types
#define RECIPIENT_FIELDS(F, T)                                                         \
    F(T, Str, Name, "")                                                                \
    F(T, Str, Gift, "")                                                                \
    F(T, bool, Attended, "")
BURROW_STRUCT(Recipient, RECIPIENT_FIELDS);
// doc: end

// doc: functypes
// func(string) string and func([]string, string) string. The environment of
// each function value is the allocator its result is made in.
#define CASE_SIG(IN, OUT) IN(0, Str) OUT(Str)
BURROW_FUNC(CaseFunc, Str, Str s);
BURROW_FUNC_TYPE_DEFINE(CaseFunc, CASE_SIG);

BURROW_SLICE_TYPE(Names, Str);
#define JOIN_SIG(IN, OUT) IN(0, Names) IN(1, Str) OUT(Str)
BURROW_FUNC(JoinFunc, Str, Slice elems, Str sep);
BURROW_FUNC_TYPE_DEFINE(JoinFunc, JOIN_SIG);
// doc: end

static Str title(void *env, Str s) {
    return strings_title(env, s);
}

static Str lower(void *env, Str s) {
    return strings_to_lower(env, s);
}

static Str repeat2(void *env, Str s) {
    return strings_repeat(env, s, 2);
}

static Str repeat3(void *env, Str s) {
    return strings_repeat(env, s, 3);
}

static Str join(void *env, Slice elems, Str sep) {
    return strings_join(env, elems, sep);
}

// funcs[name] = fn, for a FuncMap.
static void set_func(Map *funcs, const char *name, Any fn) {
    Str key = str_from_cstr(name);
    map_set(funcs, &key, &fn);
}

static void letter(Alloc *a, IoWriter out) {
    // doc: letter
    // Define a template.
    Str text = BURROW_S("\n"
                        "Dear {{.Name}},\n"
                        "{{if .Attended}}\n"
                        "It was a pleasure to see you at the wedding.\n"
                        "{{- else}}\n"
                        "It is a shame you couldn't make it to the wedding.\n"
                        "{{- end}}\n"
                        "{{with .Gift -}}\n"
                        "Thank you for the lovely {{.}}.\n"
                        "{{end}}\n"
                        "Best wishes,\n"
                        "Josie\n");

    // Prepare some data to insert into the template.
    Recipient recipients[] = {
        {BURROW_S("Aunt Mildred"), BURROW_S("bone china tea set"), true},
        {BURROW_S("Uncle John"), BURROW_S("moleskin pants"), false},
        {BURROW_S("Cousin Rodney"), BURROW_STR_EMPTY, false},
    };

    // Create a new template and parse the letter into it.
    Error err = BURROW_NO_ERROR;
    TextTemplate *t =
        text_template_parse(text_template_new(a, BURROW_S("letter")), text, &err);
    t = text_template_must(t, err);

    // Execute the template for each recipient.
    for (int i = 0; i < 3; i++) {
        err = text_template_execute(t, out,
                                    BURROW_ANY(TYPE_OF(Recipient), &recipients[i]));
        if (BURROW_FAILED(err))
            fmt_println_v("executing template:", err);
    }
    // doc: end
}

static void block(Alloc *a, IoWriter out) {
    // doc: block
    Str master = BURROW_S("Names:{{block \"list\" .}}{{\"\\n\"}}{{range .}}"
                          "{{println \"-\" .}}{{end}}{{end}}");
    Str overlay = BURROW_S("{{define \"list\"}} {{join . \", \"}}{{end}} ");
    JoinFunc join_fn = BURROW_FN(JoinFunc, join, a);
    Map *funcs = map_make(a, TYPE_STRING, TYPE_ANY, 1);
    set_func(funcs, "join", BURROW_ANY(TYPE_OF(JoinFunc), &join_fn));
    Str guardians[] = {BURROW_S("Gamora"), BURROW_S("Groot"), BURROW_S("Nebula"),
                       BURROW_S("Rocket"), BURROW_S("Star-Lord")};
    Slice names = slice_from(guardians, 5, 5, TYPE_STRING);
    Any data = BURROW_ANY(TYPE_OF(Names), &names);

    Error err = BURROW_NO_ERROR;
    TextTemplate *master_tmpl = text_template_parse(
        text_template_funcs(text_template_new(a, BURROW_S("master")), funcs), master,
        &err);
    if (BURROW_FAILED(err)) {
        fmt_println_v(err);
        return;
    }
    TextTemplate *clone = text_template_clone(master_tmpl, a, &err);
    TextTemplate *overlay_tmpl =
        text_template_parse(text_template_must(clone, err), overlay, &err);
    if (BURROW_FAILED(err)) {
        fmt_println_v(err);
        return;
    }
    err = text_template_execute(master_tmpl, out, data);
    if (BURROW_FAILED(err)) {
        fmt_println_v(err);
        return;
    }
    // The overlay ends in a space, which is trimmed here so that the line does
    // not end in one.
    StringsBuilder b = STRINGS_BUILDER(a);
    err = text_template_execute(overlay_tmpl, strings_builder_as_io_writer(&b), data);
    if (BURROW_FAILED(err)) {
        fmt_println_v(err);
        return;
    }
    fmt_fprint_v(out, strings_trim_suffix(strings_builder_string(&b), BURROW_S(" ")));
    // doc: end
}

static void func(Alloc *a, IoWriter out) {
    // doc: func
    // First we create a FuncMap with which to register the function.
    CaseFunc title_fn = BURROW_FN(CaseFunc, title, a);
    Map *func_map = map_make(a, TYPE_STRING, TYPE_ANY, 1);
    // The name "title" is what the function will be called in the template text.
    set_func(func_map, "title", BURROW_ANY(TYPE_OF(CaseFunc), &title_fn));

    // A simple template definition to test our function. We print the input
    // text several ways: the original, title-cased, title-cased and then
    // printed with %q, and printed with %q and then title-cased.
    Str template_text = BURROW_S("\n"
                                 "Input: {{printf \"%q\" .}}\n"
                                 "Output 0: {{title .}}\n"
                                 "Output 1: {{title . | printf \"%q\"}}\n"
                                 "Output 2: {{printf \"%q\" . | title}}\n");

    // Create a template, add the function map, and parse the text.
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_parse(
        text_template_funcs(text_template_new(a, BURROW_S("titleTest")), func_map),
        template_text, &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("parsing: %s\n", err);
        return;
    }

    // Run the template to verify the output.
    Str data = BURROW_S("the go programming language");
    err = text_template_execute(tmpl, out, BURROW_ANY(TYPE_STRING, &data));
    if (BURROW_FAILED(err))
        fmt_printf_v("execution: %s\n", err);
    // doc: end
}

static void funcs(Alloc *a, IoWriter out) {
    // doc: funcs
    // Define a simple template to test the functions.
    Str tmpl = BURROW_S("{{ . | lower | repeat }}");

    // Define the template FuncMap with two functions.
    CaseFunc lower_fn = BURROW_FN(CaseFunc, lower, a);
    CaseFunc repeat2_fn = BURROW_FN(CaseFunc, repeat2, a);
    Map *func_map = map_make(a, TYPE_STRING, TYPE_ANY, 2);
    set_func(func_map, "lower", BURROW_ANY(TYPE_OF(CaseFunc), &lower_fn));
    set_func(func_map, "repeat", BURROW_ANY(TYPE_OF(CaseFunc), &repeat2_fn));

    // Define a new template, add the FuncMap with text_template_funcs and then
    // parse the template.
    Error err = BURROW_NO_ERROR;
    TextTemplate *parsed = text_template_parse(
        text_template_funcs(text_template_new(a, BURROW_S("t")), func_map), tmpl, &err);
    if (BURROW_FAILED(err)) {
        fmt_println_v(err);
        return;
    }
    Str abc = BURROW_S("ABC\n");
    err = text_template_execute(parsed, out, BURROW_ANY(TYPE_STRING, &abc));
    if (BURROW_FAILED(err)) {
        fmt_println_v(err);
        return;
    }

    // text_template_funcs must be called before a template is parsed to add
    // functions to it, and it can also be called after to overwrite them.
    // Here the function named "repeat" is overwritten.
    CaseFunc repeat3_fn = BURROW_FN(CaseFunc, repeat3, a);
    Map *overwrite = map_make(a, TYPE_STRING, TYPE_ANY, 1);
    set_func(overwrite, "repeat", BURROW_ANY(TYPE_OF(CaseFunc), &repeat3_fn));
    text_template_funcs(parsed, overwrite);
    Str def = BURROW_S("DEF\n");
    err = text_template_execute(parsed, out, BURROW_ANY(TYPE_STRING, &def));
    if (BURROW_FAILED(err))
        fmt_println_v(err);
    // doc: end
}

#define BOOK_FIELDS(F, T)                                                              \
    F(T, float, Stars, "")                                                             \
    F(T, Str, Name, "")
BURROW_STRUCT(Book, BOOK_FIELDS);
BURROW_PTR_TYPE(BookPtr, Book);

static void if_(Alloc *a, IoWriter out) {
    // doc: if
    Error err = BURROW_NO_ERROR;
    TextTemplate *tpl = text_template_parse(
        text_template_new(a, BURROW_S("book")),
        BURROW_S("{{ if (gt .Stars 4.0) }}\"{{.Name }}\" is a great book.{{ else }}"
                 "\"{{.Name}}\" is not a great book.{{ end }}"),
        &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("failed to parse template: %s\n", err);
        return;
    }

    Book book = {4.9F, BURROW_S("Good Night, Gopher")};
    BookPtr b = &book;
    err = text_template_execute(tpl, out, BURROW_ANY(TYPE_OF(BookPtr), &b));
    if (BURROW_FAILED(err))
        fmt_printf_v("failed to execute template: %s\n", err);
    fmt_fprint_v(out, BURROW_S("\n"));
    // doc: end
}

// A file to write into the directory the file examples load from.
typedef struct TemplateFile {
    const char *name;
    const char *contents;
} TemplateFile;

// Makes a temporary directory holding files, the way Go's createTestDir does.
static Str create_test_dir(Alloc *a, const TemplateFile *files, int n) {
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("template"), &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err));
    for (int i = 0; i < n; i++) {
        Str name = str_from_cstr(files[i].name);
        Str contents = str_from_cstr(files[i].contents);
        Str elems[] = {dir, name};
        Str path = filepath_join(a, slice_from(elems, 2, 2, TYPE_STRING));
        err = os_write_file(path, slice_from_str(a, contents), 0644);
        if (BURROW_FAILED(err))
            panic_str(error_text(err));
    }
    return dir;
}

// The glob pattern for every template in dir.
static Str tmpl_pattern(Alloc *a, Str dir) {
    Str elems[] = {dir, BURROW_S("*.tmpl")};
    return filepath_join(a, slice_from(elems, 2, 2, TYPE_STRING));
}

static void glob(Alloc *a, IoWriter out) {
    // doc: glob
    // Here we create a temporary directory and fill it with our sample
    // template definition files. Usually the template files would already
    // exist in some location known to the program.
    static const TemplateFile files[] = {
        // T0.tmpl is a plain template file that just invokes T1.
        {"T0.tmpl", "T0 invokes T1: ({{template \"T1\"}})"},
        // T1.tmpl defines a template, T1 that invokes T2.
        {"T1.tmpl", "{{define \"T1\"}}T1 invokes T2: ({{template \"T2\"}}){{end}}"},
        // T2.tmpl defines a template T2.
        {"T2.tmpl", "{{define \"T2\"}}This is T2{{end}}"},
    };
    Str dir = create_test_dir(a, files, 3);

    // pattern is the glob pattern used to find all the template files.
    Str pattern = tmpl_pattern(a, dir);

    // T0.tmpl is the first name matched, so it becomes the starting template,
    // the value text_template_from_glob returns.
    Error err = BURROW_NO_ERROR;
    TextTemplate *tmpl = text_template_from_glob(a, pattern, &err);
    tmpl = text_template_must(tmpl, err);

    err = text_template_execute(tmpl, out, (Any){NULL, NULL});
    if (BURROW_FAILED(err))
        fmt_printf_v("template execution: %s\n", err);
    fmt_fprint_v(out, BURROW_S("\n"));
    (void)os_remove_all(dir);
    // doc: end
}

static void helpers(Alloc *a, IoWriter out) {
    // doc: helpers
    static const TemplateFile files[] = {
        // T1.tmpl defines a template, T1 that invokes T2.
        {"T1.tmpl", "{{define \"T1\"}}T1 invokes T2: ({{template \"T2\"}}){{end}}"},
        // T2.tmpl defines a template T2.
        {"T2.tmpl", "{{define \"T2\"}}This is T2{{end}}"},
    };
    Str dir = create_test_dir(a, files, 2);
    Str pattern = tmpl_pattern(a, dir);

    // Load the helpers.
    Error err = BURROW_NO_ERROR;
    TextTemplate *templates = text_template_from_glob(a, pattern, &err);
    templates = text_template_must(templates, err);
    // Add one driver template to the bunch, with an explicit template
    // definition.
    text_template_parse(
        templates,
        BURROW_S("{{define `driver1`}}Driver 1 calls T1: ({{template `T1`}})\n{{end}}"),
        &err);
    if (BURROW_FAILED(err))
        fmt_println_v("parsing driver1:", err);
    // Add another driver template.
    text_template_parse(
        templates,
        BURROW_S("{{define `driver2`}}Driver 2 calls T2: ({{template `T2`}})\n{{end}}"),
        &err);
    if (BURROW_FAILED(err))
        fmt_println_v("parsing driver2:", err);
    // We load all the templates before execution. This package does not
    // require that, but html/template's escaping does, so it's a good habit.
    err = text_template_execute_template(templates, out, BURROW_S("driver1"),
                                         (Any){NULL, NULL});
    if (BURROW_FAILED(err))
        fmt_printf_v("driver1 execution: %s\n", err);
    err = text_template_execute_template(templates, out, BURROW_S("driver2"),
                                         (Any){NULL, NULL});
    if (BURROW_FAILED(err))
        fmt_printf_v("driver2 execution: %s\n", err);
    (void)os_remove_all(dir);
    // doc: end
}

static void share(Alloc *a, IoWriter out) {
    // doc: share
    static const TemplateFile files[] = {
        // T0.tmpl is a plain template file that just invokes T1.
        {"T0.tmpl", "T0 ({{.}} version) invokes T1: ({{template `T1`}})\n"},
        // T1.tmpl defines a template, T1 that invokes T2. Note T2 is not defined.
        {"T1.tmpl", "{{define \"T1\"}}T1 invokes T2: ({{template \"T2\"}}){{end}}"},
    };
    Str dir = create_test_dir(a, files, 2);
    Str pattern = tmpl_pattern(a, dir);

    // Load the drivers.
    Error err = BURROW_NO_ERROR;
    TextTemplate *drivers = text_template_from_glob(a, pattern, &err);
    drivers = text_template_must(drivers, err);

    // We must define an implementation of the T2 template. First we clone the
    // drivers, then add a definition of T2 to the template name space.

    // 1. Clone the helper set to create a new name space from which to run them.
    TextTemplate *first = text_template_clone(drivers, a, &err);
    if (BURROW_FAILED(err))
        fmt_println_v("cloning helpers:", err);
    // 2. Define T2, version A, and parse it.
    text_template_parse(first, BURROW_S("{{define `T2`}}T2, version A{{end}}"), &err);
    if (BURROW_FAILED(err))
        fmt_println_v("parsing T2:", err);

    // Now repeat the whole thing, using a different version of T2.
    // 1. Clone the drivers.
    TextTemplate *second = text_template_clone(drivers, a, &err);
    if (BURROW_FAILED(err))
        fmt_println_v("cloning drivers:", err);
    // 2. Define T2, version B, and parse it.
    text_template_parse(second, BURROW_S("{{define `T2`}}T2, version B{{end}}"), &err);
    if (BURROW_FAILED(err))
        fmt_println_v("parsing T2:", err);

    // Execute the templates in the reverse order to verify the first is
    // unaffected by the second.
    Str s2 = BURROW_S("second");
    err = text_template_execute_template(second, out, BURROW_S("T0.tmpl"),
                                         BURROW_ANY(TYPE_STRING, &s2));
    if (BURROW_FAILED(err))
        fmt_printf_v("second execution: %s\n", err);
    Str s1 = BURROW_S("first");
    err = text_template_execute_template(first, out, BURROW_S("T0.tmpl"),
                                         BURROW_ANY(TYPE_STRING, &s1));
    if (BURROW_FAILED(err))
        fmt_printf_v("first: execution: %s\n", err);
    (void)os_remove_all(dir);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    IoWriter out = os_file_as_io_writer(os_stdout_file());
    letter(a, out);
    block(a, out);
    func(a, out);
    funcs(a, out);
    if_(a, out);
    glob(a, out);
    helpers(a, out);
    share(a, out);
    arena_free(&ar);
    return 0;
}

/* Output:

Dear Aunt Mildred,

It was a pleasure to see you at the wedding.
Thank you for the lovely bone china tea set.

Best wishes,
Josie

Dear Uncle John,

It is a shame you couldn't make it to the wedding.
Thank you for the lovely moleskin pants.

Best wishes,
Josie

Dear Cousin Rodney,

It is a shame you couldn't make it to the wedding.

Best wishes,
Josie
Names:
- Gamora
- Groot
- Nebula
- Rocket
- Star-Lord
Names: Gamora, Groot, Nebula, Rocket, Star-Lord
Input: "the go programming language"
Output 0: The Go Programming Language
Output 1: "The Go Programming Language"
Output 2: "The Go Programming Language"
abc
abc
def
def
def
"Good Night, Gopher" is a great book.
T0 invokes T1: (T1 invokes T2: (This is T2))
Driver 1 calls T1: (T1 invokes T2: (This is T2))
Driver 2 calls T2: (This is T2)
T0 (second version) invokes T1: (T1 invokes T2: (T2, version B))
T0 (first version) invokes T1: (T1 invokes T2: (T2, version A))
*/
