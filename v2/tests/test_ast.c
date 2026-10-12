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

static void check_list(const char *text, const uint64_t *xs, uint64_t n,
                        const uint64_t *expected, uint64_t expected_n) {
    char error[256];
    CbResult result;
    if (!cb_parse_text(text, p, error, sizeof(error)) ||
        !cb_eval_result(p, xs, n, 20000000, &result, error, sizeof(error))) {
        fprintf(stderr, "unexpected list failure: %s\n", error); exit(1);
    }
    if (result.type != CB_LIST || result.n != expected_n ||
        (expected_n && memcmp(result.xs, expected, (size_t)expected_n * sizeof(uint64_t)))) {
        fprintf(stderr, "list result mismatch\n"); exit(1);
    }
    cb_result_free(&result);
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
    check("(program (fn main () (head nil)))", xs, 3, 0);
    check("(program (fn main () (is_nil (tail nil))))", xs, 3, 1);
    check("(program (fn main () (head input)))", xs, 3, UINT64_MAX);
    check("(program (fn main () (head (tail input))))", xs, 3, 1);
    check("(program (fn main () (head input)))", NULL, 0, 0);
    check("(program (fn main () (let ys (cons 7 input) (add (head ys) (head (tail ys))))))", xs, 3, 6);
    check("(program (fn main () (call f nil)) (fn f (xs) (call g xs)) (fn g (xs) (is_nil xs)))", xs, 3, 1);
    check_list("(program (fn main () nil))", xs, 3, NULL, 0);
    check_list("(program (fn main () input))", xs, 3, xs, 3);
    check_list("(program (fn main () (tail input)))", xs, 3, xs + 1, 2);
    uint64_t list[] = {7, 8};
    check_list("(program (fn main () (cons 7 (cons 8 nil))))", xs, 3, list, 2);
    check_list("(program (fn main () (call identity input)) (fn identity (xs) xs))", xs, 3, xs, 3);
    check_list("(program (fn main () (call a input)) (fn a (xs) (call b xs)) (fn b (xs) xs))", xs, 3, xs, 3);
    check_list("(program (fn main () (let xs (cons 7 nil) (let ys (cons 8 xs) xs))))", xs, 3, list, 1);
    check_list("(program (fn main () (let xs (cons 7 nil) (let ys (cons 8 xs) (tail ys)))))", xs, 3, list, 1);
    check_list("(program (fn main () (if 1 nil (cons 9 nil))))", xs, 3, NULL, 0);
    check_list("(program (fn main () (call rev input nil)) (fn rev (xs acc) (if (is_nil xs) acc (call rev (tail xs) (cons (head xs) acc)))))",
               list, 2, (uint64_t[]){8, 7}, 2);
    reject("(program (fn main () (head 0)))");
    reject("(program (fn main () (tail 0)))");
    reject("(program (fn main () (cons input nil)))");
    reject("(program (fn main () (cons 1 2)))");
    reject("(program (fn main () (add input 1)))");
    reject("(program (fn main () (eq nil input)))");
    reject("(program (fn main () (if nil 1 2)))");
    reject("(program (fn main () (if 1 nil 0)))");
    reject("(program (fn main () (add (call f 1) (head (call f nil)))) (fn f (x) x))");
    reject("(program (fn main () (let input 0 input)))");
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
    if (!cb_parse_text("(program (fn main () (call forever nil)) (fn forever (xs) (call forever (cons 1 xs))))", p, error, sizeof(error)) ||
        cb_eval_result(p, xs, 3, 100, &(CbResult){0}, error, sizeof(error))) return 1;
    ++checks;
    free(p);
    printf("AST semantics: %u checks passed\n", checks);
    return 0;
}
