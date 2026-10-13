/* expvar, public variables such as a server's operation counters, served as
 * JSON at /debug/vars.
 *
 * A variable is published once under a name and lives as long as the program.
 * Changing one is atomic, so the code that counts and the handler that reads
 * never need a lock between them:
 *
 *     static ExpvarInt *requests;
 *
 *     static void hello(void *env, HttpResponseWriter w, HttpRequest *r) {
 *         expvar_int_add(requests, 1);
 *         ...
 *     }
 *
 *     expvar_register();
 *     requests = expvar_new_int(BURROW_S("requests"));
 *     http_handle_func(BURROW_S("/"), BURROW_FN(HttpHandlerFunc, hello, NULL));
 *     http_listen_and_serve(BURROW_S(":8080"), (HttpHandler){0});
 *
 * Go registers the handler and two variables when the package is imported:
 * cmdline, which is os.Args, and memstats, which is runtime.MemStats. C has
 * nothing that runs on import. The two variables are published the first time
 * anything here touches the list of published ones, so they are always in it,
 * and the handler goes on http_default_serve_mux when expvar_register is
 * called. A program that serves its own mux can mount expvar_handler instead.
 *
 * Every Var has a string method that returns valid JSON. A Var that is not one
 * of the types here can be anything with an ExpvarVarVT, as long as its string
 * keeps to that.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package expvar */

#ifndef BURROW_EXPVAR_H
#define BURROW_EXPVAR_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/net/http.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* expvar.Var. string returns the variable as a valid JSON value, from a.
 * append_json is optional and is Go's unexported jsonVar: it appends the same
 * JSON to b, from a, without making a string first. The zero ExpvarVar, with
 * a NULL vt, is Go's nil Var. */
typedef struct ExpvarVarVT {
    const Type *self_type;
    Str (*string)(void *self, Alloc *a);
    Slice (*append_json)(void *self, Alloc *a, Slice b);
} ExpvarVarVT;

typedef struct ExpvarVar {
    const ExpvarVarVT *vt;
    void *data;
} ExpvarVar;

/* Var.String. "null" for the nil Var. */
BURROW_OWNS(ret) Str expvar_var_string(ExpvarVar v, Alloc *a);

/* expvar.Int, a 64-bit integer. The zero value is 0 and ready to use. */
typedef struct ExpvarInt {
    SyncAtomicInt64 i;
} ExpvarInt;

extern const Type *const TYPE_EXPVAR_INT;

int64_t expvar_int_value(ExpvarInt *v);
BURROW_OWNS(ret) Str expvar_int_string(ExpvarInt *v, Alloc *a);
void expvar_int_add(ExpvarInt *v, int64_t delta);
void expvar_int_set(ExpvarInt *v, int64_t value);
BURROW_BORROWS(ret, v) ExpvarVar expvar_int_as_var(ExpvarInt *v);

/* expvar.Float, a 64-bit float. The zero value is 0 and ready to use. */
typedef struct ExpvarFloat {
    SyncAtomicUint64 f;
} ExpvarFloat;

extern const Type *const TYPE_EXPVAR_FLOAT;

double expvar_float_value(ExpvarFloat *v);
BURROW_OWNS(ret) Str expvar_float_string(ExpvarFloat *v, Alloc *a);
void expvar_float_add(ExpvarFloat *v, double delta);
void expvar_float_set(ExpvarFloat *v, double value);
BURROW_BORROWS(ret, v) ExpvarVar expvar_float_as_var(ExpvarFloat *v);

/* expvar.String, a string, which comes out quoted as JSON. The zero value is
 * "" and ready to use. Set keeps its own copy, from heap_allocator. */
typedef struct ExpvarString {
    SyncRWMutex mu;
    Str s;
} ExpvarString;

extern const Type *const TYPE_EXPVAR_STRING;

/* String.Value, the string unquoted, copied into a. */
BURROW_OWNS(ret) Str expvar_string_value(ExpvarString *v, Alloc *a);
BURROW_OWNS(ret) Str expvar_string_string(ExpvarString *v, Alloc *a);

/* String.Set. Returns false, leaving v as it was, when the copy could not be
 * made. */
bool expvar_string_set(ExpvarString *v, Str value);
BURROW_BORROWS(ret, v) ExpvarVar expvar_string_as_var(ExpvarString *v);

/* Gives back the copy expvar_string_set made. Not while v is in use. */
void expvar_string_free(ExpvarString *v);

/* expvar.Func, a function whose result is marshalled as JSON with
 * encoding/json. The result comes from a, and so does anything it points to. */
