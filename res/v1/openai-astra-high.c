#include "ast.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned reads[CB_MAX_LOCALS];
    unsigned nodes;
    int leaf;
} CbgInfo;

typedef struct {
    int reg;
    int offset;
} CbgLocation;

typedef struct {
    const CbFunction *fn;
    CbgLocation loc[CB_MAX_LOCALS];
    unsigned char known[CB_MAX_LOCALS];
    unsigned char bounded[CB_MAX_LOCALS];
    uint64_t constant[CB_MAX_LOCALS];
} CbgEnv;

typedef struct {
    const CbProgram *program;
    CbgInfo *info;
    CbgEnv *env;
    char *text;
    size_t length, capacity;
    unsigned label;
    int failed;
    int function;
    int saved;
    int top, peak;
} CbgEmit;

static const char *const cbg_saved_regs[] = {
    "%rbx", "%r12", "%r13", "%r14", "%r15"
};

static const char *const cbg_arg_regs[] = {
    "%rdx", "%rcx", "%r8", "%r9"
};

static const char *const cbg_conditions[] = {
    "e", "ne", "b", "be", "a", "ae"
};

static const char *const cbg_inverse_conditions[] = {
    "ne", "e", "ae", "a", "be", "b"
};

static void cbg_value(CbgEmit *, const CbExpr *);
static void cbg_tail(CbgEmit *, const CbExpr *);

static void cbg_error(char *error, size_t size, const char *message)
{
    if (error && size)
        snprintf(error, size, "%s", message);
}

static void cbg_put(CbgEmit *s, const char *format, ...)
{
    va_list ap, copy;
    int count;
    size_t need, capacity;
    char *text;

    if (s->failed)
        return;

    va_start(ap, format);
    va_copy(copy, ap);
    count = vsnprintf(NULL, 0, format, copy);
    va_end(copy);

    if (count < 0 || (size_t)count > SIZE_MAX - s->length - 1) {
        s->failed = 1;
        va_end(ap);
        return;
    }

    need = s->length + (size_t)count + 1;
    if (need > s->capacity) {
        capacity = s->capacity ? s->capacity : 4096;
        while (capacity < need) {
            if (capacity > SIZE_MAX / 2) {
                capacity = need;
                break;
            }
            capacity *= 2;
        }
        text = (char *)realloc(s->text, capacity);
        if (!text) {
            s->failed = 1;
            va_end(ap);
            return;
        }
        s->text = text;
        s->capacity = capacity;
    }

    vsnprintf(s->text + s->length, s->capacity - s->length,
              format, ap);
    va_end(ap);
    s->length += (size_t)count;
}

static int cbg_scan(const CbProgram *p, const CbFunction *fn,
                    const CbExpr *e, CbgInfo *info, unsigned depth)
{
    int children, i;

    if (!e || depth > CB_MAX_NODES ||
        e->op < CB_CONST || e->op > CB_GE)
        return 0;

    if (info->nodes < 49)
        ++info->nodes;

    switch (e->op) {
    case CB_CONST:
    case CB_LEN:
        return 1;
    case CB_VAR:
        if (e->index < 0 || e->index >= fn->locals)
            return 0;
        if (info->reads[e->index] < 1000000u)
            ++info->reads[e->index];
        return 1;
    case CB_GET:
        children = 1;
        break;
    case CB_IF:
        children = 3;
        break;
    case CB_LET:
        if (e->index < fn->arity || e->index >= fn->locals)
            return 0;
        children = 2;
        break;
    case CB_CALL:
        if (e->index < 0 || e->index >= p->functions ||
            e->argc < 0 || e->argc > CB_MAX_PARAMS ||
            e->argc != p->fn[e->index].arity)
            return 0;
        info->leaf = 0;
        children = e->argc;
        break;
    default:
        children = 2;
        break;
    }

    for (i = 0; i < children; ++i)
        if (!cbg_scan(p, fn, e->arg[i], info, depth + 1))
            return 0;
    return 1;
}

