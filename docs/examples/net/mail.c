#include <stdio.h>

#include "burrow/burrow.h"

#define P(s) (int)(s).len, (const char *)(s).p

static void message(Alloc *a) {
    // doc: message
    StringsReader *sr =
        strings_new_reader(a, BURROW_S("Date: Mon, 23 Jun 2015 11:40:36 -0400\n"
                                       "From: Gopher <from@example.com>\n"
                                       "To: Another Gopher <to@example.com>\n"
                                       "Subject: Gophers at Gophercon\n"
                                       "\n"
                                       "Message body\n"));
    Error err;
    MailMessage *m = mail_read_message(a, strings_reader_as_io_reader(sr), &err);
    if (m == NULL) {
        fmt_printf_v("%v\n", err);
        return;
    }
    printf("Date: %.*s\n", P(mail_header_get(m->header, BURROW_S("Date"))));
    printf("From: %.*s\n", P(mail_header_get(m->header, BURROW_S("From"))));
    printf("Subject: %.*s\n", P(mail_header_get(m->header, BURROW_S("Subject"))));
    Slice body = io_read_all(a, m->body, &err);
    printf("%.*s", (int)body.len, (const char *)body.p);

    Time t = mail_header_date(m->header, a, &err);
    printf("%.*s\n", P(time_format(t, a, TIME_RFC3339)));
    mail_message_free(m);
    // doc: end
}

static void addresses(Alloc *a) {
    // doc: addresses
    Error err;
    MailAddress *e = mail_parse_address(a, BURROW_S("Alice <alice@example.com>"), &err);
    if (e != NULL) {
        printf("%.*s %.*s\n", P(e->name), P(e->address));
        mail_address_free(a, e);
    }

    Slice list = mail_parse_address_list(
        a,
        BURROW_S(
            "Bob <bob@example.com>, eve@example.com, \"Gö, Pher\" <g@example.com>"),
        &err);
    MailAddress **v = (MailAddress **)list.p;
    for (Int i = 0; i < list.len; i++)
        printf("%.*s\n", P(mail_address_string(v[i], a)));
    mail_address_list_free(a, list);

    if (mail_parse_address(a, BURROW_S("John Doe"), &err) == NULL)
        fmt_printf_v("%v\n", err);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    message(a);
    addresses(a);
    arena_free(&ar);
    return 0;
}

/* Output:
*/
