/* crypto/tls: quic.go, a Conn that a QUIC implementation drives in place of a
 * net.Conn, as RFC 9001 describes.
 *
 * Go runs the handshake on a goroutine of its own and hands control back and
 * forth over two channels, so that only one side runs at a time. So does this.
 * The events point at what the handshake made, which Go's collector keeps
 * alive. Here the data of the events is copied into an arena the QUICConn
 * keeps, which is reset once they have all been read.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* QUICConn. */
struct TlsQUICConn {
    TlsConn *conn;

    bool session_ticket_sent;

    /* Not in Go. a is what q came from. state is conn.quic. start_ctx is the
     * context Start hands the handshake goroutine, and wg is done when that
     * goroutine is. */
    Alloc *a;
    TlsQuicState state;
    Context start_ctx;
    SyncWaitGroup wg;
};

BURROW_SENTINEL_ERROR(tqc_err_start_twice, "tls: Start called more than once");
BURROW_SENTINEL_ERROR(tqc_err_wrong_level,
                      "tls: handshake data received at wrong level");
BURROW_SENTINEL_ERROR(tqc_err_ticket_early,
                      "tls: SendSessionTicket called before handshake completed");
BURROW_SENTINEL_ERROR(tqc_err_ticket_client,
                      "tls: SendSessionTicket called on the client");
BURROW_SENTINEL_ERROR(tqc_err_ticket_twice,
                      "tls: SendSessionTicket called multiple times");
BURROW_SENTINEL_ERROR(tqc_err_store_server,
                      "tls: StoreSessionTicket called on the server");
BURROW_SENTINEL_ERROR(
    tqc_err_hand_not_empty,
    "tls: handshake buffer not empty before setting read traffic secret");

/* What []byte{} points at: empty and not nil. */
static const Byte tqc_empty[1] = {0};

static TlsQUICConn *tqc_new(Alloc *a, const TlsQUICConfig *config, bool is_client) {
    TlsQUICConn *q = BURROW_NEW(a, TlsQUICConn);
    if (q == NULL)
        return NULL;
    NetConn none;
    memset(&none, 0, sizeof none);
    q->conn = is_client ? tls_client(a, none, config->tls_config)
                        : tls_server(a, none, config->tls_config);
    TlsQuicState *qs = &q->state;
    qs->signalc = chan_make(a, TYPE_BOOL, 0);
    qs->blockedc = chan_make(a, TYPE_BOOL, 0);
    if (q->conn == NULL || qs->signalc == NULL || qs->blockedc == NULL) {
        chan_free(qs->signalc);
        chan_free(qs->blockedc);
        tls_conn_free(q->conn);
        mem_free(a, q, sizeof *q, _Alignof(TlsQUICConn));
        return NULL;
    }
    q->a = a;
    qs->enable_session_events = config->enable_session_events;
    qs->client_hello_info_conn = config->client_hello_info_conn;
    qs->events = qs->event_arr;
    qs->cap_events = (Int)(sizeof qs->event_arr / sizeof qs->event_arr[0]);
    qs->readbuf = slice_nil(TYPE_BYTE);
    qs->transport_params = slice_nil(TYPE_BYTE);
    arena_init(&qs->event_data, a, 0);
    arena_init(&qs->params, a, 0);
    q->conn->quic = qs;
    return q;
}

TlsQUICConn *tls_quic_client(Alloc *a, const TlsQUICConfig *config) {
    return tqc_new(a, config, TLS_ROLE_CLIENT);
}

TlsQUICConn *tls_quic_server(Alloc *a, const TlsQUICConfig *config) {
    return tqc_new(a, config, TLS_ROLE_SERVER);
}

static void tqc_handshake(void *env) {
    TlsQUICConn *q = (TlsQUICConn *)env;
    (void)tls_conn_handshake_context(q->conn, q->start_ctx);
}

Error tls_quic_conn_start(TlsQUICConn *q, Context ctx) {
    TlsQuicState *qs = &q->state;
    if (qs->started)
        return burrow__tls_quic_error(tqc_err_start_twice);
    qs->started = true;
    q->start_ctx = ctx;
    if (!sync_wait_group_go(&q->wg, BURROW_FN(Func, tqc_handshake, q))) {
        /* Not in Go: with no goroutine to run it, the handshake is over before
         * it starts. */
        TlsConn *c = q->conn;
        sync_mutex_lock(&c->handshake_mutex);
        burrow__tls_conn_set_handshake_err(c, burrow_err_out_of_memory);
        chan_close(qs->blockedc);
        chan_close(qs->signalc);
        sync_mutex_unlock(&c->handshake_mutex);
    }
    if (!chan_recv(qs->blockedc, NULL))
        return q->conn->handshake_err;
    return BURROW_NO_ERROR;
}

