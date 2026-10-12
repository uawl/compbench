#include "ast.h"

#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Optimizing code generator.
 *
 *  1. AST -> immutable expression trees (hash-consed metadata, unique let ids)
 *  2. tree optimizations: constant folding, algebraic simplification,
 *     let propagation, inlining of non-recursive functions, accumulator
 *     introduction for associative non-tail recursion
 *  3. lowering to a linear virtual-register IR (self tail calls become loops,
 *     CSE, loop-invariant hoisting, bounds-check elimination from guards,
 *     cmov selection, constant division by multiplication)
 *  4. liveness + interference graph + priority-based coloring
 *  5. x86-64 emission (internal calling convention: xs in %r14, n in %r15,
 *     args in %rdi,%rsi,%r8,%r9, result in %rax)
 *  6. AVX2 pre-loops for vectorizable reduction loops
 */

typedef uint64_t u64;
typedef int64_t i64;
__extension__ typedef unsigned __int128 u128;

static jmp_buf g_fail;
static char g_errmsg[256];

static void die(const char *m) {
    snprintf(g_errmsg, sizeof g_errmsg, "%s", m);
    longjmp(g_fail, 1);
}

/* ------------------------------------------------------------------ */
/* arena                                                              */
/* ------------------------------------------------------------------ */
typedef struct Arena {
    struct Arena *next;
    size_t used, cap;
    unsigned char *mem;
} Arena;
static Arena *g_arena;

static void *aalloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!g_arena || g_arena->cap - g_arena->used < n) {
        size_t cap = n > ((size_t)1 << 20) ? n : ((size_t)1 << 20);
        Arena *a = (Arena *)malloc(sizeof(Arena) + 32 + cap);
        if (!a) die("out of memory");
        a->next = g_arena;
        a->used = 0;
        a->cap = cap;
        a->mem = (unsigned char *)(((uintptr_t)(a + 1) + 15) & ~(uintptr_t)15);
        g_arena = a;
    }
    void *p = g_arena->mem + g_arena->used;
    g_arena->used += n;
    memset(p, 0, n);
    return p;
}

static void *agrow(void *p, size_t old, size_t nw) {
    void *q = aalloc(nw);
    if (old) memcpy(q, p, old);
    return q;
}

static void arena_free(void) {
    while (g_arena) {
        Arena *n = g_arena->next;
        free(g_arena);
        g_arena = n;
    }
}

/* ------------------------------------------------------------------ */
/* expression trees                                                   */
/* ------------------------------------------------------------------ */
typedef struct N {
    int op, var, fn, argc; /* argc = number of children */
    int hc, sz;            /* contains call, subtree size */
    u64 val, h;
    struct N *a[4];
} N;

static int is_cmp(int op) { return op >= CB_EQ && op <= CB_GE; }
static int is_bin(int op) { return op >= CB_ADD && op <= CB_GE; }
static int is_comm(int op) {
    return op == CB_ADD || op == CB_MUL || op == CB_AND || op == CB_OR || op == CB_XOR ||
           op == CB_EQ || op == CB_NE;
}
static int mirror_cmp(int op) {
    switch (op) {
    case CB_LT: return CB_GT;
    case CB_GT: return CB_LT;
    case CB_LE: return CB_GE;
    case CB_GE: return CB_LE;
    default: return op;
    }
}
static int inv_cmp(int op) {
    switch (op) {
    case CB_EQ: return CB_NE;
    case CB_NE: return CB_EQ;
    case CB_LT: return CB_GE;
    case CB_GE: return CB_LT;
    case CB_LE: return CB_GT;
    default: return CB_LE;
    }
}
static int fits32(u64 v) { return (i64)v == (i64)(int32_t)v; }
static int ispow2(u64 v) { return v && !(v & (v - 1)); }
static int ctz64(u64 v) { return __builtin_ctzll(v); }

static u64 fold(int op, u64 a, u64 b) {
    switch (op) {
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
    default: return 0;
    }
}

static N *fin(N *e) {
    u64 h = (u64)(e->op + 1) * 0x9E3779B97F4A7C15ull;
    int hc = e->op == CB_CALL, sz = 1;
    if (e->op == CB_CONST) h ^= e->val * 0xC2B2AE3D27D4EB4Full + (e->val >> 29);
    if (e->op == CB_VAR || e->op == CB_LET) h ^= (u64)(e->var + 1) * 0x165667B19E3779F9ull;
    if (e->op == CB_CALL) h ^= (u64)(e->fn + 7) * 0x27D4EB2F165667C5ull;
    for (int i = 0; i < e->argc; ++i) {
        h = (h ^ e->a[i]->h) * 0x100000001B3ull + (u64)i * 0x9E37ull;
        hc |= e->a[i]->hc;
        sz += e->a[i]->sz;
    }
    h ^= h >> 31;
    e->h = h;
    e->hc = hc;
    e->sz = sz;
    return e;
}

static N *mkn(int op) {
    N *e = (N *)aalloc(sizeof(N));
    e->op = op;
    return e;
}
static N *mkc(u64 v) { N *e = mkn(CB_CONST); e->val = v; return fin(e); }
static N *mkv(int v) { N *e = mkn(CB_VAR); e->var = v; return fin(e); }
static N *mklen(void) { return fin(mkn(CB_LEN)); }
static N *mk1(int op, N *a) { N *e = mkn(op); e->argc = 1; e->a[0] = a; return fin(e); }
static N *mk2(int op, N *a, N *b) {
    N *e = mkn(op);
    e->argc = 2;
    e->a[0] = a;
    e->a[1] = b;
    return fin(e);
}
static N *mk3(int op, N *a, N *b, N *c) {
    N *e = mkn(op);
    e->argc = 3;
    e->a[0] = a;
    e->a[1] = b;
    e->a[2] = c;
    return fin(e);
}
static N *mklet(int v, N *val, N *body) {
    N *e = mk2(CB_LET, val, body);
    e->var = v;
    return fin(e);
}
static N *mkcall(int fn, int argc, N **args) {
    N *e = mkn(CB_CALL);
    e->fn = fn;
    e->argc = argc;
    for (int i = 0; i < argc; ++i) e->a[i] = args[i];
    return fin(e);
}
static N *clone_kids(const N *e, N **kids) {
    N *n = mkn(e->op);
    *n = *e;
    for (int i = 0; i < e->argc; ++i) n->a[i] = kids[i];
    return fin(n);
}

static int teq(const N *a, const N *b) {
    if (a == b) return 1;
    if (a->h != b->h || a->op != b->op || a->argc != b->argc || a->val != b->val ||
        a->var != b->var || a->fn != b->fn)
        return 0;
    for (int i = 0; i < a->argc; ++i)
        if (!teq(a->a[i], b->a[i])) return 0;
    return 1;
}

static int uses(const N *e, int v) {
    if (e->op == CB_VAR) return e->var == v;
    int c = 0;
    for (int i = 0; i < e->argc; ++i) c += uses(e->a[i], v);
    return c;
}

static int isbool(const N *e) {
    if (is_cmp(e->op)) return 1;
    if (e->op == CB_CONST) return e->val <= 1;
    if (e->op == CB_AND || e->op == CB_OR || e->op == CB_XOR)
        return isbool(e->a[0]) && isbool(e->a[1]);
    if (e->op == CB_IF) return isbool(e->a[1]) && isbool(e->a[2]);
    if (e->op == CB_LET) return isbool(e->a[1]);
    return 0;
}

/* ------------------------------------------------------------------ */
/* functions                                                          */
/* ------------------------------------------------------------------ */
#define MAXF 200
typedef struct {
    int arity, nvars;
    N *body;
    int rec, live, accdone;
} Fn;
static Fn F[MAXF];
static int nF;
static int g_entry;

/* ------------------------------------------------------------------ */
/* simplifier                                                         */
/* ------------------------------------------------------------------ */
static N *local(N *e, N **k);
static N *simp(N *e);

static N *subst(N *e, int v, N *r) {
    if (e->op == CB_VAR) return e->var == v ? r : e;
    if (!e->argc) return e;
    N *k[4];
    int ch = 0;
    for (int i = 0; i < e->argc; ++i) {
        k[i] = subst(e->a[i], v, r);
        ch |= k[i] != e->a[i];
    }
    if (!ch) return e;
    return local(e, k);
}

