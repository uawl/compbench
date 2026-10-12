#include "ast.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CG_CAP 65536
#define CG_ARGS (CB_MAX_PARAMS + 1)

typedef struct CgNode CgNode;
struct CgNode {
    CbOp op;
    uint64_t v;
    int index, argc, calls, need;
    CgNode *a[CG_ARGS];
};

typedef struct {
    int valid, index, count, acc[CG_ARGS], uses_get;
    CbOp op[CG_ARGS];
    CgNode *map[CG_ARGS], *bound;
    CgNode **alias;
} CgLoop;

typedef struct {
    const CbProgram *p;
    FILE *f;
    CgNode *nodes;
    int used, failed, budget, cost[CB_MAX_FUNCTIONS];
    int fid, locals, arity, pcount, param[CG_ARGS], hidden;
    CbOp hidden_op;
    int *reg, *slot, *weight;
    int stacklocals, temps, saved, frame, frameless;
    int vslots, vmasked, vbudget;
    unsigned label, body_label;
    CgLoop loop;
} Cg;

/* rax, rcx, rdx and r11 are scratch; r14/r15 hold xs/n. */
static const char *const cg_regs[] = {
    "%rdi", "%rsi", "%r8", "%r9", "%r10", "%rbx", "%r12", "%r13"
};
static const char *const cg_in[] = {
    "%rdi", "%rsi", "%rdx", "%rcx"
};
static const char *const cg_cc[] = {
    "e", "ne", "b", "be", "a", "ae"
};
static const char *const cg_invcc[] = {
    "ne", "e", "ae", "a", "be", "b"
};

static int cg_max(int a, int b) {
    return a > b ? a : b;
}

static int cg_atom(const CgNode *e) {
    return e->op == CB_CONST || e->op == CB_VAR || e->op == CB_LEN;
}

static int cg_assoc(CbOp op) {
    return op == CB_ADD || op == CB_MUL || op == CB_AND ||
           op == CB_OR || op == CB_XOR;
}

static uint64_t cg_identity(CbOp op) {
    return op == CB_MUL ? UINT64_C(1) :
           op == CB_AND ? UINT64_MAX : UINT64_C(0);
}

static int cg_pow2(uint64_t x) {
    return x && !(x & (x - 1));
}

static unsigned cg_log2(uint64_t x) {
    unsigned n = 0;
    while (x >>= 1) ++n;
    return n;
}

static int cg_simm(uint64_t v) {
    return v <= UINT64_C(0x7fffffff) ||
           v >= UINT64_C(0xffffffff80000000);
}

static CgNode *cg_new(Cg *s, CbOp op) {
    CgNode *e;
    if (s->used == CG_CAP) {
        s->failed = 1;
        return &s->nodes[0];
    }
    e = &s->nodes[s->used++];
    e->op = op;
    return e;
}

static CgNode *cg_const(Cg *s, uint64_t v) {
    CgNode *e = cg_new(s, CB_CONST);
    e->v = v;
    return e;
}

static CgNode *cg_var(Cg *s, int index) {
    CgNode *e = cg_new(s, CB_VAR);
    e->index = index;
    return e;
}

static CgNode *cg_make(Cg *s, CbOp op, CgNode *a, CgNode *b) {
    CgNode *e = cg_new(s, op);
    e->a[0] = a;
    e->a[1] = b;
    e->calls = (a && a->calls) || (b && b->calls);
    return e;
}

static uint64_t cg_fold(CbOp op, uint64_t a, uint64_t b) {
    switch (op) {
    case CB_ADD: return a + b;
    case CB_SUB: return a - b;
    case CB_MUL: return a * b;
    case CB_DIV: return b ? a / b : 0;
    case CB_MOD: return b ? a % b : 0;
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
    default:    return 0;
    }
}

static CgNode *cg_binary(Cg *s, CbOp op, CgNode *a, CgNode *b) {
    uint64_t v;
    if (a->op == CB_CONST && b->op == CB_CONST)
        return cg_const(s, cg_fold(op, a->v, b->v));

    if ((cg_assoc(op) || op == CB_EQ || op == CB_NE) &&
        a->op == CB_CONST) {
        CgNode *t = a;
        a = b;
        b = t;
    }

    if (b->op == CB_CONST) {
        v = b->v;
        if (((op == CB_ADD || op == CB_SUB ||
              op == CB_OR || op == CB_XOR) && !v) ||
            ((op == CB_MUL || op == CB_DIV) && v == 1) ||
            (op == CB_AND && v == UINT64_MAX) ||
            ((op == CB_SHL || op == CB_SHR) && !(v & 63)))
            return a;

        if (!a->calls &&
            (((op == CB_MUL || op == CB_AND ||
               op == CB_DIV || op == CB_MOD) && !v) ||
             (op == CB_MOD && v == 1)))
            return cg_const(s, 0);

        if (op == CB_MUL && cg_pow2(v))
            return cg_make(s, CB_SHL, a, cg_const(s, cg_log2(v)));
        if (op == CB_DIV && cg_pow2(v))
            return cg_make(s, CB_SHR, a, cg_const(s, cg_log2(v)));
        if (op == CB_MOD && cg_pow2(v))
            return cg_make(s, CB_AND, a, cg_const(s, v - 1));
    }
    return cg_make(s, op, a, b);
}

static int cg_source_cost(const CbExpr *e, int limit) {
    int n = 1, k, i;
    if (!e || limit <= 0) return 1000;
    if (e->op == CB_IF) k = 3;
    else if (e->op == CB_LET || e->op >= CB_ADD) k = 2;
    else if (e->op == CB_GET) k = 1;
    else if (e->op == CB_CALL) k = e->argc;
    else k = 0;

    for (i = 0; i < k; ++i) {
        n += cg_source_cost(e->arg[i], limit - n);
        if (n > limit) break;
    }
    return n;
}

