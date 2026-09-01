#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import FuncFormatter


METHODS = {
    "hybrid": ("Hybrid", "#b91c1c", "*", "-"),
    "hl#2": (r"HL $0.32n$", "#2b6cb0", "D", (0, (3.0, 1.6))),
}
M_PLOT_VALUES = {20, 50, 100, 250, 500, 1000}

METRICS = (
    ("f1", "F1↑", None),
    ("recall", "Recall↑", None),
    ("are_percent", "ARE%↓", 0.0),
    ("mem_worker_total_kib", "Part. KiB↓", 0.0),
    ("report_volume_mib", "Up MiB↓", 0.0),
)


def read_rows(path: Path, scale_field: str) -> list[dict]:
    rows = []
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            if row["placement"] != "round_robin":
                continue
            method = row["canonical_method"]
            if method not in METHODS:
                continue
            rows.append(
                {
                    "scale": int(row[scale_field]),
                    "method": method,
                    "f1": float(row["f1"]),
                    "recall": float(row["recall"]),
                    "are_percent": float(row["are_percent"]),
                    "mem_worker_total_kib": float(row["mem_worker_total_kib"]),
                    "report_volume_mib": float(row["report_volume_kib"]) / 1024.0,
                }
            )
    return rows


def padded_limits(values: list[float], floor: float | None) -> tuple[float, float]:
    low = min(values)
    high = max(values)
    span = max(high - low, max(abs(high), 1.0) * 0.08)
    bottom = low - 0.12 * span
    if floor is not None:
        bottom = max(floor, bottom)
    return bottom, high + 0.12 * span


def compact_decimal(value: float, _position: int) -> str:
    """Render axis ticks consistently with the other CAIDA figures."""
    return f"{value:.2f}".rstrip("0").rstrip(".")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--m-summary", type=Path, required=True)
    parser.add_argument("--n-summary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--linear-m-axis",
        action="store_true",
        help="Place m values at their numerical positions instead of equal spacing",
    )
    parser.add_argument(
        "--log-m-axis",
        action="store_true",
        help="Place m values on a base-10 logarithmic axis",
    )
    args = parser.parse_args()
    if args.linear_m_axis and args.log_m_axis:
        parser.error("--linear-m-axis and --log-m-axis are mutually exclusive")

    sweeps = (
        (
            "m",
            [
                row
                for row in read_rows(args.m_summary, "m")
                if row["scale"] in M_PLOT_VALUES
            ],
            r"$m$",
        ),
        ("n", read_rows(args.n_summary, "n"), r"$n$"),
    )

    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 5.9,
            "axes.titlesize": 7.0,
            "axes.labelsize": 6.1,
            "xtick.labelsize": 5.2,
            "ytick.labelsize": 5.2,
            "legend.fontsize": 5.5,
        }
    )
    fig, axes = plt.subplots(5, 2, figsize=(3.33, 3.15), constrained_layout=False)

    for col_index, (sweep_name, rows, x_label) in enumerate(sweeps):
        scales = sorted({row["scale"] for row in rows})
        x_positions = (
            {scale: scale for scale in scales}
            if sweep_name == "n" or args.linear_m_axis or args.log_m_axis
            else {scale: index for index, scale in enumerate(scales)}
        )
        for row_index, (metric, title, floor) in enumerate(METRICS):
            ax = axes[row_index][col_index]
            all_values = [row[metric] for row in rows]
            ax.set_ylim(*padded_limits(all_values, floor))
            ax.yaxis.set_major_formatter(FuncFormatter(compact_decimal))

            for method, (label, color, marker, linestyle) in METHODS.items():
                method_rows = sorted(
                    (row for row in rows if row["method"] == method),
                    key=lambda row: row["scale"],
                )
                ax.plot(
                    [x_positions[row["scale"]] for row in method_rows],
                    [row[metric] for row in method_rows],
                    color=color,
                    linestyle=linestyle,
                    marker=marker,
                    markersize=5.6 if method == "hybrid" else 3.9,
                    markerfacecolor=color,
                    markeredgecolor="#111111",
                    markeredgewidth=0.9,
                    linewidth=1.0,
                    label=label,
                    zorder=5 if method == "hybrid" else 3,
                )

            if sweep_name == "m" and args.log_m_axis:
                ax.set_xscale("log")
                ax.minorticks_off()
            ax.set_xticks([x_positions[scale] for scale in scales])
            ax.set_xticklabels(
                [
                    str(scale) if sweep_name != "n" or index % 3 == 0 else ""
                    for index, scale in enumerate(scales)
                ]
            )
            if sweep_name in {"m", "n"}:
                ax.tick_params(axis="x", labelrotation=30)
                for tick in ax.get_xticklabels():
                    tick.set_horizontalalignment("right")
            ax.grid(True, color="#dddddd", linewidth=0.35)
            ax.tick_params(direction="out", length=2, pad=1)
            if row_index == 0:
                fixed = (
                    r"$m$ sweep ($n=400$)"
                    if sweep_name == "m"
                    else r"$n$ sweep ($m=100$)"
                )
                ax.set_title(fixed, pad=2)
            if col_index == 0:
                ax.set_ylabel(title, fontsize=6.1)
                ax.yaxis.set_label_coords(-0.22, 0.5)
            if row_index != len(METRICS) - 1:
                ax.tick_params(axis="x", labelbottom=False)

    handles = [
        Line2D(
            [],
            [],
            color=color,
            linestyle=linestyle,
            marker=marker,
            markersize=5.4 if method == "hybrid" else 4.0,
            markerfacecolor=color,
            markeredgecolor="#111111",
            markeredgewidth=0.8,
            label=label,
        )
        for method, (label, color, marker, linestyle) in METHODS.items()
    ]
    fig.legend(
        handles=handles,
        loc="upper center",
        bbox_to_anchor=(0.5, 0.995),
        ncol=2,
        frameon=False,
        handlelength=1.4,
        handletextpad=0.25,
        columnspacing=0.8,
    )
    fig.subplots_adjust(
        left=0.17,
        right=0.99,
        bottom=0.09,
        top=0.84,
        wspace=0.30,
        hspace=0.28,
    )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out)
    plt.close(fig)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