/* Not in Go: events = events[:0], with what the events kept given back. */
static void tqc_reset_events(TlsQuicState *qs) {
    qs->events = qs->event_arr;
    qs->n_events = 0;
    qs->cap_events = (Int)(sizeof qs->event_arr / sizeof qs->event_arr[0]);
    qs->next_event = 0;
    arena_reset(&qs->event_data);
}

TlsQUICEvent tls_quic_conn_next_event(TlsQUICConn *q) {
    TlsQuicState *qs = &q->state;
    TlsQUICEvent none;
    memset(&none, 0, sizeof none);
    none.kind = TLS_QUIC_NO_EVENT;
    none.data = slice_nil(TYPE_BYTE);
    Int last = qs->next_event - 1;
    if (last >= 0 && qs->events[last].data.len > 0) {
        /* Write over some of the data, so that callers don't rely on it
         * remaining valid past the next call. */
        ((Byte *)qs->events[last].data.p)[0] = 0;
    }
    if (qs->next_event >= qs->n_events && qs->waiting_for_drain) {
        qs->waiting_for_drain = false;
        (void)chan_recv(qs->signalc, NULL);
        (void)chan_recv(qs->blockedc, NULL);
    }
    if (BURROW_FAILED(q->conn->handshake_err)) {
        if (qs->error_returned)
            return none;
        qs->error_returned = true;
        tqc_reset_events(qs);
        none.kind = TLS_QUIC_ERROR_EVENT;
        none.err = q->conn->handshake_err;
        return none;
    }
    if (qs->next_event >= qs->n_events) {
        tqc_reset_events(qs);
        return none;
    }
    TlsQUICEvent e = qs->events[qs->next_event];
    memset(&qs->events[qs->next_event], 0,
           sizeof qs->events[0]); /* zero out references to data */
    qs->next_event++;
    return e;
}

Error tls_quic_conn_close(TlsQUICConn *q) {
    TlsQuicState *qs = &q->state;
    if (BURROW_CONTEXT_IS_NIL(qs->ctx))
        return BURROW_NO_ERROR; /* never started */
    BURROW_CALLF0(qs->cancel);
    (void)chan_recv(qs->signalc, NULL);
    while (chan_recv(qs->blockedc, NULL)) {
    }
    return q->conn->handshake_err;
}

Error tls_quic_conn_handle_data(TlsQUICConn *q, TlsQUICEncryptionLevel level,
                                Slice data) {
    TlsConn *c = q->conn;
    TlsQuicState *qs = &q->state;
    if (c->in.level != level)
        return burrow__tls_quic_error(
            burrow__tls_half_conn_set_error_locked(&c->in, tqc_err_wrong_level));
    qs->readbuf = data;
    (void)chan_recv(qs->signalc, NULL);
    if (chan_recv(qs->blockedc, NULL)) {
        /* The handshake goroutine is waiting for more data. */
        return BURROW_NO_ERROR;
    }
    /* The handshake goroutine has exited. */
    sync_mutex_lock(&c->handshake_mutex);
    Error err = BURROW_NO_ERROR;
    (void)bytes_buffer_write(&c->hand, qs->readbuf, &err);
    qs->readbuf = slice_nil(TYPE_BYTE);
    if (BURROW_FAILED(err)) {
        sync_mutex_unlock(&c->handshake_mutex);
        return burrow__tls_quic_error(err);
    }
    while (bytes_buffer_len(&c->hand) >= 4 && BURROW_OK(c->handshake_err)) {
        Slice b = bytes_buffer_bytes(&c->hand);
        const Byte *p = (const Byte *)b.p;
        Int n = (Int)p[1] << 16 | (Int)p[2] << 8 | (Int)p[3];
        if (n > TLS_MAX_HANDSHAKE) {
            burrow__tls_conn_set_handshake_err(
                c, fmt_errorf_v("tls: handshake message of length %d bytes exceeds "
                                "maximum of %d bytes",
                                n, (Int)TLS_MAX_HANDSHAKE));
            break;
        }
        if (b.len < 4 + n) {
            sync_mutex_unlock(&c->handshake_mutex);
            return BURROW_NO_ERROR;
        }
        Error e = burrow__tls_conn_handle_post_handshake_message(c);
        if (BURROW_FAILED(e))
            burrow__tls_conn_set_handshake_err(c, e);
    }
    err = c->handshake_err;
    sync_mutex_unlock(&c->handshake_mutex);
    if (BURROW_FAILED(err))
        return burrow__tls_quic_error(err);
    return BURROW_NO_ERROR;
}

