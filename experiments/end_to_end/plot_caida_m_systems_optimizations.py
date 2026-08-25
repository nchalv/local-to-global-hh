#!/usr/bin/env python3
"""Plot reducer runtime and exact-head delta effects over round-robin m scaling."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


METHODS = {
    "hl_bucketwise": ("HeavyLocker bucket-wise", "#2f6690", "D"),
    "hybrid_serial_delta": ("Hybrid serial", "#a11d2f", "*"),
    "hybrid_parallel_delta": ("Hybrid parallel", "#d97706", "o"),
}


def load(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def style() -> None:
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 8.5,
            "axes.labelsize": 9,
            "axes.titlesize": 9,
            "legend.fontsize": 8,
            "xtick.labelsize": 8,
            "ytick.labelsize": 8,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
        }
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reducer-summary", type=Path, required=True)
    parser.add_argument("--delta-summary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    reducer_rows = load(args.reducer_summary)
    delta_rows = load(args.delta_summary)
    args.out.mkdir(parents=True, exist_ok=True)
    style()

    fig, axes = plt.subplots(1, 2, figsize=(7.0, 2.55))
    for method, (label, color, marker) in METHODS.items():
        rows = sorted(
            (row for row in reducer_rows if row["method"] == method),
            key=lambda row: int(row["m"]),
        )
        x = [int(row["m"]) for row in rows]
        axes[0].plot(
            x,
            [float(row["reduce_mean_ms"]) for row in rows],
            color=color,
            marker=marker,
            markeredgecolor="#111111",
            markeredgewidth=0.6,
            linewidth=1.35,
            label=label,
        )
        axes[1].plot(
            x,
            [float(row["reduce_p95_ms"]) for row in rows],
            color=color,
            marker=marker,
            markeredgecolor="#111111",
            markeredgewidth=0.6,
            linewidth=1.35,
        )
    for axis, ylabel in zip(
        axes, ("Mean reduction time (ms)", "p95 reduction time (ms)")
    ):
        axis.set_xscale("log")
        axis.set_yscale("log")
        axis.set_xlabel("Input partitions $m$")
        axis.set_ylabel(ylabel)
        axis.grid(True, which="both", color="#dddddd", linewidth=0.55)
        axis.set_axisbelow(True)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        loc="upper center",
        ncol=3,
        frameon=False,
        bbox_to_anchor=(0.5, 1.01),
    )
    fig.subplots_adjust(
        left=0.10, right=0.995, bottom=0.20, top=0.77, wspace=0.30
    )
    fig.savefig(args.out / "m_reducer_runtime.pdf")
    fig.savefig(args.out / "m_reducer_runtime.png", dpi=220)
    plt.close(fig)

    rows = sorted(
        delta_rows,
        key=lambda row: (
            row["workload"] != "Synthetic",
            int(row["m"]),
        ),
    )
    labels = [
        "Synthetic" if row["workload"] == "Synthetic" else f"CAIDA\n$m={row['m']}$"
        for row in rows
    ]
    x = list(range(len(rows)))
    fig, axes = plt.subplots(1, 3, figsize=(7.0, 2.45))
    axes[0].bar(
        x,
        [100.0 * float(row["delta_selection_rate"]) for row in rows],
        color="#a11d2f",
        edgecolor="#111111",
        linewidth=0.5,
    )
    axes[1].bar(
        x,
        [
            float(row["delta_head_update_kib"])
            / float(row["full_head_update_kib"])
            for row in rows
        ],
        color="#d97706",
        edgecolor="#111111",
        linewidth=0.5,
    )
    axes[2].bar(
        x,
        [
            float(row["delta_apply_ms"]) / float(row["full_apply_ms"])
            for row in rows
        ],
        color="#2f6690",
        edgecolor="#111111",
        linewidth=0.5,
    )
    axes[0].set_ylabel("Delta-selected windows (%)")
    axes[1].set_ylabel("Head-update traffic ratio")
    axes[2].set_ylabel("Head-update time ratio")
    for axis in axes:
        axis.set_xticks(x, labels, rotation=45, ha="right")
        axis.grid(True, which="both", color="#dddddd", linewidth=0.55)
        axis.set_axisbelow(True)
    fig.subplots_adjust(
        left=0.09, right=0.995, bottom=0.31, top=0.96, wspace=0.38
    )
    fig.savefig(args.out / "head_delta_effect.pdf")
    fig.savefig(args.out / "head_delta_effect.png", dpi=220)
    plt.close(fig)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