static int cbg_constant(CbgEnv *env, const CbExpr *e, uint64_t *result)
{
    uint64_t a, b, old_value;
    unsigned char old_known;
    int ok;

    switch (e->op) {
    case CB_CONST:
        *result = e->value;
        return 1;
    case CB_VAR:
        if (!env->known[e->index])
            return 0;
        *result = env->constant[e->index];
        return 1;
    case CB_LEN:
    case CB_GET:
    case CB_CALL:
        return 0;
    case CB_IF:
        if (!cbg_constant(env, e->arg[0], &a))
            return 0;
        return cbg_constant(env, e->arg[a ? 1 : 2], result);
    case CB_LET:
        if (!cbg_constant(env, e->arg[0], &a))
            return 0;
        old_known = env->known[e->index];
        old_value = env->constant[e->index];
        env->known[e->index] = 1;
        env->constant[e->index] = a;
        ok = cbg_constant(env, e->arg[1], result);
        env->known[e->index] = old_known;
        env->constant[e->index] = old_value;
        return ok;
    default:
        break;
    }

    if (!cbg_constant(env, e->arg[0], &a) ||
        !cbg_constant(env, e->arg[1], &b))
        return 0;

    switch (e->op) {
    case CB_ADD: *result = a + b; break;
    case CB_SUB: *result = a - b; break;
    case CB_MUL: *result = a * b; break;
    case CB_DIV: *result = b ? a / b : 0; break;
    case CB_MOD: *result = b ? a % b : 0; break;
    case CB_AND: *result = a & b; break;
    case CB_OR:  *result = a | b; break;
    case CB_XOR: *result = a ^ b; break;
    case CB_SHL: *result = a << (b & 63); break;
    case CB_SHR: *result = a >> (b & 63); break;
    case CB_EQ:  *result = a == b; break;
    case CB_NE:  *result = a != b; break;
    case CB_LT:  *result = a < b; break;
    case CB_LE:  *result = a <= b; break;
    case CB_GT:  *result = a > b; break;
    case CB_GE:  *result = a >= b; break;
    default:
        return 0;
    }
    return 1;
}

static int cbg_slot(CbgEmit *s)
{
    int offset = -8 * (s->saved + s->top + 1);
    ++s->top;
    if (s->top > s->peak)
        s->peak = s->top;
    return offset;
}

static int cbg_signed_immediate(uint64_t v)
{
    return v <= UINT64_C(0x7fffffff) ||
           v >= UINT64_C(0xffffffff80000000);
}

static void cbg_load_constant(CbgEmit *s, uint64_t v,
                             int eleven, int preserve_flags)
{
    const char *r64 = eleven ? "%r11" : "%rax";
    const char *r32 = eleven ? "%r11d" : "%eax";

    if (!v && !preserve_flags)
        cbg_put(s, "  xorl %s, %s\n", r32, r32);
    else if (v <= UINT32_MAX)
        cbg_put(s, "  movl $0x%08" PRIx32 ", %s\n", (uint32_t)v, r32);
    else if (cbg_signed_immediate(v))
        cbg_put(s, "  movq $0x%016" PRIx64 ", %s\n", v, r64);
    else
        cbg_put(s, "  movabsq $0x%016" PRIx64 ", %s\n", v, r64);
}

static void cbg_location_operand(CbgEmit *s, int index,
                                 char *buffer, size_t size)
{
    CbgLocation loc = s->env->loc[index];
    if (loc.reg >= 0)
        snprintf(buffer, size, "%s", cbg_saved_regs[loc.reg]);
    else
        snprintf(buffer, size, "%d(%%rbp)", loc.offset);
}

static void cbg_load_local(CbgEmit *s, int index, const char *reg)
{
    char operand[40];
    cbg_location_operand(s, index, operand, sizeof operand);
    cbg_put(s, "  movq %s, %s\n", operand, reg);
}

static void cbg_store_local(CbgEmit *s, int index)
{
    CbgLocation loc = s->env->loc[index];
    if (loc.reg >= 0)
        cbg_put(s, "  movq %%rax, %s\n", cbg_saved_regs[loc.reg]);
    else if (loc.offset)
        cbg_put(s, "  movq %%rax, %d(%%rbp)\n", loc.offset);
}

static int cbg_atom(CbgEmit *s, const CbExpr *e)
{
    uint64_t v;
    return e->op == CB_VAR || e->op == CB_LEN ||
           cbg_constant(s->env, e, &v);
}

static void cbg_load_atom(CbgEmit *s, const CbExpr *e,
                         int eleven, int preserve_flags)
{
    uint64_t v;
    const char *reg = eleven ? "%r11" : "%rax";

    if (cbg_constant(s->env, e, &v))
        cbg_load_constant(s, v, eleven, preserve_flags);
    else if (e->op == CB_LEN)
        cbg_put(s, "  movq %%rsi, %s\n", reg);
    else
        cbg_load_local(s, e->index, reg);
}

