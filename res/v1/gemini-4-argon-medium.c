#include "ast.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  FILE *out;
  const CbProgram *prog;
  const CbFunction *fn;
  int fn_idx;
  unsigned label;
  int max_local;
  bool has_non_self_call;
  bool has_non_tail_call;
} Emit;

#define REG_LOCAL_MAX 4

static const char *local_regs[REG_LOCAL_MAX] = {"%r12", "%r13", "%r14", "%r15"};
static const char *arg_regs[CB_MAX_PARAMS] = {"%rdx", "%rcx", "%r8", "%r9"};

static void scan_expr(Emit *s, const CbExpr *e, bool is_tail) {
  if (!e) return;
  if (e->op == CB_VAR || e->op == CB_LET) {
    if (e->index > s->max_local) s->max_local = e->index;
  }
  switch (e->op) {
  case CB_CONST:
  case CB_VAR:
  case CB_LEN:
    break;
  case CB_GET:
    scan_expr(s, e->arg[0], false);
    break;
  case CB_IF:
    scan_expr(s, e->arg[0], false);
    scan_expr(s, e->arg[1], is_tail);
    scan_expr(s, e->arg[2], is_tail);
    break;
  case CB_LET:
    scan_expr(s, e->arg[0], false);
    scan_expr(s, e->arg[1], is_tail);
    break;
  case CB_CALL:
    if (e->index != s->fn_idx) s->has_non_self_call = true;
    if (!is_tail) s->has_non_tail_call = true;
    for (int i = 0; i < e->argc; ++i) scan_expr(s, e->arg[i], false);
    break;
  default:
    scan_expr(s, e->arg[0], false);
    scan_expr(s, e->arg[1], false);
    break;
  }
}

static bool is_simple(const CbExpr *e) {
  return e->op == CB_CONST || e->op == CB_VAR || e->op == CB_LEN;
}

static bool is_imm32(uint64_t val) {
  int64_t sval = (int64_t)val;
  return sval >= -2147483648LL && sval <= 2147483647LL;
}

static int local_stack_offset(int idx) {
  return -8 * (5 + (idx - REG_LOCAL_MAX));
}

static void emit_load_const(FILE *f, uint64_t val, const char *reg, const char *reg32) {
  if (val == 0) {
    fprintf(f, " xorl %s, %s\n", reg32, reg32);
  } else if (val <= 0xFFFFFFFFULL) {
    fprintf(f, " movl $0x%" PRIx64 ", %s\n", val, reg32);
  } else if (is_imm32(val)) {
    fprintf(f, " movq $%" PRId64 ", %s\n", (int64_t)val, reg);
  } else {
    fprintf(f, " movabsq $0x%016" PRIx64 ", %s\n", val, reg);
  }
}

static void emit_load_local(FILE *f, int idx, const char *reg) {
  if (idx < REG_LOCAL_MAX) {
    fprintf(f, " movq %s, %s\n", local_regs[idx], reg);
  } else {
    fprintf(f, " movq %d(%%rbp), %s\n", local_stack_offset(idx), reg);
  }
}

static void emit_store_local(FILE *f, const char *reg, int idx) {
  if (idx < REG_LOCAL_MAX) {
    fprintf(f, " movq %s, %s\n", reg, local_regs[idx]);
  } else {
    fprintf(f, " movq %s, %d(%%rbp)\n", reg, local_stack_offset(idx));
  }
}

static void emit_simple_to_reg(FILE *f, const CbExpr *e, const char *reg, const char *reg32) {
  if (e->op == CB_CONST) {
    emit_load_const(f, e->value, reg, reg32);
  } else if (e->op == CB_VAR) {
    emit_load_local(f, e->index, reg);
  } else if (e->op == CB_LEN) {
    fprintf(f, " movq %%rsi, %s\n", reg);
  }
}

static void emit_expr(Emit *s, const CbExpr *e);

