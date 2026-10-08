/* What the tests reach into, as Go's smtp_test.go does with the package's
 * unexported names.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_NET_SMTP_INTERNAL_H
#define BURROW_SRC_NET_SMTP_INTERNAL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/smtp.h"

/* &Client{Text: textproto.NewConn(conn), localName: "localhost"}, a client
 * over conn that has not read a greeting. server_name is host. NULL when a
 * says no. */
SmtpClient *burrow__smtp_client_make(Alloc *a, NetConn conn, Str host);

/* Client.helo and Client.ehlo, which send HELO and EHLO. */
Error burrow__smtp_client_helo(SmtpClient *c);
Error burrow__smtp_client_ehlo(SmtpClient *c);

#endif /* BURROW_SRC_NET_SMTP_INTERNAL_H */
