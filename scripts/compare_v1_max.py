#!/usr/bin/env python3
"""Compare Astra max and exp-a, with controlled variants kept outside res/."""

import hashlib
import json
from pathlib import Path
import subprocess

from analyze_v1 import assembly_stats, replace_once

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/analysis/v1-max"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    paths = {"astra_max": ROOT / "res/v1/openai-astra-max.c",
             "gemini": ROOT / "res/v1/gemini-3.8-flash-exp-a.c"}
    snapshots = {name: path.read_bytes() for name, path in paths.items()}
    astra = snapshots["astra_max"].decode()
    gemini = snapshots["gemini"].decode()
    variants = {
        "astra_max": astra,
        "gemini": gemini,
        "astra_max_no_simd": replace_once(astra, "if (s.loop.valid) cg_vector_loop(&s);", "if (0) cg_vector_loop(&s);"),
        "astra_max_no_entry_inline": replace_once(astra, "if (!(active & bit) && s->cost[e->index] <= 28 &&",
            "if (s->fid != s->p->entry && !(active & bit) && s->cost[e->index] <= 28 &&"),
        "gemini_no_scalar_unroll": replace_once(gemini, "try_emit_unrolled_scalar_loop(prog, &s);", "(void)0;"),
        "gemini_no_right_spine": replace_once(gemini,
            "if (detect_right_spine_recursion(s.fn, fn_idx, &s.right_spine_op, &rs_ident))", "if (0)"),
    }
    suite = ROOT / "v1/benchmarks/suite-original.tsv"
    tasks = [line.split()[2] for line in suite.read_text().splitlines() if line and not line.startswith("#")]
    reports = {}
    for name, source in variants.items():
        directory = OUT / name
        directory.mkdir(exist_ok=True)
        code, compiler = directory / "solution.c", directory / "compiler"
        code.write_text(source)
        build = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Iv1/include",
                                "v1/src/baseline.c", "v1/src/ast.c", str(code), "-o", str(compiler)],
                               cwd=ROOT, text=True, capture_output=True, timeout=30, check=True)
        (directory / "compile.log").write_text(build.stdout + build.stderr)
        print(f"\n{name}", flush=True)
        report = directory / "results.json"
        run = subprocess.run([str(ROOT / "build/v1/compbench"), "--compiler", str(compiler),
                              "--suite", str(suite), "--output", str(report)], cwd=ROOT, timeout=300)
        data = json.loads(report.read_text())
        data["exit_code"] = run.returncode
        data["assembly"] = {}
        for task in tasks:
            assembly = directory / task.replace(".ast", ".s")
            subprocess.run([str(compiler), str(ROOT / "v1/benchmarks" / task), str(assembly)],
                           check=True, timeout=15)
            data["assembly"][task] = assembly_stats(assembly)
        reports[name] = data
    result = {"source_sha256": {str(paths[name].relative_to(ROOT)): hashlib.sha256(source).hexdigest()
                                for name, source in snapshots.items()}, "variants": reports}
    original_rows = {name: {t["name"]: t for t in reports[name]["tasks"]} for name in ("astra_max", "gemini")}
    result["comparison"] = []
    for task in tasks:
        a = original_rows["astra_max"][task].get("candidate_ns")
        b = original_rows["gemini"][task].get("candidate_ns")
        result["comparison"].append({"task": task, "astra_max_ns": a, "gemini_ns": b,
                                     "gemini_vs_astra_max": a / b if a and b else None})
    result["original_sources_unchanged"] = all(paths[name].read_bytes() == source for name, source in snapshots.items())
    assert result["original_sources_unchanged"]
    (OUT / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Saved {OUT.relative_to(ROOT)}/summary.json")


if __name__ == "__main__":
    main()
