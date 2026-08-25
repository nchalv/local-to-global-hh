#!/usr/bin/env python3
"""Plot CAIDA end-to-end memory/quality frontiers from hh_bench CSV files."""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from math import floor
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import MaxNLocator, MultipleLocator


N_VALUES = (200, 400, 600, 800)
HL_LABELS = {
    "hl": "0.08n",
    "hl#1": "0.16n",
    "hl#2": "0.32n",
    "hl#3": "0.64n",
    "hl#4": "1.28n",
}
SS_LABELS = {
    "ss[policy=static q=n]": "n",
    "ss[policy=static q=2n]": "2n",
    "ss[policy=static q=4n]": "4n",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path("experiments/end_to_end"),
        help="end-to-end experiment root",
    )
    parser.add_argument(
        "--counts",
        type=Path,
        default=Path(
            "data_preparation/generated/"
            "caida_20180315_130000_200w_5s_dirA.jsonl"
        ),
        help="global CAIDA counts used to recover aggregate HH denominators",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("experiments/end_to_end/caida_comparison"),
    )
    parser.add_argument(
        "--adversarial-placement",
        choices=("dual_visibility", "filter_aware", "filter_aware_threshold07"),
        default="dual_visibility",
        help="adversarial placement paired with round robin",
    )
    parser.add_argument(
        "--comparison-dir",
        default="full_comparison_per_item",
        help="per-dataset directory containing the comparison CSV",
    )
    parser.add_argument(
        "--direct-layout",
        action="store_true",
        help=(
            "read <root>/<placement>_n<N>/csv directly, as produced by "
            "the compact benchmark suites"
        ),
    )
    parser.add_argument(
        "--hybrid-epsilon-values",
        nargs="*",
        type=float,
        default=(),
        help=(
            "Ordered Hybrid tail epsilon_M values in the comparison CSV. "
            "When provided, plot every Hybrid configuration as a curve."
        ),
    )
    parser.add_argument(
        "--axis-mode",
        choices=("broken", "compressed"),
        default="broken",
        help="vertical scale treatment for the quality panels",
    )
    return parser.parse_args()


def experiment_csv(
    root: Path,
    placement: str,
    n: int,
    comparison_dir: str,
    direct_layout: bool = False,
) -> Path:
    if direct_layout:
        directory = root / f"{placement}_n{n}"
    elif placement == "round_robin":
        directory = root / f"caida_round_robin_n{n}" / comparison_dir
    elif placement == "dual_visibility":
        directory = (
            root
            / f"caida_dual_visibility_adversary_n{n}"
            / comparison_dir
        )
    elif placement == "filter_aware":
        directory = (
            root
            / f"caida_filter_aware_visibility_adversary_n{n}"
            / comparison_dir
        )
    else:
        directory = (
            root
            / f"caida_filter_aware_visibility_threshold07_n{n}"
            / comparison_dir
        )
    paths = sorted((directory / "csv").glob("*.csv"))
    if len(paths) != 1:
        raise RuntimeError(f"expected one CSV in {directory / 'csv'}, found {len(paths)}")
    return paths[0]


def load_hh_counts(
    path: Path, n_values: tuple[int, ...]
) -> dict[int, dict[int, int]]:
    result: dict[int, dict[int, int]] = {n: {} for n in n_values}
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            record = json.loads(line)
            counts = record["counts"]
            total = sum(int(value) for value in counts.values())
            for n in n_values:
                threshold = total // n + 1
                result[n][int(record["window"])] = sum(
                    int(value) >= threshold for value in counts.values()
                )
    return result


