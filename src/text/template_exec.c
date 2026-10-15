/* text/template's executor: Execute walks a parsed template and writes what it
 * makes.
 *
 * The walk is Go's, function for function. What differs is how it stops. Go
 * panics with walkBreak and walkContinue to leave a range body, and walk here
 * returns a code for them instead. An error still panics, with a private type
 * that Execute's recover turns into the error it returns, as Go's errRecover
 * does.
 *
 * Every value the walk makes, the constants, the loop indexes and what the
 * functions return, goes in an arena Execute makes and frees before it
 * returns.
 *
 * Derived from Go's src/text/template/exec.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/text/template.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/platform.h"
#include "burrow/reflect.h"
#include "burrow/slice.h"
#include "burrow/stack.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/text/template/parse.h"
#include "burrow/thread.h"
#include "burrow/type.h"
#include "template_internal.h"

#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

/* maxExecDepth: the deepest templates may invoke templates. Only a template
 * that invokes itself by accident gets there, and the limit turns that into an
 * error rather than a stack overflow. The C stack can run out first, and the
 * walk checks for that too, with the same error. */
#if defined(BURROW_OS_WASI)
enum { TTE_MAX_EXEC_DEPTH = 1000 };
#else
enum { TTE_MAX_EXEC_DEPTH = 100000 };
#endif

/* What walk returns: carry on, or leave the range body for a break or a
 * continue. */
enum { TTE_WALK_NEXT, TTE_WALK_BREAK, TTE_WALK_CONTINUE };

/* variable: the value of a variable such as $ or $x. */
typedef struct TteVariable {
    Str name;
    ReflectValue value;
} TteVariable;

/* state: one template's part of an execution. Several executions of a
 * template can run at once, so none of this is in the template. */
typedef struct TteState {
    burrow__TtExec *x;
    TextTemplate *tmpl;
    IoWriter wr;
    ParseNode node; /* current node, for errors */
    TteVariable *vars;
    Int nvars; /* push-down stack of variable values */
    Int cap_vars;
    Int depth; /* the height of the stack of executing templates */
} TteState;

/* A node the walk only reads, as the ParseNode the parse API takes. */
#define TTE_NODE(p) ((ParseNode)(uintptr_t)(p))

static const ReflectValue tte_zero = {NULL, NULL, 0};

/* ---------------------------------------------------------------- panics */

/* The descriptor of a type of this file's own, which only panics and the
 * missing value use. */
#define TTE_PRIVATE_DESC(name, T)                                                      \
    {BURROW_S_INIT(name),                                                              \
     BURROW_S_INIT("text/template"),                                                   \
     KIND_STRUCT,                                                                      \
     (uint32_t)sizeof(T),                                                              \
     (uint16_t)_Alignof(T),                                                            \
     0,                                                                                \
     0,                                                                                \
     NULL,                                                                             \
     NULL,                                                                             \
     NULL,                                                                             \
     NULL,                                                                             \
     0,                                                                                \
     0,                                                                                \
     NULL}

/* writeError, the wrapper an error from the writer travels in. It is not an
 * error, so it cannot get out as one. */
typedef struct TteWriteError {
    Error err;
} TteWriteError;

static const Type tte_write_error_desc = TTE_PRIVATE_DESC("writeError", TteWriteError);

/* The ExecError errorf panics with, which the recover makes an Error of. */
static const Type tte_exec_error_desc =
    TTE_PRIVATE_DESC("execError", TextTemplateExecError);

/* missingValType: the type of missingVal, which stands for the final value of
 * a pipeline that has none. */
typedef struct TteMissing {
    char unused;
} TteMissing;

static const Type tte_missing_desc = TTE_PRIVATE_DESC("missingValType", TteMissing);

static const TteMissing tte_missing_val = {0};

static ReflectValue tte_missing(void) {
    return reflect_value_of(BURROW_ANY(&tte_missing_desc, (uintptr_t)&tte_missing_val));
}

static bool tte_is_missing(ReflectValue v) {
    return reflect_value_is_valid(v) && reflect_value_type(v) == &tte_missing_desc;
}

/* ------------------------------------------------------------- the state */

static Alloc *tte_a(TteState *s) {
    return s->x->a;
}

/* The missingkey option, which a template that was never set up lacks. */
static int tte_missing_key(const TteState *s) {
    return s->tmpl->common == NULL ? BURROW__TT_MAP_INVALID
                                   : s->tmpl->common->missing_key;
}

/* at: marks the state to be on node, for error reporting. */
static void tte_at(TteState *s, const void *node) {
    s->node = TTE_NODE(node);
}

static Str tte_node_string(TteState *s, const void *node) {
    return parse_node_string(TTE_NODE(node), tte_a(s));
}

/* The start of an error's message, which says where the walk is. */
static Str tte_error_prefix(TteState *s) {
    Alloc *a = tte_a(s);
    if (s->node == NULL)
        return fmt_sprintf_v(a, "template: %s: ", s->tmpl->name);
    Str context = BURROW_STR_EMPTY;
    Str location = text_template_error_context(*s->tmpl, a, s->node, &context);
    return fmt_sprintf_v(a, "template: %s: executing %q at <%s>: ", location,
                         s->tmpl->name, context);
}

BURROW_NORETURN static void tte_raise(TteState *s, Error err) {
    TextTemplateExecError e = {s->tmpl->name, err};
    panic((Any){&tte_exec_error_desc, &e});
}

/* errorf, with the message made already. */
BURROW_NORETURN static void tte_fail(TteState *s, Str msg) {
    tte_raise(s, fmt_errorf_v("%s%s", tte_error_prefix(s), msg));
}

/* errorf for a message that ends with %w and err. */
BURROW_NORETURN static void tte_fail_wrap(TteState *s, Str msg, Error err) {
    tte_raise(s, fmt_errorf_v("%s%s%w", tte_error_prefix(s), msg, err));
}

#define tte_errorf(s, ...) tte_fail((s), fmt_sprintf_v(tte_a(s), __VA_ARGS__))

BURROW_NORETURN static void tte_write_error(Error err) {
    TteWriteError e = {err};
    panic((Any){&tte_write_error_desc, &e});
}

/* errRecover: an ExecError becomes the error Execute returns, and a write
 * error the writer's own error. Running out of memory comes back as
 * burrow_err_out_of_memory. Any other panic goes on up. */
static Error tte_recover(Any p) {
    if (p.t == &tte_exec_error_desc)
        return text_template_exec_error_as_error(error_allocator(),
                                                 (const TextTemplateExecError *)p.data);
    if (p.t == &tte_write_error_desc)
        return ((const TteWriteError *)p.data)->err; /* Strip the wrapper. */
    if (p.t == TYPE_ERROR &&
        errors_is(*(const Error *)p.data, burrow_err_out_of_memory))
        return burrow_err_out_of_memory;
    panic(p);
}

