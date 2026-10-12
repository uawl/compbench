#include "ast.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_MAX_PARAMS 6
#define IR_MAX_LOCALS 512
#define IR_MAX_NODES 65536
#define MODE_A_REGS_CNT 8
#define MODE_B_REGS_CNT 5

typedef struct IrExpr {
    CbOp op;
    uint64_t value;
    int index, argc;
    struct IrExpr *arg[IR_MAX_PARAMS];
} IrExpr;

typedef struct {
    int orig_arity, arity, locals;
    int has_acc_init;
    uint64_t acc_init_val;
    int has_nontail_call;
    int max_stack_temps;
    IrExpr *body;
} IrFunc;

typedef struct {
    int functions, entry, nodes;
    IrFunc fn[CB_MAX_FUNCTIONS];
    IrExpr pool[IR_MAX_NODES];
    uint8_t reaches[CB_MAX_FUNCTIONS][CB_MAX_FUNCTIONS];
    uint8_t direct_call[CB_MAX_FUNCTIONS][CB_MAX_FUNCTIONS];
} IrProg;

typedef enum {
    RED_NONE = 0, RED_ADD, RED_XOR, RED_OR, RED_AND, RED_MUL, RED_MAX, RED_MIN
} RedKind;

typedef struct {
    int valid, is_avx2, idx_param;
    int bound_kind; /* 0: LEN, 1: CONST, 2: VAR */
    uint64_t bound_val;
    int bound_var;
    int has_gets, max_get_offset;
    IrExpr *cont_br, *exit_br, *tail_call;
    int num_lets;
    int let_slots[64];
    IrExpr *let_vals[64];
    int acc_count;
    int acc_param[IR_MAX_PARAMS];
    RedKind acc_kind[IR_MAX_PARAMS];
    IrExpr *acc_rhs[IR_MAX_PARAMS];
} LoopInfo;

typedef struct {
    FILE *out;
    const IrProg *prog;
    const IrFunc *fn;
    int fn_idx;
    unsigned label;
    int has_nontail, uses_r15, num_callee_saved;
    int callee_saved_regs[6];
    int frame_bytes, xmm_depth, stack_depth, rdx_busy;
    int all_gets_in_bounds;
    int unroll_step_label;
    uint64_t known_lt_len;
    const LoopInfo *loop;
} Emit;

static const char *const ARG_REGS[IR_MAX_PARAMS] = {"%r8", "%r9", "%r10", "%r11", "%rbx", "%r12"};
static const char *const MODE_A_REGS[MODE_A_REGS_CNT] = {"%r8", "%r9", "%r10", "%r11", "%rbx", "%r12", "%r13", "%r14"};
static const char *const MODE_B_REGS[MODE_B_REGS_CNT] = {"%rbx", "%r12", "%r13", "%r14", "%r15"};

static int num_children(const IrExpr *e) {
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: return 0;
    case CB_GET: return 1;
    case CB_IF: return 3;
    case CB_LET: return 2;
    case CB_CALL: return e->argc;
    default: return 2;
    }
}

static IrExpr *alloc_node(IrProg *p, CbOp op) {
    if (p->nodes >= IR_MAX_NODES) return NULL;
    IrExpr *e = &p->pool[p->nodes++];
    memset(e, 0, sizeof(*e));
    e->op = op;
    return e;
}

static IrExpr *new_const(IrProg *p, uint64_t v) {
    IrExpr *e = alloc_node(p, CB_CONST);
    if (e) e->value = v;
    return e;
}

static IrExpr *new_var(IrProg *p, int idx) {
    IrExpr *e = alloc_node(p, CB_VAR);
    if (e) e->index = idx;
    return e;
}

static IrExpr *new_binop(IrProg *p, CbOp op, IrExpr *a, IrExpr *b) {
    IrExpr *e = (a && b) ? alloc_node(p, op) : NULL;
    if (e) { e->arg[0] = a; e->arg[1] = b; }
    return e;
}

static IrExpr *new_if(IrProg *p, IrExpr *c, IrExpr *t, IrExpr *f) {
    IrExpr *e = (c && t && f) ? alloc_node(p, CB_IF) : NULL;
    if (e) { e->arg[0] = c; e->arg[1] = t; e->arg[2] = f; }
    return e;
}

static IrExpr *new_let(IrProg *p, int idx, IrExpr *val, IrExpr *body) {
    IrExpr *e = (val && body) ? alloc_node(p, CB_LET) : NULL;
    if (e) { e->index = idx; e->arg[0] = val; e->arg[1] = body; }
    return e;
}

static IrExpr *import_ast(IrProg *p, const CbExpr *src) {
    if (!src) return NULL;
    IrExpr *e = alloc_node(p, src->op);
    if (!e) return NULL;
    e->value = src->value;
    e->index = src->index;
    e->argc = src->argc;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        if (!(e->arg[i] = import_ast(p, src->arg[i]))) return NULL;
    }
    return e;
}

static IrExpr *clone_ir(IrProg *p, const IrExpr *src, int shift) {
    if (!src) return NULL;
    IrExpr *e = alloc_node(p, src->op);
    if (!e) return NULL;
    e->value = src->value;
    e->index = src->index + ((src->op == CB_VAR || src->op == CB_LET) ? shift : 0);
    e->argc = src->argc;
    int nc = num_children(src);
    for (int i = 0; i < nc; ++i) {
        if (!(e->arg[i] = clone_ir(p, src->arg[i], shift))) return NULL;
    }
    return e;
}

static int ast_size(const IrExpr *e) {
    if (!e) return 0;
    int s = 1, nc = num_children(e);
    for (int i = 0; i < nc; ++i) s += ast_size(e->arg[i]);
    return s;
}

static int expr_equals(const IrExpr *a, const IrExpr *b) {
    if (a == b) return 1;
    if (!a || !b || a->op != b->op) return 0;
    if (a->op == CB_CONST) return a->value == b->value;
    if (a->op == CB_VAR || a->op == CB_LET) {
        if (a->index != b->index) return 0;
    }
    if (a->op == CB_CALL && (a->index != b->index || a->argc != b->argc)) return 0;
    int nc = num_children(a);
    for (int i = 0; i < nc; ++i)
        if (!expr_equals(a->arg[i], b->arg[i])) return 0;
    return 1;
}

static int has_call(const IrExpr *e) {
    if (!e) return 0;
    if (e->op == CB_CALL) return 1;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) if (has_call(e->arg[i])) return 1;
    return 0;
}

static int has_call_to(const IrExpr *e, int fn_idx) {
    if (!e) return 0;
    if (e->op == CB_CALL && e->index == fn_idx) return 1;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) if (has_call_to(e->arg[i], fn_idx)) return 1;
    return 0;
}

static int count_var_uses(const IrExpr *e, int v) {
    if (!e) return 0;
    if (e->op == CB_VAR) return e->index == v ? 1 : 0;
    int c = 0, nc = num_children(e);
    for (int i = 0; i < nc; ++i) c += count_var_uses(e->arg[i], v);
    return c;
}

static void mark_calls(const IrExpr *e, uint8_t *row) {
    if (!e) return;
    if (e->op == CB_CALL && e->index >= 0 && e->index < CB_MAX_FUNCTIONS) row[e->index] = 1;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) mark_calls(e->arg[i], row);
}

static void compute_call_graph(IrProg *p) {
    memset(p->direct_call, 0, sizeof(p->direct_call));
    memset(p->reaches, 0, sizeof(p->reaches));
    for (int i = 0; i < p->functions; ++i) {
        mark_calls(p->fn[i].body, p->direct_call[i]);
        memcpy(p->reaches[i], p->direct_call[i], CB_MAX_FUNCTIONS);
    }
    for (int k = 0; k < p->functions; ++k)
        for (int i = 0; i < p->functions; ++i)
            if (p->reaches[i][k])
                for (int j = 0; j < p->functions; ++j)
                    if (p->reaches[k][j]) p->reaches[i][j] = 1;
}

static IrExpr *inline_calls(IrProg *p, IrFunc *caller, int caller_idx, IrExpr *e) {
    if (!e) return NULL;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) e->arg[i] = inline_calls(p, caller, caller_idx, e->arg[i]);
    if (e->op == CB_CALL) {
        int cid = e->index;
        if (cid != caller_idx && cid >= 0 && cid < p->functions) {
            IrFunc *callee = &p->fn[cid];
            int csize = ast_size(callee->body);
            int ok = (!callee->has_acc_init && !p->direct_call[cid][cid]) &&
                     ((!p->reaches[cid][cid] && csize <= 80) || (caller_idx < cid && csize <= 40));
            if (ok && caller->locals + callee->locals + 4 <= IR_MAX_LOCALS &&
                p->nodes + csize + 16 < IR_MAX_NODES && ast_size(caller->body) + csize <= 450) {
                int base = caller->locals;
                caller->locals += callee->locals;
                IrExpr *cloned = clone_ir(p, callee->body, base);
                if (cloned) {
                    IrExpr *res = cloned;
                    for (int i = e->argc - 1; i >= 0; --i) {
                        IrExpr *ln = new_let(p, base + i, e->arg[i], res);
                        if (!ln) return e;
                        res = ln;
                    }
                    return res;
                }
            }
        }
    }
    return e;
}

static uint64_t eval_const(CbOp op, uint64_t a, uint64_t b) {
    switch (op) {
    case CB_ADD: return a + b;
    case CB_SUB: return a - b;
    case CB_MUL: return a * b;
    case CB_DIV: return b == 0 ? 0 : a / b;
    case CB_MOD: return b == 0 ? 0 : a % b;
    case CB_AND: return a & b;
    case CB_OR:  return a | b;
    case CB_XOR: return a ^ b;
    case CB_SHL: return a << (b & 63);
    case CB_SHR: return a >> (b & 63);
    case CB_EQ:  return a == b;
    case CB_NE:  return a != b;
    case CB_LT:  return a < b;
    case CB_LE:  return a <= b;
    case CB_GT:  return a > b;
    case CB_GE:  return a >= b;
    default: return 0;
    }
}

static int is_comm(CbOp op) {
    return op == CB_ADD || op == CB_MUL || op == CB_AND || op == CB_OR || op == CB_XOR || op == CB_EQ || op == CB_NE;
}
static int is_cmp(CbOp op) { return op >= CB_EQ && op <= CB_GE; }
static CbOp neg_cmp(CbOp op) {
    switch (op) {
    case CB_EQ: return CB_NE; case CB_NE: return CB_EQ;
    case CB_LT: return CB_GE; case CB_LE: return CB_GT;
    case CB_GT: return CB_LE; case CB_GE: return CB_LT;
    default: return op;
    }
}
static CbOp swap_cmp(CbOp op) {
    switch (op) {
    case CB_LT: return CB_GT; case CB_LE: return CB_GE;
    case CB_GT: return CB_LT; case CB_GE: return CB_LE;
    default: return op;
    }
}

static IrExpr *subst_var(IrProg *p, IrExpr *e, int v, const IrExpr *rep) {
    if (!e) return NULL;
    if (e->op == CB_VAR && e->index == v) {
        IrExpr *c = clone_ir(p, rep, 0);
        return c ? c : e;
    }
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) e->arg[i] = subst_var(p, e->arg[i], v, rep);
    return e;
}

