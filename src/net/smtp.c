/* Derived from Go's src/net/smtp/smtp.go, the client.
 *
 * Go's responses and the strings made from them are left to its collector.
 * Here they go in the client's scratch arena, which each exported function
 * marks on the way in and releases on the way out, and an error that leaves
 * is retained in the error arena first. What has to last as long as the
 * client, the extensions the server sent and the error from the hello, is in
 * the client's own arena.
 *
 * StartTLS and TLSConnectionState wait for crypto/tls.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/smtp.h"

#include "smtp_internal.h"

#include "burrow/core.h"
#include "burrow/encoding/base64.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/textproto.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdint.h>

BURROW_SENTINEL_ERROR(smtp_err_no_tls,
                      "smtp: server offers STARTTLS, which needs crypto/tls, not "
                      "ported yet");
BURROW_SENTINEL_ERROR(smc_err_line, "smtp: A line must not contain CR or LF");
BURROW_SENTINEL_ERROR(smc_err_hello_after, "smtp: Hello called after other methods");
BURROW_SENTINEL_ERROR(smc_err_no_auth, "smtp: server doesn't support AUTH");

/* An error that is about to outlive the scratch arena. */
static Error smc_keep(Error err) {
    return BURROW_OK(err) ? err : error_retain(error_allocator(), err);
}

/* The IoReadWriteCloser that text talks through, which is c->conn. */
static Int smc_rwc_read(void *self, Slice p, Error *err) {
    NetConn conn = ((SmtpClient *)self)->conn;
    return conn.vt->reader.read(conn.data, p, err);
}

static Int smc_rwc_write(void *self, Slice p, Error *err) {
    NetConn conn = ((SmtpClient *)self)->conn;
    return conn.vt->writer.write(conn.data, p, err);
}

static Error smc_rwc_close(void *self) {
    NetConn conn = ((SmtpClient *)self)->conn;
    return conn.vt->closer.close(conn.data);
}

static const IoReadWriteCloserVT smc_rwc_vt = {
    {NULL, smc_rwc_read},
    {NULL, smc_rwc_write},
    {NULL, smc_rwc_close},
};

SmtpClient *burrow__smtp_client_make(Alloc *a, NetConn conn, Str host) {
    SmtpClient *c = BURROW_NEW(a, SmtpClient);
    if (c == NULL)
        return NULL;
    c->a = a;
    c->conn = conn;
    c->rwc = (IoReadWriteCloser){&smc_rwc_vt, c};
    arena_init(&c->arena, a, 0);
    arena_init(&c->scratch, a, 0);
    c->text = textproto_new_conn(a, c->rwc);
    c->server_name = str_clone(arena_allocator(&c->arena), host);
    c->local_name = BURROW_S("localhost");
    c->auth = slice_nil(TYPE_STRING);
    if (c->text == NULL || c->server_name.len != host.len) {
        smtp_client_free(c);
        return NULL;
    }
    return c;
}

