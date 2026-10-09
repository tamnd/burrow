/* net/smtp, the Simple Mail Transfer Protocol as defined in RFC 5321.
 *
 * Go's net/smtp, a client. It also does these extensions:
 *
 *     8BITMIME  RFC 1652
 *     AUTH      RFC 2554
 *
 * smtp_send_mail is the whole job in one call. For more control, dial with
 * smtp_dial and go through the commands one at a time:
 *
 *     Error err;
 *     SmtpClient *c = smtp_dial(a, BURROW_S("mail.example.com:25"), &err);
 *     if (c != NULL) {
 *         err = smtp_client_mail(c, BURROW_S("sender@example.org"));
 *         ...
 *         err = smtp_client_quit(c);
 *         smtp_client_free(c);
 *     }
 *
 * STARTTLS, RFC 3207, needs crypto/tls, which is not ported yet, so
 * smtp_client_start_tls and smtp_client_tls_connection_state are still to
 * come. Until then smtp_send_mail returns smtp_err_no_tls for a server that
 * offers STARTTLS, rather than send the mail in the clear where Go would have
 * encrypted it.
 *
 * Errors are made in the calling goroutine's error arena, as everywhere else
 * in burrow. A response with the wrong code is a TextprotoError.
 *
 * Go's smtp package is frozen and is not accepting new features, and the same
 * goes for this one.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/smtp */

#ifndef BURROW_NET_SMTP_H
#define BURROW_NET_SMTP_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/textproto.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- Auth */

/* smtp.ServerInfo, what an SMTP server says about itself. */
typedef struct SmtpServerInfo {
    Str name;   /* SMTP server name */
    bool tls;   /* using TLS, with valid certificate for name */
    Slice auth; /* advertised authentication mechanisms, of Str */
} SmtpServerInfo;

/* smtp.Auth, an SMTP authentication mechanism.
 *
 * start begins an authentication with a server. It returns the name of the
 * authentication protocol and, in *to_server, optionally data to include in
 * the initial AUTH message sent to the server. If it gives an error, the SMTP
 * client aborts the authentication attempt and closes the connection.
 *
 * next continues the authentication. The server has just sent the from_server
 * data. If more is true, the server expects a response, which next should
 * return; otherwise next should return Go's nil, a Slice with a NULL p. If
 * next gives an error, the SMTP client aborts the authentication attempt and
 * closes the connection.
 *
 * What either returns comes from a. */
typedef struct SmtpAuthVT {
    const Type *self_type;
    Str (*start)(void *self, const SmtpServerInfo *server, Alloc *a, Slice *to_server,
                 Error *err);
    Slice (*next)(void *self, Slice from_server, bool more, Alloc *a, Error *err);
} SmtpAuthVT;

typedef struct SmtpAuth {
    const SmtpAuthVT *vt;
    void *data;
} SmtpAuth;

/* smtp.PlainAuth. An SmtpAuth for the PLAIN mechanism of RFC 4616, which
 * authenticates to host as username with password and acts as identity.
 * Usually identity should be empty, to act as username.
 *
 * It only sends the credentials if the connection is using TLS or is to
 * localhost. Otherwise authentication fails with an error, without sending
 * them.
 *
 * The strings are copied into a, all in one allocation with the rest, which
 * smtp_auth_free gives back. The nil SmtpAuth when a says no. */
BURROW_OWNS(ret) SmtpAuth smtp_plain_auth(Alloc *a, Str identity, Str username,
                                          Str password, Str host);

/* smtp.CRAMMD5Auth. An SmtpAuth for the CRAM-MD5 mechanism of RFC 2195, which
 * authenticates as username with the challenge and response mechanism and
 * secret. Allocated as smtp_plain_auth's is. */
BURROW_OWNS(ret) SmtpAuth smtp_crammd5_auth(Alloc *a, Str username, Str secret);

/* Gives back an SmtpAuth from smtp_plain_auth or smtp_crammd5_auth to the a it
 * came from. The nil SmtpAuth does nothing. */
void smtp_auth_free(Alloc *a, SmtpAuth auth);

/* ----------------------------------------------------------------- Client */

/* What smtp_send_mail gives when the server offers STARTTLS, which needs
 * crypto/tls. Not in Go. */
extern const Error smtp_err_no_tls;

/* smtp.Client, a client connection to an SMTP server.
 *
 * text is the TextprotoConn the client talks through. It is there to allow
 * for clients to add extensions. The rest is the package's. */
typedef struct SmtpClient {
    TextprotoConn *text;

    NetConn conn;
    bool owns_conn; /* smtp_dial made conn, so smtp_client_free frees it */
    bool tls;       /* whether the client is using TLS */
    Str server_name;
    Map *ext;          /* supported extensions, of Str to Str */
    Slice auth;        /* supported auth mechanisms, of Str */
    Str local_name;    /* the name to use in HELO/EHLO */
    bool did_hello;    /* whether we've said HELO/EHLO */
    Error hello_error; /* the error from the hello */

    IoReadWriteCloser rwc; /* conn, for text */
    IoWriteCloser dot;     /* the writer under the one smtp_client_data gives */
    Alloc *a;
    Arena arena;   /* what lasts as long as the client */
    Arena scratch; /* what lasts for one command */
} SmtpClient;