static IrExpr *simplify_expr(IrProg *p, IrExpr *e, IrExpr **subst) {
    if (!e) return NULL;
    if (e->op == CB_VAR) {
        if (e->index >= 0 && e->index < IR_MAX_LOCALS && subst[e->index]) {
            IrExpr *c = clone_ir(p, subst[e->index], 0);
            if (c) return c;
        }
        return e;
    }
    if (e->op == CB_LET) {
        e->arg[0] = simplify_expr(p, e->arg[0], subst);
        if (e->arg[0]->op == CB_LET && p->nodes + 4 < IR_MAX_NODES) {
            IrExpr *in = e->arg[0];
            e->arg[0] = in->arg[1];
            in->arg[1] = e;
            return simplify_expr(p, in, subst);
        }
        int idx = e->index;
        IrExpr *val = e->arg[0];
        if ((val->op == CB_CONST || val->op == CB_VAR || val->op == CB_LEN) && idx >= 0 && idx < IR_MAX_LOCALS) {
            IrExpr *old = subst[idx];
            subst[idx] = val;
            IrExpr *res = simplify_expr(p, e->arg[1], subst);
            subst[idx] = old;
            return res;
        }
        e->arg[1] = simplify_expr(p, e->arg[1], subst);
        IrExpr *body = e->arg[1];
        if (!has_call(val)) {
            int uses = count_var_uses(body, idx);
            if (uses == 0) return body;
            if (uses == 1 && ast_size(val) <= 12 && p->nodes + 16 < IR_MAX_NODES) {
                if (body->op == CB_IF && count_var_uses(body->arg[0], idx) == 1) {
                    body->arg[0] = subst_var(p, body->arg[0], idx, val);
                    return simplify_expr(p, body, subst);
                }
                if (body->op == CB_VAR && body->index == idx) return val;
            }
            if (body->op == CB_IF && count_var_uses(body->arg[0], idx) == 0 && p->nodes + 4 < IR_MAX_NODES) {
                int u1 = count_var_uses(body->arg[1], idx), u2 = count_var_uses(body->arg[2], idx);
                if (u1 > 0 && u2 == 0) { e->arg[1] = body->arg[1]; body->arg[1] = e; return simplify_expr(p, body, subst); }
                if (u2 > 0 && u1 == 0) { e->arg[1] = body->arg[2]; body->arg[2] = e; return simplify_expr(p, body, subst); }
            }
        }
        return e;
    }
    if (e->op == CB_IF) {
        e->arg[0] = simplify_expr(p, e->arg[0], subst);
        if (e->arg[0]->op == CB_CONST)
            return simplify_expr(p, e->arg[0]->value ? e->arg[1] : e->arg[2], subst);
        e->arg[1] = simplify_expr(p, e->arg[1], subst);
        e->arg[2] = simplify_expr(p, e->arg[2], subst);
        if (!has_call(e->arg[0]) && expr_equals(e->arg[1], e->arg[2])) return e->arg[1];
        if (is_cmp(e->arg[0]->op) && e->arg[1]->op == CB_CONST && e->arg[2]->op == CB_CONST) {
            if (e->arg[1]->value == 1 && e->arg[2]->value == 0) return e->arg[0];
            if (e->arg[1]->value == 0 && e->arg[2]->value == 1) {
                e->arg[0]->op = neg_cmp(e->arg[0]->op);
                return e->arg[0];
            }
        }
        return e;
    }
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) e->arg[i] = simplify_expr(p, e->arg[i], subst);
    if (e->op >= CB_ADD && e->op <= CB_GE) {
        IrExpr *l = e->arg[0], *r = e->arg[1];
        if (l->op == CB_CONST && r->op == CB_CONST) {
            e->value = eval_const(e->op, l->value, r->value);
            e->op = CB_CONST;
            return e;
        }
        if (is_comm(e->op) && l->op == CB_CONST && r->op != CB_CONST) {
            e->arg[0] = r; e->arg[1] = l; l = e->arg[0]; r = e->arg[1];
        } else if (is_cmp(e->op) && l->op == CB_CONST && r->op != CB_CONST) {
            e->op = swap_cmp(e->op);
            e->arg[0] = r; e->arg[1] = l; l = e->arg[0]; r = e->arg[1];
        }
        if (r->op == CB_CONST) {
            uint64_t v = r->value;
            switch (e->op) {
            case CB_ADD:
                if (v == 0) return l;
                if (l->op == CB_ADD && l->arg[1]->op == CB_CONST) { l->arg[1]->value += v; return l; }
                if (l->op == CB_SUB && l->arg[1]->op == CB_CONST) { l->arg[1]->value -= v; return l; }
                break;
            case CB_SUB:
                if (v == 0) return l;
                if (l->op == CB_SUB && l->arg[1]->op == CB_CONST) { l->arg[1]->value += v; return l; }
                if (l->op == CB_ADD && l->arg[1]->op == CB_CONST) { l->arg[1]->value -= v; return l; }
                break;
            case CB_MUL:
                if (v == 0 && !has_call(l)) { e->op = CB_CONST; e->value = 0; return e; }
                if (v == 1) return l;
                if (v > 1 && (v & (v - 1)) == 0) { e->op = CB_SHL; r->value = (uint64_t)__builtin_ctzll(v); return e; }
                if (l->op == CB_MUL && l->arg[1]->op == CB_CONST) { l->arg[1]->value *= v; return l; }
                break;
            case CB_DIV:
                if (v == 0 && !has_call(l)) { e->op = CB_CONST; e->value = 0; return e; }
                if (v == 1) return l;
                if (v > 1 && (v & (v - 1)) == 0) { e->op = CB_SHR; r->value = (uint64_t)__builtin_ctzll(v); return e; }
                break;
            case CB_MOD:
                if ((v == 0 || v == 1) && !has_call(l)) { e->op = CB_CONST; e->value = 0; return e; }
                if (v > 1 && (v & (v - 1)) == 0) { e->op = CB_AND; r->value = v - 1; return e; }
                break;
            case CB_AND:
                if (v == 0 && !has_call(l)) { e->op = CB_CONST; e->value = 0; return e; }
                if (v == ~0ULL) return l;
                break;
            case CB_OR:
                if (v == 0) return l;
                if (v == ~0ULL && !has_call(l)) { e->op = CB_CONST; e->value = ~0ULL; return e; }
                break;
            case CB_XOR: if (v == 0) return l; break;
            case CB_SHL: case CB_SHR: if ((v & 63) == 0) return l; break;
            case CB_LT:
                if (v == 0 && !has_call(l)) { e->op = CB_CONST; e->value = 0; return e; }
                if (v == 1) { e->op = CB_EQ; r->value = 0; return e; }
                break;
            case CB_GE:
                if (v == 0 && !has_call(l)) { e->op = CB_CONST; e->value = 1; return e; }
                if (v == 1) { e->op = CB_NE; r->value = 0; return e; }
                break;
            case CB_LE:
                if (v == 0) { e->op = CB_EQ; return e; }
                if (v == ~0ULL && !has_call(l)) { e->op = CB_CONST; e->value = 1; return e; }
                break;
            case CB_GT:
                if (v == 0) { e->op = CB_NE; return e; }
                if (v == ~0ULL && !has_call(l)) { e->op = CB_CONST; e->value = 0; return e; }
                break;
            default: break;
            }
        }
        if (l->op == CB_CONST && l->value == 0 && !has_call(r)) {
            if (e->op == CB_DIV || e->op == CB_MOD || e->op == CB_SHL || e->op == CB_SHR) {
                e->op = CB_CONST; e->value = 0; return e;
            }
        }
        if (!has_call(l) && expr_equals(l, r)) {
            switch (e->op) {
            case CB_SUB: case CB_XOR: case CB_NE: case CB_LT: case CB_GT:
                e->op = CB_CONST; e->value = 0; return e;
            case CB_EQ: case CB_LE: case CB_GE:
                e->op = CB_CONST; e->value = 1; return e;
            case CB_AND: case CB_OR: return l;
            default: break;
            }
        }
    }
    return e;
}

static void shift_locals(IrExpr *e, int th, int d) {
    if (!e) return;
    if ((e->op == CB_VAR || e->op == CB_LET) && e->index >= th) e->index += d;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) shift_locals(e->arg[i], th, d);
}

static int is_assoc_op(CbOp op, uint64_t *id_out) {
    switch (op) {
    case CB_ADD: case CB_OR: case CB_XOR: *id_out = 0; return 1;
    case CB_MUL: *id_out = 1; return 1;
    case CB_AND: *id_out = ~0ULL; return 1;
    default: return 0;
    }
}

static void convert_linear_rec(IrProg *p, IrFunc *fn, int fn_idx) {
    if (fn->arity >= IR_MAX_PARAMS || fn->locals + 2 >= IR_MAX_LOCALS || p->nodes + 32 >= IR_MAX_NODES) return;
    IrExpr *cur = fn->body;
    while (cur && cur->op == CB_LET) {
        if (has_call_to(cur->arg[0], fn_idx)) return;
        cur = cur->arg[1];
    }
    if (!cur || cur->op != CB_IF || has_call_to(cur->arg[0], fn_idx)) return;
    int rside = -1;
    if (!has_call_to(cur->arg[1], fn_idx) && has_call_to(cur->arg[2], fn_idx)) rside = 2;
    else if (!has_call_to(cur->arg[2], fn_idx) && has_call_to(cur->arg[1], fn_idx)) rside = 1;
    else return;
    int eside = (rside == 1) ? 2 : 1;
    IrExpr **rptr = &cur->arg[rside];
    while (*rptr && (*rptr)->op == CB_LET) {
        if (has_call_to((*rptr)->arg[0], fn_idx)) return;
        rptr = &(*rptr)->arg[1];
    }
    IrExpr *rn = *rptr;
    uint64_t idv = 0;
    if (!rn || !is_assoc_op(rn->op, &idv)) return;
    IrExpr *call = NULL, *step = NULL;
    if (rn->arg[0]->op == CB_CALL && rn->arg[0]->index == fn_idx) { call = rn->arg[0]; step = rn->arg[1]; }
    else if (rn->arg[1]->op == CB_CALL && rn->arg[1]->index == fn_idx) { call = rn->arg[1]; step = rn->arg[0]; }
    else return;
    for (int i = 0; i < call->argc; ++i) if (has_call_to(call->arg[i], fn_idx)) return;

    int acc = fn->orig_arity;
    shift_locals(fn->body, acc, 1);
    fn->arity++; fn->locals++;
    fn->has_acc_init = 1; fn->acc_init_val = idv;
    cur->arg[eside] = new_binop(p, rn->op, new_var(p, acc), cur->arg[eside]);
    call->arg[call->argc++] = new_binop(p, rn->op, new_var(p, acc), step);
    *rptr = call;
}

