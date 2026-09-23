/* The README's blocks that need nothing but the core, all in one program. The
 * README uses the short spellings, so this does too. */
#define BURROW_SHORT 1
#include <stdio.h>

#include "burrow/burrow.h"

static char **args;

static void numbers(void) {
    Int a = INT64_MAX, b = 3;
    Uint hash = 0x9e3779b9;
    double f = 1e300;

    // clang-format off
    // doc: numbers
    Int n = int_add(a, b);          /* wraps, like Go. a + b is undefined if it overflows */
    Int q = int_div(a, b);          /* b == 0 stops the program with Go's message */
    Uint h = uint_shl(hash, 70);    /* zero, not h << 6, which is what x86 does */
    Int i = int_from_float64(f);    /* saturates, NaN gives zero */
    // doc: end
    // clang-format on
    printf("%lld %lld %llu %lld\n", (long long)n, (long long)q, (unsigned long long)h,
           (long long)i);
}

static void results(void) {
    Str s = S("12a");
    Error err = BURROW_NO_ERROR;

    // clang-format off
    // doc: results
    Int n = strconv_atoi(s, &err);
    Int m = strconv_atoi(s, NULL);   /* do not care why it failed */
    // doc: end
    // clang-format on
    printf("%lld %lld " STR_FMT "\n", (long long)n, (long long)m,
           STR_ARG(error_text(err)));
}

static void text(Alloc *a) {
    // doc: strconv
    Str pi = strconv_format_float(a, 3.141592653589793, 'g', -1, 64);
    double back = strconv_parse_float(pi, 64, NULL);
    Str quoted = strconv_quote(a, S("tab\there, \xff"));
    // doc: end
    printf(STR_FMT " %d " STR_FMT "\n", STR_ARG(pi), back == 3.141592653589793,
           STR_ARG(quoted));

    // doc: fmt
    Int xs[] = {3, 1, 4};
    fmt_printf_v("%-6s|%5.2f|%v|%q\n", "pi", 3.14159, slice_from(xs, 3, 3, TYPE_INT),
                 'x');
    // doc: end
}

static void strings(void) {
    char **argv = args;

    // clang-format off
    // doc: strings
    Str name = S("burrow");
    Str arg  = str_from_cstr(argv[1]);   /* borrows, does not copy */
    printf(STR_FMT "\n", STR_ARG(name));
    // doc: end
    // clang-format on
    printf("no argument is %lld bytes\n", (long long)arg.len);
}

static void runes(void) {
    Str s = S("h\xc3\xa9!");

    // doc: runes
    Int i;
    Rune r;
    for (StrIter it = str_runes(s); str_next_rune(&it, &i, &r);)
        printf("%lld: %lx\n", (long long)i, (unsigned long)r);
    // doc: end
}

static void types(void) {
    Str a = S("go"), b = S("go");

    // doc: types
    const Type *t = TYPE_INT;
    printf("%u bytes\n", t->size);

    if (type_equal(TYPE_STRING, &a, &b))
        printf("same bytes\n");
    // doc: end
}

static void slices(Alloc *a) {
    // doc: slices
    Slice xs = slice_make(a, TYPE_INT, 0, 16);
    xs = APPEND(Int, a, xs, 42);
    Int v = AT(Int, xs, 0);
    // doc: end
    printf("xs[0] is %lld\n", (long long)v);
}

#define COUNTER_FIELDS(F, T) F(T, Int, n, "")
BURROW_STRUCT(Counter, COUNTER_FIELDS);

// doc: iface
static Int counter_read(void *self, Slice p, Error *err);
static const IoReaderVT counter_reader_vt = {TYPE_OF(Counter), counter_read};

IoReader counter_as_io_reader(Counter *c) {
    IoReader r = {&counter_reader_vt, c};
    return r;
}

Int read_some(IoReader r, Slice buf, Error *err) {
    return CALL(r, read, buf, err);
}
// doc: end