static N *simp_bin(N *e, int op, N *l, N *r) {
    if (l->op == CB_CONST && r->op == CB_CONST) return mkc(fold(op, l->val, r->val));
    if (l->op == CB_CONST) {
        if (is_comm(op)) { N *t = l; l = r; r = t; }
        else if (is_cmp(op)) { N *t = l; l = r; r = t; op = mirror_cmp(op); }
    }
    if (r->op == CB_CONST) {
        u64 c = r->val;
        switch (op) {
        case CB_SUB:
            if (c == 0) return l;
            return simp_bin(NULL, CB_ADD, l, mkc(0 - c));
        case CB_ADD:
            if (c == 0) return l;
            if (l->op == CB_ADD && l->a[1]->op == CB_CONST)
                return simp_bin(NULL, CB_ADD, l->a[0], mkc(l->a[1]->val + c));
            break;
        case CB_MUL:
            if (c == 0 && !l->hc) return mkc(0);
            if (c == 1) return l;
            if (ispow2(c)) return simp_bin(NULL, CB_SHL, l, mkc((u64)ctz64(c)));
            if (l->op == CB_MUL && l->a[1]->op == CB_CONST)
                return simp_bin(NULL, CB_MUL, l->a[0], mkc(l->a[1]->val * c));
            break;
        case CB_DIV:
            if (c == 0 && !l->hc) return mkc(0);
            if (c == 1) return l;
            if (ispow2(c)) return simp_bin(NULL, CB_SHR, l, mkc((u64)ctz64(c)));
            break;
        case CB_MOD:
            if ((c == 0 || c == 1) && !l->hc) return mkc(0);
            if (ispow2(c)) return simp_bin(NULL, CB_AND, l, mkc(c - 1));
            break;
        case CB_AND:
            if (c == 0 && !l->hc) return mkc(0);
            if (c == ~0ull) return l;
            if (c == 1 && isbool(l)) return l;
            if (l->op == CB_AND && l->a[1]->op == CB_CONST)
                return simp_bin(NULL, CB_AND, l->a[0], mkc(l->a[1]->val & c));
            break;
        case CB_OR:
            if (c == 0) return l;
            if (c == ~0ull && !l->hc) return mkc(~0ull);
            break;
        case CB_XOR:
            if (c == 0) return l;
            if (c == 1 && is_cmp(l->op)) return simp_bin(NULL, inv_cmp(l->op), l->a[0], l->a[1]);
            break;
        case CB_SHL:
        case CB_SHR:
            c &= 63;
            if (c == 0) return l;
            if (c != r->val) r = mkc(c);
            if (l->op == op && l->a[1]->op == CB_CONST && (l->a[1]->val & 63) + c < 64)
                return simp_bin(NULL, op, l->a[0], mkc((l->a[1]->val & 63) + c));
            break;
        case CB_EQ:
            if (c == 0 && is_cmp(l->op)) return simp_bin(NULL, inv_cmp(l->op), l->a[0], l->a[1]);
            if (c == 1 && isbool(l)) return l;
            break;
        case CB_NE:
            if (c == 0 && isbool(l)) return l;
            if (c == 1 && is_cmp(l->op)) return simp_bin(NULL, inv_cmp(l->op), l->a[0], l->a[1]);
            break;
        case CB_LT:
            if (c == 0 && !l->hc) return mkc(0);
            if (c == 1) return simp_bin(NULL, CB_EQ, l, mkc(0));
            break;
        case CB_GE:
            if (c == 0 && !l->hc) return mkc(1);
            if (c == 1) return simp_bin(NULL, CB_NE, l, mkc(0));
            break;
        case CB_LE:
            if (c == ~0ull && !l->hc) return mkc(1);
            if (c == 0) return simp_bin(NULL, CB_EQ, l, r);
            break;
        case CB_GT:
            if (c == ~0ull && !l->hc) return mkc(0);
            if (c == 0) return simp_bin(NULL, CB_NE, l, r);
            break;
        default: break;
        }
    }
    if (!l->hc && teq(l, r)) {
        switch (op) {
        case CB_SUB: case CB_XOR: case CB_MOD: return mkc(0);
        case CB_AND: case CB_OR: return l;
        case CB_EQ: case CB_LE: case CB_GE: return mkc(1);
        case CB_NE: case CB_LT: case CB_GT: return mkc(0);
        case CB_DIV: return simp_bin(NULL, CB_NE, l, mkc(0));
        default: break;
        }
    }
    if (e && e->op == op && e->a[0] == l && e->a[1] == r) return e;
    return mk2(op, l, r);
}

static N *simp_if(N *e, N *c, N *t, N *f) {
    for (;;) {
        if (c->op == CB_CONST) return c->val ? t : f;
        if (c->op == CB_NE && c->a[1]->op == CB_CONST && c->a[1]->val == 0) { c = c->a[0]; continue; }
        if (c->op == CB_EQ && c->a[1]->op == CB_CONST && c->a[1]->val == 0) {
            N *x = t; t = f; f = x;
            c = c->a[0];
            continue;
        }
        break;
    }
    if (t->op == CB_IF && teq(t->a[0], c)) t = t->a[1];
    if (f->op == CB_IF && teq(f->a[0], c)) f = f->a[2];
    if (!c->hc && teq(t, f)) return t;
    if (t->op == CB_CONST && f->op == CB_CONST) {
        if (t->val == 1 && f->val == 0 && isbool(c)) return c;
        if (t->val == 0 && f->val == 1 && is_cmp(c->op)) return simp_bin(NULL, inv_cmp(c->op), c->a[0], c->a[1]);
    }
    if (e && e->a[0] == c && e->a[1] == t && e->a[2] == f) return e;
    return mk3(CB_IF, c, t, f);
}

static N *simp_let(N *e, int v, N *val, N *body) {
    if (val->op == CB_CONST || val->op == CB_VAR || val->op == CB_LEN) return subst(body, v, val);
    int u = uses(body, v);
    if (u == 0 && !val->hc) return body;
    if (u == 1) return subst(body, v, val);
    if (e && e->a[0] == val && e->a[1] == body) return e;
    return mklet(v, val, body);
}

/* apply local rules to e with (already simplified) children k */
static N *local(N *e, N **k) {
    switch (e->op) {
    case CB_IF: return simp_if(e, k[0], k[1], k[2]);
    case CB_LET: return simp_let(e, e->var, k[0], k[1]);
    case CB_GET:
    case CB_CALL: {
        int ch = 0;
        for (int i = 0; i < e->argc; ++i) ch |= k[i] != e->a[i];
        return ch ? clone_kids(e, k) : e;
    }
    default:
        if (is_bin(e->op)) return simp_bin(e, e->op, k[0], k[1]);
        return e;
    }
}

static N *simp(N *e) {
    if (!e->argc) return e;
    N *k[4];
    for (int i = 0; i < e->argc; ++i) k[i] = simp(e->a[i]);
    return local(e, k);
}

/* ------------------------------------------------------------------ */
/* AST conversion                                                     */
/* ------------------------------------------------------------------ */
static const CbProgram *g_prog;

static N *conv(const CbExpr *e, const CbFunction *fn, int depth) {
    if (!e) die("null expression");
    if (depth > 100000) die("expression too deep");
    if ((unsigned)e->op > (unsigned)CB_GE) die("bad opcode");
    switch (e->op) {
    case CB_CONST: return mkc(e->value);
    case CB_VAR:
        if (e->index < 0 || e->index >= fn->locals) die("bad variable slot");
        return mkv(e->index);
    case CB_LEN: return mklen();
    case CB_GET: return mk1(CB_GET, conv(e->arg[0], fn, depth + 1));
    case CB_IF:
        return mk3(CB_IF, conv(e->arg[0], fn, depth + 1), conv(e->arg[1], fn, depth + 1),
                   conv(e->arg[2], fn, depth + 1));
    case CB_LET:
        if (e->index < 0 || e->index >= fn->locals) die("bad let slot");
        return mklet(e->index, conv(e->arg[0], fn, depth + 1), conv(e->arg[1], fn, depth + 1));
    case CB_CALL: {
        if (e->index < 0 || e->index >= g_prog->functions) die("bad call target");
        if (e->argc != g_prog->fn[e->index].arity || e->argc < 0 || e->argc > CB_MAX_PARAMS)
            die("bad call arity");
        N *k[4];
        for (int i = 0; i < e->argc; ++i) k[i] = conv(e->arg[i], fn, depth + 1);
        return mkcall(e->index, e->argc, k);
    }
    default:
        return mk2(e->op, conv(e->arg[0], fn, depth + 1), conv(e->arg[1], fn, depth + 1));
    }
}

/* ------------------------------------------------------------------ */
/* call graph                                                         */
/* ------------------------------------------------------------------ */
static unsigned char g_reach[MAXF][MAXF];
static int g_ncalls[MAXF];

static void mark_calls(const N *e, unsigned char *row) {
    if (e->op == CB_CALL) row[e->fn] = 1;
    for (int i = 0; i < e->argc; ++i) mark_calls(e->a[i], row);
}
static void count_calls(const N *e) {
    if (e->op == CB_CALL) g_ncalls[e->fn]++;
    for (int i = 0; i < e->argc; ++i) count_calls(e->a[i]);
}

static void analyze_calls(void) {
    memset(g_reach, 0, sizeof g_reach);
    for (int i = 0; i < nF; ++i) mark_calls(F[i].body, g_reach[i]);
    for (int k = 0; k < nF; ++k)
        for (int i = 0; i < nF; ++i)
            if (g_reach[i][k])
                for (int j = 0; j < nF; ++j)
                    if (g_reach[k][j]) g_reach[i][j] = 1;
    for (int i = 0; i < nF; ++i) {
        F[i].rec = g_reach[i][i];
        F[i].live = (i == g_entry) || g_reach[g_entry][i];
    }
    memset(g_ncalls, 0, sizeof g_ncalls);
    for (int i = 0; i < nF; ++i)
        if (F[i].live) count_calls(F[i].body);
}