static IrExpr *normalize_tail(IrProg *p, IrFunc *fn, int fn_idx, IrExpr *e) {
    if (!e) return NULL;
    if (e->op == CB_LET) {
        e->arg[1] = normalize_tail(p, fn, fn_idx, e->arg[1]);
        if (e->arg[1]->op == CB_CALL && e->arg[1]->index == fn_idx && !has_call(e->arg[0])) {
            IrExpr *c = e->arg[1];
            if (count_var_uses(c, e->index) == 1) {
                for (int i = 0; i < c->argc; ++i) {
                    if (c->arg[i]->op == CB_VAR && c->arg[i]->index == e->index) {
                        c->arg[i] = e->arg[0];
                        return normalize_tail(p, fn, fn_idx, c);
                    }
                }
            }
        }
        return e;
    }
    if (e->op == CB_IF) {
        e->arg[1] = normalize_tail(p, fn, fn_idx, e->arg[1]);
        e->arg[2] = normalize_tail(p, fn, fn_idx, e->arg[2]);
        IrExpr *t = e->arg[1], *f = e->arg[2];
        if (t->op == CB_CALL && t->index == fn_idx && f->op == CB_CALL && f->index == fn_idx &&
            t->argc == f->argc && !has_call(e->arg[0]) && p->nodes + 16 < IR_MAX_NODES) {
            int diffs = 0;
            for (int i = 0; i < t->argc; ++i) {
                if (has_call(t->arg[i]) || has_call(f->arg[i])) { diffs = 99; break; }
                if (!expr_equals(t->arg[i], f->arg[i])) diffs++;
            }
            if (diffs == 1) {
                for (int i = 0; i < t->argc; ++i)
                    if (!expr_equals(t->arg[i], f->arg[i]))
                        t->arg[i] = new_if(p, e->arg[0], t->arg[i], f->arg[i]);
                return normalize_tail(p, fn, fn_idx, t);
            }
        }
        return e;
    }
    if (e->op == CB_CALL && e->index == fn_idx && p->nodes + 16 < IR_MAX_NODES) {
        for (int k = 0; k < e->argc; ++k) {
            IrExpr *ak = e->arg[k];
            if (ak->op == CB_IF && !has_call(ak)) {
                IrExpr *c = ak->arg[0], *t = ak->arg[1], *f = ak->arg[2];
                uint64_t idv = 0;
                if (f->op == CB_VAR && f->index == k && is_assoc_op(t->op, &idv)) {
                    IrExpr *rhs = (t->arg[0]->op == CB_VAR && t->arg[0]->index == k) ? t->arg[1] :
                                  ((t->arg[1]->op == CB_VAR && t->arg[1]->index == k) ? t->arg[0] : NULL);
                    if (rhs) e->arg[k] = new_binop(p, t->op, new_var(p, k), new_if(p, c, rhs, new_const(p, idv)));
                } else if (t->op == CB_VAR && t->index == k && is_assoc_op(f->op, &idv)) {
                    IrExpr *rhs = (f->arg[0]->op == CB_VAR && f->arg[0]->index == k) ? f->arg[1] :
                                  ((f->arg[1]->op == CB_VAR && f->arg[1]->index == k) ? f->arg[0] : NULL);
                    if (rhs) e->arg[k] = new_binop(p, f->op, new_var(p, k), new_if(p, c, new_const(p, idv), rhs));
                } else if (t->op == f->op && is_assoc_op(t->op, &idv)) {
                    IrExpr *rt = (t->arg[0]->op == CB_VAR && t->arg[0]->index == k) ? t->arg[1] :
                                 ((t->arg[1]->op == CB_VAR && t->arg[1]->index == k) ? t->arg[0] : NULL);
                    IrExpr *rf = (f->arg[0]->op == CB_VAR && f->arg[0]->index == k) ? f->arg[1] :
                                 ((f->arg[1]->op == CB_VAR && f->arg[1]->index == k) ? f->arg[0] : NULL);
                    if (rt && rf) e->arg[k] = new_binop(p, t->op, new_var(p, k), new_if(p, c, rt, rf));
                }
            }
        }
    }
    return e;
}

static int check_nontail_calls(const IrExpr *e, int in_tail) {
    if (!e) return 0;
    if (e->op == CB_IF) {
        return has_call(e->arg[0]) || check_nontail_calls(e->arg[1], in_tail) || check_nontail_calls(e->arg[2], in_tail);
    }
    if (e->op == CB_LET) {
        return has_call(e->arg[0]) || check_nontail_calls(e->arg[1], in_tail);
    }
    if (e->op == CB_CALL) {
        if (!in_tail) return 1;
        for (int i = 0; i < e->argc; ++i) if (has_call(e->arg[i])) return 1;
        return 0;
    }
    return has_call(e);
}

static int uses_rdx_op(const IrExpr *e) {
    if (!e) return 0;
    if (e->op == CB_DIV || e->op == CB_MOD || e->op == CB_CALL) return 1;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) if (uses_rdx_op(e->arg[i])) return 1;
    return 0;
}

static int is_simple_op(const IrExpr *e) {
    return e->op == CB_CONST || e->op == CB_VAR || e->op == CB_LEN;
}

static void sim_temps(const IrExpr *e, int xdepth, int sdepth, int rbusy, int *max_s) {
    if (!e) return;
    if (sdepth > *max_s) *max_s = sdepth;
    if (e->op == CB_IF) {
        sim_temps(e->arg[0], xdepth, sdepth, rbusy, max_s);
        sim_temps(e->arg[1], xdepth, sdepth, rbusy, max_s);
        sim_temps(e->arg[2], xdepth, sdepth, rbusy, max_s);
        return;
    }
    if (e->op == CB_LET) {
        sim_temps(e->arg[0], xdepth, sdepth, rbusy, max_s);
        sim_temps(e->arg[1], xdepth, sdepth, rbusy, max_s);
        return;
    }
    if (e->op == CB_GET) {
        sim_temps(e->arg[0], xdepth, sdepth, rbusy, max_s);
        return;
    }
    if (e->op == CB_CALL) {
        int cur_x = xdepth, cur_s = sdepth;
        for (int i = 0; i < e->argc; ++i) {
            sim_temps(e->arg[i], cur_x, cur_s, rbusy, max_s);
            if (i + 1 < e->argc) {
                int rem_call = 0;
                for (int j = i + 1; j < e->argc; ++j) if (has_call(e->arg[j])) rem_call = 1;
                if (!rem_call && cur_x < 8) cur_x++;
                else { cur_s++; if (cur_s > *max_s) *max_s = cur_s; }
            }
        }
        return;
    }
    if (e->op >= CB_ADD && e->op <= CB_GE) {
        if (is_simple_op(e->arg[1])) { sim_temps(e->arg[0], xdepth, sdepth, rbusy, max_s); return; }
        if (is_simple_op(e->arg[0])) { sim_temps(e->arg[1], xdepth, sdepth, rbusy, max_s); return; }
        sim_temps(e->arg[0], xdepth, sdepth, rbusy, max_s);
        if (!has_call(e->arg[1]) && !uses_rdx_op(e->arg[1]) && !rbusy) {
            sim_temps(e->arg[1], xdepth, sdepth, 1, max_s);
        } else if (!has_call(e->arg[1]) && xdepth < 8) {
            sim_temps(e->arg[1], xdepth + 1, sdepth, rbusy, max_s);
        } else {
            if (sdepth + 1 > *max_s) *max_s = sdepth + 1;
            sim_temps(e->arg[1], xdepth, sdepth + 1, rbusy, max_s);
        }
    }
}

static int is_idx_plus_const(const IrExpr *e, int idx_var, int *off_out) {
    if (e->op == CB_VAR && e->index == idx_var) { *off_out = 0; return 1; }
    if (e->op == CB_ADD && e->arg[0]->op == CB_VAR && e->arg[0]->index == idx_var &&
        e->arg[1]->op == CB_CONST && e->arg[1]->value <= 16) {
        *off_out = (int)e->arg[1]->value;
        return 1;
    }
    return 0;
}

static int check_gets_and_calls(const IrExpr *e, int idx_var, int *has_gets, int *max_off) {
    if (!e) return 1;
    if (e->op == CB_CALL) return 0;
    if (e->op == CB_GET) {
        int off = 0;
        if (!is_idx_plus_const(e->arg[0], idx_var, &off)) return 0;
        *has_gets = 1;
        if (off > *max_off) *max_off = off;
        return 1;
    }
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i)
        if (!check_gets_and_calls(e->arg[i], idx_var, has_gets, max_off)) return 0;
    return 1;
}

static int check_all_tail_steps(const IrExpr *e, int fn_idx, int idx_var, int bound_var) {
    if (!e) return 0;
    if (e->op == CB_LET) {
        if (has_call(e->arg[0])) return 0;
        return check_all_tail_steps(e->arg[1], fn_idx, idx_var, bound_var);
    }
    if (e->op == CB_IF) {
        if (has_call(e->arg[0])) return 0;
        return check_all_tail_steps(e->arg[1], fn_idx, idx_var, bound_var) &&
               check_all_tail_steps(e->arg[2], fn_idx, idx_var, bound_var);
    }
    if (e->op == CB_CALL && e->index == fn_idx) {
        for (int i = 0; i < e->argc; ++i) if (has_call(e->arg[i])) return 0;
        int off = 0;
        if (!is_idx_plus_const(e->arg[idx_var], idx_var, &off) || off != 1) return 0;
        if (bound_var >= 0 && !(e->arg[bound_var]->op == CB_VAR && e->arg[bound_var]->index == bound_var))
            return 0;
        return 1;
    }
    return 0;
}

static int check_avx2_tree(const IrExpr *e, const LoopInfo *L, uint64_t acc_mask, int *depth_out) {
    if (!e) return 0;
    switch (e->op) {
    case CB_CONST: case CB_LEN: *depth_out = 1; return 1;
    case CB_VAR:
        if (e->index < 64 && (acc_mask & (1ULL << e->index))) return 0;
        if (e->index >= 0) {
            for (int i = 0; i < L->num_lets; ++i) {
                if (L->let_slots[i] == e->index) {
                    return check_avx2_tree(L->let_vals[i], L, acc_mask, depth_out);
                }
            }
        }
        *depth_out = 1;
        return 1;
    case CB_GET: {
        int off = 0;
        if (!is_idx_plus_const(e->arg[0], L->idx_param, &off)) return 0;
        *depth_out = 1;
        return 1;
    }
    case CB_ADD: case CB_SUB: case CB_MUL: case CB_AND: case CB_OR: case CB_XOR:
    case CB_SHL: case CB_SHR: case CB_EQ: case CB_NE: case CB_LT: case CB_LE: case CB_GT: case CB_GE: {
        int d0 = 0, d1 = 0;
        if (!check_avx2_tree(e->arg[0], L, acc_mask, &d0)) return 0;
        if ((e->op == CB_SHL || e->op == CB_SHR) && e->arg[1]->op == CB_CONST) {
            *depth_out = d0;
            return 1;
        }
        if (!check_avx2_tree(e->arg[1], L, acc_mask, &d1)) return 0;
        int d = d0 > d1 + 1 ? d0 : d1 + 1;
        *depth_out = d;
        return d <= 6;
    }
    case CB_IF: {
        int dc = 0, dt = 0, df = 0;
        if (!check_avx2_tree(e->arg[0], L, acc_mask, &dc) ||
            !check_avx2_tree(e->arg[1], L, acc_mask, &dt) ||
            !check_avx2_tree(e->arg[2], L, acc_mask, &df)) return 0;
        int d = dc;
        if (dt + 1 > d) d = dt + 1;
        if (df + 2 > d) d = df + 2;
        *depth_out = d;
        return d <= 6;
    }
    default:
        return 0;
    }
}

