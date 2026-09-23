#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

// doc: declare
BURROW_FUNC(Filter, bool, Str s);
BURROW_FUNC(ReadFn, Int, Slice p, Error *err);
// doc: end

// doc: target
static bool is_empty(void *env, Str s) {
    (void)env;
    return s.len == 0;
}
// doc: end

static void close_it(void *env) {
    fclose(env);
    printf("closed\n");
}

static void build_and_call(void) {
    FILE *file = fopen("notes.txt", "w");
    if (file == NULL)
        return;
    Str line = BURROW_S("");

    // doc: build
    Filter f = BURROW_FN(Filter, is_empty, NULL);
    Func done = BURROW_FN(Func, close_it, file);
    // doc: end

    // doc: call
    bool keep = BURROW_CALLF(f, line);
    BURROW_CALLF0(done);
    // doc: end
    printf("keep the empty line: %d\n", keep);
}

static void nil(void) {
    // doc: nil
    Filter f = {NULL, NULL};
    if (BURROW_FUNC_IS_NIL(f)) {
        printf("f is nil\n");
    }
    // doc: end
}

// doc: env
typedef struct PrefixEnv {
    Str prefix;
} PrefixEnv;

static bool has_prefix(void *env, Str s) {
    PrefixEnv *e = (PrefixEnv *)env;
    if (s.len < e->prefix.len)
        return false;
    return str_eq(str_from_bytes(s.p, e->prefix.len), e->prefix);
}
// doc: end

static void capture(void) {
    // doc: value
    PrefixEnv e = {BURROW_S("go")};
    Filter f = BURROW_FN(Filter, has_prefix, &e);
    // doc: end
    printf("gopher %d, rust %d\n", BURROW_CALLF(f, BURROW_S("gopher")),
           BURROW_CALLF(f, BURROW_S("rust")));
}

static Int count_if(Slice lines, Filter keep) {
    Int n = 0;
    for (Int i = 0; i < lines.len; i++)
        if (BURROW_CALLF(keep, BURROW_AT(Str, lines, i)))
            n++;
    return n;
}

static void on_stack(void) {
    Str words[3] = {BURROW_S("go"), BURROW_S("gopher"), BURROW_S("rust")};
    Slice lines = slice_from(words, 3, 3, TYPE_STRING);

    // doc: stack
    PrefixEnv e = {BURROW_S("go")};
    Int n = count_if(lines, BURROW_FN(Filter, has_prefix, &e));
    // doc: end
    printf("%lld lines start with go\n", (long long)n);
}

typedef struct Handler {
    Filter keep;
} Handler;

static Handler *make_handler(Alloc *a) {
    Handler *h = BURROW_NEW(a, Handler);

    // doc: heap
    PrefixEnv *e = BURROW_NEW(a, PrefixEnv);
    e->prefix = BURROW_S("go");
    h->keep = BURROW_FN(Filter, has_prefix, e);
    // doc: end
    return h;
}

// doc: cost
_Static_assert(sizeof(Filter) == 2 * sizeof(void *), "two words");
// doc: end

int main(void) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    build_and_call();
    nil();
    capture();
    on_stack();
    Handler *h = make_handler(a);
    printf("handler keeps golang: %d\n", BURROW_CALLF(h->keep, BURROW_S("golang")));
    arena_free(&ar);
    return 0;
}

/* Output:
closed
keep the empty line: 1
f is nil
gopher 1, rust 0
2 lines start with go
handler keeps golang: 1
*/