/* ------------------------------------------------------------------ */
/* inlining                                                           */
/* ------------------------------------------------------------------ */
static N *rename_vars(const N *e, const int *map) {
    N *k[4];
    if (e->op == CB_VAR) return mkv(map[e->var]);
    if (!e->argc) return (N *)e;
    for (int i = 0; i < e->argc; ++i) k[i] = rename_vars(e->a[i], map);
    N *n = clone_kids(e, k);
    if (e->op == CB_LET) {
        n->var = map[e->var];
        fin(n);
    }
    return n;
}

static N *inline_call(int caller, const N *call) {
    const Fn *g = &F[call->fn];
    int base = F[caller].nvars;
    F[caller].nvars += g->nvars;
    int *map = (int *)aalloc(sizeof(int) * (size_t)(g->nvars + 1));
    for (int v = 0; v < g->nvars; ++v) map[v] = base + v;
    N *body = rename_vars(g->body, map);
    for (int j = g->arity - 1; j >= 0; --j) body = mklet(base + j, call->a[j], body);
    return body;
}

static int g_inl_budget;

static N *inl(N *e, int caller, int *changed) {
    if (!e->argc) return e;
    N *k[4];
    int ch = 0;
    for (int i = 0; i < e->argc; ++i) {
        k[i] = inl(e->a[i], caller, changed);
        ch |= k[i] != e->a[i];
    }
    N *n = ch ? clone_kids(e, k) : e;
    if (n->op == CB_CALL) {
        int g = n->fn;
        if (g != caller && !F[g].rec && F[g].live) {
            int gsz = F[g].body->sz;
            if ((gsz <= 60 || g_ncalls[g] == 1) && gsz <= g_inl_budget) {
                g_inl_budget -= gsz;
                *changed = 1;
                return inline_call(caller, n);
            }
        }
    }
    return n;
}

static void inline_all(void) {
    for (int pass = 0; pass < 12; ++pass) {
        analyze_calls();
        int any = 0;
        for (int f = 0; f < nF; ++f) {
            if (!F[f].live) continue;
            int changed = 0;
            g_inl_budget = 4000 - F[f].body->sz;
            if (g_inl_budget <= 0) continue;
            N *b = inl(F[f].body, f, &changed);
            if (changed) {
                F[f].body = simp(b);
                any = 1;
            }
        }
        if (!any) break;
    }
    analyze_calls();
}

/* ------------------------------------------------------------------ */
/* accumulator introduction                                           */
/*   f(x) = ... A op f(y) ...   ==>  f(x) = g(x, id)                 */
/*   g(x, acc) = ... g(y, acc op A) ... base -> acc op base           */
/* ------------------------------------------------------------------ */
static int is_selfcall(const N *e, int f) { return e->op == CB_CALL && e->fn == f; }

static int spine_op(const N *e, int f) {
    switch (e->op) {
    case CB_ADD: case CB_MUL: case CB_AND: case CB_OR: case CB_XOR:
        if (is_selfcall(e->a[1], f) || is_selfcall(e->a[0], f)) return e->op;
        return 0;
    case CB_SUB:
        if (is_selfcall(e->a[0], f) && !is_selfcall(e->a[1], f)) return CB_ADD;
        return 0;
    default: return 0;
    }
}

static void acc_scan(const N *e, int f, int *cnt) {
    switch (e->op) {
    case CB_IF: acc_scan(e->a[1], f, cnt); acc_scan(e->a[2], f, cnt); return;
    case CB_LET: acc_scan(e->a[1], f, cnt); return;
    default: {
        int op = spine_op(e, f);
        if (op) cnt[op]++;
    }
    }
}

static u64 op_identity(int op) {
    return op == CB_MUL ? 1 : op == CB_AND ? ~0ull : 0;
}

typedef struct {
    int f, g, op, acc;
    int *map;
} AccCtx;

static N *acc_redirect(const AccCtx *c, const N *e) {
    N *k[4];
    if (e->op == CB_VAR) return mkv(c->map[e->var]);
    if (!e->argc) return (N *)e;
    for (int i = 0; i < e->argc; ++i) k[i] = acc_redirect(c, e->a[i]);
    if (e->op == CB_CALL && e->fn == c->f) {
        k[e->argc] = mkc(op_identity(c->op));
        return mkcall(c->g, e->argc + 1, k);
    }
    N *n = clone_kids(e, k);
    if (e->op == CB_LET) {
        n->var = c->map[e->var];
        fin(n);
    }
    return n;
}

static N *acc_call(const AccCtx *c, const N *call, N *accv) {
    N *k[4];
    for (int i = 0; i < call->argc; ++i) k[i] = acc_redirect(c, call->a[i]);
    k[call->argc] = accv;
    return mkcall(c->g, call->argc + 1, k);
}

static N *acc_rewrite(const AccCtx *c, const N *e) {
    switch (e->op) {
    case CB_IF:
        return mk3(CB_IF, acc_redirect(c, e->a[0]), acc_rewrite(c, e->a[1]), acc_rewrite(c, e->a[2]));
    case CB_LET:
        return mklet(c->map[e->var], acc_redirect(c, e->a[0]), acc_rewrite(c, e->a[1]));
    case CB_CALL:
        if (e->fn == c->f) return acc_call(c, e, mkv(c->acc));
        break;
    default: {
        int op = spine_op(e, c->f);
        if (op == c->op) {
            const N *call, *other;
            if (e->op == CB_SUB) {
                call = e->a[0];
                N *neg = mk2(CB_SUB, mkc(0), acc_redirect(c, e->a[1]));
                return acc_call(c, call, mk2(CB_ADD, mkv(c->acc), neg));
            }
            if (is_selfcall(e->a[1], c->f)) { call = e->a[1]; other = e->a[0]; }
            else { call = e->a[0]; other = e->a[1]; }
            return acc_call(c, call, mk2(c->op, mkv(c->acc), acc_redirect(c, other)));
        }
    }
    }
    return mk2(c->op, mkv(c->acc), acc_redirect(c, e));
}

static void acc_intro(void) {
    analyze_calls();
    int n0 = nF;
    for (int f = 0; f < n0; ++f) {
        if (!F[f].live || !F[f].rec || F[f].accdone || F[f].arity >= CB_MAX_PARAMS) continue;
        if (nF >= MAXF) break;
        int cnt[CB_GE + 1];
        memset(cnt, 0, sizeof cnt);
        acc_scan(F[f].body, f, cnt);
        int best = 0;
        for (int op = CB_ADD; op <= CB_XOR; ++op)
            if (cnt[op] > (best ? cnt[best] : 0)) best = op;
        if (!best) continue;
        int g = nF++;
        AccCtx c;
        c.f = f;
        c.g = g;
        c.op = best;
        c.acc = F[f].arity;
        c.map = (int *)aalloc(sizeof(int) * (size_t)(F[f].nvars + 1));
        for (int v = 0; v < F[f].nvars; ++v) c.map[v] = v < F[f].arity ? v : v + 1;
        F[g].arity = F[f].arity + 1;
        F[g].nvars = F[f].nvars + 1;
        F[g].accdone = 1;
        F[g].body = simp(acc_rewrite(&c, F[f].body));
        N *k[4];
        for (int j = 0; j < F[f].arity; ++j) k[j] = mkv(j);
        k[F[f].arity] = mkc(op_identity(best));
        F[f].body = mkcall(g, F[f].arity + 1, k);
        F[f].accdone = 1;
    }
    analyze_calls();
}

/* ------------------------------------------------------------------ */
/* linear IR                                                          */
/* ------------------------------------------------------------------ */
enum {
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15
};
static const char *R64[16] = {"%rax", "%rcx", "%rdx", "%rbx", "%rsp", "%rbp", "%rsi", "%rdi",
                              "%r8", "%r9", "%r10", "%r11", "%r12", "%r13", "%r14", "%r15"};
static const char *R32[16] = {"%eax", "%ecx", "%edx", "%ebx", "%esp", "%ebp", "%esi", "%edi",
                              "%r8d", "%r9d", "%r10d", "%r11d", "%r12d", "%r13d", "%r14d", "%r15d"};
static const char *R8N[16] = {"%al", "%cl", "%dl", "%bl", "%spl", "%bpl", "%sil", "%dil",
                              "%r8b", "%r9b", "%r10b", "%r11b", "%r12b", "%r13b", "%r14b", "%r15b"};
static const int ARGREG[4] = {RDI, RSI, R8, R9};
static const int ALLOC_ORDER[10] = {R10, R11, RDI, RSI, R8, R9, RBX, RBP, R12, R13};
static const int CALLER_SAVED[6] = {RSI, RDI, R8, R9, R10, R11};
static int tracked_phys(int r) { return r == RBX || r == RBP || (r >= RSI && r <= R13); }
static int callee_saved(int r) { return r == RBX || r == RBP || r == R12 || r == R13; }

enum {
    I_MOV, I_LI, I_BIN, I_CMP, I_GET, I_GETU, I_JCC, I_JMP, I_LAB, I_CALL, I_TAIL, I_RET,
    I_SEL, I_DIVC, I_MODC
};

