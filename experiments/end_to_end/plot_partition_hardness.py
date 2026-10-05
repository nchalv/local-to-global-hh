#!/usr/bin/env python3
"""Plot partition-local structure from the matched-window diagnostic summary."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


PLACEMENTS = {
    "round_robin": ("Round robin", "#2f6690", "o", "-"),
    "filter_aware_threshold07": ("Visibility suppression", "#a11d2f", "s", "--"),
    "visibility_suppression": ("Visibility suppression", "#a11d2f", "s", "--"),
    "visibility_07": ("Visibility suppression", "#555555", "D", "--"),
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    with args.summary.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise SystemExit("partition-hardness summary is empty")

    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 9,
            "axes.labelsize": 9,
            "legend.fontsize": 8,
            "xtick.labelsize": 8,
            "ytick.labelsize": 8,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
        }
    )
    fig, axes = plt.subplots(1, 2, figsize=(7.0, 2.55))
    for placement in sorted({row["placement"] for row in rows}):
        label, color, marker, linestyle = PLACEMENTS.get(
            placement, (placement.replace("_", " ").title(), "#555555", "D", "-")
        )
        selected = sorted(
            (row for row in rows if row["placement"] == placement),
            key=lambda row: int(row["n"]),
        )
        n_values = [int(row["n"]) for row in selected]
        axes[0].plot(
            n_values,
            [float(row["local_cardinality_mean_mean"]) for row in selected],
            label=label,
            color=color,
            marker=marker,
            markerfacecolor="white",
            markeredgecolor=color,
            linestyle=linestyle,
            linewidth=1.35,
        )
        for multiplier, alpha in ((1, 1.0), (2, 0.68), (4, 0.42)):
            axes[1].plot(
                n_values,
                [
                    100.0 * float(row[f"tail_mass_beyond_{multiplier}n_fraction_mean"])
                    for row in selected
                ],
                color=color,
                linestyle=linestyle,
                marker=marker,
                markerfacecolor="white",
                markeredgecolor=color,
                linewidth=1.35,
                alpha=alpha,
                label=f"{label}, q={multiplier}n",
            )

    axes[0].set_xlabel("Heavy-hitter denominator $n$")
    axes[0].set_ylabel("Mean distinct keys per partition")
    axes[1].set_xlabel("Heavy-hitter denominator $n$")
    axes[1].set_ylabel("Residual mass beyond local top-$q$ (%)")
    for axis in axes:
        axis.grid(True, color="#dddddd", linewidth=0.55)
        axis.set_axisbelow(True)
    axes[0].legend(frameon=False, loc="best")
    axes[1].legend(frameon=False, ncol=2, fontsize=7, loc="best")
    fig.subplots_adjust(left=0.09, right=0.995, bottom=0.19, top=0.97, wspace=0.30)

    args.out.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out / "partition_hardness.pdf")
    fig.savefig(args.out / "partition_hardness.png", dpi=220)
    plt.close(fig)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
