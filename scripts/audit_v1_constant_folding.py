#!/usr/bin/env python3
"""Trace the bad constant fold and compare assembly after fixing it in copies."""

import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ANALYSIS = ROOT / "build/analysis/v1"
OUT = ANALYSIS / "constant_fold_audit"
BAD = """        e->op = CB_CONST;
        e->value = eval_const_binop(e->op, a->value, b->value);"""
FIXED = """        e->value = eval_const_binop(e->op, a->value, b->value);
        e->op = CB_CONST;"""
TRACED = """        fprintf(stderr, "CB_FOLD op=%d a=%" PRIu64 " b=%" PRIu64 " expected=%" PRIu64 "\\n",
                (int)e->op, a->value, b->value,
                eval_const_binop(e->op, a->value, b->value));
        e->op = CB_CONST;
        e->value = eval_const_binop(e->op, a->value, b->value);"""


def command(args):
    return subprocess.run(args, cwd=ROOT, text=True, capture_output=True, timeout=60, check=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    summary = json.loads((ANALYSIS / "summary.json").read_text())
    tasks = [line.split()[2] for line in (ROOT / "v1/benchmarks/suite.tsv").read_text().splitlines()
             if line and not line.startswith("#")]
    result = {"variants": {}, "original_source_unchanged": False}
    original = ROOT / "res/v1/gemini-3.8-flash-exp-a.c"
    digest = hashlib.sha256(original.read_bytes()).hexdigest()
    assert digest == summary["source_sha256"][str(original.relative_to(ROOT))]
    for variant in summary["variants"]:
        if not variant.startswith("gemini_"):
            continue
        source = (ANALYSIS / variant / "solution.c").read_text()
        assert source.count(BAD) == 1
        directory = OUT / variant
        directory.mkdir(exist_ok=True)
        compilers = {}
        for mode, replacement in [("traced", TRACED), ("fixed", FIXED)]:
            code = directory / f"{mode}.c"
            code.write_text(source.replace(BAD, replacement))
            compiler = directory / mode
            build = command(["cc", "-std=c11", "-O2", "-Iv1/include", "v1/src/baseline.c",
                             "v1/src/ast.c", str(code), "-o", str(compiler)])
            (directory / f"{mode}.compile.log").write_text(build.stdout + build.stderr)
            compilers[mode] = compiler
        rows = []
        for task in tasks:
            original_assembly = (ANALYSIS / variant / task.replace(".ast", ".s")).read_bytes()
            trace = ""
            identical = {}
            for mode, compiler in compilers.items():
                assembly = directory / task.replace(".ast", f".{mode}.s")
                generation = command([str(compiler), str(ROOT / "v1/benchmarks" / task), str(assembly)])
                if mode == "traced":
                    trace = generation.stderr
                    (directory / task.replace(".ast", ".fold.log")).write_text(trace)
                identical[mode] = assembly.read_bytes() == original_assembly
            rows.append({"task": task, "fold_events": trace.count("CB_FOLD "),
                         "traced_assembly_identical": identical["traced"],
                         "fixed_assembly_identical": identical["fixed"]})
        result["variants"][variant] = rows
        print(variant, "fold events:", sum(r["fold_events"] for r in rows),
              "fixed assembly identical:", sum(r["fixed_assembly_identical"] for r in rows), "/", len(rows), flush=True)
    # Positive control: the instrumentation must see the known failing expression.
    probe = ANALYSIS / "constant_fold_probe/add.ast"
    probe_assembly = OUT / "control.s"
    control = command([str(OUT / "gemini_full/traced"), str(probe), str(probe_assembly)])
    (OUT / "control.fold.log").write_text(control.stderr)
    assert "CB_FOLD " in control.stderr and "expected=5" in control.stderr
    result["positive_control_trace"] = control.stderr
    report = OUT / "fixed_probe.json"
    verification = command([str(ROOT / "build/v1/compbench"), "--compiler", str(OUT / "gemini_full/fixed"),
                            "--suite", str(ANALYSIS / "constant_fold_probe/suite.tsv"), "--output", str(report)])
    print(verification.stdout, end="")
    result["fixed_probe"] = json.loads(report.read_text())
    result["source_sha256"] = digest
    result["original_source_unchanged"] = hashlib.sha256(original.read_bytes()).hexdigest() == digest
    assert result["original_source_unchanged"]
    (OUT / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Audit saved:", (OUT / "summary.json").relative_to(ROOT))


if __name__ == "__main__":
    main()
