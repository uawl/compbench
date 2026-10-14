아래 AST를 Linux x86-64/AVX2 어셈블리로 변환하는 C11 코드 생성기를 구현하라. 베이스라인의 의미를 보존하면서 생성 코드의 실행 속도를 최적화하라.

파서와 `ast.h`는 이미 제공되며 수정하지 않는다. 아래 stub을 완성한 **`solution.c`의 C 소스 코드만 출력하라.** 설명, Markdown 코드 블록, `main()`은 출력하지 않는다. 필요한 보조 함수는 같은 파일에 작성한다.

```c
#include "ast.h"

int cb_emit_assembly(const CbProgram *p, const char *path,
                     char *error, size_t error_size) {
    /* p를 읽어 path에 어셈블리를 작성한다.
       성공 시 1, 실패 시 error에 메시지를 기록하고 0을 반환한다. */
}
```

측정 대상의 개요:

- 입력 배열을 순회하며 합계나 unsigned 최댓값을 구하는 정수 누산.
- 조건에 맞는 원소의 합계·개수 계산과 몫·나머지를 사용하는 정수 계산.
- 보조 함수 호출과 상호 재귀를 포함하는 입력 순회 및 원소별 계산.
- 비꼬리 재귀로 값을 누산하거나 여러 재귀 분기의 결과를 결합하는 계산.
- 리스트의 누산, 순서 변환, 원소별 변환 및 여러 변환을 조합하는 계산.
- 기존 리스트와 공유된 꼬리를 보존하는 계산, 여러 리스트와 정수 인자를 함께 사용하는 순회, 비꼬리 재귀를 통한 리스트 생성.

이 개요는 작업 종류를 설명한다. 실제 AST의 구조와 의미를 분석해 여러 프로그램에 적용할 수 있는 최적화를 구현하라.

의미와 출력 규약:

- 타입은 `u64`와 불변 `list<u64>`다. 함수 인자·지역 바인딩·반환값에 두 타입을 사용할 수 있다. 타입은 파서가 추론한다. 정수의 덧셈·뺄셈·곱셈은 modulo 2^64, 비교와 나눗셈은 unsigned다. 나누는 값이 0이면 몫과 나머지 모두 0이다. 시프트 이동량은 `b & 63`이며 오른쪽 시프트는 논리 시프트다. 비교 결과는 0 또는 1이다.
- `input`은 입력 원소를 순서대로 담은 리스트, `nil`은 빈 리스트다. `(cons x xs)`는 기존 `xs`를 변경하지 않고 앞에 원소를 붙인다. `(head nil)`은 0, `(tail nil)`은 `nil`, `(is_nil xs)`는 빈 리스트면 1, 아니면 0이다. `head`·`tail`은 첫 원소·나머지 리스트를 반환한다. 리스트 원소는 `u64`이며 중첩 리스트는 없다.
- 모든 함수는 같은 읽기 전용 입력을 본다. 기존 `len`과 `get(i)`는 원본 입력 배열의 길이와 원소를 뜻하며 `get`의 범위 밖 결과는 0이다. 범위 밖 주소를 실제로 읽지 않는다. 배열의 SIMD 정렬이나 패딩을 가정하지 않는다.
- `if`는 조건이 0이 아니면 참 분기, 아니면 거짓 분기만 평가한다. `let`은 어휘적 바인딩이며 값은 바깥 범위에서 평가한다. 직접 재귀·상호 재귀·비꼬리 재귀를 지원한다.
- 생성 파일은 GNU assembler 형식이며 PIC으로 링크 가능해야 한다. `uint64_t bench_entry(const uint64_t *xs, uint64_t n, CbArena *arena)`를 정의하고 System V AMD64 호출 규약을 지킨다. 진입 함수의 반환 타입이 `CB_U64`면 정수, `CB_LIST`면 `CbList *`를 `uint64_t`로 표현해 반환한다. 길이 0인 입력을 역참조하거나 기존 리스트·입력 배열을 변경하지 않는다.
- AST는 읽기 전용이다. 모든 연산을 지원하고 특정 함수 이름에 답을 하드코딩하지 않는다. C 표준 라이브러리를 사용할 수 있으나 코드 생성을 외부 도구에 위임하거나 참조 실행기를 호출하지 않는다. 생성 코드도 계산을 자체적으로 수행하며 호출 간 결과 캐시를 남기지 않는다. AVX2 이후의 명령어는 사용하지 않는다.

AST 구조 (`ast.h`):

