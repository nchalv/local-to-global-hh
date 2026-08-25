#!/usr/bin/env python3
"""Plot held-out CAIDA error and worker-memory trajectories."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D


PLACEMENTS = ("round_robin", "visibility_suppression")
METHODS = {
    "hl#2": (r"HL $0.32n$", "#2f6690"),
    "hybrid[head=topn-frontier reducer=streaming]": ("Hybrid", "#a11d2f"),
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
            "caida_stepwise_error_memory.pdf"
        ),
    )
    return parser.parse_args()


def load_rows(root: Path, placement: str, n: int) -> dict[str, list[dict[str, float]]]:
    path = (
        root
        / f"{placement}_n{n}"
        / "csv"
        / f"caida_round_robin_n{n}_all_methods.csv"
    )
    result = {method: [] for method in METHODS}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            method = row["method"]
            window = int(row["window"])
            if method not in result or window == 0:
                continue
            result[method].append(
                {
                    "window": window,
                    "are_percent": 100.0 * float(row["are"]),
                    "worker_kib": float(row["mem_worker_total_kib"]),
                }
            )
    for method, rows in result.items():
        rows.sort(key=lambda row: row["window"])
        if not rows:
            raise ValueError(f"missing {method!r} rows in {path}")
    return result


def legend_handles() -> list[Line2D]:
    handles = [
        Line2D([], [], color=color, linewidth=1.25, label=label)
        for label, color in METHODS.values()
    ]
    handles.extend(
        Line2D(
            [],
            [],
            color="#333333",
            linestyle=linestyle,
            linewidth=1.25,
            label=label,
        )
        for label, linestyle in (
            (r"ARE $\downarrow$", "-"),
            (r"Worker memory $\downarrow$", "--"),
        )
    )
    return handles


def plot_placement(
    path: Path,
    rows_by_method: dict[str, list[dict[str, float]]],
    *,
    show_legend: bool,
    show_xlabel: bool,
) -> None:
    fig, axis = plt.subplots(figsize=(3.33, 1.24 if show_legend else 1.05))
    worker_axis = axis.twinx()

    for method, (_, color) in METHODS.items():
        rows = rows_by_method[method]
        windows = [int(row["window"]) for row in rows]
        axis.plot(
            windows,
            [row["are_percent"] for row in rows],
            color=color,
            linewidth=0.9,
            alpha=0.78,
            zorder=3,
        )
        worker_axis.plot(
            windows,
            [row["worker_kib"] for row in rows],
            color=color,
            linestyle="--",
            linewidth=0.9,
            alpha=0.78,
            zorder=2,
        )

    axis.set_xlim(1, 199)
    axis.set_ylabel("ARE (%)")
    worker_axis.set_ylabel("Worker KiB")
    if show_xlabel:
        axis.set_xlabel("Window")
    axis.grid(axis="y", color="#d6d6d6", linewidth=0.5, alpha=0.8)
    axis.tick_params(direction="out", length=2.5, pad=1.5)
    worker_axis.tick_params(direction="out", length=2.5, pad=1.5)

    if show_legend:
        fig.legend(
            handles=legend_handles(),
            loc="upper center",
            bbox_to_anchor=(0.5, 0.995),
            ncol=4,
            frameon=False,
            columnspacing=0.65,
            handlelength=1.65,
            handletextpad=0.3,
        )
    fig.subplots_adjust(
        left=0.15,
        right=0.84,
        bottom=0.24 if show_xlabel else 0.17,
        top=0.63 if show_legend else 0.89,
    )

    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    args = parse_args()
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 6.6,
            "axes.labelsize": 7,
            "xtick.labelsize": 6.2,
            "ytick.labelsize": 6.2,
            "legend.fontsize": 5.9,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
        }
    )

    for placement in PLACEMENTS:
        path = args.out.with_name(f"{args.out.stem}_{placement}.pdf")
        plot_placement(
            path,
            load_rows(args.root, placement, args.n),
            show_legend=placement == "round_robin",
            show_xlabel=placement == "visibility_suppression",
        )
        print(f"Wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