static void cbg_immediate_operation(CbgEmit *s, const char *instruction,
                                    uint64_t v)
{
    if (cbg_signed_immediate(v))
        cbg_put(s, "  %s $0x%016" PRIx64 ", %%rax\n", instruction, v);
    else {
        cbg_load_constant(s, v, 1, 0);
        cbg_put(s, "  %s %%r11, %%rax\n", instruction);
    }
}

static unsigned cbg_log2(uint64_t n)
{
    unsigned result = 0;
    while (n >>= 1)
        ++result;
    return result;
}

/* Divide (hi:0) by d, where hi < d, using only C11 uint64_t arithmetic. */
static uint64_t cbg_divide_wide(uint64_t hi, uint64_t d, uint64_t *remainder)
{
    uint64_t q = 0, r = hi;
    unsigned i;

    for (i = 0; i < 64; ++i) {
        uint64_t carry = r >> 63;
        r <<= 1;
        q <<= 1;
        if (carry || r >= d) {
            r -= d;
            q |= 1;
        }
    }
    *remainder = r;
    return q;
}

static void cbg_divide_constant(CbgEmit *s, uint64_t d, int remainder)
{
    unsigned shift;
    uint64_t proposed, rem, twice;
    int add;

    if (!d) {
        cbg_load_constant(s, 0, 0, 0);
        return;
    }
    if (d == 1) {
        if (remainder)
            cbg_load_constant(s, 0, 0, 0);
        return;
    }

    shift = cbg_log2(d);
    if (!(d & (d - 1))) {
        if (remainder)
            cbg_immediate_operation(s, "andq", d - 1);
        else
            cbg_put(s, "  shrq $%u, %%rax\n", shift);
        return;
    }

    proposed = cbg_divide_wide(UINT64_C(1) << shift, d, &rem);
    add = d - rem >= (UINT64_C(1) << shift);
    if (add) {
        proposed += proposed;
        twice = rem + rem;
        if (twice < rem || twice >= d)
            ++proposed;
    }
    ++proposed;

    if (remainder)
        cbg_put(s, "  movq %%rax, %%rcx\n");
    if (add)
        cbg_put(s, "  movq %%rax, %%r10\n");

    cbg_load_constant(s, proposed, 1, 0);
    cbg_put(s, "  mulq %%r11\n");

    if (add) {
        cbg_put(s, "  subq %%rdx, %%r10\n"
                   "  shrq $1, %%r10\n"
                   "  leaq (%%rdx,%%r10), %%rax\n");
    } else {
        cbg_put(s, "  movq %%rdx, %%rax\n");
    }
    if (shift)
        cbg_put(s, "  shrq $%u, %%rax\n", shift);

    if (remainder) {
        cbg_load_constant(s, d, 1, 0);
        cbg_put(s, "  imulq %%r11, %%rax\n"
                   "  subq %%rax, %%rcx\n"
                   "  movq %%rcx, %%rax\n");
    }
}

static void cbg_finish_comparison(CbgEmit *s, CbOp op, int flags_only)
{
    if (!flags_only)
        cbg_put(s, "  set%s %%al\n  movzbl %%al, %%eax\n",
                cbg_conditions[op - CB_EQ]);
}

