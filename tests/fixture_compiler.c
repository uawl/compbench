#include <stdio.h>

#ifndef STYLE
#define STYLE 0
#endif

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    if (STYLE == 6) { fputs("intentional compile failure\n", stderr); return 1; }
    if (STYLE == 7) for (;;) {}
    FILE *f = fopen(argv[2], "w");
    if (!f) return 2;
    if (STYLE == 5) fputs("this is not assembly\n", f);
    else {
        fputs(".text\n.globl bench_entry\nbench_entry:\n", f);
        if (STYLE == 0) fputs("xorq %rax, %rax\nret\n", f);
        if (STYLE == 1) fputs("ud2\n", f);
        if (STYLE == 2) fputs("movq $1, (%rdi)\nxorq %rax, %rax\nret\n", f);
        if (STYLE == 3) fputs("movq (%rdi,%rsi,8), %rax\nret\n", f);
        if (STYLE == 4) fputs("jmp bench_entry\n", f);
        if (STYLE == 8) fputs("movq (%rdx), %rax\nmovq $1, (%rax)\nmovq %rax, 8(%rax)\nmovq $1, 8(%rdx)\nret\n", f);
        if (STYLE == 9) fputs("movq $7, %rax\nret\n", f);
        if (STYLE == 10) fputs("xorq %rax, %rax\nret\n", f);
        if (STYLE == 11) fputs("movq 24(%rdx), %rax\nmovq $1, (%rax)\nret\n", f);
        if (STYLE == 12) fputs("movq $1, 32(%rdx)\nxorq %rax, %rax\nret\n", f);
        if (STYLE == 13) fputs("movq (%rdx), %rax\nmovq $2, (%rax)\nmovq $0, 8(%rax)\nmovq $1, 8(%rdx)\nret\n", f);
        fputs(".section .note.GNU-stack,\"\",@progbits\n", f);
    }
    return fclose(f) ? 1 : 0;
}
