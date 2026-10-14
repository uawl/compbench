#include "ast.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_MAX_FUNCS 64
#define IR_MAX_PARAMS 6
#define IR_MAX_LOCALS 512
#define IR_MAX_NODES 65536

typedef struct IrExpr {
    CbOp op;
    uint64_t value;
    int index;
    int argc;
    int spill_off;
    int call_arg_off[IR_MAX_PARAMS];
    struct IrExpr *arg[IR_MAX_PARAMS];
} IrExpr;

typedef struct {
    char name[CB_NAME_SIZE];
    int arity;
    int locals;
    IrExpr *body;
} IrFunc;

typedef struct {
    IrFunc fn[IR_MAX_FUNCS];
    int functions;
    int entry;
    int nodes;
    IrExpr pool[IR_MAX_NODES];
} IrProg;

static int clz64(uint64_t x) {
    if (x == 0) return 64;
    int n = 0;
    if ((x >> 32) == 0) { n += 32; x <<= 32; }
    if ((x >> 48) == 0) { n += 16; x <<= 16; }
    if ((x >> 56) == 0) { n += 8;  x <<= 8;  }
    if ((x >> 60) == 0) { n += 4;  x <<= 4;  }
    if ((x >> 62) == 0) { n += 2;  x <<= 2;  }
    if ((x >> 63) == 0) { n += 1; }
    return n;
}

static int ctz64(uint64_t x) {
    if (x == 0) return 64;
    int n = 0;
    while ((x & 1ULL) == 0) {
        x >>= 1;
        ++n;
    }
    return n;
}

static int is_pow2(uint64_t x) {
    return x != 0 && (x & (x - 1ULL)) == 0;
}

static uint64_t div128_u64(uint64_t hi, uint64_t lo, uint64_t d) {
    uint64_t q = 0;
    for (int i = 63; i >= 0; --i) {
        uint64_t top = hi >> 63;
        hi = (hi << 1) | (lo >> 63);
        lo <<= 1;
        if (top || hi >= d) {
            hi -= d;
            q |= (1ULL << i);
        }
    }
    return q;
}

static void compute_magic_u64(uint64_t d, uint64_t *m_low_out, int *l_out) {
    int l = 64 - clz64(d);
    uint64_t rem_hi = (l == 64) ? (0ULL - d) : ((1ULL << l) - d);
    uint64_t rem_lo = (l == 64) ? ~0ULL : ((1ULL << l) - 1ULL);
    *m_low_out = div128_u64(rem_hi, rem_lo, d);
    *l_out = l;
}

static int fits_imm32(uint64_t v) {
    int64_t sv = (int64_t)v;
    return sv >= INT32_MIN && sv <= INT32_MAX;
}

static IrExpr *alloc_ir(IrProg *p, CbOp op) {
    if (p->nodes >= IR_MAX_NODES) return NULL;
    IrExpr *e = &p->pool[p->nodes++];
    memset(e, 0, sizeof(*e));
    e->op = op;
    return e;
}

static IrExpr *make_const(IrProg *p, uint64_t v) {
    IrExpr *e = alloc_ir(p, CB_CONST);
    if (e) e->value = v;
    return e;
}

static IrExpr *make_var(IrProg *p, int idx) {
    IrExpr *e = alloc_ir(p, CB_VAR);
    if (e) e->index = idx;
    return e;
}

static IrExpr *make_bin(IrProg *p, CbOp op, IrExpr *a, IrExpr *b) {
    if (!a || !b) return NULL;
    IrExpr *e = alloc_ir(p, op);
    if (!e) return NULL;
    e->arg[0] = a;
    e->arg[1] = b;
    return e;
}

static IrExpr *make_if(IrProg *p, IrExpr *c, IrExpr *t, IrExpr *f) {
    if (!c || !t || !f) return NULL;
    IrExpr *e = alloc_ir(p, CB_IF);
    if (!e) return NULL;
    e->arg[0] = c;
    e->arg[1] = t;
    e->arg[2] = f;
    return e;
}

static IrExpr *make_let(IrProg *p, int slot, IrExpr *v, IrExpr *b) {
    if (!v || !b) return NULL;
    IrExpr *e = alloc_ir(p, CB_LET);
    if (!e) return NULL;
    e->index = slot;
    e->arg[0] = v;
    e->arg[1] = b;
    return e;
}

static IrExpr *import_expr(IrProg *p, const CbExpr *src) {
    if (!src) return NULL;
    IrExpr *dst = alloc_ir(p, src->op);
    if (!dst) return NULL;
    dst->value = src->value;
    dst->index = src->index;
    dst->argc = src->argc;
    int nchild = 0;
    switch (src->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: nchild = 0; break;
    case CB_GET: nchild = 1; break;
    case CB_IF: nchild = 3; break;
    case CB_LET: nchild = 2; break;
    case CB_CALL: nchild = src->argc; break;
    default: nchild = 2; break;
    }
    for (int i = 0; i < nchild; ++i) {
        dst->arg[i] = import_expr(p, src->arg[i]);
        if (!dst->arg[i]) return NULL;
    }
    return dst;
}

static int num_children(const IrExpr *e) {
    if (!e) return 0;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: return 0;
    case CB_GET: return 1;
    case CB_IF: return 3;
    case CB_LET: return 2;
    case CB_CALL: return e->argc;
    default: return 2;
    }
}

static IrExpr *clone_expr(IrProg *p, const IrExpr *src, int slot_offset) {
    if (!src) return NULL;
    IrExpr *dst = alloc_ir(p, src->op);
    if (!dst) return NULL;
    dst->value = src->value;
    dst->index = src->index;
    if (src->op == CB_VAR || src->op == CB_LET) {
        dst->index += slot_offset;
    }
    dst->argc = src->argc;
    int nc = num_children(src);
    for (int i = 0; i < nc; ++i) {
        dst->arg[i] = clone_expr(p, src->arg[i], slot_offset);
        if (!dst->arg[i]) return NULL;
    }
    return dst;
}

static int expr_has_call(const IrExpr *e) {
    if (!e) return 0;
    if (e->op == CB_CALL) return 1;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        if (expr_has_call(e->arg[i])) return 1;
    }
    return 0;
}

static int expr_calls_fn(const IrExpr *e, int fn_idx) {
    if (!e) return 0;
    if (e->op == CB_CALL && e->index == fn_idx) return 1;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        if (expr_calls_fn(e->arg[i], fn_idx)) return 1;
    }
    return 0;
}

static int expr_count_calls_fn(const IrExpr *e, int fn_idx) {
    if (!e) return 0;
    int c = (e->op == CB_CALL && e->index == fn_idx) ? 1 : 0;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        c += expr_count_calls_fn(e->arg[i], fn_idx);
    }
    return c;
}

static int expr_size(const IrExpr *e) {
    if (!e) return 0;
    int s = 1;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) s += expr_size(e->arg[i]);
    return s;
}

static int expr_equal(const IrExpr *a, const IrExpr *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->op != b->op) return 0;
    if (a->op == CB_CALL) return 0;
    if (a->op == CB_CONST) return a->value == b->value;
    if (a->op == CB_VAR) return a->index == b->index;
    if (a->op == CB_LEN) return 1;
    if (a->op == CB_LET) return 0;
    int nc = num_children(a);
    for (int i = 0; i < nc; ++i) {
        if (!expr_equal(a->arg[i], b->arg[i])) return 0;
    }
    return 1;
}

static int count_var_uses(const IrExpr *e, int slot) {
    if (!e) return 0;
    if (e->op == CB_VAR) return e->index == slot ? 1 : 0;
    int c = 0;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) c += count_var_uses(e->arg[i], slot);
    return c;
}

static int count_uncond_var_uses(const IrExpr *e, int slot) {
    if (!e) return 0;
    if (e->op == CB_VAR) return e->index == slot ? 1 : 0;
    if (e->op == CB_IF) return count_uncond_var_uses(e->arg[0], slot);
    int c = 0;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) c += count_uncond_var_uses(e->arg[i], slot);
    return c;
}

static IrExpr *subst_var(IrProg *p, IrExpr *e, int slot, const IrExpr *repl) {
    if (!e) return NULL;
    if (e->op == CB_VAR && e->index == slot) {
        IrExpr *c = clone_expr(p, repl, 0);
        return c ? c : e;
    }
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        e->arg[i] = subst_var(p, e->arg[i], slot, repl);
    }
    return e;
}

static CbOp negate_cmp_op(CbOp op) {
    switch (op) {
    case CB_EQ: return CB_NE;
    case CB_NE: return CB_EQ;
    case CB_LT: return CB_GE;
    case CB_LE: return CB_GT;
    case CB_GT: return CB_LE;
    case CB_GE: return CB_LT;
    default: return op;
    }
}

static CbOp swap_cmp_op(CbOp op) {
    switch (op) {
    case CB_EQ: return CB_EQ;
    case CB_NE: return CB_NE;
    case CB_LT: return CB_GT;
    case CB_LE: return CB_GE;
    case CB_GT: return CB_LT;
    case CB_GE: return CB_LE;
    default: return op;
    }
}

static uint64_t eval_const_bin(CbOp op, uint64_t a, uint64_t b) {
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
    case CB_EQ:  return a == b ? 1 : 0;
    case CB_NE:  return a != b ? 1 : 0;
    case CB_LT:  return a < b ? 1 : 0;
    case CB_LE:  return a <= b ? 1 : 0;
    case CB_GT:  return a > b ? 1 : 0;
    case CB_GE:  return a >= b ? 1 : 0;
    default: return 0;
    }
}

static IrExpr *simplify_expr(IrProg *p, IrExpr *e) {
    if (!e) return NULL;
    if (e->op == CB_CONST || e->op == CB_VAR || e->op == CB_LEN) return e;

    if (e->op == CB_GET) {
        e->arg[0] = simplify_expr(p, e->arg[0]);
        return e;
    }

    if (e->op == CB_IF) {
        e->arg[0] = simplify_expr(p, e->arg[0]);
        if (e->arg[0]->op == CB_CONST) {
            return simplify_expr(p, e->arg[0]->value != 0 ? e->arg[1] : e->arg[2]);
        }
        e->arg[1] = simplify_expr(p, e->arg[1]);
        e->arg[2] = simplify_expr(p, e->arg[2]);
        if (!expr_has_call(e->arg[0]) && expr_equal(e->arg[1], e->arg[2])) {
            return e->arg[1];
        }
        if (e->arg[0]->op >= CB_EQ && e->arg[0]->op <= CB_GE &&
            e->arg[1]->op == CB_CONST && e->arg[2]->op == CB_CONST) {
            if (e->arg[1]->value == 1 && e->arg[2]->value == 0) {
                return e->arg[0];
            }
            if (e->arg[1]->value == 0 && e->arg[2]->value == 1) {
                e->arg[0]->op = negate_cmp_op(e->arg[0]->op);
                return e->arg[0];
            }
        }
        return e;
    }

    if (e->op == CB_LET) {
        e->arg[0] = simplify_expr(p, e->arg[0]);
        IrExpr *val = e->arg[0];
        int slot = e->index;
        if (val->op == CB_CONST || val->op == CB_VAR || val->op == CB_LEN) {
            e->arg[1] = subst_var(p, e->arg[1], slot, val);
            return simplify_expr(p, e->arg[1]);
        }
        e->arg[1] = simplify_expr(p, e->arg[1]);
        int uses = count_var_uses(e->arg[1], slot);
        if (uses == 0 && !expr_has_call(val)) {
            return e->arg[1];
        }
        if (uses == 1 && !expr_has_call(val) &&
            (count_uncond_var_uses(e->arg[1], slot) == 1 || expr_size(val) <= 3)) {
            IrExpr *res = subst_var(p, e->arg[1], slot, val);
            return simplify_expr(p, res);
        }
        return e;
    }

    if (e->op == CB_CALL) {
        for (int i = 0; i < e->argc; ++i) {
            e->arg[i] = simplify_expr(p, e->arg[i]);
        }
        return e;
    }

    /* Binary operations */
    e->arg[0] = simplify_expr(p, e->arg[0]);
    e->arg[1] = simplify_expr(p, e->arg[1]);
    IrExpr *lhs = e->arg[0];
    IrExpr *rhs = e->arg[1];

    if (lhs->op == CB_CONST && rhs->op == CB_CONST) {
        e->value = eval_const_bin(e->op, lhs->value, rhs->value);
        e->op = CB_CONST;
        e->arg[0] = e->arg[1] = NULL;
        return e;
    }

    /* Canonicalize commutative ops with constant on rhs */
    if (lhs->op == CB_CONST && rhs->op != CB_CONST) {
        if (e->op == CB_ADD || e->op == CB_MUL || e->op == CB_AND ||
            e->op == CB_OR || e->op == CB_XOR) {
            e->arg[0] = rhs;
            e->arg[1] = lhs;
            lhs = e->arg[0];
            rhs = e->arg[1];
        } else if (e->op >= CB_EQ && e->op <= CB_GE) {
            e->op = swap_cmp_op(e->op);
            e->arg[0] = rhs;
            e->arg[1] = lhs;
            lhs = e->arg[0];
            rhs = e->arg[1];
        }
    }

    switch (e->op) {
    case CB_ADD:
        if (rhs->op == CB_CONST && rhs->value == 0) return lhs;
        if (lhs->op == CB_ADD && lhs->arg[1]->op == CB_CONST && rhs->op == CB_CONST) {
            lhs->arg[1]->value += rhs->value;
            return simplify_expr(p, lhs);
        }
        break;
    case CB_SUB:
        if (rhs->op == CB_CONST && rhs->value == 0) return lhs;
        if (!expr_has_call(lhs) && expr_equal(lhs, rhs)) {
            e->op = CB_CONST; e->value = 0; e->arg[0] = e->arg[1] = NULL;
            return e;
        }
        break;
    case CB_MUL:
        if (rhs->op == CB_CONST) {
            if (rhs->value == 0 && !expr_has_call(lhs)) {
                e->op = CB_CONST; e->value = 0; e->arg[0] = e->arg[1] = NULL;
                return e;
            }
            if (rhs->value == 1) return lhs;
            if (is_pow2(rhs->value)) {
                e->op = CB_SHL;
                rhs->value = (uint64_t)ctz64(rhs->value);
                return e;
            }
        }
        break;
    case CB_DIV:
        if (rhs->op == CB_CONST) {
            if (rhs->value == 0 && !expr_has_call(lhs)) {
                e->op = CB_CONST; e->value = 0; e->arg[0] = e->arg[1] = NULL;
                return e;
            }
            if (rhs->value == 1) return lhs;
            if (is_pow2(rhs->value)) {
                e->op = CB_SHR;
                rhs->value = (uint64_t)ctz64(rhs->value);
                return e;
            }
        }
        if (lhs->op == CB_CONST && lhs->value == 0 && !expr_has_call(rhs)) {
            e->op = CB_CONST; e->value = 0; e->arg[0] = e->arg[1] = NULL;
            return e;
        }
        break;
    case CB_MOD:
        if (rhs->op == CB_CONST) {
            if ((rhs->value == 0 || rhs->value == 1) && !expr_has_call(lhs)) {
                e->op = CB_CONST; e->value = 0; e->arg[0] = e->arg[1] = NULL;
                return e;
            }
            if (is_pow2(rhs->value)) {
                e->op = CB_AND;
                rhs->value = rhs->value - 1ULL;
                return e;
            }
        }
        if (lhs->op == CB_CONST && lhs->value == 0 && !expr_has_call(rhs)) {
            e->op = CB_CONST; e->value = 0; e->arg[0] = e->arg[1] = NULL;
            return e;
        }
        break;
    case CB_AND:
        if (rhs->op == CB_CONST) {
            if (rhs->value == 0 && !expr_has_call(lhs)) {
                e->op = CB_CONST; e->value = 0; e->arg[0] = e->arg[1] = NULL;
                return e;
            }
            if (rhs->value == ~0ULL) return lhs;
        }
        if (!expr_has_call(lhs) && expr_equal(lhs, rhs)) return lhs;
        break;
    case CB_OR:
        if (rhs->op == CB_CONST) {
            if (rhs->value == 0) return lhs;
            if (rhs->value == ~0ULL && !expr_has_call(lhs)) {
                e->op = CB_CONST; e->value = ~0ULL; e->arg[0] = e->arg[1] = NULL;
                return e;
            }
        }
        if (!expr_has_call(lhs) && expr_equal(lhs, rhs)) return lhs;
        break;
    case CB_XOR:
        if (rhs->op == CB_CONST && rhs->value == 0) return lhs;
        if (!expr_has_call(lhs) && expr_equal(lhs, rhs)) {
            e->op = CB_CONST; e->value = 0; e->arg[0] = e->arg[1] = NULL;
            return e;
        }
        break;
    case CB_SHL:
    case CB_SHR:
        if (rhs->op == CB_CONST) {
            rhs->value &= 63ULL;
            if (rhs->value == 0) return lhs;
        }
        if (lhs->op == CB_CONST && lhs->value == 0 && !expr_has_call(rhs)) {
            e->op = CB_CONST; e->value = 0; e->arg[0] = e->arg[1] = NULL;
            return e;
        }
        break;
    case CB_EQ:
    case CB_NE:
    case CB_LT:
    case CB_LE:
    case CB_GT:
    case CB_GE:
        if (!expr_has_call(lhs) && expr_equal(lhs, rhs)) {
            uint64_t r = (e->op == CB_EQ || e->op == CB_LE || e->op == CB_GE) ? 1 : 0;
            e->op = CB_CONST; e->value = r; e->arg[0] = e->arg[1] = NULL;
            return e;
        }
        if (rhs->op == CB_CONST && rhs->value == 0) {
            if (e->op == CB_LT && !expr_has_call(lhs)) {
                e->op = CB_CONST; e->value = 0; e->arg[0] = e->arg[1] = NULL;
                return e;
            }
            if (e->op == CB_GE && !expr_has_call(lhs)) {
                e->op = CB_CONST; e->value = 1; e->arg[0] = e->arg[1] = NULL;
                return e;
            }
            if (e->op == CB_LE) e->op = CB_EQ;
            if (e->op == CB_GT) e->op = CB_NE;
        }
        break;
    default:
        break;
    }
    return e;
}

