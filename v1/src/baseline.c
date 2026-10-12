#include "ast.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s INPUT.ast OUTPUT.s\n", argv[0]);
        return 2;
    }
    CbProgram *p = malloc(sizeof(*p));
    char error[256];
    if (!p) return 2;
    int ok = cb_parse_file(argv[1], p, error, sizeof(error)) &&
             cb_emit_assembly(p, argv[2], error, sizeof(error));
    if (!ok) fprintf(stderr, "%s\n", error);
    free(p);
    return ok ? 0 : 1;
}
