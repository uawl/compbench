#include "ast.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(CbList) == 16 && offsetof(CbList, tail) == 8, "list ABI");
_Static_assert(offsetof(CbArena, used) == 8 && offsetof(CbArena, capacity) == 16 &&
               offsetof(CbArena, input) == 24 && offsetof(CbArena, failed) == 32, "arena ABI");

#define MAX_OPT_NODES 32768

typedef struct {
    CbFunction fn[CB_MAX_FUNCTIONS];
    int functions, entry, nodes;
    CbExpr pool[MAX_OPT_NODES];
} OptProgram;

static CbExpr *alloc_node(OptProgram *p, const CbExpr *src) {
    if (p->nodes >= MAX_OPT_NODES) return NULL;
    CbExpr *n = &p->pool[p->nodes++];
    if (src) *n = *src;
    else memset(n, 0, sizeof(*n));
    return n;
}

static CbExpr *clone_expr(OptProgram *p, const CbExpr *e, int var_offset) {
    if (!e) return NULL;
    CbExpr *n = alloc_node(p, e);
    if (!n) return NULL;
    if (n->op == CB_VAR || n->op == CB_LET) n->index += var_offset;
    int kids = 0;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: case CB_INPUT: case CB_NIL: kids = 0; break;
    case CB_GET: case CB_HEAD: case CB_TAIL: case CB_IS_NIL: kids = 1; break;
    case CB_IF: kids = 3; break;
    case CB_CALL: kids = e->argc; break;
    default: kids = 2; break;
    }
    for (int i = 0; i < kids; ++i) {
        n->arg[i] = clone_expr(p, e->arg[i], var_offset);
        if (e->arg[i] && !n->arg[i]) return NULL;
    }
    return n;
}

static bool is_pure(const CbExpr *e) {
    if (!e) return true;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: case CB_INPUT: case CB_NIL:
        return true;
    case CB_CONS: case CB_CALL:
        return false;
    case CB_GET: case CB_HEAD: case CB_TAIL: case CB_IS_NIL:
        return is_pure(e->arg[0]);
    case CB_IF:
        return is_pure(e->arg[0]) && is_pure(e->arg[1]) && is_pure(e->arg[2]);
    default:
        return is_pure(e->arg[0]) && is_pure(e->arg[1]);
    }
}

static int expr_size(const CbExpr *e) {
    if (!e) return 0;
    int s = 1;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: case CB_INPUT: case CB_NIL: break;
    case CB_GET: case CB_HEAD: case CB_TAIL: case CB_IS_NIL:
        s += expr_size(e->arg[0]); break;
    case CB_IF:
        s += expr_size(e->arg[0]) + expr_size(e->arg[1]) + expr_size(e->arg[2]); break;
    case CB_CALL:
        for (int i = 0; i < e->argc; ++i) s += expr_size(e->arg[i]);
        break;
    default:
        s += expr_size(e->arg[0]) + expr_size(e->arg[1]); break;
    }
    return s;
}

static bool has_any_call(const CbExpr *e) {
    if (!e) return false;
    if (e->op == CB_CALL) return true;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: case CB_INPUT: case CB_NIL: return false;
    case CB_GET: case CB_HEAD: case CB_TAIL: case CB_IS_NIL: return has_any_call(e->arg[0]);
    case CB_IF: return has_any_call(e->arg[0]) || has_any_call(e->arg[1]) || has_any_call(e->arg[2]);
    default: return has_any_call(e->arg[0]) || has_any_call(e->arg[1]);
    }
}

static bool has_call_to(const CbExpr *e, int fn_idx) {
    if (!e) return false;
    if (e->op == CB_CALL && e->index == fn_idx) return true;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: case CB_INPUT: case CB_NIL: return false;
    case CB_GET: case CB_HEAD: case CB_TAIL: case CB_IS_NIL: return has_call_to(e->arg[0], fn_idx);
    case CB_IF:
        return has_call_to(e->arg[0], fn_idx) || has_call_to(e->arg[1], fn_idx) || has_call_to(e->arg[2], fn_idx);
    case CB_CALL:
        for (int i = 0; i < e->argc; ++i)
            if (has_call_to(e->arg[i], fn_idx)) return true;
        return false;
    default:
        return has_call_to(e->arg[0], fn_idx) || has_call_to(e->arg[1], fn_idx);
    }
}