/* smtp.Dial. A new SmtpClient connected to the SMTP server at addr, which must
 * include a port, as in "mail.example.com:smtp". The client and the
 * connection come from a, and smtp_client_free gives both back. */
BURROW_OWNS(ret) SmtpClient *smtp_dial(Alloc *a, Str addr, Error *err);

/* smtp.NewClient. A new SmtpClient over conn, a connection to the SMTP server
 * host, once it has read the server's greeting. When the greeting is not
 * there it closes conn and gives NULL and the error. The client comes from a,
 * and conn stays the caller's, to free once the client is done with it. */
BURROW_OWNS(ret) SmtpClient *smtp_new_client(Alloc *a, NetConn conn, Str host,
                                             Error *err);

/* Client.Close, which closes the connection. It does not free c. */
BURROW_BORROWS(ret) Error smtp_client_close(SmtpClient *c);

/* Gives back c, and the connection too if smtp_dial made it, without closing
 * anything else. NULL is fine. */
void smtp_client_free(SmtpClient *c);

/* Client.Hello. Sends a HELO or EHLO to the server as local_name. Only needed
 * to control the name; the other functions send it themselves the first time,
 * as "localhost". If it is called, it has to be before any of the others. */
BURROW_BORROWS(ret) Error smtp_client_hello(SmtpClient *c, Str local_name);

/* Client.Verify. Checks the validity of an email address on the server. If it
 * gives no error, the address is valid. A refusal does not mean the address is
 * invalid, as many servers will not verify addresses for security reasons. */
BURROW_BORROWS(ret) Error smtp_client_verify(SmtpClient *c, Str addr);

/* Client.Auth. Authenticates with the mechanism auth. Only servers that
 * advertise the AUTH extension support this. */
BURROW_BORROWS(ret) Error smtp_client_auth(SmtpClient *c, SmtpAuth auth);

/* Client.Mail. Issues a MAIL command to the server using from as the sender's
 * address. If the server supports the 8BITMIME extension, it adds the
 * BODY=8BITMIME parameter, and if it supports SMTPUTF8, the SMTPUTF8 one. This
 * starts a mail transaction, which smtp_client_rcpt and smtp_client_data go
 * on with. */
BURROW_BORROWS(ret) Error smtp_client_mail(SmtpClient *c, Str from);

/* Client.Rcpt. Issues a RCPT command to the server using to as the
 * recipient's address. A call to smtp_client_mail has to come first, and it
 * may be followed by smtp_client_data or another smtp_client_rcpt. */
BURROW_BORROWS(ret) Error smtp_client_rcpt(SmtpClient *c, Str to);

/* Client.Data. Issues a DATA command and gives a writer for the mail headers
 * and body. Closing the writer finishes the message and reads the server's
 * answer. The writer belongs to c, and smtp_client_rcpt has to have been
 * called first. On an error the writer is the nil IoWriteCloser. */
BURROW_BORROWS(ret, c) IoWriteCloser smtp_client_data(SmtpClient *c, Error *err);

/* Client.Extension. Whether the server supports the extension ext, ignoring
 * case, and in *param, when it is not NULL, what the server sent with it. The
 * parameter belongs to c and lasts until the next hello, if there is one. */
bool smtp_client_extension(SmtpClient *c, Str ext, Str *param);

/* Client.Reset. Sends RSET, which aborts the mail transaction in progress. */
BURROW_BORROWS(ret) Error smtp_client_reset(SmtpClient *c);

/* Client.Noop. Sends NOOP, which does nothing but check that the connection
 * to the server is all right. */
BURROW_BORROWS(ret) Error smtp_client_noop(SmtpClient *c);

/* Client.Quit. Sends QUIT and closes the connection to the server. It does
 * not free c. */
BURROW_BORROWS(ret) Error smtp_client_quit(SmtpClient *c);

/* smtp.SendMail. Connects to the server at addr, switches to TLS if possible,
 * authenticates with auth if that is not the nil SmtpAuth and the server
 * supports it, and sends an email from the address from, to the addresses in
 * to, a Slice of Str, with the message msg, a Slice of Byte. addr must include
 * a port, as in "mail.example.com:smtp".
 *
 * The addresses in to are the SMTP RCPT addresses. msg should be an RFC 822
 * style email with headers first, a blank line, and then the message body.
 * Its lines should be CRLF terminated. Its headers would usually include
 * fields such as "From", "To", "Subject", and "Cc". Sending "Bcc" messages is
 * done by including an address in to but not in the headers of msg.
 *
 * TLS is the part still to come, so a server that offers STARTTLS gets no
 * mail, and the error is smtp_err_no_tls. */
BURROW_BORROWS(ret) Error smtp_send_mail(Str addr, SmtpAuth auth, Str from, Slice to,
                                         Slice msg);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_SMTP_H */