static void simplify_func(IrProg *p, IrFunc *fn) {
    fn->body = simplify_expr(p, fn->body);
}

/* Function Inlining */
static IrExpr *inline_calls_in_expr(IrProg *p, IrFunc *caller, IrExpr *e,
                                    int callee_idx, const IrFunc *callee) {
    if (!e) return NULL;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        e->arg[i] = inline_calls_in_expr(p, caller, e->arg[i], callee_idx, callee);
    }
    if (e->op == CB_CALL && e->index == callee_idx) {
        int base_slot = caller->locals;
        int needed_locals = callee->locals > callee->arity ? callee->locals : callee->arity;
        if (base_slot + needed_locals >= IR_MAX_LOCALS - 8 ||
            p->nodes + expr_size(callee->body) + 16 >= IR_MAX_NODES) {
            return e;
        }
        caller->locals += needed_locals;
        IrExpr *body_clone = clone_expr(p, callee->body, base_slot);
        if (!body_clone) return e;
        IrExpr *res = body_clone;
        for (int i = callee->arity - 1; i >= 0; --i) {
            res = make_let(p, base_slot + i, e->arg[i], res);
            if (!res) return e;
        }
        return res;
    }
    return e;
}

static void inline_pass(IrProg *p) {
    for (int round = 0; round < 3; ++round) {
        int changed = 0;
        for (int g = 0; g < p->functions; ++g) {
            if (g == p->entry) continue;
            simplify_func(p, &p->fn[g]);
            if (expr_calls_fn(p->fn[g].body, g)) continue;
            int g_size = expr_size(p->fn[g].body);
            if (g_size > 100) continue;
            for (int f = 0; f < p->functions; ++f) {
                if (f == g) continue;
                int calls = expr_count_calls_fn(p->fn[f].body, g);
                if (calls == 0) continue;
                if (expr_size(p->fn[f].body) + calls * g_size > 1200) continue;
                if (p->fn[f].locals + calls * (p->fn[g].locals + 4) >= IR_MAX_LOCALS - 16) continue;
                p->fn[f].body = inline_calls_in_expr(p, &p->fn[f], p->fn[f].body, g, &p->fn[g]);
                simplify_func(p, &p->fn[f]);
                changed = 1;
            }
        }
        if (!changed) break;
    }
}

/* Tail Recursion Modulo Associative Operator (TRMC) */
static int is_pure_tail_recursive(const IrExpr *e, int fn_idx, int in_tail) {
    if (!e) return 1;
    if (e->op == CB_CALL) {
        for (int i = 0; i < e->argc; ++i) {
            if (!is_pure_tail_recursive(e->arg[i], fn_idx, 0)) return 0;
        }
        if (e->index == fn_idx && !in_tail) return 0;
        return 1;
    }
    if (e->op == CB_IF) {
        return is_pure_tail_recursive(e->arg[0], fn_idx, 0) &&
               is_pure_tail_recursive(e->arg[1], fn_idx, in_tail) &&
               is_pure_tail_recursive(e->arg[2], fn_idx, in_tail);
    }
    if (e->op == CB_LET) {
        return is_pure_tail_recursive(e->arg[0], fn_idx, 0) &&
               is_pure_tail_recursive(e->arg[1], fn_idx, in_tail);
    }
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        if (!is_pure_tail_recursive(e->arg[i], fn_idx, 0)) return 0;
    }
    return 1;
}

static int check_non_nested_self_calls(const IrExpr *e, int fn_idx) {
    if (!e) return 1;
    if (e->op == CB_CALL && e->index == fn_idx) {
        for (int i = 0; i < e->argc; ++i) {
            if (expr_calls_fn(e->arg[i], fn_idx)) return 0;
        }
        return 1;
    }
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        if (!check_non_nested_self_calls(e->arg[i], fn_idx)) return 0;
    }
    return 1;
}

static int check_trmc_expr(const IrExpr *e, int fn_idx, CbOp op, int *found_trmc) {
    if (!e) return 0;
    if (e->op == CB_IF) {
        if (expr_calls_fn(e->arg[0], fn_idx)) return 0;
        return check_trmc_expr(e->arg[1], fn_idx, op, found_trmc) &&
               check_trmc_expr(e->arg[2], fn_idx, op, found_trmc);
    }
    if (e->op == CB_LET) {
        if (expr_calls_fn(e->arg[0], fn_idx)) return 0;
        return check_trmc_expr(e->arg[1], fn_idx, op, found_trmc);
    }
    if (e->op == CB_CALL && e->index == fn_idx) {
        for (int i = 0; i < e->argc; ++i) {
            if (expr_calls_fn(e->arg[i], fn_idx)) return 0;
        }
        return 1;
    }
    if (e->op == op) {
        if (check_non_nested_self_calls(e->arg[0], fn_idx) &&
            expr_calls_fn(e->arg[1], fn_idx) &&
            check_trmc_expr(e->arg[1], fn_idx, op, found_trmc)) {
            *found_trmc = 1;
            return 1;
        }
        if (e->arg[0]->op == CB_CALL && e->arg[0]->index == fn_idx &&
            check_non_nested_self_calls(e->arg[0], fn_idx) &&
            !expr_has_call(e->arg[1])) {
            *found_trmc = 1;
            return 1;
        }
    }
    return !expr_calls_fn(e, fn_idx);
}

static uint64_t assoc_identity(CbOp op) {
    switch (op) {
    case CB_ADD: case CB_XOR: case CB_OR: return 0ULL;
    case CB_MUL: return 1ULL;
    case CB_AND: return ~0ULL;
    default: return 0ULL;
    }
}

static void shift_slots_ge(IrExpr *e, int min_slot, int delta) {
    if (!e) return;
    if ((e->op == CB_VAR || e->op == CB_LET) && e->index >= min_slot) {
        e->index += delta;
    }
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) shift_slots_ge(e->arg[i], min_slot, delta);
}

static IrExpr *rewrite_self_calls_to_loop(IrProg *p, IrExpr *e, int fn_idx,
                                          int loop_idx, uint64_t ident) {
    if (!e) return NULL;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        e->arg[i] = rewrite_self_calls_to_loop(p, e->arg[i], fn_idx, loop_idx, ident);
    }
    if (e->op == CB_CALL && e->index == fn_idx) {
        e->index = loop_idx;
        e->arg[e->argc] = make_const(p, ident);
        e->argc += 1;
    }
    return e;
}

static IrExpr *transform_trmc_expr(IrProg *p, IrFunc *loop_fn, IrExpr *e,
                                   int fn_idx, int loop_idx, int acc_slot,
                                   CbOp op, uint64_t ident) {
    if (!e) return NULL;
    if (e->op == CB_IF) {
        e->arg[1] = transform_trmc_expr(p, loop_fn, e->arg[1], fn_idx, loop_idx, acc_slot, op, ident);
        e->arg[2] = transform_trmc_expr(p, loop_fn, e->arg[2], fn_idx, loop_idx, acc_slot, op, ident);
        return e;
    }
    if (e->op == CB_LET) {
        e->arg[1] = transform_trmc_expr(p, loop_fn, e->arg[1], fn_idx, loop_idx, acc_slot, op, ident);
        return e;
    }
    if (e->op == CB_CALL && e->index == fn_idx) {
        e->index = loop_idx;
        e->arg[e->argc] = make_var(p, acc_slot);
        e->argc += 1;
        return e;
    }
    if (e->op == op) {
        if (expr_calls_fn(e->arg[1], fn_idx)) {
            IrExpr *lhs = rewrite_self_calls_to_loop(p, e->arg[0], fn_idx, loop_idx, ident);
            int new_acc = loop_fn->locals++;
            IrExpr *acc_upd = make_bin(p, op, make_var(p, acc_slot), lhs);
            IrExpr *rhs = transform_trmc_expr(p, loop_fn, e->arg[1], fn_idx, loop_idx, new_acc, op, ident);
            return make_let(p, new_acc, acc_upd, rhs);
        }
        if (e->arg[0]->op == CB_CALL && e->arg[0]->index == fn_idx && !expr_has_call(e->arg[1])) {
            IrExpr *call = e->arg[0];
            call->index = loop_idx;
            call->arg[call->argc] = make_bin(p, op, make_var(p, acc_slot), e->arg[1]);
            call->argc += 1;
            return call;
        }
    }
    return make_bin(p, op, make_var(p, acc_slot), e);
}

static void trmc_pass(IrProg *p) {
    int orig_fns = p->functions;
    static const CbOp ops[] = {CB_ADD, CB_MUL, CB_XOR, CB_OR, CB_AND};
    for (int f = 0; f < orig_fns; ++f) {
        if (p->functions >= IR_MAX_FUNCS) break;
        IrFunc *fn = &p->fn[f];
        if (fn->arity >= IR_MAX_PARAMS) continue;
        if (!expr_calls_fn(fn->body, f)) continue;
        if (is_pure_tail_recursive(fn->body, f, 1)) continue;

        CbOp matched_op = CB_CONST;
        for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k) {
            int found = 0;
            if (check_trmc_expr(fn->body, f, ops[k], &found) && found) {
                matched_op = ops[k];
                break;
            }
        }
        if (matched_op == CB_CONST) continue;
        if (fn->locals + 16 >= IR_MAX_LOCALS || p->nodes + expr_size(fn->body) * 2 + 32 >= IR_MAX_NODES) continue;

        int loop_idx = p->functions++;
        IrFunc *lfn = &p->fn[loop_idx];
        memset(lfn, 0, sizeof(*lfn));
        snprintf(lfn->name, sizeof(lfn->name), "%s_trmc", fn->name);
        lfn->arity = fn->arity + 1;
        lfn->locals = (fn->locals > fn->arity ? fn->locals : fn->arity) + 1;
        lfn->body = clone_expr(p, fn->body, 0);
        if (!lfn->body) {
            p->functions--;
            continue;
        }
        int acc_slot = fn->arity;
        shift_slots_ge(lfn->body, acc_slot, 1);
        uint64_t ident = assoc_identity(matched_op);
        lfn->body = transform_trmc_expr(p, lfn, lfn->body, f, loop_idx, acc_slot, matched_op, ident);
        simplify_func(p, lfn);

        IrExpr *call = alloc_ir(p, CB_CALL);
        if (!call) continue;
        call->index = loop_idx;
        call->argc = lfn->arity;
        for (int i = 0; i < fn->arity; ++i) call->arg[i] = make_var(p, i);
        call->arg[fn->arity] = make_const(p, ident);
        fn->body = call;
        fn->locals = fn->arity;
    }
}

/* Push tail calls out of in-loop IFs to canonicalize single back-edge loops */
static int all_leaves_self_tail_call(const IrExpr *e, int fn_idx, int *call_count) {
    if (!e) return 0;
    if (e->op == CB_CALL && e->index == fn_idx) {
        for (int i = 0; i < e->argc; ++i) {
            if (expr_has_call(e->arg[i])) return 0;
        }
        (*call_count)++;
        return 1;
    }
    if (e->op == CB_IF) {
        if (expr_has_call(e->arg[0])) return 0;
        return all_leaves_self_tail_call(e->arg[1], fn_idx, call_count) &&
               all_leaves_self_tail_call(e->arg[2], fn_idx, call_count);
    }
    if (e->op == CB_LET) {
        if (expr_has_call(e->arg[0])) return 0;
        return all_leaves_self_tail_call(e->arg[1], fn_idx, call_count);
    }
    return 0;
}

static IrExpr *project_tail_arg(IrProg *p, IrFunc *fn, const IrExpr *e, int param_idx) {
    if (!e) return NULL;
    if (e->op == CB_CALL) {
        return clone_expr(p, e->arg[param_idx], 0);
    }
    if (e->op == CB_IF) {
        IrExpr *c = clone_expr(p, e->arg[0], 0);
        IrExpr *t = project_tail_arg(p, fn, e->arg[1], param_idx);
        IrExpr *f = project_tail_arg(p, fn, e->arg[2], param_idx);
        return make_if(p, c, t, f);
    }
    if (e->op == CB_LET) {
        int new_slot = fn->locals++;
        IrExpr *v = clone_expr(p, e->arg[0], 0);
        IrExpr *b = project_tail_arg(p, fn, e->arg[1], param_idx);
        IrExpr *var_repl = make_var(p, new_slot);
        b = subst_var(p, b, e->index, var_repl);
        return make_let(p, new_slot, v, b);
    }
    return NULL;
}

static IrExpr *collapse_tail_subtree(IrProg *p, IrFunc *fn, IrExpr *e, int fn_idx) {
    if (!e) return NULL;
    if (e->op == CB_LET) {
        e->arg[1] = collapse_tail_subtree(p, fn, e->arg[1], fn_idx);
        return e;
    }
    int call_cnt = 0;
    if (e->op == CB_IF && all_leaves_self_tail_call(e, fn_idx, &call_cnt) && call_cnt >= 2) {
        int sz = expr_size(e);
        if (sz <= 60 && fn->locals + fn->arity * 8 < IR_MAX_LOCALS - 16 &&
            p->nodes + sz * fn->arity + 32 < IR_MAX_NODES) {
            IrExpr *call = alloc_ir(p, CB_CALL);
            if (call) {
                call->index = fn_idx;
                call->argc = fn->arity;
                int ok = 1;
                for (int i = 0; i < fn->arity; ++i) {
                    call->arg[i] = project_tail_arg(p, fn, e, i);
                    if (!call->arg[i]) { ok = 0; break; }
                    call->arg[i] = simplify_expr(p, call->arg[i]);
                }
                if (ok) return call;
            }
        }
    }
    if (e->op == CB_IF) {
        e->arg[1] = collapse_tail_subtree(p, fn, e->arg[1], fn_idx);
        e->arg[2] = collapse_tail_subtree(p, fn, e->arg[2], fn_idx);
    }
    return e;
}

static void canonicalize_tail_loops_pass(IrProg *p) {
    for (int f = 0; f < p->functions; ++f) {
        IrFunc *fn = &p->fn[f];
        if (!expr_calls_fn(fn->body, f)) continue;
        if (!is_pure_tail_recursive(fn->body, f, 1)) continue;
        fn->body = collapse_tail_subtree(p, fn, fn->body, f);
        simplify_func(p, fn);
    }
}