static CgNode *cg_clone(Cg *s, const CbExpr *e,
                        CgNode **env, uint64_t active) {
    CgNode *a, *b, *r;
    int i;
    if (!e || s->failed) return &s->nodes[0];

    switch (e->op) {
    case CB_CONST:
        return cg_const(s, e->value);
    case CB_VAR:
        return env[e->index];
    case CB_LEN:
        return cg_new(s, CB_LEN);
    case CB_GET:
        return cg_make(s, CB_GET,
                       cg_clone(s, e->arg[0], env, active), NULL);
    case CB_IF:
        a = cg_clone(s, e->arg[0], env, active);
        if (a->op == CB_CONST)
            return cg_clone(s, e->arg[a->v ? 1 : 2], env, active);
        b = cg_clone(s, e->arg[1], env, active);
        r = cg_make(s, CB_IF, a, b);
        r->a[2] = cg_clone(s, e->arg[2], env, active);
        r->calls |= r->a[2]->calls;
        return r;
    case CB_LET: {
        CgNode *old = env[e->index];
        int slot = -1;
        a = cg_clone(s, e->arg[0], env, active);
        if (cg_atom(a)) {
            env[e->index] = a;
        } else {
            slot = s->locals++;
            env[e->index] = cg_var(s, slot);
        }
        b = cg_clone(s, e->arg[1], env, active);
        env[e->index] = old;
        if (slot < 0) return b;
        r = cg_make(s, CB_LET, a, b);
        r->index = slot;
        return r;
    }
    case CB_CALL: {
        CgNode *args[CB_MAX_PARAMS];
        const CbFunction *fn = &s->p->fn[e->index];
        uint64_t bit = UINT64_C(1) << e->index;

        for (i = 0; i < e->argc; ++i)
            args[i] = cg_clone(s, e->arg[i], env, active);

        if (!(active & bit) && s->cost[e->index] <= 28 &&
            s->budget >= s->cost[e->index] + fn->arity + 1) {
            CgNode **sub = (CgNode **)calloc(
                (size_t)fn->locals + 1, sizeof(*sub));
            int bind[CB_MAX_PARAMS];

            if (!sub) {
                s->failed = 1;
                return &s->nodes[0];
            }
            s->budget -= s->cost[e->index] + fn->arity + 1;
            for (i = 0; i < fn->arity; ++i) {
                bind[i] = -1;
                if (cg_atom(args[i])) {
                    sub[i] = args[i];
                } else {
                    bind[i] = s->locals++;
                    sub[i] = cg_var(s, bind[i]);
                }
            }
            r = cg_clone(s, fn->body, sub, active | bit);
            free(sub);
            for (i = fn->arity - 1; i >= 0; --i) {
                if (bind[i] >= 0) {
                    r = cg_make(s, CB_LET, args[i], r);
                    r->index = bind[i];
                }
            }
            return r;
        }

        r = cg_new(s, CB_CALL);
        r->index = e->index;
        r->argc = e->argc;
        r->calls = 1;
        for (i = 0; i < e->argc; ++i) r->a[i] = args[i];
        return r;
    }
    default:
        if (e->op < CB_ADD || e->op > CB_GE) {
            s->failed = 1;
            return &s->nodes[0];
        }
        a = cg_clone(s, e->arg[0], env, active);
        b = cg_clone(s, e->arg[1], env, active);
        return cg_binary(s, e->op, a, b);
    }
}

/* Add an internal accumulator to a linear associative recurrence. */
static void cg_reduce_recursion(Cg *s, CgNode *root) {
    CgNode *e, *rec, *map, *v, *stop;
    int branch, j;
    if (root->op != CB_IF) return;

    for (branch = 1; branch <= 2; ++branch) {
        e = root->a[branch];
        while (e->op == CB_LET) e = e->a[1];
        if (!cg_assoc(e->op)) continue;
        rec = NULL;
        map = NULL;

        for (j = 0; j < 2; ++j) {
            if (e->a[j]->op == CB_CALL &&
                e->a[j]->index == s->fid &&
                !e->a[1-j]->calls) {
                rec = e->a[j];
                map = e->a[1-j];
                break;
            }
        }
        if (!rec) continue;

        s->hidden = s->locals++;
        s->hidden_op = e->op;
        s->param[s->pcount++] = s->hidden;
        v = cg_var(s, s->hidden);
        rec->a[rec->argc++] = cg_binary(s, e->op, v, map);
        stop = root->a[3-branch];
        root->a[3-branch] = cg_binary(s, e->op, v, stop);
        *e = *rec;
        return;
    }
}

static int cg_children(const CgNode *e) {
    if (e->op == CB_IF) return 3;
    if (e->op == CB_LET || e->op >= CB_ADD) return 2;
    if (e->op == CB_GET) return 1;
    if (e->op == CB_CALL) return e->argc;
    return 0;
}

static int cg_need(CgNode *e) {
    int n = 0, i;
    for (i = 0; i < cg_children(e); ++i) cg_need(e->a[i]);
    if (e->op == CB_CALL) {
        n = e->argc;
        for (i = 0; i < e->argc; ++i)
            n = cg_max(n, i + e->a[i]->need);
    } else if (e->op >= CB_ADD) {
        if (cg_atom(e->a[1])) n = e->a[0]->need;
        else if (cg_atom(e->a[0])) n = e->a[1]->need;
        else n = cg_max(e->a[0]->need, 1 + e->a[1]->need);
    } else {
        for (i = 0; i < cg_children(e); ++i)
            n = cg_max(n, e->a[i]->need);
    }
    e->need = n;
    return n;
}

static int cg_normal_calls(const CgNode *e, int tail) {
    int i;
    if (e->op == CB_CALL && !tail) return 1;
    for (i = 0; i < cg_children(e); ++i) {
        int t = tail &&
            ((e->op == CB_IF && i > 0) ||
             (e->op == CB_LET && i == 1));
        if (cg_normal_calls(e->a[i], t)) return 1;
    }
    return 0;
}

static void cg_weights(Cg *s, const CgNode *e) {
    int i;
    if (e->op == CB_VAR) {
        if (s->weight[e->index] < 1000000) ++s->weight[e->index];
        return;
    }
    for (i = 0; i < cg_children(e); ++i) {
        if (e->op == CB_CALL && e->index == s->fid &&
            i < s->pcount && e->a[i]->op == CB_VAR &&
            e->a[i]->index == s->param[i])
            continue;
        cg_weights(s, e->a[i]);
    }
}

static int cg_loff(const Cg *s, int i) {
    return -8 * (s->saved + 1 + s->slot[i]);
}

static int cg_toff(const Cg *s, int i) {
    return -8 * (s->saved + s->stacklocals + 1 + i);
}

static int cg_voff(const Cg *s, int i) {
    return -8 * (s->saved + s->stacklocals + s->temps) -
           32 * (i + 1);
}

static void cg_loc(const Cg *s, int i, char *buf, size_t n) {
    if (s->reg[i] >= 0)
        snprintf(buf, n, "%s", cg_regs[s->reg[i]]);
    else
        snprintf(buf, n, "%d(%%rbp)", cg_loff(s, i));
}

static void cg_imm(Cg *s, uint64_t v,
                   const char *r, const char *r32) {
    if (!v)
        fprintf(s->f, "  xorl %s, %s\n", r32, r32);
    else if (v <= UINT32_MAX)
        fprintf(s->f, "  movl $0x%08" PRIx64 ", %s\n", v, r32);
    else
        fprintf(s->f, "  movabsq $0x%016" PRIx64 ", %s\n", v, r);
}

static void cg_load(Cg *s, const CgNode *e,
                    const char *r, const char *r32) {
    char b[64];
    if (e->op == CB_CONST) {
        cg_imm(s, e->v, r, r32);
    } else if (e->op == CB_LEN) {
        fprintf(s->f, "  movq %%r15, %s\n", r);
    } else {
        cg_loc(s, e->index, b, sizeof(b));
        if (strcmp(b, r)) fprintf(s->f, "  movq %s, %s\n", b, r);
    }
}

