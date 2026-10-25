// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/net/mail.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A list can mix bare addresses with named ones, and a name with a comma
     * in it has to be in quotes. */
    Error err = NO_ERROR;
    Slice list =
        mail_parse_address_list(a,
                                S("Ada Lovelace <ada@example.com>, bob@example.com, "
                                  "\"Smith, Jo\" <jo@example.org>"),
                                &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    for (Int i = 0; i < list.len; i++) {
        MailAddress *m = AT(MailAddress *, list, i);
        fmt_printf_v("name %-14q address %s\n", m->name, m->address);
    }

    /* Going the other way quotes the name, and a name that is not plain ASCII
     * goes in an encoded word so it can travel in a mail header. */
    MailAddress zoe = {S("Zo\xc3\xab"), S("zoe@example.com"), NULL, 0};
    fmt_println_v(mail_address_string(&zoe, a));

    static const char *bad[] = {"ada@", "Ada Lovelace", "<ada@example.com"};
    for (Int i = 0; i < 3; i++) {
        Str s = str_from_cstr(bad[i]);
        mail_parse_address(a, s, &err);
        fmt_printf_v("%-18q %v\n", s, err);
    }
    arena_free(&ar);
}
// doc: end

/* Output:
name "Ada Lovelace" address ada@example.com
name ""             address bob@example.com
name "Smith, Jo"    address jo@example.org
=?utf-8?q?Zo=C3=AB?= <zoe@example.com>
"ada@"             mail: missing '@' or angle-addr
"Ada Lovelace"     mail: no angle-addr
"<ada@example.com" mail: unclosed angle-addr
*/