/* Local CSE Pass on Call-Free Subexpressions */
static int expr_defines_any_var_in(const IrExpr *tree, const IrExpr *target) {
    if (!tree || !target) return 0;
    if (tree->op == CB_LET) {
        if (count_var_uses(target, tree->index) > 0) return 1;
    }
    int nc = num_children(tree);
    for (int i = 0; i < nc; ++i) {
        if (expr_defines_any_var_in(tree->arg[i], target)) return 1;
    }
    return 0;
}

static int count_subexpr_total(const IrExpr *tree, const IrExpr *sub) {
    if (!tree || !sub) return 0;
    if (expr_equal(tree, sub)) return 1;
    int c = 0;
    int nc = num_children(tree);
    for (int i = 0; i < nc; ++i) c += count_subexpr_total(tree->arg[i], sub);
    return c;
}

static int count_subexpr_uncond(const IrExpr *tree, const IrExpr *sub) {
    if (!tree || !sub) return 0;
    if (expr_equal(tree, sub)) return 1;
    if (tree->op == CB_IF) return count_subexpr_uncond(tree->arg[0], sub);
    int c = 0;
    int nc = num_children(tree);
    for (int i = 0; i < nc; ++i) c += count_subexpr_uncond(tree->arg[i], sub);
    return c;
}

static const IrExpr *find_repeated_uncond_subexpr(const IrExpr *root, const IrExpr *cur) {
    if (!cur) return NULL;
    if (cur->op != CB_CONST && cur->op != CB_VAR && cur->op != CB_LEN &&
        cur->op != CB_LET && cur->op != CB_IF && cur->op != CB_CALL &&
        !expr_has_call(cur)) {
        if (!expr_defines_any_var_in(root, cur) &&
            count_subexpr_uncond(root, cur) >= 1 &&
            count_subexpr_total(root, cur) >= 2) {
            return cur;
        }
    }
    if (cur->op == CB_IF) {
        return find_repeated_uncond_subexpr(root, cur->arg[0]);
    }
    int nc = num_children(cur);
    for (int i = 0; i < nc; ++i) {
        const IrExpr *found = find_repeated_uncond_subexpr(root, cur->arg[i]);
        if (found) return found;
    }
    return NULL;
}

static IrExpr *replace_subexpr_with_var(IrProg *p, IrExpr *tree, const IrExpr *sub, int slot) {
    if (!tree) return NULL;
    if (expr_equal(tree, sub)) {
        return make_var(p, slot);
    }
    int nc = num_children(tree);
    for (int i = 0; i < nc; ++i) {
        tree->arg[i] = replace_subexpr_with_var(p, tree->arg[i], sub, slot);
    }
    return tree;
}

static IrExpr *cse_walk(IrProg *p, IrFunc *fn, IrExpr *e) {
    if (!e) return NULL;
    for (int iter = 0; iter < 4; ++iter) {
        if (fn->locals >= IR_MAX_LOCALS - 8 || p->nodes >= IR_MAX_NODES - 128) break;
        const IrExpr *rep = find_repeated_uncond_subexpr(e, e);
        if (!rep) break;
        IrExpr *rep_clone = clone_expr(p, rep, 0);
        if (!rep_clone) break;
        int slot = fn->locals++;
        e = replace_subexpr_with_var(p, e, rep_clone, slot);
        e = make_let(p, slot, rep_clone, e);
        break;
    }
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        e->arg[i] = cse_walk(p, fn, e->arg[i]);
    }
    return e;
}

/* Compact local variables and rank by frequency */
static void compact_locals(IrFunc *fn) {
    int uses[IR_MAX_LOCALS];
    memset(uses, 0, sizeof(uses));
    for (int i = 0; i < fn->locals && i < IR_MAX_LOCALS; ++i) {
        uses[i] = count_var_uses(fn->body, i);
    }
    int remap[IR_MAX_LOCALS];
    for (int i = 0; i < IR_MAX_LOCALS; ++i) remap[i] = i;
    int next_slot = fn->arity;
    for (int i = fn->arity; i < fn->locals && i < IR_MAX_LOCALS; ++i) {
        if (uses[i] > 0) {
            remap[i] = next_slot++;
        }
    }
    /* Apply remap in-place */
    IrExpr *stack[IR_MAX_NODES];
    int sp = 0;
    if (fn->body) stack[sp++] = fn->body;
    while (sp > 0) {
        IrExpr *cur = stack[--sp];
        if ((cur->op == CB_VAR || cur->op == CB_LET) &&
            cur->index >= fn->arity && cur->index < IR_MAX_LOCALS) {
            cur->index = remap[cur->index];
        }
        int nc = num_children(cur);
        for (int i = 0; i < nc; ++i) {
            if (cur->arg[i]) stack[sp++] = cur->arg[i];
        }
    }
    fn->locals = next_slot;
}

/* AVX2 Loop Vectorization Analysis & Structures */
typedef enum {
    PK_INDEX = 0,
    PK_INVARIANT,
    PK_ACCUM
} ParamKind;

typedef enum {
    ACC_NONE = 0,
    ACC_ADD,
    ACC_XOR,
    ACC_OR,
    ACC_AND,
    ACC_MUL,
    ACC_MAX,
    ACC_MIN,
    ACC_HORNER_ADD,
    ACC_SHIFT_XOR
} AccumKind;

typedef struct {
    int valid;
    int idx_param;
    int cond_is_le;
    IrExpr *limit_expr;
    ParamKind pkind[IR_MAX_PARAMS];
    int num_accums;
    int acc_param[IR_MAX_PARAMS];
    AccumKind acc_kind[IR_MAX_PARAMS];
    IrExpr *acc_val[IR_MAX_PARAMS];
    IrExpr *horner_mult[IR_MAX_PARAMS];
    int shift_xor_k[IR_MAX_PARAMS];
    int max_get_offset;
    int has_contig_get;
    int uses_idx_vec;
    int uses_sign_bit;
} AvxLoopInfo;

static IrExpr *expand_lets_for_analysis(IrProg *p, const IrExpr *e, int depth) {
    if (!e || depth > 32 || p->nodes >= IR_MAX_NODES - 512) return NULL;
    if (e->op == CB_LET) {
        if (expr_has_call(e->arg[0]) || expr_size(e->arg[0]) > 24) return NULL;
        IrExpr *v = expand_lets_for_analysis(p, e->arg[0], depth + 1);
        IrExpr *b = expand_lets_for_analysis(p, e->arg[1], depth + 1);
        if (!v || !b) return NULL;
        IrExpr *sub = subst_var(p, b, e->index, v);
        return simplify_expr(p, sub);
    }
    IrExpr *c = alloc_ir(p, e->op);
    if (!c) return NULL;
    c->value = e->value;
    c->index = e->index;
    c->argc = e->argc;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        c->arg[i] = expand_lets_for_analysis(p, e->arg[i], depth + 1);
        if (!c->arg[i]) return NULL;
    }
    return simplify_expr(p, c);
}

static int expr_is_loop_invariant(const IrExpr *e, const ParamKind *pk, int arity) {
    if (!e) return 0;
    if (e->op == CB_CONST || e->op == CB_LEN) return 1;
    if (e->op == CB_VAR) {
        return (e->index >= 0 && e->index < arity && pk[e->index] == PK_INVARIANT);
    }
    if (e->op == CB_CALL || e->op == CB_LET) return 0;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        if (!expr_is_loop_invariant(e->arg[i], pk, arity)) return 0;
    }
    return 1;
}

static int expr_uses_only_idx_and_inv(const IrExpr *e, const ParamKind *pk, int arity) {
    if (!e) return 0;
    if (e->op == CB_CONST || e->op == CB_LEN) return 1;
    if (e->op == CB_VAR) {
        return (e->index >= 0 && e->index < arity &&
                (pk[e->index] == PK_INDEX || pk[e->index] == PK_INVARIANT));
    }
    if (e->op == CB_CALL || e->op == CB_LET) return 0;
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        if (!expr_uses_only_idx_and_inv(e->arg[i], pk, arity)) return 0;
    }
    return 1;
}

static IrExpr *extract_add_sub_rhs(IrProg *p, const IrExpr *e, int acc_slot) {
    if (!e) return NULL;
    if (e->op == CB_VAR && e->index == acc_slot) {
        return make_const(p, 0);
    }
    if (count_var_uses(e, acc_slot) == 0) return NULL;
    if (e->op == CB_ADD) {
        if (count_var_uses(e->arg[1], acc_slot) == 0) {
            IrExpr *r0 = extract_add_sub_rhs(p, e->arg[0], acc_slot);
            if (r0) return simplify_expr(p, make_bin(p, CB_ADD, r0, clone_expr(p, e->arg[1], 0)));
        }
        if (count_var_uses(e->arg[0], acc_slot) == 0) {
            IrExpr *r1 = extract_add_sub_rhs(p, e->arg[1], acc_slot);
            if (r1) return simplify_expr(p, make_bin(p, CB_ADD, clone_expr(p, e->arg[0], 0), r1));
        }
    } else if (e->op == CB_SUB) {
        if (count_var_uses(e->arg[1], acc_slot) == 0) {
            IrExpr *r0 = extract_add_sub_rhs(p, e->arg[0], acc_slot);
            if (r0) return simplify_expr(p, make_bin(p, CB_SUB, r0, clone_expr(p, e->arg[1], 0)));
        }
    } else if (e->op == CB_IF && count_var_uses(e->arg[0], acc_slot) == 0) {
        IrExpr *rt = extract_add_sub_rhs(p, e->arg[1], acc_slot);
        IrExpr *rf = extract_add_sub_rhs(p, e->arg[2], acc_slot);
        if (rt && rf) {
            return simplify_expr(p, make_if(p, clone_expr(p, e->arg[0], 0), rt, rf));
        }
    }
    return NULL;
}

static IrExpr *extract_assoc_rhs(IrProg *p, const IrExpr *e, int acc_slot, CbOp op, uint64_t ident) {
    if (!e) return NULL;
    if (e->op == CB_VAR && e->index == acc_slot) {
        return make_const(p, ident);
    }
    if (count_var_uses(e, acc_slot) == 0) return NULL;
    if (e->op == op) {
        if (count_var_uses(e->arg[1], acc_slot) == 0) {
            IrExpr *r0 = extract_assoc_rhs(p, e->arg[0], acc_slot, op, ident);
            if (r0) return simplify_expr(p, make_bin(p, op, r0, clone_expr(p, e->arg[1], 0)));
        }
        if (count_var_uses(e->arg[0], acc_slot) == 0) {
            IrExpr *r1 = extract_assoc_rhs(p, e->arg[1], acc_slot, op, ident);
            if (r1) return simplify_expr(p, make_bin(p, op, clone_expr(p, e->arg[0], 0), r1));
        }
    } else if (e->op == CB_IF && count_var_uses(e->arg[0], acc_slot) == 0) {
        IrExpr *rt = extract_assoc_rhs(p, e->arg[1], acc_slot, op, ident);
        IrExpr *rf = extract_assoc_rhs(p, e->arg[2], acc_slot, op, ident);
        if (rt && rf) {
            return simplify_expr(p, make_if(p, clone_expr(p, e->arg[0], 0), rt, rf));
        }
    }
    return NULL;
}

static IrExpr *match_direct_minmax(IrProg *p, const IrExpr *e, int acc_slot, int is_max) {
    if (!e || e->op != CB_IF) return NULL;
    const IrExpr *c = e->arg[0];
    const IrExpr *t = e->arg[1];
    const IrExpr *f = e->arg[2];
    if (c->op < CB_LT || c->op > CB_GE) return NULL;

    const IrExpr *val = NULL;
    int t_is_acc = (t->op == CB_VAR && t->index == acc_slot);
    int f_is_acc = (f->op == CB_VAR && f->index == acc_slot);
    if (!t_is_acc && !f_is_acc) return NULL;
    val = t_is_acc ? f : t;
    if (count_var_uses(val, acc_slot) != 0) return NULL;

    int lhs_is_acc = (c->arg[0]->op == CB_VAR && c->arg[0]->index == acc_slot && expr_equal(c->arg[1], val));
    int rhs_is_acc = (c->arg[1]->op == CB_VAR && c->arg[1]->index == acc_slot && expr_equal(c->arg[0], val));
    if (!lhs_is_acc && !rhs_is_acc) return NULL;

    /* Determine if condition means val > acc (or >=) vs val < acc (or <=) */
    CbOp eff_op = lhs_is_acc ? swap_cmp_op(c->op) : c->op; /* now eff_op compares (val OP acc) */
    int cond_is_gt = (eff_op == CB_GT || eff_op == CB_GE);
    int cond_is_lt = (eff_op == CB_LT || eff_op == CB_LE);
    int picks_val_when_true = f_is_acc;

    if (is_max) {
        if ((cond_is_gt && picks_val_when_true) || (cond_is_lt && !picks_val_when_true)) {
            return clone_expr(p, val, 0);
        }
    } else {
        if ((cond_is_lt && picks_val_when_true) || (cond_is_gt && !picks_val_when_true)) {
            return clone_expr(p, val, 0);
        }
    }
    return NULL;
}

static IrExpr *extract_minmax_rhs(IrProg *p, const IrExpr *e, int acc_slot, int is_max) {
    if (!e) return NULL;
    uint64_t ident = is_max ? 0ULL : ~0ULL;
    if (e->op == CB_VAR && e->index == acc_slot) {
        return make_const(p, ident);
    }
    IrExpr *direct = match_direct_minmax(p, e, acc_slot, is_max);
    if (direct) return direct;
    if (e->op == CB_IF && count_var_uses(e->arg[0], acc_slot) == 0) {
        IrExpr *rt = extract_minmax_rhs(p, e->arg[1], acc_slot, is_max);
        IrExpr *rf = extract_minmax_rhs(p, e->arg[2], acc_slot, is_max);
        if (rt && rf) {
            return simplify_expr(p, make_if(p, clone_expr(p, e->arg[0], 0), rt, rf));
        }
    }
    return NULL;
}

static int vec_expr_depth(const IrExpr *e) {
    if (!e) return 0;
    if (e->op == CB_CONST || e->op == CB_VAR || e->op == CB_LEN) return 0;
    if (e->op == CB_GET) return vec_expr_depth(e->arg[0]);
    if (e->op == CB_IF) {
        int d0 = vec_expr_depth(e->arg[0]);
        int d1 = vec_expr_depth(e->arg[1]) + 1;
        int d2 = vec_expr_depth(e->arg[2]) + 2;
        int m = d0 > d1 ? d0 : d1;
        return m > d2 ? m : d2;
    }
    int d0 = vec_expr_depth(e->arg[0]);
    int d1 = vec_expr_depth(e->arg[1]) + 1;
    return d0 > d1 ? d0 : d1;
}

static void inspect_vec_expr(const IrExpr *e, int idx_param, AvxLoopInfo *info) {
    if (!e) return;
    if (e->op == CB_GET) {
        const IrExpr *idx = e->arg[0];
        if (idx->op == CB_VAR && idx->index == idx_param) {
            info->has_contig_get = 1;
            if (info->max_get_offset < 0) info->max_get_offset = 0;
            return;
        }
        if (idx->op == CB_ADD && idx->arg[0]->op == CB_VAR &&
            idx->arg[0]->index == idx_param && idx->arg[1]->op == CB_CONST &&
            idx->arg[1]->value <= 64) {
            int off = (int)idx->arg[1]->value;
            info->has_contig_get = 1;
            if (off > info->max_get_offset) info->max_get_offset = off;
            return;
        }
        inspect_vec_expr(idx, idx_param, info);
        return;
    }
    if (e->op == CB_VAR && e->index == idx_param) {
        info->uses_idx_vec = 1;
    }
    if (e->op >= CB_LT && e->op <= CB_GE) {
        info->uses_sign_bit = 1;
    }
    int nc = num_children(e);
    for (int i = 0; i < nc; ++i) {
        inspect_vec_expr(e->arg[i], idx_param, info);
    }
}

