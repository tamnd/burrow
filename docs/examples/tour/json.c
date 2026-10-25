// doc: json
#define BURROW_SHORT 1
#include "burrow/burrow.h"

BURROW_SLICE_TYPE(Strs, Str);
#define REPO_FIELDS(F, T)                                                              \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, Int, Stars, "json:\"stars\"")                                                 \
    F(T, Strs, Topics, "json:\"topics\"")
BURROW_STRUCT(Repo, REPO_FIELDS);

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Repo r = {0};
    Error err = json_unmarshal(
        a, BURROW_B("{\"name\": \"burrow\", \"stars\": 41, \"topics\": [\"c\"]}"),
        ANY(TYPE_OF(Repo), &r));
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    r.Stars++;
    r.Topics = APPEND(Str, a, r.Topics, S("go"));
    Slice out = json_marshal_indent(a, ANY(TYPE_OF(Repo), &r), S(""), S("  "), &err);
    fmt_println_v(str_from_bytes(out.p, out.len));
    arena_free(&ar);
}
// doc: end

/* Output:
{
  "name": "burrow",
  "stars": 42,
  "topics": [
    "c",
    "go"
  ]
}
*/