/* ------------------------------------------------------------- variables */

/* push: a new variable on the stack. */
static void tte_push(TteState *s, Str name, ReflectValue value) {
    if (s->nvars == s->cap_vars) {
        Int cap = s->cap_vars < 8 ? 8 : s->cap_vars * 2;
        TteVariable *v =
            mem_alloc_nozero(tte_a(s), (size_t)cap * sizeof *v, _Alignof(TteVariable));
        if (v == NULL)
            burrow__tt_oom();
        if (s->nvars > 0)
            memcpy(v, s->vars, (size_t)s->nvars * sizeof *v);
        s->vars = v;
        s->cap_vars = cap;
    }
    s->vars[s->nvars++] = (TteVariable){name, value};
}

/* mark: the length of the variable stack. */
static Int tte_mark(const TteState *s) {
    return s->nvars;
}

/* pop: pops the variable stack back to mark. */
static void tte_pop(TteState *s, Int mark) {
    s->nvars = mark;
}

/* setVar: overwrites the last declared variable called name. */
static void tte_set_var(TteState *s, Str name, ReflectValue value) {
    for (Int i = tte_mark(s) - 1; i >= 0; i--) {
        if (str_eq(s->vars[i].name, name)) {
            s->vars[i].value = value;
            return;
        }
    }
    tte_errorf(s, "undefined variable: %s", name);
}

/* setTopVar: overwrites the variable n from the top of the stack. */
static void tte_set_top_var(TteState *s, Int n, ReflectValue value) {
    s->vars[s->nvars - n].value = value;
}

/* varValue: the value of the variable called name. */
static ReflectValue tte_var_value(TteState *s, Str name) {
    for (Int i = tte_mark(s) - 1; i >= 0; i--) {
        if (str_eq(s->vars[i].name, name))
            return s->vars[i].value;
    }
    tte_errorf(s, "undefined variable: %s", name);
}

/* ------------------------------------------------------------ stack room */

/* How deep the templates go before the walk asks the thread for its stack
 * bounds, which is a system call on some systems. */
enum { TTE_THREAD_CHECK_AFTER = 32 };

#define TTE_STACK_MARGIN ((uintptr_t)64 << 10)

#if !defined(BURROW_OS_WASI)
static uintptr_t tte_stack_here(void) {
#if defined(__GNUC__) || defined(__clang__)
    return (uintptr_t)__builtin_frame_address(0);
#elif defined(_MSC_VER)
    return (uintptr_t)_AddressOfReturnAddress();
#else
    volatile char probe = 0;
    uintptr_t here = (uintptr_t)&probe;
    return here;
#endif
}
#endif

/* Whether the stack is too low to go deeper. stack_floor is 0 before the first
 * look, 1 when the bounds cannot be had, and 2 on a thread whose bounds are
 * left until the nesting is deep. */
static bool tte_stack_low(burrow__TtExec *x, Int depth) {
#if defined(BURROW_OS_WASI)
    (void)x;
    (void)depth;
    return false;
#else
    if (x->stack_floor == 0) {
        burrow__Stack *st = burrow__stack_current();
        if (st != NULL && st->lo != NULL)
            x->stack_floor = (uintptr_t)st->lo + TTE_STACK_MARGIN;
        else
            x->stack_floor = 2;
    }
    if (x->stack_floor == 2) {
        if (depth < TTE_THREAD_CHECK_AFTER)
            return false;
        void *lo = NULL;
        if (!burrow__thread_stack_limit(&lo))
            lo = NULL;
        x->stack_floor = lo == NULL ? 1 : (uintptr_t)lo + TTE_STACK_MARGIN;
    }
    if (x->stack_floor == 1)
        return false;
    return tte_stack_here() < x->stack_floor;
#endif
}

/* ------------------------------------------------------------------ values */

static ReflectValue tte_value_of(TteState *s, ReflectType t, const void *p) {
    return burrow__tt_value_of(tte_a(s), t, p);
}

/* A new settable value of type t, set to its zero. */
static ReflectValue tte_new(TteState *s, ReflectType t) {
    ReflectValue p = reflect_new(tte_a(s), t);
    if (!reflect_value_is_valid(p))
        burrow__tt_oom();
    return reflect_value_elem(p);
}

/* reflect.ValueOf(v) for a v that is itself a reflect.Value. */
static ReflectValue tte_wrap(TteState *s, ReflectValue v) {
    return tte_value_of(s, TYPE_REFLECT_VALUE, &v);
}

static Str tte_type_string(ReflectType t) {
    return t == NULL ? BURROW_S("<nil>") : reflect_type_string(t);
}

/* ------------------------------------------------------------- the walk */

static Int tte_walk(TteState *s, ReflectValue dot, ParseNode node);
static ReflectValue tte_eval_pipeline(TteState *s, ReflectValue dot,
                                      const ParsePipeNode *pipe);
static ReflectValue tte_eval_arg(TteState *s, ReflectValue dot, ReflectType typ,
                                 ParseNode n);
static void tte_print_value(TteState *s, const void *n, ReflectValue v);

static void tte_write_text(TteState *s, const ParseTextNode *n) {
    if (n->text.len == 0)
        return;
    Error err = BURROW_NO_ERROR;
    s->wr.vt->write(s->wr.data,
                    slice_from(n->text.p, n->text.len, n->text.len, TYPE_UINT8), &err);
    if (BURROW_FAILED(err))
        tte_write_error(err);
}

/* walkIfOrWith: an if or a with, which differ only in that with sets dot. */
static Int tte_walk_if_or_with(TteState *s, ParseNodeType typ, ReflectValue dot,
                               const ParseBranchNode *b) {
    Int mark = tte_mark(s);
    ReflectValue val = tte_eval_pipeline(s, dot, b->pipe);
    bool ok = false;
    bool truth = burrow__tt_is_true(burrow__tt_indirect_interface(val), &ok);
    if (!ok)
        tte_errorf(s, "if/with can't use %v", burrow__tt_fmt_arg(val));
    Int code = TTE_WALK_NEXT;
    if (truth) {
        if (typ == PARSE_NODE_WITH)
            code = tte_walk(s, val, TTE_NODE(&b->list->node_type));
        else
            code = tte_walk(s, dot, TTE_NODE(&b->list->node_type));
    } else if (b->else_list != NULL) {
        code = tte_walk(s, dot, TTE_NODE(&b->else_list->node_type));
    }
    tte_pop(s, mark);
    return code;
}

/* oneIteration: sets the range's variables to index and elem and runs the
 * body. Returns whether a break ended the range. */