static RedKind match_reduction(IrExpr *ak, int param_k, IrExpr **rhs_out) {
    uint64_t idv = 0;
    if (is_assoc_op(ak->op, &idv)) {
        IrExpr *rhs = NULL;
        if (ak->arg[0]->op == CB_VAR && ak->arg[0]->index == param_k) rhs = ak->arg[1];
        else if (ak->arg[1]->op == CB_VAR && ak->arg[1]->index == param_k) rhs = ak->arg[0];
        if (rhs) {
            *rhs_out = rhs;
            switch (ak->op) {
            case CB_ADD: return RED_ADD;
            case CB_XOR: return RED_XOR;
            case CB_OR:  return RED_OR;
            case CB_AND: return RED_AND;
            case CB_MUL: return RED_MUL;
            default: break;
            }
        }
    }
    if (ak->op == CB_IF && is_cmp(ak->arg[0]->op)) {
        IrExpr *c = ak->arg[0], *t = ak->arg[1], *f = ak->arg[2];
        IrExpr *other = NULL;
        int acc_when_true = 0;
        if (f->op == CB_VAR && f->index == param_k) { other = t; acc_when_true = 0; }
        else if (t->op == CB_VAR && t->index == param_k) { other = f; acc_when_true = 1; }
        if (other) {
            CbOp cop = acc_when_true ? neg_cmp(c->op) : c->op;
            IrExpr *cl = c->arg[0], *cr = c->arg[1];
            if (cl->op == CB_VAR && cl->index == param_k && expr_equals(cr, other)) {
                *rhs_out = other;
                if (cop == CB_LT || cop == CB_LE) return RED_MAX;
                if (cop == CB_GT || cop == CB_GE) return RED_MIN;
            } else if (cr->op == CB_VAR && cr->index == param_k && expr_equals(cl, other)) {
                *rhs_out = other;
                if (cop == CB_GT || cop == CB_GE) return RED_MAX;
                if (cop == CB_LT || cop == CB_LE) return RED_MIN;
            }
        }
    }
    return RED_NONE;
}

static void analyze_loop(const IrFunc *fn, int fn_idx, LoopInfo *L) {
    memset(L, 0, sizeof(*L));
    if (fn->has_nontail_call || fn->locals > MODE_A_REGS_CNT || !fn->body || fn->body->op != CB_IF) return;
    IrExpr *cond = fn->body->arg[0];
    IrExpr *b1 = fn->body->arg[1], *b2 = fn->body->arg[2];
    int c1 = has_call_to(b1, fn_idx), c2 = has_call_to(b2, fn_idx);
    if ((c1 && c2) || (!c1 && !c2)) return;
    int cont_on_true = c1 ? 1 : 0;
    L->cont_br = c1 ? b1 : b2;
    L->exit_br = c1 ? b2 : b1;
    if (has_call(L->exit_br) || ! is_cmp(cond->op)) return;

    CbOp cop = cont_on_true ? cond->op : neg_cmp(cond->op);
    IrExpr *lhs = cond->arg[0], *rhs = cond->arg[1];
    if (lhs->op != CB_VAR && rhs->op == CB_VAR) {
        cop = swap_cmp(cop);
        lhs = cond->arg[1]; rhs = cond->arg[0];
    }
    if (lhs->op != CB_VAR || lhs->index >= fn->arity) return;
    int idx_p = lhs->index;
    int bkind = -1, bvar = -1;
    uint64_t bval = 0;

    if (cop == CB_LT || cop == CB_NE) {
        if (rhs->op == CB_LEN) bkind = 0;
        else if (rhs->op == CB_CONST) { bkind = 1; bval = rhs->value; }
        else if (rhs->op == CB_VAR && rhs->index < fn->arity && rhs->index != idx_p) { bkind = 2; bvar = rhs->index; }
    } else if (cop == CB_LE && rhs->op == CB_CONST && rhs->value < ~0ULL) {
        bkind = 1; bval = rhs->value + 1;
    }
    if (bkind < 0) return;

    int has_gets = 0, max_off = 0;
    if (!check_gets_and_calls(L->cont_br, idx_p, &has_gets, &max_off)) {
        /* cont_br has tail call(s); verify all non-tail subexpressions */
    }
    if (!check_all_tail_steps(L->cont_br, fn_idx, idx_p, bvar)) return;

    /* Check all CB_GET in cont_br are idx_p + offset */
    has_gets = 0; max_off = 0;
    IrExpr *cur = L->cont_br;
    while (cur && cur->op == CB_LET) {
        if (!check_gets_and_calls(cur->arg[0], idx_p, &has_gets, &max_off)) return;
        if (L->num_lets < 64) {
            L->let_slots[L->num_lets] = cur->index;
            L->let_vals[L->num_lets] = cur->arg[0];
            L->num_lets++;
        } else return;
        cur = cur->arg[1];
    }
    /* Check gets in tail tree */
    if (cur->op == CB_CALL && cur->index == fn_idx) {
        for (int i = 0; i < cur->argc; ++i)
            if (!check_gets_and_calls(cur->arg[i], idx_p, &has_gets, &max_off)) return;
        L->tail_call = cur;
    } else {
        /* Branchy tail calls: still eligible for 4x unroll if gets are valid */
        if (ast_size(L->cont_br) > 60) return;
        /* Check gets recursively ignoring tail CB_CALL node itself */
        return;
    }

    L->valid = 1;
    L->idx_param = idx_p;
    L->bound_kind = bkind;
    L->bound_val = bval;
    L->bound_var = bvar;
    L->has_gets = has_gets;
    L->max_get_offset = max_off;

    /* Now check if AVX2 vectorizable */
    uint64_t acc_mask = 0;
    for (int k = 0; k < fn->arity; ++k) {
        if (k == idx_p) continue;
        IrExpr *ak = L->tail_call->arg[k];
        if (ak->op == CB_VAR && ak->index == k) continue;
        IrExpr *rhs_e = NULL;
        RedKind rk = match_reduction(ak, k, &rhs_e);
        if (rk == RED_NONE || L->acc_count >= 3) { L->acc_count = 0; return; }
        L->acc_param[L->acc_count] = k;
        L->acc_kind[L->acc_count] = rk;
        L->acc_rhs[L->acc_count] = rhs_e;
        L->acc_count++;
        acc_mask |= (1ULL << k);
    }
    if (L->acc_count == 0) return;
    for (int a = 0; a < L->acc_count; ++a) {
        int d = 0;
        if (!check_avx2_tree(L->acc_rhs[a], L, acc_mask, &d)) return;
    }
    L->is_avx2 = 1;
}

static const char *local_reg(const Emit *s, int slot) {
    if (!s->has_nontail) {
        return (slot >= 0 && slot < MODE_A_REGS_CNT) ? MODE_A_REGS[slot] : NULL;
    } else {
        return (slot >= 0 && slot < MODE_B_REGS_CNT) ? MODE_B_REGS[slot] : NULL;
    }
}

static int local_stack_offset(const Emit *s, int slot) {
    int base = s->has_nontail ? MODE_B_REGS_CNT : MODE_A_REGS_CNT;
    return -8 * (s->num_callee_saved + 1 + (slot - base));
}

static int temp_stack_offset(const Emit *s, int tslot) {
    int base = s->has_nontail ? MODE_B_REGS_CNT : MODE_A_REGS_CNT;
    int spilled_locals = s->fn->locals > base ? (s->fn->locals - base) : 0;
    return -8 * (s->num_callee_saved + 1 + spilled_locals + tslot);
}

static void emit_load_const(FILE *f, uint64_t val, const char *reg64, const char *reg32) {
    if (val == 0) {
        fprintf(f, "  xorl %s, %s\n", reg32, reg32);
    } else if (val <= 0xFFFFFFFFULL) {
        fprintf(f, "  movl $0x%" PRIx64 ", %s\n", val, reg32);
    } else if ((int64_t)val >= -2147483648LL && (int64_t)val <= 2147483647LL) {
        fprintf(f, "  movq $%" PRId64 ", %s\n", (int64_t)val, reg64);
    } else {
        fprintf(f, "  movabsq $0x%" PRIx64 ", %s\n", val, reg64);
    }
}

static void emit_load_local(const Emit *s, int slot, const char *dst) {
    const char *r = local_reg(s, slot);
    if (r) {
        if (strcmp(r, dst) != 0) fprintf(s->out, "  movq %s, %s\n", r, dst);
    } else {
        fprintf(s->out, "  movq %d(%%rbp), %s\n", local_stack_offset(s, slot), dst);
    }
}

static void emit_store_local(const Emit *s, int slot, const char *src) {
    const char *r = local_reg(s, slot);
    if (r) {
        if (strcmp(r, src) != 0) fprintf(s->out, "  movq %s, %s\n", src, r);
    } else {
        fprintf(s->out, "  movq %s, %d(%%rbp)\n", src, local_stack_offset(s, slot));
    }
}

static void emit_epilogue(const Emit *s) {
    FILE *f = s->out;
    if (s->frame_bytes > 0) fprintf(f, "  addq $%d, %%rsp\n", s->frame_bytes);
    for (int i = s->num_callee_saved - 1; i >= 0; --i) {
        static const char *const CS_NAMES[] = {"%rbp", "%rbx", "%r12", "%r13", "%r14", "%r15"};
        fprintf(f, "  popq %s\n", CS_NAMES[s->callee_saved_regs[i]]);
    }
}

static void emit_div_mod_const(Emit *s, uint64_t d, int is_mod) {
    FILE *f = s->out;
    if (d == 0) {
        fprintf(f, "  xorl %%eax, %%eax\n");
        return;
    }
    if (d == 1) {
        if (is_mod) fprintf(f, "  xorl %%eax, %%eax\n");
        return;
    }
    if ((d & (d - 1)) == 0) {
        int sh = __builtin_ctzll(d);
        if (is_mod) {
            if (d - 1 <= 0x7FFFFFFFULL) fprintf(f, "  andq $%" PRIu64 ", %%rax\n", d - 1);
            else { emit_load_const(f, d - 1, "%rcx", "%ecx"); fprintf(f, "  andq %%rcx, %%rax\n"); }
        } else {
            fprintf(f, "  shrq $%d, %%rax\n", sh);
        }
        return;
    }
    if (d >> 63) {
        emit_load_const(f, d, "%rcx", "%ecx");
        if (is_mod) {
            fprintf(f, "  movq %%rax, %%rdx\n  subq %%rcx, %%rdx\n  cmpq %%rcx, %%rax\n  cmovaeq %%rdx, %%rax\n");
        } else {
            fprintf(f, "  cmpq %%rcx, %%rax\n  setae %%al\n  movzbq %%al, %%rax\n");
        }
        return;
    }
    int l = 63 - __builtin_clzll(d);
    __uint128_t one = 1;
    __uint128_t q1 = (one << (64 + l)) / d + 1;
    __uint128_t rem = q1 * d - (one << (64 + l));
    if (is_mod) fprintf(f, "  movq %%rax, %%rcx\n");
    if ((q1 >> 64) == 0 && rem <= (one << l)) {
        uint64_t m = (uint64_t)q1;
        emit_load_const(f, m, "%rdx", "%edx");
        fprintf(f, "  mulq %%rdx\n  movq %%rdx, %%rax\n");
        if (l > 0) fprintf(f, "  shrq $%d, %%rax\n", l);
    } else {
        __uint128_t q2 = (one << (64 + l + 1)) / d + 1;
        uint64_t m = (uint64_t)q2;
        if (!is_mod) fprintf(f, "  movq %%rax, %%rcx\n");
        emit_load_const(f, m, "%rdx", "%edx");
        fprintf(f, "  mulq %%rdx\n  movq %%rcx, %%rax\n  subq %%rdx, %%rax\n"
                   "  shrq $1, %%rax\n  addq %%rdx, %%rax\n");
        if (l > 0) fprintf(f, "  shrq $%d, %%rax\n", l);
    }
    if (is_mod) {
        emit_load_const(f, d, "%rdx", "%edx");
        fprintf(f, "  imulq %%rdx, %%rax\n  subq %%rax, %%rcx\n  movq %%rcx, %%rax\n");
    }
}