static CbExpr *fold_constants(CbExpr *e) {
    if (!e) return NULL;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: case CB_INPUT: case CB_NIL:
        return e;
    case CB_GET: case CB_HEAD: case CB_TAIL: case CB_IS_NIL:
        e->arg[0] = fold_constants(e->arg[0]);
        if (e->op == CB_IS_NIL && e->arg[0]->op == CB_NIL) {
            e->op = CB_CONST; e->value = 1;
        } else if (e->op == CB_HEAD && e->arg[0]->op == CB_NIL) {
            e->op = CB_CONST; e->value = 0;
        } else if (e->op == CB_TAIL && e->arg[0]->op == CB_NIL) {
            e->op = CB_NIL;
        }
        return e;
    case CB_IF:
        e->arg[0] = fold_constants(e->arg[0]);
        e->arg[1] = fold_constants(e->arg[1]);
        e->arg[2] = fold_constants(e->arg[2]);
        if (e->arg[0]->op == CB_CONST) {
            return e->arg[0]->value ? e->arg[1] : e->arg[2];
        }
        return e;
    case CB_LET:
        e->arg[0] = fold_constants(e->arg[0]);
        e->arg[1] = fold_constants(e->arg[1]);
        return e;
    case CB_CALL:
        for (int i = 0; i < e->argc; ++i) e->arg[i] = fold_constants(e->arg[i]);
        return e;
    case CB_CONS:
        e->arg[0] = fold_constants(e->arg[0]);
        e->arg[1] = fold_constants(e->arg[1]);
        return e;
    default: {
        e->arg[0] = fold_constants(e->arg[0]);
        e->arg[1] = fold_constants(e->arg[1]);
        CbExpr *a = e->arg[0], *b = e->arg[1];
        if (a->op == CB_CONST && b->op == CB_CONST) {
            uint64_t x = a->value, y = b->value, r = 0;
            switch (e->op) {
            case CB_ADD: r = x + y; break;
            case CB_SUB: r = x - y; break;
            case CB_MUL: r = x * y; break;
            case CB_DIV: r = y ? x / y : 0; break;
            case CB_MOD: r = y ? x % y : 0; break;
            case CB_AND: r = x & y; break;
            case CB_OR:  r = x | y; break;
            case CB_XOR: r = x ^ y; break;
            case CB_SHL: r = x << (y & 63); break;
            case CB_SHR: r = x >> (y & 63); break;
            case CB_EQ:  r = x == y; break;
            case CB_NE:  r = x != y; break;
            case CB_LT:  r = x < y; break;
            case CB_LE:  r = x <= y; break;
            case CB_GT:  r = x > y; break;
            case CB_GE:  r = x >= y; break;
            default: return e;
            }
            e->op = CB_CONST;
            e->value = r;
            return e;
        }
        if (e->op == CB_ADD) {
            if (b->op == CB_CONST && b->value == 0) return a;
            if (a->op == CB_CONST && a->value == 0) return b;
        } else if (e->op == CB_SUB) {
            if (b->op == CB_CONST && b->value == 0) return a;
        } else if (e->op == CB_MUL) {
            if (b->op == CB_CONST && b->value == 1) return a;
            if (a->op == CB_CONST && a->value == 1) return b;
            if (b->op == CB_CONST && b->value == 0 && is_pure(a)) { e->op = CB_CONST; e->value = 0; return e; }
            if (a->op == CB_CONST && a->value == 0 && is_pure(b)) { e->op = CB_CONST; e->value = 0; return e; }
        } else if (e->op == CB_SHL || e->op == CB_SHR) {
            if (b->op == CB_CONST && (b->value & 63) == 0) return a;
        } else if (e->op == CB_OR || e->op == CB_XOR) {
            if (b->op == CB_CONST && b->value == 0) return a;
            if (a->op == CB_CONST && a->value == 0) return b;
        }
        return e;
    }
    }
}

static CbExpr *inline_calls(OptProgram *p, CbFunction *caller, CbExpr *e, const bool *inlineable) {
    if (!e) return NULL;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: case CB_INPUT: case CB_NIL:
        return e;
    case CB_GET: case CB_HEAD: case CB_TAIL: case CB_IS_NIL:
        e->arg[0] = inline_calls(p, caller, e->arg[0], inlineable);
        return e;
    case CB_IF:
        e->arg[0] = inline_calls(p, caller, e->arg[0], inlineable);
        e->arg[1] = inline_calls(p, caller, e->arg[1], inlineable);
        e->arg[2] = inline_calls(p, caller, e->arg[2], inlineable);
        return e;
    case CB_CALL: {
        for (int i = 0; i < e->argc; ++i)
            e->arg[i] = inline_calls(p, caller, e->arg[i], inlineable);
        int target_idx = e->index;
        const CbFunction *callee = &p->fn[target_idx];
        if (inlineable[target_idx] &&
            caller->locals + callee->locals <= CB_MAX_LOCALS &&
            p->nodes + expr_size(callee->body) + callee->arity * 2 + 8 < MAX_OPT_NODES) {
            int base = caller->locals;
            for (int i = 0; i < callee->locals; ++i)
                caller->local_types[base + i] = callee->local_types[i];
            caller->locals += callee->locals;
            CbExpr *body_clone = clone_expr(p, callee->body, base);
            if (!body_clone) return e;
            CbExpr *cur = body_clone;
            for (int i = callee->arity - 1; i >= 0; --i) {
                CbExpr *let_node = alloc_node(p, NULL);
                if (!let_node) return e;
                let_node->op = CB_LET;
                let_node->type = callee->return_type;
                let_node->index = base + i;
                let_node->arg[0] = e->arg[i];
                let_node->arg[1] = cur;
                cur = let_node;
            }
            return cur;
        }
        return e;
    }
    default:
        e->arg[0] = inline_calls(p, caller, e->arg[0], inlineable);
        e->arg[1] = inline_calls(p, caller, e->arg[1], inlineable);
        return e;
    }
}