static bool tte_one_iteration(TteState *s, const ParseBranchNode *r, Int mark,
                              ReflectValue index, ReflectValue elem) {
    const ParsePipeNode *pipe = r->pipe;
    if (pipe->decl.len > 0) {
        const ParseVariableNode *v0 = BURROW_AT(ParseVariableNode *, pipe->decl, 0);
        if (pipe->is_assign) {
            /* With two variables, index comes first. With one, we use the
             * element. */
            if (pipe->decl.len > 1)
                tte_set_var(s, BURROW_AT(Str, v0->ident, 0), index);
            else
                tte_set_var(s, BURROW_AT(Str, v0->ident, 0), elem);
        } else {
            /* Set top var (lexically the second if there are two) to the
             * element. */
            tte_set_top_var(s, 1, elem);
        }
    }
    if (pipe->decl.len > 1) {
        const ParseVariableNode *v1 = BURROW_AT(ParseVariableNode *, pipe->decl, 1);
        if (pipe->is_assign) {
            tte_set_var(s, BURROW_AT(Str, v1->ident, 0), elem);
        } else {
            /* Set next var (lexically the first if there are two) to the
             * index. */
            tte_set_top_var(s, 2, index);
        }
    }
    Int code = tte_walk(s, elem, TTE_NODE(&r->list->node_type));
    tte_pop(s, mark);
    return code == TTE_WALK_BREAK;
}

static bool tte_int_kind(ReflectKind k) {
    return k == REFLECT_INT || k == REFLECT_INT8 || k == REFLECT_INT16 ||
           k == REFLECT_INT32 || k == REFLECT_INT64;
}

/* Range over an integer, which yields 0 up to it in its own type. Returns
 * whether there was an iteration. */
static bool tte_range_int(TteState *s, const ParseBranchNode *r, Int mark,
                          ReflectValue val) {
    ReflectType t = reflect_value_type(val);
    bool is_int = tte_int_kind(reflect_value_kind(val));
    uint64_t n = 0;
    if (is_int) {
        int64_t i = reflect_value_int(val);
        n = i < 0 ? 0 : (uint64_t)i;
    } else {
        n = reflect_value_uint(val);
    }
    for (uint64_t i = 0; i < n; i++) {
        ReflectValue v = tte_new(s, t);
        if (is_int)
            reflect_value_set_int(v, (int64_t)i);
        else
            reflect_value_set_uint(v, i);
        /* Pass element as second value, as we do for channels. */
        if (tte_one_iteration(s, r, mark, tte_zero, v))
            break;
    }
    return n > 0;
}

/* Range over a map, in the key order fmt prints a map in. */
static void tte_range_map(TteState *s, const ParseBranchNode *r, Int mark,
                          ReflectValue val) {
    Int n = reflect_value_len(val);
    burrow__FmtSortEntry *entries = mem_alloc_nozero(
        tte_a(s), (size_t)n * 2 * sizeof *entries, _Alignof(burrow__FmtSortEntry));
    if (entries == NULL)
        burrow__tt_oom();
    ReflectMapIter it = {0};
    reflect_map_iter_reset(&it, val);
    ReflectValue k = tte_zero;
    ReflectValue e = tte_zero;
    Int i = 0;
    while (i < n && reflect_map_iter_next(&it)) {
        k = reflect_map_iter_key(&it);
        e = reflect_map_iter_value(&it);
        entries[i++] = (burrow__FmtSortEntry){k.ptr, e.ptr};
    }
    n = i;
    burrow__fmtsort(k.typ, entries, entries + n, n);
    for (i = 0; i < n; i++) {
        ReflectValue key = {k.typ, (void *)(uintptr_t)entries[i].key, k.flag};
        ReflectValue value = {e.typ, entries[i].value, e.flag};
        if (tte_one_iteration(s, r, mark, key, value))
            break;
    }
}

/* Range over a channel, until it is closed. Returns whether there was an
 * iteration. */
static bool tte_range_chan(TteState *s, const ParseBranchNode *r, Int mark,
                           ReflectValue val) {
    Int i = 0;
    for (;; i++) {
        bool ok = false;
        ReflectValue elem = reflect_value_recv(val, tte_a(s), &ok);
        if (!ok)
            break;
        if (tte_one_iteration(s, r, mark, tte_value_of(s, TYPE_INT, &i), elem)) {
            i++;
            break;
        }
    }
    return i > 0;
}

static void tte_walk_range(TteState *s, ReflectValue dot, const ParseBranchNode *r) {
    tte_at(s, r);
    Int outer = tte_mark(s);
    bool is_nil = false;
    ReflectValue val = burrow__tt_indirect(tte_eval_pipeline(s, dot, r->pipe), &is_nil);
    /* mark top of stack before any variables in the body are pushed. */
    Int mark = tte_mark(s);
    bool ran = false;
    switch ((int)reflect_value_kind(val)) {
    case REFLECT_INT:
    case REFLECT_INT8:
    case REFLECT_INT16:
    case REFLECT_INT32:
    case REFLECT_INT64:
    case REFLECT_UINT:
    case REFLECT_UINT8:
    case REFLECT_UINT16:
    case REFLECT_UINT32:
    case REFLECT_UINT64:
    case REFLECT_UINTPTR:
        if (r->pipe->decl.len > 1)
            tte_errorf(s, "can't use %v to iterate over more than one variable",
                       burrow__tt_fmt_arg(val));
        ran = tte_range_int(s, r, mark, val);
        break;
    case REFLECT_ARRAY:
    case REFLECT_SLICE: {
        Int n = reflect_value_len(val);
        for (Int i = 0; i < n; i++) {
            if (tte_one_iteration(s, r, mark, tte_value_of(s, TYPE_INT, &i),
                                  reflect_value_index(val, i)))
                break;
        }
        ran = n > 0;
        break;
    }
    case REFLECT_MAP:
        if (reflect_value_len(val) > 0) {
            tte_range_map(s, r, mark, val);
            ran = true;
        }
        break;
    case REFLECT_CHAN:
        if (reflect_value_is_nil(val))
            break;
        if (reflect_type_chan_dir(reflect_value_type(val)) == REFLECT_SEND_DIR)
            tte_errorf(s, "range over send-only channel %v", burrow__tt_fmt_arg(val));
        ran = tte_range_chan(s, r, mark, val);
        break;
    case REFLECT_INVALID:
        break; /* An invalid value is likely a nil map, etc. and acts like an empty map. */
    default:
        /* Go ranges over a func that is an iterator too. A C function is not
         * one, so a func lands here with everything else. */
        tte_errorf(s, "range can't iterate over %v", burrow__tt_fmt_arg(val));
    }
    if (!ran && r->else_list != NULL)
        tte_walk(s, dot, TTE_NODE(&r->else_list->node_type));
    tte_pop(s, outer);
}