typedef struct {
    int op, cc;
    int d, s1, s2, x, y; /* s2 < 0: immediate operand */
    u64 imm;
    int lab, argc, w;
} In;

typedef struct {
    int isimm, v;
    u64 k;
} Op;

static Op OPR(int v) { Op o = {0, v, 0}; return o; }
static Op OPI(u64 k) { Op o = {1, -1, k}; return o; }

static In *code;
static int ncode, capcode;
static int nvr, nlab;
static int L_f, L_loop, L_arity, L_w;
static int L_pv[4];
static Op *varop;

static N **fact_a, **fact_b;
static int nfact, capfact;
static N **memo_e;
static Op *memo_o;
static int nmemo, capmemo;

static In *emit(int op) {
    if (ncode == capcode) {
        int nc = capcode ? capcode * 2 : 1024;
        code = (In *)agrow(code, sizeof(In) * (size_t)capcode, sizeof(In) * (size_t)nc);
        capcode = nc;
    }
    In *in = &code[ncode++];
    memset(in, 0, sizeof *in);
    in->op = op;
    in->d = in->s1 = in->s2 = in->x = in->y = -1;
    in->w = L_w;
    return in;
}
static int newv(void) { return nvr++; }
static int newl(void) { return nlab++; }

static void emit_mov(int d, Op o) {
    if (o.isimm) {
        In *in = emit(I_LI);
        in->d = d;
        in->imm = o.k;
    } else if (o.v != d) {
        In *in = emit(I_MOV);
        in->d = d;
        in->s1 = o.v;
    }
}
static int as_reg(Op o) {
    if (!o.isimm) return o.v;
    int d = newv();
    emit_mov(d, o);
    return d;
}
static void emit_lab(int l) { emit(I_LAB)->lab = l; }
static void emit_jmp(int l) { emit(I_JMP)->lab = l; }

static void push_fact(N *a, N *b) {
    if (nfact == capfact) {
        int nc = capfact ? capfact * 2 : 64;
        fact_a = (N **)agrow(fact_a, sizeof(N *) * (size_t)capfact, sizeof(N *) * (size_t)nc);
        fact_b = (N **)agrow(fact_b, sizeof(N *) * (size_t)capfact, sizeof(N *) * (size_t)nc);
        capfact = nc;
    }
    fact_a[nfact] = a;
    fact_b[nfact++] = b;
}
static void add_facts(N *c, int truth) {
    switch (c->op) {
    case CB_LT: if (truth) push_fact(c->a[0], c->a[1]); break;
    case CB_GT: if (truth) push_fact(c->a[1], c->a[0]); break;
    case CB_GE: if (!truth) push_fact(c->a[0], c->a[1]); break;
    case CB_LE: if (!truth) push_fact(c->a[1], c->a[0]); break;
    case CB_AND:
        if (truth && isbool(c->a[0]) && isbool(c->a[1])) { add_facts(c->a[0], 1); add_facts(c->a[1], 1); }
        break;
    case CB_OR:
        if (!truth && isbool(c->a[0]) && isbool(c->a[1])) { add_facts(c->a[0], 0); add_facts(c->a[1], 0); }
        break;
    default: break;
    }
}
static int known_lt_len(const N *x) {
    for (int i = nfact - 1; i >= 0; --i)
        if (fact_b[i]->op == CB_LEN && teq(fact_a[i], x)) return 1;
    return 0;
}

static int memo_find(const N *e, Op *o) {
    for (int i = nmemo - 1; i >= 0; --i)
        if (memo_e[i]->h == e->h && teq(memo_e[i], e)) { *o = memo_o[i]; return 1; }
    return 0;
}
static void memo_add(N *e, Op o) {
    if (nmemo == capmemo) {
        int nc = capmemo ? capmemo * 2 : 256;
        memo_e = (N **)agrow(memo_e, sizeof(N *) * (size_t)capmemo, sizeof(N *) * (size_t)nc);
        memo_o = (Op *)agrow(memo_o, sizeof(Op) * (size_t)capmemo, sizeof(Op) * (size_t)nc);
        capmemo = nc;
    }
    memo_e[nmemo] = e;
    memo_o[nmemo++] = o;
}

static Op lower(N *e);

static Op lower_bin(int op, Op l, Op r) {
    if (l.isimm && r.isimm) return OPI(fold(op, l.k, r.k));
    if (l.isimm) {
        if (is_comm(op)) { Op t = l; l = r; r = t; }
        else if (is_cmp(op)) { Op t = l; l = r; r = t; op = mirror_cmp(op); }
    }
    if (r.isimm) {
        u64 k = r.k;
        switch (op) {
        case CB_DIV:
        case CB_MOD: {
            if (k == 0) return OPI(0);
            if (k == 1) return op == CB_DIV ? l : OPI(0);
            if (ispow2(k))
                return op == CB_DIV ? lower_bin(CB_SHR, l, OPI((u64)ctz64(k))) : lower_bin(CB_AND, l, OPI(k - 1));
            int d = newv();
            In *in = emit(op == CB_DIV ? I_DIVC : I_MODC);
            in->d = d;
            in->s1 = as_reg(l);
            in->imm = k;
            return OPR(d);
        }
        case CB_SUB:
            if (k == 0) return l;
            op = CB_ADD;
            k = 0 - k;
            r = OPI(k);
            break;
        case CB_MUL:
            if (k == 0) return OPI(0);
            if (k == 1) return l;
            if (ispow2(k)) return lower_bin(CB_SHL, l, OPI((u64)ctz64(k)));
            break;
        case CB_SHL:
        case CB_SHR:
            k &= 63;
            if (k == 0) return l;
            r = OPI(k);
            break;
        default: break;
        }
        if (op == CB_ADD && k == 0) return l;
        if (!fits32(r.k)) r = OPR(as_reg(r));
    }
    int s1 = as_reg(l);
    int d = newv();
    In *in = emit(is_cmp(op) ? I_CMP : I_BIN);
    in->cc = op;
    in->d = d;
    in->s1 = s1;
    if (r.isimm) in->imm = r.k;
    else in->s2 = r.v;
    return OPR(d);
}

/* operands of a condition: jump/select on (l cc r) */
static void cond_operands(N *c, int *cc, Op *l, Op *r) {
    if (is_cmp(c->op)) {
        *l = lower(c->a[0]);
        *r = lower(c->a[1]);
        *cc = c->op;
        if (l->isimm && !r->isimm) { Op t = *l; *l = *r; *r = t; *cc = mirror_cmp(*cc); }
        if (l->isimm) *l = OPR(as_reg(*l));
        if (r->isimm && !fits32(r->k)) *r = OPR(as_reg(*r));
    } else {
        *l = OPR(as_reg(lower(c)));
        *r = OPI(0);
        *cc = CB_NE;
    }
}

static void emit_jcc(int cc, Op l, Op r, int lab) {
    In *in = emit(I_JCC);
    in->cc = cc;
    in->s1 = l.v;
    if (r.isimm) in->imm = r.k;
    else in->s2 = r.v;
    in->lab = lab;
}

/* jump to lab if truth(c) == jt, else fall through */
static void cond_jump(N *c, int jt, int lab) {
    if (c->op == CB_CONST) {
        if ((c->val != 0) == jt) emit_jmp(lab);
        return;
    }
    if ((c->op == CB_AND || c->op == CB_OR) && isbool(c->a[0]) && isbool(c->a[1])) {
        int isand = c->op == CB_AND;
        if (isand != jt) {
            /* AND, jump if false  /  OR, jump if true */
            cond_jump(c->a[0], jt, lab);
            int m = nmemo;
            cond_jump(c->a[1], jt, lab);
            nmemo = m;
        } else {
            int skip = newl();
            cond_jump(c->a[0], !jt, skip);
            int m = nmemo;
            cond_jump(c->a[1], jt, lab);
            nmemo = m;
            emit_lab(skip);
        }
        return;
    }
    int cc;
    Op l, r;
    cond_operands(c, &cc, &l, &r);
    if (!jt) cc = inv_cmp(cc);
    emit_jcc(cc, l, r, lab);
}

static int cheap(const N *e, int depth) {
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: return 1;
    case CB_ADD: case CB_SUB: case CB_AND: case CB_OR: case CB_XOR: case CB_SHL: case CB_SHR:
        return depth < 1 && cheap(e->a[0], depth + 1) && cheap(e->a[1], depth + 1);
    default: return 0;
    }
}

static Op lower_if(N *e) {
    N *c = e->a[0];
    if (!e->hc && cheap(e->a[1], 0) && cheap(e->a[2], 0)) {
        int cc;
        Op l, r;
        cond_operands(c, &cc, &l, &r);
        Op ox = lower(e->a[1]), oy = lower(e->a[2]);
        if (ox.isimm && oy.isimm && ox.k == oy.k) return ox;
        if (!ox.isimm && !oy.isimm && ox.v == oy.v) return ox;
        int x = as_reg(ox), y = as_reg(oy);
        int d = newv();
        In *in = emit(I_SEL);
        in->cc = cc;
        in->d = d;
        in->s1 = l.v;
        if (r.isimm) in->imm = r.k;
        else in->s2 = r.v;
        in->x = x;
        in->y = y;
        return OPR(d);
    }
    int d = newv(), lf = newl(), le = newl();
    cond_jump(c, 0, lf);
    int m = nmemo, fm = nfact;
    add_facts(c, 1);
    emit_mov(d, lower(e->a[1]));
    nmemo = m;
    nfact = fm;
    emit_jmp(le);
    emit_lab(lf);
    add_facts(c, 0);
    emit_mov(d, lower(e->a[2]));
    nmemo = m;
    nfact = fm;
    emit_lab(le);
    return OPR(d);
}

