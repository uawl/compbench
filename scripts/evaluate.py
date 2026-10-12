#!/usr/bin/env python3
"""Build submissions against their versioned AST, parser, harness and baseline."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main(version=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc", help="C compiler command used to build submissions")
    parser.add_argument("names", nargs="*", help="Optional submission filenames or stems")
    if version is None:
        parser.add_argument("--version", choices=("v1", "v2"), required=True)
    args = parser.parse_args()
    version = version or args.version
    directory = ROOT / version
    suite = directory / "benchmarks/suite.tsv"
    total = sum(bool(line.strip()) and not line.lstrip().startswith("#") for line in suite.read_text().splitlines())
    sources = sorted((ROOT / "res" / version).glob("*.c"))
    if args.names:
        requested = set(args.names)
        available = {s.name for s in sources} | {s.stem for s in sources}
        if requested - available:
            print(f"Unknown submissions: {', '.join(sorted(requested - available))}", file=sys.stderr)
            return 2
        sources = [s for s in sources if s.name in requested or s.stem in requested]
    if not sources:
        print(f"No C submissions in res/{version}", file=sys.stderr)
        return 2
    output = ROOT / "build/res" / version
    output.mkdir(parents=True, exist_ok=True)
    failed = 0
    for source in sources:
        executable = output / source.stem
        report_path = output / f"{source.stem}.json"
        log_path = output / f"{source.stem}.compile.log"
        provenance = {
            "submission": str(source.relative_to(ROOT)),
            "submission_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "benchmark_version": version,
            "entry_abi": "u64(xs,n)" if version == "v1" else "u64(xs,n,arena)",
            "suite": f"{version}/benchmarks/suite.tsv",
            "baseline": f"{version}/src/codegen.c",
            "harness": ("v1 scalar harness (two-argument ABI)" if version == "v1"
                        else "v2 list harness (three-argument ABI)"),
            "compile_log": str(log_path.relative_to(ROOT)),
        }

        def failure_report(status, message, returncode=None):
            report = {"schema_version": 2, "status": status, "passed": 0, "total": total,
                      "score": None, "tasks": [], "error": message, **provenance}
            if returncode is not None:
                report["compile_returncode"] = returncode
            report_path.write_text(json.dumps(report, indent=2) + "\n")
        print(f"\nEvaluating {source.relative_to(ROOT)}", flush=True)
        # A failed build must not leave an older report looking like a new result.
        report_path.unlink(missing_ok=True)
        executable.unlink(missing_ok=True)
        log_path.unlink(missing_ok=True)
        compiler = shlex.split(args.cc)
        if not compiler:
            print("Empty compiler command", file=sys.stderr)
            return 2
        try:
            build = subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", "-Wextra", f"-I{version}/include",
                                   f"{version}/src/baseline.c", f"{version}/src/ast.c", str(source), "-o", str(executable)],
                                   cwd=ROOT, timeout=30, check=False, capture_output=True,
                                   text=True, errors="replace")
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"Build failed: {error}", file=sys.stderr)
            log_path.write_text(str(error) + "\n")
            failure_report("compile_failure", str(error))
            failed += 1
            continue
        log_path.write_text(build.stdout + build.stderr)
        if build.returncode:
            failure_report("compile_failure", "C submission did not compile", build.returncode)
            print(f"Build failed; see {log_path.relative_to(ROOT)}", file=sys.stderr)
            failed += 1
            continue
        try:
            evaluation = subprocess.run([str(ROOT / "build" / version / "compbench"), "--compiler", str(executable),
                                         "--suite", str(suite.relative_to(ROOT)), "--output", str(report_path)],
                                        cwd=ROOT, timeout=300, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"Evaluation failed: {error}", file=sys.stderr)
            failure_report("evaluation_error", str(error))
            failed += 1
            continue
        produced_report = report_path.exists()
        if produced_report:
            report = json.loads(report_path.read_text())
            report.update(provenance)
            report["status"] = "pass" if evaluation.returncode == 0 else "fail"
            report_path.write_text(json.dumps(report, indent=2) + "\n")
        else:
            failure_report("evaluation_error", "Harness did not produce a report")
        if evaluation.returncode or not produced_report:
            failed += 1
    print(f"\nReports: {output.relative_to(ROOT)}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
