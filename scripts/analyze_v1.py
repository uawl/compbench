#!/usr/bin/env python3
"""Compare v1 codegen variants without editing the model submissions."""

import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/analysis/v1"


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one occurrence: {old}")
    return text.replace(old, new)


def assembly_stats(path):
    text = path.read_text()
    # Inspect only functions reachable from bench_entry, including internal tail jumps.
    matches = list(re.finditer(r"^\.(?:Lfn|Lcb_fn|Lcbfn)(\d+):\s*$", text, re.M))
    functions = {int(m.group(1)): text[m.end():matches[i + 1].start() if i + 1 < len(matches) else len(text)]
                 for i, m in enumerate(matches)}
    entry = text[:matches[0].start()] if matches else text
    work = [int(n) for n in re.findall(r"\b(?:call|jmp)\s+\.(?:Lfn|Lcb_fn|Lcbfn)(\d+)\b", entry)]
    reachable = set()
    while work:
        n = work.pop()
        if n in reachable or n not in functions:
            continue
        reachable.add(n)
        work += [int(m) for m in re.findall(r"\b(?:call|jmp)\s+\.(?:Lfn|Lcb_fn|Lcbfn)(\d+)\b", functions[n])]
    active = entry + "\n".join(functions[n] for n in sorted(reachable))
    return {
        "reachable_functions": sorted(reachable),
        "ymm_instructions": len(re.findall(r"^\s+\w+[^\n]*%ymm", active, re.M)),
        "call_instructions": len(re.findall(r"^\s+call\b", active, re.M)),
        "internal_tail_jumps": len(re.findall(r"^\s+jmp\s+\.(?:Lfn|Lcb_fn|Lcbfn)\d+\b", active, re.M)),
    }


def run():
    OUT.mkdir(parents=True, exist_ok=True)
    gemini_path = ROOT / "res/v1/gemini-3.8-flash-exp-a.c"
    astra_path = ROOT / "res/v1/openai-astra-high.c"
    gemini = gemini_path.read_text()
    astra = astra_path.read_text()
    no_simd = replace_once(gemini, "if (!emit_avx2_fastpath(prog, &s))", "if (1)")
    scalar = replace_once(no_simd, "try_emit_unrolled_scalar_loop(prog, &s);", "(void)0;")
    no_recursion_passes = replace_once(scalar, "convert_linear_rec_to_loop(prog);", "(void)0;")
    no_recursion_passes = replace_once(no_recursion_passes,
        "if (detect_right_spine_recursion(s.fn, fn_idx, &s.right_spine_op, &rs_ident))", "if (0)")
    no_mutual = replace_once(gemini,
        "inline_calls_in_expr(prog, i, prog->fn[i].body, round >= 3, &changed)",
        "inline_calls_in_expr(prog, i, prog->fn[i].body, 0, &changed)")
    variants = {
        "gemini_full": gemini,
        "gemini_no_simd": no_simd,
        "gemini_scalar": scalar,
        "gemini_no_recursion_passes": no_recursion_passes,
        "gemini_no_mutual_inline": no_mutual,
        "astra": astra,
    }
    suite = ROOT / "v1/benchmarks/suite-original.tsv"
    tasks = [line.split()[2] for line in suite.read_text().splitlines() if line and not line.startswith("#")]
    reports = {}
    # Run timings sequentially to avoid interference between our own benchmark workers.
    for name, source in variants.items():
        directory = OUT / name
        directory.mkdir(exist_ok=True)
        code = directory / "solution.c"
        code.write_text(source)
        compiler = directory / "compiler"
        build = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Iv1/include",
                                "v1/src/baseline.c", "v1/src/ast.c", str(code), "-o", str(compiler)],
                               cwd=ROOT, text=True, capture_output=True, timeout=30)
        (directory / "compile.log").write_text(build.stdout + build.stderr)
        build.check_returncode()
        print(f"\n{name}", flush=True)
        report = directory / "results.json"
        evaluation = subprocess.run([str(ROOT / "build/v1/compbench"), "--compiler", str(compiler),
                                     "--suite", str(suite), "--output", str(report)], cwd=ROOT, timeout=300)
        data = json.loads(report.read_text())
        data["exit_code"] = evaluation.returncode
        data["assembly"] = {}
        for task in tasks:
            assembly = directory / task.replace(".ast", ".s")
            subprocess.run([str(compiler), str(ROOT / "v1/benchmarks" / task), str(assembly)],
                           cwd=ROOT, check=True, timeout=15)
            data["assembly"][task] = assembly_stats(assembly)
        reports[name] = data
    # Independent semantic probe for the constant-folding issue found during source review.
    probe_dir = OUT / "constant_fold_probe"
    probe_dir.mkdir(exist_ok=True)
    (probe_dir / "add.ast").write_text("(program (fn main () (add 2 3)))\n")
    (probe_dir / "suite.tsv").write_text("1 16 add.ast\n")
    probes = {}
    for name in ("gemini_full", "astra"):
        report = probe_dir / f"{name}.json"
        result = subprocess.run([str(ROOT / "build/v1/compbench"), "--compiler", str(OUT / name / "compiler"),
                                 "--suite", str(probe_dir / "suite.tsv"), "--output", str(report)],
                                cwd=ROOT, timeout=60)
        probes[name] = {"exit_code": result.returncode, **json.loads(report.read_text())}
    summary = {
        "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in (gemini_path, astra_path)},
        "variants": reports,
        "constant_fold_probe": probes,
        "note": "Variants change optimization switches only; original submissions are untouched."
    }
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"\nSaved {OUT.relative_to(ROOT)}/summary.json", flush=True)


if __name__ == "__main__":
    run()