static void tte_walk_template(TteState *s, ReflectValue dot,
                              const ParseTemplateNode *t) {
    tte_at(s, t);
    TextTemplate *tmpl = text_template_lookup(s->tmpl, t->name);
    if (tmpl == NULL)
        tte_errorf(s, "template %q not defined", t->name);
    if (s->depth == TTE_MAX_EXEC_DEPTH || tte_stack_low(s->x, s->depth))
        tte_errorf(s, "exceeded maximum template depth (%d)", (Int)TTE_MAX_EXEC_DEPTH);
    if (tmpl->tree == NULL || tmpl->tree->root == NULL)
        tte_errorf(s, "%q is an incomplete or empty template", tmpl->name);
    /* Variables declared by the pipeline persist. */
    dot = tte_eval_pipeline(s, dot, t->pipe);
    TteState ns = *s;
    ns.depth++;
    ns.tmpl = tmpl;
    /* No dynamic scoping: template invocations inherit no variables. */
    ns.vars = NULL;
    ns.nvars = 0;
    ns.cap_vars = 0;
    tte_push(&ns, BURROW_S("$"), dot);
    tte_walk(&ns, dot, TTE_NODE(&tmpl->tree->root->node_type));
}

/* walk: steps through the major pieces of the template, writing what they
 * make as it goes. Returns what a break or continue asks for. */
static Int tte_walk(TteState *s, ReflectValue dot, ParseNode node) {
    tte_at(s, node);
    switch ((int)*node) {
    case PARSE_NODE_ACTION: {
        /* Do not pop variables so they persist until next end. Also, if the
         * action declares variables, don't print the result. */
        const ParseActionNode *n = (const ParseActionNode *)node;
        ReflectValue val = tte_eval_pipeline(s, dot, n->pipe);
        if (n->pipe->decl.len == 0)
            tte_print_value(s, n, val);
        return TTE_WALK_NEXT;
    }
    case PARSE_NODE_BREAK:
        return TTE_WALK_BREAK;
    case PARSE_NODE_COMMENT:
        return TTE_WALK_NEXT;
    case PARSE_NODE_CONTINUE:
        return TTE_WALK_CONTINUE;
    case PARSE_NODE_IF:
        return tte_walk_if_or_with(s, PARSE_NODE_IF, dot,
                                   &((const ParseIfNode *)node)->branch_node);
    case PARSE_NODE_LIST: {
        const ParseListNode *n = (const ParseListNode *)node;
        for (Int i = 0; i < n->nodes.len; i++) {
            Int code = tte_walk(s, dot, BURROW_AT(ParseNode, n->nodes, i));
            if (code != TTE_WALK_NEXT)
                return code;
        }
        return TTE_WALK_NEXT;
    }
    case PARSE_NODE_RANGE:
        tte_walk_range(s, dot, &((const ParseRangeNode *)node)->branch_node);
        return TTE_WALK_NEXT;
    case PARSE_NODE_TEMPLATE:
        tte_walk_template(s, dot, (const ParseTemplateNode *)node);
        return TTE_WALK_NEXT;
    case PARSE_NODE_TEXT:
        tte_write_text(s, (const ParseTextNode *)node);
        return TTE_WALK_NEXT;
    case PARSE_NODE_WITH:
        return tte_walk_if_or_with(s, PARSE_NODE_WITH, dot,
                                   &((const ParseWithNode *)node)->branch_node);
    default:
        tte_errorf(s, "unknown node: %s", tte_node_string(s, node));
    }
}

/* ------------------------------------------------------------- evaluation */

static ReflectValue tte_eval_command(TteState *s, ReflectValue dot,
                                     const ParseCommandNode *cmd, ReflectValue final);

/* evalPipeline: the value of a pipeline. A variable it declares is pushed on
 * the stack, so the caller pops the stack once it is done with the value. */
static ReflectValue tte_eval_pipeline(TteState *s, ReflectValue dot,
                                      const ParsePipeNode *pipe) {
    if (pipe == NULL)
        return tte_zero;
    tte_at(s, pipe);
    ReflectValue value = tte_missing();
    for (Int i = 0; i < pipe->cmds.len; i++) {
        /* previous value is this one's final arg. */
        value = tte_eval_command(s, dot, BURROW_AT(ParseCommandNode *, pipe->cmds, i),
                                 value);
        /* If the object has type interface{}, dig down one level to the thing
         * inside. */
        if (reflect_value_kind(value) == REFLECT_INTERFACE &&
            reflect_type_num_method(reflect_value_type(value)) == 0)
            value = reflect_value_elem(value);
    }
    for (Int i = 0; i < pipe->decl.len; i++) {
        const ParseVariableNode *v = BURROW_AT(ParseVariableNode *, pipe->decl, i);
        if (pipe->is_assign)
            tte_set_var(s, BURROW_AT(Str, v->ident, 0), value);
        else
            tte_push(s, BURROW_AT(Str, v->ident, 0), value);
    }
    return value;
}

static void tte_not_a_function(TteState *s, Slice args, ReflectValue final) {
    if (args.len > 1 || !tte_is_missing(final))
        tte_errorf(s, "can't give argument to non-function %s",
                   tte_node_string(s, BURROW_AT(ParseNode, args, 0)));
}

static bool tte_contains_any(Str s, const char *chars) {
    for (Int i = 0; i < s.len; i++) {
        if (strchr(chars, s.p[i]) != NULL)
            return true;
    }
    return false;
}

static bool tte_is_rune_int(Str s) {
    return s.len > 0 && s.p[0] == '\'';
}

static bool tte_is_hex_int(Str s) {
    return s.len > 2 && s.p[0] == '0' && (s.p[1] == 'x' || s.p[1] == 'X') &&
           !tte_contains_any(s, "pP");
}

/* idealConstant: a number where its type is not known, which takes the type
 * its syntax suggests. There is no uint ideal constant: the value must fit an
 * int. */
static ReflectValue tte_ideal_constant(TteState *s, const ParseNumberNode *constant) {
    tte_at(s, constant);
    if (constant->is_complex)
        return tte_value_of(s, TYPE_COMPLEX128, &constant->complex128);
    if (constant->is_float && !tte_is_hex_int(constant->text) &&
        !tte_is_rune_int(constant->text) && tte_contains_any(constant->text, ".eEpP"))
        return tte_value_of(s, TYPE_FLOAT64, &constant->float64);
    if (constant->is_int) {
        Int n = (Int)constant->int64;
        if ((int64_t)n != constant->int64)
            tte_errorf(s, "%s overflows int", constant->text);
        return tte_value_of(s, TYPE_INT, &n);
    }
    if (constant->is_uint)
        tte_errorf(s, "%s overflows int", constant->text);
    return tte_zero;
}

static ReflectValue tte_eval_field(TteState *s, ReflectValue dot, Str field_name,
                                   ParseNode node, Slice args, ReflectValue final,
                                   ReflectValue receiver);