SmtpClient *smtp_dial(Alloc *a, Str addr, Error *err) {
    Error e;
    NetConn conn = net_dial(a, BURROW_S("tcp"), addr, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    Str port;
    Str host = net_split_host_port(addr, &port, NULL);
    SmtpClient *c = smtp_new_client(a, conn, host, err);
    if (c == NULL)
        net_conn_free(conn);
    else
        c->owns_conn = true;
    return c;
}

SmtpClient *smtp_new_client(Alloc *a, NetConn conn, Str host, Error *err) {
    SmtpClient *c = burrow__smtp_client_make(a, conn, host);
    if (c == NULL) {
        (void)conn.vt->closer.close(conn.data);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Str msg;
    Error e;
    (void)textproto_conn_read_response(c->text, arena_allocator(&c->scratch), 220, &msg,
                                       &e);
    if (BURROW_FAILED(e)) {
        e = smc_keep(e);
        (void)textproto_conn_close(c->text);
        smtp_client_free(c);
        BURROW_OUT(err, e);
        return NULL;
    }
    arena_reset(&c->scratch);
    /* Go's c.tls says whether conn is a *tls.Conn, which it cannot be yet. */
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

Error smtp_client_close(SmtpClient *c) {
    return textproto_conn_close(c->text);
}

void smtp_client_free(SmtpClient *c) {
    if (c == NULL)
        return;
    textproto_conn_free(c->text);
    if (c->owns_conn)
        net_conn_free(c->conn);
    arena_free(&c->scratch);
    arena_free(&c->arena);
    mem_free(c->a, c, sizeof *c, _Alignof(SmtpClient));
}

/* cmd is a convenience function that sends a command and returns the
 * response. The message is in the scratch arena. */
static Int smc_cmd(SmtpClient *c, Int expect_code, Str *msg, Error *err, Str format,
                   Slice args) {
    *msg = BURROW_S("");
    Uint id = textproto_conn_cmd(c->text, format, args, err);
    if (BURROW_FAILED(*err))
        return 0;
    textproto_conn_start_response(c->text, id);
    Int code = textproto_conn_read_response(c->text, arena_allocator(&c->scratch),
                                            expect_code, msg, err);
    textproto_conn_end_response(c->text, id);
    return code;
}

#define smc_cmd_v(c, expect_code, msg, err, ...)                                       \
    smc_cmd((c), (expect_code), (msg), (err),                                          \
            BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))

/* A command whose answer is only its code. */
static Error smc_simple(SmtpClient *c, Int expect_code, Str format, Slice args) {
    ArenaMark m = arena_mark(&c->scratch);
    Str msg;
    Error err;
    (void)smc_cmd(c, expect_code, &msg, &err, format, args);
    err = smc_keep(err);
    arena_release(&c->scratch, m);
    return err;
}

#define smc_simple_v(c, expect_code, ...)                                              \
    smc_simple((c), (expect_code), BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))

/* helo sends the HELO greeting to the server. It should be used only when the
 * server does not support ehlo. */
Error burrow__smtp_client_helo(SmtpClient *c) {
    c->ext = NULL;
    return smc_simple_v(c, 250, "HELO %s", c->local_name);
}

/* ehlo sends the EHLO (extended hello) greeting to the server. It should be
 * the preferred greeting for servers that support it. */
Error burrow__smtp_client_ehlo(SmtpClient *c) {
    ArenaMark m = arena_mark(&c->scratch);
    Alloc *s = arena_allocator(&c->scratch);
    Alloc *pa = arena_allocator(&c->arena);
    Str msg;
    Error err;
    (void)smc_cmd_v(c, 250, &msg, &err, "EHLO %s", c->local_name);
    if (BURROW_FAILED(err)) {
        err = smc_keep(err);
        goto out;
    }
    Map *ext = map_make(pa, TYPE_STRING, TYPE_STRING, 0);
    if (ext == NULL) {
        err = burrow_err_out_of_memory;
        goto out;
    }
    Slice ext_list = strings_split(s, msg, BURROW_S("\n"));
    for (Int i = 1; i < ext_list.len; i++) {
        Str v;
        Str k = strings_cut(*(Str *)slice_at(ext_list, i), BURROW_S(" "), &v, NULL);
        k = str_clone(pa, k);
        v = str_clone(pa, v);
        if (!map_set(ext, &k, &v)) {
            err = burrow_err_out_of_memory;
            goto out;
        }
    }
    Str auth_key = BURROW_S("AUTH");
    const Str *mechs = (const Str *)map_get(ext, &auth_key);
    if (mechs != NULL)
        c->auth = strings_split(pa, *mechs, BURROW_S(" "));
    c->ext = ext;
out:
    arena_release(&c->scratch, m);
    return err;
}

/* hello runs a hello exchange if needed. */
static Error smc_hello(SmtpClient *c) {
    if (!c->did_hello) {
        c->did_hello = true;
        Error err = burrow__smtp_client_ehlo(c);
        if (BURROW_FAILED(err)) {
            err = burrow__smtp_client_helo(c);
            if (BURROW_FAILED(err))
                err = error_retain(arena_allocator(&c->arena), err);
            c->hello_error = err;
        }
    }
    return smc_keep(c->hello_error);
}

/* validateLine checks to see if a line has CR or LF as per RFC 5321. */
static Error smc_validate_line(Str line) {
    if (strings_contains_any(line, BURROW_S("\n\r")))
        return smc_err_line;
    return BURROW_NO_ERROR;
}

Error smtp_client_hello(SmtpClient *c, Str local_name) {
    Error err = smc_validate_line(local_name);
    if (BURROW_FAILED(err))
        return err;
    if (c->did_hello)
        return smc_err_hello_after;
    c->local_name = str_clone(arena_allocator(&c->arena), local_name);
    if (c->local_name.len != local_name.len)
        return burrow_err_out_of_memory;
    return smc_hello(c);
}

Error smtp_client_verify(SmtpClient *c, Str addr) {
    Error err = smc_validate_line(addr);
    if (BURROW_FAILED(err))
        return err;
    err = smc_hello(c);
    if (BURROW_FAILED(err))
        return err;
    return smc_simple_v(c, 250, "VRFY %s", addr);
}

Error smtp_client_auth(SmtpClient *c, SmtpAuth a) {
    Error err = smc_hello(c);
    if (BURROW_FAILED(err))
        return err;
    ArenaMark m = arena_mark(&c->scratch);
    Alloc *s = arena_allocator(&c->scratch);
    const Base64Encoding *encoding = base64_std_encoding;
    SmtpServerInfo info = {c->server_name, c->tls, c->auth};
    Slice resp = slice_nil(TYPE_BYTE);
    Str mech = a.vt->start(a.data, &info, s, &resp, &err);
    if (BURROW_FAILED(err)) {
        err = smc_keep(err);
        (void)smtp_client_quit(c);
        goto out;
    }
    Str resp64 = base64_encoding_encode_to_string(encoding, s, resp);
    Str line = strings_trim_space(fmt_sprintf_v(s, "AUTH %s %s", mech, resp64));
    Str msg64;
    Int code = smc_cmd_v(c, 0, &msg64, &err, "%s", line);
    for (;;) {
        if (BURROW_FAILED(err)) {
            err = smc_keep(err);
            break;
        }
        Slice msg = slice_nil(TYPE_BYTE);
        switch (code) {
        case 334:
            msg = base64_encoding_decode_string(encoding, s, msg64, &err);
            break;
        case 235:
            /* the last message isn't base64 because it isn't a challenge */
            msg =
                slice_from((Byte *)(uintptr_t)msg64.p, msg64.len, msg64.len, TYPE_BYTE);
            break;
        default: {
            TextprotoError te = {code, msg64};
            err = textproto_error_as_error(&te, s);
        }
        }
        if (BURROW_OK(err))
            resp = a.vt->next(a.data, msg, code == 334, s, &err);
        if (BURROW_FAILED(err)) {
            /* abort the AUTH */
            err = smc_keep(err);
            Str ignored;
            Error ignored_err;
            (void)smc_cmd_v(c, 501, &ignored, &ignored_err, "*");
            (void)smtp_client_quit(c);
            break;
        }
        if (slice_is_nil(resp))
            break;
        resp64 = base64_encoding_encode_to_string(encoding, s, resp);
        code = smc_cmd_v(c, 0, &msg64, &err, "%s", resp64);
    }
out:
    arena_release(&c->scratch, m);
    return err;
}

Error smtp_client_mail(SmtpClient *c, Str from) {
    static const char *const cmd_strs[] = {
        "MAIL FROM:<%s>",
        "MAIL FROM:<%s> BODY=8BITMIME",
        "MAIL FROM:<%s> SMTPUTF8",
        "MAIL FROM:<%s> BODY=8BITMIME SMTPUTF8",
    };
    Error err = smc_validate_line(from);
    if (BURROW_FAILED(err))
        return err;
    err = smc_hello(c);
    if (BURROW_FAILED(err))
        return err;
    size_t cmd_str = 0;
    if (c->ext != NULL) {
        Str bit8 = BURROW_S("8BITMIME");
        Str utf8 = BURROW_S("SMTPUTF8");
        if (map_get(c->ext, &bit8) != NULL)
            cmd_str |= 1;
        if (map_get(c->ext, &utf8) != NULL)
            cmd_str |= 2;
    }
    return smc_simple_v(c, 250, cmd_strs[cmd_str], from);
}

Error smtp_client_rcpt(SmtpClient *c, Str to) {
    Error err = smc_validate_line(to);
    if (BURROW_FAILED(err))
        return err;
    return smc_simple_v(c, 25, "RCPT TO:<%s>", to);
}

/* dataCloser, with the client as self and the dot writer in c->dot. */
static Int smc_data_write(void *self, Slice p, Error *err) {
    IoWriteCloser d = ((SmtpClient *)self)->dot;
    return d.vt->writer.write(d.data, p, err);
}

static Error smc_data_close(void *self) {
    SmtpClient *c = (SmtpClient *)self;
    (void)c->dot.vt->closer.close(c->dot.data);
    ArenaMark m = arena_mark(&c->scratch);
    Str msg;
    Error err;
    (void)textproto_conn_read_response(c->text, arena_allocator(&c->scratch), 250, &msg,
                                       &err);
    err = smc_keep(err);
    arena_release(&c->scratch, m);
    return err;
}

static const IoWriteCloserVT smc_data_vt = {
    {NULL, smc_data_write},
    {NULL, smc_data_close},
};

IoWriteCloser smtp_client_data(SmtpClient *c, Error *err) {
    Error e = smc_simple_v(c, 354, "DATA");
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e))
        return (IoWriteCloser){NULL, NULL};
    c->dot = textproto_conn_dot_writer(c->text);
    return (IoWriteCloser){&smc_data_vt, c};
}

