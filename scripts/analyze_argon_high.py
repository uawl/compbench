#!/usr/bin/env python3
"""Inspect Argon high and ablate passes on the original and supplemental v1 tasks."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from analyze_v1 import assembly_stats, replace_once

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/analysis/argon-high"
SOURCE = ROOT / "res/v1/gemini-4-argon-high.c"
ACCUMULATORS = ["none", "add", "xor", "or", "and", "mul", "max", "min", "horner", "shift_xor"]
VARIANTS = ("full", "no_simd", "no_trmc", "no_inline", "no_cse")


def build(directory, source):
    directory.mkdir(parents=True, exist_ok=True)
    code, compiler = directory / "solution.c", directory / "compiler"
    code.write_text(source)
    result = subprocess.run(
        ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Iv1/include",
         "v1/src/baseline.c", "v1/src/ast.c", str(code), "-o", str(compiler)],
        cwd=ROOT, capture_output=True, text=True, timeout=30)
    (directory / "compile.log").write_text(result.stdout + result.stderr)
    result.check_returncode()
    return compiler


def evaluate(compiler, suite, report):
    result = subprocess.run(
        [str(ROOT / "build/v1/compbench"), "--compiler", str(compiler),
         "--suite", str(suite), "--output", str(report)], cwd=ROOT, timeout=300)
    data = json.loads(report.read_text())
    data["exit_code"] = result.returncode
    return data


def inspect(compiler, task, directory):
    relative = task.relative_to(ROOT / "v1/benchmarks")
    asm = directory / "assembly" / relative.with_suffix(".s")
    asm.parent.mkdir(parents=True, exist_ok=True)
    generated = subprocess.run([str(compiler), str(task), str(asm)], cwd=ROOT,
                               capture_output=True, text=True, check=True, timeout=15)
    asm.with_suffix(".trace.log").write_text(generated.stderr)
    stats = assembly_stats(asm)
    text = asm.read_text()
    matches = list(re.finditer(r"^\.Lfn(\d+):\s*$", text, re.M))
    functions = {int(m.group(1)): text[m.end():matches[i + 1].start() if i + 1 < len(matches) else len(text)]
                 for i, m in enumerate(matches)}
    active = (text[:matches[0].start()] if matches else text) + "\n".join(
        functions[i] for i in stats["reachable_functions"])
    stats["instructions"] = {op: len(re.findall(r"^\s+" + op + r"\b", active, re.M))
                             for op in ("divq", "mulq", "imulq", "vpmuludq", "vpsllq", "vpsrlq")}
    stats["vector_loops"] = []
    for fn, valid, kinds in re.findall(r"CB_LOOP fn=(\d+) valid=(\d+) acc=([\d,]*)", generated.stderr):
        if int(fn) in stats["reachable_functions"] and int(valid):
            stats["vector_loops"].append({"function": int(fn), "accumulators": [
                ACCUMULATORS[int(k)] for k in kinds.split(",") if k]})
    return asm, stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, default=ROOT / "v1/benchmarks/suite.tsv")
    parser.add_argument("--variants", nargs="+", choices=VARIANTS, default=list(VARIANTS))
    args = parser.parse_args()
    suite = args.suite.resolve()
    rows = [line.split() for line in suite.read_text().splitlines()
            if line.strip() and not line.lstrip().startswith("#")]
    tasks = [(row[2], (suite.parent / row[2]).resolve()) for row in rows]
    snapshot = SOURCE.read_bytes()
    source = snapshot.decode()
    OUT.mkdir(parents=True, exist_ok=True)
    variants = {
        "full": source,
        "no_simd": replace_once(source, "if (avx_info.valid) {", "if (0) {"),
        "no_trmc": replace_once(source, "trmc_pass(ir);", "(void)0;"),
        "no_cse": replace_once(source, "ir->fn[i].body = cse_walk(ir, &ir->fn[i], ir->fn[i].body);", "(void)0;"),
    }
    assert source.count("inline_pass(ir);") == 2
    variants["no_inline"] = source.replace("inline_pass(ir);", "(void)0;")
    trace_point = "    analyze_avx2_loop(p, fn, fn_idx, &avx_info);"
    trace = trace_point + '''
    fprintf(stderr, "CB_LOOP fn=%d valid=%d acc=", fn_idx, avx_info.valid);
    if (avx_info.valid) for (int a = 0; a < avx_info.num_accums; ++a)
        fprintf(stderr, "%d,", (int)avx_info.acc_kind[a]);
    fprintf(stderr, "\\n");'''
    # Keep an uninstrumented compiler to verify that tracing never changes assembly.
    original = build(OUT / "original", source)
    print("\nBaseline validation", flush=True)
    baseline = evaluate(ROOT / "build/v1/baseline-compiler", suite, OUT / "baseline.json")
    assert baseline["passed"] == baseline["total"] == len(tasks)
    reports = {}
    for name in args.variants:
        directory = OUT / name
        compiler = build(directory, replace_once(variants[name], trace_point, trace))
        print(f"\n{name}", flush=True)
        data = evaluate(compiler, suite, directory / "results.json")
        data["assembly"] = {}
        for label, task in tasks:
            asm, stats = inspect(compiler, task, directory)
            if name == "no_simd":
                stats["recognized_but_disabled"] = stats.pop("vector_loops")
                stats["vector_loops"] = []
            if name == "full":
                untraced, _ = inspect(original, task, OUT / "original")
                assert asm.read_bytes() == untraced.read_bytes(), label
                stats["tracing_preserves_assembly"] = True
            data["assembly"][label] = stats
        reports[name] = data
    result = {
        "submission": str(SOURCE.relative_to(ROOT)),
        "submission_sha256": hashlib.sha256(snapshot).hexdigest(),
        "suite": str(suite.relative_to(ROOT)),
        "suite_sha256": hashlib.sha256(suite.read_bytes()).hexdigest(),
        "ast_sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in tasks},
        "baseline": baseline,
        "variants": reports,
        "original_source_unchanged": SOURCE.read_bytes() == snapshot,
        "note": "Each failed task contributes 1.0 to the geometric mean over all tasks. "
                "Traces describe recognized accumulators; "
                "assembly statistics count only functions reachable from bench_entry. "
                "Optimization switches affect copies only; timings are sequential.",
    }
    assert result["original_source_unchanged"]
    (OUT / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Saved {OUT.relative_to(ROOT)}/summary.json", flush=True)


if __name__ == "__main__":
    main()
