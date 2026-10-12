#include "ast.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CbProgram *p;
static unsigned checks;

static void check(const char *text, const uint64_t *xs, uint64_t n, uint64_t expected) {
    char error[256];
    uint64_t actual;
    if (!cb_parse_text(text, p, error, sizeof(error)) ||
        !cb_eval(p, xs, n, 20000000, &actual, error, sizeof(error))) {
        fprintf(stderr, "unexpected failure: %s\n", error); exit(1);
    }
    if (actual != expected) {
        fprintf(stderr, "expected %" PRIu64 ", got %" PRIu64 "\n", expected, actual); exit(1);
    }
    ++checks;
}

static void reject(const char *text) {
    char error[256];
    if (cb_parse_text(text, p, error, sizeof(error))) {
        fprintf(stderr, "invalid AST accepted: %s\n", text); exit(1);
    }
    ++checks;
}

int main(void) {
    p = malloc(sizeof(*p));
    if (!p) return 1;
    uint64_t xs[] = {UINT64_MAX, 1, UINT64_C(0x8000000000000000)};
    check("(program (fn main () (add 18446744073709551615 1)))", xs, 3, 0);
    check("(program (fn main () (sub 0 1)))", xs, 3, UINT64_MAX);
    check("(program (fn main () (mul 18446744073709551615 2)))", xs, 3, UINT64_MAX - 1);
    check("(program (fn main () (div 99 0)))", xs, 3, 0);
    check("(program (fn main () (mod 99 0)))", xs, 3, 0);
    check("(program (fn main () (div 18446744073709551615 2)))", xs, 3, UINT64_MAX / 2);
    check("(program (fn main () (mod 18446744073709551615 2)))", xs, 3, 1);
    check("(program (fn main () (gt 0x8000000000000000 0x7fffffffffffffff)))", xs, 3, 1);
    check("(program (fn main () (shl 1 64)))", xs, 3, 1);
    check("(program (fn main () (shr 0x8000000000000000 127)))", xs, 3, 1);
    check("(program (fn main () (get 3)))", xs, 3, 0);
    check("(program (fn main () (get 0)))", NULL, 0, 0);
    check("(program (fn main () (let x 10 (add (let x 20 x) x))))", xs, 3, 30);
    check("(program (fn main () (if 1 7 (call forever))) (fn forever () (call forever)))", xs, 3, 7);
    check("(program (fn main () (call count 100000 0)) (fn count (n a) (if (eq n 0) a (call count (sub n 1) (add a 1)))))", xs, 3, 100000);
    check("(program (fn main () (call a 100000 0)) (fn a (n x) (if (eq n 0) x (call b (sub n 1) (add x 1)))) (fn b (n x) (call a n x)))", xs, 3, 100000);
    check("(program (fn main () (call f 1 2 3 4)) (fn f (a b c d) (add (add a b) (add c d))))", xs, 3, 10);
    check("(program (fn main () (call f 10)) (fn f (n) (if (eq n 0) 0 (add n (call f (sub n 1))))))", xs, 3, 55);
    check("; comment\n(program (fn main () 012))", xs, 3, 12);
    check("(program (fn main () (let input 7 (let nil 2 (add input nil)))))", xs, 3, 9);
    reject("(program (fn main () input))");
    reject("(program (fn main () nil))");
    reject("(program (fn main () (cons 1 0)))");
    reject("(program (fn main () (head 0)))");
    reject("(program (fn main () (tail 0)))");
    reject("(program (fn main () (is_nil 0)))");
    reject("(program (fn main () 18446744073709551616))");
    reject("(program (fn main () -1))");
    reject("(program (fn main () x))");
    reject("(program (fn main () (call missing)))");
    reject("(program (fn main () (call f 1)) (fn f () 0))");
    reject("(program (fn main (x) x))");
    reject("(program (fn main () 0) (fn main () 1))");
    reject("(program (fn main () 0) (fn f (x x) x))");
    reject("(program (fn main () (let x x x)))");
    reject("(program (fn main () 0)) trailing");
    reject("(program (fn main () (add 1)))");
    reject("(program");
    char error[256];
    uint64_t result;
    if (!cb_parse_text("(program (fn main () (call main)))", p, error, sizeof(error)) ||
        cb_eval(p, xs, 3, 100, &result, error, sizeof(error))) return 1;
    ++checks;
    free(p);
    printf("AST semantics: %u checks passed\n", checks);
    return 0;
}