static int assign_temps(CbExpr *e, int cur) {
    if (!e) return cur;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: case CB_INPUT: case CB_NIL:
        break;
    case CB_GET: case CB_HEAD: case CB_TAIL: case CB_IS_NIL:
        cur = assign_temps(e->arg[0], cur);
        break;
    case CB_IF:
        cur = assign_temps(e->arg[0], cur);
        cur = assign_temps(e->arg[1], cur);
        cur = assign_temps(e->arg[2], cur);
        break;
    case CB_CALL:
        for (int i = 0; i < e->argc; ++i) cur = assign_temps(e->arg[i], cur);
        break;
    default:
        cur = assign_temps(e->arg[0], cur);
        cur = assign_temps(e->arg[1], cur);
        break;
    }
    e->temp = cur++;
    return cur;
}

/* Check if function fn_idx is eligible for Tail-Recursion Modulo Cons (TRMC). */
static bool check_trmc_tail(const CbExpr *e, int fn_idx, bool *found_trmc) {
    if (!e) return false;
    if (e->op == CB_IF) {
        if (has_call_to(e->arg[0], fn_idx)) return false;
        return check_trmc_tail(e->arg[1], fn_idx, found_trmc) &&
               check_trmc_tail(e->arg[2], fn_idx, found_trmc);
    }
    if (e->op == CB_LET) {
        if (has_call_to(e->arg[0], fn_idx)) return false;
        return check_trmc_tail(e->arg[1], fn_idx, found_trmc);
    }
    if (e->op == CB_CONS) {
        /* Support 1 or 2 chained conses ending in self-call, or start with 1 cons */
        if (has_call_to(e->arg[0], fn_idx)) return false;
        const CbExpr *tail_e = e->arg[1];
        if (tail_e->op == CB_CALL && tail_e->index == fn_idx) {
            for (int i = 0; i < tail_e->argc; ++i)
                if (has_call_to(tail_e->arg[i], fn_idx)) return false;
            *found_trmc = true;
            return true;
        }
    }
    if (e->op == CB_CALL && e->index == fn_idx) {
        for (int i = 0; i < e->argc; ++i)
            if (has_call_to(e->arg[i], fn_idx)) return false;
        return true;
    }
    return !has_call_to(e, fn_idx);
}

static bool is_trmc_function(const CbFunction *fn, int fn_idx) {
    if (fn->return_type != CB_LIST) return false;
    bool found = false;
    if (!check_trmc_tail(fn->body, fn_idx, &found)) return false;
    return found;
}

/* Check if function has any non-tail calls (after considering self-tail and TRMC). */
static bool has_non_self_tail_call(const CbExpr *e, int fn_idx, bool in_tail, bool is_trmc) {
    if (!e) return false;
    if (in_tail) {
        if (e->op == CB_IF) {
            return has_non_self_tail_call(e->arg[0], fn_idx, false, is_trmc) ||
                   has_non_self_tail_call(e->arg[1], fn_idx, true, is_trmc) ||
                   has_non_self_tail_call(e->arg[2], fn_idx, true, is_trmc);
        }
        if (e->op == CB_LET) {
            return has_non_self_tail_call(e->arg[0], fn_idx, false, is_trmc) ||
                   has_non_self_tail_call(e->arg[1], fn_idx, true, is_trmc);
        }
        if (e->op == CB_CALL && e->index == fn_idx) {
            for (int i = 0; i < e->argc; ++i)
                if (has_non_self_tail_call(e->arg[i], fn_idx, false, is_trmc)) return true;
            return false;
        }
        if (is_trmc && e->op == CB_CONS && e->arg[1]->op == CB_CALL && e->arg[1]->index == fn_idx) {
            if (has_non_self_tail_call(e->arg[0], fn_idx, false, is_trmc)) return true;
            const CbExpr *c = e->arg[1];
            for (int i = 0; i < c->argc; ++i)
                if (has_non_self_tail_call(c->arg[i], fn_idx, false, is_trmc)) return true;
            return false;
        }
    }
    if (e->op == CB_CALL) return true;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: case CB_INPUT: case CB_NIL: return false;
    case CB_GET: case CB_HEAD: case CB_TAIL: case CB_IS_NIL:
        return has_non_self_tail_call(e->arg[0], fn_idx, false, is_trmc);
    case CB_IF:
        return has_non_self_tail_call(e->arg[0], fn_idx, false, is_trmc) ||
               has_non_self_tail_call(e->arg[1], fn_idx, false, is_trmc) ||
               has_non_self_tail_call(e->arg[2], fn_idx, false, is_trmc);
    default:
        return has_non_self_tail_call(e->arg[0], fn_idx, false, is_trmc) ||
               has_non_self_tail_call(e->arg[1], fn_idx, false, is_trmc);
    }
}

