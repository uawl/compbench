#include "ast.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *cursor;
    unsigned line;
    char token[CB_NAME_SIZE];
    CbProgram *p;
    CbFunction *fn;
    char names[CB_MAX_LOCALS][CB_NAME_SIZE];
    int slots[CB_MAX_LOCALS], bindings, depth;
    char *error;
    size_t error_size;
    int failed;
} Parser;

static void fail(Parser *s, const char *format, ...) {
    if (s->failed) return;
    s->failed = 1;
    if (!s->error_size) return;
    int used = snprintf(s->error, s->error_size, "line %u: ", s->line);
    if (used < 0 || (size_t)used >= s->error_size) return;
    va_list ap;
    va_start(ap, format);
    vsnprintf(s->error + used, s->error_size - (size_t)used, format, ap);
    va_end(ap);
}

static void next(Parser *s) {
    if (s->failed) return;
    for (;;) {
        while (isspace((unsigned char)*s->cursor)) {
            if (*s->cursor == '\n') ++s->line;
            ++s->cursor;
        }
        if (*s->cursor != ';') break;
        while (*s->cursor && *s->cursor != '\n') ++s->cursor;
    }
    size_t n = 0;
    if (*s->cursor == '(' || *s->cursor == ')') {
        s->token[n++] = *s->cursor++;
    } else {
        while (*s->cursor && !isspace((unsigned char)*s->cursor) &&
               *s->cursor != '(' && *s->cursor != ')' && *s->cursor != ';') {
            if (n + 1 >= sizeof(s->token)) { fail(s, "token too long"); return; }
            s->token[n++] = *s->cursor++;
        }
    }
    s->token[n] = 0;
}

static int expect(Parser *s, const char *token) {
    if (s->failed) return 0;
    if (strcmp(s->token, token)) {
        fail(s, "expected '%s', found '%s'", token, s->token);
        return 0;
    }
    next(s);
    return !s->failed;
}

static int identifier(const char *s) {
    if (!isalpha((unsigned char)*s) && *s != '_') return 0;
    for (++s; *s; ++s)
        if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    return 1;
}

static CbExpr *node(Parser *s, CbOp op) {
    if (s->p->nodes == CB_MAX_NODES) { fail(s, "too many AST nodes"); return NULL; }
    CbExpr *e = &s->p->pool[s->p->nodes++];
    e->op = op;
    e->temp = s->fn->temps++;
    return e;
}

static int bind(Parser *s, const char *name) {
    if (!identifier(name) || !strcmp(name, "len") ||
        !strcmp(name, "input") || !strcmp(name, "nil")) {
        fail(s, "invalid or reserved binding '%s'", name); return -1;
    }
    if (s->bindings == CB_MAX_LOCALS || s->fn->locals == CB_MAX_LOCALS) {
        fail(s, "too many local bindings"); return -1;
    }
    int slot = s->fn->locals++;
    strcpy(s->names[s->bindings], name);
    s->slots[s->bindings++] = slot;
    return slot;
}

