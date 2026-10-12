#include "ast.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    FILE *out;
    const CbFunction *fn;
    unsigned label;
} Emit;

_Static_assert(sizeof(CbList) == 16 && offsetof(CbList, tail) == 8, "list ABI");
_Static_assert(offsetof(CbArena, used) == 8 && offsetof(CbArena, capacity) == 16 &&
               offsetof(CbArena, input) == 24 && offsetof(CbArena, failed) == 32, "arena ABI");

static int local_slot(int i) { return -8 * (4 + i); }
static int temp_slot(const Emit *s, const CbExpr *e) {
    return -8 * (4 + s->fn->locals + e->temp);
}
static void value(Emit *s, const CbExpr *e);

static void arguments(Emit *s, const CbExpr *e) {
    /* Private ABI: xs, n, arena in rdi/rsi/rdx; values in rcx/r8/r9/r10. */
    static const char *regs[] = {"%rcx", "%r8", "%r9", "%r10"};
    for (int i = 0; i < e->argc; ++i) value(s, e->arg[i]);
    fprintf(s->out, "  movq -8(%%rbp), %%rdi\n  movq -16(%%rbp), %%rsi\n"
                    "  movq -24(%%rbp), %%rdx\n");
    for (int i = 0; i < e->argc; ++i)
        fprintf(s->out, "  movq %d(%%rbp), %s\n", temp_slot(s, e->arg[i]), regs[i]);
}

static void value(Emit *s, const CbExpr *e) {
    FILE *f = s->out;
    unsigned label = s->label++;
    switch (e->op) {
    case CB_CONST: fprintf(f, "  movabsq $0x%016" PRIx64 ", %%rax\n", e->value); break;
    case CB_VAR: fprintf(f, "  movq %d(%%rbp), %%rax\n", local_slot(e->index)); break;
    case CB_LEN: fprintf(f, "  movq -16(%%rbp), %%rax\n"); break;
    case CB_INPUT:
        fprintf(f, "  movq -24(%%rbp), %%rax\n  movq 24(%%rax), %%rax\n"); break;
    case CB_NIL: fprintf(f, "  xorq %%rax, %%rax\n"); break;
    case CB_HEAD: case CB_TAIL: case CB_IS_NIL:
        value(s, e->arg[0]);
        fprintf(f, "  testq %%rax, %%rax\n");
        if (e->op == CB_IS_NIL) fprintf(f, "  sete %%al\n  movzbq %%al, %%rax\n");
        else fprintf(f, "  jz .Ldone%u\n  movq %d(%%rax), %%rax\n.Ldone%u:\n",
                     label, e->op == CB_HEAD ? 0 : 8, label);
        break;
    case CB_CONS:
        value(s, e->arg[0]); value(s, e->arg[1]);
        fprintf(f, "  movq -24(%%rbp), %%r11\n  movq 8(%%r11), %%rax\n"
                   "  cmpq 16(%%r11), %%rax\n  jae .Lfull%u\n"
                   "  leaq 1(%%rax), %%rcx\n  movq %%rcx, 8(%%r11)\n"
                   "  shlq $4, %%rax\n  addq (%%r11), %%rax\n"
                   "  movq %d(%%rbp), %%rcx\n  movq %%rcx, (%%rax)\n"
                   "  movq %d(%%rbp), %%rcx\n  movq %%rcx, 8(%%rax)\n"
                   "  jmp .Ldone%u\n.Lfull%u:\n  movq $1, 32(%%r11)\n"
                   "  xorq %%rax, %%rax\n.Ldone%u:\n",
                label, temp_slot(s, e->arg[0]), temp_slot(s, e->arg[1]), label, label, label);
        break;
    case CB_GET:
        value(s, e->arg[0]);
        fprintf(f, "  cmpq -16(%%rbp), %%rax\n  jae .Lzero%u\n"
                   "  movq -8(%%rbp), %%rcx\n  movq (%%rcx,%%rax,8), %%rax\n"
                   "  jmp .Ldone%u\n.Lzero%u:\n  xorq %%rax, %%rax\n.Ldone%u:\n",
                label, label, label, label);
        break;
    case CB_IF:
        value(s, e->arg[0]);
        fprintf(f, "  testq %%rax, %%rax\n  jz .Lelse%u\n", label);
        value(s, e->arg[1]);
        fprintf(f, "  jmp .Ldone%u\n.Lelse%u:\n", label, label);
        value(s, e->arg[2]);
        fprintf(f, ".Ldone%u:\n", label);
        break;
    case CB_LET:
        value(s, e->arg[0]);
        fprintf(f, "  movq %%rax, %d(%%rbp)\n", local_slot(e->index));
        value(s, e->arg[1]);
        break;
    case CB_CALL:
        arguments(s, e);
        fprintf(f, "  call .Lfn%d\n", e->index);
        break;
    default:
        value(s, e->arg[0]); value(s, e->arg[1]);
        fprintf(f, "  movq %d(%%rbp), %%rax\n  movq %d(%%rbp), %%rcx\n",
                temp_slot(s, e->arg[0]), temp_slot(s, e->arg[1]));
        switch (e->op) {
        case CB_ADD: fprintf(f, "  addq %%rcx, %%rax\n"); break;
        case CB_SUB: fprintf(f, "  subq %%rcx, %%rax\n"); break;
        case CB_MUL: fprintf(f, "  imulq %%rcx, %%rax\n"); break;
        case CB_AND: fprintf(f, "  andq %%rcx, %%rax\n"); break;
        case CB_OR: fprintf(f, "  orq %%rcx, %%rax\n"); break;
        case CB_XOR: fprintf(f, "  xorq %%rcx, %%rax\n"); break;
        case CB_SHL: fprintf(f, "  shlq %%cl, %%rax\n"); break;
        case CB_SHR: fprintf(f, "  shrq %%cl, %%rax\n"); break;
        case CB_DIV: case CB_MOD:
            fprintf(f, "  testq %%rcx, %%rcx\n  jz .Lzero%u\n"
                       "  xorq %%rdx, %%rdx\n  divq %%rcx\n", label);
            if (e->op == CB_MOD) fprintf(f, "  movq %%rdx, %%rax\n");
            fprintf(f, "  jmp .Ldone%u\n.Lzero%u:\n  xorq %%rax, %%rax\n.Ldone%u:\n",
                    label, label, label);
            break;
        default: {
            static const char *conds[] = {"e", "ne", "b", "be", "a", "ae"};
            fprintf(f, "  cmpq %%rcx, %%rax\n  set%s %%al\n  movzbq %%al, %%rax\n",
                    conds[e->op - CB_EQ]);
            break;
        }
        }
    }
    fprintf(f, "  movq %%rax, %d(%%rbp)\n", temp_slot(s, e));
}