static bool is_cmov_safe(const CbExpr *e, int depth) {
    if (!e || depth > 2) return false;
    switch (e->op) {
    case CB_CONST: case CB_VAR: case CB_LEN: case CB_NIL: case CB_INPUT:
        return true;
    case CB_ADD: case CB_SUB: case CB_MUL:
    case CB_AND: case CB_OR: case CB_XOR:
    case CB_SHL: case CB_SHR:
    case CB_EQ: case CB_NE: case CB_LT: case CB_LE: case CB_GT: case CB_GE:
        return is_cmov_safe(e->arg[0], depth + 1) && is_cmov_safe(e->arg[1], depth + 1);
    default:
        return false;
    }
}

static bool is_simple_operand(const CbExpr *e) {
    return e->op == CB_CONST || e->op == CB_VAR || e->op == CB_LEN || e->op == CB_NIL;
}

static bool fits_imm32(uint64_t v) {
    int64_t sv = (int64_t)v;
    return sv >= -2147483648LL && sv <= 2147483647LL;
}

typedef struct {
    FILE *out;
    const CbFunction *fn;
    int fn_idx;
    unsigned label;
    bool is_leaf;
    bool is_trmc;
    int reg_map[CB_MAX_LOCALS];
    int base_offset; /* bytes reserved at top of frame for callee-saved / TRMC slots */
} Emit;

static const char *const arg_regs[4] = {"%rdi", "%rsi", "%rdx", "%rcx"};
static const char *const var_regs[6] = {"%rbx", "%r15", "%r8", "%r9", "%r10", "%r11"};

static int local_slot(const Emit *s, int i) {
    return -s->base_offset - 8 * (1 + i);
}

static int temp_slot(const Emit *s, const CbExpr *e) {
    return -s->base_offset - 8 * (1 + s->fn->locals + e->temp);
}

static void emit_load_local(const Emit *s, int idx, const char *dst) {
    int r = (idx >= 0 && idx < CB_MAX_LOCALS) ? s->reg_map[idx] : -1;
    if (r >= 0) {
        if (strcmp(var_regs[r], dst) != 0)
            fprintf(s->out, "  movq %s, %s\n", var_regs[r], dst);
    } else {
        fprintf(s->out, "  movq %d(%%rbp), %s\n", local_slot(s, idx), dst);
    }
}

static void emit_store_local(const Emit *s, int idx, const char *src) {
    int r = (idx >= 0 && idx < CB_MAX_LOCALS) ? s->reg_map[idx] : -1;
    if (r >= 0) {
        if (strcmp(var_regs[r], src) != 0)
            fprintf(s->out, "  movq %s, %s\n", src, var_regs[r]);
    } else {
        fprintf(s->out, "  movq %s, %d(%%rbp)\n", src, local_slot(s, idx));
    }
}

static void emit_const(FILE *f, uint64_t v, const char *dst) {
    if (v == 0) {
        fprintf(f, "  xorq %s, %s\n", dst, dst);
    } else if (fits_imm32(v)) {
        fprintf(f, "  movq $%" PRId64 ", %s\n", (int64_t)v, dst);
    } else {
        fprintf(f, "  movabsq $0x%016" PRIx64 ", %s\n", v, dst);
    }
}

static void emit_simple_to_reg(const Emit *s, const CbExpr *e, const char *dst) {
    switch (e->op) {
    case CB_CONST: emit_const(s->out, e->value, dst); break;
    case CB_NIL:   fprintf(s->out, "  xorq %s, %s\n", dst, dst); break;
    case CB_LEN:   fprintf(s->out, "  movq %%r13, %s\n", dst); break;
    case CB_VAR:   emit_load_local(s, e->index, dst); break;
    default: break;
    }
}

static void emit_expr(Emit *s, const CbExpr *e);

