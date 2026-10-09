#include "burrow/burrow.h"

static void set(Map *m, const char *k, const char *v) {
    Str ks = str_from_cstr(k), vs = str_from_cstr(v);
    map_set(m, &ks, &vs);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    // doc: request
    /* What a web server puts in a CGI program's environment. */
    Map *env = map_make(a, TYPE_STRING, TYPE_STRING, 0);
    set(env, "SERVER_PROTOCOL", "HTTP/1.1");
    set(env, "REQUEST_METHOD", "POST");
    set(env, "HTTP_HOST", "example.com");
    set(env, "REQUEST_URI", "/search?q=gopher");
    set(env, "CONTENT_LENGTH", "11");
    set(env, "HTTP_USER_AGENT", "curl/8.5.0");
    set(env, "REMOTE_ADDR", "10.0.0.7");
    set(env, "REMOTE_PORT", "51234");

    HttpRequest *req = cgi_request_from_map(a, env, &err);
    if (req != NULL) {
        fmt_printf_v("%s %s\n", req->method, url_string(req->url, a));
        fmt_printf_v("host %s, from %s, %d bytes\n", req->host, req->remote_addr,
                     req->content_length);
        fmt_printf_v("User-Agent %s\n",
                     http_header_get(req->header, BURROW_S("User-Agent")));
        http_request_free(req);
    }
    // doc: end
    arena_free(&ar);
    return 0;
}

/* Output:
POST http://example.com/search?q=gopher
host example.com, from 10.0.0.7:51234, 11 bytes
User-Agent curl/8.5.0
*/
