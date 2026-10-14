#!/usr/bin/env python3
"""Recalculate existing scores from saved task speedups without rerunning timings."""

import argparse
import copy
import json
from pathlib import Path
import shutil

from scoring import apply_score

ROOT = Path(__file__).resolve().parents[1]


def rescore_tree(value):
    count = 0
    if isinstance(value, dict):
        for child in value.values():
            count += rescore_tree(child)
        if isinstance(value.get("tasks"), list) and isinstance(value.get("total"), int):
            apply_score(value)
            count += 1
        note = value.get("note")
        if isinstance(note, str) and note.startswith("Failed variants have no aggregate score."):
            value["note"] = note.replace("Failed variants have no aggregate score.",
                                        "Each failed task contributes 1.0 to the geometric mean over all tasks.", 1)
    elif isinstance(value, list):
        for child in value:
            count += rescore_tree(child)
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path,
                        help="JSON files/directories; default: build/res and build/analysis")
    args = parser.parse_args()
    paths = args.paths or [ROOT / "build/res", ROOT / "build/analysis"]
    files = set()
    for path in paths:
        files.update(path.rglob("*.json") if path.is_dir() else [path])
    changed, reports = 0, 0
    for path in sorted(files):
        path = path.resolve()
        if "archive" in path.parts or not path.exists():
            continue
        original = json.loads(path.read_text())
        data = copy.deepcopy(original)
        count = rescore_tree(data)
        if not count or data == original:
            continue
        relative = path.relative_to(ROOT)
        backup = ROOT / "build/archive/pre-failure-scoring" / relative
        backup.parent.mkdir(parents=True, exist_ok=True)
        if not backup.exists():
            shutil.copy2(path, backup)
        path.write_text(json.dumps(data, indent=2) + "\n")
        changed += 1
        reports += count
    print(f"Updated {reports} reports in {changed} JSON files; timings and failure statuses retained.")


if __name__ == "__main__":
    main()
