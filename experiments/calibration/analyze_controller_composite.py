#!/usr/bin/env python3
"""Validate and visualize the fixed-placement controller composite workload."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
from collections import defaultdict
from pathlib import Path
from statistics import mean


MODE_SUFFIXES = (
    "pressure_gated_comfort_probing",
    "comfort_guided_probing",
    "residual_guarded_probing",
    "probing",
)

MODE_LABELS = {
    "probing": "Bracket probing",
    "residual_guarded_probing": "Guarded bracket probing",
    "pressure_gated_comfort_probing": "Pressure-gated comfort",
    "comfort_guided_probing": "Guarded margin comfort",
}

MODE_COLORS = {
    "probing": "#B85C38",
    "residual_guarded_probing": "#6F4E7C",
    "pressure_gated_comfort_probing": "#3264A8",
    "comfort_guided_probing": "#287271",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--global-counts", required=True)
    parser.add_argument("--baselines", required=True)
    parser.add_argument("--controller-csv", required=True)
    parser.add_argument("--n", type=int, required=True)
    parser.add_argument("--epsilon-m", type=float, required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit nonzero when any suitability check fails.",
    )
    return parser.parse_args()


def number(row: dict[str, str], field: str) -> float:
    try:
        return float(row.get(field, ""))
    except (TypeError, ValueError):
        return math.nan


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def scenario_timeline(path: Path) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    steps = json.loads(path.read_text(encoding="utf-8"))
    timeline = []
    segments = []
    start = 0
    for step in steps:
        duration = int(step["duration"])
        segment = {
            "start": start,
            "end": start + duration,
            "phase": step["phase"],
            "label": step["label"],
            "difficulty": step["difficulty"],
        }
        segments.append(segment)
        timeline.extend([segment] * duration)
        start += duration
    return timeline, segments


def mode_from_path(path: Path) -> str:
    for mode in MODE_SUFFIXES:
        if path.stem.endswith("_" + mode):
            return mode
    raise ValueError(f"cannot infer controller mode from {path.name}")


def normalized_error(row: dict[str, str], n: int) -> float:
    total = number(row, "N_global")
    aae = number(row, "aae")
    return n * aae / total if total > 0 and math.isfinite(aae) else math.nan


def grouped_metrics(
    rows: list[dict[str, str]], timeline: list[dict[str, object]], n: int
) -> dict[str, dict[str, float]]:
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        window = int(number(row, "window"))
        grouped[str(timeline[window]["phase"])].append(row)

    result = {}
    for phase, phase_rows in grouped.items():
        errors = [normalized_error(row, n) for row in phase_rows]
        q_values = [number(row, "q_current") / n for row in phase_rows]
        result[phase] = {
            "windows": float(len(phase_rows)),
            "threshold_normalized_error": mean(v for v in errors if math.isfinite(v)),
            "q_over_n": mean(v for v in q_values if math.isfinite(v)),
            "hh_recall": mean(number(row, "hh_recall") for row in phase_rows),
            "hh_f1": mean(number(row, "hh_f1") for row in phase_rows),
            "candidate_hh_recall": mean(
                number(row, "candidate_hh_recall") for row in phase_rows
            ),
            "service_violation_rate": mean(
                number(row, "service_violation") for row in phase_rows
            ),
        }
    return result


def write_phase_summary(
    path: Path,
    records: list[tuple[str, str, str, dict[str, float]]],
) -> None:
    fields = [
        "kind",
        "method",
        "phase",
        "windows",
        "threshold_normalized_error",
        "q_over_n",
        "hh_recall",
        "hh_f1",
        "candidate_hh_recall",
        "service_violation_rate",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for kind, method, phase, metrics in records:
            writer.writerow(
                {
                    "kind": kind,
                    "method": method,
                    "phase": phase,
                    **{
                        key: (f"{value:.10g}" if isinstance(value, float) else value)
                        for key, value in metrics.items()
                    },
                }
            )


def plot_trajectories(
    out_dir: Path,
    controller_rows: dict[str, list[dict[str, str]]],
    segments: list[dict[str, object]],
    n: int,
    epsilon_m: float,
) -> None:
    os.environ.setdefault("MPLCONFIGDIR", "/tmp/hh_matplotlib")
    Path(os.environ["MPLCONFIGDIR"]).mkdir(parents=True, exist_ok=True)
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(2, 1, figsize=(11.5, 5.8), sharex=True)
    phase_colors = {
        "easy": "#EEF3EA",
        "moderate": "#E8EEF5",
        "hard": "#F5E6E3",
        "hard_to_easy": "#F4F0DF",
    }
    for ax in axes:
        for segment in segments:
            ax.axvspan(
                int(segment["start"]) - 0.5,
                int(segment["end"]) - 0.5,
                color=phase_colors[str(segment["difficulty"])],
                linewidth=0,
                zorder=0,
            )

    for mode, rows in controller_rows.items():
        windows = [int(number(row, "window")) for row in rows]
        axes[0].plot(
            windows,
            [normalized_error(row, n) for row in rows],
            color=MODE_COLORS[mode],
            linewidth=1.5,
            label=MODE_LABELS[mode],
            zorder=3,
        )
        axes[1].plot(
            windows,
            [number(row, "q_current") / n for row in rows],
            color=MODE_COLORS[mode],
            linewidth=1.5,
            label=MODE_LABELS[mode],
            zorder=3,
        )

    axes[0].axhline(epsilon_m, color="black", linestyle="--", linewidth=1.0)
    axes[1].axhline(1.0, color="black", linestyle=":", linewidth=1.0)
    axes[0].set_ylabel("Threshold-normalized error")
    axes[1].set_ylabel("Deployed capacity q/n")
    axes[1].set_xlabel("Window")
    for ax in axes:
        ax.grid(axis="y", alpha=0.25)
        ax.set_axisbelow(True)
        ax.margins(x=0)
    axes[0].legend(ncol=4, loc="upper center", bbox_to_anchor=(0.5, 1.22), frameon=False)
    fig.tight_layout()
    fig.savefig(out_dir / "controller_composite_trajectories.pdf", bbox_inches="tight")
    fig.savefig(out_dir / "controller_composite_trajectories.png", dpi=220, bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    timeline, segments = scenario_timeline(Path(args.scenario))

    global_rows = [
        json.loads(line)
        for line in Path(args.global_counts).read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    if len(global_rows) != len(timeline):
        raise ValueError(
            f"scenario has {len(timeline)} windows but counts contain {len(global_rows)}"
        )

    global_phase: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for row, segment in zip(global_rows, timeline):
        counts = {key: int(value) for key, value in row["counts"].items()}
        total = sum(counts.values())
        threshold = total / args.n
        phase = str(segment["phase"])
        global_phase[phase]["mass"].append(float(total))
        global_phase[phase]["cardinality"].append(float(len(counts)))
        global_phase[phase]["hh_count"].append(
            float(sum(value > threshold for value in counts.values()))
        )
        global_phase[phase]["near_count"].append(
            float(sum(0.8 * threshold <= value <= threshold for value in counts.values()))
        )

    with (out_dir / "global_phase_summary.csv").open(
        "w", newline="", encoding="utf-8"
    ) as handle:
        fields = ["phase", "windows", "mass_min", "mass_max", "cardinality_mean", "hh_mean", "hh_min", "hh_max", "near_mean"]
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for phase, values in global_phase.items():
            writer.writerow(
                {
                    "phase": phase,
                    "windows": len(values["mass"]),
                    "mass_min": int(min(values["mass"])),
                    "mass_max": int(max(values["mass"])),
                    "cardinality_mean": f"{mean(values['cardinality']):.3f}",
                    "hh_mean": f"{mean(values['hh_count']):.3f}",
                    "hh_min": int(min(values["hh_count"])),
                    "hh_max": int(max(values["hh_count"])),
                    "near_mean": f"{mean(values['near_count']):.3f}",
                }
            )

    baseline_rows = [
        row
        for row in load_csv(Path(args.baselines))
        if row.get("method_type") == "ss" and "policy=static" in row.get("method", "")
    ]
    baselines_by_method: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in baseline_rows:
        baselines_by_method[row["method"]].append(row)

    controller_rows = {}
    for path in sorted(Path(args.controller_csv).glob("*.csv")):
        mode = mode_from_path(path)
        controller_rows[mode] = [
            row
            for row in load_csv(path)
            if row.get("method_type") == "ss" and "policy=difficulty" in row.get("method", "")
        ]

    records = []
    baseline_metrics = {}
    for method, rows in baselines_by_method.items():
        metrics = grouped_metrics(rows, timeline, args.n)
        baseline_metrics[method] = metrics
        records.extend(("static", method, phase, values) for phase, values in metrics.items())
    controller_metrics = {}
    for mode, rows in controller_rows.items():
        metrics = grouped_metrics(rows, timeline, args.n)
        controller_metrics[mode] = metrics
        records.extend(("adaptive", mode, phase, values) for phase, values in metrics.items())
    write_phase_summary(out_dir / "phase_summary.csv", records)

    qn_name = next(name for name in baselines_by_method if "q=n" in name)
    q4n_name = next(name for name in baselines_by_method if "q=4n" in name)
    hard_error_qn = baseline_metrics[qn_name]["hard_step"]["threshold_normalized_error"]
    easy_error_qn = baseline_metrics[qn_name]["easy_plateau"]["threshold_normalized_error"]
    hard_error_q4n = baseline_metrics[q4n_name]["hard_step"]["threshold_normalized_error"]

    q_ranges = {}
    min_recall = 1.0
    for mode, rows in controller_rows.items():
        q_values = [number(row, "q_current") / args.n for row in rows]
        q_ranges[mode] = {"min": min(q_values), "max": max(q_values)}
        min_recall = min(min_recall, *(number(row, "hh_recall") for row in rows))

    expected_windows = set(range(len(timeline)))
    static_window_coverage = all(
        {int(number(row, "window")) for row in rows} == expected_windows
        for rows in baselines_by_method.values()
    )
    controller_window_coverage = (
        set(controller_rows) == set(MODE_SUFFIXES)
        and all(
            {int(number(row, "window")) for row in rows} == expected_windows
            for rows in controller_rows.values()
        )
    )

    checks = {
        "window_count_160": len(global_rows) == 160,
        "static_results_cover_all_windows": static_window_coverage,
        "controller_results_cover_all_windows": controller_window_coverage,
        "constant_mass_120000": all(
            sum(int(value) for value in row["counts"].values()) == 120000
            for row in global_rows
        ),
        "constant_cardinality_20000": all(len(row["counts"]) == 20000 for row in global_rows),
        "harder_than_easy_at_qn": hard_error_qn > easy_error_qn * 1.25,
        "q4n_improves_hard_error": hard_error_q4n < hard_error_qn * 0.60,
        "every_controller_moves_capacity": all(
            values["max"] >= values["min"] * 1.25 for values in q_ranges.values()
        ),
        "controller_recall_at_least_0.999": min_recall >= 0.999,
    }
    report = {
        "checks": checks,
        "static": {
            "qn_hard_error": hard_error_qn,
            "qn_easy_error": easy_error_qn,
            "q4n_hard_error": hard_error_q4n,
        },
        "controller_q_over_n_ranges": q_ranges,
        "minimum_controller_hh_recall": min_recall,
    }
    (out_dir / "acceptance.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    with (out_dir / "acceptance.txt").open("w", encoding="utf-8") as handle:
        for name, passed in checks.items():
            handle.write(f"{'PASS' if passed else 'FAIL'} {name}\n")
        handle.write(f"q=n hard error: {hard_error_qn:.6f}\n")
        handle.write(f"q=n easy error: {easy_error_qn:.6f}\n")
        handle.write(f"q=4n hard error: {hard_error_q4n:.6f}\n")
        for mode, values in q_ranges.items():
            handle.write(
                f"{mode} q/n range: {values['min']:.3f}--{values['max']:.3f}\n"
            )
        handle.write(f"minimum controller HH recall: {min_recall:.6f}\n")

    plot_trajectories(
        out_dir,
        controller_rows,
        segments,
        args.n,
        args.epsilon_m,
    )
    print((out_dir / "acceptance.txt").read_text(encoding="utf-8"), end="")
    return 0 if all(checks.values()) or not args.strict else 2


if __name__ == "__main__":
    raise SystemExit(main())
