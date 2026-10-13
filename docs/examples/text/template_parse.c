#include "burrow/burrow.h"

static void trees(Alloc *a) {
    // doc: trees
    Str text = BURROW_S("{{define \"item\"}}<li>{{.}}</li>{{end}}"
                        "<ul>{{range .Items}}{{template \"item\" .}}{{end}}</ul>");
    Error err = BURROW_NO_ERROR;
    Map *trees = parse_parse(a, BURROW_S("page"), text, BURROW_STR_EMPTY,
                             BURROW_STR_EMPTY, (Slice){0}, &err);
    if (BURROW_FAILED(err)) {
        fmt_println_v(err);
        return;
    }
    Str names[] = {BURROW_S("page"), BURROW_S("item")};
    for (int i = 0; i < 2; i++) {
        ParseTree **t = map_get(trees, &names[i]);
        fmt_printf_v("%s: %s\n", names[i], parse_list_node_string((*t)->root, a));
    }
    // doc: end
}

static void walk(Alloc *a) {
    // doc: walk
    Error err = BURROW_NO_ERROR;
    ParseTree *t = parse_new(a, BURROW_S("greeting"), (Slice){0});
    t = parse_tree_parse(
        t, BURROW_S("Hello, {{.Name}}! You have {{len .Mail}} letters."),
        BURROW_STR_EMPTY, BURROW_STR_EMPTY,
        map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0), (Slice){0}, &err);
    if (t == NULL) {
        fmt_println_v(err);
        return;
    }
    for (Int i = 0; i < t->root->nodes.len; i++) {
        ParseNode n = BURROW_AT(ParseNode, t->root->nodes, i);
        switch (*n) {
        case PARSE_NODE_TEXT:
            fmt_printf_v("text   %q\n", parse_node_string(n, a));
            break;
        case PARSE_NODE_ACTION: {
            ParseActionNode *act = (ParseActionNode *)n;
            ParseCommandNode *cmd = BURROW_AT(ParseCommandNode *, act->pipe->cmds, 0);
            fmt_printf_v("action %s, %d argument(s)\n", parse_node_string(n, a),
                         cmd->args.len);
            break;
        }
        default:
            break;
        }
    }
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
    Str bad[] = {BURROW_S("{{if .Ready}}go"), BURROW_S("{{.Name | shout}}"),
                 BURROW_S("line one\n{{.X 1 2 3 ) }}")};
    for (int i = 0; i < 3; i++) {
        Error err = BURROW_NO_ERROR;
        parse_parse(a, BURROW_S("bad"), bad[i], BURROW_STR_EMPTY, BURROW_STR_EMPTY,
                    (Slice){0}, &err);
        fmt_println_v(err);
    }
    // doc: end
}

static void funcs(Alloc *a) {
    // doc: funcs
    Map *fm = map_make(a, TYPE_STRING, TYPE_ANY, 0);
    Str name = BURROW_S("shout");
    Any f = BURROW_ANY_VAL(TYPE_INT, Int, 1); // only the name matters to the parser
    map_set(fm, &name, &f);
    Error err = BURROW_NO_ERROR;
    Map *trees =
        parse_parse(a, BURROW_S("ok"), BURROW_S("{{.Name | shout}}"), BURROW_STR_EMPTY,
                    BURROW_STR_EMPTY, (Slice){&fm, 1, 1, TYPE_UNSAFE_POINTER}, &err);
    Str ok = BURROW_S("ok");
    ParseTree **t = map_get(trees, &ok);
    fmt_println_v(err, parse_list_node_string((*t)->root, a));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    trees(a);
    walk(a);
    errors(a);
    funcs(a);
    arena_free(&ar);
    return 0;
}

/* Output:
*/