/* Emit branch to false_label when cond is 0 (false). */
static void emit_branch_if_false(Emit *s, const CbExpr *cond, unsigned false_label) {
    FILE *f = s->out;
    if (cond->op >= CB_EQ && cond->op <= CB_GE) {
        static const char *const inv_cc[] = {"ne", "e", "ae", "a", "be", "b"};
        const CbExpr *a = cond->arg[0], *b = cond->arg[1];
        if (b->op == CB_CONST && fits_imm32(b->value)) {
            emit_expr(s, a);
            if (b->value == 0) fprintf(f, "  testq %%rax, %%rax\n");
            else fprintf(f, "  cmpq $%" PRId64 ", %%rax\n", (int64_t)b->value);
        } else if (is_simple_operand(b)) {
            emit_expr(s, a);
            emit_simple_to_reg(s, b, "%rcx");
            fprintf(f, "  cmpq %%rcx, %%rax\n");
        } else {
            emit_expr(s, a);
            fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_slot(s, a));
            emit_expr(s, b);
            fprintf(f, "  movq %%rax, %%rcx\n  movq %d(%%rbp), %%rax\n  cmpq %%rcx, %%rax\n",
                    temp_slot(s, a));
        }
        fprintf(f, "  j%s .L%u\n", inv_cc[cond->op - CB_EQ], false_label);
        return;
    }
    if (cond->op == CB_IS_NIL) {
        emit_expr(s, cond->arg[0]);
        fprintf(f, "  testq %%rax, %%rax\n  jnz .L%u\n", false_label);
        return;
    }
    emit_expr(s, cond);
    fprintf(f, "  testq %%rax, %%rax\n  jz .L%u\n", false_label);
}

static void emit_call_args(Emit *s, const CbExpr *e) {
    FILE *f = s->out;
    for (int i = 0; i < e->argc; ++i) {
        emit_expr(s, e->arg[i]);
        if (i + 1 < e->argc)
            fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_slot(s, e->arg[i]));
    }
    if (e->argc > 0) {
        if (strcmp(arg_regs[e->argc - 1], "%rax") != 0)
            fprintf(f, "  movq %%rax, %s\n", arg_regs[e->argc - 1]);
        for (int i = 0; i < e->argc - 1; ++i)
            fprintf(f, "  movq %d(%%rbp), %s\n", temp_slot(s, e->arg[i]), arg_regs[i]);
    }
}

static void emit_self_tail_update(Emit *s, const CbExpr *e) {
    FILE *f = s->out;
    for (int i = 0; i < e->argc; ++i) {
        emit_expr(s, e->arg[i]);
        if (i + 1 < e->argc)
            fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_slot(s, e->arg[i]));
    }
    if (e->argc > 0) {
        emit_store_local(s, e->argc - 1, "%rax");
        for (int i = 0; i < e->argc - 1; ++i) {
            fprintf(f, "  movq %d(%%rbp), %%rax\n", temp_slot(s, e->arg[i]));
            emit_store_local(s, i, "%rax");
        }
    }
}

