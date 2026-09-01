#!/usr/bin/env python3
"""Plot the hybrid exact-approximate promotion-policy ablation."""

from __future__ import annotations

import argparse
import csv
import math
import os
from pathlib import Path


DEFAULT_SUMMARY = Path("experiments/calibration/hybrid_ablation/summary.csv")
DEFAULT_OUT = Path("experiments/calibration/hybrid_ablation/plots")


SERIES = [
    ("ss", "baseline", "Space Saving", "#666666", "///"),
    ("hybrid", "confirmed", r"Hybrid: $K_+$", "#FFFFFF", "xx"),
    ("hybrid", "frontier", r"Hybrid: $K_+\cup K_?$", "#C8C8C8", "..."),
    ("hybrid", "topn-frontier", r"Hybrid: $\mathrm{Top}_n\cup K_+\cup K_?$", "#111111", ""),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot hybrid exact-approximate tracking ablation results."
    )
    parser.add_argument(
        "--summary",
        default=str(DEFAULT_SUMMARY),
        help="Summary CSV produced by summarize_hybrid_ablation.py.",
    )
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="Output plot directory.")
    return parser.parse_args()


def fnum(row: dict[str, str], key: str) -> float:
    value = row.get(key, "")
    if value == "":
        return math.nan
    try:
        return float(value)
    except ValueError:
        return math.nan


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def import_matplotlib():
    os.environ.setdefault("MPLCONFIGDIR", "/tmp/hh_matplotlib")
    Path(os.environ["MPLCONFIGDIR"]).mkdir(parents=True, exist_ok=True)
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch
    from matplotlib.ticker import FuncFormatter

    return plt, Patch, FuncFormatter


def family_key(dataset: str) -> tuple[int, str]:
    if dataset.startswith("milp_certificate_adversary"):
        return (0, "MILP stress")
    if dataset.startswith("caida_round_robin"):
        return (1, "CAIDA round robin")
    if dataset.startswith("round_robin"):
        return (1, "Round robin")
    return (2, dataset.replace("_", " "))


def n_label(row: dict[str, str]) -> str:
    n = fnum(row, "n")
    if math.isfinite(n):
        return f"${int(round(n))}$"
    return row.get("dataset", "").replace("_", " ")


def index_rows(rows: list[dict[str, str]]) -> dict[tuple[str, str, str], dict[str, str]]:
    indexed: dict[tuple[str, str, str], dict[str, str]] = {}
    for row in rows:
        dataset = row.get("dataset", "")
        method = row.get("method", "")
        policy = row.get("head_policy", "")
        if method == "ss":
            if policy != "baseline":
                continue
            policy = "baseline"
        indexed[(dataset, method, policy)] = row
    return indexed


def panel_data(
    rows: list[dict[str, str]],
) -> dict[str, list[str]]:
    families: dict[str, list[str]] = {}
    for row in rows:
        dataset = row.get("dataset", "")
        order, family = family_key(dataset)
        if order >= 2:
            continue
        families.setdefault(family, [])
        if dataset not in families[family]:
            families[family].append(dataset)
    for family, datasets in families.items():
        datasets.sort(key=lambda dataset: min(fnum(row, "n") for row in rows if row.get("dataset") == dataset))
    return families


def metric_value(row: dict[str, str], metric: str) -> float:
    if metric == "error":
        return fnum(row, "threshold_normalized_error")
    if metric == "memory":
        return fnum(row, "mem_worker_total_kib")
    if metric == "f1":
        return fnum(row, "hh_f1")
    raise ValueError(metric)