static void emit_cond_jump(Emit *s, const CbExpr *cond, unsigned target_label, bool jump_if_false) {
  FILE *f = s->out;
  if (cond->op >= CB_EQ && cond->op <= CB_GE) {
    const CbExpr *l = cond->arg[0];
    const CbExpr *r = cond->arg[1];
    if (r->op == CB_CONST && is_imm32(r->value)) {
      emit_expr(s, l);
      if (r->value == 0) {
        if (cond->op == CB_EQ || cond->op == CB_NE) {
          fprintf(f, " testq %%rax, %%rax\n");
        } else {
          fprintf(f, " cmpq $0, %%rax\n");
        }
      } else {
        fprintf(f, " cmpq $%" PRId64 ", %%rax\n", (int64_t)r->value);
      }
    } else if (is_simple(r)) {
      emit_expr(s, l);
      if (r->op == CB_VAR && r->index < REG_LOCAL_MAX) {
        fprintf(f, " cmpq %s, %%rax\n", local_regs[r->index]);
      } else if (r->op == CB_VAR) {
        fprintf(f, " cmpq %d(%%rbp), %%rax\n", local_stack_offset(r->index));
      } else if (r->op == CB_LEN) {
        fprintf(f, " cmpq %%rsi, %%rax\n");
      } else {
        emit_simple_to_reg(f, r, "%rcx", "%ecx");
        fprintf(f, " cmpq %%rcx, %%rax\n");
      }
    } else if (is_simple(l)) {
      emit_expr(s, r);
      fprintf(f, " movq %%rax, %%rcx\n");
      emit_simple_to_reg(f, l, "%rax", "%eax");
      fprintf(f, " cmpq %%rcx, %%rax\n");
    } else {
      emit_expr(s, l);
      fprintf(f, " pushq %%rax\n");
      emit_expr(s, r);
      fprintf(f, " movq %%rax, %%rcx\n popq %%rax\n cmpq %%rcx, %%rax\n");
    }
    static const char *jcc_true[] = {"je", "jne", "jb", "jbe", "ja", "jae"};
    static const char *jcc_false[] = {"jne", "je", "jae", "ja", "jbe", "jb"};
    const char *jcc = jump_if_false ? jcc_false[cond->op - CB_EQ] : jcc_true[cond->op - CB_EQ];
    fprintf(f, " %s .L%u\n", jcc, target_label);
    return;
  }
  emit_expr(s, cond);
  fprintf(f, " testq %%rax, %%rax\n %s .L%u\n", jump_if_false ? "jz" : "jnz", target_label);
}

static void emit_call_args_to_regs(Emit *s, const CbExpr *e) {
  FILE *f = s->out;
  int argc = e->argc;
  if (argc == 0) return;

  bool all_simple = true;
  for (int i = 0; i < argc; ++i) {
    if (!is_simple(e->arg[i])) {
      all_simple = false;
      break;
    }
  }

  static const char *arg_regs32[CB_MAX_PARAMS] = {"%edx", "%ecx", "%r8d", "%r9d"};
  if (all_simple) {
    for (int i = 0; i < argc; ++i) {
      emit_simple_to_reg(f, e->arg[i], arg_regs[i], arg_regs32[i]);
    }
    return;
  }

  int pushed = 0;
  for (int i = 0; i < argc; ++i) {
    if (i == argc - 1) {
      emit_expr(s, e->arg[i]);
      fprintf(f, " movq %%rax, %s\n", arg_regs[i]);
    } else {
      emit_expr(s, e->arg[i]);
      fprintf(f, " pushq %%rax\n");
      pushed++;
    }
  }
  for (int i = pushed - 1; i >= 0; --i) {
    fprintf(f, " popq %s\n", arg_regs[i]);
  }
}