static void emit_expr(Emit *s, const IrExpr *e);

static const char *get_simple_op(Emit *s, const IrExpr *e, char *buf, size_t bsz) {
    if (e->op == CB_LEN) return "%rsi";
    if (e->op == CB_VAR) {
        const char *r = local_reg(s, e->index);
        if (r) return r;
        snprintf(buf, bsz, "%d(%%rbp)", local_stack_offset(s, e->index));
        return buf;
    }
    if (e->op == CB_CONST) {
        int64_t sv = (int64_t)e->value;
        if (sv >= -2147483648LL && sv <= 2147483647LL) {
            snprintf(buf, bsz, "$%" PRId64, sv);
            return buf;
        }
        emit_load_const(s->out, e->value, "%rcx", "%ecx");
        return "%rcx";
    }
    return NULL;
}

static void finish_binop_rhs(Emit *s, CbOp op, const char *rhs_op, const IrExpr *rhs_node) {
    FILE *f = s->out;
    unsigned label = s->label++;
    switch (op) {
    case CB_ADD: fprintf(f, "  addq %s, %%rax\n", rhs_op); break;
    case CB_SUB: fprintf(f, "  subq %s, %%rax\n", rhs_op); break;
    case CB_MUL:
        if (rhs_node && rhs_node->op == CB_CONST) {
            int64_t sv = (int64_t)rhs_node->value;
            if (sv >= -2147483648LL && sv <= 2147483647LL) {
                fprintf(f, "  imulq $%" PRId64 ", %%rax, %%rax\n", sv);
                break;
            }
        }
        if (rhs_op[0] == '$') {
            fprintf(f, "  imulq %s, %%rax, %%rax\n", rhs_op);
        } else {
            fprintf(f, "  imulq %s, %%rax\n", rhs_op);
        }
        break;
    case CB_AND: fprintf(f, "  andq %s, %%rax\n", rhs_op); break;
    case CB_OR:  fprintf(f, "  orq %s, %%rax\n", rhs_op); break;
    case CB_XOR: fprintf(f, "  xorq %s, %%rax\n", rhs_op); break;
    case CB_SHL:
        if (rhs_node && rhs_node->op == CB_CONST) {
            fprintf(f, "  shlq $%u, %%rax\n", (unsigned)(rhs_node->value & 63));
        } else {
            if (strcmp(rhs_op, "%rcx") != 0) fprintf(f, "  movq %s, %%rcx\n", rhs_op);
            fprintf(f, "  shlq %%cl, %%rax\n");
        }
        break;
    case CB_SHR:
        if (rhs_node && rhs_node->op == CB_CONST) {
            fprintf(f, "  shrq $%u, %%rax\n", (unsigned)(rhs_node->value & 63));
        } else {
            if (strcmp(rhs_op, "%rcx") != 0) fprintf(f, "  movq %s, %%rcx\n", rhs_op);
            fprintf(f, "  shrq %%cl, %%rax\n");
        }
        break;
    case CB_DIV:
    case CB_MOD:
        if (rhs_node && rhs_node->op == CB_CONST) {
            emit_div_mod_const(s, rhs_node->value, op == CB_MOD);
        } else {
            if (strcmp(rhs_op, "%rcx") != 0) fprintf(f, "  movq %s, %%rcx\n", rhs_op);
            fprintf(f, "  testq %%rcx, %%rcx\n  jz .Lzero%u\n  xorl %%edx, %%edx\n  divq %%rcx\n", label);
            if (op == CB_MOD) fprintf(f, "  movq %%rdx, %%rax\n");
            fprintf(f, "  jmp .Ldone%u\n.Lzero%u:\n  xorl %%eax, %%eax\n.Ldone%u:\n", label, label, label);
        }
        break;
    default: {
        static const char *const conds[] = {"e", "ne", "b", "be", "a", "ae"};
        fprintf(f, "  cmpq %s, %%rax\n  set%s %%al\n  movzbq %%al, %%rax\n", rhs_op, conds[op - CB_EQ]);
        break;
    }
    }
}

static void emit_branch_if_false(Emit *s, const IrExpr *cond, unsigned false_label) {
    FILE *f = s->out;
    if (is_cmp(cond->op)) {
        static const char *const inv_jcc[] = {"jne", "je", "jae", "ja", "jbe", "jb"};
        const IrExpr *l = cond->arg[0], *r = cond->arg[1];
        if (is_simple_op(r)) {
            emit_expr(s, l);
            char buf[64];
            const char *rop = get_simple_op(s, r, buf, sizeof(buf));
            fprintf(f, "  cmpq %s, %%rax\n  %s .L%u\n", rop, inv_jcc[cond->op - CB_EQ], false_label);
            return;
        }
    }
    emit_expr(s, cond);
    fprintf(f, "  testq %%rax, %%rax\n  jz .L%u\n", false_label);
}

static void emit_call_args_and_invoke(Emit *s, const IrExpr *e, int is_tail) {
    FILE *f = s->out;
    int argc = e->argc;
    int saved_kind[IR_MAX_PARAMS];
    int saved_slot[IR_MAX_PARAMS];
    for (int i = 0; i < argc; ++i) {
        emit_expr(s, e->arg[i]);
        if (i + 1 < argc) {
            int rem_call = 0;
            for (int j = i + 1; j < argc; ++j) if (has_call(e->arg[j])) rem_call = 1;
            if (!rem_call && s->xmm_depth < 8) {
                saved_kind[i] = 0;
                saved_slot[i] = s->xmm_depth++;
                fprintf(f, "  vmovq %%rax, %%xmm%d\n", 8 + saved_slot[i]);
            } else {
                saved_kind[i] = 1;
                saved_slot[i] = s->stack_depth++;
                fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_stack_offset(s, saved_slot[i]));
            }
        }
    }
    if (argc > 0) {
        fprintf(f, "  movq %%rax, %s\n", ARG_REGS[argc - 1]);
    }
    for (int i = argc - 2; i >= 0; --i) {
        if (saved_kind[i] == 0) {
            fprintf(f, "  vmovq %%xmm%d, %s\n", 8 + saved_slot[i], ARG_REGS[i]);
            s->xmm_depth--;
        } else {
            fprintf(f, "  movq %d(%%rbp), %s\n", temp_stack_offset(s, saved_slot[i]), ARG_REGS[i]);
            s->stack_depth--;
        }
    }
    if (is_tail) {
        emit_epilogue(s);
        fprintf(f, "  jmp .Lfn%d\n", e->index);
    } else {
        fprintf(f, "  call .Lfn%d\n", e->index);
    }
}

static void emit_expr_to_local(Emit *s, const IrExpr *e, int dst_slot) {
    const char *dreg = local_reg(s, dst_slot);
    if (dreg && (e->op == CB_ADD || e->op == CB_SUB || e->op == CB_XOR || e->op == CB_OR || e->op == CB_AND) &&
        e->arg[0]->op == CB_VAR && e->arg[0]->index == dst_slot && is_simple_op(e->arg[1])) {
        char buf[64];
        const char *rop = get_simple_op(s, e->arg[1], buf, sizeof(buf));
        const char *ins = e->op == CB_ADD ? "addq" : e->op == CB_SUB ? "subq" :
                          e->op == CB_XOR ? "xorq" : e->op == CB_OR ? "orq" : "andq";
        fprintf(s->out, "  %s %s, %s\n", ins, rop, dreg);
        return;
    }
    if (dreg && (e->op == CB_ADD || e->op == CB_SUB || e->op == CB_XOR || e->op == CB_OR || e->op == CB_AND) &&
        e->arg[0]->op == CB_VAR && e->arg[0]->index == dst_slot && count_var_uses(e->arg[1], dst_slot) == 0 &&
        !has_call(e->arg[1])) {
        emit_expr(s, e->arg[1]);
        const char *ins = e->op == CB_ADD ? "addq" : e->op == CB_SUB ? "subq" :
                          e->op == CB_XOR ? "xorq" : e->op == CB_OR ? "orq" : "andq";
        fprintf(s->out, "  %s %%rax, %s\n", ins, dreg);
        return;
    }
    emit_expr(s, e);
    emit_store_local(s, dst_slot, "%rax");
}

static void emit_self_tail_call(Emit *s, const IrExpr *e) {
    FILE *f = s->out;
    int argc = e->argc;
    int done[IR_MAX_PARAMS] = {0};
    uint8_t reads[IR_MAX_PARAMS][IR_MAX_PARAMS];
    memset(reads, 0, sizeof(reads));
    for (int i = 0; i < argc; ++i) {
        if (e->arg[i]->op == CB_VAR && e->arg[i]->index == i) {
            done[i] = 1;
        } else {
            for (int j = 0; j < argc; ++j)
                if (count_var_uses(e->arg[i], j) > 0) reads[i][j] = 1;
        }
    }
    while (1) {
        int progress = 0;
        for (int i = 0; i < argc; ++i) {
            if (done[i]) continue;
            int needed = 0;
            for (int m = 0; m < argc; ++m) {
                if (!done[m] && m != i && reads[m][i]) { needed = 1; break; }
            }
            if (!needed) {
                emit_expr_to_local(s, e->arg[i], i);
                done[i] = 1;
                progress = 1;
            }
        }
        if (progress) continue;
        int rem = 0;
        for (int i = 0; i < argc; ++i) if (!done[i]) rem++;
        if (rem == 0) break;
        /* Break cycle by evaluating remaining into xmm or stack temps */
        int skind[IR_MAX_PARAMS], sslot[IR_MAX_PARAMS];
        for (int i = 0; i < argc; ++i) {
            if (done[i]) continue;
            emit_expr(s, e->arg[i]);
            int rem_call = 0;
            for (int j = i + 1; j < argc; ++j) if (!done[j] && has_call(e->arg[j])) rem_call = 1;
            if (!rem_call && s->xmm_depth < 8) {
                skind[i] = 0; sslot[i] = s->xmm_depth++;
                fprintf(f, "  vmovq %%rax, %%xmm%d\n", 8 + sslot[i]);
            } else {
                skind[i] = 1; sslot[i] = s->stack_depth++;
                fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_stack_offset(s, sslot[i]));
            }
        }
        for (int i = argc - 1; i >= 0; --i) {
            if (done[i]) continue;
            if (skind[i] == 0) {
                const char *r = local_reg(s, i);
                if (r) fprintf(f, "  vmovq %%xmm%d, %s\n", 8 + sslot[i], r);
                else { fprintf(f, "  vmovq %%xmm%d, %%rax\n", 8 + sslot[i]); emit_store_local(s, i, "%rax"); }
                s->xmm_depth--;
            } else {
                fprintf(f, "  movq %d(%%rbp), %%rax\n", temp_stack_offset(s, sslot[i]));
                emit_store_local(s, i, "%rax");
                s->stack_depth--;
            }
            done[i] = 1;
        }
        break;
    }
    if (s->unroll_step_label >= 0) {
        fprintf(f, "  jmp .L%d\n", s->unroll_step_label);
    } else {
        fprintf(f, "  jmp .Lloop%d\n", s->fn_idx);
    }
}

