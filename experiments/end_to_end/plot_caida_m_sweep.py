#!/usr/bin/env python3
"""Plot quality/resource trajectories for the fixed-n CAIDA m sweep."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import FuncFormatter, MaxNLocator

sys.path.insert(0, str(Path(__file__).resolve().parent))
from summarize_caida_final import aggregate, load_hh_counts


M_VALUES = (20, 50, 100, 200, 250, 500)
# Keep the endpoints and one representative interior topology in the paper
# figure. All measured topologies remain present in the generated summary CSV.
PLOT_M_VALUES = (20, 100, 500)
M_VALUES_BY_PLACEMENT = {
    "round_robin": (*M_VALUES, 1000),
    "visibility_suppression": M_VALUES,
}
PLACEMENTS = {
    "round_robin": ("round_robin", "Round-robin placement"),
    "visibility_suppression": (
        "visibility_suppression",
        "Visibility-suppression placement",
    ),
}
METHODS = {
    "hl": {
        "label": r"HL $0.08n$",
        "allocation": r"$0.08n$",
        "color": "#9ec5e5",
        "marker": "o",
    },
    "hl#1": {
        "label": r"HL $0.16n$",
        "allocation": r"$0.16n$",
        "color": "#6da9d2",
        "marker": "s",
    },
    "hl#2": {
        "label": r"HL $0.32n$",
        "allocation": r"$0.32n$",
        "color": "#2b6cb0",
        "marker": "D",
    },
    "hl#3": {
        "label": r"HL $0.64n$",
        "allocation": r"$0.64n$",
        "color": "#174a7e",
        "marker": "^",
    },
    "ss[policy=static q=2n]": {
        "label": r"Static SS $2n$",
        "allocation": r"$2n$",
        "color": "#999999",
        "marker": "P",
    },
    "ss[policy=static q=4n]": {
        "label": r"Static SS $4n$",
        "allocation": r"$4n$",
        "color": "#555555",
        "marker": "X",
    },
    "ss[policy=difficulty]": {
        "label": "Adaptive SS",
        "allocation": "Adaptive SS",
        "color": "#d97706",
        "marker": "o",
    },
    "hybrid": {
        "label": "Hybrid",
        "allocation": "Hybrid",
        "color": "#b91c1c",
        "marker": "*",
    },
}
HL_METHODS = ("hl", "hl#1", "hl#2", "hl#3")
STATIC_METHODS = ("ss[policy=static q=2n]", "ss[policy=static q=4n]")
METRICS = (
    ("f1", r"HH F1 $\uparrow$"),
    ("recall", r"HH recall $\uparrow$"),
    ("are_percent", r"ARE (%) $\downarrow$"),
)


def consistent_decimal_formatter(axis) -> FuncFormatter:
    """Use one fractional precision per axis, leaving integers unadorned."""

    def format_tick(value: float, _position: int) -> str:
        if abs(value - round(value)) < 1e-9:
            return str(int(round(value)))
        ticks = axis.get_majorticklocs()
        precision = 1
        if any(
            abs(tick - round(tick)) >= 1e-9
            and abs(tick * 10.0 - round(tick * 10.0)) >= 1e-8
            for tick in ticks
        ):
            precision = 2
        return f"{value:.{precision}f}"

    return FuncFormatter(format_tick)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path("experiments/end_to_end/caida_holdout_m_sweep"),
    )
    parser.add_argument(
        "--counts",
        type=Path,
        default=Path(
            "data_preparation/generated/"
            "caida_20180315_133230_200w_5s_dirA.jsonl"
        ),
    )
    parser.add_argument("--n", type=int, default=400)
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("experiments/end_to_end/caida_holdout_m_sweep/plots"),
    )
    parser.add_argument(
        "--single-column",
        action="store_true",
        help="Render a compact square-panel layout at ACM single-column width",
    )
    parser.add_argument(
        "--no-legend",
        action="store_true",
        help="Omit the legend when a shared LaTeX legend is used",
    )
    parser.add_argument(
        "--panel-aspect",
        type=float,
        default=0.46,
        help="Subplot height/width ratio in single-column mode",
    )
    parser.add_argument(
        "--omit-hl-008",
        action="store_true",
        help="Omit the HeavyLocker 0.08n point from rendered configuration curves",
    )
    parser.add_argument(
        "--omit-static-ss",
        action="store_true",
        help="Omit fixed-capacity Space-Saving points from rendered configuration curves",
    )
    return parser.parse_args()


def canonical_method(method: str) -> str | None:
    if method.startswith("hybrid["):
        return "hybrid"
    return method if method in METHODS else None


def load_results(args: argparse.Namespace) -> list[dict[str, object]]:
    hh_counts = load_hh_counts(args.counts, (args.n,))[args.n]
    results: list[dict[str, object]] = []
    for placement, (directory_prefix, _) in PLACEMENTS.items():
        for m in M_VALUES_BY_PLACEMENT[placement]:
            path = (
                args.root
                / f"{directory_prefix}_m{m}"
                / "csv"
                / f"caida_round_robin_n{args.n}_all_methods.csv"
            )
            if not path.exists():
                raise FileNotFoundError(f"missing m-sweep result: {path}")
            for row in aggregate(path, hh_counts):
                method = canonical_method(str(row["method"]))
                if method is not None:
                    results.append(
                        {
                            "placement": placement,
                            "m": m,
                            "canonical_method": method,
                            **row,
                        }
                    )
    return results


def write_summary(path: Path, results: list[dict[str, object]]) -> None:
    fields = (
        "placement",
        "m",
        "canonical_method",
        "method",
        "windows",
        "precision",
        "recall",
        "f1",
        "candidate_hh_recall",
        "certified_hh_coverage",
        "ambiguous_mass",
        "aae",
        "are_percent",
        "mem_worker_total_kib",
        "report_volume_kib",
        "control_volume_kib",
        "total_communication_kib",
        "mem_coord_peak_kib",
        "q_current",
        "q_head_current",
        "q_tail_current",
    )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(
            {field: row.get(field, "") for field in fields} for row in results
        )


def plot_placement(
    path: Path,
    placement: str,
    title: str,
    results: list[dict[str, object]],
    resource: str,
    resource_label: str,
    resource_name: str,
    resource_divisor: float = 1.0,
    row_key: str = "m",
    row_values: tuple[int, ...] = M_VALUES,
    row_symbol: str = "m",
    single_column: bool = False,
    panel_aspect: float = 1.0,
    hl_methods: tuple[str, ...] = HL_METHODS,
    include_static: bool = True,
    show_legend: bool = True,
) -> None:
    base_font = 5.4 if single_column else 7.0
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": base_font,
            "axes.titlesize": 6.5 if single_column else 8.2,
            "axes.labelsize": 5.6 if single_column else 7.2,
            "xtick.labelsize": 4.8 if single_column else 6.1,
            "ytick.labelsize": 4.8 if single_column else 6.1,
            "legend.fontsize": 5.0 if single_column else 7.0,
        }
    )
    figure_width = 3.33 if single_column else 7.15
    figure_height = (
        0.88 * panel_aspect * len(row_values) + 0.72
        if single_column
        else 1.3 * len(row_values) + 0.95
    )
    fig, axes = plt.subplots(
        len(row_values),
        3,
        figsize=(figure_width, figure_height),
        squeeze=False,
    )
    placement_rows = [row for row in results if row["placement"] == placement]

    def nondominated_curve(
        points: list[tuple[float, float, str]], maximize_quality: bool
    ) -> list[tuple[float, float, str]]:
        curve: list[tuple[float, float, str]] = []
        for candidate in points:
            x_value, y_value, _ = candidate
            dominated = False
            for other in points:
                if other is candidate:
                    continue
                other_x, other_y, _ = other
                quality_no_worse = (
                    other_y >= y_value
                    if maximize_quality
                    else other_y <= y_value
                )
                quality_strict = (
                    other_y > y_value
                    if maximize_quality
                    else other_y < y_value
                )
                if (
                    other_x <= x_value
                    and quality_no_worse
                    and (other_x < x_value or quality_strict)
                ):
                    dominated = True
                    break
            if not dominated:
                curve.append(candidate)
        return sorted(curve, key=lambda point: point[0])

    for row_index, row_value in enumerate(row_values):
        scale_rows = [
            row for row in placement_rows if int(row[row_key]) == row_value
        ]
        by_method = {
            str(row["canonical_method"]): row for row in scale_rows
        }
        for column_index, (metric, metric_label) in enumerate(METRICS):
            ax = axes[row_index, column_index]
            hl_points = [
                (
                    float(by_method[method][resource]) / resource_divisor,
                    float(by_method[method][metric]),
                    method,
                )
                for method in hl_methods
            ]
            hybrid_point = (
                float(by_method["hybrid"][resource]) / resource_divisor,
                float(by_method["hybrid"][metric]),
                "hybrid",
            )
            static_points = [
                (
                    float(by_method[method][resource]) / resource_divisor,
                    float(by_method[method][metric]),
                    method,
                )
                for method in STATIC_METHODS if include_static
            ]
            adaptive_point = (
                float(by_method["ss[policy=difficulty]"][resource]) / resource_divisor,
                float(by_method["ss[policy=difficulty]"][metric]),
                "ss[policy=difficulty]",
            )
            all_points = [
                *hl_points,
                *static_points,
                adaptive_point,
                hybrid_point,
            ]

            # The thin blue trajectory exposes the full HeavyLocker width sweep.
            ax.plot(
                [point[0] for point in hl_points],
                [point[1] for point in hl_points],
                color="#2b6cb0",
                linewidth=0.75,
                alpha=0.55,
                zorder=1,
            )
            ax.plot(
                [point[0] for point in static_points],
                [point[1] for point in static_points],
                color="#777777",
                linestyle=(0, (2.0, 1.4)),
                linewidth=0.9,
                zorder=3,
            )
            # Hybrid is an isolated comparison point. Configuration-curve segments connect
            # only the ordered HeavyLocker capacity configurations.
            curve = nondominated_curve(
                hl_points, maximize_quality=metric in {"f1", "recall"}
            )
            if len(curve) > 1:
                ax.plot(
                    [point[0] for point in curve],
                    [point[1] for point in curve],
                    color="#222222",
                    linewidth=0.9 if single_column else 1.15,
                    zorder=2,
                )

            curve_methods = {point[2] for point in curve}
            for x_value, y_value, method in all_points:
                style = METHODS[method]
                is_on_curve = method in curve_methods
                emphasize = is_on_curve or method not in hl_methods
                ax.plot(
                    x_value,
                    y_value,
                    marker=style["marker"],
                    markersize=(
                        5.2 if method == "hybrid" else 3.5
                    ) if single_column else (
                        6.8 if method == "hybrid" else 4.8
                    ),
                    markerfacecolor=style["color"],
                    markeredgecolor="#111111" if emphasize else "#ffffff",
                    markeredgewidth=0.9 if emphasize else 0.7,
                    alpha=1.0 if emphasize else 0.55,
                    linestyle="none",
                    clip_on=True,
                    zorder=4 if emphasize else 3,
                )

            if row_index == 0:
                ax.set_title(metric_label, pad=2 if single_column else 4)
            if row_index == len(row_values) - 1 and not single_column:
                ax.set_xlabel(resource_label)
            if column_index == 0:
                label = (
                    rf"${row_symbol}={row_value}$"
                    if single_column
                    else rf"${row_symbol}={row_value}$" + "\n" + metric_label
                )
                ax.set_ylabel(label, labelpad=2 if single_column else 4)
            ax.grid(
                True,
                color="#dddddd",
                linewidth=0.35 if single_column else 0.5,
            )
            # Keep marker edges and star tips clear of the axes frame.
            ax.margins(x=0.18, y=0.28)
            if metric in {"f1", "recall"}:
                # Quality is bounded by 1. Scale each panel around its displayed
                # range, reserving only enough room to keep marker edges clear
                # of the top and bottom spines. The minimum span avoids a
                # degenerate axis when every displayed value is perfect.
                quality_min = min(point[1] for point in all_points)
                quality_span = max(1.0 - quality_min, 0.005)
                marker_clearance = 0.10 * quality_span
                quality_bottom = max(
                    0.0, 1.0 - quality_span - marker_clearance
                )
                quality_top = 1.0 + marker_clearance
                ax.set_ylim(bottom=quality_bottom, top=quality_top)
            if single_column:
                ax.set_box_aspect(panel_aspect)
                ax.xaxis.set_major_locator(MaxNLocator(nbins=3))
                if metric not in {"f1", "recall"}:
                    ax.yaxis.set_major_locator(MaxNLocator(nbins=4))
                ax.tick_params(axis="both", length=2, pad=1)
            if metric in {"f1", "recall"}:
                tick_span = 1.0 - quality_bottom
                quality_step = next(
                    step
                    for step in (0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5)
                    if tick_span / step <= 4.0
                )
                quality_ticks = [1.0]
                value = 1.0 - quality_step
                while value >= quality_bottom - 1e-9:
                    quality_ticks.append(value)
                    value -= quality_step
                if len(quality_ticks) == 1:
                    lower_tick = 1.0 - quality_step
                    quality_ticks.append(lower_tick)
                    tick_clearance = 0.10 * quality_step
                    quality_bottom = max(
                        0.0, min(quality_bottom, lower_tick - tick_clearance)
                    )
                    quality_top = max(
                        quality_top, 1.0 + tick_clearance
                    )
                    ax.set_ylim(bottom=quality_bottom, top=quality_top)
                ax.set_yticks(sorted(quality_ticks))
            ax.yaxis.set_major_formatter(consistent_decimal_formatter(ax.yaxis))

    handles = [
        Line2D(
            [],
            [],
            color=style["color"],
            linestyle="none",
            marker=style["marker"],
            markersize=(
                5.0 if style["marker"] == "*" else 3.7
            ) if single_column else (
                6.2 if style["marker"] == "*" else 4.8
            ),
            markerfacecolor=style["color"],
            markeredgecolor="#111111",
            markeredgewidth=0.8,
            label=style["label"],
        )
        for method, style in METHODS.items()
        if method != "hl" or "hl" in hl_methods
        if include_static or method not in STATIC_METHODS
    ]
    if show_legend:
        fig.legend(
            handles=handles,
            loc="upper center",
            bbox_to_anchor=(
                (0.06, 1.10, 0.94, 0.0) if single_column else (0.5, 0.965)
            ),
            ncol=4 if single_column else 4,
            mode="expand" if single_column else None,
            frameon=False,
            columnspacing=0.24 if single_column else 0.8,
            handletextpad=0.12 if single_column else 0.3,
            borderaxespad=0,
        )
    if single_column:
        fig.supxlabel(resource_label, y=-0.025, fontsize=5.6)
        fig.subplots_adjust(
            left=0.06,
            right=1.0,
            top=0.88,
            bottom=0.095,
            hspace=0.12,
            wspace=0.28,
        )
    else:
        fig.suptitle(
            f"{title}: {resource_name} configuration curves",
            y=0.995,
            fontsize=9,
        )
        fig.text(
            0.5,
            0.008,
            f"Black segments mark the non-dominated HL {resource_name.lower()} "
            "quality configuration curve; grey dashes connect static SS capacities. "
            "Adaptive SS and Hybrid are unconnected points.",
            ha="center",
            va="bottom",
            fontsize=6.2,
            color="#333333",
        )
        fig.subplots_adjust(
            left=0.09,
            right=0.985,
            top=0.875,
            bottom=0.085,
            hspace=0.30,
            wspace=0.30,
        )
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    args = parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    results = load_results(args)
    write_summary(args.out / "m_sweep_summary.csv", results)
    hl_methods = HL_METHODS[1:] if args.omit_hl_008 else HL_METHODS
    for placement, (_, title) in PLACEMENTS.items():
        plot_placement(
            args.out / f"caida_m_sweep_{placement}.pdf",
            placement,
            title,
            results,
            "mem_worker_total_kib",
            r"Mean per-partition memory (KiB) $\downarrow$",
            "Per-partition-memory",
            row_values=PLOT_M_VALUES,
            single_column=args.single_column,
            panel_aspect=args.panel_aspect,
            hl_methods=hl_methods,
            include_static=not args.omit_static_ss,
            show_legend=not args.no_legend,
        )
        plot_placement(
            args.out / f"caida_m_sweep_{placement}_worker_memory.pdf",
            placement,
            title,
            results,
            "mem_worker_total_kib",
            r"Mean per-partition memory (MiB) $\downarrow$",
            "Per-partition memory",
            resource_divisor=1.0,
            row_values=PLOT_M_VALUES,
            single_column=args.single_column,
            panel_aspect=args.panel_aspect,
            hl_methods=hl_methods,
            include_static=not args.omit_static_ss,
            show_legend=not args.no_legend,
        )
    print(f"Wrote m-sweep summary and figures to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