static Op lower_inner(N *e) {
    switch (e->op) {
    case CB_GET: {
        Op i = lower(e->a[0]);
        int safe = known_lt_len(e->a[0]);
        if (i.isimm && i.k >= ((u64)1 << 28)) i = OPR(as_reg(i));
        int d = newv();
        In *in = emit(safe ? I_GETU : I_GET);
        in->d = d;
        if (i.isimm) in->imm = i.k;
        else in->s1 = i.v;
        return OPR(d);
    }
    case CB_IF: return lower_if(e);
    case CB_LET:
        varop[e->var] = lower(e->a[0]);
        return lower(e->a[1]);
    case CB_CALL: {
        Op o[4];
        for (int j = 0; j < e->argc; ++j) o[j] = lower(e->a[j]);
        for (int j = 0; j < e->argc; ++j) emit_mov(ARGREG[j], o[j]);
        In *in = emit(I_CALL);
        in->lab = e->fn;
        in->argc = e->argc;
        int d = newv();
        emit_mov(d, OPR(RAX));
        return OPR(d);
    }
    default: return lower_bin(e->op, lower(e->a[0]), lower(e->a[1]));
    }
}

static Op lower(N *e) {
    switch (e->op) {
    case CB_CONST: return OPI(e->val);
    case CB_VAR: return varop[e->var];
    case CB_LEN: return OPR(R15);
    default: break;
    }
    Op o;
    if (!e->hc && memo_find(e, &o)) return o;
    o = lower_inner(e);
    if (!e->hc) memo_add(e, o);
    return o;
}

static void lower_tail(N *e) {
    switch (e->op) {
    case CB_IF: {
        int lf = newl();
        N *c = e->a[0];
        cond_jump(c, 0, lf);
        int m = nmemo, fm = nfact;
        add_facts(c, 1);
        lower_tail(e->a[1]);
        nmemo = m;
        nfact = fm;
        emit_lab(lf);
        add_facts(c, 0);
        lower_tail(e->a[2]);
        nmemo = m;
        nfact = fm;
        return;
    }
    case CB_LET:
        varop[e->var] = lower(e->a[0]);
        lower_tail(e->a[1]);
        return;
    case CB_CALL: {
        Op o[4];
        for (int j = 0; j < e->argc; ++j) o[j] = lower(e->a[j]);
        if (e->fn == L_f) {
            int dst[4], n = 0;
            Op src[4];
            for (int j = 0; j < e->argc; ++j) {
                if (!o[j].isimm && o[j].v == L_pv[j]) continue;
                dst[n] = L_pv[j];
                src[n++] = o[j];
            }
            while (n) {
                int found = -1;
                for (int a = 0; a < n && found < 0; ++a) {
                    int blocked = 0;
                    for (int b = 0; b < n; ++b)
                        if (b != a && !src[b].isimm && src[b].v == dst[a]) blocked = 1;
                    if (!blocked) found = a;
                }
                if (found < 0) {
                    int t = newv();
                    emit_mov(t, OPR(dst[0]));
                    for (int b = 0; b < n; ++b)
                        if (!src[b].isimm && src[b].v == dst[0]) src[b].v = t;
                    found = 0;
                }
                emit_mov(dst[found], src[found]);
                dst[found] = dst[n - 1];
                src[found] = src[n - 1];
                n--;
            }
            emit_jmp(L_loop);
        } else {
            for (int j = 0; j < e->argc; ++j) emit_mov(ARGREG[j], o[j]);
            In *in = emit(I_TAIL);
            in->lab = e->fn;
            in->argc = e->argc;
        }
        return;
    }
    default: {
        Op o = lower(e);
        emit_mov(RAX, o);
        emit(I_RET);
    }
    }
}

/* ---- loop analysis helpers ---- */
static void scan_selftail(const N *e, int f, int *inv, int *found) {
    switch (e->op) {
    case CB_IF: scan_selftail(e->a[1], f, inv, found); scan_selftail(e->a[2], f, inv, found); return;
    case CB_LET: scan_selftail(e->a[1], f, inv, found); return;
    case CB_CALL:
        if (e->fn == f) {
            *found = 1;
            for (int j = 0; j < e->argc; ++j)
                if (!(e->a[j]->op == CB_VAR && e->a[j]->var == j)) inv[j] = 0;
        }
        return;
    default: return;
    }
}

static int is_inv(const N *e, const int *inv, int arity) {
    if (e->hc) return 0;
    switch (e->op) {
    case CB_VAR: return e->var < arity && inv[e->var];
    case CB_LET: return 0;
    case CB_CONST: case CB_LEN: return 1;
    default:
        for (int i = 0; i < e->argc; ++i)
            if (!is_inv(e->a[i], inv, arity)) return 0;
        return 1;
    }
}

static N *g_hoist[8];
static int g_nhoist;

static void hoist_collect(N *e, const int *inv, int arity) {
    if (!e->argc || e->hc && e->op == CB_CALL) {
        if (e->op == CB_CALL)
            for (int i = 0; i < e->argc; ++i) hoist_collect(e->a[i], inv, arity);
        return;
    }
    if (e->op != CB_LET && !is_cmp(e->op) && is_inv(e, inv, arity)) {
        for (int i = 0; i < g_nhoist; ++i)
            if (teq(g_hoist[i], e)) return;
        if (g_nhoist < 8) g_hoist[g_nhoist++] = e;
        return;
    }
    for (int i = 0; i < e->argc; ++i) hoist_collect(e->a[i], inv, arity);
}

static void hoist_tail(N *e, int f, const int *inv, int arity) {
    switch (e->op) {
    case CB_IF:
        hoist_collect(e->a[0], inv, arity);
        hoist_tail(e->a[1], f, inv, arity);
        hoist_tail(e->a[2], f, inv, arity);
        return;
    case CB_LET:
        hoist_collect(e->a[0], inv, arity);
        hoist_tail(e->a[1], f, inv, arity);
        return;
    case CB_CALL:
        if (e->fn == f)
            for (int i = 0; i < e->argc; ++i) hoist_collect(e->a[i], inv, arity);
        return;
    default: return;
    }
}

/* ------------------------------------------------------------------ */
/* register allocation                                                */
/* ------------------------------------------------------------------ */
static int g_nv, g_W;
static u64 *g_adj;
static int *g_color, *g_slot;
static double *g_cost;
static int g_nslots;
static int g_used_callee[16];

static int idefs(const In *in, int *o) {
    switch (in->op) {
    case I_MOV: case I_LI: case I_BIN: case I_CMP: case I_GET: case I_GETU: case I_SEL:
    case I_DIVC: case I_MODC:
        o[0] = in->d;
        return 1;
    case I_CALL:
        for (int i = 0; i < 6; ++i) o[i] = CALLER_SAVED[i];
        return 6;
    default: return 0;
    }
}
static int iuses(const In *in, int *o) {
    int n = 0;
    switch (in->op) {
    case I_MOV: o[n++] = in->s1; break;
    case I_BIN: case I_CMP: case I_JCC: case I_DIVC: case I_MODC:
        o[n++] = in->s1;
        if (in->s2 >= 0) o[n++] = in->s2;
        break;
    case I_GET: case I_GETU:
        if (in->s1 >= 0) o[n++] = in->s1;
        break;
    case I_SEL:
        o[n++] = in->s1;
        if (in->s2 >= 0) o[n++] = in->s2;
        o[n++] = in->x;
        o[n++] = in->y;
        break;
    case I_CALL: case I_TAIL:
        for (int j = 0; j < in->argc; ++j) o[n++] = ARGREG[j];
        break;
    default: break;
    }
    return n;
}
static int tracked(int v) { return v >= 16 || (v >= 0 && tracked_phys(v)); }

static void add_edge(int a, int b) {
    if (a == b || (a < 16 && b < 16)) return;
    g_adj[(size_t)a * g_W + (b >> 6)] |= 1ull << (b & 63);
    g_adj[(size_t)b * g_W + (a >> 6)] |= 1ull << (a & 63);
}

static double *g_sort_cost;
static int cmp_cost(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    if (g_sort_cost[x] > g_sort_cost[y]) return -1;
    if (g_sort_cost[x] < g_sort_cost[y]) return 1;
    return x - y;
}

