#!/usr/bin/env python3
"""Render benchmark reports and optional optimization comparisons to PNG/SVG/PDF."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
import textwrap

from scoring import geometric_score

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.colors import LinearSegmentedColormap, TwoSlopeNorm
    from matplotlib.ticker import FuncFormatter
    import numpy as np
except ImportError:
    sys.exit("Install plotting dependencies: python3 -m pip install -r requirements-viz.txt")

ROOT = Path(__file__).resolve().parents[1]
INK = "#1d3439"
MUTED = "#60777b"
PAPER = "#f6f7f2"
GRID = "#dce4df"
TEAL = "#168a7b"
PALETTE = [TEAL, "#7065b5", "#ce8553", "#477fad", "#b36d85", "#688c4c"]
TASK_LABELS = {
    "sum": "Sum", "unsigned_max": "Unsigned\nmax", "conditional_sum": "Conditional\nsum",
    "divmod": "Division /\nmodulo", "mutual_recursion": "Mutual\nrecursion",
    "helper_mix": "Helper\nmix", "non_tail_sum": "Non-tail\nsum", "fibonacci": "Fibonacci",
}
VARIANTS = {
    "gemini_full": "Gemini · full",
    "gemini_no_simd": "Gemini · vector path disabled",
    "gemini_scalar": "Gemini · vector + unroll disabled",
    "gemini_no_recursion_passes": "Gemini · also disable recursion transforms",
    "gemini_no_mutual_inline": "Gemini · mutual inlining disabled",
    "astra": "Astra · high",
    "argon": "Argon · full",
    "exp_a": "Gemini · exp-a",
    "astra_max": "Astra · max",
    "argon_no_simd": "Argon · vector path disabled",
    "argon_no_fast_path": "Argon · vector + unroll disabled",
    "argon_no_recursion_conversion": "Argon · accumulator conversion disabled",
    "argon_no_mutual_inline": "Argon · mutual inlining disabled",
}


def positive(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value > 0


def label(name):
    # The caller already removed the file extension; dots may be model versions.
    stem = name
    if stem.startswith("openai-astra-"):
        return "Astra · " + stem.removeprefix("openai-astra-")
    if stem.startswith("gemini-"):
        version, _, variant = stem.removeprefix("gemini-").partition("-")
        return "Gemini " + version + (" · " + variant.replace("-", " ").title() if variant else "")
    return stem.replace("_", " ").replace("-", " ")


def color(name):
    if name.startswith("gemini"):
        return TEAL
    if name == "openai-astra-high":
        return PALETTE[1]
    if name == "openai-astra-max":
        return PALETTE[2]
    return PALETTE[int(hashlib.sha256(name.encode()).hexdigest()[:8], 16) % len(PALETTE)]


def read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ValueError(f"Cannot read {path}: {error}") from error


def validate_report(data, path):
    if not isinstance(data, dict) or not isinstance(data.get("tasks"), list):
        raise ValueError(f"Not a benchmark report: {path}")
    names = []
    for task in data["tasks"]:
        if not isinstance(task, dict) or not isinstance(task.get("name"), str):
            raise ValueError(f"Invalid task in {path}")
        names.append(task["name"])
        if task.get("status") == "pass" and not positive(task.get("speedup")):
            raise ValueError(f"Invalid speedup for {task['name']} in {path}")
    if len(set(names)) != len(names):
        raise ValueError(f"Duplicate task names in {path}")


def score(data):
    return geometric_score(data)


def task_speedup(data, name):
    task = next((t for t in data["tasks"] if t["name"] == name), None)
    return task["speedup"] if task and task.get("status") == "pass" else None


def read_models(directory):
    models = []
    for path in sorted(directory.glob("*.json")):
        data = read_json(path)
        validate_report(data, path)
        models.append({"name": path.stem, "label": label(path.stem), "data": data, "path": path})
    if not models:
        raise ValueError(f"No benchmark JSON files in {directory}")
    for field in ("benchmark_version", "entry_abi"):
        values = {str(m["data"].get(field, "legacy")) for m in models}
        if len(values) > 1:
            raise ValueError(f"Reports have different {field} values; render them separately")
    for field in ("suite", "suite_sha256", "total", "baseline", "seed", "harness"):
        values = {str(m["data"][field]) for m in models if m["data"].get(field) is not None}
        if len(values) > 1:
            raise ValueError(f"Reports have different {field} values; render them separately")
    task_sets = {frozenset(t["name"] for t in m["data"]["tasks"])
                 for m in models if m["data"]["tasks"]}
    if len(task_sets) > 1:
        raise ValueError("Reports use different task sets; render them separately")
    models.sort(key=lambda m: (score(m["data"]) is None, -(score(m["data"]) or 0), m["name"]))
    tasks = next(([t["name"] for t in m["data"]["tasks"]] for m in models if m["data"]["tasks"]), [])
    return models, tasks


def clean_axis(ax):
    ax.set_facecolor("white")
    for spine in ax.spines.values():
        spine.set_visible(False)
    ax.tick_params(length=0, labelcolor=MUTED)
    ax.set_axisbelow(True)


def title(fig, headline, subtitle, eyebrow):
    fig.text(.065, .956, eyebrow, fontsize=11, color=TEAL, weight="bold")
    fig.text(.065, .908, headline, fontsize=28, weight="bold", color=INK)
    fig.text(.065, .870, subtitle, fontsize=11, color=MUTED)


def save(fig, stem, output, formats, dpi):
    paths = []
    for suffix in formats:
        path = output / f"{stem}.{suffix}"
        fig.savefig(path, dpi=dpi, facecolor=fig.get_facecolor(), metadata={"Creator": "compbench"})
        print(f"Saved {path}")
        paths.append(path)
    plt.close(fig)
    return paths


def diagnostic_note(models, analysis):
    if not analysis:
        return "Scores describe the evaluated suite; they do not prove correctness for every AST."
    hashes = analysis.get("source_sha256", {})
    gemini_hash = next((v for k, v in hashes.items() if "gemini" in k), None)
    probe = analysis.get("constant_fold_probe", {}).get("gemini_full", {})
    if (probe.get("passed", 1) < probe.get("total", 1) and gemini_hash
            and any(m["data"].get("submission_sha256") == gemini_hash for m in models)):
        return "Separate semantic diagnostic: Gemini fails constant folding. Scores above apply to the public suite only."
    return "Scores describe the evaluated suite; they do not prove correctness for every AST."


def draw_models(models, tasks, analysis, args):
    n = len(models)
    bands = max(1, math.ceil(len(tasks) / args.columns))
    columns = max(1, math.ceil(len(tasks) / bands))
    groups = [tasks[i:i + columns] for i in range(0, len(tasks), columns)] or [[]]
    width = max(15, columns * 1.25 + 3)
    overall_height = max(1.3, n * .4 + .25)
    heat_height = max(1., n * .42)
    heights = [overall_height] + [heat_height] * len(groups)
    header, footer, gap = 2.25, 1.6, 1.05
    height = header + sum(heights) + gap * len(groups) + footer
    fig = plt.figure(figsize=(width, height), facecolor=PAPER)
    total = max(m["data"].get("total", 0) for m in models)
    # Physical margins keep headings and footnotes readable as heatmap rows grow.
    fig.text(.065, 1 - .40 / height, "COMPBENCH   /   MODEL COMPARISON", fontsize=11, color=TEAL, weight="bold")
    fig.text(.065, 1 - .90 / height, args.title, fontsize=28, color=INK, weight="bold")
    fig.text(.065, 1 - 1.25 / height,
             f"{n} submissions  /  {total} tasks  /  speedup vs scalar baseline · higher is better",
             fontsize=11, color=MUTED)
    grid = fig.add_gridspec(len(heights), 1, left=.20, right=.94,
                           top=1 - header / height, bottom=footer / height,
                           height_ratios=heights, hspace=gap / (sum(heights) / len(heights)))
    ax = fig.add_subplot(grid[0])
    clean_axis(ax)
    values = [score(m["data"]) for m in models]
    largest = max([v for v in values if v is not None] + [1])
    ax.set_xlim(0, largest * 1.34)
    ax.set_ylim(-.55, n - .45)
    ax.invert_yaxis()
    ax.barh(np.arange(n), [v or 0 for v in values], height=.56,
            color=[color(m["name"]) for m in models], zorder=3)
    ax.set_yticks(np.arange(n), [m["label"] for m in models], fontsize=11, color=INK)
    ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:g}×"))
    ax.grid(axis="x", color=GRID, linewidth=.8)
    ax.axvline(1, color=MUTED, linestyle=(0, (3, 3)), linewidth=1.1, zorder=2)
    ax.set_title("Overall performance", loc="left", fontsize=16, weight="bold", color=INK, pad=25)
    ax.text(0, 1.03, "Geometric mean across all tasks · each failed task contributes 1.0×",
            transform=ax.transAxes, fontsize=10, color=MUTED)
    for i, (model, value) in enumerate(zip(models, values)):
        if value is None:
            state = model["data"].get("status", "fail").replace("_", " ").upper()
            ax.text(largest * .02, i, f"NO SCORE  /  {state}", va="center", color="#b2544b", fontsize=11, weight="bold")
        else:
            ax.text(value + largest * .025, i, f"{value:.2f}×", va="center", fontsize=15, weight="bold", color=INK)
        passed = model["data"].get("passed", 0)
        ax.text(1.012, i, f"{passed}/{model['data'].get('total', '?')} pass", transform=ax.get_yaxis_transform(),
                va="center", fontsize=10, color=MUTED)
    if tasks:
        matrix = np.array([[task_speedup(m["data"], t) or np.nan for t in tasks] for m in models], dtype=float)
        logs = np.ma.masked_invalid(np.log2(matrix))
        finite = logs.compressed()
        lower = min(-1., float(np.floor(finite.min()))) if len(finite) else -1.
        upper = max(1., float(np.ceil(finite.max()))) if len(finite) else 1.
        cmap = LinearSegmentedColormap.from_list("compbench", ["#c86c61", "#f0f3e9", "#146c61"])
        cmap.set_bad("#edf0ec")
        norm = TwoSlopeNorm(vmin=lower, vcenter=0, vmax=upper)
    offset = 0
    for band, group in enumerate(groups):
        heat = fig.add_subplot(grid[band + 1])
        clean_axis(heat)
        heading = "Where the speed comes from"
        if len(groups) > 1:
            heading += f"  ·  {offset + 1}–{offset + len(group)} / {len(tasks)}"
        heat.set_title(heading, loc="left", fontsize=16, weight="bold", color=INK, pad=22)
        if not group:
            heat.axis("off")
            heat.text(.5, .5, "No task timings available", transform=heat.transAxes, ha="center", color=MUTED)
            continue
        # Keep column widths aligned when the last row has fewer benchmarks.
        position = heat.get_position()
        heat.set_position([position.x0, position.y0, position.width * len(group) / columns, position.height])
        end = offset + len(group)
        rendered = heat.imshow(logs[:, offset:end], cmap=cmap, norm=norm, aspect="auto")
        heat.set_yticks(np.arange(n), [m["label"] for m in models], fontsize=10, color=INK)
        labels = [TASK_LABELS.get(Path(t).stem, textwrap.fill(Path(t).stem.replace("_", " "), 13)) for t in group]
        heat.set_xticks(np.arange(len(group)), labels, fontsize=10)
        heat.set_xticks(np.arange(-.5, len(group), 1), minor=True)
        heat.set_yticks(np.arange(-.5, n, 1), minor=True)
        heat.grid(which="minor", color="white", linewidth=3)
        heat.tick_params(which="minor", length=0)
        for i, model in enumerate(models):
            rows = {t["name"]: t for t in model["data"]["tasks"]}
            for j, task in enumerate(group):
                value = matrix[i, offset + j]
                if np.isfinite(value):
                    rgb = cmap(norm(math.log2(value)))[:3]
                    luminance = sum(v * w for v, w in zip(rgb, (.2126, .7152, .0722)))
                    heat.text(j, i, f"{value:.1f}×", ha="center", va="center", fontsize=12, weight="bold",
                              color="white" if luminance < .54 else INK)
                else:
                    heat.text(j, i, "FAIL\n(1×)" if task in rows else "—", ha="center", va="center", color=MUTED, fontsize=11)
        offset = end
    if tasks:
        bar_ax = fig.add_axes([.20, .90 / height, .42, .14 / height])
        powers = np.arange(lower, upper + 1)
        if len(powers) > 10:
            powers = powers[::2]
        bar = fig.colorbar(rendered, cax=bar_ax, orientation="horizontal", ticks=powers)
        bar.outline.set_visible(False)
        bar.ax.set_xticklabels([f"{2 ** p:g}×" for p in powers])
        bar.ax.tick_params(length=0, labelsize=9, colors=MUTED)
        fig.text(.65, .95 / height, "Log color scale  /  1× = baseline\n— not measured  ·  failed tasks score as 1×",
                 fontsize=9, color=MUTED, va="center")
    fig.text(.065, .36 / height, diagnostic_note(models, analysis), fontsize=9, color=MUTED)
    fig.text(.065, .15 / height, "Median of 7 samples · timings include call overhead · small differences may be measurement noise.",
             fontsize=8, color=MUTED)
    return save(fig, "model-comparison", args.output_dir, args.formats, args.dpi)


def draw_optimizations(analysis, args):
    if not isinstance(analysis.get("variants"), dict) or not analysis["variants"]:
        raise ValueError("Analysis JSON must contain a nonempty variants object")
    order = [name for name in VARIANTS if name in analysis["variants"]]
    order += [name for name in analysis["variants"] if name not in order]
    variants = [(name, analysis["variants"][name]) for name in order]
    for name, report in variants:
        validate_report(report, name)
    n = len(variants)
    tasks = [t["name"] for t in variants[0][1]["tasks"]]
    fig = plt.figure(figsize=(16, max(10, 6 + n * .8)), facecolor=PAPER)
    title(fig, "Beyond vectorization", "Controlled compiler variants · each change keeps the original submissions intact",
          "COMPBENCH   /   OPTIMIZATION ANALYSIS")
    grid = fig.add_gridspec(1, 2, left=.29, right=.94, top=.77, bottom=.20, width_ratios=[1, .92], wspace=.30)
    ax = fig.add_subplot(grid[0])
    clean_axis(ax)
    values = [score(report) for _, report in variants]
    largest = max([v for v in values if v is not None] + [1])
    colors = [TEAL if name in ("gemini_full", "argon") else PALETTE[1] if name in ("astra", "exp_a")
              else PALETTE[2] if name == "astra_max" else "#82b9aa" for name, _ in variants]
    ax.barh(np.arange(n), [v or 0 for v in values], height=.57, color=colors, zorder=3)
    ax.set_yticks(np.arange(n), [VARIANTS.get(name, label(name)) for name, _ in variants], fontsize=11, color=INK)
    ax.set_ylim(n - .45, -.55)
    ax.set_xlim(0, largest * 1.27)
    ax.grid(axis="x", color=GRID, linewidth=.8)
    ax.axvline(1, color=MUTED, linestyle=(0, (3, 3)), linewidth=1)
    ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:g}×"))
    ax.set_title("Overall speedup", loc="left", fontsize=16, color=INK, weight="bold", pad=22)
    for i, value in enumerate(values):
        ax.text((value or 0) + largest * .025, i, f"{value:.2f}×" if value else "NO SCORE", va="center",
                fontsize=13, weight="bold", color=INK)
    detail = fig.add_subplot(grid[1])
    clean_axis(detail)
    key_names = [name for name in ("gemini_full", "gemini_no_simd", "gemini_scalar", "astra") if name in analysis["variants"]]
    key_colors = {"gemini_full": TEAL, "gemini_no_simd": "#cc9867", "gemini_scalar": "#7c9b9b", "astra": PALETTE[1]}
    short = {"gemini_full": "Full", "gemini_no_simd": "No vector path", "gemini_scalar": "No vector / unroll", "astra": "Astra high"}
    if "argon" in analysis["variants"]:
        key_names = [name for name in ("argon", "argon_no_simd", "argon_no_fast_path", "exp_a")
                     if name in analysis["variants"]]
        key_colors = {"argon": TEAL, "argon_no_simd": "#cc9867",
                      "argon_no_fast_path": "#7c9b9b", "exp_a": PALETTE[1]}
        short = {"argon": "Argon full", "argon_no_simd": "Argon no vector",
                 "argon_no_fast_path": "Argon no vector / unroll", "exp_a": "Gemini exp-a"}
    successful_names = [name for name in key_names if all(task_speedup(analysis["variants"][name], task) for task in tasks)]
    x = np.arange(len(tasks))
    for name in successful_names:
        values = [task_speedup(analysis["variants"][name], task) for task in tasks]
        detail.plot(x, values, marker="o", markersize=5, linewidth=2, color=key_colors[name], label=short[name])
    detail.set_yscale("log", base=2)
    detail.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}×"))
    detail.grid(axis="y", color=GRID, linewidth=.8)
    detail.axhline(1, color=MUTED, linestyle=(0, (3, 3)), linewidth=1)
    detail.set_xticks(x, [Path(t).stem.replace("_", " ") for t in tasks], rotation=55, ha="right", fontsize=9)
    detail.set_title("Per-task profile", loc="left", fontsize=16, color=INK, weight="bold", pad=22)
    detail.legend(loc="upper right", frameon=False, fontsize=9)
    fig.text(.065, .105, "Each failed task scores as 1×. Variant gains are independent and not additive.", fontsize=10, color=MUTED)
    probe = analysis.get("constant_fold_probe", {}).get("gemini_full", {})
    if probe.get("passed", 1) < probe.get("total", 1):
        fig.text(.065, .068, "Accuracy note: Gemini fails the separate constant-folding probe (2 + 3 → 0).", fontsize=10, color="#a8564e")
    fig.text(.065, .036, "Same scalar suite and baseline · median of 7 samples · analysis timings may come from a different run than model comparison.",
             fontsize=9, color=MUTED)
    return save(fig, "optimization-analysis", args.output_dir, args.formats, args.dpi)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", choices=("v1", "v2"), default="v1")
    parser.add_argument("--input-dir", type=Path)
    parser.add_argument("--analysis", type=Path)
    parser.add_argument("--no-analysis", action="store_true", help="Render model reports only")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--formats", nargs="+", choices=("png", "svg", "pdf"), default=["png", "svg"])
    parser.add_argument("--dpi", type=int, default=180)
    parser.add_argument("--title")
    parser.add_argument("--columns", type=int, default=9, help="Maximum benchmarks per heatmap row (default: 9)")
    args = parser.parse_args()
    args.input_dir = args.input_dir or ROOT / "build/res" / args.version
    args.output_dir = args.output_dir or ROOT / "build/figures" / args.version
    args.title = args.title or f"Recursive codegen · {args.version}"
    if not 72 <= args.dpi <= 600:
        parser.error("--dpi must be between 72 and 600")
    if args.columns < 1:
        parser.error("--columns must be at least 1")
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11, "text.color": INK,
                         "axes.labelcolor": MUTED, "svg.fonttype": "path", "pdf.fonttype": 42,
                         "axes.unicode_minus": True})
    try:
        models, tasks = read_models(args.input_dir)
        if any(m["data"].get("benchmark_version", args.version) != args.version for m in models):
            raise ValueError(f"Report version does not match --version {args.version}")
        analysis = None
        if not args.no_analysis:
            if args.analysis:
                analysis = read_json(args.analysis)
            else:
                automatic = ROOT / "build/analysis" / args.version / "summary.json"
                if automatic.exists():
                    candidate = read_json(automatic)
                    # Historical measurements retain their original ABI/provenance.
                    variants = list(candidate.get("variants", {}).values())
                    model_tasks = {task["name"] for task in models[0]["data"]["tasks"]}
                    if variants and all(r.get("benchmark_version") == models[0]["data"].get("benchmark_version")
                                        and r.get("harness") == models[0]["data"].get("harness")
                                        and r.get("total") == models[0]["data"].get("total")
                                        and {task["name"] for task in r.get("tasks", [])} == model_tasks
                                        for r in variants):
                        analysis = candidate
        args.output_dir.mkdir(parents=True, exist_ok=True)
        draw_models(models, tasks, analysis, args)
        if analysis:
            draw_optimizations(analysis, args)
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