static void emit_expr(Emit *s, const CbExpr *e) {
    FILE *f = s->out;
    unsigned label = s->label++;
    switch (e->op) {
    case CB_CONST:
        emit_const(f, e->value, "%rax");
        break;
    case CB_VAR:
        emit_load_local(s, e->index, "%rax");
        break;
    case CB_LEN:
        fprintf(f, "  movq %%r13, %%rax\n");
        break;
    case CB_INPUT:
        fprintf(f, "  movq 24(%%r14), %%rax\n");
        break;
    case CB_NIL:
        fprintf(f, "  xorq %%rax, %%rax\n");
        break;
    case CB_HEAD:
    case CB_TAIL:
    case CB_IS_NIL:
        emit_expr(s, e->arg[0]);
        fprintf(f, "  testq %%rax, %%rax\n");
        if (e->op == CB_IS_NIL) {
            fprintf(f, "  sete %%al\n  movzbq %%al, %%rax\n");
        } else {
            fprintf(f, "  jz .L%u\n  movq %d(%%rax), %%rax\n.L%u:\n",
                    label, e->op == CB_HEAD ? 0 : 8, label);
        }
        break;
    case CB_CONS:
        emit_expr(s, e->arg[0]);
        fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_slot(s, e->arg[0]));
        emit_expr(s, e->arg[1]);
        fprintf(f, "  movq %%rax, %%rdx\n"
                   "  movq 8(%%r14), %%rax\n"
                   "  cmpq 16(%%r14), %%rax\n  jae .Lfull%u\n"
                   "  leaq 1(%%rax), %%rcx\n  movq %%rcx, 8(%%r14)\n"
                   "  shlq $4, %%rax\n  addq (%%r14), %%rax\n"
                   "  movq %d(%%rbp), %%rcx\n  movq %%rcx, (%%rax)\n"
                   "  movq %%rdx, 8(%%rax)\n"
                   "  jmp .L%u\n.Lfull%u:\n  movq $1, 32(%%r14)\n"
                   "  xorq %%rax, %%rax\n.L%u:\n",
                label, temp_slot(s, e->arg[0]), label, label, label);
        break;
    case CB_GET:
        emit_expr(s, e->arg[0]);
        fprintf(f, "  cmpq %%r13, %%rax\n  jae .Lzero%u\n"
                   "  movq (%%r12,%%rax,8), %%rax\n"
                   "  jmp .L%u\n.Lzero%u:\n  xorq %%rax, %%rax\n.L%u:\n",
                label, label, label, label);
        break;
    case CB_IF: {
        const CbExpr *cond = e->arg[0], *t_br = e->arg[1], *f_br = e->arg[2];
        if (is_cmov_safe(t_br, 0) && is_cmov_safe(f_br, 0)) {
            emit_expr(s, t_br);
            fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_slot(s, t_br));
            emit_expr(s, f_br);
            fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_slot(s, f_br));
            if (cond->op >= CB_EQ && cond->op <= CB_GE) {
                static const char *const cc[] = {"e", "ne", "b", "be", "a", "ae"};
                const CbExpr *ca = cond->arg[0], *cb = cond->arg[1];
                if (is_simple_operand(cb)) {
                    emit_expr(s, ca);
                    emit_simple_to_reg(s, cb, "%rcx");
                    fprintf(f, "  cmpq %%rcx, %%rax\n");
                } else {
                    emit_expr(s, ca);
                    fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_slot(s, ca));
                    emit_expr(s, cb);
                    fprintf(f, "  movq %%rax, %%rcx\n  movq %d(%%rbp), %%rax\n  cmpq %%rcx, %%rax\n",
                            temp_slot(s, ca));
                }
                fprintf(f, "  movq %d(%%rbp), %%rax\n  movq %d(%%rbp), %%rcx\n  cmov%sq %%rcx, %%rax\n",
                        temp_slot(s, f_br), temp_slot(s, t_br), cc[cond->op - CB_EQ]);
            } else {
                emit_expr(s, cond);
                fprintf(f, "  testq %%rax, %%rax\n  movq %d(%%rbp), %%rax\n"
                           "  movq %d(%%rbp), %%rcx\n  cmovnzq %%rcx, %%rax\n",
                        temp_slot(s, f_br), temp_slot(s, t_br));
            }
            break;
        }
        unsigned else_lbl = s->label++;
        emit_branch_if_false(s, cond, else_lbl);
        emit_expr(s, t_br);
        fprintf(f, "  jmp .L%u\n.L%u:\n", label, else_lbl);
        emit_expr(s, f_br);
        fprintf(f, ".L%u:\n", label);
        break;
    }
    case CB_LET:
        emit_expr(s, e->arg[0]);
        emit_store_local(s, e->index, "%rax");
        emit_expr(s, e->arg[1]);
        break;
    case CB_CALL:
        emit_call_args(s, e);
        fprintf(f, "  call .Lfn%d\n", e->index);
        break;
    default: {
        const CbExpr *a = e->arg[0], *b = e->arg[1];
        /* Fast path for division/modulo by constant power of 2 */
        if ((e->op == CB_DIV || e->op == CB_MOD) && b->op == CB_CONST) {
            uint64_t d = b->value;
            emit_expr(s, a);
            if (d == 0) {
                fprintf(f, "  xorq %%rax, %%rax\n");
                break;
            }
            if ((d & (d - 1)) == 0) {
                if (e->op == CB_DIV) {
                    int sh = 0;
                    while ((1ULL << sh) != d) ++sh;
                    if (sh > 0) fprintf(f, "  shrq $%d, %%rax\n", sh);
                } else {
                    uint64_t mask = d - 1;
                    if (fits_imm32(mask)) {
                        fprintf(f, "  andq $%" PRId64 ", %%rax\n", (int64_t)mask);
                    } else {
                        fprintf(f, "  movabsq $0x%016" PRIx64 ", %%rcx\n  andq %%rcx, %%rax\n", mask);
                    }
                }
                break;
            }
        }
        /* Fast path for immediate RHS */
        if (b->op == CB_CONST && fits_imm32(b->value)) {
            int64_t imm = (int64_t)b->value;
            switch (e->op) {
            case CB_ADD:
                emit_expr(s, a);
                fprintf(f, "  addq $%" PRId64 ", %%rax\n", imm);
                return;
            case CB_SUB:
                emit_expr(s, a);
                fprintf(f, "  subq $%" PRId64 ", %%rax\n", imm);
                return;
            case CB_MUL:
                emit_expr(s, a);
                fprintf(f, "  imulq $%" PRId64 ", %%rax, %%rax\n", imm);
                return;
            case CB_AND:
                emit_expr(s, a);
                fprintf(f, "  andq $%" PRId64 ", %%rax\n", imm);
                return;
            case CB_OR:
                emit_expr(s, a);
                fprintf(f, "  orq $%" PRId64 ", %%rax\n", imm);
                return;
            case CB_XOR:
                emit_expr(s, a);
                fprintf(f, "  xorq $%" PRId64 ", %%rax\n", imm);
                return;
            case CB_SHL:
                emit_expr(s, a);
                fprintf(f, "  shlq $%u, %%rax\n", (unsigned)(b->value & 63));
                return;
            case CB_SHR:
                emit_expr(s, a);
                fprintf(f, "  shrq $%u, %%rax\n", (unsigned)(b->value & 63));
                return;
            case CB_EQ: case CB_NE: case CB_LT: case CB_LE: case CB_GT: case CB_GE: {
                static const char *const conds[] = {"e", "ne", "b", "be", "a", "ae"};
                emit_expr(s, a);
                if (imm == 0) fprintf(f, "  testq %%rax, %%rax\n");
                else fprintf(f, "  cmpq $%" PRId64 ", %%rax\n", imm);
                fprintf(f, "  set%s %%al\n  movzbq %%al, %%rax\n", conds[e->op - CB_EQ]);
                return;
            }
            default:
                break;
            }
        }
        if (is_simple_operand(b)) {
            emit_expr(s, a);
            emit_simple_to_reg(s, b, "%rcx");
        } else {
            emit_expr(s, a);
            fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_slot(s, a));
            emit_expr(s, b);
            fprintf(f, "  movq %%rax, %%rcx\n  movq %d(%%rbp), %%rax\n", temp_slot(s, a));
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
        case CB_MOD:
            fprintf(f, "  testq %%rcx, %%rcx\n  jz .Lzero%u\n"
                       "  xorq %%rdx, %%rdx\n  divq %%rcx\n", label);
            if (e->op == CB_MOD) fprintf(f, "  movq %%rdx, %%rax\n");
            fprintf(f, "  jmp .L%u\n.Lzero%u:\n  xorq %%rax, %%rax\n.L%u:\n",
                    label, label, label);
            break;
        default: {
            static const char *const conds[] = {"e", "ne", "b", "be", "a", "ae"};
            fprintf(f, "  cmpq %%rcx, %%rax\n  set%s %%al\n  movzbq %%al, %%rax\n",
                    conds[e->op - CB_EQ]);
            break;
        }
        }
        break;
    }
    }
}