static void tail(Emit *s, const CbExpr *e) {
    if (e->op == CB_IF) {
        unsigned label = s->label++;
        value(s, e->arg[0]);
        fprintf(s->out, "  testq %%rax, %%rax\n  jz .Lelse%u\n", label);
        tail(s, e->arg[1]);
        fprintf(s->out, ".Lelse%u:\n", label);
        tail(s, e->arg[2]);
    } else if (e->op == CB_LET) {
        value(s, e->arg[0]);
        fprintf(s->out, "  movq %%rax, %d(%%rbp)\n", local_slot(e->index));
        tail(s, e->arg[1]);
    } else if (e->op == CB_CALL) {
        arguments(s, e);
        fprintf(s->out, "  leave\n  jmp .Lfn%d\n", e->index);
    } else {
        value(s, e);
        fprintf(s->out, "  leave\n  ret\n");
    }
}

int cb_emit_assembly(const CbProgram *p, const char *path, char *error, size_t error_size) {
    FILE *f = fopen(path, "w");
    if (!f) { snprintf(error, error_size, "%s: %s", path, strerror(errno)); return 0; }
    Emit s = {.out = f};
    fprintf(f, ".text\n.globl bench_entry\n.type bench_entry, @function\n"
               "bench_entry:\n  jmp .Lfn%d\n.size bench_entry, .-bench_entry\n", p->entry);
    static const char *regs[] = {"%rcx", "%r8", "%r9", "%r10"};
    for (int i = 0; i < p->functions; ++i) {
        s.fn = &p->fn[i];
        unsigned bytes = (unsigned)(3 + s.fn->locals + s.fn->temps) * 8;
        bytes = (bytes + 15) & ~15u;
        fprintf(f, ".p2align 4\n.Lfn%d:\n  pushq %%rbp\n  movq %%rsp, %%rbp\n"
                   "  subq $%u, %%rsp\n  movq %%rdi, -8(%%rbp)\n"
                   "  movq %%rsi, -16(%%rbp)\n  movq %%rdx, -24(%%rbp)\n", i, bytes);
        for (int j = 0; j < s.fn->arity; ++j)
            fprintf(f, "  movq %s, %d(%%rbp)\n", regs[j], local_slot(j));
        tail(&s, s.fn->body);
    }
    fprintf(f, ".section .note.GNU-stack,\"\",@progbits\n");
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    if (!ok) snprintf(error, error_size, "could not write %s", path);
    return ok;
}