def aggregate(path: Path, hh_counts: dict[int, int]) -> dict[str, dict[str, float]]:
    accum: dict[str, dict[str, float]] = defaultdict(
        lambda: defaultdict(float)
    )
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            window = int(row["window"])
            if window == 0:
                continue
            method = row["method"]
            actual = hh_counts[window]
            recall = float(row["hh_recall"])
            precision = float(row["hh_precision"])
            tp = round(recall * actual)
            predicted = round(tp / precision) if precision > 0.0 else 0
            fp = max(0, predicted - tp)

            item = accum[method]
            item["tp"] += tp
            item["fp"] += fp
            item["fn"] += actual - tp
            item["hh_items"] += actual
            item["rel_error"] += float(row["are"]) * actual
            item["coord_peak_kib"] += float(row["mem_coord_peak_kib"])
            item["worker_kib"] += float(row["mem_worker_total_kib"])
            item["upstream_kib"] += float(row["report_volume_kib"])
            item["windows"] += 1

    summary: dict[str, dict[str, float]] = {}
    for method, item in accum.items():
        precision_den = item["tp"] + item["fp"]
        recall_den = item["tp"] + item["fn"]
        precision = item["tp"] / precision_den if precision_den else 1.0
        recall = item["tp"] / recall_den if recall_den else 1.0
        f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
        summary[method] = {
            "precision": precision,
            "recall": recall,
            "f1": f1,
            "are_percent": 100.0 * item["rel_error"] / item["hh_items"],
            "coord_peak_mib": item["coord_peak_kib"] / item["windows"] / 1024.0,
            "worker_mib": item["worker_kib"] / item["windows"] / 1024.0,
            "upstream_kib": item["upstream_kib"] / item["windows"],
        }
    return summary


def write_summary(
    path: Path, summaries: dict[tuple[str, int], dict[str, dict[str, float]]]
) -> None:
    fields = (
        "placement",
        "n",
        "method",
        "precision",
        "recall",
        "f1",
        "are_percent",
        "worker_mib",
        "coord_peak_mib",
        "upstream_kib",
    )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for (placement, n), methods in summaries.items():
            for method, metrics in methods.items():
                writer.writerow({"placement": placement, "n": n, "method": method, **metrics})


def series(summary: dict[str, dict[str, float]], methods: list[str], metric: str):
    points = [summary[method] for method in methods]
    return (
        [point["worker_mib"] for point in points],
        [point[metric] for point in points],
    )


def resolve_method(summary: dict[str, dict[str, float]], name: str) -> str:
    if name in summary:
        return name
    matches = [method for method in summary if method.startswith(name)]
    if len(matches) != 1:
        raise RuntimeError(f"expected one method matching {name!r}, found {matches}")
    return matches[0]


def hybrid_methods(
    summary: dict[str, dict[str, float]], epsilon_values: tuple[float, ...]
) -> list[str]:
    matches = [
        method
        for method in summary
        if method.startswith("hybrid[head=topn-frontier")
    ]

    def order(method: str) -> int:
        suffix = method.rsplit("#", 1)
        return int(suffix[1]) if len(suffix) == 2 and suffix[1].isdigit() else 0

    matches.sort(key=order)
    if not matches:
        raise RuntimeError("no topn-frontier Hybrid result found")
    if epsilon_values and len(matches) != len(epsilon_values):
        raise RuntimeError(
            "Hybrid epsilon sweep length does not match the number of Hybrid "
            f"results: {len(epsilon_values)} values, {len(matches)} methods"
        )
    return matches