static void regalloc(void) {
    int nv = nvr, W = (nv + 63) / 64;
    g_nv = nv;
    g_W = W;
    /* blocks */
    int *bstart = (int *)aalloc(sizeof(int) * (size_t)(ncode + 2));
    int *blk_of_lab = (int *)aalloc(sizeof(int) * (size_t)(nlab + 1));
    int nb = 0;
    for (int i = 0; i < ncode; ++i) {
        int lead = i == 0 || code[i].op == I_LAB;
        if (i > 0) {
            int p = code[i - 1].op;
            if (p == I_JCC || p == I_JMP || p == I_RET || p == I_TAIL) lead = 1;
        }
        if (lead) bstart[nb++] = i;
        if (code[i].op == I_LAB) blk_of_lab[code[i].lab] = nb - 1;
    }
    bstart[nb] = ncode;
    int *succ = (int *)aalloc(sizeof(int) * 2 * (size_t)(nb + 1));
    for (int b = 0; b < nb; ++b) {
        In *last = &code[bstart[b + 1] - 1];
        succ[2 * b] = succ[2 * b + 1] = -1;
        if (last->op == I_JCC) {
            succ[2 * b] = blk_of_lab[last->lab];
            if (b + 1 < nb) succ[2 * b + 1] = b + 1;
        } else if (last->op == I_JMP) {
            succ[2 * b] = blk_of_lab[last->lab];
        } else if (last->op != I_RET && last->op != I_TAIL) {
            if (b + 1 < nb) succ[2 * b] = b + 1;
        }
    }
    u64 *use = (u64 *)calloc((size_t)nb * W, 8);
    u64 *def = (u64 *)calloc((size_t)nb * W, 8);
    u64 *lin = (u64 *)calloc((size_t)nb * W, 8);
    u64 *lout = (u64 *)calloc((size_t)nb * W, 8);
    u64 *live = (u64 *)calloc((size_t)W, 8);
    g_adj = (u64 *)calloc((size_t)nv * W, 8);
    if (!use || !def || !lin || !lout || !live || !g_adj) {
        free(use); free(def); free(lin); free(lout); free(live); free(g_adj);
        g_adj = NULL;
        die("out of memory");
    }
    int D[16], U[16];
    for (int b = 0; b < nb; ++b) {
        u64 *ub = use + (size_t)b * W, *db = def + (size_t)b * W;
        for (int i = bstart[b]; i < bstart[b + 1]; ++i) {
            int nu = iuses(&code[i], U), nd = idefs(&code[i], D);
            for (int k = 0; k < nu; ++k) {
                int v = U[k];
                if (tracked(v) && !(db[v >> 6] >> (v & 63) & 1)) ub[v >> 6] |= 1ull << (v & 63);
            }
            for (int k = 0; k < nd; ++k) {
                int v = D[k];
                if (tracked(v)) db[v >> 6] |= 1ull << (v & 63);
            }
        }
    }
    for (int changed = 1; changed;) {
        changed = 0;
        for (int b = nb - 1; b >= 0; --b) {
            u64 *o = lout + (size_t)b * W, *in = lin + (size_t)b * W;
            for (int s = 0; s < 2; ++s) {
                int t = succ[2 * b + s];
                if (t < 0) continue;
                u64 *ti = lin + (size_t)t * W;
                for (int w = 0; w < W; ++w) o[w] |= ti[w];
            }
            u64 *ub = use + (size_t)b * W, *db = def + (size_t)b * W;
            for (int w = 0; w < W; ++w) {
                u64 nw = ub[w] | (o[w] & ~db[w]);
                if (nw != in[w]) { in[w] = nw; changed = 1; }
            }
        }
    }
    g_cost = (double *)aalloc(sizeof(double) * (size_t)nv);
    int *occ = (int *)aalloc(sizeof(int) * (size_t)nv);
    /* partner lists */
    int cap_mv = 64, nmv = 0;
    int *mva = (int *)aalloc(sizeof(int) * (size_t)cap_mv), *mvb = (int *)aalloc(sizeof(int) * (size_t)cap_mv);
#define ADD_MV(A, B) do { if (nmv == cap_mv) { \
        mva = (int *)agrow(mva, sizeof(int) * (size_t)cap_mv, sizeof(int) * (size_t)cap_mv * 2); \
        mvb = (int *)agrow(mvb, sizeof(int) * (size_t)cap_mv, sizeof(int) * (size_t)cap_mv * 2); \
        cap_mv *= 2; } mva[nmv] = (A); mvb[nmv++] = (B); } while (0)
    for (int b = 0; b < nb; ++b) {
        memcpy(live, lout + (size_t)b * W, (size_t)W * 8);
        for (int i = bstart[b + 1] - 1; i >= bstart[b]; --i) {
            In *in = &code[i];
            int nu = iuses(in, U), nd = idefs(in, D);
            for (int k = 0; k < nd; ++k) {
                int d = D[k];
                if (!tracked(d)) continue;
                g_cost[d] += in->w;
                occ[d]++;
                for (int w = 0; w < W; ++w) {
                    u64 bits = live[w];
                    while (bits) {
                        int l = w * 64 + ctz64(bits);
                        bits &= bits - 1;
                        if (l == d) continue;
                        if (in->op == I_MOV && l == in->s1) continue;
                        add_edge(d, l);
                    }
                }
            }
            for (int k = 0; k < nd; ++k)
                if (tracked(D[k])) live[D[k] >> 6] &= ~(1ull << (D[k] & 63));
            for (int k = 0; k < nu; ++k) {
                int u = U[k];
                if (!tracked(u)) continue;
                live[u >> 6] |= 1ull << (u & 63);
                g_cost[u] += in->w;
                occ[u]++;
            }
            if (in->op == I_MOV && tracked(in->d) && tracked(in->s1)) ADD_MV(in->d, in->s1);
            if ((in->op == I_BIN || in->op == I_CMP) && in->d >= 16 && in->s1 >= 16) ADD_MV(in->d, in->s1);
            if (in->op == I_SEL && in->d >= 16) { ADD_MV(in->d, in->y); ADD_MV(in->d, in->x); }
        }
    }
#undef ADD_MV
    free(use); free(def); free(lin); free(lout); free(live);
    /* partner adjacency */
    int *pcnt = (int *)aalloc(sizeof(int) * (size_t)(nv + 1));
    for (int i = 0; i < nmv; ++i) { pcnt[mva[i]]++; pcnt[mvb[i]]++; }
    int *pst = (int *)aalloc(sizeof(int) * (size_t)(nv + 1));
    for (int v = 0, s = 0; v < nv; ++v) { pst[v] = s; s += pcnt[v]; }
    pst[nv] = 2 * nmv;
    int *plist = (int *)aalloc(sizeof(int) * (size_t)(2 * nmv + 1));
    int *pfill = (int *)aalloc(sizeof(int) * (size_t)(nv + 1));
    for (int i = 0; i < nmv; ++i) {
        plist[pst[mva[i]] + pfill[mva[i]]++] = mvb[i];
        plist[pst[mvb[i]] + pfill[mvb[i]]++] = mva[i];
    }
    /* coloring */
    g_color = (int *)aalloc(sizeof(int) * (size_t)nv);
    g_slot = (int *)aalloc(sizeof(int) * (size_t)nv);
    for (int v = 0; v < nv; ++v) { g_color[v] = v < 16 ? v : -1; g_slot[v] = -1; }
    int *order = (int *)aalloc(sizeof(int) * (size_t)nv);
    int no = 0;
    for (int v = 16; v < nv; ++v)
        if (occ[v]) order[no++] = v;
    g_sort_cost = g_cost;
    qsort(order, (size_t)no, sizeof(int), cmp_cost);
    memset(g_used_callee, 0, sizeof g_used_callee);
    g_nslots = 0;
    unsigned char *slot_used = (unsigned char *)aalloc((size_t)nv + 1);
    for (int oi = 0; oi < no; ++oi) {
        int v = order[oi];
        unsigned forbid = 0;
        u64 *row = g_adj + (size_t)v * W;
        for (int w = 0; w < W; ++w) {
            u64 bits = row[w];
            while (bits) {
                int u = w * 64 + ctz64(bits);
                bits &= bits - 1;
                if (g_color[u] >= 0) forbid |= 1u << g_color[u];
            }
        }
        int c = -1;
        for (int p = pst[v]; p < pst[v] + pcnt[v] && c < 0; ++p) {
            int q = plist[p], qc = g_color[q];
            if (qc >= 0 && tracked_phys(qc) && !(forbid >> qc & 1)) c = qc;
        }
        for (int k = 0; k < 10 && c < 0; ++k)
            if (!(forbid >> ALLOC_ORDER[k] & 1)) c = ALLOC_ORDER[k];
        if (c >= 0) {
            g_color[v] = c;
            if (callee_saved(c)) g_used_callee[c] = 1;
            continue;
        }
        /* spill: choose a slot not used by interfering spilled vregs */
        memset(slot_used, 0, (size_t)g_nslots + 1);
        for (int w = 0; w < W; ++w) {
            u64 bits = row[w];
            while (bits) {
                int u = w * 64 + ctz64(bits);
                bits &= bits - 1;
                if (g_slot[u] >= 0) slot_used[g_slot[u]] = 1;
            }
        }
        int s = 0;
        while (slot_used[s]) s++;
        g_slot[v] = s;
        if (s + 1 > g_nslots) g_nslots = s + 1;
    }
    free(g_adj);
    g_adj = NULL;
}

