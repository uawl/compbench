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