/* evalFieldChain: .X.Y.Z, maybe with arguments. dot is what the arguments are
 * evaluated with, and receiver what the chain walks along. */
static ReflectValue tte_eval_field_chain(TteState *s, ReflectValue dot,
                                         ReflectValue receiver, ParseNode node,
                                         Slice ident, Slice args, ReflectValue final) {
    Int n = ident.len;
    for (Int i = 0; i < n - 1; i++)
        receiver =
            tte_eval_field(s, dot, BURROW_AT(Str, ident, i), node,
                           slice_nil(TYPE_UNSAFE_POINTER), tte_missing(), receiver);
    /* Now if it's a method, it gets the arguments. */
    return tte_eval_field(s, dot, BURROW_AT(Str, ident, n - 1), node, args, final,
                          receiver);
}

static ReflectValue tte_eval_field_node(TteState *s, ReflectValue dot,
                                        const ParseFieldNode *field, Slice args,
                                        ReflectValue final) {
    tte_at(s, field);
    return tte_eval_field_chain(s, dot, dot, TTE_NODE(&field->node_type), field->ident,
                                args, final);
}

static ReflectValue tte_eval_chain_node(TteState *s, ReflectValue dot,
                                        const ParseChainNode *chain, Slice args,
                                        ReflectValue final) {
    tte_at(s, chain);
    if (chain->field.len == 0)
        tte_errorf(s, "internal error: no fields in evalChainNode");
    if (*chain->node == PARSE_NODE_NIL)
        tte_errorf(s, "indirection through explicit nil in %s",
                   tte_node_string(s, chain));
    /* (pipe).Field1.Field2 has pipe as .Node, fields as .Field. Eval the
     * pipeline, then the fields. */
    ReflectValue pipe = tte_eval_arg(s, dot, NULL, chain->node);
    return tte_eval_field_chain(s, dot, pipe, TTE_NODE(&chain->node_type), chain->field,
                                args, final);
}

static ReflectValue tte_eval_variable_node(TteState *s, ReflectValue dot,
                                           const ParseVariableNode *variable,
                                           Slice args, ReflectValue final) {
    /* $x.Field has $x as the first ident, Field as the second. Eval the var,
     * then the fields. */
    tte_at(s, variable);
    ReflectValue value = tte_var_value(s, BURROW_AT(Str, variable->ident, 0));
    if (variable->ident.len == 1) {
        tte_not_a_function(s, args, final);
        return value;
    }
    Slice rest = variable->ident;
    rest.p = (Str *)rest.p + 1;
    rest.len--;
    rest.cap--;
    return tte_eval_field_chain(s, dot, value, TTE_NODE(&variable->node_type), rest,
                                args, final);
}

/* findFunction: the function called name, from the template's FuncMap first
 * and the builtins second. */
static bool tte_find_function(TteState *s, Str name, ReflectValue *fn,
                              bool *is_builtin) {
    TextTemplate *tmpl = s->tmpl;
    *is_builtin = false;
    if (tmpl != NULL && tmpl->common != NULL) {
        bool found = false;
        sync_rw_mutex_r_lock(&tmpl->common->mu_funcs);
        const ReflectValue *v = map_get(tmpl->common->exec_funcs, &name);
        if (v != NULL && reflect_value_is_valid(*v)) {
            *fn = *v;
            found = true;
        }
        sync_rw_mutex_r_unlock(&tmpl->common->mu_funcs);
        if (found)
            return true;
    }
    if (burrow__tt_find_builtin(s->x, name, fn)) {
        *is_builtin = true;
        return true;
    }
    return false;
}

static ReflectValue tte_eval_call(TteState *s, ReflectValue dot, ReflectValue fun,
                                  bool is_builtin, ParseNode node, Str name, Slice args,
                                  ReflectValue final);

static ReflectValue tte_eval_function(TteState *s, ReflectValue dot,
                                      const ParseIdentifierNode *node, ParseNode cmd,
                                      Slice args, ReflectValue final) {
    tte_at(s, node);
    Str name = node->ident;
    ReflectValue function = tte_zero;
    bool is_builtin = false;
    if (!tte_find_function(s, name, &function, &is_builtin))
        tte_errorf(s, "%q is not a defined function", name);
    return tte_eval_call(s, dot, function, is_builtin, cmd, name, args, final);
}

/* evalField: (.Field) or (.Field arg1 arg2). final is the value of the
 * pipeline so far, if there is one. */
static ReflectValue tte_eval_field(TteState *s, ReflectValue dot, Str field_name,
                                   ParseNode node, Slice args, ReflectValue final,
                                   ReflectValue receiver) {
    if (!reflect_value_is_valid(receiver)) {
        /* Treat invalid value as missing map key. */
        if (tte_missing_key(s) == BURROW__TT_MAP_ERROR)
            tte_errorf(s, "nil data; no entry for key %q", field_name);
        return tte_zero;
    }
    ReflectType typ = reflect_value_type(receiver);
    bool is_nil = false;
    receiver = burrow__tt_indirect(receiver, &is_nil);
    if (reflect_value_kind(receiver) == REFLECT_INTERFACE && is_nil)
        /* Calling a method on a nil interface can't work. */
        tte_errorf(s, "nil pointer evaluating %s.%s", reflect_type_string(typ),
                   field_name);

    /* A receiver is always a pointer in burrow, so T has every method of *T and
     * there is no need to take the address first, as Go does. */
    ReflectValue method = reflect_value_method_by_name(receiver, tte_a(s), field_name);
    if (reflect_value_is_valid(method))
        return tte_eval_call(s, dot, method, false, node, field_name, args, final);
    bool has_args = args.len > 1 || !tte_is_missing(final);
    /* It's not a method; must be a field of a struct or an element of a map. */
    switch ((int)reflect_value_kind(receiver)) {
    case REFLECT_STRUCT: {
        ReflectStructField t_field;
        if (reflect_type_field_by_name(tte_a(s), reflect_value_type(receiver),
                                       field_name, &t_field)) {
            Error err = BURROW_NO_ERROR;
            ReflectValue field =
                reflect_value_field_by_index_err(receiver, t_field.index, &err);
            if (!reflect_struct_field_is_exported(t_field))
                tte_errorf(s, "%s is an unexported field of struct type %s", field_name,
                           reflect_type_string(typ));
            if (BURROW_FAILED(err))
                tte_errorf(s, "%v", err);
            /* If it's a function, we must call it. */
            if (has_args)
                tte_errorf(s, "%s has arguments but cannot be invoked as function",
                           field_name);
            return field;
        }
        break;
    }
    case REFLECT_MAP: {
        /* If it's a map, attempt to use the field name as a key. */
        ReflectType mt = reflect_value_type(receiver);
        if (reflect_type_assignable_to(TYPE_STRING, reflect_type_key(mt))) {
            if (has_args)
                tte_errorf(s, "%s is not a method but has arguments", field_name);
            ReflectValue name_val = tte_value_of(s, TYPE_STRING, &field_name);
            ReflectValue result = reflect_value_map_index(receiver, name_val);
            if (!reflect_value_is_valid(result)) {
                switch (tte_missing_key(s)) {
                case BURROW__TT_MAP_ZERO:
                    result = reflect_zero(reflect_type_elem(mt));
                    break;
                case BURROW__TT_MAP_ERROR:
                    tte_errorf(s, "map has no entry for key %q", field_name);
                default: /* mapInvalid: just use the invalid value. */
                    break;
                }
            }
            return result;
        }
        break;
    }
    case REFLECT_POINTER: {
        ReflectType etyp = reflect_type_elem(reflect_value_type(receiver));
        if (reflect_type_kind(etyp) == REFLECT_STRUCT) {
            ReflectStructField f;
            /* If there's no such field, say "can't evaluate" instead of "nil
             * pointer evaluating". */
            if (!reflect_type_field_by_name(tte_a(s), etyp, field_name, &f))
                break;
        }
        if (is_nil)
            tte_errorf(s, "nil pointer evaluating %s.%s", reflect_type_string(typ),
                       field_name);
        break;
    }
    default:
        break;
    }
    tte_errorf(s, "can't evaluate field %s in type %s", field_name,
               reflect_type_string(typ));
}