def draw_compressed(
    path: Path,
    summaries: dict[tuple[str, int], dict[str, dict[str, float]]],
    placements: tuple[str, str],
    hybrid_epsilon_values: tuple[float, ...],
) -> None:
    plt.rcParams.update(
        {
            "font.size": 7.2,
            "axes.titlesize": 8.5,
            "axes.labelsize": 8,
            "legend.fontsize": 7.2,
            "xtick.labelsize": 6.5,
            "ytick.labelsize": 6.0,
            "font.family": "serif",
        }
    )
    fig, axes = plt.subplots(2, 4, figsize=(7.15, 4.35), sharey=False)
    colors = {
        "hl": "#2b6cb0",
        "ss": "#777777",
        "adaptive": "#d97706",
        "hybrid": "#b91c1c",
    }
    hl_methods = list(HL_LABELS)
    ss_methods = list(SS_LABELS)
    for column, n in enumerate(N_VALUES):
        # Use one linear scale per subplot.  This exposes the meaningful local
        # frontier while retaining every configuration in the corresponding
        # n-specific comparison.
        panel_points = list(summaries[(placements[0], n)].values())
        recall_floor = min(point["recall"] for point in panel_points)
        are_ceiling = max(point["are_percent"] for point in panel_points)
        recall_compressed = recall_floor < 0.9
        are_compressed = are_ceiling > 10.0

        def display_y(metric: str, value: float) -> float:
            if metric == "recall":
                if not recall_compressed:
                    return value
                if value < 0.9:
                    return 0.14 * (value - recall_floor) / (0.9 - recall_floor)
                return 0.24 + 0.76 * (value - 0.9) / 0.1
            if not are_compressed:
                return value / are_ceiling
            if value <= 10.0:
                return 0.78 * value / 10.0
            return 0.88 + 0.12 * (value - 10.0) / (are_ceiling - 10.0)

        def display_values(metric: str, values: list[float]) -> list[float]:
            return [display_y(metric, value) for value in values]

        def crosses_break(metric: str, left: float, right: float) -> bool:
            if metric == "recall" and recall_compressed:
                return (left < 0.9) != (right < 0.9)
            if metric == "are_percent" and are_compressed:
                return (left <= 10.0) != (right <= 10.0)
            return False

        def plot_curve(ax, x: list[float], y: list[float], **kwargs) -> None:
            """Draw contiguous portions only; a scale break is not a data edge."""
            start = 0
            for index in range(1, len(x)):
                if crosses_break(metric, y[index - 1], y[index]):
                    if index - start > 1:
                        ax.plot(x[start:index], display_values(metric, y[start:index]), **kwargs)
                    start = index
            if len(x) - start > 1:
                ax.plot(x[start:], display_values(metric, y[start:]), **kwargs)
        for row_index, metric in enumerate(("recall", "are_percent")):
            ax = axes[row_index, column]
            ax.set_xscale("linear")
            ax.set_yscale("linear")
            for placement in placements:
                summary = summaries[(placement, n)]
                style = {"linestyle": "-"}
                marker_face = None

                x, y = series(summary, hl_methods, metric)
                plot_curve(
                    ax,
                    x,
                    y,
                    color=colors["hl"],
                    linewidth=1.15,
                    linestyle=style["linestyle"],
                    zorder=4,
                )
                if metric == "recall":
                    visible = [(x_value, y_value) for x_value, y_value in zip(x, y) if y_value >= recall_floor]
                    if visible:
                        ax.plot(
                            [point[0] for point in visible],
                            display_values(metric, [point[1] for point in visible]),
                            color=colors["hl"],
                            marker="o",
                            markersize=3.8,
                            markerfacecolor=marker_face,
                            markeredgewidth=0.8,
                            linestyle="none",
                            zorder=5,
                        )
                    clipped_x = [x_value for x_value, y_value in zip(x, y) if y_value < recall_floor]
                    if clipped_x:
                        ax.plot(
                            clipped_x,
                            [recall_floor + 0.002] * len(clipped_x),
                            color=colors["hl"],
                            marker="v",
                            markersize=4.2,
                            markerfacecolor=marker_face,
                            markeredgewidth=0.8,
                            linestyle="none",
                            clip_on=False,
                            zorder=6,
                        )
                else:
                    visible = [(x_value, y_value) for x_value, y_value in zip(x, y) if y_value <= are_ceiling]
                    if visible:
                        ax.plot(
                            [point[0] for point in visible],
                            display_values(metric, [point[1] for point in visible]),
                            color=colors["hl"],
                            marker="o",
                            markersize=3.8,
                            markerfacecolor=marker_face,
                            markeredgewidth=0.8,
                            linestyle="none",
                            zorder=5,
                        )
                    clipped_x = [x_value for x_value, y_value in zip(x, y) if y_value > are_ceiling]
                    if clipped_x:
                        ax.plot(
                            clipped_x,
                            [are_ceiling * 0.985] * len(clipped_x),
                            color=colors["hl"],
                            marker="^",
                            markersize=4.2,
                            markerfacecolor=marker_face,
                            markeredgewidth=0.8,
                            linestyle="none",
                            clip_on=False,
                            zorder=6,
                        )
                x, y = series(summary, ss_methods, metric)
                plot_curve(
                    ax,
                    x,
                    y,
                    color=colors["ss"],
                    linewidth=0.95,
                    linestyle=style["linestyle"],
                    zorder=2,
                )
                if metric == "recall":
                    visible = [(x_value, y_value) for x_value, y_value in zip(x, y) if y_value >= recall_floor]
                    if visible:
                        ax.plot(
                            [point[0] for point in visible],
                            display_values(metric, [point[1] for point in visible]),
                            color=colors["ss"],
                            marker="s",
                            markersize=3.6,
                            markerfacecolor=marker_face,
                            markeredgewidth=0.8,
                            linestyle="none",
                            zorder=3,
                        )
                    clipped_x = [x_value for x_value, y_value in zip(x, y) if y_value < recall_floor]
                    if clipped_x:
                        ax.plot(
                            clipped_x,
                            [recall_floor + 0.002] * len(clipped_x),
                            color=colors["ss"],
                            marker="v",
                            markersize=4.0,
                            markerfacecolor=marker_face,
                            markeredgewidth=0.8,
                            linestyle="none",
                            clip_on=False,
                            zorder=6,
                        )
                else:
                    visible = [(x_value, y_value) for x_value, y_value in zip(x, y) if y_value <= are_ceiling]
                    if visible:
                        ax.plot(
                            [point[0] for point in visible],
                            display_values(metric, [point[1] for point in visible]),
                            color=colors["ss"],
                            marker="s",
                            markersize=3.6,
                            markerfacecolor=marker_face,
                            markeredgewidth=0.8,
                            linestyle="none",
                            zorder=3,
                        )
                    clipped_x = [x_value for x_value, y_value in zip(x, y) if y_value > are_ceiling]
                    if clipped_x:
                        ax.plot(
                            clipped_x,
                            [are_ceiling * 0.985] * len(clipped_x),
                            color=colors["ss"],
                            marker="^",
                            markersize=4.0,
                            markerfacecolor=marker_face,
                            markeredgewidth=0.8,
                            linestyle="none",
                            clip_on=False,
                            zorder=6,
                        )
                method = resolve_method(summary, "ss[policy=difficulty")
                point = summary[method]
                point_y = display_y(metric, point[metric])
                ax.plot(
                    point["worker_mib"],
                    point_y,
                    marker="^" if metric == "are_percent" and point[metric] > are_ceiling else "D",
                    markersize=4.8,
                    markerfacecolor=colors["adaptive"],
                    markeredgecolor=colors["adaptive"],
                    markeredgewidth=0.9,
                    linestyle="none",
                    zorder=5,
                )

                methods = hybrid_methods(summary, hybrid_epsilon_values)
                points = [summary[method] for method in methods]
                if len(points) > 1:
                    plot_curve(
                        ax,
                        [point["worker_mib"] for point in points],
                        [point[metric] for point in points],
                        color=colors["hybrid"],
                        linewidth=0.9,
                        linestyle=style["linestyle"],
                        zorder=4,
                    )
                for point in points:
                    point_y = display_y(metric, point[metric])
                    ax.plot(
                        point["worker_mib"],
                        point_y,
                        marker="^" if metric == "are_percent" and point[metric] > are_ceiling else "*",
                        markersize=6.2,
                        markerfacecolor=colors["hybrid"],
                        markeredgecolor=colors["hybrid"],
                        markeredgewidth=0.9,
                        linestyle="none",
                        zorder=5,
                    )

            ax.grid(True, which="major", color="#dddddd", linewidth=0.55)
            ax.set_axisbelow(True)
            if row_index == 0:
                ax.set_title(rf"$n={n}$")
                if recall_compressed:
                    ax.set_ylim(0.0, 1.0)
                    ax.set_yticks((0.0, 0.24, 0.62, 1.0))
                    ax.set_yticklabels((f"{recall_floor:.2f}", "0.90", "0.95", "1.00"))
                    ax.text(-0.08, 0.19, "//", transform=ax.transAxes,
                            ha="center", va="center", fontsize=7)
                else:
                    ax.set_ylim(recall_floor, 1.005)
                    ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
            else:
                if are_compressed:
                    ax.set_ylim(0.0, 1.0)
                    ax.set_yticks((0.0, 0.39, 0.78, 1.0))
                    ax.set_yticklabels(("0", "5", "10", f"{are_ceiling:.0f}"))
                    ax.text(-0.08, 0.83, "//", transform=ax.transAxes,
                            ha="center", va="center", fontsize=7)
                else:
                    ax.set_ylim(0.0, are_ceiling)
                    ax.yaxis.set_major_locator(MaxNLocator(nbins=5))

    axes[0, 0].set_ylabel(r"HH recall $\uparrow$")
    axes[1, 0].set_ylabel(r"ARE (%) $\downarrow$")
    fig.supxlabel("Worker memory per partition (MiB)", y=0.02, fontsize=8)

    method_handles = [
        Line2D([], [], color=colors["ss"], marker="s", linewidth=1, label="Static SS: $n,2n,4n$"),
        Line2D([], [], color=colors["hl"], marker="o", linewidth=1.2, label="HL: $0.08n$ to $1.28n$"),
        Line2D([], [], color=colors["adaptive"], marker="D", linestyle="none", label="Adaptive SS"),
        Line2D(
            [], [], color=colors["hybrid"], marker="*", markersize=7,
            linewidth=0.9 if hybrid_epsilon_values else 0.0,
            linestyle="-" if hybrid_epsilon_values else "none",
            label=("Hybrid tail $\\epsilon_M$ sweep" if hybrid_epsilon_values else "Hybrid"),
        ),
    ]
    fig.legend(
        handles=method_handles,
        loc="upper center",
        ncol=4,
        frameon=False,
        bbox_to_anchor=(0.5, 0.99),
        columnspacing=1.05,
        handletextpad=0.45,
    )
    fig.text(
        0.5,
        0.89,
        r"Allocation order along each curve (left to right): "
        r"Static SS $n,2n,4n$; HL $0.08n,0.16n,0.32n,0.64n,1.28n$.",
        ha="center",
        va="center",
        fontsize=6.6,
        color="#333333",
    )
    fig.subplots_adjust(left=0.085, right=0.995, bottom=0.15, top=0.79, wspace=0.25, hspace=0.18)
    # The figure reserves explicit margins for its legend and shared labels.
    # Avoid bbox_inches="tight": it intermittently clips those figure-level
    # artists when the plotted placement changes.
    fig.savefig(path)
    plt.close(fig)


