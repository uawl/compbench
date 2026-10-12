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
    if (!identifier(name) || !strcmp(name, "len")) {
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
        } else if (!strcmp(op, "get")) {
            e = node(s, CB_GET);
            if (e) e->arg[0] = expression(s);
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
    int failed;
} Eval;

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
    Eval s = {.p = p, .xs = xs, .n = n, .steps = budget};
    uint64_t frame[CB_MAX_LOCALS] = {0};
    *result = evaluate(&s, p->fn[p->entry].body, frame, 0);
    if (s.failed) snprintf(error, error_size, "reference step/depth limit exceeded");
    return !s.failed;
}