static CbExpr *expression(Parser *s) {
    if (s->failed) return NULL;
    if (++s->depth > 256) { fail(s, "AST nesting exceeds 256"); return NULL; }
    CbExpr *e = NULL;
    if (!strcmp(s->token, "(")) {
        next(s);
        char op[CB_NAME_SIZE];
        strcpy(op, s->token);
        next(s);
        if (!strcmp(op, "if")) {
            e = node(s, CB_IF);
            if (e) for (int i = 0; i < 3; ++i) e->arg[i] = expression(s);
        } else if (!strcmp(op, "let")) {
            e = node(s, CB_LET);
            char name[CB_NAME_SIZE];
            strcpy(name, s->token);
            next(s);
            if (e) e->arg[0] = expression(s);
            int old = s->bindings;
            int slot = bind(s, name);
            if (e) { e->index = slot; e->arg[1] = expression(s); }
            s->bindings = old;
        } else if (!strcmp(op, "call")) {
            e = node(s, CB_CALL);
            if (e) strcpy(e->name, s->token);
            if (!identifier(s->token)) fail(s, "invalid function name");
            next(s);
            while (!s->failed && strcmp(s->token, ")")) {
                if (e->argc == CB_MAX_PARAMS) { fail(s, "at most four call arguments"); break; }
                e->arg[e->argc++] = expression(s);
            }
        } else if (!strcmp(op, "get") || !strcmp(op, "head") ||
                   !strcmp(op, "tail") || !strcmp(op, "is_nil")) {
            CbOp kind = !strcmp(op, "get") ? CB_GET : !strcmp(op, "head") ? CB_HEAD :
                        !strcmp(op, "tail") ? CB_TAIL : CB_IS_NIL;
            e = node(s, kind);
            if (e) e->arg[0] = expression(s);
        } else if (!strcmp(op, "cons")) {
            e = node(s, CB_CONS);
            if (e) { e->arg[0] = expression(s); e->arg[1] = expression(s); }
        } else {
            static const struct { const char *name; CbOp op; } ops[] = {
                {"add", CB_ADD}, {"sub", CB_SUB}, {"mul", CB_MUL},
                {"div", CB_DIV}, {"mod", CB_MOD}, {"and", CB_AND},
                {"or", CB_OR}, {"xor", CB_XOR}, {"shl", CB_SHL},
                {"shr", CB_SHR}, {"eq", CB_EQ}, {"ne", CB_NE},
                {"lt", CB_LT}, {"le", CB_LE}, {"gt", CB_GT}, {"ge", CB_GE}
            };
            for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
                if (!strcmp(op, ops[i].name)) { e = node(s, ops[i].op); break; }
            if (!e && !s->failed) fail(s, "unknown operation '%s'", op);
            if (e) { e->arg[0] = expression(s); e->arg[1] = expression(s); }
        }
        expect(s, ")");
    } else if (isdigit((unsigned char)s->token[0])) {
        errno = 0;
        char *end;
        int base = s->token[0] == '0' && (s->token[1] == 'x' || s->token[1] == 'X') ? 16 : 10;
        unsigned long long v = strtoull(s->token, &end, base);
        if (errno || *end) fail(s, "invalid u64 literal '%s'", s->token);
        else { e = node(s, CB_CONST); if (e) e->value = (uint64_t)v; }
        next(s);
    } else if (!strcmp(s->token, "len")) {
        e = node(s, CB_LEN);
        next(s);
    } else if (!strcmp(s->token, "input") || !strcmp(s->token, "nil")) {
        e = node(s, !strcmp(s->token, "input") ? CB_INPUT : CB_NIL);
        next(s);
    } else {
        int slot = -1;
        for (int i = s->bindings - 1; i >= 0; --i)
            if (!strcmp(s->token, s->names[i])) { slot = s->slots[i]; break; }
        if (slot < 0) fail(s, "unbound variable '%s'", s->token);
        else { e = node(s, CB_VAR); if (e) e->index = slot; }
        next(s);
    }
    --s->depth;
    return e;
}

/* Unify expression, local-slot and function-result types across recursion. */
#define TYPE_COUNT (CB_MAX_NODES + CB_MAX_FUNCTIONS * (CB_MAX_LOCALS + 1))
typedef struct { int parent[TYPE_COUNT], kind[TYPE_COUNT]; } Types;

static int root(Types *t, int i) {
    int r = i;
    while (t->parent[r] != r) r = t->parent[r];
    while (t->parent[i] != i) { int next_id = t->parent[i]; t->parent[i] = r; i = next_id; }
    return r;
}

static void unify(Parser *s, Types *t, int a, int b) {
    a = root(t, a); b = root(t, b);
    if (a == b) return;
    if (t->kind[a] >= 0 && t->kind[b] >= 0 && t->kind[a] != t->kind[b]) {
        fail(s, "type mismatch: u64 and list"); return;
    }
    t->parent[b] = a;
    if (t->kind[a] < 0) t->kind[a] = t->kind[b];
}

static void require_type(Parser *s, Types *t, int id, CbType kind) {
    id = root(t, id);
    if (t->kind[id] >= 0 && t->kind[id] != (int)kind) fail(s, "type mismatch: u64 and list");
    else t->kind[id] = (int)kind;
}

static int local_type_id(int fn, int slot) {
    return CB_MAX_NODES + fn * (CB_MAX_LOCALS + 1) + slot;
}

static int expr_id(const Parser *s, const CbExpr *e) { return (int)(e - s->p->pool); }