static void emit_epilogue(const Emit *s) {
    FILE *f = s->out;
    if (s->reg_map[0] == 0) fprintf(f, "  movq -8(%%rbp), %%rbx\n");
    if (s->reg_map[1] == 1) fprintf(f, "  movq -16(%%rbp), %%r15\n");
    fprintf(f, "  leave\n  ret\n");
}

static void emit_tail(Emit *s, const CbExpr *e) {
    FILE *f = s->out;
    if (e->op == CB_IF) {
        const CbExpr *cond = e->arg[0], *t_br = e->arg[1], *f_br = e->arg[2];
        if (!s->is_trmc && is_cmov_safe(t_br, 0) && is_cmov_safe(f_br, 0)) {
            emit_expr(s, e);
            emit_epilogue(s);
            return;
        }
        unsigned else_lbl = s->label++;
        emit_branch_if_false(s, cond, else_lbl);
        emit_tail(s, t_br);
        fprintf(f, ".L%u:\n", else_lbl);
        emit_tail(s, f_br);
        return;
    }
    if (e->op == CB_LET) {
        emit_expr(s, e->arg[0]);
        emit_store_local(s, e->index, "%rax");
        emit_tail(s, e->arg[1]);
        return;
    }
    if (s->is_trmc && e->op == CB_CONS &&
        e->arg[1]->op == CB_CALL && e->arg[1]->index == s->fn_idx) {
        unsigned full_lbl = s->label++;
        const CbExpr *h = e->arg[0];
        const CbExpr *c = e->arg[1];
        emit_expr(s, h);
        fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_slot(s, h));
        emit_self_tail_update(s, c);
        fprintf(f, "  movq 8(%%r14), %%rax\n"
                   "  cmpq 16(%%r14), %%rax\n  jae .Lfull%u\n"
                   "  leaq 1(%%rax), %%rcx\n  movq %%rcx, 8(%%r14)\n"
                   "  shlq $4, %%rax\n  addq (%%r14), %%rax\n"
                   "  movq %d(%%rbp), %%rcx\n  movq %%rcx, (%%rax)\n"
                   "  movq $0, 8(%%rax)\n"
                   "  movq -32(%%rbp), %%rdx\n  movq %%rax, (%%rdx)\n"
                   "  leaq 8(%%rax), %%rdx\n  movq %%rdx, -32(%%rbp)\n"
                   "  jmp .Ltail%d\n"
                   ".Lfull%u:\n  movq $1, 32(%%r14)\n  xorq %%rax, %%rax\n",
                full_lbl, temp_slot(s, h), s->fn_idx, full_lbl);
        emit_epilogue(s);
        return;
    }
    if (e->op == CB_CALL) {
        if (e->index == s->fn_idx) {
            emit_self_tail_update(s, e);
            fprintf(f, "  jmp .Ltail%d\n", s->fn_idx);
            return;
        }
        if (!s->is_trmc) {
            emit_call_args(s, e);
            if (s->reg_map[0] == 0) fprintf(f, "  movq -8(%%rbp), %%rbx\n");
            if (s->reg_map[1] == 1) fprintf(f, "  movq -16(%%rbp), %%r15\n");
            fprintf(f, "  leave\n  jmp .Lfn%d\n", e->index);
            return;
        }
    }
    emit_expr(s, e);
    if (s->is_trmc) {
        fprintf(f, "  movq -32(%%rbp), %%rdx\n  movq %%rax, (%%rdx)\n"
                   "  movq -24(%%rbp), %%rax\n");
    }
    emit_epilogue(s);
}