Error tls_quic_conn_send_session_ticket(TlsQUICConn *q,
                                        TlsQUICSessionTicketOptions opts) {
    TlsConn *c = q->conn;
    if (c->config->session_tickets_disabled)
        return BURROW_NO_ERROR;
    if (!sync_atomic_bool_load(&c->is_handshake_complete))
        return burrow__tls_quic_error(tqc_err_ticket_early);
    if (c->is_client)
        return burrow__tls_quic_error(tqc_err_ticket_client);
    if (q->session_ticket_sent)
        return burrow__tls_quic_error(tqc_err_ticket_twice);
    q->session_ticket_sent = true;
    return burrow__tls_quic_error(
        burrow__tls_conn_send_session_ticket(c, opts.early_data, opts.extra));
}

/* Not in Go: the ClientSessionState a QUICStoreSession event handed out
 * session from, or NULL. */
static TlsClientSessionState *tqc_stored(const TlsQuicState *qs,
                                         const TlsSessionState *session) {
    for (Int i = 0; i < qs->n_stored; i++)
        if (qs->stored[i]->session == session)
            return qs->stored[i];
    return NULL;
}

Error tls_quic_conn_store_session(TlsQUICConn *q, TlsSessionState *session) {
    TlsConn *c = q->conn;
    TlsQuicState *qs = &q->state;
    TlsClientSessionState *kept = tqc_stored(qs, session);
    if (!c->is_client) {
        if (kept == NULL)
            tls_session_state_free(session);
        return burrow__tls_quic_error(tqc_err_store_server);
    }
    Arena ar;
    arena_init(&ar, q->a, 0);
    Str cache_key = burrow__tls_conn_client_session_cache_key(c, arena_allocator(&ar));
    /* Not in Go: with no cache there is nowhere to put it, where Go would
     * panic. */
    if (cache_key.len == 0 || c->config->client_session_cache.vt == NULL) {
        arena_free(&ar);
        if (kept == NULL)
            tls_session_state_free(session);
        return BURROW_NO_ERROR;
    }
    if (kept != NULL) {
        tls_client_session_cache_put(c->config->client_session_cache, cache_key, kept);
        arena_free(&ar);
        return BURROW_NO_ERROR;
    }
    Error err = BURROW_NO_ERROR;
    TlsClientSessionState *cs =
        tls_new_resumption_state(session->ticket, session, &err);
    if (cs == NULL) {
        arena_free(&ar);
        return burrow__tls_quic_error(err);
    }
    tls_client_session_cache_put(c->config->client_session_cache, cache_key, cs);
    tls_client_session_state_release(cs);
    arena_free(&ar);
    return BURROW_NO_ERROR;
}

TlsConn *burrow__tls_quic_conn_conn(TlsQUICConn *q) {
    return q->conn;
}

TlsConnectionState tls_quic_conn_connection_state(TlsQUICConn *q) {
    return tls_conn_connection_state(q->conn);
}

void tls_quic_conn_set_transport_parameters(TlsQUICConn *q, Slice params) {
    TlsQuicState *qs = &q->state;
    if (params.len == 0) {
        params = slice_from((Byte *)(uintptr_t)tqc_empty, 0, 0, TYPE_BYTE);
    } else {
        Slice kept = slice_append(arena_allocator(&qs->params), slice_nil(TYPE_BYTE),
                                  params.p, params.len);
        if (kept.len != params.len)
            runtime_panic(BURROW_S("tls: out of memory"));
        params = kept;
    }
    qs->transport_params = params;
    if (qs->started) {
        (void)chan_recv(qs->signalc, NULL);
        (void)chan_recv(qs->blockedc, NULL);
    }
}

void tls_quic_conn_free(TlsQUICConn *q) {
    if (q == NULL)
        return;
    TlsQuicState *qs = &q->state;
    (void)tls_quic_conn_close(q);
    sync_wait_group_wait(&q->wg);
    if (!BURROW_CONTEXT_IS_NIL(qs->ctx))
        context_release(qs->ctx);
    for (Int i = 0; i < qs->n_stored; i++)
        tls_client_session_state_release(qs->stored[i]);
    if (qs->stored != NULL)
        mem_free(q->a, qs->stored, (size_t)qs->cap_stored * sizeof qs->stored[0],
                 _Alignof(TlsClientSessionState *));
    chan_free(qs->signalc);
    chan_free(qs->blockedc);
    arena_free(&qs->event_data);
    arena_free(&qs->params);
    tls_conn_free(q->conn);
    mem_free(q->a, q, sizeof *q, _Alignof(TlsQUICConn));
}

