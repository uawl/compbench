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


class ListNode(ctypes.Structure):
    pass


ListNode._fields_ = [("head", ctypes.c_uint64), ("tail", ctypes.POINTER(ListNode))]


class Arena(ctypes.Structure):
    _fields_ = [("nodes", ctypes.POINTER(ListNode)), ("used", ctypes.c_uint64),
                ("capacity", ctypes.c_uint64), ("input", ctypes.POINTER(ListNode)),
                ("failed", ctypes.c_uint64)]


def run(*args, **kwargs):
    return subprocess.run(args, cwd=ROOT, text=True, capture_output=True,
                          timeout=45, **kwargs)


class HarnessTests(unittest.TestCase):
    def test_baseline_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "results.json"
            result = run("./build/v2/compbench", "--compiler", "./build/v2/baseline-compiler",
                         "--output", str(output), "--seed", "12345")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(output.read_text())
            self.assertEqual(report["passed"], 14)
            self.assertEqual(report["seed"], 12345)
            self.assertEqual(report["benchmark_version"], "v2")
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
            result = run("./build/v2/baseline-compiler", str(ast), str(assembly))
            self.assertEqual(result.returncode, 0, result.stderr)
            result = run("cc", "-shared", "-fPIC", str(assembly), "-o", str(library))
            self.assertEqual(result.returncode, 0, result.stderr)
            native = ctypes.CDLL(str(library)).bench_entry
            native.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint64, ctypes.POINTER(Arena)]
            native.restype = ctypes.c_uint64
            arena = Arena()
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
                    self.assertEqual(native(xs, 3, ctypes.byref(arena)), expected[i] & MASK, (op, a, b))
            xs = (ctypes.c_uint64 * 3)(99, 1, 2)
            self.assertEqual(native(xs, 3, ctypes.byref(arena)), 0)

    def test_lists_against_independent_oracle(self):
        rng = random.Random(123)
        programs = {
            "list_sum": lambda xs: sum(xs) & MASK,
            "list_reverse": lambda xs: xs[::-1],
            "list_map": lambda xs: [(x * 3 + 1) & MASK for x in xs],
            "list_sharing": lambda xs: (sum(xs) & MASK) ^ (sum(x ^ (1 << 63) for x in xs) & MASK),
            "list_zip": lambda xs: [((x ^ (xs[i + 1] if i + 1 < len(xs) else 0)) + i) & MASK
                                      for i, x in enumerate(xs)],
            "list_map_recursive": lambda xs: [x // (x & 7) if x & 7 else 0 for x in xs],
        }
        special = {
            "identity": ("input", lambda xs: xs),
            "empty": ("(tail nil)", lambda xs: []),
            "alias": ("(let xs (cons 7 input) (let ys (cons 8 xs) (add (head xs) (head ys))))", lambda xs: 15),
            "prepend": ("(cons 7 input)", lambda xs: [7] + xs),
        }
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            for name, oracle in list(programs.items()) + [(n, spec[1]) for n, spec in special.items()]:
                with self.subTest(program=name):
                    ast = ROOT / "v2/benchmarks" / f"{name}.ast"
                    if name in special:
                        ast = directory / f"{name}.ast"
                        ast.write_text(f"(program (fn main () {special[name][0]}))")
                    assembly = directory / f"{name}.s"
                    library = directory / f"{name}.so"
                    result = run("./build/v2/baseline-compiler", str(ast), str(assembly))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = run("cc", "-shared", "-fPIC", str(assembly), "-o", str(library))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    native = ctypes.CDLL(str(library)).bench_entry
                    native.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint64, ctypes.POINTER(Arena)]
                    native.restype = ctypes.c_uint64
                    lengths = [0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32]
                    if name != "list_map_recursive": lengths += [129, 4096]
                    for n in lengths:
                        values = [rng.getrandbits(64) if i % 3 else MASK for i in range(n)]
                        xs = (ctypes.c_uint64 * max(1, n))(*values)
                        cells = (ListNode * max(1, n))()
                        for i, x in enumerate(values):
                            cells[i].head = x
                            if i + 1 < n: cells[i].tail = ctypes.pointer(cells[i + 1])
                        nodes = (ListNode * max(8, 2 * n + 4))()
                        arena = Arena(nodes=nodes, capacity=len(nodes),
                                      input=ctypes.pointer(cells[0]) if n else None)
                        value = native(xs, n, ctypes.byref(arena))
                        self.assertEqual(arena.failed, 0)
                        expected = oracle(values)
                        if isinstance(expected, list):
                            actual = []
                            cell = ctypes.cast(value, ctypes.POINTER(ListNode))
                            while cell and len(actual) <= len(expected):
                                actual.append(cell.contents.head)
                                cell = cell.contents.tail
                            self.assertFalse(cell, "cyclic or long native list")
                            self.assertEqual(actual, expected, (name, n))
                        else:
                            self.assertEqual(value, expected, (name, n))
                        self.assertEqual([cells[i].head for i in range(n)], values)
                        self.assertEqual(list(xs)[:n], values)
                        if name == "list_reverse" and n == 2:
                            arena.used = 0; arena.failed = 0; arena.capacity = 1
                            native(xs, n, ctypes.byref(arena))
                            self.assertEqual(arena.failed, 1)
                            self.assertEqual(arena.used, 1)

    def test_invalid_list_results_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            ast = directory / "list.ast"
            ast.write_text("(program (fn main () (cons 1 nil)))")
            manifest = directory / "suite.tsv"
            manifest.write_text("1 16 list.ast\n")
            for style, expected_error in [(8, "cyclic"), (9, "invalid"), (10, "short"),
                                           (11, "signal"), (12, "capacity"), (13, "wrong list element")]:
                with self.subTest(style=style):
                    compiler = directory / "compiler"
                    result = run("cc", "-O2", f"-DSTYLE={style}", "tests/fixture_compiler.c", "-o", str(compiler))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    output = directory / "result.json"
                    result = run("./build/v2/compbench", "--compiler", str(compiler), "--suite", str(manifest), "--output", str(output))
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    report = json.loads(output.read_text())
                    self.assertIsNone(report["score"])
                    self.assertIn(expected_error, report["tasks"][0]["error"])

    def test_failures_are_reported_and_score_is_withheld(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            manifest = directory / "suite.tsv"
            manifest.write_text(f"1 16 {ROOT / 'v2/benchmarks/sum.ast'}\n")
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
                    result = run("./build/v2/compbench", "--compiler", str(compiler),
                                 "--suite", str(manifest), "--output", str(output))
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    report = json.loads(output.read_text())
                    self.assertEqual(report["passed"], 0)
                    self.assertIsNone(report["score"])
                    self.assertIn(expected_error, report["tasks"][0]["error"])

    def test_invalid_ast_and_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            ast = directory / "invalid.ast"
            ast.write_text("(program (fn main () (call missing)))")
            manifest = directory / "suite.tsv"
            manifest.write_text("1 16 invalid.ast\n")
            result = run("./build/v2/compbench", "--compiler", "./build/v2/baseline-compiler",
                         "--suite", str(manifest))
            self.assertEqual(result.returncode, 1)
            self.assertIn("unknown function", result.stdout)
        result = run("./build/v2/compbench", "--compiler", "./build/v2/baseline-compiler", "--seed", "-1")
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