static void emit_binop(Emit *s, const CbExpr *e) {
  FILE *f = s->out;
  const CbExpr *l = e->arg[0];
  const CbExpr *r = e->arg[1];
  CbOp op = e->op;

  if (r->op == CB_CONST) {
    uint64_t v = r->value;
    if (op == CB_SHL || op == CB_SHR) {
      emit_expr(s, l);
      unsigned sh = (unsigned)(v & 63);
      if (sh != 0) {
        fprintf(f, " %s $%u, %%rax\n", op == CB_SHL ? "shlq" : "shrq", sh);
      }
      return;
    }
    if ((op == CB_ADD || op == CB_SUB || op == CB_AND || op == CB_OR || op == CB_XOR || op == CB_MUL) && is_imm32(v)) {
      emit_expr(s, l);
      if (op == CB_ADD) fprintf(f, " addq $%" PRId64 ", %%rax\n", (int64_t)v);
      else if (op == CB_SUB) fprintf(f, " subq $%" PRId64 ", %%rax\n", (int64_t)v);
      else if (op == CB_AND) fprintf(f, " andq $%" PRId64 ", %%rax\n", (int64_t)v);
      else if (op == CB_OR) fprintf(f, " orq $%" PRId64 ", %%rax\n", (int64_t)v);
      else if (op == CB_XOR) fprintf(f, " xorq $%" PRId64 ", %%rax\n", (int64_t)v);
      else if (op == CB_MUL) fprintf(f, " imulq $%" PRId64 ", %%rax, %%rax\n", (int64_t)v);
      return;
    }
    if ((op == CB_DIV || op == CB_MOD) && v != 0 && (v & (v - 1)) == 0) {
      emit_expr(s, l);
      if (op == CB_DIV) {
        unsigned sh = 0;
        while ((1ULL << sh) != v) sh++;
        if (sh > 0) fprintf(f, " shrq $%u, %%rax\n", sh);
      } else {
        if (is_imm32(v - 1)) {
          fprintf(f, " andq $%" PRId64 ", %%rax\n", (int64_t)(v - 1));
        } else {
          fprintf(f, " movabsq $0x%016" PRIx64 ", %%rcx\n andq %%rcx, %%rax\n", v - 1);
        }
      }
      return;
    }
  }

  if (is_simple(r)) {
    emit_expr(s, l);
    emit_simple_to_reg(f, r, "%rcx", "%ecx");
  } else if (is_simple(l)) {
    emit_expr(s, r);
    fprintf(f, " movq %%rax, %%rcx\n");
    emit_simple_to_reg(f, l, "%rax", "%eax");
  } else {
    emit_expr(s, l);
    fprintf(f, " pushq %%rax\n");
    emit_expr(s, r);
    fprintf(f, " movq %%rax, %%rcx\n popq %%rax\n");
  }

  switch (op) {
  case CB_ADD: fprintf(f, " addq %%rcx, %%rax\n"); break;
  case CB_SUB: fprintf(f, " subq %%rcx, %%rax\n"); break;
  case CB_MUL: fprintf(f, " imulq %%rcx, %%rax\n"); break;
  case CB_AND: fprintf(f, " andq %%rcx, %%rax\n"); break;
  case CB_OR:  fprintf(f, " orq %%rcx, %%rax\n"); break;
  case CB_XOR: fprintf(f, " xorq %%rcx, %%rax\n"); break;
  case CB_SHL: fprintf(f, " shlq %%cl, %%rax\n"); break;
  case CB_SHR: fprintf(f, " shrq %%cl, %%rax\n"); break;
  case CB_DIV:
  case CB_MOD: {
    unsigned lbl_zero = s->label++;
    unsigned lbl_done = s->label++;
    fprintf(f, " testq %%rcx, %%rcx\n jz .L%u\n xorl %%edx, %%edx\n divq %%rcx\n", lbl_zero);
    if (op == CB_MOD) fprintf(f, " movq %%rdx, %%rax\n");
    fprintf(f, " jmp .L%u\n.L%u:\n xorl %%eax, %%eax\n.L%u:\n", lbl_done, lbl_zero, lbl_done);
    break;
  }
  default: {
    static const char *conds[] = {"e", "ne", "b", "be", "a", "ae"};
    fprintf(f, " xorl %%edx, %%edx\n cmpq %%rcx, %%rax\n set%s %%dl\n movq %%rdx, %%rax\n",
            conds[op - CB_EQ]);
    break;
  }
  }
}

static void emit_expr(Emit *s, const CbExpr *e) {
  FILE *f = s->out;
  switch (e->op) {
  case CB_CONST:
    emit_load_const(f, e->value, "%rax", "%eax");
    break;
  case CB_VAR:
    emit_load_local(f, e->index, "%rax");
    break;
  case CB_LEN:
    fprintf(f, " movq %%rsi, %%rax\n");
    break;
  case CB_GET: {
    unsigned lbl_zero = s->label++;
    unsigned lbl_done = s->label++;
    emit_expr(s, e->arg[0]);
    fprintf(f, " cmpq %%rsi, %%rax\n jae .L%u\n movq (%%rdi,%%rax,8), %%rax\n jmp .L%u\n.L%u:\n xorl %%eax, %%eax\n.L%u:\n",
            lbl_zero, lbl_done, lbl_zero, lbl_done);
    break;
  }
  case CB_IF: {
    unsigned lbl_else = s->label++;
    unsigned lbl_done = s->label++;
    emit_cond_jump(s, e->arg[0], lbl_else, true);
    emit_expr(s, e->arg[1]);
    fprintf(f, " jmp .L%u\n.L%u:\n", lbl_done, lbl_else);
    emit_expr(s, e->arg[2]);
    fprintf(f, ".L%u:\n", lbl_done);
    break;
  }
  case CB_LET:
    emit_expr(s, e->arg[0]);
    emit_store_local(f, "%rax", e->index);
    emit_expr(s, e->arg[1]);
    break;
  case CB_CALL:
    emit_call_args_to_regs(s, e);
    fprintf(f, " call .Lfn%d\n", e->index);
    break;
  default:
    emit_binop(s, e);
    break;
  }
}

