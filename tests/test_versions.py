"""Check the language/ABI boundary between the two benchmark versions."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=45)


class VersionTests(unittest.TestCase):
    def test_prompts_match_their_fixed_sources(self):
        result = run("python3", "scripts/build_prompt.py", "--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        v1 = (ROOT / "v1/prompts/compiler.md").read_text()
        v2 = (ROOT / "v2/prompts/compiler.md").read_text()
        self.assertNotIn("CbArena", v1)
        self.assertNotRegex(v1, r"\bCB_CONS\b")
        self.assertIn("bench_entry(const uint64_t *xs, uint64_t n)", v1)
        self.assertRegex(v2, r"\bCB_CONS\b")
        self.assertIn("CbArena *arena", v2)

    def test_v1_rejects_list_program_accepted_by_v2(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            ast = directory / "list.ast"
            ast.write_text("(program (fn main () (head (cons 7 nil))))\n")
            suite = directory / "suite.tsv"
            suite.write_text("1 16 list.ast\n")
            for version, expected_exit in (("v1", 1), ("v2", 0)):
                compiler = f"./build/{version}/baseline-compiler"
                generated = run(compiler, str(ast), str(directory / f"{version}.s"))
                self.assertEqual(generated.returncode, expected_exit, generated.stderr)
                report = directory / f"{version}.json"
                result = run(f"./build/{version}/compbench", "--compiler", compiler,
                             "--suite", str(suite), "--output", str(report))
                self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
                data = json.loads(report.read_text())
                self.assertEqual(data["benchmark_version"], version)
                self.assertEqual(data["entry_abi"], "u64(xs,n)" if version == "v1" else "u64(xs,n,arena)")
                self.assertEqual(data["baseline"], f"{version}/src/codegen.c")
                if version == "v1":
                    self.assertEqual(data["score"], 1.0)
                    self.assertIn("unknown operation", data["tasks"][0]["error"])
                else:
                    self.assertEqual(data["passed"], 1)

    @unittest.skipUnless(importlib.util.find_spec("matplotlib") and importlib.util.find_spec("numpy"),
                         "Optional chart dependencies are not installed")
    def test_graph_rejects_different_versions_with_same_tasks(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for version in ("v1", "v2"):
                # Keep every other field identical to test the version check itself.
                data = {"benchmark_version": version, "passed": 1, "total": 1, "score": 2,
                        "tasks": [{"name": "sum.ast", "status": "pass", "speedup": 2}]}
                (directory / f"{version}.json").write_text(json.dumps(data))
            result = run("python3", "scripts/render_results.py", "--input-dir", str(directory),
                         "--output-dir", str(directory / "figures"), "--no-analysis")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("different benchmark_version", result.stderr)

    @unittest.skipUnless(importlib.util.find_spec("matplotlib") and importlib.util.find_spec("numpy"),
                         "Optional chart dependencies are not installed")
    def test_graph_rejects_different_suite_sizes_without_task_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for total in (8, 26):
                data = {"benchmark_version": "v1", "passed": 0, "total": total,
                        "status": "compile_failure", "score": 1, "tasks": []}
                (directory / f"model_{total}.json").write_text(json.dumps(data))
            result = run("python3", "scripts/render_results.py", "--input-dir", str(directory),
                         "--output-dir", str(directory / "figures"), "--no-analysis")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("different total", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