static void type_expression(Parser *s, Types *t, int fn, const CbExpr *e) {
    if (s->failed) return;
    int id = expr_id(s, e);
    for (int j = 0; j < CB_MAX_PARAMS; ++j)
        if (e->arg[j]) type_expression(s, t, fn, e->arg[j]);
    switch (e->op) {
    case CB_CONST: case CB_LEN: require_type(s, t, id, CB_U64); break;
    case CB_VAR: unify(s, t, id, local_type_id(fn, e->index)); break;
    case CB_INPUT: case CB_NIL: require_type(s, t, id, CB_LIST); break;
    case CB_GET:
        require_type(s, t, id, CB_U64);
        require_type(s, t, expr_id(s, e->arg[0]), CB_U64); break;
    case CB_CONS:
        require_type(s, t, id, CB_LIST);
        require_type(s, t, expr_id(s, e->arg[0]), CB_U64);
        require_type(s, t, expr_id(s, e->arg[1]), CB_LIST); break;
    case CB_HEAD: case CB_TAIL: case CB_IS_NIL:
        require_type(s, t, id, e->op == CB_TAIL ? CB_LIST : CB_U64);
        require_type(s, t, expr_id(s, e->arg[0]), CB_LIST); break;
    case CB_IF:
        require_type(s, t, expr_id(s, e->arg[0]), CB_U64);
        unify(s, t, id, expr_id(s, e->arg[1]));
        unify(s, t, id, expr_id(s, e->arg[2])); break;
    case CB_LET:
        unify(s, t, local_type_id(fn, e->index), expr_id(s, e->arg[0]));
        unify(s, t, id, expr_id(s, e->arg[1])); break;
    case CB_CALL:
        unify(s, t, id, local_type_id(e->index, CB_MAX_LOCALS));
        for (int j = 0; j < e->argc; ++j)
            unify(s, t, expr_id(s, e->arg[j]), local_type_id(e->index, j));
        break;
    default:
        require_type(s, t, id, CB_U64);
        require_type(s, t, expr_id(s, e->arg[0]), CB_U64);
        require_type(s, t, expr_id(s, e->arg[1]), CB_U64); break;
    }
}

static void infer_types(Parser *s) {
    Types *t = malloc(sizeof(*t));
    if (!t) { fail(s, "out of memory"); return; }
    for (int i = 0; i < TYPE_COUNT; ++i) { t->parent[i] = i; t->kind[i] = -1; }
    for (int i = 0; i < s->p->functions && !s->failed; ++i) {
        type_expression(s, t, i, s->p->fn[i].body);
        unify(s, t, local_type_id(i, CB_MAX_LOCALS), expr_id(s, s->p->fn[i].body));
    }
    for (int i = 0; i < s->p->nodes; ++i)
        s->p->pool[i].type = t->kind[root(t, i)] == CB_LIST ? CB_LIST : CB_U64;
    for (int i = 0; i < s->p->functions; ++i) {
        CbFunction *f = &s->p->fn[i];
        f->return_type = f->body->type;
        for (int j = 0; j < f->locals; ++j)
            f->local_types[j] = t->kind[root(t, local_type_id(i, j))] == CB_LIST ? CB_LIST : CB_U64;
    }
    free(t);
}