Error burrow__tls_quic_error(Error err) {
    if (BURROW_OK(err))
        return BURROW_NO_ERROR;
    if (errors_as(err, TYPE_TLS_ALERT_ERROR) != NULL)
        return err;
    uint8_t a = TLS_ALERT_INTERNAL_ERROR;
    (void)burrow__tls_as_alert(err, &a);
    /* Not in Go: with no memory for the AlertError, err goes as it is. */
    Error ae = tls_alert_error_as_error(a, error_allocator());
    if (errors_is(ae, burrow_err_out_of_memory))
        return err;
    /* Return an error wrapping the original error and an AlertError.
     * Truncate the text of the alert to 0 characters. */
    return fmt_errorf_v("%w%.0w", err, ae);
}

/* ---------------------------------------------------------------- the Conn */

/* Not in Go: append(events, e), with data, if it is not nil, copied into
 * event_data for the event. */
static Error tqc_push(TlsQuicState *qs, TlsQUICEvent e, Slice data) {
    Alloc *ea = arena_allocator(&qs->event_data);
    e.data = slice_nil(TYPE_BYTE);
    if (data.len > 0) {
        e.data = slice_append(ea, e.data, data.p, data.len);
        if (e.data.len != data.len)
            return burrow_err_out_of_memory;
    }
    if (qs->n_events == qs->cap_events) {
        Int cap = qs->cap_events * 2;
        TlsQUICEvent *events = BURROW_NEW_N(ea, TlsQUICEvent, (size_t)cap);
        if (events == NULL)
            return burrow_err_out_of_memory;
        memcpy(events, qs->events, (size_t)qs->n_events * sizeof events[0]);
        qs->events = events;
        qs->cap_events = cap;
    }
    qs->events[qs->n_events++] = e;
    return BURROW_NO_ERROR;
}

static TlsQUICEvent tqc_event(TlsQUICEventKind kind) {
    TlsQUICEvent e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.data = slice_nil(TYPE_BYTE);
    return e;
}

/* quicWaitForSignal notifies the QUICConn that handshake progress is blocked,
 * and waits for a signal that the handshake should proceed.
 *
 * The handshake may become blocked waiting for handshake bytes or for the user
 * to provide transport parameters. */
static Error tqc_wait_for_signal(TlsConn *c) {
    TlsQuicState *qs = c->quic;
    /* Drop the handshake mutex while blocked to allow the user to call
     * ConnectionState before the handshake completes. */
    sync_mutex_unlock(&c->handshake_mutex);
    bool v = false;
    chan_send(qs->blockedc, &v);
    chan_send(qs->signalc, &v);
    Error err = BURROW_NO_ERROR;
    if (BURROW_FAILED(context_err(qs->ctx))) {
        /* The connection has been closed. */
        err = burrow__tls_conn_send_alert_locked(c, TLS_ALERT_CLOSE_NOTIFY);
    } else {
        (void)bytes_buffer_write(&c->hand, qs->readbuf, &err);
        qs->readbuf = slice_nil(TYPE_BYTE);
    }
    sync_mutex_lock(&c->handshake_mutex);
    return err;
}