static void cg_store(Cg *s, int i, const char *r) {
    char b[64];
    if (!s->weight[i]) return;
    cg_loc(s, i, b, sizeof(b));
    if (strcmp(b, r)) fprintf(s->f, "  movq %s, %s\n", r, b);
}

static void cg_value(Cg *, const CgNode *, int);

static void cg_pair(Cg *s, const CgNode *a,
                    const CgNode *b, int d) {
    if (cg_atom(b)) {
        cg_value(s, a, d);
        cg_load(s, b, "%rcx", "%ecx");
    } else if (cg_atom(a)) {
        cg_value(s, b, d);
        fprintf(s->f, "  movq %%rax, %%rcx\n");
        cg_load(s, a, "%rax", "%eax");
    } else {
        cg_value(s, a, d);
        fprintf(s->f, "  movq %%rax, %d(%%rbp)\n", cg_toff(s, d));
        cg_value(s, b, d + 1);
        fprintf(s->f,
                "  movq %%rax, %%rcx\n"
                "  movq %d(%%rbp), %%rax\n", cg_toff(s, d));
    }
}

static void cg_rhs(Cg *s, const CgNode *e, char *b, size_t n) {
    if (e->op == CB_CONST && cg_simm(e->v)) {
        int64_t v = e->v <= INT64_MAX
            ? (int64_t)e->v
            : -1 - (int64_t)(UINT64_MAX - e->v);
        snprintf(b, n, "$%" PRId64, v);
    } else if (e->op == CB_VAR) {
        cg_loc(s, e->index, b, n);
    } else if (e->op == CB_LEN) {
        snprintf(b, n, "%%r15");
    } else {
        cg_load(s, e, "%rcx", "%ecx");
        snprintf(b, n, "%%rcx");
    }
}

static void cg_compare(Cg *s, const CgNode *e, int d) {
    char b[64];
    if (cg_atom(e->a[1])) {
        cg_value(s, e->a[0], d);
        cg_rhs(s, e->a[1], b, sizeof(b));
        fprintf(s->f, "  cmpq %s, %%rax\n", b);
    } else {
        cg_pair(s, e->a[0], e->a[1], d);
        fprintf(s->f, "  cmpq %%rcx, %%rax\n");
    }
}

static void cg_branch(Cg *s, const CgNode *e, int d,
                      unsigned label, int truth) {
    if (e->op == CB_CONST) {
        if (!!e->v == !!truth)
            fprintf(s->f, "  jmp .Lcg%u\n", label);
    } else if (e->op >= CB_EQ && e->op <= CB_GE) {
        cg_compare(s, e, d);
        fprintf(s->f, "  j%s .Lcg%u\n",
                truth ? cg_cc[e->op-CB_EQ] : cg_invcc[e->op-CB_EQ],
                label);
    } else {
        cg_value(s, e, d);
        fprintf(s->f,
                "  testq %%rax, %%rax\n"
                "  j%s .Lcg%u\n", truth ? "nz" : "z", label);
    }
}

/* ceil(2^(64+k)/d)-2^64, where k=ceil(log2(d)). */
static uint64_t cg_reciprocal(uint64_t d, unsigned k) {
    uint64_t rem =
        (k == 64 ? UINT64_C(0) : UINT64_C(1) << k) - d;
    uint64_t q = 0;
    unsigned i;
    for (i = 0; i < 64; ++i) {
        uint64_t carry = rem >> 63;
        rem <<= 1;
        q <<= 1;
        if (carry || rem >= d) {
            rem -= d;
            q |= 1;
        }
    }
    return q + (rem != 0);
}

static void cg_divconst(Cg *s, CbOp op, uint64_t d) {
    unsigned k;
    if (!d) {
        fprintf(s->f, "  xorl %%eax, %%eax\n");
        return;
    }
    if (d == 1) {
        if (op == CB_MOD) fprintf(s->f, "  xorl %%eax, %%eax\n");
        return;
    }
    if (cg_pow2(d)) {
        if (op == CB_DIV) {
            fprintf(s->f, "  shrq $%u, %%rax\n", cg_log2(d));
        } else {
            cg_imm(s, d - 1, "%rcx", "%ecx");
            fprintf(s->f, "  andq %%rcx, %%rax\n");
        }
        return;
    }
    if (d > (UINT64_C(1) << 63)) {
        cg_imm(s, d, "%rcx", "%ecx");
        if (op == CB_DIV) {
            fprintf(s->f,
                    "  cmpq %%rcx, %%rax\n"
                    "  setae %%al\n"
                    "  movzbl %%al, %%eax\n");
        } else {
            fprintf(s->f,
                    "  movq %%rax, %%rdx\n"
                    "  subq %%rcx, %%rdx\n"
                    "  cmpq %%rcx, %%rax\n"
                    "  cmovaeq %%rdx, %%rax\n");
        }
        return;
    }

    k = cg_log2(d) + 1;
    fprintf(s->f, "  movq %%rax, %%r11\n");
    cg_imm(s, cg_reciprocal(d, k), "%rcx", "%ecx");
    fprintf(s->f,
            "  mulq %%rcx\n"
            "  movq %%r11, %%rax\n"
            "  subq %%rdx, %%rax\n"
            "  shrq $1, %%rax\n"
            "  addq %%rdx, %%rax\n");
    if (k > 1) fprintf(s->f, "  shrq $%u, %%rax\n", k - 1);
    if (op == CB_MOD) {
        cg_imm(s, d, "%rcx", "%ecx");
        fprintf(s->f,
                "  imulq %%rcx, %%rax\n"
                "  subq %%rax, %%r11\n"
                "  movq %%r11, %%rax\n");
    }
}

static void cg_arguments(Cg *s, const CgNode *e, int d) {
    int i;
    for (i = 0; i < e->argc; ++i) {
        cg_value(s, e->a[i], d + i);
        fprintf(s->f, "  movq %%rax, %d(%%rbp)\n",
                cg_toff(s, d + i));
    }
    for (i = 0; i < e->argc; ++i)
        fprintf(s->f, "  movq %d(%%rbp), %s\n",
                cg_toff(s, d + i), cg_in[i]);
}