def add_grouped_bars(
    ax,
    *,
    datasets: list[str],
    indexed: dict[tuple[str, str, str], dict[str, str]],
    metric: str,
    ylabel: str,
    title: str,
    yformatter=None,
) -> None:
    width = min(0.22, 0.76 / max(1, len(SERIES)))
    center = (len(SERIES) - 1) / 2.0
    offsets = [(idx - center) * width for idx in range(len(SERIES))]
    xs = list(range(len(datasets)))

    for (method, policy, label, color, hatch), offset in zip(SERIES, offsets):
        values = []
        for dataset in datasets:
            row = indexed.get((dataset, method, policy))
            values.append(metric_value(row, metric) if row is not None else math.nan)
        ax.bar(
            [x + offset for x in xs],
            values,
            width=width,
            label=label,
            color=color,
            edgecolor="#222222",
            linewidth=0.8,
            hatch=hatch,
            alpha=0.92,
            zorder=3,
        )

    tick_labels = []
    for dataset in datasets:
        row = next(
            (
                indexed[key]
                for key in indexed
                if key[0] == dataset and key[1] == "ss"
            ),
            None,
        )
        tick_labels.append(n_label(row) if row else dataset.replace("_", " "))
    ax.set_xticks(xs)
    ax.set_xticklabels(tick_labels)
    if ylabel:
        ax.set_ylabel(ylabel, fontsize=11.5)
    if yformatter is not None:
        ax.yaxis.set_major_formatter(yformatter)
    if title:
        ax.set_title(title, fontsize=12, pad=7)
    ax.tick_params(axis="both", labelsize=10, length=3, width=0.8)
    ax.set_axisbelow(True)
    ax.grid(axis="y", alpha=0.28, linewidth=0.8, zorder=0)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def main() -> int:
    args = parse_args()
    summary = Path(args.summary)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = [
        row
        for row in load_rows(summary)
        if row.get("method") in {"ss", "hybrid"}
        and math.isfinite(fnum(row, "threshold_normalized_error"))
        and math.isfinite(fnum(row, "mem_worker_total_kib"))
        and math.isfinite(fnum(row, "hh_f1"))
    ]
    if not rows:
        raise RuntimeError(f"no usable hybrid ablation rows found in {summary}")

    plt, Patch, FuncFormatter = import_matplotlib()
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 11.0,
            "axes.titlesize": 12.0,
            "axes.labelsize": 11.0,
            "xtick.labelsize": 10.0,
            "ytick.labelsize": 10.0,
            "legend.fontsize": 9.0,
        }
    )
    indexed = index_rows(rows)
    families = panel_data(rows)
    ordered_families = [family for _, family in sorted({family_key(row.get("dataset", "")) for row in rows}) if family in families]

    single_family = len(ordered_families) == 1
    fig, axes = plt.subplots(
        len(ordered_families),
        3,
        figsize=(7.0, 3.08 if single_family else 5.06),
        sharex=False,
    )
    if len(ordered_families) == 1:
        axes = [axes]

    metric_titles = (
        "Threshold-normalized\nerror $\\downarrow$",
        "Mean per-partition\nmemory (KiB) $\\downarrow$",
        "HH F1 $\\uparrow$",
    )
    for family_idx, (row_axes, family) in enumerate(zip(axes, ordered_families)):
        datasets = families[family]
        add_grouped_bars(
            row_axes[0],
            datasets=datasets,
            indexed=indexed,
            metric="error",
            ylabel="",
            title=metric_titles[0] if family_idx == 0 else "",
            yformatter=FuncFormatter(
                lambda value, _: "0"
                if abs(value) < 1e-12
                else f"{value:.3f}".rstrip("0").rstrip(".")
            ),
        )
        row_axes[0].set_ylim(bottom=0.0)
        add_grouped_bars(
            row_axes[1],
            datasets=datasets,
            indexed=indexed,
            metric="memory",
            ylabel="",
            title=metric_titles[1] if family_idx == 0 else "",
        )
        add_grouped_bars(
            row_axes[2],
            datasets=datasets,
            indexed=indexed,
            metric="f1",
            ylabel="",
            title=metric_titles[2] if family_idx == 0 else "",
            yformatter=FuncFormatter(
                lambda value, _: str(int(round(value)))
                if abs(value - round(value)) < 1e-9
                else f"{value:.2f}"
            ),
        )
        f1_values = [
            metric_value(indexed[(dataset, method, policy)], "f1")
            for dataset in datasets
            for method, policy, _, _, _ in SERIES
            if (dataset, method, policy) in indexed
        ]
        f1_bottom = 0.05 * math.floor((min(f1_values) - 0.02) / 0.05)
        row_axes[2].set_ylim(bottom=max(0.0, f1_bottom), top=1.005)

    handles = [
        Patch(
            facecolor=color,
            edgecolor="#222222",
            linewidth=0.8,
            hatch=hatch,
            label=label,
        )
        for _, _, label, color, hatch in SERIES
    ]
    fig.legend(
        handles=handles,
        loc="upper center",
        bbox_to_anchor=(0.5, 0.995),
        ncol=4 if single_family else 2,
        fontsize=9.0 if single_family else 10,
        frameon=False,
        columnspacing=0.75 if single_family else 1.0,
        handletextpad=0.35 if single_family else 0.45,
    )
    fig.subplots_adjust(
        left=0.07 if single_family else 0.15,
        right=0.99,
        bottom=0.18 if single_family else 0.09,
        top=0.66 if single_family else 0.73,
        hspace=0.30,
        wspace=0.30 if single_family else 0.34,
    )
    if not single_family:
        for family_idx, family in enumerate(ordered_families):
            bounds = axes[family_idx][0].get_position()
            label = "MILP" if family == "MILP stress" else family
            fig.text(
                0.055,
                (bounds.y0 + bounds.y1) / 2,
                label,
                ha="center",
                va="center",
                rotation=90,
                fontsize=11.5,
            )

    pdf_path = out_dir / "hybrid_ablation_quality_memory.pdf"
    png_path = out_dir / "hybrid_ablation_quality_memory.png"
    fig.savefig(pdf_path)
    fig.savefig(png_path, dpi=220)
    print(f"plots written to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