static void emit_epilogue(Emit *s) {
  FILE *f = s->out;
  int num_reg_locals = s->fn->locals < REG_LOCAL_MAX ? s->fn->locals : REG_LOCAL_MAX;
  bool use_frame = (s->fn->locals > REG_LOCAL_MAX) || s->has_non_tail_call;
  if (!use_frame && num_reg_locals == 0) {
    fprintf(f, " ret\n");
    return;
  }
  if (use_frame) {
    for (int i = 0; i < num_reg_locals; ++i) {
      fprintf(f, " movq %d(%%rbp), %s\n", -8 * (1 + i), local_regs[i]);
    }
    fprintf(f, " leave\n ret\n");
  } else {
    for (int i = num_reg_locals - 1; i >= 0; --i) {
      fprintf(f, " popq %s\n", local_regs[i]);
    }
    fprintf(f, " ret\n");
  }
}

static void emit_tail(Emit *s, const CbExpr *e) {
  FILE *f = s->out;
  if (e->op == CB_IF) {
    unsigned lbl_else = s->label++;
    emit_cond_jump(s, e->arg[0], lbl_else, true);
    emit_tail(s, e->arg[1]);
    fprintf(f, ".L%u:\n", lbl_else);
    emit_tail(s, e->arg[2]);
  } else if (e->op == CB_LET) {
    emit_expr(s, e->arg[0]);
    emit_store_local(f, "%rax", e->index);
    emit_tail(s, e->arg[1]);
  } else if (e->op == CB_CALL) {
    if (e->index == s->fn_idx) {
      emit_call_args_to_regs(s, e);
      for (int i = 0; i < e->argc; ++i) {
        emit_store_local(f, arg_regs[i], i);
      }
      fprintf(f, " jmp .Lloop%d\n", s->fn_idx);
    } else {
      emit_call_args_to_regs(s, e);
      int num_reg_locals = s->fn->locals < REG_LOCAL_MAX ? s->fn->locals : REG_LOCAL_MAX;
      bool use_frame = (s->fn->locals > REG_LOCAL_MAX) || s->has_non_tail_call;
      if (use_frame) {
        for (int i = 0; i < num_reg_locals; ++i) {
          fprintf(f, " movq %d(%%rbp), %s\n", -8 * (1 + i), local_regs[i]);
        }
        fprintf(f, " leave\n jmp .Lfn%d\n", e->index);
      } else {
        for (int i = num_reg_locals - 1; i >= 0; --i) {
          fprintf(f, " popq %s\n", local_regs[i]);
        }
        fprintf(f, " jmp .Lfn%d\n", e->index);
      }
    }
  } else {
    emit_expr(s, e);
    emit_epilogue(s);
  }
}

typedef struct {
  bool valid;
  int step;
  int idx_var;
  int acc_var;
  CbOp reduce_op;
  bool limit_is_len;
  uint64_t limit_const;
} LoopPattern;