/* validateType: value, made valid and assignable to typ, or an error. */
static ReflectValue tte_validate_type(TteState *s, ReflectValue value,
                                      ReflectType typ) {
    if (!reflect_value_is_valid(value)) {
        if (typ == NULL)
            /* An untyped nil interface{}. Accept as a proper nil value. */
            return tte_zero;
        if (burrow__tt_can_be_nil(typ))
            /* Like above, but use the zero value of the non-nil type. */
            return reflect_zero(typ);
        tte_errorf(s, "invalid value; expected %s", reflect_type_string(typ));
    }
    if (typ == TYPE_REFLECT_VALUE && reflect_value_type(value) != typ)
        return tte_wrap(s, value);
    if (typ != NULL && !reflect_type_assignable_to(reflect_value_type(value), typ)) {
        if (reflect_value_kind(value) == REFLECT_INTERFACE &&
            !reflect_value_is_nil(value)) {
            value = reflect_value_elem(value);
            if (reflect_type_assignable_to(reflect_value_type(value), typ))
                return value;
            /* fallthrough */
        }
        /* Does one dereference or indirection work? One is almost always all
         * you need. */
        ReflectType vt = reflect_value_type(value);
        if (reflect_value_kind(value) == REFLECT_POINTER &&
            reflect_type_assignable_to(reflect_type_elem(vt), typ)) {
            value = reflect_value_elem(value);
            if (!reflect_value_is_valid(value))
                tte_errorf(s, "dereference of nil pointer of type %s",
                           reflect_type_string(typ));
        } else if (reflect_type_assignable_to(reflect_pointer_to(vt), typ) &&
                   reflect_value_can_addr(value)) {
            value = reflect_value_addr(value, tte_a(s));
        } else {
            tte_errorf(s, "wrong type for value; expected %s; got %s",
                       reflect_type_string(typ), reflect_type_string(vt));
        }
    }
    return value;
}

/* evalBool and the others: a constant of the kind typ needs, in a new value of
 * typ. */
static ReflectValue tte_eval_bool(TteState *s, ReflectType typ, ParseNode n) {
    tte_at(s, n);
    if (*n == PARSE_NODE_BOOL) {
        ReflectValue value = tte_new(s, typ);
        reflect_value_set_bool(value, ((const ParseBoolNode *)n)->true_);
        return value;
    }
    tte_errorf(s, "expected bool; found %s", tte_node_string(s, n));
}

static ReflectValue tte_eval_string(TteState *s, ReflectType typ, ParseNode n) {
    tte_at(s, n);
    if (*n == PARSE_NODE_STRING) {
        ReflectValue value = tte_new(s, typ);
        reflect_value_set_string(value, ((const ParseStringNode *)n)->text);
        return value;
    }
    tte_errorf(s, "expected string; found %s", tte_node_string(s, n));
}

static ReflectValue tte_eval_integer(TteState *s, ReflectType typ, ParseNode n) {
    tte_at(s, n);
    if (*n == PARSE_NODE_NUMBER && ((const ParseNumberNode *)n)->is_int) {
        ReflectValue value = tte_new(s, typ);
        reflect_value_set_int(value, ((const ParseNumberNode *)n)->int64);
        return value;
    }
    tte_errorf(s, "expected integer; found %s", tte_node_string(s, n));
}

static ReflectValue tte_eval_unsigned_integer(TteState *s, ReflectType typ,
                                              ParseNode n) {
    tte_at(s, n);
    if (*n == PARSE_NODE_NUMBER && ((const ParseNumberNode *)n)->is_uint) {
        ReflectValue value = tte_new(s, typ);
        reflect_value_set_uint(value, ((const ParseNumberNode *)n)->uint64);
        return value;
    }
    tte_errorf(s, "expected unsigned integer; found %s", tte_node_string(s, n));
}

static ReflectValue tte_eval_float(TteState *s, ReflectType typ, ParseNode n) {
    tte_at(s, n);
    if (*n == PARSE_NODE_NUMBER && ((const ParseNumberNode *)n)->is_float) {
        ReflectValue value = tte_new(s, typ);
        reflect_value_set_float(value, ((const ParseNumberNode *)n)->float64);
        return value;
    }
    tte_errorf(s, "expected float; found %s", tte_node_string(s, n));
}

static ReflectValue tte_eval_complex(TteState *s, ReflectType typ, ParseNode n) {
    if (*n == PARSE_NODE_NUMBER && ((const ParseNumberNode *)n)->is_complex) {
        ReflectValue value = tte_new(s, typ);
        reflect_value_set_complex(value, ((const ParseNumberNode *)n)->complex128);
        return value;
    }
    tte_errorf(s, "expected complex; found %s", tte_node_string(s, n));
}

