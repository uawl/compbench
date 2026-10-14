"""Shared scoring rule: each failed or uncompleted task contributes 1.0."""

import math

SCORE_POLICY = "geomean_failures_as_1"
FAILURE_SPEEDUP = 1.0


def geometric_score(report):
    total = report.get("total")
    if not isinstance(total, int) or isinstance(total, bool) or total <= 0:
        return None
    tasks = report.get("tasks", [])
    if len(tasks) > total:
        raise ValueError("Report has more tasks than its declared total")
    terms = []
    for task in tasks:
        if task.get("status") != "pass":
            continue  # log(1.0) = 0; failures still count in the denominator.
        value = task.get("speedup")
        if (not isinstance(value, (int, float)) or isinstance(value, bool)
                or not math.isfinite(value) or value <= 0):
            raise ValueError(f"Invalid speedup for {task.get('name', '?')}")
        terms.append(math.log(value))
    return math.exp(math.fsum(terms) / total)


def apply_score(report):
    value = geometric_score(report)
    report["score"] = round(value, 6) if value is not None else None
    report["score_policy"] = SCORE_POLICY
    report["failure_speedup"] = FAILURE_SPEEDUP
    return report