```c
#ifndef COMPBENCH_AST_H
#define COMPBENCH_AST_H

#include <stddef.h>
#include <stdint.h>

#define CB_MAX_FUNCTIONS 64
#define CB_MAX_PARAMS 4
#define CB_MAX_LOCALS 128
#define CB_MAX_NODES 4096
#define CB_NAME_SIZE 64
#define CB_MAX_LIST_NODES 1048576

typedef enum { CB_U64, CB_LIST } CbType;

/* Persistent lists of u64. NULL represents nil. */
typedef struct CbList {
    uint64_t head;
    const struct CbList *tail;
} CbList;

/* The caller resets used and failed before every generated entry call. */
typedef struct {
    CbList *nodes;
    uint64_t used, capacity;
    const CbList *input;
    uint64_t failed;
} CbArena;

typedef struct {
    CbType type;
    uint64_t scalar;
    uint64_t *xs;
    uint64_t n;
} CbResult;

typedef enum {
    CB_CONST, CB_VAR, CB_LEN, CB_GET, CB_IF, CB_LET, CB_CALL,
    CB_ADD, CB_SUB, CB_MUL, CB_DIV, CB_MOD, CB_AND, CB_OR, CB_XOR,
    CB_SHL, CB_SHR, CB_EQ, CB_NE, CB_LT, CB_LE, CB_GT, CB_GE,
    CB_INPUT, CB_NIL, CB_CONS, CB_HEAD, CB_TAIL, CB_IS_NIL
} CbOp;

typedef struct CbExpr {
    CbOp op;
    CbType type;
    uint64_t value;
    int index, temp, argc;
    char name[CB_NAME_SIZE];
    struct CbExpr *arg[CB_MAX_PARAMS];
} CbExpr;

typedef struct {
    char name[CB_NAME_SIZE];
    int arity, locals, temps;
    CbType return_type, local_types[CB_MAX_LOCALS];
    CbExpr *body;
} CbFunction;

typedef struct {
    CbFunction fn[CB_MAX_FUNCTIONS];
    int functions, entry, nodes;
    CbExpr pool[CB_MAX_NODES];
} CbProgram;

/* Error messages are returned to the caller; parsing never exits the process. */
int cb_parse_file(const char *path, CbProgram *p, char *error, size_t error_size);
int cb_parse_text(const char *text, CbProgram *p, char *error, size_t error_size);
int cb_eval(const CbProgram *p, const uint64_t *xs, uint64_t n,
            uint64_t budget, uint64_t *result, char *error, size_t error_size);
/* List results are flattened into owned xs; release with cb_result_free(). */
int cb_eval_result(const CbProgram *p, const uint64_t *xs, uint64_t n,
                   uint64_t budget, CbResult *result, char *error, size_t error_size);
void cb_result_free(CbResult *result);
int cb_emit_assembly(const CbProgram *p, const char *path,
                     char *error, size_t error_size);

#endif
```

`p->entry`는 인자가 없는 진입 함수의 `p->fn` 인덱스다. 노드의 `type`, 함수의 `return_type`과 `local_types`는 추론된 타입이다. 각 함수의 인자는 지역 슬롯 `0 .. arity-1`이고 `locals`는 인자와 `let` 슬롯의 총수다. `let`마다 별도 슬롯을 부여한다. `temp`는 함수 안에서 고유한 표현식 번호이며 `temps`는 그 개수다. 슬롯과 노드 번호는 실행 순서를 뜻하지 않는다.

리스트 표현은 `CbList { head, tail }`이며 NULL이 `nil`이다. `arena->input`은 `xs[0 .. n)`에 대응하는 읽기 전용 입력 리스트다. 호출 시 `arena->used`와 `arena->failed`는 0이다. 새 셀은 `arena->nodes[arena->used++]`에 초기화한다. 공간이 부족하면 범위 밖에 쓰지 않고 `arena->failed = 1`로 표시한다. `nodes`, `capacity`, `input` 필드는 바꾸지 않는다. 반환 리스트의 모든 셀은 사용한 arena 영역이나 입력 리스트에 속해야 하고, 다음 호출 전에 유효해야 한다. 셀은 공유할 수 있으나 초기화 후 변경하지 않는다.

| 노드 | 필드 |
| --- | --- |
| `CB_CONST` | `value`: 상수 |
| `CB_VAR` | `index`: 지역 슬롯 |
| `CB_LEN` | 배열 길이 |
| `CB_GET` | `arg[0]`: 원소 인덱스 |
| `CB_INPUT`, `CB_NIL` | 입력 리스트, 빈 리스트 |
| `CB_CONS` | `arg[0]`: `u64` 원소, `arg[1]`: 나머지 리스트 |
| `CB_HEAD`, `CB_TAIL`, `CB_IS_NIL` | `arg[0]`: 리스트 |
| `CB_IF` | `arg[0]`: 조건, `arg[1]`: 참 분기, `arg[2]`: 거짓 분기 |
| `CB_LET` | `index`: 새 슬롯, `arg[0]`: 값, `arg[1]`: 본문 |
| `CB_CALL` | `index`: 함수 인덱스, `argc`: 인자 수, `arg[0 .. argc)`: 인자 |
| 이항 연산 | `arg[0]`: 왼쪽, `arg[1]`: 오른쪽 피연산자 |

`argc`는 호출 노드에서만 사용한다. 호출 인자는 왼쪽부터 평가한다. 함수의 `body`와 노드의 자식 포인터는 `p->pool` 안의 노드를 가리킨다. 바인딩과 호출 대상은 파서가 이미 해결했다.

베이스라인 codegen 전체. 출발점이나 일반 코드 생성 경로로 사용해도 된다:

```c
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
```