static void cbg_constant_operation(CbgEmit *s, CbOp op,
                                   uint64_t v, int flags_only)
{
    switch (op) {
    case CB_ADD:
        if (v)
            cbg_immediate_operation(s, "addq", v);
        break;
    case CB_SUB:
        if (v)
            cbg_immediate_operation(s, "subq", v);
        break;
    case CB_MUL:
        if (!v)
            cbg_load_constant(s, 0, 0, 0);
        else if (v == 1)
            return;
        else if (v == UINT64_MAX)
            cbg_put(s, "  negq %%rax\n");
        else if (!(v & (v - 1)))
            cbg_put(s, "  shlq $%u, %%rax\n", cbg_log2(v));
        else if (v == 3 || v == 5 || v == 9)
            cbg_put(s, "  leaq (%%rax,%%rax,%u), %%rax\n",
                    (unsigned)(v - 1));
        else
            cbg_immediate_operation(s, "imulq", v);
        break;
    case CB_DIV:
    case CB_MOD:
        cbg_divide_constant(s, v, op == CB_MOD);
        break;
    case CB_AND:
        if (!v)
            cbg_load_constant(s, 0, 0, 0);
        else if (v == UINT32_MAX)
            cbg_put(s, "  movl %%eax, %%eax\n");
        else if (v != UINT64_MAX)
            cbg_immediate_operation(s, "andq", v);
        break;
    case CB_OR:
        if (v == UINT64_MAX)
            cbg_load_constant(s, v, 0, 0);
        else if (v)
            cbg_immediate_operation(s, "orq", v);
        break;
    case CB_XOR:
        if (v == UINT64_MAX)
            cbg_put(s, "  notq %%rax\n");
        else if (v)
            cbg_immediate_operation(s, "xorq", v);
        break;
    case CB_SHL:
    case CB_SHR:
        if (v & 63)
            cbg_put(s, "  %s $%u, %%rax\n",
                    op == CB_SHL ? "shlq" : "shrq", (unsigned)(v & 63));
        break;
    default:
        if (!v)
            cbg_put(s, "  testq %%rax, %%rax\n");
        else
            cbg_immediate_operation(s, "cmpq", v);
        cbg_finish_comparison(s, op, flags_only);
        break;
    }
}

static void cbg_operand_operation(CbgEmit *s, CbOp op,
                                  const char *operand, int flags_only)
{
    const char *instruction = NULL;
    unsigned label;

    switch (op) {
    case CB_ADD: instruction = "addq"; break;
    case CB_SUB: instruction = "subq"; break;
    case CB_MUL: instruction = "imulq"; break;
    case CB_AND: instruction = "andq"; break;
    case CB_OR:  instruction = "orq"; break;
    case CB_XOR: instruction = "xorq"; break;
    default:
        break;
    }

    if (instruction) {
        cbg_put(s, "  %s %s, %%rax\n", instruction, operand);
        return;
    }

    if (op == CB_SHL || op == CB_SHR) {
        if (strcmp(operand, "%rcx"))
            cbg_put(s, "  movq %s, %%rcx\n", operand);
        cbg_put(s, "  %s %%cl, %%rax\n",
                op == CB_SHL ? "shlq" : "shrq");
        return;
    }

    if (op == CB_DIV || op == CB_MOD) {
        label = s->label++;
        if (strcmp(operand, "%rcx"))
            cbg_put(s, "  movq %s, %%rcx\n", operand);
        cbg_put(s, "  testq %%rcx, %%rcx\n"
                   "  jz .Lcb_zero%u\n"
                   "  xorl %%edx, %%edx\n"
                   "  divq %%rcx\n", label);
        if (op == CB_MOD)
            cbg_put(s, "  movq %%rdx, %%rax\n");
        cbg_put(s, "  jmp .Lcb_done%u\n"
                   ".Lcb_zero%u:\n"
                   "  xorl %%eax, %%eax\n"
                   ".Lcb_done%u:\n", label, label, label);
        return;
    }

    cbg_put(s, "  cmpq %s, %%rax\n", operand);
    cbg_finish_comparison(s, op, flags_only);
}

static int cbg_commutative(CbOp op)
{
    return op == CB_ADD || op == CB_MUL || op == CB_AND ||
           op == CB_OR || op == CB_XOR || op == CB_EQ || op == CB_NE;
}

static void cbg_binary(CbgEmit *s, const CbExpr *e, int flags_only)
{
    const CbExpr *left = e->arg[0], *right = e->arg[1], *swap;
    uint64_t v;
    char operand[40];
    int base, slot;

    if (cbg_commutative(e->op) &&
        cbg_constant(s->env, left, &v) &&
        !cbg_constant(s->env, right, &v)) {
        swap = left;
        left = right;
        right = swap;
    }

    if (cbg_constant(s->env, right, &v)) {
        cbg_value(s, left);
        cbg_constant_operation(s, e->op, v, flags_only);
        return;
    }

    if (right->op == CB_VAR || right->op == CB_LEN) {
        cbg_value(s, left);
        if (right->op == CB_LEN)
            snprintf(operand, sizeof operand, "%%rsi");
        else
            cbg_location_operand(s, right->index, operand, sizeof operand);
        cbg_operand_operation(s, e->op, operand, flags_only);
        return;
    }

    if (cbg_atom(s, left)) {
        cbg_value(s, right);
        cbg_put(s, "  movq %%rax, %%rcx\n");
        cbg_load_atom(s, left, 0, 0);
    } else {
        base = s->top;
        slot = cbg_slot(s);
        cbg_value(s, left);
        cbg_put(s, "  movq %%rax, %d(%%rbp)\n", slot);
        cbg_value(s, right);
        cbg_put(s, "  movq %%rax, %%rcx\n"
                   "  movq %d(%%rbp), %%rax\n", slot);
        s->top = base;
    }
    cbg_operand_operation(s, e->op, "%rcx", flags_only);
}