static Int counter_read(void *self, Slice p, Error *err) {
    Counter *c = self;
    Int i = 0;

    for (; i < p.len && c->n < 10; i++)
        AT(Byte, p, i) = (Byte)('0' + c->n++);
    if (i == 0)
        *err = io_eof;
    return i;
}

static Int written;

static Int sink_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)err;
    written += p.len;
    return p.len;
}

static const IoWriterVT sink_vt = {NULL, sink_write};

static void interfaces(Alloc *a) {
    Counter c = {0};
    Byte bytes[4];
    Error err = BURROW_NO_ERROR;
    Int got =
        read_some(counter_as_io_reader(&c), slice_from(bytes, 4, 4, TYPE_BYTE), &err);
    printf("read %lld: %.*s\n", (long long)got, (int)got, (const char *)bytes);

    // doc: any
    Any v = ANY_VAL(TYPE_INT, Int, 42);
    // doc: end
    printf("the any holds %lld\n", (long long)*(Int *)any_assert(v, TYPE_INT));

    IoWriter dst = {&sink_vt, NULL};
    IoReader src = counter_as_io_reader(&c);
    // doc: io
    int64_t n = io_copy(a, dst, src, &err);
    // doc: end
    printf("copied %lld, the sink saw %lld\n", (long long)n, (long long)written);
}

typedef struct PrefixEnv {
    Str prefix;
} PrefixEnv;

/* Declared before Filter is, which C allows for a declaration that is not the
 * definition. It keeps the README's block in the order the README wants. */
struct Filter;
Int count_if(Slice lines, struct Filter keep);

// doc: func
BURROW_FUNC(Filter, bool, Str s);

static bool has_prefix(void *env, Str s);

Int count_go_lines(Slice lines) {
    PrefixEnv e = {S("go")};
    return count_if(lines, FN(Filter, has_prefix, &e));
}
// doc: end

Int count_if(Slice lines, Filter keep) {
    Int n = 0;
    for (Int i = 0; i < lines.len; i++)
        if (BURROW_CALLF(keep, AT(Str, lines, i)))
            n++;
    return n;
}

static bool has_prefix(void *env, Str s) {
    PrefixEnv *e = env;
    return s.len >= e->prefix.len &&
           str_eq(str_from_bytes(s.p, e->prefix.len), e->prefix);
}

static void functions(Alloc *a) {
    Slice lines = slice_make(a, TYPE_STRING, 0, 4);
    lines = APPEND(Str, a, lines, S("go build"));
    lines = APPEND(Str, a, lines, S("make"));
    lines = APPEND(Str, a, lines, S("go test"));
    printf("%lld lines start with go\n", (long long)count_go_lines(lines));
}

static void maps(Alloc *a) {
    // doc: maps
    Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);
    MAP_SET(Str, Int, counts, S("the"), 1);

    Int *n = MAP_GET(Str, Int, counts, S("the"));
    if (n != NULL)
        (*n)++;
    // doc: end

    Map *m = counts;
    Int total = 0;
    // doc: map-iter
    const void *k;
    void *v;
    for (MapIter it = map_iter(m); map_next(&it, &k, &v);)
        total += *(Int *)v;
    // doc: end
    printf("the is counted %lld times\n", (long long)total);
}

static void parse(Str input) {
    if (input.len == 0)
        panic_str(S("empty input"));
}

static void log_bad_input(Str why) {
    printf("bad input: " STR_FMT "\n", STR_ARG(why));
}

static void panics(Alloc *a) {
    Str input = S("");
    Error err = BURROW_NO_ERROR;

    // doc: panic
    BURROW_TRY {
        parse(input);
    }
    BURROW_CATCH(p) {
        log_bad_input(panic_text(p));
        err = errors_new(a, BURROW_S("bad input"));
    }
    BURROW_TRY_END;
    // doc: end
    printf("err is %s\n", FAILED(err) ? "set" : "nil");
}