/* ------------------------------------------------------------------ */
/* emission                                                           */
/* ------------------------------------------------------------------ */
static FILE *g_out;
static int g_any_vec;

static void P(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("  ", g_out);
    vfprintf(g_out, fmt, ap);
    fputc('\n', g_out);
    va_end(ap);
}

typedef struct { int r, off; } Loc;
static Loc LC(int v) {
    Loc l = {0, 0};
    if (v < 16) l.r = v;
    else if (g_color[v] >= 0) l.r = g_color[v];
    else { l.r = -1; l.off = g_slot[v] * 8; }
    return l;
}
static Loc LR(int r) { Loc l = {r, 0}; return l; }
static char g_sbuf[16][48];
static int g_sbi;
static const char *S(Loc l) {
    if (l.r >= 0) return R64[l.r];
    char *b = g_sbuf[g_sbi++ & 15];
    snprintf(b, 48, "%d(%%rsp)", l.off);
    return b;
}
static const char *IMM(u64 k) {
    char *b = g_sbuf[g_sbi++ & 15];
    snprintf(b, 48, "$%" PRId64, (i64)k);
    return b;
}
static int SAME(Loc a, Loc b) { return a.r >= 0 ? a.r == b.r : (b.r < 0 && a.off == b.off); }

static void mv(Loc d, Loc s) {
    if (SAME(d, s)) return;
    if (d.r < 0 && s.r < 0) {
        P("movq %s, %%rax", S(s));
        P("movq %%rax, %s", S(d));
    } else {
        P("movq %s, %s", S(s), S(d));
    }
}
static void li(Loc d, u64 k) {
    if (d.r >= 0) {
        if (k == 0) P("xorl %s, %s", R32[d.r], R32[d.r]);
        else if (k <= 0xffffffffull) P("movl $%" PRIu64 ", %s", k, R32[d.r]);
        else if (fits32(k)) P("movq %s, %s", IMM(k), R64[d.r]);
        else P("movabsq $%" PRId64 ", %s", (i64)k, R64[d.r]);
    } else {
        if (fits32(k)) P("movq %s, %s", IMM(k), S(d));
        else {
            P("movabsq $%" PRId64 ", %%rax", (i64)k);
            P("movq %%rax, %s", S(d));
        }
    }
}

static const char *CCS[6] = {"e", "ne", "b", "be", "a", "ae"};
static const char *ccs(int cc) { return CCS[cc - CB_EQ]; }

static void cmpflags(int s1, int s2, u64 imm) {
    Loc a = LC(s1);
    if (s2 < 0) {
        if (imm == 0 && a.r >= 0) P("testq %s, %s", R64[a.r], R64[a.r]);
        else P("cmpq %s, %s", IMM(imm), S(a));
    } else {
        Loc b = LC(s2);
        if (a.r < 0 && b.r < 0) {
            P("movq %s, %%rax", S(a));
            P("cmpq %s, %%rax", S(b));
        } else {
            P("cmpq %s, %s", S(b), S(a));
        }
    }
}

static void alu2(const char *mn, Loc d, Loc a, const char *bs, int hasb, Loc b, int comm, int issub) {
    if (d.r >= 0) {
        if (SAME(d, a)) P("%s %s, %s", mn, bs, S(d));
        else if (hasb && SAME(d, b)) {
            if (comm) P("%s %s, %s", mn, S(a), S(d));
            else if (issub) { P("negq %s", S(d)); P("addq %s, %s", S(a), S(d)); }
            else { P("movq %s, %%rax", S(a)); P("%s %s, %%rax", mn, bs); mv(d, LR(RAX)); }
        } else {
            mv(d, a);
            P("%s %s, %s", mn, bs, S(d));
        }
    } else {
        if (SAME(d, a) && (!hasb || b.r >= 0)) P("%s %s, %s", mn, bs, S(d));
        else {
            mv(LR(RAX), a);
            P("%s %s, %%rax", mn, bs);
            mv(d, LR(RAX));
        }
    }
}

static int magic_simple(u64 d, u64 *m, int *s) {
    for (int sh = 0; sh < 64; ++sh) {
        int p = 64 + sh;
        u128 two_p = (u128)1 << p;
        u128 q = two_p / d + 1;
        if (q >> 64) return 0;
        u128 e = q * d - two_p;
        if (e <= ((u128)1 << sh)) {
            *m = (u64)q;
            *s = sh;
            return 1;
        }
    }
    return 0;
}

static void e_divc(const In *in, int mod) {
    Loc d = LC(in->d), a = LC(in->s1);
    u64 k = in->imm;
    if (k >> 63) {
        P("movabsq $%" PRId64 ", %%rcx", (i64)k);
        if (!mod) {
            P("xorl %%eax, %%eax");
            P("cmpq %%rcx, %s", S(a));
            P("setae %%al");
            mv(d, LR(RAX));
        } else {
            mv(LR(RDX), a);
            P("movq %%rdx, %%rax");
            P("subq %%rcx, %%rax");
            P("cmovbq %%rdx, %%rax");
            mv(d, LR(RAX));
        }
        return;
    }
    u64 m;
    int s, q;
    if (magic_simple(k, &m, &s)) {
        P("movabsq $%" PRId64 ", %%rax", (i64)m);
        P("mulq %s", S(a));
        if (s) P("shrq $%d, %%rdx", s);
        q = RDX;
    } else {
        int l = 64 - __builtin_clzll(k - 1); /* ceil(log2 k) */
        u128 num = (u128)(((u64)1 << l) - k) << 64;
        u64 m2 = (u64)(num / k) + 1;
        P("movabsq $%" PRId64 ", %%rax", (i64)m2);
        P("mulq %s", S(a));
        mv(LR(RAX), a);
        P("subq %%rdx, %%rax");
        P("shrq $1, %%rax");
        P("addq %%rdx, %%rax");
        if (l - 1) P("shrq $%d, %%rax", l - 1);
        q = RAX;
    }
    if (!mod) {
        mv(d, LR(q));
        return;
    }
    if (fits32(k)) P("imulq %s, %s, %%rax", IMM(k), R64[q]);
    else {
        P("movabsq $%" PRId64 ", %%rcx", (i64)k);
        if (q != RAX) P("movq %s, %%rax", R64[q]);
        P("imulq %%rcx, %%rax");
    }
    mv(LR(RDX), a);
    P("subq %%rax, %%rdx");
    mv(d, LR(RDX));
}

static void e_bin(const In *in) {
    Loc d = LC(in->d), a = LC(in->s1);
    int hasb = in->s2 >= 0;
    Loc b = hasb ? LC(in->s2) : LR(RAX);
    const char *bs = hasb ? S(b) : IMM(in->imm);
    switch (in->cc) {
    case CB_ADD:
        if (d.r >= 0 && a.r >= 0 && !SAME(d, a)) {
            if (!hasb) { P("leaq %" PRId64 "(%s), %s", (i64)in->imm, R64[a.r], R64[d.r]); return; }
            if (b.r >= 0 && !SAME(d, b)) { P("leaq (%s,%s), %s", R64[a.r], R64[b.r], R64[d.r]); return; }
        }
        alu2("addq", d, a, bs, hasb, b, 1, 0);
        return;
    case CB_SUB: alu2("subq", d, a, bs, hasb, b, 0, 1); return;
    case CB_AND: alu2("andq", d, a, bs, hasb, b, 1, 0); return;
    case CB_OR: alu2("orq", d, a, bs, hasb, b, 1, 0); return;
    case CB_XOR: alu2("xorq", d, a, bs, hasb, b, 1, 0); return;
    case CB_MUL: {
        Loc t = d.r >= 0 ? d : LR(RAX);
        if (!hasb) {
            u64 k = in->imm;
            if ((k == 3 || k == 5 || k == 9) && a.r >= 0)
                P("leaq (%s,%s,%d), %s", R64[a.r], R64[a.r], (int)k - 1, R64[t.r]);
            else
                P("imulq %s, %s, %s", bs, S(a), R64[t.r]);
        } else if (SAME(t, a)) P("imulq %s, %s", bs, R64[t.r]);
        else if (SAME(t, b)) P("imulq %s, %s", S(a), R64[t.r]);
        else { mv(t, a); P("imulq %s, %s", bs, R64[t.r]); }
        mv(d, t);
        return;
    }
    case CB_SHL:
    case CB_SHR: {
        const char *mn = in->cc == CB_SHL ? "shlq" : "shrq";
        if (!hasb) { alu2(mn, d, a, bs, 0, b, 0, 0); return; }
        mv(LR(RCX), b);
        alu2(mn, d, a, "%cl", 0, b, 0, 0);
        return;
    }
    case CB_DIV:
    case CB_MOD:
        mv(LR(RCX), b);
        P("xorl %%eax, %%eax");
        P("xorl %%edx, %%edx");
        P("testq %%rcx, %%rcx");
        P("je 1f");
        mv(LR(RAX), a);
        P("divq %%rcx");
        fprintf(g_out, "1:\n");
        mv(d, LR(in->cc == CB_DIV ? RAX : RDX));
        return;
    default: die("bad binop");
    }
}