static int cbg_condition(CbgEmit *s, const CbExpr *e)
{
    if (e->op >= CB_EQ && e->op <= CB_GE) {
        cbg_binary(s, e, 1);
        return e->op - CB_EQ;
    }
    cbg_value(s, e);
    cbg_put(s, "  testq %%rax, %%rax\n");
    return CB_NE - CB_EQ;
}

/* Return a local whose index is provably below len on this branch. */
static int cbg_bound_fact(const CbExpr *e, int truth)
{
    if (e->op < CB_EQ || e->op > CB_GE)
        return -1;

    if (e->arg[0]->op == CB_VAR && e->arg[1]->op == CB_LEN) {
        if ((truth && e->op == CB_LT) || (!truth && e->op == CB_GE))
            return e->arg[0]->index;
    }
    if (e->arg[0]->op == CB_LEN && e->arg[1]->op == CB_VAR) {
        if ((truth && e->op == CB_GT) || (!truth && e->op == CB_LE))
            return e->arg[1]->index;
    }
    return -1;
}

static void cbg_branch_body(CbgEmit *s, const CbExpr *condition,
                            const CbExpr *body, int truth, int tail)
{
    int index = cbg_bound_fact(condition, truth);
    unsigned char old = 0;

    if (index >= 0) {
        old = s->env->bounded[index];
        s->env->bounded[index] = 1;
    }
    if (tail)
        cbg_tail(s, body);
    else
        cbg_value(s, body);
    if (index >= 0)
        s->env->bounded[index] = old;
}

static void cbg_let(CbgEmit *s, const CbExpr *e, int tail)
{
    CbgEnv *env = s->env;
    uint64_t v = 0, old_value = env->constant[e->index];
    unsigned char old_known = env->known[e->index];
    unsigned char old_bound = env->bounded[e->index];
    int known, bounded;

    known = cbg_constant(env, e->arg[0], &v);
    bounded = e->arg[0]->op == CB_VAR &&
              env->bounded[e->arg[0]->index];

    if (!known) {
        cbg_value(s, e->arg[0]);
        cbg_store_local(s, e->index);
    }

    env->known[e->index] = (unsigned char)known;
    env->constant[e->index] = v;
    env->bounded[e->index] = (unsigned char)bounded;

    if (tail)
        cbg_tail(s, e->arg[1]);
    else
        cbg_value(s, e->arg[1]);

    env->known[e->index] = old_known;
    env->constant[e->index] = old_value;
    env->bounded[e->index] = old_bound;
}

static int cbg_can_inline(CbgEmit *s, int index)
{
    return s->info[index].leaf && s->info[index].nodes <= 48;
}

static void cbg_inline(CbgEmit *s, const CbExpr *e)
{
    CbgEnv inner;
    CbgEnv *outer = s->env;
    const CbFunction *fn = &s->program->fn[e->index];
    int base = s->top, i;

    memset(&inner, 0, sizeof inner);
    inner.fn = fn;
    for (i = 0; i < fn->locals; ++i)
        inner.loc[i].reg = -1;

    for (i = 0; i < e->argc; ++i)
        inner.loc[i].offset = cbg_slot(s);

    for (i = 0; i < e->argc; ++i) {
        uint64_t v = 0;
        inner.known[i] =
            (unsigned char)cbg_constant(outer, e->arg[i], &v);
        inner.constant[i] = v;
        inner.bounded[i] = (unsigned char)
            (e->arg[i]->op == CB_VAR &&
             outer->bounded[e->arg[i]->index]);
        cbg_value(s, e->arg[i]);
        if (s->info[e->index].reads[i] && !inner.known[i])
            cbg_put(s, "  movq %%rax, %d(%%rbp)\n", inner.loc[i].offset);
    }

    for (i = fn->arity; i < fn->locals; ++i)
        if (s->info[e->index].reads[i])
            inner.loc[i].offset = cbg_slot(s);

    s->env = &inner;
    cbg_value(s, fn->body);
    s->env = outer;
    s->top = base;
}