static void cg_value(Cg *s, const CgNode *e, int d) {
    unsigned l, end;
    char b[64];
    const char *ins;

    if (cg_atom(e)) {
        cg_load(s, e, "%rax", "%eax");
        return;
    }

    switch (e->op) {
    case CB_GET:
        l = s->label++;
        end = s->label++;
        cg_value(s, e->a[0], d);
        fprintf(s->f,
                "  cmpq %%r15, %%rax\n"
                "  jae .Lcg%u\n"
                "  movq (%%r14,%%rax,8), %%rax\n"
                "  jmp .Lcg%u\n"
                ".Lcg%u:\n"
                "  xorl %%eax, %%eax\n"
                ".Lcg%u:\n", l, end, l, end);
        return;
    case CB_IF:
        l = s->label++;
        end = s->label++;
        cg_branch(s, e->a[0], d, l, 0);
        cg_value(s, e->a[1], d);
        fprintf(s->f, "  jmp .Lcg%u\n.Lcg%u:\n", end, l);
        cg_value(s, e->a[2], d);
        fprintf(s->f, ".Lcg%u:\n", end);
        return;
    case CB_LET:
        cg_value(s, e->a[0], d);
        cg_store(s, e->index, "%rax");
        cg_value(s, e->a[1], d);
        return;
    case CB_CALL:
        cg_arguments(s, e, d);
        fprintf(s->f, "  call .Lcbfn%d\n", e->index);
        return;
    default:
        break;
    }

    if (e->op >= CB_EQ) {
        cg_compare(s, e, d);
        fprintf(s->f,
                "  set%s %%al\n"
                "  movzbl %%al, %%eax\n", cg_cc[e->op-CB_EQ]);
        return;
    }

    if ((e->op == CB_DIV || e->op == CB_MOD) &&
        e->a[1]->op == CB_CONST) {
        cg_value(s, e->a[0], d);
        cg_divconst(s, e->op, e->a[1]->v);
        return;
    }

    if ((e->op == CB_SHL || e->op == CB_SHR) &&
        e->a[1]->op == CB_CONST) {
        cg_value(s, e->a[0], d);
        fprintf(s->f, "  %sq $%u, %%rax\n",
                e->op == CB_SHL ? "shl" : "shr",
                (unsigned)(e->a[1]->v & 63));
        return;
    }

    if (e->op == CB_ADD || e->op == CB_SUB || e->op == CB_MUL ||
        e->op == CB_AND || e->op == CB_OR || e->op == CB_XOR) {
        ins = e->op == CB_ADD ? "add" :
              e->op == CB_SUB ? "sub" :
              e->op == CB_MUL ? "imul" :
              e->op == CB_AND ? "and" :
              e->op == CB_OR ? "or" : "xor";
        if (cg_atom(e->a[1])) {
            cg_value(s, e->a[0], d);
            cg_rhs(s, e->a[1], b, sizeof(b));
        } else {
            cg_pair(s, e->a[0], e->a[1], d);
            snprintf(b, sizeof(b), "%%rcx");
        }
        fprintf(s->f, "  %sq %s, %%rax\n", ins, b);
        return;
    }

    cg_pair(s, e->a[0], e->a[1], d);
    if (e->op == CB_SHL || e->op == CB_SHR) {
        fprintf(s->f, "  %sq %%cl, %%rax\n",
                e->op == CB_SHL ? "shl" : "shr");
        return;
    }

    l = s->label++;
    end = s->label++;
    fprintf(s->f,
            "  testq %%rcx, %%rcx\n"
            "  jz .Lcg%u\n"
            "  xorl %%edx, %%edx\n"
            "  divq %%rcx\n", l);
    if (e->op == CB_MOD)
        fprintf(s->f, "  movq %%rdx, %%rax\n");
    fprintf(s->f,
            "  jmp .Lcg%u\n"
            ".Lcg%u:\n"
            "  xorl %%eax, %%eax\n"
            ".Lcg%u:\n", end, l, end);
}

static void cg_epilogue(Cg *s) {
    int i;
    if (s->frameless) return;
    if (!s->saved) {
        fprintf(s->f, "  leave\n");
        return;
    }
    fprintf(s->f, "  leaq -%d(%%rbp), %%rsp\n", 8 * s->saved);
    for (i = s->saved - 1; i >= 0; --i)
        fprintf(s->f, "  popq %s\n", cg_regs[5 + i]);
    fprintf(s->f, "  popq %%rbp\n");
}

static unsigned cg_param_refs(const Cg *s, const CgNode *e) {
    unsigned r = 0;
    int i;
    if (e->op == CB_VAR) {
        for (i = 0; i < s->pcount; ++i)
            if (e->index == s->param[i]) r |= 1u << i;
    } else {
        for (i = 0; i < cg_children(e); ++i)
            r |= cg_param_refs(s, e->a[i]);
    }
    return r;
}

static void cg_self_tail(Cg *s, const CgNode *e) {
    unsigned changed = 0, left, refs[CG_ARGS];
    int i, j, n = 0, order[CG_ARGS], pure = 1;

    for (i = 0; i < e->argc; ++i) {
        refs[i] = cg_param_refs(s, e->a[i]);
        if (e->a[i]->calls) pure = 0;
        if (e->a[i]->op != CB_VAR ||
            e->a[i]->index != s->param[i])
            changed |= 1u << i;
    }

    left = changed;
    if (pure) {
        while (left) {
            for (i = 0; i < e->argc; ++i) {
                if (!(left & (1u << i))) continue;
                for (j = 0; j < e->argc; ++j) {
                    if (j != i && (left & (1u << j)) &&
                        (refs[j] & (1u << i)))
                        break;
                }
                if (j == e->argc) break;
            }
            if (i == e->argc) break;
            order[n++] = i;
            left &= ~(1u << i);
        }
    }

    if (pure && !left) {
        for (j = 0; j < n; ++j) {
            i = order[j];
            cg_value(s, e->a[i], 0);
            cg_store(s, s->param[i], "%rax");
        }
    } else {
        for (i = 0; i < e->argc; ++i) {
            if (changed & (1u << i)) {
                cg_value(s, e->a[i], i);
                fprintf(s->f, "  movq %%rax, %d(%%rbp)\n",
                        cg_toff(s, i));
            }
        }
        for (i = 0; i < e->argc; ++i) {
            if ((changed & (1u << i)) &&
                s->weight[s->param[i]]) {
                fprintf(s->f, "  movq %d(%%rbp), %%rax\n",
                        cg_toff(s, i));
                cg_store(s, s->param[i], "%rax");
            }
        }
    }
    fprintf(s->f, "  jmp .Lcg%u\n", s->body_label);
}

static void cg_tail(Cg *s, const CgNode *e) {
    unsigned l;
    if (e->op == CB_IF) {
        l = s->label++;
        cg_branch(s, e->a[0], 0, l, 0);
        cg_tail(s, e->a[1]);
        fprintf(s->f, ".Lcg%u:\n", l);
        cg_tail(s, e->a[2]);
    } else if (e->op == CB_LET) {
        cg_value(s, e->a[0], 0);
        cg_store(s, e->index, "%rax");
        cg_tail(s, e->a[1]);
    } else if (e->op == CB_CALL && e->index == s->fid &&
               e->argc == s->pcount) {
        cg_self_tail(s, e);
    } else if (e->op == CB_CALL) {
        cg_arguments(s, e, 0);
        cg_epilogue(s);
        fprintf(s->f, "  jmp .Lcbfn%d\n", e->index);
    } else {
        cg_value(s, e, 0);
        cg_epilogue(s);
        fprintf(s->f, "  ret\n");
    }
}

