/* What fcgi.c and fcgi_child.c share, and what the tests reach into, as Go's
 * fcgi_test.go does with the package's unexported names.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_NET_FCGI_INTERNAL_H
#define BURROW_SRC_NET_FCGI_INTERNAL_H

#include "burrow/net/http/fcgi.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/sync.h"

#include <stdint.h>

/* recType, a record type. */
enum {
    BURROW__FCGI_TYPE_BEGIN_REQUEST = 1,
    BURROW__FCGI_TYPE_ABORT_REQUEST = 2,
    BURROW__FCGI_TYPE_END_REQUEST = 3,
    BURROW__FCGI_TYPE_PARAMS = 4,
    BURROW__FCGI_TYPE_STDIN = 5,
    BURROW__FCGI_TYPE_STDOUT = 6,
    BURROW__FCGI_TYPE_STDERR = 7,
    BURROW__FCGI_TYPE_DATA = 8,
    BURROW__FCGI_TYPE_GET_VALUES = 9,
    BURROW__FCGI_TYPE_GET_VALUES_RESULT = 10,
    BURROW__FCGI_TYPE_UNKNOWN_TYPE = 11
};

/* keep the connection between web-server and responder open after request */
#define BURROW__FCGI_FLAG_KEEP_CONN 1

#define BURROW__FCGI_MAX_WRITE 65535 /* maximum record body */
#define BURROW__FCGI_MAX_PAD 255

enum {
    BURROW__FCGI_ROLE_RESPONDER = 1, /* only Responders are implemented. */
    BURROW__FCGI_ROLE_AUTHORIZER,
    BURROW__FCGI_ROLE_FILTER
};

enum {
    BURROW__FCGI_STATUS_REQUEST_COMPLETE,
    BURROW__FCGI_STATUS_CANT_MULTIPLEX,
    BURROW__FCGI_STATUS_OVERLOADED,
    BURROW__FCGI_STATUS_UNKNOWN_ROLE
};

typedef struct burrow__FcgiHeader {
    uint8_t version;
    uint8_t type;
    uint16_t id;
    uint16_t content_length;
    uint8_t padding_length;
    uint8_t reserved;
} burrow__FcgiHeader;

typedef struct burrow__FcgiRecord {
    burrow__FcgiHeader h;
    Byte buf[BURROW__FCGI_MAX_WRITE + BURROW__FCGI_MAX_PAD];
} burrow__FcgiRecord;

/* record.read. */
Error burrow__fcgi_record_read(burrow__FcgiRecord *rec, IoReader r);
/* record.content. */
Slice burrow__fcgi_record_content(burrow__FcgiRecord *rec);

/* conn, which sends records over a connection. Go's rwc is the three
 * interfaces here, since a NetConn and an IoReadWriteCloser are different
 * types. */
typedef struct burrow__FcgiConn {
    SyncMutex mutex;
    IoReader r;
    IoWriter w;
    IoCloser c;
    Error close_err;
    bool closed;

    /* to avoid allocations */
    Byte buf[8 + BURROW__FCGI_MAX_WRITE + BURROW__FCGI_MAX_PAD];
} burrow__FcgiConn;

/* newConn, in a. NULL when a says no. */
burrow__FcgiConn *burrow__fcgi_new_conn(Alloc *a, IoReader r, IoWriter w, IoCloser c);
/* conn.Close. */
Error burrow__fcgi_conn_close(burrow__FcgiConn *c);
/* conn.writeRecord. */
Error burrow__fcgi_conn_write_record(burrow__FcgiConn *c, uint8_t rec_type,
                                     uint16_t req_id, Slice b);
/* conn.writeEndRequest. */
Error burrow__fcgi_conn_write_end_request(burrow__FcgiConn *c, uint16_t req_id,
                                          Int app_status, uint8_t protocol_status);
/* conn.writePairs, for a Map of Str to Str. */
Error burrow__fcgi_conn_write_pairs(burrow__FcgiConn *c, uint8_t rec_type,
                                    uint16_t req_id, Map *pairs);

/* readSize. The size, and in *n how many bytes of s it took, 0 when s is too
 * short. */
uint32_t burrow__fcgi_read_size(Slice s, Int *n);
/* encodeSize, into b, which has room for 4. How many bytes it took. */
Int burrow__fcgi_encode_size(Byte *b, uint32_t size);

/* bufWriter: a bufio.Writer over a streamWriter, which closes the stream when
 * it is closed. Made in a and given back with it. */
typedef struct burrow__FcgiWriter burrow__FcgiWriter;
burrow__FcgiWriter *burrow__fcgi_new_writer(Alloc *a, burrow__FcgiConn *c,
                                            uint8_t rec_type, uint16_t req_id);
Int burrow__fcgi_writer_write(burrow__FcgiWriter *w, Slice p, Error *err);
IoWriter burrow__fcgi_writer_as_io_writer(burrow__FcgiWriter *w);
Error burrow__fcgi_writer_flush(burrow__FcgiWriter *w);
Error burrow__fcgi_writer_close(burrow__FcgiWriter *w);
void burrow__fcgi_writer_free(Alloc *a, burrow__FcgiWriter *w);

/* child. It counts its users: the one who made it, which gives it back with
 * burrow__fcgi_child_release, and each request it is serving. The last one
 * frees it, and frees nc too when nc has a vt. */
typedef struct burrow__FcgiChild burrow__FcgiChild;
burrow__FcgiChild *burrow__fcgi_new_child(IoReader r, IoWriter w, IoCloser c,
                                          HttpHandler handler);
void burrow__fcgi_child_set_net_conn(burrow__FcgiChild *c, NetConn nc);
/* child.serve. */
void burrow__fcgi_child_serve(burrow__FcgiChild *c);
/* child.handleRecord. */
Error burrow__fcgi_child_handle_record(burrow__FcgiChild *c, burrow__FcgiRecord *rec);
void burrow__fcgi_child_release(burrow__FcgiChild *c);

#endif /* BURROW_SRC_NET_FCGI_INTERNAL_H */