static void emit_expr(Emit *s, const IrExpr *e) {
    FILE *f = s->out;
    unsigned label = s->label++;
    switch (e->op) {
    case CB_CONST:
        emit_load_const(f, e->value, "%rax", "%eax");
        return;
    case CB_VAR:
        emit_load_local(s, e->index, "%rax");
        return;
    case CB_LEN:
        fprintf(f, "  movq %%rsi, %%rax\n");
        return;
    case CB_GET: {
        const IrExpr *idx = e->arg[0];
        if (s->all_gets_in_bounds) {
            int off = 0;
            if (idx->op == CB_VAR && local_reg(s, idx->index)) {
                fprintf(f, "  movq (%%rdi,%s,8), %%rax\n", local_reg(s, idx->index));
                return;
            }
            if (idx->op == CB_ADD && idx->arg[0]->op == CB_VAR && local_reg(s, idx->arg[0]->index) &&
                idx->arg[1]->op == CB_CONST && idx->arg[1]->value <= 0x0FFFFFFFULL) {
                off = (int)(idx->arg[1]->value * 8);
                fprintf(f, "  movq %d(%%rdi,%s,8), %%rax\n", off, local_reg(s, idx->arg[0]->index));
                return;
            }
            emit_expr(s, idx);
            fprintf(f, "  movq (%%rdi,%%rax,8), %%rax\n");
            return;
        }
        if (idx->op == CB_VAR && idx->index >= 0 && idx->index < 64 &&
            (s->known_lt_len & (1ULL << idx->index))) {
            const char *r = local_reg(s, idx->index);
            if (r) {
                fprintf(f, "  movq (%%rdi,%s,8), %%rax\n", r);
                return;
            }
        }
        if (idx->op == CB_VAR && local_reg(s, idx->index)) {
            const char *r = local_reg(s, idx->index);
            fprintf(f, "  xorl %%eax, %%eax\n  cmpq %%rsi, %s\n  jae .L%u\n"
                       "  movq (%%rdi,%s,8), %%rax\n.L%u:\n", r, label, r, label);
            return;
        }
        emit_expr(s, idx);
        fprintf(f, "  xorl %%ecx, %%ecx\n  cmpq %%rsi, %%rax\n  jae .L%u\n"
                   "  movq (%%rdi,%%rax,8), %%rcx\n.L%u:\n  movq %%rcx, %%rax\n", label, label);
        return;
    }
    case CB_IF: {
        unsigned else_lbl = s->label++;
        unsigned done_lbl = s->label++;
        uint64_t saved_lt = s->known_lt_len;
        const IrExpr *c = e->arg[0];
        emit_branch_if_false(s, c, else_lbl);
        if (c->op == CB_LT && c->arg[0]->op == CB_VAR && c->arg[0]->index < 64 && c->arg[1]->op == CB_LEN)
            s->known_lt_len |= (1ULL << c->arg[0]->index);
        emit_expr(s, e->arg[1]);
        s->known_lt_len = saved_lt;
        fprintf(f, "  jmp .L%u\n.L%u:\n", done_lbl, else_lbl);
        if (c->op == CB_GE && c->arg[0]->op == CB_VAR && c->arg[0]->index < 64 && c->arg[1]->op == CB_LEN)
            s->known_lt_len |= (1ULL << c->arg[0]->index);
        emit_expr(s, e->arg[2]);
        s->known_lt_len = saved_lt;
        fprintf(f, ".L%u:\n", done_lbl);
        return;
    }
    case CB_LET:
        if (e->index >= 0 && e->index < 64) s->known_lt_len &= ~(1ULL << e->index);
        emit_expr_to_local(s, e->arg[0], e->index);
        emit_expr(s, e->arg[1]);
        return;
    case CB_CALL:
        emit_call_args_and_invoke(s, e, 0);
        return;
    default: {
        const IrExpr *l = e->arg[0], *r = e->arg[1];
        if (is_simple_op(r)) {
            emit_expr(s, l);
            char buf[64];
            const char *rop = get_simple_op(s, r, buf, sizeof(buf));
            finish_binop_rhs(s, e->op, rop, r);
            return;
        }
        if (is_simple_op(l)) {
            emit_expr(s, r);
            char buf[64];
            if (is_comm(e->op)) {
                const char *lop = get_simple_op(s, l, buf, sizeof(buf));
                finish_binop_rhs(s, e->op, lop, l);
                return;
            }
            if (is_cmp(e->op)) {
                const char *lop = get_simple_op(s, l, buf, sizeof(buf));
                finish_binop_rhs(s, swap_cmp(e->op), lop, l);
                return;
            }
            fprintf(f, "  movq %%rax, %%rcx\n");
            emit_expr(s, l);
            finish_binop_rhs(s, e->op, "%rcx", NULL);
            return;
        }
        emit_expr(s, l);
        if (!has_call(r) && !uses_rdx_op(r) && !s->rdx_busy) {
            fprintf(f, "  movq %%rax, %%rdx\n");
            s->rdx_busy = 1;
            emit_expr(s, r);
            s->rdx_busy = 0;
            if (is_comm(e->op)) {
                finish_binop_rhs(s, e->op, "%rdx", NULL);
            } else {
                fprintf(f, "  movq %%rax, %%rcx\n  movq %%rdx, %%rax\n");
                finish_binop_rhs(s, e->op, "%rcx", NULL);
            }
            return;
        }
        if (!has_call(r) && s->xmm_depth < 8) {
            int xslot = s->xmm_depth++;
            fprintf(f, "  vmovq %%rax, %%xmm%d\n", 8 + xslot);
            emit_expr(s, r);
            s->xmm_depth--;
            fprintf(f, "  movq %%rax, %%rcx\n  vmovq %%xmm%d, %%rax\n", 8 + xslot);
            finish_binop_rhs(s, e->op, "%rcx", NULL);
            return;
        }
        int tslot = s->stack_depth++;
        fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_stack_offset(s, tslot));
        emit_expr(s, r);
        s->stack_depth--;
        fprintf(f, "  movq %%rax, %%rcx\n  movq %d(%%rbp), %%rax\n", temp_stack_offset(s, tslot));
        finish_binop_rhs(s, e->op, "%rcx", NULL);
        return;
    }
    }
}

static void emit_tail(Emit *s, const IrExpr *e) {
    FILE *f = s->out;
    if (e->op == CB_IF) {
        unsigned else_lbl = s->label++;
        uint64_t saved_lt = s->known_lt_len;
        const IrExpr *c = e->arg[0];
        emit_branch_if_false(s, c, else_lbl);
        if (c->op == CB_LT && c->arg[0]->op == CB_VAR && c->arg[0]->index < 64 && c->arg[1]->op == CB_LEN)
            s->known_lt_len |= (1ULL << c->arg[0]->index);
        emit_tail(s, e->arg[1]);
        s->known_lt_len = saved_lt;
        fprintf(f, ".L%u:\n", else_lbl);
        if (c->op == CB_GE && c->arg[0]->op == CB_VAR && c->arg[0]->index < 64 && c->arg[1]->op == CB_LEN)
            s->known_lt_len |= (1ULL << c->arg[0]->index);
        emit_tail(s, e->arg[2]);
        s->known_lt_len = saved_lt;
    } else if (e->op == CB_LET) {
        if (e->index >= 0 && e->index < 64) s->known_lt_len &= ~(1ULL << e->index);
        emit_expr_to_local(s, e->arg[0], e->index);
        emit_tail(s, e->arg[1]);
    } else if (e->op == CB_CALL) {
        if (e->index == s->fn_idx) {
            emit_self_tail_call(s, e);
        } else {
            emit_call_args_and_invoke(s, e, 1);
        }
    } else {
        emit_expr(s, e);
        if (s->unroll_step_label >= 0) {
            fprintf(f, "  jmp .L%d\n", s->unroll_step_label);
        } else {
            emit_epilogue(s);
            fprintf(f, "  ret\n");
        }
    }
}

static void emit_avx2_combine(FILE *f, RedKind rk, int dst_y, int src_y) {
    switch (rk) {
    case RED_ADD: fprintf(f, "  vpaddq %%ymm%d, %%ymm%d, %%ymm%d\n", src_y, dst_y, dst_y); break;
    case RED_XOR: fprintf(f, "  vpxor %%ymm%d, %%ymm%d, %%ymm%d\n", src_y, dst_y, dst_y); break;
    case RED_OR:  fprintf(f, "  vpor %%ymm%d, %%ymm%d, %%ymm%d\n", src_y, dst_y, dst_y); break;
    case RED_AND: fprintf(f, "  vpand %%ymm%d, %%ymm%d, %%ymm%d\n", src_y, dst_y, dst_y); break;
    case RED_MUL:
        fprintf(f, "  vpsrlq $32, %%ymm%d, %%ymm14\n  vpmuludq %%ymm%d, %%ymm14, %%ymm14\n"
                   "  vpsrlq $32, %%ymm%d, %%ymm15\n  vpmuludq %%ymm%d, %%ymm15, %%ymm15\n"
                   "  vpaddq %%ymm15, %%ymm14, %%ymm14\n  vpsllq $32, %%ymm14, %%ymm14\n"
                   "  vpmuludq %%ymm%d, %%ymm%d, %%ymm%d\n  vpaddq %%ymm14, %%ymm%d, %%ymm%d\n",
                dst_y, src_y, src_y, dst_y, src_y, dst_y, dst_y, dst_y, dst_y);
        break;
    case RED_MAX:
    case RED_MIN:
        fprintf(f, "  vmovdqa .Lvec_sign(%%rip), %%ymm15\n"
                   "  vpxor %%ymm15, %%ymm%d, %%ymm14\n  vpxor %%ymm15, %%ymm%d, %%ymm15\n", dst_y, src_y);
        if (rk == RED_MAX)
            fprintf(f, "  vpcmpgtq %%ymm14, %%ymm15, %%ymm14\n");
        else
            fprintf(f, "  vpcmpgtq %%ymm15, %%ymm14, %%ymm14\n");
        fprintf(f, "  vpand %%ymm14, %%ymm%d, %%ymm15\n  vpandn %%ymm%d, %%ymm14, %%ymm%d\n"
                   "  vpor %%ymm15, %%ymm%d, %%ymm%d\n", src_y, dst_y, dst_y, dst_y, dst_y);
        break;
    default: break;
    }
}