static bool detect_simple_array_reduction(const CbFunction *fn, int fn_idx, LoopPattern *pat) {
  memset(pat, 0, sizeof(*pat));
  if (fn->arity != 2 || fn->locals != 2) return false;
  const CbExpr *body = fn->body;
  if (!body || body->op != CB_IF) return false;

  const CbExpr *cond = body->arg[0];
  const CbExpr *t_branch = body->arg[1];
  const CbExpr *f_branch = body->arg[2];

  const CbExpr *base_expr = NULL;
  const CbExpr *rec_expr = NULL;
  int idx_var = -1;
  int acc_var = -1;
  bool limit_is_len = false;
  uint64_t limit_const = 0;

  if (cond->op == CB_GE || cond->op == CB_EQ) {
    if (cond->arg[0]->op == CB_VAR && cond->arg[1]->op == CB_LEN) {
      idx_var = cond->arg[0]->index;
      limit_is_len = true;
      base_expr = t_branch;
      rec_expr = f_branch;
    }
  } else if (cond->op == CB_LT) {
    if (cond->arg[0]->op == CB_VAR && cond->arg[1]->op == CB_LEN) {
      idx_var = cond->arg[0]->index;
      limit_is_len = true;
      base_expr = f_branch;
      rec_expr = t_branch;
    }
  }

  if (idx_var < 0 || idx_var > 1) return false;
  acc_var = 1 - idx_var;

  if (!base_expr || base_expr->op != CB_VAR || base_expr->index != acc_var) return false;
  if (!rec_expr || rec_expr->op != CB_CALL || rec_expr->index != fn_idx || rec_expr->argc != 2) return false;

  const CbExpr *next_idx = rec_expr->arg[idx_var];
  const CbExpr *next_acc = rec_expr->arg[acc_var];

  if (next_idx->op != CB_ADD) return false;
  if (next_idx->arg[0]->op != CB_VAR || next_idx->arg[0]->index != idx_var) return false;
  if (next_idx->arg[1]->op != CB_CONST || next_idx->arg[1]->value != 1) return false;

  if (next_acc->op != CB_ADD && next_acc->op != CB_XOR && next_acc->op != CB_OR && next_acc->op != CB_AND) return false;

  const CbExpr *a0 = next_acc->arg[0];
  const CbExpr *a1 = next_acc->arg[1];
  bool ok_acc = false;
  if (a0->op == CB_VAR && a0->index == acc_var && a1->op == CB_GET &&
      a1->arg[0]->op == CB_VAR && a1->arg[0]->index == idx_var) {
    ok_acc = true;
  } else if (a1->op == CB_VAR && a1->index == acc_var && a0->op == CB_GET &&
             a0->arg[0]->op == CB_VAR && a0->arg[0]->index == idx_var) {
    ok_acc = true;
  }
  if (!ok_acc) return false;

  pat->valid = true;
  pat->step = 1;
  pat->idx_var = idx_var;
  pat->acc_var = acc_var;
  pat->reduce_op = next_acc->op;
  pat->limit_is_len = limit_is_len;
  pat->limit_const = limit_const;
  return true;
}

static void emit_avx2_reduction(Emit *s, const LoopPattern *pat) {
  FILE *f = s->out;
  int idx = s->fn_idx;
  const char *idx_reg = arg_regs[pat->idx_var];
  const char *acc_reg = arg_regs[pat->acc_var];
  const char *vec_op = "vpaddq";
  const char *scalar_op = "addq";
  if (pat->reduce_op == CB_XOR) { vec_op = "vpxor"; scalar_op = "xorq"; }
  else if (pat->reduce_op == CB_OR) { vec_op = "vpor"; scalar_op = "orq"; }
  else if (pat->reduce_op == CB_AND) { vec_op = "vpand"; scalar_op = "andq"; }

  fprintf(f, ".p2align 4\n.Lfn%d:\n", idx);
  fprintf(f, " movq %s, %%r8\n", idx_reg);
  fprintf(f, " movq %s, %%rax\n", acc_reg);
  fprintf(f, " cmpq %%rsi, %%r8\n jae .Lret%d\n", idx);
  fprintf(f, " movq %%rsi, %%r9\n subq %%r8, %%r9\n");
  if (pat->reduce_op != CB_AND) {
    fprintf(f, " cmpq $16, %%r9\n jb .Lscalar%d\n", idx);
    fprintf(f, " leaq (%%rdi,%%r8,8), %%r10\n");
    fprintf(f, " vpxor %%ymm0, %%ymm0, %%ymm0\n");
    fprintf(f, " vpxor %%ymm1, %%ymm1, %%ymm1\n");
    fprintf(f, " vpxor %%ymm2, %%ymm2, %%ymm2\n");
    fprintf(f, " vpxor %%ymm3, %%ymm3, %%ymm3\n");
    fprintf(f, ".p2align 4\n.Lvec%d:\n", idx);
    fprintf(f, " %s (%%r10), %%ymm0, %%ymm0\n", vec_op);
    fprintf(f, " %s 32(%%r10), %%ymm1, %%ymm1\n", vec_op);
    fprintf(f, " %s 64(%%r10), %%ymm2, %%ymm2\n", vec_op);
    fprintf(f, " %s 96(%%r10), %%ymm3, %%ymm3\n", vec_op);
    fprintf(f, " addq $128, %%r10\n addq $16, %%r8\n subq $16, %%r9\n cmpq $16, %%r9\n jae .Lvec%d\n", idx);
    fprintf(f, " %s %%ymm1, %%ymm0, %%ymm0\n", vec_op);
    fprintf(f, " %s %%ymm3, %%ymm2, %%ymm2\n", vec_op);
    fprintf(f, " %s %%ymm2, %%ymm0, %%ymm0\n", vec_op);
    fprintf(f, " vextracti128 $1, %%ymm0, %%xmm1\n");
    fprintf(f, " %s %%xmm1, %%xmm0, %%xmm0\n", vec_op);
    fprintf(f, " vpshufd $0x4e, %%xmm0, %%xmm1\n");
    fprintf(f, " %s %%xmm1, %%xmm0, %%xmm0\n", vec_op);
    fprintf(f, " vmovq %%xmm0, %%r11\n");
    fprintf(f, " %s %%r11, %%rax\n", scalar_op);
    fprintf(f, " vzeroupper\n");
    fprintf(f, " cmpq %%rsi, %%r8\n jae .Lret%d\n", idx);
  }
  fprintf(f, ".p2align 4\n.Lscalar%d:\n", idx);
  fprintf(f, " %s (%%rdi,%%r8,8), %%rax\n", scalar_op);
  fprintf(f, " incq %%r8\n cmpq %%rsi, %%r8\n jb .Lscalar%d\n", idx);
  fprintf(f, ".Lret%d:\n ret\n", idx);
}