static void analyze_avx2_loop(IrProg *p, const IrFunc *fn, int fn_idx, AvxLoopInfo *info) {
    memset(info, 0, sizeof(*info));
    info->max_get_offset = -1;
    if (fn->arity < 1 || fn->arity > 5) return;
    if (!expr_calls_fn(fn->body, fn_idx)) return;
    if (!is_pure_tail_recursive(fn->body, fn_idx, 1)) return;

    int saved_nodes = p->nodes;
    IrExpr *exp_body = expand_lets_for_analysis(p, fn->body, 0);
    if (!exp_body || exp_body->op != CB_IF) {
        p->nodes = saved_nodes;
        return;
    }

    IrExpr *cond = exp_body->arg[0];
    IrExpr *b1 = exp_body->arg[1];
    IrExpr *b2 = exp_body->arg[2];
    IrExpr *call_node = NULL;
    int cont_when_true = 0;

    if (b1->op == CB_CALL && b1->index == fn_idx && !expr_has_call(b2)) {
        call_node = b1;
        cont_when_true = 1;
    } else if (b2->op == CB_CALL && b2->index == fn_idx && !expr_has_call(b1)) {
        call_node = b2;
        cont_when_true = 0;
    } else {
        p->nodes = saved_nodes;
        return;
    }

    for (int i = 0; i < fn->arity; ++i) {
        if (expr_has_call(call_node->arg[i])) {
            p->nodes = saved_nodes;
            return;
        }
    }

    /* Identify candidate index parameter: next[i] = i + 1 */
    int idx_p = -1;
    for (int i = 0; i < fn->arity; ++i) {
        IrExpr *nx = call_node->arg[i];
        if (nx->op == CB_ADD && nx->arg[0]->op == CB_VAR && nx->arg[0]->index == i &&
            nx->arg[1]->op == CB_CONST && nx->arg[1]->value == 1) {
            idx_p = i;
            break;
        }
    }
    if (idx_p < 0) {
        p->nodes = saved_nodes;
        return;
    }

    /* Classify invariant parameters */
    for (int i = 0; i < fn->arity; ++i) {
        if (i == idx_p) {
            info->pkind[i] = PK_INDEX;
        } else if (call_node->arg[i]->op == CB_VAR && call_node->arg[i]->index == i) {
            info->pkind[i] = PK_INVARIANT;
        } else {
            info->pkind[i] = PK_ACCUM;
        }
    }

    /* Check loop condition compares idx_p with loop-invariant limit_expr */
    if (cond->op < CB_EQ || cond->op > CB_GE) {
        p->nodes = saved_nodes;
        return;
    }
    CbOp cmp_op = cont_when_true ? cond->op : negate_cmp_op(cond->op);
    IrExpr *lhs = cond->arg[0];
    IrExpr *rhs = cond->arg[1];
    if (rhs->op == CB_VAR && rhs->index == idx_p) {
        cmp_op = swap_cmp_op(cmp_op);
        IrExpr *tmp = lhs; lhs = rhs; rhs = tmp;
    }
    if (lhs->op != CB_VAR || lhs->index != idx_p) {
        p->nodes = saved_nodes;
        return;
    }
    if (!expr_is_loop_invariant(rhs, info->pkind, fn->arity)) {
        p->nodes = saved_nodes;
        return;
    }
    if (cmp_op == CB_LT || cmp_op == CB_NE) {
        info->cond_is_le = 0;
    } else if (cmp_op == CB_LE) {
        info->cond_is_le = 1;
    } else {
        p->nodes = saved_nodes;
        return;
    }
    info->idx_param = idx_p;
    info->limit_expr = rhs;

    /* Classify accumulator parameters */
    int num_acc = 0;
    for (int i = 0; i < fn->arity; ++i) {
        if (info->pkind[i] != PK_ACCUM) continue;
        if (num_acc >= 3) { p->nodes = saved_nodes; return; }
        IrExpr *nx = call_node->arg[i];
        AccumKind ak = ACC_NONE;
        IrExpr *rhs_val = NULL;
        IrExpr *h_mult = NULL;
        int sx_k = 0;

        if ((rhs_val = extract_add_sub_rhs(p, nx, i)) != NULL &&
            expr_uses_only_idx_and_inv(rhs_val, info->pkind, fn->arity)) {
            ak = ACC_ADD;
        } else if ((rhs_val = extract_assoc_rhs(p, nx, i, CB_XOR, 0ULL)) != NULL &&
                   expr_uses_only_idx_and_inv(rhs_val, info->pkind, fn->arity)) {
            ak = ACC_XOR;
        } else if ((rhs_val = extract_assoc_rhs(p, nx, i, CB_OR, 0ULL)) != NULL &&
                   expr_uses_only_idx_and_inv(rhs_val, info->pkind, fn->arity)) {
            ak = ACC_OR;
        } else if ((rhs_val = extract_assoc_rhs(p, nx, i, CB_AND, ~0ULL)) != NULL &&
                   expr_uses_only_idx_and_inv(rhs_val, info->pkind, fn->arity)) {
            ak = ACC_AND;
        } else if ((rhs_val = extract_assoc_rhs(p, nx, i, CB_MUL, 1ULL)) != NULL &&
                   expr_uses_only_idx_and_inv(rhs_val, info->pkind, fn->arity)) {
            ak = ACC_MUL;
        } else if ((rhs_val = extract_minmax_rhs(p, nx, i, 1)) != NULL &&
                   expr_uses_only_idx_and_inv(rhs_val, info->pkind, fn->arity)) {
            ak = ACC_MAX;
            info->uses_sign_bit = 1;
        } else if ((rhs_val = extract_minmax_rhs(p, nx, i, 0)) != NULL &&
                   expr_uses_only_idx_and_inv(rhs_val, info->pkind, fn->arity)) {
            ak = ACC_MIN;
            info->uses_sign_bit = 1;
        } else if (nx->op == CB_ADD || nx->op == CB_SUB) {
            /* Check Horner recurrence: (acc * C) +- val or (acc << k) +- val */
            IrExpr *term0 = nx->arg[0];
            IrExpr *term1 = nx->arg[1];
            for (int swap = 0; swap < 2; ++swap) {
                if (swap == 1) {
                    if (nx->op == CB_SUB) break;
                    IrExpr *t = term0; term0 = term1; term1 = t;
                }
                if (expr_uses_only_idx_and_inv(term1, info->pkind, fn->arity)) {
                    IrExpr *cand_c = NULL;
                    if (term0->op == CB_MUL) {
                        if (term0->arg[0]->op == CB_VAR && term0->arg[0]->index == i &&
                            expr_is_loop_invariant(term0->arg[1], info->pkind, fn->arity)) {
                            cand_c = term0->arg[1];
                        } else if (term0->arg[1]->op == CB_VAR && term0->arg[1]->index == i &&
                                   expr_is_loop_invariant(term0->arg[0], info->pkind, fn->arity)) {
                            cand_c = term0->arg[0];
                        }
                    } else if (term0->op == CB_SHL && term0->arg[0]->op == CB_VAR &&
                               term0->arg[0]->index == i && term0->arg[1]->op == CB_CONST) {
                        uint64_t sh = term0->arg[1]->value & 63ULL;
                        cand_c = make_const(p, 1ULL << sh);
                    }
                    if (cand_c) {
                        ak = ACC_HORNER_ADD;
                        h_mult = cand_c;
                        rhs_val = (nx->op == CB_SUB)
                                  ? simplify_expr(p, make_bin(p, CB_SUB, make_const(p, 0), clone_expr(p, term1, 0)))
                                  : clone_expr(p, term1, 0);
                        break;
                    }
                }
            }
        } else if (nx->op == CB_XOR) {
            /* Check (acc << k) ^ val */
            IrExpr *term0 = nx->arg[0];
            IrExpr *term1 = nx->arg[1];
            for (int swap = 0; swap < 2; ++swap) {
                if (swap == 1) { IrExpr *t = term0; term0 = term1; term1 = t; }
                if (term0->op == CB_SHL && term0->arg[0]->op == CB_VAR &&
                    term0->arg[0]->index == i && term0->arg[1]->op == CB_CONST &&
                    expr_uses_only_idx_and_inv(term1, info->pkind, fn->arity)) {
                    int k = (int)(term0->arg[1]->value & 63ULL);
                    if (k > 0 && k < 64) {
                        ak = ACC_SHIFT_XOR;
                        sx_k = k;
                        rhs_val = clone_expr(p, term1, 0);
                        break;
                    }
                }
            }
        }

        if (ak == ACC_NONE || !rhs_val || vec_expr_depth(rhs_val) > 4) {
            p->nodes = saved_nodes;
            return;
        }
        inspect_vec_expr(rhs_val, idx_p, info);
        info->acc_param[num_acc] = i;
        info->acc_kind[num_acc] = ak;
        info->acc_val[num_acc] = rhs_val;
        info->horner_mult[num_acc] = h_mult;
        info->shift_xor_k[num_acc] = sx_k;
        num_acc++;
    }

    if (num_acc == 0) {
        p->nodes = saved_nodes;
        return;
    }
    info->num_accums = num_acc;
    info->valid = 1;
}

/* Code Generation Context */
static const char *const arg_regs[IR_MAX_PARAMS] = {
    "%rdx", "%rcx", "%r8", "%r9", "%r10", "%r11"
};
static const char *const callee_saved_pool[5] = {
    "%rbx", "%r12", "%r13", "%r14", "%r15"
};
static const char *const extra_local_pool[2] = {
    "%r8", "%r9"
};

typedef struct {
    FILE *out;
    const IrProg *prog;
    const IrFunc *fn;
    int fn_idx;
    unsigned label_seq;
    int has_ext_or_nontail_call;
    int num_cs_used;
    const char *local_reg[IR_MAX_LOCALS];
    int local_off[IR_MAX_LOCALS];
    const char *scratch_regs[4];
    int num_scratch;
    int frame_bytes;
    int next_rbp_off;
    uint8_t proven_lt_len[IR_MAX_LOCALS];
} EmitCtx;

static int alloc_stack_slot(EmitCtx *s) {
    s->next_rbp_off -= 8;
    return s->next_rbp_off;
}

static int is_simple_rhs(const EmitCtx *s, const IrExpr *e) {
    if (!e) return 0;
    if (e->op == CB_CONST || e->op == CB_LEN) return 1;
    if (e->op == CB_VAR && e->index >= 0 && e->index < IR_MAX_LOCALS) return 1;
    (void)s;
    return 0;
}

static void prepass_expr(EmitCtx *s, IrExpr *e, int depth);

static void prepass_tail(EmitCtx *s, IrExpr *e) {
    if (!e) return;
    if (e->op == CB_IF) {
        prepass_expr(s, e->arg[0], 0);
        prepass_tail(s, e->arg[1]);
        prepass_tail(s, e->arg[2]);
        return;
    }
    if (e->op == CB_LET) {
        prepass_expr(s, e->arg[0], 0);
        prepass_tail(s, e->arg[1]);
        return;
    }
    if (e->op == CB_CALL) {
        int any_call = 0;
        for (int i = 0; i < e->argc; ++i) {
            if (expr_has_call(e->arg[i])) any_call = 1;
        }
        for (int i = 0; i < e->argc; ++i) {
            int d = (!any_call && i < 2) ? i : 0;
            prepass_expr(s, e->arg[i], d);
            if (i < e->argc - 1) {
                if (any_call || i >= 2 || d >= s->num_scratch) {
                    e->call_arg_off[i] = alloc_stack_slot(s);
                }
            }
        }
        return;
    }
    prepass_expr(s, e, 0);
}

static void prepass_expr(EmitCtx *s, IrExpr *e, int depth) {
    if (!e) return;
    if (e->op == CB_CONST || e->op == CB_VAR || e->op == CB_LEN) return;
    if (e->op == CB_GET) {
        prepass_expr(s, e->arg[0], depth);
        return;
    }
    if (e->op == CB_IF) {
        prepass_expr(s, e->arg[0], depth);
        prepass_expr(s, e->arg[1], depth);
        prepass_expr(s, e->arg[2], depth);
        return;
    }
    if (e->op == CB_LET) {
        prepass_expr(s, e->arg[0], depth);
        prepass_expr(s, e->arg[1], depth);
        return;
    }
    if (e->op == CB_CALL) {
        int any_call = 0;
        for (int i = 0; i < e->argc; ++i) {
            if (expr_has_call(e->arg[i])) any_call = 1;
        }
        for (int i = 0; i < e->argc; ++i) {
            int d = (!any_call && i < 2 && depth == 0) ? i : 0;
            prepass_expr(s, e->arg[i], d);
            if (i < e->argc - 1) {
                if (any_call || i >= 2 || depth > 0 || d >= s->num_scratch) {
                    e->call_arg_off[i] = alloc_stack_slot(s);
                }
            }
        }
        return;
    }
    /* Binary op */
    if (is_simple_rhs(s, e->arg[1]) && e->op != CB_DIV && e->op != CB_MOD) {
        prepass_expr(s, e->arg[0], depth);
        return;
    }
    if (e->arg[1]->op == CB_CONST && (e->op == CB_DIV || e->op == CB_MOD)) {
        prepass_expr(s, e->arg[0], depth);
        return;
    }
    prepass_expr(s, e->arg[0], depth);
    if (expr_has_call(e->arg[1]) || depth >= s->num_scratch) {
        e->spill_off = alloc_stack_slot(s);
        prepass_expr(s, e->arg[1], 0);
    } else {
        e->spill_off = 0;
        prepass_expr(s, e->arg[1], depth + 1);
    }
}