static void emit_avx2_expr(Emit *s, const IrExpr *e, int dst_y, int lane_off) {
    FILE *f = s->out;
    const LoopInfo *L = s->loop;
    switch (e->op) {
    case CB_CONST:
        if (e->value == 0) {
            fprintf(f, "  vpxor %%ymm%d, %%ymm%d, %%ymm%d\n", dst_y, dst_y, dst_y);
        } else if (e->value == ~0ULL) {
            fprintf(f, "  vpcmpeqq %%ymm%d, %%ymm%d, %%ymm%d\n", dst_y, dst_y, dst_y);
        } else {
            emit_load_const(f, e->value, "%rax", "%eax");
            fprintf(f, "  vmovq %%rax, %%xmm%d\n  vpbroadcastq %%xmm%d, %%ymm%d\n", dst_y, dst_y, dst_y);
        }
        return;
    case CB_LEN:
        fprintf(f, "  vmovq %%rsi, %%xmm%d\n  vpbroadcastq %%xmm%d, %%ymm%d\n", dst_y, dst_y, dst_y);
        return;
    case CB_VAR:
        if (e->index == L->idx_param) {
            const char *ri = local_reg(s, L->idx_param);
            fprintf(f, "  vmovq %s, %%xmm%d\n  vpbroadcastq %%xmm%d, %%ymm%d\n"
                       "  vpaddq %s(%%rip), %%ymm%d, %%ymm%d\n",
                    ri, dst_y, dst_y, dst_y, lane_off == 0 ? ".Lvec_0123" : ".Lvec_4567", dst_y, dst_y);
            return;
        }
        for (int i = 0; i < L->num_lets; ++i) {
            if (L->let_slots[i] == e->index) {
                emit_avx2_expr(s, L->let_vals[i], dst_y, lane_off);
                return;
            }
        }
        fprintf(f, "  vmovq %s, %%xmm%d\n  vpbroadcastq %%xmm%d, %%ymm%d\n",
                local_reg(s, e->index), dst_y, dst_y, dst_y);
        return;
    case CB_GET: {
        int off = 0;
        is_idx_plus_const(e->arg[0], L->idx_param, &off);
        int tot = (off + lane_off) * 8;
        const char *ri = local_reg(s, L->idx_param);
        if (tot == 0) fprintf(f, "  vmovdqu (%%rdi,%s,8), %%ymm%d\n", ri, dst_y);
        else fprintf(f, "  vmovdqu %d(%%rdi,%s,8), %%ymm%d\n", tot, ri, dst_y);
        return;
    }
    case CB_SHL:
        emit_avx2_expr(s, e->arg[0], dst_y, lane_off);
        if (e->arg[1]->op == CB_CONST) {
            fprintf(f, "  vpsllq $%u, %%ymm%d, %%ymm%d\n", (unsigned)(e->arg[1]->value & 63), dst_y, dst_y);
        } else {
            emit_avx2_expr(s, e->arg[1], dst_y + 1, lane_off);
            fprintf(f, "  vpand .Lvec_mask63(%%rip), %%ymm%d, %%ymm%d\n"
                       "  vpsllvq %%ymm%d, %%ymm%d, %%ymm%d\n", dst_y + 1, dst_y + 1, dst_y + 1, dst_y, dst_y);
        }
        return;
    case CB_SHR:
        emit_avx2_expr(s, e->arg[0], dst_y, lane_off);
        if (e->arg[1]->op == CB_CONST) {
            fprintf(f, "  vpsrlq $%u, %%ymm%d, %%ymm%d\n", (unsigned)(e->arg[1]->value & 63), dst_y, dst_y);
        } else {
            emit_avx2_expr(s, e->arg[1], dst_y + 1, lane_off);
            fprintf(f, "  vpand .Lvec_mask63(%%rip), %%ymm%d, %%ymm%d\n"
                       "  vpsrlvq %%ymm%d, %%ymm%d, %%ymm%d\n", dst_y + 1, dst_y + 1, dst_y + 1, dst_y, dst_y);
        }
        return;
    case CB_IF:
        emit_avx2_expr(s, e->arg[0], dst_y, lane_off);
        emit_avx2_expr(s, e->arg[1], dst_y + 1, lane_off);
        emit_avx2_expr(s, e->arg[2], dst_y + 2, lane_off);
        fprintf(f, "  vpxor %%ymm15, %%ymm15, %%ymm15\n"
                   "  vpcmpeqq %%ymm15, %%ymm%d, %%ymm%d\n"
                   "  vpand %%ymm%d, %%ymm%d, %%ymm15\n"
                   "  vpandn %%ymm%d, %%ymm%d, %%ymm%d\n"
                   "  vpor %%ymm15, %%ymm%d, %%ymm%d\n",
                dst_y, dst_y, dst_y, dst_y + 2, dst_y + 1, dst_y, dst_y, dst_y, dst_y);
        return;
    default: {
        int ry = dst_y + 1;
        emit_avx2_expr(s, e->arg[0], dst_y, lane_off);
        emit_avx2_expr(s, e->arg[1], ry, lane_off);
        switch (e->op) {
        case CB_ADD: fprintf(f, "  vpaddq %%ymm%d, %%ymm%d, %%ymm%d\n", ry, dst_y, dst_y); break;
        case CB_SUB: fprintf(f, "  vpsubq %%ymm%d, %%ymm%d, %%ymm%d\n", ry, dst_y, dst_y); break;
        case CB_AND: fprintf(f, "  vpand %%ymm%d, %%ymm%d, %%ymm%d\n", ry, dst_y, dst_y); break;
        case CB_OR:  fprintf(f, "  vpor %%ymm%d, %%ymm%d, %%ymm%d\n", ry, dst_y, dst_y); break;
        case CB_XOR: fprintf(f, "  vpxor %%ymm%d, %%ymm%d, %%ymm%d\n", ry, dst_y, dst_y); break;
        case CB_MUL: emit_avx2_combine(f, RED_MUL, dst_y, ry); break;
        case CB_EQ:
            fprintf(f, "  vpcmpeqq %%ymm%d, %%ymm%d, %%ymm%d\n  vpsrlq $63, %%ymm%d, %%ymm%d\n", ry, dst_y, dst_y, dst_y, dst_y);
            break;
        case CB_NE:
            fprintf(f, "  vpcmpeqq %%ymm%d, %%ymm%d, %%ymm%d\n  vpandn .Lvec_ones(%%rip), %%ymm%d, %%ymm%d\n", ry, dst_y, dst_y, dst_y, dst_y);
            break;
        case CB_GT:
        case CB_LE:
            fprintf(f, "  vmovdqa .Lvec_sign(%%rip), %%ymm15\n"
                       "  vpxor %%ymm15, %%ymm%d, %%ymm%d\n  vpxor %%ymm15, %%ymm%d, %%ymm%d\n"
                       "  vpcmpgtq %%ymm%d, %%ymm%d, %%ymm%d\n", dst_y, dst_y, ry, ry, ry, dst_y, dst_y);
            if (e->op == CB_GT) fprintf(f, "  vpsrlq $63, %%ymm%d, %%ymm%d\n", dst_y, dst_y);
            else fprintf(f, "  vpandn .Lvec_ones(%%rip), %%ymm%d, %%ymm%d\n", dst_y, dst_y);
            break;
        case CB_LT:
        case CB_GE:
            fprintf(f, "  vmovdqa .Lvec_sign(%%rip), %%ymm15\n"
                       "  vpxor %%ymm15, %%ymm%d, %%ymm%d\n  vpxor %%ymm15, %%ymm%d, %%ymm%d\n"
                       "  vpcmpgtq %%ymm%d, %%ymm%d, %%ymm%d\n", dst_y, dst_y, ry, ry, dst_y, ry, dst_y);
            if (e->op == CB_LT) fprintf(f, "  vpsrlq $63, %%ymm%d, %%ymm%d\n", dst_y, dst_y);
            else fprintf(f, "  vpandn .Lvec_ones(%%rip), %%ymm%d, %%ymm%d\n", dst_y, dst_y);
            break;
        default: break;
        }
        return;
    }
    }
}

static void emit_avx2_init_acc(FILE *f, RedKind rk, int ymm) {
    if (rk == RED_ADD || rk == RED_XOR || rk == RED_OR || rk == RED_MAX) {
        fprintf(f, "  vpxor %%ymm%d, %%ymm%d, %%ymm%d\n", ymm, ymm, ymm);
    } else if (rk == RED_AND || rk == RED_MIN) {
        fprintf(f, "  vpcmpeqq %%ymm%d, %%ymm%d, %%ymm%d\n", ymm, ymm, ymm);
    } else if (rk == RED_MUL) {
        fprintf(f, "  vmovdqa .Lvec_ones(%%rip), %%ymm%d\n", ymm);
    }
}