static void cbg_arguments(CbgEmit *s, const CbExpr *e)
{
    int slots[CB_MAX_PARAMS];
    int base = s->top, i;

    if (!e->argc)
        return;

    for (i = 0; i + 1 < e->argc; ++i)
        slots[i] = cbg_slot(s);

    for (i = 0; i + 1 < e->argc; ++i) {
        cbg_value(s, e->arg[i]);
        cbg_put(s, "  movq %%rax, %d(%%rbp)\n", slots[i]);
    }

    cbg_value(s, e->arg[e->argc - 1]);
    cbg_put(s, "  movq %%rax, %s\n", cbg_arg_regs[e->argc - 1]);

    for (i = 0; i + 1 < e->argc; ++i)
        cbg_put(s, "  movq %d(%%rbp), %s\n", slots[i], cbg_arg_regs[i]);

    s->top = base;
}

static void cbg_value(CbgEmit *s, const CbExpr *e)
{
    uint64_t v, a, b;
    unsigned label;
    int condition;

    if (cbg_constant(s->env, e, &v)) {
        cbg_load_constant(s, v, 0, 0);
        return;
    }

    switch (e->op) {
    case CB_CONST:
        cbg_load_constant(s, e->value, 0, 0);
        break;
    case CB_VAR:
        cbg_load_local(s, e->index, "%rax");
        break;
    case CB_LEN:
        cbg_put(s, "  movq %%rsi, %%rax\n");
        break;
    case CB_GET:
        cbg_value(s, e->arg[0]);
        if (e->arg[0]->op == CB_VAR &&
            s->env->bounded[e->arg[0]->index]) {
            cbg_put(s, "  movq (%%rdi,%%rax,8), %%rax\n");
        } else {
            label = s->label++;
            cbg_put(s, "  cmpq %%rsi, %%rax\n"
                       "  jae .Lcb_zero%u\n"
                       "  movq (%%rdi,%%rax,8), %%rax\n"
                       "  jmp .Lcb_done%u\n"
                       ".Lcb_zero%u:\n"
                       "  xorl %%eax, %%eax\n"
                       ".Lcb_done%u:\n", label, label, label, label);
        }
        break;
    case CB_LET:
        cbg_let(s, e, 0);
        break;
    case CB_IF:
        if (cbg_constant(s->env, e->arg[0], &v)) {
            cbg_value(s, e->arg[v ? 1 : 2]);
            break;
        }

        if (cbg_atom(s, e->arg[1]) && cbg_atom(s, e->arg[2])) {
            condition = cbg_condition(s, e->arg[0]);
            if (cbg_constant(s->env, e->arg[1], &a) &&
                cbg_constant(s->env, e->arg[2], &b) &&
                ((a == 1 && b == 0) || (a == 0 && b == 1))) {
                cbg_put(s, "  set%s %%al\n  movzbl %%al, %%eax\n",
                        a ? cbg_conditions[condition]
                          : cbg_inverse_conditions[condition]);
            } else {
                cbg_load_atom(s, e->arg[2], 0, 1);
                cbg_load_atom(s, e->arg[1], 1, 1);
                cbg_put(s, "  cmov%s %%r11, %%rax\n",
                        cbg_conditions[condition]);
            }
            break;
        }

        label = s->label++;
        condition = cbg_condition(s, e->arg[0]);
        cbg_put(s, "  j%s .Lcb_else%u\n",
                cbg_inverse_conditions[condition], label);
        cbg_branch_body(s, e->arg[0], e->arg[1], 1, 0);
        cbg_put(s, "  jmp .Lcb_done%u\n.Lcb_else%u:\n", label, label);
        cbg_branch_body(s, e->arg[0], e->arg[2], 0, 0);
        cbg_put(s, ".Lcb_done%u:\n", label);
        break;
    case CB_CALL:
        if (cbg_can_inline(s, e->index))
            cbg_inline(s, e);
        else {
            cbg_arguments(s, e);
            cbg_put(s, "  call .Lcb_fn%d\n", e->index);
        }
        break;
    default:
        cbg_binary(s, e, 0);
        break;
    }
}