bool smtp_client_extension(SmtpClient *c, Str ext, Str *param) {
    BURROW_OUT(param, BURROW_S(""));
    if (BURROW_FAILED(smc_hello(c)))
        return false;
    if (c->ext == NULL)
        return false;
    ArenaMark m = arena_mark(&c->scratch);
    ext = strings_to_upper(arena_allocator(&c->scratch), ext);
    const Str *v = (const Str *)map_get(c->ext, &ext);
    arena_release(&c->scratch, m);
    if (v == NULL)
        return false;
    BURROW_OUT(param, *v);
    return true;
}

Error smtp_client_reset(SmtpClient *c) {
    Error err = smc_hello(c);
    if (BURROW_FAILED(err))
        return err;
    return smc_simple_v(c, 250, "RSET");
}

Error smtp_client_noop(SmtpClient *c) {
    Error err = smc_hello(c);
    if (BURROW_FAILED(err))
        return err;
    return smc_simple_v(c, 250, "NOOP");
}

Error smtp_client_quit(SmtpClient *c) {
    (void)smc_hello(c); /* ignore error; we're quitting anyhow */
    Error err = smc_simple_v(c, 221, "QUIT");
    if (BURROW_FAILED(err))
        return err;
    return textproto_conn_close(c->text);
}

/* The body of smtp_send_mail once the client is there, so that it has one
 * place to close and free it. */
