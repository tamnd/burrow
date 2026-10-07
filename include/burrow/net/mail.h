/* net/mail, parsing mail messages.
 *
 * Go's net/mail. mail_read_message splits a message into its header and its
 * body, mail_parse_address and mail_parse_address_list read the addresses in
 * fields such as From and To, and mail_parse_date reads a Date field.
 *
 *     Error err;
 *     MailAddress *addr = mail_parse_address(a, BURROW_S("Alice <alice@example.com>"),
 *                                            &err);
 *     if (BURROW_OK(err)) {
 *         // addr->name is "Alice", addr->address is "alice@example.com"
 *         mail_address_free(a, addr);
 *     }
 *
 * For the most part this follows the syntax of RFC 5322 as extended by RFC
 * 6532, and it departs from it in the places Go does:
 *
 *   - Obsolete address forms are not parsed, including addresses with
 *     embedded route information.
 *   - Not all of the spacing RFC 5322 calls CFWS is supported, such as an
 *     address broken across lines.
 *   - No Unicode normalisation is done.
 *   - A leading From line, as in the mbox format of RFC 4155, is allowed.
 *
 * A MailAddress from this package is one allocation that holds the struct and
 * both strings, as a Url is, so the input can go away while it lives, and
 * mail_address_free gives it back. A list is a Slice of MailAddress *, each
 * one its own allocation, and mail_address_list_free gives back the lot.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/mail */

#ifndef BURROW_NET_MAIL_H
#define BURROW_NET_MAIL_H

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mime.h"
#include "burrow/net/textproto.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- Header */

/* mail.Header, the fields of a message header, each key to its values in the
 * order they came. It is the same Map as a TextprotoMIMEHeader, so the
 * textproto_mime_header functions work on it, and the keys are in the
 * canonical form textproto_canonical_mime_header_key gives. */
typedef TextprotoMIMEHeader MailHeader;

/* mail.ErrHeaderNotPresent, "mail: header not in message", which is what
 * mail_header_date and mail_header_address_list give for a field that is not
 * there. */
extern const Error mail_err_header_not_present;

/* Header.Get. The first value for key, or "" when there is none. key is put in
 * canonical form first, so "content-type" finds "Content-Type". To get every
 * value of a key, or one that is not canonical, use the Map. */
BURROW_BORROWS(ret, h) Str mail_header_get(MailHeader h, Str key);

/* Header.Date. The Date field read by mail_parse_date, with any fixed zone it
 * needs made from a. */
Time mail_header_date(MailHeader h, Alloc *a, Error *err);

/* Header.AddressList. The field key read by mail_parse_address_list, with the
 * list made from a. */
BURROW_OWNS(ret) Slice mail_header_address_list(MailHeader h, Alloc *a, Str key,
                                                Error *err);

/* ------------------------------------------------------------------ Message */

/* mail.Message, a message whose header has been read. body reads what comes
 * after the header. The other fields are the message's own. */
typedef struct MailMessage {
    MailHeader header;
    IoReader body;

    Alloc *a;
    Arena arena; /* the header and its strings */
    BufioReader *br;
    TextprotoReader *tp;
} MailMessage;

extern const Type *const TYPE_MAIL_MESSAGE;

/* mail.ReadMessage. Reads the header from r, and leaves the body to be read
 * from the message's body. The message comes from a, and so do the reader
 * and the buffer behind body. The header lives in an arena of the message's
 * own, so it is good until mail_message_free.
 *
 * Like Go, this does not check the header lines the way textproto does for
 * HTTP, since RFC 5322 is looser, and a header that ends at the end of the
 * input with no blank line after it is still a message. NULL with err set
 * means the header could not be read, or burrow_err_out_of_memory that a said
 * no. r should have a limit on its size, as with textproto. */
BURROW_OWNS(ret) MailMessage *mail_read_message(Alloc *a, IoReader r, Error *err);

/* Gives back the message, its header and its reader. body cannot be read
 * after it. NULL is fine. */
void mail_message_free(MailMessage *m);

/* -------------------------------------------------------------------- dates */

/* mail.ParseDate. A date in the form RFC 5322 gives, such as "Fri, 21 Nov
 * 1997 09:55:06 -0600", with the obsolete forms Go takes as well, such as a
 * two digit year, a zone name such as "GMT" or "UT", and a comment after the
 * zone. A zone offset or name that is not UTC or Local gives a fixed zone made
 * from a. */
Time mail_parse_date(Alloc *a, Str date, Error *err);

/* ---------------------------------------------------------------- addresses */

/* mail.Address. "Barry Gibbs <bg@example.com>" is the name "Barry Gibbs" and
 * the address "bg@example.com". The name may be empty.
 *
 * mem and size are not yours. They say where the block of a MailAddress from
 * this package is, and stay zero in one you fill in yourself. */
typedef struct MailAddress {
    Str name;    /* the proper name, which may be empty */
    Str address; /* user@domain */

    void *mem;
    size_t size;
} MailAddress;

extern const Type *const TYPE_MAIL_ADDRESS;

/* mail.ParseAddress. A single address, such as "Barry Gibbs <bg@example.com>"
 * or "bg@example.com", in one allocation from a. NULL with err set when it is
 * not one address, or when a said no. */
BURROW_OWNS(ret) MailAddress *mail_parse_address(Alloc *a, Str address, Error *err);

/* mail.ParseAddressList. A list of addresses separated by commas, which may
 * include groups such as "Team: a@example.com, b@example.com;", as a Slice of
 * MailAddress *. The array and each address come from a. An error gives an
 * empty Slice, and so does a group with nobody in it. When a says no the error
 * is burrow_err_out_of_memory. */
BURROW_OWNS(ret) Slice mail_parse_address_list(Alloc *a, Str list, Error *err);

/* Gives back an address from this package. NULL is fine, and so is one you
 * filled in yourself, which is left alone. */
void mail_address_free(Alloc *a, MailAddress *addr);

/* Gives back each address in list and then the array. */
void mail_address_list_free(Alloc *a, Slice list);

/* Address.String. The address in the form RFC 5322 gives, made in a: the name
 * in quotes, or as an RFC 2047 encoded word when it is not plain ASCII, then
 * the address in angle brackets, with the part before the @ quoted when it
 * needs to be. "Bob" and "bob@example.com" give `"Bob" <bob@example.com>`.
 * The empty string when a says no. */
BURROW_OWNS(ret) Str mail_address_string(const MailAddress *addr, Alloc *a);

/* mail.AddressParser. word_decoder, which may be NULL, decodes the RFC 2047
 * encoded words in names. Without one, the charsets the mime package knows,
 * UTF-8, ISO-8859-1 and US-ASCII, work and any other is an error. A zeroed
 * MailAddressParser is ready to use. */
typedef struct MailAddressParser {
    const MimeWordDecoder *word_decoder;
} MailAddressParser;

extern const Type *const TYPE_MAIL_ADDRESS_PARSER;

/* AddressParser.Parse and ParseList. mail_parse_address and
 * mail_parse_address_list with p's decoder. */
BURROW_OWNS(ret) MailAddress *mail_address_parser_parse(const MailAddressParser *p,
                                                        Alloc *a, Str address,
                                                        Error *err);
BURROW_OWNS(ret) Slice mail_address_parser_parse_list(const MailAddressParser *p,
                                                      Alloc *a, Str list, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_MAIL_H */