static CgNode *cg_resolve(const CgLoop *l, CgNode *e) {
    while (e->op == CB_VAR && l->alias[e->index])
        e = l->alias[e->index];
    return e;
}

static int cg_isvar(const CgLoop *l, CgNode *e, int index) {
    e = cg_resolve(l, e);
    return e->op == CB_VAR && e->index == index;
}

static int cg_vcheck(Cg *s, CgNode *e, unsigned invariant,
                     unsigned char *bound) {
    CgLoop *l = &s->loop;
    int i, ok;
    if (--s->vbudget < 0) return 0;
    e = cg_resolve(l, e);

    switch (e->op) {
    case CB_CONST:
    case CB_LEN:
        return 1;
    case CB_VAR:
        if (e->index == l->index || bound[e->index]) return 1;
        for (i = 0; i < s->pcount; ++i) {
            if ((invariant & (1u << i)) &&
                e->index == s->param[i])
                return 1;
        }
        return 0;
    case CB_GET:
        e = cg_resolve(l, e->a[0]);
        if (cg_isvar(l, e, l->index)) {
            l->uses_get = 1;
            return 1;
        }
        if (e->op == CB_CONST || e->op == CB_LEN) return 1;
        if (e->op == CB_VAR) {
            for (i = 0; i < s->pcount; ++i) {
                if ((invariant & (1u << i)) &&
                    e->index == s->param[i])
                    return 1;
            }
        }
        return 0;
    case CB_LET:
        if (!cg_vcheck(s, e->a[0], invariant, bound)) return 0;
        bound[e->index] = 1;
        ok = cg_vcheck(s, e->a[1], invariant, bound);
        bound[e->index] = 0;
        return ok;
    case CB_CALL:
    case CB_DIV:
    case CB_MOD:
        return 0;
    default:
        for (i = 0; i < cg_children(e); ++i) {
            if (!cg_vcheck(s, e->a[i], invariant, bound))
                return 0;
        }
        return 1;
    }
}

static void cg_find_loop(Cg *s, CgNode *root) {
    CgLoop *l = &s->loop;
    CgNode *call, *cond, *a, *b, *x;
    int branch, ip, i, j, relation, valid;
    unsigned invariant;
    unsigned char *bound;

    if (root->op != CB_IF) return;
    l->alias = (CgNode **)calloc(
        (size_t)s->locals + 1, sizeof(*l->alias));
    bound = (unsigned char *)calloc((size_t)s->locals + 1, 1);
    if (!l->alias || !bound) {
        s->failed = 1;
        free(bound);
        return;
    }

    for (branch = 1; branch <= 2 && !l->valid; ++branch) {
        memset(l->alias, 0,
               ((size_t)s->locals + 1) * sizeof(*l->alias));
        call = root->a[branch];
        while (call->op == CB_LET && !call->a[0]->calls) {
            l->alias[call->index] = call->a[0];
            call = call->a[1];
        }
        if (call->op != CB_CALL || call->index != s->fid ||
            call->argc != s->pcount)
            continue;

        cond = root->a[0];
        if (cond->op < CB_EQ || cond->op > CB_GE) continue;

        for (ip = 0; ip < s->pcount && !l->valid; ++ip) {
            int index = s->param[ip];
            x = cg_resolve(l, call->a[ip]);
            if (x->op != CB_ADD) continue;
            a = cg_resolve(l, x->a[0]);
            b = cg_resolve(l, x->a[1]);
            if (a->op == CB_CONST) {
                x = a;
                a = b;
                b = x;
            }
            if (!cg_isvar(l, a, index) ||
                b->op != CB_CONST || b->v != 1)
                continue;

            a = cond->a[0];
            b = cond->a[1];
            relation = cond->op;
            if (cg_isvar(l, b, index)) {
                static const int swap[] = {
                    CB_EQ, CB_NE, CB_GT, CB_GE, CB_LT, CB_LE
                };
                x = a;
                a = b;
                b = x;
                relation = swap[relation-CB_EQ];
            }
            if (!cg_isvar(l, a, index)) continue;

            if (branch == 2) {
                static const int inv[] = {
                    CB_NE, CB_EQ, CB_GE, CB_GT, CB_LE, CB_LT
                };
                relation = inv[relation-CB_EQ];
            }
            if (relation != CB_LT && relation != CB_NE) continue;
            b = cg_resolve(l, b);
            if (!cg_atom(b)) continue;

            invariant = 0;
            valid = 1;
            l->count = 0;
            l->uses_get = 0;
            l->index = index;
            l->bound = b;

            for (i = 0; i < s->pcount; ++i) {
                if (i == ip) continue;
                x = cg_resolve(l, call->a[i]);
                if (cg_isvar(l, x, s->param[i])) {
                    invariant |= 1u << i;
                    continue;
                }
                if (!cg_assoc(x->op)) {
                    valid = 0;
                    break;
                }
                for (j = 0; j < 2; ++j)
                    if (cg_isvar(l, x->a[j], s->param[i])) break;
                if (j == 2) {
                    valid = 0;
                    break;
                }
                l->acc[l->count] = s->param[i];
                l->op[l->count] = x->op;
                l->map[l->count++] = x->a[1-j];
            }
            if (!valid || !l->count) continue;

            if (b->op == CB_VAR) {
                for (i = 0; i < s->pcount; ++i) {
                    if ((invariant & (1u << i)) &&
                        s->param[i] == b->index)
                        break;
                }
                if (i == s->pcount) continue;
            }

            s->vbudget = 2048;
            for (i = 0; i < l->count; ++i) {
                if (!cg_vcheck(s, l->map[i], invariant, bound))
                    break;
            }
            if (i == l->count) l->valid = 1;
        }
    }
    free(bound);
}

static int cg_vatom(Cg *s, CgNode *e) {
    e = cg_resolve(&s->loop, e);
    return cg_atom(e) || e->op == CB_GET;
}

static int cg_vneed(Cg *s, CgNode *e) {
    int a, b;
    e = cg_resolve(&s->loop, e);
    if (cg_vatom(s, e)) return 0;
    if (e->op == CB_LET)
        return cg_max(cg_vneed(s, e->a[0]),
                      cg_vneed(s, e->a[1]));
    if (e->op == CB_IF)
        return cg_max(
            1 + cg_vneed(s, e->a[0]),
            cg_max(2 + cg_vneed(s, e->a[1]),
                   3 + cg_vneed(s, e->a[2])));
    a = cg_vneed(s, e->a[0]);
    b = cg_vneed(s, e->a[1]);
    return cg_vatom(s, e->a[1]) ? a : cg_max(a, b + 1);
}