int cb_emit_assembly(const CbProgram *p, const char *path,
                     char *error, size_t error_size) {
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(error, error_size, "%s: %s", path, strerror(errno));
        return 0;
    }

    OptProgram *opt = calloc(1, sizeof(OptProgram));
    if (!opt) {
        fclose(f);
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    opt->functions = p->functions;
    opt->entry = p->entry;
    for (int i = 0; i < p->functions; ++i) {
        opt->fn[i] = p->fn[i];
        opt->fn[i].body = clone_expr(opt, p->fn[i].body, 0);
        opt->fn[i].body = fold_constants(opt->fn[i].body);
    }

    for (int pass = 0; pass < 2; ++pass) {
        bool inlineable[CB_MAX_FUNCTIONS] = {false};
        for (int i = 0; i < opt->functions; ++i) {
            if (i != opt->entry && !has_any_call(opt->fn[i].body) &&
                expr_size(opt->fn[i].body) <= 24) {
                inlineable[i] = true;
            }
        }
        for (int i = 0; i < opt->functions; ++i) {
            opt->fn[i].body = inline_calls(opt, &opt->fn[i], opt->fn[i].body, inlineable);
            opt->fn[i].body = fold_constants(opt->fn[i].body);
        }
    }

    for (int i = 0; i < opt->functions; ++i) {
        opt->fn[i].temps = assign_temps(opt->fn[i].body, 0);
    }

    fprintf(f, ".text\n.p2align 4\n.globl bench_entry\n.type bench_entry, @function\n"
               "bench_entry:\n"
               "  pushq %%rbp\n  movq %%rsp, %%rbp\n"
               "  pushq %%r12\n  pushq %%r13\n  pushq %%r14\n  pushq %%r15\n"
               "  movq %%rdi, %%r12\n  movq %%rsi, %%r13\n  movq %%rdx, %%r14\n"
               "  call .Lfn%d\n"
               "  popq %%r15\n  popq %%r14\n  popq %%r13\n  popq %%r12\n"
               "  popq %%rbp\n  ret\n"
               ".size bench_entry, .-bench_entry\n",
            opt->entry);

    Emit s = {.out = f, .label = 1};
    for (int i = 0; i < opt->functions; ++i) {
        s.fn = &opt->fn[i];
        s.fn_idx = i;
        s.is_trmc = is_trmc_function(s.fn, i);
        s.is_leaf = !has_non_self_tail_call(s.fn->body, i, true, s.is_trmc);
        for (int j = 0; j < CB_MAX_LOCALS; ++j) s.reg_map[j] = -1;
        int max_regs = s.is_leaf ? 6 : 2;
        for (int j = 0; j < s.fn->locals && j < max_regs; ++j) {
            s.reg_map[j] = j;
        }
        s.base_offset = s.is_trmc ? 32 : 16;
        unsigned bytes = (unsigned)s.base_offset + (unsigned)(s.fn->locals + s.fn->temps) * 8;
        bytes = (bytes + 15u) & ~15u;

        fprintf(f, ".p2align 4\n.Lfn%d:\n  pushq %%rbp\n  movq %%rsp, %%rbp\n", i);
        if (bytes > 0) fprintf(f, "  subq $%u, %%rsp\n", bytes);
        if (s.reg_map[0] == 0) fprintf(f, "  movq %%rbx, -8(%%rbp)\n");
        if (s.reg_map[1] == 1) fprintf(f, "  movq %%r15, -16(%%rbp)\n");
        for (int j = 0; j < s.fn->arity; ++j) {
            emit_store_local(&s, j, arg_regs[j]);
        }
        if (s.is_trmc) {
            fprintf(f, "  movq $0, -24(%%rbp)\n"
                       "  leaq -24(%%rbp), %%rax\n"
                       "  movq %%rax, -32(%%rbp)\n");
        }
        fprintf(f, ".Ltail%d:\n", i);
        emit_tail(&s, s.fn->body);
    }

    fprintf(f, ".section .note.GNU-stack,\"\",@progbits\n");
    free(opt);
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    if (!ok) snprintf(error, error_size, "could not write %s", path);
    return ok;
}