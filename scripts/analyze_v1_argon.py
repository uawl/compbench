#!/usr/bin/env python3
"""Compare Argon with exp-a/Astra max and isolate Argon's optimization passes."""

import hashlib
import json
from pathlib import Path
import subprocess

from analyze_v1 import assembly_stats, replace_once

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/analysis/v1-argon"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    paths = {
        "argon": ROOT / "res/v1/gemini-4-argon.c",
        "exp_a": ROOT / "res/v1/gemini-3.8-flash-exp-a.c",
        "astra_max": ROOT / "res/v1/openai-astra-max.c",
    }
    if not paths["exp_a"].exists():
        paths["exp_a"] = paths["exp_a"].with_suffix(".c.old")
    snapshots = {name: path.read_bytes() for name, path in paths.items()}
    originals = {name: source.decode() for name, source in snapshots.items()}
    argon = originals["argon"]
    no_simd = replace_once(argon, "analyze_loop(fn, fn_idx, &loop);",
                           "analyze_loop(fn, fn_idx, &loop);\n    loop.is_avx2 = 0;")
    variants = {
        **originals,
        "argon_no_simd": no_simd,
        "argon_no_fast_path": replace_once(argon, "if (loop.valid) {", "if (0) {"),
        "argon_no_recursion_conversion": replace_once(
            argon, "convert_linear_rec(prog, &prog->fn[i], i);", "(void)0;"),
        "argon_no_mutual_inline": replace_once(
            argon, "(caller_idx < cid && csize <= 40)", "(0)"),
    }
    suite = ROOT / "v1/benchmarks/suite.tsv"
    tasks = [line.split()[2] for line in suite.read_text().splitlines()
             if line and not line.startswith("#")]
    reports = {}
    # Evaluate sequentially; parallel timing workers would distort the comparison.
    for name, source in variants.items():
        directory = OUT / name
        directory.mkdir(exist_ok=True)
        code, compiler = directory / "solution.c", directory / "compiler"
        code.write_text(source)
        build = subprocess.run(
            ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Iv1/include",
             "v1/src/baseline.c", "v1/src/ast.c", str(code), "-o", str(compiler)],
            cwd=ROOT, text=True, capture_output=True, timeout=30)
        (directory / "compile.log").write_text(build.stdout + build.stderr)
        build.check_returncode()
        print(f"\n{name}", flush=True)
        report = directory / "results.json"
        run = subprocess.run(
            [str(ROOT / "build/v1/compbench"), "--compiler", str(compiler),
             "--suite", str(suite), "--output", str(report)], cwd=ROOT, timeout=300)
        data = json.loads(report.read_text())
        data["exit_code"] = run.returncode
        data["assembly"] = {}
        for task in tasks:
            assembly = directory / task.replace(".ast", ".s")
            subprocess.run([str(compiler), str(ROOT / "v1/benchmarks" / task), str(assembly)],
                           cwd=ROOT, check=True, timeout=15)
            data["assembly"][task] = assembly_stats(assembly)
        reports[name] = data

    # Exercise folding separately from the eight performance workloads.
    probe_dir = OUT / "semantic_probes"
    probe_dir.mkdir(exist_ok=True)
    expressions = {
        "constant_add": "(add 2 3)",
        "constant_sub_wrap": "(sub 0 1)",
        "constant_mul_wrap": "(mul 18446744073709551615 2)",
        "constant_div_zero": "(div 123 0)",
        "constant_mod_zero": "(mod 123 0)",
        "constant_unsigned_gt": "(gt 18446744073709551615 1)",
        "constant_shift_mask": "(shl 1 65)",
        "constant_nested": "(xor (mul (add 2 3) 7) (mod 29 6))",
        "runtime_div_zero": "(div (get 0) 0)",
        "runtime_mod_zero": "(mod (get 0) 0)",
        "runtime_div_self": "(div (get 0) (get 0))",
        "let_constant": "(let x (add 2 3) (mul x x))",
    }
    for name, expression in expressions.items():
        (probe_dir / f"{name}.ast").write_text(f"(program (fn main () {expression}))\n")
    probe_suite = probe_dir / "suite.tsv"
    probe_suite.write_text("".join(f"1 16 {name}.ast\n" for name in expressions))
    probe_report = probe_dir / "results.json"
    probe_run = subprocess.run(
        [str(ROOT / "build/v1/compbench"), "--compiler", str(OUT / "argon/compiler"),
         "--suite", str(probe_suite), "--output", str(probe_report)], cwd=ROOT, timeout=120)
    probes = json.loads(probe_report.read_text())
    probes["exit_code"] = probe_run.returncode

    rows = {name: {task["name"]: task for task in reports[name]["tasks"]} for name in originals}
    comparison = []
    for task in tasks:
        times = {name: rows[name][task].get("candidate_ns") for name in originals}
        a = times["argon"]
        comparison.append({"task": task, **{name + "_ns": ns for name, ns in times.items()},
                           "argon_vs_exp_a": times["exp_a"] / a if a and times["exp_a"] else None,
                           "argon_vs_astra_max": times["astra_max"] / a if a and times["astra_max"] else None})
    # A submission may be renamed between .c and .c.old during a long evaluation.
    for name, path in paths.items():
        if not path.exists() and path.suffix == ".old":
            paths[name] = path.with_suffix("")
    unchanged = all(paths[name].read_bytes() == source for name, source in snapshots.items())
    result = {
        "source_sha256": {str(paths[name].relative_to(ROOT)): hashlib.sha256(source).hexdigest()
                          for name, source in snapshots.items()},
        "variants": reports,
        "comparison": comparison,
        "semantic_probes": probes,
        "original_sources_unchanged": unchanged,
        "note": "Current harness, scalar v1 baseline; optimization switches changed only in copies. "
                "No-fast-path retains the original register/frame allocation. "
                "No-recursion-conversion disables hidden accumulators in both sum and Fibonacci.",
    }
    assert unchanged
    (OUT / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Saved {OUT.relative_to(ROOT)}/summary.json", flush=True)


if __name__ == "__main__":
    main()
