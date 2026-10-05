#!/usr/bin/env python3
"""Render the supplementary visibility-suppression margin sweep from summary CSV."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


METHODS = {
    "Adaptive SS": ("#666666", "o", 0.15),
    "Hybrid": ("#111111", "*", 0.40),
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    with args.summary.open(newline="", encoding="utf-8") as handle:
        rows = [
            row
            for row in csv.DictReader(handle)
            if row["placement"] != "round_robin"
        ]
    n_values = sorted({int(row["n"]) for row in rows})
    if not rows or not n_values:
        raise SystemExit("summary contains no visibility-suppression rows")

    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 8,
            "axes.labelsize": 8.5,
            "axes.titlesize": 9,
            "legend.fontsize": 8,
            "xtick.labelsize": 7.5,
            "ytick.labelsize": 7.5,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
        }
    )
    fig, axes = plt.subplots(2, len(n_values), figsize=(11.0, 4.5), squeeze=False)
    for column, n in enumerate(n_values):
        for method, (color, marker, selected_epsilon) in METHODS.items():
            selected = sorted(
                (row for row in rows if int(row["n"]) == n and row["method"] == method),
                key=lambda row: float(row["worker_mib"]),
            )
            x = [float(row["worker_mib"]) for row in selected]
            for axis, field in ((axes[0, column], "f1"), (axes[1, column], "normalized_error")):
                axis.plot(x, [float(row[field]) for row in selected], color=color, linewidth=1.1)
                for row in selected:
                    chosen = abs(float(row["epsilon_m"]) - selected_epsilon) < 1e-9
                    axis.plot(
                        float(row["worker_mib"]),
                        float(row[field]),
                        marker=marker,
                        markersize=7 if chosen else 4,
                        markerfacecolor=color if chosen else "white",
                        markeredgecolor="black",
                        markeredgewidth=0.65,
                        linestyle="none",
                        label=method if column == 0 and field == "f1" and chosen else None,
                    )
        axes[0, column].set_title(f"$n={n}$")
        axes[1, column].set_xlabel("Mean per-partition memory (MiB)")
        for axis in axes[:, column]:
            axis.grid(True, color="#dddddd", linewidth=0.5)
            axis.set_axisbelow(True)
    axes[0, 0].set_ylabel("HH F1")
    axes[1, 0].set_ylabel("Threshold-normalized error")
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, frameon=False, bbox_to_anchor=(0.5, 1.01))
    fig.subplots_adjust(left=0.065, right=0.995, bottom=0.14, top=0.86, hspace=0.34, wspace=0.28)
    args.out.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out / "margin_visibility.pdf")
    fig.savefig(args.out / "margin_visibility.png", dpi=220)
    plt.close(fig)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