static void cbg_epilogue(CbgEmit *s)
{
    int i;

    if (!s->saved) {
        cbg_put(s, "  leave\n");
        return;
    }

    cbg_put(s, "  leaq %d(%%rbp), %%rsp\n", -8 * s->saved);
    for (i = s->saved - 1; i >= 0; --i)
        cbg_put(s, "  popq %s\n", cbg_saved_regs[i]);
    cbg_put(s, "  popq %%rbp\n");
}

static void cbg_self_tail(CbgEmit *s, const CbExpr *e)
{
    int indices[CB_MAX_PARAMS], slots[CB_MAX_PARAMS];
    int count = 0, base = s->top, i, index;

    for (i = 0; i < e->argc; ++i) {
        if (e->arg[i]->op == CB_VAR && e->arg[i]->index == i)
            continue;
        indices[count++] = i;
    }

    for (i = 0; i + 1 < count; ++i)
        slots[i] = cbg_slot(s);

    for (i = 0; i + 1 < count; ++i) {
        cbg_value(s, e->arg[indices[i]]);
        cbg_put(s, "  movq %%rax, %d(%%rbp)\n", slots[i]);
    }

    if (count) {
        index = indices[count - 1];
        cbg_value(s, e->arg[index]);
        cbg_store_local(s, index);
    }

    for (i = 0; i + 1 < count; ++i) {
        index = indices[i];
        if (s->env->loc[index].reg >= 0 || s->env->loc[index].offset) {
            cbg_put(s, "  movq %d(%%rbp), %%rax\n", slots[i]);
            cbg_store_local(s, index);
        }
    }

    s->top = base;
    cbg_put(s, "  jmp .Lcb_loop%d\n", s->function);
}

static void cbg_tail(CbgEmit *s, const CbExpr *e)
{
    uint64_t v;
    unsigned label;
    int condition;

    if (cbg_constant(s->env, e, &v)) {
        cbg_load_constant(s, v, 0, 0);
        cbg_epilogue(s);
        cbg_put(s, "  ret\n");
        return;
    }

    if (e->op == CB_LET) {
        cbg_let(s, e, 1);
        return;
    }

    if (e->op == CB_IF) {
        if (cbg_constant(s->env, e->arg[0], &v)) {
            cbg_tail(s, e->arg[v ? 1 : 2]);
            return;
        }

        if (cbg_atom(s, e->arg[1]) && cbg_atom(s, e->arg[2])) {
            cbg_value(s, e);
            cbg_epilogue(s);
            cbg_put(s, "  ret\n");
            return;
        }

        label = s->label++;
        condition = cbg_condition(s, e->arg[0]);
        cbg_put(s, "  j%s .Lcb_else%u\n",
                cbg_inverse_conditions[condition], label);
        cbg_branch_body(s, e->arg[0], e->arg[1], 1, 1);
        cbg_put(s, ".Lcb_else%u:\n", label);
        cbg_branch_body(s, e->arg[0], e->arg[2], 0, 1);
        return;
    }

    if (e->op == CB_CALL && e->index == s->function) {
        cbg_self_tail(s, e);
        return;
    }

    if (e->op == CB_CALL && !cbg_can_inline(s, e->index)) {
        cbg_arguments(s, e);
        cbg_epilogue(s);
        cbg_put(s, "  jmp .Lcb_fn%d\n", e->index);
        return;
    }

    cbg_value(s, e);
    cbg_epilogue(s);
    cbg_put(s, "  ret\n");
}

static void cbg_prepare_function(CbgEmit *s, CbgEnv *env, int index)
{
    const CbFunction *fn = &s->program->fn[index];
    CbgInfo *info = &s->info[index];
    int i, r, best;
    unsigned score;

    memset(env, 0, sizeof *env);
    env->fn = fn;
    s->env = env;
    s->function = index;
    s->saved = 0;
    s->top = 0;
    s->peak = 0;
    s->length = 0;

    for (i = 0; i < fn->locals; ++i)
        env->loc[i].reg = -1;

    for (r = 0; r < 5; ++r) {
        best = -1;
        score = 0;
        for (i = 0; i < fn->locals; ++i) {
            if (env->loc[i].reg < 0 && info->reads[i] > score) {
                score = info->reads[i];
                best = i;
            }
        }
        if (best < 0)
            break;
        env->loc[best].reg = r;
        ++s->saved;
    }

    for (i = 0; i < fn->locals; ++i)
        if (info->reads[i] && env->loc[i].reg < 0)
            env->loc[i].offset = cbg_slot(s);
}

