"""Check failure-neutral scores in both C harnesses and Python report consumers."""

import copy
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import evaluate
from rescore_results import rescore_tree
from scoring import SCORE_POLICY, apply_score, geometric_score


class ScoringTests(unittest.TestCase):
    def test_failed_and_missing_tasks_still_count_in_denominator(self):
        report = {"total": 3, "passed": 1, "status": "fail", "score": None,
                  "tasks": [{"name": "fast", "status": "pass", "speedup": 8},
                            {"name": "wrong", "status": "fail", "speedup": 999}]}
        self.assertAlmostEqual(geometric_score(report), 2.0)
        self.assertAlmostEqual(geometric_score({"total": 3, "tasks": [
            {"status": "pass", "speedup": .125}, {"status": "fail"}]}), .5)

    def test_all_failures_and_compile_failures_score_one(self):
        for report in ({"total": 8, "tasks": [], "status": "compile_failure"},
                       {"total": 2, "tasks": [{"status": "fail"}, {"status": "fail"}]}):
            self.assertEqual(apply_score(report)["score"], 1.0)
            self.assertEqual(report["score_policy"], SCORE_POLICY)
        self.assertIsNone(geometric_score({"total": 0, "tasks": []}))

    def test_nested_rescoring_preserves_measurements_and_errors(self):
        data = {"variants": {"model": {"total": 2, "passed": 1, "score": None, "tasks": [
            {"name": "a", "status": "pass", "speedup": 4, "candidate_ns": 2},
            {"name": "b", "status": "fail", "error": "timeout"}]}}}
        tasks = copy.deepcopy(data["variants"]["model"]["tasks"])
        self.assertEqual(rescore_tree(data), 1)
        self.assertEqual(data["variants"]["model"]["score"], 2.0)
        self.assertEqual(data["variants"]["model"]["tasks"], tasks)

    def test_evaluator_scores_compiler_failure_as_one(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "res/v1/model.c"
            source.parent.mkdir(parents=True)
            source.write_text("invalid C")
            suite = root / "v1/benchmarks/suite.tsv"
            suite.parent.mkdir(parents=True)
            suite.write_text("1 16 a.ast\n1 16 b.ast\n")
            failure = subprocess.CompletedProcess([], 1, "", "intentional compilation failure")
            with patch.object(evaluate, "ROOT", root), patch.object(sys, "argv", ["evaluate", "--version", "v1"]), \
                    patch.object(evaluate.subprocess, "run", return_value=failure):
                self.assertEqual(evaluate.main(), 1)
            report = json.loads((root / "build/res/v1/model.json").read_text())
            self.assertEqual(report["status"], "compile_failure")
            self.assertEqual(report["score"], 1.0)
            self.assertEqual(report["total"], 2)

    @unittest.skipUnless(importlib.util.find_spec("matplotlib") and importlib.util.find_spec("numpy"),
                         "Optional chart dependencies are not installed")
    def test_graph_recalculates_old_null_score_with_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            report = {"total": 3, "passed": 1, "status": "fail", "score": None, "tasks": [
                {"name": "fast", "status": "pass", "speedup": 8},
                {"name": "wrong", "status": "fail"}, {"name": "timeout", "status": "fail"}]}
            (directory / "model.json").write_text(json.dumps(report))
            result = subprocess.run([sys.executable, str(ROOT / "scripts/render_results.py"),
                                     "--input-dir", str(directory), "--output-dir", str(directory / "plots"),
                                     "--formats", "svg", "--no-analysis"], cwd=ROOT,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            figure = (directory / "plots/model-comparison.svg").read_text()
            self.assertIn("2.00×", figure)
            self.assertNotIn("NO SCORE", figure)

    def test_both_harnesses_score_mixed_passes_and_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            compiler = directory / "compiler"
            subprocess.run(["cc", "-O2", str(ROOT / "tests/fixture_compiler.c"), "-o", str(compiler)],
                           check=True, capture_output=True, text=True, timeout=15)
            for name, value in (("pass", 0), ("fail_one", 1), ("fail_two", 2)):
                (directory / f"{name}.ast").write_text(f"(program (fn main () {value}))")
            suite = directory / "suite.tsv"
            suite.write_text("1 16 pass.ast\n1 16 fail_one.ast\n1 16 fail_two.ast\n")
            for version in ("v1", "v2"):
                with self.subTest(version=version):
                    output = directory / f"{version}.json"
                    result = subprocess.run([str(ROOT / "build" / version / "compbench"),
                                             "--compiler", str(compiler), "--suite", str(suite),
                                             "--output", str(output)], cwd=ROOT,
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    report = json.loads(output.read_text())
                    self.assertEqual((report["passed"], report["total"]), (1, 3))
                    expected = math.exp(math.log(report["tasks"][0]["speedup"]) / 3)
                    self.assertAlmostEqual(report["score"], expected, delta=1e-6)
                    self.assertEqual(report["score_policy"], SCORE_POLICY)
                    self.assertEqual(report["failure_speedup"], 1.0)
                    self.assertIn("failures=1x", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
