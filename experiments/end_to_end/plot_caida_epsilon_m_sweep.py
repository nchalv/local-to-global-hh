#!/usr/bin/env python3
"""Summarize and plot Adaptive SS and Hybrid epsilon_M sweeps."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import MaxNLocator


N_VALUES = (200, 400, 600, 800)
PLACEMENTS = (
    ("round_robin", "Round robin"),
    ("filter_aware_threshold07", "Visibility-suppression placement"),
)
SELECTED_EPSILON = {
    "Adaptive SS": 0.15,
    "Hybrid": 0.40,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument(
        "--counts",
        type=Path,
        default=Path(
            "data_preparation/generated/"
            "caida_20180315_130000_200w_5s_dirA.jsonl"
        ),
    )
    parser.add_argument("--epsilon-m-values", nargs="+", type=float, required=True)
    parser.add_argument("--out", type=Path, required=True)
    return parser.parse_args()


def instance_index(method: str) -> int:
    match = re.search(r"#(\d+)$", method)
    return int(match.group(1)) if match else 0


def epsilon_value(method: str, epsilon_values: list[float]) -> float:
    match = re.search(r"epsilonM=([0-9.]+)", method)
    if match:
        return float(match.group(1))
    index = instance_index(method)
    if index >= len(epsilon_values):
        raise RuntimeError(f"no epsilon_M value for method {method!r}")
    return epsilon_values[index]


def method_family(method: str) -> str | None:
    if method.startswith("ss[policy=difficulty"):
        return "Adaptive SS"
    if method.startswith("hybrid[head=topn-frontier"):
        return "Hybrid"
    return None


def csv_path(root: Path, placement: str, n: int) -> Path:
    paths = sorted((root / f"{placement}_n{n}" / "csv").glob("*.csv"))
    if len(paths) != 1:
        raise RuntimeError(
            f"expected one CSV under {root / f'{placement}_n{n}' / 'csv'}, "
            f"found {len(paths)}"
        )
    return paths[0]


def load_hh_counts(path: Path) -> dict[int, dict[int, int]]:
    result = {n: {} for n in N_VALUES}
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            row = json.loads(line)
            counts = [int(value) for value in row["counts"].values()]
            total = sum(counts)
            window = int(row["window"])
            for n in N_VALUES:
                threshold = total // n + 1
                result[n][window] = sum(value >= threshold for value in counts)
    return result


def aggregate(
    path: Path,
    epsilon_values: list[float],
    placement: str,
    n: int,
    hh_counts: dict[int, int],
) -> list[dict[str, float | int | str]]:
    accum: dict[str, dict[str, float]] = defaultdict(lambda: defaultdict(float))
    normalized_samples: dict[str, list[float]] = defaultdict(list)
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            if int(row["window"]) == 0:
                continue
            family = method_family(row["method"])
            if family is None:
                continue
            item = accum[row["method"]]
            actual = hh_counts[int(row["window"])]
            precision = float(row["hh_precision"])
            recall = float(row["hh_recall"])
            tp = round(recall * actual)
            predicted = round(tp / precision) if precision > 0.0 else 0
            item["tp"] += tp
            item["fp"] += max(0, predicted - tp)
            item["fn"] += actual - tp
            normalized_error = n * float(row["aae"]) / float(row["N_global"])
            item["normalized_error"] += normalized_error
            normalized_samples[row["method"]].append(normalized_error)
            item["worker_kib"] += float(row["mem_worker_total_kib"])
            item["upstream_kib"] += float(row["report_volume_kib"])
            item["coord_peak_kib"] += float(row["mem_coord_peak_kib"])
            item["windows"] += 1

    result: list[dict[str, float | int | str]] = []
    for method, item in accum.items():
        epsilon_m = epsilon_value(method, epsilon_values)
        windows = item["windows"]
        precision_den = item["tp"] + item["fp"]
        recall_den = item["tp"] + item["fn"]
        precision = item["tp"] / precision_den if precision_den else 1.0
        recall = item["tp"] / recall_den if recall_den else 1.0
        f1 = (
            2.0 * precision * recall / (precision + recall)
            if precision + recall
            else 0.0
        )
        samples = sorted(normalized_samples[method])
        p95 = samples[max(0, math.ceil(0.95 * len(samples)) - 1)]
        result.append(
            {
                "placement": placement,
                "n": n,
                "method": method_family(method),
                "epsilon_m": epsilon_m,
                "windows": int(windows),
                "precision": precision,
                "recall": recall,
                "f1": f1,
                "normalized_error": item["normalized_error"] / windows,
                "normalized_error_p95": p95,
                "target_violation_rate": (
                    sum(value > epsilon_m for value in samples) / len(samples)
                ),
                "worker_mib": item["worker_kib"] / windows / 1024.0,
                "upstream_kib": item["upstream_kib"] / windows,
                "coord_peak_kib": item["coord_peak_kib"] / windows,
            }
        )
    return sorted(result, key=lambda row: (str(row["method"]), float(row["epsilon_m"])))


def write_summary(path: Path, rows: list[dict[str, object]]) -> None:
    fields = (
        "placement",
        "n",
        "method",
        "epsilon_m",
        "windows",
        "precision",
        "recall",
        "f1",
        "normalized_error",
        "normalized_error_p95",
        "target_violation_rate",
        "worker_mib",
        "upstream_kib",
        "coord_peak_kib",
    )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def draw(path: Path, rows: list[dict[str, object]], placement: str, title: str) -> None:
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 11.0,
            "axes.titlesize": 12.0,
            "axes.labelsize": 11.0,
            "xtick.labelsize": 10.0,
            "ytick.labelsize": 10.0,
        }
    )
    fig, axes = plt.subplots(2, 4, figsize=(7.0, 3.65), sharey="row")
    styles = {
        "Adaptive SS": ("#d97706", "o"),
        "Hybrid": ("#b91c1c", "*"),
    }
    line_styles = {
        "Adaptive SS": ("#777777", ":"),
        "Hybrid": ("#111111", "-"),
    }
    metrics = (
        ("f1", r"HH F1$\uparrow$"),
        ("normalized_error", "Thresh.-norm. err.$\\downarrow$"),
    )
    plot_epsilon_max = {"Adaptive SS": 0.25, "Hybrid": float("inf")}
    clipped_extensions = []
    for column, n in enumerate(N_VALUES):
        selected = [
            row for row in rows
            if row["placement"] == placement and int(row["n"]) == n
        ]
        for row_index, (metric, ylabel) in enumerate(metrics):
            ax = axes[row_index, column]
            for family, (color, marker) in styles.items():
                line_color, line_style = line_styles[family]
                all_points = sorted(
                    (row for row in selected if row["method"] == family),
                    key=lambda row: float(row["epsilon_m"]),
                )
                points = [
                    row for row in all_points
                    if float(row["epsilon_m"]) <= plot_epsilon_max[family]
                ]
                ax.plot(
                    [1024.0 * float(row["worker_mib"]) for row in points],
                    [float(row[metric]) for row in points],
                    color=line_color,
                    linestyle=line_style,
                    linewidth=0.9 if family == "Hybrid" else 1.7,
                )
                omitted = [row for row in all_points if row not in points]
                if points and omitted:
                    clipped_extensions.append(
                        (
                            ax,
                            line_color,
                            line_style,
                            (
                                1024.0 * float(points[-1]["worker_mib"]),
                                float(points[-1][metric]),
                            ),
                            (
                                1024.0 * float(omitted[0]["worker_mib"]),
                                float(omitted[0][metric]),
                            ),
                        )
                    )
                selected_epsilon = SELECTED_EPSILON[family]
                selected_point = next(
                    (
                        point
                        for point in points
                        if math.isclose(
                            float(point["epsilon_m"]),
                            selected_epsilon,
                            rel_tol=0.0,
                            abs_tol=1e-9,
                        )
                    ),
                    None,
                )
                if selected_point is None:
                    raise RuntimeError(
                        f"selected epsilon_M={selected_epsilon:g} is absent "
                        f"for {family} under {placement}, n={n}"
                    )
                alternative_points = [
                    point for point in points if point is not selected_point
                ]
                if family == "Hybrid":
                    ax.scatter(
                        [1024.0 * float(point["worker_mib"]) for point in alternative_points],
                        [float(point[metric]) for point in alternative_points],
                        marker="o",
                        s=10,
                        color="#111111",
                        linewidths=0,
                        zorder=4,
                    )
                else:
                    ax.scatter(
                        [1024.0 * float(point["worker_mib"]) for point in alternative_points],
                        [float(point[metric]) for point in alternative_points],
                        marker="o",
                        s=17,
                        facecolors="white",
                        edgecolors="#111111",
                        linewidths=0.8,
                        zorder=4,
                    )
                ax.scatter(
                    [1024.0 * float(selected_point["worker_mib"])],
                    [float(selected_point[metric])],
                    marker=marker,
                    s=70 if family == "Hybrid" else 42,
                    facecolors=color,
                    edgecolors="#111111",
                    linewidths=0.85,
                    zorder=6,
                )
            ax.grid(True, color="#dddddd", linewidth=0.75)
            ax.set_axisbelow(True)
            ax.xaxis.set_major_locator(MaxNLocator(nbins=4))
            ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
            if column == 0:
                ax.set_ylabel(ylabel)
            else:
                ax.tick_params(axis="y", left=False, labelleft=False)
            if row_index == 0:
                ax.set_title(rf"$n={n}$")
    # Show the direction of the truncated Adaptive SS curve without allowing the
    # omitted point to expand the shared axes.
    fig.canvas.draw()
    for ax, line_color, line_style, start, end in clipped_extensions:
        xlim, ylim = ax.get_xlim(), ax.get_ylim()
        ax.plot(
            [start[0], end[0]],
            [start[1], end[1]],
            color=line_color,
            linestyle=line_style,
            linewidth=1.7,
            clip_on=True,
            zorder=1.5,
        )
        ax.set_xlim(xlim)
        ax.set_ylim(ylim)
    fig.legend(
        handles=[
            Line2D(
                [],
                [],
                color=line_styles[family][0],
                linestyle=line_styles[family][1],
                marker=marker,
                markersize=8.0 if family == "Hybrid" else 6.2,
                markerfacecolor=color,
                markeredgecolor="#111111",
                markeredgewidth=0.85,
                linewidth=0.9 if family == "Hybrid" else 1.7,
                label=rf"{family}: selected $\epsilon_M={SELECTED_EPSILON[family]:g}$",
            )
            for family, (color, marker) in styles.items()
        ],
        loc="upper center",
        ncol=2,
        frameon=False,
        bbox_to_anchor=(0.5, 0.955),
        fontsize=9.8,
        columnspacing=1.1,
        handletextpad=0.5,
    )
    fig.supxlabel("Mean per-partition memory (KiB)", y=0.02, fontsize=11.0)
    fig.subplots_adjust(
        left=0.13, right=0.99, bottom=0.15, top=0.78, wspace=0.24, hspace=0.38
    )
    fig.savefig(path)
    plt.close(fig)


def main() -> int:
    args = parse_args()
    hh_counts = load_hh_counts(args.counts)
    rows: list[dict[str, object]] = []
    for placement, _ in PLACEMENTS:
        for n in N_VALUES:
            rows.extend(
                aggregate(
                    csv_path(args.root, placement, n),
                    args.epsilon_m_values,
                    placement,
                    n,
                    hh_counts[n],
                )
            )
    args.out.mkdir(parents=True, exist_ok=True)
    write_summary(args.out / "summary.csv", rows)
    for placement, title in PLACEMENTS:
        draw(args.out / f"epsilon_m_sweep_{placement}.pdf", rows, placement, title)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