int cb_emit_assembly(const CbProgram *p, const char *path,
                     char *error, size_t error_size)
{
    CbgEmit s;
    CbgEnv env;
    CbgInfo *info;
    FILE *out;
    int i, j, ok = 1;
    unsigned bytes;

    if (error && error_size)
        error[0] = '\0';

    if (!p || !path || p->functions <= 0 ||
        p->functions > CB_MAX_FUNCTIONS ||
        p->entry < 0 || p->entry >= p->functions) {
        cbg_error(error, error_size, "invalid program");
        return 0;
    }

    for (i = 0; i < p->functions; ++i) {
        if (p->fn[i].arity < 0 || p->fn[i].arity > CB_MAX_PARAMS ||
            p->fn[i].locals < p->fn[i].arity ||
            p->fn[i].locals > CB_MAX_LOCALS || !p->fn[i].body) {
            cbg_error(error, error_size, "invalid function");
            return 0;
        }
    }
    if (p->fn[p->entry].arity) {
        cbg_error(error, error_size, "entry function must have no arguments");
        return 0;
    }

    info = (CbgInfo *)calloc((size_t)p->functions, sizeof *info);
    if (!info) {
        cbg_error(error, error_size, "out of memory");
        return 0;
    }

    for (i = 0; i < p->functions; ++i) {
        info[i].leaf = 1;
        if (!cbg_scan(p, &p->fn[i], p->fn[i].body, &info[i], 0)) {
            free(info);
            cbg_error(error, error_size, "invalid expression");
            return 0;
        }
    }

    out = fopen(path, "w");
    if (!out) {
        if (error && error_size)
            snprintf(error, error_size, "%s: %s", path, strerror(errno));
        free(info);
        return 0;
    }

    memset(&s, 0, sizeof s);
    s.program = p;
    s.info = info;

    fprintf(out, ".text\n"
                 ".p2align 4\n"
                 ".globl bench_entry\n"
                 ".type bench_entry, @function\n"
                 "bench_entry:\n"
                 "  jmp .Lcb_fn%d\n"
                 ".size bench_entry, .-bench_entry\n", p->entry);

    for (i = 0; i < p->functions; ++i) {
        cbg_prepare_function(&s, &env, i);
        cbg_put(&s, ".p2align 4\n.Lcb_loop%d:\n", i);
        cbg_tail(&s, p->fn[i].body);

        if (s.failed) {
            ok = 0;
            break;
        }

        bytes = (unsigned)s.peak * 8u;
        if ((bytes + (unsigned)s.saved * 8u) & 15u)
            bytes += 8u;

        fprintf(out, ".p2align 4\n"
                     ".type .Lcb_fn%d, @function\n"
                     ".Lcb_fn%d:\n"
                     "  pushq %%rbp\n"
                     "  movq %%rsp, %%rbp\n", i, i);

        for (j = 0; j < s.saved; ++j)
            fprintf(out, "  pushq %s\n", cbg_saved_regs[j]);
        if (bytes)
            fprintf(out, "  subq $%u, %%rsp\n", bytes);

        for (j = 0; j < p->fn[i].arity; ++j) {
            if (env.loc[j].reg >= 0)
                fprintf(out, "  movq %s, %s\n", cbg_arg_regs[j],
                        cbg_saved_regs[env.loc[j].reg]);
            else if (env.loc[j].offset)
                fprintf(out, "  movq %s, %d(%%rbp)\n",
                        cbg_arg_regs[j], env.loc[j].offset);
        }

        if (fwrite(s.text, 1, s.length, out) != s.length) {
            ok = 0;
            break;
        }
        fprintf(out, ".size .Lcb_fn%d, .-.Lcb_fn%d\n", i, i);
    }

    if (ok)
        fprintf(out, ".section .note.GNU-stack,\"\",@progbits\n");

    if (ferror(out))
        ok = 0;
    if (fclose(out))
        ok = 0;

    if (!ok) {
        if (s.failed)
            cbg_error(error, error_size, "out of memory generating assembly");
        else if (error && error_size)
            snprintf(error, error_size, "could not write %s", path);
    }

    free(s.text);
    free(info);
    return ok;
}