int cb_emit_assembly(const CbProgram *p, const char *path,
                     char *error, size_t error_size) {
  for (int i = 0; i < p->nodes; ++i) {
    if (p->pool[i].op > CB_GE) {
      snprintf(error, error_size, "v1 baseline supports scalar ASTs only");
      return 0;
    }
  }
  FILE *f = fopen(path, "w");
  if (!f) {
    snprintf(error, error_size, "%s: %s", path, strerror(errno));
    return 0;
  }

  Emit s;
  memset(&s, 0, sizeof(s));
  s.out = f;
  s.prog = p;
  s.label = 1;

  fprintf(f, ".text\n.globl bench_entry\n.type bench_entry, @function\n"
             "bench_entry:\n jmp .Lfn%d\n.size bench_entry, .-bench_entry\n",
          p->entry);

  for (int i = 0; i < p->functions; ++i) {
    s.fn = &p->fn[i];
    s.fn_idx = i;
    s.max_local = s.fn->arity - 1;
    s.has_non_self_call = false;
    s.has_non_tail_call = false;
    scan_expr(&s, s.fn->body, true);

    LoopPattern pat;
    if (detect_simple_array_reduction(s.fn, i, &pat)) {
      emit_avx2_reduction(&s, &pat);
      continue;
    }

    int num_reg_locals = s.fn->locals < REG_LOCAL_MAX ? s.fn->locals : REG_LOCAL_MAX;
    int stack_locals = s.fn->locals > REG_LOCAL_MAX ? (s.fn->locals - REG_LOCAL_MAX) : 0;
    bool use_frame = (stack_locals > 0) || s.has_non_tail_call;

    fprintf(f, ".p2align 4\n.Lfn%d:\n", i);
    if (use_frame) {
      unsigned frame_bytes = (unsigned)(4 + stack_locals) * 8;
      frame_bytes = (frame_bytes + 15) & ~15u;
      fprintf(f, " pushq %%rbp\n movq %%rsp, %%rbp\n subq $%u, %%rsp\n", frame_bytes);
      for (int j = 0; j < num_reg_locals; ++j) {
        fprintf(f, " movq %s, %d(%%rbp)\n", local_regs[j], -8 * (1 + j));
      }
    } else {
      for (int j = 0; j < num_reg_locals; ++j) {
        fprintf(f, " pushq %s\n", local_regs[j]);
      }
    }

    for (int j = 0; j < s.fn->arity; ++j) {
      emit_store_local(f, arg_regs[j], j);
    }

    fprintf(f, ".Lloop%d:\n", i);
    emit_tail(&s, s.fn->body);
  }

  fprintf(f, ".section .note.GNU-stack,\"\",@progbits\n");
  int ok = !ferror(f);
  if (fclose(f)) ok = 0;
  if (!ok) snprintf(error, error_size, "could not write %s", path);
  return ok;
}