static void cg_vconstant(Cg *s, uint64_t v, int r) {
    if (!v) {
        fprintf(s->f, "  vpxor %%ymm%d, %%ymm%d, %%ymm%d\n",
                r, r, r);
    } else {
        cg_imm(s, v, "%rax", "%eax");
        fprintf(s->f,
                "  vmovq %%rax, %%xmm%d\n"
                "  vpbroadcastq %%xmm%d, %%ymm%d\n", r, r, r);
    }
}

static void cg_vload(Cg *s, CgNode *e, int r) {
    char b[64];
    int i, scalar = 0;
    unsigned l, end;

    e = cg_resolve(&s->loop, e);
    if (e->op == CB_CONST) {
        cg_vconstant(s, e->v, r);
        return;
    }
    if (e->op == CB_VAR && e->index == s->loop.index) {
        fprintf(s->f, "  vmovdqa %%ymm12, %%ymm%d\n", r);
        return;
    }
    if (e->op == CB_GET) {
        CgNode *idx = cg_resolve(&s->loop, e->a[0]);
        if (cg_isvar(&s->loop, idx, s->loop.index)) {
            cg_load(s, idx, "%rax", "%eax");
            if (s->vmasked) {
                fprintf(s->f,
                        "  vpmaskmovq (%%r14,%%rax,8), "
                        "%%ymm14, %%ymm%d\n", r);
            } else {
                fprintf(s->f,
                        "  vmovdqu (%%r14,%%rax,8), %%ymm%d\n", r);
            }
            return;
        }

        l = s->label++;
        end = s->label++;
        if (s->vmasked)
            fprintf(s->f,
                    "  vptest %%ymm14, %%ymm14\n"
                    "  jz .Lcg%u\n", l);
        cg_load(s, idx, "%rax", "%eax");
        fprintf(s->f,
                "  cmpq %%r15, %%rax\n"
                "  jae .Lcg%u\n"
                "  movq (%%r14,%%rax,8), %%rax\n"
                "  jmp .Lcg%u\n"
                ".Lcg%u:\n"
                "  xorl %%eax, %%eax\n"
                ".Lcg%u:\n"
                "  vmovq %%rax, %%xmm%d\n"
                "  vpbroadcastq %%xmm%d, %%ymm%d\n",
                l, end, l, end, r, r, r);
        return;
    }

    if (e->op == CB_LEN) {
        snprintf(b, sizeof(b), "%%r15");
        scalar = 1;
    } else {
        for (i = 0; i < s->pcount; ++i)
            if (e->index == s->param[i]) scalar = 1;
        if (scalar) cg_loc(s, e->index, b, sizeof(b));
    }

    if (!scalar) {
        fprintf(s->f, "  vmovdqu %d(%%rbp), %%ymm%d\n",
                cg_voff(s, e->index), r);
    } else if (b[0] == '%') {
        fprintf(s->f,
                "  vmovq %s, %%xmm%d\n"
                "  vpbroadcastq %%xmm%d, %%ymm%d\n",
                b, r, r, r);
    } else {
        fprintf(s->f, "  vpbroadcastq %s, %%ymm%d\n", b, r);
    }
}

static void cg_vmul(Cg *s, const char *w) {
    fprintf(s->f,
            "  vpmuludq %%%s1, %%%s0, %%%s2\n"
            "  vpsrlq $32, %%%s0, %%%s3\n"
            "  vpmuludq %%%s1, %%%s3, %%%s3\n"
            "  vpsrlq $32, %%%s1, %%%s4\n"
            "  vpmuludq %%%s0, %%%s4, %%%s4\n"
            "  vpaddq %%%s4, %%%s3, %%%s3\n"
            "  vpsllq $32, %%%s3, %%%s3\n"
            "  vpaddq %%%s3, %%%s2, %%%s0\n",
            w, w, w, w, w, w, w, w, w, w,
            w, w, w, w, w, w, w, w, w, w, w);
}

static const char *cg_vins(CbOp op) {
    return op == CB_ADD ? "vpaddq" :
           op == CB_SUB ? "vpsubq" :
           op == CB_AND ? "vpand" :
           op == CB_OR ? "vpor" : "vpxor";
}

static void cg_vector_value(Cg *s, CgNode *e, int d) {
    int off = s->locals + d;
    CbOp op;
    e = cg_resolve(&s->loop, e);
    op = e->op;

    if (cg_vatom(s, e)) {
        cg_vload(s, e, 0);
        return;
    }

    if (op == CB_LET) {
        cg_vector_value(s, e->a[0], d);
        fprintf(s->f, "  vmovdqu %%ymm0, %d(%%rbp)\n",
                cg_voff(s, e->index));
        cg_vector_value(s, e->a[1], d);
        return;
    }

    if (op == CB_IF) {
        int oldmask = s->vmasked;
        fprintf(s->f, "  vmovdqu %%ymm14, %d(%%rbp)\n",
                cg_voff(s, off));
        cg_vector_value(s, e->a[0], d + 1);
        fprintf(s->f,
                "  vpxor %%ymm1, %%ymm1, %%ymm1\n"
                "  vpcmpeqq %%ymm1, %%ymm0, %%ymm0\n"
                "  vmovdqu %%ymm0, %d(%%rbp)\n"
                "  vpandn %%ymm14, %%ymm0, %%ymm14\n",
                cg_voff(s, off + 1));

        s->vmasked = 1;
        cg_vector_value(s, e->a[1], d + 2);
        fprintf(s->f,
                "  vmovdqu %%ymm0, %d(%%rbp)\n"
                "  vmovdqu %d(%%rbp), %%ymm14\n"
                "  vpand %d(%%rbp), %%ymm14, %%ymm14\n",
                cg_voff(s, off + 2), cg_voff(s, off),
                cg_voff(s, off + 1));

        cg_vector_value(s, e->a[2], d + 3);
        fprintf(s->f,
                "  vmovdqu %d(%%rbp), %%ymm1\n"
                "  vmovdqu %d(%%rbp), %%ymm2\n"
                "  vpand %%ymm2, %%ymm0, %%ymm0\n"
                "  vpandn %%ymm1, %%ymm2, %%ymm1\n"
                "  vpor %%ymm1, %%ymm0, %%ymm0\n"
                "  vmovdqu %d(%%rbp), %%ymm14\n",
                cg_voff(s, off + 2), cg_voff(s, off + 1),
                cg_voff(s, off));
        s->vmasked = oldmask;
        return;
    }

    cg_vector_value(s, e->a[0], d);
    if ((op == CB_SHL || op == CB_SHR) &&
        cg_resolve(&s->loop, e->a[1])->op == CB_CONST) {
        fprintf(s->f, "  %s $%u, %%ymm0, %%ymm0\n",
                op == CB_SHL ? "vpsllq" : "vpsrlq",
                (unsigned)(cg_resolve(&s->loop, e->a[1])->v & 63));
        return;
    }

    if (cg_vatom(s, e->a[1])) {
        cg_vload(s, e->a[1], 1);
    } else {
        fprintf(s->f, "  vmovdqu %%ymm0, %d(%%rbp)\n",
                cg_voff(s, off));
        cg_vector_value(s, e->a[1], d + 1);
        fprintf(s->f,
                "  vmovdqa %%ymm0, %%ymm1\n"
                "  vmovdqu %d(%%rbp), %%ymm0\n", cg_voff(s, off));
    }

    if (op == CB_MUL) {
        cg_vmul(s, "ymm");
        return;
    }

    if (op == CB_SHL || op == CB_SHR) {
        cg_vconstant(s, 63, 2);
        fprintf(s->f,
                "  vpand %%ymm2, %%ymm1, %%ymm1\n"
                "  %s %%ymm1, %%ymm0, %%ymm0\n",
                op == CB_SHL ? "vpsllvq" : "vpsrlvq");
        return;
    }

    if (op >= CB_EQ) {
        if (op == CB_EQ || op == CB_NE) {
            fprintf(s->f,
                    "  vpcmpeqq %%ymm1, %%ymm0, %%ymm0\n");
        } else {
            cg_vconstant(s, UINT64_C(1) << 63, 2);
            fprintf(s->f,
                    "  vpxor %%ymm2, %%ymm0, %%ymm0\n"
                    "  vpxor %%ymm2, %%ymm1, %%ymm1\n");
            if (op == CB_LT || op == CB_GE)
                fprintf(s->f,
                        "  vpcmpgtq %%ymm0, %%ymm1, %%ymm0\n");
            else
                fprintf(s->f,
                        "  vpcmpgtq %%ymm1, %%ymm0, %%ymm0\n");
        }
        if (op == CB_NE || op == CB_LE || op == CB_GE) {
            fprintf(s->f,
                    "  vpcmpeqq %%ymm2, %%ymm2, %%ymm2\n"
                    "  vpxor %%ymm2, %%ymm0, %%ymm0\n");
        }
        fprintf(s->f, "  vpsrlq $63, %%ymm0, %%ymm0\n");
        return;
    }

    fprintf(s->f, "  %s %%ymm1, %%ymm0, %%ymm0\n", cg_vins(op));
}