static ReflectValue tte_eval_empty_interface(TteState *s, ReflectValue dot,
                                             ParseNode n) {
    tte_at(s, n);
    Slice none = slice_nil(TYPE_UNSAFE_POINTER);
    switch ((int)*n) {
    case PARSE_NODE_BOOL:
        return tte_value_of(s, TYPE_BOOL, &((const ParseBoolNode *)n)->true_);
    case PARSE_NODE_DOT:
        return dot;
    case PARSE_NODE_FIELD:
        return tte_eval_field_node(s, dot, (const ParseFieldNode *)n, none,
                                   tte_missing());
    case PARSE_NODE_IDENTIFIER:
        return tte_eval_function(s, dot, (const ParseIdentifierNode *)n, n, none,
                                 tte_missing());
    case PARSE_NODE_NIL:
        /* NilNode is handled in evalArg, the only place that calls here. */
        tte_errorf(s, "evalEmptyInterface: nil (can't happen)");
    case PARSE_NODE_NUMBER:
        return tte_ideal_constant(s, (const ParseNumberNode *)n);
    case PARSE_NODE_STRING:
        return tte_value_of(s, TYPE_STRING, &((const ParseStringNode *)n)->text);
    case PARSE_NODE_VARIABLE:
        return tte_eval_variable_node(s, dot, (const ParseVariableNode *)n, none,
                                      tte_missing());
    case PARSE_NODE_PIPE:
        return tte_eval_pipeline(s, dot, (const ParsePipeNode *)n);
    default:
        break;
    }
    tte_errorf(s, "can't handle assignment of %s to empty interface argument",
               tte_node_string(s, n));
}

static ReflectValue tte_eval_arg(TteState *s, ReflectValue dot, ReflectType typ,
                                 ParseNode n) {
    tte_at(s, n);
    Slice none = slice_nil(TYPE_UNSAFE_POINTER);
    switch ((int)*n) {
    case PARSE_NODE_DOT:
        return tte_validate_type(s, dot, typ);
    case PARSE_NODE_NIL:
        if (typ != NULL && burrow__tt_can_be_nil(typ))
            return reflect_zero(typ);
        tte_errorf(s, "cannot assign nil to %s", tte_type_string(typ));
    case PARSE_NODE_FIELD: {
        ParseNode self[1] = {n};
        return tte_validate_type(
            s,
            tte_eval_field_node(s, dot, (const ParseFieldNode *)n,
                                slice_from(self, 1, 1, TYPE_UNSAFE_POINTER),
                                tte_missing()),
            typ);
    }
    case PARSE_NODE_VARIABLE:
        return tte_validate_type(s,
                                 tte_eval_variable_node(s, dot,
                                                        (const ParseVariableNode *)n,
                                                        none, tte_missing()),
                                 typ);
    case PARSE_NODE_PIPE:
        return tte_validate_type(s, tte_eval_pipeline(s, dot, (const ParsePipeNode *)n),
                                 typ);
    case PARSE_NODE_IDENTIFIER:
        return tte_validate_type(s,
                                 tte_eval_function(s, dot,
                                                   (const ParseIdentifierNode *)n, n,
                                                   none, tte_missing()),
                                 typ);
    case PARSE_NODE_CHAIN:
        return tte_validate_type(
            s,
            tte_eval_chain_node(s, dot, (const ParseChainNode *)n, none, tte_missing()),
            typ);
    default:
        break;
    }
    switch (typ == NULL ? (int)REFLECT_INVALID : (int)reflect_type_kind(typ)) {
    case REFLECT_BOOL:
        return tte_eval_bool(s, typ, n);
    case REFLECT_COMPLEX64:
    case REFLECT_COMPLEX128:
        return tte_eval_complex(s, typ, n);
    case REFLECT_FLOAT32:
    case REFLECT_FLOAT64:
        return tte_eval_float(s, typ, n);
    case REFLECT_INT:
    case REFLECT_INT8:
    case REFLECT_INT16:
    case REFLECT_INT32:
    case REFLECT_INT64:
        return tte_eval_integer(s, typ, n);
    case REFLECT_INTERFACE:
        if (reflect_type_num_method(typ) == 0)
            return tte_eval_empty_interface(s, dot, n);
        break;
    case REFLECT_STRUCT:
        if (typ == TYPE_REFLECT_VALUE)
            return tte_wrap(s, tte_eval_empty_interface(s, dot, n));
        break;
    case REFLECT_STRING:
        return tte_eval_string(s, typ, n);
    case REFLECT_UINT:
    case REFLECT_UINT8:
    case REFLECT_UINT16:
    case REFLECT_UINT32:
    case REFLECT_UINT64:
    case REFLECT_UINTPTR:
        return tte_eval_unsigned_integer(s, typ, n);
    default:
        break;
    }
    tte_errorf(s, "can't handle %s for arg of type %s", tte_node_string(s, n),
               tte_type_string(typ));
}

/* evalCall: calls a function or a method, which already has its receiver
 * bound. args, when there are any, has the function's own node first, as a
 * shell's argument list has the command. */