def draw_broken(
    path: Path,
    summaries: dict[tuple[str, int], dict[str, dict[str, float]]],
    placements: tuple[str, ...],
    hybrid_epsilon_values: tuple[float, ...],
) -> None:
    """Use adjacent axes to make a conventional, explicitly broken y scale."""
    plt.rcParams.update(
        {
            "font.size": 7.2,
            "axes.titlesize": 8.5,
            "axes.labelsize": 8,
            "legend.fontsize": 7.2,
            "xtick.labelsize": 6.5,
            "ytick.labelsize": 6.0,
            "font.family": "serif",
        }
    )
    fig = plt.figure(figsize=(7.15, 4.0))
    outer = fig.add_gridspec(
        2,
        1,
        height_ratios=(1.0, 1.0),
        hspace=0.17,
        left=0.085,
        right=0.995,
        bottom=0.14,
        top=0.82,
    )
    recall_grid = outer[0].subgridspec(1, 4, wspace=0.25)
    are_grid = outer[1].subgridspec(1, 4, wspace=0.25)
    colors = {
        "hl": "#2b6cb0",
        "ss": "#777777",
        "adaptive": "#d97706",
        "hybrid": "#b91c1c",
    }
    hl_methods = list(HL_LABELS)
    ss_methods = list(SS_LABELS)
    adversarial_recall_layout = placements[0] != "round_robin"

    def points_in_band(x, y, low, high):
        return [(xv, yv) for xv, yv in zip(x, y) if low <= yv <= high]

    def break_band(values, *, minimum_gap, padding_fraction=0.25):
        """Select the largest empty interval, leaving clear space around points."""
        ordered = sorted(set(values))
        if len(ordered) < 2:
            return None
        lower, upper = max(zip(ordered, ordered[1:]), key=lambda pair: pair[1] - pair[0])
        gap = upper - lower
        if gap < minimum_gap:
            return None
        padding = gap * padding_fraction
        return lower + padding, upper - padding

    def plot_curve(ax, x, y, low, high, *, color, linewidth, marker, size, zorder):
        # Clip each connecting segment to this axis's vertical interval.  A
        # segment that crosses a break therefore reaches the bordering spine,
        # rather than stopping at its last in-range data point.
        for index in range(1, len(x)):
            x0, y0 = x[index - 1], y[index - 1]
            x1, y1 = x[index], y[index]
            if y0 == y1:
                if low <= y0 <= high:
                    ax.plot((x0, x1), (y0, y1), color=color,
                            linewidth=linewidth, zorder=zorder)
                continue
            t0, t1 = 0.0, 1.0
            if y0 < low or y1 < low:
                t_low = (low - y0) / (y1 - y0)
                if y0 < low:
                    t0 = max(t0, t_low)
                else:
                    t1 = min(t1, t_low)
            if y0 > high or y1 > high:
                t_high = (high - y0) / (y1 - y0)
                if y0 > high:
                    t0 = max(t0, t_high)
                else:
                    t1 = min(t1, t_high)
            if t0 <= t1:
                ax.plot(
                    (x0 + t0 * (x1 - x0), x0 + t1 * (x1 - x0)),
                    (y0 + t0 * (y1 - y0), y0 + t1 * (y1 - y0)),
                    color=color,
                    linewidth=linewidth,
                    zorder=zorder,
                )
        visible = points_in_band(x, y, low, high)
        if visible:
            ax.plot([item[0] for item in visible], [item[1] for item in visible],
                    color=color, marker=marker, markersize=size,
                    markeredgewidth=0.8, linestyle="none", zorder=zorder + 1)

    def plot_panel(ax, summary, metric, low, high):
        x, y = series(summary, hl_methods, metric)
        plot_curve(ax, x, y, low, high, color=colors["hl"], linewidth=1.15,
                   marker="o", size=3.8, zorder=4)
        x, y = series(summary, ss_methods, metric)
        plot_curve(ax, x, y, low, high, color=colors["ss"], linewidth=0.95,
                   marker="s", size=3.6, zorder=2)

        adaptive = summary[resolve_method(summary, "ss[policy=difficulty")]
        if low <= adaptive[metric] <= high:
            ax.plot(adaptive["worker_mib"], adaptive[metric], marker="D",
                    markersize=4.8, markerfacecolor=colors["adaptive"],
                    markeredgecolor=colors["adaptive"], linestyle="none", zorder=6)

        hybrid = [summary[method] for method in hybrid_methods(summary, hybrid_epsilon_values)]
        x = [point["worker_mib"] for point in hybrid]
        y = [point[metric] for point in hybrid]
        plot_curve(ax, x, y, low, high, color=colors["hybrid"], linewidth=0.9,
                   marker="*", size=6.2, zorder=5)

    def marks(upper, lower):
        upper.spines["bottom"].set_visible(False)
        lower.spines["top"].set_visible(False)
        upper.tick_params(labelbottom=False, bottom=False)
        lower.tick_params(top=False)
        d = 0.012
        kwargs = dict(color="black", clip_on=False, linewidth=0.75)
        upper.plot((-d, d), (-d, d), transform=upper.transAxes, **kwargs)
        upper.plot((1 - d, 1 + d), (-d, d), transform=upper.transAxes, **kwargs)
        lower.plot((-d, d), (1 - d, 1 + d), transform=lower.transAxes, **kwargs)
        lower.plot((1 - d, 1 + d), (1 - d, 1 + d), transform=lower.transAxes, **kwargs)

    for column, n in enumerate(N_VALUES):
        summary = summaries[(placements[0], n)]
        all_methods = [
            *hl_methods,
            *ss_methods,
            resolve_method(summary, "ss[policy=difficulty"),
            *hybrid_methods(summary, hybrid_epsilon_values),
        ]
        x_values = [summary[method]["worker_mib"] for method in all_methods]
        x_low, x_high = min(x_values), max(x_values)
        x_pad = max(0.003, (x_high - x_low) * 0.07)

        recalls = [summary[method]["recall"] for method in all_methods]
        are_values = [summary[method]["are_percent"] for method in all_methods]
        recall_tick_step = 0.1 if adversarial_recall_layout else 0.05
        recall_low = (
            0.45
            if adversarial_recall_layout
            else max(0.0, floor((min(recalls) - 0.01) / recall_tick_step) * recall_tick_step)
        )
        are_low_floor = max(0.0, min(are_values) * 0.8)
        recall_band = break_band(recalls, minimum_gap=0.08)
        are_band = break_band(are_values, minimum_gap=5.0)
        recall_is_broken = recall_band is not None
        are_is_broken = are_band is not None
        if recall_band is not None:
            recall_break_low, recall_break_high = recall_band
        high_recall_focus = not any(
            0.85 <= value <= 0.90 for value in recalls
        )
        if high_recall_focus and min(recalls) < 0.85:
            # This interval is empty on both placements.  Use it to expand
            # the crowded high-recall frontier into one upper panel.
            recall_break_low, recall_break_high = 0.85, 0.90
            recall_is_broken = True
        if are_band is not None:
            are_break_low, are_break_high = are_band
        are_low_focus = are_is_broken and min(are_values) <= 5.0 and any(
            value > 5.0 for value in are_values
        )
        if are_low_focus:
            if adversarial_recall_layout:
                are_focus_upper_low = 6.5
            else:
                first_upper = min(value for value in are_values if value > 5.0)
                are_focus_upper_low = 5.0 + 0.25 * (first_upper - 5.0)
        are_high_limit = max(
            (are_break_high + 0.5) if are_band is not None else 0.0,
            max(are_values) + 1.0,
        )

        if recall_is_broken:
            recall_height_ratios = (
                (2.5, 1.25)
                if high_recall_focus
                else (1.005 - recall_break_high, recall_break_low - recall_low)
            )
            rec_parts = recall_grid[0, column].subgridspec(
                2,
                1,
                height_ratios=recall_height_ratios,
                hspace=0.04,
            )
            rec_high = fig.add_subplot(rec_parts[0, 0])
            rec_low = fig.add_subplot(rec_parts[1, 0], sharex=rec_high)
        else:
            rec_high = fig.add_subplot(recall_grid[0, column])
            rec_low = None

        if are_is_broken:
            are_height_ratios = (
                (1.25, 2.5)
                if are_low_focus
                else (are_high_limit - are_break_high, are_break_low - are_low_floor)
            )
            are_parts = are_grid[0, column].subgridspec(
                2,
                1,
                height_ratios=are_height_ratios,
                hspace=0.04,
            )
            are_high = fig.add_subplot(are_parts[0, 0], sharex=rec_high)
            are_low = fig.add_subplot(are_parts[1, 0], sharex=rec_high)
        else:
            are_high = fig.add_subplot(are_grid[0, column], sharex=rec_high)
            are_low = None
        panels = [rec_high, are_high]
        if rec_low is not None:
            panels.append(rec_low)
        if are_low is not None:
            panels.append(are_low)
        for ax in panels:
            ax.set_xlim(x_low - x_pad, x_high + x_pad)
            ax.grid(True, which="major", color="#dddddd", linewidth=0.55)
            ax.set_axisbelow(True)
        rec_high.set_title(rf"$n={n}$")

        if rec_low is not None:
            rec_high.set_ylim(recall_break_high, 1.005)
            rec_high.yaxis.set_major_locator(
                MultipleLocator(0.02 if high_recall_focus else recall_tick_step)
            )
            if high_recall_focus:
                rec_high.set_yticks((0.92, 0.94, 0.96, 0.98, 1.00))
            rec_low.set_ylim(recall_low, recall_break_low)
            rec_low.yaxis.set_major_locator(MultipleLocator(recall_tick_step))
            marks(rec_high, rec_low)
            rec_low.tick_params(labelbottom=False, bottom=False)
        else:
            rec_high.set_ylim(0.90 if high_recall_focus else recall_low, 1.005)
            rec_high.yaxis.set_major_locator(
                MultipleLocator(0.02 if high_recall_focus else recall_tick_step)
            )
            rec_high.tick_params(labelbottom=False, bottom=False)

        if are_low is not None:
            are_upper_low = are_focus_upper_low if are_low_focus else are_break_high
            are_lower_high = 5.0 if are_low_focus else are_break_low
            are_high.set_ylim(are_upper_low, are_high_limit)
            are_high.yaxis.set_major_locator(MultipleLocator(5.0))
            are_low.set_ylim(0.0 if are_low_focus else are_low_floor, are_lower_high)
            are_low.yaxis.set_major_locator(
                MultipleLocator(1.0 if are_low_focus else 5.0)
            )
            marks(are_high, are_low)
            are_high.tick_params(labelbottom=False, bottom=False)
        else:
            are_high.set_ylim(0.0, are_high_limit)
            are_high.yaxis.set_major_locator(MultipleLocator(5.0))

        for placement in placements:
            current = summaries[(placement, n)]
            if rec_low is not None:
                plot_panel(rec_high, current, "recall", recall_break_high, 1.005)
                plot_panel(rec_low, current, "recall", recall_low, recall_break_low)
            else:
                plot_panel(rec_high, current, "recall", recall_low, 1.005)
            if are_low is not None:
                are_upper_low = are_focus_upper_low if are_low_focus else are_break_high
                are_lower_high = 5.0 if are_low_focus else are_break_low
                plot_panel(are_high, current, "are_percent", are_upper_low, are_high_limit)
                plot_panel(
                    are_low,
                    current,
                    "are_percent",
                    0.0 if are_low_focus else are_low_floor,
                    are_lower_high,
                )
            else:
                are_high.set_ylim(are_low_floor, are_high_limit)
                plot_panel(are_high, current, "are_percent", are_low_floor, are_high_limit)
    fig.text(0.016, 0.65, r"HH recall $\uparrow$", rotation=90,
             ha="center", va="center", fontsize=8)
    fig.text(0.016, 0.30, r"ARE (%) $\downarrow$", rotation=90,
             ha="center", va="center", fontsize=8)
    fig.supxlabel("Worker memory per partition (MiB)", y=0.02, fontsize=8)
    fig.legend(
        handles=[
            Line2D([], [], color=colors["ss"], marker="s", linewidth=1,
                   label="Static SS: $n,2n,4n$"),
            Line2D([], [], color=colors["hl"], marker="o", linewidth=1.2,
                   label="HL: $0.08n$ to $1.28n$"),
            Line2D([], [], color=colors["adaptive"], marker="D", linestyle="none",
                   label="Adaptive SS"),
            Line2D([], [], color=colors["hybrid"], marker="*", markersize=7,
                   linestyle="-" if hybrid_epsilon_values else "none", label="Hybrid"),
        ],
        loc="upper center", ncol=4, frameon=False, bbox_to_anchor=(0.5, 0.99),
        columnspacing=1.05, handletextpad=0.45,
    )
    fig.text(0.5, 0.895,
             r"Allocation order along each curve (left to right): "
             r"Static SS $n,2n,4n$; HL $0.08n,0.16n,0.32n,0.64n,1.28n$.",
             ha="center", va="center", fontsize=6.6, color="#333333")
    fig.savefig(path)
    plt.close(fig)


def main() -> int:
    args = parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    summaries: dict[tuple[str, int], dict[str, dict[str, float]]] = {}
    placements = ("round_robin", args.adversarial_placement)
    hh_counts_by_n = load_hh_counts(args.counts, N_VALUES)
    for n in N_VALUES:
        hh_counts = hh_counts_by_n[n]
        for placement in placements:
            summaries[(placement, n)] = aggregate(
                experiment_csv(
                    args.root,
                    placement,
                    n,
                    args.comparison_dir,
                    args.direct_layout,
                ),
                hh_counts,
            )

    write_summary(args.out / "summary.csv", summaries)
    renderer = draw_broken if args.axis_mode == "broken" else draw_compressed
    for placement, filename in (
        ("round_robin", "caida_memory_quality_round_robin.pdf"),
        (args.adversarial_placement, "caida_memory_quality_visibility_cap07.pdf"),
    ):
        renderer(
            args.out / filename,
            summaries,
            (placement,),
            tuple(args.hybrid_epsilon_values),
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
