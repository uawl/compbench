"""Validate new benchmark definitions against independent Python u64 oracles."""

import ctypes
import math
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "v1/benchmarks"
MASK = (1 << 64) - 1
NAMES = {
    "product", "unsigned_min", "horner", "horner_invariant", "shift_xor", "shift_xor_wide",
    "constant_div", "constant_mod", "variable_div", "variable_mod", "offset_window",
    "indexed_gather", "three_accumulators", "tail_rotation", "non_tail_product",
    "non_associative_sub", "helper_cse", "variable_shift",
}


def oracle(name, xs):
    if name in ("product", "non_tail_product"):
        return math.prod(x | 1 for x in xs) & MASK
    if name == "unsigned_min":
        return min(xs, default=MASK)
    if name in ("horner", "horner_invariant", "shift_xor", "shift_xor_wide"):
        acc = 17
        k = 31 if name == "horner" else ((xs[0] if xs else 0) | 1)
        for x in xs:
            if name.startswith("shift_xor"):
                acc = ((acc << (5 if name == "shift_xor" else 17)) ^ x) & MASK
            else:
                acc = (acc * k + x) & MASK
        return acc
    if name in ("constant_div", "constant_mod", "variable_div", "variable_mod"):
        result = 0
        for x in xs:
            d = 7 if name.startswith("constant") else x & 7
            result += ((x // d if name.endswith("div") else x % d) if d else 0)
        return result & MASK
    if name == "offset_window":
        return sum(x ^ (xs[i + 1] if i + 1 < len(xs) else 0) for i, x in enumerate(xs)) & MASK
    if name == "indexed_gather":
        return sum(xs[j] if j < len(xs) else 0 for j in ((i * 17) & 1023 for i in range(len(xs)))) & MASK
    if name == "three_accumulators":
        total, bits, prod = 0, 0, 1
        for x in xs:
            x ^= x >> 7
            total = (total + x) & MASK
            bits ^= x
            prod = (prod * (x | 1)) & MASK
        return total ^ ((bits + prod) & MASK)
    if name == "tail_rotation":
        a, b, c = 1, 2, 3
        for x in xs:
            a, b, c = b, c, (a + x) & MASK
        return a ^ ((b + c) & MASK)
    if name == "non_associative_sub":
        acc = 0
        for x in reversed(xs):
            acc = (x - acc) & MASK
        return acc
    if name == "helper_cse":
        return sum(2 * ((x ^ (x >> 13)) * 11400714819323198485) for x in xs) & MASK
    if name == "variable_shift":
        acc = 0
        for i, x in enumerate(xs):
            acc ^= ((x << ((i + 65) & 63)) & MASK) ^ (x >> (x & 63))
        return acc
    raise ValueError(name)


def manifest_rows(path):
    return [line.strip() for line in path.read_text().splitlines()
            if line.strip() and not line.lstrip().startswith("#")]


class OptimizationTests(unittest.TestCase):
    def test_regular_suite_contains_original_and_optimization_tasks_once(self):
        extra = manifest_rows(BENCH / "suite-optimizations.tsv")
        self.assertEqual({Path(line.split()[2]).stem for line in extra}, NAMES)
        original = manifest_rows(BENCH / "suite-original.tsv")
        self.assertEqual(len(original), 8)
        regular = manifest_rows(BENCH / "suite.tsv")
        self.assertEqual(regular, original + extra)
        self.assertEqual(len({line.split()[2] for line in regular}), 26)
        self.assertEqual(manifest_rows(BENCH / "suite-extended.tsv"), regular)

    def test_baseline_assembly_matches_independent_oracles(self):
        rng = random.Random(20261014)
        checks = 0
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for name in sorted(NAMES):
                asm, library = directory / f"{name}.s", directory / f"{name}.so"
                subprocess.run([str(ROOT / "build/v1/baseline-compiler"),
                                str(BENCH / "optimization" / f"{name}.ast"), str(asm)],
                               check=True, capture_output=True, text=True, timeout=15)
                subprocess.run(["cc", "-shared", "-fPIC", str(asm), "-o", str(library)],
                               check=True, capture_output=True, text=True, timeout=15)
                native = ctypes.CDLL(str(library)).bench_entry
                native.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint64]
                native.restype = ctypes.c_uint64
                for n in (0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 129):
                    if name.startswith("non_") and n > 32:
                        continue
                    patterns = [[0] * n, [MASK] * n, list(range(n)),
                                [((1 << 63), MASK, 0, 64, 65, 1, 7)[i % 7] for i in range(n)],
                                [rng.getrandbits(64) for _ in range(n)]]
                    for values in patterns:
                        xs = (ctypes.c_uint64 * max(n, 1))(*values)
                        self.assertEqual(native(xs, n), oracle(name, values), (name, n, values))
                        checks += 1
        print(f"Supplemental scalar benchmarks: {checks} independent oracle checks passed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