static void e_cmp(const In *in) {
    Loc d = LC(in->d), a = LC(in->s1);
    int ok = d.r >= 0 && !SAME(d, a) && (in->s2 < 0 || !SAME(d, LC(in->s2)));
    if (ok) {
        P("xorl %s, %s", R32[d.r], R32[d.r]);
        cmpflags(in->s1, in->s2, in->imm);
        P("set%s %s", ccs(in->cc), R8N[d.r]);
    } else {
        cmpflags(in->s1, in->s2, in->imm);
        P("set%s %%al", ccs(in->cc));
        P("movzbl %%al, %%eax");
        mv(d, LR(RAX));
    }
}

static void e_sel(const In *in) {
    Loc d = LC(in->d), x = LC(in->x), y = LC(in->y);
    cmpflags(in->s1, in->s2, in->imm);
    const char *cc = ccs(in->cc), *ncc = ccs(inv_cmp(in->cc));
    if (d.r >= 0) {
        if (SAME(d, x)) { if (!SAME(x, y)) P("cmov%sq %s, %s", ncc, S(y), S(d)); }
        else if (SAME(d, y)) P("cmov%sq %s, %s", cc, S(x), S(d));
        else { P("movq %s, %s", S(y), S(d)); P("cmov%sq %s, %s", cc, S(x), S(d)); }
    } else {
        P("movq %s, %%rax", S(y));
        P("cmov%sq %s, %%rax", cc, S(x));
        mv(d, LR(RAX));
    }
}

static void e_get(const In *in, int checked) {
    Loc d = LC(in->d);
    if (in->s1 < 0) {
        i64 off = (i64)in->imm * 8;
        if (!checked) {
            if (d.r >= 0) P("movq %" PRId64 "(%%r14), %s", off, R64[d.r]);
            else { P("movq %" PRId64 "(%%r14), %%rax", off); mv(d, LR(RAX)); }
            return;
        }
        int t = d.r >= 0 ? d.r : RAX;
        P("xorl %s, %s", R32[t], R32[t]);
        P("cmpq %s, %%r15", IMM(in->imm));
        P("jbe 1f");
        P("movq %" PRId64 "(%%r14), %s", off, R64[t]);
        fprintf(g_out, "1:\n");
        mv(d, LR(t));
        return;
    }
    Loc s = LC(in->s1);
    int R;
    if (s.r >= 0) R = s.r;
    else { P("movq %s, %%rax", S(s)); R = RAX; }
    if (!checked) {
        int t = d.r >= 0 ? d.r : RDX;
        P("movq (%%r14,%s,8), %s", R64[R], R64[t]);
        mv(d, LR(t));
        return;
    }
    int t = (d.r >= 0 && d.r != R) ? d.r : (R == RAX ? RDX : RAX);
    P("xorl %s, %s", R32[t], R32[t]);
    P("cmpq %%r15, %s", R64[R]);
    P("jae 1f");
    P("movq (%%r14,%s,8), %s", R64[R], R64[t]);
    fprintf(g_out, "1:\n");
    mv(d, LR(t));
}

static int g_frame;
static void e_epilogue(void) {
    if (g_frame) P("addq $%d, %%rsp", g_frame);
    for (int r = 15; r >= 0; --r)
        if (g_used_callee[r]) P("popq %s", R64[r]);
}

static void emit_code(int f) {
    g_frame = g_nslots * 8;
    for (int r = 0; r < 16; ++r)
        if (g_used_callee[r]) P("pushq %s", R64[r]);
    if (g_frame) P("subq $%d, %%rsp", g_frame);
    for (int i = 0; i < ncode; ++i) {
        In *in = &code[i];
        switch (in->op) {
        case I_MOV: mv(LC(in->d), LC(in->s1)); break;
        case I_LI: li(LC(in->d), in->imm); break;
        case I_BIN: e_bin(in); break;
        case I_CMP: e_cmp(in); break;
        case I_GET: e_get(in, 1); break;
        case I_GETU: e_get(in, 0); break;
        case I_DIVC: e_divc(in, 0); break;
        case I_MODC: e_divc(in, 1); break;
        case I_SEL: e_sel(in); break;
        case I_JCC:
            cmpflags(in->s1, in->s2, in->imm);
            P("j%s .L%d_%d", ccs(in->cc), f, in->lab);
            break;
        case I_JMP: {
            int j = i + 1, skip = 0;
            while (j < ncode && code[j].op == I_LAB) {
                if (code[j].lab == in->lab) skip = 1;
                j++;
            }
            if (!skip) P("jmp .L%d_%d", f, in->lab);
            break;
        }
        case I_LAB: fprintf(g_out, ".L%d_%d:\n", f, in->lab); break;
        case I_CALL: P("call .Lf%d", in->lab); break;
        case I_TAIL: e_epilogue(); P("jmp .Lf%d", in->lab); break;
        case I_RET: e_epilogue(); P("ret"); break;
        default: die("bad instruction");
        }
    }
}

static void vec_preamble(int f);

static void compile_fn(int f) {
    Fn *fn = &F[f];
    code = NULL;
    ncode = capcode = 0;
    nvr = 16;
    nlab = 0;
    nmemo = nfact = 0;
    L_f = f;
    L_arity = fn->arity;
    L_w = 1;
    varop = (Op *)aalloc(sizeof(Op) * (size_t)(fn->nvars + 1));
    for (int j = 0; j < fn->arity; ++j) {
        L_pv[j] = newv();
        emit_mov(L_pv[j], OPR(ARGREG[j]));
        varop[j] = OPR(L_pv[j]);
    }
    int inv[4] = {1, 1, 1, 1}, found = 0;
    scan_selftail(fn->body, f, inv, &found);
    L_loop = newl();
    if (found) {
        g_nhoist = 0;
        hoist_tail(fn->body, f, inv, fn->arity);
        for (int i = 0; i < g_nhoist; ++i) lower(g_hoist[i]);
        emit_lab(L_loop);
        L_w = 16;
    }
    lower_tail(fn->body);
    regalloc();
    fprintf(g_out, ".p2align 4\n.Lf%d:\n", f);
    vec_preamble(f);
    emit_code(f);
}

/* ------------------------------------------------------------------ */
/* AVX2 vectorization of reduction loops (placeholder, filled below)  */
/* ------------------------------------------------------------------ */
static void vec_preamble(int f) { (void)f; }

/* ------------------------------------------------------------------ */
/* driver                                                             */
/* ------------------------------------------------------------------ */
int cb_emit_assembly(const CbProgram *p, const char *path, char *error, size_t error_size) {
    FILE *f = NULL;
    g_arena = NULL;
    g_adj = NULL;
    g_errmsg[0] = 0;
    if (setjmp(g_fail)) {
        if (f) fclose(f);
        free(g_adj);
        g_adj = NULL;
        arena_free();
        if (error && error_size) snprintf(error, error_size, "%s", g_errmsg[0] ? g_errmsg : "codegen failed");
        return 0;
    }
    if (!p || p->functions <= 0 || p->functions > CB_MAX_FUNCTIONS || p->entry < 0 ||
        p->entry >= p->functions)
        die("invalid program");
    if (p->fn[p->entry].arity != 0) die("entry function must take no arguments");
    g_prog = p;
    nF = p->functions;
    g_entry = p->entry;
    memset(F, 0, sizeof F);
    for (int i = 0; i < nF; ++i) {
        const CbFunction *cf = &p->fn[i];
        if (cf->arity < 0 || cf->arity > CB_MAX_PARAMS || cf->locals < cf->arity) die("bad function header");
        F[i].arity = cf->arity;
        F[i].nvars = cf->locals;
        F[i].body = simp(conv(cf->body, cf, 0));
    }
    analyze_calls();
    acc_intro();
    inline_all();
    acc_intro();
    inline_all();

    f = fopen(path, "w");
    if (!f) {
        snprintf(g_errmsg, sizeof g_errmsg, "%s: %s", path, strerror(errno));
        longjmp(g_fail, 1);
    }
    g_out = f;
    g_any_vec = 0;
    fprintf(f, ".text\n");
    for (int i = 0; i < nF; ++i)
        if (F[i].live) compile_fn(i);
    fprintf(f, ".globl bench_entry\n.type bench_entry, @function\n.p2align 4\nbench_entry:\n");
    P("pushq %%rbx");
    P("pushq %%rbp");
    P("pushq %%r12");
    P("pushq %%r13");
    P("pushq %%r14");
    P("pushq %%r15");
    P("movq %%rdi, %%r14");
    P("movq %%rsi, %%r15");
    P("call .Lf%d", g_entry);
    if (g_any_vec) P("vzeroupper");
    P("popq %%r15");
    P("popq %%r14");
    P("popq %%r13");
    P("popq %%r12");
    P("popq %%rbp");
    P("popq %%rbx");
    P("ret");
    fprintf(f, ".size bench_entry, .-bench_entry\n");
    fprintf(f, ".section .note.GNU-stack,\"\",@progbits\n");
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    f = NULL;
    arena_free();
    if (!ok) {
        if (error && error_size) snprintf(error, error_size, "could not write %s", path);
        return 0;
    }
    return 1;
}