static void cg_vector_loop(Cg *s) {
    CgLoop *l = &s->loop;
    unsigned skip = s->label++, loop = s->label++;
    int limit = cg_toff(s, s->temps - 1), i, r;
    char idx[64], acc[64];

    cg_loc(s, l->index, idx, sizeof(idx));
    cg_load(s, l->bound, "%rax", "%eax");
    if (l->uses_get) {
        fprintf(s->f,
                "  cmpq %%r15, %%rax\n"
                "  cmovaq %%r15, %%rax\n");
    }
    fprintf(s->f,
            "  cmpq %s, %%rax\n"
            "  jbe .Lcg%u\n"
            "  movq %%rax, %d(%%rbp)\n"
            "  subq %s, %%rax\n"
            "  cmpq $4, %%rax\n"
            "  jb .Lcg%u\n", idx, skip, limit, idx, skip);

    for (i = 0; i < l->count; ++i)
        cg_vconstant(s, cg_identity(l->op[i]), 8 + i);

    fprintf(s->f,
            "  movq %s, %%rax\n"
            "  vmovq %%rax, %%xmm12\n"
            "  vpbroadcastq %%xmm12, %%ymm12\n"
            "  vpaddq .Lcb_lanes(%%rip), %%ymm12, %%ymm12\n", idx);
    cg_vconstant(s, 4, 13);
    fprintf(s->f, "  vpcmpeqq %%ymm14, %%ymm14, %%ymm14\n");
    s->vmasked = 0;

    fprintf(s->f, "  .p2align 4\n.Lcg%u:\n", loop);
    for (i = 0; i < l->count; ++i) {
        cg_vector_value(s, l->map[i], 0);
        r = 8 + i;
        if (l->op[i] == CB_MUL) {
            fprintf(s->f, "  vmovdqa %%ymm%d, %%ymm1\n", r);
            cg_vmul(s, "ymm");
            fprintf(s->f, "  vmovdqa %%ymm0, %%ymm%d\n", r);
        } else {
            fprintf(s->f, "  %s %%ymm0, %%ymm%d, %%ymm%d\n",
                    cg_vins(l->op[i]), r, r);
        }
    }
    fprintf(s->f,
            "  addq $4, %s\n"
            "  vpaddq %%ymm13, %%ymm12, %%ymm12\n"
            "  movq %d(%%rbp), %%rax\n"
            "  subq %s, %%rax\n"
            "  cmpq $4, %%rax\n"
            "  jae .Lcg%u\n", idx, limit, idx, loop);

    for (i = 0; i < l->count; ++i) {
        r = 8 + i;
        fprintf(s->f, "  vextracti128 $1, %%ymm%d, %%xmm0\n", r);
        if (l->op[i] == CB_MUL) {
            fprintf(s->f, "  vmovdqa %%xmm%d, %%xmm1\n", r);
            cg_vmul(s, "xmm");
            fprintf(s->f, "  vpsrldq $8, %%xmm0, %%xmm1\n");
            cg_vmul(s, "xmm");
        } else {
            const char *ins = cg_vins(l->op[i]);
            fprintf(s->f,
                    "  %s %%xmm%d, %%xmm0, %%xmm0\n"
                    "  vpsrldq $8, %%xmm0, %%xmm1\n"
                    "  %s %%xmm1, %%xmm0, %%xmm0\n",
                    ins, r, ins);
        }
        fprintf(s->f, "  vmovq %%xmm0, %%rax\n");
        cg_loc(s, l->acc[i], acc, sizeof(acc));
        if (l->op[i] == CB_MUL) {
            fprintf(s->f, "  imulq %s, %%rax\n", acc);
            cg_store(s, l->acc[i], "%rax");
        } else {
            fprintf(s->f, "  %sq %%rax, %s\n",
                    l->op[i] == CB_ADD ? "add" :
                    l->op[i] == CB_AND ? "and" :
                    l->op[i] == CB_OR ? "or" : "xor", acc);
        }
    }
    fprintf(s->f, "  vzeroupper\n.Lcg%u:\n", skip);
}

