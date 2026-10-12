#ifndef COMPBENCH_AST_H
#define COMPBENCH_AST_H

#include <stddef.h>
#include <stdint.h>

#define CB_MAX_FUNCTIONS 64
#define CB_MAX_PARAMS 4
#define CB_MAX_LOCALS 128
#define CB_MAX_NODES 4096
#define CB_NAME_SIZE 64

typedef enum {
    CB_CONST, CB_VAR, CB_LEN, CB_GET, CB_IF, CB_LET, CB_CALL,
    CB_ADD, CB_SUB, CB_MUL, CB_DIV, CB_MOD, CB_AND, CB_OR, CB_XOR,
    CB_SHL, CB_SHR, CB_EQ, CB_NE, CB_LT, CB_LE, CB_GT, CB_GE
} CbOp;

typedef struct CbExpr {
    CbOp op;
    uint64_t value;
    int index, temp, argc;
    char name[CB_NAME_SIZE];
    struct CbExpr *arg[CB_MAX_PARAMS];
} CbExpr;

typedef struct {
    char name[CB_NAME_SIZE];
    int arity, locals, temps;
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
int cb_emit_assembly(const CbProgram *p, const char *path,
                     char *error, size_t error_size);

#endif