static void atomics(void) {
    // doc: atomics
    static int64_t hits;
    sync_atomic_add_int64(&hits, 1);

    static SyncAtomicInt64 served;
    sync_atomic_int64_add(&served, 1);
    // doc: end
    printf("hits %lld, served %lld\n", (long long)hits,
           (long long)sync_atomic_int64_load(&served));
}

// doc: locks
static SyncMutex mu;
static int balance;

void deposit(int n) {
    sync_mutex_lock(&mu);
    balance += n;
    sync_mutex_unlock(&mu);
}
// doc: end

typedef struct Job {
    Int n;
    Int result;
} Job;

static void work(void *env) {
    Job *j = env;
    j->result = j->n * 2;
    deposit((int)j->n);
}

static int starts;

static void start(void *env) {
    (void)env;
    starts++;
}

static void waiting(void) {
    Job jobs[3] = {{1, 0}, {2, 0}, {3, 0}};
    int n = 3;

    // doc: wait-group
    static SyncWaitGroup wg;

    for (int i = 0; i < n; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, work, &jobs[i]));

    sync_wait_group_wait(&wg);
    // doc: end
    printf("balance %d, last result %lld\n", balance, (long long)jobs[2].result);

    for (int i = 0; i < 2; i++) {
        // doc: once
        static SyncOnce started;

        sync_once_do(&started, BURROW_FN(Func, start, NULL));
        // doc: end
    }
    printf("started %d time\n", starts);
}

static void use(Int n) {
    printf("hits is %lld\n", (long long)n);
}

static void sync_map(void) {
    // doc: sync-map
    static SyncMap cache;
    cache = SYNC_MAP(heap_allocator(), TYPE_STRING, TYPE_INT);

    SYNC_MAP_STORE(Str, Int, &cache, BURROW_S("hits"), 1);

    Int n;
    if (SYNC_MAP_LOAD(Str, &cache, BURROW_S("hits"), &n))
        use(n);
    // doc: end
    sync_map_free(&cache);
}

#define BUF_FIELDS(F, T) F(T, Int, len, "")
BURROW_STRUCT(Buf, BUF_FIELDS);

static Any make_buf(void *env) {
    (void)env;
    return ANY(TYPE_OF(Buf), BURROW_NEW(heap_allocator(), Buf));
}

static void drop_buf(void *env, Any v) {
    (void)env;
    mem_free(heap_allocator(), v.data, sizeof(Buf), _Alignof(Buf));
}

static void fill(Buf *b) {
    b->len = 1;
    printf("got a buffer\n");
}

static void pool(void) {
    // doc: pool
    static SyncPool bufs;
    bufs = SYNC_POOL(heap_allocator(), BURROW_FN(SyncPoolNewFunc, make_buf, NULL),
                     BURROW_FN(SyncPoolFreeFunc, drop_buf, NULL));

    Any v = sync_pool_get(&bufs);
    Buf *b = any_assert(v, TYPE_OF(Buf));
    fill(b);
    sync_pool_put(&bufs, v);
    // doc: end
    sync_pool_free(&bufs);
}

static void run(void *env) {
    Arena ar;
    (void)env;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    numbers();
    results();
    strings();
    runes();
    text(a);
    types();
    slices(a);
    interfaces(a);
    functions(a);
    maps(a);
    panics(a);
    atomics();
    waiting();
    sync_map();
    pool();
    arena_free(&ar);
}

int main(int argc, char **argv) {
    (void)argc;
    args = argv;
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
-9223372036854775806 3074457345618258602 0 9223372036854775807
0 0 strconv.Atoi: parsing "12a": invalid syntax
burrow
no argument is 0 bytes
0: 68
1: e9
3: 21
3.141592653589793 1 "tab\there, \xff"
pi    | 3.14|[3 1 4]|'x'
8 bytes
same bytes
xs[0] is 42
read 4: 0123
the any holds 42
copied 6, the sink saw 6
2 lines start with go
the is counted 2 times
bad input: empty input
err is set
hits 1, served 1
balance 6, last result 6
started 1 time
hits is 1
got a buffer
*/
