#!/usr/bin/env python3
"""Plot per-window CAIDA quality and per-partition-memory distributions."""

from __future__ import annotations

import argparse
import csv
import statistics
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D


PLACEMENTS = {
    "round_robin": "Round robin",
    "visibility_suppression": "Visibility suppression",
}
METHODS = {
    "hl#2": (r"HL $0.32n$", "#2f6690", (0, (5.0, 2.6)), "s"),
    "hybrid[head=topn-frontier reducer=streaming]": ("Hybrid", "#a11d2f", "-", "o"),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path("experiments/end_to_end/caida_holdout_n_sweep"),
    )
    parser.add_argument("--n", type=int, default=600)
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(
            "experiments/end_to_end/caida_holdout_n_sweep/plots/"
            "caida_window_cdfs.pdf"
        ),
    )
    return parser.parse_args()


def load_values(
    root: Path, placement: str, n: int
) -> dict[str, dict[str, list[float]]]:
    path = (
        root
        / f"{placement}_n{n}"
        / "csv"
        / f"caida_round_robin_n{n}_all_methods.csv"
    )
    result = {
        method: {"are_percent": [], "worker_kib": []}
        for method in METHODS
    }
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            method = row["method"]
            if method not in result or int(row["window"]) == 0:
                continue
            result[method]["are_percent"].append(100.0 * float(row["are"]))
            result[method]["worker_kib"].append(
                float(row["mem_worker_total_kib"])
            )
    for method, metrics in result.items():
        for metric, values in metrics.items():
            if not values:
                raise ValueError(f"missing {method!r} {metric} values in {path}")
    return result


def draw_ecdf(
    axis: plt.Axes,
    values: list[float],
    color: str,
    linestyle: str | tuple,
    marker: str,
) -> None:
    ordered = sorted(values)
    fractions = [(index + 1) / len(ordered) for index in range(len(ordered))]
    axis.step(
        ordered,
        fractions,
        where="post",
        color=color,
        linestyle=linestyle,
        linewidth=1.25,
    )
    median = statistics.median(ordered)
    axis.vlines(
        median,
        0.0,
        0.5,
        color=color,
        linestyle=":",
        linewidth=0.8,
        alpha=0.9,
    )
    axis.plot(
        [median],
        [0.5],
        marker=marker,
        markersize=2.8,
        markerfacecolor="white",
        markeredgecolor=color,
        markeredgewidth=0.8,
        zorder=3,
    )


def main() -> int:
    args = parse_args()
    data = {
        placement: load_values(args.root, placement, args.n)
        for placement in PLACEMENTS
    }
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 6.5,
            "axes.labelsize": 6.8,
            "axes.titlesize": 7,
            "xtick.labelsize": 6,
            "ytick.labelsize": 6,
            "legend.fontsize": 6.2,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
        }
    )

    fig, axes = plt.subplots(
        2,
        2,
        figsize=(3.33, 1.92),
        sharey=True,
        constrained_layout=False,
    )
    metrics = (
        ("are_percent", ""),
        ("worker_kib", ""),
    )
    for column, (placement, placement_label) in enumerate(PLACEMENTS.items()):
        axes[0, column].set_title(placement_label, pad=2.5)
        for row, (metric, xlabel) in enumerate(metrics):
            axis = axes[row, column]
            for method, (_, color, linestyle, marker) in METHODS.items():
                draw_ecdf(
                    axis,
                    data[placement][method][metric],
                    color,
                    linestyle,
                    marker,
                )
            axis.set_ylim(0.0, 1.0)
            axis.set_xlabel(xlabel, labelpad=1.5)
            axis.set_box_aspect(0.34)
            axis.grid(color="#d8d8d8", linewidth=0.45, alpha=0.8)
            axis.tick_params(direction="out", length=2.3, pad=1.3)

    fig.supylabel("Window fraction", x=0.02, fontsize=6.8)
    fig.supxlabel(
        "Per-partition memory (KiB)", y=0.060, fontsize=6.8
    )

    fig.legend(
        handles=[
            Line2D(
                [], [],
                color=color,
                linestyle=linestyle,
                marker=marker,
                markersize=3.2,
                markerfacecolor="white",
                markeredgecolor=color,
                linewidth=1.3,
                label=label,
            )
            for label, color, linestyle, marker in METHODS.values()
        ],
        loc="upper center",
        bbox_to_anchor=(0.5, 0.995),
        ncol=2,
        frameon=False,
        columnspacing=1.0,
        handlelength=2.5,
        handletextpad=0.35,
    )
    fig.subplots_adjust(
        left=0.14,
        right=0.985,
        bottom=0.16,
        top=0.83,
        wspace=0.28,
        hspace=0.34,
    )
    plot_center = (
        axes[0, 0].get_position().x0
        + axes[0, 1].get_position().x1
    ) / 2.0
    row_gap_center = (
        axes[0, 0].get_position().y0
        + axes[1, 0].get_position().y1
    ) / 2.0
    fig.text(
        plot_center,
        row_gap_center - 0.030,
        "Average relative error (%)",
        ha="center",
        va="center",
        fontsize=6.8,
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