static void setup_function_frame(EmitCtx *s, IrFunc *fn, int fn_idx) {
    s->fn = fn;
    s->fn_idx = fn_idx;
    s->has_ext_or_nontail_call = !is_pure_tail_recursive(fn->body, fn_idx, 1);
    memset(s->local_reg, 0, sizeof(s->local_reg));
    memset(s->local_off, 0, sizeof(s->local_off));
    memset(s->proven_lt_len, 0, sizeof(s->proven_lt_len));

    int max_cs = 5;
    int cs_count = 0;
    for (int i = 0; i < fn->arity && i < max_cs; ++i) {
        s->local_reg[i] = callee_saved_pool[cs_count++];
    }

    /* Rank remaining locals by use count */
    int uses[IR_MAX_LOCALS];
    int order[IR_MAX_LOCALS];
    int nrem = 0;
    for (int i = fn->arity; i < fn->locals && i < IR_MAX_LOCALS; ++i) {
        uses[i] = count_var_uses(fn->body, i);
        if (uses[i] > 0) order[nrem++] = i;
    }
    for (int i = 0; i < nrem; ++i) {
        for (int j = i + 1; j < nrem; ++j) {
            if (uses[order[j]] > uses[order[i]]) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }

    int extra_used = 0;
    for (int k = 0; k < nrem; ++k) {
        int slot = order[k];
        if (cs_count < max_cs) {
            s->local_reg[slot] = callee_saved_pool[cs_count++];
        } else if (!s->has_ext_or_nontail_call && extra_used < 2) {
            s->local_reg[slot] = extra_local_pool[extra_used++];
        }
    }
    s->num_cs_used = cs_count;

    if (!s->has_ext_or_nontail_call && extra_used > 0) {
        s->scratch_regs[0] = "%r10";
        s->scratch_regs[1] = "%r11";
        s->num_scratch = 2;
    } else {
        s->scratch_regs[0] = "%r10";
        s->scratch_regs[1] = "%r11";
        s->scratch_regs[2] = "%r8";
        s->scratch_regs[3] = "%r9";
        s->num_scratch = 4;
    }

    s->next_rbp_off = -8 * s->num_cs_used;
    for (int i = 0; i < fn->locals && i < IR_MAX_LOCALS; ++i) {
        if (!s->local_reg[i] && (i < fn->arity || count_var_uses(fn->body, i) > 0)) {
            s->local_off[i] = alloc_stack_slot(s);
        }
    }

    prepass_tail(s, fn->body);
    int total_stack_bytes = (-s->next_rbp_off) - (8 * s->num_cs_used);
    if (total_stack_bytes > 0) {
        s->frame_bytes = (total_stack_bytes + 15) & ~15;
    } else {
        s->frame_bytes = 0;
    }
}

static void emit_load_const(FILE *f, uint64_t val, const char *reg64, const char *reg32) {
    if (val == 0) {
        fprintf(f, "  xorl %s, %s\n", reg32, reg32);
    } else if (val <= 0xFFFFFFFFULL) {
        fprintf(f, "  movl $0x%" PRIx64 ", %s\n", val, reg32);
    } else if (fits_imm32(val)) {
        fprintf(f, "  movq $%" PRId64 ", %s\n", (int64_t)val, reg64);
    } else {
        fprintf(f, "  movabsq $0x%016" PRIx64 ", %s\n", val, reg64);
    }
}

static void emit_load_var(EmitCtx *s, int slot, const char *dst_reg) {
    if (s->local_reg[slot]) {
        if (strcmp(s->local_reg[slot], dst_reg) != 0) {
            fprintf(s->out, "  movq %s, %s\n", s->local_reg[slot], dst_reg);
        }
    } else {
        fprintf(s->out, "  movq %d(%%rbp), %s\n", s->local_off[slot], dst_reg);
    }
}

static void emit_store_var(EmitCtx *s, int slot, const char *src_reg) {
    if (s->local_reg[slot]) {
        if (strcmp(s->local_reg[slot], src_reg) != 0) {
            fprintf(s->out, "  movq %s, %s\n", src_reg, s->local_reg[slot]);
        }
    } else {
        fprintf(s->out, "  movq %s, %d(%%rbp)\n", src_reg, s->local_off[slot]);
    }
}

static void emit_epilogue(EmitCtx *s) {
    FILE *f = s->out;
    if (s->frame_bytes > 0) {
        fprintf(f, "  addq $%d, %%rsp\n", s->frame_bytes);
    }
    for (int i = s->num_cs_used - 1; i >= 0; --i) {
        fprintf(f, "  popq %s\n", callee_saved_pool[i]);
    }
    if (s->frame_bytes > 0) {
        fprintf(f, "  popq %%rbp\n");
    }
}

static void emit_expr(EmitCtx *s, const IrExpr *e, int depth);

static void emit_div_mod_const(EmitCtx *s, uint64_t d, int is_mod) {
    FILE *f = s->out;
    if (d == 0) {
        fprintf(f, "  xorl %%eax, %%eax\n");
        return;
    }
    if (d == 1) {
        if (is_mod) fprintf(f, "  xorl %%eax, %%eax\n");
        return;
    }
    if (is_pow2(d)) {
        int k = ctz64(d);
        if (is_mod) {
            if (fits_imm32(d - 1ULL)) {
                fprintf(f, "  andq $%" PRId64 ", %%rax\n", (int64_t)(d - 1ULL));
            } else {
                emit_load_const(f, d - 1ULL, "%rcx", "%ecx");
                fprintf(f, "  andq %%rcx, %%rax\n");
            }
        } else {
            fprintf(f, "  shrq $%d, %%rax\n", k);
        }
        return;
    }
    uint64_t m_low = 0;
    int l = 0;
    compute_magic_u64(d, &m_low, &l);
    fprintf(f, "  movq %%rax, %%rcx\n");
    emit_load_const(f, m_low, "%rax", "%eax");
    fprintf(f, "  mulq %%rcx\n");
    if (is_mod) {
        fprintf(f, "  movq %%rcx, %%rax\n");
    }
    fprintf(f, "  subq %%rdx, %%rcx\n"
               "  shrq $1, %%rcx\n"
               "  addq %%rdx, %%rcx\n");
    if (l > 1) {
        fprintf(f, "  shrq $%d, %%rcx\n", l - 1);
    }
    if (is_mod) {
        if (fits_imm32(d)) {
            fprintf(f, "  imulq $%" PRId64 ", %%rcx, %%rcx\n", (int64_t)d);
        } else {
            emit_load_const(f, d, "%rdx", "%edx");
            fprintf(f, "  imulq %%rdx, %%rcx\n");
        }
        fprintf(f, "  subq %%rcx, %%rax\n");
    } else {
        fprintf(f, "  movq %%rcx, %%rax\n");
    }
}

static const char *cc_suffix(CbOp op) {
    static const char *const names[] = {"e", "ne", "b", "be", "a", "ae"};
    return names[op - CB_EQ];
}

static const char *inv_cc_suffix(CbOp op) {
    static const char *const names[] = {"ne", "e", "ae", "a", "be", "b"};
    return names[op - CB_EQ];
}

static void emit_cmp_flags(EmitCtx *s, const IrExpr *cmp, int depth) {
    FILE *f = s->out;
    const IrExpr *lhs = cmp->arg[0];
    const IrExpr *rhs = cmp->arg[1];

    if (lhs->op == CB_VAR && s->local_reg[lhs->index] && is_simple_rhs(s, rhs)) {
        const char *lreg = s->local_reg[lhs->index];
        if (rhs->op == CB_CONST && rhs->value == 0 && (cmp->op == CB_EQ || cmp->op == CB_NE)) {
            fprintf(f, "  testq %s, %s\n", lreg, lreg);
            return;
        }
        if (rhs->op == CB_CONST && fits_imm32(rhs->value)) {
            fprintf(f, "  cmpq $%" PRId64 ", %s\n", (int64_t)rhs->value, lreg);
            return;
        }
        if (rhs->op == CB_LEN) {
            fprintf(f, "  cmpq %%rsi, %s\n", lreg);
            return;
        }
        if (rhs->op == CB_VAR) {
            if (s->local_reg[rhs->index]) {
                fprintf(f, "  cmpq %s, %s\n", s->local_reg[rhs->index], lreg);
            } else {
                fprintf(f, "  cmpq %d(%%rbp), %s\n", s->local_off[rhs->index], lreg);
            }
            return;
        }
    }

    if (is_simple_rhs(s, rhs)) {
        emit_expr(s, lhs, depth);
        if (rhs->op == CB_CONST && rhs->value == 0 && (cmp->op == CB_EQ || cmp->op == CB_NE)) {
            fprintf(f, "  testq %%rax, %%rax\n");
        } else if (rhs->op == CB_CONST && fits_imm32(rhs->value)) {
            fprintf(f, "  cmpq $%" PRId64 ", %%rax\n", (int64_t)rhs->value);
        } else if (rhs->op == CB_CONST) {
            emit_load_const(f, rhs->value, "%rcx", "%ecx");
            fprintf(f, "  cmpq %%rcx, %%rax\n");
        } else if (rhs->op == CB_LEN) {
            fprintf(f, "  cmpq %%rsi, %%rax\n");
        } else if (rhs->op == CB_VAR) {
            if (s->local_reg[rhs->index]) {
                fprintf(f, "  cmpq %s, %%rax\n", s->local_reg[rhs->index]);
            } else {
                fprintf(f, "  cmpq %d(%%rbp), %%rax\n", s->local_off[rhs->index]);
            }
        }
        return;
    }

    emit_expr(s, lhs, depth);
    if (cmp->spill_off != 0) {
        fprintf(f, "  movq %%rax, %d(%%rbp)\n", cmp->spill_off);
        emit_expr(s, rhs, 0);
        fprintf(f, "  movq %%rax, %%rcx\n"
                   "  movq %d(%%rbp), %%rax\n", cmp->spill_off);
    } else {
        const char *sreg = s->scratch_regs[depth];
        fprintf(f, "  movq %%rax, %s\n", sreg);
        emit_expr(s, rhs, depth + 1);
        fprintf(f, "  movq %%rax, %%rcx\n"
                   "  movq %s, %%rax\n", sreg);
    }
    fprintf(f, "  cmpq %%rcx, %%rax\n");
}

static void emit_jump_if_false(EmitCtx *s, const IrExpr *cond, int depth, unsigned false_label) {
    FILE *f = s->out;
    if (cond->op >= CB_EQ && cond->op <= CB_GE) {
        emit_cmp_flags(s, cond, depth);
        fprintf(f, "  j%s .Lelse_%u\n", inv_cc_suffix(cond->op), false_label);
    } else {
        emit_expr(s, cond, depth);
        fprintf(f, "  testq %%rax, %%rax\n  jz .Lelse_%u\n", false_label);
    }
}

static int detect_lt_len_var(const IrExpr *cond, int *var_when_true, int *var_when_false) {
    *var_when_true = -1;
    *var_when_false = -1;
    if (!cond) return 0;
    if (cond->op == CB_LT && cond->arg[0]->op == CB_VAR && cond->arg[1]->op == CB_LEN) {
        *var_when_true = cond->arg[0]->index;
        return 1;
    }
    if (cond->op == CB_GT && cond->arg[0]->op == CB_LEN && cond->arg[1]->op == CB_VAR) {
        *var_when_true = cond->arg[1]->index;
        return 1;
    }
    if (cond->op == CB_GE && cond->arg[0]->op == CB_VAR && cond->arg[1]->op == CB_LEN) {
        *var_when_false = cond->arg[0]->index;
        return 1;
    }
    if (cond->op == CB_LE && cond->arg[0]->op == CB_LEN && cond->arg[1]->op == CB_VAR) {
        *var_when_false = cond->arg[1]->index;
        return 1;
    }
    return 0;
}

static void emit_ext_call_args(EmitCtx *s, const IrExpr *e, int depth) {
    FILE *f = s->out;
    int any_call = 0;
    for (int i = 0; i < e->argc; ++i) {
        if (expr_has_call(e->arg[i])) any_call = 1;
    }
    for (int i = 0; i < e->argc; ++i) {
        int d = (!any_call && i < 2 && depth == 0) ? i : 0;
        emit_expr(s, e->arg[i], d);
        if (i < e->argc - 1) {
            if (e->call_arg_off[i] != 0) {
                fprintf(f, "  movq %%rax, %d(%%rbp)\n", e->call_arg_off[i]);
            } else {
                fprintf(f, "  movq %%rax, %s\n", s->scratch_regs[i]);
            }
        }
    }
    if (e->argc > 0) {
        int last = e->argc - 1;
        fprintf(f, "  movq %%rax, %s\n", arg_regs[last]);
        for (int i = last - 1; i >= 0; --i) {
            if (e->call_arg_off[i] != 0) {
                fprintf(f, "  movq %d(%%rbp), %s\n", e->call_arg_off[i], arg_regs[i]);
            } else {
                fprintf(f, "  movq %s, %s\n", s->scratch_regs[i], arg_regs[i]);
            }
        }
    }
}

static void emit_inplace_param_update(EmitCtx *s, int slot, const IrExpr *rhs) {
    FILE *f = s->out;
    const char *dreg = s->local_reg[slot];
    if (dreg && (rhs->op == CB_ADD || rhs->op == CB_SUB || rhs->op == CB_XOR ||
                 rhs->op == CB_OR || rhs->op == CB_AND || rhs->op == CB_MUL)) {
        const IrExpr *other = NULL;
        if (rhs->arg[0]->op == CB_VAR && rhs->arg[0]->index == slot &&
            count_var_uses(rhs->arg[1], slot) == 0) {
            other = rhs->arg[1];
        } else if (rhs->op != CB_SUB && rhs->arg[1]->op == CB_VAR &&
                   rhs->arg[1]->index == slot && count_var_uses(rhs->arg[0], slot) == 0) {
            other = rhs->arg[0];
        }
        if (other) {
            const char *mnem = NULL;
            switch (rhs->op) {
            case CB_ADD: mnem = "addq"; break;
            case CB_SUB: mnem = "subq"; break;
            case CB_XOR: mnem = "xorq"; break;
            case CB_OR:  mnem = "orq";  break;
            case CB_AND: mnem = "andq"; break;
            case CB_MUL: mnem = "imulq"; break;
            default: break;
            }
            if (mnem) {
                if (other->op == CB_CONST && fits_imm32(other->value)) {
                    fprintf(f, "  %s $%" PRId64 ", %s\n", mnem, (int64_t)other->value, dreg);
                    return;
                }
                if (other->op == CB_VAR && s->local_reg[other->index]) {
                    fprintf(f, "  %s %s, %s\n", mnem, s->local_reg[other->index], dreg);
                    return;
                }
                if (other->op == CB_GET && other->arg[0]->op == CB_VAR) {
                    int idx_v = other->arg[0]->index;
                    if (idx_v >= 0 && idx_v < IR_MAX_LOCALS &&
                        s->proven_lt_len[idx_v] && s->local_reg[idx_v]) {
                        fprintf(f, "  %s (%%rdi,%s,8), %s\n", mnem, s->local_reg[idx_v], dreg);
                        return;
                    }
                }
                emit_expr(s, other, 0);
                fprintf(f, "  %s %%rax, %s\n", mnem, dreg);
                return;
            }
        }
    }
    emit_expr(s, rhs, 0);
    emit_store_var(s, slot, "%rax");
}

static void emit_self_tail_call(EmitCtx *s, const IrExpr *e) {
    FILE *f = s->out;
    int argc = e->argc;
    int any_call = 0;
    for (int i = 0; i < argc; ++i) {
        if (expr_has_call(e->arg[i])) any_call = 1;
    }

    if (!any_call) {
        int done[IR_MAX_PARAMS];
        int remaining = 0;
        for (int i = 0; i < argc; ++i) {
            if (e->arg[i]->op == CB_VAR && e->arg[i]->index == i) {
                done[i] = 1;
            } else {
                done[i] = 0;
                remaining++;
            }
        }
        /* Greedily emit in-place updates for parameters not read by remaining undone args */
        while (remaining > 0) {
            int picked = -1;
            for (int j = 0; j < argc; ++j) {
                if (done[j]) continue;
                int read_by_other = 0;
                for (int k = 0; k < argc; ++k) {
                    if (k == j || done[k]) continue;
                    if (count_var_uses(e->arg[k], j) > 0) {
                        read_by_other = 1;
                        break;
                    }
                }
                if (!read_by_other) {
                    picked = j;
                    break;
                }
            }
            if (picked < 0) break;
            emit_inplace_param_update(s, picked, e->arg[picked]);
            done[picked] = 1;
            remaining--;
        }

        if (remaining == 0) {
            fprintf(f, "  jmp .Lscalar_%d\n", s->fn_idx);
            return;
        }
        /* Resolve remaining cyclic parameters (e.g. swap (a, b) = (b, a+b)) */
        if (remaining == 2 && s->num_scratch >= 1) {
            int idx0 = -1, idx1 = -1;
            for (int j = 0; j < argc; ++j) {
                if (!done[j]) {
                    if (idx0 < 0) idx0 = j;
                    else idx1 = j;
                }
            }
            emit_expr(s, e->arg[idx0], 0);
            fprintf(f, "  movq %%rax, %s\n", s->scratch_regs[0]);
            emit_expr(s, e->arg[idx1], 1);
            emit_store_var(s, idx1, "%rax");
            emit_store_var(s, idx0, s->scratch_regs[0]);
            fprintf(f, "  jmp .Lscalar_%d\n", s->fn_idx);
            return;
        }
    }

    /* General fallback for self-tail-call */
    for (int i = 0; i < argc; ++i) {
        int d = (!any_call && i < 2) ? i : 0;
        emit_expr(s, e->arg[i], d);
        if (i < argc - 1) {
            if (e->call_arg_off[i] != 0) {
                fprintf(f, "  movq %%rax, %d(%%rbp)\n", e->call_arg_off[i]);
            } else {
                fprintf(f, "  movq %%rax, %s\n", s->scratch_regs[i]);
            }
        }
    }
    if (argc > 0) {
        int last = argc - 1;
        emit_store_var(s, last, "%rax");
        for (int i = last - 1; i >= 0; --i) {
            if (e->call_arg_off[i] != 0) {
                fprintf(f, "  movq %d(%%rbp), %%rax\n", e->call_arg_off[i]);
                emit_store_var(s, i, "%rax");
            } else {
                emit_store_var(s, i, s->scratch_regs[i]);
            }
        }
    }
    fprintf(f, "  jmp .Lscalar_%d\n", s->fn_idx);
}

static void emit_expr(EmitCtx *s, const IrExpr *e, int depth) {
    FILE *f = s->out;
    switch (e->op) {
    case CB_CONST:
        emit_load_const(f, e->value, "%rax", "%eax");
        return;
    case CB_VAR:
        emit_load_var(s, e->index, "%rax");
        return;
    case CB_LEN:
        fprintf(f, "  movq %%rsi, %%rax\n");
        return;
    case CB_GET: {
        const IrExpr *idx = e->arg[0];
        if (idx->op == CB_VAR && idx->index >= 0 && idx->index < IR_MAX_LOCALS &&
            s->proven_lt_len[idx->index] && s->local_reg[idx->index]) {
            fprintf(f, "  movq (%%rdi,%s,8), %%rax\n", s->local_reg[idx->index]);
            return;
        }
        unsigned lbl = s->label_seq++;
        emit_expr(s, idx, depth);
        if (idx->op == CB_VAR && idx->index >= 0 && idx->index < IR_MAX_LOCALS &&
            s->proven_lt_len[idx->index]) {
            fprintf(f, "  movq (%%rdi,%%rax,8), %%rax\n");
            return;
        }
        fprintf(f, "  xorl %%ecx, %%ecx\n"
                   "  cmpq %%rsi, %%rax\n"
                   "  jae .Lget_%u\n"
                   "  movq (%%rdi,%%rax,8), %%rcx\n"
                   ".Lget_%u:\n"
                   "  movq %%rcx, %%rax\n", lbl, lbl);
        return;
    }
    case CB_IF: {
        unsigned lbl = s->label_seq++;
        int vt = -1, vf = -1;
        detect_lt_len_var(e->arg[0], &vt, &vf);
        emit_jump_if_false(s, e->arg[0], depth, lbl);
        uint8_t saved_t = 0;
        if (vt >= 0 && vt < IR_MAX_LOCALS) {
            saved_t = s->proven_lt_len[vt];
            s->proven_lt_len[vt] = 1;
        }
        emit_expr(s, e->arg[1], depth);
        if (vt >= 0 && vt < IR_MAX_LOCALS) s->proven_lt_len[vt] = saved_t;
        fprintf(f, "  jmp .Ldone_%u\n.Lelse_%u:\n", lbl, lbl);
        uint8_t saved_f = 0;
        if (vf >= 0 && vf < IR_MAX_LOCALS) {
            saved_f = s->proven_lt_len[vf];
            s->proven_lt_len[vf] = 1;
        }
        emit_expr(s, e->arg[2], depth);
        if (vf >= 0 && vf < IR_MAX_LOCALS) s->proven_lt_len[vf] = saved_f;
        fprintf(f, ".Ldone_%u:\n", lbl);
        return;
    }
    case CB_LET:
        emit_expr(s, e->arg[0], depth);
        emit_store_var(s, e->index, "%rax");
        emit_expr(s, e->arg[1], depth);
        return;
    case CB_CALL:
        emit_ext_call_args(s, e, depth);
        fprintf(f, "  call .Lfn%d\n", e->index);
        return;
    default:
        break;
    }

    /* Binary operations */
    const IrExpr *lhs = e->arg[0];
    const IrExpr *rhs = e->arg[1];

    if (e->op >= CB_EQ && e->op <= CB_GE) {
        emit_cmp_flags(s, e, depth);
        fprintf(f, "  set%s %%al\n  movzbl %%al, %%eax\n", cc_suffix(e->op));
        return;
    }

    if ((e->op == CB_DIV || e->op == CB_MOD) && rhs->op == CB_CONST) {
        emit_expr(s, lhs, depth);
        emit_div_mod_const(s, rhs->value, e->op == CB_MOD);
        return;
    }

    if (is_simple_rhs(s, rhs) && e->op != CB_DIV && e->op != CB_MOD) {
        emit_expr(s, lhs, depth);
        if (e->op == CB_SHL || e->op == CB_SHR) {
            const char *sh_op = (e->op == CB_SHL) ? "shlq" : "shrq";
            if (rhs->op == CB_CONST) {
                uint64_t sh = rhs->value & 63ULL;
                if (sh != 0) fprintf(f, "  %s $%" PRIu64 ", %%rax\n", sh_op, sh);
            } else if (rhs->op == CB_LEN) {
                fprintf(f, "  movq %%rsi, %%rcx\n  %s %%cl, %%rax\n", sh_op);
            } else if (rhs->op == CB_VAR) {
                emit_load_var(s, rhs->index, "%rcx");
                fprintf(f, "  %s %%cl, %%rax\n", sh_op);
            }
            return;
        }
        const char *mnem = NULL;
        switch (e->op) {
        case CB_ADD: mnem = "addq"; break;
        case CB_SUB: mnem = "subq"; break;
        case CB_MUL: mnem = "imulq"; break;
        case CB_AND: mnem = "andq"; break;
        case CB_OR:  mnem = "orq";  break;
        case CB_XOR: mnem = "xorq"; break;
        default: break;
        }
        if (rhs->op == CB_CONST) {
            if (fits_imm32(rhs->value)) {
                if (e->op == CB_MUL) {
                    fprintf(f, "  imulq $%" PRId64 ", %%rax, %%rax\n", (int64_t)rhs->value);
                } else {
                    fprintf(f, "  %s $%" PRId64 ", %%rax\n", mnem, (int64_t)rhs->value);
                }
            } else {
                emit_load_const(f, rhs->value, "%rcx", "%ecx");
                fprintf(f, "  %s %%rcx, %%rax\n", mnem);
            }
        } else if (rhs->op == CB_LEN) {
            fprintf(f, "  %s %%rsi, %%rax\n", mnem);
        } else if (rhs->op == CB_VAR) {
            if (s->local_reg[rhs->index]) {
                fprintf(f, "  %s %s, %%rax\n", mnem, s->local_reg[rhs->index]);
            } else {
                fprintf(f, "  %s %d(%%rbp), %%rax\n", mnem, s->local_off[rhs->index]);
            }
        }
        return;
    }

    /* General binary operation */
    emit_expr(s, lhs, depth);
    if (e->spill_off != 0) {
        fprintf(f, "  movq %%rax, %d(%%rbp)\n", e->spill_off);
        emit_expr(s, rhs, 0);
        fprintf(f, "  movq %%rax, %%rcx\n"
                   "  movq %d(%%rbp), %%rax\n", e->spill_off);
    } else {
        const char *sreg = s->scratch_regs[depth];
        fprintf(f, "  movq %%rax, %s\n", sreg);
        emit_expr(s, rhs, depth + 1);
        fprintf(f, "  movq %%rax, %%rcx\n"
                   "  movq %s, %%rax\n", sreg);
    }

    switch (e->op) {
    case CB_ADD: fprintf(f, "  addq %%rcx, %%rax\n"); break;
    case CB_SUB: fprintf(f, "  subq %%rcx, %%rax\n"); break;
    case CB_MUL: fprintf(f, "  imulq %%rcx, %%rax\n"); break;
    case CB_AND: fprintf(f, "  andq %%rcx, %%rax\n"); break;
    case CB_OR:  fprintf(f, "  orq %%rcx, %%rax\n"); break;
    case CB_XOR: fprintf(f, "  xorq %%rcx, %%rax\n"); break;
    case CB_SHL: fprintf(f, "  shlq %%cl, %%rax\n"); break;
    case CB_SHR: fprintf(f, "  shrq %%cl, %%rax\n"); break;
    case CB_DIV:
    case CB_MOD: {
        unsigned lbl = s->label_seq++;
        fprintf(f, "  testq %%rcx, %%rcx\n"
                   "  jz .Ldivz_%u\n"
                   "  xorl %%edx, %%edx\n"
                   "  divq %%rcx\n", lbl);
        if (e->op == CB_MOD) fprintf(f, "  movq %%rdx, %%rax\n");
        fprintf(f, "  jmp .Ldivd_%u\n"
                   ".Ldivz_%u:\n"
                   "  xorl %%eax, %%eax\n"
                   ".Ldivd_%u:\n", lbl, lbl, lbl);
        break;
    }
    default:
        break;
    }
}

static void emit_tail(EmitCtx *s, const IrExpr *e) {
    FILE *f = s->out;
    if (e->op == CB_IF) {
        unsigned lbl = s->label_seq++;
        int vt = -1, vf = -1;
        detect_lt_len_var(e->arg[0], &vt, &vf);
        emit_jump_if_false(s, e->arg[0], 0, lbl);
        uint8_t saved_t = 0;
        if (vt >= 0 && vt < IR_MAX_LOCALS) {
            saved_t = s->proven_lt_len[vt];
            s->proven_lt_len[vt] = 1;
        }
        emit_tail(s, e->arg[1]);
        if (vt >= 0 && vt < IR_MAX_LOCALS) s->proven_lt_len[vt] = saved_t;
        fprintf(f, ".Lelse_%u:\n", lbl);
        uint8_t saved_f = 0;
        if (vf >= 0 && vf < IR_MAX_LOCALS) {
            saved_f = s->proven_lt_len[vf];
            s->proven_lt_len[vf] = 1;
        }
        emit_tail(s, e->arg[2]);
        if (vf >= 0 && vf < IR_MAX_LOCALS) s->proven_lt_len[vf] = saved_f;
        return;
    }
    if (e->op == CB_LET) {
        emit_expr(s, e->arg[0], 0);
        emit_store_var(s, e->index, "%rax");
        emit_tail(s, e->arg[1]);
        return;
    }
    if (e->op == CB_CALL) {
        if (e->index == s->fn_idx) {
            emit_self_tail_call(s, e);
        } else {
            emit_ext_call_args(s, e, 0);
            emit_epilogue(s);
            fprintf(f, "  jmp .Lfn%d\n", e->index);
        }
        return;
    }
    emit_expr(s, e, 0);
    emit_epilogue(s);
    fprintf(f, "  ret\n");
}

/* Shrink-Wrapped Base-Case Fast Path for Non-Tail Recursive Functions */
static int is_fast_base_atom(const IrExpr *e, int arity) {
    if (!e) return 0;
    if (e->op == CB_CONST || e->op == CB_LEN) return 1;
    if (e->op == CB_VAR && e->index >= 0 && e->index < arity) return 1;
    return 0;
}

static int is_fast_base_expr(const IrExpr *e, int arity) {
    if (!e) return 0;
    if (is_fast_base_atom(e, arity)) return 1;
    if ((e->op == CB_ADD || e->op == CB_SUB || e->op == CB_XOR ||
         e->op == CB_OR || e->op == CB_AND) &&
        is_fast_base_atom(e->arg[0], arity) && is_fast_base_atom(e->arg[1], arity)) {
        return 1;
    }
    return 0;
}

static void emit_fast_base_atom(FILE *f, const IrExpr *e, const char *dst64, const char *dst32) {
    if (e->op == CB_CONST) {
        emit_load_const(f, e->value, dst64, dst32);
    } else if (e->op == CB_LEN) {
        fprintf(f, "  movq %%rsi, %s\n", dst64);
    } else if (e->op == CB_VAR) {
        if (strcmp(arg_regs[e->index], dst64) != 0) {
            fprintf(f, "  movq %s, %s\n", arg_regs[e->index], dst64);
        }
    }
}

static void emit_fast_base_expr(FILE *f, const IrExpr *e) {
    if (is_fast_base_atom(e, IR_MAX_PARAMS)) {
        emit_fast_base_atom(f, e, "%rax", "%eax");
        return;
    }
    emit_fast_base_atom(f, e->arg[0], "%rax", "%eax");
    emit_fast_base_atom(f, e->arg[1], "%r10", "%r10d");
    switch (e->op) {
    case CB_ADD: fprintf(f, "  addq %%r10, %%rax\n"); break;
    case CB_SUB: fprintf(f, "  subq %%r10, %%rax\n"); break;
    case CB_XOR: fprintf(f, "  xorq %%r10, %%rax\n"); break;
    case CB_OR:  fprintf(f, "  orq %%r10, %%rax\n"); break;
    case CB_AND: fprintf(f, "  andq %%r10, %%rax\n"); break;
    default: break;
    }
}

static void try_emit_shrink_wrap(EmitCtx *s) {
    const IrFunc *fn = s->fn;
    if (!s->has_ext_or_nontail_call) return;
    if (s->num_cs_used == 0 && s->frame_bytes == 0) return;
    const IrExpr *body = fn->body;
    if (!body || body->op != CB_IF) return;

    const IrExpr *cond = body->arg[0];
    if (cond->op < CB_EQ || cond->op > CB_GE) return;
    if (!is_fast_base_expr(cond->arg[0], fn->arity) ||
        !is_fast_base_atom(cond->arg[1], fn->arity)) return;

    const IrExpr *base_b = NULL;
    CbOp jump_to_rec_op = cond->op;
    if (!expr_has_call(body->arg[1]) && is_fast_base_expr(body->arg[1], fn->arity)) {
        base_b = body->arg[1];
        jump_to_rec_op = negate_cmp_op(cond->op);
    } else if (!expr_has_call(body->arg[2]) && is_fast_base_expr(body->arg[2], fn->arity)) {
        base_b = body->arg[2];
        jump_to_rec_op = cond->op;
    } else {
        return;
    }

    FILE *f = s->out;
    unsigned lbl = s->label_seq++;
    emit_fast_base_expr(f, cond->arg[0]);
    if (cond->arg[1]->op == CB_CONST && fits_imm32(cond->arg[1]->value)) {
        fprintf(f, "  cmpq $%" PRId64 ", %%rax\n", (int64_t)cond->arg[1]->value);
    } else {
        emit_fast_base_atom(f, cond->arg[1], "%r10", "%r10d");
        fprintf(f, "  cmpq %%r10, %%rax\n");
    }
    fprintf(f, "  j%s .Lprol_%u\n", cc_suffix(jump_to_rec_op), lbl);
    emit_fast_base_expr(f, base_b);
    fprintf(f, "  ret\n.Lprol_%u:\n", lbl);
}

/* AVX2 Vector Expression & Loop Codegen */
static void emit_ymm_mul64(FILE *f, int y_a, int y_b, int y_out) {
    /* Multiplies 4x64-bit unsigned lanes of ymm(y_a) and ymm(y_b) into ymm(y_out)
       using %ymm1 and %ymm2 as temporaries (caller ensures y_a, y_b != 1, 2, or y_b == 1). */
    if (y_b == 1) {
        /* Copy %ymm1 to %ymm2 first if needed */
        fprintf(f, "  vpsrlq $32, %%ymm%d, %%ymm2\n"
                   "  vpmuludq %%ymm1, %%ymm2, %%ymm2\n"
                   "  vpsrlq $32, %%ymm1, %%ymm1\n"
                   "  vpmuludq %%ymm%d, %%ymm1, %%ymm1\n"
                   "  vpaddq %%ymm2, %%ymm1, %%ymm1\n"
                   "  vpsllq $32, %%ymm1, %%ymm1\n"
                   "  vpmuludq %%ymm%d, %%ymm%d, %%ymm%d\n",
                y_a, y_a, y_b, y_a, y_out);
        return;
    }
    fprintf(f, "  vpsrlq $32, %%ymm%d, %%ymm1\n"
               "  vpsrlq $32, %%ymm%d, %%ymm2\n"
               "  vpmuludq %%ymm%d, %%ymm1, %%ymm1\n"
               "  vpmuludq %%ymm%d, %%ymm2, %%ymm2\n"
               "  vpaddq %%ymm2, %%ymm1, %%ymm1\n"
               "  vpsllq $32, %%ymm1, %%ymm1\n"
               "  vpmuludq %%ymm%d, %%ymm%d, %%ymm%d\n"
               "  vpaddq %%ymm1, %%ymm%d, %%ymm%d\n",
            y_a, y_b, y_b, y_a, y_b, y_a, y_out, y_out, y_out);
}

static void emit_vec_expr(EmitCtx *s, const AvxLoopInfo *info, const IrExpr *e, int vdepth) {
    FILE *f = s->out;
    switch (e->op) {
    case CB_CONST:
        if (e->value == 0) {
            fprintf(f, "  vpxor %%ymm0, %%ymm0, %%ymm0\n");
        } else if (e->value == ~0ULL) {
            fprintf(f, "  vpcmpeqq %%ymm0, %%ymm0, %%ymm0\n");
        } else {
            emit_load_const(f, e->value, "%rax", "%eax");
            fprintf(f, "  vmovq %%rax, %%xmm0\n  vpbroadcastq %%xmm0, %%ymm0\n");
        }
        return;
    case CB_LEN:
        fprintf(f, "  vmovq %%rsi, %%xmm0\n  vpbroadcastq %%xmm0, %%ymm0\n");
        return;
    case CB_VAR:
        if (e->index == info->idx_param) {
            fprintf(f, "  vmovdqa %%ymm3, %%ymm0\n");
        } else {
            emit_load_var(s, e->index, "%rax");
            fprintf(f, "  vmovq %%rax, %%xmm0\n  vpbroadcastq %%xmm0, %%ymm0\n");
        }
        return;
    case CB_GET: {
        const IrExpr *idx = e->arg[0];
        const char *idx_reg = s->local_reg[info->idx_param];
        if (idx->op == CB_VAR && idx->index == info->idx_param) {
            fprintf(f, "  vmovdqu (%%rdi,%s,8), %%ymm0\n", idx_reg);
            return;
        }
        if (idx->op == CB_ADD && idx->arg[0]->op == CB_VAR &&
            idx->arg[0]->index == info->idx_param && idx->arg[1]->op == CB_CONST &&
            idx->arg[1]->value <= 64) {
            int byte_off = (int)idx->arg[1]->value * 8;
            fprintf(f, "  vmovdqu %d(%%rdi,%s,8), %%ymm0\n", byte_off, idx_reg);
            return;
        }
        /* Safe 4-lane bounds-checked gather fallback */
        emit_vec_expr(s, info, idx, vdepth);
        unsigned l0 = s->label_seq++, l1 = s->label_seq++;
        unsigned l2 = s->label_seq++, l3 = s->label_seq++;
        fprintf(f, "  vmovq %%xmm0, %%rax\n"
                   "  xorl %%ecx, %%ecx\n"
                   "  cmpq %%rsi, %%rax\n"
                   "  jae .Lvget_%u\n"
                   "  movq (%%rdi,%%rax,8), %%rcx\n"
                   ".Lvget_%u:\n"
                   "  vpextrq $1, %%xmm0, %%rax\n"
                   "  xorl %%edx, %%edx\n"
                   "  cmpq %%rsi, %%rax\n"
                   "  jae .Lvget_%u\n"
                   "  movq (%%rdi,%%rax,8), %%rdx\n"
                   ".Lvget_%u:\n"
                   "  vmovq %%rcx, %%xmm1\n"
                   "  vpinsrq $1, %%rdx, %%xmm1, %%xmm1\n"
                   "  vextracti128 $1, %%ymm0, %%xmm2\n"
                   "  vmovq %%xmm2, %%rax\n"
                   "  xorl %%ecx, %%ecx\n"
                   "  cmpq %%rsi, %%rax\n"
                   "  jae .Lvget_%u\n"
                   "  movq (%%rdi,%%rax,8), %%rcx\n"
                   ".Lvget_%u:\n"
                   "  vpextrq $1, %%xmm2, %%rax\n"
                   "  xorl %%edx, %%edx\n"
                   "  cmpq %%rsi, %%rax\n"
                   "  jae .Lvget_%u\n"
                   "  movq (%%rdi,%%rax,8), %%rdx\n"
                   ".Lvget_%u:\n"
                   "  vmovq %%rcx, %%xmm2\n"
                   "  vpinsrq $1, %%rdx, %%xmm2, %%xmm2\n"
                   "  vinserti128 $1, %%xmm2, %%ymm1, %%ymm0\n",
                l0, l0, l1, l1, l2, l2, l3, l3);
        return;
    }
    case CB_IF: {
        int s_cond = 9 + vdepth;
        int s_then = 9 + vdepth + 1;
        emit_vec_expr(s, info, e->arg[0], vdepth);
        fprintf(f, "  vpxor %%ymm1, %%ymm1, %%ymm1\n"
                   "  vpcmpeqq %%ymm1, %%ymm0, %%ymm%d\n", s_cond);
        emit_vec_expr(s, info, e->arg[1], vdepth + 1);
        fprintf(f, "  vmovdqa %%ymm0, %%ymm%d\n", s_then);
        emit_vec_expr(s, info, e->arg[2], vdepth + 2);
        /* Blend: Then ^ ((Then ^ Else) & Mask_cond_eq_0) */
        fprintf(f, "  vpxor %%ymm%d, %%ymm0, %%ymm0\n"
                   "  vpand %%ymm%d, %%ymm0, %%ymm0\n"
                   "  vpxor %%ymm%d, %%ymm0, %%ymm0\n",
                s_then, s_cond, s_then);
        return;
    }
    default:
        break;
    }

    /* Binary vector ops */
    const IrExpr *lhs = e->arg[0];
    const IrExpr *rhs = e->arg[1];

    if ((e->op == CB_SHL || e->op == CB_SHR) && rhs->op == CB_CONST) {
        emit_vec_expr(s, info, lhs, vdepth);
        int sh = (int)(rhs->value & 63ULL);
        if (sh != 0) {
            fprintf(f, "  %s $%d, %%ymm0, %%ymm0\n",
                    e->op == CB_SHL ? "vpsllq" : "vpsrlq", sh);
        }
        return;
    }

    if ((e->op == CB_DIV || e->op == CB_MOD) && rhs->op == CB_CONST) {
        emit_vec_expr(s, info, lhs, vdepth);
        /* 4-lane scalar magic div/mod */
        fprintf(f, "  vextracti128 $1, %%ymm0, %%xmm2\n"
                   "  vmovq %%xmm0, %%rax\n");
        emit_div_mod_const(s, rhs->value, e->op == CB_MOD);
        fprintf(f, "  vmovq %%rax, %%xmm1\n"
                   "  vpextrq $1, %%xmm0, %%rax\n");
        emit_div_mod_const(s, rhs->value, e->op == CB_MOD);
        fprintf(f, "  vpinsrq $1, %%rax, %%xmm1, %%xmm1\n"
                   "  vmovq %%xmm2, %%rax\n");
        emit_div_mod_const(s, rhs->value, e->op == CB_MOD);
        fprintf(f, "  vmovq %%rax, %%xmm0\n"
                   "  vpextrq $1, %%xmm2, %%rax\n");
        emit_div_mod_const(s, rhs->value, e->op == CB_MOD);
        fprintf(f, "  vpinsrq $1, %%rax, %%xmm0, %%xmm0\n"
                   "  vinserti128 $1, %%xmm0, %%ymm1, %%ymm0\n");
        return;
    }

    int s_lhs = 9 + vdepth;
    emit_vec_expr(s, info, lhs, vdepth);
    fprintf(f, "  vmovdqa %%ymm0, %%ymm%d\n", s_lhs);
    emit_vec_expr(s, info, rhs, vdepth + 1);

    /* Now lhs is in %ymm{s_lhs}, rhs is in %ymm0 */
    switch (e->op) {
    case CB_ADD: fprintf(f, "  vpaddq %%ymm0, %%ymm%d, %%ymm0\n", s_lhs); break;
    case CB_SUB: fprintf(f, "  vpsubq %%ymm0, %%ymm%d, %%ymm0\n", s_lhs); break;
    case CB_AND: fprintf(f, "  vpand %%ymm0, %%ymm%d, %%ymm0\n", s_lhs); break;
    case CB_OR:  fprintf(f, "  vpor %%ymm0, %%ymm%d, %%ymm0\n", s_lhs); break;
    case CB_XOR: fprintf(f, "  vpxor %%ymm0, %%ymm%d, %%ymm0\n", s_lhs); break;
    case CB_MUL:
        emit_ymm_mul64(f, s_lhs, 0, 0);
        break;
    case CB_SHL:
    case CB_SHR:
        fprintf(f, "  movl $63, %%eax\n"
                   "  vmovq %%rax, %%xmm1\n"
                   "  vpbroadcastq %%xmm1, %%ymm1\n"
                   "  vpand %%ymm1, %%ymm0, %%ymm0\n"
                   "  %s %%ymm0, %%ymm%d, %%ymm0\n",
                e->op == CB_SHL ? "vpsllvq" : "vpsrlvq", s_lhs);
        break;
    case CB_EQ:
        fprintf(f, "  vpcmpeqq %%ymm0, %%ymm%d, %%ymm0\n"
                   "  vpsrlq $63, %%ymm0, %%ymm0\n", s_lhs);
        break;
    case CB_NE:
        fprintf(f, "  vpcmpeqq %%ymm0, %%ymm%d, %%ymm0\n"
                   "  vpcmpeqq %%ymm1, %%ymm1, %%ymm1\n"
                   "  vpxor %%ymm1, %%ymm0, %%ymm0\n"
                   "  vpsrlq $63, %%ymm0, %%ymm0\n", s_lhs);
        break;
    case CB_GT:
        fprintf(f, "  vpxor %%ymm5, %%ymm%d, %%ymm1\n"
                   "  vpxor %%ymm5, %%ymm0, %%ymm2\n"
                   "  vpcmpgtq %%ymm2, %%ymm1, %%ymm0\n"
                   "  vpsrlq $63, %%ymm0, %%ymm0\n", s_lhs);
        break;
    case CB_LT:
        fprintf(f, "  vpxor %%ymm5, %%ymm%d, %%ymm1\n"
                   "  vpxor %%ymm5, %%ymm0, %%ymm2\n"
                   "  vpcmpgtq %%ymm1, %%ymm2, %%ymm0\n"
                   "  vpsrlq $63, %%ymm0, %%ymm0\n", s_lhs);
        break;
    case CB_GE:
        fprintf(f, "  vpxor %%ymm5, %%ymm%d, %%ymm1\n"
                   "  vpxor %%ymm5, %%ymm0, %%ymm2\n"
                   "  vpcmpgtq %%ymm1, %%ymm2, %%ymm0\n"
                   "  vpcmpeqq %%ymm1, %%ymm1, %%ymm1\n"
                   "  vpxor %%ymm1, %%ymm0, %%ymm0\n"
                   "  vpsrlq $63, %%ymm0, %%ymm0\n", s_lhs);
        break;
    case CB_LE:
        fprintf(f, "  vpxor %%ymm5, %%ymm%d, %%ymm1\n"
                   "  vpxor %%ymm5, %%ymm0, %%ymm2\n"
                   "  vpcmpgtq %%ymm2, %%ymm1, %%ymm0\n"
                   "  vpcmpeqq %%ymm1, %%ymm1, %%ymm1\n"
                   "  vpxor %%ymm1, %%ymm0, %%ymm0\n"
                   "  vpsrlq $63, %%ymm0, %%ymm0\n", s_lhs);
        break;
    case CB_DIV:
    case CB_MOD: {
        for (int lane = 0; lane < 4; ++lane) {
            unsigned lz = s->label_seq++, ld = s->label_seq++;
            if (lane == 0) {
                fprintf(f, "  vmovq %%xmm%d, %%rax\n  vmovq %%xmm0, %%rcx\n", s_lhs);
            } else if (lane == 1) {
                fprintf(f, "  vpextrq $1, %%xmm%d, %%rax\n  vpextrq $1, %%xmm0, %%rcx\n", s_lhs);
            } else if (lane == 2) {
                fprintf(f, "  vextracti128 $1, %%ymm%d, %%xmm1\n"
                           "  vextracti128 $1, %%ymm0, %%xmm2\n"
                           "  vmovq %%xmm1, %%rax\n  vmovq %%xmm2, %%rcx\n", s_lhs);
            } else {
                fprintf(f, "  vpextrq $1, %%xmm1, %%rax\n  vpextrq $1, %%xmm2, %%rcx\n");
            }
            fprintf(f, "  testq %%rcx, %%rcx\n"
                       "  jz .Lvdivz_%u\n"
                       "  xorl %%edx, %%edx\n"
                       "  divq %%rcx\n", lz);
            if (e->op == CB_MOD) fprintf(f, "  movq %%rdx, %%rax\n");
            fprintf(f, "  jmp .Lvdivd_%u\n.Lvdivz_%u:\n  xorl %%eax, %%eax\n.Lvdivd_%u:\n",
                    ld, lz, ld);
            if (lane == 0) fprintf(f, "  movq %%rax, %%r10\n");
            else if (lane == 1) fprintf(f, "  vmovq %%r10, %%xmm%d\n  vpinsrq $1, %%rax, %%xmm%d, %%xmm%d\n", s_lhs, s_lhs, s_lhs);
            else if (lane == 2) fprintf(f, "  movq %%rax, %%r10\n");
            else fprintf(f, "  vmovq %%r10, %%xmm2\n  vpinsrq $1, %%rax, %%xmm2, %%xmm2\n  vinserti128 $1, %%xmm2, %%ymm%d, %%ymm0\n", s_lhs);
        }
        break;
    }
    default:
        break;
    }
}

static void emit_avx2_loop(EmitCtx *s, const AvxLoopInfo *info) {
    FILE *f = s->out;
    int fn_idx = s->fn_idx;
    const char *r_i = s->local_reg[info->idx_param];

    /* 1. Evaluate limit_expr into %r11 and check vector loop bounds */
    emit_expr(s, info->limit_expr, 0);
    fprintf(f, "  movq %%rax, %%r11\n");

    if (info->cond_is_le) {
        fprintf(f, "  cmpq %%r11, %s\n  ja .Lscalar_%d\n", r_i, fn_idx);
        if (info->has_contig_get) {
            int moff = info->max_get_offset;
            fprintf(f, "  cmpq $%d, %%rsi\n  jb .Lscalar_%d\n"
                       "  movq %%rsi, %%rax\n", moff + 4, fn_idx);
            if (moff > 0) fprintf(f, "  subq $%d, %%rax\n", moff);
            fprintf(f, "  cmpq %%rax, %s\n  jae .Lscalar_%d\n"
                       "  subq %s, %%rax\n"
                       "  movq %%r11, %%rcx\n"
                       "  subq %s, %%rcx\n"
                       "  cmpq %%rcx, %%rax\n"
                       "  cmovaq %%rcx, %%rax\n",
                    r_i, fn_idx, r_i, r_i);
        } else {
            fprintf(f, "  movq %%r11, %%rax\n  subq %s, %%rax\n", r_i);
        }
    } else {
        fprintf(f, "  cmpq %%r11, %s\n  jae .Lscalar_%d\n", r_i, fn_idx);
        if (info->has_contig_get) {
            int moff = info->max_get_offset;
            fprintf(f, "  cmpq $%d, %%rsi\n  jb .Lscalar_%d\n"
                       "  movq %%rsi, %%rax\n", moff + 4, fn_idx);
            if (moff > 0) fprintf(f, "  subq $%d, %%rax\n", moff);
            fprintf(f, "  cmpq %%rax, %%r11\n"
                       "  cmovaq %%rax, %%r11\n"
                       "  cmpq %%r11, %s\n  jae .Lscalar_%d\n",
                    r_i, fn_idx);
        }
        fprintf(f, "  movq %%r11, %%rax\n  subq %s, %%rax\n", r_i);
    }

    fprintf(f, "  cmpq $4, %%rax\n"
               "  jb .Lscalar_%d\n"
               "  andq $-4, %%rax\n"
               "  leaq (%s,%%rax), %%r11\n",
            fn_idx, r_i);

    /* 2. Initialize YMM index/sign/accumulators */
    if (info->uses_idx_vec) {
        fprintf(f, "  vmovq %s, %%xmm3\n"
                   "  leaq 1(%s), %%rax\n"
                   "  vpinsrq $1, %%rax, %%xmm3, %%xmm3\n"
                   "  leaq 2(%s), %%rax\n"
                   "  vmovq %%rax, %%xmm1\n"
                   "  leaq 3(%s), %%rax\n"
                   "  vpinsrq $1, %%rax, %%xmm1, %%xmm1\n"
                   "  vinserti128 $1, %%xmm1, %%ymm3, %%ymm3\n"
                   "  movl $4, %%eax\n"
                   "  vmovq %%rax, %%xmm4\n"
                   "  vpbroadcastq %%xmm4, %%ymm4\n",
                r_i, r_i, r_i, r_i);
    }

    if (info->uses_sign_bit) {
        fprintf(f, "  movabsq $0x8000000000000000, %%rax\n"
                   "  vmovq %%rax, %%xmm5\n"
                   "  vpbroadcastq %%xmm5, %%ymm5\n");
    }

    for (int a = 0; a < info->num_accums; ++a) {
        int y_acc = 6 + a;
        const char *r_acc = s->local_reg[info->acc_param[a]];
        switch (info->acc_kind[a]) {
        case ACC_ADD:
        case ACC_XOR:
        case ACC_OR:
            fprintf(f, "  vpxor %%ymm%d, %%ymm%d, %%ymm%d\n", y_acc, y_acc, y_acc);
            break;
        case ACC_AND:
            fprintf(f, "  vpcmpeqq %%ymm%d, %%ymm%d, %%ymm%d\n", y_acc, y_acc, y_acc);
            break;
        case ACC_MUL:
            fprintf(f, "  movl $1, %%eax\n"
                       "  vmovq %%rax, %%xmm%d\n"
                       "  vpbroadcastq %%xmm%d, %%ymm%d\n", y_acc, y_acc, y_acc);
            break;
        case ACC_MAX:
        case ACC_MIN:
            fprintf(f, "  vmovq %s, %%xmm%d\n"
                       "  vpbroadcastq %%xmm%d, %%ymm%d\n", r_acc, y_acc, y_acc, y_acc);
            break;
        case ACC_HORNER_ADD:
        case ACC_SHIFT_XOR:
            fprintf(f, "  vpxor %%ymm%d, %%ymm%d, %%ymm%d\n"
                       "  vpinsrq $1, %s, %%xmm%d, %%xmm1\n"
                       "  vinserti128 $1, %%xmm1, %%ymm%d, %%ymm%d\n",
                    y_acc, y_acc, y_acc, r_acc, y_acc, y_acc, y_acc);
            break;
        default:
            break;
        }
    }

    /* 3. Emit main AVX2 loop body */
    fprintf(f, ".p2align 4\n.Lvec_%d:\n", fn_idx);
    for (int a = 0; a < info->num_accums; ++a) {
        int y_acc = 6 + a;
        emit_vec_expr(s, info, info->acc_val[a], 0);
        switch (info->acc_kind[a]) {
        case ACC_ADD:
            fprintf(f, "  vpaddq %%ymm0, %%ymm%d, %%ymm%d\n", y_acc, y_acc);
            break;
        case ACC_XOR:
            fprintf(f, "  vpxor %%ymm0, %%ymm%d, %%ymm%d\n", y_acc, y_acc);
            break;
        case ACC_OR:
            fprintf(f, "  vpor %%ymm0, %%ymm%d, %%ymm%d\n", y_acc, y_acc);
            break;
        case ACC_AND:
            fprintf(f, "  vpand %%ymm0, %%ymm%d, %%ymm%d\n", y_acc, y_acc);
            break;
        case ACC_MUL:
            emit_ymm_mul64(f, y_acc, 0, y_acc);
            break;
        case ACC_MAX:
            fprintf(f, "  vpxor %%ymm5, %%ymm0, %%ymm1\n"
                       "  vpxor %%ymm5, %%ymm%d, %%ymm2\n"
                       "  vpcmpgtq %%ymm2, %%ymm1, %%ymm1\n"
                       "  vpxor %%ymm%d, %%ymm0, %%ymm2\n"
                       "  vpand %%ymm1, %%ymm2, %%ymm2\n"
                       "  vpxor %%ymm2, %%ymm%d, %%ymm%d\n",
                    y_acc, y_acc, y_acc, y_acc);
            break;
        case ACC_MIN:
            fprintf(f, "  vpxor %%ymm5, %%ymm0, %%ymm1\n"
                       "  vpxor %%ymm5, %%ymm%d, %%ymm2\n"
                       "  vpcmpgtq %%ymm2, %%ymm1, %%ymm1\n"
                       "  vpxor %%ymm%d, %%ymm0, %%ymm2\n"
                       "  vpand %%ymm1, %%ymm2, %%ymm2\n"
                       "  vpxor %%ymm2, %%ymm0, %%ymm%d\n",
                    y_acc, y_acc, y_acc);
            break;
        case ACC_HORNER_ADD: {
            const IrExpr *hm = info->horner_mult[a];
            if (hm->op == CB_CONST && is_pow2(hm->value)) {
                int k = ctz64(hm->value);
                if (4 * k < 64) {
                    fprintf(f, "  vpsllq $%d, %%ymm%d, %%ymm%d\n"
                               "  vpaddq %%ymm0, %%ymm%d, %%ymm%d\n",
                            4 * k, y_acc, y_acc, y_acc, y_acc);
                } else {
                    fprintf(f, "  vmovdqa %%ymm0, %%ymm%d\n", y_acc);
                }
            } else {
                if (hm->op == CB_CONST) {
                    uint64_t c = hm->value;
                    uint64_t c2 = c * c;
                    uint64_t c4 = c2 * c2;
                    emit_load_const(f, c4, "%rax", "%eax");
                } else {
                    emit_expr(s, hm, 0);
                    fprintf(f, "  imulq %%rax, %%rax\n  imulq %%rax, %%rax\n");
                }
                fprintf(f, "  vmovq %%rax, %%xmm1\n"
                           "  vpbroadcastq %%xmm1, %%ymm1\n"
                           "  vmovdqa %%ymm0, %%ymm9\n");
                emit_ymm_mul64(f, y_acc, 1, y_acc);
                fprintf(f, "  vpaddq %%ymm1, %%ymm%d, %%ymm%d\n"
                           "  vpaddq %%ymm9, %%ymm%d, %%ymm%d\n",
                        y_acc, y_acc, y_acc, y_acc);
            }
            break;
        }
        case ACC_SHIFT_XOR: {
            int k4 = 4 * info->shift_xor_k[a];
            if (k4 < 64) {
                fprintf(f, "  vpsllq $%d, %%ymm%d, %%ymm%d\n"
                           "  vpxor %%ymm0, %%ymm%d, %%ymm%d\n",
                        k4, y_acc, y_acc, y_acc, y_acc);
            } else {
                fprintf(f, "  vmovdqa %%ymm0, %%ymm%d\n", y_acc);
            }
            break;
        }
        default:
            break;
        }
    }

    if (info->uses_idx_vec) {
        fprintf(f, "  vpaddq %%ymm4, %%ymm3, %%ymm3\n");
    }
    fprintf(f, "  addq $4, %s\n"
               "  cmpq %%r11, %s\n"
               "  jb .Lvec_%d\n",
            r_i, r_i, fn_idx);

    /* 4. Horizontal reduction of %ymm{6+a} into scalar accumulator registers */
    for (int a = 0; a < info->num_accums; ++a) {
        int y_acc = 6 + a;
        const char *r_acc = s->local_reg[info->acc_param[a]];
        switch (info->acc_kind[a]) {
        case ACC_ADD:
            fprintf(f, "  vextracti128 $1, %%ymm%d, %%xmm1\n"
                       "  vpaddq %%xmm1, %%xmm%d, %%xmm1\n"
                       "  vpsrldq $8, %%xmm1, %%xmm2\n"
                       "  vpaddq %%xmm2, %%xmm1, %%xmm1\n"
                       "  vmovq %%xmm1, %%rax\n"
                       "  addq %%rax, %s\n",
                    y_acc, y_acc, r_acc);
            break;
        case ACC_XOR:
            fprintf(f, "  vextracti128 $1, %%ymm%d, %%xmm1\n"
                       "  vpxor %%xmm1, %%xmm%d, %%xmm1\n"
                       "  vpsrldq $8, %%xmm1, %%xmm2\n"
                       "  vpxor %%xmm2, %%xmm1, %%xmm1\n"
                       "  vmovq %%xmm1, %%rax\n"
                       "  xorq %%rax, %s\n",
                    y_acc, y_acc, r_acc);
            break;
        case ACC_OR:
            fprintf(f, "  vextracti128 $1, %%ymm%d, %%xmm1\n"
                       "  vpor %%xmm1, %%xmm%d, %%xmm1\n"
                       "  vpsrldq $8, %%xmm1, %%xmm2\n"
                       "  vpor %%xmm2, %%xmm1, %%xmm1\n"
                       "  vmovq %%xmm1, %%rax\n"
                       "  orq %%rax, %s\n",
                    y_acc, y_acc, r_acc);
            break;
        case ACC_AND:
            fprintf(f, "  vextracti128 $1, %%ymm%d, %%xmm1\n"
                       "  vpand %%xmm1, %%xmm%d, %%xmm1\n"
                       "  vpsrldq $8, %%xmm1, %%xmm2\n"
                       "  vpand %%xmm2, %%xmm1, %%xmm1\n"
                       "  vmovq %%xmm1, %%rax\n"
                       "  andq %%rax, %s\n",
                    y_acc, y_acc, r_acc);
            break;
        case ACC_MUL:
            fprintf(f, "  vextracti128 $1, %%ymm%d, %%xmm1\n"
                       "  vmovq %%xmm%d, %%rax\n"
                       "  imulq %%rax, %s\n"
                       "  vpextrq $1, %%xmm%d, %%rax\n"
                       "  imulq %%rax, %s\n"
                       "  vmovq %%xmm1, %%rax\n"
                       "  imulq %%rax, %s\n"
                       "  vpextrq $1, %%xmm1, %%rax\n"
                       "  imulq %%rax, %s\n",
                    y_acc, y_acc, r_acc, y_acc, r_acc, r_acc, r_acc);
            break;
        case ACC_MAX:
        case ACC_MIN: {
            const char *cmov = (info->acc_kind[a] == ACC_MAX) ? "cmovaq" : "cmovbq";
            fprintf(f, "  vextracti128 $1, %%ymm%d, %%xmm1\n"
                       "  vmovq %%xmm%d, %%rax\n"
                       "  cmpq %s, %%rax\n  %s %%rax, %s\n"
                       "  vpextrq $1, %%xmm%d, %%rax\n"
                       "  cmpq %s, %%rax\n  %s %%rax, %s\n"
                       "  vmovq %%xmm1, %%rax\n"
                       "  cmpq %s, %%rax\n  %s %%rax, %s\n"
                       "  vpextrq $1, %%xmm1, %%rax\n"
                       "  cmpq %s, %%rax\n  %s %%rax, %s\n",
                    y_acc, y_acc, r_acc, cmov, r_acc,
                    y_acc, r_acc, cmov, r_acc,
                    r_acc, cmov, r_acc,
                    r_acc, cmov, r_acc);
            break;
        }
        case ACC_HORNER_ADD: {
            emit_expr(s, info->horner_mult[a], 0);
            fprintf(f, "  movq %%rax, %%rcx\n"
                       "  vextracti128 $1, %%ymm%d, %%xmm1\n"
                       "  vmovq %%xmm%d, %%rax\n"
                       "  vpextrq $1, %%xmm%d, %%rdx\n"
                       "  imulq %%rcx, %%rax\n"
                       "  addq %%rdx, %%rax\n"
                       "  vmovq %%xmm1, %%rdx\n"
                       "  imulq %%rcx, %%rax\n"
                       "  addq %%rdx, %%rax\n"
                       "  vpextrq $1, %%xmm1, %%rdx\n"
                       "  imulq %%rcx, %%rax\n"
                       "  addq %%rdx, %%rax\n"
                       "  movq %%rax, %s\n",
                    y_acc, y_acc, y_acc, r_acc);
            break;
        }
        case ACC_SHIFT_XOR: {
            int k = info->shift_xor_k[a];
            fprintf(f, "  vextracti128 $1, %%ymm%d, %%xmm1\n"
                       "  vmovq %%xmm%d, %%rax\n"
                       "  vpextrq $1, %%xmm%d, %%rdx\n"
                       "  shlq $%d, %%rax\n"
                       "  xorq %%rdx, %%rax\n"
                       "  vmovq %%xmm1, %%rdx\n"
                       "  shlq $%d, %%rax\n"
                       "  xorq %%rdx, %%rax\n"
                       "  vpextrq $1, %%xmm1, %%rdx\n"
                       "  shlq $%d, %%rax\n"
                       "  xorq %%rdx, %%rax\n"
                       "  movq %%rax, %s\n",
                    y_acc, y_acc, y_acc, k, k, k, r_acc);
            break;
        }
        default:
            break;
        }
    }
}

static void emit_function(EmitCtx *s, IrProg *p, int fn_idx) {
    FILE *f = s->out;
    IrFunc *fn = &p->fn[fn_idx];
    setup_function_frame(s, fn, fn_idx);

    int saved_nodes = p->nodes;
    AvxLoopInfo avx_info;
    analyze_avx2_loop(p, fn, fn_idx, &avx_info);

    fprintf(f, ".p2align 4\n.Lfn%d:\n", fn_idx);
    try_emit_shrink_wrap(s);

    /* Prologue */
    if (s->frame_bytes > 0) {
        fprintf(f, "  pushq %%rbp\n  movq %%rsp, %%rbp\n");
    }
    for (int i = 0; i < s->num_cs_used; ++i) {
        fprintf(f, "  pushq %s\n", callee_saved_pool[i]);
    }
    if (s->frame_bytes > 0) {
        fprintf(f, "  subq $%d, %%rsp\n", s->frame_bytes);
    }

    for (int i = 0; i < fn->arity; ++i) {
        emit_store_var(s, i, arg_regs[i]);
    }

    if (avx_info.valid) {
        emit_avx2_loop(s, &avx_info);
    }
    p->nodes = saved_nodes;

    fprintf(f, ".p2align 4\n.Lscalar_%d:\n", fn_idx);
    emit_tail(s, fn->body);
}

int cb_emit_assembly(const CbProgram *p, const char *path,
                     char *error, size_t error_size) {
    if (!p || !path) {
        if (error && error_size > 0) snprintf(error, error_size, "invalid arguments");
        return 0;
    }
    for (int i = 0; i < p->nodes; ++i) {
        if (p->pool[i].op > CB_GE) {
            if (error && error_size > 0) {
                snprintf(error, error_size, "v1 baseline supports scalar ASTs only");
            }
            return 0;
        }
    }

    IrProg *ir = (IrProg *)calloc(1, sizeof(IrProg));
    if (!ir) {
        if (error && error_size > 0) snprintf(error, error_size, "out of memory");
        return 0;
    }

    ir->functions = p->functions;
    ir->entry = p->entry;
    for (int i = 0; i < p->functions; ++i) {
        memcpy(ir->fn[i].name, p->fn[i].name, CB_NAME_SIZE);
        ir->fn[i].arity = p->fn[i].arity;
        ir->fn[i].locals = p->fn[i].locals > p->fn[i].arity ? p->fn[i].locals : p->fn[i].arity;
        ir->fn[i].body = import_expr(ir, p->fn[i].body);
        if (!ir->fn[i].body) {
            free(ir);
            if (error && error_size > 0) snprintf(error, error_size, "AST import failed");
            return 0;
        }
    }

    /* High-level optimization pipeline */
    for (int i = 0; i < ir->functions; ++i) simplify_func(ir, &ir->fn[i]);
    inline_pass(ir);
    trmc_pass(ir);
    inline_pass(ir);
    canonicalize_tail_loops_pass(ir);
    for (int i = 0; i < ir->functions; ++i) {
        ir->fn[i].body = cse_walk(ir, &ir->fn[i], ir->fn[i].body);
        simplify_func(ir, &ir->fn[i]);
        compact_locals(&ir->fn[i]);
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s: %s", path, strerror(errno));
        }
        free(ir);
        return 0;
    }

    fprintf(f, ".text\n"
               ".p2align 4\n"
               ".globl bench_entry\n"
               ".type bench_entry, @function\n"
               "bench_entry:\n"
               "  pushq %%rbp\n"
               "  pushq %%rbx\n"
               "  pushq %%r12\n"
               "  pushq %%r13\n"
               "  pushq %%r14\n"
               "  pushq %%r15\n"
               "  subq $8, %%rsp\n"
               "  call .Lfn%d\n"
               "  vzeroupper\n"
               "  addq $8, %%rsp\n"
               "  popq %%r15\n"
               "  popq %%r14\n"
               "  popq %%r13\n"
               "  popq %%r12\n"
               "  popq %%rbx\n"
               "  popq %%rbp\n"
               "  ret\n"
               ".size bench_entry, .-bench_entry\n",
            ir->entry);

    EmitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = f;
    ctx.prog = ir;

    for (int i = 0; i < ir->functions; ++i) {
        emit_function(&ctx, ir, i);
    }

    fprintf(f, ".section .note.GNU-stack,\"\",@progbits\n");

    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    if (!ok && error && error_size > 0) {
        snprintf(error, error_size, "could not write %s", path);
    }
    free(ir);
    return ok;
}