static int cg_allocate(Cg *s, CgNode *root) {
    int i, j, best, leaf = !cg_normal_calls(root, 1);
    int taken[8] = {0};
    unsigned bytes;

    s->reg = (int *)malloc(((size_t)s->locals + 1) * sizeof(int));
    s->slot = (int *)malloc(((size_t)s->locals + 1) * sizeof(int));
    s->weight = (int *)calloc((size_t)s->locals + 1, sizeof(int));
    if (!s->reg || !s->slot || !s->weight) return 0;

    for (i = 0; i < s->locals; ++i)
        s->reg[i] = s->slot[i] = -1;
    cg_weights(s, root);

    if (s->loop.valid) {
        ++s->weight[s->loop.index];
        for (i = 0; i < s->loop.count; ++i)
            ++s->weight[s->loop.acc[i]];
    }

    if (leaf) {
        for (i = 0; i < s->arity && i < 2; ++i) {
            if (s->weight[i]) {
                s->reg[i] = i;
                taken[i] = 1;
            }
        }
    }

    for (j = leaf ? 0 : 5; j < 8; ++j) {
        if (taken[j]) continue;
        best = -1;
        for (i = 0; i < s->locals; ++i) {
            if (s->reg[i] < 0 && s->weight[i] &&
                (best < 0 || s->weight[i] > s->weight[best]))
                best = i;
        }
        if (best < 0) break;
        s->reg[best] = j;
        taken[j] = 1;
    }

    s->saved = 0;
    s->stacklocals = 0;
    for (j = 5; j < 8; ++j)
        if (taken[j]) s->saved = j - 4;
    for (i = 0; i < s->locals; ++i) {
        if (s->reg[i] < 0 && s->weight[i])
            s->slot[i] = s->stacklocals++;
    }

    s->temps = cg_need(root);
    s->vslots = 0;
    if (s->loop.valid) {
        int n = 0;
        for (i = 0; i < s->loop.count; ++i)
            n = cg_max(n, cg_vneed(s, s->loop.map[i]));
        s->vslots = s->locals + n;
        ++s->temps;
    }

    bytes = (unsigned)(s->saved + s->stacklocals + s->temps) * 8u +
            (unsigned)s->vslots * 32u;
    s->frame = (int)((bytes + 15u) & ~15u) - 8 * s->saved;
    s->frameless = leaf && !s->saved && !s->stacklocals &&
                   !s->temps && !s->vslots;
    return 1;
}

static void cg_cleanup(Cg *s) {
    free(s->nodes);
    free(s->reg);
    free(s->slot);
    free(s->weight);
    free(s->loop.alias);
    s->nodes = NULL;
    s->reg = s->slot = s->weight = NULL;
    s->loop.alias = NULL;
}

int cb_emit_assembly(const CbProgram *p, const char *path,
                     char *error, size_t error_size) {
    Cg s;
    int i, j, ok = 1;
    const char *why = "could not generate assembly";
    memset(&s, 0, sizeof(s));
    if (error && error_size) error[0] = 0;

    if (!p || !path || p->functions <= 0 ||
        p->functions > CB_MAX_FUNCTIONS ||
        p->entry < 0 || p->entry >= p->functions ||
        p->fn[p->entry].arity) {
        if (error && error_size)
            snprintf(error, error_size, "invalid program or output path");
        return 0;
    }

    s.p = p;
    s.f = fopen(path, "w");
    if (!s.f) {
        if (error && error_size)
            snprintf(error, error_size, "%s: %s", path, strerror(errno));
        return 0;
    }

    for (i = 0; i < p->functions; ++i)
        s.cost[i] = cg_source_cost(p->fn[i].body, 28);

    fprintf(s.f,
            ".text\n"
            ".globl bench_entry\n"
            ".type bench_entry,@function\n"
            "bench_entry:\n"
            "  pushq %%r14\n"
            "  pushq %%r15\n"
            "  subq $8, %%rsp\n"
            "  movq %%rdi, %%r14\n"
            "  movq %%rsi, %%r15\n"
            "  call .Lcbfn%d\n"
            "  addq $8, %%rsp\n"
            "  popq %%r15\n"
            "  popq %%r14\n"
            "  ret\n"
            ".size bench_entry,.-bench_entry\n", p->entry);

    for (i = 0; i < p->functions; ++i) {
        CgNode *root, *env[CB_MAX_LOCALS] = {0};
        const CbFunction *fn = &p->fn[i];

        if (fn->arity < 0 || fn->arity > CB_MAX_PARAMS ||
            fn->locals < fn->arity || fn->locals > CB_MAX_LOCALS) {
            ok = 0;
            why = "invalid function";
            break;
        }

        s.fid = i;
        s.locals = s.arity = s.pcount = fn->arity;
        s.hidden = -1;
        s.used = 1;
        s.failed = 0;
        s.budget = 8192;
        memset(&s.loop, 0, sizeof(s.loop));
        s.nodes = (CgNode *)calloc(CG_CAP, sizeof(*s.nodes));
        if (!s.nodes) {
            ok = 0;
            why = "out of memory";
            break;
        }

        for (j = 0; j < fn->arity; ++j) {
            s.param[j] = j;
            env[j] = cg_var(&s, j);
        }

        root = cg_clone(&s, fn->body, env, UINT64_C(1) << i);
        if (!s.failed) cg_reduce_recursion(&s, root);
        if (!s.failed) cg_find_loop(&s, root);
        if (s.failed || !cg_allocate(&s, root)) {
            ok = 0;
            why = "out of memory or expression too large";
            cg_cleanup(&s);
            break;
        }

        fprintf(s.f, ".p2align 4\n.Lcbfn%d:\n", i);
        if (!s.frameless) {
            fprintf(s.f,
                    "  pushq %%rbp\n"
                    "  movq %%rsp, %%rbp\n");
            for (j = 0; j < s.saved; ++j)
                fprintf(s.f, "  pushq %s\n", cg_regs[5 + j]);
            if (s.frame)
                fprintf(s.f, "  subq $%d, %%rsp\n", s.frame);
        }

        for (j = 0; j < fn->arity; ++j)
            cg_store(&s, j, cg_in[j]);

        if (s.hidden >= 0) {
            cg_imm(&s, cg_identity(s.hidden_op), "%rax", "%eax");
            cg_store(&s, s.hidden, "%rax");
        }

        s.body_label = s.label++;
        if (s.loop.valid) cg_vector_loop(&s);
        fprintf(s.f, ".Lcg%u:\n", s.body_label);
        cg_tail(&s, root);
        cg_cleanup(&s);

        if (ferror(s.f)) {
            ok = 0;
            why = "could not write assembly";
            break;
        }
    }

    fprintf(s.f,
            ".section .rodata\n"
            ".p2align 5\n"
            ".Lcb_lanes:\n"
            "  .quad 0,1,2,3\n"
            ".section .note.GNU-stack,\"\",@progbits\n");

    if (ferror(s.f)) {
        ok = 0;
        why = "could not write assembly";
    }
    if (fclose(s.f)) {
        ok = 0;
        why = "could not close assembly output";
    }
    if (!ok && error && error_size)
        snprintf(error, error_size, "%s: %s", path, why);
    return ok;
}