static ReflectValue tte_eval_call(TteState *s, ReflectValue dot, ReflectValue fun,
                                  bool is_builtin, ParseNode node, Str name, Slice args,
                                  ReflectValue final) {
    if (args.len > 0) {
        /* Zeroth arg is function name/node; not passed to function. */
        args.p = (ParseNode *)args.p + 1;
        args.len--;
        args.cap--;
    }
    ReflectType typ = reflect_value_type(fun);
    Int num_in = args.len;
    bool has_final = !tte_is_missing(final);
    if (has_final)
        num_in++;
    Int num_fixed = args.len;
    Int typ_num_in = reflect_type_num_in(typ);
    bool variadic = reflect_type_is_variadic(typ);
    if (variadic) {
        num_fixed = typ_num_in - 1; /* last arg is the variadic one. */
        if (num_in < num_fixed)
            tte_errorf(s, "wrong number of args for %s: want at least %d got %d", name,
                       typ_num_in - 1, args.len);
    } else if (num_in != typ_num_in) {
        tte_errorf(s, "wrong number of args for %s: want %d got %d", name, typ_num_in,
                   num_in);
    }
    Error gerr = burrow__tt_good_func(name, typ);
    if (BURROW_FAILED(gerr))
        tte_errorf(s, "%v", gerr);

    /* Special case for builtin and/or, which short-circuit. */
    bool is_or = str_eq(name, BURROW_S("or"));
    if (is_builtin && (is_or || str_eq(name, BURROW_S("and")))) {
        ReflectType arg_type = reflect_type_in(typ, 0);
        ReflectValue v = tte_zero;
        for (Int i = 0; i < args.len; i++) {
            ReflectValue w =
                tte_eval_arg(s, dot, arg_type, BURROW_AT(ParseNode, args, i));
            v = *(const ReflectValue *)w.ptr;
            bool ok = false;
            if (burrow__tt_is_true(burrow__tt_indirect_interface(v), &ok) == is_or)
                return v;
        }
        if (has_final)
            /* The last argument to and/or is coming from the pipeline. We
             * didn't short circuit on an earlier argument, so we are going to
             * return this one, after checking its type. */
            v = burrow__tt_unwrap(tte_validate_type(s, final, arg_type));
        return v;
    }

    /* Build the arg list. One more for call, which puts the callee's name
     * first. */
    Int extra = is_builtin && str_eq(name, BURROW_S("call")) ? 1 : 0;
    Slice argv =
        slice_make(tte_a(s), TYPE_REFLECT_VALUE, num_in + extra, num_in + extra);
    if (num_in + extra > 0 && argv.p == NULL)
        burrow__tt_oom();
    ReflectValue *av = (ReflectValue *)argv.p + extra;
    /* Args must be evaluated. Fixed args first. */
    Int i = 0;
    for (; i < num_fixed && i < args.len; i++)
        av[i] = tte_eval_arg(s, dot, reflect_type_in(typ, i),
                             BURROW_AT(ParseNode, args, i));
    /* Now the ... args. */
    if (variadic) {
        /* Argument is a slice. */
        ReflectType arg_type = reflect_type_elem(reflect_type_in(typ, typ_num_in - 1));
        for (; i < args.len; i++)
            av[i] = tte_eval_arg(s, dot, arg_type, BURROW_AT(ParseNode, args, i));
    }
    /* Add final value if necessary. */
    if (has_final) {
        ReflectType t = reflect_type_in(typ, typ_num_in - 1);
        if (variadic) {
            if (num_in - 1 < num_fixed)
                /* The added final argument corresponds to a fixed parameter of
                 * the function. */
                t = reflect_type_in(typ, num_in - 1);
            else
                /* The added final argument corresponds to the variadic part. */
                t = reflect_type_elem(t);
        }
        av[i] = tte_validate_type(s, final, t);
    }

    /* Special case for the "call" builtin. Insert the name of the callee
     * function as the first argument. */
    if (extra > 0) {
        Str callee_name = args.len == 0
                              /* final must be present or we would have errored
                               * out above. */
                              ? reflect_value_string(final)
                              : tte_node_string(s, BURROW_AT(ParseNode, args, 0));
        BURROW_AT(ReflectValue, argv, 0) = tte_value_of(s, TYPE_STRING, &callee_name);
        fun = burrow__tt_call_fn(s->x);
    }

    Error err = BURROW_NO_ERROR;
    ReflectValue v = burrow__tt_safe_call(tte_a(s), fun, argv, &err);
    /* If we have an error that is not nil, stop execution and return that
     * error to the caller. */
    if (BURROW_FAILED(err)) {
        tte_at(s, node);
        tte_fail_wrap(s, fmt_sprintf_v(tte_a(s), "error calling %s: ", name), err);
    }
    return burrow__tt_unwrap(v);
}

static ReflectValue tte_eval_command(TteState *s, ReflectValue dot,
                                     const ParseCommandNode *cmd, ReflectValue final) {
    ParseNode first_word = BURROW_AT(ParseNode, cmd->args, 0);
    switch ((int)*first_word) {
    case PARSE_NODE_FIELD:
        return tte_eval_field_node(s, dot, (const ParseFieldNode *)first_word,
                                   cmd->args, final);
    case PARSE_NODE_CHAIN:
        return tte_eval_chain_node(s, dot, (const ParseChainNode *)first_word,
                                   cmd->args, final);
    case PARSE_NODE_IDENTIFIER:
        /* Must be a function. */
        return tte_eval_function(s, dot, (const ParseIdentifierNode *)first_word,
                                 TTE_NODE(&cmd->node_type), cmd->args, final);
    case PARSE_NODE_PIPE:
        /* Parenthesized pipeline. The arguments are all inside the pipeline;
         * final must be absent. */
        tte_not_a_function(s, cmd->args, final);
        return tte_eval_pipeline(s, dot, (const ParsePipeNode *)first_word);
    case PARSE_NODE_VARIABLE:
        return tte_eval_variable_node(s, dot, (const ParseVariableNode *)first_word,
                                      cmd->args, final);
    default:
        break;
    }
    tte_at(s, first_word);
    tte_not_a_function(s, cmd->args, final);
    switch ((int)*first_word) {
    case PARSE_NODE_BOOL:
        return tte_value_of(s, TYPE_BOOL, &((const ParseBoolNode *)first_word)->true_);
    case PARSE_NODE_DOT:
        return dot;
    case PARSE_NODE_NIL:
        tte_errorf(s, "nil is not a command");
    case PARSE_NODE_NUMBER:
        return tte_ideal_constant(s, (const ParseNumberNode *)first_word);
    case PARSE_NODE_STRING:
        return tte_value_of(s, TYPE_STRING,
                            &((const ParseStringNode *)first_word)->text);
    default:
        break;
    }
    tte_errorf(s, "can't evaluate command %q", tte_node_string(s, first_word));
}

/* printValue: writes v the way fmt prints it. */
static void tte_print_value(TteState *s, const void *n, ReflectValue v) {
    tte_at(s, n);
    Any iface;
    if (!burrow__tt_printable(v, &iface))
        tte_errorf(s, "can't print %s of type %s", tte_node_string(s, n),
                   reflect_type_string(reflect_value_type(v)));
    Error err = BURROW_NO_ERROR;
    fmt_fprint(s->wr, slice_from(&iface, 1, 1, TYPE_ANY), &err);
    if (BURROW_FAILED(err))
        tte_write_error(err);
}

/* ---------------------------------------------------------------- Execute */

/* The walk, which panics on an error. */
static void tte_execute(TteState *s, ReflectValue value) {
    tte_push(s, BURROW_S("$"), value);
    if (s->tmpl->tree == NULL || s->tmpl->tree->root == NULL)
        tte_errorf(s, "%q is an incomplete or empty template", s->tmpl->name);
    tte_walk(s, value, TTE_NODE(&s->tmpl->tree->root->node_type));
}

/* execute, apart from its arena, which errRecover leaves alone. */
static Error tte_run(TteState *s, ReflectValue value) {
    Error err = BURROW_NO_ERROR;
    BURROW_TRY {
        tte_execute(s, value);
    }
    BURROW_CATCH(p) {
        err = tte_recover(p);
    }
    BURROW_TRY_END;
    return err;
}

Error text_template_execute(TextTemplate *t, IoWriter wr, Any data) {
    ReflectValue value;
    if (data.t == TYPE_REFLECT_VALUE)
        value = *(const ReflectValue *)data.data;
    else
        value = reflect_value_of(data);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    burrow__TtExec x = {arena_allocator(&ar), 0, NULL};
    TteState s = {&x, t, wr, NULL, NULL, 0, 0, 0};
    Error err = tte_run(&s, value);
    arena_free(&ar);
    return err;
}

Error text_template_execute_template(TextTemplate *t, IoWriter wr, Str name, Any data) {
    TextTemplate *tmpl = text_template_lookup(t, name);
    if (tmpl == NULL)
        return fmt_errorf_v("template: no template %q associated with template %q",
                            name, t->name);
    return text_template_execute(tmpl, wr, data);
}