int cb_parse_text(const char *text, CbProgram *p, char *error, size_t error_size) {
    memset(p, 0, sizeof(*p));
    p->entry = -1;
    Parser s = {.cursor = text, .line = 1, .p = p, .error = error, .error_size = error_size};
    next(&s);
    expect(&s, "("); expect(&s, "program");
    while (!s.failed && strcmp(s.token, ")")) {
        if (p->functions == CB_MAX_FUNCTIONS) { fail(&s, "too many functions"); break; }
        expect(&s, "("); expect(&s, "fn");
        if (s.failed) break;
        s.fn = &p->fn[p->functions++];
        if (!identifier(s.token)) { fail(&s, "invalid function name"); break; }
        strcpy(s.fn->name, s.token);
        for (int i = 0; i < p->functions - 1; ++i)
            if (!strcmp(p->fn[i].name, s.token)) fail(&s, "duplicate function '%s'", s.token);
        if (!strcmp(s.token, "main")) p->entry = p->functions - 1;
        next(&s);
        expect(&s, "(");
        s.bindings = 0;
        while (!s.failed && strcmp(s.token, ")")) {
            if (s.fn->arity == CB_MAX_PARAMS) { fail(&s, "at most four parameters"); break; }
            for (int i = 0; i < s.bindings; ++i)
                if (!strcmp(s.names[i], s.token)) fail(&s, "duplicate parameter '%s'", s.token);
            bind(&s, s.token); ++s.fn->arity; next(&s);
        }
        expect(&s, ")");
        s.fn->body = expression(&s);
        expect(&s, ")");
    }
    expect(&s, ")");
    if (!s.failed && *s.token) fail(&s, "unexpected trailing input");
    if (!s.failed && (p->entry < 0 || p->fn[p->entry].arity))
        fail(&s, "a zero-argument main function is required");
    for (int i = 0; !s.failed && i < p->nodes; ++i) {
        CbExpr *e = &p->pool[i];
        if (e->op != CB_CALL) continue;
        e->index = -1;
        for (int j = 0; j < p->functions; ++j)
            if (!strcmp(e->name, p->fn[j].name)) { e->index = j; break; }
        if (e->index < 0) fail(&s, "unknown function '%s'", e->name);
        else if (e->argc != p->fn[e->index].arity) fail(&s, "wrong arity for '%s'", e->name);
    }
    if (!s.failed) infer_types(&s);
    return !s.failed;
}

int cb_parse_file(const char *path, CbProgram *p, char *error, size_t error_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(error, error_size, "cannot open %s: %s", path, strerror(errno)); return 0; }
    char *text = malloc(1024 * 1024 + 1);
    if (!text) { fclose(f); snprintf(error, error_size, "out of memory"); return 0; }
    size_t n = fread(text, 1, 1024 * 1024 + 1, f);
    int ok = !ferror(f) && n <= 1024 * 1024;
    fclose(f);
    if (ok) { text[n] = 0; ok = cb_parse_text(text, p, error, error_size); }
    else snprintf(error, error_size, "AST exceeds 1 MiB or could not be read");
    free(text);
    return ok;
}

typedef struct {
    const CbProgram *p;
    const uint64_t *xs;
    uint64_t n, steps;
    const CbList *input;
    struct ListBlock *blocks;
    uint64_t allocated;
    int failed;
} Eval;

typedef struct ListBlock {
    struct ListBlock *next;
    size_t used;
    CbList nodes[1024];
} ListBlock;

static const CbList *cons(Eval *s, uint64_t head, const CbList *tail) {
    if (s->failed || s->allocated == CB_MAX_LIST_NODES) { s->failed = 1; return NULL; }
    if (!s->blocks || s->blocks->used == 1024) {
        ListBlock *b = malloc(sizeof(*b));
        if (!b) { s->failed = 1; return NULL; }
        b->next = s->blocks; b->used = 0; s->blocks = b;
    }
    CbList *cell = &s->blocks->nodes[s->blocks->used++];
    ++s->allocated;
    cell->head = head; cell->tail = tail;
    return cell;
}

