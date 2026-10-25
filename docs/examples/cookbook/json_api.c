// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/net/http/httptest.h"

#define ACCOUNT_FIELDS(F, T)                                                           \
    F(T, Int, ID, "json:\"id\"")                                                       \
    F(T, Str, Name, "json:\"name\"")
BURROW_STRUCT(Account, ACCOUNT_FIELDS);

/* What the handlers share. The names come from a, since a request's arena
 * is gone once its handler returns. */
typedef struct Store {
    SyncMutex mu;
    Alloc *a;
    Account accounts[8];
    Int n;
} Store;

/* Writes v as the response's JSON body, with the status code. */
static void write_json(Alloc *a, HttpResponseWriter w, Int code, Any v) {
    http_header_set(http_response_writer_header(w), S("Content-Type"),
                    S("application/json"));
    http_response_writer_write_header(w, code);
    JsonEncoder *enc = json_new_encoder(a, http_response_writer_as_io_writer(w));
    (void)json_encoder_encode(enc, v);
    json_encoder_free(enc);
}

/* GET /users/{id} */
static void get_user(void *env, HttpResponseWriter w, HttpRequest *r) {
    Store *s = env;
    Error err = NO_ERROR;
    Int id = strconv_atoi(http_request_path_value(r, S("id")), &err);
    sync_mutex_lock(&s->mu);
    bool found = OK(err) && id >= 1 && id <= s->n;
    Account u = found ? s->accounts[id - 1] : (Account){0};
    sync_mutex_unlock(&s->mu);
    if (!found) {
        http_error(w, S("no such user"), HTTP_STATUS_NOT_FOUND);
        return;
    }
    write_json(arena_allocator(&r->arena), w, HTTP_STATUS_OK,
               ANY(TYPE_OF(Account), &u));
}

/* POST /users, with the new user's name in the body. */
static void add_user(void *env, HttpResponseWriter w, HttpRequest *r) {
    Store *s = env;
    Account u = {0};
    JsonDecoder *dec = json_new_decoder(arena_allocator(&r->arena),
                                        io_read_closer_as_io_reader(r->body));
    Error err = json_decoder_decode(dec, ANY(TYPE_OF(Account), &u));
    json_decoder_free(dec);
    if (FAILED(err) || u.Name.len == 0) {
        http_error(w, S("a user needs a name"), HTTP_STATUS_BAD_REQUEST);
        return;
    }
    sync_mutex_lock(&s->mu);
    bool full = s->n == (Int)(sizeof s->accounts / sizeof s->accounts[0]);
    if (!full) {
        u.ID = s->n + 1;
        u.Name = strings_clone(s->a, u.Name);
        s->accounts[s->n++] = u;
    }
    sync_mutex_unlock(&s->mu);
    if (full) {
        http_error(w, S("no room for another user"), HTTP_STATUS_INSUFFICIENT_STORAGE);
        return;
    }
    write_json(arena_allocator(&r->arena), w, HTTP_STATUS_CREATED,
               ANY(TYPE_OF(Account), &u));
}

/* Prints the user in res, or what went wrong. */
static void show(Alloc *a, Str what, HttpResponse *res, Error err, Int want) {
    if (FAILED(err)) {
        fmt_println_v(what, err);
        return;
    }
    IoReader body = io_read_closer_as_io_reader(res->body);
    if (res->status_code != want) {
        Slice b = io_read_all(a, body, NULL);
        Str text = strings_trim_space(str_from_bytes(b.p, b.len));
        fmt_printf_v("%s: %s: %s\n", what, res->status, text);
    } else {
        Account u = {0};
        JsonDecoder *dec = json_new_decoder(a, body);
        err = json_decoder_decode(dec, ANY(TYPE_OF(Account), &u));
        json_decoder_free(dec);
        if (FAILED(err))
            fmt_println_v(what, err);
        else
            fmt_printf_v("%s: %+v\n", what, ANY(TYPE_OF(Account), &u));
    }
    http_response_free(res);
}

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Store store = {
        .a = a, .accounts = {{1, S("Ada Lovelace")}, {2, S("Linus Torvalds")}}, .n = 2};
    HttpServeMux *mux = http_new_serve_mux(heap_allocator());
    http_serve_mux_handle_func(mux, S("GET /users/{id}"),
                               FN(HttpHandlerFunc, get_user, &store));
    http_serve_mux_handle_func(mux, S("POST /users"),
                               FN(HttpHandlerFunc, add_user, &store));
    HttptestServer *ts =
        httptest_new_server(heap_allocator(), http_serve_mux_as_handler(mux));
    HttpClient *c = httptest_server_client(ts);

    Error err = NO_ERROR;
    Str url = fmt_sprintf_v(a, "%s/users/1", ts->url);
    HttpResponse *res = http_client_get(c, url, &err);
    show(a, S("GET /users/1"), res, err, HTTP_STATUS_OK);

    url = fmt_sprintf_v(a, "%s/users", ts->url);
    StringsReader *body = strings_new_reader(a, S("{\"name\": \"Grace Hopper\"}"));
    res = http_client_post(c, url, S("application/json"),
                           strings_reader_as_io_reader(body), &err);
    show(a, S("POST /users"), res, err, HTTP_STATUS_CREATED);

    body = strings_new_reader(a, S("{}"));
    res = http_client_post(c, url, S("application/json"),
                           strings_reader_as_io_reader(body), &err);
    show(a, S("POST /users"), res, err, HTTP_STATUS_CREATED);

    url = fmt_sprintf_v(a, "%s/users/3", ts->url);
    res = http_client_get(c, url, &err);
    show(a, S("GET /users/3"), res, err, HTTP_STATUS_OK);

    url = fmt_sprintf_v(a, "%s/users/9", ts->url);
    res = http_client_get(c, url, &err);
    show(a, S("GET /users/9"), res, err, HTTP_STATUS_OK);

    httptest_server_free(ts);
    http_serve_mux_free(mux);
    arena_free(&ar);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
// doc: end

/* Output:
GET /users/1: {ID:1 Name:Ada Lovelace}
POST /users: {ID:3 Name:Grace Hopper}
POST /users: 400 Bad Request: a user needs a name
GET /users/3: {ID:3 Name:Grace Hopper}
GET /users/9: 404 Not Found: no such user
*/
