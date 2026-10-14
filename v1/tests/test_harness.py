"""Integration checks; the compiler, evaluator and harness themselves are C."""
import ctypes
import json
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MASK = (1 << 64) - 1


def run(*args, **kwargs):
    return subprocess.run(args, cwd=ROOT, text=True, capture_output=True,
                          timeout=45, **kwargs)


class HarnessTests(unittest.TestCase):
    def test_baseline_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "results.json"
            result = run("./build/v1/compbench", "--compiler", "./build/v1/baseline-compiler",
                         "--output", str(output), "--seed", "12345")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(output.read_text())
            self.assertEqual(report["passed"], 26)
            self.assertEqual(report["total"], 26)
            self.assertEqual(report["seed"], 12345)
            self.assertEqual(report["benchmark_version"], "v1")
            self.assertGreater(report["score"], 0)
            self.assertTrue(all(t["checks"] >= 280 for t in report["tasks"]))

    def test_codegen_against_independent_u64_oracle(self):
        operations = ["add", "sub", "mul", "div", "mod", "and", "or", "xor",
                      "shl", "shr", "eq", "ne", "lt", "le", "gt", "ge"]
        expression = "(get 99)"
        for i, op in reversed(list(enumerate(operations))):
            expression = f"(if (eq (get 0) {i}) ({op} (get 1) (get 2)) {expression})"
        with tempfile.TemporaryDirectory() as directory:
            ast, assembly, library = [Path(directory) / f"ops.{suffix}" for suffix in ("ast", "s", "so")]
            ast.write_text(f"(program (fn main () {expression}))")
            result = run("./build/v1/baseline-compiler", str(ast), str(assembly))
            self.assertEqual(result.returncode, 0, result.stderr)
            result = run("cc", "-shared", "-fPIC", str(assembly), "-o", str(library))
            self.assertEqual(result.returncode, 0, result.stderr)
            native = ctypes.CDLL(str(library)).bench_entry
            native.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint64]
            native.restype = ctypes.c_uint64
            rng = random.Random(2026)
            edges = [0, 1, 2, 63, 64, 65, (1 << 63) - 1, 1 << 63, MASK]
            pairs = [(a, b) for a in edges for b in edges]
            pairs += [(rng.getrandbits(64), rng.getrandbits(64)) for _ in range(100)]
            for a, b in pairs:
                expected = [a + b, a - b, a * b, a // b if b else 0,
                            a % b if b else 0, a & b, a | b, a ^ b,
                            a << (b & 63), a >> (b & 63), a == b, a != b,
                            a < b, a <= b, a > b, a >= b]
                for i, op in enumerate(operations):
                    xs = (ctypes.c_uint64 * 3)(i, a, b)
                    self.assertEqual(native(xs, 3), expected[i] & MASK, (op, a, b))
            xs = (ctypes.c_uint64 * 3)(99, 1, 2)
            self.assertEqual(native(xs, 3), 0)

    def test_failures_are_reported_and_score_is_neutral(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            manifest = directory / "suite.tsv"
            manifest.write_text(f"1 16 {ROOT / 'v1/benchmarks/sum.ast'}\n")
            for style, expected_error in [(0, "wrong result"), (1, "signal"),
                                           (2, "signal"), (3, "signal"),
                                           (4, "timeout"), (5, "status"),
                                           (6, "intentional compile failure"),
                                           (7, "timeout")]:
                with self.subTest(style=style):
                    compiler = directory / "compiler"
                    result = run("cc", "-O2", f"-DSTYLE={style}", "tests/fixture_compiler.c",
                                 "-o", str(compiler))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    output = directory / "results.json"
                    result = run("./build/v1/compbench", "--compiler", str(compiler),
                                 "--suite", str(manifest), "--output", str(output))
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    report = json.loads(output.read_text())
                    self.assertEqual(report["passed"], 0)
                    self.assertEqual(report["score"], 1.0)
                    self.assertIn(expected_error, report["tasks"][0]["error"])

    def test_invalid_ast_and_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            ast = directory / "invalid.ast"
            ast.write_text("(program (fn main () (call missing)))")
            manifest = directory / "suite.tsv"
            manifest.write_text("1 16 invalid.ast\n")
            result = run("./build/v1/compbench", "--compiler", "./build/v1/baseline-compiler",
                         "--suite", str(manifest))
            self.assertEqual(result.returncode, 1)
            self.assertIn("unknown function", result.stdout)
        result = run("./build/v1/compbench", "--compiler", "./build/v1/baseline-compiler", "--seed", "-1")
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