BURROW_FUNC(ExpvarFunc, Any, Alloc *a);

extern const Type *const TYPE_EXPVAR_FUNC;

/* Func.Value, f's result. */
Any expvar_func_value(ExpvarFunc f, Alloc *a);

/* Func.String: f's result as JSON, or "" if encoding/json says no, as in Go. */
BURROW_OWNS(ret) Str expvar_func_string(ExpvarFunc f, Alloc *a);

/* f as a Var. The Var points at f, so f has to outlive it. */
BURROW_BORROWS(ret, f) ExpvarVar expvar_func_as_var(ExpvarFunc *f);

/* expvar.KeyValue, one entry in a Map. */
typedef struct ExpvarKeyValue {
    Str key;
    ExpvarVar value;
} ExpvarKeyValue;

/* What expvar_map_do and expvar_do call for each entry. kv.key is only good
 * until the call returns. */
BURROW_FUNC(ExpvarDoFunc, void, ExpvarKeyValue kv);

/* expvar.Map, a string to Var map whose keys come out in sorted order. The
 * zero value is an empty map whose memory comes from heap_allocator, and
 * EXPVAR_MAP picks another allocator.
 *
 * The map keeps its own copy of each key, and expvar_map_add and
 * expvar_map_add_float make the ExpvarInt or ExpvarFloat for a new key. Those
 * come from the map's allocator and are given back by expvar_map_free and not
 * before, since a get running at the same time as a delete may still be
 * looking at them. With an arena there is nothing to give back. */
typedef struct ExpvarMap {
    Alloc *a;
    SyncOnce once;
    SyncMap m;
    SyncRWMutex keys_mu;
    Slice keys;
    Slice owned;
} ExpvarMap;

#define EXPVAR_MAP(alloc) ((ExpvarMap){.a = (alloc)})

extern const Type *const TYPE_EXPVAR_MAP;

/* Map.Init, which removes every key and returns v. */
ExpvarMap *expvar_map_init(ExpvarMap *v);

/* Map.Get, the nil Var when key is not there. */
ExpvarVar expvar_map_get(ExpvarMap *v, Str key);

/* Map.Set. Returns false, with the map unchanged, when it could not
 * allocate. */
bool expvar_map_set(ExpvarMap *v, Str key, ExpvarVar av);

/* Map.Add and Map.AddFloat: add delta to the ExpvarInt or ExpvarFloat under
 * key, making one if the key is new. A key that holds some other kind of Var
 * is left alone. Return false when the new entry could not be allocated. */
bool expvar_map_add(ExpvarMap *v, Str key, int64_t delta);
bool expvar_map_add_float(ExpvarMap *v, Str key, double delta);

/* Map.Delete. */
void expvar_map_delete(ExpvarMap *v, Str key);

/* Map.Do, f for each entry in key order. The key list is locked while it
 * runs, so f must not add a key, but entries can still be updated. */
void expvar_map_do(ExpvarMap *v, ExpvarDoFunc f);

/* Map.String, the map as a JSON object. */
BURROW_OWNS(ret) Str expvar_map_string(ExpvarMap *v, Alloc *a);
BURROW_BORROWS(ret, v) ExpvarVar expvar_map_as_var(ExpvarMap *v);

/* Gives back the keys, the entries and what expvar_map_add made, and leaves an
 * empty map. Not while anything else is using v. */
void expvar_map_free(ExpvarMap *v);

/* expvar.Publish, which adds v to the published variables under name. name
 * is copied. Publishing a name twice panics, with Go's message. */
void expvar_publish(Str name, ExpvarVar v);

/* expvar.Get, the variable published as name, or the nil Var. */
ExpvarVar expvar_get(Str name);

/* expvar.NewInt, NewFloat, NewMap and NewString: a new variable, published as
 * name. They live as long as the program, and are NULL when there was no
 * memory for one. */
ExpvarInt *expvar_new_int(Str name);
ExpvarFloat *expvar_new_float(Str name);
ExpvarMap *expvar_new_map(Str name);
ExpvarString *expvar_new_string(Str name);

/* expvar.Do, f for each published variable in name order. */
void expvar_do(ExpvarDoFunc f);

/* expvar.Handler, the handler that serves the published variables as one JSON
 * object, a variable to a line. */
HttpHandler expvar_handler(void);

/* What importing the package does in Go: puts expvar_handler on
 * http_default_serve_mux as "GET /debug/vars", or as "/debug/vars" when
 * GODEBUG has httpmuxgo121=1. Only the first call does anything. */
void expvar_register(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_EXPVAR_H */