static Error smc_send(SmtpClient *c, SmtpAuth a, Str from, Slice to, Slice msg) {
    Error err = smc_hello(c);
    if (BURROW_FAILED(err))
        return err;
    if (smtp_client_extension(c, BURROW_S("STARTTLS"), NULL))
        return smtp_err_no_tls;
    if (a.vt != NULL && c->ext != NULL) {
        Str auth_key = BURROW_S("AUTH");
        if (map_get(c->ext, &auth_key) == NULL)
            return smc_err_no_auth;
        err = smtp_client_auth(c, a);
        if (BURROW_FAILED(err))
            return err;
    }
    err = smtp_client_mail(c, from);
    if (BURROW_FAILED(err))
        return err;
    for (Int i = 0; i < to.len; i++) {
        err = smtp_client_rcpt(c, *(const Str *)slice_at(to, i));
        if (BURROW_FAILED(err))
            return err;
    }
    IoWriteCloser w = smtp_client_data(c, &err);
    if (w.vt == NULL)
        return err;
    (void)w.vt->writer.write(w.data, msg, &err);
    if (BURROW_FAILED(err))
        return err;
    err = w.vt->closer.close(w.data);
    if (BURROW_FAILED(err))
        return err;
    return smtp_client_quit(c);
}

Error smtp_send_mail(Str addr, SmtpAuth a, Str from, Slice to, Slice msg) {
    Error err = smc_validate_line(from);
    if (BURROW_FAILED(err))
        return err;
    for (Int i = 0; i < to.len; i++) {
        err = smc_validate_line(*(const Str *)slice_at(to, i));
        if (BURROW_FAILED(err))
            return err;
    }
    SmtpClient *c = smtp_dial(heap_allocator(), addr, &err);
    if (c == NULL)
        return err;
    err = smc_send(c, a, from, to, msg);
    (void)smtp_client_close(c);
    smtp_client_free(c);
    return err;
}
