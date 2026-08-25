#!/usr/bin/env python3
"""Compare partition-level support structure for matched global windows."""

from __future__ import annotations

import argparse
import csv
import gzip
import json
import math
import os
import statistics
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def mean_or_zero(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def load_window(path: Path) -> dict[str, list[list[object]]]:
    with gzip.open(path, "rt", encoding="utf-8") as handle:
        return json.load(handle)


def analyze_window(
    partitions: dict[str, list[list[object]]], window: int, n: int
) -> dict[str, float]:
    local_cardinalities: list[float] = []
    local_loads: list[float] = []
    local_tail_mass: dict[int, list[float]] = {
        multiplier: [] for multiplier in (1, 2, 4)
    }
    local_rank_cutoffs: dict[int, list[float]] = {
        multiplier: [] for multiplier in (1, 2, 4)
    }
    global_counts: defaultdict[str, int] = defaultdict(int)
    support_counts: defaultdict[str, int] = defaultdict(int)

    for records in partitions.values():
        local_cardinalities.append(float(len(records)))
        load = 0
        counts: list[int] = []
        for key, raw_count in records:
            count = int(raw_count)
            load += count
            counts.append(count)
            global_counts[str(key)] += count
            support_counts[str(key)] += 1
        local_loads.append(float(load))
        counts.sort(reverse=True)
        prefix = [0]
        for count in counts:
            prefix.append(prefix[-1] + count)
        for multiplier in (1, 2, 4):
            q = multiplier * n
            retained = prefix[min(q, len(counts))]
            local_tail_mass[multiplier].append(float(load - retained))
            local_rank_cutoffs[multiplier].append(
                float(counts[q - 1]) if len(counts) >= q else 0.0
            )

    total_mass = sum(local_loads)
    threshold = total_mass / n
    heavy_hitters = [
        key for key, count in global_counts.items() if count >= threshold
    ]
    boundary_keys = [
        key
        for key, count in global_counts.items()
        if 0.5 * threshold <= count < 1.5 * threshold
    ]
    non_hh_boundary_keys = [
        key
        for key, count in global_counts.items()
        if 0.5 * threshold <= count < threshold
    ]

    mean_load = mean_or_zero(local_loads)
    mean_cardinality = mean_or_zero(local_cardinalities)
    result = {
        "window": float(window),
        "global_mass": total_mass,
        "global_cardinality": float(len(global_counts)),
        "local_cardinality_mean": mean_cardinality,
        "local_cardinality_p95": percentile(local_cardinalities, 0.95),
        "local_cardinality_max": max(local_cardinalities, default=0.0),
        "local_cardinality_over_n": mean_cardinality / n,
        "local_load_mean": mean_load,
        "local_load_p95": percentile(local_loads, 0.95),
        "local_load_max_over_mean": (
            max(local_loads, default=0.0) / mean_load if mean_load else 0.0
        ),
        "hh_count": float(len(heavy_hitters)),
        "hh_support_mean": mean_or_zero(
            [float(support_counts[key]) for key in heavy_hitters]
        ),
        "boundary_key_count": float(len(boundary_keys)),
        "boundary_support_mean": mean_or_zero(
            [float(support_counts[key]) for key in boundary_keys]
        ),
        "non_hh_boundary_count": float(len(non_hh_boundary_keys)),
        "non_hh_boundary_support_mean": mean_or_zero(
            [float(support_counts[key]) for key in non_hh_boundary_keys]
        ),
        "all_key_support_mean": mean_or_zero(
            [float(value) for value in support_counts.values()]
        ),
        "mass_weighted_support": (
            sum(
                global_counts[key] * support
                for key, support in support_counts.items()
            )
            / total_mass
            if total_mass
            else 0.0
        ),
    }
    for multiplier in (1, 2, 4):
        result[f"tail_mass_beyond_{multiplier}n_fraction"] = (
            sum(local_tail_mass[multiplier]) / total_mass
            if total_mass
            else 0.0
        )
        result[f"local_{multiplier}n_rank_count_mean"] = mean_or_zero(
            local_rank_cutoffs[multiplier]
        )
    return result


def parse_dataset(value: str) -> tuple[str, int, Path]:
    try:
        label, raw_n, raw_path = value.split(":", 2)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "dataset must be LABEL:N:STREAM_DIRECTORY"
        ) from error
    return label, int(raw_n), Path(raw_path)


def analyze_path(
    job: tuple[Path, list[tuple[str, int]]],
) -> tuple[int, list[tuple[str, int, dict[str, float]]]]:
    path, variants = job
    partitions = load_window(path)
    window = int(path.name.split("_")[1].split(".")[0])
    return window, [
        (label, n, analyze_window(partitions, window, n))
        for label, n in variants
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--dataset",
        action="append",
        required=True,
        type=parse_dataset,
        help="LABEL:N:STREAM_DIRECTORY; may be repeated",
    )
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--workers",
        type=int,
        default=min(8, os.cpu_count() or 1),
        help="parallel window decoders (default: min(8, CPU count))",
    )
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    window_rows: list[dict[str, object]] = []
    summary_rows: list[dict[str, object]] = []

    grouped: defaultdict[Path, list[tuple[str, int]]] = defaultdict(list)
    for label, n, stream_dir in args.dataset:
        grouped[stream_dir].append((label, n))

    metrics_by_dataset: dict[tuple[str, int], list[dict[str, float]]] = {}
    for stream_dir, variants in grouped.items():
        paths = sorted(stream_dir.glob("window_*.json.gz"))
        if not paths:
            raise FileNotFoundError(f"no window files under {stream_dir}")
        for label, n in variants:
            metrics_by_dataset[(label, n)] = []
        print(f"Analyzing {stream_dir} ({len(paths)} windows)")
        jobs = ((path, variants) for path in paths)
        with ProcessPoolExecutor(max_workers=args.workers) as executor:
            results = executor.map(analyze_path, jobs, chunksize=1)
            for completed, (_, rows) in enumerate(results, start=1):
                for label, n, row in rows:
                    metrics_by_dataset[(label, n)].append(row)
                if completed % 25 == 0 or completed == len(paths):
                    print(f"  {completed}/{len(paths)} windows", flush=True)

    for label, n, stream_dir in args.dataset:
        metrics = metrics_by_dataset[(label, n)]
        for row in metrics:
            window_rows.append({"placement": label, "n": n, **row})

        summary: dict[str, object] = {
            "placement": label,
            "n": n,
            "windows": len(metrics),
        }
        for field in metrics[0]:
            if field == "window":
                continue
            values = [float(row[field]) for row in metrics]
            summary[f"{field}_mean"] = mean_or_zero(values)
            summary[f"{field}_p95"] = percentile(values, 0.95)
        summary_rows.append(summary)

    window_path = args.out / "partition_hardness_windows.csv"
    summary_path = args.out / "partition_hardness_summary.csv"
    with window_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(window_rows[0]))
        writer.writeheader()
        writer.writerows(window_rows)
    with summary_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summary_rows[0]))
        writer.writeheader()
        writer.writerows(summary_rows)

    print(f"Wrote {window_path}")
    print(f"Wrote {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
