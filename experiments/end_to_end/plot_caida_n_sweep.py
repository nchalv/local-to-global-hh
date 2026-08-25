#!/usr/bin/env python3
"""Plot quality/resource frontiers for the fixed-m CAIDA n sweep."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from plot_caida_m_sweep import HL_METHODS, canonical_method, plot_placement
from summarize_caida_final import aggregate, load_hh_counts


N_VALUES = (200, 400, 600, 800, 1000)
PLOT_N_VALUES = (200, 600, 1000)
N_VALUES_BY_PLACEMENT = {
    "round_robin": (*N_VALUES, 1200, 1400, 1600, 1800, 2000),
    "visibility_suppression": N_VALUES,
}
PLACEMENTS = {
    "round_robin": ("round_robin", "Round-robin placement"),
    "visibility_suppression": (
        "visibility_suppression",
        "Visibility-suppression placement",
    ),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path("experiments/end_to_end/caida_holdout_n_sweep"),
    )
    parser.add_argument(
        "--counts",
        type=Path,
        default=Path(
            "data_preparation/generated/"
            "caida_20180315_133230_200w_5s_dirA.jsonl"
        ),
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("experiments/end_to_end/caida_holdout_n_sweep/plots"),
    )
    parser.add_argument(
        "--single-column",
        action="store_true",
        help="Render a compact square-panel layout at ACM single-column width",
    )
    parser.add_argument(
        "--panel-aspect",
        type=float,
        default=0.52,
        help="Subplot height/width ratio in single-column mode",
    )
    parser.add_argument(
        "--omit-hl-008",
        action="store_true",
        help="Omit the HeavyLocker 0.08n point from rendered frontiers",
    )
    parser.add_argument(
        "--omit-static-ss",
        action="store_true",
        help="Omit fixed-capacity Space-Saving points from rendered frontiers",
    )
    return parser.parse_args()


def load_results(args: argparse.Namespace) -> list[dict[str, object]]:
    hh_counts = load_hh_counts(
        args.counts,
        tuple(
            sorted(
                {
                    n
                    for values in N_VALUES_BY_PLACEMENT.values()
                    for n in values
                }
            )
        ),
    )
    results: list[dict[str, object]] = []
    for placement, (directory_prefix, _) in PLACEMENTS.items():
        for n in N_VALUES_BY_PLACEMENT[placement]:
            path = (
                args.root
                / f"{directory_prefix}_n{n}"
                / "csv"
                / f"caida_round_robin_n{n}_all_methods.csv"
            )
            if not path.exists():
                raise FileNotFoundError(f"missing n-sweep result: {path}")
            for row in aggregate(path, hh_counts[n]):
                method = canonical_method(str(row["method"]))
                if method is not None:
                    results.append(
                        {
                            "placement": placement,
                            "n": n,
                            "canonical_method": method,
                            **row,
                        }
                    )
    return results


def write_summary(path: Path, results: list[dict[str, object]]) -> None:
    fields = (
        "placement",
        "n",
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


def main() -> int:
    args = parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    results = load_results(args)
    write_summary(args.out / "n_sweep_summary.csv", results)
    hl_methods = HL_METHODS[1:] if args.omit_hl_008 else HL_METHODS

    for placement, (_, title) in PLACEMENTS.items():
        common = {
            "placement": placement,
            "title": title,
            "results": results,
            "row_key": "n",
            "row_values": PLOT_N_VALUES,
            "row_symbol": "n",
            "single_column": args.single_column,
            "panel_aspect": args.panel_aspect,
            "hl_methods": hl_methods,
            "include_static": not args.omit_static_ss,
        }
        plot_placement(
            args.out / f"caida_n_sweep_{placement}.pdf",
            resource="report_volume_kib",
            resource_label=(
                r"Upstream communication (KiB/window) $\downarrow$"
            ),
            resource_name="Upstream-communication",
            **common,
        )
        plot_placement(
            args.out / f"caida_n_sweep_{placement}_worker_memory.pdf",
            resource="mem_worker_total_kib",
            resource_label=r"Mean worker memory (KiB) $\downarrow$",
            resource_name="Worker-memory",
            **common,
        )

    print(f"Wrote n-sweep summary and figures to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