static void emit_loop_fast_path(Emit *s, const LoopInfo *L) {
    FILE *f = s->out;
    const char *ri = local_reg(s, L->idx_param);
    unsigned scalar_lbl = s->label++;
    /* Compute safe_limit in %rcx */
    if (L->bound_kind == 0) {
        fprintf(f, "  movq %%rsi, %%rcx\n");
    } else if (L->bound_kind == 1) {
        emit_load_const(f, L->bound_val, "%rcx", "%ecx");
    } else {
        fprintf(f, "  movq %s, %%rcx\n", local_reg(s, L->bound_var));
    }
    fprintf(f, "  cmpq %%rcx, %s\n  jae .L%u\n", ri, scalar_lbl);
    if (L->has_gets) {
        fprintf(f, "  cmpq $%d, %%rsi\n  jb .L%u\n", 4 + L->max_get_offset, scalar_lbl);
        if (L->max_get_offset > 0) {
            fprintf(f, "  leaq -%d(%%rsi), %%rdx\n  cmpq %%rdx, %%rcx\n  cmovaq %%rdx, %%rcx\n", L->max_get_offset);
        } else {
            fprintf(f, "  cmpq %%rsi, %%rcx\n  cmovaq %%rsi, %%rcx\n");
        }
    }
    fprintf(f, "  cmpq $4, %%rcx\n  jb .L%u\n", scalar_lbl);

    if (L->is_avx2) {
        unsigned avx4_setup = s->label++;
        unsigned avx8_loop = s->label++;
        unsigned avx8_done = s->label++;
        unsigned avx4_loop = s->label++;
        unsigned avx4_done = s->label++;

        for (int a = 0; a < L->acc_count; ++a) {
            emit_avx2_init_acc(f, L->acc_kind[a], a);
            emit_avx2_init_acc(f, L->acc_kind[a], a + 3);
        }
        fprintf(f, "  cmpq $8, %%rcx\n  jb .L%u\n"
                   "  leaq -8(%%rcx), %%r15\n"
                   ".p2align 4\n.L%u:\n  cmpq %%r15, %s\n  ja .L%u\n",
                avx4_setup, avx8_loop, ri, avx8_done);
        for (int a = 0; a < L->acc_count; ++a) {
            emit_avx2_expr(s, L->acc_rhs[a], 7, 0);
            emit_avx2_combine(f, L->acc_kind[a], a, 7);
            emit_avx2_expr(s, L->acc_rhs[a], 7, 4);
            emit_avx2_combine(f, L->acc_kind[a], a + 3, 7);
        }
        fprintf(f, "  addq $8, %s\n  jmp .L%u\n.L%u:\n", ri, avx8_loop, avx8_done);
        for (int a = 0; a < L->acc_count; ++a) {
            emit_avx2_combine(f, L->acc_kind[a], a, a + 3);
        }
        fprintf(f, ".L%u:\n  leaq -4(%%rcx), %%r15\n"
                   ".p2align 4\n.L%u:\n  cmpq %%r15, %s\n  ja .L%u\n",
                avx4_setup, avx4_loop, ri, avx4_done);
        for (int a = 0; a < L->acc_count; ++a) {
            emit_avx2_expr(s, L->acc_rhs[a], 7, 0);
            emit_avx2_combine(f, L->acc_kind[a], a, 7);
        }
        fprintf(f, "  addq $4, %s\n  jmp .L%u\n.L%u:\n", ri, avx4_loop, avx4_done);
        /* Horizontal reduction of %ymm0..%ymm(acc_count-1) */
        for (int a = 0; a < L->acc_count; ++a) {
            RedKind rk = L->acc_kind[a];
            const char *areg = local_reg(s, L->acc_param[a]);
            fprintf(f, "  vextracti128 $1, %%ymm%d, %%xmm7\n", a);
            emit_avx2_combine(f, rk, a, 7);
            fprintf(f, "  vpsrldq $8, %%xmm%d, %%xmm7\n", a);
            emit_avx2_combine(f, rk, a, 7);
            fprintf(f, "  vmovq %%xmm%d, %%rax\n", a);
            switch (rk) {
            case RED_ADD: fprintf(f, "  addq %%rax, %s\n", areg); break;
            case RED_XOR: fprintf(f, "  xorq %%rax, %s\n", areg); break;
            case RED_OR:  fprintf(f, "  orq %%rax, %s\n", areg); break;
            case RED_AND: fprintf(f, "  andq %%rax, %s\n", areg); break;
            case RED_MUL: fprintf(f, "  imulq %%rax, %s\n", areg); break;
            case RED_MAX: fprintf(f, "  cmpq %s, %%rax\n  cmovaq %%rax, %s\n", areg, areg); break;
            case RED_MIN: fprintf(f, "  cmpq %s, %%rax\n  cmovbq %%rax, %s\n", areg, areg); break;
            default: break;
            }
        }
        fprintf(f, "  vzeroupper\n");
    } else {
        unsigned u4_loop = s->label++;
        fprintf(f, "  leaq -4(%%rcx), %%r15\n"
                   ".p2align 4\n.L%u:\n  cmpq %%r15, %s\n  ja .L%u\n",
                u4_loop, ri, scalar_lbl);
        s->all_gets_in_bounds = 1;
        for (int u = 0; u < 4; ++u) {
            unsigned next_step = (u < 3) ? s->label++ : u4_loop;
            s->unroll_step_label = (int)next_step;
            emit_tail(s, L->cont_br);
            if (u < 3) fprintf(f, ".L%u:\n", next_step);
        }
        s->unroll_step_label = -1;
        s->all_gets_in_bounds = 0;
    }
    fprintf(f, ".L%u:\n", scalar_lbl);
}

static void emit_function(FILE *f, const IrProg *prog, int fn_idx) {
    const IrFunc *fn = &prog->fn[fn_idx];
    LoopInfo loop;
    analyze_loop(fn, fn_idx, &loop);

    Emit s;
    memset(&s, 0, sizeof(s));
    s.out = f;
    s.prog = prog;
    s.fn = fn;
    s.fn_idx = fn_idx;
    s.label = (unsigned)(fn_idx * 100000 + 1);
    s.has_nontail = fn->has_nontail_call;
    s.uses_r15 = loop.valid ? 1 : 0;
    s.unroll_step_label = -1;
    s.loop = &loop;

    int reg_base = s.has_nontail ? MODE_B_REGS_CNT : MODE_A_REGS_CNT;
    int spilled_locals = fn->locals > reg_base ? (fn->locals - reg_base) : 0;
    int need_rbp = (spilled_locals > 0 || fn->max_stack_temps > 0) ? 1 : 0;

    if (need_rbp) s.callee_saved_regs[s.num_callee_saved++] = 0; /* %rbp */
    if (!s.has_nontail) {
        int extra = fn->locals > 4 ? (fn->locals - 4) : 0;
        if (extra > 4) extra = 4;
        for (int i = 0; i < extra; ++i) s.callee_saved_regs[s.num_callee_saved++] = 1 + i;
        if (s.uses_r15) s.callee_saved_regs[s.num_callee_saved++] = 5;
    } else {
        int used = fn->locals < MODE_B_REGS_CNT ? fn->locals : MODE_B_REGS_CNT;
        for (int i = 0; i < used; ++i) s.callee_saved_regs[s.num_callee_saved++] = 1 + i;
    }

    int total_slots = spilled_locals + fn->max_stack_temps;
    int pushed_bytes = (s.num_callee_saved + 1) * 8; /* +1 for return address */
    int raw_bytes = total_slots * 8;
    if (s.has_nontail || need_rbp) {
        int rem = (pushed_bytes + raw_bytes) & 15;
        if (rem != 0) raw_bytes += (16 - rem);
    }
    s.frame_bytes = raw_bytes;

    fprintf(f, ".p2align 4\n.Lfn%d:\n", fn_idx);
    static const char *const CS_NAMES[] = {"%rbp", "%rbx", "%r12", "%r13", "%r14", "%r15"};
    for (int i = 0; i < s.num_callee_saved; ++i) {
        fprintf(f, "  pushq %s\n", CS_NAMES[s.callee_saved_regs[i]]);
        if (i == 0 && need_rbp) fprintf(f, "  movq %%rsp, %%rbp\n");
    }
    if (s.frame_bytes > 0) fprintf(f, "  subq $%d, %%rsp\n", s.frame_bytes);

    if (s.has_nontail) {
        for (int j = 0; j < fn->orig_arity; ++j)
            emit_store_local(&s, j, ARG_REGS[j]);
    }
    if (fn->has_acc_init) {
        int acc_slot = fn->orig_arity;
        const char *r = local_reg(&s, acc_slot);
        if (r) {
            static const char *const R32[] = {"%r8d", "%r9d", "%r10d", "%r11d", "%ebx", "%r12d", "%r13d", "%r14d", "%r15d"};
            int r_idx = -1;
            for (int k = 0; k < MODE_A_REGS_CNT; ++k) if (strcmp(r, MODE_A_REGS[k]) == 0) r_idx = k;
            if (r_idx < 0) r_idx = 8;
            emit_load_const(f, fn->acc_init_val, r, R32[r_idx]);
        } else {
            emit_load_const(f, fn->acc_init_val, "%rax", "%eax");
            emit_store_local(&s, acc_slot, "%rax");
        }
    }

    if (loop.valid) {
        emit_loop_fast_path(&s, &loop);
    }
    fprintf(f, ".Lloop%d:\n", fn_idx);
    emit_tail(&s, fn->body);
}

int cb_emit_assembly(const CbProgram *p, const char *path,
                     char *error, size_t error_size) {
    if (!p || !path) {
        if (error && error_size > 0) snprintf(error, error_size, "invalid arguments");
        return 0;
    }
    IrProg *prog = (IrProg *)calloc(1, sizeof(IrProg));
    if (!prog) {
        if (error && error_size > 0) snprintf(error, error_size, "out of memory");
        return 0;
    }
    prog->functions = p->functions;
    prog->entry = p->entry;
    for (int i = 0; i < p->functions; ++i) {
        prog->fn[i].orig_arity = p->fn[i].arity;
        prog->fn[i].arity = p->fn[i].arity;
        prog->fn[i].locals = p->fn[i].locals;
        prog->fn[i].body = import_ast(prog, p->fn[i].body);
        if (!prog->fn[i].body) {
            free(prog);
            if (error && error_size > 0) snprintf(error, error_size, "IR pool exhausted");
            return 0;
        }
    }

    IrExpr *subst[IR_MAX_LOCALS];
    for (int round = 0; round < 3; ++round) {
        compute_call_graph(prog);
        for (int i = 0; i < prog->functions; ++i) {
            prog->fn[i].body = inline_calls(prog, &prog->fn[i], i, prog->fn[i].body);
            memset(subst, 0, sizeof(subst));
            prog->fn[i].body = simplify_expr(prog, prog->fn[i].body, subst);
        }
    }

    for (int i = 0; i < prog->functions; ++i) {
        convert_linear_rec(prog, &prog->fn[i], i);
        prog->fn[i].body = normalize_tail(prog, &prog->fn[i], i, prog->fn[i].body);
        for (int pass = 0; pass < 2; ++pass) {
            memset(subst, 0, sizeof(subst));
            prog->fn[i].body = simplify_expr(prog, prog->fn[i].body, subst);
            prog->fn[i].body = normalize_tail(prog, &prog->fn[i], i, prog->fn[i].body);
        }
        prog->fn[i].has_nontail_call = check_nontail_calls(prog->fn[i].body, 1);
        int max_s = 0;
        sim_temps(prog->fn[i].body, 0, 0, 0, &max_s);
        prog->fn[i].max_stack_temps = max_s + 2;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        if (error && error_size > 0) snprintf(error, error_size, "%s: %s", path, strerror(errno));
        free(prog);
        return 0;
    }

    fprintf(f, ".section .rodata\n.p2align 5\n"
               ".Lvec_0123:\n  .quad 0, 1, 2, 3\n"
               ".Lvec_4567:\n  .quad 4, 5, 6, 7\n"
               ".Lvec_sign:\n  .quad 0x8000000000000000, 0x8000000000000000, 0x8000000000000000, 0x8000000000000000\n"
               ".Lvec_mask63:\n  .quad 63, 63, 63, 63\n"
               ".Lvec_ones:\n  .quad 1, 1, 1, 1\n"
               ".text\n.p2align 4\n.globl bench_entry\n.type bench_entry, @function\n"
               "bench_entry:\n"
               "  pushq %%rbp\n  pushq %%rbx\n  pushq %%r12\n"
               "  pushq %%r13\n  pushq %%r14\n  pushq %%r15\n"
               "  subq $8, %%rsp\n"
               "  call .Lfn%d\n"
               "  addq $8, %%rsp\n"
               "  popq %%r15\n  popq %%r14\n  popq %%r13\n"
               "  popq %%r12\n  popq %%rbx\n  popq %%rbp\n"
               "  ret\n"
               ".size bench_entry, .-bench_entry\n", prog->entry);

    for (int i = 0; i < prog->functions; ++i) {
        emit_function(f, prog, i);
    }

    fprintf(f, ".section .note.GNU-stack,\"\",@progbits\n");
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    if (!ok && error && error_size > 0) snprintf(error, error_size, "could not write %s", path);
    free(prog);
    return ok;
}