#!/usr/bin/env python3
"""Plot per-method CAIDA-B error and per-partition-memory trajectories."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D


PLACEMENTS = ("round_robin", "visibility_suppression")
METHODS = {
    "hl#2": (r"HeavyLocker ($w=0.32n$)", "#2f6690"),
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


def legend_handles(color: str) -> list[Line2D]:
    return [
        Line2D(
            [],
            [],
            color=color,
            linestyle=linestyle,
            linewidth=1.25,
            label=label,
        )
        for label, linestyle in (
            (r"ARE $\downarrow$", "-"),
            (r"Per-partition memory $\downarrow$", "--"),
        )
    ]


def plot_trajectory(
    path: Path,
    rows: list[dict[str, float]],
    *,
    title: str,
    color: str,
    are_limits: tuple[float, float],
    memory_limits: tuple[float, float],
) -> None:
    fig, axis = plt.subplots(figsize=(3.33, 1.62))
    worker_axis = axis.twinx()

    windows = [int(row["window"]) for row in rows]
    axis.plot(
        windows,
        [row["are_percent"] for row in rows],
        color=color,
        linewidth=0.9,
        alpha=0.82,
        zorder=3,
    )
    worker_axis.plot(
        windows,
        [row["worker_kib"] for row in rows],
        color=color,
        linestyle="--",
        linewidth=0.9,
        alpha=0.82,
        zorder=2,
    )

    axis.set_xlim(1, 199)
    axis.set_ylim(*are_limits)
    worker_axis.set_ylim(*memory_limits)
    axis.set_ylabel("ARE (%)")
    worker_axis.set_ylabel("Per-partition KiB")
    axis.set_xlabel("Window")
    fig.suptitle(title, y=0.98)
    axis.grid(axis="y", color="#d6d6d6", linewidth=0.5, alpha=0.8)
    axis.tick_params(direction="out", length=2.5, pad=1.5)
    worker_axis.tick_params(direction="out", length=2.5, pad=1.5)

    fig.legend(
        handles=legend_handles(color),
        loc="upper center",
        bbox_to_anchor=(0.5, 0.84),
        ncol=2,
        frameon=False,
        columnspacing=0.9,
        handlelength=1.8,
        handletextpad=0.35,
    )
    fig.subplots_adjust(
        left=0.15,
        right=0.84,
        bottom=0.24,
        top=0.68,
    )

    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, bbox_inches="tight")
    fig.savefig(path.with_suffix(".png"), bbox_inches="tight", dpi=220)
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

    placement_titles = {
        "round_robin": "Round robin",
        "visibility_suppression": "Visibility suppression",
    }
    for placement in PLACEMENTS:
        rows_by_method = load_rows(args.root, placement, args.n)
        all_rows = [row for rows in rows_by_method.values() for row in rows]
        max_are = max(row["are_percent"] for row in all_rows)
        memory_values = [row["worker_kib"] for row in all_rows]
        memory_span = max(memory_values) - min(memory_values)
        memory_padding = max(1.0, 0.05 * memory_span)
        are_limits = (0.0, 1.05 * max_are)
        memory_limits = (
            min(memory_values) - memory_padding,
            max(memory_values) + memory_padding,
        )
        for method, (method_label, color) in METHODS.items():
            method_slug = "heavylocker" if method.startswith("hl") else "hybrid"
            path = args.out.with_name(
                f"{args.out.stem}_{placement}_{method_slug}.pdf"
            )
            plot_trajectory(
                path,
                rows_by_method[method],
                title=f"{placement_titles[placement]}: {method_label}",
                color=color,
                are_limits=are_limits,
                memory_limits=memory_limits,
            )
            print(f"Wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