static uint64_t evaluate(Eval *s, const CbExpr *e, uint64_t *frame, unsigned depth) {
    uint64_t local[CB_MAX_LOCALS];
    if (depth > 256) { s->failed = 1; return 0; }
    for (;;) {
        if (s->failed || !s->steps) { s->failed = 1; return 0; }
        --s->steps;
        switch (e->op) {
        case CB_CONST: return e->value;
        case CB_VAR: return frame[e->index];
        case CB_LEN: return s->n;
        case CB_INPUT: return (uint64_t)(uintptr_t)s->input;
        case CB_NIL: return 0;
        case CB_CONS: {
            uint64_t head = evaluate(s, e->arg[0], frame, depth + 1);
            const CbList *tail = (const CbList *)(uintptr_t)evaluate(s, e->arg[1], frame, depth + 1);
            return (uint64_t)(uintptr_t)cons(s, head, tail);
        }
        case CB_HEAD: case CB_TAIL: case CB_IS_NIL: {
            const CbList *list = (const CbList *)(uintptr_t)evaluate(s, e->arg[0], frame, depth + 1);
            if (e->op == CB_IS_NIL) return list == NULL;
            if (e->op == CB_HEAD) return list ? list->head : 0;
            return (uint64_t)(uintptr_t)(list ? list->tail : NULL);
        }
        case CB_GET: {
            uint64_t i = evaluate(s, e->arg[0], frame, depth + 1);
            return i < s->n ? s->xs[i] : 0;
        }
        case CB_IF:
            e = e->arg[evaluate(s, e->arg[0], frame, depth + 1) ? 1 : 2];
            continue;
        case CB_LET:
            frame[e->index] = evaluate(s, e->arg[0], frame, depth + 1);
            e = e->arg[1];
            continue;
        case CB_CALL: {
            uint64_t args[CB_MAX_PARAMS];
            for (int i = 0; i < e->argc; ++i)
                args[i] = evaluate(s, e->arg[i], frame, depth + 1);
            for (int i = 0; i < e->argc; ++i) local[i] = args[i];
            frame = local;
            e = s->p->fn[e->index].body;
            continue;
        }
        default: break;
        }
        uint64_t a = evaluate(s, e->arg[0], frame, depth + 1);
        uint64_t b = evaluate(s, e->arg[1], frame, depth + 1);
        switch (e->op) {
        case CB_ADD: return a + b;
        case CB_SUB: return a - b;
        case CB_MUL: return a * b;
        case CB_DIV: return b ? a / b : 0;
        case CB_MOD: return b ? a % b : 0;
        case CB_AND: return a & b;
        case CB_OR: return a | b;
        case CB_XOR: return a ^ b;
        case CB_SHL: return a << (b & 63);
        case CB_SHR: return a >> (b & 63);
        case CB_EQ: return a == b;
        case CB_NE: return a != b;
        case CB_LT: return a < b;
        case CB_LE: return a <= b;
        case CB_GT: return a > b;
        case CB_GE: return a >= b;
        default: s->failed = 1; return 0;
        }
    }
}

int cb_eval(const CbProgram *p, const uint64_t *xs, uint64_t n,
            uint64_t budget, uint64_t *result, char *error, size_t error_size) {
    if (p->fn[p->entry].return_type != CB_U64) {
        snprintf(error, error_size, "cb_eval requires a u64 result; use cb_eval_result"); return 0;
    }
    CbResult value;
    int ok = cb_eval_result(p, xs, n, budget, &value, error, error_size);
    *result = value.scalar;
    cb_result_free(&value);
    return ok;
}

int cb_eval_result(const CbProgram *p, const uint64_t *xs, uint64_t n,
                   uint64_t budget, CbResult *result, char *error, size_t error_size) {
    memset(result, 0, sizeof(*result));
    result->type = p->fn[p->entry].return_type;
    Eval s = {.p = p, .xs = xs, .n = n, .steps = budget};
    CbList *input = NULL;
    if (n) {
        if (n > CB_MAX_LIST_NODES || !(input = malloc((size_t)n * sizeof(*input)))) {
            snprintf(error, error_size, "reference input list allocation failed"); return 0;
        }
        for (uint64_t i = 0; i < n; ++i) {
            input[i].head = xs[i]; input[i].tail = i + 1 < n ? &input[i + 1] : NULL;
        }
        s.input = input;
    }
    uint64_t frame[CB_MAX_LOCALS] = {0};
    uint64_t value = evaluate(&s, p->fn[p->entry].body, frame, 0);
    if (!s.failed && result->type == CB_U64) result->scalar = value;
    if (!s.failed && result->type == CB_LIST) {
        const CbList *list = (const CbList *)(uintptr_t)value;
        for (const CbList *cell = list; cell; cell = cell->tail) ++result->n;
        if (result->n) {
            result->xs = malloc((size_t)result->n * sizeof(uint64_t));
            if (!result->xs) s.failed = 1;
            else for (uint64_t i = 0; i < result->n; ++i, list = list->tail) result->xs[i] = list->head;
        }
    }
    while (s.blocks) { ListBlock *next_block = s.blocks->next; free(s.blocks); s.blocks = next_block; }
    free(input);
    if (s.failed) {
        cb_result_free(result);
        snprintf(error, error_size, "reference step/depth/list-allocation limit exceeded");
    }
    return !s.failed;
}

void cb_result_free(CbResult *result) { free(result->xs); result->xs = NULL; result->n = 0; }