Error burrow__tls_conn_quic_read_handshake_bytes(TlsConn *c, Int n) {
    while (bytes_buffer_len(&c->hand) < n) {
        Error err = tqc_wait_for_signal(c);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

Error burrow__tls_conn_quic_set_read_secret(TlsConn *c, TlsQUICEncryptionLevel level,
                                            uint16_t suite, Slice secret) {
    /* Ensure that there are no buffered handshake messages before changing
     * the read keys, since that can cause messages to be parsed that were
     * encrypted using old keys which are no longer appropriate. */
    if (bytes_buffer_len(&c->hand) != 0) {
        (void)burrow__tls_conn_send_alert(c, TLS_ALERT_UNEXPECTED_MESSAGE);
        return tqc_err_hand_not_empty;
    }
    TlsQUICEvent e = tqc_event(TLS_QUIC_SET_READ_SECRET);
    e.level = level;
    e.suite = suite;
    return tqc_push(c->quic, e, secret);
}

Error burrow__tls_conn_quic_set_write_secret(TlsConn *c, TlsQUICEncryptionLevel level,
                                             uint16_t suite, Slice secret) {
    TlsQUICEvent e = tqc_event(TLS_QUIC_SET_WRITE_SECRET);
    e.level = level;
    e.suite = suite;
    return tqc_push(c->quic, e, secret);
}

Error burrow__tls_conn_quic_write_crypto_data(TlsConn *c, TlsQUICEncryptionLevel level,
                                              Slice data) {
    TlsQuicState *qs = c->quic;
    TlsQUICEvent *last = NULL;
    if (qs->n_events > 0)
        last = &qs->events[qs->n_events - 1];
    if (last == NULL || last->kind != TLS_QUIC_WRITE_DATA || last->level != level) {
        TlsQUICEvent e = tqc_event(TLS_QUIC_WRITE_DATA);
        e.level = level;
        return tqc_push(qs, e, data);
    }
    if (data.len == 0)
        return BURROW_NO_ERROR;
    Int want = last->data.len + data.len;
    last->data =
        slice_append(arena_allocator(&qs->event_data), last->data, data.p, data.len);
    if (last->data.len != want)
        return burrow_err_out_of_memory;
    return BURROW_NO_ERROR;
}

Error burrow__tls_conn_quic_resume_session(TlsConn *c, TlsSessionState *session) {
    TlsQuicState *qs = c->quic;
    TlsQUICEvent e = tqc_event(TLS_QUIC_RESUME_SESSION);
    e.session_state = session;
    Error err = tqc_push(qs, e, slice_nil(TYPE_BYTE));
    if (BURROW_FAILED(err))
        return err;
    qs->waiting_for_drain = true;
    while (qs->waiting_for_drain) {
        err = tqc_wait_for_signal(c);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

Error burrow__tls_conn_quic_store_session(TlsConn *c, TlsClientSessionState *cs) {
    TlsQuicState *qs = c->quic;
    /* Not in Go: with no memory to keep it, the ticket is dropped, as
     * handleNewSessionTicket does. */
    if (qs->n_stored == qs->cap_stored) {
        Int cap = qs->cap_stored == 0 ? 4 : qs->cap_stored * 2;
        TlsClientSessionState **stored =
            BURROW_NEW_N(c->a, TlsClientSessionState *, (size_t)cap);
        if (stored == NULL) {
            tls_client_session_state_release(cs);
            return BURROW_NO_ERROR;
        }
        if (qs->stored != NULL) {
            memcpy(stored, qs->stored, (size_t)qs->n_stored * sizeof stored[0]);
            mem_free(c->a, qs->stored, (size_t)qs->cap_stored * sizeof stored[0],
                     _Alignof(TlsClientSessionState *));
        }
        qs->stored = stored;
        qs->cap_stored = cap;
    }
    qs->stored[qs->n_stored++] = cs;
    TlsQUICEvent e = tqc_event(TLS_QUIC_STORE_SESSION);
    e.session_state = cs->session;
    (void)tqc_push(qs, e, slice_nil(TYPE_BYTE));
    return BURROW_NO_ERROR;
}

Error burrow__tls_conn_quic_set_transport_parameters(TlsConn *c, Slice params) {
    return tqc_push(c->quic, tqc_event(TLS_QUIC_TRANSPORT_PARAMETERS), params);
}

Slice burrow__tls_conn_quic_get_transport_parameters(TlsConn *c, Error *err) {
    TlsQuicState *qs = c->quic;
    if (qs->transport_params.p == NULL) {
        Error e = tqc_push(qs, tqc_event(TLS_QUIC_TRANSPORT_PARAMETERS_REQUIRED),
                           slice_nil(TYPE_BYTE));
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return slice_nil(TYPE_BYTE);
        }
    }
    while (qs->transport_params.p == NULL) {
        Error e = tqc_wait_for_signal(c);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return slice_nil(TYPE_BYTE);
        }
    }
    return qs->transport_params;
}

Error burrow__tls_conn_quic_handshake_complete(TlsConn *c) {
    return tqc_push(c->quic, tqc_event(TLS_QUIC_HANDSHAKE_DONE), slice_nil(TYPE_BYTE));
}

Error burrow__tls_conn_quic_rejected_early_data(TlsConn *c) {
    return tqc_push(c->quic, tqc_event(TLS_QUIC_REJECTED_EARLY_DATA),
                    slice_nil(TYPE_BYTE));
}
